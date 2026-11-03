/**
 * @file node_router.c
 * @brief 1. ESP32 (EdgeRouter Düğümü) Uygulama ve Akış Gerçeklemesi.
 *
 * Bu modül, EdgeRouter rolünü üstlenen 1. ESP32'nin tüm sistem adımlarını yürütür:
 * 1. NAT tablosunu ve oturum motorunu başlatır.
 * 2. Wi-Fi Ingress katmanını başlatır.
 * 3. Wi-Fi Station (STA) arayüzünü başlatır ve AP'ye bağlanır.
 * 4. SPI Link katmanını başlatıp gelen paketleri forwarder_handle_packet'e bağlar.
 * 5. SPI arayüzünden paket geliyormuş gibi 20 saniyede bir sentetik ICMP paketi
 *    oluşturup SPI dinleme fonksiyonuna (`spi_in_receive`) teslim eder.
 */

#if defined(CONFIG_NODE_ROUTER)

#include "node_router.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "forwarder.h"
#include "ip_utils.h"
#include "nat.h"
#include "pkt_buf.h"
#include "spi_in.h"
#include "spi_link.h"
#include "wifi_in.h"
#include "wifi_sta.h"

static const char *TAG = "node_router";

#if defined(CONFIG_ENABLE_SYNTHETIC_TEST_PACKETS)
/**
 * @brief 20 saniyede bir sentetik ICMP paketi üreterek SPI dinleme katmanına aktaran test görevi.
 *
 * Üretilen paket `spi_in_receive(buf)` fonksiyonu ile doğrudan SPI giriş kuyruğuna aktarılır.
 * Böylece paket SPI dinleyicisi -> Forwarder -> NAT (SNAT) -> Wi-Fi STA iletim hattı
 * boyunca işlenir ve adımlar konsola loglanır.
 *
 * @param arg Task argümanı (kullanılmıyor).
 */
static void spi_test_generator_task(void *arg)
{
    ESP_LOGI(TAG, "SPI Test Paket Ureteci devrede (Her 20 sn'de bir paket SPI dinleyicisine verilecek)");

    uint16_t seq = 0;
    uint16_t ip_id = 1;
    uint32_t packet_count = 0;
    const uint32_t src_ip = esp_ip4addr_aton("10.54.0.2");
    const uint8_t payload[] = "EdgeRouter_ICMP_Ping_Payload";

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20000));

        /* Hedef IP olarak AP Gateway adresini al, henüz alınamadıysa varsayılan 192.168.50.1 kullan */
        uint32_t dst_ip = esp_ip4addr_aton("192.168.50.1");
        esp_netif_ip_info_t ip_info;
        if (wifi_sta_get_ip_info(&ip_info) && ip_info.gw.addr != 0) {
            dst_ip = ip_info.gw.addr;
        }

        /* 1. Paket için yeni bir bellek tamponu tahsis et (Zero-Copy) */
        pkt_buf_t *buf = pkt_buf_alloc(0);
        if (!buf) {
            ESP_LOGE(TAG, "[SPI Ureteci] Paket tamponu tahsis edilemedi!");
            continue;
        }

        /* 2. Tampon içerisine IPv4 ICMP Echo Request (Ping) paketini oluştur */
        const size_t n = ip_utils_build_icmp_echo(buf->data, buf->capacity,
                                                  src_ip, dst_ip,
                                                  ip_id++, 0xABCD, ++seq,
                                                  payload, sizeof(payload));
        if (n == 0) {
            ESP_LOGE(TAG, "[SPI Ureteci] ICMP paketi insa edilemedi!");
            pkt_buf_free(buf);
            continue;
        }

        buf->len = n;
        buf->rx_if = NET_IF_SPI_RX;
        buf->tx_if = NET_IF_NONE;
        buf->seq_num = ++packet_count;

        /* 3. Paketin detaylarını logla */
        ip_packet_info_t info;
        if (ip_utils_parse(buf->data, buf->len, &info)) {
            printf("\n\n");
            fflush(stdout);
            ESP_LOGI(TAG, "==================================================");
            ESP_LOGI(TAG, ">>> [Paket Ureteci #%" PRIu32 "] Paket Olusturuldu -> SPI Dinleyicisine (spi_in_receive) Aktariliyor...", buf->seq_num);
            ESP_LOGI(TAG, "    |-- Arayuz      : SPI Giris (HSPI / SPI2_HOST)");
            ESP_LOGI(TAG, "    |-- Kaynak IP   : %s (Ic Ag Cihazi)", info.src_str);
            ESP_LOGI(TAG, "    |-- Hedef IP    : %s (Gateway)", info.dst_str);
            ESP_LOGI(TAG, "    |-- Protokol    : %s (%s)", info.proto_name, info.type_desc);
            ESP_LOGI(TAG, "    |-- ICMP Bilgisi: ID=0x%04X, Seq=%" PRIu16, info.icmp_id, info.icmp_seq);
            ESP_LOGI(TAG, "    |-- Toplam Boyut: %u byte", (unsigned)buf->len);
            ESP_LOGI(TAG, "    \\-- Tampon Ref  : %p (Sahiplik SPI katmanina aktariliyor)", (void *)buf);
            ESP_LOGI(TAG, "==================================================");
        }

        /* 4. Paketi SPI dinleme fonksiyonuna teslim et (Sahiplik devredilir, zero-copy) */
        const esp_err_t err = spi_in_receive(buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[Paket Ureteci #%" PRIu32 "] spi_in_receive basarisiz: %s", packet_count, esp_err_to_name(err));
        }
    }
}
#endif /* CONFIG_ENABLE_SYNTHETIC_TEST_PACKETS */

esp_err_t node_router_start(void)
{
    printf("\n\n");
    fflush(stdout);
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, ">>> [1. ESP32] EdgeRouter Baslatiliyor <<<");
    ESP_LOGI(TAG, "==================================================");

    /* -------------------------------------------------------------------------
     * [Adım 1/5] NAT Tablosunu ve Oturum Motorunu Başlat
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 1/5] NAT tablosu ve oturum motoru baslatiliyor...");
    nat_init();
    ESP_LOGI(TAG, "[Adim 1/5] NAT Baslatildi: BASARILI");

    /* -------------------------------------------------------------------------
     * [Adım 2/5] Wi-Fi Giriş (Ingress) Katmanını Başlat ve Forwarder'a Bağla
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 2/5] Wi-Fi Giris katmani baslatiliyor (Forwarder entegrasyonu)...");
    const esp_err_t wifi_in_err = wifi_in_start(forwarder_handle_packet);
    if (wifi_in_err == ESP_OK) {
        ESP_LOGI(TAG, "[Adim 2/5] Wi-Fi Giris Katmani Baslatildi: BASARILI");
    } else {
        ESP_LOGE(TAG, "[Adim 2/5] Wi-Fi Giris Katmani Baslatilamadi: %s", esp_err_to_name(wifi_in_err));
    }

    /* -------------------------------------------------------------------------
     * [Adım 3/5] Wi-Fi Station Arayüzünü Başlat ve AP'ye Bağlan
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 3/5] Wi-Fi Station baslatiliyor (Hedef AP: '%s')...", CONFIG_ROUTER_WIFI_SSID);
    const esp_err_t wifi_err = wifi_sta_start();
    if (wifi_err == ESP_OK) {
        esp_netif_ip_info_t ip_info;
        if (wifi_sta_get_ip_info(&ip_info)) {
            ESP_LOGI(TAG, "[Adim 3/5] Wi-Fi Baglantisi: BASARILI");
            ESP_LOGI(TAG, "    |-- Alinan IP : " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "    |-- Ag Gecidi : " IPSTR, IP2STR(&ip_info.gw));
            ESP_LOGI(TAG, "    \\-- Netmask   : " IPSTR, IP2STR(&ip_info.netmask));
        } else {
            ESP_LOGI(TAG, "[Adim 3/5] Wi-Fi Baglantisi: BASARILI (IP bilgisi bekleniyor)");
        }
    } else {
        ESP_LOGE(TAG, "[Adim 3/5] Wi-Fi Baglantisi: BASARISIZ (%s)", esp_err_to_name(wifi_err));
    }

    /* -------------------------------------------------------------------------
     * [Adım 4/5] SPI Link Katmanını Başlat ve Forwarder'a Bağla
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 4/5] SPI Link katmani baslatiliyor (Forwarder entegrasyonu)...");
    const esp_err_t spi_err = spi_link_start(forwarder_handle_packet);
    if (spi_err == ESP_OK) {
        ESP_LOGI(TAG, "[Adim 4/5] SPI Link Baslatildi: BASARILI");
    } else {
        ESP_LOGE(TAG, "[Adim 4/5] SPI Link Baslatilamadi: %s", esp_err_to_name(spi_err));
    }

#if defined(CONFIG_ENABLE_SYNTHETIC_TEST_PACKETS)
    /* -------------------------------------------------------------------------
     * [Adım 5/5] SPI Test Paket Üreteci Task'ini Başlat (20 saniyede bir)
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 5/5] SPI Test Paket Ureteci gorevi olusturuluyor...");
    const BaseType_t task_ret = xTaskCreate(spi_test_generator_task, "spi_test_gen", 4096, NULL, 5, NULL);
    if (task_ret == pdPASS) {
        ESP_LOGI(TAG, "[Adim 5/5] SPI Test Paket Ureteci: BASARILI (Her 20 sn'de bir SPI dinleyicisine iletecek)");
    } else {
        ESP_LOGE(TAG, "[Adim 5/5] SPI Test Paket Ureteci: BASARISIZ (Gorev olusturulamadi)");
    }
#else
    /* -------------------------------------------------------------------------
     * [Adım 5/5] Sentetik Paket Üreteci Pasif (Yalnızca Gerçek Donanım SPI Dinleniyor)
     * ------------------------------------------------------------------------- */
    ESP_LOGI(TAG, "[Adim 5/5] SPI Test Paket Ureteci: PASIF (Gercek Donanim HSPI Slave Dinlemede)");
#endif

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "EdgeRouter baslatildi. Cift Yonlu Paket Yonlendirme Devrede.");
    ESP_LOGI(TAG, "==================================================");

    return ESP_OK;
}

#endif /* CONFIG_NODE_ROUTER */

