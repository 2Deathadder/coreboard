# Coreboard

Centre de contrôle matériel **natif** pour Linux, écrit en C (GTK4 + Cairo/Pango), au style d'**Armoury Crate** (barre latérale, en-têtes à barres rouges, cartes GPU/CPU/Ventilateurs/Mémoire, modes de fonctionnement)
(noir / rouge, ratio 16:9 conservé quelle que soit la taille de la fenêtre).
Il détecte ce que ton PC expose réellement et **masque** ce qui n'existe pas.

## Installation (Arch et dérivées)

    git clone https://github.com/2Deathadder/coreboard.git
    cd coreboard/packaging/arch && makepkg -si

Le paquet installe aussi le démon de refroidissement et la règle udev du clavier RGB (aucune étape `sudo make` en plus).

## Compilation / installation

Dépendances : `gcc`, `make`, `pkg-config`, GTK ≥ 4.10, json-glib ≥ 1.6, `fontconfig`, `python3`, `curl`
(Arch et dérivées, Ubuntu 24.04+, Debian 13+, Fedora 38+, openSUSE Tumbleweed). `make deps` les installe pour la distribution détectée.

    make deps            # dépendances de compilation (pacman, apt, dnf ou zypper)
    make                 # compile
    make install         # ~/.local (PREFIX=/usr/local pour un autre préfixe)
    make uninstall
    sudo make install-fand   # démon root de refroidissement adaptatif (MSI), installé dans /usr/local/libexec/coreboard
    make test            # test d'intégration de la couche matériel (change puis restaure écran/volume/profil)

`coreboard` lance l'app ; un second appel `coreboard --toggle` ferme la fenêtre si elle est déjà ouverte
(instance unique via D-Bus/GApplication, idéal pour un raccourci clavier).

## Ce qui est détecté

| Fonction | Sources |
|---|---|
| Profils d'alimentation | power-profiles-daemon ou tuned-ppd (D-Bus), sinon ACPI `platform_profile` |
| CPU | Intel (`coretemp`, `intel_pstate`) et AMD (`k10temp`, `amd_pstate`) : Turbo/Boost, EPP, gouverneur |
| GPU | NVIDIA (NVML chargée dynamiquement, sans réveiller un dGPU endormi), AMD et Intel (DRM/sysfs) |
| Mode GPU hybride | `supergfxctl`, `envycontrol` |
| Écran (fréquence) | Hyprland, Sway (`swaymsg`), KDE Plasma (`kscreen-doctor`), GNOME (D-Bus Mutter), X11 (`xrandr`), wlroots (`wlr-randr`) |
| Luminosité | `/sys/class/backlight` via `brightnessctl`, sinon systemd-logind (sans root) |
| Audio | `pactl` (PulseAudio/PipeWire) ou `wpctl` |
| Filtre lumière bleue | Omarchy, GNOME, KDE Plasma (réglage natif), sinon `gammastep`, `redshift`, `wlsunset` |
| Ventilateurs / températures | hwmon (lecture seule) |
| Batterie / disque / réseau | sysfs, `nmcli` |

Les écritures root (Turbo, EPP, gouverneur, profil ACPI, envycontrol) passent par `pkexec` (polkit).

## Compatibilité

| | Hyprland | Sway | KDE Plasma | GNOME | X11 (autres) |
|---|---|---|---|---|---|
| Matériel, profils, CPU/GPU, audio, luminosité | ✓ | ✓ | ✓ | ✓ | ✓ |
| Fréquence d'écran | ✓ | ✓ | ✓ | ✓ | ✓ (`xrandr`) |
| Filtre lumière bleue | outil autonome | outil autonome | ✓ natif | ✓ natif | outil autonome |
| Scénarios / fenêtre active | ✓ | ✓ | XWayland (`xprop`) | XWayland (`xprop`) | ✓ (`xprop`) |
| Macros : rejeu | `wtype` | `wtype` | `ydotool` | `ydotool` | `xdotool` |
| Macros : raccourci global | automatique | commande copiée | commande copiée | commande copiée | commande copiée |
| GameVisual (filtre d'écran) | ✓ | — | — | — | — |

Sous GNOME/KDE en Wayland, les jeux Proton/Wine tournent en XWayland : ils sont donc détectés par les scénarios.
Hors Hyprland, « Raccourci » copie la commande `coreboard --macro "Nom"` à coller dans les raccourcis personnalisés du bureau.

Installations depuis l'interface : paquets de la distribution via `pkexec` (pacman, apt, dnf, zypper), un paquet absent
des dépôts n'empêchant pas les autres. Ce qui n'est pas dans les dépôts est téléchargé en version officielle dans
`~/.local` sans root : umu-launcher (zipapp), legendary (binaire), steamcmd (archive Valve). Sur Arch, steamcmd,
legendary et lgogdownloader sont compilés depuis l'AUR.

Matériel : CPU Intel, AMD et ARM ; GPU NVIDIA, AMD et Intel. L'éclairage RGB (MSI MysticLight) et le refroidissement
adaptatif (MSI Katana, firmware vérifié avant toute écriture) ne s'affichent que sur le matériel concerné.

## Fonctionnalités type Armoury Crate

- **Appareils** : périphériques externes (USB, Bluetooth) et batteries (UPower : souris, clavier, casque…).
- **Scénarios** : quand une application prend le focus (Hyprland), applique un mode de fonctionnement et une fréquence d'écran, puis restaure les réglages précédents. Règles dans `~/.config/coreboard/scenarios.json`.
  Tant qu'un scénario est activé, Coreboard reste actif en arrière-plan après la fermeture de la fenêtre ; `coreboard --background` le lance sans fenêtre (à mettre dans `exec-once`) et `coreboard --quit` l'arrête.
- **Jeux** : bibliothèque Steam, applications de catégorie « Jeu » et applications ajoutées depuis les fenêtres ouvertes (`~/.config/coreboard/games.json`), avec création d'un profil de scénario en un clic.

- **Éclairage** : RGB du clavier MSI MysticLight (4 zones) : Éteint, Fixe, Respiration, Cycle, Vague, **Personnalisé** (un effet et une couleur différents par zone) et **Musique** (le clavier réagit à l'audio du système ; nécessite `parec`) avec trois styles — couleur choisie, spectre, battements (détection du tempo) — et réglages de sensibilité, de lissage et de lueur de fond. L'aperçu utilise les durées de cycle réelles du clavier (3 à 24 s). Réglage mémorisé dans `~/.config/coreboard/rgb.json` et réappliqué au démarrage.
  Accès au clavier : `sudo make install-udev` (règle udev « uaccess », une seule fois). En ligne de commande : `coreboard --rgb static 00ff00`, `--rgb off`, `--rgb breathing RRGGBB`, `--rgb cycle`, `--rgb wave [gauche|droite]`, `--rgb luminosite 50`.

- **GameVisual** : filtres couleur plein écran (shader Hyprland) — préréglages Défaut, FPS, Course, Cinéma, Vivid, Lecture, Monochrome ou réglages libres (luminosité, contraste, saturation, gamma, température). Réglage mémorisé dans `~/.config/coreboard/gamevisual.json` et réappliqué au démarrage. Un filtre actif peut désactiver le rendu direct des jeux en plein écran.
- **Macros** : enregistre les touches tapées dans la fenêtre Coreboard (texte, combinaisons, pauses), rejoue-les avec `wtype` (répétitions ×N) et assigne-leur un raccourci global. Macros dans `~/.config/coreboard/macros.json` ; les raccourcis sont écrits dans `~/.config/hypr/coreboard-macros.lua` (chargé par `bindings.lua` avec `pcall(require, "hypr.coreboard-macros")`). Lancement en ligne de commande : `coreboard --macro "Nom"`. Une étape de type `cmd` (commande shell) peut être ajoutée à la main dans le JSON.

- **Jeux Windows** : lance des `.exe` (Black Myth: Wukong, Cyberpunk, Elden Ring…) via **Proton-GE et umu-launcher** : DirectX 11/12 traduit en Vulkan (DXVK / VKD3D-Proton), préfixe Wine dédié par jeu, corrections automatiques pour les jeux connus, GPU NVIDIA dédié, cache de shaders durable, DLSS/NVAPI, ray tracing (DXR), GameMode et MangoHud en option. Ajout d'un jeu : « Ajouter un .exe » ; options par jeu (arguments, préfixe, journal) ; réglages dans `~/.config/coreboard/wingames.json`, préfixes dans `~/.local/share/coreboard/prefixes/`, journaux dans `~/.local/share/coreboard/logs/`.
  **Analyse d'exécutable (C, `src/pe.c`)** : à l'ajout, Coreboard lit l'en-tête PE du `.exe` (architecture, table d'imports, chaînes, fichiers voisins) et affiche sur la carte : 32/64 bits, API graphique (DirectX 9/10/11/12, Vulkan), moteur (Unreal, Unity…), DLSS/NVAPI, .NET, et l'anti-triche détecté. Un anti-triche noyau (Vanguard, Ricochet…) ou un exécutable ARM64 demande une confirmation avant le lancement. En ligne de commande : `coreboard --inspect JEU.exe`.
  **Amélioration des performances et du rendu** : préréglages par jeu (Performance, Équilibré, Qualité, Économie) et réglages fins — ntsync, compilation de shaders asynchrone, faible latence, filtrage anisotrope 16x, DLSS avec mise à jour automatique, ray tracing, mise à l'échelle FSR, limiteur d'images (DXVK et MangoHud), caches de shaders persistants par jeu. Un **diagnostic système** signale ce qui limite les jeux (module ntsync, `vm.max_map_count`, limite de fichiers, groupe `gamemode`, alimentation, espace disque) et « Optimiser le système » applique les corrections en une demande de mot de passe. GameVisual ajoute netteté et vibrance à l'écran entier.
  **DLSS** : détection du GPU et de ses capacités (Super Resolution, Ray Reconstruction, Frame Generation 2x à partir de la série 40, Multi Frame Generation et DLSS 5 sur la série 50), versions DLSS présentes dans le dossier de chaque jeu (lecture des ressources de version des DLL `nvngx_dlss*.dll`), forçage du modèle Super Resolution (dernier, J, K, L, M), Frame Generation, Ray Reconstruction et indicateur à l'écran via dxvk-nvapi. **DLSS 5** : NVIDIA ne le propose pour l'instant que sur RTX 50 (RTX 40 « prévue »), et sous Linux seule une couche tierce expérimentale (DLSS5VKLayer) existe ; Coreboard affiche donc son statut réel sans l'activer artificiellement.
  Composants : umu-launcher, GameMode, MangoHud et Vulkan 32 bits (bouton « Installer les composants manquants », paquets de la distribution via pkexec, umu-launcher officiel dans `~/.local` s'il n'est pas dans les dépôts) ; Proton-GE se télécharge depuis l'interface (téléchargement reprenable, aussi `coreboard --install-proton`). **Limite** : les jeux à anti-triche noyau (Warzone, Valorant, Fortnite…) ne fonctionnent pas sous Linux.

## Raccourcis

La touche dédiée F7 (sans Fn, signal `XF86Tools`) et `SUPER + ALT + R` ouvrent/ferment Coreboard (`coreboard --toggle`), à déclarer dans `~/.config/hypr/bindings.lua` :

    o.bind("XF86Tools", "Coreboard", "coreboard --toggle")
    o.bind("SUPER + ALT + R", "Coreboard", "coreboard --toggle")

Autres bureaux : ajoute un raccourci personnalisé avec la commande `coreboard --toggle`
(GNOME : Paramètres → Clavier → Raccourcis personnalisés ; KDE : Configuration → Raccourcis → Ajouter une commande ;
Sway : `bindsym $mod+Alt+r exec coreboard --toggle`).

## Photo de l'appareil

La page d'accueil affiche la photo de ton PC si `~/.local/share/coreboard/devices/<modèle>.png` existe ; sinon un
portable ou un PC de bureau dessiné, selon le type de châssis détecté. Clique sur l'image pour choisir ta photo
(idéalement sur fond blanc) : le fond blanc est supprimé et l'image recadrée. Aucune photo de constructeur n'est fournie.

    coreboard --set-image ~/Downloads/mon-pc.jpg
    coreboard --set-image https://i.pinimg.com/…/image.jpg    # URL directe d'image (clic droit → copier l'adresse de l'image)

## Hyprland (optionnel)

    o.window({ class = "^dev\\.coreboard\\.Coreboard$" }, { float = true, center = true, size = { 1280, 700 }, tag = "-default-opacity", opacity = "1.0 1.0" })
    o.bind("SUPER + ALT + R", "Coreboard", "coreboard --toggle")

Polices embarquées : Orbitron et Rajdhani (licence SIL OFL).

## Licence

GPL-3.0-or-later (voir `LICENSE`). Polices Orbitron et Rajdhani : SIL OFL. Sous-module `fistgirl` : MIT.
