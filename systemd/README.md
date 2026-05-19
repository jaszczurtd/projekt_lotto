# Automatyczne powiadomienia lotto — systemd

Pakiet uruchamia codziennie generację propozycji kuponów systemami skróconymi
(wheels) i wysyła wyniki mailem. Działa jako jednostki systemd na Raspberry Pi
lub dowolnym Linuksie z systemd.

## Co robi

| Tryb | Komendy | Kiedy |
|------|---------|-------|
| **Mini Lotto 5/42** | `play-mini --wheel 6/5/4`<br>`play-mini --wheel 7/5/5` | codziennie o 08:00 |
| **Lotto 6/49** | `play --wheel 7/6/5`<br>`play --wheel 8/6/6` | wtorek / czwartek / sobota o 08:00 |

Mail zawiera pełny output obu komend (gotowe zestawy kuponów) oraz załączony
plik logu.

## Wymagania

- Linux z **systemd**
- Python 3 (`/usr/bin/env python3`)
- Projekt skompilowany (`make` w katalogu projektu → plik wykonywalny `lotto`)
- Dostęp do serwera SMTP (może być Gmail, Outlook, własny serwer itp.)

## Pliki

```
systemd/
├── send-status.py          — wysyłka maila przez SMTP (Python)
├── lotto_run_and_mail.sh   — wrapper: uruchamia skrypt, buduje body, wysyła mail
├── mini_lotto_play.sh      — skrypt play-mini (oba wheels)
├── lotto_play.sh           — skrypt play (oba wheels)
├── lotto-mini.service      — jednostka systemd dla Mini Lotto
├── lotto-mini.timer        — timer: codziennie 08:00
├── lotto-draw.service      — jednostka systemd dla Lotto
├── lotto-draw.timer        — timer: Wt/Cz/Sb 08:00
├── lotto.env.example       — szablon konfiguracji SMTP
├── install.sh              — skrypt instalacyjny
└── README.md               — ten plik
```

## Krok po kroku

### 1. Skompiluj projekt

```bash
cd ~/Documents/projekt_lotto
make
```

Upewnij się, że plik `lotto` istnieje i jest wykonywalny:

```bash
./lotto --help
```

### 2. Skonfiguruj SMTP

Skopiuj przykładowy plik konfiguracyjny:

```bash
sudo cp ~/Documents/projekt_lotto/systemd/lotto.env.example /etc/lotto.env
sudo chmod 600 /etc/lotto.env
sudo chown root:pi /etc/lotto.env
```

Otwórz go i uzupełnij dane:

```bash
sudo nano /etc/lotto.env
```

Zawartość do wypełnienia:

```ini
SMTP_HOST=smtp.example.com   # np. smtp.gmail.com
SMTP_PORT=587                # 587 = STARTTLS (zalecane), 465 = SMTPS
SMTP_USER=nadawca@example.com
SMTP_PASS=twoje_haslo
MAIL_FROM=nadawca@example.com
MAIL_TO=odbiorca@example.com
```

#### Konfiguracja dla popularnych dostawców

**Gmail** (wymaga hasła do aplikacji — nie hasła konta):

```ini
SMTP_HOST=smtp.gmail.com
SMTP_PORT=587
SMTP_USER=twoj@gmail.com
SMTP_PASS=xxxx_xxxx_xxxx_xxxx   # hasło do aplikacji z konta Google
MAIL_FROM=twoj@gmail.com
MAIL_TO=odbiorca@example.com
```

> Hasło do aplikacji wygenerujesz pod: Konto Google → Bezpieczeństwo →
> Weryfikacja dwuetapowa → Hasła do aplikacji.

**Outlook / Hotmail:**

```ini
SMTP_HOST=smtp-mail.outlook.com
SMTP_PORT=587
SMTP_USER=twoj@outlook.com
SMTP_PASS=twoje_haslo
MAIL_FROM=twoj@outlook.com
MAIL_TO=odbiorca@example.com
```

### 3. Zainstaluj jednostki systemd

Uruchom skrypt instalacyjny jako root:

```bash
cd ~/Documents/projekt_lotto/systemd
sudo bash install.sh
```

Skrypt wykona automatycznie:
1. skopiowanie `lotto.env.example` → `/etc/lotto.env` (jeśli plik nie istnieje),
2. nadanie `chmod +x` skryptom `.sh`,
3. skopiowanie `.service` i `.timer` do `/etc/systemd/system/`,
4. `systemctl daemon-reload`,
5. `systemctl enable --now` dla obu timerów.

### 4. Sprawdź działanie timerów

```bash
systemctl list-timers lotto-mini.timer lotto-draw.timer
```

Przykładowy output:

```
NEXT                         LEFT       LAST                         PASSED UNIT              ACTIVATES
Wed 2026-05-20 08:00:00 CEST 11h left   Tue 2026-05-19 08:00:00 CEST 1h ago lotto-mini.timer  lotto-mini.service
Thu 2026-05-21 08:00:00 CEST 1 day left Sat 2026-05-17 08:00:00 CEST 2d ago lotto-draw.timer  lotto-draw.service
```

### 5. Test natychmiastowy

Uruchom usługi ręcznie bez czekania na timer:

```bash
# Mini Lotto
sudo systemctl start lotto-mini.service

# Lotto
sudo systemctl start lotto-draw.service
```

Sprawdź status i logi:

```bash
systemctl status lotto-mini.service
journalctl -u lotto-mini.service -n 50 --no-pager

systemctl status lotto-draw.service
journalctl -u lotto-draw.service -n 50 --no-pager
```

## Zmiana godziny uruchamiania

Edytuj odpowiedni plik `.timer` (lub zmień w `/etc/systemd/system/`):

```ini
# lotto-mini.timer — przykład zmiany na 07:30
OnCalendar=*-*-* 07:30:00

# lotto-draw.timer — przykład zmiany na 09:00
OnCalendar=Tue,Thu,Sat *-*-* 09:00:00
```

Po zmianie przeładuj konfigurację:

```bash
sudo systemctl daemon-reload
sudo systemctl restart lotto-mini.timer
sudo systemctl restart lotto-draw.timer
```

## Wyłączenie / odinstalowanie

```bash
# Wyłączenie (bez usuwania)
sudo systemctl disable --now lotto-mini.timer lotto-draw.timer

# Pełne usunięcie
sudo systemctl disable --now lotto-mini.timer lotto-draw.timer
sudo rm /etc/systemd/system/lotto-mini.{service,timer}
sudo rm /etc/systemd/system/lotto-draw.{service,timer}
sudo systemctl daemon-reload
sudo rm /etc/lotto.env
```

## Bezpieczeństwo

- `/etc/lotto.env` zawiera hasło SMTP — plik ma uprawnienia `600` (tylko root
  i użytkownik `pi` mogą go odczytać). Nie commituj go do repozytorium.
- `send-status.py` używa `ssl.create_default_context()` — weryfikuje certyfikat
  serwera SMTP przez systemowe CA (bezpieczne).
- Skrypty uruchamiane są jako użytkownik `pi` (nie root) — minimalne uprawnienia.

## Rozwiązywanie problemów

| Objaw | Możliwa przyczyna | Rozwiązanie |
|-------|-------------------|-------------|
| Brak maila, status OK | Błędne dane SMTP | Sprawdź `/etc/lotto.env`, przetestuj: `sudo systemctl start lotto-mini.service` i `journalctl -u lotto-mini.service` |
| `BLAD: nie znaleziono pliku wykonywalnego` | Brak skompilowanego `lotto` | `cd ~/Documents/projekt_lotto && make` |
| `MAIL_TO not set` | Brak zmiennej w env | Uzupełnij `/etc/lotto.env` |
| Timer nie uruchamia się po restarcie | `Persistent=true` powinno pomóc | Sprawdź `systemctl list-timers` |
| Gmail odrzuca połączenie | Brak hasła do aplikacji | Wygeneruj hasło do aplikacji w ustawieniach Google |
