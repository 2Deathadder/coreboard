PREFIX ?= $(HOME)/.local
PKGS   := gtk4 json-glib-1.0 fontconfig gio-unix-2.0
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DDATADIR=\"$(PREFIX)/share/coreboard\" $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm -ldl

coreboard: src/main.c src/hw.c src/hw.h src/features.c src/features.h src/rgb.c src/rgb.h src/music.c src/music.h src/gamevisual.c src/gamevisual.h src/macros.c src/macros.h src/winrun.c src/winrun.h src/pe.c src/pe.h src/tune.c src/tune.h src/dlss.c src/dlss.h src/fanctl.c src/fanctl.h
	$(CC) $(CFLAGS) src/main.c src/hw.c src/features.c src/rgb.c src/music.c src/gamevisual.c src/macros.c src/winrun.c src/pe.c src/tune.c src/dlss.c src/fanctl.c -o $@ $(LDLIBS)

clean:
	rm -f coreboard

.PHONY: clean

test: tests/hwtest.c src/hw.c src/hw.h
	$(CC) $(CFLAGS) tests/hwtest.c src/hw.c -o tests/hwtest $(LDLIBS)
	./tests/hwtest

.PHONY: test

install: coreboard
	# ne supprime que les fichiers installés : ce dossier contient aussi les données utilisateur (préfixes Wine, journaux, téléchargements)
	rm -rf $(PREFIX)/share/coreboard/fonts $(PREFIX)/share/coreboard/devices
	rm -f $(PREFIX)/share/coreboard/coreboard-fand   # ancien emplacement du démon root (modifiable par l'utilisateur)
	install -Dm755 coreboard $(PREFIX)/bin/coreboard
	install -Dm644 data/fonts/Orbitron.ttf $(PREFIX)/share/coreboard/fonts/Orbitron.ttf
	install -Dm644 data/fonts/Rajdhani-Medium.ttf $(PREFIX)/share/coreboard/fonts/Rajdhani-Medium.ttf
	install -Dm644 data/fonts/Rajdhani-Bold.ttf $(PREFIX)/share/coreboard/fonts/Rajdhani-Bold.ttf
	-install -Dm644 data/devices/*.png -t $(PREFIX)/share/coreboard/devices
	install -Dm644 data/sounds/startup.ogg $(PREFIX)/share/coreboard/sounds/startup.ogg
	install -Dm755 data/fistgirl_helper.py $(PREFIX)/share/coreboard/fistgirl_helper.py
	-install -Dm755 fistgirl/add-on/get_ff_link.py $(PREFIX)/share/coreboard/get_ff_link.py
	install -Dm644 icon.svg $(PREFIX)/share/icons/hicolor/scalable/apps/coreboard.svg
	install -Dm644 data/coreboard.desktop $(PREFIX)/share/applications/coreboard.desktop
	sed -i 's|^Exec=.*|Exec=$(PREFIX)/bin/coreboard|' $(PREFIX)/share/applications/coreboard.desktop
	-update-desktop-database $(PREFIX)/share/applications 2>/dev/null
	-gtk-update-icon-cache -q -t $(PREFIX)/share/icons/hicolor 2>/dev/null
	@echo "Coreboard installé dans $(PREFIX) (bin/coreboard). Vérifie que $(PREFIX)/bin est dans ton PATH."


uninstall:
	rm -rf $(PREFIX)/share/coreboard/fonts $(PREFIX)/share/coreboard/devices $(PREFIX)/share/coreboard/sounds $(PREFIX)/bin/coreboard $(PREFIX)/share/applications/coreboard.desktop $(PREFIX)/share/icons/hicolor/scalable/apps/coreboard.svg
	-update-desktop-database $(PREFIX)/share/applications 2>/dev/null

.PHONY: install uninstall

# accès au clavier RGB (règle udev « uaccess », numéro < 73 obligatoire : à lancer avec sudo)
install-udev:
	rm -f /etc/udev/rules.d/99-coreboard-rgb.rules
	install -Dm644 data/70-coreboard-rgb.rules /etc/udev/rules.d/70-coreboard-rgb.rules
	udevadm control --reload
	udevadm trigger --subsystem-match=hidraw --action=change

.PHONY: install-udev

# démon root de refroidissement : installé hors du dossier utilisateur, propriété de root (à lancer avec sudo)
FAND_DIR ?= /usr/local/libexec/coreboard
install-fand:
	install -o root -g root -Dm755 data/coreboard-fand $(FAND_DIR)/coreboard-fand

uninstall-fand:
	rm -f $(FAND_DIR)/coreboard-fand
	-rmdir $(FAND_DIR) 2>/dev/null

.PHONY: install-fand uninstall-fand
