# niri-drops waybar module. `make install` puts it in ~/.local/lib/waybar.
CFLAGS ?= -O2 -pipe
PKGS = gtk+-3.0 json-glib-1.0 gio-unix-2.0
DEST = $(HOME)/.local/lib/waybar

niri-drops.so: drops.c waybar_cffi_module.h
	$(CC) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -fPIC -shared -o $@ drops.c \
		$(shell pkg-config --cflags --libs $(PKGS)) -lm

install: niri-drops.so
	install -Dm755 niri-drops.so $(DEST)/niri-drops.so

clean:
	rm -f niri-drops.so

.PHONY: install clean
