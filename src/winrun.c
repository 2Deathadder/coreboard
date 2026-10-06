/* Coreboard — adaptateur Windows (Proton-GE via umu-launcher) */
#include "winrun.h"
#include "pe.h"
#include "dlss.h"
#include "fanctl.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static WinGame wg[MAXWG]; static int nwg;

/* jeux dont les corrections Proton (protonfixes) existent : identifiant Steam */
static const struct { const char *key; int appid; } KNOWN[] = {
    {"black myth", 2358720}, {"wukong", 2358720}, {"cyberpunk", 1091500}, {"elden ring", 1245620}, {"eldenring", 1245620},
    {"hogwarts", 990080}, {"baldur", 1086940}, {"bg3", 1086940}, {"red dead", 1174180}, {"rdr2", 1174180}, {"god of war", 1593500}, {"witcher3", 292030}, {"witcher 3", 292030}};

static char *data_dir(const char *sub) {
    gchar *d = g_build_filename(g_get_user_data_dir(), "coreboard", sub, NULL);
    g_mkdir_with_parents(d, 0755);
    return d;
}
static char *cfg_file(void) {
    gchar *d = g_build_filename(g_get_user_config_dir(), "coreboard", NULL);
    g_mkdir_with_parents(d, 0755);
    gchar *p = g_build_filename(d, "wingames.json", NULL); g_free(d);
    return p;
}

WinGame *wg_list(int *n) { *n = nwg; return wg; }

/* analyse PE en tâche de fond (la recherche de chaînes lit jusqu'à 400 Mo) ; le résultat est rangé dans la bonne entrée côté boucle principale */
typedef struct { char exe[512], info[200], warn[240], dlss[160]; int kernel_ac, arch32, dx12; gboolean ok; } Analysis;

static gboolean analysis_done(gpointer p) {
    Analysis *a = p;
    for (int i = 0; i < nwg; i++) if (!strcmp(wg[i].exe, a->exe)) {
        wg[i].analyzed = TRUE; wg[i].kernel_ac = a->kernel_ac; wg[i].arch32 = a->arch32; wg[i].dx12 = a->dx12;
        g_strlcpy(wg[i].info, a->ok ? a->info : "Analyse impossible (fichier non reconnu comme exécutable Windows)", sizeof wg[i].info);
        g_strlcpy(wg[i].warn, a->warn, sizeof wg[i].warn); g_strlcpy(wg[i].dlss_info, a->dlss, sizeof wg[i].dlss_info);
    }
    g_free(a);
    return G_SOURCE_REMOVE;
}
static gpointer analysis_thread(gpointer p) {
    Analysis *a = p; PeInfo pi;
    a->ok = pe_analyze(a->exe, &pi);
    if (a->ok) { pe_summary(&pi, a->info, sizeof a->info); pe_advice(&pi, a->warn, sizeof a->warn); a->kernel_ac = pi.ac_kernel[0] != 0 || pi.machine == 0xaa64; a->arch32 = !pi.is64 && !pi.dotnet; a->dx12 = pi.dx12; }
    DlssGame dg; dlss_scan_game(a->exe, &dg);
    if (dg.any) {
        char g1[64] = "";
        if (dg.sr[0]) dlss_generation(dg.sr, g1, sizeof g1);
        g_snprintf(a->dlss, sizeof a->dlss, "Super Resolution %s (%s)%s%s%s%s", dg.sr[0] ? dg.sr : "absent", g1, dg.fg[0] ? " · Frame Generation " : "", dg.fg, dg.rr[0] ? " · Ray Reconstruction " : "", dg.rr);
    } else g_strlcpy(a->dlss, "Aucune DLL DLSS trouvée dans le dossier du jeu (le jeu peut ne pas gérer le DLSS)", sizeof a->dlss);
    g_idle_add(analysis_done, a);
    return NULL;
}
static void analyze_async(const char *exe) {
    Analysis *a = g_new0(Analysis, 1); g_strlcpy(a->exe, exe, sizeof a->exe);
    g_thread_unref(g_thread_new("coreboard-pe", analysis_thread, a));
}

static void slugify(const char *s, char *out, size_t n) {
    size_t k = 0;
    for (; *s && k + 1 < n; s++) {
        if (g_ascii_isalnum(*s)) out[k++] = g_ascii_tolower(*s);
        else if (k && out[k - 1] != '-') out[k++] = '-';
    }
    while (k && out[k - 1] == '-') k--;
    out[k] = 0;
}

const char *wg_log_path(int i, char *buf, size_t n) {
    char slug[80]; slugify(wg[i].name, slug, sizeof slug);
    gchar *d = data_dir("logs");
    g_snprintf(buf, n, "%s/%s.log", d, slug); g_free(d);
    return buf;
}

/* ------------------------------------------------------------------ persistance */
void wg_save(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_array(b);
    for (int i = 0; i < nwg; i++) {
        json_builder_begin_object(b);
        #define S(k, v) json_builder_set_member_name(b, k); json_builder_add_string_value(b, v)
        #define I(k, v) json_builder_set_member_name(b, k); json_builder_add_int_value(b, v)
        S("name", wg[i].name); S("exe", wg[i].exe); S("args", wg[i].args); S("prefix", wg[i].prefix);
        I("appid", wg[i].appid); I("hud", wg[i].hud); I("gamemode", wg[i].gamemode); I("dlss", wg[i].dlss); I("rt", wg[i].rt); I("log", wg[i].log);
        I("preset", wg[i].preset); I("ntsync", wg[i].ntsync); I("shader_async", wg[i].shader_async); I("lowlat", wg[i].lowlat); I("dlss_up", wg[i].dlss_up);
        I("aniso", wg[i].aniso); I("fsr", wg[i].fsr); I("fsr_str", wg[i].fsr_str); I("fps_cap", wg[i].fps_cap);
        I("sr_preset", wg[i].sr_preset); I("fg", wg[i].fg); I("rr_latest", wg[i].rr_latest); I("dlss_ind", wg[i].dlss_ind);
        #undef S
        #undef I
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    JsonGenerator *g = json_generator_new(); JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root); json_generator_set_pretty(g, TRUE);
    gchar *p = cfg_file(); json_generator_to_file(g, p, NULL);
    g_free(p); json_node_free(root); g_object_unref(g); g_object_unref(b);
}

void wg_init(void) {
    gchar *p = cfg_file(); JsonParser *jp = json_parser_new();
    nwg = 0;
    if (json_parser_load_from_file(jp, p, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(jp))) {
        JsonArray *a = json_node_get_array(json_parser_get_root(jp));
        for (guint i = 0; i < json_array_get_length(a) && nwg < MAXWG; i++) {
            JsonObject *o = json_array_get_object_element(a, i); WinGame *w = &wg[nwg++];
            memset(w, 0, sizeof *w);
            #define JS(k) (json_object_has_member(o, k) ? json_object_get_string_member(o, k) : "")
            #define JI(k, d) (json_object_has_member(o, k) ? (int)json_object_get_int_member(o, k) : (d))
            g_strlcpy(w->name, JS("name"), sizeof w->name); g_strlcpy(w->exe, JS("exe"), sizeof w->exe);
            g_strlcpy(w->args, JS("args"), sizeof w->args); g_strlcpy(w->prefix, JS("prefix"), sizeof w->prefix);
            w->appid = JI("appid", 0); w->hud = JI("hud", 0); w->gamemode = JI("gamemode", 1); w->dlss = JI("dlss", 1); w->rt = JI("rt", 0); w->log = JI("log", 0);
            w->preset = JI("preset", 2); w->ntsync = JI("ntsync", 1); w->shader_async = JI("shader_async", 1); w->lowlat = JI("lowlat", 0); w->dlss_up = JI("dlss_up", 1);
            w->aniso = JI("aniso", 1); w->fsr = JI("fsr", 0); w->fsr_str = JI("fsr_str", 2); w->fps_cap = JI("fps_cap", 0);
            w->sr_preset = JI("sr_preset", 0); w->fg = JI("fg", 0); w->rr_latest = JI("rr_latest", 0); w->dlss_ind = JI("dlss_ind", 0);
            g_strlcpy(w->status, "Prêt", sizeof w->status);
            analyze_async(w->exe);
            #undef JS
            #undef JI
        }
    }
    g_object_unref(jp); g_free(p);
}

/* ------------------------------------------------------------------ préréglages d'amélioration */
const char *wg_preset_name(int p) { static const char *N[] = {"Personnalisé", "Performance", "Équilibré", "Qualité", "Économie"}; return p >= 0 && p <= 4 ? N[p] : "?"; }

void wg_apply_preset(int i, int p) {
    if (i < 0 || i >= nwg) return;
    WinGame *w = &wg[i]; w->preset = p;
    if (p == 0) { wg_save(); return; }
    w->ntsync = 1; w->gamemode = 1; w->dlss = 1; w->fsr = 0; w->fps_cap = 0; w->rt = 0; w->hud = w->hud;
    switch (p) {
    case 1: w->shader_async = 1; w->lowlat = 1; w->dlss_up = 1; w->aniso = 0; break;                    /* le plus de FPS et la plus faible latence */
    case 2: w->shader_async = 1; w->lowlat = 0; w->dlss_up = 1; w->aniso = 1; break;                    /* compromis */
    case 3: w->shader_async = 0; w->lowlat = 0; w->dlss_up = 1; w->aniso = 1; w->rt = w->dx12; break;   /* meilleure image, ray tracing si DirectX 12 */
    default: w->shader_async = 1; w->lowlat = 0; w->dlss_up = 0; w->aniso = 0; w->fps_cap = 60; break;  /* économie : images plafonnées à 60 */
    }
    wg_save();
}

/* ------------------------------------------------------------------ gestion de la liste */
int wg_add(const char *exe) {
    if (nwg >= MAXWG) return -1;
    for (int i = 0; i < nwg; i++) if (!strcmp(wg[i].exe, exe)) return i;
    WinGame *w = &wg[nwg]; memset(w, 0, sizeof *w);
    g_strlcpy(w->exe, exe, sizeof w->exe);
    /* nom : dossier du jeu si l'exécutable a un nom générique, sinon le nom du fichier */
    gchar *base = g_path_get_basename(exe), *dir = g_path_get_dirname(exe), *dirname = g_path_get_basename(dir);
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    gchar *lb = g_ascii_strdown(base, -1);
    gboolean generic = strstr(lb, "shipping") || strstr(lb, "launcher") || strstr(lb, "setup") || strstr(lb, "game") || strlen(base) < 4;
    g_strlcpy(w->name, generic && *dirname && strcmp(dirname, "/") ? dirname : base, sizeof w->name);
    /* si le chemin contient « Binaries/Win64 » (Unreal Engine), remonte au dossier du jeu */
    if (strstr(exe, "/Binaries/Win64/")) {
        gchar *cut = g_strndup(exe, strstr(exe, "/Binaries/Win64/") - exe), *g2 = g_path_get_basename(cut);
        if (strstr(g2, "Content") == NULL && *g2) {
            /* .../Game/Binaries/Win64/X.exe : le dossier du jeu est parent de « Game » s'il s'appelle b1, Game, etc. */
            gchar *parent = g_path_get_dirname(cut), *pn = g_path_get_basename(parent);
            g_strlcpy(w->name, *pn && strlen(pn) > 2 ? pn : g2, sizeof w->name);
            g_free(parent); g_free(pn);
        }
        g_free(cut); g_free(g2);
    }
    if (!strchr(w->name, ' ') && !strchr(w->name, '_') && !strchr(w->name, '-')) {          /* « BlackMythWukong » → « Black Myth Wukong » */
        char sp[sizeof w->name * 2]; size_t o = 0;
        for (const char *c = w->name; *c && o + 2 < sizeof sp; c++) { if (c != w->name && g_ascii_isupper(*c) && g_ascii_islower(c[-1])) sp[o++] = ' '; sp[o++] = *c; }
        sp[o] = 0; g_strlcpy(w->name, sp, sizeof w->name);
    }
    for (guint k = 0; k < G_N_ELEMENTS(KNOWN); k++) { gchar *ln = g_ascii_strdown(w->name, -1); if (strstr(ln, KNOWN[k].key)) w->appid = KNOWN[k].appid; g_free(ln); }
    if (!w->appid) { gchar *le = g_ascii_strdown(exe, -1); for (guint k = 0; k < G_N_ELEMENTS(KNOWN) && !w->appid; k++) if (strstr(le, KNOWN[k].key)) w->appid = KNOWN[k].appid; g_free(le); }
    g_free(base); g_free(dir); g_free(dirname); g_free(lb);
    char slug[80]; slugify(w->name, slug, sizeof slug);
    gchar *pd = data_dir("prefixes");
    g_snprintf(w->prefix, sizeof w->prefix, "%s/%s", pd, slug); g_free(pd);
    w->gamemode = 1; w->dlss = 1; w->fsr_str = 2;
    g_strlcpy(w->status, "Prêt", sizeof w->status);
    nwg++; wg_apply_preset(nwg - 1, 2);
    analyze_async(w->exe);
    return nwg - 1;
}

void wg_remove(int i, gboolean delete_prefix) {
    if (i < 0 || i >= nwg) return;
    if (wg[i].running) wg_stop(i);
    if (delete_prefix && wg[i].prefix[0]) { const char *a[] = {"rm", "-rf", "--", wg[i].prefix, NULL}; g_spawn_sync(NULL, (char **)a, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL); }
    memmove(&wg[i], &wg[i + 1], (nwg - i - 1) * sizeof(WinGame)); nwg--; wg_save();
}

/* ------------------------------------------------------------------ outils */
static gboolean proton_installed(void) {
    const char *roots[] = {".local/share/Steam/compatibilitytools.d", ".local/share/umu/compatibilitytools", ".local/share/umu", NULL};
    for (int r = 0; roots[r]; r++) {
        gchar *d = g_build_filename(g_get_home_dir(), roots[r], NULL); GDir *dir = g_dir_open(d, 0, NULL); const char *f; gboolean ok = FALSE;
        while (dir && (f = g_dir_read_name(dir))) if (g_str_has_prefix(f, "GE-Proton") || g_str_has_prefix(f, "UMU-Proton")) ok = TRUE;
        if (dir) g_dir_close(dir);
        g_free(d);
        if (ok) return TRUE;
    }
    return FALSE;
}

WgTools wg_tools(void) {
    WgTools t = {g_find_program_in_path("umu-run") != NULL, proton_installed(), g_find_program_in_path("gamemoderun") != NULL, g_find_program_in_path("mangohud") != NULL};
    return t;
}

static gboolean installing; static void (*inst_done)(gboolean, gpointer); static gpointer inst_data;
gboolean wg_installing(void) { return installing; }

static gboolean inst_finish(gpointer p) { gboolean ok = GPOINTER_TO_INT(p); installing = FALSE; if (inst_done) inst_done(ok, inst_data); return G_SOURCE_REMOVE; }
static gpointer inst_thread(gpointer d) {
    (void)d;
    const char *argv[] = {"pkexec", "pacman", "-S", "--needed", "--noconfirm", "umu-launcher", "gamemode", "lib32-gamemode", "mangohud", "lib32-mangohud", "lib32-vulkan-icd-loader", NULL};
    gint st = 1; g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &st, NULL);
    g_idle_add(inst_finish, GINT_TO_POINTER(st == 0));
    return NULL;
}
void wg_install_deps(void (*done)(gboolean, gpointer), gpointer d) {
    if (installing) return;
    installing = TRUE; inst_done = done; inst_data = d;
    g_thread_unref(g_thread_new("coreboard-pacman", inst_thread, NULL));
}

/* ------------------------------------------------------------------ lancement */
static void child_setup(gpointer d) { (void)d; setsid(); }         /* groupe de processus dédié : « Arrêter » tue tout le jeu */

static void on_exit_cb(GPid pid, gint status, gpointer d) {
    int i = GPOINTER_TO_INT(d);
    if (i >= 0 && i < nwg && wg[i].pid == pid) {
        wg[i].running = FALSE; wg[i].pid = 0; fan_game_end();
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) g_strlcpy(wg[i].status, "Terminé", sizeof wg[i].status);
        else if (WIFSIGNALED(status)) g_strlcpy(wg[i].status, "Arrêté", sizeof wg[i].status);
        else g_snprintf(wg[i].status, sizeof wg[i].status, "Fermé (code %d) — voir le journal", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    g_spawn_close_pid(pid);
}

void wg_launch(int i) {
    if (i < 0 || i >= nwg || wg[i].running) return;
    fan_game_begin();
    WinGame *w = &wg[i];
    if (!g_file_test(w->exe, G_FILE_TEST_EXISTS)) { g_strlcpy(w->status, "Exécutable introuvable", sizeof w->status); return; }
    if (!g_find_program_in_path("umu-run")) { g_strlcpy(w->status, "umu-launcher n'est pas installé", sizeof w->status); return; }
    g_mkdir_with_parents(w->prefix, 0755);
    char logp[600]; wg_log_path(i, logp, sizeof logp);

    gchar **env = g_get_environ();
    char id[32]; g_snprintf(id, sizeof id, "umu-%d", w->appid);
    env = g_environ_setenv(env, "WINEPREFIX", w->prefix, TRUE);
    env = g_environ_setenv(env, "GAMEID", id, TRUE);
    env = g_environ_setenv(env, "PROTONPATH", "GE-Proton", TRUE);
    env = g_environ_setenv(env, "STORE", "none", TRUE);
    env = g_environ_setenv(env, "CB_LOG", logp, TRUE);
    /* GPU NVIDIA dédié (portables hybrides) + cache de shaders durable */
    env = g_environ_setenv(env, "__NV_PRIME_RENDER_OFFLOAD", "1", TRUE);
    env = g_environ_setenv(env, "__VK_LAYER_NV_optimus", "NVIDIA_only", TRUE);
    env = g_environ_setenv(env, "__GLX_VENDOR_LIBRARY_NAME", "nvidia", TRUE);
    env = g_environ_setenv(env, "__GL_SHADER_DISK_CACHE", "1", TRUE);
    env = g_environ_setenv(env, "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1", TRUE);
    env = g_environ_setenv(env, "WINEDEBUG", w->log ? "+timestamp" : "-all", TRUE);
    if (w->log) env = g_environ_setenv(env, "PROTON_LOG", "1", TRUE);
    if (w->dlss) { env = g_environ_setenv(env, "PROTON_ENABLE_NVAPI", "1", TRUE); env = g_environ_setenv(env, "DXVK_ENABLE_NVAPI", "1", TRUE); }
    if (w->rt) env = g_environ_setenv(env, "VKD3D_CONFIG", "dxr,dxr11", TRUE);
    /* --- amélioration --- */
    char slug[80]; slugify(w->name, slug, sizeof slug);
    { gchar *sc = data_dir("shadercache"), *dir = g_build_filename(sc, slug, NULL); g_mkdir_with_parents(dir, 0755);   /* caches de shaders persistants : moins de saccades */
      env = g_environ_setenv(env, "DXVK_STATE_CACHE_PATH", dir, TRUE); env = g_environ_setenv(env, "VKD3D_SHADER_CACHE_PATH", dir, TRUE); env = g_environ_setenv(env, "__GL_SHADER_DISK_CACHE_PATH", dir, TRUE);
      g_free(sc); g_free(dir); }
    if (w->ntsync && g_file_test("/dev/ntsync", G_FILE_TEST_EXISTS)) env = g_environ_setenv(env, "PROTON_USE_NTSYNC", "1", TRUE);
    if (w->shader_async) env = g_environ_setenv(env, "DXVK_ASYNC", "1", TRUE);
    if (w->dlss && w->dlss_up) env = g_environ_setenv(env, "PROTON_DLSS_UPGRADE", "1", TRUE);
    if (w->dlss) {                                                          /* DLSS 4 / 4.5 : forçage des modèles et de la génération d'images via dxvk-nvapi */
        const DlssGpu *gp = dlss_gpu(); const char *pe = dlss_preset_env(w->sr_preset);
        if (pe) { env = g_environ_setenv(env, "DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE", "on", TRUE); env = g_environ_setenv(env, "DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION", pe, TRUE); }
        if (w->rr_latest && gp->rr) { env = g_environ_setenv(env, "DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE", "on", TRUE); env = g_environ_setenv(env, "DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION", "render_preset_latest", TRUE); }
        if (w->fg && gp->fg) env = g_environ_setenv(env, "DXVK_NVAPI_DRS_NGX_DLSS_FG_OVERRIDE", "on", TRUE);
        if (w->dlss_ind) env = g_environ_setenv(env, "DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS", "DLSSIndicator=1024,DLSSGIndicator=2", TRUE);
    }
    if (w->arch32) env = g_environ_setenv(env, "PROTON_FORCE_LARGE_ADDRESS_AWARE", "1", TRUE);
    if (w->fsr) { env = g_environ_setenv(env, "WINE_FULLSCREEN_FSR", "1", TRUE); char st[8]; g_snprintf(st, sizeof st, "%d", CLAMP(w->fsr_str, 0, 5)); env = g_environ_setenv(env, "WINE_FULLSCREEN_FSR_STRENGTH", st, TRUE); }
    { GString *dc = g_string_new(NULL);                                    /* configuration DXVK passée par l'environnement */
      if (w->dlss) g_string_append(dc, "dxgi.nvapiHack = False; ");              /* expose le GPU NVIDIA au jeu (nécessaire au DLSS) */
      if (w->lowlat) g_string_append(dc, "dxgi.maxFrameLatency = 1; ");
      if (w->aniso) g_string_append(dc, "d3d9.samplerAnisotropy = 16; d3d11.samplerAnisotropy = 16; ");
      if (dc->len) env = g_environ_setenv(env, "DXVK_CONFIG", dc->str, TRUE);
      g_string_free(dc, TRUE); }
    if (w->lowlat) env = g_environ_setenv(env, "__GL_MaxFramesAllowed", "1", TRUE);
    gboolean use_hud = (w->hud || w->fps_cap > 0) && g_find_program_in_path("mangohud");
    if (w->fps_cap > 0) { char fr[16]; g_snprintf(fr, sizeof fr, "%d", w->fps_cap); env = g_environ_setenv(env, "DXVK_FRAME_RATE", fr, TRUE); }
    if (use_hud) {                                                          /* MangoHud : limiteur d'images (DirectX 12 compris) et/ou affichage */
      GString *mg = g_string_new(w->hud ? "fps,frametime,frame_timing,gpu_stats,gpu_temp,cpu_stats,ram,vram,gpu_power" : "no_display");
      if (w->fps_cap > 0) g_string_append_printf(mg, ",fps_limit=%d", w->fps_cap);
      gchar *mh = g_string_free(mg, FALSE);
      env = g_environ_setenv(env, "MANGOHUD_CONFIG", mh, TRUE); g_free(mh); }

    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup("sh")); g_ptr_array_add(a, g_strdup("-c")); g_ptr_array_add(a, g_strdup("exec \"$@\" >>\"$CB_LOG\" 2>&1")); g_ptr_array_add(a, g_strdup("sh"));
    if (w->gamemode && g_find_program_in_path("gamemoderun")) g_ptr_array_add(a, g_strdup("gamemoderun"));
    if (use_hud) g_ptr_array_add(a, g_strdup("mangohud"));
    g_ptr_array_add(a, g_strdup("umu-run")); g_ptr_array_add(a, g_strdup(w->exe));
    if (w->args[0]) { gchar **av; gint ac; if (g_shell_parse_argv(w->args, &ac, &av, NULL)) { for (int k = 0; k < ac; k++) g_ptr_array_add(a, g_strdup(av[k])); g_strfreev(av); } }
    g_ptr_array_add(a, NULL);

    gchar *cwd = g_path_get_dirname(w->exe);
    FILE *lf = fopen(logp, "a"); if (lf) { fprintf(lf, "\n=== Lancement de %s (%s) ===\n", w->name, id); fclose(lf); }
    GError *err = NULL; GPid pid = 0;
    if (g_spawn_async(cwd, (char **)a->pdata, env, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, child_setup, NULL, &pid, &err)) {
        w->pid = pid; w->running = TRUE; g_strlcpy(w->status, "Démarrage… (le premier lancement prépare Proton et peut être long)", sizeof w->status);
        g_child_watch_add(pid, on_exit_cb, GINT_TO_POINTER(i));
    } else { g_snprintf(w->status, sizeof w->status, "Échec : %s", err->message); g_error_free(err); }
    g_free(cwd); g_strfreev(env); g_ptr_array_free(a, TRUE);
}

void wg_stop(int i) {
    if (i < 0 || i >= nwg || !wg[i].running || wg[i].pid <= 0) return;
    kill(-wg[i].pid, SIGTERM);                                       /* tout le groupe (umu, Proton, wineserver, jeu) */
    g_strlcpy(wg[i].status, "Arrêt…", sizeof wg[i].status);
}

/* dernière ligne utile du journal d'un jeu en cours : progression du téléchargement de Proton, erreurs… */
void wg_refresh_status(void) {
    for (int i = 0; i < nwg; i++) {
        if (!wg[i].running) continue;
        char lp[600]; wg_log_path(i, lp, sizeof lp);
        FILE *f = fopen(lp, "r"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); long off = sz > 1500 ? sz - 1500 : 0; fseek(f, off, SEEK_SET);
        char buf[1501]; size_t r = fread(buf, 1, 1500, f); buf[r] = 0; fclose(f);
        gchar **lines = g_strsplit(buf, "\n", -1); const char *last = NULL;
        for (int k = g_strv_length(lines) - 1; k >= 0; k--) { if (strlen(g_strstrip(lines[k])) > 3 && !strstr(lines[k], "===")) { last = lines[k]; break; } }
        if (last) {
            if (strstr(last, "Downloading")) g_snprintf(wg[i].status, sizeof wg[i].status, "Téléchargement de Proton-GE… %.60s", last);
            else g_snprintf(wg[i].status, sizeof wg[i].status, "En cours · %.90s", last);
        }
        g_strfreev(lines);
    }
}

/* ------------------------------------------------------------------ Proton-GE : téléchargement reprenable */
static GMutex dl_lock; static WgDl dl; static gboolean dl_busy;

WgDl wg_dl(void) { g_mutex_lock(&dl_lock); WgDl d = dl; g_mutex_unlock(&dl_lock); return d; }
static void dl_set(int st, gint64 done, gint64 total, const char *msg) {
    g_mutex_lock(&dl_lock); dl.state = st; if (done >= 0) dl.done = done; if (total >= 0) dl.total = total; g_strlcpy(dl.msg, msg, sizeof dl.msg); g_mutex_unlock(&dl_lock);
}

/* verrou « pid total » : un seul téléchargement à la fois, même entre deux processus (interface et ligne de commande) */
static gchar *lock_path(void) { gchar *d = data_dir("downloads"), *p = g_build_filename(d, "proton.lock", NULL); g_free(d); return p; }
static gboolean lock_read(long *pid, gint64 *total) {
    gchar *p = lock_path(), *txt = NULL; long a = 0; long long b = 0;
    gboolean ok = g_file_get_contents(p, &txt, NULL, NULL) && sscanf(txt, "%ld %lld", &a, &b) >= 1 && a > 0 && a != (long)getpid() && kill((pid_t)a, 0) == 0;
    if (ok) { *pid = a; *total = b; }
    g_free(txt); g_free(p);
    return ok;
}
static void lock_write(gint64 total) { gchar *p = lock_path(), *t = g_strdup_printf("%ld %lld", (long)getpid(), (long long)total); g_file_set_contents(p, t, -1, NULL); g_free(t); g_free(p); }
gboolean wg_download_running(void) { long a; gint64 t; return lock_read(&a, &t); }

static gpointer dl_worker(gpointer d) {
    (void)d;
    { long other; gint64 total;
      if (lock_read(&other, &total)) {                            /* un autre processus télécharge déjà : on suit sa progression */
          gchar *dd = data_dir("downloads"); GDir *dir = g_dir_open(dd, 0, NULL); const char *f; gchar *part = NULL;
          while (dir && (f = g_dir_read_name(dir))) if (g_str_has_suffix(f, ".tar.gz")) { g_free(part); part = g_build_filename(dd, f, NULL); }
          if (dir) g_dir_close(dir);
          long o2; gint64 t2;
          while (lock_read(&o2, &t2)) {
              GStatBuf sb; gint64 have = part && g_stat(part, &sb) == 0 ? (gint64)sb.st_size : 0;
              dl_set(2, have, t2, "Téléchargement de Proton-GE…"); g_usleep(1000000);
          }
          g_free(part); g_free(dd);
          dl_set(proton_installed() ? 4 : 5, -1, -1, proton_installed() ? "Proton-GE installé" : "Téléchargement interrompu");
          g_mutex_lock(&dl_lock); dl_busy = FALSE; g_mutex_unlock(&dl_lock);
          return NULL;
      } }
    dl_set(1, 0, 0, "Recherche de la dernière version de Proton-GE…");
    gchar *url = NULL, *name = NULL; gint64 size = 0;
    {   /* API GitHub : dernière version ; à défaut, version connue */
        const char *argv[] = {"curl", "-sL", "--max-time", "60", "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest", NULL};
        gchar *out = NULL; gint st = 1;
        if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &st, NULL) && !st && out) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
                JsonObject *o = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(o, "assets")) {
                    JsonArray *as = json_object_get_array_member(o, "assets");
                    for (guint i = 0; i < json_array_get_length(as) && !url; i++) {
                        JsonObject *a = json_array_get_object_element(as, i); const char *n = json_object_get_string_member(a, "name");
                        if (n && !strchr(n, '/') && g_str_has_suffix(n, ".tar.gz") && !strstr(n, "aarch64") && !strstr(n, "arm")) { name = g_strdup(n); url = g_strdup(json_object_get_string_member(a, "browser_download_url")); size = json_object_get_int_member(a, "size"); }
                    }
                }
            }
            g_object_unref(jp);
        }
        g_free(out);
    }
    if (!url) { name = g_strdup("GE-Proton11-7-x86_64.tar.gz"); url = g_strdup("https://github.com/GloriousEggroll/proton-ge-custom/releases/download/GE-Proton11-7/GE-Proton11-7-x86_64.tar.gz"); size = 563784602; }

    gchar *dd = data_dir("downloads"), *part = g_build_filename(dd, name, NULL);
    lock_write(size);
    gchar *sumurl;                                                  /* « .tar.gz » (7) → « .sha512sum » (10) : nouvelle chaîne */
    { const char *e = strstr(url, ".tar.gz"); sumurl = e ? g_strdup_printf("%.*s.sha512sum", (int)(e - url), url) : g_strdup(url); }
    gboolean installed = FALSE; const char *fail = "Téléchargement interrompu";
    for (int attempt = 0; attempt < 2 && !installed; attempt++) {
        dl_set(2, 0, size, attempt ? "Nouveau téléchargement (l'archive était corrompue)…" : "Téléchargement de Proton-GE…");
        /* curl reprend là où il s'est arrêté (-C -) et réessaie sans limite : la connexion peut tomber */
        const char *argv[] = {"curl", "-L", "-sS", "-C", "-", "--retry", "100000", "--retry-delay", "3", "--retry-all-errors", "--connect-timeout", "30", "-o", part, url, NULL};
        GPid pid; gboolean ok = FALSE;
        if (g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &pid, NULL)) {
            int st = 0;
            for (;;) {
                GStatBuf sb; gint64 have = g_stat(part, &sb) == 0 ? (gint64)sb.st_size : 0;
                dl_set(2, have, -1, "Téléchargement de Proton-GE…");
                pid_t r = waitpid(pid, &st, WNOHANG);
                if (r == pid) { ok = WIFEXITED(st) && WEXITSTATUS(st) == 0; break; }
                g_usleep(1000000);
            }
            g_spawn_close_pid(pid);
        }
        if (!ok) { fail = "Téléchargement interrompu"; break; }
        /* intégrité : somme SHA-512 publiée avec la version */
        dl_set(3, -1, -1, "Vérification de l'intégrité (SHA-512)…");
        gchar *want = NULL, *got = NULL; gint st1 = 1, st2 = 1;
        const char *c1[] = {"curl", "-sL", "--max-time", "60", sumurl, NULL};
        const char *c2[] = {"sha512sum", part, NULL};
        g_spawn_sync(NULL, (char **)c1, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &want, NULL, &st1, NULL);
        g_spawn_sync(NULL, (char **)c2, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &got, NULL, &st2, NULL);
        gboolean sum_ok = want && got && !st1 && !st2 && strlen(want) >= 128 && strncmp(want, got, 128) == 0;
        gboolean sum_unknown = !want || st1 || strlen(want) < 128;         /* somme injoignable : on accepte, tar validera l'archive */
        g_free(want); g_free(got);
        if (!sum_ok && !sum_unknown) { g_unlink(part); fail = "Archive corrompue"; continue; }
        dl_set(3, -1, -1, "Extraction de Proton-GE…");
        gchar *ct = g_build_filename(g_get_home_dir(), ".local/share/Steam/compatibilitytools.d", NULL); g_mkdir_with_parents(ct, 0755);
        const char *tar[] = {"tar", "-xzf", part, "-C", ct, NULL}; gint tst = 1;
        g_spawn_sync(NULL, (char **)tar, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &tst, NULL);
        g_free(ct);
        if (tst == 0) { g_unlink(part); installed = TRUE; } else { g_unlink(part); fail = "Extraction impossible (archive corrompue)"; }
    }
    dl_set(installed ? 4 : 5, -1, -1, installed ? "Proton-GE installé" : fail);
    g_free(sumurl);
    { gchar *lp = lock_path(); g_unlink(lp); g_free(lp); }
    g_free(url); g_free(name); g_free(dd); g_free(part);
    g_mutex_lock(&dl_lock); dl_busy = FALSE; g_mutex_unlock(&dl_lock);
    return NULL;
}

void wg_install_proton_async(void) {
    g_mutex_lock(&dl_lock); gboolean go = !dl_busy; dl_busy = TRUE; g_mutex_unlock(&dl_lock);
    if (go) g_thread_unref(g_thread_new("coreboard-proton", dl_worker, NULL));
}
