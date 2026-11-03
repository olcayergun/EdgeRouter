/**
 * @file spi_out.h
 * @brief SPI Çıkış Katmanı (SPI Out / Egress) Arayüzü.
 *
 * Yönlendiriciden SPI hattına doğru gönderilecek paketlerin kuyruklanması
 * ve aktarılması işlemlerini yönetir.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name VSPI (SPI3_HOST) Gönderim (TX Egress / MASTER) Donanım Tanımları
 * @{
 */
#define VSPI_TX_HOST        SPI3_HOST
#define VSPI_TX_HOST_NAME   "VSPI (SPI3_HOST) [MASTER]"
#define VSPI_TX_PIN_SCLK    18
#define VSPI_TX_PIN_MISO    19
#define VSPI_TX_PIN_MOSI    23
#define VSPI_TX_PIN_CS      5
#define VSPI_TX_DMA_CHAN    SPI_DMA_CH_AUTO
/** @} */

/**
 * @brief SPI çıkış yönünü (router -> SPI) başlatır.
 *
 * TX FreeRTOS kuyruğunu (`s_tx_q`) ve iletim görevini (`spi_tx_task`) oluşturur.
 * Paket göndermek için genel `spi_link_send()` fonksiyonu kullanılır.
 *
 * @return esp_err_t Başarılı ise ESP_OK, bellek yetersizliğinde ESP_ERR_NO_MEM.
 */
esp_err_t spi_out_start(void);

#ifdef __cplusplus
}
#endif
