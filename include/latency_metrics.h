/**
 * @file latency_metrics.h
 * @brief ICMP Paket Katman Gecikme Ölçüm ve Telemetri Modülü Arayüzü.
 *
 * ESP-AP istemcisinden gelen ICMP (Ping) paketlerinin sistem katmanlarında
 * (Wi-Fi Alım, Yönlendirici, NAT Dönüşümü, Kuyruklar, SPI İletimi vb.)
 * ne kadar süre (milisaniye ve mikrosaniye) harcadığını ölçer.
 *
 * Kapatıp açmak için ENABLE_ICMP_LATENCY_METRICS derleme anahtarı kullanılır.
 */

#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_timer.h"

/* =========================================================================
 * DERLEME ANAHTARI (BUILD SWITCH): ICMP KATMAN GECİKME ÖLÇÜMÜ
 * 1 : Ölçüm ve terminal raporlama aktif.
 * 0 : Ölçüm tamamen kapalı (sıfır ek yük, maksimum ağ hızı).
 * ========================================================================= */
#ifndef ENABLE_ICMP_LATENCY_METRICS
#define ENABLE_ICMP_LATENCY_METRICS 0
#endif

#if ENABLE_ICMP_LATENCY_METRICS

/**
 * @brief Tek bir paketin katmanlar arası geçiş zaman damgaları (mikrosaniye cinsinden).
 */
typedef struct {
    int64_t t_rx_hook;      /**< Paketin havadan/SPI'den ilk alındığı an */
    int64_t t_fwd_start;    /**< Yönlendiriciye (Forwarder) girdiği an */
    int64_t t_fwd_done;     /**< Yönlendirici ve TTL/NAT kararının bittiği an */
    int64_t t_tx_q_in;      /**< Çıkış arayüz kuyruğuna alındığı an */
    int64_t t_tx_q_out;     /**< Çıkış arayüz kuyruğundan çıktığı an */
    int64_t t_tx_hw_done;   /**< Donanımsal iletimin (SPI/Wi-Fi) bittiği an */
} pkt_latency_t;

/**
 * @brief Sistem sayacından mikrosaniye cinsinden geçerli zamanı alır.
 *
 * @return int64_t Açılıştan itibaren geçen mikrosaniye.
 */
static inline int64_t latency_now_us(void)
{
    return esp_timer_get_time();
}

/**
 * @brief 2. ESP32 (AP Node) üzerinde ICMP gidiş yönü (Client -> SPI) metrik raporunu basar.
 *
 * @param seq Paket sıra / sequence numarası.
 * @param len Paket boyutu (bayt).
 * @param lat Zaman damgalarını tutan metrik yapısı.
 */
void latency_report_ap_egress(uint32_t seq, size_t len, const pkt_latency_t *lat);

/**
 * @brief 2. ESP32 (AP Node) üzerinde ICMP dönüş yönü (SPI -> Client) metrik raporunu basar.
 *
 * @param seq Paket sıra / sequence numarası.
 * @param len Paket boyutu (bayt).
 * @param lat Zaman damgalarını tutan metrik yapısı.
 */
void latency_report_ap_ingress(uint32_t seq, size_t len, const pkt_latency_t *lat);

/**
 * @brief 1. ESP32 (STA Node) üzerinde ICMP gidiş yönü (SPI -> WAN) metrik raporunu basar.
 *
 * @param seq Paket sıra / sequence numarası.
 * @param len Paket boyutu (bayt).
 * @param lat Zaman damgalarını tutan metrik yapısı.
 */
void latency_report_sta_egress(uint32_t seq, size_t len, const pkt_latency_t *lat);

/**
 * @brief 1. ESP32 (STA Node) üzerinde ICMP dönüş yönü (WAN -> SPI) metrik raporunu basar.
 *
 * @param seq Paket sıra / sequence numarası.
 * @param len Paket boyutu (bayt).
 * @param lat Zaman damgalarını tutan metrik yapısı.
 */
void latency_report_sta_ingress(uint32_t seq, size_t len, const pkt_latency_t *lat);

#endif /* ENABLE_ICMP_LATENCY_METRICS */

