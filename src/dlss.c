/* Coreboard — prise en charge DLSS */
#include "dlss.h"
#include "pe.h"
#include <ctype.h>
#include <string.h>

const DlssGpu *dlss_gpu(void) {
    static DlssGpu g; static gboolean done;
    if (done) return &g;
    done = TRUE;
    const char *argv[] = {"nvidia-smi", "--query-gpu=name", "--format=csv,noheader", NULL};
    gchar *out = NULL; gint st = 1;
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &st, NULL) || st || !out) { g_free(out); return &g; }
    g_strlcpy(g.name, g_strstrip(out), sizeof g.name);
    char *nl = strchr(g.name, '\n'); if (nl) *nl = 0;
    g_free(out);
    /* génération d'après le nom : « RTX 4050 » → 40, « RTX 5070 » → 50, « RTX PRO 6000 Blackwell » → 50 */
    const char *r = strstr(g.name, "RTX ");
    if (r) {
        const char *d = r + 4;
        while (*d && !isdigit((unsigned char)*d)) { if (!strncmp(d, "Blackwell", 9)) { g.series = 50; break; } d++; }
        if (!g.series && isdigit((unsigned char)d[0]) && isdigit((unsigned char)d[1]) && isdigit((unsigned char)d[2]) && isdigit((unsigned char)d[3])) g.series = (d[0] - '0') * 10 + (d[1] - '0');
        else if (!g.series && strstr(g.name, "RTX A")) g.series = 30;
        else if (!g.series && isdigit((unsigned char)d[0]) && isdigit((unsigned char)d[1])) g.series = 20;
    }
    if (g.series >= 20) { g.sr = TRUE; g.rr = TRUE; }
    if (g.series >= 40) g.fg = TRUE;                 /* génération d'images (2x) : architecture Ada et suivantes */
    if (g.series >= 50) { g.mfg = TRUE; g.dlss5 = TRUE; }   /* Multi Frame Generation et DLSS 5 : Blackwell (série 50) d'après NVIDIA, série 40 « prévue » */
    return &g;
}

static gboolean find_file(const char *dir, const char *name, int depth, char *out, size_t n, int *budget) {
    GDir *d = g_dir_open(dir, 0, NULL); const char *f; gboolean ok = FALSE;
    while (d && !ok && (f = g_dir_read_name(d)) && --(*budget) > 0) {
        gchar *p = g_build_filename(dir, f, NULL);
        if (!g_ascii_strcasecmp(f, name)) { g_strlcpy(out, p, n); ok = TRUE; }
        else if (depth > 0 && g_file_test(p, G_FILE_TEST_IS_DIR) && !g_file_test(p, G_FILE_TEST_IS_SYMLINK)) ok = find_file(p, name, depth - 1, out, n, budget);
        g_free(p);
    }
    if (d) g_dir_close(d);
    return ok;
}

void dlss_scan_game(const char *exe, DlssGame *o) {
    memset(o, 0, sizeof *o);
    static const char *N[3] = {"nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssd.dll"};
    char *dst[3] = {o->sr, o->fg, o->rr};
    gchar *dir = g_path_get_dirname(exe);
    /* le jeu est souvent 2 à 4 niveaux au-dessus de l'exécutable (Unreal : Jeu/Binaries/Win64/x.exe) */
    for (int up = 0; up < 4; up++) {
        for (int k = 0; k < 3; k++) if (!dst[k][0]) {
            char path[600]; int budget = 30000;
            if (find_file(dir, N[k], up == 0 ? 2 : 7, path, sizeof path, &budget)) { pe_file_version(path, dst[k], 24); if (!dst[k][0]) g_strlcpy(dst[k], "?", 24); o->any = TRUE; }
        }
        if (o->any) break;
        gchar *p = g_path_get_dirname(dir); if (!strcmp(p, dir) || strlen(p) < 6) { g_free(p); break; } g_free(dir); dir = p;
    }
    g_free(dir);
}

const char *dlss_generation(const char *v, char *buf, size_t n) {
    int major = atoi(v);
    if (!v[0]) g_strlcpy(buf, "absent", n);
    else if (major >= 310) g_strlcpy(buf, "DLSS 4 ou plus récent (transformer)", n);
    else if (major >= 3) g_snprintf(buf, n, "DLSS 3.%d (réseau convolutif)", atoi(strchr(v, '.') ? strchr(v, '.') + 1 : "0"));
    else if (major >= 2) g_strlcpy(buf, "DLSS 2", n);
    else g_strlcpy(buf, "version inconnue", n);
    return buf;
}

gboolean dlss5_layer_present(void) {
    const char *dirs[] = {"/usr/share/vulkan/implicit_layer.d", "/etc/vulkan/implicit_layer.d", NULL};
    gchar *home = g_build_filename(g_get_user_data_dir(), "vulkan/implicit_layer.d", NULL);
    gboolean ok = FALSE;
    for (int i = 0; i < 3 && !ok; i++) {
        const char *d = i < 2 ? dirs[i] : home; GDir *dir = g_dir_open(d, 0, NULL); const char *f;
        while (dir && (f = g_dir_read_name(dir))) { gchar *l = g_ascii_strdown(f, -1); if (strstr(l, "dlss5")) ok = TRUE; g_free(l); }
        if (dir) g_dir_close(dir);
    }
    g_free(home);
    return ok;
}

const char *dlss5_reason(void) {
    const DlssGpu *g = dlss_gpu();
    static char b[200];
    if (!g->series) return "Pas de GPU NVIDIA RTX détecté.";
    if (!g->dlss5) {
        g_snprintf(b, sizeof b, "Indisponible sur %s : NVIDIA ne le propose que sur la série RTX 50 (série 40 prévue, sans date).", g->name);
        return b;
    }
    if (!dlss5_layer_present()) return "Sous Linux, il faut la couche tierce expérimentale DLSS5VKLayer (non installée).";
    return "";
}

const char *dlss_preset_env(int p) {
    static const char *E[6] = {NULL, "render_preset_latest", "render_preset_j", "render_preset_k", "render_preset_l", "render_preset_m"};
    return p >= 0 && p < 6 ? E[p] : NULL;
}
