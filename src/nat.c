/**
 * @file nat.c
 * @brief Ağ Adresi ve Port Dönüştürme (Network Address and Port Translation / NAPT) Modülü Gerçeklemesi.
 *
 * Yerel alt ağdaki cihazların tek bir WAN (Wi-Fi) IP adresi arkasından dış ağa
 * erişebilmesini sağlayan NAPT (Port Masquerading), dinamik port havuzu yönetimi,
 * durum takibi (Connection Tracking) ve çoklu çekirdek kilit mekanizması içerir.
 * Sıfır kopyalama (Zero-Copy) prensibine tam uyumludur: doğrudan `pkt_buf_t`
 * bellek tamponu üzerindeki IP, TCP, UDP ve ICMP başlıklarını yerinde günceller ve
 * standartlara uygun olarak sağlama toplamlarını (checksum) yeniler.
 */

#if defined(CONFIG_NODE_ROUTER)

#include "nat.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ip_utils.h"

static const char *TAG = "nat";

/** Statik NAPT oturum tablosu */
static nat_entry_t s_nat_table[NAT_MAX_ENTRIES];

/** NAPT modülü başlatıldı mı? */
static bool s_initialized = false;

/** Thread-safety için FreeRTOS Mutex kilidi */
static SemaphoreHandle_t s_nat_mutex = NULL;

/** Dinamik port tahsisi için sonraki aday port */
static uint16_t s_next_nat_port = NAT_PORT_RANGE_START;

/** DNAT eşleşmesi bulunamayan paket sayacı */
static uint32_t s_dnat_miss_count = 0;

/** DNAT Miss logları için son basılma zamanı (mikrosaniye) */
static int64_t s_last_miss_log_us = 0;

/**
 * @brief 16-bit tamsayıyı hedef belleğe Big-Endian (Network Byte Order) olarak yazar.
 */
static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/**
 * @brief NAT tablosunu eşzamanlı erişime karşı kilitler.
 */
static inline void nat_lock(void)
{
    if (s_nat_mutex) {
        xSemaphoreTake(s_nat_mutex, portMAX_DELAY);
    }
}

/**
 * @brief NAT tablosu kilidini serbest bırakır.
 */
static inline void nat_unlock(void)
{
    if (s_nat_mutex) {
        xSemaphoreGive(s_nat_mutex);
    }
}

/**
 * @brief Sistem açılışından itibaren geçen süreyi (saniye cinsinden) döndürür.
 */
static uint32_t get_uptime_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

/**
 * @brief Süresi dolmuş eski NAT oturumlarını tablodan temizler (Kilit altında çağrılmalıdır).
 */
static void nat_clean_expired_locked(void)
{
    const uint32_t now = get_uptime_sec();
    for (size_t i = 0; i < NAT_MAX_ENTRIES; i++) {
        if (s_nat_table[i].in_use) {
            const uint32_t timeout = (s_nat_table[i].timeout_sec > 0) ?
                                      s_nat_table[i].timeout_sec : NAT_TIMEOUT_TCP_ESTABLISHED_SEC;
            if (now >= s_nat_table[i].last_active_sec + timeout) {
                ESP_LOGD(TAG, "Zaman asimina ugrayan NAT oturumu silindi [index %u, proto %u, port %u]",
                         (unsigned)i, s_nat_table[i].protocol, s_nat_table[i].nat_port);
                s_nat_table[i].in_use = false;
            }
        }
    }
}

/**
 * @brief Port havuzundan belirtilen protokol için boş bir dış port tahsis eder (Kilit altında çağrılmalıdır).
 *
 * @param protocol L4 protokolü (6: TCP, 17: UDP, 1: ICMP)
 * @return uint16_t Tahsis edilen port (veya ICMP ID), havuz doluysa 0.
 */
static uint16_t nat_allocate_port_locked(uint8_t protocol)
{
    const uint16_t range_size = NAT_PORT_RANGE_END - NAT_PORT_RANGE_START + 1;

    for (uint16_t attempt = 0; attempt < range_size; attempt++) {
        uint16_t candidate_port = s_next_nat_port++;
        if (s_next_nat_port > NAT_PORT_RANGE_END) {
            s_next_nat_port = NAT_PORT_RANGE_START;
        }

        bool collision = false;
        for (size_t i = 0; i < NAT_MAX_ENTRIES; i++) {
            if (s_nat_table[i].in_use &&
                s_nat_table[i].protocol == protocol &&
                s_nat_table[i].nat_port == candidate_port) {
                collision = true;
                break;
            }
        }

        if (!collision) {
            return candidate_port;
        }
    }

    return 0; /* Havuz tamamen dolu */
}

esp_err_t nat_init(void)
{
    if (!s_nat_mutex) {
        s_nat_mutex = xSemaphoreCreateMutex();
        if (!s_nat_mutex) {
            ESP_LOGE(TAG, "NAT mutex baslatilamadi!");
            return ESP_ERR_NO_MEM;
        }
    }

    nat_lock();
    memset(s_nat_table, 0, sizeof(s_nat_table));
    s_next_nat_port = NAT_PORT_RANGE_START;
    s_dnat_miss_count = 0;
    s_last_miss_log_us = 0;
    s_initialized = true;
    nat_unlock();

    ESP_LOGI(TAG, "NAPT modulu devrede (Maksimum oturum: %d, Port havuzu: %u-%u)",
             NAT_MAX_ENTRIES, NAT_PORT_RANGE_START, NAT_PORT_RANGE_END);
    return ESP_OK;
}

size_t nat_get_active_count(void)
{
    size_t count = 0;
    nat_lock();
    for (size_t i = 0; i < NAT_MAX_ENTRIES; i++) {
        if (s_nat_table[i].in_use) {
            count++;
        }
    }
    nat_unlock();
    return count;
}

uint32_t nat_get_dnat_miss_count(void)
{
    return s_dnat_miss_count;
}

/**
 * @brief Giden paket için var olan oturumu bulur ya da yeni bir NAPT girişi oluşturur (Kilit altında çağrılır).
 */
static nat_entry_t *find_or_create_outbound_locked(uint8_t protocol, uint32_t orig_src,
                                                   uint32_t orig_dst, uint16_t orig_port,
                                                   uint16_t orig_dst_port, uint32_t nat_src)
{
    nat_clean_expired_locked();
    const uint32_t now = get_uptime_sec();

    /* 1. Önceden açılmış aynı iç kaynak IP + port + hedef IP + port oturumu var mı? */
    for (size_t i = 0; i < NAT_MAX_ENTRIES; i++) {
        if (s_nat_table[i].in_use &&
            s_nat_table[i].protocol == protocol &&
            s_nat_table[i].orig_src_ip == orig_src &&
            s_nat_table[i].orig_port == orig_port &&
            s_nat_table[i].orig_dst_ip == orig_dst &&
            s_nat_table[i].orig_dst_port == orig_dst_port) {
            s_nat_table[i].last_active_sec = now;
            return &s_nat_table[i];
        }
    }

    /* 2. Yeni oturum için port havuzundan dış port tahsis et */
    uint16_t allocated_nat_port = nat_allocate_port_locked(protocol);
    if (allocated_nat_port == 0) {
        ESP_LOGE(TAG, "NAPT port havuzu tukendi! Yeni port tahsis edilemedi.");
        return NULL;
    }

    /* 3. Tablodaki ilk boş göze kaydet */
    for (size_t i = 0; i < NAT_MAX_ENTRIES; i++) {
        if (!s_nat_table[i].in_use) {
            s_nat_table[i].in_use = true;
            s_nat_table[i].protocol = protocol;
            s_nat_table[i].tcp_state = NAT_TCP_STATE_NONE;
            s_nat_table[i].orig_src_ip = orig_src;
            s_nat_table[i].orig_dst_ip = orig_dst;
            s_nat_table[i].orig_port = orig_port;
            s_nat_table[i].orig_dst_port = orig_dst_port;
            s_nat_table[i].nat_src_ip = nat_src;
            s_nat_table[i].nat_port = allocated_nat_port;
            s_nat_table[i].last_active_sec = now;

            /* Varsayılan zaman aşımı süresini ata */
            if (protocol == 6) {
                s_nat_table[i].timeout_sec = NAT_TIMEOUT_TCP_TRANS_SEC;
            } else if (protocol == 17) {
                s_nat_table[i].timeout_sec = NAT_TIMEOUT_UDP_SEC;
            } else {
                s_nat_table[i].timeout_sec = NAT_TIMEOUT_ICMP_SEC;
            }

            return &s_nat_table[i];
        }
    }

    ESP_LOGE(TAG, "NAT tablosu dolu! Yeni oturum acilamadi (Maks: %d).", NAT_MAX_ENTRIES);
    return NULL;
}

esp_err_t nat_apply_snat_fast(pkt_buf_t *buf, uint32_t wan_ip_be, const ip_packet_info_t *info)
{
    if (!buf || buf->len < 20) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        nat_init();
    }

    ip_packet_info_t local_info;
    if (!info) {
        if (!ip_utils_parse(buf->data, buf->len, &local_info)) {
            return ESP_ERR_INVALID_ARG;
        }
        info = &local_info;
    }

    /* Kaynak zaten WAN IP'si ise dönüştürmeye gerek yok */
    if (info->src_ip == wan_ip_be) {
        return ESP_OK;
    }

    const uint8_t proto = info->protocol;
    uint16_t orig_port = 0;
    uint16_t orig_dst_port = 0;

    if (info->is_icmp) {
        orig_port = info->icmp_id;
    } else if (proto == 6 || proto == 17) {
        orig_port = info->src_port;
        orig_dst_port = info->dst_port;
    }

    nat_lock();
    nat_entry_t *entry = find_or_create_outbound_locked(proto, info->src_ip,
                                                         info->dst_ip, orig_port,
                                                         orig_dst_port, wan_ip_be);
    if (!entry) {
        nat_unlock();
        return ESP_ERR_NO_MEM;
    }

    const uint16_t nat_port = entry->nat_port;

    /* TCP Bağlantı Durumu Takibi (Connection Tracking) */
    if (proto == 6 && buf->len >= (size_t)(info->ihl_bytes + 14)) {
        const uint8_t tcp_flags = buf->data[info->ihl_bytes + 13];
        if (tcp_flags & 0x02) { /* SYN */
            entry->tcp_state = NAT_TCP_STATE_SYN_SENT;
            entry->timeout_sec = NAT_TIMEOUT_TCP_TRANS_SEC;
        } else if (tcp_flags & (0x01 | 0x04)) { /* FIN veya RST */
            entry->tcp_state = NAT_TCP_STATE_FIN_WAIT;
            entry->timeout_sec = NAT_TIMEOUT_TCP_CLOSED_SEC;
        } else if (entry->tcp_state == NAT_TCP_STATE_SYN_SENT && (tcp_flags & 0x10)) { /* ACK */
            entry->tcp_state = NAT_TCP_STATE_ESTABLISHED;
            entry->timeout_sec = NAT_TIMEOUT_TCP_ESTABLISHED_SEC;
        }
    }
    nat_unlock();

    /* 1. IPv4 Kaynak IP adresini WAN IP ile yerinde (Zero-Copy) değiştir */
    memcpy(buf->data + 12, &wan_ip_be, 4);

    /* 2. L4 Port veya ICMP ID değerini yerinde güncelle (NAPT) */
    if (proto == 6 || proto == 17) {
        /* TCP ve UDP başlığında Kaynak Port ilk 2 bayttır (offset: ihl_bytes) */
        put_be16(buf->data + info->ihl_bytes, nat_port);
    } else if (info->is_icmp && buf->len >= (size_t)(info->ihl_bytes + 8)) {
        /* ICMP Identifier offset: ihl_bytes + 4 */
        put_be16(buf->data + info->ihl_bytes + 4, nat_port);
    }

    /* 3. IPv4 ve L4 (ICMP/TCP/UDP) sağlama toplamlarını (checksum) güncelle */
    ip_utils_update_checksums(buf->data, buf->len);

    ESP_LOGD(TAG, "[NAT #%" PRIu32 "] SNAT: %s:%u -> WAN:%u (Proto: %u, Aktif: %u)",
             buf->seq_num, info->src_str, orig_port, nat_port, proto, (unsigned)nat_get_active_count());

    return ESP_OK;
}

esp_err_t nat_apply_snat(pkt_buf_t *buf, uint32_t wan_ip_be)
{
    return nat_apply_snat_fast(buf, wan_ip_be, NULL);
}

esp_err_t nat_apply_dnat_fast(pkt_buf_t *buf, const ip_packet_info_t *info)
{
    if (!buf || buf->len < 20) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_NOT_FOUND;
    }

    ip_packet_info_t local_info;
    if (!info) {
        if (!ip_utils_parse(buf->data, buf->len, &local_info)) {
            return ESP_ERR_INVALID_ARG;
        }
        info = &local_info;
    }

    uint16_t lookup_port = 0;
    if (info->is_icmp) {
        lookup_port = info->icmp_id;
    } else if (info->protocol == 6 || info->protocol == 17) {
        lookup_port = info->dst_port;
    }

    nat_lock();
    const uint32_t now = get_uptime_sec();
    nat_entry_t *matched_entry = NULL;

    /* Güçlendirilmiş DNAT Eşleştirmesi:
     * 1. Protokol eşleşmeli
     * 2. nat_port (WAN portu / ID) eşleşmeli
     * 3. Eğer orig_dst_ip tanımlıysa, gelen paketin kaynak IP'si bu oturumun hedefi olmalıdır (Anti-Spoofing).
     */
    for (size_t i = 0; i < NAT_MAX_ENTRIES; i++) {
        if (s_nat_table[i].in_use &&
            s_nat_table[i].protocol == info->protocol &&
            s_nat_table[i].nat_port == lookup_port) {

            /* Opsiyonel kaynak doğrulama kontrolü */
            if (s_nat_table[i].orig_dst_ip != 0 && s_nat_table[i].orig_dst_ip != info->src_ip) {
                continue; /* Yanıt beklenen sunucudan gelmemiş */
            }

            matched_entry = &s_nat_table[i];
            matched_entry->last_active_sec = now;

            /* TCP Durum Takibi (Gelen yanıt) */
            if (info->protocol == 6 && buf->len >= (size_t)(info->ihl_bytes + 14)) {
                const uint8_t tcp_flags = buf->data[info->ihl_bytes + 13];
                if ((tcp_flags & 0x12) == 0x12) { /* SYN + ACK */
                    matched_entry->tcp_state = NAT_TCP_STATE_ESTABLISHED;
                    matched_entry->timeout_sec = NAT_TIMEOUT_TCP_ESTABLISHED_SEC;
                } else if (tcp_flags & (0x01 | 0x04)) { /* FIN veya RST */
                    matched_entry->tcp_state = NAT_TCP_STATE_FIN_WAIT;
                    matched_entry->timeout_sec = NAT_TIMEOUT_TCP_CLOSED_SEC;
                }
            }
            break;
        }
    }

    if (!matched_entry) {
        nat_unlock();
        s_dnat_miss_count++;

        /* Rate-Limited Loglama (Saniyede en fazla 5 log = 200 ms) */
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_miss_log_us >= 200000) {
            s_last_miss_log_us = now_us;
            ESP_LOGW(TAG, "[NAT] DNAT MISS! Eslestirilemeyen paket: Proto=%s, DstPort=%u, Src=%s (Toplam Miss: %" PRIu32 ")",
                     info->proto_name, lookup_port, info->src_str, s_dnat_miss_count);
        }
        return ESP_ERR_NOT_FOUND;
    }

    const uint32_t orig_src_ip = matched_entry->orig_src_ip;
    const uint16_t orig_port = matched_entry->orig_port;
    nat_unlock();

    /* 1. Hedef IP'yi orijinal iç istemci IP adresi ile yerinde değiştir */
    memcpy(buf->data + 16, &orig_src_ip, 4);

    /* 2. Hedef Port / ICMP ID'yi orijinal iç istemci portu ile değiştir (NAPT De-translation) */
    if (info->protocol == 6 || info->protocol == 17) {
        /* Hedef Port TCP/UDP başlığında 2. ve 3. bayttır (offset: ihl_bytes + 2) */
        put_be16(buf->data + info->ihl_bytes + 2, orig_port);
    } else if (info->is_icmp && buf->len >= (size_t)(info->ihl_bytes + 8)) {
        put_be16(buf->data + info->ihl_bytes + 4, orig_port);
    }

    /* 3. Checksum değerlerini yeniden hesapla (IPv4 + L4) */
    ip_utils_update_checksums(buf->data, buf->len);

    ESP_LOGD(TAG, "[NAT #%" PRIu32 "] DNAT: WAN:%u -> %s:%u (Proto: %s)",
             buf->seq_num, lookup_port, info->dst_str, orig_port, info->proto_name);

    return ESP_OK;
}

esp_err_t nat_apply_dnat(pkt_buf_t *buf)
{
    return nat_apply_dnat_fast(buf, NULL);
}

#endif /* CONFIG_NODE_ROUTER */
