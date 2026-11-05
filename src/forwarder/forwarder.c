/**
 * @file forwarder.c
 * @brief EdgeRouter Paket Yönlendirme (Forwarder) Katmanı Gerçeklemesi.
 *
 * Gelen tüm IP paketleri `forwarder_handle_packet` üzerinden yalnızca bellek adresi
 * (`pkt_buf_t *`) olarak buraya aktarılır.
 * Fonksiyon paketin içeriğini inceler, yönlendirme kararını verir, NAPT dönüşümlerini
 * uygular ve çıkış arayüzüne iletilene kadar paketin aynı tamponda kalmasını (Zero-Copy) sağlar.
 * Çift parse yapılmaz; ayrıştırılan başlık bilgisi (`ip_packet_info_t`) NAPT katmanına aktarılır.
 */

#if defined(CONFIG_NODE_ROUTER)

#include "forwarder.h"

#include <inttypes.h>

#include "esp_log.h"
#include "ip_utils.h"
#include "nat.h"
#include "pkt_buf.h"
#include "spi_link.h"
#include "wifi_sta.h"

static const char *TAG = "forwarder";

esp_err_t forwarder_handle_packet(pkt_buf_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }

#if ENABLE_ICMP_LATENCY_METRICS
    if (buf->is_icmp) {
        buf->lat.t_fwd_start = latency_now_us();
    }
#endif

    /* 1. Paketin IPv4 ve üst protokol alanlarını ayrıştır (Tek parse) */
    ip_packet_info_t info;
    if (!ip_utils_parse(buf->data, buf->len, &info)) {
        ESP_LOGE(TAG, "[Paket #%" PRIu32 "] Gecersiz veya bozuk IPv4 paketi, dusuruluyor.", buf->seq_num);
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    /* 2. DÖNGÜ (LOOP) ENGELLEME KONTROLÜ: TTL Kontrolü (RFC 791) */
    if (info.ttl <= 1) {
        ESP_LOGW(TAG, "[Paket #%" PRIu32 " | LOOP ENGELLENDI] Paket TTL suresi doldu (TTL=%u), paket dusuruldu.",
                 buf->seq_num, info.ttl);
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    /* TTL bir azaltılır ve sağlama toplamları (checksum) yenilenir */
    buf->data[8]--;
    ip_utils_update_checksums(buf->data, buf->len);

    /* 3. Arayüz Bazlı Yönlendirme ve NAPT Kararı */
    if (buf->rx_if == NET_IF_SPI_RX) {
        buf->tx_if = NET_IF_WIFI_STA;
        const uint32_t wan_ip = wifi_sta_get_wan_ip();

        /* [Madde 9: Local Delivery / Hairpin NAT Kontrolü]
         * Eğer iç ağdan gelen paketin hedefi Router'ın kendi WAN IP'si ise
         * paket dışarıya (Wi-Fi) çıkmadan yerel olarak değerlendirilir.
         */
        if (wan_ip != 0 && info.dst_ip == wan_ip) {
            ESP_LOGI(TAG, "[FWD #%" PRIu32 "] Hedef Router WAN IP (%s), yerel teslimat / hairpin.",
                     buf->seq_num, info.dst_str);
        }

        /* TCP SYN paketlerinde MSS değerini 1400 bayta sınırla (MTU / Path MTU garantisi) */
        if (info.protocol == 6) {
            ip_utils_clamp_mss(buf->data, buf->len, 1400);
        }

        /* [NAPT / SNAT] Dönüşüm yerinde uygulanır (Tek parse paylaşımı) */
        const esp_err_t nat_err = nat_apply_snat_fast(buf, wan_ip, &info);
        if (nat_err != ESP_OK) {
            ESP_LOGE(TAG, "[STA-FWD #%" PRIu32 "] NAT basarisiz (%s), paket dusuruldu.",
                     buf->seq_num, esp_err_to_name(nat_err));
            pkt_buf_free(buf);
            return nat_err;
        }

        ESP_LOGI(TAG, "[STA-FWD #%" PRIu32 "] HSPI -> Wi-Fi STA (SNAT OK: %s -> %s)",
                 buf->seq_num, info.src_str, info.dst_str);

#if ENABLE_ICMP_LATENCY_METRICS
        if (buf->is_icmp) {
            buf->lat.t_fwd_done = latency_now_us();
        }
#endif

        /* Sıfır kopyalama ile tampon Wi-Fi STA çıkış katmanına teslim edilir */
        return wifi_sta_send(buf);
    } else if (buf->rx_if == NET_IF_WIFI_STA) {
        buf->tx_if = NET_IF_SPI_TX;

        /* Dışarıdan gelen TCP SYN-ACK yanıtlarında da MSS değerini 1400 bayta sınırla */
        if (info.protocol == 6) {
            ip_utils_clamp_mss(buf->data, buf->len, 1400);
        }

        /* [NAPT / DNAT] Dış ağdan gelen yanıta Ters NAT uygula (Tek parse paylaşımı) */
        const esp_err_t dnat_err = nat_apply_dnat_fast(buf, &info);
        if (dnat_err != ESP_OK) {
            pkt_buf_free(buf);
            return dnat_err;
        }

        ESP_LOGI(TAG, "[STA-FWD #%" PRIu32 "] Wi-Fi STA -> HSPI (DNAT OK: Yanit donusturuldu)",
                 buf->seq_num);

#if ENABLE_ICMP_LATENCY_METRICS
        if (buf->is_icmp) {
            buf->lat.t_fwd_done = latency_now_us();
        }
#endif

        /* Sıfır kopyalama ile tampon SPI çıkış katmanına teslim edilir */
        return spi_link_send(buf);
    } else {
        ESP_LOGW(TAG, "[STA-FWD] Tanimsiz giris arayuzu (%d), dusuruldu.", buf->rx_if);
        pkt_buf_free(buf);
        return ESP_ERR_NOT_FOUND;
    }
}

#endif /* CONFIG_NODE_ROUTER */
