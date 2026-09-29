/* Coreboard — refroidissement adaptatif au lancement d'un jeu (MSI EC : cooler_boost / fan_mode via le module msi-ec) */
#ifndef FANCTL_H
#define FANCTL_H
#include <glib.h>

gboolean fan_supported(void);                 /* msi-ec chargé et accessible en écriture */
void fan_game_begin(void);                    /* démarre la régulation (fenêtre de lancement) */
void fan_game_end(void);                      /* rend la main au mode automatique */
int fan_level(void);                          /* -1 inactif, 0 auto, 1 avancé, 2 cooler boost */
#endif
