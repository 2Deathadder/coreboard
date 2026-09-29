/* Coreboard — analyse d'exécutables Windows (format PE) : architecture, API graphique, moteur, anti-triche, .NET */
#ifndef PE_H
#define PE_H
#include <glib.h>

typedef struct {
    gboolean valid, is64, dotnet, console;
    int machine;                                   /* 0x14c x86, 0x8664 x64, 0xaa64 ARM64 */
    gboolean dx9, dx10, dx11, dx12, vulkan, opengl;
    gboolean nvapi, dlss, xinput, steam_api, eos;
    char engine[24];                               /* « Unreal Engine », « Unity », « Godot »… */
    char ac_kernel[32];                            /* anti-triche noyau/incompatible Linux : Vanguard, Ricochet… */
    char ac_soft[32];                              /* anti-triche compatible Proton si le jeu l'active : EasyAntiCheat, BattlEye */
    char imports[16][32];                          /* premières DLL importées */
    int nimports;
} PeInfo;

gboolean pe_analyze(const char *path, PeInfo *out);                 /* en-têtes + imports + recherche de chaînes + fichiers voisins */
void pe_summary(const PeInfo *p, char *buf, size_t n);              /* « 64 bits · DirectX 12 · Unreal Engine · DLSS » */
void pe_advice(const PeInfo *p, char *buf, size_t n);               /* avertissement (anti-triche, .NET…), chaîne vide si rien à signaler */
gboolean pe_file_version(const char *path, char *out, size_t n);   /* version de fichier d'une DLL/EXE (ressource de version) */
#endif
