// lotto.c
// Moduł główny Lotto 6/49: pobieranie historii, optymalizacja systemu,
// backtest i generowanie systemów do gry.

#include "lotto.h"

// ============================================================
// Dane globalne
// ============================================================
static int history[MAX_DRAWS][DRAW_SIZE];
static int draws_total = 0;

// Statystyki dla bieżącego okna analizy
static float freq[MAX_N + 1];
static int last_seen[MAX_N + 1];
static float pair_count[MAX_N + 1][MAX_N + 1];
static int window_len = 0;
static float window_mass = 0.0f;
static float g_decay_lambda = 0.03f;

// Raportowanie postępu zaimplementowane w gui.c; deklaracje w lotto.h

// Funkcje pomocnicze wyodrębnione do tools.c/.h, obsługa sieci w network.c/.h

// ============================================================
// KeySet — zbiór kluczy do deduplikacji losowań (tryb fetch)
// ============================================================
typedef struct {
    char **keys;
    size_t count;
    size_t cap;
} KeySet;

static void keyset_init(KeySet *ks) { ks->keys = NULL; ks->count = 0; ks->cap = 0; }

static void keyset_free(KeySet *ks) {
    for (size_t i = 0; i < ks->count; i++) free(ks->keys[i]);
    free(ks->keys);
}

static int keyset_contains(const KeySet *ks, const char *k) {
    for (size_t i = 0; i < ks->count; i++)
        if (strcmp(ks->keys[i], k) == 0) return 1;
    return 0;
}

static void keyset_add(KeySet *ks, const char *k) {
    if (keyset_contains(ks, k)) return;
    if (ks->count == ks->cap) {
        size_t nc = (ks->cap == 0) ? 256 : ks->cap * 2;
        char **np = (char **)realloc(ks->keys, nc * sizeof(char *));
        if (!np) { fprintf(stderr, "OOM\n"); exit(1); }
        ks->keys = np;
        ks->cap = nc;
    }
    size_t len = strlen(k) + 1;
    ks->keys[ks->count] = (char *)malloc(len);
    if (!ks->keys[ks->count]) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(ks->keys[ks->count], k, len);
    ks->count++;
}

// ============================================================
// Wczytywanie i zapis historii losowań
// ============================================================
static void load_history(void) {
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) { draws_total = 0; return; }

    draws_total = 0;
    while (draws_total < MAX_DRAWS) {
        int a[6];
        if (fscanf(f, "%d %d %d %d %d %d", &a[0],&a[1],&a[2],&a[3],&a[4],&a[5]) != 6) break;
        for (int i = 0; i < 6; i++) {
            if (a[i] < 1 || a[i] > 49) {
                fprintf(stderr, "Invalid value in history line %d.\n", draws_total + 1);
                fclose(f);
                exit(1);
            }
        }
        sort6(a);
        memcpy(history[draws_total], a, sizeof(a));
        draws_total++;
    }
    fclose(f);
}

// ============================================================
// Budowanie statystyk na oknie [start, end)
// ============================================================
static void build_stats_window(int start, int end) {
    memset(freq, 0, sizeof(freq));
    memset(pair_count, 0, sizeof(pair_count));
    for (int i = 0; i <= MAX_N; i++) last_seen[i] = -1;

    if (start < 0) start = 0;
    if (end > draws_total) end = draws_total;
    if (end <= start) { window_len = 0; window_mass = 0.0f; return; }

    window_len = end - start;
    window_mass = 0.0f;

    for (int di = start; di < end; di++) {
        int rel = di - start;
        int age = end - 1 - di;
        float w = expf(-g_decay_lambda * (float)age);
        window_mass += w;
        const int *d = history[di];
        for (int i = 0; i < 6; i++) {
            freq[d[i]] += w;
            last_seen[d[i]] = rel;
        }
        for (int i = 0; i < 6; i++) {
            for (int j = i + 1; j < 6; j++) {
                int x = d[i], y = d[j];
                if (x > y) { int t = x; x = y; y = t; }
                pair_count[x][y] += w;
            }
        }
    }
}

// ============================================================
// Funkcje oceny (scoring) — częstotliwość, PMI par, kara tłumu
// ============================================================
static float safe_log(float x) {
    if (x < 1e-12f) x = 1e-12f;
    return logf(x);
}

static float norm_denominator(void) {
    return (window_mass > 1e-9f) ? window_mass : (float)window_len;
}

static float pair_pmi(int x, int y) {
    if (x > y) { int t = x; x = y; y = t; }
    if (window_len <= 0) return 0.0f;

    float den = norm_denominator();

    float px  = freq[x] / den;
    float py  = freq[y] / den;
    float pxy = pair_count[x][y] / den;

    if (px <= 0.0f || py <= 0.0f || pxy <= 0.0f) return 0.0f;
    return safe_log(pxy / (px * py));
}

static float set_pair_score(const int *S, int k) {
    float s = 0.0f;
    int cnt = 0;
    for (int i = 0; i < k; i++)
        for (int j = i + 1; j < k; j++) { s += pair_pmi(S[i], S[j]); cnt++; }
    return cnt ? (s / (float)cnt) : 0.0f;
}

// Stała zaniku eksponencjalnego: λ kontroluje szybkość zaniku
// Przy λ=0.03 liczba niewidziana od 30 losowań ma wagę e^(-0.9) ≈ 0.41
static const float DEFAULT_DECAY_LAMBDA = 0.03f;

// Eksponencjalny zanik świeżości: e^(-λ * gap)
static float exp_recency(int n) {
    if (window_len <= 0) return 0.0f;
    if (last_seen[n] < 0) return 0.0f;  // nigdy nie widziana w oknie
    int gap = window_len - 1 - last_seen[n];
    return expf(-g_decay_lambda * (float)gap);
}

static float set_freq_recency(const int *S, int k) {
    if (window_len <= 0) return 0.0f;
    float den = norm_denominator();
    float s = 0.0f;

    for (int i = 0; i < k; i++) {
        int n = S[i];
        float f = freq[n] / den;
        float rec = exp_recency(n);

        float center = 1.0f - fabsf(25.0f - (float)n) / 25.0f;
        s += (0.65f * f + 0.30f * rec + 0.05f * center);
    }
    return s / (float)k;
}

// Gap analysis: bonus za zaległe numery
// Jeśli numer nie pojawiał się dłużej niż oczekiwana przerwa (okno/freq),
// daje pozytywny bonus proporcjonalny do nadwyżki przerwy.
static float set_gap_score(const int *S, int k) {
    if (window_len <= 0) return 0.0f;
    float den = norm_denominator();
    float score = 0.0f;

    for (int i = 0; i < k; i++) {
        int n = S[i];
        int gap = (last_seen[n] >= 0) ? (window_len - 1 - last_seen[n]) : window_len;
        float expected_gap = (freq[n] > 1e-9f)
            ? den / freq[n]
            : (float)window_len;
        float ratio = (float)gap / expected_gap;
        // Symetrycznie: bonus za zaległe, malus za zbyt świeże.
        float centered = ratio - 1.0f;
        if (centered > 2.0f) centered = 2.0f;
        if (centered < -1.0f) centered = -1.0f;
        score += centered;
    }
    return score / (float)k;
}

static float crowd_penalty(const int *S, int k) {
    int T[64];
    memcpy(T, S, sizeof(int) * (size_t)k);
    qsort(T, (size_t)k, sizeof(int), cmp_int_asc);

    int dates = 0, seq_steps = 0, end_pop = 0;
    int endings[10] = {0};

    for (int i = 0; i < k; i++) {
        if (T[i] <= 31) dates++;
        int d = T[i] % 10;
        endings[d]++;
        if (d == 0 || d == 5 || d == 7) end_pop++;
        if (i > 0 && T[i] == T[i-1] + 1) seq_steps++;
    }

    float p = 0.0f;
    p += 0.25f * (float)dates;
    p += 0.50f * (float)end_pop;
    p += 0.30f * (float)seq_steps;

    // Silna kara za długie ciągi kolejnych liczb
    if (seq_steps >= 3) p += 1.5f * (float)(seq_steps - 2);

    for (int d = 0; d < 10; d++)
        if (endings[d] >= 3) p += 0.40f * (float)(endings[d] - 2);

    float target = (float)k * 25.0f;
    float sumv = (float)set_sum(T, k);
    float dist = fabsf(sumv - target);
    float closeness = fmaxf(0.0f, 1.0f - dist / (target > 0.0f ? target : 1.0f));
    p += 0.6f * closeness;

    // Kara za rozkład: kara za małe odstępy między kolejnymi liczbami
    // Dobry zestaw lotto powinien być rozłożony równomiernie w całym zakresie
    if (k >= 2) {
        float min_ideal_gap = (float)(MAX_N - 1) / (float)(k - 1);
        float threshold = min_ideal_gap * 0.4f;
        float gap_penalty = 0.0f;
        for (int i = 1; i < k; i++) {
            float gap = (float)(T[i] - T[i-1]);
            if (gap < threshold) {
                float deficit = (threshold - gap) / threshold;
                gap_penalty += deficit * deficit; // quadratic: harsh on tiny gaps
            }
        }
        p += 3.0f * gap_penalty;
    }

    return p;
}

static float objective(const int *S, int k, const Weights *w) {
    float a = set_freq_recency(S, k);
    float b = set_pair_score(S, k);
    float c = crowd_penalty(S, k);
    float g = set_gap_score(S, k);

    int even = 0;
    for (int i = 0; i < k; i++)
        if ((S[i] % 2) == 0) even++;
    float parity_skew = fabsf((float)even - (float)k / 2.0f) / (float)k;
    float center_term = 1.0f - parity_skew;

    return w->w_freq * a + w->w_pair * b - fabsf(w->w_crowd) * c
         + w->w_center * center_term + w->w_gap * g;
}

// ============================================================
// Seed początkowy + przeszukiwanie lokalne
// ============================================================
// Forward declaration
static void local_search(int *S, int k, const Weights *w, int ls_iters, int cand_per_iter, bool verbose);

static void seed_set(int *S, int k) {
    typedef struct { int n; float s; } Cand;
    Cand C[MAX_N + 1];
    int m = 0;

    for (int n = 1; n <= MAX_N; n++) {
        float f = (window_len > 0) ? (freq[n] / norm_denominator()) : 0.0f;
        float rec = exp_recency(n);
        C[m++] = (Cand){ n, 0.65f * f + 0.35f * rec };
    }

    for (int i = 0; i < m; i++)
        for (int j = i + 1; j < m; j++)
            if (C[j].s > C[i].s) { Cand t = C[i]; C[i] = C[j]; C[j] = t; }

    for (int i = 0; i < k && i < m; i++)
        S[i] = C[i].n;
    qsort(S, (size_t)k, sizeof(int), cmp_int_asc);
}

// Seed warstwowy: wybiera najlepszy numer z każdego przedziału zakresu [1..MAX_N],
// zapewniając równomierne rozłożenie po całym zakresie
static void seed_set_spread(int *S, int k) {
    typedef struct { int n; float s; } Cand;
    Cand C[MAX_N + 1];
    int m = 0;

    for (int n = 1; n <= MAX_N; n++) {
        float f = (window_len > 0) ? (freq[n] / norm_denominator()) : 0.0f;
        float rec = exp_recency(n);
        C[m++] = (Cand){ n, 0.65f * f + 0.35f * rec };
    }

    // Podział [1..MAX_N] na k przedziałów, wybór najlepszego z każdego
    float bucket_size = (float)MAX_N / (float)k;
    int picked = 0;
    for (int b = 0; b < k && picked < k; b++) {
        int lo = (int)(b * bucket_size) + 1;
        int hi = (int)((b + 1) * bucket_size);
        if (hi > MAX_N) hi = MAX_N;
        if (b == k - 1) hi = MAX_N;

        int best_n = lo;
        float best_s = -1.0f;
        for (int i = 0; i < m; i++) {
            if (C[i].n >= lo && C[i].n <= hi) {
                bool already = false;
                for (int j = 0; j < picked; j++)
                    if (S[j] == C[i].n) { already = true; break; }
                if (!already && C[i].s > best_s) {
                    best_s = C[i].s;
                    best_n = C[i].n;
                }
            }
        }
        S[picked++] = best_n;
    }
    qsort(S, (size_t)k, sizeof(int), cmp_int_asc);
}

static void local_search(int *S, int k, const Weights *w, int ls_iters, int cand_per_iter, bool verbose) {
    float best = objective(S, k, w);
    int bestS[64];
    memcpy(bestS, S, sizeof(int) * (size_t)k);

    for (int it = 0; it < ls_iters; it++) {
        int idx = rand() % k;
        int old = S[idx];

        float local_best = best;

        for (int t = 0; t < cand_per_iter; t++) {
            int cand = 1 + (rand() % MAX_N);
            if (cand == old || set_contains(S, k, cand)) continue;

            // Kopia próbna, aby S nie było modyfikowane podczas oceny kandydata
            int trial[64];
            memcpy(trial, S, sizeof(int) * (size_t)k);
            trial[idx] = cand;
            qsort(trial, (size_t)k, sizeof(int), cmp_int_asc);

            float val = objective(trial, k, w);
            if (val > local_best) {
                local_best = val;
                memcpy(bestS, trial, sizeof(int) * (size_t)k);
            }
        }

        if (local_best > best) {
            best = local_best;
            memcpy(S, bestS, sizeof(int) * (size_t)k);
        }

        // Raportowanie postępu
        gui_set_progress((float)it / (float)ls_iters);
        if ((it % 1000) == 0) {
            gui_set_status("Local search: %d/%d (best=%.4f)", it, ls_iters, best);
            if (verbose && it != 0) {
                fprintf(stderr, "  LS progress: it=%d/%d best=%.6f\n", it, ls_iters, best);
                fflush(stderr);
            }
        }
    }
    gui_set_progress(1.0f);
}

// ============================================================
// Kalibracja walk-forward (tylko tryb full)
// ============================================================
static float utility_from_hits(int hits) {
    switch (hits) {
        case 6: return 1000.0f;
        case 5: return 50.0f;
        case 4: return 5.0f;
        case 3: return 1.0f;
        default: return 0.0f;
    }
}

static float walk_forward_score(const Weights *w, int train_win, int step, int K) {
    if (draws_total <= train_win) return 0.0f;

    float sumU = 0.0f;
    int n = 0;

    for (int t = train_win; t < draws_total; t += step) {
        build_stats_window(t - train_win, t);

        int S[64];
        seed_set(S, K);
        local_search(S, K, w, LS_ITERS_FAST, CAND_FAST, false);

        int hits = count_hits(history[t], S, K);
        sumU += utility_from_hits(hits);
        n++;
    }

    return (n ? sumU / (float)n : 0.0f);
}

static Weights calibrate_weights(int train_win, int step, int K) {
    Weights best = {1.0f, 0.4f, 1.0f, 0.05f, 0.3f};
    float bestScore = -1.0f;

    fprintf(stderr, "Calibrating weights (walk-forward)...\n");

    for (float wf = 0.5f; wf <= 2.0f; wf += 0.5f) {
        for (float wp = 0.0f; wp <= 1.5f; wp += 0.5f) {
            for (float wc = 0.5f; wc <= 2.0f; wc += 0.5f) {
                for (float wcen = 0.0f; wcen <= 0.1f; wcen += 0.05f) {
                    for (float wg = 0.0f; wg <= 0.6f; wg += 0.3f) {
                        Weights w = { wf, wp, wc, wcen, wg };
                        float s = walk_forward_score(&w, train_win, step, K);
                        if (s > bestScore) {
                            bestScore = s;
                            best = w;
                        }
                    }
                }
            }
        }
    }

    fprintf(stderr,
        "Calibrated (proxy=%.4f): w_freq=%.2f w_pair=%.2f w_crowd=%.2f w_center=%.2f w_gap=%.2f\n",
        bestScore, best.w_freq, best.w_pair, best.w_crowd, best.w_center, best.w_gap);

    return best;
}

// ============================================================
// Catch-up fetch: pobieranie brakujących losowań z ostatnich N dni
// ============================================================
// Stałe CATCHUP_* zdefiniowane w lotto.h

static void subtract_days(int *y, int *m, int *d, int n) {
    for (int i = 0; i < n; i++) {
        (*d)--;
        if (*d < 1) {
            (*m)--;
            if (*m < 1) { *m = 12; (*y)--; }
            *d = days_in_month(*y, *m);
        }
    }
}

static int is_draw_day_for_game(const char *game_type, int y, int m, int d) {
    if (strcmp(game_type, GAME_TYPE_MINI) == 0) {
        // Mini Lotto: Mon-Sat (not Sunday; dow_sakamoto returns 0 for Sunday)
        return dow_sakamoto(y, m, d) != 0;
    }
    return is_lotto_draw_day(y, m, d);
}

// ============================================================
// Funkcje pomocnicze wyjścia
// ============================================================
static void print_set(const char *label, const int *S, int k) {
    printf("%s", label);
    for (int i = 0; i < k; i++)
        printf("%d%s", S[i], (i + 1 < k ? " " : "\n"));
}

static void print_combinations_6_of_k(const int *S, int k) {
    for (int i0 = 0; i0 < k; i0++)
    for (int i1 = i0+1; i1 < k; i1++)
    for (int i2 = i1+1; i2 < k; i2++)
    for (int i3 = i2+1; i3 < k; i3++)
    for (int i4 = i3+1; i4 < k; i4++)
    for (int i5 = i4+1; i5 < k; i5++)
        printf("%d %d %d %d %d %d\n", S[i0], S[i1], S[i2], S[i3], S[i4], S[i5]);
}

static void report_metrics(const int *S, int k, const Weights *w) {
    float a = set_freq_recency(S, k);
    float b = set_pair_score(S, k);
    float c = crowd_penalty(S, k);
    float g = set_gap_score(S, k);

    int even = 0;
    for (int i = 0; i < k; i++)
        if ((S[i] % 2) == 0) even++;

    printf("\n[METRICS]\n");
    printf("freq_recency = %.6f\n", a);
    printf("pair_PMI     = %.6f\n", b);
    printf("crowd_pen    = %.6f\n", c);
    printf("gap_score    = %.6f\n", g);
    printf("sum          = %d\n", set_sum(S, k));
    printf("evens        = %d/%d\n", even, k);
    printf("objective    = %.6f\n", objective(S, k, w));
}

// ============================================================
// CMD: fetch — pobieranie historii losowań z API
// ============================================================
static int add_draw_if_new_generic(const int *draw, int draw_size, KeySet *ks, FILE *out) {
    int tmp[16];
    if (draw_size > (int)(sizeof(tmp) / sizeof(tmp[0]))) return 0;

    memcpy(tmp, draw, sizeof(int) * (size_t)draw_size);
    qsort(tmp, (size_t)draw_size, sizeof(int), cmp_int_asc);

    char key[128] = {0};
    size_t used = 0;
    for (int i = 0; i < draw_size; i++) {
        int w = snprintf(key + used, sizeof(key) - used, "%d%s", tmp[i], (i + 1 < draw_size) ? " " : "");
        if (w < 0 || (size_t)w >= sizeof(key) - used) return 0;
        used += (size_t)w;
    }

    if (keyset_contains(ks, key)) return 0;
    fprintf(out, "%s\n", key);
    keyset_add(ks, key);
    return 1;
}

static int parse_draw_array(const cJSON *arr, int *draw, int draw_size, int max_n, char *err, size_t err_sz) {
    if (!cJSON_IsArray(arr)) {
        snprintf(err, err_sz, "Pole 'resultsJson' nie jest tablica.");
        return 0;
    }

    int n = cJSON_GetArraySize((cJSON *)arr);
    if (n < draw_size) {
        snprintf(err, err_sz, "Pole 'resultsJson' ma %d elementow (oczekiwano >= %d).", n, draw_size);
        return 0;
    }

    for (int i = 0; i < draw_size; i++) {
        cJSON *it = cJSON_GetArrayItem((cJSON *)arr, i);
        if (!cJSON_IsNumber(it)) {
            snprintf(err, err_sz, "Element %d w 'resultsJson' nie jest liczba.", i);
            return 0;
        }

        int v = it->valueint;
        if ((double)v != it->valuedouble || v < 1 || v > max_n) {
            snprintf(err, err_sz, "Niepoprawna wartosc w 'resultsJson[%d]': %.6f", i, it->valuedouble);
            return 0;
        }
        draw[i] = v;
    }

    return 1;
}

static int parse_results_json_string(const char *json_str, int *draw, int draw_size, int max_n, char *err, size_t err_sz) {
    cJSON *inner = cJSON_Parse(json_str);
    if (!inner) {
        const char *ep = cJSON_GetErrorPtr();
        snprintf(err, err_sz, "Nie mozna sparsowac resultsJson: %s", ep ? ep : "nieznany blad");
        return 0;
    }

    int ok = parse_draw_array(inner, draw, draw_size, max_n, err, err_sz);
    cJSON_Delete(inner);
    return ok;
}

static void scan_for_results_json(
    const cJSON *node,
    const char *game_type,
    KeySet *ks,
    FILE *out,
    int draw_size,
    int max_n,
    int *appended,
    int *found,
    int *parse_errors,
    char *err,
    size_t err_sz
) {
    if (!node) return;

    if (cJSON_IsObject(node)) {
        // Sprawdź czy obiekt zawiera gameType pasujący do oczekiwanego
        const cJSON *gt = cJSON_GetObjectItemCaseSensitive(node, "gameType");
        int type_matches = (!gt || !cJSON_IsString(gt) || !gt->valuestring ||
                            strcmp(gt->valuestring, game_type) == 0);

        for (const cJSON *ch = node->child; ch; ch = ch->next) {
            if (type_matches && ch->string && strcmp(ch->string, "resultsJson") == 0) {
                int draw[16] = {0};
                int ok = 0;
                (*found)++;

                if (cJSON_IsString(ch) && ch->valuestring) {
                    ok = parse_results_json_string(ch->valuestring, draw, draw_size, max_n, err, err_sz);
                } else if (cJSON_IsArray(ch)) {
                    ok = parse_draw_array(ch, draw, draw_size, max_n, err, err_sz);
                } else {
                    snprintf(err, err_sz, "Pole 'resultsJson' ma nieobslugiwany typ.");
                }

                if (ok) {
                    int added = add_draw_if_new_generic(draw, draw_size, ks, out);
                    *appended += added;

                    // Wypisz pobrane numery z informacją o statusie
                    char nums[128] = {0};
                    size_t pos = 0;
                    int sorted[16];
                    memcpy(sorted, draw, sizeof(int) * (size_t)draw_size);
                    qsort(sorted, (size_t)draw_size, sizeof(int), cmp_int_asc);
                    for (int i = 0; i < draw_size; i++) {
                        int w = snprintf(nums + pos, sizeof(nums) - pos, "%d%s",
                                         sorted[i], (i + 1 < draw_size) ? " " : "");
                        if (w < 0 || (size_t)w >= sizeof(nums) - pos) break;
                        pos += (size_t)w;
                    }
                    printf("  [%s] %s\n", added ? "DODANO" : "juz dodane", nums);
                } else {
                    (*parse_errors)++;
                }
            }
            scan_for_results_json(ch, game_type, ks, out, draw_size, max_n, appended, found, parse_errors, err, err_sz);
        }
        return;
    }

    if (cJSON_IsArray(node)) {
        for (const cJSON *ch = node->child; ch; ch = ch->next)
            scan_for_results_json(ch, game_type, ks, out, draw_size, max_n, appended, found, parse_errors, err, err_sz);
    }
}

static int extract_and_append_draws(
    const char *json,
    const char *game_type,
    KeySet *ks,
    FILE *out,
    int draw_size,
    int max_n,
    int *parse_errors,
    char *err,
    size_t err_sz
) {
    if (parse_errors) *parse_errors = 0;
    if (err_sz > 0) err[0] = '\0';

    if (!json || !*json) {
        snprintf(err, err_sz, "Pusta odpowiedz JSON.");
        if (parse_errors) *parse_errors = 1;
        return -1;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        const char *ep = cJSON_GetErrorPtr();
        snprintf(err, err_sz, "Nie mozna sparsowac odpowiedzi API: %s", ep ? ep : "nieznany blad");
        if (parse_errors) *parse_errors = 1;
        return -1;
    }

    int appended = 0;
    int found = 0;
    int errs = 0;

    scan_for_results_json(root, game_type, ks, out, draw_size, max_n, &appended, &found, &errs, err, err_sz);
    cJSON_Delete(root);

    if (found == 0) {
        snprintf(err, err_sz, "Brak pola 'resultsJson' w odpowiedzi API.");
        if (parse_errors) *parse_errors = 1;
        return -1;
    }

    if (parse_errors) *parse_errors = errs;
    return appended;
}

// ============================================================
// Catch-up fetch: pobieranie brakujących losowań z ostatnich N dni
// ============================================================
int catchup_fetch_draws_for_game(const char *game_type, const char *history_file, int draw_size, int max_n) {
    char secret[512];
    if (!load_api_secret(secret, sizeof(secret))) {
        return -1;
    }

    // Określenie zakresu dat: koniec = dziś
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    int y2 = tm_now->tm_year + 1900;
    int m2 = tm_now->tm_mon + 1;
    int d2 = tm_now->tm_mday;

    // Budowanie zbioru kluczy z istniejącej historii do deduplikacji
    KeySet ks;
    keyset_init(&ks);
    {
        FILE *f = fopen(history_file, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                int vals[16] = {0};
                int ok = 1;
                const char *p = line;
                for (int i = 0; i < draw_size; i++) {
                    char *end = NULL;
                    long v = strtol(p, &end, 10);
                    if (end == p || v < 1 || v > max_n) { ok = 0; break; }
                    vals[i] = (int)v;
                    p = end;
                }
                if (ok) {
                    int tmp[16];
                    memcpy(tmp, vals, sizeof(int) * (size_t)draw_size);
                    qsort(tmp, (size_t)draw_size, sizeof(int), cmp_int_asc);

                    char key[128] = {0};
                    size_t used = 0;
                    for (int i = 0; i < draw_size; i++) {
                        int w = snprintf(key + used, sizeof(key) - used, "%d%s", tmp[i], (i + 1 < draw_size) ? " " : "");
                        if (w < 0 || (size_t)w >= sizeof(key) - used) { ok = 0; break; }
                        used += (size_t)w;
                    }
                    if (ok) keyset_add(&ks, key);
                }
            }
            fclose(f);
        }
    }

    // Wybór okna lookback: bootstrap 2-letni gdy historia jest zbyt mała
    int existing_draws = (int)ks.count;
    int lookback_days = (existing_draws < CATCHUP_BOOTSTRAP_MIN)
                        ? CATCHUP_BOOTSTRAP_DAYS
                        : CATCHUP_DAYS;
    if (existing_draws < CATCHUP_BOOTSTRAP_MIN)
        fprintf(stderr, "Historia zbyt mala (%d losowan) — bootstrap: pobieranie z ostatnich %d dni.\n",
                existing_draws, lookback_days);

    int y1 = y2, m1 = m2, d1 = d2;
    subtract_days(&y1, &m1, &d1, lookback_days);

    FILE *out = fopen(history_file, "a");
    if (!out) {
        perror("fopen");
        keyset_free(&ks);
        return -1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int appended = 0, requests = 0, failures = 0, consecutive_429 = 0;
    int y = y1, m = m1, d = d1;

    fprintf(stderr, "Pobieranie brakujacych losowan (%04d-%02d-%02d .. %04d-%02d-%02d)...\n",
            y1, m1, d1, y2, m2, d2);

    while (!(y > y2 || (y == y2 && m > m2) || (y == y2 && m == m2 && d > d2))) {
        if (is_draw_day_for_game(game_type, y, m, d)) {
            char drawDate[32];
            snprintf(drawDate, sizeof(drawDate), "%04d-%02d-%02dT00:00:00Z", y, m, d);

            char url[512];
            snprintf(url, sizeof(url),
                     "%s?gameType=%s&drawDate=%s&index=1&size=50&sort=drawDate&order=ASC",
                     API_BASE, game_type, drawDate);

            HttpResp r = http_get(url, secret);
            requests++;

            gui_set_status("Fetch: %04d-%02d-%02d (nowe: %d)", y, m, d, appended);

            if (r.curl_rc == CURLE_OK && r.http_code == 200) {
                char json_err[256] = {0};
                int parse_errors = 0;
                int added = extract_and_append_draws(r.body, game_type, &ks, out, draw_size, max_n, &parse_errors, json_err, sizeof(json_err));

                if (added < 0 || parse_errors > 0) {
                    fprintf(stderr,
                            "JSON error for %04d-%02d-%02d: %s (parse_errors=%d)\n",
                            y, m, d,
                            json_err[0] ? json_err : "nieznany blad",
                            parse_errors);
                    failures++;
                }

                if (added > 0) {
                    fflush(out);
                    appended += added;
                }
                free(r.body);
            } else if (r.curl_rc == CURLE_OK && r.http_code == 429) {
                consecutive_429++;
                long wait = (r.retry_after_s > 0) ? r.retry_after_s * 1000L + 200 : 5000;
                fprintf(stderr, "HTTP 429, czekam %ld ms... (consecutive=%d)\n", wait, consecutive_429);
                free(r.body);
                if (consecutive_429 >= 10) {
                    fprintf(stderr, "Too many consecutive 429s (%d). Stopping catchup.\n", consecutive_429);
                    failures++;
                    break;
                }
                msleep(wait);
                continue; // retry same day
            } else if (r.curl_rc == CURLE_OK && r.http_code == 404) {
                // For some dates API may legitimately return no draw result yet.
                fprintf(stderr, "%s %04d-%02d-%02d: brak wynikow (HTTP 404).\n", game_type, y, m, d);
                free(r.body);
            } else {
                char ctx[128];
                snprintf(ctx, sizeof(ctx), "Fetch %s %04d-%02d-%02d", game_type, y, m, d);
                report_fetch_error(ctx, r.curl_rc, r.http_code, r.body);
                failures++;
                free(r.body);
            }
            if (requests > 1) msleep(CATCHUP_SLEEP);
        }
        next_day(&y, &m, &d);
    }

    curl_global_cleanup();
    fclose(out);
    keyset_free(&ks);

    if (appended > 0) {
        fprintf(stderr, "Pobrano %d nowych losowan (%s, zapytan: %d).\n", appended, game_type, requests);
    } else {
        fprintf(stderr, "Historia aktualna (%s, zapytan: %d).\n", game_type, requests);
    }

    if (failures > 0) {
        fprintf(stderr, "Bledy pobierania dla %s: %d\n", game_type, failures);
        return -1;
    }

    return appended;
}

static int catchup_fetch_draws(void) {
    int rc = catchup_fetch_draws_for_game(GAME_TYPE, HISTORY_FILE, DRAW_SIZE, MAX_N);
    if (rc < 0) return rc;

    // Reload main Lotto history into memory
    load_history();
    return rc;
}

static int cmd_fetch_for_game(
    int argc,
    char **argv,
    const char *game_type,
    const char *history_file,
    int draw_size,
    int max_n,
    const char *cmd_name
) {
    char secret[512];
    if (!load_api_secret(secret, sizeof(secret))) {
        return 2;
    }

    const char *from_str = NULL, *to_str = NULL;
    long sleep_ms_val = 1200;
    int max_req = 400, max_429 = 10;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) from_str = argv[++i];
        else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) to_str = argv[++i];
        else if (strcmp(argv[i], "--sleep-ms") == 0 && i + 1 < argc) sleep_ms_val = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--max-req") == 0 && i + 1 < argc) max_req = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-429") == 0 && i + 1 < argc) max_429 = atoi(argv[++i]);
    }

    if (!from_str || !to_str) {
        fprintf(stderr,
                "Usage: lotto %s --from YYYY-MM-DD --to YYYY-MM-DD [--sleep-ms N] [--max-req N] [--max-429 N]\n",
                cmd_name);
        return 2;
    }

    int y1, m1, d1, y2, m2, d2;
    if (!parse_ymd(from_str, &y1, &m1, &d1) || !parse_ymd(to_str, &y2, &m2, &d2)) {
        fprintf(stderr, "Invalid date format. Use YYYY-MM-DD.\n");
        return 2;
    }

    KeySet ks;
    keyset_init(&ks);

    // Wczytanie istniejących linii historii jako kluczy do deduplikacji
    {
        FILE *f = fopen(history_file, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                int vals[16] = {0};
                int ok = 1;
                const char *p = line;
                for (int i = 0; i < draw_size; i++) {
                    char *end = NULL;
                    long v = strtol(p, &end, 10);
                    if (end == p || v < 1 || v > max_n) { ok = 0; break; }
                    vals[i] = (int)v;
                    p = end;
                }

                if (ok) {
                    int tmp[16];
                    memcpy(tmp, vals, sizeof(int) * (size_t)draw_size);
                    qsort(tmp, (size_t)draw_size, sizeof(int), cmp_int_asc);

                    char key[128] = {0};
                    size_t used = 0;
                    for (int i = 0; i < draw_size; i++) {
                        int w = snprintf(key + used, sizeof(key) - used, "%d%s", tmp[i], (i + 1 < draw_size) ? " " : "");
                        if (w < 0 || (size_t)w >= sizeof(key) - used) { ok = 0; break; }
                        used += (size_t)w;
                    }
                    if (ok) keyset_add(&ks, key);
                }
            }
            fclose(f);
        }
    }

    FILE *out = fopen(history_file, "a");
    if (!out) { perror("fopen"); keyset_free(&ks); return 1; }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int appended_total = 0, requests = 0, consecutive_429 = 0, failures = 0;
    long backoff_ms = sleep_ms_val;

    int y = y1, m = m1, d = d1;
    while (!(y > y2 || (y == y2 && m > m2) || (y == y2 && m == m2 && d > d2))) {
        if (requests >= max_req) {
            fprintf(stderr, "Request limit reached: %d\n", max_req);
            break;
        }

        if (is_draw_day_for_game(game_type, y, m, d)) {
            char drawDate[32];
            snprintf(drawDate, sizeof(drawDate), "%04d-%02d-%02dT00:00:00Z", y, m, d);

            char url[512];
            snprintf(url, sizeof(url),
                     "%s?gameType=%s&drawDate=%s&index=1&size=50&sort=drawDate&order=ASC",
                     API_BASE, game_type, drawDate);

            HttpResp r = http_get(url, secret);
            requests++;

            // Aktualizacja postępu
            gui_set_progress((float)requests / (float)max_req);
            gui_set_status("Fetch %s: %04d-%02d-%02d (req %d/%d, new: %d)",
                           game_type, y, m, d, requests, max_req, appended_total);

            if (r.curl_rc != CURLE_OK) {
                fprintf(stderr, "CURL error for %04d-%02d-%02d: %s\n",
                        y, m, d, curl_easy_strerror(r.curl_rc));
                failures++;
                free(r.body);
                msleep(sleep_ms_val);
                next_day(&y, &m, &d);
                continue;
            }

            if (r.http_code == 200) {
                consecutive_429 = 0;
                backoff_ms = sleep_ms_val;
                char json_err[256] = {0};
                int parse_errors = 0;
                int added = extract_and_append_draws(r.body, game_type, &ks, out, draw_size, max_n, &parse_errors, json_err, sizeof(json_err));
                if (added < 0 || parse_errors > 0) {
                    fprintf(stderr,
                            "JSON error for %04d-%02d-%02d: %s (parse_errors=%d)\n",
                            y, m, d,
                            json_err[0] ? json_err : "nieznany blad",
                            parse_errors);
                    failures++;
                }
                if (added > 0) appended_total += added;
                free(r.body);
                msleep(sleep_ms_val);
            } else if (r.http_code == 429) {
                consecutive_429++;
                long wait_ms;
                if (r.retry_after_s > 0)
                    wait_ms = (r.retry_after_s * 1000L) + 200;
                else {
                    wait_ms = backoff_ms;
                    if (backoff_ms < 120000) backoff_ms *= 2;
                    if (wait_ms > 120000) wait_ms = 120000;
                }

                fprintf(stderr, "HTTP 429 for %04d-%02d-%02d. Sleep %ld ms (consecutive=%d)\n",
                        y, m, d, wait_ms, consecutive_429);
                free(r.body);

                if (consecutive_429 >= max_429) {
                    fprintf(stderr, "Too many consecutive 429s (%d). Stopping.\n", max_429);
                    failures++;
                    break;
                }
                msleep(wait_ms);
            } else if (r.http_code == 404) {
                fprintf(stderr, "%s %04d-%02d-%02d: brak wynikow (HTTP 404).\n", game_type, y, m, d);
                free(r.body);
                msleep(sleep_ms_val);
            } else {
                char ctx[128];
                snprintf(ctx, sizeof(ctx), "Fetch %s %04d-%02d-%02d", game_type, y, m, d);
                report_fetch_error(ctx, r.curl_rc, r.http_code, r.body);
                failures++;
                free(r.body);
                msleep(sleep_ms_val);
            }
        }

        next_day(&y, &m, &d);
    }

    curl_global_cleanup();
    fclose(out);
    keyset_free(&ks);

    printf("Done (%s). Requests: %d, new draws appended: %d\n", game_type, requests, appended_total);
    return (failures > 0) ? 1 : 0;
}

int cmd_fetch(int argc, char **argv) {
    return cmd_fetch_for_game(argc, argv, GAME_TYPE, HISTORY_FILE, DRAW_SIZE, MAX_N, "fetch");
}

int cmd_fetch_mini(int argc, char **argv) {
    return cmd_fetch_for_game(argc, argv, GAME_TYPE_MINI, MINI_HISTORY_FILE, MINI_DRAW_SIZE, MINI_MAX_N, "fetch-mini");
}

// ============================================================
// CMD: optimize — optymalizacja zestawu Lotto
// ============================================================
int cmd_optimize(int argc, char **argv) {
    int K = 8;
    int train_win = 600;
    bool full_mode = false;
    unsigned int seed = DEFAULT_RNG_SEED;
    float decay_lambda = DEFAULT_DECAY_LAMBDA;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) K = atoi(argv[++i]);
        else if (strcmp(argv[i], "--train") == 0 && i + 1 < argc) train_win = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_seed_arg(argv[++i], &seed)) {
                fprintf(stderr, "Invalid --seed value. Use unsigned integer.\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--decay-lambda") == 0 && i + 1 < argc) {
            if (!parse_float_arg(argv[++i], &decay_lambda) || decay_lambda < 0.0f || decay_lambda > 1.0f) {
                fprintf(stderr, "Invalid --decay-lambda value. Use float in range 0..1.\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "full") == 0) full_mode = true;
            else if (strcmp(argv[i], "fast") == 0) full_mode = false;
            else { fprintf(stderr, "Unknown mode: %s (use 'fast' or 'full')\n", argv[i]); return 2; }
        }
    }

    g_decay_lambda = decay_lambda;
    srand(seed);
    fprintf(stderr, "RNG seed: %u, decay_lambda: %.4f\n", seed, g_decay_lambda);

    if (K < 6 || K > 15) { fprintf(stderr, "K must be 6..15\n"); return 2; }

    gui_set_status("Loading history (%s)...", HISTORY_FILE);
    load_history();

    // Pobranie brakujących losowań z API
    gui_set_status("Pobieranie brakujących losowań...");
    if (catchup_fetch_draws() < 0) {
        fprintf(stderr, "Nie udalo sie pobrac brakujacych losowan Lotto.\n");
        return 1;
    }

    if (draws_total < 50) {
        fprintf(stderr, "History too small: %d draws.\n", draws_total);
        return 1;
    }

    if (train_win >= draws_total) train_win = draws_total - 1;
    if (train_win < 50) train_win = 50;

    Weights w;
    int ls_iters, cand;

    if (full_mode) {
        gui_set_status("Full mode: walk-forward calibration...");
        fprintf(stderr, "Full mode: walk-forward calibration + %d LS iterations.\n", LS_ITERS_FULL);
        w = calibrate_weights(train_win, DEFAULT_CALIB_STEP, K);
        ls_iters = LS_ITERS_FULL;
        cand = CAND_FULL;
    } else {
        gui_set_status("Fast mode: optimizing...");
        fprintf(stderr, "Fast mode: fixed weights, %d LS iterations.\n", LS_ITERS_FAST);
        w = (Weights){ 1.4f, 0.8f, 0.25f, 0.05f, 0.3f };
        ls_iters = LS_ITERS_FAST;
        cand = CAND_FAST;
    }

    build_stats_window(draws_total - train_win, draws_total);

    int S[64];
    seed_set(S, K);
    print_set("Seed: ", S, K);

    local_search(S, K, &w, ls_iters, cand, true);
    print_set("\nBest set: ", S, K);

    report_metrics(S, K, &w);

    printf("\n[COMBINATIONS 6-of-%d]\n", K);
    print_combinations_6_of_k(S, K);

    return 0;
}

// ============================================================
// CMD: backtest — test historyczny algorytmu
// ============================================================

// Prawdopodobieństwo hipergeometryczne: P(X = k) dla losowania DRAW_SIZE z N piłek,
// z K zaznaczonymi. Używane do obliczania bazowej (losowej) skuteczności trafień.
static double log_comb(int n, int k) {
    if (k < 0 || k > n) return -1e30;
    return lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0);
}

static double hypergeom_pmf(int k, int K, int N, int n) {
    return exp(log_comb(K, k) + log_comb(N - K, n - k) - log_comb(N, n));
}

// ---- Losowa baza odniesienia Monte Carlo ----

// Generuje losowy podbiór K elementów z [1..MAX_N] (Fisher-Yates)
static void random_subset(int *S, int K) {
    int pool[MAX_N];
    for (int i = 0; i < MAX_N; i++) pool[i] = i + 1;
    for (int i = 0; i < K; i++) {
        int j = i + rand() % (MAX_N - i);
        int tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
        S[i] = pool[i];
    }
    qsort(S, (size_t)K, sizeof(int), cmp_int_asc);
}

// Oblicza C(n, k) dokładnie (dla małych wartości, K <= 15)
static long comb_nk(int n, int k) {
    if (k < 0 || k > n) return 0;
    if (k == 0 || k == n) return 1;
    if (k > n - k) k = n - k;
    long r = 1;
    for (int i = 0; i < k; i++) {
        r = r * (n - i) / (i + 1);
    }
    return r;
}

// Statystyki okresu dla analizy kroczącej
typedef struct {
    int start_draw;
    int end_draw;
    int test_count;
    int hits_3plus;
    float total_utility;
} PeriodStats;

// ============================================================
// Autotune: automatyczny dobór okna treningowego
// ============================================================
typedef struct {
    int best_train;
    float best_lift;
} AutotuneResult;

static AutotuneResult autotune_train_window(int K, float progress_base, float progress_span) {
    AutotuneResult res = { 200, -1.0f };
    int train_vals[] = { 100, 150, 200, 300, 400, 500 };
    int n_tr = (int)(sizeof(train_vals) / sizeof(train_vals[0]));

    fprintf(stderr, "Autotune: skanowanie okien treningowych...\n");

    for (int ti = 0; ti < n_tr; ti++) {
        int tw = train_vals[ti];
        if (tw >= draws_total) continue;

        int hits3 = 0, tested = 0;
        Weights wt = { 1.0f, 0.5f, 2.0f, 0.05f, 0.3f };
        for (int t = tw; t < draws_total; t += 5) {
            build_stats_window(t - tw, t);
            int S[64];
            if (K <= DRAW_SIZE) {
                seed_set_spread(S, K);
            } else {
                seed_set(S, K);
            }
            local_search(S, K, &wt, 2000, 80, false);
            if (count_hits(history[t], S, K) >= 3) hits3++;
            tested++;
        }
        float rate = (tested > 0) ? (float)hits3 / (float)tested : 0.0f;
        double hg3 = 0;
        for (int h = 3; h <= DRAW_SIZE; h++)
            hg3 += hypergeom_pmf(h, K, MAX_N, DRAW_SIZE);
        float lift = (hg3 > 0) ? (float)(rate / hg3) : 0.0f;

        gui_set_progress(progress_base + progress_span * (float)(ti + 1) / (float)n_tr);
        gui_set_status("Autotune: okno %d, lift=%.2fx", tw, lift);
        fprintf(stderr, "  train=%d: 3+rate=%.3f%% lift=%.2fx\n", tw, 100.0 * rate, lift);

        if (lift > res.best_lift) { res.best_lift = lift; res.best_train = tw; }
    }

    fprintf(stderr, "Autotune: najlepsze okno = %d (lift=%.2fx)\n\n", res.best_train, res.best_lift);
    return res;
}

int cmd_backtest(int argc, char **argv) {
    int K = 8;
    int train_win = -1;  // -1 = autotune
    int step = 1;
    int mc_sims = MC_SIMS;
    bool autotune = true;
    unsigned int seed = DEFAULT_RNG_SEED;
    float decay_lambda = DEFAULT_DECAY_LAMBDA;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) K = atoi(argv[++i]);
        else if (strcmp(argv[i], "--train") == 0 && i + 1 < argc) {
            train_win = atoi(argv[++i]);
            autotune = false;
        }
        else if (strcmp(argv[i], "--step") == 0 && i + 1 < argc) step = atoi(argv[++i]);
        else if (strcmp(argv[i], "--mc") == 0 && i + 1 < argc) mc_sims = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_seed_arg(argv[++i], &seed)) {
                fprintf(stderr, "Invalid --seed value. Use unsigned integer.\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--decay-lambda") == 0 && i + 1 < argc) {
            if (!parse_float_arg(argv[++i], &decay_lambda) || decay_lambda < 0.0f || decay_lambda > 1.0f) {
                fprintf(stderr, "Invalid --decay-lambda value. Use float in range 0..1.\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--autotune") == 0) autotune = true;
        else if (strcmp(argv[i], "--no-autotune") == 0) autotune = false;
    }

    g_decay_lambda = decay_lambda;
    srand(seed);
    fprintf(stderr, "RNG seed: %u, decay_lambda: %.4f\n", seed, g_decay_lambda);

    if (K < 6 || K > 15) { fprintf(stderr, "K must be 6..15\n"); return 2; }
    if (step < 1) step = 1;
    if (mc_sims < 100) mc_sims = 100;
    if (mc_sims > 50000) mc_sims = 50000;

    gui_set_status("Ładowanie historii...");
    load_history();

    gui_set_status("Pobieranie brakujących losowań...");
    if (catchup_fetch_draws() < 0) {
        fprintf(stderr, "Nie udalo sie pobrac brakujacych losowan Lotto.\n");
        return 1;
    }

    if (draws_total < 50) {
        fprintf(stderr, "History too small: %d draws.\n", draws_total);
        return 1;
    }

    if (autotune) {
        gui_set_status("Autotune okna treningowego...");
        AutotuneResult at = autotune_train_window(K, 0.0f, 0.15f);
        train_win = at.best_train;
        fprintf(stderr, "Backtest użyje okna treningowego: %d (lift=%.2fx)\n\n",
                train_win, at.best_lift);
    } else if (train_win < 0) {
        train_win = 600;
    }

    if (train_win >= draws_total) train_win = draws_total - 1;
    if (train_win < 50) train_win = 50;

    Weights w = { 1.0f, 0.5f, 2.0f, 0.05f, 0.3f };

    int test_draws = 0;
    int hit_counts[DRAW_SIZE + 1] = {0};
    float total_utility = 0.0f;
    int max_drought = 0;
    int current_drought = 0;

    int start_t = train_win;
    int end_t = draws_total;
    int total_test_est = (end_t - start_t + step - 1) / step;

    // Analiza krocząca — podział testu na okresy
    int period_size = total_test_est / 10;
    if (period_size < 10) period_size = 10;
    PeriodStats periods[MAX_PERIODS];
    int n_periods = 0;
    int period_idx = 0;
    if (n_periods < MAX_PERIODS) {
        periods[0] = (PeriodStats){start_t, 0, 0, 0, 0.0f};
        n_periods = 1;
    }

    fprintf(stderr, "=== BACKTEST START ===\n");
    fprintf(stderr, "History: %d draws, training window: %d%s, test: ~%d draws (step=%d)\n",
            draws_total, train_win, autotune ? " (autotune)" : "", total_test_est, step);
    fprintf(stderr, "System K=%d -> C(%d,6)=%ld tickets per draw\n", K, K, comb_nk(K, 6));
    fprintf(stderr, "Monte Carlo simulations: %d\n\n", mc_sims);

    float phase1_base = autotune ? 0.15f : 0.0f;
    float phase1_span = autotune ? 0.45f : 0.60f;
    float phase2_base = autotune ? 0.60f : 0.60f;

    // ---- Faza 1: Backtest algorytmu ----
    gui_set_status("Faza 1/2: Backtest algorytmu...");

    for (int t = start_t; t < end_t; t += step) {
        build_stats_window(t - train_win, t);

        int S[64];
        seed_set(S, K);
        local_search(S, K, &w, LS_ITERS_FAST, CAND_FAST, false);

        int hits = count_hits(history[t], S, K);
        int h_idx = hits > DRAW_SIZE ? DRAW_SIZE : hits;
        hit_counts[h_idx]++;
        total_utility += utility_from_hits(hits);
        test_draws++;

        if (hits >= 3) {
            current_drought = 0;
        } else {
            current_drought++;
            if (current_drought > max_drought) max_drought = current_drought;
        }

        // Śledzenie okresów kroczących
        periods[period_idx].test_count++;
        periods[period_idx].end_draw = t;
        if (hits >= 3) periods[period_idx].hits_3plus++;
        periods[period_idx].total_utility += utility_from_hits(hits);

        if (periods[period_idx].test_count >= period_size && period_idx + 1 < MAX_PERIODS) {
            period_idx++;
            n_periods = period_idx + 1;
            periods[period_idx] = (PeriodStats){t + step, 0, 0, 0, 0.0f};
        }

        // Postęp: faza 1
        float phase1_pct = (float)test_draws / (float)total_test_est;
        gui_set_progress(phase1_base + phase1_pct * phase1_span);
        if (test_draws % 50 == 0) {
            int h3p = hit_counts[3] + hit_counts[4] + hit_counts[5] + hit_counts[6];
            gui_set_status("Faza 1/2: Backtest %d/%d (3+: %d, %.2f%%)",
                           test_draws, total_test_est, h3p,
                           100.0 * (double)h3p / (double)test_draws);
            fprintf(stderr, "  [algo] %d/%d draws | 3+hits=%d (%.2f%%)\n",
                    test_draws, total_test_est, h3p,
                    100.0 * (double)h3p / (double)test_draws);
            fflush(stderr);
        }
    }

    // Zamknięcie ostatniego okresu
    if (n_periods > 0 && periods[n_periods - 1].test_count == 0 && n_periods > 1)
        n_periods--;

    // ---- Faza 2: Losowa baza Monte Carlo ----
    gui_set_status("Faza 2/2: Monte Carlo (%d symulacji)...", mc_sims);
    fprintf(stderr, "\n  [MC] Running %d random simulations...\n", mc_sims);

    // Ile symulacji MC osiągnęło wynik >= algorytmu
    int mc_better_utility = 0;
    int mc_better_3plus = 0;

    int algo_3plus = 0;
    for (int h = 3; h <= DRAW_SIZE; h++) algo_3plus += hit_counts[h];

    // Akumulacja rozkładu trafień MC do uśrednienia
    double mc_avg_hits[DRAW_SIZE + 1] = {0};
    double mc_avg_utility = 0.0;

    // Indeksy losowań testowych dla MC
    int *test_indices = (int *)malloc((size_t)test_draws * sizeof(int));
    if (!test_indices) {
        fprintf(stderr, "OOM: cannot allocate test indices.\n");
        return 1;
    }
    {
        int idx = 0;
        for (int t = start_t; t < end_t && idx < test_draws; t += step)
            test_indices[idx++] = t;
    }

    for (int sim = 0; sim < mc_sims; sim++) {
        int mc_hits_total[DRAW_SIZE + 1] = {0};
        float mc_util = 0.0f;

        for (int di = 0; di < test_draws; di++) {
            int R[64];
            random_subset(R, K);
            int hits = count_hits(history[test_indices[di]], R, K);
            int h_idx = hits > DRAW_SIZE ? DRAW_SIZE : hits;
            mc_hits_total[h_idx]++;
            mc_util += utility_from_hits(hits);
        }

        int mc_3p = 0;
        for (int h = 3; h <= DRAW_SIZE; h++) mc_3p += mc_hits_total[h];

        if (mc_util >= total_utility) mc_better_utility++;
        if (mc_3p >= algo_3plus) mc_better_3plus++;

        for (int h = 0; h <= DRAW_SIZE; h++)
            mc_avg_hits[h] += (double)mc_hits_total[h];
        mc_avg_utility += (double)mc_util;

        // Postęp: faza 2
        if ((sim + 1) % 20 == 0 || sim == mc_sims - 1) {
            float phase2_pct = (float)(sim + 1) / (float)mc_sims;
            gui_set_progress(phase2_base + phase2_pct * (1.0f - phase2_base));
            gui_set_status("Faza 2/2: Monte Carlo %d/%d", sim + 1, mc_sims);
        }
        if ((sim + 1) % 200 == 0) {
            fprintf(stderr, "  [MC] %d/%d simulations done\n", sim + 1, mc_sims);
            fflush(stderr);
        }
    }

    free(test_indices);

    // Normalizacja średnich MC
    for (int h = 0; h <= DRAW_SIZE; h++)
        mc_avg_hits[h] /= (double)mc_sims;
    mc_avg_utility /= (double)mc_sims;

    double p_value_utility = (double)(mc_better_utility + 1) / (double)(mc_sims + 1);
    double p_value_3plus   = (double)(mc_better_3plus + 1) / (double)(mc_sims + 1);

    gui_set_progress(1.0f);
    gui_set_status("Generowanie raportu...");

    // ---- Teoretyczna baza hipergeometryczna ----
    double hg_pmf[DRAW_SIZE + 1];
    for (int h = 0; h <= DRAW_SIZE; h++)
        hg_pmf[h] = hypergeom_pmf(h, K, MAX_N, DRAW_SIZE);

    double hg_3plus = 0.0, hg_4plus = 0.0;
    for (int h = 3; h <= DRAW_SIZE; h++) hg_3plus += hg_pmf[h];
    for (int h = 4; h <= DRAW_SIZE; h++) hg_4plus += hg_pmf[h];

    double hg_utility = 0.0;
    for (int h = 0; h <= DRAW_SIZE; h++)
        hg_utility += hg_pmf[h] * (double)utility_from_hits(h);

    // Rzeczywiste wskaźniki
    int hits_3plus = algo_3plus;
    int hits_4plus = 0;
    for (int h = 4; h <= DRAW_SIZE; h++) hits_4plus += hit_counts[h];

    double actual_3plus = (double)hits_3plus / (double)test_draws;
    double actual_4plus = (double)hits_4plus / (double)test_draws;
    double avg_utility  = (double)total_utility / (double)test_draws;

    // Empiryczne wskaźniki MC
    double mc_rate_3plus = 0.0;
    for (int h = 3; h <= DRAW_SIZE; h++)
        mc_rate_3plus += mc_avg_hits[h];
    mc_rate_3plus /= (double)test_draws;

    double mc_avg_util_per_draw = mc_avg_utility / (double)test_draws;

    long ck6 = comb_nk(K, 6);

    // =============== WYNIKI ===============
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║             BACKTEST RESULTS — Algorithm vs. Random         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("Konfiguracja:\n");
    printf("  Historia:         %d losowań\n", draws_total);
    printf("  Okno treningowe:  %d losowań%s\n", train_win, autotune ? " (autotune)" : "");
    printf("  Test draws:       %d (step=%d)\n", test_draws, step);
    printf("  System size K:    %d\n", K);
    printf("  Tickets/draw:     C(%d,6) = %ld\n", K, ck6);
    printf("  Monte Carlo sims: %d\n\n", mc_sims);

    // --- Tabela rozkładu trafień ---
    printf("┌─────────────────────────────────────────────────────────────┐\n");
    printf("│                    Hit Distribution                         │\n");
    printf("├──────┬───────┬──────────┬──────────┬──────────┬─────────────┤\n");
    printf("│ Hits │ Count │ Algo %%   │ MC Rnd %% │ Theory %%│    Lift     │\n");
    printf("├──────┼───────┼──────────┼──────────┼──────────┼─────────────┤\n");
    for (int h = 0; h <= DRAW_SIZE; h++) {
        double actual_pct = 100.0 * (double)hit_counts[h] / (double)test_draws;
        double mc_pct     = 100.0 * mc_avg_hits[h] / (double)test_draws;
        double theory_pct = 100.0 * hg_pmf[h];
        double lift = (mc_pct > 0.001) ? (actual_pct / mc_pct) : 0.0;
        printf("│  %d   │ %5d │ %7.3f%% │ %7.3f%% │ %7.3f%% │ %7.2fx     │\n",
               h, hit_counts[h], actual_pct, mc_pct, theory_pct, lift);
    }
    printf("└──────┴───────┴──────────┴──────────┴──────────┴─────────────┘\n\n");

    // --- Zagregowane metryki ---
    printf("┌─────────────────────────────────────────────────────────────┐\n");
    printf("│                   Aggregate Metrics                         │\n");
    printf("├─────────────────────────────────────────────────────────────┤\n");

    double lift3 = (mc_rate_3plus > 0) ? actual_3plus / mc_rate_3plus : 0.0;
    double lift4 = (hg_4plus > 0) ? actual_4plus / hg_4plus : 0.0;
    double lift_util = (mc_avg_util_per_draw > 0) ? avg_utility / mc_avg_util_per_draw : 0.0;

    printf("│ Hit rate >=3:  %7.4f%%  (MC random: %7.4f%%)  lift: %.2fx │\n",
           100.0 * actual_3plus, 100.0 * mc_rate_3plus, lift3);
    printf("│ Hit rate >=4:  %7.4f%%  (theory:    %7.4f%%)  lift: %.2fx │\n",
           100.0 * actual_4plus, 100.0 * hg_4plus, lift4);
    printf("│ Avg utility:   %7.4f   (MC random:  %7.4f)  lift: %.2fx │\n",
           avg_utility, mc_avg_util_per_draw, lift_util);
    printf("│ Max drought:   %d draws without >=3 hits                  │\n", max_drought);
    printf("└─────────────────────────────────────────────────────────────┘\n\n");

    // --- Istotność statystyczna ---
    printf("┌─────────────────────────────────────────────────────────────┐\n");
    printf("│              Statistical Significance (Monte Carlo)         │\n");
    printf("├─────────────────────────────────────────────────────────────┤\n");
    printf("│ p-value (utility): %.4f", p_value_utility);
    if (p_value_utility < 0.05)
        printf("  *** SIGNIFICANT (p < 0.05) ***");
    else if (p_value_utility < 0.10)
        printf("  *   marginal (p < 0.10)");
    else
        printf("      not significant");
    printf("      │\n");

    printf("│ p-value (3+ hits): %.4f", p_value_3plus);
    if (p_value_3plus < 0.05)
        printf("  *** SIGNIFICANT (p < 0.05) ***");
    else if (p_value_3plus < 0.10)
        printf("  *   marginal (p < 0.10)");
    else
        printf("      not significant");
    printf("      │\n");

    printf("│                                                             │\n");
    printf("│ Interpretation: p-value = fraction of random simulations    │\n");
    printf("│ that scored >= algorithm.  Lower = better for algorithm.    │\n");
    printf("└─────────────────────────────────────────────────────────────┘\n\n");

    // --- Analiza krocząca ---
    if (n_periods > 1) {
        printf("┌─────────────────────────────────────────────────────────────┐\n");
        printf("│              Rolling Performance (by period)                │\n");
        printf("├────────┬─────────┬───────┬──────────┬──────────────────────┤\n");
        printf("│ Period │ Draws   │ 3+hit │ Rate %%   │ Avg Util             │\n");
        printf("├────────┼─────────┼───────┼──────────┼──────────────────────┤\n");
        for (int p = 0; p < n_periods; p++) {
            if (periods[p].test_count == 0) continue;
            double rate = 100.0 * (double)periods[p].hits_3plus / (double)periods[p].test_count;
            double util = (double)periods[p].total_utility / (double)periods[p].test_count;

            // Pasek wizualny dla wskaźnika trafień
            int bar_len = (int)(rate * 2.0);
            if (bar_len > 20) bar_len = 20;
            char bar[32];
            for (int b = 0; b < bar_len; b++) bar[b] = '#';
            bar[bar_len] = '\0';

            printf("│  %3d   │  %5d  │ %4d  │ %6.2f%%  │ %6.4f %-13s │\n",
                   p + 1, periods[p].test_count, periods[p].hits_3plus, rate, util, bar);
        }
        printf("└────────┴─────────┴───────┴──────────┴──────────────────────┘\n\n");
    }

    // --- Werdykt ---
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                        VERDICT                              ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");

    bool algo_wins = false;
    if (p_value_utility < 0.05 && lift3 > 1.0) {
        printf("║ POSITIVE: Algorithm is statistically significantly better   ║\n");
        printf("║ than random (p=%.4f).                                     ║\n", p_value_utility);
        printf("║ 3+ hit lift: %.2fx, utility lift: %.2fx                     ║\n", lift3, lift_util);
        algo_wins = true;
    } else if (lift3 > 1.05) {
        printf("║ PROMISING: Algorithm shows %.1f%% improvement over random   ║\n", (lift3 - 1.0) * 100.0);
        printf("║ for 3+ hits, but NOT statistically significant (p=%.4f).  ║\n", p_value_3plus);
        printf("║ Consider more test data or MC simulations.                  ║\n");
    } else if (lift3 > 0.95) {
        printf("║ NEUTRAL: Algorithm performs similarly to random selection.   ║\n");
        printf("║ Lift ~%.2fx. No evidence of meaningful edge.                ║\n", lift3);
    } else {
        printf("║ NEGATIVE: Algorithm performs WORSE than random.             ║\n");
        printf("║ Lift: %.2fx. Review weights and approach.                   ║\n", lift3);
    }

    // Podpowiedź kosztowa
    double ticket_cost = 3.0; // cena kuponu w PLN
    double expected_draw_cost = (double)ck6 * ticket_cost;
    printf("║                                                              ║\n");
    printf("║ Cost/draw: %ld tickets x %.0f PLN = %.0f PLN                    ║\n",
           ck6, ticket_cost, expected_draw_cost);
    if (algo_wins) {
        printf("║ Edge exists but lottery EV is still negative in practice.    ║\n");
    }
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}

// ============================================================
// CMD: play — pobierz + analizuj + generuj systemy do gry
// ============================================================
int cmd_play(int argc, char **argv) {
    int max_system = 10;
    int proposals = 1;
    unsigned int seed = DEFAULT_RNG_SEED;
    float decay_lambda = DEFAULT_DECAY_LAMBDA;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--max-system") == 0 && i + 1 < argc)
            max_system = atoi(argv[++i]);
        else if (strcmp(argv[i], "--proposals") == 0 && i + 1 < argc)
            proposals = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_seed_arg(argv[++i], &seed)) {
                fprintf(stderr, "Invalid --seed value. Use unsigned integer.\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--decay-lambda") == 0 && i + 1 < argc) {
            if (!parse_float_arg(argv[++i], &decay_lambda) || decay_lambda < 0.0f || decay_lambda > 1.0f) {
                fprintf(stderr, "Invalid --decay-lambda value. Use float in range 0..1.\n");
                return 2;
            }
        }
    }
    if (proposals < 1) proposals = 1;
    if (proposals > 10) proposals = 10;

    g_decay_lambda = decay_lambda;
    srand(seed);
    fprintf(stderr, "RNG seed: %u, decay_lambda: %.4f\n", seed, g_decay_lambda);

    if (max_system < 7) max_system = 7;
    if (max_system > 12) max_system = 12;

    gui_set_status("Ładowanie historii...");
    load_history();
    gui_set_status("Pobieranie brakujących losowań...");
    if (catchup_fetch_draws() < 0) {
        fprintf(stderr, "Nie udalo sie pobrac brakujacych losowan Lotto.\n");
        return 1;
    }

    if (draws_total < 100) {
        fprintf(stderr, "Za mało losowań w historii: %d (min. 100)\n", draws_total);
        return 1;
    }

    // ---- Faza 1: Autotune okna treningowego ----
    // Używa max_system jako K, aby okno było skalibrowane
    // dla największego generowanego systemu
    gui_set_status("Faza 1/3: Autotune okna treningowego...");
    AutotuneResult at = autotune_train_window(max_system, 0.0f, 0.30f);
    int best_train = at.best_train;
    float best_lift = at.best_lift;

    // ---- Faza 2: Generowanie systemów dla każdego K ----
    gui_set_status("Faza 2/3: Generowanie systemów...");
    gui_set_progress(0.35f);

    build_stats_window(draws_total - best_train, draws_total);

    // Oblicz wynik (score) dla każdego numeru
    float num_score[MAX_N + 1];
    for (int n = 1; n <= MAX_N; n++) {
        float f = (window_len > 0) ? (freq[n] / norm_denominator()) : 0.0f;
        float rec = exp_recency(n);
        num_score[n] = 0.65f * f + 0.35f * rec;
    }

    Weights w_score = { 1.0f, 0.5f, 2.0f, 0.05f, 0.3f };

    // =============== OUTPUT ===============
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║             LOTTO — SYSTEMY DO GRY                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("Analiza: %d losowań, okno treningowe: %d (lift=%.2fx vs random)\n\n",
           draws_total, best_train, best_lift);

    int total_cost = 0;
    int n_systems = max_system - 6;

    for (int K = 7; K <= max_system; K++) {
        long ck6 = comb_nk(K, 6);
        int cost_per = (int)ck6 * 3;

        // Zbierz najlepszych 'proposals' kandydatów
        int top_sys[10][64];
        float top_score[10];
        for (int p = 0; p < proposals; p++) top_score[p] = -1e30f;

        int n_cand = 5000 * proposals;
        float bsz = (float)MAX_N / (float)K;

        for (int c = 0; c < n_cand; c++) {
            int T[64];
            for (int b = 0; b < K; b++) {
                int lo = (int)(b * bsz) + 1;
                int hi = (int)((b + 1) * bsz);
                if (b == K - 1) hi = MAX_N;

                float tw = 0.0f;
                for (int n = lo; n <= hi; n++) tw += num_score[n] + 0.01f;
                float r = ((float)rand() / (float)RAND_MAX) * tw;
                float acc = 0.0f;
                T[b] = lo;
                for (int n = lo; n <= hi; n++) {
                    acc += num_score[n] + 0.01f;
                    if (acc >= r) { T[b] = n; break; }
                }
            }
            qsort(T, (size_t)K, sizeof(int), cmp_int_asc);

            bool unique = true;
            for (int i = 1; i < K; i++)
                if (T[i] == T[i-1]) { unique = false; break; }
            if (!unique) continue;

            float sc = objective(T, K, &w_score);

            // Znajdź najsłabszą pozycję w top-N
            int worst = 0;
            for (int p = 1; p < proposals; p++)
                if (top_score[p] < top_score[worst]) worst = p;

            if (sc > top_score[worst]) {
                // Sprawdź czy ten zestaw nie jest duplikatem istniejących propozycji
                bool dup = false;
                for (int p = 0; p < proposals; p++) {
                    if (top_score[p] <= -1e29f) continue;
                    if (memcmp(T, top_sys[p], sizeof(int) * (size_t)K) == 0) { dup = true; break; }
                }
                if (!dup) {
                    top_score[worst] = sc;
                    memcpy(top_sys[worst], T, sizeof(int) * (size_t)K);
                }
            }
        }

        // Sortuj propozycje malejąco wg score
        for (int i = 0; i < proposals; i++)
            for (int j = i + 1; j < proposals; j++)
                if (top_score[j] > top_score[i]) {
                    float ts = top_score[i]; top_score[i] = top_score[j]; top_score[j] = ts;
                    int tmp[64]; memcpy(tmp, top_sys[i], sizeof(int) * (size_t)K);
                    memcpy(top_sys[i], top_sys[j], sizeof(int) * (size_t)K);
                    memcpy(top_sys[j], tmp, sizeof(int) * (size_t)K);
                }

        printf("  System %2d (6+%d) — %ld kuponów × 3 PLN = %d PLN:\n", K, K - 6, ck6, cost_per);
        for (int p = 0; p < proposals; p++) {
            if (top_score[p] <= -1e29f) continue;
            printf("    propozycja %d:  ", p + 1);
            for (int i = 0; i < K; i++) printf("%2d ", top_sys[p][i]);
            printf("\n");
        }
        printf("\n");

        total_cost += cost_per * proposals;

        float sys_pct = (float)(K - 6) / (float)n_systems;
        gui_set_progress(0.35f + 0.60f * sys_pct);
        gui_set_status("Generowanie systemu %d...", K);
    }

    printf("────────────────────────────────────────────────────────────\n");
    printf("Propozycji na system: %d\n", proposals);
    printf("Łączny koszt (wszystkie propozycje): %d PLN\n\n", total_cost);

    // Wyświetl top-10 gorących numerów
    typedef struct { int n; float s; } NS;
    NS top[MAX_N];
    for (int i = 0; i < MAX_N; i++) top[i] = (NS){ i + 1, num_score[i + 1] };
    for (int i = 0; i < MAX_N; i++)
        for (int j = i + 1; j < MAX_N; j++)
            if (top[j].s > top[i].s) { NS tmp = top[i]; top[i] = top[j]; top[j] = tmp; }

    printf("Top-10 gorących numerów: ");
    for (int i = 0; i < 10; i++)
        printf("%d%s", top[i].n, i < 9 ? ", " : "\n");

    gui_set_progress(1.0f);
    gui_set_status("Gotowe");

    return 0;
}

// Implementacje komend Mini Lotto przeniesione do mini_lotto.c
