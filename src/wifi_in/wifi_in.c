/**
 * @file wifi_in.c
 * @brief Wi-Fi Giriş Katmanı (Wi-Fi In / Ingress) Gerçeklemesi.
 *
 * Wi-Fi Station (WAN) arayüzünden yönlendiriciye doğru gelen paket akışını sağlar.
 * Dış ağdan gelen her paket için `pkt_buf_alloc` ile bir tampon oluşturulur ve veri
 * doğrudan bu tampona yazılır. Ardından yönlendiriciye yalnızca bu tamponun
 * bellek adresi (`pkt_buf_t *`) iletilir.
 */

#if defined(CONFIG_NODE_ROUTER)

#include "wifi_in.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ip_utils.h"
#include "pkt_buf.h"

static const char *TAG = "wifi_in";

/** Wi-Fi RX kuyruk derinliği */
#define WIFI_RX_QUEUE_DEPTH 16

/** Gelen paketleri karşılayacak yönlendirici callback'i */
static wifi_in_rx_cb_t s_rx_cb = NULL;

/** Gelen paket tamponları için FreeRTOS kuyruğu */
static QueueHandle_t s_rx_q = NULL;

/**
 * @brief Dış ağdan gelen paketleri dinleyen ve yönlendiriciye aktaran FreeRTOS görevi.
 *
 * @param arg Task argümanı (kullanılmıyor).
 */
static void wifi_rx_task(void *arg)
{
    pkt_buf_t *buf = NULL;

    ESP_LOGI(TAG, "wifi_rx_task basladi (Wi-Fi giris dinleyicisi aktif)");

    for (;;) {
        if (xQueueReceive(s_rx_q, &buf, portMAX_DELAY) == pdTRUE) {
            if (!buf) {
                continue;
            }

            /* Ayrıştırılmış paket detaylarını özet logla */
            ip_packet_info_t info;
            if (ip_utils_parse(buf->data, buf->len, &info)) {
                ESP_LOGI(TAG, "[STA-RX #%" PRIu32 "] Alindi: %s -> %s (%s, %u byte)",
                         buf->seq_num, info.src_str, info.dst_str, info.proto_name, (unsigned)buf->len);
            }

            /* Tampon adresini yönlendiriciye devret (Sahiplik forwarder'a geçer) */
            if (s_rx_cb) {
                const esp_err_t err = s_rx_cb(buf);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "router Wi-Fi paketini kabul etmedi: %s", esp_err_to_name(err));
                }
            } else {
                pkt_buf_free(buf);
            }
        }
    }
}

esp_err_t wifi_in_start(wifi_in_rx_cb_t rx_cb)
{
    if (!rx_cb) {
        return ESP_ERR_INVALID_ARG;
    }

    s_rx_cb = rx_cb;

    s_rx_q = xQueueCreate(WIFI_RX_QUEUE_DEPTH, sizeof(pkt_buf_t *));
    if (!s_rx_q) {
        ESP_LOGE(TAG, "wifi_in_start: s_rx_q kuyrugu olusturulamadi");
        return ESP_ERR_NO_MEM;
    }

    /* Wi-Fi RX task'ini Core 1'e sabitle (Core 0 Wi-Fi PHY/MAC interruptlarina tahsisli) */
    if (xTaskCreatePinnedToCore(wifi_rx_task, "wifi_rx", 4096, NULL, 7, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "wifi_in_start: wifi_rx_task olusturulamadi");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Wi-Fi giris yonu hazir | kuyruk derinligi=%d", WIFI_RX_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t wifi_in_receive(pkt_buf_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_rx_q) {
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    buf->rx_if = NET_IF_WIFI_STA;
    buf->tx_if = NET_IF_NONE;

    if (xQueueSend(s_rx_q, &buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "wifi_in_receive: RX kuyrugu dolu, paket dusuruldu");
        pkt_buf_free(buf);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#endif /* CONFIG_NODE_ROUTER */
