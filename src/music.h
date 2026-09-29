/* Coreboard — analyse audio en temps réel (sortie système) pour la synchronisation musicale du clavier */
#ifndef MUSIC_H
#define MUSIC_H
#include <glib.h>

gboolean music_start(char *err, size_t n);   /* lance la capture (parec sur le moniteur de la sortie par défaut) */
void music_stop(void);
gboolean music_running(void);
void music_set_params(int sens_pct, int smooth_pct);   /* sensibilité et lissage, 0..100 */
/* niveaux 0..1 (graves, bas-médiums, médiums, aigus). reset = TRUE : renvoie le pic depuis le dernier appel (aucun pic manqué
   entre deux images) ; FALSE : valeur instantanée, sans toucher aux pics (aperçu) */
void music_levels(double out[4], gboolean reset);
double music_beat_env(void);                 /* 1 au moment d'un battement, retombe en ~250 ms */
int music_beat_count(void);                  /* nombre de battements détectés depuis le démarrage */
#endif
