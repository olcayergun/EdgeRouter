/**
 * @file node_router.h
 * @brief 1. ESP32 (EdgeRouter Düğümü) Ana Başlatıcı Arayüzü.
 *
 * Bu modül, EdgeRouter rolünü üstlenen 1. ESP32'nin NAT motorunu, Wi-Fi Station
 * arayüzünü, SPI bağlantısını ve yönlendirme (forwarder) döngüsünü başlatır.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief EdgeRouter düğümünü (1. ESP32) başlatır.
 *
 * Sırasıyla:
 * 1. NAT tablosunu ve oturum motorunu başlatır.
 * 2. Wi-Fi Ingress katmanını başlatır.
 * 3. Wi-Fi Station arayüzünü başlatıp WAN AP'sine bağlanır.
 * 4. Çift yönlü SPI linkini başlatıp forwarder'a bağlar.
 * 5. Sentetik test paket üreteci görevini devreye alır.
 *
 * @return esp_err_t Başarılı ise ESP_OK.
 */
esp_err_t node_router_start(void);

#ifdef __cplusplus
}
#endif

