// mini_lotto.c
// Moduł Mini Lotto 5/42: optymalizacja, backtest i generowanie systemów.
// Struktura analogiczna do Lotto 6/49, ale z innymi parametrami gry.

#include "lotto.h"

// Wynik automatycznego doboru okna treningowego
typedef struct {
    int best_train;
    float best_lift;
} AutotuneResult;

// Bezpieczny logarytm z dolnym ograniczeniem (zapobiega log(0))
static float safe_log(float x) {
    if (x < 1e-12f) x = 1e-12f;
    return logf(x);
}

static void print_set(const char *label, const int *S, int k) {
    printf("%s", label);
    for (int i = 0; i < k; i++)
        printf("%d%s", S[i], (i + 1 < k ? " " : "\n"));
}

// Log-kombinacja: ln(C(n,k)) — do obliczeń hipergeometrycznych
static double log_comb(int n, int k) {
    if (k < 0 || k > n) return -1e30;
    return lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0);
}

// PMF rozkładu hipergeometrycznego: P(X=k)
static double hypergeom_pmf(int k, int K, int N, int n) {
    return exp(log_comb(K, k) + log_comb(N - K, n - k) - log_comb(N, n));
}

// Generuje losowy podbiór K elementów z [1..max_n] (Fisher-Yates)
static void random_subset_generic(int *S, int K, int max_n) {
    int pool[64];
    for (int i = 0; i < max_n; i++) pool[i] = i + 1;
    for (int i = 0; i < K; i++) {
        int j = i + rand() % (max_n - i);
        int tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
        S[i] = pool[i];
    }
    qsort(S, (size_t)K, sizeof(int), cmp_int_asc);
}

// Oblicza C(n,k) dokładnie (dla małych wartości)
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

// Dane globalne Mini Lotto
static int mini_history[MAX_DRAWS][MINI_DRAW_SIZE];
static int mini_draws_total = 0;
static float mini_freq[MINI_MAX_N + 1];
static int mini_last_seen[MINI_MAX_N + 1];
static float mini_pair_count[MINI_MAX_N + 1][MINI_MAX_N + 1];
static int mini_window_len = 0;
static float mini_window_mass = 0.0f;
static float mini_decay_lambda = 0.03f;

// Wczytuje historię losowań Mini Lotto z pliku
static void load_mini_history(void) {
    FILE *f = fopen(MINI_HISTORY_FILE, "r");
    if (!f) { mini_draws_total = 0; return; }

    mini_draws_total = 0;
    while (mini_draws_total < MAX_DRAWS) {
        int a[MINI_DRAW_SIZE];
        if (fscanf(f, "%d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4]) != MINI_DRAW_SIZE) break;
        bool valid = true;
        for (int i = 0; i < MINI_DRAW_SIZE; i++) {
            if (a[i] < 1 || a[i] > MINI_MAX_N) {
                fprintf(stderr, "Invalid value in mini history line %d.\n", mini_draws_total + 1);
                valid = false;
                break;
            }
        }
        if (!valid) { fclose(f); exit(1); }
        qsort(a, MINI_DRAW_SIZE, sizeof(int), cmp_int_asc);
        memcpy(mini_history[mini_draws_total], a, sizeof(a));
        mini_draws_total++;
    }
    fclose(f);
}

// Buduje statystyki (częstotliwość, ostatnie wystąpienie, pary) na oknie [start, end)
static void build_mini_stats_window(int start, int end) {
    memset(mini_freq, 0, sizeof(mini_freq));
    memset(mini_pair_count, 0, sizeof(mini_pair_count));
    for (int i = 0; i <= MINI_MAX_N; i++) mini_last_seen[i] = -1;

    if (start < 0) start = 0;
    if (end > mini_draws_total) end = mini_draws_total;
    if (end <= start) { mini_window_len = 0; mini_window_mass = 0.0f; return; }

    mini_window_len = end - start;
    mini_window_mass = 0.0f;
    for (int di = start; di < end; di++) {
        int rel = di - start;
        int age = end - 1 - di;
        float w = expf(-mini_decay_lambda * (float)age);
        mini_window_mass += w;
        const int *d = mini_history[di];
        for (int i = 0; i < MINI_DRAW_SIZE; i++) {
            int v = d[i];
            if (v < 1 || v > MINI_MAX_N) continue;
            mini_freq[v] += w;
            mini_last_seen[v] = rel;
        }
        for (int i = 0; i < MINI_DRAW_SIZE; i++) {
            for (int j = i + 1; j < MINI_DRAW_SIZE; j++) {
                int x = d[i], y = d[j];
                if (x > y) { int t = x; x = y; y = t; }
                if (x >= 1 && y <= MINI_MAX_N) mini_pair_count[x][y] += w;
            }
        }
    }
}

// Eksponencjalny zanik świeżości: e^(-λ * gap)
static float mini_exp_recency(int n) {
    if (mini_window_len <= 0) return 0.0f;
    if (mini_last_seen[n] < 0) return 0.0f;
    int gap = mini_window_len - 1 - mini_last_seen[n];
    return expf(-mini_decay_lambda * (float)gap);
}

static float mini_norm_denominator(void) {
    return (mini_window_mass > 1e-9f) ? mini_window_mass : (float)mini_window_len;
}

// Gap analysis: bonus za zaległe numery w Mini Lotto
static float mini_gap_bonus(const int *S, int k) {
    if (mini_window_len <= 0) return 0.0f;
    float den = mini_norm_denominator();
    float score = 0.0f;
    for (int i = 0; i < k; i++) {
        int n = S[i];
        int gap = (mini_last_seen[n] >= 0) ? (mini_window_len - 1 - mini_last_seen[n]) : mini_window_len;
        float expected_gap = (mini_freq[n] > 1e-9f)
            ? den / mini_freq[n]
            : (float)mini_window_len;
        float ratio = (float)gap / expected_gap;
        float centered = ratio - 1.0f;
        if (centered > 2.0f) centered = 2.0f;
        if (centered < -1.0f) centered = -1.0f;
        score += centered;
    }
    return score / (float)k;
}

// PMI (Pointwise Mutual Information) dla pary liczb w Mini Lotto
static float mini_pair_pmi(int x, int y) {
    if (x > y) { int t = x; x = y; y = t; }
    if (mini_window_len <= 0 || x < 1 || y > MINI_MAX_N) return 0.0f;

    float den = mini_norm_denominator();

    float px = mini_freq[x] / den;
    float py = mini_freq[y] / den;
    float pxy = mini_pair_count[x][y] / den;

    if (px <= 0.0f || py <= 0.0f || pxy <= 0.0f) return 0.0f;
    return safe_log(pxy / (px * py));
}

// Wynik (score) systemu Mini Lotto: częstotliwość + pary + gap bonus - kary za klastry
static float mini_system_score(const int *S, int k, const float *num_score) {
    float score = 0.0f;
    int T[16];
    memcpy(T, S, sizeof(int) * (size_t)k);
    qsort(T, (size_t)k, sizeof(int), cmp_int_asc);

    for (int i = 0; i < k; i++) score += num_score[T[i]];

    float pair_bonus = 0.0f;
    int pair_cnt = 0;
    for (int i = 0; i < k; i++) {
        for (int j = i + 1; j < k; j++) {
            pair_bonus += mini_pair_pmi(T[i], T[j]);
            pair_cnt++;
        }
    }
    if (pair_cnt > 0) pair_bonus /= (float)pair_cnt;

    int seq_steps = 0;
    for (int i = 1; i < k; i++) if (T[i] == T[i - 1] + 1) seq_steps++;
    float spread_pen = 0.0f;
    for (int i = 1; i < k; i++) {
        int gap = T[i] - T[i - 1];
        if (gap < 2) spread_pen += 0.7f;
    }
    spread_pen += 0.25f * (float)seq_steps;

    float gb = mini_gap_bonus(T, k);

    return score + 0.8f * pair_bonus + 0.3f * gb - spread_pen;
}

// Buduje wynik (score) każdego numeru na podstawie częstotliwości i świeżości
static void mini_build_num_score(float *num_score) {
    for (int n = 1; n <= MINI_MAX_N; n++) {
        float f = (mini_window_len > 0) ? (mini_freq[n] / mini_norm_denominator()) : 0.0f;
        float rec = mini_exp_recency(n);
        num_score[n] = 0.65f * f + 0.35f * rec;
    }
}

// Wyświetla wszystkie kombinacje 5-elementowe z systemu K-elementowego
static void print_combinations_5_of_k(const int *S, int k) {
    for (int i0 = 0; i0 < k; i0++)
    for (int i1 = i0 + 1; i1 < k; i1++)
    for (int i2 = i1 + 1; i2 < k; i2++)
    for (int i3 = i2 + 1; i3 < k; i3++)
    for (int i4 = i3 + 1; i4 < k; i4++)
        printf("%d %d %d %d %d\n", S[i0], S[i1], S[i2], S[i3], S[i4]);
}

static void report_mini_metrics(const int *S, int k, const float *num_score) {
    float sc = mini_system_score(S, k, num_score);
    printf("\n[MINI METRICS]\n");
    printf("score        = %.6f\n", sc);
    printf("sum          = %d\n", set_sum(S, k));

    int even = 0;
    for (int i = 0; i < k; i++) if ((S[i] % 2) == 0) even++;
    printf("evens        = %d/%d\n", even, k);
}

// Użyteczność (utility) na podstawie liczby trafień Mini Lotto
static float mini_utility_from_hits(int hits) {
    switch (hits) {
        case 5: return 500.0f;
        case 4: return 25.0f;
        case 3: return 2.0f;
        default: return 0.0f;
    }
}

// Generuje optymalny system K-elementowy dla bieżącego okna statystyk
static void mini_pick_system_for_window(int K, int *S) {
    float num_score[MINI_MAX_N + 1] = {0.0f};
    for (int n = 1; n <= MINI_MAX_N; n++) {
        float f = (mini_window_len > 0) ? (mini_freq[n] / mini_norm_denominator()) : 0.0f;
        float rec = mini_exp_recency(n);
        num_score[n] = 0.65f * f + 0.35f * rec;
    }

    typedef struct { int n; float s; } NS;
    NS cand[MINI_MAX_N];
    for (int i = 0; i < MINI_MAX_N; i++) cand[i] = (NS){ i + 1, num_score[i + 1] };
    for (int i = 0; i < MINI_MAX_N; i++) {
        for (int j = i + 1; j < MINI_MAX_N; j++) {
            if (cand[j].s > cand[i].s) {
                NS t = cand[i]; cand[i] = cand[j]; cand[j] = t;
            }
        }
    }
    for (int i = 0; i < K; i++) S[i] = cand[i].n;
    qsort(S, (size_t)K, sizeof(int), cmp_int_asc);

    float best = mini_system_score(S, K, num_score);
    int iters = 2000;
    for (int it = 0; it < iters; it++) {
        int idx = rand() % K;
        int old = S[idx];
        int repl = 1 + rand() % MINI_MAX_N;
        if (repl == old || set_contains(S, K, repl)) continue;

        int trial[16];
        memcpy(trial, S, sizeof(int) * (size_t)K);
        trial[idx] = repl;
        qsort(trial, (size_t)K, sizeof(int), cmp_int_asc);
        float sc = mini_system_score(trial, K, num_score);
        if (sc > best) {
            best = sc;
            memcpy(S, trial, sizeof(int) * (size_t)K);
        }
    }
}

// Automatyczny dobór okna treningowego — skanuje różne rozmiary okien
// i wybiera to, które daje najwyższy lift względem wartości losowej
static AutotuneResult autotune_mini_train_window(int K, float progress_base, float progress_span) {
    // Wartość domyślna: połowa dostępnej historii
    int fallback = (mini_draws_total > 1) ? mini_draws_total / 2 : MINI_DRAW_SIZE;
    if (fallback < MINI_DRAW_SIZE) fallback = MINI_DRAW_SIZE;
    AutotuneResult res = { fallback, -1.0f };
    // Uwzględnij małe okna, aby i rzadkie historie były przeskanowane
    int train_vals[] = { 10, 15, 20, 30, 40, 60, 80, 120, 160, 220, 300, 400 };
    int n_tr = (int)(sizeof(train_vals) / sizeof(train_vals[0]));

    fprintf(stderr, "Autotune: skanowanie okien treningowych (MINI)...\n");
    for (int ti = 0; ti < n_tr; ti++) {
        int tw = train_vals[ti];
        if (tw >= mini_draws_total) continue;

        int hits3 = 0, tested = 0;
        for (int t = tw; t < mini_draws_total; t += 5) {
            int S[16];
            build_mini_stats_window(t - tw, t);
            mini_pick_system_for_window(K, S);
            if (count_hits_generic(mini_history[t], MINI_DRAW_SIZE, S, K) >= 3) hits3++;
            tested++;
        }

        float rate = (tested > 0) ? (float)hits3 / (float)tested : 0.0f;
        double hg3 = 0.0;
        for (int h = 3; h <= MINI_DRAW_SIZE; h++) hg3 += hypergeom_pmf(h, K, MINI_MAX_N, MINI_DRAW_SIZE);
        float lift = (hg3 > 0.0) ? (float)(rate / hg3) : 0.0f;

        gui_set_progress(progress_base + progress_span * (float)(ti + 1) / (float)n_tr);
        gui_set_status("Autotune: okno %d, lift=%.2fx (MINI)", tw, lift);
        fprintf(stderr, "  train=%d: 3+rate=%.3f%% lift=%.2fx (MINI)\n", tw, 100.0f * rate, lift);

        if (lift > res.best_lift) { res.best_lift = lift; res.best_train = tw; }
    }

    fprintf(stderr, "Autotune: najlepsze okno = %d (lift=%.2fx, MINI)\n\n", res.best_train, res.best_lift);
    return res;
}

int cmd_backtest_mini(int argc, char **argv) {
    int K = 7;
    int train_win = -1;
    int step = 1;
    int mc_sims = MC_SIMS;
    bool autotune = true;
    unsigned int seed = DEFAULT_RNG_SEED;

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
        else if (strcmp(argv[i], "--autotune") == 0) autotune = true;
        else if (strcmp(argv[i], "--no-autotune") == 0) autotune = false;
    }

    srand(seed);
    fprintf(stderr, "RNG seed: %u\n", seed);

    if (K < MINI_DRAW_SIZE || K > 12) { fprintf(stderr, "K must be %d..12\n", MINI_DRAW_SIZE); return 2; }
    if (step < 1) step = 1;
    if (mc_sims < 100) mc_sims = 100;
    if (mc_sims > 50000) mc_sims = 50000;

    gui_set_status("Ladowanie historii Mini Lotto...");
    load_mini_history();
    gui_set_status("Pobieranie brakujacych losowan Mini Lotto...");
    if (catchup_fetch_draws_for_game(GAME_TYPE_MINI, MINI_HISTORY_FILE, MINI_DRAW_SIZE, MINI_MAX_N) < 0) {
        fprintf(stderr, "Nie udalo sie pobrac brakujacych losowan Mini Lotto.\n");
        return 1;
    }
    load_mini_history();

    if (mini_draws_total < MINI_DRAW_SIZE + 1) {
        fprintf(stderr, "Za malo losowan w historii Mini Lotto: %d (potrzeba min. %d)\n",
                mini_draws_total, MINI_DRAW_SIZE + 1);
        return 1;
    }
    if (mini_draws_total < 30)
        fprintf(stderr, "Uwaga: mala historia (%d losowan) — wyniki backtestow moga byc nieistotne statystycznie.\n",
                mini_draws_total);

    if (autotune) {
        gui_set_status("Autotune okna treningowego (MINI)...");
        AutotuneResult at = autotune_mini_train_window(K, 0.0f, 0.15f);
        train_win = at.best_train;
    } else if (train_win < 0) {
        train_win = 220;
    }

    // Ograniczenie: okno treningowe musi być < całkowita historia i >= MINI_DRAW_SIZE
    if (train_win >= mini_draws_total) train_win = mini_draws_total - 1;
    if (train_win < MINI_DRAW_SIZE)    train_win = MINI_DRAW_SIZE;
    if (train_win >= mini_draws_total) train_win = mini_draws_total - 1;

    int start_t = train_win;
    int end_t = mini_draws_total;
    int total_test_est = (end_t - start_t + step - 1) / step;

    int hit_counts[MINI_DRAW_SIZE + 1] = {0};
    int test_draws = 0;
    float total_utility = 0.0f;
    int max_drought = 0, current_drought = 0;

    fprintf(stderr, "=== MINI BACKTEST START ===\n");
    fprintf(stderr, "History: %d draws, training window: %d%s, test: ~%d draws (step=%d)\n",
            mini_draws_total, train_win, autotune ? " (autotune)" : "", total_test_est, step);
    fprintf(stderr, "System K=%d -> C(%d,%d)=%ld tickets per draw\n", K, K, MINI_DRAW_SIZE, comb_nk(K, MINI_DRAW_SIZE));
    fprintf(stderr, "Monte Carlo simulations: %d\n\n", mc_sims);

    gui_set_status("Faza 1/2: Backtest MINI...");
    for (int t = start_t; t < end_t; t += step) {
        int S[16];
        build_mini_stats_window(t - train_win, t);
        mini_pick_system_for_window(K, S);

        int hits = count_hits_generic(mini_history[t], MINI_DRAW_SIZE, S, K);
        int h_idx = hits > MINI_DRAW_SIZE ? MINI_DRAW_SIZE : hits;
        hit_counts[h_idx]++;
        total_utility += mini_utility_from_hits(hits);
        test_draws++;

        if (hits >= 3) current_drought = 0;
        else {
            current_drought++;
            if (current_drought > max_drought) max_drought = current_drought;
        }

        float pct = (float)test_draws / (float)total_test_est;
        gui_set_progress(0.15f + 0.45f * pct);
        if ((test_draws % 50) == 0) {
            int h3p = hit_counts[3] + hit_counts[4] + hit_counts[5];
            gui_set_status("Faza 1/2: MINI %d/%d (3+: %d, %.2f%%)",
                           test_draws, total_test_est, h3p,
                           100.0 * (double)h3p / (double)test_draws);
        }
    }

    int algo_3plus = hit_counts[3] + hit_counts[4] + hit_counts[5];
    int *test_indices = (int *)malloc((size_t)test_draws * sizeof(int));
    if (!test_indices) {
        fprintf(stderr, "OOM: cannot allocate test indices (MINI).\n");
        return 1;
    }
    int idx = 0;
    for (int t = start_t; t < end_t && idx < test_draws; t += step) test_indices[idx++] = t;

    int mc_better_utility = 0;
    int mc_better_3plus = 0;
    double mc_avg_hits[MINI_DRAW_SIZE + 1] = {0};
    double mc_avg_utility = 0.0;

    gui_set_status("Faza 2/2: Monte Carlo MINI (%d symulacji)...", mc_sims);
    for (int sim = 0; sim < mc_sims; sim++) {
        int mc_hits_total[MINI_DRAW_SIZE + 1] = {0};
        float mc_util = 0.0f;

        for (int di = 0; di < test_draws; di++) {
            int R[16];
            random_subset_generic(R, K, MINI_MAX_N);
            int hits = count_hits_generic(mini_history[test_indices[di]], MINI_DRAW_SIZE, R, K);
            int h_idx = hits > MINI_DRAW_SIZE ? MINI_DRAW_SIZE : hits;
            mc_hits_total[h_idx]++;
            mc_util += mini_utility_from_hits(hits);
        }

        int mc_3p = mc_hits_total[3] + mc_hits_total[4] + mc_hits_total[5];
        if (mc_util >= total_utility) mc_better_utility++;
        if (mc_3p >= algo_3plus) mc_better_3plus++;

        for (int h = 0; h <= MINI_DRAW_SIZE; h++) mc_avg_hits[h] += (double)mc_hits_total[h];
        mc_avg_utility += (double)mc_util;

        if ((sim + 1) % 20 == 0 || sim == mc_sims - 1) {
            float pct = (float)(sim + 1) / (float)mc_sims;
            gui_set_progress(0.60f + 0.40f * pct);
            gui_set_status("Faza 2/2: Monte Carlo MINI %d/%d", sim + 1, mc_sims);
        }
    }
    free(test_indices);

    for (int h = 0; h <= MINI_DRAW_SIZE; h++) mc_avg_hits[h] /= (double)mc_sims;
    mc_avg_utility /= (double)mc_sims;

    double p_value_utility = (double)(mc_better_utility + 1) / (double)(mc_sims + 1);
    double p_value_3plus = (double)(mc_better_3plus + 1) / (double)(mc_sims + 1);

    double hg_pmf[MINI_DRAW_SIZE + 1];
    for (int h = 0; h <= MINI_DRAW_SIZE; h++) hg_pmf[h] = hypergeom_pmf(h, K, MINI_MAX_N, MINI_DRAW_SIZE);

    double hg_3plus = 0.0;
    for (int h = 3; h <= MINI_DRAW_SIZE; h++) hg_3plus += hg_pmf[h];

    double actual_3plus = (double)algo_3plus / (double)test_draws;
    double mc_rate_3plus = (mc_avg_hits[3] + mc_avg_hits[4] + mc_avg_hits[5]) / (double)test_draws;
    double avg_utility = (double)total_utility / (double)test_draws;
    double mc_avg_util = mc_avg_utility / (double)test_draws;

    printf("=== MINI LOTTO BACKTEST (5/42) ===\n");
    printf("History: %d, train: %d%s, test: %d, K=%d, MC=%d\n",
           mini_draws_total, train_win, autotune ? " (autotune)" : "", test_draws, K, mc_sims);
    printf("Tickets/draw: C(%d,5)=%ld\n\n", K, comb_nk(K, MINI_DRAW_SIZE));

    printf("Hit distribution (algo vs MC avg vs theory):\n");
    for (int h = 0; h <= MINI_DRAW_SIZE; h++) {
        double algo_pct = 100.0 * (double)hit_counts[h] / (double)test_draws;
        double mc_pct = 100.0 * mc_avg_hits[h] / (double)test_draws;
        double th_pct = 100.0 * hg_pmf[h];
        printf("  hits=%d: algo=%7.3f%%  mc=%7.3f%%  theory=%7.3f%%\n", h, algo_pct, mc_pct, th_pct);
    }

    printf("\n3+ hit rate: algo=%7.4f%%  mc=%7.4f%%  theory=%7.4f%%  lift_vs_mc=%.2fx\n",
           100.0 * actual_3plus, 100.0 * mc_rate_3plus, 100.0 * hg_3plus,
           (mc_rate_3plus > 0.0) ? (actual_3plus / mc_rate_3plus) : 0.0);
    printf("Avg utility: algo=%7.4f  mc=%7.4f  lift=%.2fx\n",
           avg_utility, mc_avg_util, (mc_avg_util > 0.0) ? (avg_utility / mc_avg_util) : 0.0);
    printf("Max drought (>=3): %d\n", max_drought);
    printf("p-value utility=%.4f, p-value 3+=%.4f\n", p_value_utility, p_value_3plus);

    gui_set_progress(1.0f);
    gui_set_status("Gotowe (MINI backtest)");
    return 0;
}

int cmd_optimize_mini(int argc, char **argv) {
    int K = 7;
    int train_win = -1;
    bool autotune = true;
    bool full_mode = false;
    unsigned int seed = DEFAULT_RNG_SEED;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) K = atoi(argv[++i]);
        else if (strcmp(argv[i], "--train") == 0 && i + 1 < argc) {
            train_win = atoi(argv[++i]);
            autotune = false;
        }
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_seed_arg(argv[++i], &seed)) {
                fprintf(stderr, "Invalid --seed value. Use unsigned integer.\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--autotune") == 0) autotune = true;
        else if (strcmp(argv[i], "--no-autotune") == 0) autotune = false;
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "full") == 0) full_mode = true;
            else if (strcmp(argv[i], "fast") == 0) full_mode = false;
            else { fprintf(stderr, "Unknown mode: %s (use 'fast' or 'full')\n", argv[i]); return 2; }
        }
    }

    srand(seed);
    fprintf(stderr, "RNG seed: %u\n", seed);

    if (K < MINI_DRAW_SIZE || K > 12) { fprintf(stderr, "K must be %d..12\n", MINI_DRAW_SIZE); return 2; }

    gui_set_status("Ladowanie historii Mini Lotto...");
    load_mini_history();
    gui_set_status("Pobieranie brakujacych losowan Mini Lotto...");
    if (catchup_fetch_draws_for_game(GAME_TYPE_MINI, MINI_HISTORY_FILE, MINI_DRAW_SIZE, MINI_MAX_N) < 0) {
        fprintf(stderr, "Nie udalo sie pobrac brakujacych losowan Mini Lotto.\n");
        return 1;
    }
    load_mini_history();

    if (mini_draws_total < MINI_DRAW_SIZE) {
        fprintf(stderr, "Mini Lotto history too small: %d draws.\n", mini_draws_total);
        return 1;
    }
    if (mini_draws_total < 30)
        fprintf(stderr, "Uwaga: mala historia (%d losowan) — optymalizacja moze byc mniej trafna.\n",
                mini_draws_total);

    if (autotune) {
        gui_set_status("Autotune okna treningowego (MINI)...");
        AutotuneResult at = autotune_mini_train_window(K, 0.0f, 0.20f);
        train_win = at.best_train;
    } else if (train_win < 0) {
        train_win = 220;
    }

    // Ograniczenie: okno treningowe musi być < całkowita historia i >= MINI_DRAW_SIZE
    if (train_win >= mini_draws_total) train_win = mini_draws_total - 1;
    if (train_win < MINI_DRAW_SIZE)    train_win = MINI_DRAW_SIZE;
    if (train_win >= mini_draws_total) train_win = mini_draws_total - 1;

    build_mini_stats_window(mini_draws_total - train_win, mini_draws_total);

    int restarts = full_mode ? 8 : 3;
    int best_set[16] = {0};
    float best_score = -1e30f;
    float num_score[MINI_MAX_N + 1] = {0.0f};
    mini_build_num_score(num_score);

    gui_set_status("Optymalizacja MINI...");
    for (int r = 0; r < restarts; r++) {
        int cand[16] = {0};
        mini_pick_system_for_window(K, cand);
        float sc = mini_system_score(cand, K, num_score);
        if (sc > best_score) {
            best_score = sc;
            memcpy(best_set, cand, sizeof(int) * (size_t)K);
        }
        gui_set_progress((float)(r + 1) / (float)restarts);
    }

    printf("MINI optimize (%s), train=%d, draws=%d\n", full_mode ? "full" : "fast", train_win, mini_draws_total);
    print_set("Best set: ", best_set, K);
    report_mini_metrics(best_set, K, num_score);

    printf("\n[COMBINATIONS 5-of-%d]\n", K);
    print_combinations_5_of_k(best_set, K);

    gui_set_progress(1.0f);
    gui_set_status("Gotowe (MINI optimize)");
    return 0;
}

int cmd_play_mini(int argc, char **argv) {
    int max_system = 9;
    int proposals = 1;
    int train_win = -1;
    bool autotune = true;
    unsigned int seed = DEFAULT_RNG_SEED;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--max-system") == 0 && i + 1 < argc)
            max_system = atoi(argv[++i]);
        else if (strcmp(argv[i], "--proposals") == 0 && i + 1 < argc)
            proposals = atoi(argv[++i]);
        else if (strcmp(argv[i], "--train") == 0 && i + 1 < argc) {
            train_win = atoi(argv[++i]);
            autotune = false;
        }
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_seed_arg(argv[++i], &seed)) {
                fprintf(stderr, "Invalid --seed value. Use unsigned integer.\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--autotune") == 0) autotune = true;
        else if (strcmp(argv[i], "--no-autotune") == 0) autotune = false;
    }
    if (proposals < 1) proposals = 1;
    if (proposals > 10) proposals = 10;

    srand(seed);
    fprintf(stderr, "RNG seed: %u\n", seed);

    if (max_system < MINI_DRAW_SIZE) max_system = MINI_DRAW_SIZE;
    if (max_system > 12) max_system = 12;

    gui_set_status("Ladowanie historii Mini Lotto...");
    load_mini_history();

    gui_set_status("Pobieranie brakujacych losowan Mini Lotto...");
    if (catchup_fetch_draws_for_game(GAME_TYPE_MINI, MINI_HISTORY_FILE, MINI_DRAW_SIZE, MINI_MAX_N) < 0) {
        fprintf(stderr, "Nie udalo sie pobrac brakujacych losowan Mini Lotto.\n");
        return 1;
    }

    load_mini_history();
    if (mini_draws_total < MINI_DRAW_SIZE) {
        fprintf(stderr, "Za malo losowan w historii Mini Lotto: %d (min. %d)\n",
                mini_draws_total, MINI_DRAW_SIZE);
        return 1;
    }
    if (mini_draws_total < 30)
        fprintf(stderr, "Uwaga: mala historia (%d losowan) — systemy oparte na ograniczonych danych.\n",
                mini_draws_total);

    if (autotune) {
        gui_set_status("Autotune okna treningowego (MINI)...");
        AutotuneResult at = autotune_mini_train_window(max_system, 0.0f, 0.20f);
        train_win = at.best_train;
    } else if (train_win < 0) {
        train_win = (mini_draws_total < 350) ? mini_draws_total : 350;
    }
    if (train_win >= mini_draws_total) train_win = mini_draws_total - 1;
    if (train_win < MINI_DRAW_SIZE)    train_win = MINI_DRAW_SIZE;
    if (train_win >= mini_draws_total) train_win = mini_draws_total - 1;

    build_mini_stats_window(mini_draws_total - train_win, mini_draws_total);

    float num_score[MINI_MAX_N + 1] = {0.0f};
    for (int n = 1; n <= MINI_MAX_N; n++) {
        float f = (mini_window_len > 0) ? (float)mini_freq[n] / (float)mini_window_len : 0.0f;
        float rec = mini_exp_recency(n);
        num_score[n] = 0.65f * f + 0.35f * rec;
    }

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║         MINI LOTTO (5/42) — SYSTEMY DO GRY                 ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");
    printf("Analiza: %d losowan, okno treningowe: %d\n\n", mini_draws_total, train_win);

    int total_cost = 0;
    int steps = max_system - MINI_DRAW_SIZE + 1;
    for (int K = MINI_DRAW_SIZE; K <= max_system; K++) {
        int top_sys[10][16];
        float top_score[10];
        for (int p = 0; p < proposals; p++) top_score[p] = -1e30f;

        int n_cand = 5000 * proposals;
        float bsz = (float)MINI_MAX_N / (float)K;

        for (int c = 0; c < n_cand; c++) {
            int T[16] = {0};
            for (int b = 0; b < K; b++) {
                int lo = (int)(b * bsz) + 1;
                int hi = (int)((b + 1) * bsz);
                if (b == K - 1) hi = MINI_MAX_N;
                if (hi < lo) hi = lo;

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
            for (int i = 1; i < K; i++) {
                if (T[i] == T[i - 1]) { unique = false; break; }
            }
            if (!unique) continue;

            float sc = mini_system_score(T, K, num_score);

            int worst = 0;
            for (int p = 1; p < proposals; p++)
                if (top_score[p] < top_score[worst]) worst = p;

            if (sc > top_score[worst]) {
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
                    int tmp[16]; memcpy(tmp, top_sys[i], sizeof(int) * (size_t)K);
                    memcpy(top_sys[i], top_sys[j], sizeof(int) * (size_t)K);
                    memcpy(top_sys[j], tmp, sizeof(int) * (size_t)K);
                }

        long ck = comb_nk(K, MINI_DRAW_SIZE);
        int cost_per = (int)ck * 2;

        printf("  System %2d (5+%d) — %ld kuponow x 2 PLN = %d PLN:\n", K, K - MINI_DRAW_SIZE, ck, cost_per);
        for (int p = 0; p < proposals; p++) {
            if (top_score[p] <= -1e29f) continue;
            printf("    propozycja %d:  ", p + 1);
            for (int i = 0; i < K; i++) printf("%2d ", top_sys[p][i]);
            printf("\n");
        }
        printf("\n");

        total_cost += cost_per * proposals;
        float pct = (float)(K - MINI_DRAW_SIZE + 1) / (float)steps;
        gui_set_progress(0.20f + 0.80f * pct);
        gui_set_status("Generowanie Mini Lotto system %d...", K);
    }

    printf("────────────────────────────────────────────────────────────\n");
    printf("Propozycji na system: %d\n", proposals);
    printf("Laczny koszt (wszystkie propozycje): %d PLN\n", total_cost);

    gui_set_progress(1.0f);
    gui_set_status("Gotowe");
    return 0;
}
