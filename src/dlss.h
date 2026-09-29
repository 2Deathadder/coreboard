/* Coreboard — prise en charge DLSS : capacités du GPU, versions présentes dans un jeu, réglages dxvk-nvapi */
#ifndef DLSS_H
#define DLSS_H
#include <glib.h>

typedef struct {
    char name[64];
    int series;                                   /* 20, 30, 40, 50… (0 si GPU non NVIDIA) */
    gboolean sr, rr, fg, mfg, dlss5;              /* Super Resolution, Ray Reconstruction, Frame Generation, Multi Frame Generation, DLSS 5 */
} DlssGpu;

typedef struct { char sr[24], fg[24], rr[24]; gboolean any; } DlssGame;

const DlssGpu *dlss_gpu(void);                                              /* détecté une fois (nvidia-smi) */
void dlss_scan_game(const char *exe, DlssGame *out);                        /* cherche nvngx_dlss*.dll autour de l'exécutable */
const char *dlss_generation(const char *version, char *buf, size_t n);      /* « 310.2.1.0 » → « DLSS 4 (modèle transformer) » */
gboolean dlss5_layer_present(void);                                         /* couche Vulkan tierce DLSS 5 installée */
const char *dlss5_reason(void);                                             /* pourquoi le DLSS 5 n'est pas disponible (ou chaîne vide) */
const char *dlss_preset_env(int preset);                                    /* 0 jeu, 1 dernier, 2..5 = J, K, L, M → valeur dxvk-nvapi (NULL = pas de forçage) */
#endif
