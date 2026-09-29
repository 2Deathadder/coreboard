/* Coreboard — GameVisual : filtres couleur plein écran (shader Hyprland) */
#ifndef GAMEVISUAL_H
#define GAMEVISUAL_H
#include <glib.h>

typedef struct {
    int enabled;
    int preset;                         /* indice dans gv_preset_name(), GV_MANUAL = réglages libres */
    double bright, contrast, sat, gamma;   /* 1.0 = neutre */
    int kelvin;                         /* 6500 = neutre */
    double vibrance, sharpen;           /* vibrance : -0.5..1 (0 = neutre) ; netteté : 0..1 */
} GvState;

#define GV_NPRESETS 7
#define GV_MANUAL GV_NPRESETS

void gv_init(void);                     /* charge gamevisual.json et applique */
GvState *gv_state(void);
const char *gv_preset_name(int i);
void gv_select_preset(int i);           /* charge les valeurs du préréglage */
gboolean gv_apply(char *err, size_t n); /* écrit le shader, l'applique et enregistre */
gboolean gv_supported(void);            /* Hyprland disponible */
#endif
