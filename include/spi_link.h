/**
 * @file spi_link.h
 * @brief Birleşik SPI Bağlantı Katmanı (SPI Link Facade) Arayüzü.
 *
 * SPI giriş (`spi_in`) ve çıkış (`spi_out`) modüllerini tek bir çatı altında
 * toplayan genel SPI yönetim ve veri aktarım arayüzüdür.
 * Tüm veri aktarımı `pkt_buf_t` işaretçileri üzerinden kopyalamasız (zero-copy) yürütülür.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pkt_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Tek bir IP paketi için kabul edilen azami bayt sınırı (MTU boyutu) */
#define SPI_LINK_MAX_PKT_LEN 1500

/** SPI Çerçeveleme Başlatıcı Sihirli Bayt Değeri (Magic Word) */
#define SPI_FRAME_MAGIC 0xA55A

/**
 * @brief SPI hattında gürültü ve kısmi transfer tespiti için çerçeve başlığı.
 */
typedef struct __attribute__((packed)) {
    uint16_t magic;    /**< Çerçeve sihirli belirteci (0xA55A) */
    uint16_t len;      /**< IP paketi bayt uzunluğu */
    uint16_t crc16;    /**< Paket verisi RFC 1071 sağlama toplamı */
    uint16_t seq;      /**< Çerçeve sıra numarası */
} spi_frame_hdr_t;

/**
 * @brief SPI hattından gelen paket tamponunu yönlendiriciye ileten geri çağırım tipi.
 *
 * @param buf Ham IP paketini ve metaverilerini tutan tampon işaretçisi.
 * @return esp_err_t ESP_OK → paket kabul edildi.
 */
typedef esp_err_t (*spi_link_rx_cb_t)(pkt_buf_t *buf);

/**
 * @brief SPI linkini (giriş ve çıkış kanallarını) başlatır.
 *
 * @param rx_cb Gelen paketleri işleyecek router callback fonksiyonu.
 * @return esp_err_t ESP_OK veya başlatma hatası.
 */
esp_err_t spi_link_start(spi_link_rx_cb_t rx_cb);

/**
 * @brief Router tarafından SPI hattına bir paket göndermek için çağrılır.
 *
 * Sahiplik Kuralı (Ownership):
 * `buf` işaretçisi doğrudan TX kuyruğuna alınır. Gönderim tamamlandığında
 * donanım/task seviyesinde `pkt_buf_free()` ile serbest bırakılır.
 *
 * @param buf Gönderilecek paket tamponu işaretçisi.
 * @return esp_err_t ESP_OK veya kuyruk dolu/argüman geçersiz hatası.
 */
esp_err_t spi_link_send(pkt_buf_t *buf);

#ifdef __cplusplus
}
#endif
