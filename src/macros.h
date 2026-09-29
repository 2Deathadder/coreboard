/* Coreboard — macros clavier : enregistrement (touches saisies dans la fenêtre), rejeu (wtype), raccourcis Hyprland */
#ifndef MACROS_H
#define MACROS_H
#include <glib.h>

#define MAXMACRO 24
#define MAXSTEP 160

typedef enum { MS_KEY, MS_TEXT, MS_DELAY, MS_CMD } MacroStepType;
typedef struct { int type; char value[160]; } MacroStep;    /* KEY : « ctrl+shift+Return » ; TEXT : texte ; DELAY : millisecondes ; CMD : commande shell */
typedef struct { char name[48], bind[64]; int repeat, nsteps, playing; MacroStep steps[MAXSTEP]; } Macro;

void mx_init(void);
Macro *mx_list(int *n);
int mx_add(const char *name);                       /* nouvelle macro vide ; renvoie son indice ou -1 */
void mx_remove(int i);
void mx_save(void);                                 /* enregistre macros.json et régénère les raccourcis Hyprland */
void mx_rename(int i, const char *name);

/* enregistrement : mods = bits 1 majuscule, 2 ctrl, 4 alt, 8 super */
void mx_rec_start(int i, gboolean append);
void mx_rec_stop(void);
gboolean mx_recording(void);
int mx_rec_index(void);
void mx_rec_key(const char *keyname, unsigned mods, gunichar ch);
void mx_step_remove(int i, int s);
void mx_step_add_delay(int i, int ms);

/* rejeu */
gboolean mx_wtype_available(void);
void mx_play_async(int i);                          /* thread ; ignoré si déjà en cours */
int mx_play_by_name(const char *name);              /* synchrone (ligne de commande) : 0 = ok */
const char *mx_step_label(const MacroStep *s, char *buf, size_t n);
#endif
