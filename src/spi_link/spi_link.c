/**
 * @file spi_link.c
 * @brief Birleşik SPI Bağlantı Katmanı (SPI Link Facade) Gerçeklemesi.
 *
 * Bu modül, yönlendirici ana kodunun doğrudan `spi_in` ve `spi_out` alt sistemlerini
 * ayrı ayrı yönetmesi yerine tek bir API (`spi_link_start`) üzerinden başlatılmasını sağlar.
 */

#include "spi_link.h"

#include "esp_check.h"
#include "esp_log.h"
#include "spi_in.h"
#include "spi_out.h"

static const char *TAG = "spi_link";

/**
 * @brief SPI çift yönlü iletişim hattını ayağa kaldırır.
 *
 * 1. `spi_out_start()` ile TX kuyruğunu ve aktarım görevini oluşturur.
 * 2. `spi_in_start()` ile gelen paket dinleyicisini kurar ve `rx_cb`'ye bağlar.
 *
 * @param rx_cb Gelen paketler için yönlendirici callback fonksiyonu.
 * @return esp_err_t ESP_OK veya hata kodu.
 */
esp_err_t spi_link_start(spi_link_rx_cb_t rx_cb)
{
    if (!rx_cb) {
        ESP_LOGE(TAG, "spi_link_start: rx_cb NULL olamaz");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(spi_out_start(), TAG, "spi_out_start basarisiz");
    ESP_RETURN_ON_ERROR(spi_in_start(rx_cb), TAG, "spi_in_start basarisiz");

    ESP_LOGI(TAG, "Dual-SPI Baglanti Mimarisi Hazir:");
    ESP_LOGI(TAG, "    |-- Giris Hatti (RX) : %s [SCLK:%d MISO:%d MOSI:%d CS:%d] (Sadece Alis)",
             HSPI_RX_HOST_NAME, HSPI_RX_PIN_SCLK, HSPI_RX_PIN_MISO, HSPI_RX_PIN_MOSI, HSPI_RX_PIN_CS);
    ESP_LOGI(TAG, "    \\-- Cikis Hatti (TX) : %s [SCLK:%d MISO:%d MOSI:%d CS:%d] (Sadece Gonderim)",
             VSPI_TX_HOST_NAME, VSPI_TX_PIN_SCLK, VSPI_TX_PIN_MISO, VSPI_TX_PIN_MOSI, VSPI_TX_PIN_CS);
    return ESP_OK;
}
