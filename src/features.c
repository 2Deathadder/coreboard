/* Coreboard — profils de scénario, bibliothèque de jeux, périphériques */
#include "features.h"
#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include "hw.h"
#include "fanctl.h"
#include "compat.h"


/* ------------------------------------------------------------------ utilitaires */
static gchar *run_out(const char *const *argv) {
    gchar *out = NULL; gint st = 0;
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &st, NULL) || st) { g_free(out); return NULL; }
    return out;
}

static gchar *cfg_path(const char *file) {
    gchar *dir = g_build_filename(g_get_user_config_dir(), "coreboard", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *p = g_build_filename(dir, file, NULL);
    g_free(dir);
    return p;
}

static JsonNode *load_json(const char *file) {
    gchar *p = cfg_path(file);
    JsonParser *jp = json_parser_new();
    JsonNode *root = NULL;
    if (json_parser_load_from_file(jp, p, NULL)) root = json_node_copy(json_parser_get_root(jp));
    g_object_unref(jp); g_free(p);
    return root;
}

static void save_json(const char *file, JsonBuilder *b) {
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root); json_generator_set_pretty(g, TRUE);
    gchar *p = cfg_path(file);
    json_generator_to_file(g, p, NULL);
    g_free(p); json_node_free(root); g_object_unref(g);
}

static const char *jstr(JsonObject *o, const char *k) { return json_object_has_member(o, k) ? json_object_get_string_member(o, k) : ""; }

/* ------------------------------------------------------------------ installation automatique des outils optionnels
   (steamcmd, legendary, lgogdownloader). Hors Arch : dépôts de la distribution ou version officielle téléchargée
   dans ~/.local (compat.c). Sur Arch, ces trois-là ne sont que sur AUR (pas dans les dépôts officiels), donc
   pas de simple « pacman -S » possible. On les compile soi-même (git clone + makepkg, sans privilège requis :
   makepkg sans -s/--syncdeps n'invoque jamais sudo) puis on installe le paquet obtenu via pkexec, exactement le
   même mécanisme pkexec+pacman déjà utilisé ailleurs dans Coreboard. */
static GMutex inst_lock; static gboolean inst_busy; static char inst_target[32];
static void (*inst_done_cb)(const char *pkg, gboolean ok, const char *why);

typedef struct { gchar *pkg; gboolean ok; const char *why; } InstDone;
static gboolean inst_done_idle(gpointer p) {
    InstDone *r = p;
    if (inst_done_cb) inst_done_cb(r->pkg, r->ok, r->why);
    g_free(r->pkg); g_free(r);
    return G_SOURCE_REMOVE;
}
void fx_tool_install_set_done(void (*cb)(const char *pkg, gboolean ok, const char *why)) { inst_done_cb = cb; }

static gboolean run_ok(const char *cwd, const char *const *argv) {
    gint st = 1;
    return g_spawn_sync(cwd, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, NULL, NULL, &st, NULL) && st == 0;
}

/* le miroir pacman de cette machine a des accès intermittents (délais de résolution DNS) : on retente avant
   d'abandonner, pour les étapes réseau (résolution de dépendances, installation du paquet compilé). */
static gboolean run_ok_retry(const char *cwd, const char *const *argv, int tries) {
    for (int i = 0; i < tries; i++) {
        if (run_ok(cwd, argv)) return TRUE;
        if (i + 1 < tries) g_usleep(2 * G_USEC_PER_SEC);
    }
    return FALSE;
}

/* dépendances de compilation/exécution (dépôts officiels, jamais AUR) de chaque outil : makepkg sans -s/--syncdeps
   n'installe rien tout seul, donc on les pose nous-mêmes via pacman avant de compiler. */
static const char *aur_predeps(const char *pkg) {
    if (!strcmp(pkg, "steamcmd")) return "lib32-glibc lib32-gcc-libs";
    if (!strcmp(pkg, "legendary")) return "python python-filelock python-pycryptodomex python-requests python-build python-installer python-uv-build python-wheel";
    if (!strcmp(pkg, "lgogdownloader")) return "boost boost-libs cmake help2man curl jsoncpp rhash tidy tinyxml2";
    return "";
}

static gpointer inst_thread(gpointer d) {
    gchar *pkg = d;
    gboolean ok = FALSE;
    const char *why = "dossier temporaire impossible à créer";
    /* hors Arch (ou sans makepkg/git) : dépôts de la distribution, sinon version officielle dans ~/.local (sans root) */
    if (compat_pkgmgr() != PM_PACMAN || !compat_has("makepkg") || !compat_has("git")) {
        if (compat_pkg_available(pkg)) {
            const char *l[] = {pkg, NULL};
            ok = compat_install_pkgs(l);
            why = ok ? NULL : "installation refusée ou échouée";
        } else if (!strcmp(pkg, "lgogdownloader")) {
            why = "absent des dépôts de cette distribution";
        } else {
            ok = compat_install_user_tool(pkg);
            why = ok ? NULL : "téléchargement impossible (connexion, ou curl manquant)";
        }
        goto done;
    }
    const char *deps = aur_predeps(pkg);
    if (*deps) {
        gchar **dl = g_strsplit(deps, " ", -1);
        GPtrArray *a = g_ptr_array_new();
        g_ptr_array_add(a, (gpointer)"pkexec"); g_ptr_array_add(a, (gpointer)"pacman"); g_ptr_array_add(a, (gpointer)"-S");
        g_ptr_array_add(a, (gpointer)"--needed"); g_ptr_array_add(a, (gpointer)"--noconfirm");
        for (int i = 0; dl[i]; i++) g_ptr_array_add(a, dl[i]);
        g_ptr_array_add(a, NULL);
        run_ok_retry(NULL, (const char *const *)a->pdata, 3);   /* résultat ignoré : un échec ici fera simplement échouer la compilation juste après */
        g_ptr_array_free(a, TRUE);
        g_strfreev(dl);
    }
    gchar *dir = g_dir_make_tmp("coreboard-aur-XXXXXX", NULL);
    if (dir) {
        gchar *url = g_strdup_printf("https://aur.archlinux.org/%s.git", pkg);
        const char *clone[] = {"git", "clone", "--depth", "1", url, dir, NULL};
        why = "téléchargement depuis AUR impossible";
        if (run_ok_retry(NULL, clone, 3)) {
            why = "compilation échouée (dépendances manquantes ?)";
            const char *build[] = {"makepkg", "--noconfirm", "--needed", "--skippgpcheck", NULL};
            if (run_ok(dir, build)) {
                const char *list[] = {"makepkg", "--packagelist", NULL};
                gchar *out = NULL; gint st = 1;
                if (g_spawn_sync(dir, (char **)list, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, &out, NULL, &st, NULL) && out) {
                    gchar **lines = g_strsplit(out, "\n", -1);
                    const char *pkgpath = NULL;
                    for (int i = 0; lines[i] && !pkgpath; i++) if (*lines[i] && !strstr(lines[i], "-debug-")) pkgpath = lines[i];
                    why = "paquet compilé introuvable";
                    if (pkgpath) {
                        const char *inst[] = {"pkexec", "pacman", "-U", "--noconfirm", pkgpath, NULL};
                        ok = run_ok_retry(NULL, inst, 3);
                        why = ok ? NULL : "installation refusée ou échouée";
                    }
                    g_strfreev(lines);
                }
                g_free(out);
            }
        }
        g_free(url);
        const char *rmrf[] = {"rm", "-rf", dir, NULL};
        run_ok(NULL, rmrf);
        g_free(dir);
    }
  done:
    g_mutex_lock(&inst_lock); inst_busy = FALSE; inst_target[0] = 0; g_mutex_unlock(&inst_lock);
    InstDone *r = g_new0(InstDone, 1); r->pkg = pkg; r->ok = ok; r->why = why;   /* signalé à l'interface (toast) */
    g_idle_add(inst_done_idle, r);
    return NULL;
}

gboolean fx_tool_installing(void) { g_mutex_lock(&inst_lock); gboolean b = inst_busy; g_mutex_unlock(&inst_lock); return b; }
const char *fx_tool_installing_name(void) {          /* thread principal uniquement */
    static char t[32];
    g_mutex_lock(&inst_lock); g_strlcpy(t, inst_target, sizeof t); g_mutex_unlock(&inst_lock);
    return t;
}

void fx_tool_install(const char *pkg) {
    g_mutex_lock(&inst_lock);
    gboolean go = !inst_busy;
    if (go) { inst_busy = TRUE; g_strlcpy(inst_target, pkg, sizeof inst_target); }
    g_mutex_unlock(&inst_lock);
    if (go) g_thread_unref(g_thread_new("coreboard-toolinst", inst_thread, g_strdup(pkg)));
}

/* ------------------------------------------------------------------ fenêtres ouvertes (Hyprland, Sway, X11/XWayland : compat.c) */
static Win wins[MAXWIN]; static int nwin; static gchar *active_cls;

gboolean fx_windows_supported(void) { return compat_win_backend() != WB_NONE; }

void fx_windows_refresh(void) {
    if (!fx_windows_supported()) return;
    CWin cw[MAXWIN]; int n = compat_windows(cw, MAXWIN);
    for (int i = 0; i < n; i++) {
        g_strlcpy(wins[i].cls, cw[i].cls, sizeof wins[i].cls); g_strlcpy(wins[i].title, cw[i].title, sizeof wins[i].title); wins[i].pid = cw[i].pid;
    }
    nwin = n;
}

int fx_windows(Win *out, int max) { int n = MIN(nwin, max); memcpy(out, wins, n * sizeof(Win)); return n; }

static void active_class_refresh(void) { g_free(active_cls); active_cls = compat_active_class(); }

/* ------------------------------------------------------------------ scénarios */
static Scenario sc[MAXSC]; static int nsc;
static int sc_active = -1; static char sc_prev_prof[32]; static int sc_prev_hz;

static void sc_load(void) {
    JsonNode *root = load_json("scenarios.json");
    nsc = 0;
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *a = json_node_get_array(root);
        for (guint i = 0; i < json_array_get_length(a) && nsc < MAXSC; i++) {
            JsonObject *o = json_array_get_object_element(a, i); Scenario *s = &sc[nsc++];
            memset(s, 0, sizeof *s);
            g_strlcpy(s->name, jstr(o, "name"), sizeof s->name); g_strlcpy(s->cls, jstr(o, "class"), sizeof s->cls);
            g_strlcpy(s->profile, jstr(o, "profile"), sizeof s->profile);
            s->refresh = json_object_has_member(o, "refresh") ? (int)json_object_get_int_member(o, "refresh") : 0;
            s->enabled = json_object_has_member(o, "enabled") ? json_object_get_boolean_member(o, "enabled") : TRUE;
        }
    }
    if (root) json_node_free(root);
}

void fx_scenarios_save(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_array(b);
    for (int i = 0; i < nsc; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name"); json_builder_add_string_value(b, sc[i].name);
        json_builder_set_member_name(b, "class"); json_builder_add_string_value(b, sc[i].cls);
        json_builder_set_member_name(b, "profile"); json_builder_add_string_value(b, sc[i].profile);
        json_builder_set_member_name(b, "refresh"); json_builder_add_int_value(b, sc[i].refresh);
        json_builder_set_member_name(b, "enabled"); json_builder_add_boolean_value(b, sc[i].enabled);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    save_json("scenarios.json", b); g_object_unref(b);
}

Scenario *fx_scenarios(int *n) { *n = nsc; return sc; }

int fx_scenario_add(const char *name, const char *cls) {
    if (nsc >= MAXSC) return -1;
    for (int i = 0; i < nsc; i++) if (!g_ascii_strcasecmp(sc[i].cls, cls)) return i;
    HwState st; hw_snapshot(&st);
    Scenario *s = &sc[nsc]; memset(s, 0, sizeof *s);
    g_strlcpy(s->name, name, sizeof s->name); g_strlcpy(s->cls, cls, sizeof s->cls); s->enabled = TRUE;
    for (int i = 0; i < st.nprofs; i++) if (!strcmp(st.profs[i], "performance")) g_strlcpy(s->profile, "performance", sizeof s->profile);
    if (!s->profile[0] && st.nprofs) g_strlcpy(s->profile, st.profs[st.nprofs - 1], sizeof s->profile);
    for (int i = 0; i < st.disp.nmodes; i++) s->refresh = MAX(s->refresh, st.disp.modes[i]);
    nsc++; fx_scenarios_save();
    return nsc - 1;
}

void fx_scenario_remove(int i) {
    if (i < 0 || i >= nsc) return;
    if (sc_active == i) sc_active = -1; else if (sc_active > i) sc_active--;
    memmove(&sc[i], &sc[i + 1], (nsc - i - 1) * sizeof(Scenario)); nsc--; fx_scenarios_save();
}

const char *fx_scenario_active(void) { return sc_active >= 0 && sc_active < nsc ? sc[sc_active].name : NULL; }

static void ignore_cb(const char *m, gboolean ok, gpointer d) { (void)m; (void)ok; (void)d; }

static void sc_apply(const char *prof, int hz) {
    char b[16];
    if (prof && *prof) hw_apply("profile", prof, ignore_cb, NULL);
    if (hz > 0) { g_snprintf(b, sizeof b, "%d", hz); hw_apply("refresh", b, ignore_cb, NULL); }
}

static gboolean cls_match(const char *pattern, const char *cls) {
    if (!*pattern || !cls) return FALSE;
    gchar *p = g_ascii_strdown(pattern, -1), *c = g_ascii_strdown(cls, -1);
    gboolean ok = strstr(c, p) != NULL;
    g_free(p); g_free(c);
    return ok;
}

void fx_scenario_tick(void) {
    gboolean any = FALSE;
    for (int i = 0; i < nsc; i++) any |= sc[i].enabled;
    if (!fx_windows_supported() || (!any && sc_active < 0)) return;
    active_class_refresh();
    int hit = -1;
    for (int i = 0; i < nsc && hit < 0; i++) if (sc[i].enabled && cls_match(sc[i].cls, active_cls)) hit = i;
    if (hit == sc_active) return;
    HwState st; hw_snapshot(&st);
    if (hit >= 0) {
        if (sc_active < 0) { g_strlcpy(sc_prev_prof, st.prof_cur, sizeof sc_prev_prof); sc_prev_hz = st.disp.hz; }
        sc_active = hit;
        sc_apply(strcmp(sc[hit].profile, st.prof_cur) ? sc[hit].profile : NULL, sc[hit].refresh != st.disp.hz ? sc[hit].refresh : 0);
    } else {
        sc_active = -1;
        sc_apply(strcmp(sc_prev_prof, st.prof_cur) ? sc_prev_prof : NULL, sc_prev_hz != st.disp.hz ? sc_prev_hz : 0);
    }
}

/* ------------------------------------------------------------------ jeux */
static Game games[MAXGAME]; static int ngames;

static Game *game_new(const char *name, const char *source) {
    if (ngames >= MAXGAME) return NULL;
    Game *g = &games[ngames++]; memset(g, 0, sizeof *g);
    g_strlcpy(g->name, name, sizeof g->name); g_strlcpy(g->source, source, sizeof g->source);
    return g;
}

static void scan_steam(void) {
    const char *home = g_get_home_dir();
    const char *roots[] = {".local/share/Steam/steamapps", ".steam/steam/steamapps", ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps", NULL};
    for (int r = 0; roots[r]; r++) {
        gchar *dir = g_build_filename(home, roots[r], NULL);
        GDir *d = g_dir_open(dir, 0, NULL);
        const char *f;
        while (d && (f = g_dir_read_name(d))) {
            if (!g_str_has_prefix(f, "appmanifest_") || !g_str_has_suffix(f, ".acf")) continue;
            gchar *path = g_build_filename(dir, f, NULL), *txt = NULL;
            if (g_file_get_contents(path, &txt, NULL, NULL)) {
                char appid[24] = "", name[64] = "";
                gchar **lines = g_strsplit(txt, "\n", -1);
                for (int i = 0; lines[i]; i++) {
                    char k[32], v[128];
                    if (sscanf(lines[i], " \"%31[^\"]\" \"%127[^\"]\"", k, v) == 2) {
                        if (!strcmp(k, "appid") && !appid[0]) g_strlcpy(appid, v, sizeof appid);
                        else if (!strcmp(k, "name") && !name[0]) g_strlcpy(name, v, sizeof name);
                    }
                }
                g_strfreev(lines);
                if (appid[0] && name[0] && !g_str_has_prefix(name, "Steamworks") && !g_str_has_prefix(name, "Proton") && !g_str_has_prefix(name, "Steam Linux")) {
                    Game *g = game_new(name, "Steam");
                    if (g) { g_strlcpy(g->id, appid, sizeof g->id); g_snprintf(g->cls, sizeof g->cls, "steam_app_%s", appid); }
                }
            }
            g_free(txt); g_free(path);
        }
        if (d) g_dir_close(d);
        g_free(dir);
    }
}

/* Epic Games installées via Legendary (lanceur libre officieux, gère l'authentification lui-même) */
static void scan_legendary(void) {
    if (!compat_has("legendary")) return;
    gchar *p = g_build_filename(g_get_home_dir(), ".config/legendary/installed.json", NULL);
    gchar *txt = NULL;
    if (g_file_get_contents(p, &txt, NULL, NULL)) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, txt, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
            JsonObject *root = json_node_get_object(json_parser_get_root(jp));
            JsonObjectIter it; json_object_iter_init(&it, root);
            const gchar *appname; JsonNode *val;
            while (json_object_iter_next(&it, &appname, &val)) {
                if (!JSON_NODE_HOLDS_OBJECT(val)) continue;
                const char *title = jstr(json_node_get_object(val), "title");
                Game *g = game_new(*title ? title : appname, "Epic");
                if (!g) break;
                g_strlcpy(g->id, appname, sizeof g->id);
            }
        }
        g_object_unref(jp);
    }
    g_free(txt); g_free(p);
}

/* jeux GOG installés via Heroic (lanceur libre pour GOG/Epic/Amazon ; gère l'authentification lui-même) */
static void scan_gog_heroic(void) {
    const char *home = g_get_home_dir();
    const char *roots[] = {".config/heroic", ".var/app/com.heroicgameslauncher.hgl/config/heroic", NULL};
    for (int r = 0; roots[r]; r++) {
        gchar *base = g_build_filename(home, roots[r], "gog_store", NULL);
        gchar *inst_p = g_build_filename(base, "installed.json", NULL);
        gchar *lib_p = g_build_filename(base, "library.json", NULL);
        gchar *inst_txt = NULL, *lib_txt = NULL;
        if (g_file_get_contents(inst_p, &inst_txt, NULL, NULL)) {
            GHashTable *titles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            if (g_file_get_contents(lib_p, &lib_txt, NULL, NULL)) {
                JsonParser *jl = json_parser_new();
                if (json_parser_load_from_data(jl, lib_txt, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jl))) {
                    JsonObject *lo = json_node_get_object(json_parser_get_root(jl));
                    if (json_object_has_member(lo, "games") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(lo, "games"))) {
                        JsonArray *arr = json_object_get_array_member(lo, "games");
                        for (guint i = 0; i < json_array_get_length(arr); i++) {
                            JsonObject *go = json_array_get_object_element(arr, i);
                            const char *id = jstr(go, "app_name"), *title = jstr(go, "title");
                            if (*id && *title) g_hash_table_insert(titles, g_strdup(id), g_strdup(title));
                        }
                    }
                }
                g_object_unref(jl);
            }
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, inst_txt, -1, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(jp))) {
                JsonArray *arr = json_node_get_array(json_parser_get_root(jp));
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    JsonObject *o = json_array_get_object_element(arr, i);
                    const char *id = jstr(o, "appName");
                    if (!*id) continue;
                    const char *title = g_hash_table_lookup(titles, id);
                    Game *g = game_new(title ? title : id, "GOG");
                    if (!g) break;
                    g_strlcpy(g->id, id, sizeof g->id);
                }
            }
            g_object_unref(jp);
            g_hash_table_destroy(titles);
        }
        g_free(inst_txt); g_free(lib_txt); g_free(inst_p); g_free(lib_p); g_free(base);
    }
}

/* bibliothèque Lutris (agrégateur GOG/Epic/Origin/… par Wine) : lecture directe de sa base pga.db */
static void scan_lutris(void) {
    gchar *db = g_build_filename(g_get_home_dir(), ".local/share/lutris/pga.db", NULL);
    if (!g_file_test(db, G_FILE_TEST_EXISTS) || !compat_has("sqlite3") || !compat_has("lutris")) { g_free(db); return; }
    const char *argv[] = {"sqlite3", "-noheader", "-separator", "\t", db, "SELECT slug,name FROM games WHERE installed=1;", NULL};
    gchar *out = run_out(argv);
    g_free(db);
    if (!out) return;
    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i] && *lines[i]; i++) {
        gchar **f = g_strsplit(lines[i], "\t", 2);
        if (f[0] && f[1] && *f[0] && *f[1]) {
            Game *g = game_new(f[1], "Lutris");
            if (g) g_strlcpy(g->id, f[0], sizeof g->id);
        }
        g_strfreev(f);
    }
    g_strfreev(lines); g_free(out);
}

static void scan_desktop(void) {
    GList *all = g_app_info_get_all();
    for (GList *l = all; l && ngames < MAXGAME; l = l->next) {
        GAppInfo *ai = l->data;
        if (!G_IS_DESKTOP_APP_INFO(ai) || !g_app_info_should_show(ai)) continue;
        const char *cat = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(ai));
        if (!cat || !strstr(cat, "Game")) continue;
        const char *id = g_app_info_get_id(ai);
        if (id && g_str_has_prefix(id, "steam")) continue;
        Game *g = game_new(g_app_info_get_display_name(ai), "Application");
        if (!g) break;
        g->app = g_object_ref(ai);
        const char *wm = g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(ai));
        if (wm) g_strlcpy(g->cls, wm, sizeof g->cls);
        else { gchar *b = g_path_get_basename(g_app_info_get_executable(ai)); g_strlcpy(g->cls, b, sizeof g->cls); g_free(b); }
    }
    g_list_free_full(all, g_object_unref);
}

static void scan_manual(void) {
    JsonNode *root = load_json("games.json");
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *a = json_node_get_array(root);
        for (guint i = 0; i < json_array_get_length(a); i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            Game *g = game_new(jstr(o, "name"), "Ajouté");
            if (!g) break;
            g->manual = TRUE; g_strlcpy(g->cls, jstr(o, "class"), sizeof g->cls); g_strlcpy(g->command, jstr(o, "command"), sizeof g->command);
        }
    }
    if (root) json_node_free(root);
}

static void manual_save(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_array(b);
    for (int i = 0; i < ngames; i++) {
        if (!games[i].manual) continue;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name"); json_builder_add_string_value(b, games[i].name);
        json_builder_set_member_name(b, "class"); json_builder_add_string_value(b, games[i].cls);
        json_builder_set_member_name(b, "command"); json_builder_add_string_value(b, games[i].command);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    save_json("games.json", b); g_object_unref(b);
}

/* certains jeux s'ajoutent plusieurs fois (Steam + un ou plusieurs raccourcis .desktop du même nom) : on ne
   garde que la source la plus fiable pour lancer le jeu. */
static int source_priority(const char *src) {
    if (!strcmp(src, "Ajouté")) return 0;
    if (!strcmp(src, "Steam")) return 1;
    if (!strcmp(src, "Epic")) return 2;
    if (!strcmp(src, "GOG")) return 3;
    if (!strcmp(src, "Lutris")) return 4;
    return 5;   /* Application (raccourci .desktop générique) */
}

static int game_cmp(const void *a, const void *b) {
    const Game *ga = a, *gb = b;
    int c = g_ascii_strcasecmp(ga->name, gb->name);
    return c ? c : source_priority(ga->source) - source_priority(gb->source);
}

void fx_games_rescan(void) {
    for (int i = 0; i < ngames; i++) if (games[i].app) g_object_unref(games[i].app);
    ngames = 0;
    scan_manual(); scan_steam(); scan_legendary(); scan_gog_heroic(); scan_lutris(); scan_desktop();
    qsort(games, ngames, sizeof(Game), game_cmp);
    int w = 0;
    for (int i = 0; i < ngames; i++) {
        if (w > 0 && !g_ascii_strcasecmp(games[w - 1].name, games[i].name)) {
            if (games[i].app) g_object_unref(games[i].app);   /* doublon ignoré : on libère sa référence GAppInfo */
            continue;
        }
        if (w != i) games[w] = games[i];
        w++;
    }
    ngames = w;
}

Game *fx_games(int *n) { *n = ngames; return games; }

void fx_game_launch(const Game *g) {
    fan_game_begin();
    if (g->app) { g_app_info_launch(g->app, NULL, NULL, NULL); return; }
    /* argv explicite : un identifiant contenant espaces ou guillemets ne peut pas être découpé en plusieurs arguments */
    gchar *uri = NULL; const char *prog = NULL, *sub = NULL;
    if (!strcmp(g->source, "Steam")) { prog = "steam"; uri = g_strdup_printf("steam://rungameid/%s", g->id); }
    else if (!strcmp(g->source, "Epic")) { prog = "legendary"; sub = "launch"; uri = g_strdup(g->id); }
    else if (!strcmp(g->source, "GOG")) { prog = "xdg-open"; uri = g_strdup_printf("heroic://launch/gog/%s", g->id); }
    else if (!strcmp(g->source, "Lutris")) { prog = "lutris"; uri = g_strdup_printf("lutris:rungame/%s", g->id); }
    if (prog) {
        const char *argv[] = {prog, sub ? sub : uri, sub ? uri : NULL, NULL};
        g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
        g_free(uri);
        return;
    } else if (g->command[0]) {
        const char *argv[] = {"sh", "-c", g->command, NULL};
        g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
    }
}

/* ajoute une appli ouverte : la commande de lancement est relue depuis /proc/<pid>/cmdline */
int fx_game_add_manual(const Win *w) {
    for (int i = 0; i < ngames; i++) if (!g_ascii_strcasecmp(games[i].cls, w->cls)) return i;
    gchar *path = g_strdup_printf("/proc/%d/cmdline", w->pid), *raw = NULL; gsize len = 0;
    GString *cmd = g_string_new(NULL);
    if (g_file_get_contents(path, &raw, &len, NULL)) {
        for (gsize i = 0; i < len; i += strlen(raw + i) + 1) {
            gchar *q = g_shell_quote(raw + i);
            if (cmd->len) g_string_append_c(cmd, ' ');
            g_string_append(cmd, q); g_free(q);
        }
    }
    g_free(raw); g_free(path);
    const char *dot = strrchr(w->cls, '.');
    Game *g = game_new(dot && dot[1] ? dot + 1 : w->cls, "Ajouté");
    if (!g) { g_string_free(cmd, TRUE); return -1; }
    g->manual = TRUE; g_strlcpy(g->cls, w->cls, sizeof g->cls); g_strlcpy(g->command, cmd->str, sizeof g->command);
    g_string_free(cmd, TRUE);
    manual_save(); fx_games_rescan();
    return 0;
}

void fx_game_remove(int i) {
    if (i < 0 || i >= ngames || !games[i].manual) return;
    games[i].manual = FALSE; manual_save();   /* la ligne est ignorée à l'enregistrement puis retirée au rescan */
    fx_games_rescan();
}

/* ------------------------------------------------------------------ périphériques */
static GMutex dev_lock; static Dev dev_cache[MAXDEV]; static int ndev_cache; static gboolean dev_busy;

static void kind_from_name(const char *name, char *kind, size_t n) {
    gchar *l = g_ascii_strdown(name, -1);
    const char *k = "usb";
    if (strstr(l, "mouse") || strstr(l, "souris")) k = "mouse";
    else if (strstr(l, "keyboard") || strstr(l, "clavier")) k = "keyboard";
    else if (strstr(l, "headset") || strstr(l, "headphone") || strstr(l, "casque") || strstr(l, "buds")) k = "headset";
    else if (strstr(l, "gamepad") || strstr(l, "controller") || strstr(l, "manette")) k = "gamepad";
    g_strlcpy(kind, k, n); g_free(l);
}

static Dev *dev_add(Dev *arr, int *n, const char *name) {
    if (*n >= MAXDEV) return NULL;
    Dev *d = &arr[(*n)++]; memset(d, 0, sizeof *d); d->battery = -1;
    g_strlcpy(d->name, name, sizeof d->name);
    return d;
}

static void gather_usb(Dev *arr, int *n) {
    GDir *d = g_dir_open("/sys/bus/usb/devices", 0, NULL); const char *f;
    while (d && (f = g_dir_read_name(d))) {
        if (strchr(f, ':') || g_str_has_prefix(f, "usb")) continue;
        gchar *base = g_strdup_printf("/sys/bus/usb/devices/%s/", f), *p, *prod = NULL, *man = NULL, *rem = NULL, *cls = NULL;
        p = g_strconcat(base, "product", NULL); g_file_get_contents(p, &prod, NULL, NULL); g_free(p);
        p = g_strconcat(base, "manufacturer", NULL); g_file_get_contents(p, &man, NULL, NULL); g_free(p);
        p = g_strconcat(base, "removable", NULL); g_file_get_contents(p, &rem, NULL, NULL); g_free(p);
        p = g_strconcat(base, "bDeviceClass", NULL); g_file_get_contents(p, &cls, NULL, NULL); g_free(p);
        if (prod) g_strstrip(prod);
        if (man) g_strstrip(man);
        if (rem) g_strstrip(rem);
        if (cls) g_strstrip(cls);
        gboolean skip = !prod || !*prod || strstr(prod, "Root Hub") || (rem && !strcmp(rem, "fixed")) || (cls && !strcmp(cls, "09"));
        if (!skip) {
            Dev *dv = dev_add(arr, n, prod);
            if (dv) { kind_from_name(prod, dv->kind, sizeof dv->kind); g_strlcpy(dv->info, man ? man : "USB", sizeof dv->info); dv->connected = 1; }
        }
        g_free(base); g_free(prod); g_free(man); g_free(rem); g_free(cls);
    }
    if (d) g_dir_close(d);
}

static void gather_bluetooth(Dev *arr, int *n) {
    const char *a1[] = {"bluetoothctl", "devices", NULL};
    gchar *out = run_out(a1); if (!out) return;
    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char mac[24], name[64];
        if (sscanf(lines[i], "Device %23s %63[^\n]", mac, name) != 2) continue;
        const char *a2[] = {"bluetoothctl", "info", mac, NULL};
        gchar *info = run_out(a2);
        Dev *d = dev_add(arr, n, name); if (!d) { g_free(info); break; }
        g_strlcpy(d->kind, "bt", sizeof d->kind); g_strlcpy(d->info, "Bluetooth", sizeof d->info);
        if (info) {
            d->connected = strstr(info, "Connected: yes") != NULL;
            const char *ic = strstr(info, "Icon: ");
            if (ic) {
                if (g_str_has_prefix(ic + 6, "input-mouse")) g_strlcpy(d->kind, "mouse", sizeof d->kind);
                else if (g_str_has_prefix(ic + 6, "input-keyboard")) g_strlcpy(d->kind, "keyboard", sizeof d->kind);
                else if (g_str_has_prefix(ic + 6, "audio-")) g_strlcpy(d->kind, "headset", sizeof d->kind);
                else if (g_str_has_prefix(ic + 6, "input-gaming")) g_strlcpy(d->kind, "gamepad", sizeof d->kind);
                else if (g_str_has_prefix(ic + 6, "phone")) g_strlcpy(d->kind, "phone", sizeof d->kind);
            }
            const char *bp = strstr(info, "Battery Percentage:");
            if (bp) { const char *par = strchr(bp, '('); if (par) d->battery = atoi(par + 1); }
        }
        g_free(info);
    }
    g_strfreev(lines); g_free(out);
}

static void gather_upower(Dev *arr, int *n) {
    const char *a1[] = {"upower", "-e", NULL};
    gchar *out = run_out(a1); if (!out) return;
    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i] || strstr(lines[i], "line_power") || strstr(lines[i], "DisplayDevice") || strstr(lines[i], "battery_BAT")) continue;
        const char *a2[] = {"upower", "-i", lines[i], NULL};
        gchar *info = run_out(a2); if (!info) continue;
        char model[64] = ""; int pct = -1;
        gchar **l2 = g_strsplit(info, "\n", -1);
        for (int j = 0; l2[j]; j++) {
            gchar *t = g_strstrip(l2[j]);
            if (g_str_has_prefix(t, "model:")) g_strlcpy(model, g_strstrip(t + 6), sizeof model);
            else if (g_str_has_prefix(t, "percentage:")) pct = atoi(t + 11);
        }
        g_strfreev(l2);
        if (model[0] && pct >= 0) {
            gboolean merged = FALSE;
            for (int j = 0; j < *n; j++) if (!g_ascii_strcasecmp(arr[j].name, model)) { arr[j].battery = pct; merged = TRUE; }
            if (!merged) { Dev *d = dev_add(arr, n, model); if (d) { kind_from_name(model, d->kind, sizeof d->kind); g_strlcpy(d->info, "Batterie", sizeof d->info); d->battery = pct; d->connected = 1; } }
        }
        g_free(info);
    }
    g_strfreev(lines); g_free(out);
}

static int dev_cmp(const void *a, const void *b) { return ((const Dev *)b)->connected - ((const Dev *)a)->connected; }

static gpointer dev_thread(gpointer d) {
    (void)d;
    Dev tmp[MAXDEV]; int n = 0;
    gather_bluetooth(tmp, &n); gather_usb(tmp, &n); gather_upower(tmp, &n);
    qsort(tmp, n, sizeof(Dev), dev_cmp);
    g_mutex_lock(&dev_lock); memcpy(dev_cache, tmp, sizeof tmp); ndev_cache = n; dev_busy = FALSE; g_mutex_unlock(&dev_lock);
    return NULL;
}

void fx_devices_refresh(void) {
    g_mutex_lock(&dev_lock);
    gboolean go = !dev_busy; dev_busy = TRUE;
    g_mutex_unlock(&dev_lock);
    if (go) g_thread_unref(g_thread_new("coreboard-dev", dev_thread, NULL));
}

int fx_devices(Dev *out, int max) {
    g_mutex_lock(&dev_lock);
    int n = MIN(ndev_cache, max); memcpy(out, dev_cache, n * sizeof(Dev));
    g_mutex_unlock(&dev_lock);
    return n;
}

/* ------------------------------------------------------------------ jeux gratuits Steam (free-to-play)
   Recherche publique du store (aucune connexion requise) ; le téléchargement réel est ensuite fait par
   Steam lui-même via steam://install, comme pour le lancement d'un jeu déjà installé. */
static GMutex sf_lock; static FreeGame sf_cache[MAXFREE]; static int sf_n; static gboolean sf_busy, sf_ready;

static int freegame_cmp(const void *a, const void *b) { return g_ascii_strcasecmp(((const FreeGame *)a)->name, ((const FreeGame *)b)->name); }

static gpointer steamfree_thread(gpointer d) {
    (void)d;
    const char *argv[] = {"curl", "-s", "--max-time", "20",
        "https://store.steampowered.com/api/storesearch/?term=&maxprice=free&cc=us&l=french&start=0&count=50", NULL};
    gchar *out = run_out(argv);
    FreeGame tmp[MAXFREE]; int n = 0;
    if (out) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
            JsonObject *root = json_node_get_object(json_parser_get_root(jp));
            if (json_object_has_member(root, "items")) {
                JsonArray *items = json_object_get_array_member(root, "items");
                for (guint i = 0; i < json_array_get_length(items) && n < MAXFREE; i++) {
                    JsonObject *o = json_array_get_object_element(items, i);
                    JsonNode *pn = json_object_get_member(o, "price");
                    gboolean is_free = !pn || (JSON_NODE_HOLDS_OBJECT(pn) && json_object_get_int_member(json_node_get_object(pn), "final") == 0);
                    if (!is_free || !json_object_has_member(o, "name") || !json_object_has_member(o, "id")) continue;
                    FreeGame *g = &tmp[n++];
                    g_strlcpy(g->name, json_object_get_string_member(o, "name"), sizeof g->name);
                    g->id = json_object_get_int_member(o, "id");
                }
            }
        }
        g_object_unref(jp);
    }
    g_free(out);
    qsort(tmp, n, sizeof(FreeGame), freegame_cmp);
    g_mutex_lock(&sf_lock); memcpy(sf_cache, tmp, sizeof tmp); sf_n = n; sf_busy = FALSE; sf_ready = TRUE; g_mutex_unlock(&sf_lock);
    return NULL;
}

gboolean fx_steamfree_ready(void) { g_mutex_lock(&sf_lock); gboolean r = sf_ready; g_mutex_unlock(&sf_lock); return r; }

void fx_steamfree_refresh(void) {
    if (!compat_has("curl")) return;
    g_mutex_lock(&sf_lock);
    gboolean go = !sf_busy; sf_busy = TRUE;
    g_mutex_unlock(&sf_lock);
    if (go) g_thread_unref(g_thread_new("coreboard-steamfree", steamfree_thread, NULL));
}

int fx_steamfree(FreeGame *out, int max) {
    g_mutex_lock(&sf_lock);
    int n = MIN(sf_n, max); memcpy(out, sf_cache, n * sizeof(FreeGame));
    g_mutex_unlock(&sf_lock);
    return n;
}

/* ------------------------------------------------------------------ recherche libre dans le catalogue Steam
   Même API publique que la liste des jeux gratuits, mais sans filtre de prix : n'importe quel jeu du store. */
static GMutex ss_lock; static SteamHit ss_cache[MAXHITS]; static int ss_n; static gboolean ss_busy; static char ss_query[96];

static int steamhit_cmp(const void *a, const void *b) { return g_ascii_strcasecmp(((const SteamHit *)a)->name, ((const SteamHit *)b)->name); }

static gpointer steamsearch_thread(gpointer d) {
    gchar *query = d;
    gchar *enc = g_uri_escape_string(query, NULL, FALSE);
    gchar *url = g_strdup_printf("https://store.steampowered.com/api/storesearch/?term=%s&cc=us&l=french&start=0&count=%d", enc, MAXHITS);
    const char *argv[] = {"curl", "-s", "--max-time", "20", url, NULL};
    gchar *out = run_out(argv);
    SteamHit tmp[MAXHITS]; int n = 0;
    if (out) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
            JsonObject *root = json_node_get_object(json_parser_get_root(jp));
            if (json_object_has_member(root, "items")) {
                JsonArray *items = json_object_get_array_member(root, "items");
                for (guint i = 0; i < json_array_get_length(items) && n < MAXHITS; i++) {
                    JsonObject *o = json_array_get_object_element(items, i);
                    if (!json_object_has_member(o, "name") || !json_object_has_member(o, "id")) continue;
                    SteamHit *g = &tmp[n++];
                    g_strlcpy(g->name, json_object_get_string_member(o, "name"), sizeof g->name);
                    g->id = json_object_get_int_member(o, "id");
                    JsonNode *pn = json_object_get_member(o, "price");
                    if (pn && JSON_NODE_HOLDS_OBJECT(pn)) { g->free = FALSE; g->price_cents = (int)json_object_get_int_member(json_node_get_object(pn), "final"); }
                    else { g->free = TRUE; g->price_cents = 0; }
                }
            }
        }
        g_object_unref(jp);
    }
    g_free(out); g_free(url); g_free(enc);
    qsort(tmp, n, sizeof(SteamHit), steamhit_cmp);
    g_mutex_lock(&ss_lock); memcpy(ss_cache, tmp, sizeof tmp); ss_n = n; ss_busy = FALSE; g_mutex_unlock(&ss_lock);
    g_free(query);
    return NULL;
}

void fx_steamsearch_query(const char *q) {
    if (!q || !*q || !compat_has("curl")) return;
    g_mutex_lock(&ss_lock);
    g_strlcpy(ss_query, q, sizeof ss_query);
    gboolean go = !ss_busy; ss_busy = TRUE;
    g_mutex_unlock(&ss_lock);
    if (go) g_thread_unref(g_thread_new("coreboard-steamsearch", steamsearch_thread, g_strdup(q)));
}

int fx_steamsearch(SteamHit *out, int max) {
    g_mutex_lock(&ss_lock);
    int n = MIN(ss_n, max); memcpy(out, ss_cache, n * sizeof(SteamHit));
    g_mutex_unlock(&ss_lock);
    return n;
}

const char *fx_steamsearch_query_str(void) { return ss_query; }

/* ------------------------------------------------------------------ session Steam intégrée (steamcmd)
   steamcmd ne vide son tampon de sortie qu'à sa propre sortie (rien à voir avec un problème réseau : vérifié
   en le laissant tourner plus d'une minute sur une session interactive persistante, sans qu'une seule ligne
   n'arrive jamais). Chaque action est donc une invocation ponctuelle « +login … +quit » qui se termine seule,
   ce qui garantit un vidage complet du tampon. Le mot de passe n'est jamais écrit sur le disque ni réutilisé :
   les téléchargements ultérieurs ne renvoient que l'identifiant, en s'appuyant sur la session que steamcmd
   garde en cache localement après une première connexion réussie. */
static GMutex scm_lock;              /* protège scm_status */
static GMutex scm_write_lock;        /* protège scm_in (stdin du job en cours, pour répondre à Steam Guard) */
static ScmStatus scm_status;
static FILE *scm_in;
static char scm_user[64];

gboolean fx_steam_available(void) { return compat_has("steamcmd"); }

static gboolean scm_write(const char *fmt, ...) {
    g_mutex_lock(&scm_write_lock);
    gboolean ok = scm_in != NULL;
    if (ok) { va_list ap; va_start(ap, fmt); vfprintf(scm_in, fmt, ap); va_end(ap); fflush(scm_in); }
    g_mutex_unlock(&scm_write_lock);
    return ok;
}

typedef struct { char user[64], pass[64], game[96]; gint64 appid; gboolean download; } ScmJob;

static gpointer scm_job_thread(gpointer d) {
    ScmJob *j = d;
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, (gpointer)"steamcmd");
    g_ptr_array_add(a, (gpointer)"+login"); g_ptr_array_add(a, j->user);
    if (j->pass[0]) g_ptr_array_add(a, j->pass);
    gchar *appidbuf = NULL;
    if (j->download) {
        appidbuf = g_strdup_printf("%" G_GINT64_FORMAT, j->appid);
        g_ptr_array_add(a, (gpointer)"+app_update"); g_ptr_array_add(a, appidbuf); g_ptr_array_add(a, (gpointer)"validate");
    }
    g_ptr_array_add(a, (gpointer)"+quit");
    g_ptr_array_add(a, NULL);

    gint in_fd, out_fd; GPid pid; GError *err = NULL;
    gboolean ok = g_spawn_async_with_pipes(NULL, (char **)a->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, &pid, &in_fd, &out_fd, NULL, &err);
    g_ptr_array_free(a, TRUE); g_free(appidbuf);
    if (!ok) {
        g_mutex_lock(&scm_lock);
        scm_status.error = TRUE; scm_status.dl_active = FALSE;
        g_strlcpy(scm_status.msg, "Impossible de lancer steamcmd.", sizeof scm_status.msg);
        g_mutex_unlock(&scm_lock);
        g_clear_error(&err); g_free(j); return NULL;
    }
    g_mutex_lock(&scm_write_lock); scm_in = fdopen(in_fd, "w"); g_mutex_unlock(&scm_write_lock);
    FILE *out = fdopen(out_fd, "r");
    gboolean awaiting_login = FALSE;
    char line[512];
    while (out && fgets(line, sizeof line, out)) {
        g_strchomp(line);
        if (!*line) continue;
        g_mutex_lock(&scm_lock);
        if (strstr(line, "Checking for available updates") || strstr(line, "Verifying installation")) {
            if (!scm_status.connected) g_strlcpy(scm_status.msg, "Mise à jour de steamcmd (peut prendre un moment la première fois)…", sizeof scm_status.msg);
        } else if (strstr(line, "Logging in user")) {
            awaiting_login = TRUE; scm_status.need_guard = FALSE; scm_status.error = FALSE;
            g_strlcpy(scm_status.msg, "Connexion à Steam…", sizeof scm_status.msg);
        } else if (awaiting_login && (strstr(line, "Guard") || strstr(line, "code:"))) {
            scm_status.need_guard = TRUE; g_strlcpy(scm_status.msg, "Code Steam Guard requis (reçu par e-mail ou dans l'appli Steam).", sizeof scm_status.msg);
        } else if (awaiting_login && (strstr(line, "FAILED") || strstr(line, "Login Failure") || strstr(line, "Invalid Password") || strstr(line, "Two-factor"))) {
            awaiting_login = FALSE; scm_status.connected = FALSE; scm_status.need_guard = FALSE; scm_status.error = TRUE;
            g_strlcpy(scm_status.msg, "Connexion refusée (identifiant, mot de passe ou code incorrect).", sizeof scm_status.msg);
        } else if (awaiting_login && (strstr(line, "Waiting for user info") || strstr(line, "Logged in OK") || strstr(line, "OK"))) {
            awaiting_login = FALSE; scm_status.connected = TRUE; scm_status.need_guard = FALSE; scm_status.error = FALSE;
            g_strlcpy(scm_status.msg, j->download ? "Connecté — préparation du téléchargement…" : "Connecté à Steam.", sizeof scm_status.msg);
        } else if (strstr(line, "progress:")) {
            double pct; long long done, total;
            if (sscanf(strstr(line, "progress:"), "progress: %lf (%lld / %lld)", &pct, &done, &total) == 3) {
                scm_status.dl_active = TRUE; scm_status.pct = pct; scm_status.done = done; scm_status.total = total;
                g_snprintf(scm_status.msg, sizeof scm_status.msg, "Téléchargement de %s…", scm_status.game);
            }
        } else if (strstr(line, "fully installed") || strstr(line, "Success!")) {
            scm_status.dl_active = FALSE; g_snprintf(scm_status.msg, sizeof scm_status.msg, "%s installé.", scm_status.game);
        } else if (strstr(line, "ERROR!")) {
            scm_status.dl_active = FALSE; scm_status.error = TRUE; g_strlcpy(scm_status.msg, line, sizeof scm_status.msg);
        }
        g_mutex_unlock(&scm_lock);
    }
    if (out) fclose(out); else close(out_fd);
    g_mutex_lock(&scm_write_lock); if (scm_in) { fclose(scm_in); scm_in = NULL; } g_mutex_unlock(&scm_write_lock);
    g_mutex_lock(&scm_lock);
    if (!scm_status.connected && !scm_status.error) {
        scm_status.error = TRUE; scm_status.need_guard = FALSE;
        g_strlcpy(scm_status.msg, "steamcmd s'est arrêté de façon inattendue avant la fin de la connexion.", sizeof scm_status.msg);
    } else if (scm_status.dl_active) {
        scm_status.dl_active = FALSE; scm_status.error = TRUE;
        g_strlcpy(scm_status.msg, "steamcmd s'est arrêté de façon inattendue pendant le téléchargement.", sizeof scm_status.msg);
    }
    g_mutex_unlock(&scm_lock);
    g_free(j);
    return NULL;
}

void fx_steam_login(const char *user, const char *pass) {
    if (!user || !*user) return;
    if (!fx_steam_available()) {
        g_mutex_lock(&scm_lock); scm_status.error = TRUE;
        g_strlcpy(scm_status.msg, "steamcmd introuvable (installe le paquet « steamcmd »).", sizeof scm_status.msg);
        g_mutex_unlock(&scm_lock); return;
    }
    g_mutex_lock(&scm_lock); memset(&scm_status, 0, sizeof scm_status); g_strlcpy(scm_status.msg, "Connexion à Steam…", sizeof scm_status.msg); g_mutex_unlock(&scm_lock);
    g_strlcpy(scm_user, user, sizeof scm_user);
    ScmJob *j = g_new0(ScmJob, 1);
    g_strlcpy(j->user, user, sizeof j->user);
    if (pass) g_strlcpy(j->pass, pass, sizeof j->pass);
    g_thread_unref(g_thread_new("coreboard-scmlogin", scm_job_thread, j));
    JsonBuilder *b = json_builder_new(); json_builder_begin_object(b);
    json_builder_set_member_name(b, "username"); json_builder_add_string_value(b, user);
    json_builder_end_object(b); save_json("steam.json", b); g_object_unref(b);
}

void fx_steam_guard_code(const char *code) {
    if (!code || !*code) return;
    if (!scm_write("%s\n", code)) {
        g_mutex_lock(&scm_lock);
        scm_status.need_guard = FALSE; scm_status.error = TRUE;
        g_strlcpy(scm_status.msg, "La session a expiré avant que le code soit reçu — relance la connexion.", sizeof scm_status.msg);
        g_mutex_unlock(&scm_lock);
    }
}

void fx_steam_logout(void) {
    g_mutex_lock(&scm_lock); memset(&scm_status, 0, sizeof scm_status); g_mutex_unlock(&scm_lock);
    scm_user[0] = 0;
}

gboolean fx_steam_logged_in(void) { g_mutex_lock(&scm_lock); gboolean ok = scm_status.connected; g_mutex_unlock(&scm_lock); return ok; }

const char *fx_steam_username(void) {
    static char buf[64];
    if (!buf[0]) {
        JsonNode *root = load_json("steam.json");
        if (root && JSON_NODE_HOLDS_OBJECT(root)) g_strlcpy(buf, jstr(json_node_get_object(root), "username"), sizeof buf);
        if (root) json_node_free(root);
    }
    return buf;
}

ScmStatus fx_steam_status(void) { g_mutex_lock(&scm_lock); ScmStatus s = scm_status; g_mutex_unlock(&scm_lock); return s; }

void fx_steam_download(gint64 appid, const char *name) {
    if (!fx_steam_logged_in() || !scm_user[0]) return;
    g_mutex_lock(&scm_lock);
    g_strlcpy(scm_status.game, name, sizeof scm_status.game);
    scm_status.dl_active = TRUE; scm_status.pct = 0; scm_status.done = 0; scm_status.total = 0; scm_status.error = FALSE;
    g_snprintf(scm_status.msg, sizeof scm_status.msg, "Préparation du téléchargement de %s…", name);
    g_mutex_unlock(&scm_lock);
    ScmJob *j = g_new0(ScmJob, 1);
    g_strlcpy(j->user, scm_user, sizeof j->user);
    g_strlcpy(j->game, name, sizeof j->game);
    j->appid = appid; j->download = TRUE;
    g_thread_unref(g_thread_new("coreboard-scmdl", scm_job_thread, j));
}

/* ------------------------------------------------------------------ session Epic intégrée (Legendary)
   Legendary est le client Epic Games open-source en ligne de commande. La connexion passe par la page web
   officielle d'Epic (https://legendary.gl/epiclogin) : Coreboard ne voit jamais de mot de passe, seulement un
   code d'autorisation à usage unique que Legendary échange puis garde en cache localement (~/.config/legendary).
   Toute la sortie utile de Legendary (progression, erreurs) est écrite sur stderr, jamais stdout. */
static GMutex epdl_lock; static EpicStatus ep_status;   /* connexion en cours + téléchargement en cours */
static GMutex ep_lock; static EpicLibEntry ep_cache[MAXEPIC]; static int ep_n; static gboolean ep_busy, ep_ready;

gboolean fx_epic_available(void) { return compat_has("legendary"); }

gboolean fx_epic_logged_in(void) {
    gchar *p = g_build_filename(g_get_home_dir(), ".config/legendary/user.json", NULL);
    gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return ok;
}

EpicStatus fx_epic_status(void) { g_mutex_lock(&epdl_lock); EpicStatus s = ep_status; g_mutex_unlock(&epdl_lock); return s; }

static gpointer epic_login_thread(gpointer d) {
    gchar *code = d;
    const char *argv[] = {"legendary", "auth", "--code", code, NULL};
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, NULL, NULL, NULL, NULL);
    g_mutex_lock(&epdl_lock);
    gboolean ok = fx_epic_logged_in();
    ep_status.error = !ok;
    g_strlcpy(ep_status.msg, ok ? "Connecté à Epic Games." : "Connexion refusée (code invalide ou expiré).", sizeof ep_status.msg);
    g_mutex_unlock(&epdl_lock);
    g_free(code);
    return NULL;
}

void fx_epic_login(const char *code) {
    if (!code || !*code || !fx_epic_available()) return;
    g_mutex_lock(&epdl_lock); ep_status.error = FALSE; g_strlcpy(ep_status.msg, "Connexion à Epic Games…", sizeof ep_status.msg); g_mutex_unlock(&epdl_lock);
    g_thread_unref(g_thread_new("coreboard-epiclogin", epic_login_thread, g_strdup(code)));
}

void fx_epic_logout(void) {
    if (fx_epic_available()) {
        const char *argv[] = {"legendary", "auth", "--delete", NULL};
        g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, NULL, NULL, NULL, NULL);
    }
    g_mutex_lock(&epdl_lock); memset(&ep_status, 0, sizeof ep_status); g_mutex_unlock(&epdl_lock);
    g_mutex_lock(&ep_lock); ep_n = 0; ep_ready = FALSE; g_mutex_unlock(&ep_lock);
}

static int epiclib_cmp(const void *a, const void *b) { return g_ascii_strcasecmp(((const EpicLibEntry *)a)->title, ((const EpicLibEntry *)b)->title); }

static gpointer epic_list_thread(gpointer d) {
    (void)d;
    const char *argv[] = {"legendary", "list-games", "--json", NULL};
    gchar *out = run_out(argv);
    EpicLibEntry tmp[MAXEPIC]; int n = 0;
    if (out) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, out, -1, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(jp))) {
            JsonArray *arr = json_node_get_array(json_parser_get_root(jp));
            for (guint i = 0; i < json_array_get_length(arr) && n < MAXEPIC; i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                const char *appname = jstr(o, "app_name"), *title = jstr(o, "app_title");
                if (!*appname) continue;
                EpicLibEntry *e = &tmp[n++];
                g_strlcpy(e->appname, appname, sizeof e->appname); g_strlcpy(e->title, *title ? title : appname, sizeof e->title);
            }
        }
        g_object_unref(jp);
    }
    g_free(out);
    qsort(tmp, n, sizeof(EpicLibEntry), epiclib_cmp);
    g_mutex_lock(&ep_lock); memcpy(ep_cache, tmp, sizeof tmp); ep_n = n; ep_busy = FALSE; ep_ready = TRUE; g_mutex_unlock(&ep_lock);
    return NULL;
}

void fx_epic_refresh(void) {
    if (!fx_epic_available() || !fx_epic_logged_in()) return;
    g_mutex_lock(&ep_lock);
    gboolean go = !ep_busy; ep_busy = TRUE;
    g_mutex_unlock(&ep_lock);
    if (go) g_thread_unref(g_thread_new("coreboard-epiclist", epic_list_thread, NULL));
}

int fx_epic_list(EpicLibEntry *out, int max) {
    g_mutex_lock(&ep_lock);
    int n = MIN(ep_n, max); memcpy(out, ep_cache, n * sizeof(EpicLibEntry));
    g_mutex_unlock(&ep_lock);
    return n;
}

gboolean fx_epic_ready(void) { g_mutex_lock(&ep_lock); gboolean r = ep_ready; g_mutex_unlock(&ep_lock); return r; }

static gpointer epic_dl_thread(gpointer d) {
    gchar *appname = d;
    gint err_fd; GPid pid; GError *gerr = NULL;
    const char *argv[] = {"legendary", "--yes", "install", appname, NULL};
    gboolean ok = g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, &pid, NULL, NULL, &err_fd, &gerr);
    if (!ok) {
        g_mutex_lock(&epdl_lock); ep_status.dl_active = FALSE; ep_status.error = TRUE;
        g_strlcpy(ep_status.msg, "Impossible de lancer legendary.", sizeof ep_status.msg);
        g_mutex_unlock(&epdl_lock);
        g_clear_error(&gerr); g_free(appname); return NULL;
    }
    FILE *errf = fdopen(err_fd, "r");
    char line[512]; gboolean done = FALSE;
    while (errf && fgets(line, sizeof line, errf)) {
        g_strchomp(line);
        if (!*line) continue;
        g_mutex_lock(&epdl_lock);
        if (strstr(line, "Progress:")) {
            double pct;
            if (sscanf(strstr(line, "Progress:"), "Progress: %lf%%", &pct) == 1) {
                ep_status.pct = pct;
                g_snprintf(ep_status.msg, sizeof ep_status.msg, "Téléchargement de %s…", ep_status.game);
            }
        } else if (strstr(line, "Finished installation process")) {
            done = TRUE; ep_status.dl_active = FALSE;
            g_snprintf(ep_status.msg, sizeof ep_status.msg, "%s installé.", ep_status.game);
        } else if (strstr(line, "ERROR") || strstr(line, "FATAL")) {
            ep_status.error = TRUE; g_strlcpy(ep_status.msg, line, sizeof ep_status.msg);
        }
        g_mutex_unlock(&epdl_lock);
    }
    if (errf) fclose(errf); else close(err_fd);
    g_mutex_lock(&epdl_lock);
    if (!done && ep_status.dl_active) {
        ep_status.dl_active = FALSE; ep_status.error = TRUE;
        g_strlcpy(ep_status.msg, "L'installation s'est arrêtée de façon inattendue.", sizeof ep_status.msg);
    }
    g_mutex_unlock(&epdl_lock);
    g_free(appname);
    return NULL;
}

void fx_epic_download(const char *appname, const char *title) {
    if (!fx_epic_logged_in() || !appname || !*appname) return;
    g_mutex_lock(&epdl_lock);
    g_strlcpy(ep_status.game, title, sizeof ep_status.game);
    ep_status.dl_active = TRUE; ep_status.pct = 0; ep_status.error = FALSE;
    g_snprintf(ep_status.msg, sizeof ep_status.msg, "Préparation du téléchargement de %s…", title);
    g_mutex_unlock(&epdl_lock);
    g_thread_unref(g_thread_new("coreboard-epicdl", epic_dl_thread, g_strdup(appname)));
}

/* ------------------------------------------------------------------ bibliothèque du compte GOG via lgogdownloader
   lgogdownloader gère lui-même l'authentification GOG (lancer une fois « lgogdownloader --login » dans un
   terminal) ; Coreboard ne fait que lister le compte et déclencher un téléchargement réel des jeux non installés,
   y compris les jeux obtenus gratuitement sur le site GOG. */
static GMutex gog_lock; static GogLibEntry gog_cache[MAXGOGLIB]; static int gog_n; static gboolean gog_busy;

gboolean fx_gog_available(void) { return compat_has("lgogdownloader"); }

static int goglib_cmp(const void *a, const void *b) { return g_ascii_strcasecmp(((const GogLibEntry *)a)->title, ((const GogLibEntry *)b)->title); }

static gpointer gog_thread(gpointer d) {
    (void)d;
    const char *argv[] = {"lgogdownloader", "--list", "json", NULL};
    gchar *out = run_out(argv);
    GogLibEntry tmp[MAXGOGLIB]; int n = 0;
    if (out) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, out, -1, NULL)) {
            JsonNode *root = json_parser_get_root(jp);
            JsonArray *arr = NULL;
            if (JSON_NODE_HOLDS_ARRAY(root)) arr = json_node_get_array(root);
            else if (JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *ro = json_node_get_object(root);
                if (json_object_has_member(ro, "games") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(ro, "games")))
                    arr = json_object_get_array_member(ro, "games");
            }
            for (guint i = 0; arr && i < json_array_get_length(arr) && n < MAXGOGLIB; i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                const char *slug = jstr(o, "gamename");
                const char *title = jstr(o, "title"); if (!*title) title = slug;
                if (!*slug) continue;
                GogLibEntry *g = &tmp[n++];
                g_strlcpy(g->slug, slug, sizeof g->slug); g_strlcpy(g->title, title, sizeof g->title);
            }
        }
        g_object_unref(jp);
    }
    g_free(out);
    qsort(tmp, n, sizeof(GogLibEntry), goglib_cmp);
    g_mutex_lock(&gog_lock); memcpy(gog_cache, tmp, sizeof tmp); gog_n = n; gog_busy = FALSE; g_mutex_unlock(&gog_lock);
    return NULL;
}

void fx_gog_refresh(void) {
    if (!fx_gog_available()) return;
    g_mutex_lock(&gog_lock);
    gboolean go = !gog_busy; gog_busy = TRUE;
    g_mutex_unlock(&gog_lock);
    if (go) g_thread_unref(g_thread_new("coreboard-gog", gog_thread, NULL));
}

int fx_gog_list(GogLibEntry *out, int max) {
    g_mutex_lock(&gog_lock);
    int n = MIN(gog_n, max); memcpy(out, gog_cache, n * sizeof(GogLibEntry));
    g_mutex_unlock(&gog_lock);
    return n;
}

void fx_gog_download(const GogLibEntry *g) {
    gchar *dir = g_build_filename(g_get_home_dir(), "Games", "GOG", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *pattern = g_strdup_printf("^%s$", g->slug);
    const char *argv[] = {"lgogdownloader", "--download", "--game", pattern, "--include", "installers", "--directory", dir, NULL};
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(pattern); g_free(dir);
}

void fx_init(void) { sc_load(); fx_games_rescan(); }

/* ------------------------------------------------------------------ FitGirl Repacks : Fistgirl
   fistgirl_helper.py gère : recherche sur fitgirl-repacks.site, déchiffrement PrivateBin,
   résolution des liens fuckingfast.co (contournement DNS via DoH Cloudflare),
   et téléchargement reprenable multi-parties. Les sorties JSON sont parsées ligne par ligne. */

/* Chemin de fistgirl_helper.py : DATADIR (install), puis exe_dir/data/ (dev), puis CWD/data/ */
static const char *fg_helper_path(void) {
    static char buf[512];
    if (buf[0]) return buf;   /* déjà résolu */

    /* 1. DATADIR (après make install : ~/.local/share/coreboard/) */
    g_snprintf(buf, sizeof buf, "%s/fistgirl_helper.py", DATADIR);
    if (g_file_test(buf, G_FILE_TEST_IS_REGULAR)) return buf;

    /* 2. À partir du chemin du binaire en cours */
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        gchar *dir = g_path_get_dirname(exe); g_free(exe);
        /* 2a. dev : binaire à la racine du projet (./coreboard), data/ juste à côté */
        g_snprintf(buf, sizeof buf, "%s/data/fistgirl_helper.py", dir);
        if (g_file_test(buf, G_FILE_TEST_IS_REGULAR)) { g_free(dir); return buf; }
        /* 2b. installé : binaire dans PREFIX/bin/, data dans PREFIX/share/coreboard/ */
        g_snprintf(buf, sizeof buf, "%s/../share/coreboard/fistgirl_helper.py", dir);
        if (g_file_test(buf, G_FILE_TEST_IS_REGULAR)) { g_free(dir); return buf; }
        g_free(dir);
    }

    /* 3. Chemin relatif depuis le répertoire de travail (dernier recours) */
    g_strlcpy(buf, "data/fistgirl_helper.py", sizeof buf);
    return buf;
}


/* Exécute fistgirl_helper.py et renvoie stdout (ou NULL en cas d'erreur) */
static gchar *fg_run(const char *const *args) {
    const char *helper = fg_helper_path();
    int argc = 0; while (args[argc]) argc++;
    /* Construit argv: python3 helper arg1 arg2 ... */
    const char **argv = g_new0(const char *, argc + 3);
    argv[0] = "python3"; argv[1] = helper;
    for (int i = 0; i < argc; i++) argv[i + 2] = args[i];
    gchar *out = NULL; gint st = 0;
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, &out, NULL, &st, NULL);
    g_free(argv);
    /* en cas d'échec le helper écrit aussi {"error": …} sur stdout : on le garde pour l'afficher */
    if (!out || !*out) { g_free(out); return NULL; }
    return out;
}

/* ---- Recherche ---- */
static GMutex fg_search_lock;
static FgSearchHit fg_results_cache[MAXFG_RESULTS];
static int fg_nresults;
static char fg_query_str[128];
static gboolean fg_search_busy;
static gboolean fg_search_done;   /* TRUE une fois la première recherche terminée */
static char fg_query_pending[128]; /* requête arrivée pendant une recherche : relancée ensuite */

typedef struct { char query[128]; } FgSearchArg;

static gpointer fg_search_thread(gpointer d) {
    FgSearchArg *a = d;
  again:;
    const char *args[] = {"search", a->query, NULL};
    gchar *out = fg_run(args);
    FgSearchHit tmp[MAXFG_RESULTS]; int n = 0;
    if (out) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, out, -1, NULL)) {
            JsonNode *root = json_parser_get_root(jp);
            if (JSON_NODE_HOLDS_ARRAY(root)) {
                JsonArray *arr = json_node_get_array(root);
                for (guint i = 0; i < json_array_get_length(arr) && n < MAXFG_RESULTS; i++) {
                    JsonObject *o = json_array_get_object_element(arr, i);
                    const char *t = jstr(o, "title"), *u = jstr(o, "page_url");
                    if (!*t || !*u) continue;
                    g_strlcpy(tmp[n].title, t, sizeof tmp[n].title);
                    g_strlcpy(tmp[n].page_url, u, sizeof tmp[n].page_url);
                    n++;
                }
            }
        }
        g_object_unref(jp); g_free(out);
    }
    g_mutex_lock(&fg_search_lock);
    if (fg_query_pending[0]) {                       /* résultats périmés : on enchaîne sur la dernière requête */
        g_strlcpy(a->query, fg_query_pending, sizeof a->query); fg_query_pending[0] = 0;
        g_mutex_unlock(&fg_search_lock);
        goto again;
    }
    memcpy(fg_results_cache, tmp, n * sizeof(FgSearchHit));
    fg_nresults = n; fg_search_busy = FALSE; fg_search_done = TRUE;
    g_mutex_unlock(&fg_search_lock);
    g_free(a);
    return NULL;
}

void fx_fg_search(const char *query) {
    g_mutex_lock(&fg_search_lock);
    gboolean go = !fg_search_busy; fg_search_busy = TRUE; fg_search_done = FALSE;
    g_strlcpy(fg_query_str, query, sizeof fg_query_str);
    fg_nresults = 0;
    if (!go) g_strlcpy(fg_query_pending, query, sizeof fg_query_pending);
    g_mutex_unlock(&fg_search_lock);
    if (!go) return;
    FgSearchArg *a = g_new0(FgSearchArg, 1);
    g_strlcpy(a->query, query, sizeof a->query);
    g_thread_unref(g_thread_new("coreboard-fg-search", fg_search_thread, a));
}

int fx_fg_results(FgSearchHit *out, int max) {
    g_mutex_lock(&fg_search_lock);
    int n = MIN(fg_nresults, max);
    memcpy(out, fg_results_cache, n * sizeof(FgSearchHit));
    g_mutex_unlock(&fg_search_lock);
    return n;
}

gboolean fx_fg_search_busy(void) {
    g_mutex_lock(&fg_search_lock); gboolean b = fg_search_busy; g_mutex_unlock(&fg_search_lock); return b;
}

const char *fx_fg_query(void) {                      /* thread principal uniquement */
    static char q[128];
    g_mutex_lock(&fg_search_lock); g_strlcpy(q, fg_query_str, sizeof q); g_mutex_unlock(&fg_search_lock);
    return q;
}


/* ---- Résolution d'URL → liste de fichiers ---- */
static GMutex fg_resolve_lock;
static gboolean fg_resolved, fg_resolve_busy, fg_resolve_error;
static char fg_game_title[128], fg_resolve_msg[200], fg_page_url[512];
static int fg_optional_count, fg_resolve_gen;
static gchar *fg_resolved_json;    /* JSON complet du job (taille libre : des dizaines de parties) */
static int fg_file_count_cache;

typedef struct { char url[512]; int gen; } FgResolveArg;

static gpointer fg_resolve_thread(gpointer d) {
    FgResolveArg *a = d;
    const char *args[] = {"resolve", a->url, NULL};
    gchar *out = fg_run(args);
    int gen = a->gen;
    g_free(a);
    gboolean ok = FALSE; char title[128] = "", err[200] = ""; int nf = 0, nopt = 0; gchar *jbuf = NULL;
    if (out) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
            JsonObject *o = json_node_get_object(json_parser_get_root(jp));
            const char *t = jstr(o, "title");
            nf = json_object_has_member(o, "file_count") ? (int)json_object_get_int_member(o, "file_count") : 0;
            if (json_object_has_member(o, "optional_files")) nopt = (int)json_array_get_length(json_object_get_array_member(o, "optional_files"));
            if (*t && nf > 0) { g_strlcpy(title, t, sizeof title); ok = TRUE; jbuf = g_strdup(out); }
            else g_strlcpy(err, *jstr(o, "error") ? jstr(o, "error") : "Aucun fichier à télécharger pour ce repack", sizeof err);
        } else g_strlcpy(err, "Réponse illisible du helper FitGirl", sizeof err);
        g_object_unref(jp); g_free(out);
    } else g_strlcpy(err, "Impossible de lancer fistgirl_helper.py (python3 installé ?)", sizeof err);
    g_mutex_lock(&fg_resolve_lock);
    if (gen == fg_resolve_gen) {                         /* une sélection plus récente remplace celle-ci */
        fg_resolved = TRUE; fg_resolve_error = !ok; fg_resolve_busy = FALSE;
        g_strlcpy(fg_resolve_msg, err, sizeof fg_resolve_msg);
        if (ok) {
            g_strlcpy(fg_game_title, title, sizeof fg_game_title);
            g_free(fg_resolved_json); fg_resolved_json = jbuf; jbuf = NULL;
            fg_file_count_cache = nf; fg_optional_count = nopt;
        }
    }
    g_mutex_unlock(&fg_resolve_lock);
    g_free(jbuf);
    return NULL;
}

static void fg_dl_reset_idle(void);

void fx_fg_resolve(const char *url) {
    fg_dl_reset_idle();                                  /* l'état terminé/erreur concernait le jeu précédent */
    g_mutex_lock(&fg_resolve_lock);
    fg_resolved = FALSE; fg_resolve_error = FALSE; fg_resolve_busy = TRUE;
    fg_game_title[0] = '\0'; fg_resolve_msg[0] = 0; fg_file_count_cache = 0; fg_optional_count = 0;
    g_clear_pointer(&fg_resolved_json, g_free);
    g_strlcpy(fg_page_url, url, sizeof fg_page_url);
    int gen = ++fg_resolve_gen;
    g_mutex_unlock(&fg_resolve_lock);
    FgResolveArg *a = g_new0(FgResolveArg, 1);
    g_strlcpy(a->url, url, sizeof a->url); a->gen = gen;
    g_thread_unref(g_thread_new("coreboard-fg-resolve", fg_resolve_thread, a));
}

gboolean fx_fg_resolved(void) { g_mutex_lock(&fg_resolve_lock); gboolean r = fg_resolved; g_mutex_unlock(&fg_resolve_lock); return r; }
int      fx_fg_file_count(void) { g_mutex_lock(&fg_resolve_lock); int n = fg_file_count_cache; g_mutex_unlock(&fg_resolve_lock); return n; }
int      fx_fg_optional_count(void) { g_mutex_lock(&fg_resolve_lock); int n = fg_optional_count; g_mutex_unlock(&fg_resolve_lock); return n; }
gboolean fx_fg_resolve_busy(void) { g_mutex_lock(&fg_resolve_lock); gboolean b = fg_resolve_busy; g_mutex_unlock(&fg_resolve_lock); return b; }
const char *fx_fg_resolve_error(void) {              /* thread principal uniquement */
    static char m[200];
    g_mutex_lock(&fg_resolve_lock); g_strlcpy(m, fg_resolve_msg, sizeof m); g_mutex_unlock(&fg_resolve_lock);
    return m;
}
const char *fx_fg_page_url(void) {                   /* thread principal uniquement */
    static char u[512];
    g_mutex_lock(&fg_resolve_lock); g_strlcpy(u, fg_page_url, sizeof u); g_mutex_unlock(&fg_resolve_lock);
    return u;
}
const char *fx_fg_game_title(void) {                 /* thread principal uniquement */
    static char t[128];
    g_mutex_lock(&fg_resolve_lock); g_strlcpy(t, fg_game_title, sizeof t); g_mutex_unlock(&fg_resolve_lock);
    return t;
}

/* ---- Téléchargement ---- */
static GMutex fg_dl_lock;
static FgDlStatus fg_dl;
static GSubprocess *fg_dl_proc;     /* processus du helper en cours (protégé par fg_dl_lock) */
static gboolean fg_dl_cancelled;


static void fg_dl_reset_idle(void) {
    g_mutex_lock(&fg_dl_lock);
    if (!fg_dl.active && !fg_dl_proc) memset(&fg_dl, 0, sizeof fg_dl);
    g_mutex_unlock(&fg_dl_lock);
}

FgDlStatus fx_fg_dl_status(void) {
    g_mutex_lock(&fg_dl_lock); FgDlStatus s = fg_dl; g_mutex_unlock(&fg_dl_lock); return s;
}

/* Parse une ligne JSON émise par fistgirl_helper.py download et met à jour fg_dl */
static void fg_parse_event(const char *line) {
    if (!line || !*line || *line != '{') return;
    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, line, -1, NULL)) { g_object_unref(jp); return; }
    if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) { g_object_unref(jp); return; }
    JsonObject *o = json_node_get_object(json_parser_get_root(jp));
    const char *ev = jstr(o, "event");
    g_mutex_lock(&fg_dl_lock);
    if (fg_dl_cancelled) { g_mutex_unlock(&fg_dl_lock); g_object_unref(jp); return; }   /* sorties d'après l'annulation */
    if (!g_strcmp0(ev, "job_start")) {
        g_strlcpy(fg_dl.game, jstr(o, "title"), sizeof fg_dl.game);
        g_strlcpy(fg_dl.dest, jstr(o, "dest"), sizeof fg_dl.dest);
        fg_dl.file_count = json_object_has_member(o, "file_count") ? (int)json_object_get_int_member(o, "file_count") : 0;
        fg_dl.file_done = 0; fg_dl.active = TRUE; fg_dl.completed = FALSE; fg_dl.error = FALSE;
        g_strlcpy(fg_dl.msg, "Démarrage…", sizeof fg_dl.msg);
    } else if (!g_strcmp0(ev, "resolving_link")) {
        g_strlcpy(fg_dl.file_name, jstr(o, "file"), sizeof fg_dl.file_name);
        fg_dl.resolving = TRUE;
        g_strlcpy(fg_dl.msg, "Obtention du lien de téléchargement…", sizeof fg_dl.msg);
    } else if (!g_strcmp0(ev, "file_start")) {
        g_strlcpy(fg_dl.file_name, jstr(o, "file"), sizeof fg_dl.file_name);
        fg_dl.file_current = json_object_has_member(o, "file_index") ? (int)json_object_get_int_member(o, "file_index") : fg_dl.file_current;
        fg_dl.pct = 0; fg_dl.file_bytes = 0; fg_dl.file_size = 0; fg_dl.conns = 0; fg_dl.speed_bps = 0;
        g_strlcpy(fg_dl.msg, "Connexion…", sizeof fg_dl.msg);
    } else if (!g_strcmp0(ev, "progress")) {
        fg_dl.resolving = FALSE;
        fg_dl.file_bytes = json_object_has_member(o, "file_done") ? json_object_get_int_member(o, "file_done") : fg_dl.file_bytes;
        fg_dl.file_size = json_object_has_member(o, "file_total") ? json_object_get_int_member(o, "file_total") : fg_dl.file_size;
        fg_dl.conns = json_object_has_member(o, "conns") ? (int)json_object_get_int_member(o, "conns") : fg_dl.conns;
        fg_dl.pct = json_object_has_member(o, "pct") ? json_object_get_double_member(o, "pct") : fg_dl.pct;
        fg_dl.speed_bps = json_object_has_member(o, "speed_bps") ? json_object_get_double_member(o, "speed_bps") : 0;
        fg_dl.game_done = json_object_has_member(o, "game_done") ? json_object_get_int_member(o, "game_done") : fg_dl.game_done;
        fg_dl.game_total = json_object_has_member(o, "game_total") ? json_object_get_int_member(o, "game_total") : fg_dl.game_total;
        g_strlcpy(fg_dl.file_name, jstr(o, "file"), sizeof fg_dl.file_name);
        g_strlcpy(fg_dl.msg, "Téléchargement", sizeof fg_dl.msg);
    } else if (!g_strcmp0(ev, "file_done") || !g_strcmp0(ev, "file_cached")) {
        fg_dl.file_done++;
        fg_dl.pct = 100; fg_dl.resolving = FALSE; fg_dl.conns = 0;
    } else if (!g_strcmp0(ev, "job_completed")) {
        fg_dl.active = FALSE; fg_dl.completed = TRUE;
        g_snprintf(fg_dl.msg, sizeof fg_dl.msg, "Téléchargement terminé : %s", fg_dl.game);
    } else if (json_object_has_member(o, "error")) {
        fg_dl.active = FALSE; fg_dl.error = TRUE;
        g_strlcpy(fg_dl.msg, jstr(o, "error"), sizeof fg_dl.msg);
    }
    g_mutex_unlock(&fg_dl_lock);
    g_object_unref(jp);
}

typedef struct { gchar *job_json; char dest[512]; } FgDlArg;

static gpointer fg_dl_thread(gpointer d) {
    FgDlArg *a = d;

    /* Écrit le JSON du job dans un fichier temporaire au nom unique (aucune collision entre sessions) */
    gchar *tmp = NULL;
    gint tfd = g_file_open_tmp("coreboard-fg-job-XXXXXX.json", &tmp, NULL);
    if (tfd >= 0) close(tfd);
    if (tfd < 0 || !g_file_set_contents(tmp, a->job_json, -1, NULL)) {
        g_mutex_lock(&fg_dl_lock); fg_dl.active = FALSE; fg_dl.error = TRUE; g_strlcpy(fg_dl.msg, "Impossible d'écrire le fichier du job", sizeof fg_dl.msg); g_mutex_unlock(&fg_dl_lock);
        if (tmp) { g_unlink(tmp); g_free(tmp); }
        g_free(a->job_json); g_free(a); return NULL;
    }

    const char *helper = fg_helper_path();
    const char *argv[] = {"python3", helper, "download", tmp, a->dest, NULL};

    GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    g_free(a->job_json); g_free(a);
    if (!proc) {
        g_mutex_lock(&fg_dl_lock); fg_dl.active = FALSE; fg_dl.error = TRUE; g_strlcpy(fg_dl.msg, "Impossible de lancer fistgirl_helper.py", sizeof fg_dl.msg); g_mutex_unlock(&fg_dl_lock);
        g_unlink(tmp); g_free(tmp); return NULL;
    }
    g_mutex_lock(&fg_dl_lock);
    gboolean cancelled = fg_dl_cancelled;
    if (cancelled) g_subprocess_force_exit(proc);            /* annulé pendant le démarrage */
    else fg_dl_proc = g_object_ref(proc);
    g_mutex_unlock(&fg_dl_lock);

    GInputStream *out_stream = g_subprocess_get_stdout_pipe(proc);
    GDataInputStream *dat = g_data_input_stream_new(out_stream);
    gsize len; GError *err = NULL; char *line;
    while ((line = g_data_input_stream_read_line(dat, &len, NULL, &err)) != NULL) {
        fg_parse_event(line);
        g_free(line);
    }
    g_clear_error(&err);
    g_object_unref(dat);
    g_subprocess_wait(proc, NULL, NULL);
    g_unlink(tmp); g_free(tmp);
    /* Si le processus s'est terminé sans job_completed, marquer comme erreur (sauf annulation volontaire) */
    g_mutex_lock(&fg_dl_lock);
    if (fg_dl_proc == proc) g_clear_object(&fg_dl_proc);
    if (fg_dl_cancelled) { fg_dl.active = FALSE; fg_dl.error = FALSE; g_strlcpy(fg_dl.msg, "Annulé", sizeof fg_dl.msg); }
    else if (fg_dl.active) { fg_dl.active = FALSE; fg_dl.error = TRUE; g_strlcpy(fg_dl.msg, "Téléchargement interrompu", sizeof fg_dl.msg); }
    fg_dl_cancelled = FALSE;
    g_mutex_unlock(&fg_dl_lock);
    g_object_unref(proc);
    return NULL;
}

void fx_fg_download(const char *dest_dir) {
    g_mutex_lock(&fg_resolve_lock);
    gchar *jbuf = g_strdup(fg_resolved_json);
    g_mutex_unlock(&fg_resolve_lock);
    if (!jbuf || !*jbuf) { g_free(jbuf); return; }

    g_mutex_lock(&fg_dl_lock);
    /* déjà en cours, ou processus annulé pas encore terminé : on n'en lance pas un second dans le même dossier */
    if (fg_dl.active || fg_dl_proc || fg_dl_cancelled) { g_mutex_unlock(&fg_dl_lock); g_free(jbuf); return; }
    memset(&fg_dl, 0, sizeof fg_dl);
    fg_dl.active = TRUE;
    g_strlcpy(fg_dl.msg, "Initialisation…", sizeof fg_dl.msg);
    g_mutex_unlock(&fg_dl_lock);

    FgDlArg *a = g_new0(FgDlArg, 1);
    a->job_json = jbuf;
    g_strlcpy(a->dest, dest_dir, sizeof a->dest);
    g_thread_unref(g_thread_new("coreboard-fg-dl", fg_dl_thread, a));
}

void fx_fg_cancel(void) {
    g_mutex_lock(&fg_dl_lock);
    if (fg_dl.active) {
        fg_dl.active = FALSE; fg_dl_cancelled = TRUE;
        g_strlcpy(fg_dl.msg, "Annulé", sizeof fg_dl.msg);
        if (fg_dl_proc) g_subprocess_force_exit(fg_dl_proc);   /* arrête vraiment le téléchargement (reprenable plus tard) */
    }
    g_mutex_unlock(&fg_dl_lock);
}

