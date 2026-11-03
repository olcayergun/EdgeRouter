/**
 * @file wifi_ap.c
 * @brief 2. ESP32 Wi-Fi SoftAP (Erişim Noktası) Katmanı Gerçeklemesi.
 *
 * ESP-IDF Wi-Fi sürücüsünü SoftAP modunda yapılandırır, DHCP sunucusunu
 * `10.54.0.1/24` yerel alt ağı için kurar.
 * Bağlanan istemcilerden gelen IPv4 paketlerini kanca (hook) ile yakalayıp
 * SPI Link hattına iletir; SPI'den gelen yanıtları ise Wi-Fi üzerinden istemciye basar.
 */

#if defined(CONFIG_NODE_AP)

#include "wifi_ap.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "freertos/FreeRTOS.h"
#include "ip_utils.h"
#include "node_ap.h"
#include "nvs_flash.h"
#include "pkt_buf.h"
#include "spi_link.h"

/* -------------------------------------------------------------------------
 * Compatibility shim: ensure esp_netif_transmit() prototype is visible.
 * ------------------------------------------------------------------------- */
#ifndef ESP_NETIF_TRANSMIT_DECLARED
#define ESP_NETIF_TRANSMIT_DECLARED
esp_err_t esp_netif_transmit(esp_netif_t *netif, void *buffer, size_t len);
#endif

static const char *TAG = "wifi_ap";

static esp_netif_t *s_ap_netif = NULL;
static bool s_ap_started = false;

/** İstemci IP - MAC eşleşme tablosu (Gelen yanıtlarda hedef MAC'i bulmak için) */
#define CLIENT_TABLE_SIZE 8
typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    bool in_use;
} client_entry_t;

static client_entry_t s_client_table[CLIENT_TABLE_SIZE];

/**
 * @brief İstemci IP ve MAC adresini tabloya kaydeder veya günceller.
 */
static void client_table_update(uint32_t ip, const uint8_t *mac)
{
    if (!mac || ip == 0) return;

    for (size_t i = 0; i < CLIENT_TABLE_SIZE; i++) {
        if (s_client_table[i].in_use && s_client_table[i].ip == ip) {
            memcpy(s_client_table[i].mac, mac, 6);
            return;
        }
    }
    for (size_t i = 0; i < CLIENT_TABLE_SIZE; i++) {
        if (!s_client_table[i].in_use) {
            s_client_table[i].in_use = true;
            s_client_table[i].ip = ip;
            memcpy(s_client_table[i].mac, mac, 6);
            return;
        }
    }
}

/**
 * @brief İstemci IP'sine ait MAC adresini tablodan arar.
 */
static bool client_table_get_mac(uint32_t ip, uint8_t *out_mac)
{
    if (!out_mac || ip == 0) return false;

    for (size_t i = 0; i < CLIENT_TABLE_SIZE; i++) {
        if (s_client_table[i].in_use && s_client_table[i].ip == ip) {
            memcpy(out_mac, s_client_table[i].mac, 6);
            return true;
        }
    }
    return false;
}

/**
 * @brief Wi-Fi SoftAP fiziksel RX kancası (Hook).
 *
 * SoftAP arayüzüne bağlı istemcilerden gelen Ethernet paketlerini yakalar.
 * DHCP, Multicast (mDNS/SSDP) ve Broadcast paketlerini yerel ağda tutarak
 * sadece harici WAN yönüne veya yerel istemciye iletilecek tekil (unicast)
 * IPv4 paketlerini merkezi yönlendiriciye iletir.
 */
static esp_err_t wifi_ap_rx_hook(esp_netif_t *esp_netif, void *buffer, size_t len, void *eb)
{
    if (buffer && len >= 14 + 20) {
        const uint8_t *eth_hdr = (const uint8_t *)buffer;
        const uint16_t ethertype = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];

        if (ethertype == 0x0800) {
            const uint8_t *ip_pkt = eth_hdr + 14;
            const size_t ip_len = len - 14;

            ip_packet_info_t info;
            if (ip_utils_parse(ip_pkt, ip_len, &info)) {
                /* 1. DHCP (UDP 67/68) paketleri yerel AP tarafından işlenir, filtrele */
                const bool is_dhcp = (info.protocol == 17 && (info.src_port == 67 || info.src_port == 68 ||
                                                              info.dst_port == 67 || info.dst_port == 68));

                /* 2. Multicast (224.0.0.0/4) paketleri (mDNS, SSDP vb.) SPI'ye gitmemeli */
                const uint8_t dst_first_byte = ip_pkt[16];
                const bool is_multicast = (dst_first_byte >= 224 && dst_first_byte <= 239);

                /* 3. Yerel alt ağ broadcast paketleri SPI'ye gitmemeli */
                const bool is_broadcast = (info.dst_ip == 0xFFFFFFFF ||
                                           info.dst_ip == esp_ip4addr_aton("10.54.0.255"));

                if (!is_dhcp && !is_multicast && !is_broadcast) {
                    static uint32_t s_ap_rx_count = 0;
                    s_ap_rx_count++;

                    /* İstemcinin MAC adresini IP eşleşme tablosuna kaydet */
                    client_table_update(info.src_ip, eth_hdr + 6);

                    /* Zero-Copy: Yeni tampon tahsis et ve AP merkezi yönlendiricisine ver */
                    pkt_buf_t *rx_buf = pkt_buf_alloc(0);
                    if (rx_buf) {
                        memcpy(rx_buf->data, ip_pkt, ip_len);
                        rx_buf->len = ip_len;
                        rx_buf->rx_if = NET_IF_WIFI_AP;
                        rx_buf->tx_if = NET_IF_NONE;
                        rx_buf->seq_num = (info.is_icmp && info.icmp_seq > 0) ? info.icmp_seq : s_ap_rx_count;
#if ENABLE_ICMP_LATENCY_METRICS
                        rx_buf->is_icmp = info.is_icmp;
                        rx_buf->lat.t_rx_hook = latency_now_us();
#endif

                        ESP_LOGI(TAG, "[AP-RX #%" PRIu32 "] Istemciden Alindi: %s -> %s (%s, %u byte)",
                                 rx_buf->seq_num, info.src_str, info.dst_str, info.proto_name, (unsigned)rx_buf->len);

                        /* 2. ESP32'nin tek merkezi yönlendirme fonksiyonuna teslim et */
                        const esp_err_t fwd_err = node_ap_forward_packet(rx_buf);
                        if (fwd_err != ESP_OK) {
                            ESP_LOGE(TAG, "node_ap_forward_packet hatasi: %s", esp_err_to_name(fwd_err));
                        }
                    }
                }
            }
        }
    }

    return esp_netif_receive(s_ap_netif, buffer, len, eb);
}

/**
 * @brief Wi-Fi AP olay işleyicisi (İstemci bağlandı / ayrıldı).
 */
static void wifi_ap_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "[SoftAP] Yeni istemci baglandi: MAC=%02x:%02x:%02x:%02x:%02x:%02x (AID=%d)",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5], event->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = (const wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "[SoftAP] Istemci ayrildi: MAC=%02x:%02x:%02x:%02x:%02x:%02x (AID=%d)",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5], event->aid);
    }
}

/**
 * @brief NVS Flash birimini başlatır.
 */
static esp_err_t ap_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS yeniden olusturuluyor...");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wifi_ap_start(void)
{
    if (s_ap_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ap_nvs_init(), TAG, "NVS baslatilamadi");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init basarisiz");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop basarisiz");

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "Default Wi-Fi AP netif olusturulamadi");
        return ESP_FAIL;
    }

    /* SoftAP Yerel IP Yapılandırması (10.54.0.1 / 255.255.255.0) */
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(s_ap_netif), TAG, "dhcps_stop basarisiz");
    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = esp_ip4addr_aton(DEFAULT_AP_IP);
    ip_info.gw.addr = esp_ip4addr_aton(DEFAULT_AP_GW);
    ip_info.netmask.addr = esp_ip4addr_aton(DEFAULT_AP_NETMASK);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), TAG, "set_ip_info basarisiz");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "dhcps_start basarisiz");

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init basarisiz");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_ap_event_handler, NULL, NULL),
        TAG, "WIFI_EVENT handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage");

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.ap.ssid, DEFAULT_AP_SSID, sizeof(wifi_cfg.ap.ssid));
    strlcpy((char *)wifi_cfg.ap.password, DEFAULT_AP_PASS, sizeof(wifi_cfg.ap.password));
    wifi_cfg.ap.ssid_len = strlen(DEFAULT_AP_SSID);
    wifi_cfg.ap.channel = DEFAULT_AP_CHANNEL;
    wifi_cfg.ap.max_connection = DEFAULT_AP_MAX_CONN;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    /* Wi-Fi Güç Tasarrufunu kapat */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps (WIFI_PS_NONE)");

    /* Wi-Fi RX Paket Kancasını (Hook) bağla */
    wifi_netif_driver_t driver = (wifi_netif_driver_t)esp_netif_get_io_driver(s_ap_netif);
    if (driver) {
        esp_wifi_register_if_rxcb(driver, wifi_ap_rx_hook, s_ap_netif);
        ESP_LOGI(TAG, "Wi-Fi AP RX Kancasi baglandi.");
    }

    s_ap_started = true;
    ESP_LOGI(TAG, "SoftAP Baslatildi! SSID: '%s' | Parola: '%s' | IP: %s",
             DEFAULT_AP_SSID, DEFAULT_AP_PASS, DEFAULT_AP_IP);

    return ESP_OK;
}

esp_err_t wifi_ap_send(pkt_buf_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ap_started || !s_ap_netif) {
        ESP_LOGW(TAG, "wifi_ap_send: AP henuz aktif degil, paket dusuruldu");
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    ip_packet_info_t info;
    const bool parsed = ip_utils_parse(buf->data, buf->len, &info);

    /* Layer-2 Ethernet Çerçevesi oluştur */
    uint8_t eth_frame[14 + PKT_BUF_MAX_PAYLOAD];
    uint8_t ap_mac[6] = { 0 };
    uint8_t dst_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; /* Varsayılan broadcast */

    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);

    if (parsed) {
        /* İstemci tablosundan hedef IP'ye ait MAC adresini sorgula */
        client_table_get_mac(info.dst_ip, dst_mac);
    }

    memcpy(eth_frame, dst_mac, 6);
    memcpy(eth_frame + 6, ap_mac, 6);
    eth_frame[12] = 0x08;
    eth_frame[13] = 0x00;
    memcpy(eth_frame + 14, buf->data, buf->len);

    const size_t eth_len = buf->len + 14;
    esp_err_t err = esp_netif_transmit(s_ap_netif, eth_frame, eth_len);

#if ENABLE_ICMP_LATENCY_METRICS
    if (buf->is_icmp) {
        buf->lat.t_tx_hw_done = latency_now_us();
        latency_report_ap_ingress(buf->seq_num, buf->len, &buf->lat);
    }
#endif

    if (parsed) {
        ESP_LOGI(TAG, "[AP-TX #%" PRIu32 "] Istemciye Basildi: %s -> %s (%s, %u byte) | TX: %s",
                 buf->seq_num, info.src_str, info.dst_str, info.proto_name,
                 (unsigned)buf->len, (err == ESP_OK) ? "OK" : "ERR");
    } else {
        ESP_LOGI(TAG, "[AP-TX #%" PRIu32 "] Istemciye Basildi: %u byte | TX: %s",
                 buf->seq_num, (unsigned)buf->len, (err == ESP_OK) ? "OK" : "ERR");
    }

    pkt_buf_free(buf);
    return err;
}

bool wifi_ap_get_ip_info(esp_netif_ip_info_t *info)
{
    if (!info || !s_ap_netif || !s_ap_started) {
        return false;
    }
    return esp_netif_get_ip_info(s_ap_netif, info) == ESP_OK;
}

#endif /* CONFIG_NODE_AP */

