#!/usr/bin/env bash
# lotto_run_and_mail.sh — wrapper uruchamiający skrypt lotto i wysyłający
# wyniki mailem przez send-status.py (analogia do backup_run_and_mail.sh).
#
# Użycie: lotto_run_and_mail.sh <NAZWA> <SKRYPT>
#
# Zmienne środowiskowe (z EnvironmentFile=/etc/lotto.env):
#   SMTP_HOST, SMTP_PORT, SMTP_USER, SMTP_PASS, MAIL_FROM, MAIL_TO
set -uo pipefail

NAME="${1:?Uzycie: $0 <NAZWA> <SKRYPT>}"
SCRIPT="${2:?Uzycie: $0 <NAZWA> <SKRYPT>}"

# send-status.py leży obok tego skryptu (oba w katalogu systemd/ projektu)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEND_STATUS="${SCRIPT_DIR}/send-status.py"

LOG_DIR="/tmp"
LOG_FILE="${LOG_DIR}/lotto_${NAME}.log"
BODY_FILE="${LOG_DIR}/lotto_${NAME}_body.txt"

START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
RC=0

bash "$SCRIPT" >"$LOG_FILE" 2>&1 || RC=$?

FINISHED="$(date '+%Y-%m-%d %H:%M:%S')"

if [ "$RC" -eq 0 ]; then
    STATUS="OK"
else
    STATUS="FAIL"
fi

{
    echo "Lotto status: $STATUS"
    echo "Zadanie:      $NAME"
    echo "Host:         $(hostname -f 2>/dev/null || hostname)"
    echo "Start:        $START_TIME"
    echo "Koniec:       $FINISHED"
    echo "Exit code:    $RC"
    echo
    echo "Wyniki:"
    echo "----------------------------------------"
    cat "$LOG_FILE"
} >"$BODY_FILE"

python3 "$SEND_STATUS" \
    --subject "[lotto] $(hostname) $NAME: $STATUS" \
    --body-file "$BODY_FILE" \
    --attach "$LOG_FILE" || true

exit "$RC"
