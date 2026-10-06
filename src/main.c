/* Coreboard — interface native GTK4 + Cairo/Pango (canevas 1280×720, ratio 16:9 conservé). */
#include <fontconfig/fontconfig.h>
#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <unistd.h>
#include "features.h"
#include "gamevisual.h"
#include "hw.h"
#include "macros.h"
#include "pe.h"
#include "tune.h"
#include "dlss.h"
#include "winrun.h"
#include "music.h"
#include "rgb.h"
#include "compat.h"

#ifndef DATADIR
#define DATADIR "/usr/local/share/coreboard"
#endif
#define APP_ID "dev.coreboard.Coreboard"
#define SW 1280.0
#define SH 720.0

/* palette (relevée sur les captures Armoury Crate) */
#define C_BG 0x050505
#define C_PANEL 0x0b0b0c
#define C_PANEL2 0x131315
#define C_LINE 0x232326
#define C_LINE2 0x3a3a40
#define C_RED 0xff1a3a
#define C_TEXT 0xffffff
#define C_LABEL 0xc4c4c8
#define C_MUTE 0x808086
#define C_OK 0x2be36f

/* ------------------------------------------------------------------ état UI */
static GtkWidget *area, *win;
static HwState hw;
static double mx = -1, my = -1;
static gboolean mdown, mclick, hw_started;
static gpointer active_id;
static int page;
static char toast_msg[320]; static gint64 toast_until;

typedef struct { char key[16], val[32]; gint64 until; } Opt;
static Opt opts[16];

static void opt_set(const char *k, const char *v) {
    Opt *slot = NULL;
    for (int i = 0; i < 16; i++) if (!strcmp(opts[i].key, k)) slot = &opts[i];
    for (int i = 0; i < 16 && !slot; i++) if (opts[i].until < g_get_monotonic_time()) slot = &opts[i];
    if (!slot) slot = &opts[0];
    g_strlcpy(slot->key, k, sizeof slot->key); g_strlcpy(slot->val, v, sizeof slot->val);
    slot->until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
}

static void apply_opts(HwState *s) {
    gint64 now = g_get_monotonic_time();
    for (int i = 0; i < 16; i++) {
        Opt *o = &opts[i];
        if (!o->key[0] || o->until < now) continue;
        if (!strcmp(o->key, "profile")) g_strlcpy(s->prof_cur, o->val, sizeof s->prof_cur);
        else if (!strcmp(o->key, "gpu_mode")) g_strlcpy(s->gm_cur, o->val, sizeof s->gm_cur);
        else if (!strcmp(o->key, "refresh")) s->disp.hz = atoi(o->val);
        else if (!strcmp(o->key, "turbo")) s->turbo = atoi(o->val);
        else if (!strcmp(o->key, "epp")) g_strlcpy(s->epp, o->val, sizeof s->epp);
        else if (!strcmp(o->key, "governor")) g_strlcpy(s->governor, o->val, sizeof s->governor);
        else if (!strcmp(o->key, "mute")) s->muted = atoi(o->val);
        else if (!strcmp(o->key, "volume")) s->volume = atoi(o->val);
        else if (!strcmp(o->key, "brightness")) s->brightness = atoi(o->val);
        else if (!strcmp(o->key, "nightlight")) s->night_on = atoi(o->val);
    }
}

static void show_toast(const char *m) {
    g_strlcpy(toast_msg, m, sizeof toast_msg);
    toast_until = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
    if (area) gtk_widget_queue_draw(area);
}

static gboolean redraw_later(gpointer p) { (void)p; if (area) gtk_widget_queue_draw(area); return G_SOURCE_REMOVE; }

/* redessin différé : une seule minuterie en attente, quel que soit le nombre de demandes */
static guint redraw_timer;
static gboolean redraw_once(gpointer p) { (void)p; redraw_timer = 0; if (area) gtk_widget_queue_draw(area); return G_SOURCE_REMOVE; }
static void schedule_redraw(guint ms) { if (!redraw_timer) redraw_timer = g_timeout_add(ms, redraw_once, NULL); }

static void on_tool_installed(const char *pkg, gboolean ok, const char *why) {
    char b[200];
    if (ok) g_snprintf(b, sizeof b, "%s installé", pkg);
    else g_snprintf(b, sizeof b, "Installation de %s : %s", pkg, why ? why : "échec");
    show_toast(b);
}

/* installation d'un paquet de la distribution en tâche de fond (une demande de mot de passe), avec toast du résultat */
typedef struct { char logical[32], label[48]; gboolean ok; } PkgJob;
static gboolean pkg_busy;
static gboolean pkg_done_idle(gpointer p) {
    PkgJob *j = p; char b[160];
    g_snprintf(b, sizeof b, j->ok ? "%s installé" : "Installation de %s annulée ou impossible", j->label);
    show_toast(b); pkg_busy = FALSE; g_free(j);
    if (area) gtk_widget_queue_draw(area);
    return G_SOURCE_REMOVE;
}
static gpointer pkg_thread(gpointer p) { PkgJob *j = p; const char *l[] = {j->logical, NULL}; j->ok = compat_install_pkgs(l); g_idle_add(pkg_done_idle, j); return NULL; }
static void install_pkg_async(const char *logical, const char *label) {
    if (pkg_busy) return;
    if (!compat_pkg_available(logical)) { char b[160]; g_snprintf(b, sizeof b, "%s n'est pas proposé par %s : installe-le à la main", label, compat_distro_name()); show_toast(b); return; }
    pkg_busy = TRUE;
    PkgJob *j = g_new0(PkgJob, 1); g_strlcpy(j->logical, logical, sizeof j->logical); g_strlcpy(j->label, label, sizeof j->label);
    g_thread_unref(g_thread_new("coreboard-pkg", pkg_thread, j));
}

/* outil de saisie simulée adapté à la session : wtype (Hyprland, Sway…), xdotool (X11), ydotool (GNOME/KDE Wayland) */
static const char *macro_tool_for_session(void) {
    if (!g_getenv("WAYLAND_DISPLAY")) return "xdotool";
    if (compat_win_backend() == WB_HYPRLAND || compat_win_backend() == WB_SWAY) return "wtype";
    return "ydotool";
}

static void on_job(const char *msg, gboolean ok, gpointer d) {
    (void)d;
    if (!ok) { char b[340]; g_snprintf(b, sizeof b, "Échec : %s", msg && *msg ? msg : "erreur inconnue"); show_toast(b); }
    if (area) gtk_widget_queue_draw(area);
}

static void act(const char *key, const char *val) {
    opt_set(key, val);
    hw_apply(key, val, on_job, NULL);
}
static void act_i(const char *key, int v) { char b[16]; g_snprintf(b, sizeof b, "%d", v); act(key, b); }

/* ------------------------------------------------------------------ dessin de base */
static const char *prof_title(const char *n, const char **desc);
static void ask_text(int kind, int i, const char *title, const char *initial);
static void ask_steam_login(void);
#define X0 216.0          /* bord gauche du contenu */
static double LWv = 1280, LHv = 700, kk = 1;   /* taille logique de la scène (remplit toute la fenêtre) et échelle */
#define XR (LWv - 36.0)  /* bord droit du contenu */
#define CW (XR - X0)
#define SBW 188.0         /* largeur de la barre latérale */
#define VY0 20.0          /* haut de la zone visible du canevas (pas de barre de titre : la fenêtre est gérée par Hyprland) */
#define F_SANS 0
#define F_ORB 1
#define F_RAJ 2

static void rgba(cairo_t *cr, guint32 c, double a) {
    cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a);
}

/* carte plate : fond quasi noir, filet gris ; « hot » = léger halo rouge en haut */
static void panel(cairo_t *cr, double x, double y, double w, double h, gboolean hot) {
    cairo_rectangle(cr, x, y, w, h); rgba(cr, C_PANEL, 1); cairo_fill(cr);
    if (hot) {
        cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x, y + h);
        cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.07, 0.17, 0.10); cairo_pattern_add_color_stop_rgba(p, 1, 1, 0.07, 0.17, 0);
        cairo_rectangle(cr, x, y, w, h); cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
    }
    cairo_rectangle(cr, x + .5, y + .5, w - 1, h - 1); rgba(cr, C_LINE, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
}

static double text(cairo_t *cr, const char *s, double x, double y, double px, int weight, int font,
                   guint32 col, double alpha, int align, double spacing) {
    static const char *FAM[] = {"Segoe UI, Noto Sans, Sans", "Orbitron", "Rajdhani"};
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FAM[font]);
    pango_font_description_set_weight(fd, weight);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    if (spacing > 0) {
        PangoAttrList *al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_letter_spacing_new((int)(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, al); pango_attr_list_unref(al);
    }
    pango_layout_set_text(l, s, -1);
    int w, h; pango_layout_get_pixel_size(l, &w, &h);
    double tx = align == 0 ? x : align == 1 ? x - w / 2.0 : x - w;
    if (alpha >= 0) { cairo_move_to(cr, tx, y - h / 2.0); rgba(cr, col, alpha); pango_cairo_show_layout(cr, l); cairo_new_path(cr); }
    g_object_unref(l); pango_font_description_free(fd);
    return w;
}

static double utext(cairo_t *cr, const char *s, double x, double y, double px, int weight, int font,
                    guint32 col, int align, double spacing) {
    gchar *u = g_utf8_strup(s, -1);
    double w = text(cr, u, x, y, px, weight, font, col, 1, align, spacing);
    g_free(u);
    return w;
}

static gboolean hit(double x, double y, double w, double h) { return mx >= x && mx <= x + w && my >= y && my <= y + h; }
static gboolean clicked(double x, double y, double w, double h) {
    if (mclick && hit(x, y, w, h)) { mclick = FALSE; return TRUE; }
    return FALSE;
}
#define WID(x, y) ((gpointer)(guintptr)(1 + (int)(x) * 2000 + (int)(y)))

/* petit libellé de champ (gris clair) */
static void label(cairo_t *cr, const char *s, double x, double y) { text(cr, s, x, y, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0); }
static void value(cairo_t *cr, const char *s, double right, double y) { text(cr, s, right, y, 12.5, 400, F_SANS, C_TEXT, 1, 2, 0); }

/* les trois barres obliques rouges des en-têtes Armoury Crate */
static void slashes(cairo_t *cr, double x, double y, double h, int n, guint32 col, double alpha) {
    for (int i = 0; i < n; i++) {
        double x0 = x + i * 7;
        cairo_move_to(cr, x0 + h * .42 + 3.5, y - h / 2); cairo_line_to(cr, x0 + h * .42 + 7.5, y - h / 2);
        cairo_line_to(cr, x0 + 4, y + h / 2); cairo_line_to(cr, x0, y + h / 2); cairo_close_path(cr);
        rgba(cr, col, alpha); cairo_fill(cr);
    }
}

static void section(cairo_t *cr, const char *s, double x, double y) {
    slashes(cr, x, y, 12, 3, C_RED, 1);
    text(cr, s, x + 32, y, 14, 600, F_SANS, C_TEXT, 1, 0, 0);
}

static void ptitle(cairo_t *cr, const char *s) { text(cr, s, X0, 51, 21, 600, F_SANS, C_TEXT, 1, 0, 0); }

/* ligne « libellé … valeur » ; la variante _bar ajoute le filet de progression violet → rouge */
static void row(cairo_t *cr, double x, double y, double w, const char *l, const char *v) { label(cr, l, x, y); value(cr, v, x + w, y); }
static void row_bar(cairo_t *cr, double x, double y, double w, const char *l, const char *v, double pct) {
    row(cr, x, y, w, l, v);
    cairo_rectangle(cr, x, y + 11, w, 1); rgba(cr, 0x3c3c40, 1); cairo_fill(cr);
    pct = CLAMP(pct, 0, 100);
    if (pct > 0) {
        cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + MAX(w * pct / 100, 1), 0);
        cairo_pattern_add_color_stop_rgb(p, 0, 0x4a / 255.0, 0x3c / 255.0, 0xe8 / 255.0);
        cairo_pattern_add_color_stop_rgb(p, 1, 0xff / 255.0, 0x14 / 255.0, 0x3a / 255.0);
        cairo_rectangle(cr, x, y + 10.5, w * pct / 100, 2); cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
    }
}

static void spark(cairo_t *cr, double x, double y, double w, double h, const float *d, int n, double max, gboolean grid, guint32 col, gboolean fill) {
    if (grid) {
        rgba(cr, 0x1f1f23, 1); cairo_set_line_width(cr, 1);
        for (int i = 1; i < 4; i++) { cairo_move_to(cr, x, y + h * i / 4.0 + .5); cairo_line_to(cr, x + w, y + h * i / 4.0 + .5); cairo_stroke(cr); }
    }
    cairo_new_path(cr);
    for (int i = 0; i < n; i++) {
        double px = x + i * w / (n - 1), py = y + h - CLAMP(d[i] / max, 0, 1) * (h - 6) - 3;
        if (i) cairo_line_to(cr, px, py); else cairo_move_to(cr, px, py);
    }
    cairo_path_t *path = cairo_copy_path(cr);
    if (fill) {
        cairo_line_to(cr, x + w, y + h); cairo_line_to(cr, x, y + h); cairo_close_path(cr);
        cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + h);
        cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.07, 0.17, 0.26); cairo_pattern_add_color_stop_rgba(p, 1, 1, 0.07, 0.17, 0);
        cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
    }
    cairo_new_path(cr); cairo_append_path(cr, path);
    rgba(cr, col, 0.25); cairo_set_line_width(cr, 4); cairo_stroke_preserve(cr);
    rgba(cr, col, 1); cairo_set_line_width(cr, 1.6); cairo_stroke(cr);
    cairo_path_destroy(path);
}

/* ------------------------------------------------------------------ icônes */
static void icon(cairo_t *cr, int i, double cx, double cy, guint32 col, double sc) {
    cairo_save(cr); cairo_new_path(cr); cairo_translate(cr, cx, cy); cairo_scale(cr, sc, sc); rgba(cr, col, 1); cairo_set_line_width(cr, 1.7 / sc);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (i) {
    case 0: cairo_move_to(cr, -11, -1); cairo_line_to(cr, 0, -11); cairo_line_to(cr, 11, -1); cairo_line_to(cr, 11, 11); cairo_line_to(cr, -11, 11); cairo_close_path(cr);
        cairo_move_to(cr, -3, 11); cairo_line_to(cr, -3, 3); cairo_line_to(cr, 3, 3); cairo_line_to(cr, 3, 11); cairo_stroke(cr); break;
    case 1: cairo_arc(cr, 0, 0, 11, 0, 2 * G_PI); cairo_move_to(cr, 0, 0); cairo_line_to(cr, 6, -5); cairo_stroke(cr); break;
    case 2: cairo_rectangle(cr, -12, -8, 24, 15); cairo_move_to(cr, -6, 11); cairo_line_to(cr, 6, 11); cairo_stroke(cr); break;
    case 3: cairo_move_to(cr, -11, -4); cairo_line_to(cr, -6, -4); cairo_line_to(cr, 0, -9); cairo_line_to(cr, 0, 9); cairo_line_to(cr, -6, 4); cairo_line_to(cr, -11, 4); cairo_close_path(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, 2, 0, 6, -G_PI / 4, G_PI / 4); cairo_new_sub_path(cr); cairo_arc(cr, 2, 0, 11, -G_PI / 4, G_PI / 4); cairo_stroke(cr); break;
    case 4: cairo_move_to(cr, -12, 1); cairo_line_to(cr, -6, 1); cairo_line_to(cr, -3, -9); cairo_line_to(cr, 2, 10); cairo_line_to(cr, 5, 1); cairo_line_to(cr, 12, 1); cairo_stroke(cr); break;
    case 5: cairo_arc(cr, 0, 0, 4, 0, 2 * G_PI);
        for (int k = 0; k < 8; k++) { double a = k * G_PI / 4; cairo_move_to(cr, cos(a) * 8, sin(a) * 8); cairo_line_to(cr, cos(a) * 12, sin(a) * 12); }
        cairo_stroke(cr); break;
    case 6: cairo_rectangle(cr, -12, -6, 21, 12); cairo_move_to(cr, 9, -2.5); cairo_line_to(cr, 12, -2.5); cairo_line_to(cr, 12, 2.5); cairo_line_to(cr, 9, 2.5); cairo_stroke(cr); break;
    case 8: cairo_move_to(cr, 0, 10); cairo_line_to(cr, 0, -9); cairo_move_to(cr, -3, -6); cairo_line_to(cr, 0, -10); cairo_line_to(cr, 3, -6);
        cairo_move_to(cr, 0, 4); cairo_line_to(cr, -6, -1); cairo_line_to(cr, -6, -3); cairo_move_to(cr, 0, 0); cairo_line_to(cr, 6, -4); cairo_line_to(cr, 6, -5);
        cairo_new_sub_path(cr); cairo_arc(cr, 0, 10, 2, 0, 2 * G_PI); cairo_new_sub_path(cr); cairo_arc(cr, 6, -7, 2, 0, 2 * G_PI); cairo_stroke(cr); break;
    case 9: cairo_move_to(cr, -5, -5); cairo_line_to(cr, 5, 5); cairo_line_to(cr, 0, 10); cairo_line_to(cr, 0, -10); cairo_line_to(cr, 5, -5); cairo_line_to(cr, -5, 5); cairo_stroke(cr); break;
    case 10: cairo_move_to(cr, -6, -3); cairo_arc(cr, 0, -4, 6, G_PI, 2 * G_PI); cairo_line_to(cr, 6, 4); cairo_arc(cr, 0, 4, 6, 0, G_PI); cairo_close_path(cr);
        cairo_move_to(cr, 0, -10); cairo_line_to(cr, 0, -3); cairo_stroke(cr); break;
    case 11: cairo_rectangle(cr, -12, -7, 24, 14);
        for (int k = 0; k < 4; k++) { cairo_move_to(cr, -8 + k * 5, -2.5); cairo_line_to(cr, -7 + k * 5, -2.5); }
        cairo_move_to(cr, -6, 3); cairo_line_to(cr, 6, 3); cairo_stroke(cr); break;
    case 12: cairo_arc(cr, 0, 2, 9, G_PI, 2 * G_PI); cairo_rectangle(cr, -11, 2, 4, 8); cairo_rectangle(cr, 7, 2, 4, 8); cairo_stroke(cr); break;
    case 13: cairo_move_to(cr, -8, -6); cairo_line_to(cr, 8, -6); cairo_curve_to(cr, 13, -6, 14, 4, 11, 8); cairo_line_to(cr, 8, 8); cairo_line_to(cr, 4, 3);
        cairo_line_to(cr, -4, 3); cairo_line_to(cr, -8, 8); cairo_line_to(cr, -11, 8); cairo_curve_to(cr, -14, 4, -13, -6, -8, -6);
        cairo_move_to(cr, -7, -1); cairo_line_to(cr, -3, -1); cairo_move_to(cr, -5, -3); cairo_line_to(cr, -5, 1);
        cairo_new_sub_path(cr); cairo_arc(cr, 5, -2, 1, 0, 2 * G_PI); cairo_new_sub_path(cr); cairo_arc(cr, 8, 0, 1, 0, 2 * G_PI); cairo_stroke(cr); break;
    case 14: cairo_move_to(cr, 0, -9); cairo_line_to(cr, 11, -3); cairo_line_to(cr, 0, 3); cairo_line_to(cr, -11, -3); cairo_close_path(cr);
        cairo_move_to(cr, -11, 2); cairo_line_to(cr, 0, 8); cairo_line_to(cr, 11, 2); cairo_stroke(cr); break;
    case 15: cairo_arc(cr, 0, 0, 11, 0, 2 * G_PI); cairo_new_sub_path(cr); cairo_arc(cr, 0, 0, 5, 0, 2 * G_PI);
        cairo_move_to(cr, 0, -11); cairo_line_to(cr, 0, -5); cairo_move_to(cr, 9.5, 5.5); cairo_line_to(cr, 4.3, 2.5); cairo_move_to(cr, -9.5, 5.5); cairo_line_to(cr, -4.3, 2.5); cairo_stroke(cr); break;
    case 16: cairo_rectangle(cr, -11, -9, 22, 18); cairo_move_to(cr, -3, -4); cairo_line_to(cr, 5, 0); cairo_line_to(cr, -3, 4); cairo_close_path(cr); cairo_stroke(cr); break;
    default: cairo_arc(cr, 0, 9, 1.4, 0, 2 * G_PI); cairo_move_to(cr, -5, 3); cairo_arc(cr, 0, 9, 7, -G_PI * .82, -G_PI * .18);
        cairo_new_sub_path(cr); cairo_arc(cr, 0, 9, 13, -G_PI * .8, -G_PI * .2); cairo_stroke(cr);
    }
    cairo_restore(cr);
}

/* icônes des modes de fonctionnement : 0 quadrillé, 1 silencieux, 2 performance, 3 turbo */
static int mode_kind(const char *k) {
    if (!strcmp(k, "power-saver") || !strcmp(k, "low-power") || !strcmp(k, "quiet") || !strcmp(k, "cool")) return 1;
    if (!strcmp(k, "balanced-performance")) return 2;
    if (!strcmp(k, "performance")) return 3;
    return 0;
}

static void mode_icon(cairo_t *cr, int kind, double cx, double cy, guint32 col) {
    cairo_save(cr); cairo_translate(cr, cx, cy); rgba(cr, col, 1); cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND); cairo_new_path(cr);
    if (kind == 0) {
        cairo_rectangle(cr, -9, -9, 7, 7); cairo_rectangle(cr, 2, -9, 7, 7); cairo_rectangle(cr, -9, 2, 7, 7); cairo_rectangle(cr, 2, 2, 7, 7); cairo_stroke(cr);
    } else {
        static const double NEEDLE[] = {0, 195, 300, 335};
        double a = NEEDLE[kind] * G_PI / 180;
        cairo_arc(cr, 0, 1, 11, 135 * G_PI / 180, 405 * G_PI / 180); cairo_stroke(cr);
        cairo_move_to(cr, 0, 1); cairo_line_to(cr, cos(a) * 8, 1 + sin(a) * 8); cairo_stroke(cr);
        cairo_arc(cr, 0, 1, 1.6, 0, 2 * G_PI); cairo_fill(cr);
        if (kind == 3) { cairo_move_to(cr, -5, -11); cairo_line_to(cr, 0, -15); cairo_line_to(cr, 5, -11); cairo_stroke(cr); }
    }
    cairo_restore(cr);
}

/* rangée « Mode de fonctionnement » (icône + libellé, soulignement rouge sur l'actif) — hauteur 72 */
static void modes_row(cairo_t *cr, double x, double y, double w) {
    HwState *s = &hw;
    if (!s->nprofs) return;
    double cell = MIN(w / s->nprofs, 128);
    cairo_rectangle(cr, x, y + 72, w, 1); rgba(cr, 0x2c2c30, 1); cairo_fill(cr);
    for (int i = 0; i < s->nprofs; i++) {
        const char *d; const char *t = prof_title(s->profs[i], &d);
        double bx = x + i * cell, cx = bx + cell / 2;
        gboolean on = !strcmp(s->profs[i], s->prof_cur), hov = hit(bx, y, cell, 74);
        if (on) {
            cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + 73);
            cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.07, 0.17, 0); cairo_pattern_add_color_stop_rgba(p, 1, 1, 0.07, 0.17, 0.22);
            cairo_rectangle(cr, bx + 6, y, cell - 12, 73); cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
            cairo_rectangle(cr, bx + 10, y + 71, cell - 20, 2); rgba(cr, C_RED, 1); cairo_fill(cr);
        }
        guint32 col = on ? 0xffffff : (hov ? 0xe0e0e0 : 0xb4b4b8);
        mode_icon(cr, mode_kind(s->profs[i]), cx, y + 22, col);
        text(cr, t, cx, y + 52, 13, on ? 700 : 400, F_SANS, col, 1, 1, 0);
        if (clicked(bx, y, cell, 74)) act("profile", s->profs[i]);
    }
}

/* ------------------------------------------------------------------ widgets */
/* bouton d'action à filet clair (« Réglages système ») */
static gboolean outline_btn(cairo_t *cr, double x, double y, double w, double h, const char *t, double fs) {
    gboolean hov = hit(x, y, w, h);
    cairo_rectangle(cr, x, y, w, h); rgba(cr, hov ? 0x1a1a1c : 0x050505, 1); cairo_fill(cr);
    cairo_rectangle(cr, x + .5, y + .5, w - 1, h - 1); rgba(cr, hov ? 0xffffff : 0xb8b8bc, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    text(cr, t, x + w / 2, y + h / 2, fs, 600, F_SANS, C_TEXT, 1, 1, 0);
    return clicked(x, y, w, h);
}

static gboolean mode_btn(cairo_t *cr, double x, double y, double w, double h, const char *title, const char *desc, gboolean on) {
    gboolean hov = hit(x, y, w, h);
    cairo_rectangle(cr, x, y, w, h); rgba(cr, hov ? 0x131315 : C_PANEL, 1); cairo_fill(cr);
    if (on) {
        cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + h);
        cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.07, 0.17, 0); cairo_pattern_add_color_stop_rgba(p, 1, 1, 0.07, 0.17, 0.24);
        cairo_rectangle(cr, x, y, w, h); cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
        cairo_rectangle(cr, x, y + h - 2, w, 2); rgba(cr, C_RED, 1); cairo_fill(cr);
    }
    cairo_rectangle(cr, x + .5, y + .5, w - 1, h - 1); rgba(cr, hov ? C_LINE2 : C_LINE, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    double ty = desc && *desc ? y + h / 2 - 9 : y + h / 2;
    text(cr, title, x + w / 2, ty, h > 60 ? 18 : 15, on ? 700 : 600, F_SANS, on ? 0xffffff : 0xd8d8dc, 1, 1, 0);
    if (desc && *desc) text(cr, desc, x + w / 2, y + h / 2 + 15, 12, 400, F_SANS, C_MUTE, 1, 1, 0);
    return clicked(x, y, w, h);
}

static gboolean toggle(cairo_t *cr, double x, double y, gboolean on) {
    cairo_rectangle(cr, x, y, 44, 22); rgba(cr, on ? C_RED : 0x2a2a2e, 1); cairo_fill(cr);
    cairo_rectangle(cr, on ? x + 25 : x + 3, y + 3, 16, 16); rgba(cr, on ? 0xffffff : 0x77777c, 1); cairo_fill(cr);
    return clicked(x, y, 44, 22);
}

/* renvoie TRUE quand la valeur change (glissement) ; *released = TRUE au relâchement */
static gboolean slider(cairo_t *cr, double x, double y, double w, double *v, double lo, double hi, gboolean *released) {
    gpointer id = WID(x, y);
    gboolean changed = FALSE;
    if (released) *released = FALSE;
    if (mclick && hit(x - 8, y - 14, w + 16, 34)) { active_id = id; mclick = FALSE; }
    if (active_id == id) {
        if (mdown) { double nv = lo + CLAMP((mx - x) / w, 0, 1) * (hi - lo); if (fabs(nv - *v) > 0.01) { *v = nv; changed = TRUE; } }
        else { active_id = NULL; if (released) *released = TRUE; }
    }
    double p = (*v - lo) / (hi - lo), tx = x + w * p;
    cairo_rectangle(cr, x, y - 1, w, 2); rgba(cr, 0x3c3c40, 1); cairo_fill(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(x, 0, x + MAX(w * p, 1), 0);
    cairo_pattern_add_color_stop_rgb(g, 0, 0x4a / 255.0, 0x3c / 255.0, 0xe8 / 255.0); cairo_pattern_add_color_stop_rgb(g, 1, 1, 0x14 / 255.0, 0x3a / 255.0);
    cairo_rectangle(cr, x, y - 1.5, w * p, 3); cairo_set_source(cr, g); cairo_fill(cr); cairo_pattern_destroy(g);
    cairo_arc(cr, tx, y, 7, 0, 2 * G_PI); rgba(cr, C_RED, 0.3); cairo_fill(cr);
    cairo_arc(cr, tx, y, 4.5, 0, 2 * G_PI); rgba(cr, 0xffffff, 1); cairo_fill(cr);
    return changed;
}

static int segmented(cairo_t *cr, double x, double y, double w, double h, char (*items)[32], const char *(*lab)(const char *), int n, const char *cur) {
    int res = -1; double bw = (w - (n - 1) * 6) / n;
    for (int i = 0; i < n; i++) {
        double bx = x + i * (bw + 6);
        gboolean on = !strcmp(items[i], cur), hov = hit(bx, y, bw, h);
        cairo_rectangle(cr, bx, y, bw, h); rgba(cr, on ? 0x260a10 : (hov ? 0x151517 : 0x0e0e10), 1); cairo_fill(cr);
        cairo_rectangle(cr, bx + .5, y + .5, bw - 1, h - 1); rgba(cr, on ? C_RED : C_LINE, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
        text(cr, lab ? lab(items[i]) : items[i], bx + bw / 2, y + h / 2, 12.5, on ? 700 : 400, F_SANS, on ? 0xffffff : C_LABEL, 1, 1, 0);
        if (clicked(bx, y, bw, h)) res = i;
    }
    return res;
}

static void gauge(cairo_t *cr, double cx, double cy, double r, double rpm, double maxrpm, const char *val) {
    double a0 = 135 * G_PI / 180, sweep = 270 * G_PI / 180, f = CLAMP(rpm / maxrpm, 0, 1);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT); cairo_set_line_width(cr, 6);
    rgba(cr, 0x2a2a2e, 1); cairo_arc(cr, cx, cy, r, a0, a0 + sweep); cairo_stroke(cr);
    if (f > 0.005) {
        cairo_pattern_t *g = cairo_pattern_create_linear(cx - r, cy, cx + r, cy);
        cairo_pattern_add_color_stop_rgb(g, 0, 0x4a / 255.0, 0x3c / 255.0, 0xe8 / 255.0); cairo_pattern_add_color_stop_rgb(g, 1, 1, 0x14 / 255.0, 0x3a / 255.0);
        cairo_set_source(cr, g); cairo_arc(cr, cx, cy, r, a0, a0 + sweep * f); cairo_stroke(cr); cairo_pattern_destroy(g);
    }
    text(cr, val, cx, cy - 4, 30, 500, F_RAJ, C_TEXT, 1, 1, 0);
    text(cr, "RPM", cx, cy + 22, 12, 400, F_SANS, C_MUTE, 1, 1, 1);
}

/* libellés */
static const char *prof_title(const char *n, const char **desc) {
    static const struct { const char *k, *t, *d; } T[] = {
        {"power-saver", "Silencieux", "Économie"}, {"low-power", "Silencieux", "Économie"}, {"quiet", "Silencieux", "Discret"},
        {"cool", "Frais", "Températures"}, {"balanced", "Équilibré", "Standard"}, {"balanced-performance", "Performance", "Équilibré+"},
        {"performance", "Turbo", "Puissance max"}};
    for (guint i = 0; i < G_N_ELEMENTS(T); i++) if (!strcmp(T[i].k, n)) { *desc = T[i].d; return T[i].t; }
    *desc = ""; return n;
}
static const char *gpu_title(const char *n, const char **desc) {
    static const struct { const char *k, *t, *d; } T[] = {
        {"Integrated", "Intégré", "iGPU seul"}, {"integrated", "Intégré", "iGPU seul"}, {"Hybrid", "Hybride", "iGPU + dGPU"}, {"hybrid", "Hybride", "iGPU + dGPU"},
        {"AsusMuxDgpu", "Dédié", "dGPU seul"}, {"nvidia", "Dédié", "dGPU seul"}, {"Vfio", "VFIO", "Passthrough"}};
    for (guint i = 0; i < G_N_ELEMENTS(T); i++) if (!strcmp(T[i].k, n)) { *desc = T[i].d; return T[i].t; }
    *desc = ""; return n;
}
static const char *epp_label(const char *n) {
    static const struct { const char *k, *t; } T[] = {{"performance", "Perf."}, {"balance_performance", "Équ.+"}, {"default", "Défaut"}, {"balance_power", "Éco"}, {"power", "Min"}};
    for (guint i = 0; i < G_N_ELEMENTS(T); i++) if (!strcmp(T[i].k, n)) return T[i].t;
    return n;
}

static void fmt_rate(char *b, size_t n, double v) {
    if (v > 1048576) g_snprintf(b, n, "%.1f Mo/s", v / 1048576); else g_snprintf(b, n, "%.0f Ko/s", v / 1024);
}

/* ------------------------------------------------------------------ Accueil */
/* portable stylisé, centré en (cx, cy) */
static void model_slug(const char *model, char *slug, size_t max) {
    size_t n = 0;
    for (const char *c = model; *c && n + 1 < max; c++) {
        if (g_ascii_isalnum(*c)) slug[n++] = g_ascii_tolower(*c);
        else if (n && slug[n - 1] != '-') slug[n++] = '-';
    }
    while (n && slug[n - 1] == '-') n--;
    slug[n] = 0;
}

/* photo de l'appareil : devices/<modèle>.png dans ~/.local/share/coreboard ou DATADIR (nom = modèle en minuscules, tirets) */
static int import_image(const char *src);
static cairo_surface_t *img; static gboolean tried;     /* photo de l'appareil (rechargée après un import) */
static void device_image_reload(void) { if (img) cairo_surface_destroy(img); img = NULL; tried = FALSE; }

static gboolean device_image(cairo_t *cr, double cx, double cy) {
    if (!tried && hw.model[0]) {
        tried = TRUE;
        char slug[96]; model_slug(hw.model, slug, sizeof slug);
        const char *dirs[] = {g_get_user_data_dir(), DATADIR "/.."};
        for (int i = 0; i < 2 && !img; i++) {
            gchar *f = g_strdup_printf("%s/%scoreboard/devices/%s.png", dirs[i], i ? "" : "", slug);
            if (i) { g_free(f); f = g_strdup_printf("%s/devices/%s.png", DATADIR, slug); }
            cairo_surface_t *sf = cairo_image_surface_create_from_png(f);
            if (cairo_surface_status(sf) == CAIRO_STATUS_SUCCESS) img = sf; else cairo_surface_destroy(sf);
            g_free(f);
        }
    }
    if (!img) return FALSE;
    double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    double k = MIN(400 / iw, 250 / ih);
    cairo_save(cr); cairo_translate(cr, cx - iw * k / 2, cy - ih * k / 2); cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr); cairo_restore(cr);
    return TRUE;
}

static void laptop(cairo_t *cr, double cx, double cy) {
    cairo_save(cr); cairo_translate(cr, cx - 200, cy - 118); cairo_scale(cr, 1.43, 1.43);
    /* écran */
    cairo_rectangle(cr, 50, 6, 200, 118); rgba(cr, 0x2a2a2e, 1); cairo_fill(cr);
    cairo_rectangle(cr, 53, 9, 194, 112);
    cairo_pattern_t *p = cairo_pattern_create_linear(53, 9, 247, 121);
    cairo_pattern_add_color_stop_rgb(p, 0, 0x12 / 255.0, 0x0a / 255.0, 0x3a / 255.0); cairo_pattern_add_color_stop_rgb(p, .55, 0x2a / 255.0, 0x0c / 255.0, 0x5a / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, 0x60 / 255.0, 0x06 / 255.0, 0x2a / 255.0);
    cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
    /* œil stylisé au centre de l'écran */
    cairo_move_to(cr, 118, 74); cairo_curve_to(cr, 135, 40, 175, 40, 185, 62); cairo_curve_to(cr, 160, 54, 145, 64, 122, 84);
    rgba(cr, 0xd9d0ff, 0.9); cairo_set_line_width(cr, 3); cairo_stroke(cr);
    cairo_move_to(cr, 96, 92); cairo_line_to(cr, 204, 92); rgba(cr, C_RED, 0.8); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
    /* base */
    cairo_move_to(cr, 22, 130); cairo_line_to(cr, 278, 130); cairo_line_to(cr, 268, 146); cairo_line_to(cr, 32, 146); cairo_close_path(cr);
    rgba(cr, 0x3a3a3f, 1); cairo_fill(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(32, 0, 268, 0);
    cairo_pattern_add_color_stop_rgb(g, 0, 0x3a / 255.0, 0x6a / 255.0, 0xff / 255.0); cairo_pattern_add_color_stop_rgb(g, .5, 0xa0 / 255.0, 0x30 / 255.0, 0xd0 / 255.0);
    cairo_pattern_add_color_stop_rgb(g, 1, 1, 0x14 / 255.0, 0x3a / 255.0);
    cairo_rectangle(cr, 34, 145, 232, 2); cairo_set_source(cr, g); cairo_fill(cr); cairo_pattern_destroy(g);
    cairo_restore(cr);
}

/* PC de bureau stylisé (écran + tour), même palette que le portable */
static void desktop_pc(cairo_t *cr, double cx, double cy) {
    cairo_save(cr); cairo_translate(cr, cx - 200, cy - 118); cairo_scale(cr, 1.43, 1.43);
    /* écran */
    cairo_rectangle(cr, 18, 10, 190, 112); rgba(cr, 0x2a2a2e, 1); cairo_fill(cr);
    cairo_rectangle(cr, 21, 13, 184, 104);
    cairo_pattern_t *p = cairo_pattern_create_linear(21, 13, 205, 117);
    cairo_pattern_add_color_stop_rgb(p, 0, 0x12 / 255.0, 0x0a / 255.0, 0x3a / 255.0); cairo_pattern_add_color_stop_rgb(p, .55, 0x2a / 255.0, 0x0c / 255.0, 0x5a / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, 0x60 / 255.0, 0x06 / 255.0, 0x2a / 255.0);
    cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
    cairo_move_to(cr, 82, 74); cairo_curve_to(cr, 99, 40, 139, 40, 149, 62); cairo_curve_to(cr, 124, 54, 109, 64, 86, 84);
    rgba(cr, 0xd9d0ff, 0.9); cairo_set_line_width(cr, 3); cairo_stroke(cr);
    cairo_move_to(cr, 60, 92); cairo_line_to(cr, 168, 92); rgba(cr, C_RED, 0.8); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
    /* pied */
    cairo_rectangle(cr, 104, 122, 18, 16); rgba(cr, 0x3a3a3f, 1); cairo_fill(cr);
    cairo_rectangle(cr, 78, 138, 70, 6); rgba(cr, 0x3a3a3f, 1); cairo_fill(cr);
    /* tour */
    cairo_rectangle(cr, 222, 18, 62, 126); rgba(cr, 0x2a2a2e, 1); cairo_fill(cr);
    cairo_rectangle(cr, 222.5, 18.5, 61, 125); rgba(cr, 0x3a3a3f, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    for (int i = 0; i < 3; i++) {                                        /* ventilateurs RGB en façade */
        cairo_arc(cr, 253, 46 + i * 34, 12, 0, 2 * G_PI);
        guint32 c = i == 0 ? 0x3a6aff : i == 1 ? 0xa030d0 : 0xff143a;
        rgba(cr, c, 0.9); cairo_set_line_width(cr, 2.2); cairo_stroke(cr);
        cairo_arc(cr, 253, 46 + i * 34, 3, 0, 2 * G_PI); rgba(cr, c, 0.6); cairo_fill(cr);
    }
    cairo_rectangle(cr, 230, 136, 46, 2); rgba(cr, C_RED, 0.8); cairo_fill(cr);
    cairo_restore(cr);
}

/* type de châssis SMBIOS : portable, convertible, tablette… sinon PC de bureau */
static gboolean is_laptop_chassis(void) {
    static int r = -1;
    if (r < 0) {
        gchar *t = NULL; int ct = 0;
        if (g_file_get_contents("/sys/class/dmi/id/chassis_type", &t, NULL, NULL)) ct = atoi(t);
        g_free(t);
        r = ct == 8 || ct == 9 || ct == 10 || ct == 11 || ct == 14 || ct == 30 || ct == 31 || ct == 32 || ct == 0;  /* 0 : inconnu → portable */
    }
    return r;
}

static void on_photo_chosen(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    gchar *path = g_file_get_path(f); g_object_unref(f);
    if (path && import_image(path) == 0) { device_image_reload(); show_toast("Photo de l'appareil enregistrée"); }
    else if (path) show_toast("Image illisible ou vide");
    g_free(path);
    if (area) gtk_widget_queue_draw(area);
}

/* photo de son PC : n'importe quelle image (le fond blanc est retiré et l'image recadrée) */
static void pick_device_photo(void) {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Photo de ton PC (idéalement sur fond blanc)");
    GtkFileFilter *ff = gtk_file_filter_new(); gtk_file_filter_set_name(ff, "Images"); gtk_file_filter_add_mime_type(ff, "image/*");
    GListStore *fl = g_list_store_new(GTK_TYPE_FILE_FILTER); g_list_store_append(fl, ff);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fl)); gtk_file_dialog_set_default_filter(d, ff);
    gtk_file_dialog_open(d, GTK_WINDOW(win), NULL, on_photo_chosen, NULL);
    g_object_unref(ff); g_object_unref(fl); g_object_unref(d);
}

/* bloc appareil : photo, nom, CPU/GPU, bouton — cy = centre vertical de la photo */
static void home_device(cairo_t *cr, double lx, double cy) {
    HwState *s = &hw;
    gboolean photo = device_image(cr, lx, cy);
    if (!photo) { if (is_laptop_chassis()) laptop(cr, lx, cy); else desktop_pc(cr, lx, cy); }
    if (hit(lx - 200, cy - 125, 400, 250)) {                 /* survol : invite à mettre sa propre photo */
        text(cr, photo ? "Cliquer pour changer la photo" : "Cliquer pour ajouter une photo de ton PC", lx, cy + 135, 11.5, 600, F_SANS, C_LABEL, 1, 1, 0);
    }
    if (clicked(lx - 200, cy - 125, 400, 250)) pick_device_photo();
    gchar *mu = g_utf8_strup(s->model, -1);
    text(cr, mu, lx, cy + 156, 19, 800, F_ORB, C_TEXT, 1, 1, 1);
    g_free(mu);
    gchar *cl = g_strdup(s->cpu_name);
    { char *p; while ((p = strstr(cl, "(R)")) || (p = strstr(cl, "(TM)"))) memmove(p, p + (p[1] == 'R' ? 3 : 4), strlen(p)); }
    text(cr, cl, lx, cy + 182, 11, 400, F_SANS, 0xc4c4c8, 1, 1, 0); g_free(cl);
    text(cr, s->gpu_present ? s->gpu_name : "Aucun GPU détecté", lx, cy + 198, 11, 400, F_SANS, 0xc4c4c8, 1, 1, 0);
    if (outline_btn(cr, lx - 122, cy + 214, 244, 30, "Réglages système", 13)) page = 5;
}

/* boîte « Appareils (n) » — y = ligne de l'en-tête, la boîte commence 14 px plus bas */
static void home_devices(cairo_t *cr, double x, double y, double w, double bh) {
    HwState *s = &hw; char b[64];
    static gboolean dev_open = TRUE;
    struct { int ic; char t[24], sub[40]; } D[4]; int nd = 0;
    if (s->disp.w > 0) { D[nd].ic = 2; g_strlcpy(D[nd].t, "Écran", 24); g_snprintf(D[nd].sub, 40, "%dx%d · %d Hz", s->disp.w, s->disp.h, s->disp.hz); nd++; }
    if (s->bat_present) { D[nd].ic = 6; g_strlcpy(D[nd].t, "Batterie", 24); g_snprintf(D[nd].sub, 40, "%d %%", s->bat_pct); nd++; }
    if (s->ssid[0]) { D[nd].ic = 7; g_strlcpy(D[nd].t, "Réseau", 24); g_strlcpy(D[nd].sub, s->ssid, 40); nd++; }
    if (s->audio_backend[0]) { D[nd].ic = 3; g_strlcpy(D[nd].t, "Audio", 24); g_snprintf(D[nd].sub, 40, s->muted ? "Muet" : "%d %%", s->volume); nd++; }
    slashes(cr, x, y, 12, 3, C_RED, 1);
    g_snprintf(b, sizeof b, "Appareils (%d)", nd); text(cr, b, x + 32, y, 13, 600, F_SANS, C_TEXT, 1, 0, 0);
    double vx = x + w - 50;
    text(cr, "Tout voir", vx, y, 12.5, 400, F_SANS, hit(vx - 80, y - 10, 80, 20) ? 0xffffff : 0xd0d0d4, 1, 2, 0);
    if (clicked(vx - 80, y - 10, 80, 20)) page = 5;
    double ax = x + w - 24;
    cairo_move_to(cr, ax - 7, dev_open ? y + 3 : y - 3); cairo_line_to(cr, ax, dev_open ? y - 4 : y + 4); cairo_line_to(cr, ax + 7, dev_open ? y + 3 : y - 3);
    rgba(cr, 0xd0d0d4, 1); cairo_set_line_width(cr, 1.6); cairo_stroke(cr);
    if (clicked(ax - 16, y - 12, 36, 24)) dev_open = !dev_open;
    if (!dev_open) return;
    double by = y + 14, bx = x;
    cairo_move_to(cr, bx, by + bh - 8); cairo_line_to(cr, bx, by); cairo_line_to(cr, bx + w, by); cairo_line_to(cr, bx + w, by + bh - 8);
    cairo_curve_to(cr, bx + w, by + bh, bx + w - 3, by + bh, bx + w - 8, by + bh); cairo_line_to(cr, bx + 8, by + bh);
    cairo_curve_to(cr, bx + 3, by + bh, bx, by + bh, bx, by + bh - 8);
    rgba(cr, 0xa0a0a6, 0.8); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    if (!nd) text(cr, "Aucun appareil détecté", bx + w / 2, by + bh / 2, 13, 400, F_SANS, C_MUTE, 1, 1, 0);
    double step = nd > 1 ? MIN(140, (w - 44 - 96) / (nd - 1)) : 0, gw = 96 + step * MAX(nd - 1, 0), tx0 = bx + (w - gw) / 2;
    for (int i = 0; i < nd; i++) {
        double tx = tx0 + i * step, ty = by + (bh - 100) / 2;
        cairo_rectangle(cr, tx, ty, 96, 100); rgba(cr, 0xffffff, hit(tx, ty, 96, 100) ? 0.06 : 0.025); cairo_fill(cr);
        icon(cr, D[i].ic, tx + 48, ty + 30, 0xe0e0e4, 1.35);
        text(cr, D[i].t, tx + 48, ty + 68, 12, 600, F_SANS, C_TEXT, 1, 1, 0);
        text(cr, D[i].sub, tx + 48, ty + 85, 10.5, 400, F_SANS, C_MUTE, 1, 1, 0);
    }
}

/* utilisation système (GPU/CPU/ventilateurs/mémoire) + mode de fonctionnement
   ty1/ty2 = ordonnées des titres des deux rangées, ym = en-tête « Mode de fonctionnement » */
static void home_usage(cairo_t *cr, double x0, double w, double ty1, double ty2, double ym) {
    HwState *s = &hw; char b[128], b2[64];
    double cw = (w - 45) / 2, gx = x0, cx = x0 + cw + 45, y;
    section(cr, "Utilisation système", gx, ty1 - 37);
    /* GPU */
    text(cr, "GPU", gx, ty1, 30, 500, F_RAJ, C_TEXT, 1, 0, 0); slashes(cr, gx + cw - 44, ty1 - 5, 10, 4, 0xffffff, 0.22);
    y = ty1 + 35;
    if (s->gpu_suspended) {
        row(cr, gx, y, cw, "État", "En veille"); y += 26;
    } else {
        if (s->gpu_clock > 0) g_snprintf(b, sizeof b, "%.0f MHz", s->gpu_clock); else g_strlcpy(b, "--", sizeof b);
        row_bar(cr, gx, y, cw, "Fréquence", b, s->gpu_clock / 25); y += 34;
        g_snprintf(b, sizeof b, "%.0f %%", s->gpu_util); row_bar(cr, gx, y, cw, "Utilisation", b, s->gpu_util); y += 32;
    }
    if (s->gpu_mem_total > 0) { g_snprintf(b, sizeof b, "%.1f / %.1f Go", s->gpu_mem_used / 1024, s->gpu_mem_total / 1024); row(cr, gx, y, cw, "Mémoire", b); y += 19; }
    if (s->gpu_temp > 0) { g_snprintf(b, sizeof b, "%.0f °C", s->gpu_temp); row(cr, gx, y, cw, "Température", b); y += 19; }
    if (s->gpu_power > 0) { g_snprintf(b, sizeof b, "%.1f W", s->gpu_power); row(cr, gx, y, cw, "Puissance", b); y += 19; }
    if (!s->gpu_present) row(cr, gx, y, cw, "Aucun GPU", "- -");
    /* CPU */
    text(cr, "CPU", cx, ty1, 30, 500, F_RAJ, C_TEXT, 1, 0, 0); slashes(cr, cx + cw - 44, ty1 - 5, 10, 4, 0xffffff, 0.22);
    y = ty1 + 35;
    g_snprintf(b, sizeof b, "%.0f MHz", s->cpu_freq); row_bar(cr, cx, y, cw, "Fréquence", b, s->cpu_freq_max > 0 ? 100 * s->cpu_freq / s->cpu_freq_max : 0); y += 34;
    g_snprintf(b, sizeof b, "%.0f %%", s->cpu_usage); row_bar(cr, cx, y, cw, "Utilisation", b, s->cpu_usage); y += 32;
    if (s->cpu_has_temp) { g_snprintf(b, sizeof b, "%.0f °C", s->cpu_temp); row(cr, cx, y, cw, "Température", b); y += 19; }
    g_snprintf(b, sizeof b, "%d", s->threads); row(cr, cx, y, cw, "Cœurs logiques", b); y += 19;
    if (s->governor[0]) row(cr, cx, y, cw, "Gouverneur", s->governor);
    /* Ventilateurs */
    text(cr, "Ventilateurs", gx, ty2, 30, 500, F_RAJ, C_TEXT, 1, 0, 0); slashes(cr, gx + cw - 44, ty2 - 5, 10, 4, 0xffffff, 0.22);
    y = ty2 + 36;
    if (!s->nfans) row(cr, gx, y, cw, "Aucun capteur", "- -");
    for (int i = 0; i < s->nfans && i < 6; i++) {
        g_snprintf(b, sizeof b, "%d RPM", s->fans[i].rpm);
        if (i < 2) { row_bar(cr, gx, y, cw, s->fans[i].label, b, s->fans[i].rpm / 65.0); y += i == 1 ? 30 : 34; }
        else { row(cr, gx, y, cw, s->fans[i].label, b); y += 19; }
    }
    /* Mémoire */
    text(cr, "Mémoire", cx, ty2, 30, 500, F_RAJ, C_TEXT, 1, 0, 0); slashes(cr, cx + cw - 44, ty2 - 5, 10, 4, 0xffffff, 0.22);
    y = ty2 + 36;
    g_snprintf(b, sizeof b, "%.1f / %.1f Go", s->disk_used, s->disk_total); row_bar(cr, cx, y, cw, "Stockage", b, s->disk_total ? 100 * s->disk_used / s->disk_total : 0); y += 34;
    g_snprintf(b, sizeof b, "%.1f / %.1f Go", s->ram_used / 1024, s->ram_total / 1024); row_bar(cr, cx, y, cw, "RAM", b, s->ram_pct); y += 30;
    fmt_rate(b2, sizeof b2, s->net_down); fmt_rate(b, sizeof b, s->net_up);
    { char n[128]; g_snprintf(n, sizeof n, "↓ %s  ↑ %s", b2, b); row(cr, cx, y, cw, "Réseau", n); y += 19; }
    { int m = (int)(s->uptime / 60); g_snprintf(b, sizeof b, "%d h %02d min", m / 60, m % 60); row(cr, cx, y, cw, "Disponibilité", b); }
    /* Mode de fonctionnement */
    section(cr, "Mode de fonctionnement", gx, ym);
    if (outline_btn(cr, x0 + w - 132, ym - 11, 132, 24, "Réglages avancés", 11.5)) page = 1;
    if (s->nprofs) modes_row(cr, gx, ym + 26, w);
    else text(cr, "Aucun profil d'alimentation disponible", gx, ym + 64, 13, 400, F_SANS, C_MUTE, 1, 0, 0);
}

static double page_h, scroll_y, scroll_max;   /* défilement des pages longues */
static gboolean home_compact;   /* fenêtre haute (demi-écran) : disposition empilée */

static void page_home(cairo_t *cr) {
    ptitle(cr, "Accueil");
    if (home_compact) {
        double cxm = X0 + CW / 2;
        home_device(cr, cxm, 172);
        home_devices(cr, X0, 428, CW, 140);
        home_usage(cr, X0, CW, 659, 876, 1072);
    } else {
        double e = MAX(0, LHv - 700), gx = 731;
        home_device(cr, 447, 200);
        home_devices(cr, X0, 478 + e * 0.7, 474, 150);
        home_usage(cr, gx, XR - gx, 133, 350 + e * 0.35, 546 + e * 0.7);
    }
}

/* ------------------------------------------------------------------ Performances */
static void page_perf(cairo_t *cr) {
    HwState *s = &hw; char b[96];
    ptitle(cr, "Performances");
    double y0 = 100;
    if (s->nprofs) {
        section(cr, "Mode de fonctionnement", X0, 100);
        modes_row(cr, X0, 122, CW);
        y0 = 216;
    }
    double cw = (CW - 28) / 3, ch = 270, x0 = X0;
    /* CPU */
    panel(cr, x0, y0, cw, ch, FALSE);
    section(cr, "Processeur", x0 + 18, y0 + 26);
    double ry = y0 + 66;
    if (s->turbo >= 0) {
        text(cr, "Turbo Boost", x0 + 18, ry, 14, 400, F_SANS, C_TEXT, 1, 0, 0);
        if (toggle(cr, x0 + cw - 62, ry - 11, s->turbo == 1)) act_i("turbo", !s->turbo);
        ry += 44;
    }
    if (s->nepp || s->ngov) {
        gboolean epp = s->nepp > 0;
        text(cr, epp ? "Préférence énergie" : "Gouverneur", x0 + 18, ry, 14, 400, F_SANS, C_TEXT, 1, 0, 0);
        char (*items)[32] = epp ? s->epps : (char (*)[32])s->governors;
        char tmp[MAXCH][32]; int n = epp ? s->nepp : s->ngov;
        if (!epp) { for (int i = 0; i < n; i++) g_strlcpy(tmp[i], s->governors[i], 32); items = tmp; }
        int r = segmented(cr, x0 + 18, ry + 16, cw - 36, 28, items, epp ? epp_label : NULL, n, epp ? s->epp : s->governor);
        if (r >= 0) act(epp ? "epp" : "governor", items[r]);
        ry += 68;
    }
    row(cr, x0 + 18, ry, cw - 36, "Gouverneur", s->governor); ry += 28;
    g_snprintf(b, sizeof b, "%.2f / %.2f GHz", s->cpu_freq / 1000, s->cpu_freq_max / 1000); row(cr, x0 + 18, ry, cw - 36, "Fréquence moy. / max", b); ry += 28;
    g_snprintf(b, sizeof b, "%d", s->threads); row(cr, x0 + 18, ry, cw - 36, "Cœurs logiques", b);
    if (s->turbo >= 0 || s->nepp || s->ngov) text(cr, "Turbo / énergie : élévation pkexec", x0 + 18, y0 + ch - 16, 10.5, 400, F_SANS, 0x5a5a60, 1, 0, 0);
    /* ventilateurs */
    for (int i = 0; i < 2; i++) {
        double x = X0 + (i + 1) * (cw + 14);
        panel(cr, x, y0, cw, ch, FALSE);
        section(cr, i < s->nfans ? s->fans[i].label : "Ventilateur", x + 18, y0 + 26);
        if (i < s->nfans) { g_snprintf(b, sizeof b, "%d", s->fans[i].rpm); gauge(cr, x + cw / 2, y0 + 150, 70, s->fans[i].rpm, 6500, b); }
        else text(cr, "Aucun capteur", x + cw / 2, y0 + 140, 14, 400, F_SANS, C_MUTE, 1, 1, 0);
    }
    /* températures */
    double ty = y0 + ch + 14, tw = (CW - 14) / 2.0;
    for (int i = 0; i < 2; i++) {
        double x = X0 + i * (tw + 14), t = i ? s->gpu_temp : s->cpu_temp; gboolean ok = i ? (s->gpu_temp > 0) : s->cpu_has_temp;
        panel(cr, x, ty, tw, 70, FALSE);
        if (ok) g_snprintf(b, sizeof b, "%.0f °C", t); else g_snprintf(b, sizeof b, "%s", i && s->gpu_suspended ? "Veille" : "N/D");
        row_bar(cr, x + 18, ty + 26, tw - 36, i ? "Température GPU" : "Température CPU", b, ok ? t : 0);
    }
}

/* ------------------------------------------------------------------ Écran & GPU */
static void page_display(cairo_t *cr) {
    HwState *s = &hw; char b[64];
    ptitle(cr, "Écran & GPU");
    double y = 100;
    if (s->ngm) {
        section(cr, "Mode GPU", X0, y + 6);
        double bw = (CW - (s->ngm - 1) * 10) / s->ngm;
        static char pending[32]; static gint64 pend_until;
        for (int i = 0; i < s->ngm; i++) {
            const char *d; const char *t = gpu_title(s->gms[i], &d);
            gboolean armed = pend_until > g_get_monotonic_time() && !strcmp(pending, s->gms[i]);
            if (mode_btn(cr, X0 + i * (bw + 10), y + 26, bw, 74, t, armed ? "Clique pour confirmer" : d, !strcmp(s->gms[i], s->gm_cur))) {
                if (armed) { act("gpu_mode", s->gms[i]); pending[0] = 0; show_toast("Mode GPU changé : effectif après déconnexion"); }
                else { g_strlcpy(pending, s->gms[i], sizeof pending); pend_until = g_get_monotonic_time() + 4 * G_USEC_PER_SEC; g_timeout_add(4100, redraw_later, NULL); }
            }
        }
        text(cr, "Un changement de mode GPU nécessite de fermer la session.", X0, y + 114, 11.5, 400, F_SANS, 0x66666a, 1, 0, 0);
        y += 148;
    }
    if (s->disp.nmodes > 1) {
        section(cr, "Fréquence de rafraîchissement", X0, y + 6);
        for (int i = 0; i < s->disp.nmodes; i++) {
            int hz = s->disp.modes[i]; g_snprintf(b, sizeof b, "%d Hz", hz);
            if (mode_btn(cr, X0 + i * 170, y + 26, 160, 74, b, hz >= 100 ? "Fluide" : "Économie", hz == s->disp.hz)) act_i("refresh", hz);
        }
        y += 122;
    }
    double tw = (CW - 14) / 2.0; int col = 0;
    if (s->brightness >= 0) {
        panel(cr, X0, y, tw, 84, FALSE);
        label(cr, "Luminosité", X0 + 18, y + 26);
        static double bv; static gint64 last;
        if (active_id == NULL) bv = s->brightness;
        g_snprintf(b, sizeof b, "%.0f %%", bv); value(cr, b, X0 + tw - 18, y + 26);
        gboolean rel;
        if (slider(cr, X0 + 20, y + 58, tw - 40, &bv, 1, 100, &rel) && g_get_monotonic_time() - last > 120000) { last = g_get_monotonic_time(); act_i("brightness", (int)bv); }
        if (rel) act_i("brightness", (int)bv);
        col = 1;
    }
    if (s->night_supported) {
        double x = X0 + col * (tw + 14);
        panel(cr, x, y, tw, 84, FALSE);
        text(cr, "Filtre lumière bleue", x + 18, y + 30, 14, 600, F_SANS, C_TEXT, 1, 0, 0);
        text(cr, "Réduit la lumière bleue de l'écran", x + 18, y + 58, 11.5, 400, F_SANS, 0x66666a, 1, 0, 0);
        if (toggle(cr, x + tw - 62, y + 31, s->night_on)) act_i("nightlight", !s->night_on);
    }
    if (!s->ngm && s->disp.nmodes <= 1 && s->brightness < 0 && !s->night_supported)
        text(cr, "Aucun réglage d'écran ou de GPU disponible sur cette machine.", X0, 118, 14, 400, F_SANS, C_MUTE, 1, 0, 0);
}

/* ------------------------------------------------------------------ Audio */
static void page_audio(cairo_t *cr) {
    HwState *s = &hw; char b[32];
    ptitle(cr, "Audio");
    if (!s->audio_backend[0]) { text(cr, "Aucun serveur audio détecté (pactl / wpctl).", X0, 118, 14, 400, F_SANS, C_MUTE, 1, 0, 0); return; }
    panel(cr, X0, 100, 564, 190, FALSE);
    section(cr, "Sortie par défaut", X0 + 18, 126);
    label(cr, "Volume", X0 + 18, 166);
    static double vv; static gint64 last;
    if (active_id == NULL) vv = MIN(s->volume, 100);
    g_snprintf(b, sizeof b, "%.0f %%", vv); value(cr, b, X0 + 546, 166);
    gboolean rel;
    if (slider(cr, X0 + 20, 204, 524, &vv, 0, 100, &rel) && g_get_monotonic_time() - last > 100000) { last = g_get_monotonic_time(); act_i("volume", (int)vv); }
    if (rel) act_i("volume", (int)vv);
    text(cr, "Muet", X0 + 18, 252, 14, 400, F_SANS, C_TEXT, 1, 0, 0);
    if (toggle(cr, X0 + 500, 241, s->muted)) act_i("mute", !s->muted);
}

/* ------------------------------------------------------------------ Système */
static void page_system(cairo_t *cr) {
    HwState *s = &hw; char b[64];
    ptitle(cr, "Système");
    double tw = (CW - 14) / 2.0, th = MAX(184, (VY0 + LHv - 100 - 14 - 94 - 20 - 14) / 2);
    struct { const char *t; const float *a, *b; double v; } C[4] = {
        {"Utilisation CPU", s->h_cpu, NULL, s->cpu_usage}, {"Utilisation GPU", s->h_gpu, NULL, s->gpu_util},
        {"Mémoire", s->h_ram, NULL, s->ram_pct}, {"Températures (CPU / GPU)", s->h_ctemp, s->h_gtemp, 0}};
    for (int i = 0; i < 4; i++) {
        double x = X0 + (i % 2) * (tw + 14), y = 100 + (i / 2) * (th + 14);
        panel(cr, x, y, tw, th, FALSE);
        section(cr, C[i].t, x + 18, y + 26);
        if (i < 3) g_snprintf(b, sizeof b, "%.0f %%", C[i].v); else g_snprintf(b, sizeof b, "%.0f° / %.0f°", s->cpu_temp, s->gpu_temp);
        value(cr, b, x + tw - 18, y + 26);
        spark(cr, x + 18, y + 46, tw - 36, th - 62, C[i].a, HIST, 100, TRUE, C_RED, TRUE);
        if (C[i].b) spark(cr, x + 18, y + 46, tw - 36, th - 62, C[i].b, HIST, 100, FALSE, 0xffffff, FALSE);
    }
    double cy = 100 + 2 * th + 28 + 14;
    panel(cr, X0, cy, CW, 80, FALSE);
    section(cr, "Cœurs", X0 + 18, cy + 40);
    int n = s->ncores; double bw = MIN(22, 760.0 / MAX(n, 1) - 4);
    for (int i = 0; i < n; i++) {
        double bx = XR - 18 - (n - i) * (bw + 4) + 4, hgt = MAX(2, s->cores[i] * 0.5);
        cairo_rectangle(cr, bx, cy + 66 - hgt, bw, hgt); rgba(cr, C_RED, 1); cairo_fill(cr);
    }
}

/* ------------------------------------------------------------------ Réglages */
static void page_about(cairo_t *cr) {
    HwState *s = &hw; char v[160];
    ptitle(cr, "Réglages");
    panel(cr, X0, 100, 640, 430, FALSE);
    section(cr, "À propos et capacités détectées", X0 + 18, 126);
    const char *rows[12][2]; int n = 0;
    g_snprintf(v, sizeof v, "%s %s", s->vendor, s->model); gchar *dev = g_strdup(v);
    gchar *ses = g_strdup_printf("%s · %s", s->session, s->wm);
    gchar *cpu = g_strdup_printf("%s", s->turbo >= 0 ? (s->nepp ? "Turbo + EPP" : (s->ngov ? "Turbo + gouverneur" : "Turbo")) : "Lecture seule");
    gchar *gpu = g_strdup_printf("%s (%d)", s->gpu_present ? s->gpu_vendor : "non détecté", s->gpu_count);
    gchar *fan = g_strdup_printf("%d capteur(s), lecture seule", s->nfans);
    rows[n][0] = "Appareil"; rows[n++][1] = dev;
    rows[n][0] = "Noyau"; rows[n++][1] = s->kernel;
    rows[n][0] = "Session"; rows[n++][1] = ses;
    rows[n][0] = "Profils d'alimentation"; rows[n++][1] = s->prof_backend[0] ? s->prof_backend : "non disponible";
    rows[n][0] = "CPU"; rows[n++][1] = cpu;
    rows[n][0] = "GPU"; rows[n++][1] = gpu;
    rows[n][0] = "Mode GPU"; rows[n++][1] = s->gm_backend[0] ? s->gm_backend : "non disponible";
    rows[n][0] = "Écran"; rows[n++][1] = s->disp.backend[0] ? s->disp.backend : "non disponible";
    rows[n][0] = "Ventilateurs"; rows[n++][1] = fan;
    rows[n][0] = "Luminosité"; rows[n++][1] = s->brightness >= 0 ? "oui" : "non";
    rows[n][0] = "Audio"; rows[n++][1] = s->audio_backend[0] ? s->audio_backend : "non disponible";
    rows[n][0] = "Implémentation"; rows[n++][1] = "C natif · GTK4 + Cairo";
    for (int i = 0; i < n; i++) {
        double y = 166 + i * 29;
        row(cr, X0 + 18, y, 604, rows[i][0], rows[i][1]);
        cairo_rectangle(cr, X0 + 18, y + 13, 604, 1); rgba(cr, 0x1c1c1f, 1); cairo_fill(cr);
    }
    g_free(dev); g_free(ses); g_free(cpu); g_free(gpu); g_free(fan);
}

/* ------------------------------------------------------------------ Appareils / Scénarios / Jeux */


static void text_fit(cairo_t *cr, const char *s, double x, double y, double px, int weight, int font, guint32 col, double maxw, int align) {
    if (text(cr, s, 0, 0, px, weight, font, 0, -1, 0, 0) <= maxw) { text(cr, s, x, y, px, weight, font, col, 1, align, 0); return; }
    gchar *t = g_strdup(s); glong n = g_utf8_strlen(t, -1);
    while (n > 1) {
        n--; *g_utf8_offset_to_pointer(t, n) = 0;
        gchar *e = g_strconcat(t, "…", NULL);
        if (text(cr, e, 0, 0, px, weight, font, 0, -1, 0, 0) <= maxw) { text(cr, e, x, y, px, weight, font, col, 1, align, 0); g_free(e); break; }
        g_free(e);
    }
    g_free(t);
}

static int dev_icon(const char *k) {
    static const struct { const char *k; int ic; } T[] = {{"mouse", 10}, {"keyboard", 11}, {"headset", 12}, {"gamepad", 13}, {"usb", 8}};
    for (guint i = 0; i < G_N_ELEMENTS(T); i++) if (!strcmp(T[i].k, k)) return T[i].ic;
    return 9;
}
static const char *dev_kind_label(const char *k) {
    static const struct { const char *k, *t; } T[] = {{"mouse", "Souris"}, {"keyboard", "Clavier"}, {"headset", "Audio"}, {"gamepad", "Manette"}, {"phone", "Téléphone"}, {"bt", "Bluetooth"}, {"usb", "USB"}};
    for (guint i = 0; i < G_N_ELEMENTS(T); i++) if (!strcmp(T[i].k, k)) return T[i].t;
    return k;
}

static int grid_cols(double w, double cardw, double gap) { return MAX(1, (int)((w + gap) / (cardw + gap))); }

/* pastille cliquable (application ouverte) */
static gboolean chip(cairo_t *cr, double x, double y, const char *t, double *w) {
    double tw = text(cr, t, 0, 0, 12.5, 400, F_SANS, 0, -1, 0, 0) + 28;
    gboolean hov = hit(x, y, tw, 30);
    cairo_rectangle(cr, x, y, tw, 30); rgba(cr, hov ? 0x1a1a1c : 0x0e0e10, 1); cairo_fill(cr);
    cairo_rectangle(cr, x + .5, y + .5, tw - 1, 29); rgba(cr, hov ? C_RED : C_LINE2, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    text(cr, t, x + 14, y + 15, 12.5, 400, F_SANS, C_TEXT, 1, 0, 0);
    *w = tw;
    return clicked(x, y, tw, 30);
}

static void page_devices(cairo_t *cr) {
    static Dev devs[MAXDEV]; static gint64 last;
    ptitle(cr, "Appareils");
    gint64 now = g_get_monotonic_time();
    if (now - last > 6 * G_USEC_PER_SEC) { fx_devices_refresh(); last = now; }
    int n = fx_devices(devs, MAXDEV);
    text(cr, "Périphériques externes : USB, Bluetooth et batteries (UPower)", X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    if (!n) { text(cr, last ? "Aucun périphérique externe détecté." : "Recherche…", X0, 130, 14, 400, F_SANS, C_MUTE, 1, 0, 0); return; }
    double gap = 16, cwid; int cols = grid_cols(CW, 230, gap);
    cwid = (CW - (cols - 1) * gap) / cols;
    for (int i = 0; i < n; i++) {
        double x = X0 + (i % cols) * (cwid + gap), y = 104 + (i / cols) * (176 + gap);
        Dev *d = &devs[i];
        panel(cr, x, y, cwid, 176, d->connected);
        icon(cr, dev_icon(d->kind), x + cwid / 2, y + 52, d->connected ? 0xffffff : 0x66666c, 2.2);
        text_fit(cr, d->name, x + cwid / 2, y + 110, 14, 600, F_SANS, d->connected ? C_TEXT : C_MUTE, cwid - 28, 1);
        char b[80]; g_snprintf(b, sizeof b, "%s · %s", dev_kind_label(d->kind), d->connected ? d->info : "Non connecté");
        text_fit(cr, b, x + cwid / 2, y + 132, 11.5, 400, F_SANS, C_MUTE, cwid - 28, 1);
        if (d->battery >= 0) { g_snprintf(b, sizeof b, "%d %%", d->battery); row_bar(cr, x + 18, y + 150, cwid - 36, "Batterie", b, d->battery); }
    }
    page_h = 104 + ((n + cols - 1) / cols) * (176 + gap);
}

static const char *prof_label(const char *k) { const char *d; if (!*k) return "Inchangé"; return prof_title(k, &d); }
static const char *hz_label(const char *k) { static char b[16]; if (atoi(k) <= 0) return "Inchangé"; g_snprintf(b, sizeof b, "%s Hz", k); return b; }

static void page_scenarios(cairo_t *cr) {
    HwState *s = &hw; int n; Scenario *sc = fx_scenarios(&n);
    ptitle(cr, "Profils de scénario");
    text(cr, "Quand une application prend le focus, applique un mode de fonctionnement et une fréquence d'écran, puis restaure les réglages précédents.", X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    const char *act_name = fx_scenario_active();
    if (act_name) { char b[96]; g_snprintf(b, sizeof b, "Actif : %s", act_name); text(cr, b, XR, 51, 13, 600, F_SANS, C_RED, 1, 2, 0); }
    if (!fx_windows_supported()) {
        text(cr, g_getenv("DISPLAY") ? "Détection de la fenêtre active indisponible : xprop est nécessaire (Hyprland et Sway n'en ont pas besoin)."
                                     : "Détection de la fenêtre active indisponible sur ce bureau (Hyprland, Sway ou X11/XWayland requis).", X0, 104, 12.5, 400, F_SANS, C_RED, 1, 0, 0);
        if (g_getenv("DISPLAY") && outline_btn(cr, XR - 400, 38, 130, 26, pkg_busy ? "Installation…" : "Installer xprop", 12) && !pkg_busy) install_pkg_async("xprop", "xprop");
    } else if (compat_win_backend() == WB_X11 && g_getenv("WAYLAND_DISPLAY"))
        text(cr, "Bureau Wayland (GNOME/KDE) : seuls les jeux et applications XWayland sont détectés — c'est le cas de la plupart des jeux Proton/Wine.", X0, 104, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    double y = 122;
    if (!n) { text(cr, "Aucun profil. Ajoute-en un depuis une application ouverte ci-dessous.", X0, y + 14, 14, 400, F_SANS, C_MUTE, 1, 0, 0); y += 44; }
    for (int i = 0; i < n; i++) {
        double x = X0, w = CW, h = 78;
        gboolean on = act_name && !strcmp(act_name, sc[i].name);
        panel(cr, x, y, w, h, on);
        text_fit(cr, sc[i].name, x + 18, y + 28, 15, 600, F_SANS, sc[i].enabled ? C_TEXT : C_MUTE, 170, 0);
        char b[96]; g_snprintf(b, sizeof b, "Fenêtre : %s", sc[i].cls); text_fit(cr, b, x + 18, y + 52, 11.5, 400, F_SANS, C_MUTE, 170, 0);
        /* suppression */
        double dx = x + w - 44; gboolean dh = hit(dx, y + 26, 26, 26);
        cairo_rectangle(cr, dx, y + 26, 26, 26); rgba(cr, dh ? C_RED : 0x1a1a1c, 1); cairo_fill(cr);
        cairo_move_to(cr, dx + 8, y + 34); cairo_line_to(cr, dx + 18, y + 44); cairo_move_to(cr, dx + 18, y + 34); cairo_line_to(cr, dx + 8, y + 44);
        rgba(cr, 0xffffff, 1); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
        if (clicked(dx, y + 26, 26, 26)) { fx_scenario_remove(i); return; }
        /* activation */
        if (toggle(cr, x + w - 104, y + 28, sc[i].enabled)) { sc[i].enabled = !sc[i].enabled; fx_scenarios_save(); }
        /* fréquence */
        double rw = 190, rx = x + w - 104 - 20 - rw;
        text(cr, "Fréquence écran", rx, y + 16, 10.5, 400, F_SANS, C_MUTE, 1, 0, 0);
        char hz[MAXCH + 1][32]; int nh = 0; g_strlcpy(hz[nh++], "0", 32);
        for (int k = 0; k < s->disp.nmodes && nh < MAXCH; k++) g_snprintf(hz[nh++], 32, "%d", s->disp.modes[k]);
        char cur[32]; g_snprintf(cur, sizeof cur, "%d", sc[i].refresh);
        if (nh > 1) { int r = segmented(cr, rx, y + 30, rw, 30, hz, hz_label, nh, cur); if (r >= 0) { sc[i].refresh = atoi(hz[r]); fx_scenarios_save(); } }
        else text(cr, "Non réglable", rx, y + 45, 12, 400, F_SANS, C_MUTE, 1, 0, 0);
        /* profil */
        double pw = 300, px = rx - 16 - pw;
        text(cr, "Mode de fonctionnement", px, y + 16, 10.5, 400, F_SANS, C_MUTE, 1, 0, 0);
        char pf[MAXCH + 1][32]; int np = 0; g_strlcpy(pf[np++], "", 32);
        for (int k = 0; k < s->nprofs && np < MAXCH; k++) g_strlcpy(pf[np++], s->profs[k], 32);
        if (np > 1) { int r = segmented(cr, px, y + 30, pw, 30, pf, prof_label, np, sc[i].profile); if (r >= 0) { g_strlcpy(sc[i].profile, pf[r], sizeof sc[i].profile); fx_scenarios_save(); } }
        else text(cr, "Non réglable", px, y + 45, 12, 400, F_SANS, C_MUTE, 1, 0, 0);
        y += h + 10;
    }
    /* ajout depuis une application ouverte */
    y += 12;
    section(cr, "Ajouter depuis une application ouverte", X0, y);
    Win w[MAXWIN]; int nw = fx_windows(w, MAXWIN);
    double cx = X0, cy = y + 24; int shown = 0;
    for (int i = 0; i < nw; i++) {
        gboolean have = FALSE; for (int k = 0; k < n; k++) have |= !g_ascii_strcasecmp(sc[k].cls, w[i].cls);
        if (have) continue;
        double tw = text(cr, w[i].cls, 0, 0, 12.5, 400, F_SANS, 0, -1, 0, 0) + 28;
        if (cx + tw > XR) { cx = X0; cy += 38; }
        double cw2; if (chip(cr, cx, cy, w[i].cls, &cw2)) { const char *dot = strrchr(w[i].cls, '.'); fx_scenario_add(dot && dot[1] ? dot + 1 : w[i].cls, w[i].cls); return; }
        cx += cw2 + 10; shown++;
    }
    if (!shown) text(cr, fx_windows_supported() ? "Aucune autre application ouverte." : "Indisponible sur ce bureau (Hyprland, Sway ou X11/XWayland requis).", X0, cy + 15, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    page_h = cy + 60;
}

static void open_path(const char *p) { const char *a[] = {"xdg-open", p, NULL}; g_spawn_async(NULL, (char **)a, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL); }

/* ------------------------------------------------------------------ pochettes de jeux (façon Heroic)
   Téléchargement en tâche de fond, converties en PNG et mises en cache disque (~/.cache/coreboard/covers) : le
   rendu ne charge ensuite que des PNG via le décodeur natif de cairo, comme la photo d'appareil de la page d'accueil. */
#define MAXCOVER 256
typedef struct { char key[64]; cairo_surface_t *surf; gboolean tried; } Cover;
static Cover covers[MAXCOVER]; static int ncovers;
static GMutex cover_lock;

static Cover *cover_find(const char *key) { for (int i = 0; i < ncovers; i++) if (!strcmp(covers[i].key, key)) return &covers[i]; return NULL; }

typedef struct { char key[64], url[300], cache_png[400]; } CoverJob;

static gpointer cover_thread(gpointer d) {
    CoverJob *j = d;
    if (!g_file_test(j->cache_png, G_FILE_TEST_EXISTS)) {
        gchar *tmp = NULL; int fd = g_file_open_tmp("coreboard-cover-XXXXXX", &tmp, NULL); if (fd >= 0) close(fd);
        const char *argv[] = {"curl", "-fsSL", "--max-time", "15", "-o", tmp, j->url, NULL};
        gint st = 1;
        /* le réseau de cette machine a des résolutions DNS intermittentes : on retente avant d'abandonner */
        for (int attempt = 0; attempt < 3 && st != 0; attempt++) {
            if (attempt) g_usleep(1500000);
            g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &st, NULL);
        }
        if (st == 0) {
            GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(tmp, 240, 360, FALSE, NULL);
            if (pb) {
                gchar *dir = g_path_get_dirname(j->cache_png); g_mkdir_with_parents(dir, 0755); g_free(dir);
                gdk_pixbuf_save(pb, j->cache_png, "png", NULL, NULL);
                g_object_unref(pb);
            }
        }
        if (tmp) { g_unlink(tmp); g_free(tmp); }
    }
    cairo_surface_t *surf = NULL;
    if (g_file_test(j->cache_png, G_FILE_TEST_EXISTS)) {
        cairo_surface_t *sf = cairo_image_surface_create_from_png(j->cache_png);
        if (cairo_surface_status(sf) == CAIRO_STATUS_SUCCESS) surf = sf; else cairo_surface_destroy(sf);
    }
    g_mutex_lock(&cover_lock);
    Cover *c = cover_find(j->key);
    if (c) c->surf = surf;
    g_mutex_unlock(&cover_lock);
    g_free(j);
    return NULL;
}

/* renvoie la pochette en cache (NULL le temps du téléchargement, définitivement si l'image est indisponible) */
static cairo_surface_t *cover_get(const char *key, const char *url) {
    if (!url || !*url) return NULL;
    g_mutex_lock(&cover_lock);
    Cover *c = cover_find(key);
    if (!c && ncovers < MAXCOVER) { c = &covers[ncovers++]; memset(c, 0, sizeof *c); g_strlcpy(c->key, key, sizeof c->key); }
    cairo_surface_t *surf = c ? c->surf : NULL;
    gboolean need = c && !c->tried;
    if (need) c->tried = TRUE;
    g_mutex_unlock(&cover_lock);
    if (need) {
        CoverJob *j = g_new0(CoverJob, 1);
        g_strlcpy(j->key, key, sizeof j->key);
        g_strlcpy(j->url, url, sizeof j->url);
        gchar *dir = g_build_filename(g_get_user_cache_dir(), "coreboard", "covers", NULL);
        gchar *fn = g_strdup_printf("%s.png", key);
        gchar *full = g_build_filename(dir, fn, NULL);
        g_strlcpy(j->cache_png, full, sizeof j->cache_png);
        g_free(dir); g_free(fn); g_free(full);
        g_thread_unref(g_thread_new("coreboard-cover", cover_thread, j));
    }
    return surf;
}

/* couleur de repli stable (dérivée du nom) pour les jeux sans pochette téléchargeable */
static guint32 placeholder_color(const char *name) {
    guint32 h = 5381; for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = ((h << 5) + h) + *p;
    static const guint32 pal[] = {0x2a1a3a, 0x1a2a3a, 0x3a1a2a, 0x1a3a2a, 0x3a2a1a, 0x2a1a1a, 0x1a1a3a, 0x3a1a3a, 0x263a1a, 0x1a3a36};
    return pal[h % (sizeof pal / sizeof pal[0])];
}

/* dessine une pochette (image ou pastille de couleur + initiale) dans le rectangle donné, recadrée sans déformation */
static void draw_cover(cairo_t *cr, double x, double y, double w, double h, const char *name, cairo_surface_t *img) {
    cairo_save(cr); cairo_rectangle(cr, x, y, w, h); cairo_clip(cr);
    if (img) {
        double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        double sc = MAX(w / iw, h / ih);
        cairo_translate(cr, x + (w - iw * sc) / 2, y + (h - ih * sc) / 2);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    } else {
        rgba(cr, placeholder_color(name), 1); cairo_rectangle(cr, x, y, w, h); cairo_fill(cr);
        char initial[2] = {name && name[0] ? (char)g_ascii_toupper(name[0]) : '?', 0};
        text(cr, initial, x + w / 2, y + h / 2 + MIN(w, h) * 0.14, MIN(w, h) * 0.4, 700, F_SANS, 0xffffff, 1, 1, 0);
    }
    cairo_restore(cr);
}

static void page_games(cairo_t *cr) {
    int n; Game *g = fx_games(&n); int nsc_; Scenario *sc = fx_scenarios(&nsc_);
    /* tant que cette page est ouverte, on se redessine régulièrement : sans ça, les recherches/téléchargements
       en tâche de fond (Steam, Epic, GOG) ne remontent à l'écran qu'au hasard d'un prochain redessin. */
    schedule_redraw(1000);
    ptitle(cr, "Bibliothèque de jeux");
    if (outline_btn(cr, XR - 240, 38, 100, 26, "Rescanner", 12)) fx_games_rescan();
    text(cr, "Steam, Epic (Legendary), GOG (Heroic), Lutris, applications de catégorie « Jeu » et applications ajoutées. Un jeu peut avoir son profil de scénario.", X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    double gap = 14, y = 106; int cols = grid_cols(CW, 150, gap);
    double cwid = (CW - (cols - 1) * gap) / cols;
    double cimgh = cwid * 1.5, ccardh = cimgh + 46;   /* pochette portrait façon Heroic + bande titre/actions */
    /* ---- compte Steam intégré (steamcmd) : connexion et téléchargement sans ouvrir le client Steam ---- */
    {
        ScmStatus st = fx_steam_status();
        section(cr, "Compte Steam", X0, y);
        if (!fx_steam_available()) {
            gboolean busy = fx_tool_installing() && !strcmp(fx_tool_installing_name(), "steamcmd");
            text(cr, busy ? "Installation de steamcmd en cours (mot de passe demandé)…" : "steamcmd (connexion et téléchargement Steam) n'est pas installé.",
                 X0, y + 24, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
            if (!busy && outline_btn(cr, X0, y + 42, 170, 30, "Installer steamcmd", 12.5)) fx_tool_install("steamcmd");
            y += 82;
        } else if (st.need_guard) {
            text(cr, st.msg, X0, y + 24, 12.5, 400, F_SANS, C_TEXT, 1, 0, 0);
            if (outline_btn(cr, X0, y + 42, 180, 30, "Saisir le code", 12.5)) ask_text(4, 0, "Code Steam Guard", "");
            y += 82;
            schedule_redraw(600);   /* attend la réponse de steamcmd une fois le code envoyé */
        } else if (!st.connected) {
            text(cr, st.msg[0] ? st.msg : "Connecte-toi pour télécharger tes jeux directement depuis Coreboard (via steamcmd, l'outil officiel Valve).",
                 X0, y + 24, 12.5, 400, F_SANS, st.error ? C_RED : C_MUTE, 1, 0, 0);
            if (outline_btn(cr, X0, y + 44, 190, 30, "Se connecter à Steam", 12.5)) ask_steam_login();
            y += 84;
            if (st.msg[0] && !st.error) schedule_redraw(600);   /* connexion en cours : on continue de vérifier l'état */
        } else {
            char b[100]; g_snprintf(b, sizeof b, "Connecté à Steam (%s)", fx_steam_username());
            text(cr, b, X0, y + 24, 12.5, 400, F_SANS, C_OK, 1, 0, 0);
            if (outline_btn(cr, XR - 150, y - 5, 150, 26, "Se déconnecter", 12)) fx_steam_logout();
            y += 40;
            if (st.dl_active) {
                char b2[220]; g_snprintf(b2, sizeof b2, "%s (%.0f %%)", st.msg, CLAMP(st.pct, 0, 100));
                text_fit(cr, b2, X0, y + 14, 12.5, 400, F_SANS, C_TEXT, CW, 0);
                cairo_rectangle(cr, X0, y + 22, CW, 4); rgba(cr, 0x2a2a2e, 1); cairo_fill(cr);
                cairo_rectangle(cr, X0, y + 22, CW * CLAMP(st.pct, 0, 100) / 100.0, 4); rgba(cr, C_RED, 1); cairo_fill(cr);
                y += 40;
                schedule_redraw(500);
            }
        }
        y += 14;
    }
    /* ---- compte Epic Games intégré (Legendary) : connexion et téléchargement sans client Epic ---- */
    {
        EpicStatus est = fx_epic_status();
        section(cr, "Compte Epic Games", X0, y);
        if (!fx_epic_available()) {
            gboolean busy = fx_tool_installing() && !strcmp(fx_tool_installing_name(), "legendary");
            text(cr, busy ? "Installation de legendary en cours (mot de passe demandé)…" : "legendary (client Epic Games open-source) n'est pas installé.",
                 X0, y + 24, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
            if (!busy && outline_btn(cr, X0, y + 42, 170, 30, "Installer legendary", 12.5)) fx_tool_install("legendary");
            y += 82;
        } else if (!fx_epic_logged_in()) {
            text(cr, est.msg[0] ? est.msg : "Connecte-toi via la page officielle Epic (aucun mot de passe ne passe par Coreboard).",
                 X0, y + 24, 12.5, 400, F_SANS, est.error ? C_RED : C_MUTE, 1, 0, 0);
            if (outline_btn(cr, X0, y + 44, 220, 30, "Se connecter à Epic Games", 12.5)) {
                open_path("https://legendary.gl/epiclogin");
                ask_text(5, 0, "Code Epic (authorizationCode)", "");
            }
            y += 84;
        } else {
            text(cr, "Connecté à Epic Games", X0, y + 24, 12.5, 400, F_SANS, C_OK, 1, 0, 0);
            if (outline_btn(cr, XR - 150, y - 5, 150, 26, "Se déconnecter", 12)) fx_epic_logout();
            y += 40;
            if (est.msg[0] && (est.dl_active || est.error)) {
                text_fit(cr, est.msg, X0, y + 14, 12.5, 400, F_SANS, est.error ? C_RED : C_TEXT, CW, 0);
                if (est.dl_active) {
                    cairo_rectangle(cr, X0, y + 22, CW, 4); rgba(cr, 0x2a2a2e, 1); cairo_fill(cr);
                    cairo_rectangle(cr, X0, y + 22, CW * CLAMP(est.pct, 0, 100) / 100.0, 4); rgba(cr, C_RED, 1); cairo_fill(cr);
                }
                y += 40;
            }
            static gint64 ep_last;
            gint64 enow = g_get_monotonic_time();
            if (!ep_last || enow - ep_last > 30 * G_USEC_PER_SEC) { fx_epic_refresh(); ep_last = enow; }
            EpicLibEntry el[MAXEPIC]; int nel = fx_epic_list(el, MAXEPIC);
            if (!nel) { text(cr, fx_epic_ready() ? "Bibliothèque Epic vide (ou tout est déjà installé)." : "Récupération de ta bibliothèque Epic…", X0, y + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0); y += 40; }
            else {
                double gap5 = 12; int cols5 = grid_cols(CW, 220, gap5);
                double cwid5 = (CW - (cols5 - 1) * gap5) / cols5;
                for (int i = 0; i < nel; i++) {
                    double x5 = X0 + (i % cols5) * (cwid5 + gap5), cy5 = y + (i / cols5) * (84 + gap5);
                    panel(cr, x5, cy5, cwid5, 84, FALSE);
                    text_fit(cr, el[i].title, x5 + 14, cy5 + 26, 13.5, 600, F_SANS, C_TEXT, cwid5 - 28, 0);
                    if (outline_btn(cr, x5 + 14, cy5 + 42, cwid5 - 28, 28, "Télécharger", 12)) {
                        fx_epic_download(el[i].appname, el[i].title);
                        char b4[140]; g_snprintf(b4, sizeof b4, "Téléchargement de %s (Epic)…", el[i].title); show_toast(b4);
                    }
                }
                y += ((nel + cols5 - 1) / cols5) * (84 + gap5) + 12;
            }
        }
        y += 14;
    }
    /* ---- recherche libre dans le catalogue Steam : installation directe des résultats ---- */
    {
        const char *q = fx_steamsearch_query_str();
        section(cr, "Rechercher un jeu Steam à installer", X0, y);
        if (outline_btn(cr, XR - 190, y - 5, 190, 26, *q ? "Nouvelle recherche" : "Rechercher un jeu…", 12))
            ask_text(3, 0, "Rechercher un jeu Steam", q);
        y += 24;
        SteamHit hits[MAXHITS]; int nh = fx_steamsearch(hits, MAXHITS);
        if (!*q) { text(cr, "Cherche n'importe quel jeu du store Steam (pas seulement les gratuits).", X0, y + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0); y += 40; }
        else if (!nh) { char b[140]; g_snprintf(b, sizeof b, "Recherche de « %s »…", q); text(cr, b, X0, y + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0); y += 40; schedule_redraw(500); }
        else {
            double gap4 = 12; int cols4 = grid_cols(CW, 220, gap4);
            double cwid4 = (CW - (cols4 - 1) * gap4) / cols4;
            for (int i = 0; i < nh; i++) {
                double x4 = X0 + (i % cols4) * (cwid4 + gap4), cy4 = y + (i / cols4) * (84 + gap4);
                panel(cr, x4, cy4, cwid4, 84, FALSE);
                text_fit(cr, hits[i].name, x4 + 14, cy4 + 24, 13.5, 600, F_SANS, C_TEXT, cwid4 - 28, 0);
                char pr[32]; if (hits[i].free) g_strlcpy(pr, "Gratuit", sizeof pr); else g_snprintf(pr, sizeof pr, "%.2f $", hits[i].price_cents / 100.0);
                text(cr, pr, x4 + 14, cy4 + 40, 11.5, 400, F_SANS, C_MUTE, 1, 0, 0);
                if (outline_btn(cr, x4 + 14, cy4 + 48, cwid4 - 28, 26, "Télécharger", 11.5)) {
                    if (fx_steam_logged_in()) fx_steam_download(hits[i].id, hits[i].name);
                    else { show_toast("Connecte-toi à Steam ci-dessus d'abord"); ask_steam_login(); }
                }
            }
            y += ((nh + cols4 - 1) / cols4) * (84 + gap4) + 12;
        }
        y += 26;
    }
    section(cr, "Bibliothèque installée", X0, y);
    y += 24;
    if (!n) { text(cr, "Aucun jeu détecté. Ajoute une application ouverte ci-dessous.", X0, y + 14, 14, 400, F_SANS, C_MUTE, 1, 0, 0); y += 44; }
    for (int i = 0; i < n; i++) {
        double x = X0 + (i % cols) * (cwid + gap), cy = y + (i / cols) * (ccardh + gap);
        panel(cr, x, cy, cwid, ccardh, FALSE);
        cairo_surface_t *img = NULL;
        if (!strcmp(g[i].source, "Steam") && g[i].id[0]) {
            char key[64], url[300];
            g_snprintf(key, sizeof key, "steam_%s", g[i].id);
            g_snprintf(url, sizeof url, "https://cdn.cloudflare.steamstatic.com/steam/apps/%s/library_600x900.jpg", g[i].id);
            img = cover_get(key, url);
        }
        draw_cover(cr, x, cy, cwid, cimgh, g[i].name, img);
        gboolean hovimg = hit(x, cy, cwid, cimgh);
        if (hovimg) { rgba(cr, 0x000000, 0.3); cairo_rectangle(cr, x, cy, cwid, cimgh); cairo_fill(cr); }
        if (clicked(x, cy, cwid, cimgh)) {
            char b[120]; g_snprintf(b, sizeof b, "Lancement de %s…", g[i].name); show_toast(b); fx_game_launch(&g[i]);
        }
        text_fit(cr, g[i].name, x + 8, cy + cimgh + 18, 12.5, 600, F_SANS, C_TEXT, cwid - 16, 0);
        double ay = cy + cimgh + 34;
        gboolean has = FALSE; for (int k = 0; k < nsc_; k++) has |= g[i].cls[0] && !g_ascii_strcasecmp(sc[k].cls, g[i].cls);
        if (g[i].cls[0]) {
            const char *lbl = has ? "Profil ✓" : "Créer un profil";
            gboolean lh = hit(x + 8, ay - 10, cwid - 24, 16);
            text(cr, lbl, x + 8, ay, 10.5, 500, F_SANS, lh ? C_TEXT : C_MUTE, 1, 0, 0);
            if (clicked(x + 8, ay - 10, cwid - 24, 16)) {
                if (has) page = 7; else { fx_scenario_add(g[i].name, g[i].cls); show_toast("Profil de scénario créé : règle-le dans « Scénarios »"); }
            }
        } else text(cr, g[i].source, x + 8, ay, 10.5, 400, F_SANS, C_MUTE, 1, 0, 0);
        if (g[i].manual) {
            double dx = x + cwid - 16; gboolean dh = hit(dx - 8, ay - 10, 16, 16);
            cairo_move_to(cr, dx - 5, ay - 6); cairo_line_to(cr, dx + 5, ay + 4); cairo_move_to(cr, dx + 5, ay - 6); cairo_line_to(cr, dx - 5, ay + 4);
            rgba(cr, dh ? C_RED : C_MUTE, 1); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
            if (clicked(dx - 8, ay - 10, 16, 16)) { fx_game_remove(i); return; }
        }
    }
    y += ((n + cols - 1) / cols) * (ccardh + gap) + 12;
    section(cr, "Ajouter une application ouverte", X0, y);
    Win w[MAXWIN]; int nw = fx_windows(w, MAXWIN);
    double cx = X0, cy = y + 24; int shown = 0;
    for (int i = 0; i < nw; i++) {
        gboolean have = FALSE; for (int k = 0; k < n; k++) have |= !g_ascii_strcasecmp(g[k].cls, w[i].cls);
        if (have) continue;
        double tw = text(cr, w[i].cls, 0, 0, 12.5, 400, F_SANS, 0, -1, 0, 0) + 28;
        if (cx + tw > XR) { cx = X0; cy += 38; }
        double cw2; if (chip(cr, cx, cy, w[i].cls, &cw2)) { fx_game_add_manual(&w[i]); show_toast("Application ajoutée à la bibliothèque"); return; }
        cx += cw2 + 10; shown++;
    }
    if (!shown) text(cr, fx_windows_supported() ? "Aucune autre application ouverte." : "Indisponible sur ce bureau (Hyprland, Sway ou X11/XWayland requis).", X0, cy + 15, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    /* ---- jeux gratuits Steam (free-to-play) : recherche publique + installation réelle via steam://install ---- */
    cy += 50;
    { static gint64 sf_last;
      gint64 now2 = g_get_monotonic_time();
      gboolean stale = !sf_last || now2 - sf_last > 30 * G_USEC_PER_SEC;
      if (stale) { fx_steamfree_refresh(); sf_last = now2; }
      section(cr, "Jeux gratuits Steam (free-to-play)", X0, cy);
      if (outline_btn(cr, XR - 110, cy - 5, 110, 26, "Actualiser", 12)) { fx_steamfree_refresh(); sf_last = now2; }
      FreeGame fg[MAXFREE]; int nfg = fx_steamfree(fg, MAXFREE);
      double gap2 = 12, y2 = cy + 24; int cols2 = grid_cols(CW, 220, gap2);
      double cwid2 = (CW - (cols2 - 1) * gap2) / cols2;
      if (!nfg) {
          gboolean ready = fx_steamfree_ready();
          text(cr, ready ? "Aucun jeu gratuit trouvé (réessaie plus tard, ou vérifie ta connexion réseau)." : "Recherche…", X0, y2 + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
          cy = y2 + 40;
          if (!ready) schedule_redraw(500);
      }
      else {
          for (int i = 0; i < nfg; i++) {
              double x2 = X0 + (i % cols2) * (cwid2 + gap2), cy2 = y2 + (i / cols2) * (84 + gap2);
              panel(cr, x2, cy2, cwid2, 84, FALSE);
              text_fit(cr, fg[i].name, x2 + 14, cy2 + 26, 13.5, 600, F_SANS, C_TEXT, cwid2 - 28, 0);
              if (outline_btn(cr, x2 + 14, cy2 + 42, cwid2 - 28, 28, "Télécharger", 12)) {
                  if (fx_steam_logged_in()) fx_steam_download(fg[i].id, fg[i].name);
                  else { show_toast("Connecte-toi à Steam ci-dessus d'abord"); ask_steam_login(); }
              }
          }
          cy = y2 + ((nfg + cols2 - 1) / cols2) * (84 + gap2) + 12;
      }
    }
    /* ---- bibliothèque du compte GOG (lgogdownloader) : téléchargement réel des jeux non installés ---- */
    cy += 26;
    section(cr, "Bibliothèque GOG (lgogdownloader)", X0, cy);
    if (!fx_gog_available()) {
        gboolean busy = fx_tool_installing() && !strcmp(fx_tool_installing_name(), "lgogdownloader");
        text(cr, busy ? "Installation de lgogdownloader en cours (mot de passe demandé)…" : "Une fois installé, lance « lgogdownloader --login » dans un terminal pour activer le téléchargement direct (y compris les jeux gratuits ajoutés depuis gog.com).",
             X0, cy + 24, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
        if (!busy && outline_btn(cr, X0, cy + 42, 200, 30, "Installer lgogdownloader", 12.5)) fx_tool_install("lgogdownloader");
        cy += 82;
    } else {
        static gint64 gog_last;
        gint64 now3 = g_get_monotonic_time();
        if (!gog_last || now3 - gog_last > 30 * G_USEC_PER_SEC) { fx_gog_refresh(); gog_last = now3; }
        if (outline_btn(cr, XR - 110, cy - 5, 110, 26, "Actualiser", 12)) { fx_gog_refresh(); gog_last = now3; }
        GogLibEntry gl[MAXGOGLIB]; int ngl = fx_gog_list(gl, MAXGOGLIB);
        double y3 = cy + 24;
        if (!ngl) { text(cr, "Aucun jeu trouvé (compte non connecté, ou recherche en cours).", X0, y3 + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0); cy = y3 + 40; }
        else {
            double gap3 = 12; int cols3 = grid_cols(CW, 220, gap3);
            double cwid3 = (CW - (cols3 - 1) * gap3) / cols3;
            for (int i = 0; i < ngl; i++) {
                double x3 = X0 + (i % cols3) * (cwid3 + gap3), cy3 = y3 + (i / cols3) * (84 + gap3);
                panel(cr, x3, cy3, cwid3, 84, FALSE);
                text_fit(cr, gl[i].title, x3 + 14, cy3 + 26, 13.5, 600, F_SANS, C_TEXT, cwid3 - 28, 0);
                if (outline_btn(cr, x3 + 14, cy3 + 42, cwid3 - 28, 28, "Télécharger", 12)) {
                    fx_gog_download(&gl[i]);
                    char b[140]; g_snprintf(b, sizeof b, "Téléchargement de %s (GOG)…", gl[i].title); show_toast(b);
                }
            }
            cy = y3 + ((ngl + cols3 - 1) / cols3) * (84 + gap3) + 12;
        }
    }
    /* ---- trouver de nouveaux jeux gratuits à ajouter à un compte (GOG, itch.io) ---- */
    cy += 26;
    section(cr, "Trouver de nouveaux jeux gratuits", X0, cy);
    text(cr, "Ces boutiques exigent de « récupérer » le jeu sur leur site avant de pouvoir le télécharger ; ouvre la liste, ajoute-le à ton compte, puis télécharge-le ci-dessus (GOG) ou depuis ton lanceur (itch.io).", X0, cy + 24, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    if (outline_btn(cr, X0, cy + 42, 190, 30, "GOG — gratuits permanents", 12.5))
        open_path("https://www.gog.com/en/games?priceRange=0,0&hideDLCs=true");
    if (outline_btn(cr, X0 + 204, cy + 42, 150, 30, "itch.io — gratuits", 12.5))
        open_path("https://itch.io/games/free");
    page_h = cy + 96;
}

/* ------------------------------------------------------------------ Éclairage RGB du clavier */
static void hsv2rgb(double h, double sat, double v, guint32 *out) {
    double c = v * sat, hh = fmod(h, 360) / 60, x = c * (1 - fabs(fmod(hh, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if (hh < 1) { r = c; g = x; } else if (hh < 2) { r = x; g = c; } else if (hh < 3) { g = c; b = x; }
    else if (hh < 4) { g = x; b = c; } else if (hh < 5) { r = x; b = c; } else { r = c; b = x; }
    *out = ((guint32)((r + m) * 255 + .5) << 16) | ((guint32)((g + m) * 255 + .5) << 8) | (guint32)((b + m) * 255 + .5);
}
static void rgb2hsv(guint32 c, double *h, double *sat, double *v) {
    double r = ((c >> 16) & 255) / 255.0, g = ((c >> 8) & 255) / 255.0, b = (c & 255) / 255.0;
    double mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), d = mx - mn;
    *v = mx; *sat = mx > 0 ? d / mx : 0;
    if (d < 1e-6) return;                     /* teinte conservée pour les gris */
    if (mx == r) *h = 60 * fmod((g - b) / d + 6, 6); else if (mx == g) *h = 60 * ((b - r) / d + 2); else *h = 60 * ((r - g) / d + 4);
}

/* zone touchée à la colonne c (0..19) du dessin du clavier */
static int key_zone(int c) { return c < 5 ? 0 : c < 12 ? 1 : c < 16 ? 2 : 3; }

/* glisser dans un rectangle (2 axes) — renvoie TRUE si (*u,*v) ∈ [0,1]² a changé */
static gboolean drag2d(double x, double y, double w, double h, double *u, double *v) {
    gpointer id = WID(x, y);
    if (mclick && hit(x, y, w, h)) { active_id = id; mclick = FALSE; }
    if (active_id != id) return FALSE;
    if (!mdown) { active_id = NULL; return TRUE; }
    *u = CLAMP((mx - x) / w, 0, 1); *v = CLAMP((my - y) / h, 0, 1);
    return TRUE;
}

/* ------------------------------------------------------------------ éléments animés (widgets séparés)
   Le rendu GTK d'un DrawingArea plein écran coûte cher à chaque image : les parties animées (touches du clavier,
   jauges audio) vivent donc dans de petits widgets superposés, redessinés seuls, et le fond dans un widget qui ne change qu'au redimensionnement. */
typedef struct { GtkWidget *w; double x, y, rw, rh; gboolean show, animate; } Live;
static Live live_keys, live_meters;
static GtkWidget *bg_area;
static double lk_cell;                       /* côté d'une touche (unités de la scène) */

/* ------------------------------------------------------------------ animation de démarrage (façon boot ROG)
   Ne joue qu'une fois par processus (voir create_window) : la fenêtre est en instance unique et se
   toggle toute la journée, un vrai « boot » ne doit se voir qu'au premier lancement.
   La sortie attend le premier relevé matériel valide (hw_snapshot().uptime > 0) au lieu d'une durée
   fixe, pour qu'on ne retombe jamais sur l'écran « Chargement… » juste après l'animation ; un plancher
   garde une sensation de « boot » même si les données arrivent tout de suite, un plafond de sécurité
   évite un écran noir indéfini si la détection matérielle traîne. */
#define BOOT_HOLD_MIN 2.5  /* secondes minimum avant de pouvoir sortir */
#define BOOT_HOLD_MAX 5.0  /* sécurité : sortie forcée si les données ne sont toujours pas prêtes */
#define BOOT_OUTRO 0.35    /* durée du fondu de sortie */
static GtkWidget *boot_area;
static gint64 boot_t0;
static gint64 boot_ready_t0; /* instant (monotonic) du premier relevé valide ; 0 = pas encore */

static double boot_outro_start(void) {
    double ready_t = boot_ready_t0 ? (boot_ready_t0 - boot_t0) / 1e6 : -1;
    return ready_t >= 0 ? MAX(ready_t, BOOT_HOLD_MIN) : BOOT_HOLD_MAX;
}

static void boot_play_sound(void) {
    gchar *cand[3] = {g_build_filename(DATADIR, "sounds", "startup.ogg", NULL), NULL, NULL};
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        gchar *dir = g_path_get_dirname(exe);
        cand[1] = g_build_filename(dir, "data", "sounds", "startup.ogg", NULL);
        cand[2] = g_build_filename(dir, "..", "data", "sounds", "startup.ogg", NULL);
        g_free(dir); g_free(exe);
    }
    const char *path = NULL;
    for (int i = 0; i < 3 && !path; i++) if (cand[i] && g_file_test(cand[i], G_FILE_TEST_EXISTS)) path = cand[i];
    if (path) {
        static const char *players[] = {"paplay", "pw-play"};
        for (int i = 0; i < 2; i++) {
            gchar *argv[] = {(gchar *)players[i], (gchar *)path, NULL};
            if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL)) break;
        }
    }
    for (int i = 0; i < 3; i++) g_free(cand[i]);
}

static void boot_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
    (void)a; (void)d;
    double t = (g_get_monotonic_time() - boot_t0) / 1e6;
    double cx = w / 2.0, cy = h / 2.0;
    double since_outro = t - boot_outro_start();
    double out_k = since_outro < 0 ? 1.0 : (1 - CLAMP(since_outro / BOOT_OUTRO, 0, 1));
    /* voile noir qui masque l'interface en dessous (tant que les données ne sont pas prêtes), puis s'efface */
    rgba(cr, 0x000000, out_k); cairo_paint(cr);
    /* halo rouge qui monte, respire doucement pendant l'attente, puis retombe à la sortie */
    double glow = CLAMP(t / 0.5, 0, 1) * (0.85 + 0.15 * sin(t * 2.4)) * out_k;
    if (glow > 0.01) {
        cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, MAX(w, h) * 0.55);
        cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.1, 0.2, 0.35 * glow);
        cairo_pattern_add_color_stop_rgba(p, 1, 1, 0.1, 0.2, 0);
        cairo_set_source(cr, p); cairo_paint(cr); cairo_pattern_destroy(p);
    }
    /* ligne de scan qui balaie l'écran au tout début */
    if (t < 0.45) {
        double sy = h * CLAMP(t / 0.42, 0, 1);
        rgba(cr, C_RED, (1 - t / 0.42) * 0.8); cairo_rectangle(cr, 0, sy - 1, w, 2); cairo_fill(cr);
    }
    /* logotype qui apparaît, reste affiché tant que les données ne sont pas prêtes, puis s'efface avec le voile */
    double logo_a = CLAMP((t - 0.15) / 0.3, 0, 1) * out_k;
    if (logo_a > 0.01) {
        double sc = 1.06 - 0.06 * CLAMP((t - 0.15) / 0.3, 0, 1);
        cairo_save(cr); cairo_translate(cr, cx, cy); cairo_scale(cr, sc, sc); cairo_translate(cr, -cx, -cy);
        double lw = text(cr, "COREBOARD", 0, 0, 30, 700, F_ORB, C_TEXT, -1, 1, 6);
        slashes(cr, cx - lw / 2 - 36, cy, 20, 3, C_RED, logo_a);
        text(cr, "COREBOARD", cx, cy, 30, 700, F_ORB, C_TEXT, logo_a, 1, 6);
        double barw = 220 * CLAMP((t - 0.25) / 0.5, 0, 1);
        rgba(cr, C_RED, logo_a); cairo_rectangle(cr, cx - barw / 2, cy + 30, barw, 2); cairo_fill(cr);
        cairo_restore(cr);
    }
    /* flash bref synchronisé avec la note de fin du son */
    double fd = fabs(t - 0.95);
    if (fd < 0.12) { rgba(cr, 0xffe8ea, (1 - fd / 0.12) * 0.5); cairo_paint(cr); }
}

static gboolean boot_tick(gpointer d) {
    (void)d;
    if (!boot_ready_t0) {
        HwState s; hw_snapshot(&s);
        if (s.uptime > 0) boot_ready_t0 = g_get_monotonic_time();
    }
    if (boot_area) gtk_widget_queue_draw(boot_area);
    double t = (g_get_monotonic_time() - boot_t0) / 1e6;
    if (t >= boot_outro_start() + BOOT_OUTRO) {
        if (boot_area) gtk_widget_set_visible(boot_area, FALSE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void live_draw_keys(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
    (void)a; (void)w; (void)h; (void)d;
    RgbState *st = rgb_state();
    gboolean custom = st->mode == RGBM_CUSTOM;
    guint32 mcol[4] = {0}; if (st->mode == RGBM_MUSIC) rgb_music_colors(mcol);
    cairo_scale(cr, kk, kk); cairo_translate(cr, -live_keys.x, -live_keys.y);
    double cell = lk_cell, kx0 = live_keys.x + 2, ky0 = live_keys.y + 2;
    double tsec = g_get_monotonic_time() / 1e6, kb = st->brightness / 100.0;
    for (int r = 0; r < 6; r++) for (int c = 0; c < 20; c++) {
        if (c == 15) continue;                                   /* espace entre clavier et pavé numérique */
        if (r == 5 && c < 2) continue;
        int z = key_zone(c);
        int zm = custom ? st->zmode[z] : st->mode, zs = custom ? st->zspeed[z] : st->speed;
        guint32 col = custom || (st->mode == RGBM_STATIC && !st->sync) ? st->color[z] : st->color[0];
        double k = kb, rr = ((col >> 16) & 255) / 255.0, gg = ((col >> 8) & 255) / 255.0, bb = (col & 255) / 255.0;
        double T = rgb_cycle_seconds(zs), phase = fmod(tsec / T, 1.0);
        guint32 t;
        switch (zm) {
        case RGBM_OFF: rr = gg = bb = 0; break;
        case RGBM_BREATH: k *= fabs(1 - 2 * phase); break;       /* plein → noir → plein sur un cycle */
        case RGBM_CYCLE: hsv2rgb(360 * phase, 1, 1, &t); rr = ((t >> 16) & 255) / 255.0; gg = ((t >> 8) & 255) / 255.0; bb = (t & 255) / 255.0; break;
        case RGBM_WAVE: {
            double hh = c * 18.0 + (st->dir_left ? -360 : 360) * phase;
            hsv2rgb(fmod(fmod(hh, 360) + 360, 360), 1, 1, &t); rr = ((t >> 16) & 255) / 255.0; gg = ((t >> 8) & 255) / 255.0; bb = (t & 255) / 255.0; break; }
        case RGBM_MUSIC: rr = ((mcol[z] >> 16) & 255) / 255.0; gg = ((mcol[z] >> 8) & 255) / 255.0; bb = (mcol[z] & 255) / 255.0; break;
        default: break;
        }
        cairo_rectangle(cr, kx0 + c * cell + 1.5, ky0 + r * cell + 1.5, cell - 3, cell - 3);
        if (zm == RGBM_OFF) rgba(cr, 0x151517, 1); else cairo_set_source_rgb(cr, rr * k, gg * k, bb * k);
        cairo_fill(cr);
    }
}

static void live_draw_meters(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
    (void)a; (void)w; (void)h; (void)d;
    double lev[4]; music_levels(lev, FALSE);
    cairo_scale(cr, kk, kk); cairo_translate(cr, -live_meters.x, -live_meters.y);
    double rw = live_meters.rw, mh = 40, bw = (rw - 30 - 3 * 8) / 4, my0 = live_meters.y;
    static const char *BL[4] = {"Graves", "Bas-méd.", "Médiums", "Aigus"};
    for (int i = 0; i < 4; i++) {
        double bx = live_meters.x + i * (bw + 8);
        cairo_rectangle(cr, bx, my0, bw, mh); rgba(cr, 0x141416, 1); cairo_fill(cr);
        cairo_rectangle(cr, bx, my0 + mh * (1 - lev[i]), bw, mh * lev[i]); rgba(cr, C_RED, 1); cairo_fill(cr);
        text(cr, BL[i], bx + bw / 2, my0 + mh + 10, 10, 400, F_SANS, C_MUTE, 1, 1, 0);
    }
    cairo_arc(cr, live_meters.x + rw - 10, my0 + mh / 2, 8, 0, 2 * G_PI); rgba(cr, C_RED, 0.15 + 0.85 * music_beat_env()); cairo_fill(cr);
}

/* place un widget animé sur son rectangle (unités de la scène) ; appelé après chaque dessin de l'interface */
static void live_place(Live *l) {
    if (!l->w) return;
    gboolean vis = l->show && scroll_y < 1;
    gtk_widget_set_visible(l->w, vis);
    if (!vis) return;
    int px = (int)floor(l->x * kk), py = (int)floor((l->y - VY0) * kk), sw = (int)ceil(l->rw * kk) + 2, sh = (int)ceil(l->rh * kk) + 2;
    if (gtk_widget_get_margin_start(l->w) != px) gtk_widget_set_margin_start(l->w, px);
    if (gtk_widget_get_margin_top(l->w) != MAX(py, 0)) gtk_widget_set_margin_top(l->w, MAX(py, 0));
    int cw, ch; gtk_widget_get_size_request(l->w, &cw, &ch);
    if (cw != sw || ch != sh) gtk_widget_set_size_request(l->w, sw, sh);
}

static gboolean live_tick(gpointer d) {
    (void)d;
    if (live_keys.w && live_keys.show && live_keys.animate) gtk_widget_queue_draw(live_keys.w);
    if (live_meters.w && live_meters.show && live_meters.animate) gtk_widget_queue_draw(live_meters.w);
    return G_SOURCE_CONTINUE;
}

static void page_rgb(cairo_t *cr) {
    RgbState *st = rgb_state();
    static int sel = 0; static gboolean dirty; static gint64 last_commit;
    static double hue, sat = 1, val = 1; static guint32 hsv_col = 0xffffffff;
    static char err[160];
    gboolean changed = FALSE;
    ptitle(cr, "Éclairage");
    live_keys.show = live_meters.show = FALSE;
    RgbStatus rs = rgb_status();
    if (rs == RGB_NONE) {
        text(cr, "Aucun clavier RGB compatible détecté (MSI MysticLight, 4 zones).", X0, 100, 14, 400, F_SANS, C_MUTE, 1, 0, 0);
        return;
    }
    if (rs == RGB_NOACCESS) {
        panel(cr, X0, 76, CW, 58, TRUE);
        text(cr, "Accès au clavier refusé : installe la règle udev une fois, puis rebranche ou relance.", X0 + 18, 96, 13, 600, F_SANS, C_TEXT, 1, 0, 0);
        text(cr, "sudo make install-udev   (dans le dossier des sources)", X0 + 18, 118, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
    } else text(cr, rgb_device_name(), X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    double top = rs == RGB_NOACCESS ? 150 : 106;
    double lw = MAX(440, CW * 0.52), rx = X0 + lw + 22, rw = XR - rx;

    gboolean custom = st->mode == RGBM_CUSTOM, music = st->mode == RGBM_MUSIC, mfixed = music && st->music_style == 0;
    static const char *ZN[4] = {"Gauche (WASD)", "Lettres", "Flèches", "Pavé num."};
    int emode = custom ? st->zmode[sel] : st->mode;             /* effet dont on règle couleur et vitesse */
    gboolean per_zone = custom || ((st->mode == RGBM_STATIC || mfixed) && !st->sync);

    /* --- aperçu du clavier (mêmes durées de cycle que le clavier réel) --- */
    double ph = 250;
    panel(cr, X0, top, lw, ph, FALSE);
    section(cr, "Aperçu", X0 + 18, top + 24);
    double cell = (lw - 44) / 20.0, kx0 = X0 + 22, ky0 = top + 56;
    lk_cell = cell;
    live_keys.x = kx0 - 2; live_keys.y = ky0 - 2; live_keys.rw = 20 * cell + 4; live_keys.rh = 6 * cell + 4;
    live_keys.show = TRUE; live_keys.animate = st->mode >= RGBM_BREATH;
    if (per_zone)
        for (int r = 0; r < 6; r++) for (int c = 0; c < 20; c++)
            if (c != 15 && !(r == 5 && c < 2) && clicked(kx0 + c * cell, ky0 + r * cell, cell, cell)) sel = key_zone(c);
    if (per_zone) {                                              /* liseré autour de la zone choisie */
        static const int C0[4] = {0, 5, 12, 16}, C1[4] = {5, 12, 16, 20};
        cairo_rectangle(cr, kx0 + C0[sel] * cell - 1, ky0 - 2, (C1[sel] - C0[sel]) * cell + 2, 6 * cell + 4);
        rgba(cr, 0xffffff, 0.9); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
        char zl[4][32]; for (int i = 0; i < 4; i++) g_strlcpy(zl[i], ZN[i], 32);
        int r = segmented(cr, X0 + 18, top + ph - 44, lw - 36, 30, zl, NULL, 4, zl[sel]);
        if (r >= 0) sel = r;
    } else text(cr, st->mode == RGBM_STATIC || mfixed ? "Les quatre zones partagent la même couleur." : music ? (st->music_style == 2 ? "Toutes les zones flashent sur les battements." : "Chaque zone suit une bande de fréquences.") : "Effet matériel : géré par le clavier.", X0 + 18, top + ph - 30, 12, 400, F_SANS, C_MUTE, 1, 0, 0);

    /* --- réglages --- */
    double y = top;
    section(cr, "Effet", rx, y + 6);
    static const char *ML[RGBM_COUNT] = {"Éteint", "Fixe", "Respiration", "Cycle", "Vague", "Personnalisé", "Musique"};
    char row1[4][32], row2[3][32];
    for (int i = 0; i < 4; i++) g_strlcpy(row1[i], ML[i], 32);
    for (int i = 0; i < 3; i++) g_strlcpy(row2[i], ML[4 + i], 32);
    int mr = segmented(cr, rx, y + 26, rw, 30, row1, NULL, 4, st->mode < 4 ? ML[st->mode] : "");
    int mr2 = segmented(cr, rx, y + 62, rw, 30, row2, NULL, 3, st->mode >= 4 ? ML[st->mode] : "");
    if (mr2 >= 0) mr = 4 + mr2;
    if (mr >= 0 && mr != st->mode) { st->mode = mr; changed = TRUE; }
    y += 108;

    if (custom) {
        char t[64]; g_snprintf(t, sizeof t, "Effet — %s", ZN[sel]);
        section(cr, t, rx, y);
        char zm[5][32]; for (int i = 0; i < 5; i++) g_strlcpy(zm[i], ML[i], 32);
        int r = segmented(cr, rx, y + 20, rw, 30, zm, NULL, 5, ML[st->zmode[sel]]);
        if (r >= 0 && r != st->zmode[sel]) { st->zmode[sel] = r; changed = TRUE; }
        y += 68;
        emode = st->zmode[sel];
    }
    if (music) {
        section(cr, "Synchronisation musicale", rx, y);
        text(cr, music_running() ? "Écoute la sortie audio du système." : "Capture audio inactive (parec requis).", rx, y + 24, 12, 400, F_SANS, music_running() ? C_OK : C_RED, 1, 0, 0);
        char ms[3][32] = {"Couleur choisie", "Spectre", "Battements"};
        int r = segmented(cr, rx, y + 38, rw, 30, ms, NULL, 3, ms[st->music_style]);
        if (r >= 0 && r != st->music_style) { st->music_style = r; changed = TRUE; }
        /* indicateurs en direct : 4 bandes + battement (widget animé) */
        live_meters.x = rx; live_meters.y = y + 80; live_meters.rw = rw; live_meters.rh = 40 + 16; live_meters.show = TRUE; live_meters.animate = TRUE;
        static double mv, ms_; gboolean rel; char b[24];
        if (active_id == NULL) { mv = st->music_sens; ms_ = st->music_smooth; }
        label(cr, "Sensibilité", rx, y + 148); g_snprintf(b, sizeof b, "%.0f %%", mv); value(cr, b, rx + rw, y + 148);
        if (slider(cr, rx + 4, y + 172, rw - 8, &mv, 0, 100, &rel)) { st->music_sens = (int)mv; changed = TRUE; }
        if (rel) { st->music_sens = (int)mv; changed = TRUE; }
        label(cr, "Lissage (retombée)", rx, y + 202); g_snprintf(b, sizeof b, "%.0f %%", ms_); value(cr, b, rx + rw, y + 202);
        if (slider(cr, rx + 4, y + 226, rw - 8, &ms_, 0, 100, &rel)) { st->music_smooth = (int)ms_; changed = TRUE; }
        if (rel) { st->music_smooth = (int)ms_; changed = TRUE; }
        text(cr, "Lueur de fond sans son", rx, y + 258, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
        if (toggle(cr, rx + rw - 44, y + 247, st->music_glow)) { st->music_glow = !st->music_glow; changed = TRUE; }
        y += 290;
    }

    if (emode == RGBM_STATIC || emode == RGBM_BREATH || mfixed) {
        int z = per_zone ? sel : 0;
        guint32 *tc = &st->color[z];
        if (*tc != hsv_col) { rgb2hsv(*tc, &hue, &sat, &val); hsv_col = *tc; }
        section(cr, "Couleur", rx, y + 6);
        gboolean can_sync = !custom && (st->mode == RGBM_STATIC || mfixed);
        if (can_sync) {
            text(cr, "Synchroniser", rx + rw - 44 - 10, y + 6, 12.5, 400, F_SANS, C_LABEL, 1, 2, 0);
            if (toggle(cr, rx + rw - 44, y - 5, st->sync)) {
                st->sync = !st->sync;
                if (st->sync) for (int i = 1; i < 4; i++) st->color[i] = st->color[0];
                changed = TRUE;
            }
        }
        double svw = rw, svh = 110, sy = y + 28;
        guint32 pure; hsv2rgb(hue, 1, 1, &pure);
        cairo_pattern_t *g1 = cairo_pattern_create_linear(rx, 0, rx + svw, 0);
        cairo_pattern_add_color_stop_rgb(g1, 0, 1, 1, 1); cairo_pattern_add_color_stop_rgb(g1, 1, ((pure >> 16) & 255) / 255.0, ((pure >> 8) & 255) / 255.0, (pure & 255) / 255.0);
        cairo_rectangle(cr, rx, sy, svw, svh); cairo_set_source(cr, g1); cairo_fill(cr); cairo_pattern_destroy(g1);
        cairo_pattern_t *g2 = cairo_pattern_create_linear(0, sy, 0, sy + svh);
        cairo_pattern_add_color_stop_rgba(g2, 0, 0, 0, 0, 0); cairo_pattern_add_color_stop_rgba(g2, 1, 0, 0, 0, 1);
        cairo_rectangle(cr, rx, sy, svw, svh); cairo_set_source(cr, g2); cairo_fill(cr); cairo_pattern_destroy(g2);
        double u = sat, v = 1 - val;
        if (drag2d(rx, sy, svw, svh, &u, &v)) { sat = u; val = 1 - v; changed = TRUE; }
        cairo_arc(cr, rx + sat * svw, sy + (1 - val) * svh, 6, 0, 2 * G_PI); rgba(cr, 0xffffff, 1); cairo_set_line_width(cr, 2); cairo_stroke(cr);
        double hy = sy + svh + 12;
        cairo_pattern_t *g3 = cairo_pattern_create_linear(rx, 0, rx + svw, 0);
        for (int i = 0; i <= 6; i++) { guint32 c; hsv2rgb(i * 60.0, 1, 1, &c); cairo_pattern_add_color_stop_rgb(g3, i / 6.0, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0); }
        cairo_rectangle(cr, rx, hy, svw, 16); cairo_set_source(cr, g3); cairo_fill(cr); cairo_pattern_destroy(g3);
        double hu = hue / 360, hv = 0;
        if (drag2d(rx, hy - 4, svw, 24, &hu, &hv)) { hue = hu * 359.999; changed = TRUE; }
        cairo_rectangle(cr, rx + hue / 360 * svw - 3, hy - 3, 6, 22); rgba(cr, 0xffffff, 1); cairo_fill(cr);
        static const guint32 SW_[8] = {0xff0033, 0xff7a00, 0xffe600, 0x22ff44, 0x00d5ff, 0x2a5cff, 0xb400ff, 0xffffff};
        double sw = (svw - 7 * 8) / 8, wy = hy + 28;
        for (int i = 0; i < 8; i++) {
            double wx = rx + i * (sw + 8); gboolean hv2 = hit(wx, wy, sw, 24);
            cairo_rectangle(cr, wx, wy, sw, 24); rgba(cr, SW_[i], 1); cairo_fill(cr);
            if (hv2 || *tc == SW_[i]) { cairo_rectangle(cr, wx - 1.5, wy - 1.5, sw + 3, 27); rgba(cr, 0xffffff, 1); cairo_set_line_width(cr, 1.5); cairo_stroke(cr); }
            if (clicked(wx, wy, sw, 24)) { rgb2hsv(SW_[i], &hue, &sat, &val); *tc = SW_[i]; hsv_col = SW_[i]; if (can_sync && st->sync) for (int k = 1; k < 4; k++) st->color[k] = SW_[i]; changed = TRUE; }
        }
        if (changed && (active_id != NULL || mclick)) {
            guint32 nc; hsv2rgb(hue, sat, val, &nc);
            *tc = nc; hsv_col = nc;
            if (can_sync && st->sync) for (int k = 0; k < 4; k++) st->color[k] = nc;
        }
        char hx[16]; g_snprintf(hx, sizeof hx, "#%06X", *tc);
        text(cr, hx, rx + 92, y + 6, 12.5, 600, F_SANS, C_TEXT, 1, 0, 0);
        y = wy + 40;
    }
    /* luminosité / vitesse / direction */
    gboolean any_wave = st->mode == RGBM_WAVE;
    if (custom) for (int i = 0; i < 4; i++) any_wave |= st->zmode[i] == RGBM_WAVE;
    if (st->mode != RGBM_OFF) {
        static double bv, sv; char b[48]; gboolean rel;
        int *spd = custom ? &st->zspeed[sel] : &st->speed;
        if (active_id == NULL) { bv = st->brightness; sv = *spd; }
        label(cr, "Luminosité", rx, y); g_snprintf(b, sizeof b, "%.0f %%", bv); value(cr, b, rx + rw, y);
        if (slider(cr, rx + 4, y + 26, rw - 8, &bv, 0, 100, &rel)) { st->brightness = (int)bv; changed = TRUE; }
        if (rel) { st->brightness = (int)bv; changed = TRUE; }
        y += 56;
        if (emode == RGBM_BREATH || emode == RGBM_CYCLE || emode == RGBM_WAVE) {
            label(cr, custom ? "Vitesse de la zone" : "Vitesse", rx, y);
            g_snprintf(b, sizeof b, "cycle de %.0f s", rgb_cycle_seconds((int)sv)); value(cr, b, rx + rw, y);
            if (slider(cr, rx + 4, y + 26, rw - 8, &sv, 0, 100, &rel)) { *spd = (int)sv; changed = TRUE; }
            if (rel) { *spd = (int)sv; changed = TRUE; }
            y += 56;
        }
        if (any_wave) {
            label(cr, "Direction de la vague", rx, y);
            char dl[2][32] = {"Vers la gauche", "Vers la droite"};
            int dr = segmented(cr, rx, y + 16, rw, 30, dl, NULL, 2, dl[st->dir_left ? 1 : 0]);
            if (dr >= 0 && dr != st->dir_left) { st->dir_left = dr; changed = TRUE; }
            y += 60;
        }
    }
    if (changed) dirty = TRUE;
    /* écriture limitée à ~12 fois par seconde pendant un glissement */
    if (dirty) {
        gint64 now = g_get_monotonic_time();
        if (now - last_commit >= 80000) {
            dirty = FALSE; last_commit = now;
            if (!rgb_commit(err, sizeof err) && err[0]) show_toast(err);
        } else schedule_redraw(100);
    }
    page_h = MAX(top + ph, y) + 20;
}

/* ------------------------------------------------------------------ GameVisual */
static void reload_hypr_then_reapply(void);

static void page_gv(cairo_t *cr) {
    GvState *g = gv_state();
    static gboolean dirty; static gint64 last; static char err[96];
    static double vb, vc, vs, vg, vk, vv, vsh; static gboolean init;
    gboolean changed = FALSE, rel;
    ptitle(cr, "GameVisual");
    text(cr, "Filtres couleur appliqués à tout l'écran. Un filtre actif peut désactiver le rendu direct des jeux en plein écran.", X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    if (!gv_supported()) text(cr, "GameVisual nécessite Hyprland.", X0, 104, 12.5, 400, F_SANS, C_RED, 1, 0, 0);
    double y = 118, bw = MIN(CW, 640);
    section(cr, "Préréglage", X0, y);
    text(cr, "Filtre activé", X0 + bw - 44 - 10, y, 12.5, 400, F_SANS, C_LABEL, 1, 2, 0);
    if (toggle(cr, X0 + bw - 44, y - 11, g->enabled)) { g->enabled = !g->enabled; changed = TRUE; }
    char r1[4][32], r2[4][32]; int np = 0;
    for (int i = 0; i < 4; i++) g_strlcpy(r1[i], gv_preset_name(i), 32);
    for (int i = 4; i < 7; i++) g_strlcpy(r2[i - 4], gv_preset_name(i), 32);
    g_strlcpy(r2[3], "Manuel", 32); (void)np;
    const char *cur = gv_preset_name(g->preset);
    int a1 = segmented(cr, X0, y + 20, bw, 34, r1, NULL, 4, cur);
    int a2 = segmented(cr, X0, y + 60, bw, 34, r2, NULL, 4, cur);
    if (a1 >= 0) { gv_select_preset(a1); g->enabled = TRUE; changed = TRUE; init = FALSE; }
    if (a2 >= 0) { if (a2 < 3) gv_select_preset(4 + a2); else g->preset = GV_MANUAL; g->enabled = TRUE; changed = TRUE; init = FALSE; }
    y += 124;

    if (!init || active_id == NULL) { vb = g->bright * 100; vc = g->contrast * 100; vs = g->sat * 100; vg = g->gamma; vk = g->kelvin; vv = g->vibrance * 100; vsh = g->sharpen * 100; init = TRUE; }
    section(cr, "Réglages", X0, y);
    struct { const char *l; double *v, lo, hi; char fmt; } R[7] = {
        {"Luminosité", &vb, 50, 150, 'p'}, {"Contraste", &vc, 50, 160, 'p'}, {"Saturation", &vs, 0, 200, 'p'}, {"Gamma (ombres)", &vg, 0.5, 1.8, 'g'}, {"Température de couleur", &vk, 3000, 10000, 'k'},
        {"Vibrance (couleurs ternes)", &vv, -50, 100, 'p'}, {"Netteté", &vsh, 0, 100, 'p'}};
    for (int i = 0; i < 7; i++) {
        double yy = y + 34 + i * 52; char b[32];
        label(cr, R[i].l, X0, yy);
        if (R[i].fmt == 'p') g_snprintf(b, sizeof b, "%.0f %%", *R[i].v); else if (R[i].fmt == 'g') g_snprintf(b, sizeof b, "%.2f", *R[i].v); else g_snprintf(b, sizeof b, "%.0f K", *R[i].v);
        value(cr, b, X0 + bw, yy);
        if (slider(cr, X0 + 4, yy + 24, bw - 8, R[i].v, R[i].lo, R[i].hi, &rel) || rel) {
            g->bright = vb / 100; g->contrast = vc / 100; g->sat = vs / 100; g->gamma = vg; g->kelvin = (int)vk; g->vibrance = vv / 100; g->sharpen = vsh / 100;
            g->preset = GV_MANUAL; g->enabled = TRUE; changed = TRUE;
        }
    }
    if (outline_btn(cr, X0, y + 34 + 7 * 52 + 6, 140, 30, "Réinitialiser", 12.5)) { gv_select_preset(0); init = FALSE; changed = TRUE; }
    if (changed) dirty = TRUE;
    if (dirty) {
        gint64 now = g_get_monotonic_time();
        if (now - last >= 100000) { dirty = FALSE; last = now; if (!gv_apply(err, sizeof err) && err[0]) show_toast(err); }
        else schedule_redraw(120);
    }
    page_h = y + 34 + 7 * 52 + 60;
}

/* ------------------------------------------------------------------ Macros */
static int mx_bind_idx = -1;            /* macro dont on capture le raccourci */
static int mx_open_idx = -1;            /* macro dont on affiche les étapes */

static int ask_kind;                     /* 0 = nom de macro, 1 = arguments d'un jeu Windows, 2 = nom d'un jeu Windows, 3 = recherche Steam, 4 = code Steam Guard, 5 = code Epic, 6 = recherche FitGirl */

static void ask_name_done(GtkEntry *e, gpointer d) {
    int i = GPOINTER_TO_INT(d);
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    if (ask_kind == 0) { if (*t) { mx_rename(i, t); reload_hypr_then_reapply(); } }
    else if (ask_kind == 3) { if (*t) fx_steamsearch_query(t); }
    else if (ask_kind == 4) { if (*t) fx_steam_guard_code(t); }
    else if (ask_kind == 5) { if (*t) fx_epic_login(t); }
    else if (ask_kind == 6) { if (*t) fx_fg_search(t); }
    else { int n; WinGame *w = wg_list(&n); if (i >= 0 && i < n) { g_strlcpy(ask_kind == 1 ? w[i].args : w[i].name, t, ask_kind == 1 ? sizeof w[i].args : sizeof w[i].name); wg_save(); } }
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(e))));
    if (area) gtk_widget_queue_draw(area);
}


/* petite fenêtre de saisie (l'interface dessinée n'a pas de champ texte) */
static void ask_text(int kind, int i, const char *title, const char *initial) {
    ask_kind = kind;
    GtkWidget *w = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w), title);
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(win)); gtk_window_set_modal(GTK_WINDOW(w), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(w), 420, 90);
    GtkWidget *e = gtk_entry_new(); gtk_editable_set_text(GTK_EDITABLE(e), initial);
    gtk_widget_set_margin_start(e, 16); gtk_widget_set_margin_end(e, 16); gtk_widget_set_margin_top(e, 16); gtk_widget_set_margin_bottom(e, 16);
    g_signal_connect(e, "activate", G_CALLBACK(ask_name_done), GINT_TO_POINTER(i));
    gtk_window_set_child(GTK_WINDOW(w), e); gtk_window_present(GTK_WINDOW(w)); gtk_widget_grab_focus(e);
}
static void ask_name(int i, const char *initial) { ask_text(0, i, "Nom de la macro", initial); }

/* fenêtre de connexion Steam (identifiant + mot de passe) : le mot de passe n'est jamais mémorisé par Coreboard,
   seul steamcmd garde sa propre session locale une fois la connexion réussie. */
static void steam_login_go(GtkWidget *btn, gpointer d) {
    GtkWidget *box = d;
    GtkWidget *ue = g_object_get_data(G_OBJECT(box), "user"), *pe = g_object_get_data(G_OBJECT(box), "pass");
    const char *u = gtk_editable_get_text(GTK_EDITABLE(ue)), *p = gtk_editable_get_text(GTK_EDITABLE(pe));
    if (*u) fx_steam_login(u, p);
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(btn)));
    if (area) gtk_widget_queue_draw(area);
}
static void ask_steam_login(void) {
    GtkWidget *w = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w), "Connexion à Steam");
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(win)); gtk_window_set_modal(GTK_WINDOW(w), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(w), 360, 160);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16); gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    GtkWidget *ue = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(ue), "Identifiant Steam");
    gtk_editable_set_text(GTK_EDITABLE(ue), fx_steam_username());
    GtkWidget *pe = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(pe), "Mot de passe");
    gtk_entry_set_visibility(GTK_ENTRY(pe), FALSE);
    GtkWidget *go = gtk_button_new_with_label("Connexion");
    g_object_set_data(G_OBJECT(box), "user", ue); g_object_set_data(G_OBJECT(box), "pass", pe);
    g_signal_connect(go, "clicked", G_CALLBACK(steam_login_go), box);
    g_signal_connect(pe, "activate", G_CALLBACK(steam_login_go), box);
    gtk_box_append(GTK_BOX(box), ue); gtk_box_append(GTK_BOX(box), pe); gtk_box_append(GTK_BOX(box), go);
    gtk_window_set_child(GTK_WINDOW(w), box); gtk_window_present(GTK_WINDOW(w)); gtk_widget_grab_focus(ue);
}

static gboolean small_btn(cairo_t *cr, double x, double y, double w, const char *t, gboolean on) {
    gboolean hov = hit(x, y, w, 28);
    cairo_rectangle(cr, x, y, w, 28); rgba(cr, on ? 0x260a10 : (hov ? 0x1a1a1c : 0x0e0e10), 1); cairo_fill(cr);
    cairo_rectangle(cr, x + .5, y + .5, w - 1, 27); rgba(cr, on ? C_RED : (hov ? 0xffffff : C_LINE2), 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    text(cr, t, x + w / 2, y + 14, 12, 600, F_SANS, C_TEXT, 1, 1, 0);
    return clicked(x, y, w, 28);
}

static void page_macros(cairo_t *cr) {
    int n; Macro *m = mx_list(&n);
    ptitle(cr, "Macros");
    text(cr, "Enregistre des touches tapées dans cette fenêtre, puis rejoue-les ou lance-les avec un raccourci.", X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    if (!mx_wtype_available()) {
        const char *tool = macro_tool_for_session(); char b[200];
        g_snprintf(b, sizeof b, "Rejeu impossible : %s est nécessaire pour simuler le clavier%s.", tool, !strcmp(tool, "ydotool") ? " (avec le service ydotoold actif)" : "");
        text(cr, b, X0, 104, 12.5, 400, F_SANS, C_RED, 1, 0, 0);
        if (outline_btn(cr, XR - 400, 38, 130, 26, pkg_busy ? "Installation…" : "Installer", 12) && !pkg_busy) install_pkg_async(tool, tool);
    }
    if (outline_btn(cr, XR - 260, 38, 130, 26, "Nouvelle macro", 12)) {
        char nm[48]; g_snprintf(nm, sizeof nm, "Macro %d", n + 1);
        int i = mx_add(nm); if (i >= 0) { mx_rec_start(i, FALSE); mx_open_idx = i; } else show_toast("Nombre maximal de macros atteint");
        return;
    }
    double y = 116;
    if (mx_recording()) {
        int ri = mx_rec_index();
        panel(cr, X0, y, CW, 60, TRUE);
        cairo_arc(cr, X0 + 28, y + 30, 6, 0, 2 * G_PI); rgba(cr, C_RED, 0.5 + 0.5 * sin(g_get_monotonic_time() / 3e5)); cairo_fill(cr);
        char b[160]; g_snprintf(b, sizeof b, "Enregistrement de « %s » — tape tes touches dans cette fenêtre (%d étapes)", m[ri].name, m[ri].nsteps);
        text_fit(cr, b, X0 + 46, y + 30, 13.5, 600, F_SANS, C_TEXT, CW - 190, 0);
        if (outline_btn(cr, XR - 130, y + 15, 112, 30, "Terminer", 13)) mx_rec_stop();
        schedule_redraw(200);
        y += 76;
    } else if (mx_bind_idx >= 0) {
        panel(cr, X0, y, CW, 60, TRUE);
        text(cr, "Appuie sur la combinaison à assigner (ex. Ctrl + Alt + 1). Échap : annuler — Suppr : retirer le raccourci.", X0 + 18, y + 30, 13.5, 600, F_SANS, C_TEXT, 1, 0, 0);
        y += 76;
    }
    if (!n) { text(cr, "Aucune macro. Clique sur « Nouvelle macro », puis tape la séquence de touches à enregistrer.", X0, y + 14, 14, 400, F_SANS, C_MUTE, 1, 0, 0); y += 44; }
    for (int i = 0; i < n; i++) {
        double h = 96;
        panel(cr, X0, y, CW, h, mx_recording() && mx_rec_index() == i);
        if (hit(X0 + 18, y + 12, 260, 28) && clicked(X0 + 18, y + 12, 260, 28)) { ask_name(i, m[i].name); }
        text_fit(cr, m[i].name, X0 + 18, y + 26, 15, 600, F_SANS, C_TEXT, 260, 0);
        char b[120]; g_snprintf(b, sizeof b, "%d étape%s · raccourci : %s", m[i].nsteps, m[i].nsteps > 1 ? "s" : "", m[i].bind[0] ? m[i].bind : "aucun");
        text_fit(cr, b, X0 + 18, y + 52, 11.5, 400, F_SANS, C_MUTE, 300, 0);
        /* boutons alignés à droite */
        double bx = XR - 18 - 28;
        if (small_btn(cr, bx, y + 14, 28, "×", FALSE)) { mx_remove(i); if (mx_open_idx == i) mx_open_idx = -1; else if (mx_open_idx > i) mx_open_idx--; reload_hypr_then_reapply(); return; }
        bx -= 8 + 84; if (small_btn(cr, bx, y + 14, 84, mx_open_idx == i ? "Étapes ▴" : "Étapes ▾", mx_open_idx == i)) mx_open_idx = mx_open_idx == i ? -1 : i;
        bx -= 8 + 100; if (small_btn(cr, bx, y + 14, 100, "Raccourci", mx_bind_idx == i)) {
            if (compat_win_backend() == WB_HYPRLAND) { mx_bind_idx = mx_bind_idx == i ? -1 : i; mx_rec_stop(); }
            else {                                             /* autres bureaux : raccourci à déclarer dans leurs réglages */
                gchar *qn = g_shell_quote(m[i].name), *cmd = g_strdup_printf("coreboard --macro %s", qn);
                gdk_clipboard_set_text(gtk_widget_get_clipboard(area), cmd);
                show_toast("Commande copiée : ajoute-la comme raccourci personnalisé dans les réglages clavier du bureau");
                g_free(cmd); g_free(qn);
            }
        }
        bx -= 8 + 110; if (small_btn(cr, bx, y + 14, 110, "Enregistrer", mx_recording() && mx_rec_index() == i)) { if (mx_recording() && mx_rec_index() == i) mx_rec_stop(); else { mx_rec_start(i, FALSE); mx_bind_idx = -1; mx_open_idx = i; } }
        bx -= 8 + 84; if (small_btn(cr, bx, y + 14, 84, m[i].playing ? "…" : "Lancer", FALSE)) { mx_play_async(i); show_toast("Macro lancée : le focus doit être sur l'application cible"); }
        /* répétitions */
        double rx = XR - 18 - 130;
        text(cr, "Répéter", rx, y + 68, 12, 400, F_SANS, C_LABEL, 1, 0, 0);
        if (small_btn(cr, rx + 60, y + 54, 26, "−", FALSE) && m[i].repeat > 1) { m[i].repeat--; mx_save(); }
        g_snprintf(b, sizeof b, "×%d", m[i].repeat); text(cr, b, rx + 100, y + 68, 12.5, 600, F_SANS, C_TEXT, 1, 1, 0);
        if (small_btn(cr, rx + 114, y + 54, 26, "+", FALSE) && m[i].repeat < 99) { m[i].repeat++; mx_save(); }
        y += h + 10;
        if (mx_open_idx == i) {
            double lh = 26 * m[i].nsteps + 46;
            panel(cr, X0, y - 4, CW, lh, FALSE);
            for (int s = 0; s < m[i].nsteps; s++) {
                char sb[200]; text_fit(cr, mx_step_label(&m[i].steps[s], sb, sizeof sb), X0 + 18, y + 14 + s * 26, 12.5, 400, F_SANS, C_LABEL, CW - 90, 0);
                double dx = XR - 18 - 22; gboolean dh = hit(dx, y + 3 + s * 26, 22, 22);
                cairo_move_to(cr, dx + 6, y + 9 + s * 26); cairo_line_to(cr, dx + 16, y + 19 + s * 26); cairo_move_to(cr, dx + 16, y + 9 + s * 26); cairo_line_to(cr, dx + 6, y + 19 + s * 26);
                rgba(cr, dh ? C_RED : C_MUTE, 1); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
                if (clicked(dx, y + 3 + s * 26, 22, 22)) { mx_step_remove(i, s); return; }
            }
            double py = y + 26 * m[i].nsteps + 6;
            if (!m[i].nsteps) text(cr, "Aucune étape enregistrée.", X0 + 18, y + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0), py = y + 30;
            text(cr, "Ajouter une pause :", X0 + 18, py + 14, 12, 400, F_SANS, C_LABEL, 1, 0, 0);
            static const int PMS[3] = {100, 500, 1000}; static const char *PL[3] = {"+100 ms", "+500 ms", "+1 s"};
            for (int k = 0; k < 3; k++) if (small_btn(cr, X0 + 150 + k * 84, py, 76, PL[k], FALSE)) mx_step_add_delay(i, PMS[k]);
            y += lh + 6;
        }
    }
    page_h = y + 20;
}

/* ------------------------------------------------------------------ Jeux Windows (Proton-GE) */
static void on_exe_chosen(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    gchar *path = g_file_get_path(f); g_object_unref(f);
    int i = path ? wg_add(path) : -1;
    if (i < 0) show_toast("Impossible d'ajouter ce jeu (liste pleine ?)");
    else { int n; WinGame *w = wg_list(&n); char b[160]; g_snprintf(b, sizeof b, "Ajouté : %s%s", w[i].name, w[i].appid ? " (corrections Proton connues)" : ""); show_toast(b); }
    g_free(path);
    if (area) gtk_widget_queue_draw(area);
}

static void pick_exe(void) {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choisir l'exécutable du jeu (.exe)");
    GtkFileFilter *ff = gtk_file_filter_new(); gtk_file_filter_set_name(ff, "Exécutables Windows");
    gtk_file_filter_add_pattern(ff, "*.exe"); gtk_file_filter_add_pattern(ff, "*.EXE");
    GListStore *fl = g_list_store_new(GTK_TYPE_FILE_FILTER); g_list_store_append(fl, ff);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fl)); gtk_file_dialog_set_default_filter(d, ff);
    gtk_file_dialog_open(d, GTK_WINDOW(win), NULL, on_exe_chosen, NULL);
    g_object_unref(ff); g_object_unref(fl); g_object_unref(d);
}

static void on_tune_done(gboolean ok, gpointer d) { (void)d; show_toast(ok ? "Système optimisé (reconnecte ta session pour GameMode)" : "Optimisation annulée ou échouée"); if (area) gtk_widget_queue_draw(area); }

static void on_deps_done(gboolean ok, gpointer d) { (void)d; show_toast(ok ? "Composants installés" : "Installation annulée ou échouée"); if (area) gtk_widget_queue_draw(area); }


static void page_windows(cairo_t *cr) {
    static int open_idx = -1;
    int n; WinGame *w = wg_list(&n); WgTools t = wg_tools();
    ptitle(cr, "Jeux Windows");
    text(cr, "Lance des jeux Windows (.exe) avec Proton-GE : DirectX 11/12 est traduit en Vulkan, avec les corrections automatiques pour les jeux connus.", X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    if (outline_btn(cr, XR - 260, 38, 130, 26, "Ajouter un .exe", 12)) { pick_exe(); return; }

    /* état de l'environnement */
    double y = 104;
    panel(cr, X0, y, CW, 66, FALSE);
    const struct { const char *l; gboolean ok; } T[4] = {{"umu-launcher", t.umu}, {"Proton-GE", t.proton}, {"GameMode", t.gamemode}, {"MangoHud", t.mangohud}};
    double cx = X0 + 18;
    for (int i = 0; i < 4; i++) {
        cairo_arc(cr, cx + 6, y + 22, 5, 0, 2 * G_PI); rgba(cr, T[i].ok ? C_OK : (i == 1 ? 0xe0a800 : C_RED), 1); cairo_fill(cr);
        double tw = text(cr, T[i].l, cx + 18, y + 22, 12.5, 600, F_SANS, C_TEXT, 1, 0, 0);
        cx += 18 + tw + 26;
    }
    WgDl dl = wg_dl();
    if (!t.proton && t.umu && dl.state == 0 && wg_download_running()) wg_install_proton_async();   /* se raccorde à un téléchargement lancé ailleurs */
    if (!t.proton && t.umu && dl.state >= 1 && dl.state <= 3) {
        char b[200];
        if (dl.state == 2 && dl.total > 0) g_snprintf(b, sizeof b, "Téléchargement de Proton-GE : %.0f / %.0f Mo (%.0f %%) — reprend automatiquement si la connexion tombe", dl.done / 1048576.0, dl.total / 1048576.0, 100.0 * dl.done / dl.total);
        else g_snprintf(b, sizeof b, "%s", dl.msg);
        text_fit(cr, b, X0 + 18, y + 46, 11.5, 400, F_SANS, C_TEXT, CW - 36, 0);
        if (dl.state == 2 && dl.total > 0) { cairo_rectangle(cr, X0 + 18, y + 58, CW - 36, 3); rgba(cr, 0x2a2a2e, 1); cairo_fill(cr); cairo_rectangle(cr, X0 + 18, y + 58, (CW - 36) * dl.done / (double)dl.total, 3); rgba(cr, C_RED, 1); cairo_fill(cr); }
        schedule_redraw(1000);
    } else if (!t.proton && t.umu) {
        text(cr, dl.state == 5 ? dl.msg : "Proton-GE (environ 540 Mo) est nécessaire pour lancer les jeux.", X0 + 18, y + 46, 11.5, 400, F_SANS, dl.state == 5 ? C_RED : C_MUTE, 1, 0, 0);
        if (outline_btn(cr, XR - 18 - 250, y + 18, 250, 30, dl.state == 5 ? "Reprendre le téléchargement" : "Télécharger Proton-GE", 12.5)) wg_install_proton_async();
    }
    else if (!t.umu || !t.gamemode || !t.mangohud) text(cr, "Composants manquants.", X0 + 18, y + 46, 11.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    else text(cr, "Environnement prêt.", X0 + 18, y + 46, 11.5, 400, F_SANS, C_OK, 1, 0, 0);
    if (!t.umu || !t.gamemode || !t.mangohud) {
        if (wg_installing()) text(cr, "Installation en cours (mot de passe demandé)…", XR - 18, y + 33, 12.5, 600, F_SANS, C_TEXT, 1, 2, 0);
        else if (outline_btn(cr, XR - 18 - 250, y + 18, 250, 30, "Installer les composants manquants", 12.5)) wg_install_deps(on_deps_done, NULL);
    }
    y += 82;

    /* diagnostic système : ce qui limite les performances des jeux */
    static gboolean diag_open = TRUE; static TuneItem items[16]; static int nitems; static gint64 diag_at; static gboolean fixable;
    if (g_get_monotonic_time() - diag_at > 5 * G_USEC_PER_SEC) { nitems = tune_check(items, 16, &fixable); diag_at = g_get_monotonic_time(); }
    int nbad = 0; for (int k = 0; k < nitems; k++) nbad += items[k].status == 1;
    section(cr, "Optimisation du système", X0, y);
    { char sb[80]; g_snprintf(sb, sizeof sb, nbad ? "%d point%s à corriger" : "Tout est en ordre", nbad, nbad > 1 ? "s" : "");
      text(cr, sb, X0 + CW - 40, y, 12.5, 600, F_SANS, nbad ? 0xe0a800 : C_OK, 1, 2, 0); }
    cairo_move_to(cr, XR - 18, diag_open ? y + 3 : y - 3); cairo_line_to(cr, XR - 11, diag_open ? y - 4 : y + 4); cairo_line_to(cr, XR - 4, diag_open ? y + 3 : y - 3);
    rgba(cr, 0xd0d0d4, 1); cairo_set_line_width(cr, 1.6); cairo_stroke(cr);
    if (clicked(XR - 30, y - 12, 34, 24)) diag_open = !diag_open;
    y += 18;
    if (diag_open) {
        int rows = (nitems + 1) / 2; double dh = rows * 26 + 18 + (fixable ? 44 : 0);
        panel(cr, X0, y, CW, dh, FALSE);
        double colw = (CW - 36) / 2;
        for (int k = 0; k < nitems; k++) {
            double ox = X0 + 18 + (k % 2) * colw, oy = y + 18 + (k / 2) * 26;
            cairo_arc(cr, ox + 5, oy, 4.5, 0, 2 * G_PI); rgba(cr, items[k].status == 0 ? C_OK : items[k].status == 1 ? 0xe0a800 : 0x6c6c72, 1); cairo_fill(cr);
            text_fit(cr, items[k].label, ox + 16, oy, 12, 600, F_SANS, C_TEXT, colw * 0.42, 0);
            text_fit(cr, items[k].value, ox + 16 + colw * 0.44, oy, 11.5, 400, F_SANS, items[k].status == 1 ? 0xe0a800 : C_LABEL, colw * 0.56 - 24, 0);
        }
        if (fixable) {
            double by = y + 18 + rows * 26;
            if (tune_fixing()) text(cr, "Optimisation en cours (mot de passe demandé)…", X0 + 18, by + 16, 12.5, 600, F_SANS, C_TEXT, 1, 0, 0);
            else if (outline_btn(cr, X0 + 18, by + 2, 290, 30, "Optimiser le système (ntsync, GameMode…)", 12.5)) tune_fix_async(on_tune_done, NULL);
            text(cr, "Charge ntsync, règle les limites mémoire et autorise GameMode à régler le processeur.", X0 + 322, by + 17, 11.5, 400, F_SANS, C_MUTE, 1, 0, 0);
        }
        y += dh + 14;
    } else y += 6;

    if (!n) { text(cr, "Aucun jeu. Clique sur « Ajouter un .exe » puis choisis l'exécutable (ex. b1-Win64-Shipping.exe de Black Myth: Wukong).", X0, y + 14, 14, 400, F_SANS, C_MUTE, 1, 0, 0); y += 44; }
    for (int i = 0; i < n; i++) {
        double h = 112;
        panel(cr, X0, y, CW, h, w[i].running);
        if (clicked(X0 + 18, y + 12, 300, 28)) { ask_text(2, i, "Nom du jeu", w[i].name); return; }
        text_fit(cr, w[i].name, X0 + 18, y + 26, 15, 600, F_SANS, C_TEXT, 300, 0);
        text_fit(cr, w[i].exe, X0 + 18, y + 46, 11, 400, F_SANS, C_MUTE, CW - 330, 0);
        text_fit(cr, w[i].analyzed ? w[i].info : "Analyse de l'exécutable…", X0 + 18, y + 66, 11.5, 600, F_SANS, C_LABEL, CW - 36, 0);
        if (w[i].warn[0] && !w[i].running) text_fit(cr, w[i].warn, X0 + 18, y + 88, 11.5, 400, F_SANS, w[i].kernel_ac ? C_RED : 0xe0a800, CW - 36, 0);
        else text_fit(cr, w[i].status, X0 + 18, y + 88, 11.5, 400, F_SANS, w[i].running ? C_OK : C_MUTE, CW - 36, 0);
        double bx = XR - 18 - 28;
        if (small_btn(cr, bx, y + 14, 28, "×", FALSE)) { wg_remove(i, FALSE); if (open_idx == i) open_idx = -1; return; }
        bx -= 8 + 90; if (small_btn(cr, bx, y + 14, 90, open_idx == i ? "Options ▴" : "Options ▾", open_idx == i)) open_idx = open_idx == i ? -1 : i;
        bx -= 8 + 100;
        static int armed = -1; static gint64 armed_until;
        gboolean is_armed = armed == i && armed_until > g_get_monotonic_time();
        if (small_btn(cr, bx, y + 14, 100, w[i].running ? "Arrêter" : is_armed ? "Confirmer ?" : "Jouer", w[i].running || is_armed)) {
            if (w[i].running) wg_stop(i);
            else if (w[i].kernel_ac && !is_armed) { armed = i; armed_until = g_get_monotonic_time() + 6 * G_USEC_PER_SEC; show_toast("Anti-triche noyau ou architecture incompatible : clique de nouveau pour lancer malgré le risque"); schedule_redraw(6100); }
            else { armed = -1; wg_launch(i); show_toast("Lancement… le premier démarrage d'un jeu prépare Proton et peut durer"); }
        }
        y += h + 10;
        if (open_idx == i) {
            double oh = 432 + (w[i].fsr ? 40 : 0) + (w[i].dlss ? 214 : 0);
            panel(cr, X0, y - 4, CW, oh, FALSE);
            double oy0 = y + 4, colw = (CW - 36 - 24) / 2;
            section(cr, "Amélioration des performances et du rendu", X0 + 18, oy0 + 12);
            char pn[5][32]; for (int k = 0; k < 5; k++) g_strlcpy(pn[k], wg_preset_name(k == 0 ? 1 : k == 1 ? 2 : k == 2 ? 3 : k == 3 ? 4 : 0), 32);
            /* ordre affiché : Performance, Équilibré, Qualité, Économie, Personnalisé */
            int cur = w[i].preset == 1 ? 0 : w[i].preset == 2 ? 1 : w[i].preset == 3 ? 2 : w[i].preset == 4 ? 3 : 4;
            int pr = segmented(cr, X0 + 18, oy0 + 30, CW - 36, 32, pn, NULL, 5, pn[cur]);
            if (pr >= 0 && pr < 4) { wg_apply_preset(i, pr + 1); }
            struct { const char *l; int *v; gboolean pre; } O[9] = {
                {"ntsync (synchronisation noyau)", &w[i].ntsync, TRUE}, {"Compilation de shaders asynchrone", &w[i].shader_async, TRUE},
                {"Faible latence (file d'images = 1)", &w[i].lowlat, TRUE}, {"Filtrage anisotrope 16x", &w[i].aniso, TRUE},
                {"DLSS / NVAPI (NVIDIA)", &w[i].dlss, TRUE}, {"DLSS : mise à jour automatique", &w[i].dlss_up, TRUE},
                {"Ray tracing (DXR)", &w[i].rt, TRUE}, {"Mise à l'échelle FSR (plein écran)", &w[i].fsr, TRUE}, {"GameMode (priorité CPU)", &w[i].gamemode, TRUE}};
            for (int k = 0; k < 9; k++) {
                double ox = X0 + 18 + (k % 2) * (colw + 24), oy = oy0 + 76 + (k / 2) * 34;
                text_fit(cr, O[k].l, ox, oy + 12, 12.5, 400, F_SANS, C_LABEL, colw - 60, 0);
                if (toggle(cr, ox + colw - 44, oy, *O[k].v)) { *O[k].v = !*O[k].v; w[i].preset = 0; wg_save(); }
            }
            double ly = oy0 + 76 + 5 * 34 + 8;
            text(cr, "Limiteur d'images", X0 + 18, ly + 14, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
            char cp[6][32] = {"Illimité", "30", "60", "90", "120", "144"}; static const int CV[6] = {0, 30, 60, 90, 120, 144};
            int ci = 0; for (int k = 0; k < 6; k++) if (CV[k] == w[i].fps_cap) ci = k;
            int cr2 = segmented(cr, X0 + 18 + 160, ly, MIN(CW - 36 - 160, 480), 30, cp, NULL, 6, cp[ci]);
            if (cr2 >= 0) { w[i].fps_cap = CV[cr2]; w[i].preset = 0; wg_save(); }
            double fy = ly + 40;
            if (w[i].fsr) {
                char fs[6][32] = {"Ultra qualité", "Qualité", "Équilibré", "Performance", "Max", "Net"}; (void)fs;
                text(cr, "Netteté FSR", X0 + 18, fy + 14, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
                char fl[6][32] = {"0", "1", "2", "3", "4", "5"}; char cur2[8]; g_snprintf(cur2, sizeof cur2, "%d", w[i].fsr_str);
                int fr = segmented(cr, X0 + 18 + 160, fy, 300, 30, fl, NULL, 6, cur2);
                if (fr >= 0) { w[i].fsr_str = fr; wg_save(); }
                fy += 40;
            }
            if (w[i].dlss) {                                                       /* --- DLSS --- */
                const DlssGpu *gp = dlss_gpu();
                section(cr, "DLSS", X0 + 18, fy + 10);
                char gl[220];
                g_snprintf(gl, sizeof gl, "%s   ·   Super Resolution %s   Ray Reconstruction %s   Frame Generation %s   Multi Frame Gen. %s   DLSS 5 %s",
                           gp->name[0] ? gp->name : "GPU non NVIDIA", gp->sr ? "✓" : "✗", gp->rr ? "✓" : "✗", gp->fg ? "✓" : "✗", gp->mfg ? "✓" : "✗", gp->dlss5 ? "✓" : "✗");
                text_fit(cr, gl, X0 + 18, fy + 34, 11.5, 400, F_SANS, C_LABEL, CW - 36, 0);
                text_fit(cr, w[i].dlss_info[0] ? w[i].dlss_info : "Analyse du dossier du jeu…", X0 + 18, fy + 54, 11.5, 400, F_SANS, C_MUTE, CW - 36, 0);
                text(cr, "Modèle Super Resolution", X0 + 18, fy + 82, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
                char sm[6][32] = {"Choix du jeu", "Le plus récent", "J", "K", "L (4.5)", "M (4.5)"};
                int sr = segmented(cr, X0 + 18 + 190, fy + 68, MIN(CW - 36 - 190, 520), 30, sm, NULL, 6, sm[CLAMP(w[i].sr_preset, 0, 5)]);
                if (sr >= 0) { w[i].sr_preset = sr; wg_save(); }
                struct { const char *l; int *v; gboolean ok; } D[3] = {
                    {gp->fg ? "Frame Generation (2x, RTX 40 et +)" : "Frame Generation : GPU non compatible", &w[i].fg, gp->fg},
                    {"Ray Reconstruction : dernier modèle", &w[i].rr_latest, gp->rr}, {"Indicateur DLSS à l'écran (vérification)", &w[i].dlss_ind, TRUE}};
                for (int k = 0; k < 3; k++) {
                    double ox = X0 + 18 + (k % 2) * (colw + 24), oy = fy + 108 + (k / 2) * 34;
                    text_fit(cr, D[k].l, ox, oy + 12, 12.5, 400, F_SANS, D[k].ok ? C_LABEL : C_MUTE, colw - 60, 0);
                    if (D[k].ok && toggle(cr, ox + colw - 44, oy, *D[k].v)) { *D[k].v = !*D[k].v; wg_save(); }
                }
                const char *r5 = dlss5_reason();
                text_fit(cr, r5[0] ? "DLSS 5 : " : "DLSS 5 : prêt (GPU compatible) — activé par le jeu s'il le propose", X0 + 18, fy + 184, 11.5, 600, F_SANS, r5[0] ? 0xe0a800 : C_OK, 200, 0);
                if (r5[0]) text_fit(cr, r5, X0 + 18 + 62, fy + 184, 11.5, 400, F_SANS, 0xe0a800, CW - 36 - 62, 0);
                fy += 214;
            }
            char b[200]; g_snprintf(b, sizeof b, w[i].appid ? "Corrections Proton : oui (identifiant %d)" : "Corrections Proton : aucune connue pour ce nom", w[i].appid);
            text(cr, b, X0 + 18, fy + 10, 11.5, 400, F_SANS, w[i].appid ? C_OK : C_MUTE, 1, 0, 0);
            g_snprintf(b, sizeof b, w[i].args[0] ? "Arguments : %s" : "Arguments : aucun", w[i].args);
            text_fit(cr, b, X0 + 18, fy + 30, 11.5, 400, F_SANS, C_MUTE, CW - 36, 0);
            struct { const char *l; int *v; } O2[2] = {{"Afficher les FPS (MangoHud)", &w[i].hud}, {"Journal détaillé (débogage)", &w[i].log}};
            for (int k = 0; k < 2; k++) {
                double ox = X0 + 18 + k * (colw + 24), oy = fy + 46;
                text(cr, O2[k].l, ox, oy + 12, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
                if (toggle(cr, ox + colw - 44, oy, *O2[k].v)) { *O2[k].v = !*O2[k].v; wg_save(); }
            }
            double bb = fy + 88;
            if (small_btn(cr, X0 + 18, bb, 100, "Arguments…", FALSE)) { ask_text(1, i, "Arguments de lancement", w[i].args); return; }
            if (small_btn(cr, X0 + 126, bb, 130, "Ouvrir le préfixe", FALSE)) open_path(w[i].prefix);
            if (small_btn(cr, X0 + 264, bb, 130, "Ouvrir le journal", FALSE)) { char lp[600]; wg_log_path(i, lp, sizeof lp); open_path(lp); }
            if (small_btn(cr, X0 + 402, bb, 160, "Supprimer le préfixe", FALSE)) { const char *a2[] = {"rm", "-rf", "--", w[i].prefix, NULL}; g_spawn_async(NULL, (char **)a2, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL); show_toast("Préfixe supprimé (sauvegardes et réglages du jeu inclus)"); }
            y += oh + 6;
        }
    }
    page_h = y + 20;
}

/* ------------------------------------------------------------------ chrome (barre de titre + latérale) */
static void nav_item(cairo_t *cr, double y, int pg, const char *t, int ic) {
    const double h = 34;
    gboolean on = page == pg, hov = hit(0, y, SBW, h);
    if (on) {
        cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, SBW, 0);
        cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.07, 0.17, 0.42); cairo_pattern_add_color_stop_rgba(p, 1, 1, 0.07, 0.17, 0.03);
        cairo_rectangle(cr, 0, y, SBW, h); cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
        cairo_rectangle(cr, 0, y, 3, h); rgba(cr, C_RED, 1); cairo_fill(cr);
    } else if (hov) { cairo_rectangle(cr, 0, y, SBW, h); rgba(cr, 0xffffff, 0.05); cairo_fill(cr); }
    guint32 col = on || hov ? 0xffffff : 0xd6d6da;
    icon(cr, ic, 26, y + h / 2, col, 0.68);
    text(cr, t, 48, y + h / 2, 13.5, 400, F_SANS, col, 1, 0, 0);
    if (clicked(0, y, SBW, h)) page = pg;
}
static void nav_section(cairo_t *cr, double y, const char *t) { text(cr, t, 16, y + 13, 11.5, 400, F_SANS, 0x808086, 1, 0, 0); }

/* ------------------------------------------------------------------ Page FitGirl Repacks */
/* ---- FitGirl : dossier de destination (par défaut ~/Games/FitGirl/<jeu>, modifiable) */
static char fg_base_dir[512];

static void on_fg_folder(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    gchar *path = g_file_get_path(f); g_object_unref(f);
    if (path) { g_strlcpy(fg_base_dir, path, sizeof fg_base_dir); g_free(path); }
    if (area) gtk_widget_queue_draw(area);
}

static void pick_fg_folder(void) {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Dossier des téléchargements FitGirl");
    gchar *cur = g_build_filename(g_get_home_dir(), "Games", NULL); GFile *cf = g_file_new_for_path(fg_base_dir[0] ? fg_base_dir : cur);
    gtk_file_dialog_set_initial_folder(d, cf); g_object_unref(cf); g_free(cur);
    gtk_file_dialog_select_folder(d, GTK_WINDOW(win), NULL, on_fg_folder, NULL);
    g_object_unref(d);
}

/* dossier du jeu : nom court (avant « – » ou « + »), sans caractères interdits */
static void fg_game_dir(const char *title, char *out, size_t n) {
    if (!fg_base_dir[0]) g_snprintf(fg_base_dir, sizeof fg_base_dir, "%s/Games/FitGirl", g_get_home_dir());
    gchar *t = g_strdup(*title ? title : "Jeu");
    for (const char *cut[] = {" – ", " - ", " + ", " (", NULL}, **c = cut; *c; c++) { char *p = strstr(t, *c); if (p && p != t) *p = 0; }
    GString *g = g_string_new(NULL);                   /* « God of War: Ragnarök » → « God of War - Ragnarök » */
    for (const char *p = t; *p; p++) {
        if (*p == ':') g_string_append(g, " -");
        else if (strchr("/\\*?\"<>|", *p)) g_string_append_c(g, ' ');
        else if (!(*p == ' ' && g->len && g->str[g->len - 1] == ' ')) g_string_append_c(g, *p);
    }
    g_free(t); t = g_string_free(g, FALSE);
    g_strstrip(t);
    g_snprintf(out, n, "%s/%s", fg_base_dir, *t ? t : "Jeu");
    g_free(t);
}

/* chemin affiché : ~ pour le dossier personnel */
static void fg_short_path(const char *p, char *out, size_t n) {
    const char *home = g_get_home_dir(); size_t hl = strlen(home);
    if (!strncmp(p, home, hl) && (p[hl] == '/' || !p[hl])) g_snprintf(out, n, "~%s", p + hl); else g_strlcpy(out, p, n);
}

static void fmt_size(char *b, size_t n, double v) {
    if (v >= 1073741824.0) g_snprintf(b, n, "%.1f Go", v / 1073741824.0);
    else g_snprintf(b, n, "%.0f Mo", v / 1048576.0);
}

static void fmt_eta(char *b, size_t n, double s) {
    if (s < 60) g_snprintf(b, n, "%.0f s", s);
    else if (s < 3600) g_snprintf(b, n, "%.0f min", s / 60);
    else g_snprintf(b, n, "%d h %02d", (int)(s / 3600), (int)fmod(s / 60, 60));
}

/* bouton plein rouge : action principale */
static gboolean primary_btn(cairo_t *cr, double x, double y, double w, double h, const char *t) {
    gboolean hov = hit(x, y, w, h);
    cairo_rectangle(cr, x, y, w, h); rgba(cr, hov ? 0xff3a55 : C_RED, 1); cairo_fill(cr);
    text(cr, t, x + w / 2, y + h / 2, 13, 700, F_SANS, 0xffffff, 1, 1, 0);
    return clicked(x, y, w, h);
}

/* barre de progression fine avec fond */
static void fg_bar(cairo_t *cr, double x, double y, double w, double h, double frac) {
    cairo_rectangle(cr, x, y, w, h); rgba(cr, 0x1e1e22, 1); cairo_fill(cr);
    cairo_rectangle(cr, x, y, w * CLAMP(frac, 0, 1), h); rgba(cr, C_RED, 1); cairo_fill(cr);
}

/* ---- FitGirl : extraction puis installation dans Wine (Jeux Windows) */
static char fg_setup_exe[512];            /* installateur lancé (repéré par son chemin : les indices peuvent bouger) */
static gboolean fg_auto_install;          /* « Extraire et installer » : lancer l'installateur dès la fin de l'extraction */
static gint64 fg_del_confirm;             /* double clic de confirmation pour supprimer les archives */
static gboolean fg_installed;             /* jeu installé et ajouté : les archives peuvent être supprimées */

static int wg_find(const char *exe) {
    int n; WinGame *w = wg_list(&n);
    for (int i = 0; i < n; i++) if (!strcmp(w[i].exe, exe)) return i;
    return -1;
}

/* nom court du jeu : avant « – », « + » ou « ( » */
static void fg_short_title(const char *title, char *out, size_t n) {
    g_strlcpy(out, *title ? title : "Jeu", n);
    for (const char *cut[] = {" – ", " - ", " + ", " (", NULL}, **c = cut; *c; c++) { char *p = strstr(out, *c); if (p && p != out) *p = 0; }
    g_strstrip(out);
}

static gboolean fg_launch_setup(const char *setup, const char *title) {
    WgTools t = wg_tools();
    if (!t.umu || !t.proton) { show_toast("Installe d'abord umu-launcher et Proton-GE (page Jeux Windows)"); return FALSE; }
    int i = wg_add(setup);
    if (i < 0) { show_toast("Liste des jeux Windows pleine"); return FALSE; }
    int n; WinGame *w = wg_list(&n);
    char st[64]; fg_short_title(title, st, sizeof st);
    g_snprintf(w[i].name, sizeof w[i].name, "%.48s (installation)", st);
    /* préfixe Wine propre au jeu (sinon nommé d'après le dossier « extracted », commun à tous les repacks) */
    char slug[96]; model_slug(st, slug, sizeof slug);
    gchar *pd = g_path_get_dirname(w[i].prefix);
    g_snprintf(w[i].prefix, sizeof w[i].prefix, "%s/fitgirl-%s", pd, *slug ? slug : "jeu");
    g_free(pd);
    wg_save(); wg_launch(i);
    g_strlcpy(fg_setup_exe, setup, sizeof fg_setup_exe);
    show_toast("Installateur FitGirl lancé : suis ses étapes (dossier proposé : C:\\Games)");
    return TRUE;
}

/* exécutable du jeu installé, choisi dans le préfixe Wine de l'installateur (même préfixe, même disque C:) */
static void on_fg_game_exe(GObject *src, GAsyncResult *res, gpointer d) {
    gchar *title = d;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    gchar *exe = f ? g_file_get_path(f) : NULL;
    if (f) g_object_unref(f);
    int si = fg_setup_exe[0] ? wg_find(fg_setup_exe) : -1;
    if (exe && si >= 0) {
        int n; WinGame *w = wg_list(&n);
        char prefix[512]; g_strlcpy(prefix, w[si].prefix, sizeof prefix);
        int j = wg_add(exe);
        if (j >= 0) {
            w = wg_list(&n);
            g_strlcpy(w[j].prefix, prefix, sizeof w[j].prefix);
            char st[64]; fg_short_title(title, st, sizeof st); g_strlcpy(w[j].name, st, sizeof w[j].name);
            wg_save();
            wg_remove(wg_find(fg_setup_exe), FALSE);        /* l'entrée de l'installateur ne sert plus (préfixe conservé) */
            fg_setup_exe[0] = 0; fg_installed = TRUE;
            show_toast("Jeu ajouté : lance-le depuis « Jeux Windows »");
            page = 12;
        } else show_toast("Liste des jeux Windows pleine");
    }
    g_free(exe); g_free(title);
    if (area) gtk_widget_queue_draw(area);
}

static void pick_fg_game_exe(const char *title) {
    int si = wg_find(fg_setup_exe); if (si < 0) return;
    int n; WinGame *w = wg_list(&n);
    gchar *games = g_build_filename(w[si].prefix, "drive_c", "Games", NULL), *drive = g_build_filename(w[si].prefix, "drive_c", NULL);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Exécutable du jeu installé (dans C:\\Games)");
    GFile *init = g_file_new_for_path(g_file_test(games, G_FILE_TEST_IS_DIR) ? games : drive);
    gtk_file_dialog_set_initial_folder(dlg, init); g_object_unref(init);
    GtkFileFilter *ff = gtk_file_filter_new(); gtk_file_filter_set_name(ff, "Exécutables Windows");
    gtk_file_filter_add_pattern(ff, "*.exe"); gtk_file_filter_add_pattern(ff, "*.EXE");
    GListStore *fl = g_list_store_new(GTK_TYPE_FILE_FILTER); g_list_store_append(fl, ff);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(fl)); gtk_file_dialog_set_default_filter(dlg, ff);
    gtk_file_dialog_open(dlg, GTK_WINDOW(win), NULL, on_fg_game_exe, g_strdup(title));
    g_object_unref(ff); g_object_unref(fl); g_object_unref(dlg); g_free(games); g_free(drive);
}

/* suppression du dossier de téléchargement, seulement s'il est bien sous le dossier FitGirl choisi */
static gboolean fg_delete_download(const char *dest) {
    if (!fg_base_dir[0] || !g_str_has_prefix(dest, fg_base_dir) || strlen(dest) <= strlen(fg_base_dir) + 1 || dest[strlen(fg_base_dir)] != '/' || strstr(dest, "/..")) return FALSE;
    const char *a[] = {"rm", "-rf", "--", dest, NULL}; gint st = 1;
    return g_spawn_sync(NULL, (char **)a, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &st, NULL) && st == 0;
}

static void page_fitgirl(cairo_t *cr) {
    ptitle(cr, "FitGirl Repacks");
    text(cr, "Recherche et téléchargement de repacks FitGirl : plusieurs connexions par fichier, reprise automatique après coupure.", X0, 82, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
    const char *q = fx_fg_query();
    if (*q && outline_btn(cr, XR - 310, 38, 170, 26, "Nouvelle recherche…", 12)) { ask_text(6, 0, "Rechercher un repack FitGirl", q); return; }

    static gboolean paused;                     /* « Pause » : le helper est arrêté, l'état des segments est conservé */
    double y = 104;
    FgDlStatus st = fx_fg_dl_status();
    const char *page_url = fx_fg_page_url();
    gboolean busy_res = fx_fg_resolve_busy(), resolved = fx_fg_resolved();
    gboolean show_card = *page_url || st.active || st.completed || st.error;

    /* ---- carte du jeu sélectionné / téléchargement ---- */
    if (show_card) {
        const char *gtitle = st.active || st.completed ? st.game : fx_fg_game_title();
        const char *rerr = fx_fg_resolve_error();
        int nfiles = fx_fg_file_count(), nopt = fx_fg_optional_count();
        char dest[512]; fg_game_dir(gtitle, dest, sizeof dest);
        double ch = st.active ? 176 : 144;
        panel(cr, X0, y, CW, ch, st.active);
        double px = X0 + 22, pw = CW - 44, bx = XR - 22;

        section(cr, st.active ? "Téléchargement en cours" : st.completed ? "Téléchargement terminé" : "Jeu sélectionné", px, y + 24);
        text_fit(cr, *gtitle ? gtitle : (busy_res ? "Recherche des liens…" : "—"), px, y + 54, 16, 700, F_SANS, C_TEXT, pw - 230, 0);

        if (st.active) {
            char l1[256], a[32], b2[32], c[32], l2[256];
            if (st.resolving) g_snprintf(l1, sizeof l1, "Partie %d / %d — obtention du lien…", st.file_current + 1, st.file_count);
            else g_snprintf(l1, sizeof l1, "Partie %d / %d — %s", st.file_current + 1, st.file_count, st.file_name);
            text_fit(cr, l1, px, y + 84, 12.5, 400, F_SANS, C_LABEL, pw - 140, 0);
            if (st.file_size > 0) { fmt_size(a, sizeof a, st.file_bytes); fmt_size(b2, sizeof b2, st.file_size); g_snprintf(c, sizeof c, "%s / %s", a, b2); text(cr, c, bx, y + 84, 12.5, 600, F_SANS, C_TEXT, 1, 2, 0); }
            fg_bar(cr, px, y + 96, pw, 4, st.pct / 100.0);

            double gfrac = st.game_total > 0 ? (double)st.game_done / st.game_total : 0;
            char rate[32]; fmt_rate(rate, sizeof rate, st.speed_bps);
            if (st.game_total > 0) {
                fmt_size(a, sizeof a, st.game_done); fmt_size(b2, sizeof b2, st.game_total);
                g_snprintf(l2, sizeof l2, "Total : %s / ~%s (%.0f %%)  ·  %s  ·  %d connexion%s", a, b2, gfrac * 100, rate, st.conns, st.conns > 1 ? "s" : "");
                if (st.speed_bps > 1024) { fmt_eta(c, sizeof c, (st.game_total - st.game_done) / st.speed_bps); g_strlcat(l2, "  ·  reste ~", sizeof l2); g_strlcat(l2, c, sizeof l2); }
            } else g_snprintf(l2, sizeof l2, "%s  ·  %s", st.msg, rate);
            text_fit(cr, l2, px, y + 124, 12.5, 600, F_SANS, C_TEXT, pw, 0);
            fg_bar(cr, px, y + 136, pw, 6, gfrac);
            if (outline_btn(cr, bx - 110, y + 12, 110, 28, "Pause", 12)) { fx_fg_cancel(); paused = TRUE; }
            { char sp[520]; fg_short_path(st.dest[0] ? st.dest : dest, sp, sizeof sp); text_fit(cr, sp, px, y + 160, 11.5, 400, F_SANS, C_MUTE, pw, 0); }
            schedule_redraw(500);
        } else if (st.completed) {
            FgExStatus ex = fx_fg_ex_status();
            int si = fg_setup_exe[0] ? wg_find(fg_setup_exe) : -1;
            int nwg; WinGame *wgl = wg_list(&nwg);
            char sp[520], dm[600]; fg_short_path(st.dest, sp, sizeof sp);
            if (ex.done && !ex.error && fg_auto_install) {          /* enchaînement automatique après l'extraction */
                fg_auto_install = FALSE;
                fg_launch_setup(ex.setup, st.game);
                si = wg_find(fg_setup_exe);
            }
            if (ex.active) {
                g_snprintf(dm, sizeof dm, "Extraction des archives… %d %%", ex.pct);
                text(cr, dm, px, y + 84, 12.5, 600, F_SANS, C_TEXT, 1, 0, 0);
                fg_bar(cr, px, y + 98, pw - 230, 6, ex.pct / 100.0);
                text(cr, "Les archives sont décompressées dans le dossier « extracted » du jeu.", px, y + 122, 12, 400, F_SANS, C_MUTE, 1, 0, 0);
                schedule_redraw(500);
            } else if (ex.error) {
                text_fit(cr, ex.msg, px, y + 84, 12.5, 600, F_SANS, C_RED, pw - 230, 0);
                if (primary_btn(cr, bx - 190, y + 40, 190, 34, "Réessayer l'extraction")) { fg_auto_install = TRUE; fx_fg_extract(st.dest); }
                if (outline_btn(cr, bx - 190, y + 86, 190, 28, "Ouvrir le dossier", 12)) open_path(st.dest);
            } else if (ex.done && si >= 0 && wgl[si].running) {
                text(cr, "Installation en cours dans Wine : suis les étapes de l'installateur FitGirl.", px, y + 84, 12.5, 600, F_SANS, C_TEXT, 1, 0, 0);
                text(cr, "Garde le dossier proposé (C:\\Games). L'installation peut prendre longtemps.", px, y + 106, 12, 400, F_SANS, C_MUTE, 1, 0, 0);
                schedule_redraw(1000);
            } else if (ex.done && si >= 0) {
                text(cr, "✓  Installateur terminé. Choisis l'exécutable du jeu (dans C:\\Games) pour l'ajouter à Jeux Windows.", px, y + 84, 12.5, 600, F_SANS, C_OK, 1, 0, 0);
                if (primary_btn(cr, bx - 190, y + 40, 190, 34, "Ajouter le jeu installé")) pick_fg_game_exe(st.game);
                if (outline_btn(cr, bx - 190, y + 86, 190, 28, "Relancer l'installateur", 12)) { wg_launch(si); }
            } else if (ex.done && fg_installed) {
                text(cr, "✓  Jeu installé et ajouté à « Jeux Windows ».", px, y + 84, 12.5, 600, F_SANS, C_OK, 1, 0, 0);
                if (primary_btn(cr, bx - 190, y + 40, 190, 34, "Ouvrir Jeux Windows")) page = 12;
            } else if (ex.done) {
                text(cr, "✓  Archives extraites.", px, y + 84, 12.5, 600, F_SANS, C_OK, 1, 0, 0);
                if (primary_btn(cr, bx - 190, y + 40, 190, 34, "Lancer l'installateur")) fg_launch_setup(ex.setup, st.game);
            } else {
                g_snprintf(dm, sizeof dm, "✓  Toutes les parties sont téléchargées dans %s", sp);
                text_fit(cr, dm, px, y + 84, 12.5, 600, F_SANS, C_OK, pw - 230, 0);
                if (fx_fg_extract_tool()) {
                    text(cr, "Extraction des archives puis installation du jeu dans Wine (Proton-GE).", px, y + 106, 12, 400, F_SANS, C_MUTE, 1, 0, 0);
                    if (primary_btn(cr, bx - 190, y + 40, 190, 34, "Extraire et installer")) { fg_auto_install = TRUE; fx_fg_extract(st.dest); }
                } else {
                    text(cr, "7-Zip est nécessaire pour extraire les archives RAR de FitGirl.", px, y + 106, 12, 400, F_SANS, C_LABEL, 1, 0, 0);
                    if (primary_btn(cr, bx - 190, y + 40, 190, 34, pkg_busy ? "Installation…" : "Installer 7-Zip") && !pkg_busy) install_pkg_async("7zip", "7-Zip");
                }
                if (outline_btn(cr, bx - 190, y + 86, 190, 28, "Ouvrir le dossier", 12)) open_path(st.dest);
            }
            /* libérer l'espace : archives et fichiers extraits ne servent plus une fois le jeu installé */
            if (fg_installed && ex.done) {
                gboolean armed = g_get_monotonic_time() < fg_del_confirm;
                if (outline_btn(cr, px, y + 112, 300, 24, armed ? "Confirmer : supprimer le dossier téléchargé" : "Libérer l'espace (supprimer les archives)", 11.5)) {
                    if (!armed) { fg_del_confirm = g_get_monotonic_time() + 4 * G_USEC_PER_SEC; schedule_redraw(4100); }
                    else { fg_del_confirm = 0; show_toast(fg_delete_download(st.dest) ? "Archives supprimées" : "Suppression refusée (dossier inattendu)"); }
                }
            }
        } else if (busy_res) {
            int dots = (int)(g_get_monotonic_time() / 400000) % 4;
            char m[80]; g_snprintf(m, sizeof m, "Recherche des liens FuckingFast%.*s", dots, "...");
            text(cr, m, px, y + 84, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
            schedule_redraw(400);
        } else if (resolved && *rerr) {
            text_fit(cr, rerr, px, y + 84, 12.5, 600, F_SANS, C_RED, pw - 140, 0);
            text(cr, "Choisis un autre résultat, ou réessaie dans un instant.", px, y + 106, 12, 400, F_SANS, C_MUTE, 1, 0, 0);
            if (outline_btn(cr, bx - 120, y + 40, 120, 30, "Réessayer", 12)) { gchar *u = g_strdup(page_url); fx_fg_resolve(u); g_free(u); }
        } else if (resolved) {
            char info[256];
            if (nopt > 0) g_snprintf(info, sizeof info, "%d partie%s  ·  %d fichier%s optionnel%s ignoré%s (voix, bonus)", nfiles, nfiles > 1 ? "s" : "", nopt, nopt > 1 ? "s" : "", nopt > 1 ? "s" : "", nopt > 1 ? "s" : "");
            else g_snprintf(info, sizeof info, "%d partie%s", nfiles, nfiles > 1 ? "s" : "");
            text(cr, info, px, y + 84, 12.5, 400, F_SANS, C_LABEL, 1, 0, 0);
            char sp[520], dl[600]; fg_short_path(dest, sp, sizeof sp); g_snprintf(dl, sizeof dl, "Dossier : %s", sp);
            text_fit(cr, dl, px, y + 106, 12, 400, F_SANS, C_MUTE, pw - 230, 0);
            if (st.error) text_fit(cr, st.msg, px, y + 126, 12, 600, F_SANS, C_RED, pw - 230, 0);
            else if (paused) text(cr, "En pause : les parties et segments déjà reçus sont conservés.", px, y + 126, 12, 400, F_SANS, C_LABEL, 1, 0, 0);
            if (outline_btn(cr, bx - 190, y + 86, 190, 28, "Changer de dossier…", 12)) pick_fg_folder();
            const char *lbl = st.error || paused ? "Reprendre" : "Télécharger";
            if (primary_btn(cr, bx - 190, y + 40, 190, 34, lbl)) {
                paused = FALSE;
                g_mkdir_with_parents(dest, 0755);
                fx_fg_download(dest);
                show_toast("Téléchargement lancé");
            }
        }
        y += ch + 24;
    }

    /* ---- résultats de recherche ---- */
    FgSearchHit hits[MAXFG_RESULTS];
    int nhits = fx_fg_results(hits, MAXFG_RESULTS);
    gboolean searching = fx_fg_search_busy();
    char qbuf[200];
    g_snprintf(qbuf, sizeof qbuf, *q ? "Résultats pour « %s »" : "Rechercher un repack", q);
    section(cr, qbuf, X0, y + 8);
    y += 28;
    if (!*q) {
        panel(cr, X0, y, CW, 84, FALSE);
        text(cr, "Lance une recherche pour trouver un jeu (ex. « elden ring »).", X0 + 22, y + 30, 13, 400, F_SANS, C_LABEL, 1, 0, 0);
        if (primary_btn(cr, X0 + 22, y + 44, 190, 30, "Rechercher un jeu…")) { ask_text(6, 0, "Rechercher un repack FitGirl", q); return; }
        y += 84;
    } else if (searching) {
        int dots = (int)(g_get_monotonic_time() / 400000) % 4;
        char m[80]; g_snprintf(m, sizeof m, "Recherche en cours%.*s", dots, "...");
        text(cr, m, X0, y + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
        schedule_redraw(400);
        y += 40;
    } else if (nhits == 0) {
        const char *se = fx_fg_search_error();
        if (*se) {
            text_fit(cr, se, X0, y + 14, 12.5, 600, F_SANS, C_RED, CW - 140, 0);
            if (outline_btn(cr, XR - 120, y, 120, 28, "Réessayer", 12)) { gchar *qq = g_strdup(q); fx_fg_search(qq); g_free(qq); }
        } else text(cr, "Aucun résultat. Essaie un autre terme (titre en anglais).", X0, y + 14, 12.5, 400, F_SANS, C_MUTE, 1, 0, 0);
        y += 40;
    } else {
        double gap = 12, cardh = 96;
        int cols = grid_cols(CW, 280, gap);
        double cw = (CW - (cols - 1) * gap) / cols;
        gboolean locked = st.active;            /* pas de changement de jeu pendant un téléchargement */
        for (int i = 0; i < nhits; i++) {
            double x = X0 + (i % cols) * (cw + gap), ry = y + (i / cols) * (cardh + gap);
            gboolean sel = !strcmp(hits[i].page_url, page_url);
            panel(cr, x, ry, cw, cardh, sel);
            if (sel) { cairo_rectangle(cr, x, ry, 3, cardh); rgba(cr, C_RED, 1); cairo_fill(cr); }
            text_fit(cr, hits[i].title, x + 16, ry + 26, 13.5, 600, F_SANS, C_TEXT, cw - 32, 0);
            if (sel) text(cr, busy_res ? "Recherche des liens…" : "Sélectionné", x + 16, ry + 66, 12, 600, F_SANS, C_RED, 1, 0, 0);
            else if (!locked && outline_btn(cr, x + 16, ry + 52, cw - 32, 28, "Sélectionner", 12)) {
                paused = FALSE;
                fg_installed = FALSE; fg_auto_install = FALSE; fg_del_confirm = 0;
                fx_fg_resolve(hits[i].page_url);
                if (area) gtk_widget_queue_draw(area);
            } else if (locked) {                   /* bouton grisé : un seul téléchargement à la fois */
                cairo_rectangle(cr, x + 16.5, ry + 52.5, cw - 33, 27); rgba(cr, C_LINE2, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr);
                text(cr, "Sélectionner", x + cw / 2, ry + 66, 12, 600, F_SANS, C_MUTE, 1, 1, 0);
            }
        }
        y += ((nhits + cols - 1) / cols) * (cardh + gap);
    }

    page_h = y + 24;
}

static void chrome(cairo_t *cr) {
    /* barre latérale */
    cairo_rectangle(cr, 0, VY0, SBW, LHv); rgba(cr, 0x0a0a0c, 0.6); cairo_fill(cr);
    nav_item(cr, 46, 0, "Accueil", 0);

    nav_item(cr, 80, 6, "Appareils", 8);
    nav_section(cr, 122, "Contrôle");
    nav_item(cr, 148, 1, "Performances", 1);
    nav_item(cr, 182, 2, "Écran & GPU", 2);
    nav_item(cr, 216, 3, "Audio", 3);
    nav_section(cr, 258, "Playground");
    nav_item(cr, 284, 9, "Éclairage", 11);
    nav_item(cr, 318, 10, "GameVisual", 15);
    nav_section(cr, 360, "Assistant");
    nav_item(cr, 386, 7, "Scénarios", 14);
    nav_item(cr, 420, 8, "Jeux", 13);
    nav_item(cr, 454, 11, "Macros", 16);
    nav_item(cr, 488, 12, "Jeux Windows", 2);
    nav_item(cr, 522, 13, "FitGirl Repacks", 8);
    nav_section(cr, 564, "Surveillance");
    nav_item(cr, 590, 4, "Système", 4);

    nav_item(cr, VY0 + LHv - 52, 5, "Réglages", 5);

    /* raccourcis en haut à droite du contenu (Système, Audio, Réglages) */
    static const int SC[3] = {4, 3, 5};
    for (int i = 0; i < 3; i++) {
        double x = XR - 20 - (2 - i) * 46, y = 51;
        gboolean hov = hit(x - 18, y - 16, 36, 32), on = page == SC[i];
        icon(cr, SC[i], x, y, on ? C_RED : (hov ? 0xffffff : 0xe2e2e6), 0.95);
        if (clicked(x - 18, y - 16, 36, 32)) page = SC[i];
    }
}

static void background(cairo_t *cr) {
    rgba(cr, C_BG, 1); cairo_paint(cr);
    cairo_pattern_t *p = cairo_pattern_create_radial(LWv - 154, VY0 - 72, 0, LWv - 154, VY0 - 72, 620);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 0, 0.2, 0.22); cairo_pattern_add_color_stop_rgba(p, 1, 1, 0, 0.2, 0);
    cairo_set_source(cr, p); cairo_paint(cr); cairo_pattern_destroy(p);
    p = cairo_pattern_create_radial(0, VY0 + LHv + 70, 0, 0, VY0 + LHv + 70, 480);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 0, 0.2, 0.08); cairo_pattern_add_color_stop_rgba(p, 1, 1, 0, 0.2, 0);
    cairo_set_source(cr, p); cairo_paint(cr); cairo_pattern_destroy(p);
    /* fines diagonales de la première version */
    rgba(cr, 0xffffff, 0.014); cairo_set_line_width(cr, 2);
    for (double x = -LHv; x < LWv; x += 14) { cairo_move_to(cr, x, VY0 + LHv); cairo_line_to(cr, x + LHv * 0.47, VY0); }
    cairo_stroke(cr);
}

static void bg_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
    (void)a; (void)w; (void)h; (void)d;
    cairo_scale(cr, kk, kk); cairo_translate(cr, 0, -VY0);
    background(cr);
}

static void draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
    (void)a; (void)d;
    gint64 t_draw0 = g_get_monotonic_time();
    hw_snapshot(&hw); apply_opts(&hw);
    /* la scène remplit toute la fenêtre : taille de conception minimale selon la page, le reste de la place est absorbé par la mise en page */
    double mw = 1100, mh = 660;
    home_compact = FALSE;
    if (page == 0) { if (w >= 1.5 * h) { mw = 1280; mh = 700; } else { mw = 1000; mh = 1180; home_compact = TRUE; } }
    double k = MIN(w / mw, h / mh);
    kk = k; LWv = w / k; LHv = h / k;
    /* le fond est un widget séparé (bg_area) ; on ne le redessine que si la taille ou la disposition changent */
    static int bw_, bh_; static double bk_, blw_, blh_;
    if (bw_ != w || bh_ != h || bk_ != k || blw_ != LWv || blh_ != LHv) {
        bw_ = w; bh_ = h; bk_ = k; blw_ = LWv; blh_ = LHv;
        if (bg_area) gtk_widget_queue_draw(bg_area);
    }
    cairo_scale(cr, k, k); cairo_translate(cr, 0, -VY0);
    static int last_page = -1;
    if (last_page != page) { scroll_y = 0; last_page = page; }
    page_h = 0;
    double my_saved = my;
    cairo_save(cr); cairo_translate(cr, 0, -scroll_y); if (my >= 0) my += scroll_y;
    live_keys.show = live_meters.show = FALSE;
    if (hw.uptime <= 0) utext(cr, "Chargement…", LWv / 2, VY0 + LHv / 2, 20, 600, F_SANS, C_MUTE, 1, 4);
    else switch (page) { case 0: page_home(cr); break; case 1: page_perf(cr); break; case 2: page_display(cr); break;
                    case 3: page_audio(cr); break; case 4: page_system(cr); break; case 5: page_about(cr); break;
                    case 6: page_devices(cr); break; case 7: page_scenarios(cr); break; case 9: page_rgb(cr); break; case 10: page_gv(cr); break; case 11: page_macros(cr); break; case 12: page_windows(cr); break; case 13: page_fitgirl(cr); break; default: page_games(cr); }
    cairo_restore(cr);
    my = my_saved;
    chrome(cr);
    scroll_max = MAX(0, page_h - (VY0 + LHv) + 24);
    if (scroll_y > scroll_max) scroll_y = scroll_max;
    if (toast_until > g_get_monotonic_time()) {
        double tw = text(cr, toast_msg, 0, 0, 14, 400, F_SANS, 0, -1, 0, 0) + 36;
        tw = MIN(tw, 560);
        cairo_rectangle(cr, XR + 10 - tw, VY0 + LHv - 56, tw, 40); rgba(cr, 0x1a0a0e, 1); cairo_fill(cr);
        cairo_rectangle(cr, XR + 10 - tw, VY0 + LHv - 56, 3, 40); rgba(cr, C_RED, 1); cairo_fill(cr);
        text(cr, toast_msg, XR + 10 - tw + 18, VY0 + LHv - 36, 14, 400, F_SANS, C_TEXT, 1, 0, 0);
        schedule_redraw(500);
    }
    mclick = FALSE;
    live_place(&live_keys); live_place(&live_meters);
    if (g_getenv("COREBOARD_DEBUG")) {
        static gint64 win_start; static int frames; static gint64 busy;
        gint64 now = g_get_monotonic_time(); busy += now - t_draw0; frames++;
        if (now - win_start > G_USEC_PER_SEC) { g_printerr("%d images/s, %.1f ms par image, zone %dx%d\n", frames, busy / 1000.0 / frames, w, h); frames = 0; busy = 0; win_start = now; }
    }
}

/* ------------------------------------------------------------------ import d'une photo (--set-image) */
static gboolean is_bg(const guchar *p, int ch) { return p[0] >= 232 && p[1] >= 232 && p[2] >= 232 && (ch < 4 || p[3] > 0); }

/* Charge une image (fichier ou URL directe), supprime le fond blanc par remplissage depuis les bords,
   recadre et l'enregistre dans ~/.local/share/coreboard/devices/<modèle>.png */
static int import_image(const char *src) {
    gchar *tmp = NULL; const char *path = src;
    if (g_str_has_prefix(src, "http://") || g_str_has_prefix(src, "https://")) {
        int fd = g_file_open_tmp("coreboard-XXXXXX", &tmp, NULL); if (fd >= 0) close(fd);
        const char *argv[] = {"curl", "-fsSL", "-A", "Mozilla/5.0", "-o", tmp, src, NULL};
        gint st = 1; if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &st, NULL) || st) {
            g_printerr("Téléchargement impossible : %s\n", src); g_free(tmp); return 1; }
        path = tmp;
    }
    GError *err = NULL;
    GdkPixbuf *pb0 = gdk_pixbuf_new_from_file(path, &err);
    if (tmp) { g_unlink(tmp); g_free(tmp); }
    if (!pb0) { g_printerr("Image illisible : %s\n", err ? err->message : "?"); g_clear_error(&err); return 1; }
    GdkPixbuf *pb = gdk_pixbuf_add_alpha(pb0, FALSE, 0, 0, 0); g_object_unref(pb0);
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), rs = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    #define P(x, y) (px + (y) * rs + (x) * 4)
    /* déjà transparente sur les bords → on garde le fond existant */
    gboolean had_alpha = FALSE;
    for (int x = 0; x < w && !had_alpha; x++) had_alpha = P(x, 0)[3] < 250 || P(x, h - 1)[3] < 250;
    for (int y = 0; y < h && !had_alpha; y++) had_alpha = P(0, y)[3] < 250 || P(w - 1, y)[3] < 250;
    if (!had_alpha) {
        guint8 *bg = g_malloc0((gsize)w * h); int *stack = g_malloc((gsize)w * h * sizeof(int)); int sp = 0;
        #define PUSH(x, y) do { int i_ = (y) * w + (x); if (!bg[i_] && is_bg(P(x, y), 4)) { bg[i_] = 1; stack[sp++] = i_; } } while (0)
        for (int x = 0; x < w; x++) { PUSH(x, 0); PUSH(x, h - 1); }
        for (int y = 0; y < h; y++) { PUSH(0, y); PUSH(w - 1, y); }
        while (sp) {
            int i = stack[--sp], x = i % w, y = i / w;
            if (x > 0) PUSH(x - 1, y);
            if (x < w - 1) PUSH(x + 1, y);
            if (y > 0) PUSH(x, y - 1);
            if (y < h - 1) PUSH(x, y + 1);
        }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            guchar *p = P(x, y);
            if (bg[y * w + x]) { p[3] = 0; continue; }
            gboolean edge = (x > 0 && bg[y * w + x - 1]) || (x < w - 1 && bg[y * w + x + 1]) || (y > 0 && bg[(y - 1) * w + x]) || (y < h - 1 && bg[(y + 1) * w + x]);
            if (edge) { int mn = MIN(p[0], MIN(p[1], p[2])); if (mn > 190) p[3] = (guchar)CLAMP((255 - mn) * 255 / 65, 0, 255); }
        }
        g_free(bg); g_free(stack);
    }
    /* recadrage sur la zone visible */
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (P(x, y)[3] > 16) { x0 = MIN(x0, x); x1 = MAX(x1, x); y0 = MIN(y0, y); y1 = MAX(y1, y); }
    if (x1 < 0) { g_printerr("Image vide après suppression du fond.\n"); g_object_unref(pb); return 1; }
    x0 = MAX(x0 - 2, 0); y0 = MAX(y0 - 2, 0); x1 = MIN(x1 + 2, w - 1); y1 = MIN(y1 + 2, h - 1);
    GdkPixbuf *out = gdk_pixbuf_new_subpixbuf(pb, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    HwState id; memset(&id, 0, sizeof id); hw_identity(&id);
    char slug[96]; model_slug(id.model, slug, sizeof slug);
    gchar *dir = g_build_filename(g_get_user_data_dir(), "coreboard", "devices", NULL); g_mkdir_with_parents(dir, 0755);
    gchar *dest = g_strdup_printf("%s/%s.png", dir, slug);
    int rc = 0;
    if (!gdk_pixbuf_save(out, dest, "png", &err, NULL)) { g_printerr("Écriture impossible : %s\n", err->message); rc = 1; }
    else g_print("Photo enregistrée pour « %s » : %s\n", id.model, dest);
    g_free(dest); g_free(dir); g_object_unref(out); g_object_unref(pb);
    #undef P
    #undef PUSH
    return rc;
}

/* ------------------------------------------------------------------ événements */
static void to_stage(double x, double y, double *sx, double *sy) {
    int w = gtk_widget_get_width(area), h = gtk_widget_get_height(area);
    (void)w; (void)h;
    *sx = x / kk; *sy = y / kk + VY0;
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer d) { (void)c; (void)d; to_stage(x, y, &mx, &my); gtk_widget_queue_draw(area); }
static void on_leave(GtkEventControllerMotion *c, gpointer d) { (void)c; (void)d; mx = my = -1; gtk_widget_queue_draw(area); }

static void on_press(GtkGestureClick *g, int n, double x, double y, gpointer d) {
    (void)g; (void)n; (void)d;
    to_stage(x, y, &mx, &my);
    mdown = TRUE; mclick = TRUE;
    gtk_widget_queue_draw(area);
}
static void on_release(GtkGestureClick *g, int n, double x, double y, gpointer d) { (void)g; (void)n; (void)d; to_stage(x, y, &mx, &my); mdown = FALSE; gtk_widget_queue_draw(area); }

static void on_new_data(void) { if (area) gtk_widget_queue_draw(area); }
static gboolean tick(gpointer d) { (void)d; if (area) gtk_widget_queue_draw(area); return G_SOURCE_CONTINUE; }

/* ------------------------------------------------------------------ application */
static void setup_fonts(void) {
    FcConfig *cfg = FcConfigGetCurrent();
    FcConfigAppFontAddDir(cfg, (const FcChar8 *)DATADIR "/fonts");
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        gchar *dir = g_path_get_dirname(exe), *dev = g_build_filename(dir, "..", "data", "fonts", NULL);
        FcConfigAppFontAddDir(cfg, (const FcChar8 *)dev);
        g_free(dev); g_free(dir); g_free(exe);
    }
}

static gboolean reapply_gv(gpointer d) { (void)d; char e[64]; if (gv_state()->enabled) gv_apply(e, sizeof e); return G_SOURCE_REMOVE; }

/* recharge la configuration Hyprland (nouveaux raccourcis de macros) puis réapplique le filtre GameVisual, que le rechargement efface */
static void reload_hypr_then_reapply(void) {
    if (!gv_supported()) return;
    const char *argv[] = {"hyprctl", "reload", NULL};
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
    g_timeout_add(2500, reapply_gv, NULL);
}

static gboolean is_modifier_name(const char *n) {
    static const char *P[] = {"Shift_", "Control_", "Alt_", "Super_", "Meta_", "Hyper_", "Caps_Lock", "Num_Lock", "ISO_Level", "Scroll_Lock", "Mode_switch"};
    for (guint i = 0; i < G_N_ELEMENTS(P); i++) if (g_str_has_prefix(n, P[i])) return TRUE;
    return FALSE;
}

/* capture des touches de la fenêtre : enregistrement d'une macro ou assignation d'un raccourci */
static gboolean on_win_key(GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType st, gpointer d) {
    (void)c; (void)keycode; (void)d;
    const char *name = gdk_keyval_name(keyval);
    if (!name || is_modifier_name(name)) return FALSE;
    unsigned mods = ((st & GDK_SHIFT_MASK) ? 1 : 0) | ((st & GDK_CONTROL_MASK) ? 2 : 0) | ((st & GDK_ALT_MASK) ? 4 : 0) | ((st & GDK_SUPER_MASK) ? 8 : 0);
    if (mx_bind_idx >= 0) {
        int n; Macro *m = mx_list(&n);
        if (!strcmp(name, "Escape")) { mx_bind_idx = -1; }
        else if (!strcmp(name, "Delete") || !strcmp(name, "BackSpace")) { m[mx_bind_idx].bind[0] = 0; mx_save(); reload_hypr_then_reapply(); mx_bind_idx = -1; }
        else {
            gboolean special = !g_unichar_isprint(gdk_keyval_to_unicode(keyval)) || g_str_has_prefix(name, "F");
            if (!(mods & 14) && !special) { show_toast("Ajoute Ctrl, Alt ou Super à la touche"); return TRUE; }
            GString *b = g_string_new(NULL);
            if (mods & 8) g_string_append(b, "SUPER + ");
            if (mods & 2) g_string_append(b, "CTRL + ");
            if (mods & 4) g_string_append(b, "ALT + ");
            if (mods & 1) g_string_append(b, "SHIFT + ");
            gchar *k = strlen(name) == 1 ? g_ascii_strup(name, -1) : g_strdup(name);
            g_string_append(b, k); g_free(k);
            g_strlcpy(m[mx_bind_idx].bind, b->str, sizeof m[mx_bind_idx].bind); g_string_free(b, TRUE);
            mx_save(); reload_hypr_then_reapply(); mx_bind_idx = -1;
            show_toast("Raccourci enregistré");
        }
        if (area) gtk_widget_queue_draw(area);
        return TRUE;
    }
    if (mx_recording()) {
        gchar *nm = strlen(name) == 1 ? g_ascii_strdown(name, -1) : g_strdup(name);
        mx_rec_key((mods & 14) ? nm : name, mods, gdk_keyval_to_unicode(keyval));
        g_free(nm);
        if (area) gtk_widget_queue_draw(area);
        return TRUE;
    }
    return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer d) { (void)w; (void)d; win = NULL; area = NULL; bg_area = NULL; boot_area = NULL; live_keys.w = live_meters.w = NULL; }

/* le moteur (matériel, scénarios) vit tant que l'application vit, fenêtre ouverte ou non */
static gboolean backend_up, held, held_forced;
static GApplication *the_app;

static gboolean any_scenario_enabled(void) {
    int n; Scenario *sc = fx_scenarios(&n);
    for (int i = 0; i < n; i++) if (sc[i].enabled) return TRUE;
    return FALSE;
}

/* reste en arrière-plan (fenêtre fermée) tant qu'un profil de scénario est activé ou avec --background */
static void update_hold(void) {
    gboolean need = held_forced || any_scenario_enabled() || rgb_wants_background();
    if (need && !held) { g_application_hold(the_app); held = TRUE; }
    else if (!need && held) { g_application_release(the_app); held = FALSE; }
}

static gboolean music_timer(gpointer d) { (void)d; rgb_music_tick(); return G_SOURCE_CONTINUE; }

static gboolean fx_timer(gpointer d) {
    (void)d;
    fx_scenario_tick();
    if (area && (page == 7 || page == 8)) { fx_windows_refresh(); gtk_widget_queue_draw(area); }
    wg_refresh_status();
    if (area && page == 12) gtk_widget_queue_draw(area);
    update_hold();
    return G_SOURCE_CONTINUE;
}

static void ensure_backend(GApplication *app) {
    if (backend_up) return;
    backend_up = TRUE; the_app = app;
    hw_set_notify(on_new_data); hw_start(); hw_started = TRUE;
    fx_init(); fx_tool_install_set_done(on_tool_installed); fx_windows_refresh(); rgb_init(); gv_init(); mx_init(); wg_init();
    g_timeout_add(1000, tick, NULL);
    g_timeout_add(2000, fx_timer, NULL);
    g_timeout_add(45, music_timer, NULL);
    update_hold();
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer d) {
    (void)c; (void)dx; (void)d;
    scroll_y = CLAMP(scroll_y + dy * 50, 0, scroll_max);
    gtk_widget_queue_draw(area);
    return TRUE;
}

static void create_window(GApplication *app) {
    ensure_backend(app);
    if (g_getenv("COREBOARD_PAGE")) page = CLAMP(atoi(g_getenv("COREBOARD_PAGE")), 0, 12);
    win = gtk_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(win), "Coreboard");
    gtk_window_set_default_size(GTK_WINDOW(win), 1280, 700);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(win), "coreboard");
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window { background: #000; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    /* superposition : fond (statique) < interface (area) < éléments animés (clavier, jauges) */
    GtkWidget *overlay = gtk_overlay_new();
    bg_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bg_area), bg_draw, NULL, NULL);
    gtk_widget_set_hexpand(bg_area, TRUE); gtk_widget_set_vexpand(bg_area, TRUE); gtk_widget_set_can_target(bg_area, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), bg_area);
    area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, NULL, NULL);
    gtk_widget_set_hexpand(area, TRUE); gtk_widget_set_vexpand(area, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), area);
    Live *LV[2] = {&live_keys, &live_meters};
    for (int i = 0; i < 2; i++) {
        LV[i]->w = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(LV[i]->w), i ? live_draw_meters : live_draw_keys, NULL, NULL);
        gtk_widget_set_halign(LV[i]->w, GTK_ALIGN_START); gtk_widget_set_valign(LV[i]->w, GTK_ALIGN_START);
        gtk_widget_set_can_target(LV[i]->w, FALSE); gtk_widget_set_visible(LV[i]->w, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), LV[i]->w);
    }
    static guint live_timer;
    if (!live_timer) live_timer = g_timeout_add(50, live_tick, NULL);
    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(on_motion), NULL); g_signal_connect(mc, "leave", G_CALLBACK(on_leave), NULL);
    gtk_widget_add_controller(area, mc);
    GtkEventController *sc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), NULL);
    gtk_widget_add_controller(area, sc);
    GtkGesture *gc = gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), 1);
    g_signal_connect(gc, "pressed", G_CALLBACK(on_press), NULL); g_signal_connect(gc, "released", G_CALLBACK(on_release), NULL);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(gc));
    static gboolean boot_played;
    if (!boot_played) {
        boot_played = TRUE;
        boot_area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(boot_area), boot_draw, NULL, NULL);
        gtk_widget_set_hexpand(boot_area, TRUE); gtk_widget_set_vexpand(boot_area, TRUE); gtk_widget_set_can_target(boot_area, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), boot_area); /* ajouté en dernier : au-dessus de tout le reste */
        boot_t0 = g_get_monotonic_time();
        g_timeout_add(16, boot_tick, NULL);
        boot_play_sound();
    }
    gtk_window_set_child(GTK_WINDOW(win), overlay);
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_win_key), NULL);
    gtk_widget_add_controller(win, kc);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);
    gtk_window_present(GTK_WINDOW(win));
}

static int on_cmdline(GApplication *app, GApplicationCommandLine *cl, gpointer d) {
    (void)d;
    int argc; gchar **argv = g_application_command_line_get_arguments(cl, &argc);
    gboolean toggle_req = FALSE, bg = FALSE, quit = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--toggle")) toggle_req = TRUE;
        else if (!strcmp(argv[i], "--background")) bg = TRUE;
        else if (!strcmp(argv[i], "--quit")) quit = TRUE;
    }
    g_strfreev(argv);
    if (quit) { held_forced = FALSE; g_application_quit(app); return 0; }
    ensure_backend(app);
    if (bg) { held_forced = TRUE; update_hold(); return 0; }
    if (win) { if (toggle_req) gtk_window_close(GTK_WINDOW(win)); else gtk_window_present(GTK_WINDOW(win)); }
    else create_window(app);
    return 0;
}

static void on_shutdown(GApplication *app, gpointer d) { (void)app; (void)d; music_stop(); if (hw_started) hw_stop(); }

/* coreboard --rgb off | static RRGGBB | breathing RRGGBB | cycle | wave [gauche|droite] | luminosite N */
static int rgb_cli(int argc, char **argv) {
    rgb_init();
    if (rgb_status() == RGB_NONE) { g_printerr("Aucun clavier RGB compatible.\n"); return 1; }
    RgbState *st = rgb_state();
    if (argc < 1) { g_printerr("Usage : coreboard --rgb off|static RRGGBB|breathing RRGGBB|cycle|wave [gauche|droite]|luminosite N\n"); return 2; }
    const char *m = argv[0]; guint32 c = argc > 1 ? (guint32)strtoul(argv[1][0] == '#' ? argv[1] + 1 : argv[1], NULL, 16) : st->color[0];
    if (!strcmp(m, "off")) st->mode = RGBM_OFF;
    else if (!strcmp(m, "static")) { st->mode = RGBM_STATIC; st->sync = 1; for (int i = 0; i < 4; i++) st->color[i] = c; }
    else if (!strcmp(m, "breathing")) { st->mode = RGBM_BREATH; st->color[0] = c; }
    else if (!strcmp(m, "cycle")) st->mode = RGBM_CYCLE;
    else if (!strcmp(m, "wave")) { st->mode = RGBM_WAVE; if (argc > 1) st->dir_left = !strcmp(argv[1], "droite"); }
    else if (!strcmp(m, "luminosite") && argc > 1) st->brightness = CLAMP(atoi(argv[1]), 0, 100);
    else { g_printerr("Commande inconnue : %s\n", m); return 2; }
    char err[160];
    if (!rgb_commit(err, sizeof err)) { g_printerr("%s\n", err); return 1; }
    g_print("Clavier : %s\n", m);
    return 0;
}

/* coreboard --inspect JEU.exe : affiche ce que l'analyseur PE sait de l'exécutable */
static int inspect_cli(const char *path) {
    PeInfo p;
    if (!pe_analyze(path, &p)) { g_printerr("Ce n'est pas un exécutable Windows (PE) lisible : %s\n", path); return 1; }
    char sum[200], adv[240];
    pe_summary(&p, sum, sizeof sum); pe_advice(&p, adv, sizeof adv);
    g_print("Fichier         : %s\nRésumé          : %s\nArchitecture    : %s\n", path, sum, p.machine == 0xaa64 ? "ARM64" : p.machine == 0x8664 ? "x86_64" : p.machine == 0x14c ? "x86" : "autre");
    g_print("API graphique   : %s%s%s%s%s%s\n", p.dx12 ? "DirectX 12 " : "", p.dx11 ? "DirectX 11 " : "", p.dx10 ? "DirectX 10 " : "", p.dx9 ? "DirectX 9 " : "", p.vulkan ? "Vulkan " : "", p.opengl ? "OpenGL" : "");
    g_print("Moteur          : %s\nNVIDIA          : %s%s\nAnti-triche     : %s\n", p.engine[0] ? p.engine : "non identifié", p.nvapi ? "NVAPI " : "", p.dlss ? "DLSS" : "", p.ac_kernel[0] ? p.ac_kernel : p.ac_soft[0] ? p.ac_soft : "aucun détecté");
    g_print("Steam / EOS     : %s%s\nImports         :", p.steam_api ? "steam_api " : "", p.eos ? "EOS" : "");
    for (int i = 0; i < p.nimports; i++) g_print(" %s", p.imports[i]);
    g_print("\n");
    if (adv[0]) g_print("Attention       : %s\n", adv);
    return 0;
}

int main(int argc, char **argv) {
    compat_init();                                 /* ~/.local/bin dans le PATH : outils installés sans root */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--set-image")) {
            if (i + 1 >= argc) { g_printerr("Usage : coreboard --set-image FICHIER_OU_URL\n"); return 2; }
            return import_image(argv[i + 1]);
        }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--rgb")) return rgb_cli(argc - i - 1, argv + i + 1);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--macro")) {
            if (i + 1 >= argc) { g_printerr("Usage : coreboard --macro NOM\n"); return 2; }
            return mx_play_by_name(argv[i + 1]);
        }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--install-proton")) {
            wg_install_proton_async();
            for (;;) {
                g_usleep(5 * G_USEC_PER_SEC); WgDl d = wg_dl();
                if (d.state == 2 && d.total > 0) g_print("%s %.0f / %.0f Mo (%.0f %%)\n", d.msg, d.done / 1048576.0, d.total / 1048576.0, 100.0 * d.done / d.total);
                else g_print("%s\n", d.msg);
                if (d.state >= 4) return d.state == 4 ? 0 : 1;
            }
        }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--inspect")) {
            if (i + 1 >= argc) { g_printerr("Usage : coreboard --inspect JEU.exe\n"); return 2; }
            return inspect_cli(argv[i + 1]);
        }
    setup_fonts();
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_cmdline), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
