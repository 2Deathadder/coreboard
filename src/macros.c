/* Coreboard — macros clavier */
#include "macros.h"
#include "compat.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

static Macro mx[MAXMACRO]; static int nmx;
static int rec = -1; static gint64 rec_last;

static char *cfg(const char *dir, const char *f) {
    gchar *d = g_build_filename(g_get_user_config_dir(), dir, NULL);
    g_mkdir_with_parents(d, 0755);
    gchar *p = g_build_filename(d, f, NULL); g_free(d);
    return p;
}

Macro *mx_list(int *n) { *n = nmx; return mx; }
gboolean mx_recording(void) { return rec >= 0; }
int mx_rec_index(void) { return rec; }
gboolean mx_wtype_available(void) { return compat_input() != IB_NONE; }   /* wtype, xdotool ou ydotool */

/* échappement Lua : seuls \ et " (l'UTF-8 passe tel quel ; g_strescape produirait des séquences octales invalides en Lua) */
static gchar *lua_escape(const char *s) {
    GString *o = g_string_new(NULL);
    for (; *s; s++) { if (*s == '\\' || *s == '"') g_string_append_c(o, '\\'); g_string_append_c(o, *s); }
    return g_string_free(o, FALSE);
}

/* ------------------------------------------------------------------ raccourcis Hyprland (fichier chargé par bindings.lua) */
static void write_binds(void) {
    /* raccourcis globaux écrits pour Hyprland seulement ; ailleurs l'utilisateur les déclare dans les réglages du bureau */
    gchar *hd = g_build_filename(g_get_user_config_dir(), "hypr", NULL);
    gboolean hypr = g_file_test(hd, G_FILE_TEST_IS_DIR); g_free(hd);
    if (!hypr) return;
    GString *g = g_string_new("-- Généré par Coreboard (macros) : ne pas modifier à la main.\n");
    gchar *self = g_file_read_link("/proc/self/exe", NULL);
    for (int i = 0; i < nmx; i++) {
        if (!mx[i].bind[0]) continue;
        gchar *qn = g_shell_quote(mx[i].name), *cmd = g_strdup_printf("%s --macro %s", self ? self : "coreboard", qn);
        g_free(qn);
        gchar *esc = lua_escape(cmd), *desc = lua_escape(mx[i].name);
        g_string_append_printf(g, "hl.bind(\"%s\", hl.dsp.exec_cmd(\"%s\"), { description = \"Macro : %s\" })\n", mx[i].bind, esc, desc);
        g_free(esc); g_free(desc); g_free(cmd);
    }
    g_free(self);
    gchar *p = cfg("hypr", "coreboard-macros.lua");
    g_file_set_contents(p, g->str, -1, NULL);
    g_free(p); g_string_free(g, TRUE);
}

void mx_save(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_array(b);
    for (int i = 0; i < nmx; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name"); json_builder_add_string_value(b, mx[i].name);
        json_builder_set_member_name(b, "bind"); json_builder_add_string_value(b, mx[i].bind);
        json_builder_set_member_name(b, "repeat"); json_builder_add_int_value(b, mx[i].repeat);
        json_builder_set_member_name(b, "steps"); json_builder_begin_array(b);
        for (int s = 0; s < mx[i].nsteps; s++) {
            static const char *T[] = {"key", "text", "delay", "cmd"};
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, T[mx[i].steps[s].type]);
            json_builder_set_member_name(b, "value"); json_builder_add_string_value(b, mx[i].steps[s].value);
            json_builder_end_object(b);
        }
        json_builder_end_array(b); json_builder_end_object(b);
    }
    json_builder_end_array(b);
    JsonGenerator *gen = json_generator_new(); JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root); json_generator_set_pretty(gen, TRUE);
    gchar *p = cfg("coreboard", "macros.json"); json_generator_to_file(gen, p, NULL);
    g_free(p); json_node_free(root); g_object_unref(gen); g_object_unref(b);
    write_binds();
}

void mx_init(void) {
    gchar *p = cfg("coreboard", "macros.json"); JsonParser *jp = json_parser_new();
    nmx = 0;
    if (json_parser_load_from_file(jp, p, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(jp))) {
        JsonArray *a = json_node_get_array(json_parser_get_root(jp));
        for (guint i = 0; i < json_array_get_length(a) && nmx < MAXMACRO; i++) {
            JsonObject *o = json_array_get_object_element(a, i); Macro *m = &mx[nmx++];
            memset(m, 0, sizeof *m);
            #define JS(k) (json_object_has_member(o, k) ? json_object_get_string_member(o, k) : "")
            g_strlcpy(m->name, JS("name"), sizeof m->name); g_strlcpy(m->bind, JS("bind"), sizeof m->bind);
            m->repeat = json_object_has_member(o, "repeat") ? (int)json_object_get_int_member(o, "repeat") : 1;
            if (m->repeat < 1) m->repeat = 1;
            if (json_object_has_member(o, "steps")) {
                JsonArray *sa = json_object_get_array_member(o, "steps");
                for (guint s = 0; s < json_array_get_length(sa) && m->nsteps < MAXSTEP; s++) {
                    JsonObject *so = json_array_get_object_element(sa, s); const char *t = json_object_get_string_member(so, "type");
                    MacroStep *st = &m->steps[m->nsteps++];
                    st->type = !strcmp(t, "text") ? MS_TEXT : !strcmp(t, "delay") ? MS_DELAY : !strcmp(t, "cmd") ? MS_CMD : MS_KEY;
                    g_strlcpy(st->value, json_object_get_string_member(so, "value"), sizeof st->value);
                }
            }
            #undef JS
        }
    }
    g_object_unref(jp); g_free(p);
}

int mx_add(const char *name) {
    if (nmx >= MAXMACRO) return -1;
    Macro *m = &mx[nmx]; memset(m, 0, sizeof *m);
    g_strlcpy(m->name, name, sizeof m->name); m->repeat = 1;
    nmx++; mx_save();
    return nmx - 1;
}

void mx_remove(int i) {
    if (i < 0 || i >= nmx) return;
    if (rec == i) rec = -1; else if (rec > i) rec--;
    memmove(&mx[i], &mx[i + 1], (nmx - i - 1) * sizeof(Macro)); nmx--; mx_save();
}

void mx_rename(int i, const char *name) { if (i >= 0 && i < nmx && *name) { g_strlcpy(mx[i].name, name, sizeof mx[i].name); mx_save(); } }

/* ------------------------------------------------------------------ enregistrement */
void mx_rec_start(int i, gboolean append) {
    if (i < 0 || i >= nmx) return;
    if (!append) mx[i].nsteps = 0;
    rec = i; rec_last = 0;
}
void mx_rec_stop(void) { if (rec >= 0) { rec = -1; mx_save(); } }

static MacroStep *push(Macro *m) { return m->nsteps < MAXSTEP ? &m->steps[m->nsteps++] : NULL; }

void mx_rec_key(const char *name, unsigned mods, gunichar ch) {
    if (rec < 0) return;
    Macro *m = &mx[rec];
    gint64 now = g_get_monotonic_time();
    if (rec_last && now - rec_last >= 150000) {                              /* pause entre deux touches → étape de délai */
        MacroStep *d = push(m); if (d) { d->type = MS_DELAY; g_snprintf(d->value, sizeof d->value, "%d", (int)((now - rec_last) / 10000) * 10); }
    }
    rec_last = now;
    gboolean printable = ch >= 32 && ch != 127 && !(mods & (2 | 4 | 8));       /* texte simple (la majuscule est déjà dans le caractère) */
    if (printable) {
        MacroStep *last = m->nsteps ? &m->steps[m->nsteps - 1] : NULL;
        char u[8]; int ul = g_unichar_to_utf8(ch, u); u[ul] = 0;
        if (last && last->type == MS_TEXT && strlen(last->value) + ul < sizeof last->value) strcat(last->value, u);
        else { MacroStep *s = push(m); if (s) { s->type = MS_TEXT; g_strlcpy(s->value, u, sizeof s->value); } }
        return;
    }
    GString *k = g_string_new(NULL);
    gboolean shift = mods & 1;
    if (!strcmp(name, "ISO_Left_Tab")) { name = "Tab"; shift = TRUE; }
    if (shift) g_string_append(k, "shift+");
    if (mods & 2) g_string_append(k, "ctrl+");
    if (mods & 4) g_string_append(k, "alt+");
    if (mods & 8) g_string_append(k, "logo+");
    g_string_append(k, name);
    MacroStep *s = push(m);
    if (s) { s->type = MS_KEY; g_strlcpy(s->value, k->str, sizeof s->value); }
    g_string_free(k, TRUE);
}

void mx_step_remove(int i, int s) {
    if (i < 0 || i >= nmx || s < 0 || s >= mx[i].nsteps) return;
    memmove(&mx[i].steps[s], &mx[i].steps[s + 1], (mx[i].nsteps - s - 1) * sizeof(MacroStep)); mx[i].nsteps--; mx_save();
}
void mx_step_add_delay(int i, int ms) {
    if (i < 0 || i >= nmx || mx[i].nsteps >= MAXSTEP) return;
    MacroStep *s = &mx[i].steps[mx[i].nsteps++]; s->type = MS_DELAY; g_snprintf(s->value, sizeof s->value, "%d", ms); mx_save();
}

const char *mx_step_label(const MacroStep *s, char *buf, size_t n) {
    switch (s->type) {
    case MS_KEY: g_snprintf(buf, n, "Touche  %s", s->value); break;
    case MS_TEXT: g_snprintf(buf, n, "Texte  « %s »", s->value); break;
    case MS_DELAY: g_snprintf(buf, n, "Pause  %s ms", s->value); break;
    default: g_snprintf(buf, n, "Commande  %s", s->value);
    }
    return buf;
}

/* ------------------------------------------------------------------ rejeu */
static void run_cmd(const char *const *argv) { g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL); }

/* saisie simulée via compat.c : wtype (wlroots), xdotool (X11), ydotool (tout bureau, démon ydotoold) */
static void play_step(const MacroStep *s) {
    switch (s->type) {
    case MS_DELAY: g_usleep((gulong)CLAMP(atoi(s->value), 0, 60000) * 1000); break;
    case MS_TEXT: compat_type_text(s->value); break;
    case MS_CMD: { const char *a[] = {"sh", "-c", s->value, NULL}; run_cmd(a); break; }
    default: compat_key_combo(s->value);
    }
}

static void play(Macro *m) {
    Macro copy = *m;                                              /* copie : l'édition pendant le rejeu ne casse rien */
    for (int r = 0; r < copy.repeat; r++) for (int s = 0; s < copy.nsteps; s++) { play_step(&copy.steps[s]); g_usleep(15000); }
}

static gpointer play_thread(gpointer p) { Macro *m = p; play(m); m->playing = FALSE; return NULL; }

void mx_play_async(int i) {
    if (i < 0 || i >= nmx || mx[i].playing || !mx[i].nsteps) return;
    mx[i].playing = TRUE;
    g_thread_unref(g_thread_new("coreboard-macro", play_thread, &mx[i]));
}

int mx_play_by_name(const char *name) {
    mx_init();
    for (int i = 0; i < nmx; i++) if (!g_ascii_strcasecmp(mx[i].name, name)) { play(&mx[i]); return 0; }
    g_printerr("Macro introuvable : %s\n", name);
    return 1;
}
