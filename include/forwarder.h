/**
 * @file forwarder.h
 * @brief EdgeRouter Paket Yönlendirme (Forwarder) Katmanı Arayüzü.
 *
 * Bu modül, SPI arayüzü ile Wi-Fi STA arayüzü arasındaki IP paketi akışını
 * koordine eden temel yönlendirme mekanizmasını tanımlar.
 */

#pragma once

#include "esp_err.h"
#include "pkt_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Router'a gelen IP paket tamponunu karşılar ve yönlendirme kararı verir.
 *
 * İmzası `spi_link_rx_cb_t` ile tam uyumludur. Gelen paketin IP başlığını çözümler,
 * doğrulamasını yapar ve hedef adrese göre ilgili çıkış arayüzüne iletir.
 *
 * Sahiplik Kuralı (Ownership):
 * Bu fonksiyon çağrıldığında `buf` sahipliği forwarder modülüne geçer.
 * Forwarder paketi bir çıkış arayüzüne teslim ederse sahiplik o arayüze geçer;
 * eğer paket düşürülürse veya iletilemezse `pkt_buf_free(buf)` çağrılmalıdır.
 *
 * @param buf Giriş arayüzünden alınan paket tamponu işaretçisi.
 * @return esp_err_t ESP_OK veya paket işlenemediğinde hata kodu.
 */
esp_err_t forwarder_handle_packet(pkt_buf_t *buf);

#ifdef __cplusplus
}
#endif
