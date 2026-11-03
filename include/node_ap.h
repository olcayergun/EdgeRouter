/**
 * @file node_ap.h
 * @brief 2. ESP32 (SoftAP Köprü Düğümü) Ana Başlatıcı ve Yönlendirici Arayüzü.
 *
 * Bu modül, SoftAP (Erişim Noktası) rolünü üstlenen 2. ESP32'nin yerel Wi-Fi
 * ağını ve SPI üzerinden 1. ESP32'ye çift yönlü köprüleme (bridge) akışını başlatır.
 * Ayrıca istemcilerden veya SPI'den gelen tüm paketlerin tek bir noktadan
 * yönlendirilmesini sağlayan `node_ap_forward_packet` fonksiyonunu sunar.
 */

#pragma once

#include "esp_err.h"
#include "pkt_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AP Köprü düğümünü (2. ESP32) başlatır.
 *
 * Sırasıyla:
 * 1. SPI Link katmanını başlatıp gelen paketleri `node_ap_forward_packet`'e bağlar.
 * 2. Wi-Fi SoftAP ağını (`10.54.0.1/24`) ayağa kaldırır.
 * 3. Wi-Fi RX kancasını (hook) `node_ap_forward_packet`'e bağlar.
 *
 * @return esp_err_t Başarılı ise ESP_OK.
 */
esp_err_t node_ap_start(void);

/**
 * @brief 2. ESP32 (SoftAP Düğümü) Tek Merkezi Paket Yönlendirme Fonksiyonu.
 *
 * Wi-Fi AP istemcilerinden veya SPI giriş hattından gelen tüm paketler tek bu fonksiyon
 * üzerinden incelenir.
 * - Döngü engelleme (Loop Prevention) kontrolleri (TTL kontrolü, aynı arayüze basma engeli) yapılır.
 * - TTL değeri bir azaltılır ve IP checksum yenilenir.
 * - Hedef IP adresi analizi:
 *   - Wi-Fi AP'den gelip WAN yönüne giden paketler -> SPI_TX (`spi_link_send`) ile 1. ESP32'ye iletilir.
 *   - SPI'den gelip yerel SoftAP istemcisine (`10.54.0.x`) giden yanıtlar -> `wifi_ap_send` ile istemciye iletilir.
 *   - SPI'den gelip yerel ağda olmayan paketler -> Loop oluşturmaması için düşürülür.
 *
 * @param buf Yönlendirilecek paket tamponu (Sahiplik bu fonksiyona geçer).
 * @return esp_err_t Başarılı ise ESP_OK.
 */
esp_err_t node_ap_forward_packet(pkt_buf_t *buf);

#ifdef __cplusplus
}
#endif
