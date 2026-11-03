/**
 * @file wifi_sta.h
 * @brief Wi-Fi İstasyon (Station / STA) Katmanı Arayüzü.
 *
 * ESP32'nin yerel bir kablosuz erişim noktasına (AP) bağlanmasını, IP adresi
 * edinmesini ve bağlantı durumunun takip edilmesini sağlar.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "pkt_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi Station modunu başlatır ve yapılandırılan AP'ye bağlanır.
 *
 * NVS, varsayılan TCP/IP yığıtı (Netif) ve Event Loop'u başlatır.
 * Bağlantı sağlanıp geçerli bir IPv4 adresi alınana kadar (azami `WIFI_CONNECT_TIMEOUT_MS`) bekler.
 *
 * @return esp_err_t ESP_OK → başarıyla bağlandı ve IP alındı.
 */
esp_err_t wifi_sta_start(void);

/**
 * @brief Wi-Fi istasyon arayüzünün güncel IPv4 yapılandırmasını döndürür.
 *
 * @param[out] info Alınan IP, Gateway ve Alt Ağ Maskesi (Netmask) bilgilerini tutan yapı.
 * @return true Geçerli (0.0.0.0 olmayan) bir IP adresi mevcut.
 * @return false Henüz IP alınamamış veya arayüz başlatılmamış.
 */
bool wifi_sta_get_ip_info(esp_netif_ip_info_t *info);

/**
 * @brief Wi-Fi STA için oluşturulan `esp_netif_t` nesnesini döndürür.
 *
 * @return esp_netif_t* Başlatılmış netif nesnesi (başlatılmadıysa NULL).
 */
esp_netif_t *wifi_sta_get_netif(void);

/**
 * @brief Wi-Fi STA arayüzünün güncel WAN IPv4 adresini (ağ bayt sırasında) döndürür.
 *
 * AP'ye bağlıysa DHCP'den alınan IP adresi döner; henüz IP alınmamışsa 0 döner.
 *
 * @return uint32_t Ağ bayt sırasında (Big Endian) IPv4 adresi.
 */
uint32_t wifi_sta_get_wan_ip(void);

/**
 * @brief Wi-Fi STA arayüzü üzerinden paket tamponunu dış ağa iletir.
 *
 * Sıfır kopyalama (Zero-Copy) prensibine göre çalışır; fonksiyon paketin
 * sahipliğini devralır. İletim tamamlandıktan (veya hata durumunda) sonra
 * `pkt_buf_free(buf)` çağrısıyla tamponu bellekten serbest bırakır.
 *
 * @param buf İletilecek paket tamponu (`pkt_buf_t *`).
 * @return esp_err_t ESP_OK → paket başarıyla fiziksel ağ arayüzüne iletildi.
 */
esp_err_t wifi_sta_send(pkt_buf_t *buf);

#ifdef __cplusplus
}
#endif
