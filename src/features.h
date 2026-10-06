/* Coreboard — fonctionnalités « Armoury Crate » : profils de scénario, bibliothèque de jeux, périphériques */
#ifndef FEATURES_H
#define FEATURES_H
#include <gio/gio.h>

#define MAXSC 16
#define MAXGAME 128
#define MAXWIN 24
#define MAXDEV 24
#define MAXFREE 60
#define MAXGOGLIB 200
#define MAXHITS 30
#define MAXEPIC 200

/* profil appliqué automatiquement quand une appli (classe de fenêtre) prend le focus */
typedef struct { char name[64], cls[64], profile[32]; int refresh, enabled; } Scenario;
typedef struct { char name[64], id[64], cls[64], source[12]; gboolean manual; GAppInfo *app; char command[256]; } Game;
typedef struct { char cls[64], title[80]; int pid; } Win;
typedef struct { char name[64], kind[12], info[48]; int battery, connected; } Dev;
typedef struct { char name[80]; gint64 id; } FreeGame;              /* jeu free-to-play Steam */
typedef struct { char slug[80], title[96]; } GogLibEntry;           /* jeu du compte GOG (lgogdownloader) */
typedef struct { char name[96]; gint64 id; int price_cents; gboolean free; } SteamHit;  /* résultat de recherche dans le catalogue Steam */
typedef struct {
    gboolean connected, need_guard, error;
    gboolean dl_active; double pct; gint64 done, total;
    char msg[160], game[80];
} ScmStatus;   /* état de la session steamcmd (connexion + téléchargement en cours) */
typedef struct { char appname[80], title[96]; } EpicLibEntry;   /* jeu du catalogue Epic possédé (Legendary) */
typedef struct { gboolean dl_active, error; double pct; char msg[160], game[96]; } EpicStatus;

void fx_init(void);

/* installation automatique (pacman + pkexec) des outils optionnels : steamcmd, legendary, lgogdownloader */
gboolean fx_tool_installing(void);
const char *fx_tool_installing_name(void);   /* nom du paquet en cours d'installation ("" si aucun) */
void fx_tool_install(const char *pkg);
void fx_tool_install_set_done(void (*cb)(const char *pkg, gboolean ok, const char *why));   /* appelé dans le thread principal */

/* scénarios */
Scenario *fx_scenarios(int *n);
int fx_scenario_add(const char *name, const char *cls);
void fx_scenario_remove(int i);
void fx_scenarios_save(void);
const char *fx_scenario_active(void);   /* nom du scénario appliqué, ou NULL */
void fx_scenario_tick(void);            /* à appeler périodiquement (boucle principale) */

/* jeux */
Game *fx_games(int *n);
void fx_games_rescan(void);
void fx_game_launch(const Game *g);
int fx_game_add_manual(const Win *w);
void fx_game_remove(int i);

/* fenêtres ouvertes (Hyprland) */
gboolean fx_windows_supported(void);
int fx_windows(Win *out, int max);       /* dernier instantané ; fx_windows_refresh() le met à jour */
void fx_windows_refresh(void);

/* périphériques (USB externes, Bluetooth, batteries UPower) — instantané mis à jour en tâche de fond */
int fx_devices(Dev *out, int max);
void fx_devices_refresh(void);

/* jeux gratuits Steam (free-to-play) */
void fx_steamfree_refresh(void);
int fx_steamfree(FreeGame *out, int max);
gboolean fx_steamfree_ready(void);   /* FALSE tant qu'aucune recherche n'a encore abouti */

/* bibliothèque du compte GOG via lgogdownloader (déjà connecté par l'utilisateur avec --login) */
gboolean fx_gog_available(void);
void fx_gog_refresh(void);
int fx_gog_list(GogLibEntry *out, int max);
void fx_gog_download(const GogLibEntry *g);

/* recherche libre dans le catalogue Steam (n'importe quel jeu, pas seulement gratuit) */
void fx_steamsearch_query(const char *q);
int fx_steamsearch(SteamHit *out, int max);
const char *fx_steamsearch_query_str(void);

/* session Steam intégrée (steamcmd, l'outil officiel Valve en ligne de commande) : connexion et téléchargement
   réel directement depuis Coreboard, sans ouvrir le client Steam. Le mot de passe n'est jamais écrit sur le
   disque ; seul le nom d'utilisateur est mémorisé pour préremplir la prochaine connexion. */
gboolean fx_steam_available(void);            /* steamcmd installé */
gboolean fx_steam_logged_in(void);
const char *fx_steam_username(void);          /* dernier identifiant utilisé (vide si aucun) */
void fx_steam_login(const char *user, const char *pass);   /* pass vide = tente la session mémorisée par steamcmd */
void fx_steam_guard_code(const char *code);    /* répond à la demande de code Steam Guard en cours */
void fx_steam_logout(void);
ScmStatus fx_steam_status(void);
void fx_steam_download(gint64 appid, const char *name);    /* nécessite d'être connecté */

/* session Epic intégrée (Legendary, le client Epic Games open-source en ligne de commande) : connexion et
   téléchargement réel directement depuis Coreboard, sans ouvrir de client Epic. La connexion se fait via la
   page web officielle d'Epic (aucun mot de passe ne passe par Coreboard) : on récupère un code d'autorisation
   à usage unique que Legendary garde ensuite en cache localement. */
gboolean fx_epic_available(void);             /* legendary installé */
gboolean fx_epic_logged_in(void);
void fx_epic_login(const char *code);         /* code affiché par https://legendary.gl/epiclogin */
void fx_epic_logout(void);
void fx_epic_refresh(void);                   /* liste le catalogue possédé (tâche de fond) */
int fx_epic_list(EpicLibEntry *out, int max);
gboolean fx_epic_ready(void);
void fx_epic_download(const char *appname, const char *title);   /* nécessite d'être connecté */
EpicStatus fx_epic_status(void);

/* ---- FitGirl Repacks : recherche et téléchargement direct via Fistgirl (fistgirl_helper.py)
        Moteur : paste.fitgirl-repacks.site → déchiffrement PrivateBin → fuckingfast.co CDN
        Entièrement natif, sans FDM, avec reprise de téléchargement. */

#define MAXFG_RESULTS 20    /* résultats de recherche */
#define MAXFG_FILES   100   /* parties d'un même repack */

typedef struct {
    char title[128];      /* titre du repack */
    char page_url[256];   /* URL de la page fitgirl-repacks.site */
} FgSearchHit;

typedef struct {
    int  file_count;                 /* nombre total de parties */
    int  file_done;                  /* parties téléchargées */
    int  file_current;               /* indice de la partie en cours (0-based) */
    char file_name[80];              /* nom du fichier en cours */
    char game[128];                  /* titre du jeu */
    char dest[256];                  /* dossier de destination */
    double pct;                      /* progression de la partie courante (0-100) */
    double speed_bps;                /* débit en octets/s */
    gint64 file_bytes, file_size;    /* octets de la partie en cours */
    int conns;                       /* connexions actives (téléchargement segmenté) */
    gboolean resolving;              /* lien direct de la partie en cours de résolution */
    gint64 game_done, game_total;    /* octets jeu entier */
    gboolean active;                 /* téléchargement en cours */
    gboolean completed;              /* jeu complet téléchargé */
    gboolean error;
    char msg[200];
} FgDlStatus;

/* Recherche de repacks dans le catalogue FitGirl */
void     fx_fg_search(const char *query);      /* lance en tâche de fond */
int      fx_fg_results(FgSearchHit *out, int max);
gboolean fx_fg_search_busy(void);               /* TRUE tant que la recherche tourne */
const char *fx_fg_query(void);
const char *fx_fg_search_error(void);           /* erreur de la dernière recherche ("" si aucune) */                  /* requête courante */

/* Résout une URL (page jeu ou paste ou ff directe) → liste de fichiers */
void     fx_fg_resolve(const char *url);        /* lance en tâche de fond */
gboolean fx_fg_resolved(void);                  /* TRUE une fois la résolution terminée */
gboolean fx_fg_resolve_busy(void);              /* TRUE pendant la résolution */
const char *fx_fg_resolve_error(void);          /* message d'erreur de la dernière résolution ("" si aucune) */
int      fx_fg_file_count(void);
int      fx_fg_optional_count(void);            /* fichiers optionnels (voix, bonus) non téléchargés */
const char *fx_fg_page_url(void);               /* page du jeu sélectionné */
const char *fx_fg_game_title(void);

/* Téléchargement FitGirl */
void     fx_fg_download(const char *dest_dir);  /* démarre le téléchargement dans dest_dir */
void     fx_fg_cancel(void);                     /* annule et supprime les fichiers temporaires */
FgDlStatus fx_fg_dl_status(void);

/* Extraction du repack téléchargé (7-Zip ou unrar) puis repérage de l'installateur setup.exe */
typedef struct {
    gboolean active, done, error;
    int pct;                         /* progression de l'extraction (0-100) */
    char msg[200], setup[512], dest[512];
} FgExStatus;
gboolean fx_fg_extract_tool(void);               /* 7z, 7zz ou unrar présent */
void     fx_fg_extract(const char *dir, gboolean delete_archives);   /* lance en tâche de fond ; archives supprimées après succès si demandé */
FgExStatus fx_fg_ex_status(void);
void     fx_fg_ex_reset(void);
void     fx_fg_ex_set_done(const char *setup);    /* archives déjà extraites : installateur connu */

#endif

