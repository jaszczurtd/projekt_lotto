#!/usr/bin/env bash
# mini_lotto_play.sh — generuje propozycje Mini Lotto 5/42 systemami
# skróconymi (wheels) i wypisuje wyniki na stdout.
# Uruchamiany przez lotto_run_and_mail.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOTTO_BIN="$(dirname "$SCRIPT_DIR")/lotto"

if [ ! -x "$LOTTO_BIN" ]; then
    echo "BLAD: nie znaleziono pliku wykonywalnego: $LOTTO_BIN" >&2
    exit 1
fi

echo "========================================================"
echo "  Mini Lotto 5/42 — systemy skrócone (wheels)"
echo "  Data: $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================================"
echo

echo "--- Wheel 6/5/4 (pula 6 liczb, gwarancja 4-z-5) ---"
echo "    Jezeli wsrod Twoich 6 liczb znajda sie >=4 wylosowane,"
echo "    co najmniej 1 kupon trafi 4 liczby."
echo
"$LOTTO_BIN" play-mini --wheel 6/5/4
echo

echo "--- Wheel 7/5/5 (pula 7 liczb, gwarancja 5-z-5 = pelny system) ---"
echo "    Jezeli wsrod Twoich 7 liczb znajda sie wszystkie 5 wylosowanych,"
echo "    co najmniej 1 kupon trafi wszystkie 5."
echo
"$LOTTO_BIN" play-mini --wheel 7/5/5
echo

echo "--- Max System 8 ---"
echo
"$LOTTO_BIN" play-mini --max-system 8 --proposals 2 --autotune
echo

