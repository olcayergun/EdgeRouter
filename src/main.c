/**
 * @file main.c
 * @brief EdgeRouter Ana Uygulama Giriş Noktası (Application Dispatcher).
 *
 * Derleme bayrağına göre sistemi ilgili düğüm rolünde başlatır:
 * - `CONFIG_NODE_ROUTER`: 1. ESP32 (EdgeRouter - STA + NAT + SPI Yönlendirici)
 * - `CONFIG_NODE_AP`: 2. ESP32 (AP Node - SoftAP + SPI Köprüsü)
 */

#include "esp_log.h"
#include "latency_metrics.h"

#if defined(CONFIG_NODE_ROUTER)
#include "node_router.h"
#elif defined(CONFIG_NODE_AP)
#include "node_ap.h"
#endif

void app_main(void)
{
#if ENABLE_ICMP_LATENCY_METRICS
    /* Zaman ölçüm modunda sistem loglarını susturarak yalnızca telemetri bas */
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("\n");
    printf("********************************************************************************\n");
    printf(">>> [METRIK SISTEMI AKTIF] ICMP Katman Gecikme Olcumu Devrede                <<<\n");
    printf(">>> (Sistem genel loglari kapali, sadece milisaniye/mikrosaniye metrikleri)  <<<\n");
    printf("********************************************************************************\n\n");
    fflush(stdout);
#else
    /* Standart çalışma modunda logları INFO seviyesinde tut */
    esp_log_level_set("*", ESP_LOG_INFO);
#endif

#if defined(CONFIG_NODE_ROUTER)
    node_router_start();
#elif defined(CONFIG_NODE_AP)
    node_ap_start();
#else
    #error "Gecersiz derleme hedefi! Lutfen CONFIG_NODE_ROUTER veya CONFIG_NODE_AP tanimlayin."
#endif
}
