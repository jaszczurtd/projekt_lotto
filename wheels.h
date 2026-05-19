// wheels.h
// Katalog covering designs (skróconych systemów lotto) z gwarancją
// trafienia t-z-k przy puli v liczb. Każdy "blok" to jeden kupon.
//
// Notacja: C(v, k, t)
//   v = rozmiar puli wybranej przez gracza,
//   k = rozmiar kuponu (6 dla Lotto, 5 dla Mini Lotto),
//   t = gwarantowana liczba trafień, jeśli wśród v liczb gracza
//       znajdzie się co najmniej t wylosowanych.

#ifndef WHEELS_H
#define WHEELS_H

#include <stdio.h>
#include <stdbool.h>

#define WHEEL_MAX_BLOCK_SIZE 8     // max k (zapas)
#define WHEEL_MAX_BLOCKS     512   // max liczba kuponów per system

typedef struct {
    int v;                 // rozmiar puli
    int k;                 // rozmiar bloku/kuponu
    int t;                 // gwarancja t-z-k
    int n_blocks;          // liczba bloków (kuponów)
    int blocks[WHEEL_MAX_BLOCKS][WHEEL_MAX_BLOCK_SIZE]; // indeksy 0..v-1
    const char *source;    // pochodzenie ("full", "manual", "La Jolla", ...)
    bool is_optimal;       // czy znana wartość minimalna
} Wheel;

// Zwraca wpis z katalogu pasujący do (v, k, t), albo NULL.
const Wheel *wheel_find(int v, int k, int t);

// Iteruje po katalogu wszystkich wheeli o danym k i wypisuje listę
// dostępnych konfiguracji do strumienia (czytelnie dla użytkownika CLI/GUI).
void wheel_list_available(int k, FILE *out);

// Aplikuje wheel do puli liczb (tablica długości w->v, posortowana rosnąco).
// Zapisuje do `out` n_blocks * k liczb (row-major). Zwraca liczbę bloków.
int wheel_apply(const Wheel *w, const int *pool, int *out);

// Self-test katalogu: weryfikuje, że każdy wpis faktycznie pokrywa
// wszystkie t-podzbiory. Zwraca true, jeśli wszystko OK; w przeciwnym razie
// wypisuje błąd do stderr i zwraca false.
bool wheels_self_test(void);

#endif
