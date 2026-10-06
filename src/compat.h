/* Coreboard — compatibilité multi-distributions et multi-bureaux :
 * gestionnaire de paquets, outils installés dans le dossier utilisateur, fenêtres ouvertes, saisie clavier simulée. */
#ifndef COMPAT_H
#define COMPAT_H
#include <glib.h>

typedef enum { PM_NONE, PM_PACMAN, PM_APT, PM_DNF, PM_ZYPPER } PkgMgr;
typedef enum { WB_NONE, WB_HYPRLAND, WB_SWAY, WB_X11 } WinBackend;
typedef enum { IB_NONE, IB_WTYPE, IB_XDOTOOL, IB_YDOTOOL } InputBackend;

gboolean compat_has(const char *prog);         /* programme présent dans le PATH (sans fuite mémoire) */
void compat_init(void);                         /* ~/.local/bin ajouté au PATH (outils installés sans root) */
const char *compat_distro_name(void);           /* PRETTY_NAME de /etc/os-release */
PkgMgr compat_pkgmgr(void);
const char *compat_pkgmgr_name(void);           /* "pacman", "apt"… ou "" */
gboolean compat_is_desktop(const char *name);   /* XDG_CURRENT_DESKTOP contient name (sans casse) */

/* Paquets « logiques » (umu-launcher, gamemode, mangohud, vulkan32, lib32-gcc, lgogdownloader, xdotool…) traduits
   pour la distribution. Installation en une seule demande de mot de passe, paquet par paquet : un paquet absent des
   dépôts n'empêche pas les autres. Renvoie FALSE si aucun gestionnaire n'est connu ou si l'élévation a été refusée. */
gboolean compat_pkg_available(const char *logical);
gboolean compat_install_pkgs(const char *const *logical);

/* Outils téléchargés dans le dossier utilisateur quand la distribution ne les fournit pas (sans root) :
   "umu-run" (zipapp officiel), "legendary" (binaire officiel), "steamcmd" (archive Valve). */
gboolean compat_install_user_tool(const char *tool);

/* Fenêtres ouvertes : Hyprland, Sway, ou X11 (aussi les jeux XWayland sous GNOME/KDE Wayland) */
typedef struct { char cls[64], title[80]; int pid; } CWin;
WinBackend compat_win_backend(void);
const char *compat_win_backend_name(void);
int compat_windows(CWin *out, int max);
gchar *compat_active_class(void);               /* classe de la fenêtre active, ou NULL */

/* Saisie clavier simulée (macros) : wtype (wlroots), xdotool (X11 / XWayland), ydotool (partout, démon requis) */
InputBackend compat_input(void);
const char *compat_input_name(void);
gboolean compat_type_text(const char *s);
gboolean compat_key_combo(const char *combo);   /* « ctrl+shift+Return » (noms de touches GDK/X) */
#endif
