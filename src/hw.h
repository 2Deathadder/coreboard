/* Coreboard — couche matériel (sysfs, hwmon, NVML, DRM, D-Bus, hyprctl…) */
#ifndef HW_H
#define HW_H
#include <glib.h>

#define HIST 60
#define MAXFAN 8
#define MAXCH 8

typedef struct { char label[40]; int rpm; } Fan;
typedef struct { char backend[16], name[40]; int w, h, hz, modes[16], nmodes, x, y; double scale; } Disp;

typedef struct {
    /* appareil */
    char vendor[48], model[80], kernel[64], session[24], wm[40];
    double uptime;
    /* CPU */
    char cpu_name[96]; double cpu_usage, cpu_temp, cpu_freq, cpu_freq_max; int cpu_has_temp, threads;
    double cores[64]; int ncores;
    char governor[32]; char governors[MAXCH][24]; int ngov;
    char epp[32]; char epps[MAXCH][32]; int nepp;
    int turbo;                       /* -1 non supporté, 0 off, 1 on */
    /* GPU principal */
    int gpu_present, gpu_count, gpu_suspended; char gpu_vendor[10], gpu_name[100];
    double gpu_util, gpu_temp, gpu_power, gpu_clock, gpu_mem_used, gpu_mem_total;
    /* RAM / disque / réseau */
    double ram_total, ram_used, ram_pct;
    double disk_total, disk_used, disk_temp;
    double net_down, net_up; char ssid[64]; int signal;
    /* ventilateurs */
    Fan fans[MAXFAN]; int nfans;
    /* batterie */
    int bat_present, bat_pct, bat_health; char bat_status[24]; double bat_watts;
    /* profils */
    char prof_backend[32], prof_cur[32]; char profs[MAXCH][32]; int nprofs;
    /* mode GPU hybride */
    char gm_backend[16], gm_cur[24]; char gms[MAXCH][24]; int ngm;
    /* écran */
    Disp disp;
    int brightness;                  /* -1 si absent */
    char audio_backend[8]; int volume, muted;
    int night_supported, night_on;
    /* historiques (le plus récent en dernier) */
    float h_cpu[HIST], h_gpu[HIST], h_ram[HIST], h_ctemp[HIST], h_gtemp[HIST];
} HwState;

typedef void (*HwMessageCb)(const char *msg, gboolean ok, gpointer data);

void hw_identity(HwState *s);
void hw_start(void);
/* Appelé dans la boucle principale GLib à chaque nouvelle mesure. */
void hw_set_notify(void (*cb)(void));
void hw_stop(void);
void hw_snapshot(HwState *out);
/* Applique un réglage en tâche de fond ; cb est appelé dans la boucle principale GLib. */
void hw_apply(const char *key, const char *value, HwMessageCb cb, gpointer data);
#endif
