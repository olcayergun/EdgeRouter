<!-- graft:start -->
## Graft — repo context graph

This repo is indexed in `graft/`: small linked markdown nodes that explain each
system and carry exact file:line spans, kept in sync with the code through git.

For ANY task here — understanding how something works, finding where code lives,
or scoping a change — get context from the graph before grepping or opening
source files. Re-ask freely (it's cheap) and reuse literal identifiers you
already have (symbol, error string, file name) as the query. New to this repo?
Run `graft map` first — a token-budgeted orientation (dir clusters, hubs,
hotspots), no LLM, no key.

- Run `graft ask "<your question>" --source` → ranked nodes with the relevant
  code spans inlined (each hit's ≤8-line crux by default; `--full` for whole
  definitions when the crux isn't enough). Match the tool to the task shape:
  for understanding or editing, the top node IS the answer — cite its
  `covers:` file:line spans and edit straight from `--source`. For
  exhaustive tasks ("every occurrence / every caller of this pattern"), ranked
  results are top-N, not complete — run `graft grep "<literal>"` instead
  (exhaustive over indexed files, grouped by enclosing symbol), falling back
  to raw `grep -rn` only for unindexed files.
- `graft skeleton <file>` → every definition's signature + span, ~10× cheaper
  than reading the file; use it to skim an API surface.
- `graft callers <symbol>` gives precomputed, exact edges — who calls this.
  Add `--direction out` for what it calls, or `--depth N` to walk
  transitively for the full blast radius. For structural questions, skip
  ranking and use this directly.
- Or browse: `graft/INDEX.md` lists every node; follow the links.
- Monorepos and folders of multiple repos rank fairly across sub-projects —
  hits carry `[scope/]` labels naming which one they're from. Narrow with
  `graft ask "<task>" --in <scope>/` once you know where you're working.

If a returned span is truncated ("+N more lines"), open the file at that exact
range before finalizing. Only open source files when a node genuinely lacks a
needed detail, and then at the exact file:line the node points to — never
re-read whole files.

After big code changes, refresh the graph with `graft build` (deterministic,
no API key, $0).
<!-- graft:end -->

---

# EdgeRouter Geliştirici ve Ajan Rehberi

EdgeRouter, ESP32 mikrodenetleyicisi (`esp-wrover-kit`) üzerinde çalışan; SPI arayüzü ile Wi-Fi Station (STA) arasında IP paketlerini yönlendiren modüler bir uç ağ yönlendiricisidir (edge router / gateway).

## 1. Mimari ve Modül Sorumlulukları

Proje modüler bir C mimarisi ile yapılandırılmıştır. Tüm paket akışı **Zero-Copy / Single Buffer** prensibine dayanır; bir paket arayüzden geldiğinde bir `pkt_buf_t` tamponuna yazılır ve iletileceği çıkış arayüzüne kadar fonksiyonlar arasında yalnızca tampon adresi (`pkt_buf_t *`) geçirilir:

| Modül / Dosya | Konum | Sorumluluk ve İşlev |
| :--- | :--- | :--- |
| **`main`** | `src/main.c` | **Uygulama Giriş Noktası (`app_main`):** Wi-Fi STA ve SPI Link katmanlarını sırayla başlatır, gelen paketleri forwarder'a bağlar. |
| **`pkt_buf`** | `include/pkt_buf.h`<br>`src/pkt_buf.c` | **Ağ Paket Tamponu (Packet Buffer):** Giriş/çıkış arayüz bilgisi, veri uzunluğu ve ham baytları barındıran tamponları tahsis (`pkt_buf_alloc`) ve imha (`pkt_buf_free`) eder. |
| **`forwarder`** | `include/forwarder.h`<br>`src/forwarder/forwarder.c` | **Paket Yönlendirme:** Gelen `pkt_buf_t *` paketlerini ayrıştırır, doğrulamasını yapar ve yönlendirme/NAT kararlarını yürütür. |
| **`spi_in`** | `include/spi_in.h`<br>`src/spi_in/spi_in.c` | **SPI Giriş (Ingress):** SPI üzerinden gelen paketleri `pkt_buf_t` tamponuna alıp router'a teslim eder (`spi_rx_task`). |
| **`spi_out`** | `include/spi_out.h`<br>`src/spi_out/spi_out.c` | **SPI Çıkış (Egress):** Yönlendiriciden SPI'ye gidecek paket tamponlarını FreeRTOS kuyruğuna (`s_tx_q`) alır, `spi_tx_task` üzerinden aktarır ve iletim bittiğinde tamponu serbest bırakır. |
| **`spi_link`** | `include/spi_link.h`<br>`src/spi_link/spi_link.c` | **SPI Birleşik Arayüz (Facade):** `spi_in` ve `spi_out` alt sistemlerini tek bir çatı altında başlatır (`spi_link_start`). |
| **`wifi_sta`** | `include/wifi_sta.h`<br>`src/wifi_sta.c` | **Wi-Fi İstasyon Yönetimi:** NVS başlatma, Netif kurulumu, Kconfig AP kimlik bilgileriyle bağlantı, otomatik yeniden bağlanma ve IP/Gateway sorgulama sağlar. |
| **`wifi_in`** | `include/wifi_in.h`<br>`src/wifi_in/wifi_in.c` | **Wi-Fi Giriş (Ingress):** Wi-Fi Station arayüzünden gelen paketleri `pkt_buf_t` tamponuna alıp router'a iletir (`wifi_rx_task`). |
| **`nat`** | `include/nat.h`<br>`src/nat.c` | **Ağ Adresi Dönüştürme (NAT):** Yerel alt ağdaki (`10.54.0.0/24`) paketlerin WAN IP'sine dönüştürülmesini (SNAT), oturum tablosunu ve ters dönüşümü (DNAT) tampon üzerinde yerinde yönetir. |
| **`ip_utils`** | `include/ip_utils.h`<br>`src/ip_utils.c` | **Ağ Protokol Yardımcıları:** RFC 1071 internet checksum hesabı, sentetik ICMP Echo paketi oluşturma (`ip_utils_build_icmp_echo`) ve paket log formatlama (`ip_utils_describe`). |

---

## 2. Kodlama ve Dokümantasyon Standartları (Zorunlu Kurallar)

Projeye kod eklerken veya değişiklik yaparken aşağıdaki belgelendirme kurallarına **istisnasız** uyulmalıdır:

1. **Sıfır Kopyalama (Zero-Copy) Prensibi:**
   * Paket verileri katmanlar arasında `memcpy` ile kopyalanmamalı, `pkt_buf_t *` tampon işaretçisi aktarılmalıdır.
   * Paketin sahipliği aktarılır; paketi en son tüketen veya düşüren katman `pkt_buf_free()` çağırmakla yükümlüdür.

2. **Dosya Başlığı Yorumları (File Header Comments):**
   * Her `.c` ve `.h` dosyasının en başında Doxygen formatında bir `@file` bloğu bulunmalıdır.
   * Dosyanın ne işe yaradığı, projedeki rolü ve ilişkili olduğu diğer modüller `@brief` ve detay paragraflarıyla açıklanmalıdır.

3. **Fonksiyon Doxygen Dokümantasyonu:**
   * Dışa açık (public) ve kritik dahili (static) fonksiyonların başında:
     * `@brief` ile fonksiyonun amacı,
     * `@param` veya `@param[out]` ile parametrelerin türü/işlevi,
     * `@return` ile dönüş değerlerinin anlamı (örn. `ESP_OK`, `ESP_ERR_NO_MEM`) net şekilde belirtilmelidir.

4. **FreeRTOS ve ESP-IDF Notları:**
   * Task'lerin (örn. `spi_rx_task`, `spi_tx_task`, `wifi_rx_task`) periyotları, kuyruk derinlikleri ve bellek kullanım durumları yorumlarda açıkça tarif edilmelidir.

5. **Satır İçi Açıklayıcı Yorumlar:**
   * Kod blokları (özellikle protokol başlık hesaplamaları, bit manipülasyonları ve donanım kayıtları) öncesinde amacını anlatan kısa Türkçe/İngilizce satır içi yorumlar bulundurulmalıdır.

---

## 3. Derleme ve Geliştirme

* **Derleme:** `pio run` (PlatformIO üzerinden ESP-IDF ve Ninja ile derler).
* **Flaşlama / Yükleme:** `pio run -t upload`
* **Seri Port Log İzleme:** `pio device monitor`
* **Repo Grafiğini Güncelleme:** Kod tabanında büyük değişiklik yapıldığında mutlaka `graft build` çalıştırılmalıdır.
