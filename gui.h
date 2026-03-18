// gui.h
// Interfejs GUI (GTK) — API raportowania postępu i punkt wejścia GUI.

#ifndef GUI_H
#define GUI_H

// Ustawia tekst statusu wyświetlany w GUI (thread-safe)
void gui_set_status(const char *fmt, ...);

// Ustawia postęp paska (0.0 – 1.0, thread-safe)
void gui_set_progress(float fraction);

// Uruchamia okno GTK
int cmd_gui(int argc, char **argv);

#endif
