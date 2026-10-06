/* Coreboard — refroidissement adaptatif au lancement d'un jeu.
 * L'écriture dans l'EC exige root : un démon (data/coreboard-fand) est lancé une seule fois par pkexec à chaque lancement.
 * Il règle les ventilateurs selon la température (max CPU/GPU) et sa vitesse de montée, puis rend la main quand
 * coreboard ferme son stdin (fin du jeu) ou à l'expiration de la fenêtre. */
#include "fanctl.h"
#include <signal.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#define WINDOW_S 600                        /* durée maximale de la régulation après un lancement */

static GPid pid; static int in_fd = -1, out_fd = -1, level = -1; static guint watch, io_watch;

#ifndef FAND_DIR
#define FAND_DIR "/usr/local/libexec/coreboard"     /* installé par « sudo make install-fand » */
#endif

/* le démon est exécuté en root : il doit appartenir à root et n'être modifiable par personne d'autre, lui comme
   chacun des dossiers parents (sinon un programme tournant sous le compte utilisateur pourrait le remplacer). */
static gboolean root_owned(const char *path) {
    gchar *p = g_strdup(path); gboolean ok = p != NULL;
    while (ok) {
        struct stat sb;
        if (lstat(p, &sb) != 0 || S_ISLNK(sb.st_mode) || sb.st_uid != 0 || (sb.st_mode & (S_IWGRP | S_IWOTH))) { ok = FALSE; break; }
        if (!strcmp(p, "/")) break;
        gchar *up = g_path_get_dirname(p); g_free(p); p = up;
    }
    g_free(p); return ok;
}

static char *daemon_path(void) {
    char *p = g_build_filename(FAND_DIR, "coreboard-fand", NULL);
    if (g_file_test(p, G_FILE_TEST_IS_REGULAR) && root_owned(p)) return p;
    g_free(p); return NULL;
}

gboolean fan_supported(void) {
    char *p = daemon_path(); gboolean ok = p && g_find_program_in_path("pkexec") && g_find_program_in_path("python3");
    g_free(p); return ok;
}
int fan_level(void) { return level; }

static void cleanup(void) {
    if (io_watch) { g_source_remove(io_watch); io_watch = 0; }
    if (in_fd >= 0) { close(in_fd); in_fd = -1; }                     /* stdin fermé : le démon restaure l'EC et s'arrête */
    if (out_fd >= 0) { close(out_fd); out_fd = -1; }
    level = -1;
}

static void on_exit_cb(GPid p, gint status, gpointer d) { g_spawn_close_pid(p); pid = 0; watch = 0; cleanup(); }

static gboolean on_out(GIOChannel *c, GIOCondition cond, gpointer d) {
    char b[16]; ssize_t n = read(out_fd, b, sizeof b - 1);
    if (n <= 0) { io_watch = 0; return G_SOURCE_REMOVE; }
    for (ssize_t i = n - 1; i >= 0; i--) if (b[i] >= '0' && b[i] <= '2') { level = b[i] - '0'; break; }
    return G_SOURCE_CONTINUE;
}

void fan_game_begin(void) {
    if (pid || !fan_supported()) return;
    char *dp = daemon_path(), win[16]; g_snprintf(win, sizeof win, "%d", WINDOW_S);
    char *argv[] = {"pkexec", "python3", dp, win, NULL};
    if (g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
                                 NULL, NULL, &pid, &in_fd, &out_fd, NULL, NULL)) {
        watch = g_child_watch_add(pid, on_exit_cb, NULL);
        GIOChannel *ch = g_io_channel_unix_new(out_fd);
        io_watch = g_io_add_watch(ch, G_IO_IN | G_IO_HUP, on_out, NULL); g_io_channel_unref(ch);
    }
    g_free(dp);
}

void fan_game_end(void) { cleanup(); }
