// tools.h
// Deklaracje funkcji narzędziowych: sortowanie, zbiory, parsowanie dat,
// operacje na czasie, obsługa klucza API.

#ifndef TOOLS_H
#define TOOLS_H

#include <stdbool.h>
#include <stddef.h>

int cmp_int_asc(const void *a, const void *b);       // komparator rosnący dla qsort
void sort6(int a[6]);                                  // sortuje 6-elementową tablicę
bool set_contains(const int *S, int k, int v);         // czy zbiór S zawiera wartość v
int set_sum(const int *S, int k);                      // suma elementów zbioru
int count_hits(const int *draw, const int *S, int k);  // liczba trafień (Lotto 6/49)
int count_hits_generic(const int *draw, int draw_size, const int *S, int k); // j.w., dowolny rozmiar losowania
void msleep(long ms);                                  // uśpienie wątku na ms milisekund
int parse_seed_arg(const char *s, unsigned int *out);  // parsuje argument --seed
int parse_float_arg(const char *s, float *out);         // parsuje argument float (np. --decay-lambda)
int load_api_secret(char *buf, size_t buf_sz);         // wczytuje klucz API z pliku
int parse_ymd(const char *s, int *y, int *m, int *d); // parsuje datę YYYY-MM-DD
int days_in_month(int y, int m);                       // liczba dni w miesiącu
void next_day(int *y, int *m, int *d);                 // przechodzi do następnego dnia
int dow_sakamoto(int y, int m, int d);                 // dzień tygodnia (algorytm Sakamoto)
int is_lotto_draw_day(int y, int m, int d);            // czy dzień losowania Lotto (wt/czw/sb)

#endif
