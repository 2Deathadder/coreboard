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
#endif
