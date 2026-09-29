/* Coreboard — diagnostic et optimisation du système pour les jeux (ntsync, limites, GameMode, alimentation…) */
#ifndef TUNE_H
#define TUNE_H
#include <glib.h>

typedef struct { char label[48], value[96]; int status; } TuneItem;   /* status : 0 correct, 1 à corriger, 2 information */

int tune_check(TuneItem *out, int max, gboolean *fixable);        /* fixable : une correction système (pkexec) est disponible */
void tune_fix_async(void (*done)(gboolean ok, gpointer d), gpointer d);
gboolean tune_fixing(void);
#endif
