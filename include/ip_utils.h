/**
 * @file ip_utils.h
 * @brief IP ve ICMP Protokol Yardımcıları (IP Utilities) Arayüzü.
 *
 * Ham IPv4 ve ICMP paketlerinin oluşturulması, RFC 1071 internet sağlama toplamı
 * (checksum) hesabı ve paket başlıklarının okunabilir metne dönüştürülmesi işlevlerini sağlar.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ayrıştırılmış IPv4 ve üst protokol detaylarını tutan veri yapısı.
 */
typedef struct {
    uint8_t version;            /**< IPv4 sürümü (örn. 4) */
    uint8_t ihl_bytes;          /**< Başlık uzunluğu (Internet Header Length, bayt) */
    uint8_t ttl;                /**< Yaşam süresi (Time to Live) */
    uint8_t protocol;           /**< IP Protokol numarası (1: ICMP, 6: TCP, 17: UDP) */
    uint16_t total_len;         /**< IP başlığında belirtilen toplam paket boyutu */
    uint16_t ip_id;             /**< Paket kimlik belirteci (Identification) */
    uint32_t src_ip;            /**< Kaynak IP (ağ bayt sırasında) */
    uint32_t dst_ip;            /**< Hedef IP (ağ bayt sırasında) */
    char src_str[16];           /**< Kaynak IP metin formatı (örn. "192.168.4.2") */
    char dst_str[16];           /**< Hedef IP metin formatı (örn. "192.168.1.1") */
    const char *proto_name;     /**< Protokol adı ("ICMP", "TCP", "UDP", "Bilinmeyen") */

    /* ICMP Özel Alanları */
    bool is_icmp;               /**< Paket ICMP protokolü mü? */
    uint8_t icmp_type;          /**< ICMP tipi (8: Echo Request, 0: Echo Reply) */
    uint8_t icmp_code;          /**< ICMP alt kodu */
    uint16_t icmp_id;           /**< ICMP tanımlayıcı (Identifier) */
    uint16_t icmp_seq;          /**< ICMP sıra numarası (Sequence Number) */
    const char *type_desc;      /**< Detaylı paket tipi açıklaması ("Echo Request (Ping)" vb.) */

    /* TCP / UDP Port Alanları */
    uint16_t src_port;          /**< Kaynak Port (TCP veya UDP) */
    uint16_t dst_port;          /**< Hedef Port (TCP veya UDP) */
} ip_packet_info_t;

/**
 * @brief Ham bir IPv4 paketini ayrıştırıp detaylarını `ip_packet_info_t` yapısına doldurur.
 *
 * @param pkt İncelenecek ham IP paketi bayt dizisi.
 * @param len Paketin bayt boyutu.
 * @param[out] info Ayrıştırılan bilgilerin yazılacağı yapı işaretçisi.
 * @return true Paket geçerli bir IPv4 paketi ve başarıyla ayrıştırıldı.
 * @return false Paket boyutu yetersiz veya IPv4 başlığı geçersiz.
 */
bool ip_utils_parse(const uint8_t *pkt, size_t len, ip_packet_info_t *info);

/**
 * @brief Standart bir IPv4 ICMP Echo Request (Ping) paketi oluşturur.
 *
 * Verilen parametreler doğrultusunda IPv4 ve ICMP başlıklarını doldurur,
 * sağlama toplamlarını (checksum) hesaplar ve yük (payload) verisini yerleştirir.
 *
 * @param[out] buf Paketin yazılacağı hedef bellek tamponu.
 * @param buf_len Hedef tamponun azami kapasitesi.
 * @param src_be Ağ bayt sırasında (Big Endian) kaynak IPv4 adresi.
 * @param dst_be Ağ bayt sırasında (Big Endian) hedef IPv4 adresi.
 * @param ip_id IPv4 başlığındaki kimlik (Identification) alanı.
 * @param icmp_id ICMP başlığındaki tanımlayıcı (Identifier) alanı.
 * @param seq ICMP sıra numarası (Sequence Number).
 * @param payload Eklenecek veri yükü işaretçisi (boş veri için NULL olabilir).
 * @param payload_len Veri yükünün bayt boyutu.
 * @return size_t Oluşturulan paketin toplam bayt boyutu (hata durumunda 0).
 */
size_t ip_utils_build_icmp_echo(uint8_t *buf, size_t buf_len,
                                uint32_t src_be, uint32_t dst_be,
                                uint16_t ip_id, uint16_t icmp_id, uint16_t seq,
                                const uint8_t *payload, size_t payload_len);

/**
 * @brief Standart bir IPv4 ICMP Echo Reply (Ping Yanıtı) paketi oluşturur.
 *
 * @param[out] buf Paketin yazılacağı hedef bellek tamponu.
 * @param buf_len Hedef tamponun azami kapasitesi.
 * @param src_be Ağ bayt sırasında (Big Endian) kaynak IPv4 adresi (Yanıtı üreten).
 * @param dst_be Ağ bayt sırasında (Big Endian) hedef IPv4 adresi (Yanıtın gideceği).
 * @param ip_id IPv4 başlığındaki kimlik (Identification) alanı.
 * @param icmp_id ICMP başlığındaki tanımlayıcı (Identifier) alanı.
 * @param seq ICMP sıra numarası (Sequence Number).
 * @param payload Eklenecek veri yükü işaretçisi (boş veri için NULL olabilir).
 * @param payload_len Veri yükünün bayt boyutu.
 * @return size_t Oluşturulan paketin toplam bayt boyutu (hata durumunda 0).
 */
size_t ip_utils_build_icmp_reply(uint8_t *buf, size_t buf_len,
                                 uint32_t src_be, uint32_t dst_be,
                                 uint16_t ip_id, uint16_t icmp_id, uint16_t seq,
                                 const uint8_t *payload, size_t payload_len);

/**
 * @brief Ham bir IPv4 paketini tek satırlık insan tarafından okunabilir log metnine çevirir.
 *
 * Protokol tipini (ICMP, TCP, UDP), kaynak ve hedef IP adreslerini, ICMP ise
 * tip (echo/reply) ve sıra numarası gibi kritik bilgileri ayrıştırır.
 *
 * @param pkt İncelenecek ham IP paketi bayt dizisi.
 * @param len Paketin bayt boyutu.
 * @param[out] out Açıklama metninin yazılacağı karakter dizisi.
 * @param out_len Hedef karakter dizisinin boyutu.
 */
void ip_utils_describe(const uint8_t *pkt, size_t len, char *out, size_t out_len);

/**
 * @brief RFC 1071 standardına uygun 16-bit Internet Checksum hesaplar.
 *
 * @param data Sağlama toplamı hesaplanacak veri dizisi.
 * @param len Dizinin bayt boyutu.
 * @return uint16_t Hesaplanan 16-bit checksum (ağ bayt sırasında).
 */
uint16_t ip_utils_checksum(const uint8_t *data, size_t len);

/**
 * @brief Değiştirilmiş bir IPv4 paketinin ve üst protokollerinin (ICMP, TCP, UDP)
 *        sağlama toplamlarını (checksum) RFC 791, RFC 792, RFC 793, RFC 768 ve
 *        RFC 1071 standartlarına uygun olarak günceller.
 *
 * IPv4 başlık sağlama toplamını günceller. Ardından protokol ICMP, TCP veya UDP ise
 * ilgili pseudo-header ve Layer 4 başlık checksum alanını sıfırlayıp yeniden hesaplar.
 *
 * @param pkt İncelenecek ve güncellenecek ham paket tamponu.
 * @param len Paketin toplam boyutu.
 */
void ip_utils_update_checksums(uint8_t *pkt, size_t len);

#ifdef __cplusplus
}
#endif
