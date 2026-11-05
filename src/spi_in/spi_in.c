/**
 * @file spi_in.c
 * @brief SPI Giriş Katmanı (SPI In / Ingress) Gerçeklemesi.
 *
 * SPI hattından yönlendiriciye doğru gelen paket akışını sağlar.
 * Donanım HSPI Slave DMA pre-queue mimarisi ile sıfır kopyalama (Zero-Copy)
 * prensibinde doğrudan yönlendirici geri çağırımına (s_rx_cb) aktarılır.
 * SPI Çerçeveleme (Magic, Length, CRC16) ve hata koruması içerir.
 * Gereksiz ara FreeRTOS kuyrukları kaldırılmıştır.
 */

#include "spi_in.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/spi_slave.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ip_utils.h"
#include "pkt_buf.h"
#include "spi_link.h"

static const char *TAG = "spi_in";

/** Donanım seviyesinde aynı anda hazır bekletilecek DMA alım tamponu sayısı */
#define SPI_SLAVE_PREQUEUE_COUNT 3

/** Gelen paketleri karşılayacak yönlendirici callback'i */
static spi_link_rx_cb_t s_rx_cb = NULL;

/**
 * @brief Donanım SPI Slave alıcı görevi (HSPI / SPI2_HOST).
 *
 * Karşıdaki ESP32 (Master) veri gönderene kadar bloklanır.
 * Donanım DMA hattında her zaman hazır bir tampon bulundurmak için
 * çoklu kuyruklama (pre-queueing) tekniği kullanılır. Bir transfer biter bitmez
 * diğer hazır transfer devreye girer; böylece paket kaybı önlenir.
 * Gelen paketin SPI Çerçeve başlığı (Magic + CRC16) kontrol edilir.
 *
 * @param arg Task argümanı (kullanılmıyor).
 */
static void spi_slave_rx_task(void *arg)
{
    ESP_LOGI(TAG, "spi_slave_rx_task basladi (Donanim HSPI Slave Pre-Queue ve CRC Kontrolu aktif)");

    static uint32_t s_rx_hw_count = 0;
    spi_slave_transaction_t trans[SPI_SLAVE_PREQUEUE_COUNT];
    pkt_buf_t *bufs[SPI_SLAVE_PREQUEUE_COUNT] = { NULL };

    /* Başlangıçta donanım DMA kuyruğunu doldur */
    for (int i = 0; i < SPI_SLAVE_PREQUEUE_COUNT; i++) {
        bufs[i] = pkt_buf_alloc(0);
        if (!bufs[i]) {
            ESP_LOGE(TAG, "spi_slave_rx_task: baslangic tamponu tahsis edilemedi!");
            vTaskDelete(NULL);
            return;
        }
        memset(&trans[i], 0, sizeof(spi_slave_transaction_t));
        trans[i].length = PKT_BUF_MAX_PAYLOAD * 8;
        trans[i].rx_buffer = bufs[i]->data;
        trans[i].user = (void *)(intptr_t)i;
        spi_slave_queue_trans(HSPI_RX_HOST, &trans[i], portMAX_DELAY);
    }

    for (;;) {
        spi_slave_transaction_t *ret_trans = NULL;
        esp_err_t ret = spi_slave_get_trans_result(HSPI_RX_HOST, &ret_trans, portMAX_DELAY);
        if (ret == ESP_OK && ret_trans != NULL) {
            const int idx = (int)(intptr_t)ret_trans->user;
            pkt_buf_t *received = bufs[idx];

            if (ret_trans->trans_len > 0) {
                const size_t raw_len = ret_trans->trans_len / 8;
                bool valid_packet = false;

                /* 1. SPI Çerçeveleme Başlığı ve CRC16 Kontrolü */
                if (raw_len >= sizeof(spi_frame_hdr_t)) {
                    const spi_frame_hdr_t *hdr = (const spi_frame_hdr_t *)received->data;
                    if (hdr->magic == SPI_FRAME_MAGIC && hdr->len <= PKT_BUF_MAX_PAYLOAD &&
                        raw_len >= (sizeof(spi_frame_hdr_t) + hdr->len)) {
                        /* CRC16 Doğrulaması */
                        const uint16_t calc_crc = ip_utils_checksum(received->data + sizeof(spi_frame_hdr_t), hdr->len);
                        if (calc_crc == hdr->crc16) {
                            /* Başlık alanlarını memmove öncesi yerel değişkenlere kopyala (in-place kaydırma başlığı ezer) */
                            const uint16_t pkt_len = hdr->len;
                            const uint16_t pkt_seq = hdr->seq;

                            /* Veriyi yerinde (in-place) başa kaydırarak IP paketine dönüştür */
                            memmove(received->data, received->data + sizeof(spi_frame_hdr_t), pkt_len);
                            received->len = pkt_len;
                            received->seq_num = (pkt_seq > 0) ? pkt_seq : ++s_rx_hw_count;
                            valid_packet = true;
                        } else {
                            ESP_LOGW(TAG, "SPI CRC hatasi! Beklenen: 0x%04X, Hesaplanan: 0x%04X (Paket dusuruldu)",
                                     hdr->crc16, calc_crc);
                        }
                    }
                }

                /* 2. Geriye Dönük Uyumluluk: Çerçevesiz ham IPv4 paketi kontrolü */
                if (!valid_packet && raw_len >= 20 && (received->data[0] >> 4) == 4) {
                    received->len = raw_len;
                    received->seq_num = ++s_rx_hw_count;
                    valid_packet = true;
                }

#if ENABLE_ICMP_LATENCY_METRICS
                if (valid_packet) {
                    received->lat.t_rx_hook = latency_now_us();
                    if (received->len >= 20 && received->data[9] == 1) {
                        received->is_icmp = true;
                    }
                }
#endif

                /* Donanımın hiçbir zaman boş kalmaması için hemen yeni tampon bağla ve kuyruğa ver */
                pkt_buf_t *new_buf = pkt_buf_alloc(0);
                if (new_buf) {
                    bufs[idx] = new_buf;
                    memset(&trans[idx], 0, sizeof(spi_slave_transaction_t));
                    trans[idx].length = PKT_BUF_MAX_PAYLOAD * 8;
                    trans[idx].rx_buffer = new_buf->data;
                    trans[idx].user = (void *)(intptr_t)idx;
                    spi_slave_queue_trans(HSPI_RX_HOST, &trans[idx], portMAX_DELAY);
                } else {
                    ESP_LOGE(TAG, "Yeni SPI RX tamponu alinamadi!");
                }

                /* Alınan geçerli paketi doğrudan yönlendiriciye devret (Zero-Copy) */
                if (valid_packet && s_rx_cb) {
                    received->rx_if = NET_IF_SPI_RX;
                    received->tx_if = NET_IF_NONE;
                    esp_err_t cb_err = s_rx_cb(received);
                    if (cb_err != ESP_OK) {
                        ESP_LOGE(TAG, "spi_slave_rx_task: s_rx_cb basarisiz: %s", esp_err_to_name(cb_err));
                    }
                } else {
                    pkt_buf_free(received);
                }
            } else {
                /* Boş transfer: aynı tamponu tekrar donanıma bağla */
                memset(&trans[idx], 0, sizeof(spi_slave_transaction_t));
                trans[idx].length = PKT_BUF_MAX_PAYLOAD * 8;
                trans[idx].rx_buffer = received->data;
                trans[idx].user = (void *)(intptr_t)idx;
                spi_slave_queue_trans(HSPI_RX_HOST, &trans[idx], portMAX_DELAY);
            }
        }
    }
}

esp_err_t spi_in_start(spi_link_rx_cb_t rx_cb)
{
    if (!rx_cb) {
        ESP_LOGE(TAG, "spi_in_start: rx_cb NULL olamaz");
        return ESP_ERR_INVALID_ARG;
    }

    s_rx_cb = rx_cb;

    /* 1. HSPI (SPI2_HOST) Slave Veri Yolu (Bus) Yapılandırması */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = HSPI_RX_PIN_MOSI,
        .miso_io_num = HSPI_RX_PIN_MISO,
        .sclk_io_num = HSPI_RX_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PKT_BUF_MAX_PAYLOAD,
        .flags = SPICOMMON_BUSFLAG_SLAVE,
        .intr_flags = 0,
    };

    /* 2. SPI Slave Arayüz Parametreleri */
    const spi_slave_interface_config_t slv_cfg = {
        .spics_io_num = HSPI_RX_PIN_CS,     /* CS pini (GPIO 15) */
        .flags = 0,
        .queue_size = 4,
        .mode = 0,                          /* SPI Mode 0 (CPOL=0, CPHA=0) */
        .post_setup_cb = NULL,
        .post_trans_cb = NULL,
    };

    esp_err_t err = spi_slave_initialize(HSPI_RX_HOST, &bus_cfg, &slv_cfg, HSPI_RX_DMA_CHAN);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_slave_initialize (HSPI Slave) basarisiz: %s", esp_err_to_name(err));
        return err;
    }

    /* SPI Slave alıcı görevini Core 1'e sabitle (Core 0 Wi-Fi sürücüsüne tahsisli kalır).
     * Yığın boyutu, alım anında doğrudan çağrılan L2/L3 iletim (Wi-Fi driver tx)
     * ve NAPT yığını için güvenli marj sağlayacak şekilde 8192 bayta genişletilmiştir. */
    if (xTaskCreatePinnedToCore(spi_slave_rx_task, "spi_slv_rx", 8192, NULL, 10, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "spi_in_start: spi_slave_rx_task olusturulamadi");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SPI Ingress (SLAVE) hazir | Host=%s | SCLK=GPIO%d MISO=GPIO%d MOSI=GPIO%d CS=GPIO%d (Core: 1, CRC Framing Aktif)",
             HSPI_RX_HOST_NAME, HSPI_RX_PIN_SCLK, HSPI_RX_PIN_MISO, HSPI_RX_PIN_MOSI, HSPI_RX_PIN_CS);
    return ESP_OK;
}

esp_err_t spi_in_receive(pkt_buf_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_rx_cb) {
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    buf->rx_if = NET_IF_SPI_RX;
    buf->tx_if = NET_IF_NONE;

    /* Doğrudan callback'e teslim edilir (Sahiplik router'a geçer, sıfır ara kuyruk gecikmesi) */
    return s_rx_cb(buf);
}
