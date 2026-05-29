// Główny nagłówek projektu Lotto.
// Definiuje stałe konfiguracyjne, struktury danych oraz deklaracje
// funkcji wspólnych dla modułów Lotto 6/49 i Mini Lotto 5/42.

#ifndef LOTTO_H
#define LOTTO_H

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "network.h"
#include "tools.h"
#include "gui.h"
#include "wheels.h"

// Konfiguracja — ścieżki plików, limity, stałe gier
#define HISTORY_FILE      "lotto_historia.txt"      // plik historii Lotto 6/49
#define MINI_HISTORY_FILE "mini_lotto_historia.txt"  // plik historii Mini Lotto 5/42
#define KEY_FILE          "lotto_key.txt"             // plik z kluczem API
#define MAX_DRAWS         30000                       // maks. liczba przechowywanych losowań
#define MAX_N             49                          // maks. liczba w Lotto 6/49
#define DRAW_SIZE         6                           // rozmiar jednego losowania Lotto
#define MINI_MAX_N        42                          // maks. liczba w Mini Lotto 5/42
#define MINI_DRAW_SIZE    5                           // rozmiar jednego losowania Mini Lotto
#define DEFAULT_RNG_SEED  19771024u                   // domyślny seed generatora liczb losowych

// Endpointy API Lotto
#define API_BASE       "https://developers.lotto.pl/api/open/v1/lotteries/draw-results/by-date-per-game"
#define GAME_TYPE      "Lotto"      // identyfikator gry Lotto 6/49 w API
#define GAME_TYPE_MINI "MiniLotto"  // identyfikator gry Mini Lotto 5/42 w API

// Automatyczne pobieranie brakujących losowań (catchup)
#define CATCHUP_DAYS            30    // normalne okno lookback przy aktualnej historii (dni)
#define CATCHUP_BOOTSTRAP_DAYS  730   // okno bootstrap gdy historia jest zbyt mała (dni, ~2 lata)
#define CATCHUP_BOOTSTRAP_MIN   100   // minimalna liczba losowań poniżej której włącza się bootstrap
#define CATCHUP_SLEEP           1200  // opóźnienie między zapytaniami API (ms)

// Przeszukiwanie lokalne – parametry optymalizacji
#define LS_ITERS_FAST 6000   // liczba iteracji w trybie fast
#define LS_ITERS_FULL 30000  // liczba iteracji w trybie full
#define CAND_FAST     120    // kandydatów na iterację w trybie fast
#define CAND_FULL     250    // kandydatów na iterację w trybie full

// Kalibracja walk-forward
#define DEFAULT_CALIB_STEP 1  // krok okna kalibracyjnego (liczba losowań)

// Backtest
#define MC_SIMS      2000  // liczba symulacji Monte Carlo do oceny istotności
#define MAX_PERIODS  200   // maks. liczba okresów w analizie kroczącego backtestu

// Wagi funkcji celu używane przy scoringu zestawów liczb
typedef struct {
    float w_freq;    // waga częstotliwości i świeżości
    float w_pair;    // waga korelacji par (PMI)
    float w_crowd;   // waga kary za popularne wzorce
    float w_center;  // waga parzystości/wyśrodkowania
    float w_gap;     // waga bonusu za zaległość (gap analysis)
} Weights;

// Komendy CLI — każda odpowiada jednemu trybowi pracy programu
int cmd_fetch(int argc, char **argv);          // pobieranie historii Lotto z API
int cmd_fetch_mini(int argc, char **argv);     // pobieranie historii Mini Lotto z API
int cmd_optimize(int argc, char **argv);       // optymalizacja systemu Lotto
int cmd_optimize_mini(int argc, char **argv);  // optymalizacja systemu Mini Lotto
int cmd_backtest(int argc, char **argv);       // backtest algorytmu Lotto
int cmd_backtest_mini(int argc, char **argv);  // backtest algorytmu Mini Lotto
int cmd_play(int argc, char **argv);           // generowanie systemów Lotto do gry
int cmd_play_mini(int argc, char **argv);      // generowanie systemów Mini Lotto do gry

// Pobiera brakujące losowania z API dla danej gry. Wspólna dla obu modułów.
int catchup_fetch_draws_for_game(const char *game_type, const char *history_file, int draw_size, int max_n);

#endif
