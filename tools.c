// tools.c
// Funkcje narzędziowe: sortowanie, operacje na zbiorach, parsowanie dat,
// obliczanie dnia tygodnia, obsługa klucza API.

#define _POSIX_C_SOURCE 199309L  // wymagane dla nanosleep()

#include "lotto.h"  // KEY_FILE and other shared constants

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Komparator rosnący do użytku z qsort()
int cmp_int_asc(const void *a, const void *b) {
    return (*(const int *)a - *(const int *)b);
}

// Sortuje tablicę 6 liczb rosnąco
void sort6(int a[6]) {
    qsort(a, 6, sizeof(int), cmp_int_asc);
}

bool set_contains(const int *S, int k, int v) {
    for (int i = 0; i < k; i++) {
        if (S[i] == v) return true;
    }
    return false;
}

int set_sum(const int *S, int k) {
    int s = 0;
    for (int i = 0; i < k; i++) s += S[i];
    return s;
}

// Liczy ile liczb z losowania (6/49) występuje w zestawie S o rozmiarze k
int count_hits(const int *draw, const int *S, int k) {
    int hits = 0;
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < k; j++) {
            if (draw[i] == S[j]) {
                hits++;
                break;
            }
        }
    }
    return hits;
}

// Wersja generyczna: j.w., ale dla dowolnego rozmiaru losowania
int count_hits_generic(const int *draw, int draw_size, const int *S, int k) {
    int hits = 0;
    for (int i = 0; i < draw_size; i++) {
        for (int j = 0; j < k; j++) {
            if (draw[i] == S[j]) {
                hits++;
                break;
            }
        }
    }
    return hits;
}

void msleep(long ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int parse_seed_arg(const char *s, unsigned int *out) {
    if (!s || !*s || !out) return 0;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT_MAX) return 0;
    *out = (unsigned int)v;
    return 1;
}

int parse_float_arg(const char *s, float *out) {
    if (!s || !*s || !out) return 0;
    errno = 0;
    char *end = NULL;
    float v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

// Wczytuje klucz API z pliku KEY_FILE, przycinając białe znaki
int load_api_secret(char *buf, size_t buf_sz) {
    if (!buf || buf_sz < 2) return 0;

    FILE *f = fopen(KEY_FILE, "r");
    if (!f) {
        fprintf(stderr, "Error: brak pliku z kluczem API: %s\n", KEY_FILE);
        return 0;
    }

    if (!fgets(buf, (int)buf_sz, f)) {
        fclose(f);
        fprintf(stderr, "Error: plik %s jest pusty.\n", KEY_FILE);
        return 0;
    }
    fclose(f);

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace((unsigned char)buf[len - 1]))) {
        buf[len - 1] = '\0';
        len--;
    }
    while (*buf && isspace((unsigned char)*buf)) memmove(buf, buf + 1, strlen(buf));

    if (!*buf) {
        fprintf(stderr, "Error: plik %s nie zawiera poprawnego klucza API.\n", KEY_FILE);
        return 0;
    }

    return 1;
}

int parse_ymd(const char *s, int *y, int *m, int *d) {
    if (sscanf(s, "%d-%d-%d", y, m, d) != 3) return 0;
    if (*m < 1 || *m > 12) return 0;
    if (*d < 1 || *d > 31) return 0;
    return 1;
}

int days_in_month(int y, int m) {
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d = dim[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) d = 29;
    return d;
}

void next_day(int *y, int *m, int *d) {
    (*d)++;
    if (*d > days_in_month(*y, *m)) {
        *d = 1;
        (*m)++;
    }
    if (*m > 12) {
        *m = 1;
        (*y)++;
    }
}

// Dzień tygodnia wg algorytmu Sakamoto (0=niedz, 1=pon, ..., 6=sob)
int dow_sakamoto(int y, int m, int d) {
    static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// Czy data to dzień losowania Lotto 6/49 (wtorek=2, czwartek=4, sobota=6)
int is_lotto_draw_day(int y, int m, int d) {
    int w = dow_sakamoto(y, m, d);
    return (w == 2 || w == 4 || w == 6);
}
