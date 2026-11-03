/**
 * @file pkt_buf.c
 * @brief Ağ Paket Tamponu (Packet Buffer / PktBuf) Yönetimi Gerçeklemesi.
 *
 * Tampon tahsisi, serbest bırakılması ve metaveri formatlamasını yürütür.
 * İleride dinamik heap tahsisi yerine sabit boyutlu bir FreeRTOS bellek havuzuna
 * (memory pool / ring buffer) geçiş için bu modülün dahili fonksiyonları güncellenebilir.
 */

#include "pkt_buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ip_utils.h"

static const char *TAG = "pkt_buf";

pkt_buf_t *pkt_buf_alloc(size_t initial_len)
{
    if (initial_len > PKT_BUF_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "pkt_buf_alloc: istenen boyut azami kapasiteyi asiyor (%u > %d)",
                 (unsigned)initial_len, PKT_BUF_MAX_PAYLOAD);
        return NULL;
    }

    pkt_buf_t *buf = (pkt_buf_t *)heap_caps_malloc(sizeof(pkt_buf_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    if (!buf) {
        /* Fallback: Standart malloc dene */
        buf = (pkt_buf_t *)malloc(sizeof(pkt_buf_t));
    }
    if (!buf) {
        ESP_LOGE(TAG, "pkt_buf_alloc: bellek yetersiz (gerekli: %u byte)",
                 (unsigned)sizeof(pkt_buf_t));
        return NULL;
    }

    buf->rx_if = NET_IF_NONE;
    buf->tx_if = NET_IF_NONE;
    buf->seq_num = 0;
    buf->len = initial_len;
    buf->capacity = PKT_BUF_MAX_PAYLOAD;
#if ENABLE_ICMP_LATENCY_METRICS
    memset(&buf->lat, 0, sizeof(buf->lat));
    buf->is_icmp = false;
#endif

    return buf;
}

void pkt_buf_free(pkt_buf_t *buf)
{
    if (buf) {
        free(buf);
    }
}

const char *net_if_to_name(net_if_t iface)
{
    switch (iface) {
    case NET_IF_SPI_RX:
        return "HSPI_RX(SPI2)";
    case NET_IF_SPI_TX:
        return "VSPI_TX(SPI3)";
    case NET_IF_WIFI_STA:
        return "WIFI_STA";
    case NET_IF_WIFI_AP:
        return "WIFI_AP";
    case NET_IF_NONE:
    default:
        return "NONE";
    }
}

void pkt_buf_describe(const pkt_buf_t *buf, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!buf) {
        snprintf(out, out_len, "[NULL buf]");
        return;
    }

    char pkt_summary[96];
    ip_utils_describe(buf->data, buf->len, pkt_summary, sizeof(pkt_summary));

    snprintf(out, out_len, "[if_in:%s if_out:%s] %s",
             net_if_to_name(buf->rx_if),
             net_if_to_name(buf->tx_if),
             pkt_summary);
}

