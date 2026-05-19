// wheels.c
// Implementacja katalogu wheels (covering designs) wraz z weryfikacją.
//
// Założenia:
//   - Indeksowanie bloków: 0..v-1 (mapowane do liczb gracza w `wheel_apply`).
//   - Każdy wpis musi przejść `wheels_self_test()` — pełna kontrola,
//     czy każdy t-podzbiór jest pokryty przez co najmniej jeden blok.
//
// Stan katalogu (start projektu):
//   - "full" wheels C(v,k,k) — generowane na bieżąco przy `wheel_find`
//     (każdy 6-zbiór z v jest jednoznacznie pokryty: gwarancja k-z-k).
//     Praktycznie: to po prostu pełen system K, identyczny z dotychczasowym
//     zachowaniem `cmd_play`. Stanowi baseline.
//   - C(7, 6, 5) — zweryfikowane minimum (6 bloków, gwarancja 5-z-6
//     przy puli 7 liczb).
//   - C(6, 5, 5) — trywialny pełny system Mini.
//
// Kolejne optymalne pokrycia (np. C(9,6,3)=7, C(10,6,4)=20, C(12,6,4)=38,
// C(8,5,3)=8) dodawane będą w kolejnych krokach z La Jolla Covering Repository
// (po dodaniu każdego wpisu uruchamiany jest self-test).

#include "wheels.h"

#include <stdlib.h>
#include <string.h>

// ============================================================
// Wpisy statyczne (manualnie zweryfikowane lub z La Jolla)
// ============================================================

// C(7, 6, 5) — minimum = 6 bloków.
// Konstrukcja: bloki indeksowane przez "wykluczony element" i ∈ {0..5}.
// Pokrycie: każdy 5-podzbiór = dopełnienie pary {x,y} w {0..6}.
//           Pokryty, gdy wybrano blok B_x lub B_y. Para {6,*} ma * ∈ {0..5},
//           więc co najmniej jeden indeks należy do {0..5} — zawsze pokryte.
static const int W_7_6_5[6][6] = {
    {1, 2, 3, 4, 5, 6}, // excludes 0
    {0, 2, 3, 4, 5, 6}, // excludes 1
    {0, 1, 3, 4, 5, 6}, // excludes 2
    {0, 1, 2, 4, 5, 6}, // excludes 3
    {0, 1, 2, 3, 5, 6}, // excludes 4
    {0, 1, 2, 3, 4, 6}, // excludes 5
};

// Statyczny katalog (poza wheel'ami pełnymi generowanymi on-the-fly).
// Uwaga: tablica `blocks` w typie `Wheel` jest duża (~16 KB).
// Aby nie marnować RAM, przechowujemy tylko meta + wskaźnik na dane bloków
// poprzez bufor lokalny budowany w `wheel_find` (lazy fill).
typedef struct {
    int v;
    int k;
    int t;
    int n_blocks;
    const int *blocks_flat; // długość n_blocks * k
    const char *source;
    bool is_optimal;
} CatalogEntry;

static const CatalogEntry STATIC_CATALOG[] = {
    {7, 6, 5, 6, (const int *)W_7_6_5, "manual", true},
};
static const int STATIC_CATALOG_LEN =
    (int)(sizeof(STATIC_CATALOG) / sizeof(STATIC_CATALOG[0]));

// Bufor zwracanych wheeli (pojedynczy slot — wystarcza dla aktualnego użycia,
// ponieważ wynik `wheel_find` jest natychmiast konsumowany przez `wheel_apply`).
static Wheel g_returned_wheel;

// ============================================================
// Generowanie "pełnego" wheela C(v, k, k) — wszystkie C(v,k) bloków.
// ============================================================
static int build_full_wheel(int v, int k, Wheel *out) {
    if (k < 1 || k > WHEEL_MAX_BLOCK_SIZE) return 0;
    if (v < k || v > 16) return 0; // C(16,6)=8008 > WHEEL_MAX_BLOCKS — odcinamy

    // Iteracja po wszystkich k-podzbiorach {0..v-1} w porządku leksykograficznym.
    int idx[WHEEL_MAX_BLOCK_SIZE];
    for (int i = 0; i < k; i++) idx[i] = i;

    int n = 0;
    while (1) {
        if (n >= WHEEL_MAX_BLOCKS) return 0; // za duże, abort
        for (int j = 0; j < k; j++) out->blocks[n][j] = idx[j];
        n++;

        // Następny podzbiór
        int p = k - 1;
        while (p >= 0 && idx[p] == v - k + p) p--;
        if (p < 0) break;
        idx[p]++;
        for (int j = p + 1; j < k; j++) idx[j] = idx[j - 1] + 1;
    }

    out->v = v;
    out->k = k;
    out->t = k;
    out->n_blocks = n;
    out->source = "full";
    out->is_optimal = true; // pełny system jest trywialnie optymalny dla t=k
    return n;
}

// ============================================================
// API
// ============================================================
const Wheel *wheel_find(int v, int k, int t) {
    // 1) Sprawdź katalog statyczny.
    for (int i = 0; i < STATIC_CATALOG_LEN; i++) {
        const CatalogEntry *e = &STATIC_CATALOG[i];
        if (e->v == v && e->k == k && e->t == t) {
            g_returned_wheel.v = e->v;
            g_returned_wheel.k = e->k;
            g_returned_wheel.t = e->t;
            g_returned_wheel.n_blocks = e->n_blocks;
            g_returned_wheel.source = e->source;
            g_returned_wheel.is_optimal = e->is_optimal;
            for (int b = 0; b < e->n_blocks; b++)
                for (int j = 0; j < e->k; j++)
                    g_returned_wheel.blocks[b][j] = e->blocks_flat[b * e->k + j];
            return &g_returned_wheel;
        }
    }

    // 2) Pełny wheel C(v, k, k) — generowany na bieżąco.
    if (t == k) {
        if (build_full_wheel(v, k, &g_returned_wheel) > 0)
            return &g_returned_wheel;
    }

    return NULL;
}

int wheel_apply(const Wheel *w, const int *pool, int *out) {
    if (!w || !pool || !out) return 0;
    for (int b = 0; b < w->n_blocks; b++)
        for (int j = 0; j < w->k; j++)
            out[b * w->k + j] = pool[w->blocks[b][j]];
    return w->n_blocks;
}

void wheel_list_available(int k, FILE *out) {
    fprintf(out, "Dostepne systemy skrocone (wheels) dla k=%d:\n", k);

    // Wpisy statyczne
    for (int i = 0; i < STATIC_CATALOG_LEN; i++) {
        const CatalogEntry *e = &STATIC_CATALOG[i];
        if (e->k != k) continue;
        fprintf(out, "  C(%d,%d,%d) = %d kuponow  [%s%s]\n",
                e->v, e->k, e->t, e->n_blocks,
                e->source, e->is_optimal ? ", optymalny" : "");
    }

    // Pełne wheels (zakres v: k..k+6, aby nie przekroczyć limitu)
    int max_v = (k == 6) ? 10 : 11;
    for (int v = k; v <= max_v; v++) {
        Wheel tmp;
        if (build_full_wheel(v, k, &tmp) > 0) {
            fprintf(out, "  C(%d,%d,%d) = %d kuponow  [full]\n",
                    v, k, k, tmp.n_blocks);
        }
    }
}

// ============================================================
// Self-test: weryfikacja pokrycia wszystkich t-podzbiorów.
// ============================================================

// Sprawdza, czy t-podzbiór `sub` (długości t, indeksy 0..v-1) jest zawarty
// w bloku `block` (długości k, posortowanym rosnąco).
// Zakłada, że `sub` też jest posortowany rosnąco.
static bool subset_contained(const int *sub, int t, const int *block, int k) {
    int i = 0, j = 0;
    while (i < t && j < k) {
        if (sub[i] == block[j]) { i++; j++; }
        else if (block[j] < sub[i]) j++;
        else return false;
    }
    return i == t;
}

// Sortuje kopię bloku rosnąco (do weryfikacji).
static void sort_block(const int *src, int *dst, int k) {
    memcpy(dst, src, sizeof(int) * (size_t)k);
    for (int i = 1; i < k; i++) {
        int x = dst[i], j = i - 1;
        while (j >= 0 && dst[j] > x) { dst[j + 1] = dst[j]; j--; }
        dst[j + 1] = x;
    }
}

static bool verify_one_wheel(int v, int k, int t,
                             const int *blocks_flat, int n_blocks,
                             const char *label) {
    if (t < 1 || t > k || k > WHEEL_MAX_BLOCK_SIZE || v > 16) {
        fprintf(stderr, "[wheels] %s: invalid params v=%d k=%d t=%d\n",
                label, v, k, t);
        return false;
    }

    // Posortuj bloki raz.
    int sorted_blocks[WHEEL_MAX_BLOCKS][WHEEL_MAX_BLOCK_SIZE];
    for (int b = 0; b < n_blocks; b++)
        sort_block(blocks_flat + b * k, sorted_blocks[b], k);

    // Iteruj po wszystkich t-podzbiorach {0..v-1}.
    int sub[WHEEL_MAX_BLOCK_SIZE];
    for (int i = 0; i < t; i++) sub[i] = i;

    while (1) {
        bool covered = false;
        for (int b = 0; b < n_blocks; b++) {
            if (subset_contained(sub, t, sorted_blocks[b], k)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            fprintf(stderr, "[wheels] %s: subset {", label);
            for (int i = 0; i < t; i++)
                fprintf(stderr, "%d%s", sub[i], i + 1 < t ? "," : "");
            fprintf(stderr, "} NOT covered\n");
            return false;
        }

        // Następny podzbiór
        int p = t - 1;
        while (p >= 0 && sub[p] == v - t + p) p--;
        if (p < 0) break;
        sub[p]++;
        for (int j = p + 1; j < t; j++) sub[j] = sub[j - 1] + 1;
    }

    return true;
}

bool wheels_self_test(void) {
    bool ok = true;
    char label[64];
    for (int i = 0; i < STATIC_CATALOG_LEN; i++) {
        const CatalogEntry *e = &STATIC_CATALOG[i];
        snprintf(label, sizeof(label), "C(%d,%d,%d)", e->v, e->k, e->t);
        if (!verify_one_wheel(e->v, e->k, e->t,
                              e->blocks_flat, e->n_blocks, label)) {
            ok = false;
        }
    }
    return ok;
}
