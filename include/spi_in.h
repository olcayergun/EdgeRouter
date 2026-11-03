/**
 * @file spi_in.h
 * @brief SPI Giriş Katmanı (SPI In / Ingress) Arayüzü.
 *
 * SPI donanımından gelen IP paketlerini karşılayıp
 * router çekirdeğine paket tamponu adresi (`pkt_buf_t *`) ile teslim eden akışı yönetir.
 */

#pragma once

#include "esp_err.h"
#include "pkt_buf.h"
#include "spi_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name HSPI (SPI2_HOST) Alış (RX Ingress / SLAVE) Donanım Tanımları
 * @{
 */
#define HSPI_RX_HOST        SPI2_HOST
#define HSPI_RX_HOST_NAME   "HSPI (SPI2_HOST) [SLAVE]"
#define HSPI_RX_PIN_SCLK    14
#define HSPI_RX_PIN_MISO    12
#define HSPI_RX_PIN_MOSI    13
#define HSPI_RX_PIN_CS      15
#define HSPI_RX_DMA_CHAN    SPI_DMA_CH_AUTO
/** @} */

/**
 * @brief SPI giriş yönünü (SPI -> router) başlatır.
 *
 * SPI RX kuyruğunu ve alıcı görevini (`spi_rx_task`) oluşturur.
 *
 * @param rx_cb Gelen her geçerli paket tamponu için tetiklenecek geri çağırım (callback) fonksiyonu.
 * @return esp_err_t ESP_OK veya görev oluşturulamadıysa hata kodu.
 */
esp_err_t spi_in_start(spi_link_rx_cb_t rx_cb);

/**
 * @brief SPI hattından gelen bir paket tamponunu ingress katmanına teslim eder.
 *
 * @param buf SPI donanımından okunan paket tamponu (`pkt_buf_t *`).
 * @return esp_err_t ESP_OK veya hata kodu.
 */
esp_err_t spi_in_receive(pkt_buf_t *buf);

#ifdef __cplusplus
}
#endif
