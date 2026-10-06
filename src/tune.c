/* Coreboard — diagnostic et optimisation du système pour les jeux */
#include "tune.h"
#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <grp.h>
#include <unistd.h>

static void put(TuneItem *o, int *n, int max, const char *l, int st, const char *fmt, ...) {
    if (*n >= max) return;
    TuneItem *t = &o[(*n)++]; g_strlcpy(t->label, l, sizeof t->label); t->status = st;
    va_list ap; va_start(ap, fmt); g_vsnprintf(t->value, sizeof t->value, fmt, ap); va_end(ap);
}

static gboolean read_str(const char *path, char *buf, size_t n) {
    gchar *t = NULL;
    if (!g_file_get_contents(path, &t, NULL, NULL)) return FALSE;
    g_strlcpy(buf, g_strstrip(t), n); g_free(t);
    return TRUE;
}

static gboolean user_in_group(const char *grp) {
    struct group *g = getgrnam(grp); if (!g) return FALSE;
    gid_t gs[128]; int n = getgroups(128, gs);
    for (int i = 0; i < n; i++) if (gs[i] == g->gr_gid) return TRUE;
    /* appartenance enregistrée mais session non rechargée : compte comme « en attente » côté appelant */
    const char *me = g_get_user_name();
    for (char **m = g->gr_mem; m && *m; m++) if (!strcmp(*m, me)) return FALSE;
    return FALSE;
}
static gboolean user_listed_in_group(const char *grp) {
    struct group *g = getgrnam(grp); if (!g) return FALSE;
    const char *me = g_get_user_name();
    for (char **m = g->gr_mem; m && *m; m++) if (!strcmp(*m, me)) return TRUE;
    return FALSE;
}

/* bibliothèque ELF 32 bits présente (le dossier varie : /usr/lib32 Arch, /usr/lib/i386-linux-gnu Debian, /usr/lib Fedora) */
static gboolean elf32_exists(const char *const *paths) {
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "rb"); if (!f) continue;
        unsigned char h[5] = {0}; size_t r = fread(h, 1, 5, f); fclose(f);
        if (r == 5 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 1) return TRUE;
    }
    return FALSE;
}

/* pilotes Vulkan installés (fichiers ICD) : NVIDIA, AMD (radeon), Intel… */
static void vulkan_icds(char *out, size_t n) {
    const char *dirs[] = {"/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d", NULL};
    gboolean nv = FALSE, amd = FALSE, intel = FALSE, other = FALSE;
    for (int i = 0; dirs[i]; i++) {
        GDir *d = g_dir_open(dirs[i], 0, NULL); const char *f;
        while (d && (f = g_dir_read_name(d))) {
            if (strstr(f, "nvidia")) nv = TRUE; else if (strstr(f, "radeon") || strstr(f, "amd")) amd = TRUE;
            else if (strstr(f, "intel")) intel = TRUE; else if (g_str_has_suffix(f, ".json") && !strstr(f, "lvp")) other = TRUE;
        }
        if (d) g_dir_close(d);
    }
    out[0] = 0;
    if (nv) g_strlcat(out, "NVIDIA ", n);
    if (amd) g_strlcat(out, "AMD ", n);
    if (intel) g_strlcat(out, "Intel ", n);
    if (!out[0] && other) g_strlcat(out, "autre ", n);
    g_strchomp(out);
}

/* module ntsync fourni par le noyau en cours (Linux 6.14 et plus) */
static gboolean ntsync_module_exists(void) {
    const char *argv[] = {"modinfo", "-n", "ntsync", NULL}; gint st = 1;
    return g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &st, NULL) && st == 0;
}

int tune_check(TuneItem *out, int max, gboolean *fixable) {
    int n = 0; char b[128]; *fixable = FALSE;

    /* ntsync : synchronisation Windows dans le noyau, la plus rapide pour Wine/Proton */
    if (g_file_test("/dev/ntsync", G_FILE_TEST_EXISTS)) put(out, &n, max, "ntsync (synchronisation)", 0, "actif");
    else if (compat_has("modprobe") && ntsync_module_exists()) { put(out, &n, max, "ntsync (synchronisation)", 1, "module non chargé"); *fixable = TRUE; }
    else put(out, &n, max, "ntsync (synchronisation)", 2, "indisponible : esync/fsync utilisés");

    /* vm.max_map_count : les gros jeux (Unreal Engine 5) en demandent beaucoup */
    if (read_str("/proc/sys/vm/max_map_count", b, sizeof b)) {
        long v = atol(b);
        if (v >= 1048576) put(out, &n, max, "Zones mémoire (max_map_count)", 0, "%ld", v);
        else { put(out, &n, max, "Zones mémoire (max_map_count)", 1, "%ld (trop bas pour les gros jeux)", v); *fixable = TRUE; }
    }

    /* descripteurs de fichiers (esync) */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        if (rl.rlim_max >= 524288) put(out, &n, max, "Fichiers ouverts (limite)", 0, "%lu", (unsigned long)rl.rlim_max);
        else put(out, &n, max, "Fichiers ouverts (limite)", 1, "%lu (esync peut échouer)", (unsigned long)rl.rlim_max);
    }

    /* GameMode : passe le processeur en performance pendant le jeu (groupe « gamemode » requis) */
    if (!compat_has("gamemoderun")) put(out, &n, max, "GameMode", 2, "non installé");
    else if (user_in_group("gamemode")) put(out, &n, max, "GameMode", 0, "actif (réglage du processeur autorisé)");
    else if (user_listed_in_group("gamemode")) put(out, &n, max, "GameMode", 2, "groupe ajouté : reconnecte ta session");
    else { put(out, &n, max, "GameMode", 1, "ne peut pas régler le processeur (groupe manquant)"); *fixable = TRUE; }

    /* pilotes Vulkan (DXVK/VKD3D en ont besoin) */
    vulkan_icds(b, sizeof b);
    if (b[0]) put(out, &n, max, "Pilote Vulkan", 0, "%s", b);
    else put(out, &n, max, "Pilote Vulkan", 1, "aucun : installe le pilote Vulkan de ton GPU (mesa-vulkan-drivers, vulkan-radeon, nvidia…)");
    const char *v32[] = {"/usr/lib32/libvulkan.so.1", "/usr/lib/i386-linux-gnu/libvulkan.so.1", "/lib/i386-linux-gnu/libvulkan.so.1", "/usr/lib/libvulkan.so.1", NULL};
    if (elf32_exists(v32)) put(out, &n, max, "Vulkan 32 bits", 0, "présent");
    else put(out, &n, max, "Vulkan 32 bits", 1, "absent (jeux 32 bits)");

    /* alimentation */
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL); const char *f; gboolean bat = FALSE, discharging = FALSE;
    while (d && (f = g_dir_read_name(d))) {
        char p[96], s[32], ty[24] = "", sc[24] = "";             /* batterie du PC (pas souris ni casque), quel que soit son nom */
        g_snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", f); read_str(p, ty, sizeof ty);
        g_snprintf(p, sizeof p, "/sys/class/power_supply/%s/scope", f); read_str(p, sc, sizeof sc);
        if (strcmp(ty, "Battery") || !strcmp(sc, "Device")) continue;
        g_snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", f);
        if (read_str(p, s, sizeof s)) { bat = TRUE; if (!strcmp(s, "Discharging")) discharging = TRUE; }
    }
    if (d) g_dir_close(d);
    if (bat) { if (discharging) put(out, &n, max, "Alimentation", 1, "sur batterie : performances réduites, branche le secteur"); else put(out, &n, max, "Alimentation", 0, "secteur"); }

    /* espace disque pour les préfixes et les caches de shaders */
    struct statvfs sv; gchar *home = g_strdup(g_get_home_dir());
    if (statvfs(home, &sv) == 0) {
        double gb = (double)sv.f_bavail * sv.f_frsize / 1073741824.0;
        put(out, &n, max, "Espace disque libre", gb >= 30 ? 0 : 1, "%.0f Go%s", gb, gb >= 30 ? "" : " (peu pour un gros jeu)");
    }
    g_free(home);
    return n;
}

/* ------------------------------------------------------------------ correction (une seule demande de mot de passe) */
static gboolean fixing; static void (*fix_done)(gboolean, gpointer); static gpointer fix_data;
gboolean tune_fixing(void) { return fixing; }

static gboolean fix_finish(gpointer p) { gboolean ok = GPOINTER_TO_INT(p); fixing = FALSE; if (fix_done) fix_done(ok, fix_data); return G_SOURCE_REMOVE; }
static gpointer fix_thread(gpointer d) {
    (void)d;
    const char *script =
        "set -e\n"
        "if [ -e /dev/ntsync ] || modprobe ntsync 2>/dev/null; then mkdir -p /etc/modules-load.d; echo ntsync > /etc/modules-load.d/coreboard-ntsync.conf; fi\n"
        "mkdir -p /etc/sysctl.d\n"
        "printf 'vm.max_map_count = 2147483642\\n' > /etc/sysctl.d/99-coreboard-games.conf\n"
        "sysctl -q -w vm.max_map_count=2147483642\n"
        "getent group gamemode >/dev/null && usermod -aG gamemode \"$1\" || true\n";
    const char *argv[] = {"pkexec", "sh", "-c", script, "sh", g_get_user_name(), NULL};
    gint st = 1;
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &st, NULL);
    g_idle_add(fix_finish, GINT_TO_POINTER(st == 0));
    return NULL;
}
void tune_fix_async(void (*done)(gboolean, gpointer), gpointer d) {
    if (fixing) return;
    fixing = TRUE; fix_done = done; fix_data = d;
    g_thread_unref(g_thread_new("coreboard-tune", fix_thread, NULL));
}
