#!/usr/bin/env bash
# install.sh — instaluje jednostki systemd dla lotto na docelowej maszynie.
#
# Uruchom jako root (lub z sudo) z katalogu systemd/ projektu:
#   cd ~/Documents/projekt_lotto/systemd
#   sudo bash install.sh
set -euo pipefail

SYSTEMD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SYSTEMD_DIR")"

echo "=== Instalacja lotto systemd ==="
echo "Katalog projektu: $PROJECT_DIR"
echo "Katalog systemd:  $SYSTEMD_DIR"
echo

# 1. Plik środowiskowy SMTP
ENV_FILE="/etc/lotto.env"
if [ ! -f "$ENV_FILE" ]; then
    echo "[1/5] Kopiowanie $ENV_FILE.example -> $ENV_FILE ..."
    cp "$SYSTEMD_DIR/lotto.env.example" "$ENV_FILE"
    chmod 600 "$ENV_FILE"
    chown root:pi "$ENV_FILE"
    echo "      UWAGA: uzupelnij $ENV_FILE przed pierwszym uruchomieniem!"
else
    echo "[1/5] $ENV_FILE juz istnieje — pomijanie."
fi

# 2. Nadanie uprawnień wykonywalności skryptom
echo "[2/5] Nadawanie chmod +x skryptom ..."
chmod +x \
    "$SYSTEMD_DIR/lotto_run_and_mail.sh" \
    "$SYSTEMD_DIR/mini_lotto_play.sh" \
    "$SYSTEMD_DIR/lotto_play.sh"

# 3. Kopiowanie jednostek do /etc/systemd/system/
echo "[3/5] Kopiowanie plikow .service i .timer ..."
cp "$SYSTEMD_DIR/lotto-mini.service"  /etc/systemd/system/
cp "$SYSTEMD_DIR/lotto-mini.timer"    /etc/systemd/system/
cp "$SYSTEMD_DIR/lotto-draw.service"  /etc/systemd/system/
cp "$SYSTEMD_DIR/lotto-draw.timer"    /etc/systemd/system/

# 4. Przeładowanie konfiguracji systemd
echo "[4/5] systemctl daemon-reload ..."
systemctl daemon-reload

# 5. Włączenie i uruchomienie timerów
echo "[5/5] Wlaczanie timerów ..."
systemctl enable --now lotto-mini.timer
systemctl enable --now lotto-draw.timer

echo
echo "=== Gotowe ==="
echo
echo "Aktywne timery:"
systemctl list-timers lotto-mini.timer lotto-draw.timer --no-pager
echo
echo "Aby przetestowac natychmiastowe uruchomienie:"
echo "  sudo systemctl start lotto-mini.service"
echo "  sudo systemctl start lotto-draw.service"
echo
echo "Logi:"
echo "  journalctl -u lotto-mini.service -n 50"
echo "  journalctl -u lotto-draw.service -n 50"
