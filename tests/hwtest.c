/* Test manuel de la couche matériel : make test */
#include <stdio.h>
#include <string.h>
#include "../src/hw.h"

static GMainLoop *loop; static int pending;
static void cb(const char *msg, gboolean ok, gpointer d) { printf("  -> %s %s\n", (const char *)d, ok ? "OK" : msg); if (--pending == 0) g_main_loop_quit(loop); }

static void run_job(const char *k, const char *v) { pending = 1; hw_apply(k, v, cb, (gpointer)k); loop = g_main_loop_new(NULL, FALSE); g_main_loop_run(loop); g_usleep(1300000); }

int main(void) {
    hw_start();
    HwState s; hw_snapshot(&s);
    printf("%s %s | %s | %d threads | temp %.0f | turbo %d | epp %s\n", s.vendor, s.model, s.cpu_name, s.threads, s.cpu_temp, s.turbo, s.epp);
    printf("GPU %s [%s] util %.0f%% temp %.0f susp=%d | RAM %.0f%% | fans %d | bat %d%% %s\n", s.gpu_name, s.gpu_vendor, s.gpu_util, s.gpu_temp, s.gpu_suspended, s.ram_pct, s.nfans, s.bat_pct, s.bat_status);
    printf("profil %s (%s) [", s.prof_cur, s.prof_backend); for (int i = 0; i < s.nprofs; i++) printf("%s ", s.profs[i]); printf("]\n");
    printf("modeGPU %s (%s) | ecran %s %dx%d@%d modes:", s.gm_cur, s.gm_backend, s.disp.backend, s.disp.w, s.disp.h, s.disp.hz); for (int i = 0; i < s.disp.nmodes; i++) printf(" %d", s.disp.modes[i]); printf("\n");
    printf("audio %s %d%% mute=%d | lum %d | nuit %d\n", s.audio_backend, s.volume, s.muted, s.brightness, s.night_supported);
    int hz0 = s.disp.hz, vol0 = s.volume; char prof0[32]; g_strlcpy(prof0, s.prof_cur, 32);
    char b[16];
    printf("refresh 60:\n"); run_job("refresh", "60"); hw_snapshot(&s); printf("  hz=%d\n", s.disp.hz);
    g_snprintf(b, 16, "%d", hz0); run_job("refresh", b); hw_snapshot(&s); printf("  hz=%d (restauré)\n", s.disp.hz);
    printf("volume 70:\n"); run_job("volume", "70"); hw_snapshot(&s); printf("  vol=%d\n", s.volume);
    g_snprintf(b, 16, "%d", vol0); run_job("volume", b); hw_snapshot(&s); printf("  vol=%d (restauré)\n", s.volume);
    const char *other = strcmp(prof0, "balanced") ? "balanced" : "power-saver";
    printf("profil %s:\n", other); run_job("profile", other); hw_snapshot(&s); printf("  profil=%s\n", s.prof_cur);
    run_job("profile", prof0); hw_snapshot(&s); printf("  profil=%s (restauré)\n", s.prof_cur);
    printf("mauvaise valeur:\n"); run_job("profile", "inexistant");
    hw_stop();
    return 0;
}
