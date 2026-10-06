#!/usr/bin/env python3
"""Version anglaise du site : générée à partir du modèle français (seule source à maintenir).
Chaque phrase française doit exister dans le modèle : si elle change, la génération s'arrête pour signaler la traduction à revoir."""
import sys

SRC, OUT = sys.argv[1], sys.argv[2]
s = open(SRC, encoding="utf-8").read()

def tr(fr, en, every=False):
    global s
    n = s.count(fr)
    if n == 0 or (n > 1 and not every):
        sys.exit(f"i18n_en : « {fr[:70]} » trouvé {n} fois dans le modèle français")
    s = s.replace(fr, en)

# --- en-tête, URL, langue, chemins relatifs (la page anglaise est dans /en/)
tr('<html lang="fr">', '<html lang="en">')
tr('<title>Coreboard — Centre de contrôle matériel pour Linux, alternative à Armoury Crate</title>',
   '<title>Coreboard — Hardware control center for Linux, an Armoury Crate alternative</title>')
tr('content="Coreboard est un centre de contrôle matériel natif et gratuit pour Linux : modes de performance, CPU, GPU, écran, éclairage RGB, scénarios par application, macros et jeux Windows via Proton. Open source (GPL-3.0)."',
   'content="Coreboard is a free, native hardware control center for Linux: performance modes, CPU, GPU, display, RGB lighting, per-app scenarios, macros and Windows games through Proton. Open source (GPL-3.0)."')
tr('content="Coreboard, Linux, centre de contrôle, Armoury Crate Linux, alternative Armoury Crate, MSI Center Linux, RGB Linux, Proton-GE, jeux Linux, GTK4, Hyprland, GNOME, KDE"',
   'content="Coreboard, Linux, control center, Armoury Crate Linux, Armoury Crate alternative, MSI Center Linux, RGB Linux, Proton-GE, Linux gaming, GTK4, Hyprland, GNOME, KDE"')
tr('<link rel="canonical" href="https://2deathadder.github.io/coreboard/">', '<link rel="canonical" href="https://2deathadder.github.io/coreboard/en/">')
tr('<meta property="og:locale" content="fr_FR">', '<meta property="og:locale" content="en_US">\n<meta property="og:locale:alternate" content="fr_FR">')
tr('<meta property="og:url" content="https://2deathadder.github.io/coreboard/">', '<meta property="og:url" content="https://2deathadder.github.io/coreboard/en/">')
tr('Coreboard — Le centre de contrôle matériel natif pour Linux', 'Coreboard — The native hardware control center for Linux', every=True)
tr('content="Modes de performance, CPU/GPU, écran, RGB, scénarios, macros et jeux Windows via Proton, dans une seule application native et open source."',
   'content="Performance modes, CPU/GPU, display, RGB, scenarios, macros and Windows games through Proton, in one native open-source app."')
tr('content="Coreboard, le centre de contrôle matériel natif pour Linux"', 'content="Coreboard, the native hardware control center for Linux"')
tr('content="Une seule application native et open source pour piloter tout ton PC sous Linux."',
   'content="One native, open-source app to control your whole PC on Linux."')
tr('href="favicon.svg"', 'href="../favicon.svg"')
tr('src="favicon.svg"', 'src="../favicon.svg"')
tr('src="img/', 'src="../img/', every=True)
tr('data-img="img/', 'data-img="../img/', every=True)

# --- données structurées
tr('"description": "Centre de contrôle matériel natif pour Linux (GTK4) : modes de performance, CPU, GPU, écran, éclairage RGB, scénarios par application, macros et jeux Windows via Proton-GE."',
   '"description": "Native hardware control center for Linux (GTK4): performance modes, CPU, GPU, display, RGB lighting, per-app scenarios, macros and Windows games through Proton-GE."')
tr('"url": "https://2deathadder.github.io/coreboard/",', '"url": "https://2deathadder.github.io/coreboard/en/",')
tr('"inLanguage": "fr",', '"inLanguage": "en",')
tr('"priceCurrency": "EUR"', '"priceCurrency": "USD"')

# --- FAQ (dans les données structurées et dans la page)
FAQ = [
 ("Coreboard est-il une alternative à Armoury Crate ou MSI Center sous Linux ?", "Coreboard est-il une alternative à Armoury Crate ou MSI Center sous Linux&nbsp;?",
  "Is Coreboard an Armoury Crate or MSI Center alternative for Linux?"),
 ("Sur quelles distributions Linux Coreboard fonctionne-t-il ?", "Sur quelles distributions Linux Coreboard fonctionne-t-il&nbsp;?",
  "Which Linux distributions does Coreboard run on?"),
 ("Quels bureaux sont pris en charge ?", "Quels bureaux sont pris en charge&nbsp;?", "Which desktops are supported?"),
 ("Mon PC doit-il être un MSI ou un ASUS ?", "Mon PC doit-il être un MSI ou un ASUS&nbsp;?", "Does my PC have to be an MSI or an ASUS?"),
 ("Coreboard est-il gratuit ?", "Coreboard est-il gratuit&nbsp;?", "Is Coreboard free?"),
]
for q_ld, q_html, en in FAQ:
    tr(q_ld, en); tr(q_html, en)
tr("Oui. Coreboard regroupe dans une seule application native ce que proposent Armoury Crate ou MSI Center sous Windows",
   "Yes. Coreboard brings together in one native app what Armoury Crate or MSI Center offer on Windows", every=True)
tr("modes de performance, réglages du processeur et du GPU, écran, éclairage RGB, scénarios par application et gestion des jeux. Il n'est affilié ni à ASUS ni à MSI.",
   "performance modes, CPU and GPU tuning, display, RGB lighting, per-app scenarios and game management. It is affiliated with neither ASUS nor MSI.", every=True)
tr("Arch Linux et ses dérivées (Omarchy, CachyOS, EndeavourOS, Manjaro), Debian 13 et Ubuntu 24.04 ou plus récents, Fedora 38 ou plus récent et openSUSE Tumbleweed. Il faut GTK 4.10 ou plus récent.",
   "Arch Linux and its derivatives (Omarchy, CachyOS, EndeavourOS, Manjaro), Debian 13 and Ubuntu 24.04 or newer, Fedora 38 or newer and openSUSE Tumbleweed. GTK 4.10 or newer is required.", every=True)
tr("Hyprland, Sway, KDE Plasma, GNOME et les bureaux X11. Les fonctions s'adaptent", "Hyprland, Sway, KDE Plasma, GNOME and X11 desktops. Features adapt", every=True)
tr("fréquence d'écran via le bureau, filtre lumière bleue natif sous GNOME et KDE, détection des jeux XWayland.",
   "refresh rate through the desktop, native night light on GNOME and KDE, detection of XWayland games.", every=True)
tr("Non. Coreboard détecte ce que ton PC expose réellement (processeur Intel, AMD ou ARM, GPU NVIDIA, AMD ou Intel, écran, batterie) et masque ce qui n'existe pas. Seuls l'éclairage du clavier et le refroidissement adaptatif sont propres à certains portables MSI.",
   "No. Coreboard detects what your PC actually exposes (Intel, AMD or ARM CPU, NVIDIA, AMD or Intel GPU, display, battery) and hides what isn't there. Only keyboard lighting and adaptive cooling are specific to some MSI laptops.", every=True)
tr("Oui, Coreboard est un logiciel libre et gratuit sous licence GPL-3.0. Le code source est sur", "Yes, Coreboard is free and open-source software under the GPL-3.0 license. The source code is on", every=True)

# --- navigation
tr('aria-label="Coreboard, retour en haut"', 'aria-label="Coreboard, back to top"')
tr('aria-label="Navigation principale"', 'aria-label="Main navigation"')
tr('<a href="#fonctionnalites">Fonctionnalités</a>', '<a href="#fonctionnalites">Features</a>')
tr('<a href="#captures">Captures</a>', '<a href="#captures">Screenshots</a>')
tr('<a href="#compatibilite">Compatibilité</a>', '<a href="#compatibilite">Compatibility</a>')
tr('aria-label="Code source sur GitHub"', 'aria-label="Source code on GitHub"')
tr('<a class="lang" href="en/" hreflang="en" lang="en">EN</a>', '<a class="lang" href="../" hreflang="fr" lang="fr">FR</a>')
tr('href="#installation">Installer</a>', 'href="#installation">Install</a>')

# --- héros
tr('Jeux FitGirl installés automatiquement, sans aucune fenêtre', 'FitGirl games installed automatically, with no windows at all')
tr('<span class="sr-only">Coreboard : </span>Le centre de contrôle matériel natif pour Linux', '<span class="sr-only">Coreboard: </span>The native hardware control center for Linux')
tr("Performances, processeur, GPU, écran, éclairage RGB, scénarios et jeux&nbsp;: tout ton PC dans une seule application rapide, écrite en C et GTK4.",
   "Performance, CPU, GPU, display, RGB lighting, scenarios and games: your whole PC in one fast app, written in C and GTK4.")
tr("Installer Coreboard", "Install Coreboard")
tr("Voir l'interface", "See the interface")
tr("Gratuit · Open source (GPL-3.0)", "Free · Open source (GPL-3.0)")

# --- pourquoi
tr(">Pourquoi Coreboard<", ">Why Coreboard<")
tr("L'équivalent d'Armoury Crate, pensé pour Linux", "The Armoury Crate equivalent, built for Linux")
tr("Sous Windows, les constructeurs fournissent un panneau pour tout régler. Sous Linux, il fallait jongler entre dix outils en ligne de commande. Coreboard les réunit dans une interface sombre et soignée.",
   "On Windows, manufacturers ship one panel to tune everything. On Linux, you had to juggle a dozen command-line tools. Coreboard brings them together in a polished dark interface.")
tr("<b>Natif et léger</b> : écrit en C avec GTK4, démarre instantanément, aucun moteur web embarqué.", "<b>Native and lightweight</b>: written in C with GTK4, starts instantly, no embedded web engine.")
tr("<b>Il s'adapte à ton PC</b> : il détecte ce que ton matériel expose vraiment et masque le reste.", "<b>It adapts to your PC</b>: it detects what your hardware really exposes and hides the rest.")
tr("<b>Sans compte ni télémétrie</b> : tout reste sur ta machine.", "<b>No account, no telemetry</b>: everything stays on your machine.")
tr('alt="Accueil de Coreboard : photo de l\'appareil, utilisation du GPU et du CPU, ventilateurs, mémoire et modes de fonctionnement Silencieux, Équilibré, Turbo"',
   'alt="Coreboard home: device picture, GPU and CPU usage, fans, memory and Silent, Balanced, Turbo performance modes"')

# --- fonctionnalités
tr('<span class="eyebrow">Fonctionnalités</span>', '<span class="eyebrow">Features</span>')
tr("Tout ton matériel, au même endroit", "All your hardware, in one place")
tr("Chaque réglage passe par les interfaces standard de Linux&nbsp;: power-profiles-daemon, sysfs, D-Bus, polkit.", "Every setting goes through standard Linux interfaces: power-profiles-daemon, sysfs, D-Bus, polkit.")
tr("<h3>Modes de fonctionnement</h3>", "<h3>Performance modes</h3>")
tr("Silencieux, Équilibré, Turbo&nbsp;: un clic bascule le profil d'alimentation du système.", "Silent, Balanced, Turbo: one click switches the system power profile.")
tr("<h3>Processeur et GPU</h3>", "<h3>CPU and GPU</h3>")
tr("Turbo Boost, préférence énergétique, gouverneur, températures, fréquences et mode GPU hybride.", "Turbo Boost, energy preference, governor, temperatures, frequencies and hybrid GPU mode.")
tr("<h3>Écran</h3>", "<h3>Display</h3>")
tr("Fréquence de rafraîchissement, luminosité et filtre lumière bleue, sous Hyprland, Sway, KDE, GNOME ou X11.", "Refresh rate, brightness and night light, on Hyprland, Sway, KDE, GNOME or X11.")
tr("<h3>Éclairage RGB</h3>", "<h3>RGB lighting</h3>")
tr("Effets par zone, couleurs personnalisées et mode Musique qui fait réagir le clavier au son.", "Per-zone effects, custom colors and a Music mode that makes the keyboard react to sound.")
tr("<h3>Scénarios</h3>", "<h3>Scenarios</h3>")
tr("Quand un jeu prend le focus, Coreboard applique le mode et la fréquence voulus, puis restaure les réglages.", "When a game gets focus, Coreboard applies the mode and refresh rate you chose, then restores your settings.")
tr("Enregistre des séquences de touches, rejoue-les avec répétitions et assigne-leur un raccourci global.", "Record key sequences, replay them with repeats and bind them to a global shortcut.")
tr("<h3>Bibliothèque de jeux</h3>", "<h3>Game library</h3>")
tr("Steam, Epic (Legendary), GOG, Lutris et applications de jeu, réunis et lançables d'un clic.", "Steam, Epic (Legendary), GOG, Lutris and game apps, gathered and launched in one click.")
tr("<h3>Jeux Windows</h3>", "<h3>Windows games</h3>")
tr("Proton-GE et umu-launcher, DLSS, ray tracing, GameMode, MangoHud et un diagnostic qui optimise le système.", "Proton-GE and umu-launcher, DLSS, ray tracing, GameMode, MangoHud and a check-up that tunes your system.")
tr("Recherche, téléchargement segmenté reprenable, extraction et installation dans Wine. <a href=\"#fitgirl\">En savoir plus</a>", "Search, resumable multi-connection download, extraction and installation in Wine. <a href=\"#fitgirl\">Learn more</a>")

# --- captures
tr('<span class="eyebrow">Captures</span>', '<span class="eyebrow">Screenshots</span>')
tr("Une interface sombre, nette et lisible", "A dark, crisp and readable interface")
tr("Barre latérale, en-têtes rouges, cartes de mesures en direct&nbsp;: l'ergonomie des centres de contrôle gaming, sans leur lourdeur.", "Sidebar, red headers, live metric cards: the feel of gaming control centers, without the bloat.")
tr('aria-label="Pages de Coreboard"', 'aria-label="Coreboard pages"')
tr('data-cap="Performances : profil, Turbo Boost, préférence énergétique, ventilateurs et températures."', 'data-cap="Performance: profile, Turbo Boost, energy preference, fans and temperatures."')
tr('>Performances</button>', '>Performance</button>')
tr('data-cap="Éclairage : aperçu du clavier par zone, effets, couleur et luminosité."', 'data-cap="Lighting: per-zone keyboard preview, effects, color and brightness."')
tr('>Éclairage</button>', '>Lighting</button>')
tr('data-cap="Écran et GPU : mode GPU hybride, luminosité et filtre lumière bleue."', 'data-cap="Display and GPU: hybrid GPU mode, brightness and night light."')
tr('>Écran &amp; GPU</button>', '>Display &amp; GPU</button>')
tr('data-cap="Jeux Windows : environnement Proton prêt et diagnostic d\'optimisation du système."', 'data-cap="Windows games: Proton environment ready and system optimization check-up."')
tr('>Jeux Windows</button>', '>Windows games</button>')
tr('alt="Page Performances de Coreboard : profil, Turbo Boost, préférence énergétique, ventilateurs et températures"', 'alt="Coreboard Performance page: profile, Turbo Boost, energy preference, fans and temperatures"')
tr('>Performances : profil, Turbo Boost, préférence énergétique, ventilateurs et températures.</figcaption>', '>Performance: profile, Turbo Boost, energy preference, fans and temperatures.</figcaption>')

# --- FitGirl
tr("Du repack au jeu installé, sans quitter Coreboard", "From repack to installed game, without leaving Coreboard")
tr("Recherche un repack FitGirl, télécharge-le et installe-le dans Wine en quelques clics. Plus besoin de gestionnaire de téléchargement ni de manipulations dans le terminal.",
   "Search for a FitGirl repack, download it and install it in Wine in a few clicks. No download manager, no terminal juggling.")
tr("<b>Rechercher</b><span>Les résultats du catalogue FitGirl s'affichent directement dans l'application.", "<b>Search</b><span>Results from the FitGirl catalog show up right inside the app.")
tr("<b>Télécharger</b><span>Huit connexions par fichier, reprise exacte après une coupure, une pause ou un redémarrage. Les fichiers optionnels (voix, bonus) sont ignorés.",
   "<b>Download</b><span>Eight connections per file, exact resume after a dropout, a pause or a reboot. Optional files (voices, bonus) are skipped.")
tr("<b>Extraire et installer</b><span>7-Zip extrait les archives, puis l'installation se fait toute seule, sans aucune fenêtre, dans Wine (Proton-GE) avec un préfixe propre au jeu. La progression s'affiche dans Coreboard.",
   "<b>Extract and install</b><span>7-Zip extracts the archives, then installation runs on its own, with no windows at all, in Wine (Proton-GE) with a dedicated prefix. Progress shows in Coreboard.")
tr("<b>Jouer</b><span>Le jeu rejoint « Jeux Windows » avec DLSS, GameMode et MangoHud. Les archives peuvent ensuite être supprimées pour libérer l'espace.",
   "<b>Play</b><span>The game joins “Windows games” with DLSS, GameMode and MangoHud. Archives can then be deleted to free up space.")
tr("Respecte le droit d'auteur de ton pays&nbsp;: télécharge uniquement des jeux que tu as le droit d'utiliser. Coreboard n'est pas affilié à FitGirl Repacks et n'héberge aucun fichier.",
   "Respect copyright law in your country: only download games you have the right to use. Coreboard is not affiliated with FitGirl Repacks and hosts no files.")
tr('alt="Page FitGirl Repacks de Coreboard : recherche, jeu sélectionné avec le nombre de parties, bouton Télécharger et choix du dossier"',
   'alt="Coreboard FitGirl Repacks page: search, selected game with its number of parts, Download button and folder choice"')

# --- compatibilité
tr('<span class="eyebrow">Compatibilité</span>', '<span class="eyebrow">Compatibility</span>')
tr("Toutes les distributions, tous les bureaux", "Every distribution, every desktop")
tr("Coreboard choisit lui-même le bon outil selon ton système, et installe ce qui manque avec le gestionnaire de paquets de ta distribution.",
   "Coreboard picks the right tool for your system on its own, and installs what's missing with your distribution's package manager.")
tr('aria-label="Distributions prises en charge"', 'aria-label="Supported distributions"')
tr("Fonctions disponibles selon le bureau", "Features available per desktop")
tr('<th scope="col">Fonction</th>', '<th scope="col">Feature</th>')
tr(">Oui<", ">Yes<", every=True)
tr(">Natif<", ">Native<", every=True)
tr(">Jeux XWayland<", ">XWayland games<", every=True)
tr("Matériel, profils, CPU, GPU, audio", "Hardware, profiles, CPU, GPU, audio")
tr("Fréquence d'écran", "Refresh rate")
tr("Filtre lumière bleue", "Night light")
tr("Scénarios par application", "Per-app scenarios")
tr("Macros (rejeu)", "Macros (playback)")
tr("GameVisual (filtre d'écran)", "GameVisual (screen filter)")
tr("Processeurs Intel, AMD et ARM · GPU NVIDIA, AMD et Intel · GTK 4.10 ou plus récent requis.", "Intel, AMD and ARM CPUs · NVIDIA, AMD and Intel GPUs · GTK 4.10 or newer required.")

# --- installation
tr("Installe Coreboard en deux minutes", "Install Coreboard in two minutes")
tr("Choisis ta distribution, copie les commandes dans un terminal.", "Pick your distribution and paste the commands into a terminal.")
tr(">Arch &amp; dérivées<", ">Arch &amp; derivatives<")
tr(">Copier<", ">Copy<", every=True)
tr("# paquet natif : démon de refroidissement et règle udev inclus", "# native package: cooling daemon and udev rule included")
tr("Ensuite, lance <code>coreboard</code> depuis le menu des applications. Un raccourci clavier sur <code>coreboard --toggle</code> ouvre et ferme la fenêtre.",
   "Then launch <code>coreboard</code> from your app menu. A keyboard shortcut on <code>coreboard --toggle</code> opens and closes the window.")

# --- FAQ, pied de page, scripts
tr("Questions fréquentes", "Frequently asked questions")
tr("© 2026 Coreboard · Logiciel libre sous licence GPL-3.0<br>Coreboard n'est affilié ni à ASUS, ni à MSI, ni à Omarchy.",
   "© 2026 Coreboard · Free software under the GPL-3.0 license<br>Coreboard is affiliated with neither ASUS, MSI nor Omarchy.")
tr('aria-label="Liens du pied de page"', 'aria-label="Footer links"')
tr(">Versions<", ">Releases<")
tr(">Signaler un problème<", ">Report an issue<")
tr(">Licence<", ">License<")
tr("img.alt = 'Page ' + t.textContent + ' de Coreboard : ' + t.dataset.cap;", "img.alt = 'Coreboard ' + t.textContent + ' page: ' + t.dataset.cap;")
tr("b.textContent = 'Copié'", "b.textContent = 'Copied'")
tr("b.textContent = 'Échec'", "b.textContent = 'Failed'")
tr("b.textContent = 'Copier'", "b.textContent = 'Copy'")

# ponctuation française restante (espace insécable avant « : » et « ? ») → ponctuation anglaise
s = s.replace("&nbsp;:", ":").replace("&nbsp;?", "?").replace("on Windows : ", "on Windows: ").replace("Features adapt : ", "Features adapt: ")

open(OUT, "w", encoding="utf-8").write(s)
