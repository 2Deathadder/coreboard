/* Coreboard — compatibilité multi-distributions et multi-bureaux (voir compat.h) */
#include "compat.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static gchar *run_out(const char *const *argv) {
    gchar *out = NULL; gint st = 0;
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &st, NULL) || st) { g_free(out); return NULL; }
    return out;
}
static gboolean run_ok(const char *const *argv) {
    gint st = 1;
    return g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &st, NULL) && st == 0;
}
gboolean compat_has(const char *p) { gchar *f = g_find_program_in_path(p); gboolean ok = f != NULL; g_free(f); return ok; }
#define has compat_has

/* ------------------------------------------------------------------ distribution */
void compat_init(void) {
    /* lancé depuis un menu, le PATH n'inclut pas toujours ~/.local/bin, où vont les outils installés sans root */
    gchar *lb = g_build_filename(g_get_home_dir(), ".local", "bin", NULL);
    const char *path = g_getenv("PATH");
    gchar **parts = g_strsplit(path ? path : "/usr/local/bin:/usr/bin:/bin", ":", -1);
    gboolean found = FALSE; for (int i = 0; parts[i]; i++) found |= !strcmp(parts[i], lb);
    if (!found) { gchar *np = g_strconcat(path ? path : "/usr/local/bin:/usr/bin:/bin", ":", lb, NULL); g_setenv("PATH", np, TRUE); g_free(np); }
    g_strfreev(parts); g_free(lb);
}

static gchar *os_release_field(const char *key) {
    gchar *txt = NULL, *val = NULL;
    const char *force = g_getenv("COREBOARD_OS_RELEASE");          /* tests : simuler une autre distribution */
    if (force ? !g_file_get_contents(force, &txt, NULL, NULL)
              : !g_file_get_contents("/etc/os-release", &txt, NULL, NULL) && !g_file_get_contents("/usr/lib/os-release", &txt, NULL, NULL)) return NULL;
    gchar **ln = g_strsplit(txt, "\n", -1);
    size_t kl = strlen(key);
    for (int i = 0; ln[i] && !val; i++) if (!strncmp(ln[i], key, kl) && ln[i][kl] == '=') {
        gchar *v = g_strstrip(g_strdup(ln[i] + kl + 1));
        size_t l = strlen(v);
        if (l >= 2 && (v[0] == '"' || v[0] == '\'') && v[l - 1] == v[0]) { v[l - 1] = 0; memmove(v, v + 1, l - 1); }
        val = v;
    }
    g_strfreev(ln); g_free(txt);
    return val;
}

const char *compat_distro_name(void) {
    static gchar *name;
    if (!name) { name = os_release_field("PRETTY_NAME"); if (!name) name = g_strdup("Linux"); }
    return name;
}

PkgMgr compat_pkgmgr(void) {
    static int pm = -1;
    if (pm >= 0) return pm;
    gchar *id = os_release_field("ID"), *like = os_release_field("ID_LIKE");
    gchar *all = g_strdup_printf(" %s %s ", id ? id : "", like ? like : "");
    struct { const char *ids, *bin; PkgMgr m; } T[] = {
        {" arch manjaro endeavouros cachyos garuda omarchy ", "pacman", PM_PACMAN},
        {" debian ubuntu linuxmint pop elementary zorin kali ", "apt-get", PM_APT},
        {" fedora rhel centos rocky almalinux nobara ultramarine ", "dnf", PM_DNF},
        {" opensuse suse opensuse-tumbleweed opensuse-leap sles ", "zypper", PM_ZYPPER},
    };
    pm = PM_NONE;
    for (guint i = 0; i < G_N_ELEMENTS(T) && pm == PM_NONE; i++) {      /* ID / ID_LIKE d'abord… */
        gchar **w = g_strsplit(g_strstrip(g_strdup(all)), " ", -1);
        for (int k = 0; w[k] && pm == PM_NONE; k++) if (*w[k]) { gchar *pat = g_strdup_printf(" %s ", w[k]); if (strstr(T[i].ids, pat) && has(T[i].bin)) pm = T[i].m; g_free(pat); }
        g_strfreev(w);
    }
    for (guint i = 0; i < G_N_ELEMENTS(T) && pm == PM_NONE; i++) if (has(T[i].bin)) pm = T[i].m;   /* …puis l'outil présent */
    g_free(all); g_free(id); g_free(like);
    return pm;
}

const char *compat_pkgmgr_name(void) {
    static const char *N[] = {"", "pacman", "apt", "dnf", "zypper"};
    return N[compat_pkgmgr()];
}

gboolean compat_is_desktop(const char *name) {
    const char *d = g_getenv("XDG_CURRENT_DESKTOP");
    if (!d) return FALSE;
    gchar *a = g_ascii_strdown(d, -1), *b = g_ascii_strdown(name, -1);
    gboolean r = strstr(a, b) != NULL;
    g_free(a); g_free(b);
    return r;
}

/* ------------------------------------------------------------------ paquets */
/* nom logique → noms par gestionnaire (pacman, apt, dnf, zypper) ; NULL = absent des dépôts officiels */
static const struct { const char *logical, *pm[4]; } PKGS[] = {
    {"umu-launcher",   {"umu-launcher", NULL, "umu-launcher", NULL}},
    {"gamemode",       {"gamemode", "gamemode", "gamemode", "gamemode"}},
    {"gamemode32",     {"lib32-gamemode", NULL, "gamemode.i686", NULL}},
    {"mangohud",       {"mangohud", "mangohud", "mangohud", "mangohud"}},
    {"mangohud32",     {"lib32-mangohud", NULL, "mangohud.i686", "mangohud-32bit"}},
    {"vulkan32",       {"lib32-vulkan-icd-loader", "libvulkan1:i386", "vulkan-loader.i686", "libvulkan1-32bit"}},
    {"lib32-gcc",      {"lib32-glibc lib32-gcc-libs", "lib32gcc-s1 lib32stdc++6", "glibc.i686 libstdc++.i686", "glibc-32bit libgcc_s1-32bit libstdc++6-32bit"}},
    {"lgogdownloader", {NULL, "lgogdownloader", "lgogdownloader", NULL}},
    {"brightnessctl",  {"brightnessctl", "brightnessctl", "brightnessctl", "brightnessctl"}},
    {"xdotool",        {"xdotool", "xdotool", "xdotool", "xdotool"}},
    {"xprop",          {"xorg-xprop", "x11-utils", "xprop", "xprop"}},
    {"wtype",          {"wtype", "wtype", "wtype", "wtype"}},
    {"ydotool",        {"ydotool", "ydotool", "ydotool", "ydotool"}},
    {"7zip",           {"7zip", "7zip", "p7zip p7zip-plugins", "7zip"}},
};

static const char *pkg_names(const char *logical) {
    PkgMgr pm = compat_pkgmgr();
    if (pm == PM_NONE) return NULL;
    for (guint i = 0; i < G_N_ELEMENTS(PKGS); i++) if (!strcmp(PKGS[i].logical, logical)) return PKGS[i].pm[pm - 1];
    return NULL;
}

gboolean compat_pkg_available(const char *logical) { return pkg_names(logical) != NULL; }

gboolean compat_install_pkgs(const char *const *logical) {
    PkgMgr pm = compat_pkgmgr();
    if (pm == PM_NONE || !has("pkexec")) return FALSE;
    static const char *CMD[] = {"", "pacman -S --needed --noconfirm", "DEBIAN_FRONTEND=noninteractive apt-get install -y", "dnf install -y", "zypper --non-interactive install"};
    GString *sc = g_string_new("ok=0\n");
    if (pm == PM_APT) g_string_append(sc, "apt-get update -qq || true\n");
    int n = 0;
    for (int i = 0; logical[i]; i++) {
        const char *names = pkg_names(logical[i]);
        if (!names) continue;
        gchar **w = g_strsplit(names, " ", -1);
        for (int k = 0; w[k]; k++) if (*w[k]) {
            gchar *q = g_shell_quote(w[k]);
            g_string_append_printf(sc, "%s %s >/dev/null 2>&1 && ok=1\n", CMD[pm], q);   /* un échec n'arrête pas les suivants */
            g_free(q); n++;
        }
        g_strfreev(w);
    }
    g_string_append(sc, "[ $ok = 1 ]\n");
    gboolean r = FALSE;
    if (n) { const char *argv[] = {"pkexec", "sh", "-c", sc->str, NULL}; r = run_ok(argv); }
    g_string_free(sc, TRUE);
    return r;
}

/* ------------------------------------------------------------------ outils dans le dossier utilisateur */
static gchar *github_asset(const char *repo, const char *suffix) {
    gchar *api = g_strdup_printf("https://api.github.com/repos/%s/releases/latest", repo);
    const char *argv[] = {"curl", "-sL", "--max-time", "30", api, NULL};
    gchar *out = run_out(argv), *url = NULL;
    g_free(api);
    if (!out) return NULL;
    JsonParser *jp = json_parser_new();
    if (json_parser_load_from_data(jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(jp));
        JsonArray *as = json_object_has_member(o, "assets") ? json_object_get_array_member(o, "assets") : NULL;
        for (guint i = 0; as && i < json_array_get_length(as) && !url; i++) {
            JsonObject *a = json_array_get_object_element(as, i);
            const char *n = json_object_get_string_member_with_default(a, "name", "");
            if (g_str_has_suffix(n, suffix)) url = g_strdup(json_object_get_string_member_with_default(a, "browser_download_url", ""));
        }
    }
    g_object_unref(jp); g_free(out);
    return url && *url ? url : (g_free(url), NULL);
}

static gboolean download(const char *url, const char *dest) {
    gchar *tmp = g_strconcat(dest, ".tmp", NULL);
    const char *argv[] = {"curl", "-sSfL", "--retry", "3", "--max-time", "600", "-o", tmp, url, NULL};
    gboolean ok = run_ok(argv) && g_rename(tmp, dest) == 0;
    if (!ok) g_unlink(tmp);
    g_free(tmp);
    return ok;
}

gboolean compat_install_user_tool(const char *tool) {
    if (!has("curl")) return FALSE;
    gchar *bin = g_build_filename(g_get_home_dir(), ".local", "bin", NULL), *share = g_build_filename(g_get_user_data_dir(), "coreboard", "tools", NULL);
    g_mkdir_with_parents(bin, 0755); g_mkdir_with_parents(share, 0755);
    gboolean ok = FALSE;
    if (!strcmp(tool, "legendary")) {
#if defined(__aarch64__)
        gchar *url = github_asset("derrod/legendary", "_linux_arm64");
#else
        gchar *url = github_asset("derrod/legendary", "_linux_x64");
#endif
        gchar *dst = g_build_filename(bin, "legendary", NULL);
        ok = url && download(url, dst) && g_chmod(dst, 0755) == 0;
        g_free(dst); g_free(url);
    } else if (!strcmp(tool, "umu-run")) {
        gchar *url = github_asset("Open-Wine-Components/umu-launcher", "-zipapp.tar"), *tar = g_build_filename(share, "umu.tar", NULL);
        if (url && download(url, tar)) {
            const char *x[] = {"tar", "-xf", tar, "-C", share, NULL};
            gchar *exe = g_build_filename(share, "umu", "umu-run", NULL), *link = g_build_filename(bin, "umu-run", NULL);
            if (run_ok(x) && g_file_test(exe, G_FILE_TEST_IS_REGULAR)) { g_unlink(link); ok = symlink(exe, link) == 0; }
            g_free(exe); g_free(link);
        }
        g_unlink(tar); g_free(tar); g_free(url);
    } else if (!strcmp(tool, "steamcmd")) {
        const char *libs[] = {"lib32-gcc", NULL};
        compat_install_pkgs(libs);                                   /* steamcmd est un programme 32 bits */
        gchar *dir = g_build_filename(share, "steamcmd", NULL), *tar = g_build_filename(share, "steamcmd.tar.gz", NULL);
        g_mkdir_with_parents(dir, 0755);
        if (download("https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz", tar)) {
            const char *x[] = {"tar", "-xzf", tar, "-C", dir, NULL};
            gchar *sh = g_build_filename(dir, "steamcmd.sh", NULL), *wrap = g_build_filename(bin, "steamcmd", NULL);
            if (run_ok(x) && g_file_test(sh, G_FILE_TEST_IS_REGULAR)) {
                gchar *q = g_shell_quote(sh), *body = g_strdup_printf("#!/bin/sh\nexec %s \"$@\"\n", q);
                ok = g_file_set_contents(wrap, body, -1, NULL) && g_chmod(wrap, 0755) == 0;
                g_free(q); g_free(body);
            }
            g_free(sh); g_free(wrap);
        }
        g_unlink(tar); g_free(tar); g_free(dir);
    }
    g_free(bin); g_free(share);
    return ok;
}

/* ------------------------------------------------------------------ fenêtres */
WinBackend compat_win_backend(void) {
    static int wb = -1;
    if (wb >= 0) return wb;
    if (g_getenv("HYPRLAND_INSTANCE_SIGNATURE") && has("hyprctl")) wb = WB_HYPRLAND;
    else if (g_getenv("SWAYSOCK") && has("swaymsg")) wb = WB_SWAY;
    else if (g_getenv("DISPLAY") && has("xprop")) wb = WB_X11;          /* X11, ou XWayland sous GNOME/KDE (jeux Proton inclus) */
    else wb = WB_NONE;
    return wb;
}

const char *compat_win_backend_name(void) {
    static const char *N[] = {"", "Hyprland", "Sway", "X11"};
    return N[compat_win_backend()];
}

static const char *jstr(JsonObject *o, const char *k) {
    return o && json_object_has_member(o, k) && JSON_NODE_HOLDS_VALUE(json_object_get_member(o, k)) ? json_object_get_string_member(o, k) : "";
}

static gboolean win_add(CWin *out, int *n, int max, const char *cls, const char *title, int pid) {
    if (!cls || !*cls || !strcmp(cls, "dev.coreboard.Coreboard") || *n >= max) return FALSE;
    for (int j = 0; j < *n; j++) if (!strcmp(out[j].cls, cls)) return FALSE;
    g_strlcpy(out[*n].cls, cls, sizeof out[*n].cls); g_strlcpy(out[*n].title, title ? title : "", sizeof out[*n].title); out[*n].pid = pid;
    (*n)++;
    return TRUE;
}

/* sway : arbre des conteneurs ; une fenêtre est une feuille avec un pid */
static void sway_walk(JsonObject *o, CWin *out, int *n, int max, gchar **active) {
    if (!o) return;
    if (json_object_has_member(o, "pid") && json_object_get_int_member(o, "pid") > 0) {
        const char *cls = jstr(o, "app_id");
        if (!*cls && json_object_has_member(o, "window_properties")) cls = jstr(json_object_get_object_member(o, "window_properties"), "class");
        if (out) win_add(out, n, max, cls, jstr(o, "name"), (int)json_object_get_int_member(o, "pid"));
        if (active && json_object_has_member(o, "focused") && json_object_get_boolean_member(o, "focused") && *cls) { g_free(*active); *active = g_strdup(cls); }
    }
    const char *kids[] = {"nodes", "floating_nodes"};
    for (int k = 0; k < 2; k++) if (json_object_has_member(o, kids[k])) {
        JsonArray *a = json_object_get_array_member(o, kids[k]);
        for (guint i = 0; i < json_array_get_length(a); i++) sway_walk(json_array_get_object_element(a, i), out, n, max, active);
    }
}

static gchar *sway_tree(CWin *out, int *n, int max) {
    const char *argv[] = {"swaymsg", "-t", "get_tree", "-r", NULL};
    gchar *o = run_out(argv), *active = NULL;
    if (!o) return NULL;
    JsonParser *jp = json_parser_new();
    if (json_parser_load_from_data(jp, o, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp)))
        sway_walk(json_node_get_object(json_parser_get_root(jp)), out, n, max, &active);
    g_object_unref(jp); g_free(o);
    return active;
}

/* X11 : propriétés EWMH lues avec xprop */
static gchar *x11_prop(const char *win, const char *prop) {
    const char *argv[] = {"xprop", win ? "-id" : "-root", win ? win : prop, win ? prop : NULL, NULL};
    gchar *o = run_out(argv);
    if (!o) return NULL;
    char *eq = strstr(o, " = "); if (!eq) eq = strstr(o, ": ");
    gchar *v = eq ? g_strstrip(g_strdup(eq + (eq[0] == ' ' ? 3 : 2))) : NULL;
    g_free(o);
    return v;
}
static gchar *x11_class(const char *win) {                   /* WM_CLASS(STRING) = "instance", "Classe" → Classe */
    gchar *v = x11_prop(win, "WM_CLASS"), *cls = NULL;
    if (!v) return NULL;
    char *last = strrchr(v, '"'); if (last) *last = 0;
    char *st = last ? strrchr(v, '"') : NULL;
    if (st) cls = g_strdup(st + 1);
    g_free(v);
    return cls;
}
static gchar *x11_title(const char *win) {
    gchar *v = x11_prop(win, "_NET_WM_NAME");
    if (!v || *v != '"') { g_free(v); v = x11_prop(win, "WM_NAME"); }
    if (v && *v == '"') { size_t l = strlen(v); if (l >= 2) { v[l - 1] = 0; memmove(v, v + 1, l - 1); } }
    return v;
}
static gchar *x11_active_id(void) {                         /* « window id # 0x2a00007 » */
    gchar *v = x11_prop(NULL, "_NET_ACTIVE_WINDOW"), *id = NULL;
    if (v) { char *h = strstr(v, "0x"); if (h && strcmp(h, "0x0")) { id = g_strdup(h); char *c = strpbrk(id, ", "); if (c) *c = 0; } }
    g_free(v);
    return id;
}

int compat_windows(CWin *out, int max) {
    int n = 0;
    switch (compat_win_backend()) {
    case WB_HYPRLAND: {
        const char *argv[] = {"hyprctl", "clients", "-j", NULL};
        gchar *o = run_out(argv); if (!o) break;
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, o, -1, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(jp))) {
            JsonArray *a = json_node_get_array(json_parser_get_root(jp));
            for (guint i = 0; i < json_array_get_length(a); i++) {
                JsonObject *w = json_array_get_object_element(a, i);
                win_add(out, &n, max, jstr(w, "class"), jstr(w, "title"), (int)json_object_get_int_member_with_default(w, "pid", 0));
            }
        }
        g_object_unref(jp); g_free(o);
        break;
    }
    case WB_SWAY: g_free(sway_tree(out, &n, max)); break;
    case WB_X11: {
        gchar *v = x11_prop(NULL, "_NET_CLIENT_LIST");     /* « window id # 0x…, 0x… » */
        if (!v) break;
        gchar **ids = g_strsplit_set(v, ", ", -1);
        for (int i = 0; ids[i] && n < max; i++) if (g_str_has_prefix(ids[i], "0x")) {
            gchar *cls = x11_class(ids[i]);
            if (cls) {
                gchar *t = x11_title(ids[i]), *pv = x11_prop(ids[i], "_NET_WM_PID");
                win_add(out, &n, max, cls, t, pv ? atoi(pv) : 0);
                g_free(t); g_free(pv); g_free(cls);
            }
        }
        g_strfreev(ids); g_free(v);
        break;
    }
    default: break;
    }
    return n;
}

gchar *compat_active_class(void) {
    switch (compat_win_backend()) {
    case WB_HYPRLAND: {
        const char *argv[] = {"hyprctl", "activewindow", "-j", NULL};
        gchar *o = run_out(argv), *cls = NULL;
        if (!o) return NULL;
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, o, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp)))
            cls = g_strdup(jstr(json_node_get_object(json_parser_get_root(jp)), "class"));
        g_object_unref(jp); g_free(o);
        return cls;
    }
    case WB_SWAY: { int n = 0; return sway_tree(NULL, &n, 0); }
    case WB_X11: { gchar *id = x11_active_id(), *cls = id ? x11_class(id) : NULL; g_free(id); return cls; }
    default: return NULL;
    }
}

/* ------------------------------------------------------------------ saisie clavier simulée */
InputBackend compat_input(void) {
    gboolean wl = g_getenv("WAYLAND_DISPLAY") != NULL;
    if (wl && has("wtype") && (compat_win_backend() == WB_HYPRLAND || compat_win_backend() == WB_SWAY || !compat_is_desktop("gnome"))) return IB_WTYPE;
    if (!wl && g_getenv("DISPLAY") && has("xdotool")) return IB_XDOTOOL;
    if (has("ydotool")) return IB_YDOTOOL;
    if (g_getenv("DISPLAY") && has("xdotool")) return IB_XDOTOOL;     /* XWayland : seulement vers les fenêtres X11 */
    return IB_NONE;
}

const char *compat_input_name(void) {
    static const char *N[] = {"", "wtype", "xdotool", "ydotool"};
    return N[compat_input()];
}

gboolean compat_type_text(const char *s) {
    switch (compat_input()) {
    case IB_WTYPE: { const char *a[] = {"wtype", "--", s, NULL}; return run_ok(a); }
    case IB_XDOTOOL: { const char *a[] = {"xdotool", "type", "--clearmodifiers", "--", s, NULL}; return run_ok(a); }
    case IB_YDOTOOL: { const char *a[] = {"ydotool", "type", "--", s, NULL}; return run_ok(a); }
    default: return FALSE;
    }
}

/* codes de touches Linux (input-event-codes.h) pour ydotool */
static int linux_keycode(const char *k) {
    static const struct { const char *n; int c; } T[] = {
        {"ctrl", 29}, {"control", 29}, {"shift", 42}, {"alt", 56}, {"super", 125}, {"logo", 125}, {"meta", 125},
        {"Return", 28}, {"Escape", 1}, {"Tab", 15}, {"space", 57}, {"BackSpace", 14}, {"Delete", 111}, {"Insert", 110},
        {"Home", 102}, {"End", 107}, {"Page_Up", 104}, {"Prior", 104}, {"Page_Down", 109}, {"Next", 109},
        {"Up", 103}, {"Down", 108}, {"Left", 105}, {"Right", 106}, {"minus", 12}, {"equal", 13}, {"comma", 51}, {"period", 52},
        {"slash", 53}, {"semicolon", 39}, {"apostrophe", 40}, {"bracketleft", 26}, {"bracketright", 27}, {"backslash", 43}, {"grave", 41},
    };
    for (guint i = 0; i < G_N_ELEMENTS(T); i++) if (!g_ascii_strcasecmp(T[i].n, k)) return T[i].c;
    if (strlen(k) == 1) {
        static const char *row1 = "qwertyuiop", *row2 = "asdfghjkl", *row3 = "zxcvbnm";
        char c = g_ascii_tolower(k[0]); const char *p;
        if (c >= '1' && c <= '9') return 2 + (c - '1');
        if (c == '0') return 11;
        if ((p = strchr(row1, c))) return 16 + (int)(p - row1);
        if ((p = strchr(row2, c))) return 30 + (int)(p - row2);
        if ((p = strchr(row3, c))) return 44 + (int)(p - row3);
    }
    if ((k[0] == 'F' || k[0] == 'f') && g_ascii_isdigit(k[1])) { int f = atoi(k + 1); if (f >= 1 && f <= 10) return 58 + f; if (f == 11) return 87; if (f == 12) return 88; }
    return -1;
}

gboolean compat_key_combo(const char *combo) {
    gchar **parts = g_strsplit(combo, "+", -1); int n = g_strv_length(parts);
    gboolean ok = FALSE;
    if (n == 0) { g_strfreev(parts); return FALSE; }
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    switch (compat_input()) {
    case IB_WTYPE:                                       /* wtype -M ctrl -M shift -k Return -m shift -m ctrl */
        g_ptr_array_add(a, g_strdup("wtype"));
        for (int i = 0; i < n - 1; i++) { g_ptr_array_add(a, g_strdup("-M")); g_ptr_array_add(a, g_strdup(parts[i])); }
        g_ptr_array_add(a, g_strdup("-k")); g_ptr_array_add(a, g_strdup(parts[n - 1]));
        for (int i = n - 2; i >= 0; i--) { g_ptr_array_add(a, g_strdup("-m")); g_ptr_array_add(a, g_strdup(parts[i])); }
        break;
    case IB_XDOTOOL:                                     /* xdotool key ctrl+shift+Return */
        g_ptr_array_add(a, g_strdup("xdotool")); g_ptr_array_add(a, g_strdup("key")); g_ptr_array_add(a, g_strdup("--clearmodifiers")); g_ptr_array_add(a, g_strdup(combo));
        break;
    case IB_YDOTOOL: {                                   /* ydotool key 29:1 42:1 28:1 28:0 42:0 29:0 */
        int codes[8]; int m = 0;
        for (int i = 0; i < n && m < 8; i++) { int c = linux_keycode(parts[i]); if (c < 0) { m = -1; break; } codes[m++] = c; }
        if (m <= 0) break;
        g_ptr_array_add(a, g_strdup("ydotool")); g_ptr_array_add(a, g_strdup("key"));
        for (int i = 0; i < m; i++) g_ptr_array_add(a, g_strdup_printf("%d:1", codes[i]));
        for (int i = m - 1; i >= 0; i--) g_ptr_array_add(a, g_strdup_printf("%d:0", codes[i]));
        break;
    }
    default: break;
    }
    if (a->len) { g_ptr_array_add(a, NULL); ok = run_ok((const char *const *)a->pdata); }
    g_ptr_array_free(a, TRUE); g_strfreev(parts);
    return ok;
}
