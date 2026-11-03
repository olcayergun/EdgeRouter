/**
 * @file wifi_sta.c
 * @brief Wi-Fi İstasyon (Station / STA) Katmanı Gerçeklemesi.
 *
 * ESP-IDF Wi-Fi sürücüsünü STA modunda yapılandırır, NVS flash'ı başlatır ve
 * Kconfig üzerinden belirlenen SSID ve parola ile bağlantı kurar.
 * Bağlantı koptuğunda belirlenen üst sınıra (`WIFI_MAX_RETRY`) kadar otomatik
 * olarak tekrar bağlanmayı dener.
 */

#if defined(CONFIG_NODE_ROUTER)

#include "wifi_sta.h"

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
#include "freertos/event_groups.h"
#include "forwarder.h"
#include "ip_utils.h"
#include "nvs_flash.h"
#include "pkt_buf.h"
#include "wifi_in.h"

/* -------------------------------------------------------------------------
 * Compatibility shim: ensure esp_netif_transmit() prototype is visible.
 * This is needed when the ESP‑NETIF component is not enabled via Kconfig.
 * ------------------------------------------------------------------------- */
#ifndef ESP_NETIF_TRANSMIT_DECLARED
#define ESP_NETIF_TRANSMIT_DECLARED
esp_err_t esp_netif_transmit(esp_netif_t *netif, void *buffer, size_t len);
#endif

static const char *TAG = "wifi_sta";

/** Wi-Fi bağlantısı kurulup IP alındığında set edilen bit */
#define WIFI_CONNECTED_BIT      BIT0

/** Wi-Fi bağlantısı başarısız olduğunda (deneme hakkı bittiğinde) set edilen bit */
#define WIFI_FAIL_BIT           BIT1

/** Maksimum yeniden bağlanma deneme sayısı */
#define WIFI_MAX_RETRY          10

/** Bağlantı için beklenilecek azami süre (milisaniye) */
#define WIFI_CONNECT_TIMEOUT_MS 30000

static esp_netif_t *s_netif;
static volatile bool s_connected;
static EventGroupHandle_t s_wifi_events;
static int s_retry_num;

/**
 * @brief Wi-Fi donanımından gelen ham Ethernet II çerçevelerini dinleyen RX kancası (hook).
 *
 * Havadan gelen paketleri LwIP'ye teslim edilmeden önce araya girerek yakalar.
 * Gelen paket IPv4/ICMP paketi ise bir `pkt_buf_t` tamponuna alır (Zero-Copy),
 * detaylarını ayrıştırıp konsola raporlar.
 * Ardından ağ yığıtının (DHCP, ARP vb.) bozulmaması için paketi standart
 * `esp_netif_receive` fonksiyonuna devreder.
 *
 * @param esp_netif Ağ arayüz tanıtıcısı.
 * @param buffer Gelen ham Ethernet II verisi.
 * @param len Veri uzunluğu.
 * @param eb Wi-Fi sürücüsü dahili tampon göstericisi.
 * @return esp_err_t ESP_OK veya hata kodu.
 */
static esp_err_t wifi_sta_rx_hook(esp_netif_t *esp_netif, void *buffer, size_t len, void *eb)
{
    /* Ethernet II çerçevesi: Hedef MAC (6), Kaynak MAC (6), EtherType (2) -> En az 14 bayt */
    if (buffer && len >= 14 + 20) {
        const uint8_t *eth_hdr = (const uint8_t *)buffer;
        const uint16_t ethertype = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];

        /* Sadece IPv4 (0x0800) paketlerini filtrele */
        if (ethertype == 0x0800) {
            const uint8_t *ip_pkt = eth_hdr + 14;
            const size_t ip_len = len - 14;

            ip_packet_info_t info;
            if (ip_utils_parse(ip_pkt, ip_len, &info)) {
                /* DHCP (UDP 67/68) paketleri yerel netif tarafından işlendiğinden filtrele */
                const bool is_dhcp = (info.protocol == 17 && (info.src_port == 67 || info.src_port == 68 ||
                                                              info.dst_port == 67 || info.dst_port == 68));

                /* Yalnızca yönlendiricimizin WAN IP'sine gelen yanıtları Ingress kuyruğuna devret */
                const uint32_t wan_ip = wifi_sta_get_wan_ip();
                const bool is_to_wan = (wan_ip != 0 && info.dst_ip == wan_ip);

                if (!is_dhcp && is_to_wan) {
                    static uint32_t s_rx_ip_count = 0;
                    s_rx_ip_count++;

                    /* Zero-Copy: Ayrılmış tek bir paket tamponuna al */
                    pkt_buf_t *rx_buf = pkt_buf_alloc(0);
                    if (rx_buf) {
                        memcpy(rx_buf->data, ip_pkt, ip_len);
                        rx_buf->len = ip_len;
                        rx_buf->rx_if = NET_IF_WIFI_STA;
                        rx_buf->tx_if = NET_IF_NONE;
                        rx_buf->seq_num = (info.is_icmp && info.icmp_seq > 0) ? info.icmp_seq : s_rx_ip_count;

#if ENABLE_ICMP_LATENCY_METRICS
                        rx_buf->is_icmp = info.is_icmp;
                        rx_buf->lat.t_rx_hook = latency_now_us();
#endif

                        ESP_LOGI(TAG, "[STA-RX #%" PRIu32 "] AP'den Alindi: %s -> %s (%s, %u byte)",
                                 rx_buf->seq_num, info.src_str, info.dst_str, info.proto_name, (unsigned)rx_buf->len);

                        /* Paketi ara FreeRTOS kuyruk beklemesi olmaksızın doğrudan yönlendiriciye devret (Direct Dispatch) */
                        const esp_err_t fwd_err = forwarder_handle_packet(rx_buf);
                        if (fwd_err != ESP_OK) {
                            ESP_LOGE(TAG, "forwarder_handle_packet basarisiz: %s", esp_err_to_name(fwd_err));
                        }
                    }
                }
            }
        }
    }

    /* Paketin LwIP yığıtına teslimi (DHCP, ARP, TCP/IP akışının bozulmaması için) */
    return esp_netif_receive(esp_netif, buffer, len, eb);
}

/**
 * @brief Wi-Fi ve IP olaylarını (events) dinleyen işleyici fonksiyon.
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Bağlantı koptu, yeniden deneniyor (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "AP'ye bağlanılamadı, yeniden deneme sınırı aşıldı.");
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP alındı: " IPSTR " | Gateway: " IPSTR " | Netmask: " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), IP2STR(&ev->ip_info.netmask));
        s_retry_num = 0;
        s_connected = true;

        /* Wi-Fi RX Paket Yakalama kancasını (hook) bağla */
        wifi_netif_driver_t driver = (wifi_netif_driver_t)esp_netif_get_io_driver(s_netif);
        if (driver) {
            esp_wifi_register_if_rxcb(driver, wifi_sta_rx_hook, s_netif);
            ESP_LOGI(TAG, "Wi-Fi RX Paket Yakalama Kancasi (Hook) baglandi.");
        }

        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief NVS Flash birimini başlatır; bozulma veya versiyon uyuşmazlığında silip yeniden kurar.
 */
static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS yeniden oluşturuluyor (%s)...", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    return err;
}

/**
 * @brief Wi-Fi STA modunu başlatır ve AP'ye bağlanana kadar bekler.
 */
esp_err_t wifi_sta_start(void)
{
    if (s_netif) {
        return ESP_OK; /* Zaten başlatılmış */
    }

    ESP_RETURN_ON_ERROR(nvs_init(), TAG, "NVS başlatılamadı");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init başarısız");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Default event loop oluşturulamadı");

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "Varsayılan STA netif oluşturulamadı");
        return ESP_FAIL;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init başarısız");

    /* Olay dinleyicilerini kaydet */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_handler, NULL, NULL),
        TAG, "WIFI_EVENT handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &wifi_event_handler, NULL, NULL),
        TAG, "IP_EVENT handler");

    /* Kimlik bilgileri Kconfig'ten geldiği için flash aşınmasını önlemek adına RAM'de tutulur */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage");

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_ROUTER_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_ROUTER_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    /* Wi-Fi Güç Tasarrufunu (Power Save / Modem Sleep) kapat -> Ping gecikmesini 2-4 ms'ye düşürür */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps (WIFI_PS_NONE)");

    ESP_LOGI(TAG, "'%s' AP'sine bağlanılıyor...", CONFIG_ROUTER_WIFI_SSID);

    /* Bağlantı kurulup IP alınana ya da zaman aşımı olana kadar bloklan */
    const EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                                 WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        /* Wi-Fi RX Paket Yakalama kancasını (hook) bağla */
        wifi_netif_driver_t driver = (wifi_netif_driver_t)esp_netif_get_io_driver(s_netif);
        if (driver) {
            esp_wifi_register_if_rxcb(driver, wifi_sta_rx_hook, s_netif);
            ESP_LOGI(TAG, "Wi-Fi RX Paket Yakalama Kancasi (Hook) baglandi.");
        }
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Bağlantı başarısız oldu");
    } else {
        ESP_LOGE(TAG, "Bağlantı zaman aşımına uğradı (%d ms)", WIFI_CONNECT_TIMEOUT_MS);
    }
    return ESP_FAIL;
}

/**
 * @brief Wi-Fi STA IPv4 adres bilgilerini sorgular.
 */
bool wifi_sta_get_ip_info(esp_netif_ip_info_t *info)
{
    if (!info || !s_netif || !s_connected) {
        return false;
    }
    if (esp_netif_get_ip_info(s_netif, info) != ESP_OK) {
        return false;
    }
    return info->ip.addr != 0;
}

/**
 * @brief Wi-Fi STA esp_netif nesnesini döndürür.
 */
esp_netif_t *wifi_sta_get_netif(void)
{
    return s_netif;
}

uint32_t wifi_sta_get_wan_ip(void)
{
    esp_netif_ip_info_t info;
    if (wifi_sta_get_ip_info(&info) && info.ip.addr != 0) {
        return info.ip.addr;
    }
    return 0;
}

esp_err_t wifi_sta_send(pkt_buf_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }

    ip_packet_info_t info;
    const bool parsed = ip_utils_parse(buf->data, buf->len, &info);

    if (!s_connected || !s_netif) {
        ESP_LOGW(TAG, "[STA-TX #%" PRIu32 "] Wi-Fi STA bagli degil, paket dusuruldu.", buf->seq_num);
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Wi-Fi sürücüsü (esp_wifi_internal_tx), fiziksel havaya aktarılmak üzere
     * Layer-2 (Ethernet II) çerçevesi bekler:
     * [Hedef MAC (AP BSSID - 6 byte)] [Kaynak MAC (STA - 6 byte)] [EtherType 0x0800 (IPv4 - 2 byte)] [IPv4 Verisi]
     */
    uint8_t eth_frame[14 + PKT_BUF_MAX_PAYLOAD];
    wifi_ap_record_t ap_info = { 0 };
    uint8_t sta_mac[6] = { 0 };

    esp_wifi_sta_get_ap_info(&ap_info);
    esp_wifi_get_mac(WIFI_IF_STA, sta_mac);

    memcpy(eth_frame, ap_info.bssid, 6);
    memcpy(eth_frame + 6, sta_mac, 6);
    eth_frame[12] = 0x08;
    eth_frame[13] = 0x00;
    memcpy(eth_frame + 14, buf->data, buf->len);

    const size_t eth_len = buf->len + 14;
    esp_err_t err = esp_netif_transmit(s_netif, eth_frame, eth_len);

#if ENABLE_ICMP_LATENCY_METRICS
    if (buf->is_icmp) {
        buf->lat.t_tx_hw_done = latency_now_us();
        latency_report_sta_egress(buf->seq_num, buf->len, &buf->lat);
    }
#endif

    if (parsed) {
        ESP_LOGI(TAG, "[STA-TX #%" PRIu32 "] WAN Gateway'e Iletildi: %s -> %s (%s, %u byte) | TX: %s",
                 buf->seq_num, info.src_str, info.dst_str, info.proto_name,
                 (unsigned)buf->len, (err == ESP_OK) ? "OK" : "ERR");
    } else {
        ESP_LOGI(TAG, "[STA-TX #%" PRIu32 "] WAN Gateway'e Iletildi: %u byte | TX: %s",
                 buf->seq_num, (unsigned)buf->len, (err == ESP_OK) ? "OK" : "ERR");
    }

    pkt_buf_free(buf);
    return err;
}

#endif /* CONFIG_NODE_ROUTER */
