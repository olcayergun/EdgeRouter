/**
 * @file nat.h
 * @brief Ağ Adresi ve Port Dönüştürme (Network Address and Port Translation / NAPT) Modülü Arayüzü.
 *
 * Yerel alt ağdaki (örn. 10.54.0.0/24) cihazlardan gelen IP paketlerinin,
 * yönlendiricinin dış (WAN / Wi-Fi) IP adresine ve dinamik bir dış porta dönüştürülmesini (NAPT / SNAT)
 * ve dış ağdan gelen yanıtların iç ağdaki ilgili istemci ve porta geri iletilmesini (DNAT / De-NAT)
 * sağlar.
 * TCP, UDP ve ICMP protokollerini, port havuzu tahsisini, durum takibini (Connection Tracking)
 * ve çoklu çekirdek (multi-core thread-safe) güvenliğini tam olarak destekler.
 * Tüm dönüşümler tampon üzerinde yerinde (in-place / zero-copy) gerçekleştirilir.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "pkt_buf.h"
#include "ip_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maksimum eşzamanlı aktif NAT oturumu sayısı */
#ifndef NAT_MAX_ENTRIES
#define NAT_MAX_ENTRIES 128
#endif

/** Dinamik NAPT dış port tahsis aralığı */
#define NAT_PORT_RANGE_START 40000
#define NAT_PORT_RANGE_END   49999

/** Protokol bazlı oturum zaman aşımı süreleri (saniye) */
#define NAT_TIMEOUT_UDP_SEC             30
#define NAT_TIMEOUT_TCP_ESTABLISHED_SEC 300
#define NAT_TIMEOUT_TCP_TRANS_SEC       30
#define NAT_TIMEOUT_TCP_CLOSED_SEC      10
#define NAT_TIMEOUT_ICMP_SEC            15

/**
 * @brief TCP oturum durum takibi (Connection Tracking).
 */
typedef enum {
    NAT_TCP_STATE_NONE = 0,
    NAT_TCP_STATE_SYN_SENT,
    NAT_TCP_STATE_ESTABLISHED,
    NAT_TCP_STATE_FIN_WAIT,
    NAT_TCP_STATE_CLOSED
} nat_tcp_state_t;

/**
 * @brief Tek bir NAT/NAPT oturum eşleşmesini tutan yapı.
 */
typedef struct {
    bool in_use;                /**< Bu kayıt aktif mi? */
    uint8_t protocol;           /**< IP Protokolü (1: ICMP, 6: TCP, 17: UDP) */
    nat_tcp_state_t tcp_state;  /**< TCP bağlantı durumu */
    uint32_t orig_src_ip;       /**< Orijinal iç kaynak IP (ağ bayt sırasında) */
    uint32_t orig_dst_ip;       /**< Orijinal hedef IP (ağ bayt sırasında) */
    uint16_t orig_port;         /**< Orijinal iç port veya ICMP ID (host bayt sırasında) */
    uint16_t orig_dst_port;     /**< Orijinal hedef port (host bayt sırasında) */
    uint32_t nat_src_ip;        /**< Dönüştürülen WAN IP (ağ bayt sırasında) */
    uint16_t nat_port;          /**< Dış WAN portu veya tahsis edilen ICMP ID (host bayt sırasında) */
    uint32_t last_active_sec;   /**< Son aktivite zamanı (uptime saniye) */
    uint32_t timeout_sec;       /**< Bu oturuma özel dinamik zaman aşımı süresi */
} nat_entry_t;

/**
 * @brief NAT/NAPT modülünü başlatır, oturum tablosunu ve mutex kilidini kurar.
 *
 * @return esp_err_t Başarılı ise ESP_OK.
 */
esp_err_t nat_init(void);

/**
 * @brief Giden bir pakete Kaynak NAT ve Port Çevirisi (NAPT / SNAT) uygular.
 *
 * Paketin kaynak IP adresini verilen `wan_ip_be` adresi ile yerinde değiştirir.
 * TCP veya UDP ise port havuzundan tahsis edilen dış port ile `src_port` değiştirilir.
 * ICMP ise tanımlayıcı (Identifier) değeri NAT tablosuna kaydedilip güncellenir.
 * IPv4, ICMP, TCP ve UDP sağlama toplamlarını (checksum) yeniden hesaplar.
 *
 * @param buf Değiştirilecek paket tamponu (`pkt_buf_t *`).
 * @param wan_ip_be Ağ bayt sırasında yönlendiricinin WAN IPv4 adresi.
 * @return esp_err_t Dönüşüm başarılı ise ESP_OK, tablo dolu veya geçersiz argümanda hata kodu.
 */
esp_err_t nat_apply_snat(pkt_buf_t *buf, uint32_t wan_ip_be);

/**
 * @brief Önceden ayrıştırılmış paket bilgisi ile hızlı SNAT uygular (Çift parse önleme).
 *
 * @param buf Değiştirilecek paket tamponu (`pkt_buf_t *`).
 * @param wan_ip_be Ağ bayt sırasında yönlendiricinin WAN IPv4 adresi.
 * @param info Ayrıştırılmış paket bilgisi (NULL ise dahili olarak parse edilir).
 * @return esp_err_t Dönüşüm başarılı ise ESP_OK, tablo dolu veya geçersiz argümanda hata kodu.
 */
esp_err_t nat_apply_snat_fast(pkt_buf_t *buf, uint32_t wan_ip_be, const ip_packet_info_t *info);

/**
 * @brief Gelen bir yanıta Hedef NAT ve Port Geri Çevirisi (DNAT / De-NAT) uygular.
 *
 * Dış ağdan gelen yanıt paketinin hedef IP ve port adresini, NAT tablosundaki
 * eşleşmeye göre orijinal iç ağ IP ve port adresi ile yerinde değiştirir.
 * Checksum değerlerini (IPv4 + L4) yeniden hesaplar.
 *
 * @param buf Değiştirilecek paket tamponu (`pkt_buf_t *`).
 * @return esp_err_t Eşleşme bulundu ve uygulandıysa ESP_OK, kayıt yoksa ESP_ERR_NOT_FOUND.
 */
esp_err_t nat_apply_dnat(pkt_buf_t *buf);

/**
 * @brief Önceden ayrıştırılmış paket bilgisi ile hızlı DNAT uygular (Çift parse önleme).
 *
 * @param buf Değiştirilecek paket tamponu (`pkt_buf_t *`).
 * @param info Ayrıştırılmış paket bilgisi (NULL ise dahili olarak parse edilir).
 * @return esp_err_t Eşleşme bulundu ve uygulandıysa ESP_OK, kayıt yoksa ESP_ERR_NOT_FOUND.
 */
esp_err_t nat_apply_dnat_fast(pkt_buf_t *buf, const ip_packet_info_t *info);

/**
 * @brief Aktif NAT oturumlarının sayısını döndürür.
 *
 * @return size_t Aktif kayıt adedi.
 */
size_t nat_get_active_count(void);

/**
 * @brief Eşleşme bulunamayan DNAT yanıt paketlerinin (DNAT Miss) toplam sayısını döndürür.
 *
 * @return uint32_t Düşürülen eşleşmesiz paket adedi.
 */
uint32_t nat_get_dnat_miss_count(void);

#ifdef __cplusplus
}
#endif
