/**
 * @file latency_metrics.c
 * @brief ICMP Paket Katman Gecikme Ölçüm ve Telemetri Modülü Gerçeklemesi.
 *
 * `ENABLE_ICMP_LATENCY_METRICS` anahtarı açık olduğunda ICMP paketlerinin
 * sistem katmanlarındaki işlem sürelerini hesaplayarak doğrudan standart
 * çıkışa (stdout / UART) tek bir telemetri bloğu olarak basar.
 */

#include "latency_metrics.h"

#if ENABLE_ICMP_LATENCY_METRICS

#include <stdio.h>

/**
 * @brief Mikrosaniye cinsinden farkı milisaniye ve mikrosaniye olarak ekrana yazar.
 *
 * Float biçimlendirmeye ihtiyaç duymadan 'X.YYY ms (Z us)' biçiminde yazdırır.
 */
static void print_duration(const char *label, int64_t start_us, int64_t end_us)
{
    int64_t diff = end_us - start_us;
    if (diff < 0) diff = 0;
    long ms = (long)(diff / 1000);
    long us_rem = (long)(diff % 1000);
    printf("  |-- %-32s: %ld.%03ld ms (%lld us)\r\n", label, ms, us_rem, (long long)diff);
}

void latency_report_ap_egress(uint32_t seq, size_t len, const pkt_latency_t *lat)
{
    if (!lat) return;
    int64_t total = lat->t_tx_hw_done - lat->t_rx_hook;
    if (total < 0) total = 0;

    printf("\r\n");
    printf("================================================================================\r\n");
    printf(">>> [ICMP METRIK #%" PRIu32 "] AP GIDIS (Android Istemci -> SPI Master TX) | %u Byte <<<\r\n", seq, (unsigned)len);
    printf("================================================================================\r\n");
    print_duration("[1] Wi-Fi AP RX Hook -> Fwd", lat->t_rx_hook, lat->t_fwd_start);
    print_duration("[2] Forwarder Rota Analizi & TTL", lat->t_fwd_start, lat->t_fwd_done);
    print_duration("[3] SPI TX Kuyruk Bekleme", lat->t_tx_q_in, lat->t_tx_q_out);
    print_duration("[4] Donanim SPI Master TX", lat->t_tx_q_out, lat->t_tx_hw_done);
    printf("  \\-- %-32s: %ld.%03ld ms (%lld us)\r\n",
           "TOPLAM DAHILI ISLEM SURESI",
           (long)(total / 1000), (long)(total % 1000), (long long)total);
    printf("--------------------------------------------------------------------------------\r\n\r\n");
    fflush(stdout);
}

void latency_report_ap_ingress(uint32_t seq, size_t len, const pkt_latency_t *lat)
{
    if (!lat) return;
    int64_t total = lat->t_tx_hw_done - lat->t_rx_hook;
    if (total < 0) total = 0;

    printf("\r\n");
    printf("================================================================================\r\n");
    printf(">>> [ICMP METRIK #%" PRIu32 "] AP DONUS (SPI Slave RX -> Wi-Fi AP TX) | %u Byte <<<\r\n", seq, (unsigned)len);
    printf("================================================================================\r\n");
    print_duration("[1] SPI Slave RX -> Fwd", lat->t_rx_hook, lat->t_fwd_start);
    print_duration("[2] Forwarder Istemci Eslesmesi", lat->t_fwd_start, lat->t_fwd_done);
    print_duration("[3] Wi-Fi AP Donanim TX (802.11)", lat->t_fwd_done, lat->t_tx_hw_done);
    printf("  \\-- %-32s: %ld.%03ld ms (%lld us)\r\n",
           "TOPLAM DAHILI ISLEM SURESI",
           (long)(total / 1000), (long)(total % 1000), (long long)total);
    printf("--------------------------------------------------------------------------------\r\n\r\n");
    fflush(stdout);
}

void latency_report_sta_egress(uint32_t seq, size_t len, const pkt_latency_t *lat)
{
    if (!lat) return;
    int64_t total = lat->t_tx_hw_done - lat->t_rx_hook;
    if (total < 0) total = 0;

    printf("\r\n");
    printf("================================================================================\r\n");
    printf(">>> [ICMP METRIK #%" PRIu32 "] STA GIDIS (HSPI Slave RX -> WAN Wi-Fi TX) | %u Byte <<<\r\n", seq, (unsigned)len);
    printf("================================================================================\r\n");
    print_duration("[1] HSPI Slave RX -> Forwarder", lat->t_rx_hook, lat->t_fwd_start);
    print_duration("[2] Forwarder & NAT (SNAT)", lat->t_fwd_start, lat->t_fwd_done);
    print_duration("[3] Wi-Fi STA Donanim TX", lat->t_fwd_done, lat->t_tx_hw_done);
    printf("  \\-- %-32s: %ld.%03ld ms (%lld us)\r\n",
           "TOPLAM DAHILI ISLEM SURESI",
           (long)(total / 1000), (long)(total % 1000), (long long)total);
    printf("--------------------------------------------------------------------------------\r\n\r\n");
    fflush(stdout);
}

void latency_report_sta_ingress(uint32_t seq, size_t len, const pkt_latency_t *lat)
{
    if (!lat) return;
    int64_t total = lat->t_tx_hw_done - lat->t_rx_hook;
    if (total < 0) total = 0;

    printf("\r\n");
    printf("================================================================================\r\n");
    printf(">>> [ICMP METRIK #%" PRIu32 "] STA DONUS (WAN Wi-Fi RX -> SPI Master TX) | %u Byte <<<\r\n", seq, (unsigned)len);
    printf("================================================================================\r\n");
    print_duration("[1] Wi-Fi STA RX Hook -> Fwd", lat->t_rx_hook, lat->t_fwd_start);
    print_duration("[2] Forwarder & De-NAT (DNAT)", lat->t_fwd_start, lat->t_fwd_done);
    print_duration("[3] SPI TX Kuyruk Bekleme", lat->t_tx_q_in, lat->t_tx_q_out);
    print_duration("[4] Donanim SPI Master TX", lat->t_tx_q_out, lat->t_tx_hw_done);
    printf("  \\-- %-32s: %ld.%03ld ms (%lld us)\r\n",
           "TOPLAM DAHILI ISLEM SURESI",
           (long)(total / 1000), (long)(total % 1000), (long long)total);
    printf("--------------------------------------------------------------------------------\r\n\r\n");
    fflush(stdout);
}

#endif /* ENABLE_ICMP_LATENCY_METRICS */

