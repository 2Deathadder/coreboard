/* Coreboard — diagnostic et optimisation du système pour les jeux */
#include "tune.h"
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

int tune_check(TuneItem *out, int max, gboolean *fixable) {
    int n = 0; char b[128]; *fixable = FALSE;

    /* ntsync : synchronisation Windows dans le noyau, la plus rapide pour Wine/Proton */
    if (g_file_test("/dev/ntsync", G_FILE_TEST_EXISTS)) put(out, &n, max, "ntsync (synchronisation)", 0, "actif");
    else if (g_file_test("/lib/modules", G_FILE_TEST_IS_DIR) && g_find_program_in_path("modprobe")) { put(out, &n, max, "ntsync (synchronisation)", 1, "module non chargé"); *fixable = TRUE; }
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
    if (!g_find_program_in_path("gamemoderun")) put(out, &n, max, "GameMode", 2, "non installé");
    else if (user_in_group("gamemode")) put(out, &n, max, "GameMode", 0, "actif (réglage du processeur autorisé)");
    else if (user_listed_in_group("gamemode")) put(out, &n, max, "GameMode", 2, "groupe ajouté : reconnecte ta session");
    else { put(out, &n, max, "GameMode", 1, "ne peut pas régler le processeur (groupe manquant)"); *fixable = TRUE; }

    /* GPU NVIDIA + Vulkan */
    if (g_file_test("/usr/share/vulkan/icd.d/nvidia_icd.json", G_FILE_TEST_EXISTS)) put(out, &n, max, "Vulkan NVIDIA", 0, "pilote détecté");
    else put(out, &n, max, "Vulkan NVIDIA", 2, "pilote NVIDIA absent : GPU intégré utilisé");
    if (g_file_test("/usr/lib32/libvulkan.so.1", G_FILE_TEST_EXISTS)) put(out, &n, max, "Vulkan 32 bits", 0, "présent");
    else put(out, &n, max, "Vulkan 32 bits", 1, "absent (jeux 32 bits)");

    /* alimentation */
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL); const char *f; gboolean bat = FALSE, discharging = FALSE;
    while (d && (f = g_dir_read_name(d))) if (g_str_has_prefix(f, "BAT")) {
        char p[96], s[32]; g_snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", f);
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
        "[ -e /dev/ntsync ] || modprobe ntsync || true\n"
        "echo ntsync > /etc/modules-load.d/coreboard-ntsync.conf\n"
        "printf 'vm.max_map_count = 2147483642\\n' > /etc/sysctl.d/99-coreboard-games.conf\n"
        "sysctl -q -p /etc/sysctl.d/99-coreboard-games.conf\n"
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
