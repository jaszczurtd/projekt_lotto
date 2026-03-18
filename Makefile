CC      = gcc
CFLAGS  = -O2 -std=c11 -Wall -Wextra $(shell pkg-config --cflags libcjson)
LDFLAGS = -lcurl -lm -lpthread $(shell pkg-config --libs libcjson)

# Automatyczna detekcja GTK3 — jeśli dostępne, kompilujemy z GUI
GTK_AVAILABLE := $(shell pkg-config --exists gtk+-3.0 2>/dev/null && echo yes)
ifeq ($(GTK_AVAILABLE),yes)
  CFLAGS  += -DHAS_GTK $(shell pkg-config --cflags gtk+-3.0)
  LDFLAGS += $(shell pkg-config --libs gtk+-3.0)
endif

TARGET  = lotto
SRC     = main.c lotto.c mini_lotto.c network.c tools.c gui.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
