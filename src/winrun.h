/* Coreboard — adaptateur Windows : lance des .exe (jeux DirectX 11/12) via Proton-GE et umu-launcher */
#ifndef WINRUN_H
#define WINRUN_H
#include <glib.h>

#define MAXWG 32

typedef struct {
    char name[64], exe[512], args[192], prefix[512];
    int appid;                 /* identifiant Steam connu (corrections Proton), 0 = aucun */
    int hud, gamemode, dlss, rt, log;   /* MangoHud, GameMode, NVAPI/DLSS, ray tracing (DXR), journal Proton détaillé */
    GPid pid; int running; char status[120];
    /* amélioration : préréglage (0 personnalisé, 1 performance, 2 équilibré, 3 qualité, 4 économie) et réglages fins */
    int preset, ntsync, shader_async, lowlat, dlss_up, aniso, fsr, fsr_str, fps_cap;
    int sr_preset, fg, rr_latest, dlss_ind;   /* DLSS : modèle Super Resolution (0 jeu, 1 dernier, 2..5 = J K L M), Frame Generation, Ray Reconstruction, indicateur */
    char dlss_info[160];                      /* versions DLSS trouvées dans le dossier du jeu */
    int arch32, dx12;          /* issus de l'analyse PE */
    char info[200], warn[240]; int kernel_ac, analyzed;   /* résultat de l'analyse PE (voir pe.h) */
} WinGame;

typedef struct { gboolean umu, proton, gamemode, mangohud; } WgTools;

void wg_init(void);
WinGame *wg_list(int *n);
int wg_add(const char *exe);              /* nouvelle entrée pour un .exe ; renvoie l'indice ou -1 */
void wg_remove(int i, gboolean delete_prefix);
void wg_save(void);
void wg_apply_preset(int i, int preset);   /* règle les options d'amélioration selon le préréglage */
const char *wg_preset_name(int p);
void wg_launch(int i);
void wg_stop(int i);
void wg_refresh_status(void);             /* relit la dernière ligne du journal des jeux en cours */
WgTools wg_tools(void);
const char *wg_log_path(int i, char *buf, size_t n);
void wg_install_deps(void (*done)(gboolean ok, gpointer d), gpointer d);   /* pacman via pkexec, en tâche de fond */
gboolean wg_installing(void);

/* téléchargement reprenable de Proton-GE (connexions lentes ou instables) */
typedef struct { int state; gint64 done, total; char msg[160]; } WgDl;   /* state : 0 inactif, 1 recherche, 2 téléchargement, 3 extraction, 4 terminé, 5 échec */
WgDl wg_dl(void);
void wg_install_proton_async(void);
gboolean wg_download_running(void);       /* un téléchargement est en cours (dans ce processus ou un autre) */
#endif
