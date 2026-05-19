// main.c
// Punkt wejścia aplikacji. Obsługuje parsowanie argumentów CLI
// i przekierowuje do odpowiedniej komendy.

#include "lotto.h"

// Wyświetla instrukcję użycia programu
static void usage_main(const char *prog) {
    fprintf(stderr,
        "Lotto 6/49 System v2.0\n\n"
        "Usage:\n"
        "  %s                                                          (launch GUI)\n"
        "  %s fetch    --from YYYY-MM-DD --to YYYY-MM-DD [--sleep-ms N] [--max-req N] [--max-429 N]\n"
        "  %s fetch-mini --from YYYY-MM-DD --to YYYY-MM-DD [--sleep-ms N] [--max-req N] [--max-429 N]\n"
        "  %s optimize [--mode fast|full] [-k K] [--train N] [--seed N]\n"
        "  %s optimize-mini [--mode fast|full] [-k K] [--train N|--autotune] [--seed N]\n"
        "  %s backtest [-k K] [--train N|--autotune] [--step N] [--mc N] [--seed N]\n"
        "  %s backtest-mini [-k K] [--train N|--autotune] [--step N] [--mc N] [--seed N]\n"
        "  %s play     [--max-system N] [--proposals N] [--seed N]          (Lotto systems 7..N, default 10)\n"
        "  %s play     --wheel V/6/t                                  (Lotto: skrocony system gwarantowany)\n"
        "  %s play     --list-wheels                                  (Lotto: dostepne wheels)\n"
        "  %s play-mini [--max-system N] [--proposals N] [--train N|--autotune] [--seed N]\n"
        "  %s play-mini --wheel V/5/t                                 (Mini: skrocony system gwarantowany)\n"
        "  %s play-mini --list-wheels                                 (Mini: dostepne wheels)\n"
        "                                                              (Mini Lotto systems 5..N, default 9)\n\n"
        "API key is read from %s\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, KEY_FILE);
}

int main(int argc, char **argv) {
    srand(DEFAULT_RNG_SEED);

    if (!wheels_self_test()) {
        fprintf(stderr, "FATAL: wheels catalog self-test failed.\n");
        return 1;
    }

    if (argc < 2) {
        return cmd_gui(argc, argv);
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "fetch") == 0)
        return cmd_fetch(argc - 2, argv + 2);
    else if (strcmp(cmd, "fetch-mini") == 0)
        return cmd_fetch_mini(argc - 2, argv + 2);
    else if (strcmp(cmd, "optimize") == 0)
        return cmd_optimize(argc - 2, argv + 2);
    else if (strcmp(cmd, "optimize-mini") == 0)
        return cmd_optimize_mini(argc - 2, argv + 2);
    else if (strcmp(cmd, "backtest") == 0)
        return cmd_backtest(argc - 2, argv + 2);
    else if (strcmp(cmd, "backtest-mini") == 0)
        return cmd_backtest_mini(argc - 2, argv + 2);
    else if (strcmp(cmd, "play") == 0)
        return cmd_play(argc - 2, argv + 2);
    else if (strcmp(cmd, "play-mini") == 0)
        return cmd_play_mini(argc - 2, argv + 2);
    else if (strcmp(cmd, "gui") == 0)
        return cmd_gui(argc, argv);

    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    usage_main(argv[0]);
    return 2;
}
