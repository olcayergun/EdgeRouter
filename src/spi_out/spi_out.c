/**
 * @file spi_out.c
 * @brief SPI Çıkış Katmanı (SPI Out / Egress) Gerçeklemesi.
 *
 * Yönlendirici (EdgeRouter) üzerinden SPI hattına iletilmek istenen paket tamponlarını
 * (`pkt_buf_t *`) kopyalamasız (zero-copy) olarak FreeRTOS iletim kuyruğuna (`s_tx_q`)
 * ekler. `spi_tx_task` kuyruktan tampon adresini alarak aktarımı gerçekleştirir ve
 * iletim bittiğinde tamponu serbest bırakır (`pkt_buf_free`).
 * Hata koruması ve gürültü tespiti için SPI Çerçeveleme (Magic, Length, CRC16) ekler.
 */

#include "spi_out.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ip_utils.h"
#include "pkt_buf.h"
#include "spi_link.h"

static const char *TAG = "spi_out";

/** İletim kuyruğu derinliği (maksimum bekleyen paket tamponu sayısı) */
#define TX_QUEUE_DEPTH 16

/** Gönderim kuyruğu tanıtıcısı (pkt_buf_t* işaretçilerini saklar) */
static QueueHandle_t s_tx_q;

/** SPI Master cihaz tanıtıcısı (VSPI / SPI3_HOST) */
static spi_device_handle_t s_spi_dev = NULL;

/** SPI çerçeveli gönderim için ayrılmış DMA bellek tamponu */
static uint8_t *s_tx_dma_buf = NULL;

/**
 * @brief SPI hattına paket aktaran FreeRTOS görevi (Router -> SPI yönü, MASTER).
 *
 * Kuyruktan paket tamponu adresi gelene kadar bloklanır (`portMAX_DELAY`).
 * Tampon geldiğinde çerçeve başlığını (Magic, Length, CRC16, Seq) ekler ve
 * SPI Master donanımına iletir (spi_device_polling_transmit).
 * İletim bittiğinde `pkt_buf_free()` ile tampon belleğini serbest bırakır.
 *
 * @param arg Task argümanı (kullanılmıyor).
 */
static void spi_tx_task(void *arg)
{
    pkt_buf_t *buf;

    ESP_LOGI(TAG, "spi_tx_task basladi (VSPI Master TX aktif) | kuyruk derinligi=%d", TX_QUEUE_DEPTH);

    for (;;) {
        if (xQueueReceive(s_tx_q, &buf, portMAX_DELAY) == pdTRUE) {
#if ENABLE_ICMP_LATENCY_METRICS
            if (buf->is_icmp) {
                buf->lat.t_tx_q_out = latency_now_us();
            }
#endif
            ip_packet_info_t info;
            const bool parsed = ip_utils_parse(buf->data, buf->len, &info);

            /* Gerçek donanıma aktarım: SPI Framing (Magic + Len + CRC16) ile Polling Transmit */
            esp_err_t tx_err = ESP_FAIL;
            if (s_spi_dev && buf->len > 0) {
                const size_t frame_len = sizeof(spi_frame_hdr_t) + buf->len;
                /* ESP32 SPI Slave DMA denetleyicisi bayt kaybını önlemek için 4 bayt (32-bit word) katı iletim bekler */
                const size_t dma_len = (frame_len + 3) & ~3;
                if (s_tx_dma_buf && dma_len <= (sizeof(spi_frame_hdr_t) + PKT_BUF_MAX_PAYLOAD)) {
                    spi_frame_hdr_t *hdr = (spi_frame_hdr_t *)s_tx_dma_buf;
                    hdr->magic = SPI_FRAME_MAGIC;
                    hdr->len = (uint16_t)buf->len;
                    hdr->crc16 = ip_utils_checksum(buf->data, buf->len);
                    hdr->seq = (uint16_t)buf->seq_num;
                    memcpy(s_tx_dma_buf + sizeof(spi_frame_hdr_t), buf->data, buf->len);

                    /* 4-bayt hizalaması için eklenen 1-3 dolgu (padding) baytını sıfırla */
                    if (dma_len > frame_len) {
                        memset(s_tx_dma_buf + frame_len, 0, dma_len - frame_len);
                    }

                    spi_transaction_t trans = {
                        .flags = 0,
                        .cmd = 0,
                        .addr = 0,
                        .length = dma_len * 8, /* Bit cinsinden iletim boyutu (4 bayt katı) */
                        .rxlength = 0,
                        .user = NULL,
                        .tx_buffer = s_tx_dma_buf,
                        .rx_buffer = NULL,
                    };
                    tx_err = spi_device_polling_transmit(s_spi_dev, &trans);
                } else {
                    /* Fallback: Çerçeveleme tamponu tahsis edilemediyse ham aktar */
                    spi_transaction_t trans = {
                        .flags = 0,
                        .cmd = 0,
                        .addr = 0,
                        .length = buf->len * 8,
                        .rxlength = 0,
                        .user = NULL,
                        .tx_buffer = buf->data,
                        .rx_buffer = NULL,
                    };
                    tx_err = spi_device_polling_transmit(s_spi_dev, &trans);
                }
            }

#if ENABLE_ICMP_LATENCY_METRICS
            if (buf->is_icmp) {
                buf->lat.t_tx_hw_done = latency_now_us();
#if defined(CONFIG_NODE_AP)
                latency_report_ap_egress(buf->seq_num, buf->len, &buf->lat);
#elif defined(CONFIG_NODE_ROUTER)
                latency_report_sta_ingress(buf->seq_num, buf->len, &buf->lat);
#endif
            }
#endif

            if (parsed) {
                ESP_LOGI(TAG, "[SPI-TX #%" PRIu32 "] Gonderildi (Framed): %s -> %s (%s, %u byte) | TX: %s",
                         buf->seq_num, info.src_str, info.dst_str, info.proto_name,
                         (unsigned)buf->len, (tx_err == ESP_OK) ? "OK" : "ERR");
            } else {
                ESP_LOGI(TAG, "[SPI-TX #%" PRIu32 "] Gonderildi: %u byte | TX: %s",
                         buf->seq_num, (unsigned)buf->len, (tx_err == ESP_OK) ? "OK" : "ERR");
            }

            /* Gönderim tamamlandı; tampon artık serbest bırakılabilir */
            pkt_buf_free(buf);

            /* Karşıdaki Slave DMA'sına sonraki tampona geçiş için mikro-saniye payı (50 us) */
            esp_rom_delay_us(50);
        }
    }
}

esp_err_t spi_link_send(pkt_buf_t *buf)
{
    if (!s_tx_q) {
        ESP_LOGE(TAG, "spi_link_send: kuyruk henüz oluşturulmamış");
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_STATE;
    }
    if (!buf || buf->len == 0 || buf->len > SPI_LINK_MAX_PKT_LEN) {
        ESP_LOGE(TAG, "spi_link_send: geçersiz argüman veya hatalı boyut");
        pkt_buf_free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    buf->tx_if = NET_IF_SPI_TX;

#if ENABLE_ICMP_LATENCY_METRICS
    if (buf->is_icmp) {
        buf->lat.t_tx_q_in = latency_now_us();
    }
#endif

    /* Tampon işaretçisini doğrudan kuyruğa ekle (Zero-Copy) */
    if (xQueueSend(s_tx_q, &buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "spi_link_send: TX kuyruğu dolu (depth=%d), paket düşürüldü", TX_QUEUE_DEPTH);
        pkt_buf_free(buf);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "spi_link_send: %u byte tampon adresi kuyruğa eklendi", (unsigned)buf->len);
    return ESP_OK;
}

esp_err_t spi_out_start(void)
{
    if (s_tx_q) {
        return ESP_OK; /* Zaten başlatılmış */
    }

    /* 1. SPI Çerçeveli iletim için DMA özellikli tampon tahsis et */
    if (!s_tx_dma_buf) {
        s_tx_dma_buf = (uint8_t *)heap_caps_malloc(sizeof(spi_frame_hdr_t) + PKT_BUF_MAX_PAYLOAD,
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
        if (!s_tx_dma_buf) {
            ESP_LOGW(TAG, "spi_out_start: DMA TX tamponu tahsis edilemedi, standart malloc deneniyor.");
            s_tx_dma_buf = (uint8_t *)malloc(sizeof(spi_frame_hdr_t) + PKT_BUF_MAX_PAYLOAD);
        }
    }

    /* 2. VSPI (SPI3_HOST) Master Veri Yolu (Bus) Yapılandırması */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = VSPI_TX_PIN_MOSI,
        .miso_io_num = VSPI_TX_PIN_MISO,
        .sclk_io_num = VSPI_TX_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(spi_frame_hdr_t) + PKT_BUF_MAX_PAYLOAD,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = 0,
    };

    esp_err_t err = spi_bus_initialize(VSPI_TX_HOST, &bus_cfg, VSPI_TX_DMA_CHAN);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize (VSPI Master) basarisiz: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. SPI Cihaz Parametreleri (Master Cihaz Ekleme) */
    const spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,                          /* SPI Mode 0 (CPOL=0, CPHA=0) */
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = 10 * 1000 * 1000, /* 10 MHz saat frekansı */
        .input_delay_ns = 0,
        .spics_io_num = VSPI_TX_PIN_CS,     /* CS pini (GPIO 5) */
        .flags = 0,
        .queue_size = 3,
        .pre_cb = NULL,
        .post_cb = NULL,
    };

    err = spi_bus_add_device(VSPI_TX_HOST, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device (VSPI Master) basarisiz: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Kuyruk pkt_buf_t* işaretçilerini taşıyacak şekilde oluşturulur */
    s_tx_q = xQueueCreate(TX_QUEUE_DEPTH, sizeof(pkt_buf_t *));
    if (!s_tx_q) {
        ESP_LOGE(TAG, "spi_out_start: TX kuyruğu oluşturulamadı");
        return ESP_ERR_NO_MEM;
    }

    /* FreeRTOS Görevini Core 1'e ve yüksek önceliğe (10) sabitle */
    if (xTaskCreatePinnedToCore(spi_tx_task, "spi_tx", 4096, NULL, 10, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "spi_out_start: spi_tx_task oluşturulamadı");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SPI Egress (MASTER) hazir | Host=%s | SCLK=GPIO%d MISO=GPIO%d MOSI=GPIO%d CS=GPIO%d (Clock: 10MHz | Core: 1, CRC Framing Aktif)",
             VSPI_TX_HOST_NAME, VSPI_TX_PIN_SCLK, VSPI_TX_PIN_MISO, VSPI_TX_PIN_MOSI, VSPI_TX_PIN_CS);
    return ESP_OK;
}
