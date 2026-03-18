# Lotto 6/49 System

Aplikacja C do analizy historii losowan Lotto i Mini Lotto, generowania systemow liczbowych oraz backtestu strategii.

Projekt udostepnia:
- tryb CLI,
- tryb GUI (GTK3, jesli dostepne podczas kompilacji),
- pobieranie historii z API Lotto,
- optymalizacje zestawow,
- backtest z porownaniem do losowej bazy (Monte Carlo).

## Funkcje

- obsluga dwoch gier:
  - Lotto 6/49,
  - Mini Lotto 5/42,
- pobieranie brakujacych losowan z API,
- scoring oparty m.in. o:
  - czestotliwosc,
  - recency,
  - PMI par,
  - gap score,
  - kary za wzorce "crowd" / klastrowanie,
- decay eksponencjalny w statystykach okna (Lotto i Mini),
- backtest walk-forward + Monte Carlo + p-value/lift.

## Wymagania

### Wspolne (CLI)

- Linux,
- `gcc` z obsluga C11,
- `make`,
- `pkg-config`,
- `libcurl` (devel),
- `libcjson` (devel),
- biblioteki standardowe: `pthread`, `math`.

### GUI (opcjonalnie)

- `gtk+-3.0` (devel).

Jesli GTK3 jest dostepne, Makefile kompiluje wersje z GUI automatycznie.
Jesli nie, program dziala w trybie CLI (fallback w `gui.c`).

## Instalacja zaleznosci (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential make pkg-config libcurl4-openssl-dev libcjson-dev libgtk-3-dev
```

Jesli nie chcesz GUI, `libgtk-3-dev` nie jest wymagane.

## Kompilacja

W katalogu projektu:

```bash
make
```

Powstaje plik wykonywalny:

- `./lotto`

Czyszczenie:

```bash
make clean
```

## Konfiguracja API

Aplikacja czyta token API z pliku:

- `lotto_key.txt`

Wpisz tam klucz (jedna linia). Ten plik jest ignorowany przez Git.

## Dane historii

Domyslne pliki historii:
- `lotto_historia.txt`
- `mini_lotto_historia.txt`

Te pliki sa aktualizowane przez komendy `fetch` / `fetch-mini` oraz przez mechanizm catch-up w innych komendach.

## Uzycie

```bash
./lotto
```

Uruchamia GUI (jesli zbudowane z GTK), w przeciwnym razie fallback CLI.

### 1. Fetch historii

Lotto:

```bash
./lotto fetch --from YYYY-MM-DD --to YYYY-MM-DD [--sleep-ms N] [--max-req N] [--max-429 N]
```

Mini Lotto:

```bash
./lotto fetch-mini --from YYYY-MM-DD --to YYYY-MM-DD [--sleep-ms N] [--max-req N] [--max-429 N]
```

Parametry:
- `--from`, `--to`: zakres dat,
- `--sleep-ms`: opoznienie miedzy requestami,
- `--max-req`: limit zapytan,
- `--max-429`: limit kolejnych odpowiedzi 429.

### 2. Optimize (Lotto)

```bash
./lotto optimize [--mode fast|full] [-k K] [--train N] [--seed N] [--decay-lambda X]
```

Parametry:
- `-k K`: rozmiar systemu (`6..15`),
- `--mode fast|full`:
  - `fast`: stale wagi,
  - `full`: kalibracja walk-forward,
- `--train N`: okno treningowe,
- `--seed N`: seed RNG,
- `--decay-lambda X`: sila decay (`0..1`, domyslnie `0.03`).

### 3. Optimize (Mini Lotto)

```bash
./lotto optimize-mini [--mode fast|full] [-k K] [--train N|--autotune|--no-autotune] [--seed N]
```

Parametry:
- `-k K`: rozmiar systemu (`5..12`),
- `--mode fast|full`,
- `--train N`: reczne okno treningowe (wylacza autotune),
- `--autotune`: automatyczny dobor okna,
- `--no-autotune`: wymusza brak autotune,
- `--seed N`: seed RNG.

### 4. Backtest (Lotto)

```bash
./lotto backtest [-k K] [--train N|--autotune|--no-autotune] [--step N] [--mc N] [--seed N] [--decay-lambda X]
```

Parametry:
- `-k K`: rozmiar systemu (`6..15`),
- `--train N`: reczne okno treningowe (wylacza autotune),
- `--autotune` / `--no-autotune`,
- `--step N`: krok testu,
- `--mc N`: liczba symulacji Monte Carlo,
- `--seed N`: seed RNG,
- `--decay-lambda X`: sila decay (`0..1`).

### 5. Backtest (Mini Lotto)

```bash
./lotto backtest-mini [-k K] [--train N|--autotune|--no-autotune] [--step N] [--mc N] [--seed N]
```

Parametry analogiczne do Lotto, z zakresem `K=5..12`.

### 6. Play (Lotto)

```bash
./lotto play [--max-system N] [--proposals N] [--seed N] [--decay-lambda X]
```

Parametry:
- `--max-system N`: maksymalny system (`7..12`, domyslnie 10),
- `--proposals N`: liczba propozycji na system,
- `--seed N`: seed RNG,
- `--decay-lambda X`: sila decay (`0..1`).

### 7. Play (Mini Lotto)

```bash
./lotto play-mini [--max-system N] [--proposals N] [--train N|--autotune|--no-autotune] [--seed N]
```

Parametry:
- `--max-system N`: maksymalny system (`5..12`, domyslnie 9),
- `--proposals N`: liczba propozycji na system,
- `--train N`: reczne okno treningowe,
- `--autotune` / `--no-autotune`,
- `--seed N`.

## Przyklady

```bash
# 1) Pobranie historii Lotto
./lotto fetch --from 2025-01-01 --to 2026-03-01

# 2) Szybka optymalizacja Lotto (K=8)
./lotto optimize --mode fast -k 8 --train 600 --seed 123 --decay-lambda 0.03

# 3) Backtest Mini Lotto
./lotto backtest-mini -k 7 --autotune --step 1 --mc 3000 --seed 123

# 4) Generowanie systemow Mini Lotto
./lotto play-mini --max-system 10 --proposals 3 --autotune --seed 123
```

## Uwagi

- Wyniki nie stanowia porady inwestycyjnej ani gwarancji wygranej.
- EV loterii pozostaje ujemne nawet przy poprawie metryk backtestowych.
- Dla reprodukowalnosci porownan ustawiaj `--seed`.

## Pliki ignorowane w repo

W `.gitignore` sa m.in.:
- `lotto_key.txt`,
- `*historia*`,
- `lotto` (plik binarny).
