PREFIX ?= $(HOME)/.local
# démon root : dossier appartenant à root (paquet : /usr/lib/coreboard)
FAND_DIR ?= /usr/local/libexec/coreboard
PKGS   := gtk4 json-glib-1.0 fontconfig gio-unix-2.0
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DDATADIR=\"$(PREFIX)/share/coreboard\" -DFAND_DIR=\"$(FAND_DIR)\" $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm -ldl
.DEFAULT_GOAL := coreboard

# versions minimales : GTK 4.10 (GtkFileDialog), json-glib 1.6
# (Ubuntu 24.04, Debian 13, Fedora 38, openSUSE Tumbleweed, Arch et dérivées)
check:
	@pkg-config --exists $(PKGS) || { echo "Dépendances de compilation manquantes : lance « make deps »"; exit 1; }
	@pkg-config --atleast-version=4.10 gtk4 || { echo "GTK $$(pkg-config --modversion gtk4) trop ancien : GTK 4.10 ou plus récent requis"; exit 1; }
	@pkg-config --atleast-version=1.6 json-glib-1.0 || { echo "json-glib 1.6 ou plus récent requis"; exit 1; }

# dépendances de compilation et d'exécution selon la distribution (demande le mot de passe)
deps:
	@if command -v pacman >/dev/null; then sudo pacman -S --needed base-devel gtk4 json-glib fontconfig python curl; \
	elif command -v apt-get >/dev/null; then sudo apt-get install -y build-essential pkg-config libgtk-4-dev libjson-glib-dev libfontconfig-dev python3 curl; \
	elif command -v dnf >/dev/null; then sudo dnf install -y gcc make pkgconf-pkg-config gtk4-devel json-glib-devel fontconfig-devel python3 curl; \
	elif command -v zypper >/dev/null; then sudo zypper install -y gcc make pkg-config gtk4-devel json-glib-devel fontconfig-devel python3 curl; \
	else echo "Distribution non reconnue : installe gcc, make, pkg-config, gtk4 (dev), json-glib (dev), fontconfig (dev), python3, curl"; exit 1; fi

.PHONY: check deps

coreboard: src/main.c src/hw.c src/hw.h src/features.c src/features.h src/rgb.c src/rgb.h src/music.c src/music.h src/gamevisual.c src/gamevisual.h src/macros.c src/macros.h src/winrun.c src/winrun.h src/pe.c src/pe.h src/tune.c src/tune.h src/dlss.c src/dlss.h src/fanctl.c src/fanctl.h src/compat.c src/compat.h | check
	$(CC) $(CFLAGS) src/main.c src/hw.c src/features.c src/rgb.c src/music.c src/gamevisual.c src/macros.c src/winrun.c src/pe.c src/tune.c src/dlss.c src/fanctl.c src/compat.c -o $@ $(LDLIBS)

clean:
	rm -f coreboard

.PHONY: clean

test: tests/hwtest.c src/hw.c src/hw.h
	$(CC) $(CFLAGS) tests/hwtest.c src/hw.c -o tests/hwtest $(LDLIBS)
	./tests/hwtest

.PHONY: test

# DESTDIR : racine de mise en paquet (PKGBUILD) ; les chemins compilés dans le programme restent ceux de PREFIX
D := $(DESTDIR)$(PREFIX)
install: coreboard
	# ne supprime que les fichiers installés : ce dossier contient aussi les données utilisateur (préfixes Wine, journaux, téléchargements)
	rm -rf $(D)/share/coreboard/fonts $(D)/share/coreboard/devices
	rm -f $(D)/share/coreboard/coreboard-fand   # ancien emplacement du démon root (modifiable par l'utilisateur)
	install -Dm755 coreboard $(D)/bin/coreboard
	install -Dm644 data/fonts/Orbitron.ttf $(D)/share/coreboard/fonts/Orbitron.ttf
	install -Dm644 data/fonts/Rajdhani-Medium.ttf $(D)/share/coreboard/fonts/Rajdhani-Medium.ttf
	install -Dm644 data/fonts/Rajdhani-Bold.ttf $(D)/share/coreboard/fonts/Rajdhani-Bold.ttf
	-install -Dm644 data/devices/*.png -t $(D)/share/coreboard/devices
	install -Dm644 data/sounds/startup.ogg $(D)/share/coreboard/sounds/startup.ogg
	install -Dm755 data/fistgirl_helper.py $(D)/share/coreboard/fistgirl_helper.py
	-[ -f fistgirl/add-on/get_ff_link.py ] && install -Dm755 fistgirl/add-on/get_ff_link.py $(D)/share/coreboard/get_ff_link.py
	install -Dm644 icon.svg $(D)/share/icons/hicolor/scalable/apps/coreboard.svg
	install -Dm644 data/coreboard.desktop $(D)/share/applications/coreboard.desktop
	sed -i 's|^Exec=.*|Exec=$(PREFIX)/bin/coreboard|' $(D)/share/applications/coreboard.desktop
	@if [ -z "$(DESTDIR)" ]; then update-desktop-database $(D)/share/applications 2>/dev/null; gtk-update-icon-cache -q -t $(D)/share/icons/hicolor 2>/dev/null; \
	  echo "Coreboard installé dans $(PREFIX) (bin/coreboard). Vérifie que $(PREFIX)/bin est dans ton PATH."; fi; true

# fichiers système d'un paquet (PKGBUILD, avec DESTDIR) : démon root, règle udev du clavier RGB, licence
UDEVDIR ?= /usr/lib/udev/rules.d
install-system:
	install -Dm755 data/coreboard-fand $(DESTDIR)$(FAND_DIR)/coreboard-fand
	install -Dm644 data/70-coreboard-rgb.rules $(DESTDIR)$(UDEVDIR)/70-coreboard-rgb.rules
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/coreboard/LICENSE

uninstall:
	rm -rf $(D)/share/coreboard/fonts $(D)/share/coreboard/devices $(D)/share/coreboard/sounds $(D)/share/coreboard/fistgirl_helper.py $(D)/share/coreboard/get_ff_link.py $(D)/bin/coreboard $(D)/share/applications/coreboard.desktop $(D)/share/icons/hicolor/scalable/apps/coreboard.svg
	-update-desktop-database $(D)/share/applications 2>/dev/null

.PHONY: install install-system uninstall

# accès au clavier RGB (règle udev « uaccess », numéro < 73 obligatoire : à lancer avec sudo)
install-udev:
	rm -f /etc/udev/rules.d/99-coreboard-rgb.rules
	install -Dm644 data/70-coreboard-rgb.rules /etc/udev/rules.d/70-coreboard-rgb.rules
	udevadm control --reload
	udevadm trigger --subsystem-match=hidraw --action=change

.PHONY: install-udev

# démon root de refroidissement : installé hors du dossier utilisateur, propriété de root (à lancer avec sudo)
install-fand:
	install -o root -g root -Dm755 data/coreboard-fand $(FAND_DIR)/coreboard-fand

uninstall-fand:
	rm -f $(FAND_DIR)/coreboard-fand
	-rmdir $(FAND_DIR) 2>/dev/null

.PHONY: install-fand uninstall-fand
