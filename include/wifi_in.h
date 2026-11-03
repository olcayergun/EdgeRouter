/**
 * @file wifi_in.h
 * @brief Wi-Fi Giriş Katmanı (Wi-Fi In / Ingress) Arayüzü.
 *
 * Wi-Fi Station (WAN) arayüzünden yönlendiriciye doğru gelen paket akışını sağlar.
 * Dış ağdan gelen her paket için bir `pkt_buf_t` tamponu tahsis edilir, veri
 * tampona yazılır ve yönlendiriciye yalnızca bu tamponun adresi (`pkt_buf_t *`)
 * `wifi_in_rx_cb_t` geri çağırımıyla aktarılır.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "pkt_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi girişinden gelen paket tamponunu yönlendiriciye ileten geri çağırım tipi.
 *
 * @param buf Ham IP paketini ve metaverilerini tutan tampon işaretçisi.
 * @return esp_err_t ESP_OK → paket router tarafından kabul edildi.
 */
typedef esp_err_t (*wifi_in_rx_cb_t)(pkt_buf_t *buf);

/**
 * @brief Wi-Fi giriş katmanını başlatır ve RX dinleme görevini oluşturur.
 *
 * @param rx_cb Gelen paketleri işleyecek router callback fonksiyonu (`forwarder_handle_packet`).
 * @return esp_err_t ESP_OK veya başlatma hatası.
 */
esp_err_t wifi_in_start(wifi_in_rx_cb_t rx_cb);

/**
 * @brief Wi-Fi arayüzünden gelen bir paket tamponunu ingress katmanına teslim eder.
 *
 * @param buf Wi-Fi donanımından/yığınından okunan paket tamponu (`pkt_buf_t *`).
 * @return esp_err_t ESP_OK veya hata kodu.
 */
esp_err_t wifi_in_receive(pkt_buf_t *buf);

#ifdef __cplusplus
}
#endif
