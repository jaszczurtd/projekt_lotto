// gui.c
// Interfejs graficzny GTK3: zakładki dla każdego trybu pracy,
// pasek postępu, panel wyjściowy, obsługa wątków roboczych.
// Gdy brak GTK3 (brak flagi HAS_GTK), udostępniane są implementacje
// zastępcze umożliwiające działanie programu wyłącznie w trybie CLI.

#include "lotto.h"

#ifdef HAS_GTK

#include <gtk/gtk.h>
#include <unistd.h>
#include <fcntl.h>

// Stan postępu współdzielony między wątkiem roboczym a GUI (chroniony mutexem)
static float g_progress = 0.0f;
static char g_status_text[256] = "";
static pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void gui_set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_status_mutex);
    vsnprintf(g_status_text, sizeof(g_status_text), fmt, ap);
    pthread_mutex_unlock(&g_status_mutex);
    va_end(ap);
}

void gui_set_progress(float fraction) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    pthread_mutex_lock(&g_status_mutex);
    g_progress = fraction;
    pthread_mutex_unlock(&g_status_mutex);
}

// Widgety GUI (globalne dla callbacków)
static GtkWidget *g_window;
static GtkWidget *g_notebook;
static GtkWidget *g_textview;
static GtkTextBuffer *g_textbuf;
static GtkWidget *g_run_btn;
static GtkWidget *g_spinner;
static GtkWidget *g_progress_bar;
static GtkWidget *g_status_label;
static GtkWidget *g_elapsed_label;

// Stan timera GUI
static GTimer *g_elapsed_timer = NULL;
static guint g_timer_id = 0;

// Widgety zakładki Fetch (Lotto)
static GtkWidget *g_fetch_from, *g_fetch_to;
static GtkWidget *g_fetch_sleep, *g_fetch_maxreq, *g_fetch_max429;
// Widgety zakładki Fetch (Mini Lotto)
static GtkWidget *g_fetch_mini_from, *g_fetch_mini_to;
static GtkWidget *g_fetch_mini_sleep, *g_fetch_mini_maxreq, *g_fetch_mini_max429;

// Widgety zakładki Play (Lotto)
static GtkWidget *g_play_max_system;
static GtkWidget *g_play_proposals;
// Widgety zakładki Play (Mini Lotto)
static GtkWidget *g_play_mini_max_system;
static GtkWidget *g_play_mini_proposals;

// Widgety zakładki Wheel (Lotto): pula V liczb i gwarancja t-z-6
static GtkWidget *g_wheel_v, *g_wheel_t;
// Widgety zakładki Wheel (Mini Lotto): pula V liczb i gwarancja t-z-5
static GtkWidget *g_wheel_mini_v, *g_wheel_mini_t;

// Stan wątku roboczego
static pthread_t g_worker_thread;
static volatile bool g_running = false;
static int g_pipe_fd[2];               // pipe do przechwytywania stdout/stderr
static volatile int g_last_exit_code = 0;

// Dane do asynchronicznego dodawania tekstu w GUI
typedef struct { char *text; } AppendData;

// Argumenty przekazywane do wątku roboczego
typedef struct {
    int mode; // 0=fetch-lotto, 1=fetch-mini, 2=play-lotto, 3=play-mini,
              // 4=wheel-lotto, 5=wheel-mini
    char from[32], to[32];
    int sleep_ms, max_req, max_429;
    int play_max_system;
    int play_proposals;
    int play_mini_max_system;
    int play_mini_proposals;
    int wheel_v, wheel_t;
    int wheel_mini_v, wheel_mini_t;
} WorkerArgs;

// Dodaje tekst do panelu Output z wątku roboczego (idle callback GTK)
static gboolean gui_append_text_idle(gpointer data) {
    AppendData *ad = (AppendData *)data;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(g_textbuf, &end);
    gtk_text_buffer_insert(g_textbuf, &end, ad->text, -1);

    gtk_text_buffer_get_end_iter(g_textbuf, &end);
    GtkTextMark *mark = gtk_text_buffer_get_mark(g_textbuf, "end_mark");
    if (!mark)
        mark = gtk_text_buffer_create_mark(g_textbuf, "end_mark", &end, FALSE);
    else
        gtk_text_buffer_move_mark(g_textbuf, mark, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(g_textview), mark);

    free(ad->text);
    free(ad);
    return G_SOURCE_REMOVE;
}

// Bezpieczne dodanie tekstu z dowolnego wątku (przez g_idle_add)
static void gui_append(const char *text) {
    AppendData *ad = (AppendData *)malloc(sizeof(AppendData));
    if (!ad) return;
    ad->text = strdup(text);
    if (!ad->text) {
        free(ad);
        return;
    }
    g_idle_add(gui_append_text_idle, ad);
}

// Timer: aktualizuje pasek postępu, status i czas co 100ms
static gboolean gui_timer_tick(gpointer data) {
    (void)data;
    if (!g_running) return G_SOURCE_REMOVE;

    pthread_mutex_lock(&g_status_mutex);
    float cur_progress = g_progress;
    char cur_status[256];
    memcpy(cur_status, g_status_text, sizeof(cur_status));
    pthread_mutex_unlock(&g_status_mutex);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress_bar), (double)cur_progress);
    char pct[32];
    snprintf(pct, sizeof(pct), "%.0f%%", (double)cur_progress * 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_progress_bar), pct);

    gtk_label_set_text(GTK_LABEL(g_status_label), cur_status);

    if (g_elapsed_timer) {
        double elapsed = g_timer_elapsed(g_elapsed_timer, NULL);
        int mins = (int)elapsed / 60;
        int secs = (int)elapsed % 60;
        char buf[64];
        snprintf(buf, sizeof(buf), "Elapsed: %d:%02d", mins, secs);
        gtk_label_set_text(GTK_LABEL(g_elapsed_label), buf);
    }

    return G_SOURCE_CONTINUE;
}

// Wywoływane gdy wątek roboczy zakończy pracę — przywraca stan GUI
static gboolean gui_done_idle(gpointer data) {
    (void)data;
    g_running = false;
    gtk_widget_set_sensitive(g_run_btn, TRUE);
    gtk_spinner_stop(GTK_SPINNER(g_spinner));

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress_bar), 1.0);
    if (g_last_exit_code == 0) {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_progress_bar), "100%");
    } else {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_progress_bar), "ERROR");
    }

    if (g_elapsed_timer) {
        double elapsed = g_timer_elapsed(g_elapsed_timer, NULL);
        int mins = (int)elapsed / 60;
        int secs = (int)elapsed % 60;
        char buf[128];
        if (g_last_exit_code == 0) {
            snprintf(buf, sizeof(buf), "Done (total: %d:%02d)", mins, secs);
        } else {
            snprintf(buf, sizeof(buf), "Blad (exit code: %d, czas: %d:%02d)", g_last_exit_code, mins, secs);
        }
        gtk_label_set_text(GTK_LABEL(g_status_label), buf);
        g_timer_destroy(g_elapsed_timer);
        g_elapsed_timer = NULL;
    }

    if (g_last_exit_code != 0) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(g_window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Operacja zakonczona bledem (exit code: %d)",
            g_last_exit_code);
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dlg),
            "Szczegoly bledu znajdziesz w panelu Output.");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }

    if (g_timer_id) {
        g_source_remove(g_timer_id);
        g_timer_id = 0;
    }
    return G_SOURCE_REMOVE;
}

// Wątek czytający: odczytuje dane z pipe i przekazuje do panelu Output
static void *pipe_reader_func(void *arg) {
    (void)arg;
    char buf[4096];
    ssize_t n;
    while ((n = read(g_pipe_fd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        gui_append(buf);
    }
    return NULL;
}

// Główny wątek roboczy: przekierowuje stdout/stderr przez pipe,
// uruchamia wybraną komendę, po zakończeniu przywraca deskryptory
static void *worker_func(void *arg) {
    WorkerArgs *wa = (WorkerArgs *)arg;

    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    dup2(g_pipe_fd[1], STDOUT_FILENO);
    dup2(g_pipe_fd[1], STDERR_FILENO);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    g_last_exit_code = 0;
    gui_set_progress(0.0f);
    gui_set_status("Starting...");

    int rc = 0;
    if (wa->mode == 0) {
        char sleep_str[32], maxreq_str[32], max429_str[32];
        snprintf(sleep_str, sizeof(sleep_str), "%d", wa->sleep_ms);
        snprintf(maxreq_str, sizeof(maxreq_str), "%d", wa->max_req);
        snprintf(max429_str, sizeof(max429_str), "%d", wa->max_429);
        char *args[] = {"--from", wa->from, "--to", wa->to,
                        "--sleep-ms", sleep_str, "--max-req", maxreq_str, "--max-429", max429_str};
        rc = cmd_fetch(10, args);
    } else if (wa->mode == 1) {
        char sleep_str[32], maxreq_str[32], max429_str[32];
        snprintf(sleep_str, sizeof(sleep_str), "%d", wa->sleep_ms);
        snprintf(maxreq_str, sizeof(maxreq_str), "%d", wa->max_req);
        snprintf(max429_str, sizeof(max429_str), "%d", wa->max_429);
        char *args[] = {"--from", wa->from, "--to", wa->to,
                        "--sleep-ms", sleep_str, "--max-req", maxreq_str, "--max-429", max429_str};
        rc = cmd_fetch_mini(10, args);
    } else if (wa->mode == 2) {
        char max_sys_str[16], prop_str[16];
        snprintf(max_sys_str, sizeof(max_sys_str), "%d", wa->play_max_system);
        snprintf(prop_str, sizeof(prop_str), "%d", wa->play_proposals);
        char *args[] = {"--max-system", max_sys_str, "--proposals", prop_str};
        rc = cmd_play(4, args);
    } else if (wa->mode == 3) {
        char max_sys_str[16], prop_str[16];
        snprintf(max_sys_str, sizeof(max_sys_str), "%d", wa->play_mini_max_system);
        snprintf(prop_str, sizeof(prop_str), "%d", wa->play_mini_proposals);
        char *args[] = {"--max-system", max_sys_str, "--proposals", prop_str};
        rc = cmd_play_mini(4, args);
    } else if (wa->mode == 4) {
        char wheel_str[32];
        snprintf(wheel_str, sizeof(wheel_str), "%d/6/%d", wa->wheel_v, wa->wheel_t);
        char *args[] = {"--wheel", wheel_str};
        rc = cmd_play(2, args);
    } else {
        char wheel_str[32];
        snprintf(wheel_str, sizeof(wheel_str), "%d/5/%d", wa->wheel_mini_v, wa->wheel_mini_t);
        char *args[] = {"--wheel", wheel_str};
        rc = cmd_play_mini(2, args);
    }

    fflush(stdout);
    fflush(stderr);

    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);

    close(g_pipe_fd[1]);
    close(g_pipe_fd[0]);

    char msg[128];
    snprintf(msg, sizeof(msg), "\n--- Finished (exit code: %d) ---\n", rc);
    gui_append(msg);
    g_last_exit_code = rc;

    free(wa);
    g_idle_add(gui_done_idle, NULL);
    return NULL;
}

// Obsługa kliknięcia przycisku "Run" — zbiera parametry z aktywnej zakładki
// i uruchamia wątek roboczy
static void on_run_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    if (g_running) return;

    g_running = true;
    gtk_widget_set_sensitive(g_run_btn, FALSE);
    gtk_spinner_start(GTK_SPINNER(g_spinner));

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress_bar), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_progress_bar), "0%");
    gtk_label_set_text(GTK_LABEL(g_status_label), "Starting...");
    gtk_label_set_text(GTK_LABEL(g_elapsed_label), "Elapsed: 0:00");

    if (g_elapsed_timer) g_timer_destroy(g_elapsed_timer);
    g_elapsed_timer = g_timer_new();
    g_timer_start(g_elapsed_timer);

    if (g_timer_id) g_source_remove(g_timer_id);
    g_timer_id = g_timeout_add(100, gui_timer_tick, NULL);

    gtk_text_buffer_set_text(g_textbuf, "", 0);

    WorkerArgs *wa = (WorkerArgs *)calloc(1, sizeof(WorkerArgs));
    if (!wa) {
        gui_append("Error: out of memory.\n");
        g_running = false;
        gtk_widget_set_sensitive(g_run_btn, TRUE);
        gtk_spinner_stop(GTK_SPINNER(g_spinner));
        return;
    }

    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    wa->mode = page;

    if (page == 0) {
        snprintf(wa->from, sizeof(wa->from), "%s", gtk_entry_get_text(GTK_ENTRY(g_fetch_from)));
        snprintf(wa->to, sizeof(wa->to), "%s", gtk_entry_get_text(GTK_ENTRY(g_fetch_to)));
        wa->sleep_ms = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_fetch_sleep));
        wa->max_req = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_fetch_maxreq));
        wa->max_429 = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_fetch_max429));
    } else if (page == 1) {
        snprintf(wa->from, sizeof(wa->from), "%s", gtk_entry_get_text(GTK_ENTRY(g_fetch_mini_from)));
        snprintf(wa->to, sizeof(wa->to), "%s", gtk_entry_get_text(GTK_ENTRY(g_fetch_mini_to)));
        wa->sleep_ms = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_fetch_mini_sleep));
        wa->max_req = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_fetch_mini_maxreq));
        wa->max_429 = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_fetch_mini_max429));
    } else if (page == 2) {
        wa->play_max_system = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_play_max_system));
        wa->play_proposals = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_play_proposals));
    } else if (page == 3) {
        wa->play_mini_max_system = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_play_mini_max_system));
        wa->play_mini_proposals = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_play_mini_proposals));
    } else if (page == 4) {
        wa->wheel_v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_wheel_v));
        wa->wheel_t = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_wheel_t));
    } else {
        wa->wheel_mini_v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_wheel_mini_v));
        wa->wheel_mini_t = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_wheel_mini_t));
    }

    if (pipe(g_pipe_fd) != 0) {
        gui_append("Error: pipe() failed.\n");
        g_running = false;
        gtk_widget_set_sensitive(g_run_btn, TRUE);
        gtk_spinner_stop(GTK_SPINNER(g_spinner));
        free(wa);
        return;
    }

    pthread_t reader;
    if (pthread_create(&reader, NULL, pipe_reader_func, NULL) != 0) {
        gui_append("Error: cannot start output reader thread.\n");
        close(g_pipe_fd[0]);
        close(g_pipe_fd[1]);
        g_running = false;
        gtk_widget_set_sensitive(g_run_btn, TRUE);
        gtk_spinner_stop(GTK_SPINNER(g_spinner));
        free(wa);
        return;
    }
    pthread_detach(reader);

    if (pthread_create(&g_worker_thread, NULL, worker_func, wa) != 0) {
        gui_append("Error: cannot start worker thread.\n");
        close(g_pipe_fd[0]);
        close(g_pipe_fd[1]);
        g_running = false;
        gtk_widget_set_sensitive(g_run_btn, TRUE);
        gtk_spinner_stop(GTK_SPINNER(g_spinner));
        free(wa);
        return;
    }
    pthread_detach(g_worker_thread);
}

// Tworzy wiersz: etykieta + widget wejściowy
static GtkWidget *make_label_entry_row(const char *label_text, GtkWidget *widget) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_size_request(label, 140, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
    return hbox;
}

static GtkWidget *create_fetch_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    g_fetch_from = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_fetch_from), "2010-01-01");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_fetch_from), "YYYY-MM-DD");
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Date from:", g_fetch_from), FALSE, FALSE, 0);

    g_fetch_to = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_fetch_to), "2026-03-18");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_fetch_to), "YYYY-MM-DD");
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Date to:", g_fetch_to), FALSE, FALSE, 0);

    g_fetch_sleep = gtk_spin_button_new_with_range(100, 10000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_fetch_sleep), 1200);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Sleep (ms):", g_fetch_sleep), FALSE, FALSE, 0);

    g_fetch_maxreq = gtk_spin_button_new_with_range(1, 10000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_fetch_maxreq), 400);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Max requests:", g_fetch_maxreq), FALSE, FALSE, 0);

    g_fetch_max429 = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_fetch_max429), 10);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Max 429 errors:", g_fetch_max429), FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *create_fetch_mini_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    g_fetch_mini_from = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_fetch_mini_from), "2010-01-01");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_fetch_mini_from), "YYYY-MM-DD");
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Date from:", g_fetch_mini_from), FALSE, FALSE, 0);

    g_fetch_mini_to = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_fetch_mini_to), "2026-03-18");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_fetch_mini_to), "YYYY-MM-DD");
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Date to:", g_fetch_mini_to), FALSE, FALSE, 0);

    g_fetch_mini_sleep = gtk_spin_button_new_with_range(100, 10000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_fetch_mini_sleep), 1200);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Sleep (ms):", g_fetch_mini_sleep), FALSE, FALSE, 0);

    g_fetch_mini_maxreq = gtk_spin_button_new_with_range(1, 10000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_fetch_mini_maxreq), 400);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Max requests:", g_fetch_mini_maxreq), FALSE, FALSE, 0);

    g_fetch_mini_max429 = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_fetch_mini_max429), 10);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Max 429 errors:", g_fetch_mini_max429), FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *create_wheel_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    GtkWidget *info = gtk_label_new(
        "System skrocony (wheel) dla Lotto 6/49.\n"
        "Wybierasz V liczb z puli, system generuje n kuponow tak,\n"
        "by gwarantowac >=1 trafienie t-z-6, gdy w Twojej puli V\n"
        "znajdzie sie >=t wylosowanych liczb.\n"
        "Dostepnosc: --list-wheels w CLI. Pelny system: t = 6.");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), info, FALSE, FALSE, 4);

    g_wheel_v = gtk_spin_button_new_with_range(6, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wheel_v), 7);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Pula V liczb:", g_wheel_v), FALSE, FALSE, 0);

    g_wheel_t = gtk_spin_button_new_with_range(2, 6, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wheel_t), 5);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Gwarancja t (z 6):", g_wheel_t), FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *create_wheel_mini_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    GtkWidget *info = gtk_label_new(
        "System skrocony (wheel) dla Mini Lotto 5/42.\n"
        "Wybierasz V liczb z puli, system generuje n kuponow tak,\n"
        "by gwarantowac >=1 trafienie t-z-5, gdy w Twojej puli V\n"
        "znajdzie sie >=t wylosowanych liczb.\n"
        "Dostepnosc: --list-wheels w CLI. Pelny system: t = 5.");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), info, FALSE, FALSE, 4);

    g_wheel_mini_v = gtk_spin_button_new_with_range(5, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wheel_mini_v), 6);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Pula V liczb:", g_wheel_mini_v), FALSE, FALSE, 0);

    g_wheel_mini_t = gtk_spin_button_new_with_range(2, 5, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wheel_mini_t), 5);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Gwarancja t (z 5):", g_wheel_mini_t), FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *create_play_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    GtkWidget *info = gtk_label_new(
        "Pobiera najnowsze dane, analizuje historię\n"
        "i generuje gotowe systemy lotto do gry.\n"
        "System K = K liczb -> C(K,6) kuponów.");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), info, FALSE, FALSE, 4);

    g_play_max_system = gtk_spin_button_new_with_range(7, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_play_max_system), 10);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Maks. system (K):", g_play_max_system), FALSE, FALSE, 0);

    g_play_proposals = gtk_spin_button_new_with_range(1, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_play_proposals), 3);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Propozycji na system:", g_play_proposals), FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *create_play_mini_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    GtkWidget *info = gtk_label_new(
        "Pobiera najnowsze dane Mini Lotto (5/42),\n"
        "analizuje historie i generuje systemy do gry.\n"
        "System K = K liczb -> C(K,5) kuponow.");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), info, FALSE, FALSE, 4);

    g_play_mini_max_system = gtk_spin_button_new_with_range(5, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_play_mini_max_system), 9);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Maks. system (K):", g_play_mini_max_system), FALSE, FALSE, 0);

    g_play_mini_proposals = gtk_spin_button_new_with_range(1, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_play_mini_proposals), 3);
    gtk_box_pack_start(GTK_BOX(vbox), make_label_entry_row("Propozycji na system:", g_play_mini_proposals), FALSE, FALSE, 0);

    return vbox;
}

int cmd_gui(int argc, char **argv) {
    gtk_init(&argc, &argv);

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "Lotto + Mini Lotto Analyzer v2.0");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 720, 600);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_main), 8);

    g_notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), create_fetch_tab(), gtk_label_new("Fetch-lotto"));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), create_fetch_mini_tab(), gtk_label_new("Fetch-mini"));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), create_play_tab(), gtk_label_new("> Graj-lotto"));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), create_play_mini_tab(), gtk_label_new("> Graj-mini-lotto"));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), create_wheel_tab(), gtk_label_new("Wheel-lotto"));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), create_wheel_mini_tab(), gtk_label_new("Wheel-mini"));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), 2);
    gtk_box_pack_start(GTK_BOX(vbox_main), g_notebook, FALSE, FALSE, 0);

    GtkWidget *hbox_run = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_run_btn = gtk_button_new_with_label("Run");
    gtk_widget_set_size_request(g_run_btn, 120, 36);
    g_signal_connect(g_run_btn, "clicked", G_CALLBACK(on_run_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_run), g_run_btn, FALSE, FALSE, 0);

    g_spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(hbox_run), g_spinner, FALSE, FALSE, 0);

    g_elapsed_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(hbox_run), g_elapsed_label, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(vbox_main), hbox_run, FALSE, FALSE, 4);

    g_progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(g_progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_progress_bar), "");
    gtk_box_pack_start(GTK_BOX(vbox_main), g_progress_bar, FALSE, FALSE, 2);

    g_status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(vbox_main), g_status_label, FALSE, FALSE, 2);

    GtkWidget *frame = gtk_frame_new("Output");
    g_textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_textview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(g_textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_textview), GTK_WRAP_WORD_CHAR);
    g_textbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview));

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "textview { font-family: Monospace; font-size: 10pt; }", -1, NULL);
    GtkStyleContext *ctx = gtk_widget_get_style_context(g_textview);
    gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_textview);
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    gtk_box_pack_start(GTK_BOX(vbox_main), frame, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(g_window), vbox_main);
    gtk_widget_show_all(g_window);

    gtk_main();
    return 0;
}

#else /* brak GTK — implementacje zastępcze dla trybu CLI */

void gui_set_status(const char *fmt, ...) {
    (void)fmt;
    /* brak GUI — nic do wyświetlenia */
}

void gui_set_progress(float fraction) {
    (void)fraction;
    /* brak GUI — nic do wyświetlenia */
}

int cmd_gui(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr,
        "Blad: program skompilowany bez obslugi GUI (brak biblioteki GTK3).\n"
        "Na tym systemie dostepny jest wylacznie tryb CLI, np.:\n"
        "  ./lotto play\n"
        "  ./lotto play-mini\n"
        "  ./lotto optimize\n"
        "  ./lotto backtest\n"
        "  ./lotto fetch --from 2020-01-01 --to 2026-03-18\n");
    return 1;
}

#endif /* HAS_GTK */
