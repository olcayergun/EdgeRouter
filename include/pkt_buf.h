/**
 * @file pkt_buf.h
 * @brief Ağ Paket Tamponu (Packet Buffer / PktBuf) Yönetimi Arayüzü.
 *
 * Farklı ağ arayüzlerinden (SPI, Wi-Fi STA vb.) gelen IP paketlerinin
 * bellekte tek bir yerde tutulmasını ve arayüzler ile yönlendirme katmanları
 * arasında veri kopyalaması (memcpy) yapılmaksızın yalnızca tampon işaretçisi
 * (pointer) üzerinden aktarılmasını (zero-copy) sağlar.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "latency_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Standart azami paket tamponu veri kapasitesi (MTU + başlık marjı) */
#define PKT_BUF_MAX_PAYLOAD 1600

/**
 * @brief Paketin alındığı veya iletileceği ağ arayüzü tanımları.
 */
typedef enum {
    NET_IF_NONE = 0,    /**< Belirtilmemiş / tanımsız arayüz */
    NET_IF_SPI_RX,      /**< HSPI (SPI2_HOST) - Sadece Geliş / Alış (RX Ingress) Arayüzü */
    NET_IF_SPI = NET_IF_SPI_RX, /**< Geriye dönük uyumluluk takma adı */
    NET_IF_SPI_TX,      /**< VSPI (SPI3_HOST) - Sadece Gidiş / İletim (TX Egress) Arayüzü */
    NET_IF_WIFI_STA,    /**< Wi-Fi Station arayüzü (AP / dış ağ geçidi) */
    NET_IF_WIFI_AP      /**< Wi-Fi SoftAP arayüzü (İç ağ istemcileri) */
} net_if_t;

/**
 * @brief Tek bir IP paketini ve ilişkili metaverileri tutan tampon yapısı.
 */
typedef struct pkt_buf {
    net_if_t rx_if;                     /**< Paketin geldiği giriş arayüzü */
    net_if_t tx_if;                     /**< Paketin yönlendirileceği çıkış arayüzü */
    uint32_t seq_num;                   /**< Paket akış takip sıra numarası */
    size_t len;                         /**< Tampon içerisindeki geçerli veri bayt sayısı */
    size_t capacity;                    /**< Tamponun azami veri kapasitesi */
#if ENABLE_ICMP_LATENCY_METRICS
    pkt_latency_t lat;                  /**< Katman gecikme ölçüm zaman damgaları */
    bool is_icmp;                       /**< Paketin ICMP olup olmadığı */
#endif
    uint8_t data[PKT_BUF_MAX_PAYLOAD] __attribute__((aligned(4)));  /**< Ham paket içeriği (DMA uyumlu) */
} pkt_buf_t;

/**
 * @brief Yeni bir paket tamponu tahsis eder.
 *
 * @param initial_len İsteğe bağlı başlangıç uzunluğu (bilinmiyorsa 0 verilebilir).
 * @return pkt_buf_t* Tahsis edilen tampon işaretçisi, bellek yetersizse NULL.
 */
pkt_buf_t *pkt_buf_alloc(size_t initial_len);

/**
 * @brief Paket tamponunu serbest bırakır.
 *
 * Paket ilgili çıkış arayüzünden iletildiğinde veya hata durumunda
 * yönlendirici tarafından çağrılır.
 *
 * @param buf Serbest bırakılacak tampon işaretçisi (NULL ise güvenle yoksayılır).
 */
void pkt_buf_free(pkt_buf_t *buf);

/**
 * @brief Arayüz enum değerini insan tarafından okunabilir metne dönüştürür.
 *
 * @param iface Ağ arayüzü.
 * @return const char* Arayüz adı ("SPI", "WIFI_STA", "NONE" vb.).
 */
const char *net_if_to_name(net_if_t iface);

/**
 * @brief Paket tamponu içeriğini açıklayan tek satırlık özet log üretir.
 *
 * @param buf İncelenecek paket tamponu.
 * @param[out] out Açıklamanın yazılacağı karakter dizisi.
 * @param out_len Hedef karakter dizisi boyutu.
 */
void pkt_buf_describe(const pkt_buf_t *buf, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

