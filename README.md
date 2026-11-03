# EdgeRouter: Çift ESP32 Donanım Tabanlı Uç Ağ Yönlendiricisi (Edge Gateway)

EdgeRouter; iki adet ESP32 mikrodenetleyicisinin yüksek hızlı **Dual-SPI Simplex Veri Yolu (10 MHz)** ile birbirine bağlandığı, Wi-Fi SoftAP ve Wi-Fi Station (WAN) arayüzleri arasında tam çift yönlü IP yönlendirmesi ve yerinde Ağ Adresi Dönüştürme (NAT - Masquerade) gerçekleştiren, **Zero-Copy** mimarili bir gömülü ağ yönlendiricisidir.

---

## 1. Sistem Mimarisi ve Düğümler

Sistem iki adet ESP32 kartının ayrık görev paylaşımıyla çalışır:

```
[ Android / İstemci Cihazlar ]
            │ (Wi-Fi 802.11)
            ▼
┌──────────────────────────────────────────────────────────┐
│ 2. ESP32: SoftAP Düğümü (env: esp32-ap | /dev/ttyUSB1)   │
│  - SSID: Esp32_AP (10.54.0.1/24)                         │
│  - L2/L3 Paket Kancası (Hook) & DHCP Sunucusu            │
│  - Master TX: VSPI (10 MHz) ──► Gidiş Hattı              │
│  - Slave RX : HSPI (DMA)    ◄── Dönüş Hattı              │
└───────────────────────────┬──────────────────────────────┘
                            │
               Dual-SPI Simplex Veri Yolu (10 MHz)
                            │
┌───────────────────────────▼──────────────────────────────┐
│ 1. ESP32: Router Düğümü (env: esp-sta | /dev/ttyUSB0)    │
│  - Slave RX : HSPI (DMA)    ──► Gidiş Hattı              │
│  - Master TX: VSPI (10 MHz) ◄── Dönüş Hattı              │
│  - Sıfır Kopyalama Yönlendirici (Forwarder Core)         │
│  - Yerinde Oturum Motoru & NAT (SNAT / DNAT)             │
│  - Wi-Fi Station (WAN İstemcisi)                         │
└───────────────────────────┬──────────────────────────────┘
                            │ (Wi-Fi 802.11)
                            ▼
           [ Ev / Ofis Modemi (192.168.50.1) ➔ İnternet ]
```

---

## 2. Donanım ve Pin Bağlantı Şeması (Wiring)

İki ESP32 kartı arasında çift yönlü bağımsız veri akışı için **çift SPI (Dual Simplex)** veri yolu kurulmuştur. İletim sırasında hat çakışması veya yön değiştirme gecikmesi yaşanmaz.

> [!IMPORTANT]
> Her iki ESP32'nin **GND** pinleri birbirine mutlaka ortak bağlanmalıdır.

| Hat Yönü | Kaynak (Master TX) | Hedef (Slave RX) | Master Pini (VSPI) | Slave Pini (HSPI) | Fonksiyon |
| :--- | :--- | :--- | :---: | :---: | :--- |
| **Gidiş Hattı**<br>*(AP ➔ Router)* | **2. ESP32 (AP)** | **1. ESP32 (Router)** | **GPIO 18**<br>**GPIO 23**<br>**GPIO 19**<br>**GPIO 5** | **GPIO 14**<br>**GPIO 13**<br>**GPIO 12**<br>**GPIO 15** | SCLK (Saat)<br>MOSI (Veri)<br>MISO (Hazır)<br>CS (Seçim) |
| **Dönüş Hattı**<br>*(Router ➔ AP)* | **1. ESP32 (Router)** | **2. ESP32 (AP)** | **GPIO 18**<br>**GPIO 23**<br>**GPIO 19**<br>**GPIO 5** | **GPIO 14**<br>**GPIO 13**<br>**GPIO 12**<br>**GPIO 15** | SCLK (Saat)<br>MOSI (Veri)<br>MISO (Hazır)<br>CS (Seçim) |

---

## 3. Temel Yazılım Prensipleri ve Uygulanan Optimizasyonlar

### 1. Sıfır Kopyalama (Zero-Copy Single Buffer)
- Bir paket Wi-Fi veya SPI arayüzünden girdiğinde tek bir `pkt_buf_t` tamponuna alınır.
- Katmanlar arasında (`Hook ➔ Forwarder ➔ NAT ➔ SPI/Wi-Fi Egress`) yalnızca tampon bellek adresi (`pkt_buf_t *`) aktarılır; `memcpy` ile çoğaltma yapılmaz.
- Paketin sahipliği aktarılır; paketi ileten veya düşüren son katman `pkt_buf_free()` çağırır.

### 2. Polling SPI Master İletimi (`spi_device_polling_transmit`)
- Klasik kesme (interrupt/ISR) tabanlı SPI gönderimi, FreeRTOS görev geçişleri ve OS tick kuantizasyonu (100 Hz'de 10-20 ms) nedeniyle büyük gecikmeye yol açar.
- `spi_device_polling_transmit` ile transfer doğrudan CPU donanım durum bayrağını sorgulayarak yürütülür.
- **Sonuç:** Donanımsal iletim süresi **20 milisaniyeden 0.15 milisaniyeye (150 kat hızlanma)** indirilmiştir.

### 3. Doğrudan Görev Dağıtımı (Zero-Copy Direct Dispatch)
- Paketler ara FreeRTOS kuyruklarına (`s_rx_q`) atılıp görev uyanması beklenmek yerine, donanım alım kancasından (Hook) çıktığı mikro-saniyede doğrudan `forwarder_handle_packet` veya `node_ap_forward_packet` fonksiyonuna teslim edilir.
- **Sonuç:** Wi-Fi alım bekleme süresi **32 milisaniyeden 78 mikrosaniyeye (418 kat hızlanma)** indirilmiştir.

### 4. Çekirdek Sabitleme (Core Pinning)
- **Core 0:** Wi-Fi PHY/MAC donanım sürücüsü ve LwIP TCP/IP yığını.
- **Core 1:** SPI TX (`spi_tx_task`), SPI RX (`spi_slave_rx_task`) ve yönlendirme mantığı.
- Bu ayrım sayesinde SPI transferleri Wi-Fi kesmelerinden etkilenmez.

---

## 4. Performans ve Gecikme Ölçüm Sonuçları (ICMP Latency)

13 adet ardışık ICMP ping paketi üzerinde donanım zamanlayıcısı (`esp_timer_get_time()`) ile mikrosaniye hassasiyetinde ölçülen dahili gecikmeler:

| Ölçüm Yönü | Eski Süre (Ortalama) | Nihai Süre (Ortalama) | Başarım Artışı |
| :--- | :---: | :---: | :---: |
| **Gidiş Yolu (Telefon ➔ AP ➔ SPI ➔ Router ➔ Modem)** | `1.70 ms` | **`1.30 ms`** | ⚡ **Ultra Düşük Gecikme** |
| **Dönüş Yolu (Modem ➔ Router ➔ SPI ➔ AP ➔ Telefon)** | `35.36 ms` | **`1.55 ms`** | 🚀 **23 Kat Hızlanma** |
| **Donanım SPI İletim Süresi (Master TX)** | `18.84 ms` | **`0.15 ms`** | 🚀 **122 Kat Hızlanma** |
| **İki ESP32 İçi Toplam Çift Yönlü Geçiş** | `~37.0 ms` | **`~2.85 ms`** | 🏆 **Milisaniye Seviyesi** |

---

## 5. Telemetri ve Log Kontrol Anahtarları

UART seri portunun işlemciyi ve gecikmeyi meşgul etmemesi için sistemde tek satırlık derleme anahtarı bulunmaktadır:

[`src/main.c`](src/main.c) veya [`include/latency_metrics.h`](include/latency_metrics.h):
```c
#define ENABLE_ICMP_LATENCY_METRICS 1  /* 1: Canlı katman gecikme raporları aktif | 0: Sıfır telemetri / Üretim modu */
```
- `0` yapıldığında ölçüm fonksiyonları sıfır bellek ve CPU yüküyle `(void)0` makrosuna dönüşür.
- `main.c` içinde `esp_log_level_set("*", ESP_LOG_NONE)` ile gereksiz loglar susturulmuştur.

---

## 6. Proje Klasör Yapısı

```
EdgeRouter/
├── include/                 # Ortak başlık dosyaları
│   ├── forwarder.h          # Merkezi yönlendirici arayüzü
│   ├── ip_utils.h           # IPv4/ICMP checksum ve parser yardımcıları
│   ├── latency_metrics.h    # Mikrosaniye katman gecikme telemetrisi
│   ├── nat.h                # Yerinde NAT (SNAT/DNAT) tablosu ve oturum motoru
│   ├── node_ap.h            # 2. ESP32 SoftAP düğüm mantığı
│   ├── node_router.h        # 1. ESP32 Router düğüm mantığı
│   ├── pkt_buf.h            # Zero-Copy paket tamponu yapısı ve API
│   ├── spi_in.h             # HSPI Slave RX donanım tanımları
│   ├── spi_link.h           # Çift SPI katmanı cephesi (Facade)
│   ├── spi_out.h            # VSPI Master TX donanım tanımları
│   ├── wifi_ap.h            # SoftAP sürücüsü ve Ethernet Hook
│   └── wifi_sta.h           # Wi-Fi Station sürücüsü ve Ethernet Hook
├── src/                     # C kaynak kodları
│   ├── forwarder/           # Router yönlendirme kararı ve loop engelleme
│   ├── node_ap/             # 2. ESP32 SoftAP kancası ve L2 çerçeveleme
│   ├── node_router/         # 1. ESP32 başlatma ve akış orkestrasyonu
│   ├── spi_in/              # HSPI Slave DMA alıcısı
│   ├── spi_link/            # Dual-SPI başlatıcı
│   ├── spi_out/             # VSPI Master Polling aktarıcısı
│   ├── main.c               # app_main giriş noktası ve rol seçimi
│   ├── nat.c                # Masquerade SNAT ve DNAT motoru
│   ├── pkt_buf.c            # Tampon havuzu yönetimi
│   ├── latency_metrics.c    # Ham telemetri raporlayıcısı
│   └── ip_utils.c           # RFC 1071 checksum hesaplayıcıları
├── manage.sh                # Otomatik derleme, yükleme ve izleme betiği
├── platformio.ini           # PlatformIO ortam ve hedef yapılandırması
└── README.md                # Proje dokümantasyonu
```

---

## 7. Derleme, Flaşlama ve Canlı İzleme

Sistemde kolay yönetim için [`manage.sh`](manage.sh) kontrol merkezi bulunmaktadır:

### İnteraktif Menüyü Açma:
```bash
./manage.sh
```

### Her İki ESP32'yi Tek Komutla Yükleme:
```bash
./manage.sh upload
```
*(Önce `/dev/ttyUSB0` üzerindeki 1. ESP32'yi, ardından `/dev/ttyUSB1` üzerindeki 2. ESP32'yi otomatik flaşlar).*

### Canlı Bölünmüş Ekran Monitörünü Başlatma (tmux):
```bash
./manage.sh monitor
```
*(Üst yarıda Router `/dev/ttyUSB0`, alt yarıda SoftAP `/dev/ttyUSB1` portlarını 115200 baud ile yan yana canlı izletir).*

### Tekil Ortam Derleme (PlatformIO CLI):
```bash
pio run -e esp-sta     # 1. ESP32 (EdgeRouter)
pio run -e esp32-ap    # 2. ESP32 (SoftAP Node)
```

---

## 8. Lisans
Bu proje özel gömülü ağ ve uç yönlendirici gereksinimleri için geliştirilmiştir.

