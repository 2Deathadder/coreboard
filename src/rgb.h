/* Coreboard — éclairage RGB du clavier (MSI MysticLight, 4 zones, HID feature reports) */
#ifndef RGB_H
#define RGB_H
#include <glib.h>

typedef enum { RGB_NONE, RGB_NOACCESS, RGB_OK } RgbStatus;
/* les cinq premiers modes sont ceux du clavier (valeurs matérielles) ; Personnalisé = un effet par zone ; Musique = piloté par l'audio */
typedef enum { RGBM_OFF, RGBM_STATIC, RGBM_BREATH, RGBM_CYCLE, RGBM_WAVE, RGBM_CUSTOM, RGBM_MUSIC, RGBM_COUNT } RgbMode;

typedef struct {
    int mode;                 /* RgbMode */
    int brightness;           /* 0..100 (appliquée en mettant les couleurs à l'échelle) */
    int speed;                /* 0..100 */
    int dir_left;             /* vague : 1 = de gauche à droite */
    int sync;                 /* 1 = même couleur sur les 4 zones */
    guint32 color[4];         /* 0xRRGGBB : WASD, lettres, navigation, pavé numérique */
    int zmode[4];             /* Personnalisé : effet de chaque zone (OFF..WAVE) */
    int zspeed[4];            /* Personnalisé : vitesse de chaque zone */
    int music_style;          /* 0 = couleur choisie, 1 = spectre arc-en-ciel, 2 = battements */
    int music_sens;           /* 0..100 */
    int music_smooth;         /* 0..100 : 0 = instantané, 100 = retombée douce */
    int music_glow;           /* 1 = lueur de fond quand il n'y a pas de son */
} RgbState;

void rgb_init(void);                     /* détecte le clavier, relit rgb.json et réapplique */
RgbStatus rgb_status(void);
const char *rgb_device_name(void);
RgbState *rgb_state(void);
gboolean rgb_commit(char *err, size_t n);
double rgb_cycle_seconds(int speed);      /* durée réelle d'un cycle matériel pour une vitesse 0..100 */
void rgb_music_tick(void);                /* à appeler ~20 fois/s : envoie une image de la synchronisation musicale */
gboolean rgb_wants_background(void);      /* TRUE si la synchronisation musicale doit continuer fenêtre fermée */
void rgb_music_colors(guint32 out[4]);    /* couleurs courantes de la synchronisation (aperçu, sans consommer les pics) */ /* applique l'état courant au clavier et l'enregistre */
#endif
