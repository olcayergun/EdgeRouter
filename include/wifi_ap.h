/**
 * @file wifi_ap.h
 * @brief 2. ESP32 Wi-Fi SoftAP (Erişim Noktası) Katmanı Arayüzü.
 *
 * Yerel istemcilerin (telefon, bilgisayar vb.) bağlanacağı SoftAP ağını kurar.
 * Bağlanan istemcilerden gelen IP paketlerini yakalayıp SPI hattına aktarır,
 * SPI hattından gelen yanıtları ise Wi-Fi üzerinden istemciye iletir.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "pkt_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SoftAP varsayılan ağ parametreleri */
#ifndef DEFAULT_AP_SSID
#define DEFAULT_AP_SSID      "Esp32_AP"
#endif

#ifndef DEFAULT_AP_PASS
#define DEFAULT_AP_PASS      "1234567890"
#endif
#define DEFAULT_AP_CHANNEL   1
#define DEFAULT_AP_MAX_CONN  4
#define DEFAULT_AP_IP        "10.54.0.1"
#define DEFAULT_AP_GW        "10.54.0.1"
#define DEFAULT_AP_NETMASK   "255.255.255.0"

/**
 * @brief Wi-Fi SoftAP arayüzünü başlatır ve DHCP sunucusunu kurar.
 *
 * @return esp_err_t Başarılı ise ESP_OK.
 */
esp_err_t wifi_ap_start(void);

/**
 * @brief SPI hattından gelen bir paketi Wi-Fi üzerinden ilgili istemciye iletir.
 *
 * @param buf İletilecek paket tamponu (Sahiplik bu fonksiyona geçer).
 * @return esp_err_t Başarılı ise ESP_OK.
 */
esp_err_t wifi_ap_send(pkt_buf_t *buf);

/**
 * @brief SoftAP IP ve ağ bilgilerini sorgular.
 *
 * @param[out] info IP bilgilerinin yazılacağı yapı.
 * @return true Bilgiler başarıyla alındıysa.
 */
bool wifi_ap_get_ip_info(esp_netif_ip_info_t *info);

#ifdef __cplusplus
}
#endif

