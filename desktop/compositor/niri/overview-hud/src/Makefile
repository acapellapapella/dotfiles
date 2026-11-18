# midve-hud: the overview HUD. `make install` puts it in ~/.local/bin.
CFLAGS ?= -O2 -pipe
PKGS = gtk+-3.0 gtk-layer-shell-0 json-glib-1.0 gio-unix-2.0 pangocairo
DEST = $(HOME)/.local/bin

midve-hud: hud.c
	$(CC) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -o $@ hud.c \
		$(shell pkg-config --cflags --libs $(PKGS)) -lm -ldl

install: midve-hud
	install -Dm755 midve-hud $(DEST)/midve-hud

clean:
	rm -f midve-hud

.PHONY: install clean
