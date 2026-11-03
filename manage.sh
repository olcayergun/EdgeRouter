#!/usr/bin/env bash
# ==============================================================================
# EdgeRouter - Çift ESP32 Yönetim ve Kontrol Merkezi (Menü)
# ==============================================================================
# Bu script, 1. ESP32 (EdgeRouter) ve 2. ESP32 (SoftAP Node) donanımlarının
# yükleme (flash), seri port yönetimi ve izleme (monitor) işlemlerini tek bir
# menü altında toplar.
#
# Kullanım:
#   ./manage.sh                  -> İnteraktif görsel menüyü açar
#   ./manage.sh upload           -> Doğrudan her iki ESP32'yi yükler
#   ./manage.sh monitor          -> Doğrudan bölünmüş ekran monitörü açar
#   ./manage.sh all              -> Önce her ikisini yükler, sonra monitörü açar
# ==============================================================================

set -e

# Renk Tanımlamaları
GREEN='\033[0;32m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m' # No Color

PORT_ROUTER="/dev/ttyUSB0"
PORT_AP="/dev/ttyUSB1"
SESSION="esp_dual_monitor"

# ==============================================================================
# Yardımcı Fonksiyonlar
# ==============================================================================

# Seri Port Meşguliyetini Kontrol Et ve Süreçleri Sonlandır
free_serial_port() {
    local port="$1"
    local name="$2"

    if [ ! -e "$port" ]; then
        echo -e "${RED}[HATA] $port bulunamadı! $name bağlı mı?${NC}"
        return 1
    fi

    if fuser "$port" >/dev/null 2>&1; then
        local pids
        pids=$(fuser "$port" 2>/dev/null || true)
        echo -e "${YELLOW}[UYARI] $port ($name) meşgul! Bağlı süreçler (PID): $pids${NC}"
        echo -e "${YELLOW}        İşlem(ler) sonlandırılıyor (SIGTERM)...${NC}"
        fuser -k -TERM "$port" >/dev/null 2>&1 || true
        sleep 0.5

        if fuser "$port" >/dev/null 2>&1; then
            echo -e "${YELLOW}        Hâlâ meşgul, zorla sonlandırılıyor (SIGKILL)...${NC}"
            fuser -k -KILL "$port" >/dev/null 2>&1 || true
            sleep 0.5
        fi

        if fuser "$port" >/dev/null 2>&1; then
            echo -e "${RED}[HATA] $port serbest bırakılamadı! Lütfen işlemi elle kapatın.${NC}"
            return 1
        fi
        echo -e "${GREEN}✓ $port başarıyla serbest bırakıldı.${NC}"
    else
        echo -e "${GREEN}✓ $port boşta ve hazır.${NC}"
    fi

    return 0
}

# 1. ESP32 (EdgeRouter - ttyUSB0) Yükleme
do_upload_router() {
    echo -e "\n${BLUE}================================================================${NC}"
    echo -e "${BLUE}>>> 1. ESP32 (EdgeRouter - $PORT_ROUTER) Yükleniyor...       <<<${NC}"
    echo -e "${BLUE}================================================================${NC}"
    free_serial_port "$PORT_ROUTER" "1. ESP32 (EdgeRouter)"
    if pio run -e esp-sta -t upload; then
        echo -e "\n${GREEN}✓ 1. ESP32 (EdgeRouter) başarıyla yüklendi!${NC}"
        return 0
    else
        echo -e "\n${RED}✗ 1. ESP32 yüklemesi başarısız oldu!${NC}"
        return 1
    fi
}

# 2. ESP32 (SoftAP Node - ttyUSB1) Yükleme
do_upload_ap() {
    echo -e "\n${BLUE}================================================================${NC}"
    echo -e "${BLUE}>>> 2. ESP32 (SoftAP Node - $PORT_AP) Yükleniyor...        <<<${NC}"
    echo -e "${BLUE}================================================================${NC}"
    free_serial_port "$PORT_AP" "2. ESP32 (SoftAP Node)"
    if pio run -e esp32-ap -t upload; then
        echo -e "\n${GREEN}✓ 2. ESP32 (SoftAP Node) başarıyla yüklendi!${NC}"
        return 0
    else
        echo -e "\n${RED}✗ 2. ESP32 yüklemesi başarısız oldu!${NC}"
        return 1
    fi
}

# Her İki Cihazı da Sırayla Yükle
do_upload_all() {
    echo -e "\n${BLUE}================================================================${NC}"
    echo -e "${BLUE}>>>     EdgeRouter: Çift ESP32 Toplu Yükleme Başlatılıyor    <<<${NC}"
    echo -e "${BLUE}================================================================${NC}"

    echo -e "\n${YELLOW}[0/2] Portlar denetleniyor...${NC}"
    free_serial_port "$PORT_ROUTER" "1. ESP32 (EdgeRouter)"
    free_serial_port "$PORT_AP" "2. ESP32 (SoftAP Node)"

    echo -e "\n${YELLOW}[1/2] 1. ESP32 (EdgeRouter) yükleniyor...${NC}"
    do_upload_router

    echo -e "\n${YELLOW}[2/2] 2. ESP32 (SoftAP Node) yükleniyor...${NC}"
    do_upload_ap

    echo -e "\n${GREEN}================================================================${NC}"
    echo -e "${GREEN}>>>  TEBRİKLER! Her iki ESP32 de başarıyla yüklendi ve devrede  <<<${NC}"
    echo -e "${GREEN}================================================================${NC}"
}

# Bölünmüş Ekran Çift Seri Monitör (tmux)
do_monitor_dual() {
    if ! command -v tmux >/dev/null 2>&1; then
        echo -e "${RED}[HATA] tmux kurulu değil! Lütfen 'sudo apt install tmux' çalıştırın.${NC}"
        return 1
    fi

    echo -e "\n${CYAN}[BİLGİ] Bölünmüş ekran açılıyor...${NC}"
    echo -e "${YELLOW}İpucu: tmux ekranından çıkmak için: Ctrl+b ardından d tuşlayın.${NC}"
    sleep 1

    tmux kill-session -t "$SESSION" 2>/dev/null || true
    tmux new-session -d -s "$SESSION" -n "Monitors" "pio device monitor -e esp-sta"
    tmux split-window -h -t "$SESSION" "pio device monitor -e esp32-ap"
    tmux select-layout -t "$SESSION" even-horizontal
    tmux attach-session -t "$SESSION"
}

# Sadece 1. ESP32 Monitör
do_monitor_router() {
    echo -e "\n${CYAN}>>> 1. ESP32 (EdgeRouter) Logları Açılıyor (Çıkış: Ctrl+C)...${NC}"
    pio device monitor -e esp-sta
}

# Sadece 2. ESP32 Monitör
do_monitor_ap() {
    echo -e "\n${CYAN}>>> 2. ESP32 (SoftAP Node) Logları Açılıyor (Çıkış: Ctrl+C)...${NC}"
    pio device monitor -e esp32-ap
}

# Yükle ve Ardından Monitör Başlat
do_upload_and_monitor() {
    do_upload_all
    echo -e "\n${YELLOW}Yükleme tamamlandı. 2 saniye sonra monitör oturumu başlatılıyor...${NC}"
    sleep 2
    do_monitor_dual
}

# ==============================================================================
# Komut Satırı Argüman Kontrolü (Hızlı Çalıştırma)
# ==============================================================================
case "$1" in
    upload)
        do_upload_all
        exit 0
        ;;
    upload-router)
        do_upload_router
        exit 0
        ;;
    upload-ap)
        do_upload_ap
        exit 0
        ;;
    monitor)
        do_monitor_dual
        exit 0
        ;;
    monitor-router)
        do_monitor_router
        exit 0
        ;;
    monitor-ap)
        do_monitor_ap
        exit 0
        ;;
    all)
        do_upload_and_monitor
        exit 0
        ;;
esac

# ==============================================================================
# İnteraktif Menü Döngüsü
# ==============================================================================
while true; do
    clear 2>/dev/null || true
    echo -e "${BLUE}================================================================${NC}"
    echo -e "${BOLD}${BLUE}       EdgeRouter - Çift ESP32 Yönetim ve Kontrol Konsolu       ${NC}"
    echo -e "${BLUE}================================================================${NC}"
    echo -e " ${BOLD}1)${NC} ${GREEN}Her İki Cihazı Yükle${NC} (1. Router + 2. AP)"
    echo -e " ${BOLD}2)${NC} ${CYAN}Bölünmüş Ekranda İzle${NC} (Sol: Router | Sağ: AP) [tmux]"
    echo -e " ${BOLD}3)${NC} ${YELLOW}Yükle ve Ardından İzle${NC} (Upload All + Dual Monitor)"
    echo -e "${BLUE}----------------------------------------------------------------${NC}"
    echo -e " ${BOLD}4)${NC} Sadece 1. ESP32 Yükle (EdgeRouter - $PORT_ROUTER)"
    echo -e " ${BOLD}5)${NC} Sadece 2. ESP32 Yükle (SoftAP Node - $PORT_AP)"
    echo -e " ${BOLD}6)${NC} Sadece 1. ESP32 İzle (EdgeRouter Monitör)"
    echo -e " ${BOLD}7)${NC} Sadece 2. ESP32 İzle (SoftAP Node Monitör)"
    echo -e "${BLUE}----------------------------------------------------------------${NC}"
    echo -e " ${BOLD}8)${NC} Seri Portları Denetle ve Meşgul Süreçleri Temizle"
    echo -e " ${BOLD}0)${NC} Çıkış"
    echo -e "${BLUE}================================================================${NC}"
    if ! read -rp " Lütfen seçiminiz [0-8]: " choice; then
        echo ""
        break
    fi

    case "$choice" in
        1)
            do_upload_all || true
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        2)
            do_monitor_dual || true
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        3)
            do_upload_and_monitor || true
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        4)
            do_upload_router || true
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        5)
            do_upload_ap || true
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        6)
            do_monitor_router || true
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        7)
            do_monitor_ap || true
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        8)
            echo -e "\n${YELLOW}Seri portlar taranıyor...${NC}"
            free_serial_port "$PORT_ROUTER" "1. ESP32 (EdgeRouter)"
            free_serial_port "$PORT_AP" "2. ESP32 (SoftAP Node)"
            read -rp $'\nDevam etmek için [Enter] tuşuna basın...'
            ;;
        0|q|Q)
            echo -e "\n${GREEN}Çıkış yapıldı.${NC}"
            exit 0
            ;;
        *)
            echo -e "\n${RED}Geçersiz seçim! Lütfen menüdeki rakamlardan birini girin.${NC}"
            sleep 1
            ;;
    esac
done
