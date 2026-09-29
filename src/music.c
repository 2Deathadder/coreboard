/* Coreboard — capture audio (parec) + FFT 1024 points, 4 bandes de fréquences, détection de battements */
#include "music.h"
#include <math.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define RATE 44100
#define N 1024
#define HOP 256                       /* ~5,8 ms entre deux analyses */
#define DT ((double)HOP / RATE)

static GMutex lk;
static double lev[4], hold[4], beat_env;
static int beats;
static volatile int sens = 50, smooth = 40;
static GPid child; static int fd = -1; static GThread *th; static volatile gboolean run;

static void child_setup(gpointer d) { (void)d; prctl(PR_SET_PDEATHSIG, SIGTERM); }

static void fft(double *re, double *im) {
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= N; len <<= 1) {
        double ang = -2 * G_PI / len, wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < N; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

static gpointer worker(gpointer d) {
    (void)d;
    static const int EDGE[5] = {1, 4, 19, 94, 372};                 /* bornes de bandes en bins (43 Hz par bin) */
    gint16 win[N] = {0}, chunk[HOP];
    double hann[N], peak[4] = {0.02, 0.02, 0.02, 0.02}, sm[4] = {0};
    double flux_hist[96] = {0}, prev_bass = 0, since_beat = 1;      /* ~0,5 s d'historique du flux spectral */
    int fh = 0;
    for (int i = 0; i < N; i++) hann[i] = 0.5 - 0.5 * cos(2 * G_PI * i / (N - 1));
    while (run) {
        gsize got = 0;
        while (got < sizeof chunk && run) {
            ssize_t r = read(fd, (char *)chunk + got, sizeof chunk - got);
            if (r <= 0) { run = FALSE; break; }
            got += r;
        }
        if (!run) break;
        memmove(win, win + HOP, (N - HOP) * sizeof(gint16)); memcpy(win + (N - HOP), chunk, sizeof chunk);
        double re[N], im[N];
        for (int i = 0; i < N; i++) { re[i] = win[i] / 32768.0 * hann[i]; im[i] = 0; }
        fft(re, im);
        double tau = 0.03 + smooth / 100.0 * 0.30;                   /* retombée : 30 ms (instantané) à 330 ms (doux) */
        double decay = exp(-DT / tau), range_db = 42 - sens * 0.22;  /* plus de sensibilité = plus de plage dynamique */
        double out[4], bass_e = 0;
        for (int b = 0; b < 4; b++) {
            double sum = 0;
            for (int k = EDGE[b]; k < EDGE[b + 1]; k++) sum += re[k] * re[k] + im[k] * im[k];
            double e = sqrt(sum / (EDGE[b + 1] - EDGE[b])) / N * 8;
            if (b == 0) bass_e = e;
            peak[b] = MAX(MAX(e, 0.02), peak[b] * exp(-DT / 6.0));   /* normalisation adaptative (constante de temps ~6 s) */
            double db = 20 * log10(e / peak[b] + 1e-6);                /* 0 dB = pic récent de la bande */
            double v = CLAMP((db + range_db) / range_db, 0, 1);       /* réponse en dB : plus fidèle à l'oreille */
            if (e < 0.004) v = 0;                                      /* seuil de bruit : silence = noir */
            sm[b] = v > sm[b] ? v : sm[b] * decay;                     /* attaque instantanée, retombée douce */
            out[b] = sm[b];
        }
        /* battement : montée brusque des graves par rapport à l'historique récent */
        double flux = MAX(0, bass_e - prev_bass); prev_bass = bass_e;
        flux_hist[fh] = flux; fh = (fh + 1) % 96;
        double mean = 0; for (int i = 0; i < 96; i++) mean += flux_hist[i]; mean /= 96;
        double var = 0; for (int i = 0; i < 96; i++) var += (flux_hist[i] - mean) * (flux_hist[i] - mean); var = sqrt(var / 96);
        since_beat += DT;
        gboolean beat = since_beat > 0.18 && flux > mean + 2.2 * var + 0.01 && bass_e > 0.03;
        if (beat) since_beat = 0;
        g_mutex_lock(&lk);
        memcpy(lev, out, sizeof out);
        for (int b = 0; b < 4; b++) hold[b] = MAX(hold[b], out[b]);
        beat_env *= exp(-DT / 0.10);
        if (beat) { beat_env = 1; beats++; }
        g_mutex_unlock(&lk);
    }
    g_mutex_lock(&lk); memset(lev, 0, sizeof lev); memset(hold, 0, sizeof hold); beat_env = 0; g_mutex_unlock(&lk);
    return NULL;
}

gboolean music_running(void) { return run; }
void music_set_params(int s, int sm) { sens = CLAMP(s, 0, 100); smooth = CLAMP(sm, 0, 100); }

void music_levels(double out[4], gboolean reset) {
    g_mutex_lock(&lk);
    memcpy(out, reset ? hold : lev, sizeof lev);
    if (reset) memcpy(hold, lev, sizeof lev);
    g_mutex_unlock(&lk);
}
double music_beat_env(void) { g_mutex_lock(&lk); double v = beat_env; g_mutex_unlock(&lk); return v; }
int music_beat_count(void) { g_mutex_lock(&lk); int v = beats; g_mutex_unlock(&lk); return v; }

gboolean music_start(char *err, size_t n) {
    if (run) return TRUE;
    if (th) { g_thread_join(th); th = NULL; }
    if (!g_find_program_in_path("parec")) { g_strlcpy(err, "parec introuvable (paquet libpulse / pipewire-pulse)", n); return FALSE; }
    const char *argv[] = {"parec", "-d", "@DEFAULT_MONITOR@", "--format=s16le", "--rate=44100", "--channels=1", "--latency-msec=15", NULL};
    gint out = -1; GError *e = NULL;
    if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
                                  child_setup, NULL, &child, NULL, &out, NULL, &e)) {
        g_snprintf(err, n, "Capture audio impossible : %s", e->message); g_error_free(e); return FALSE;
    }
    fd = out; run = TRUE;
    th = g_thread_new("coreboard-music", worker, NULL);
    return TRUE;
}

void music_stop(void) {
    if (!run && !th) return;
    run = FALSE;
    if (child) { kill(child, SIGTERM); waitpid(child, NULL, 0); g_spawn_close_pid(child); child = 0; }
    if (fd >= 0) { close(fd); fd = -1; }
    if (th) { g_thread_join(th); th = NULL; }
}
