/**
 * @file node_ap.c
 * @brief 2. ESP32 (SoftAP Köprü Düğümü) Uygulama ve Akış Gerçeklemesi.
 *
 * Bu modül, SoftAP (Erişim Noktası) rolünü üstlenen 2. ESP32'nin tüm sistem adımlarını yürütür:
 * 1. SPI Link katmanını başlatır ve SPI'den gelen paketleri tek merkezi yönlendiriciye
 *    (`node_ap_forward_packet`) bağlar.
 * 2. Wi-Fi SoftAP ağını (`10.54.0.1/24`, SSID: `Esp32_AP`) ayağa kaldırır.
 * 3. Wi-Fi AP istemcilerinden veya SPI giriş hattından gelen tüm IP/ICMP paketlerini
 *    `node_ap_forward_packet` fonksiyonunda ayrıştırarak yönlendirme kararını verir,
 *    döngü (loop) engellemesini uygular ve ilgili çıkış arayüzüne teslim eder.
 */

#if defined(CONFIG_NODE_AP)

#include "node_ap.h"

#include <inttypes.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ip_utils.h"
#include "pkt_buf.h"
#include "spi_link.h"
#include "wifi_ap.h"

static const char *TAG = "node_ap";

/**
 * @brief 2. ESP32 (SoftAP Düğümü) Tek Merkezi Paket Yönlendirme Fonksiyonu.
 *
 * Wi-Fi AP istemcilerinden veya SPI giriş hattından gelen tüm paketler tek bu fonksiyon
 * üzerinden incelenir:
 * - Döngü engelleme (Loop Prevention) kontrolleri (TTL kontrolü, aynı arayüze basma engeli) yapılır.
 * - TTL değeri bir azaltılır ve IP checksum yenilenir.
 * - Hedef IP adresi analizi:
 *   - Wi-Fi AP'den gelip WAN yönüne giden paketler -> SPI_TX (`spi_link_send`) ile 1. ESP32'ye iletilir.
 *   - SPI'den gelip yerel SoftAP istemcisine (`10.54.0.x`) giden yanıtlar -> `wifi_ap_send` ile istemciye iletilir.
 *   - SPI'den gelip yerel ağda olmayan paketler -> Loop oluşturmaması için düşürülür.
 *
 * @param buf Yönlendirilecek paket tamponu (Sahiplik bu fonksiyona geçer).
 * @return esp_err_t Başarılı ise ESP_OK.
 */
esp_err_t node_ap_forward_packet(pkt_buf_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }

#if ENABLE_ICMP_LATENCY_METRICS
    if (buf->is_icmp) {
        buf->lat.t_fwd_start = latency_now_us();
    }
#endif

    /* 1. Paketi ayrıştır */
    ip_packet_info_t info;
    if (!ip_utils_parse(buf->data, buf->len, &info)) {
        ESP_LOGE(TAG, "[AP-Forwarder #%" PRIu32 "] Gecersiz veya bozuk IPv4 paketi, dusuruluyor.", buf->seq_num);
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    /* 2. DÖNGÜ (LOOP) ENGELLEME KONTROLÜ - 1: TTL Kontrolü (RFC 791) */
    if (info.ttl <= 1) {
        ESP_LOGW(TAG, "[AP-Forwarder #%" PRIu32 " | LOOP ENGELLENDI] Paket TTL suresi doldu (TTL=%u), paket dusuruldu.",
                 buf->seq_num, info.ttl);
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    /* 3. SoftAP Yerel Alt Ağ Tanımları (10.54.0.0/24) */
    const uint32_t ap_net = esp_ip4addr_aton("10.54.0.0");
    const uint32_t ap_mask = esp_ip4addr_aton("255.255.255.0");
    const uint32_t ap_self_ip = esp_ip4addr_aton("10.54.0.1");

    const bool dst_is_local = ((info.dst_ip & ap_mask) == (ap_net & ap_mask));
    const bool dst_is_self = (info.dst_ip == ap_self_ip);

    /* TTL bir azaltılır, TCP SYN paketlerinde MSS 1400'e sınırlanır ve checksum yenilenir */
    buf->data[8]--;
    if (info.protocol == 6) {
        ip_utils_clamp_mss(buf->data, buf->len, 1400);
    }
    ip_utils_update_checksums(buf->data, buf->len);

#if ENABLE_ICMP_LATENCY_METRICS
    if (buf->is_icmp) {
        buf->lat.t_fwd_done = latency_now_us();
    }
#endif

    /* -------------------------------------------------------------------------
     * SENARYO A: Paket Wi-Fi SoftAP İstemcisinden Geldi (rx_if == NET_IF_WIFI_AP)
     * ------------------------------------------------------------------------- */
    if (buf->rx_if == NET_IF_WIFI_AP) {
        /* DÖNGÜ (LOOP) ENGELLEME KONTROLÜ - 2:
         * Eğer hedef SoftAP'nin kendi yerel IP'si ise (10.54.0.1), yerel LwIP stack
         * zaten bu paketi almıştır; SPI'ye ASLA gönderilmemelidir. */
        if (dst_is_self) {
            pkt_buf_free(buf);
            return ESP_OK;
        }

        /* Eğer hedef başka bir yerel istemci ise (10.54.0.x), Wi-Fi üzerinden ona ilet */
        if (dst_is_local) {
            buf->tx_if = NET_IF_WIFI_AP;
            ESP_LOGI(TAG, "[AP-FWD #%" PRIu32 "] Yerel Yonlendirme -> Cikis: %s (%s -> %s)",
                     buf->seq_num, net_if_to_name(buf->tx_if), info.src_str, info.dst_str);
            return wifi_ap_send(buf);
        }

        /* Hedef yerel ağın dışı (WAN / Internet / Dış Ağ Geçidi): SPI ile 1. ESP32'ye (esp-sta) aktar */
        buf->tx_if = NET_IF_SPI_TX;
        ESP_LOGI(TAG, "[AP-FWD #%" PRIu32 "] WAN Yonlendirme -> Cikis: %s (%s -> %s)",
                 buf->seq_num, net_if_to_name(buf->tx_if), info.src_str, info.dst_str);
        return spi_link_send(buf);
    }

    /* -------------------------------------------------------------------------
     * SENARYO B: Paket SPI Giriş Hattından Geldi (rx_if == NET_IF_SPI_RX)
     * ------------------------------------------------------------------------- */
    else if (buf->rx_if == NET_IF_SPI_RX) {
        /* DÖNGÜ (LOOP) ENGELLEME KONTROLÜ - 3:
         * SPI'den gelen paketin hedefi yerel SoftAP ağında (10.54.0.x) DEĞİLSE,
         * bu paket kesinlikle bir yönlendirme hatası veya döngüdür.
         * Bunu ASLA tekrar SPI'ye atmamalıyız, doğrudan DÜŞÜRMELİYİZ! */
        if (!dst_is_local) {
            ESP_LOGW(TAG, "[AP-FWD #%" PRIu32 " | LOOP ENGELLENDI] SPI'den gelen hedef (%s) yerel ag disinda, dusuruldu.",
                     buf->seq_num, info.dst_str);
            pkt_buf_free(buf);
            return ESP_ERR_INVALID_STATE;
        }

        /* Hedef yerel istemci (10.54.0.x): Wi-Fi üzerinden istemciye teslim et */
        buf->tx_if = NET_IF_WIFI_AP;
        ESP_LOGI(TAG, "[AP-FWD #%" PRIu32 "] SPI Yaniti -> Cikis: %s (Istemci: %s)",
                 buf->seq_num, net_if_to_name(buf->tx_if), info.dst_str);
        return wifi_ap_send(buf);
    }

    /* -------------------------------------------------------------------------
     * Tanımsız Giriş Arayüzü -> Düşür
     * ------------------------------------------------------------------------- */
    else {
        ESP_LOGW(TAG, "[AP-FWD] Tanimsiz giris arayuzu (%d), dusuruldu.", buf->rx_if);
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_ARG;
    }
}

#if defined(CONFIG_ENABLE_SYNTHETIC_TEST_PACKETS)
/**
 * @brief Test amaçlı sentetik paket üreticisi (İsteğe bağlı).
 */
static void ap_test_packet_task(void *arg)
{
    (void)arg;
    uint16_t seq = 0;
    uint16_t ip_id = 1;
    uint32_t packet_count = 0;
    const uint32_t src_ip = esp_ip4addr_aton("10.54.0.2");
    const uint32_t dst_ip = esp_ip4addr_aton("192.168.50.1");
    const uint8_t payload[] = "EdgeRouter_AP_Ping_Test_Payload";

    vTaskDelay(pdMS_TO_TICKS(8000));

    for (;;) {
        pkt_buf_t *buf = pkt_buf_alloc(0);
        if (!buf) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        const size_t n = ip_utils_build_icmp_echo(buf->data, buf->capacity,
                                                  src_ip, dst_ip,
                                                  ip_id++, 0xABCD, ++seq,
                                                  payload, sizeof(payload));
        if (n == 0) {
            pkt_buf_free(buf);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        buf->len = n;
        buf->rx_if = NET_IF_WIFI_AP;
        buf->tx_if = NET_IF_NONE;
        buf->seq_num = ++packet_count;

        node_ap_forward_packet(buf);
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
#endif /* CONFIG_ENABLE_SYNTHETIC_TEST_PACKETS */

esp_err_t node_ap_start(void)
{
    printf("\n\n");
    fflush(stdout);
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, ">>> [2. ESP32] SoftAP & SPI Koprusu Baslatiliyor <<<");
    ESP_LOGI(TAG, "==================================================");

    /* -------------------------------------------------------------------------
     * [Adım 1/3] SPI Link Katmanını Başlat ve Merkezi Yönlendiriciye Bağla
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 1/3] SPI Link katmani baslatiliyor (Merkezi Forwarder entegrasyonu)...");
    const esp_err_t spi_err = spi_link_start(node_ap_forward_packet);
    if (spi_err == ESP_OK) {
        ESP_LOGI(TAG, "[Adim 1/3] SPI Link Baslatildi: BASARILI");
    } else {
        ESP_LOGE(TAG, "[Adim 1/3] SPI Link Baslatilamadi: %s", esp_err_to_name(spi_err));
        return spi_err;
    }

    /* -------------------------------------------------------------------------
     * [Adım 2/3] Wi-Fi SoftAP Ağını Başlat (SSID: Esp32_AP, IP: 10.54.0.1)
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 2/3] Wi-Fi SoftAP baslatiliyor...");
    const esp_err_t ap_err = wifi_ap_start();
    if (ap_err == ESP_OK) {
        ESP_LOGI(TAG, "[Adim 2/3] Wi-Fi SoftAP Baslatildi: BASARILI");
    } else {
        ESP_LOGE(TAG, "[Adim 2/3] Wi-Fi SoftAP Baslatilamadi: %s", esp_err_to_name(ap_err));
        return ap_err;
    }

    /* -------------------------------------------------------------------------
     * [Adım 3/3] Otomatik Sentetik Görev Durumu
     * ------------------------------------------------------------------------- */
#if defined(CONFIG_ENABLE_SYNTHETIC_TEST_PACKETS)
    ESP_LOGI(TAG, "[Adim 3/3] AP Test Paket Ureteci gorevi olusturuluyor...");
    xTaskCreate(ap_test_packet_task, "ap_test_pkt", 4096, NULL, 5, NULL);
#else
    ESP_LOGI(TAG, "[Adim 3/3] Otomatik Paket Ureteci PASIF (Yalnizca Gercek Istemci ve SPI Trafiki Dinlemede)");
#endif

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "2. ESP32 AP Node devrede. Istemciler baglanabilir!");
    ESP_LOGI(TAG, "    |-- SSID   : %s", DEFAULT_AP_SSID);
    ESP_LOGI(TAG, "    |-- Parola : %s", DEFAULT_AP_PASS);
    ESP_LOGI(TAG, "    \\-- Ag     : %s/24", DEFAULT_AP_IP);
    ESP_LOGI(TAG, "==================================================");

    return ESP_OK;
}

#endif /* CONFIG_NODE_AP */
