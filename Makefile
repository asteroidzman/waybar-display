PLUGIN  := libdisplay.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 json-glib-1.0 gtk-layer-shell-0
WBCOMMON ?= common
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC -I$(WBCOMMON) $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local/lib/waybar
DATADIR ?= $(HOME)/.local/share/waybar-display
DESTDIR ?=

$(PLUGIN): src/display.c $(WBCOMMON)/wbcommon.h
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(DESTDIR)$(PREFIX)/$(PLUGIN)
	install -Dm644 -t $(DESTDIR)$(DATADIR) assets/display.svg
	@echo "installed to $(DESTDIR)$(PREFIX)/$(PLUGIN) + icon in $(DESTDIR)$(DATADIR)"

test_display: tests/test_display.c src/display.c $(WBCOMMON)/wbcommon.h
	$(CC) $(CFLAGS) -o $@ tests/test_display.c $(LDLIBS)

test: test_display
	./test_display

clean:
	rm -f $(PLUGIN) test_display
.PHONY: install clean test
