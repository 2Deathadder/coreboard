/* Coreboard — éclairage RGB du clavier.
   Protocole (MSI MysticLight 1462:1601, repris d'OpenRGB « MSIKeyboard1565 ») : deux feature reports de 64 octets,
   1) sélection de zone (paquet 0x01, masque de zones), 2) mode + images clés de couleur (paquet 0x02). */
#include "rgb.h"
#include <math.h>
#include "music.h"
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <linux/hidraw.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const struct { unsigned vid, pid; const char *name; } KNOWN[] = {
    {0x1462, 0x1601, "MSI MysticLight MS-1565"},
    {0x0db0, 0x1801, "MSI MysticLight"},
};

static RgbState st = {RGBM_STATIC, 100, 50, 0, 1, {0xff0033, 0xff0033, 0xff0033, 0xff0033}, {RGBM_STATIC, RGBM_BREATH, RGBM_WAVE, RGBM_CYCLE}, {50, 50, 50, 50}, 0, 60, 40, 1};
static char devpath[64], devname[64];
static int fd = -1;
static RgbStatus status = RGB_NONE;

static char *cfg_file(void) {
    gchar *dir = g_build_filename(g_get_user_config_dir(), "coreboard", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *p = g_build_filename(dir, "rgb.json", NULL);
    g_free(dir);
    return p;
}

static void detect(void) {
    status = RGB_NONE; devpath[0] = 0;
    GDir *d = g_dir_open("/sys/class/hidraw", 0, NULL); const char *f;
    while (d && (f = g_dir_read_name(d))) {
        gchar *up = g_strdup_printf("/sys/class/hidraw/%s/device/uevent", f), *txt = NULL;
        if (g_file_get_contents(up, &txt, NULL, NULL)) {
            for (guint i = 0; i < G_N_ELEMENTS(KNOWN); i++) {
                char id[40]; g_snprintf(id, sizeof id, "HID_ID=0003:%08X:%08X", KNOWN[i].vid, KNOWN[i].pid);
                if (strstr(txt, id)) { g_snprintf(devpath, sizeof devpath, "/dev/%s", f); g_strlcpy(devname, KNOWN[i].name, sizeof devname); }
            }
        }
        g_free(txt); g_free(up);
    }
    if (d) g_dir_close(d);
    if (!devpath[0]) return;
    status = access(devpath, R_OK | W_OK) == 0 ? RGB_OK : RGB_NOACCESS;
}

RgbStatus rgb_status(void) { return status; }
const char *rgb_device_name(void) { return devname; }
RgbState *rgb_state(void) { return &st; }

static gboolean send_report(const guint8 *pkt, char *err, size_t n) {
    if (fd < 0) fd = open(devpath, O_RDWR | O_CLOEXEC);
    if (fd < 0) { g_snprintf(err, n, "Ouverture de %s impossible : %s", devpath, g_strerror(errno)); status = RGB_NOACCESS; return FALSE; }
    if (ioctl(fd, HIDIOCSFEATURE(64), pkt) < 0) {
        g_snprintf(err, n, "Envoi au clavier refusé : %s", g_strerror(errno));
        close(fd); fd = -1; return FALSE;
    }
    return TRUE;
}

static gboolean select_zone(guint8 mask, char *err, size_t n) {
    guint8 p[64] = {0x02, 0x01, mask};
    return send_report(p, err, n);
}

static void put_kf(guint8 *p, int idx, int t, guint32 c, double k) {
    p[10 + idx * 4] = t;
    p[11 + idx * 4] = (guint8)(((c >> 16) & 255) * k + .5);
    p[12 + idx * 4] = (guint8)(((c >> 8) & 255) * k + .5);
    p[13 + idx * 4] = (guint8)((c & 255) * k + .5);
}

static const int SPEED[] = {2400, 1800, 1200, 800, 500, 300};          /* durée d'un cycle, en centièmes de seconde */
static int speed_idx(int s) { return s <= 12 ? 0 : s <= 25 ? 1 : s <= 40 ? 2 : s <= 60 ? 3 : s <= 80 ? 4 : 5; }
double rgb_cycle_seconds(int speed) { return SPEED[speed_idx(speed)] / 100.0; }

static gboolean send_effect(int mode, guint32 color, int speed, char *err, size_t n) {
    int sp = speed_idx(speed);
    double k = CLAMP(st.brightness, 0, 100) / 100.0;
    guint8 p[64] = {0x02, 0x02};
    p[2] = mode; p[3] = SPEED[sp] & 0xff; p[4] = SPEED[sp] >> 8; p[7] = 15; p[8] = 1; p[9] = st.dir_left ? 1 : 0;
    switch (mode) {
    case RGBM_OFF: put_kf(p, 0, 0, 0, 0); put_kf(p, 1, 100, 0, 0); break;
    case RGBM_STATIC: put_kf(p, 0, 0, color, k); put_kf(p, 1, 100, color, k); break;
    case RGBM_BREATH: put_kf(p, 0, 0, color, k); put_kf(p, 1, 50, 0, 0); put_kf(p, 2, 100, color, k); break;
    case RGBM_CYCLE: {
        static const guint32 C[8] = {0xff0000, 0xff7800, 0xffe600, 0x00ff28, 0x00d2ff, 0x0a50ff, 0xd200ff, 0xff0000};
        static const int T[8] = {0, 14, 28, 42, 57, 71, 85, 100};
        for (int i = 0; i < 8; i++) put_kf(p, i, T[i], C[i], k);
        break; }
    default: {
        static const guint32 C[7] = {0xff0000, 0xffff00, 0x00ff00, 0x00ffff, 0x0000ff, 0xff00ff, 0xff0000};
        static const int T[7] = {0, 17, 33, 50, 67, 83, 100};
        for (int i = 0; i < 7; i++) put_kf(p, i, T[i], C[i], k);
    }
    }
    return send_report(p, err, n);
}

static void save(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mode"); json_builder_add_int_value(b, st.mode);
    json_builder_set_member_name(b, "brightness"); json_builder_add_int_value(b, st.brightness);
    json_builder_set_member_name(b, "speed"); json_builder_add_int_value(b, st.speed);
    json_builder_set_member_name(b, "dir_left"); json_builder_add_int_value(b, st.dir_left);
    json_builder_set_member_name(b, "sync"); json_builder_add_int_value(b, st.sync);
    json_builder_set_member_name(b, "music_style"); json_builder_add_int_value(b, st.music_style);
    json_builder_set_member_name(b, "music_sens"); json_builder_add_int_value(b, st.music_sens);
    json_builder_set_member_name(b, "music_smooth"); json_builder_add_int_value(b, st.music_smooth);
    json_builder_set_member_name(b, "music_glow"); json_builder_add_int_value(b, st.music_glow);
    json_builder_set_member_name(b, "colors"); json_builder_begin_array(b);
    for (int i = 0; i < 4; i++) json_builder_add_int_value(b, st.color[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "zmode"); json_builder_begin_array(b);
    for (int i = 0; i < 4; i++) json_builder_add_int_value(b, st.zmode[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "zspeed"); json_builder_begin_array(b);
    for (int i = 0; i < 4; i++) json_builder_add_int_value(b, st.zspeed[i]);
    json_builder_end_array(b); json_builder_end_object(b);
    JsonGenerator *g = json_generator_new(); JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    gchar *p = cfg_file(); json_generator_to_file(g, p, NULL);
    g_free(p); json_node_free(root); g_object_unref(g); g_object_unref(b);
}

static void load(void) {
    gchar *p = cfg_file(); JsonParser *jp = json_parser_new();
    if (json_parser_load_from_file(jp, p, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(jp));
        #define GI(k, f) if (json_object_has_member(o, k)) st.f = (int)json_object_get_int_member(o, k)
        GI("mode", mode); GI("brightness", brightness); GI("speed", speed); GI("dir_left", dir_left); GI("sync", sync);
        GI("music_style", music_style); GI("music_sens", music_sens); GI("music_smooth", music_smooth); GI("music_glow", music_glow);
        #undef GI
        if (json_object_has_member(o, "zmode")) { JsonArray *a = json_object_get_array_member(o, "zmode"); for (guint i = 0; i < 4 && i < json_array_get_length(a); i++) st.zmode[i] = CLAMP((int)json_array_get_int_element(a, i), 0, RGBM_WAVE); }
        if (json_object_has_member(o, "zspeed")) { JsonArray *a = json_object_get_array_member(o, "zspeed"); for (guint i = 0; i < 4 && i < json_array_get_length(a); i++) st.zspeed[i] = CLAMP((int)json_array_get_int_element(a, i), 0, 100); }
        if (json_object_has_member(o, "colors")) {
            JsonArray *a = json_object_get_array_member(o, "colors");
            for (guint i = 0; i < 4 && i < json_array_get_length(a); i++) st.color[i] = (guint32)json_array_get_int_element(a, i);
        }
        st.mode = CLAMP(st.mode, 0, RGBM_COUNT - 1); st.brightness = CLAMP(st.brightness, 0, 100); st.speed = CLAMP(st.speed, 0, 100); st.music_sens = CLAMP(st.music_sens, 0, 100); st.music_style = CLAMP(st.music_style, 0, 2); st.music_smooth = CLAMP(st.music_smooth, 0, 100); st.music_glow = st.music_glow ? 1 : 0;
    }
    g_object_unref(jp); g_free(p);
}

static gboolean last_sent_valid; static guint32 last_sent[4]; static gint64 last_frame;

gboolean rgb_commit(char *err, size_t n) {
    err[0] = 0;
    if (status == RGB_NONE) { g_strlcpy(err, "Aucun clavier RGB compatible", n); return FALSE; }
    detect();
    if (status != RGB_OK) { g_strlcpy(err, "Accès au clavier refusé (règle udev manquante)", n); return FALSE; }
    gboolean ok = TRUE;
    if (st.mode != RGBM_MUSIC) music_stop();
    if (st.mode == RGBM_MUSIC) {
        music_set_params(st.music_sens, st.music_smooth);
        ok = music_start(err, n);                                /* les images sont envoyées par rgb_music_tick() */
    } else if (st.mode == RGBM_CUSTOM) {
        for (int z = 0; z < 4 && ok; z++) ok = select_zone(1 << z, err, n) && send_effect(st.zmode[z], st.color[z], st.zspeed[z], err, n);
    } else if (st.mode == RGBM_STATIC && !st.sync) {
        for (int z = 0; z < 4 && ok; z++) ok = select_zone(1 << z, err, n) && send_effect(RGBM_STATIC, st.color[z], st.speed, err, n);
    } else {
        ok = select_zone(0x0f, err, n) && send_effect(st.mode, st.color[0], st.speed, err, n);
    }
    last_sent_valid = FALSE;
    save();
    return ok;
}

void rgb_init(void) {
    detect();
    load();
    if (status == RGB_OK) { char e[128]; rgb_commit(e, sizeof e); }
}

/* ------------------------------------------------------------------ synchronisation musicale */
static void hsv_rgb(double h, double v, guint32 *out) {           /* saturation 1 */
    double hh = fmod(h, 360) / 60, x = 1 - fabs(fmod(hh, 2) - 1), r = 0, g = 0, b = 0;
    if (hh < 1) { r = 1; g = x; } else if (hh < 2) { r = x; g = 1; } else if (hh < 3) { g = 1; b = x; }
    else if (hh < 4) { g = x; b = 1; } else if (hh < 5) { r = x; b = 1; } else { r = 1; b = x; }
    *out = ((guint32)(r * v * 255 + .5) << 16) | ((guint32)(g * v * 255 + .5) << 8) | (guint32)(b * v * 255 + .5);
}

/* couleur de chaque zone pour des niveaux audio (0..1), l'enveloppe de battement et le nombre de battements */
static void compute_colors(guint32 out[4], const double lev[4], double env, int beats) {
    double t = g_get_monotonic_time() / 1e6, glow = st.music_glow ? 0.07 : 0;
    for (int z = 0; z < 4; z++) {
        double v = CLAMP(lev[z], 0, 1);
        if (st.music_style == 1) {                                   /* spectre : une teinte par zone qui dérive et suit le son */
            hsv_rgb(z * 70.0 + t * 25 + lev[z] * 50, MAX(glow, v), &out[z]);
        } else if (st.music_style == 2) {                            /* battements : flash de toutes les zones, teinte qui change à chaque coup */
            hsv_rgb(beats * 47.0 + z * 14.0, MAX(glow, CLAMP(0.75 * env + 0.35 * v, 0, 1)), &out[z]);
        } else {                                                     /* couleur choisie modulée par le niveau de la zone */
            double k = MAX(glow, v);
            guint32 c = st.color[st.sync ? 0 : z];
            out[z] = ((guint32)(((c >> 16) & 255) * k + .5) << 16) | ((guint32)(((c >> 8) & 255) * k + .5) << 8) | (guint32)((c & 255) * k + .5);
        }
    }
}

void rgb_music_colors(guint32 out[4]) {
    double lev[4]; music_levels(lev, FALSE);
    compute_colors(out, lev, music_beat_env(), music_beat_count());
}

gboolean rgb_wants_background(void) { return st.mode == RGBM_MUSIC && status == RGB_OK && music_running(); }

void rgb_music_tick(void) {
    if (st.mode != RGBM_MUSIC || status != RGB_OK || !music_running()) return;
    gint64 now = g_get_monotonic_time();
    if (now - last_frame < 30000) return;                          /* ~30 images/s au maximum */
    double lev[4]; guint32 col[4]; char err[64];
    music_set_params(st.music_sens, st.music_smooth);
    music_levels(lev, TRUE);                                        /* pic depuis la dernière image : aucun battement manqué */
    compute_colors(col, lev, music_beat_env(), music_beat_count());
    gboolean force = !last_sent_valid;
    gboolean todo[4], done[4] = {FALSE};
    for (int z = 0; z < 4; z++) {
        int dr = abs((int)((col[z] >> 16) & 255) - (int)((last_sent[z] >> 16) & 255)), dg = abs((int)((col[z] >> 8) & 255) - (int)((last_sent[z] >> 8) & 255)), db = abs((int)(col[z] & 255) - (int)(last_sent[z] & 255));
        todo[z] = force || dr + dg + db >= 6;                        /* rien à envoyer : évite de saturer le contrôleur */
    }
    for (int z = 0; z < 4; z++) {
        if (!todo[z] || done[z]) continue;
        guint8 mask = 1 << z;                                        /* zones de même couleur : un seul envoi */
        for (int y = z + 1; y < 4; y++) if (todo[y] && !done[y] && col[y] == col[z]) mask |= 1 << y;
        if (!select_zone(mask, err, sizeof err) || !send_effect(RGBM_STATIC, col[z], 100, err, sizeof err)) return;
        for (int y = 0; y < 4; y++) if (mask & (1 << y)) { last_sent[y] = col[y]; done[y] = TRUE; }
    }
    last_sent_valid = TRUE; last_frame = now;
}
