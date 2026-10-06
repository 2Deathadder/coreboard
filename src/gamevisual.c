/* Coreboard — GameVisual : un fragment shader généré (luminosité, contraste, saturation, gamma, température) */
#include "gamevisual.h"
#include "compat.h"
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const struct { const char *name; double b, c, s, g; int k; double v, sh; } PRE[GV_NPRESETS] = {
    {"Défaut", 1.00, 1.00, 1.00, 1.00, 6500, 0.00, 0.00},
    {"FPS", 1.05, 1.12, 1.15, 1.20, 6800, 0.20, 0.35},          /* ombres relevées et image nette pour repérer les ennemis */
    {"Course", 1.02, 1.10, 1.35, 1.05, 6500, 0.30, 0.25},
    {"Cinéma", 0.98, 1.15, 0.90, 0.95, 6000, 0.00, 0.10},
    {"Vivid", 1.00, 1.10, 1.60, 1.00, 6500, 0.50, 0.20},
    {"Lecture", 0.95, 0.95, 0.85, 1.00, 4500, 0.00, 0.00},        /* lumière bleue réduite */
    {"Monochrome", 1.00, 1.10, 0.00, 1.00, 6500, 0.00, 0.15},
};

static GvState st = {0, 0, 1, 1, 1, 1, 6500, 0, 0};

const char *gv_preset_name(int i) { return i >= 0 && i < GV_NPRESETS ? PRE[i].name : "Manuel"; }
GvState *gv_state(void) { return &st; }

gboolean gv_supported(void) { return g_getenv("HYPRLAND_INSTANCE_SIGNATURE") && compat_has("hyprctl"); }

void gv_select_preset(int i) {
    st.preset = i;
    if (i >= 0 && i < GV_NPRESETS) { st.bright = PRE[i].b; st.contrast = PRE[i].c; st.sat = PRE[i].s; st.gamma = PRE[i].g; st.kelvin = PRE[i].k; st.vibrance = PRE[i].v; st.sharpen = PRE[i].sh; }
}

static char *cfg(const char *f) {
    gchar *d = g_build_filename(g_get_user_config_dir(), "coreboard", NULL);
    g_mkdir_with_parents(d, 0755);
    gchar *p = g_build_filename(d, f, NULL); g_free(d);
    return p;
}

/* couleur d'un corps noir → gains RGB (approximation de Tanner Helland), normalisée à 6500 K = neutre */
static void kelvin_gains(int kelvin, double out[3]) {
    double raw[2][3];
    int ks[2] = {kelvin, 6500};
    for (int i = 0; i < 2; i++) {
        double t = ks[i] / 100.0, r, g, b;
        r = t <= 66 ? 255 : 329.698727446 * pow(t - 60, -0.1332047592);
        g = t <= 66 ? 99.4708025861 * log(t) - 161.1195681661 : 288.1221695283 * pow(t - 60, -0.0755148492);
        b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * log(t - 10) - 305.0447927307;
        raw[i][0] = CLAMP(r, 0, 255) / 255; raw[i][1] = CLAMP(g, 0, 255) / 255; raw[i][2] = CLAMP(b, 0, 255) / 255;
    }
    for (int c = 0; c < 3; c++) out[c] = raw[1][c] > 0 ? raw[0][c] / raw[1][c] : 1;
}

static gboolean is_identity(void) {
    return fabs(st.bright - 1) < 1e-3 && fabs(st.contrast - 1) < 1e-3 && fabs(st.sat - 1) < 1e-3 && fabs(st.gamma - 1) < 1e-3 && st.kelvin == 6500 && fabs(st.vibrance) < 1e-3 && st.sharpen < 1e-3;
}

/* format décimal indépendant de la langue (le shader exige un point) */
static const char *num(double v, char *buf) { return g_ascii_formatd(buf, 32, "%.4f", v); }

static gboolean write_shader(const char *path) {
    double g[3]; kelvin_gains(st.kelvin, g);
    char a[32], b[32], c[32], d[32], e[32], f[32], h[32], i[32], j[32], k[32];
    gchar *src = g_strdup_printf(
        "#version 300 es\nprecision highp float;\nin vec2 v_texcoord;\nuniform sampler2D tex;\nlayout(location = 0) out vec4 fragColor;\n"
        "void main() {\n"
        "    vec4 px = texture(tex, v_texcoord);\n"
        "    vec3 c = px.rgb;\n"
        "    if (%s > 0.0) {\n"                                        /* netteté : masque flou adaptatif sur les 4 voisins (limite les halos) */
        "        vec2 o = 1.0 / vec2(textureSize(tex, 0));\n"
        "        vec3 n = texture(tex, v_texcoord + vec2(o.x, 0.0)).rgb + texture(tex, v_texcoord - vec2(o.x, 0.0)).rgb\n"
        "               + texture(tex, v_texcoord + vec2(0.0, o.y)).rgb + texture(tex, v_texcoord - vec2(0.0, o.y)).rgb;\n"
        "        vec3 d = c - n * 0.25;\n"
        "        c += clamp(d * %s * 1.6, vec3(-0.12), vec3(0.12));\n"
        "    }\n"
        "    c *= vec3(%s, %s, %s);\n"
        "    c = pow(max(c, vec3(0.0)), vec3(1.0 / %s));\n"           /* gamma : > 1 relève les ombres */
        "    c = (c - 0.5) * %s + 0.5;\n"                              /* contraste */
        "    c *= %s;\n"                                               /* luminosité */
        "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
        "    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));\n"
        "    c = mix(vec3(l), c, (%s) * (1.0 + (%s) * (1.0 - (mx - mn))));\n"   /* saturation, puis vibrance : renforce surtout les couleurs ternes */
        "    fragColor = vec4(clamp(c, 0.0, 1.0), px.a);\n}\n",
        num(st.sharpen, a), num(st.sharpen, b), num(g[0], c), num(g[1], d), num(g[2], e), num(st.gamma, f), num(st.contrast, h), num(st.bright, i), num(st.sat, j), num(st.vibrance, k));
    gboolean ok = g_file_set_contents(path, src, -1, NULL);
    g_free(src);
    return ok;
}

static void hypr_shader(const char *path) {
    gchar *lua = g_strdup_printf("hl.config({ decoration = { screen_shader = '%s' } })", path);
    const char *argv[] = {"hyprctl", "eval", lua, NULL};
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, NULL, NULL);
    g_free(lua);
}

static void save(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "enabled"); json_builder_add_int_value(b, st.enabled);
    json_builder_set_member_name(b, "preset"); json_builder_add_int_value(b, st.preset);
    json_builder_set_member_name(b, "bright"); json_builder_add_double_value(b, st.bright);
    json_builder_set_member_name(b, "contrast"); json_builder_add_double_value(b, st.contrast);
    json_builder_set_member_name(b, "sat"); json_builder_add_double_value(b, st.sat);
    json_builder_set_member_name(b, "gamma"); json_builder_add_double_value(b, st.gamma);
    json_builder_set_member_name(b, "kelvin"); json_builder_add_int_value(b, st.kelvin);
    json_builder_set_member_name(b, "vibrance"); json_builder_add_double_value(b, st.vibrance);
    json_builder_set_member_name(b, "sharpen"); json_builder_add_double_value(b, st.sharpen);
    json_builder_end_object(b);
    JsonGenerator *gen = json_generator_new(); JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root); json_generator_set_pretty(gen, TRUE);
    gchar *p = cfg("gamevisual.json"); json_generator_to_file(gen, p, NULL);
    g_free(p); json_node_free(root); g_object_unref(gen); g_object_unref(b);
}

gboolean gv_apply(char *err, size_t n) {
    err[0] = 0;
    save();
    if (!gv_supported()) { g_strlcpy(err, "GameVisual nécessite Hyprland", n); return FALSE; }
    if (!st.enabled || is_identity()) { hypr_shader(""); return TRUE; }
    gchar *path = cfg("gamevisual.frag");
    gboolean ok = write_shader(path);
    if (ok) hypr_shader(path); else g_strlcpy(err, "Écriture du shader impossible", n);
    g_free(path);
    return ok;
}

void gv_init(void) {
    gchar *p = cfg("gamevisual.json"); JsonParser *jp = json_parser_new();
    if (json_parser_load_from_file(jp, p, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(jp));
        #define GI(k, f) if (json_object_has_member(o, k)) st.f = (int)json_object_get_int_member(o, k)
        #define GD(k, f) if (json_object_has_member(o, k)) st.f = json_object_get_double_member(o, k)
        GI("enabled", enabled); GI("preset", preset); GI("kelvin", kelvin); GD("bright", bright); GD("contrast", contrast); GD("sat", sat); GD("gamma", gamma); GD("vibrance", vibrance); GD("sharpen", sharpen);
        #undef GI
        #undef GD
        st.preset = CLAMP(st.preset, 0, GV_MANUAL); st.kelvin = CLAMP(st.kelvin, 3000, 10000);
        st.bright = CLAMP(st.bright, 0.5, 1.5); st.contrast = CLAMP(st.contrast, 0.5, 1.6); st.sat = CLAMP(st.sat, 0, 2); st.gamma = CLAMP(st.gamma, 0.5, 1.8); st.vibrance = CLAMP(st.vibrance, -0.5, 1); st.sharpen = CLAMP(st.sharpen, 0, 1);
    }
    g_object_unref(jp); g_free(p);
    if (st.enabled) { char e[64]; gv_apply(e, sizeof e); }
}
