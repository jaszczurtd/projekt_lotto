#!/usr/bin/env bash
# lotto_play.sh — generuje propozycje Lotto 6/49 systemami skróconymi
# (wheels) i wypisuje wyniki na stdout.
# Uruchamiany przez lotto_run_and_mail.sh tylko w dni losowania (Wt/Cz/Sb).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOTTO_BIN="$(dirname "$SCRIPT_DIR")/lotto"

if [ ! -x "$LOTTO_BIN" ]; then
    echo "BLAD: nie znaleziono pliku wykonywalnego: $LOTTO_BIN" >&2
    exit 1
fi

echo "========================================================"
echo "  Lotto 6/49 — systemy skrócone (wheels)"
echo "  Data: $(date '+%Y-%m-%d %H:%M:%S')"
echo "  Dzien losowania: $(LC_TIME=pl_PL.UTF-8 date '+%A' 2>/dev/null || date '+%A')"
echo "========================================================"
echo

echo "--- Wheel 7/6/5 (pula 7 liczb, gwarancja 5-z-6) ---"
echo "    Jezeli wsrod Twoich 7 liczb znajda sie >=5 wylosowanych,"
echo "    co najmniej 1 kupon trafi 5 liczb."
echo
"$LOTTO_BIN" play --wheel 7/6/5
echo

echo "--- Wheel 8/6/6 (pula 8 liczb, gwarancja 6-z-6 = pelny system) ---"
echo "    Jezeli wsrod Twoich 8 liczb znajda sie wszystkie 6 wylosowanych,"
echo "    co najmniej 1 kupon trafi wszystkie 6."
echo
"$LOTTO_BIN" play --wheel 8/6/6
echo
