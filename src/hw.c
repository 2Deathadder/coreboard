/* Coreboard — couche matériel. Aucune dépendance web : sysfs/hwmon, NVML (dlopen),
 * DRM, D-Bus (power-profiles-daemon) et quelques outils système pour l'écran/audio. */
#define _GNU_SOURCE
#include "hw.h"
#include <dlfcn.h>
#include <gio/gio.h>
#include <glob.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

/* ------------------------------------------------------------------ utilitaires */
static gboolean rd(const char *path, char *buf, size_t n) {
    gchar *c = NULL;
    if (!g_file_get_contents(path, &c, NULL, NULL)) { if (n) buf[0] = 0; return FALSE; }
    g_strstrip(c);
    g_strlcpy(buf, c, n);
    g_free(c);
    return TRUE;
}

static long rdl(const char *path, long def) {
    char b[64];
    if (!rd(path, b, sizeof b) || !b[0]) return def;
    char *e; long v = strtol(b, &e, 10);
    return e == b ? def : v;
}

static gboolean has_cmd(const char *c) {
    gchar *p = g_find_program_in_path(c);
    g_free(p);
    return p != NULL;
}

/* Exécute argv ; renvoie stdout (à libérer) si code 0, sinon NULL. err reçoit stderr/stdout d'erreur. */
static gchar *run_out(char **argv, char *err, size_t en) {
    gchar *out = NULL, *e = NULL; gint st = 0;
    if (err && en) err[0] = 0;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &e, &st, NULL)) { g_free(out); g_free(e); return NULL; }
    if (!g_spawn_check_wait_status(st, NULL)) {
        if (err && en) { g_strstrip(e); g_strstrip(out); g_strlcpy(err, (e && *e) ? e : (out ? out : ""), en); }
        g_free(out); g_free(e);
        return NULL;
    }
    g_free(e);
    if (out) g_strstrip(out);
    return out;
}

static gboolean run_ok(char **argv, char *err, size_t en) {
    gchar *o = run_out(argv, err, en);
    g_free(o);
    return o != NULL;
}

static gchar *cmd1(const char *a, const char *b, const char *c) {
    char *argv[] = {(char *)a, (char *)b, (char *)c, NULL};
    if (!b) argv[1] = NULL; else if (!c) argv[2] = NULL;
    return run_out(argv, NULL, 0);
}

static gboolean priv(const char *script, char *msg, size_t n) {
    if (!has_cmd("pkexec")) { g_strlcpy(msg, "pkexec introuvable (polkit requis)", n); return FALSE; }
    char *argv[] = {"pkexec", "sh", "-c", (char *)script, NULL};
    if (run_ok(argv, msg, n)) { msg[0] = 0; return TRUE; }
    if (!msg[0]) g_strlcpy(msg, "Élévation refusée ou agent polkit absent", n);
    return FALSE;
}

/* ------------------------------------------------------------------ découverte */
static char cpu_temp_path[160], nvme_temp_path[160], backlight_dir[160];
static char fan_path[MAXFAN][160], fan_label[MAXFAN][40]; static int nfan_paths;
static char nv_pci[64];          /* /sys/bus/pci/devices/<slot> du GPU NVIDIA */
static GMutex lock; static GCond wake; static HwState S, W;
static gboolean running, force_slow, ready;
static GThread *thr;
static void (*notify_cb)(void);
static gboolean notify_idle(gpointer d) { (void)d; if (notify_cb) notify_cb(); return G_SOURCE_REMOVE; }
void hw_set_notify(void (*cb)(void)) { notify_cb = cb; }
static gboolean night_on;

static void discover(void) {
    glob_t g;
    const char *pref[] = {"coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu-thermal", "soc_thermal", NULL};
    if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &g) == 0) {
        for (int p = 0; pref[p] && !cpu_temp_path[0]; p++)
            for (size_t i = 0; i < g.gl_pathc && !cpu_temp_path[0]; i++) {
                char path[200], nm[64];
                g_snprintf(path, sizeof path, "%s/name", g.gl_pathv[i]);
                rd(path, nm, sizeof nm);
                if (strcmp(nm, pref[p])) continue;
                for (int t = 1; t < 16; t++) {
                    char lab[64];
                    g_snprintf(path, sizeof path, "%s/temp%d_label", g.gl_pathv[i], t);
                    if (rd(path, lab, sizeof lab) && (!strcmp(lab, "Package id 0") || !strcmp(lab, "Tctl") || !strcmp(lab, "Tdie"))) {
                        g_snprintf(cpu_temp_path, sizeof cpu_temp_path, "%s/temp%d_input", g.gl_pathv[i], t);
                        break;
                    }
                }
                if (!cpu_temp_path[0]) g_snprintf(cpu_temp_path, sizeof cpu_temp_path, "%s/temp1_input", g.gl_pathv[i]);
            }
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char path[200], nm[64];
            g_snprintf(path, sizeof path, "%s/name", g.gl_pathv[i]);
            rd(path, nm, sizeof nm);
            if (!strcmp(nm, "nvme") && !nvme_temp_path[0]) g_snprintf(nvme_temp_path, sizeof nvme_temp_path, "%s/temp1_input", g.gl_pathv[i]);
            for (int f = 1; f <= 8 && nfan_paths < MAXFAN; f++) {
                g_snprintf(path, sizeof path, "%s/fan%d_input", g.gl_pathv[i], f);
                if (access(path, R_OK)) continue;
                g_strlcpy(fan_path[nfan_paths], path, 160);
                char lp[200]; g_snprintf(lp, sizeof lp, "%s/fan%d_label", g.gl_pathv[i], f);
                if (!rd(lp, fan_label[nfan_paths], 40) || !fan_label[nfan_paths][0])
                    g_snprintf(fan_label[nfan_paths], 40, "Ventilateur %d", nfan_paths + 1);
                nfan_paths++;
            }
        }
        globfree(&g);
    }
    if (!cpu_temp_path[0] && glob("/sys/class/thermal/thermal_zone*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && !cpu_temp_path[0]; i++) {
            char path[200], ty[64];
            g_snprintf(path, sizeof path, "%s/type", g.gl_pathv[i]);
            rd(path, ty, sizeof ty);
            if (!strcmp(ty, "x86_pkg_temp") || !strcmp(ty, "cpu-thermal") || !strcmp(ty, "soc-thermal"))
                g_snprintf(cpu_temp_path, sizeof cpu_temp_path, "%s/temp", g.gl_pathv[i]);
        }
        globfree(&g);
    }
    /* rétroéclairage : firmware > platform > raw, en évitant nvidia_0 */
    int best = 99;
    if (glob("/sys/class/backlight/*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char path[200], ty[32];
            g_snprintf(path, sizeof path, "%s/type", g.gl_pathv[i]);
            rd(path, ty, sizeof ty);
            int r = !strcmp(ty, "firmware") ? 0 : !strcmp(ty, "platform") ? 1 : 2;
            if (strstr(g.gl_pathv[i], "nvidia")) r += 10;
            if (r < best) { best = r; g_strlcpy(backlight_dir, g.gl_pathv[i], sizeof backlight_dir); }
        }
        globfree(&g);
    }
    if (glob("/sys/bus/pci/drivers/nvidia/0000:*", 0, NULL, &g) == 0) {
        if (g.gl_pathc) g_snprintf(nv_pci, sizeof nv_pci, "/sys/bus/pci/devices/%s", strrchr(g.gl_pathv[0], '/') + 1);
        globfree(&g);
    }
}

/* ------------------------------------------------------------------ NVML (dlopen) */
typedef void *nvmlDev;
typedef struct { unsigned gpu, memory; } nvmlUtil;
typedef struct { unsigned long long total, free, used; } nvmlMem;
static struct {
    void *h; int ok;
    int (*init)(void); int (*count)(unsigned *); int (*handle)(unsigned, nvmlDev *);
    int (*name)(nvmlDev, char *, unsigned); int (*temp)(nvmlDev, int, unsigned *);
    int (*util)(nvmlDev, nvmlUtil *); int (*power)(nvmlDev, unsigned *);
    int (*clock)(nvmlDev, int, unsigned *); int (*mem)(nvmlDev, nvmlMem *);
} nv;

static void nvml_load(void) {
    nv.h = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!nv.h) return;
    nv.init = dlsym(nv.h, "nvmlInit_v2"); nv.count = dlsym(nv.h, "nvmlDeviceGetCount_v2");
    nv.handle = dlsym(nv.h, "nvmlDeviceGetHandleByIndex_v2"); nv.name = dlsym(nv.h, "nvmlDeviceGetName");
    nv.temp = dlsym(nv.h, "nvmlDeviceGetTemperature"); nv.util = dlsym(nv.h, "nvmlDeviceGetUtilizationRates");
    nv.power = dlsym(nv.h, "nvmlDeviceGetPowerUsage"); nv.clock = dlsym(nv.h, "nvmlDeviceGetClockInfo");
    nv.mem = dlsym(nv.h, "nvmlDeviceGetMemoryInfo");
    nv.ok = nv.init && nv.count && nv.handle && nv.temp && nv.util && nv.init() == 0;
}

static void lspci_name(const char *slot_path, char *out, size_t n) {
    const char *slot = strrchr(slot_path, '/');
    slot = slot ? slot + 1 : slot_path;
    char *argv[] = {"lspci", "-s", (char *)slot, NULL};
    gchar *o = has_cmd("lspci") ? run_out(argv, NULL, 0) : NULL;
    if (o) {
        char *d = strstr(o, ": ");
        char *br = d ? strchr(d, '[') : NULL, *be = br ? strrchr(br, ']') : NULL;
        if (br && be && be > br) { *be = 0; g_strlcpy(out, br + 1, n); }
        else if (d) { char *rev = strstr(d + 2, " (rev"); if (rev) *rev = 0; g_strlcpy(out, d + 2, n); }
        g_free(o);
    }
    if (!out[0]) g_strlcpy(out, "GPU", n);
}

static char nv_name[100];
static void read_gpu(HwState *s) {
    s->gpu_present = 0; s->gpu_count = 0; s->gpu_suspended = 0;
    s->gpu_util = s->gpu_temp = s->gpu_power = s->gpu_clock = s->gpu_mem_used = s->gpu_mem_total = 0;
    /* NVIDIA : on ne réveille pas un dGPU endormi (économie batterie) */
    if (nv_pci[0]) {
        char path[200], st[24];
        g_snprintf(path, sizeof path, "%s/power/runtime_status", nv_pci);
        rd(path, st, sizeof st);
        if (!nv_name[0]) lspci_name(nv_pci, nv_name, sizeof nv_name);
        s->gpu_present = 1; s->gpu_count = 1;
        g_strlcpy(s->gpu_vendor, "nvidia", sizeof s->gpu_vendor);
        if (!strcmp(st, "suspended")) { s->gpu_suspended = 1; g_strlcpy(s->gpu_name, nv_name, sizeof s->gpu_name); return; }
        nvmlDev d; unsigned v = 0;
        if (nv.ok && nv.handle(0, &d) == 0) {
            char nm[96] = "";
            if (nv.name && nv.name(d, nm, sizeof nm) == 0 && nm[0]) g_strlcpy(nv_name, nm, sizeof nv_name);
            g_strlcpy(s->gpu_name, nv_name, sizeof s->gpu_name);
            if (nv.temp(d, 0, &v) == 0) s->gpu_temp = v;
            nvmlUtil u; if (nv.util(d, &u) == 0) s->gpu_util = u.gpu;
            if (nv.power && nv.power(d, &v) == 0) s->gpu_power = v / 1000.0;
            if (nv.clock && nv.clock(d, 0, &v) == 0) s->gpu_clock = v;
            nvmlMem m; if (nv.mem && nv.mem(d, &m) == 0) { s->gpu_mem_used = m.used / 1048576.0; s->gpu_mem_total = m.total / 1048576.0; }
        } else g_strlcpy(s->gpu_name, nv_name, sizeof s->gpu_name);
        return;
    }
    /* AMD / Intel via DRM */
    glob_t g;
    if (glob("/sys/class/drm/card[0-9]", 0, NULL, &g)) return;
    int amd = -1, intel = -1;
    char vend[16];
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char p[220]; g_snprintf(p, sizeof p, "%s/device/vendor", g.gl_pathv[i]);
        rd(p, vend, sizeof vend);
        if (!strcmp(vend, "0x1002") && amd < 0) amd = i;
        if (!strcmp(vend, "0x8086") && intel < 0) intel = i;
    }
    int pick = amd >= 0 ? amd : intel;
    if (pick >= 0) {
        const char *c = g.gl_pathv[pick];
        char dev[220], p[260], st[24];
        g_snprintf(dev, sizeof dev, "%s/device", c);
        s->gpu_present = 1; s->gpu_count = (amd >= 0) + (intel >= 0);
        static char amdname[100], intname[100];
        char *nm = pick == amd ? amdname : intname;
        if (!nm[0]) lspci_name(dev, nm, 100);
        g_strlcpy(s->gpu_name, nm, sizeof s->gpu_name);
        g_snprintf(p, sizeof p, "%s/power/runtime_status", dev);
        rd(p, st, sizeof st);
        if (pick == amd) {
            g_strlcpy(s->gpu_vendor, "amd", sizeof s->gpu_vendor);
            if (!strcmp(st, "suspended")) { s->gpu_suspended = 1; }
            else {
                g_snprintf(p, sizeof p, "%s/gpu_busy_percent", dev); s->gpu_util = rdl(p, 0);
                g_snprintf(p, sizeof p, "%s/mem_info_vram_used", dev); s->gpu_mem_used = rdl(p, 0) / 1048576.0;
                g_snprintf(p, sizeof p, "%s/mem_info_vram_total", dev); s->gpu_mem_total = rdl(p, 0) / 1048576.0;
                glob_t h;
                g_snprintf(p, sizeof p, "%s/hwmon/hwmon*", dev);
                if (glob(p, 0, NULL, &h) == 0 && h.gl_pathc) {
                    g_snprintf(p, sizeof p, "%s/temp1_input", h.gl_pathv[0]); s->gpu_temp = rdl(p, 0) / 1000.0;
                    g_snprintf(p, sizeof p, "%s/power1_average", h.gl_pathv[0]); long w = rdl(p, 0);
                    if (!w) { g_snprintf(p, sizeof p, "%s/power1_input", h.gl_pathv[0]); w = rdl(p, 0); }
                    s->gpu_power = w / 1e6;
                    g_snprintf(p, sizeof p, "%s/freq1_input", h.gl_pathv[0]); s->gpu_clock = rdl(p, 0) / 1e6;
                    globfree(&h);
                }
            }
        } else {
            g_strlcpy(s->gpu_vendor, "intel", sizeof s->gpu_vendor);
            g_snprintf(p, sizeof p, "%s/gt_act_freq_mhz", c); long f = rdl(p, 0);
            if (!f) { g_snprintf(p, sizeof p, "%s/gt_cur_freq_mhz", c); f = rdl(p, 0); }
            s->gpu_clock = f;
        }
    }
    globfree(&g);
}

/* ------------------------------------------------------------------ CPU / RAM / réseau / disque */
static unsigned long long prev_tot[64], prev_idle[64]; static int prev_n;
static unsigned long long prev_rx, prev_tx; static gboolean have_net;

static void read_cpu(HwState *s) {
    gchar *c = NULL;
    if (g_file_get_contents("/proc/stat", &c, NULL, NULL)) {
        gchar **ln = g_strsplit(c, "\n", -1);
        int n = 0; double sum = 0;
        for (int i = 0; ln[i] && n < 64; i++) {
            if (strncmp(ln[i], "cpu", 3) || !g_ascii_isdigit(ln[i][3])) continue;
            unsigned long long v[10] = {0};
            sscanf(strchr(ln[i], ' '), "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
            unsigned long long tot = 0; for (int k = 0; k < 10; k++) tot += v[k];
            unsigned long long idle = v[3] + v[4];
            double u = 0;
            if (prev_n > n && tot > prev_tot[n]) u = 100.0 * (1.0 - (double)(idle - prev_idle[n]) / (double)(tot - prev_tot[n]));
            s->cores[n] = u; sum += u;
            prev_tot[n] = tot; prev_idle[n] = idle; n++;
        }
        s->ncores = n; prev_n = n;
        s->cpu_usage = n ? sum / n : 0;
        g_strfreev(ln); g_free(c);
    }
    /* fréquences */
    glob_t g; double sum = 0, mx = 0; int n = 0;
    if (glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) { double f = rdl(g.gl_pathv[i], 0) / 1000.0; sum += f; if (f > mx) mx = f; n++; }
        globfree(&g);
    }
    s->cpu_freq = n ? sum / n : 0; s->cpu_freq_max = mx;
    s->cpu_has_temp = cpu_temp_path[0] != 0;
    s->cpu_temp = s->cpu_has_temp ? rdl(cpu_temp_path, 0) / 1000.0 : 0;
}

static void split_words(const char *src, char dst[][32], int *n, int max) {
    *n = 0;
    gchar **w = g_strsplit(src, " ", -1);
    for (int i = 0; w[i] && *n < max; i++) if (w[i][0]) g_strlcpy(dst[(*n)++], w[i], 32);
    g_strfreev(w);
}

static void read_cpu_tuning(HwState *s) {
    const char *F = "/sys/devices/system/cpu/cpu0/cpufreq/";
    char p[200], b[300];
    g_snprintf(p, sizeof p, "%sscaling_governor", F); rd(p, s->governor, sizeof s->governor);
    g_snprintf(p, sizeof p, "%sscaling_available_governors", F);
    rd(p, b, sizeof b);
    char tmp[MAXCH][32]; int n; split_words(b, tmp, &n, MAXCH);
    s->ngov = n; for (int i = 0; i < n; i++) g_strlcpy(s->governors[i], tmp[i], 24);
    g_snprintf(p, sizeof p, "%senergy_performance_available_preferences", F);
    rd(p, b, sizeof b);
    split_words(b, tmp, &n, MAXCH);
    s->nepp = n; for (int i = 0; i < n; i++) g_strlcpy(s->epps[i], tmp[i], 32);
    g_snprintf(p, sizeof p, "%senergy_performance_preference", F);
    if (n) rd(p, s->epp, sizeof s->epp); else s->epp[0] = 0;
    if (!access("/sys/devices/system/cpu/intel_pstate/no_turbo", R_OK)) s->turbo = rdl("/sys/devices/system/cpu/intel_pstate/no_turbo", 1) == 0;
    else if (!access("/sys/devices/system/cpu/cpufreq/boost", R_OK)) s->turbo = rdl("/sys/devices/system/cpu/cpufreq/boost", 0) == 1;
    else s->turbo = -1;
}

static void read_mem_net_disk(HwState *s, int do_disk) {
    gchar *c = NULL; long total = 0, avail = 0;
    if (g_file_get_contents("/proc/meminfo", &c, NULL, NULL)) {
        char *t = strstr(c, "MemTotal:"), *a = strstr(c, "MemAvailable:");
        if (t) total = atol(t + 9);
        if (a) avail = atol(a + 13);
        g_free(c);
    }
    s->ram_total = total / 1024.0; s->ram_used = (total - avail) / 1024.0; s->ram_pct = total ? 100.0 * (total - avail) / total : 0;
    if (g_file_get_contents("/proc/net/dev", &c, NULL, NULL)) {
        unsigned long long rx = 0, tx = 0;
        gchar **ln = g_strsplit(c, "\n", -1);
        for (int i = 2; ln[i]; i++) {
            char *col = strchr(ln[i], ':');
            if (!col) continue;
            *col = 0;
            if (strstr(ln[i], "lo") && strlen(g_strstrip(ln[i])) == 2) continue;
            unsigned long long a, b, cc, d, e, f, gg, h, tb;
            if (sscanf(col + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &a, &b, &cc, &d, &e, &f, &gg, &h, &tb) == 9) { rx += a; tx += tb; }
        }
        s->net_down = have_net && rx >= prev_rx ? rx - prev_rx : 0;
        s->net_up = have_net && tx >= prev_tx ? tx - prev_tx : 0;
        prev_rx = rx; prev_tx = tx; have_net = TRUE;
        g_strfreev(ln); g_free(c);
    }
    if (do_disk) {
        struct statvfs v;
        if (!statvfs("/", &v)) {
            s->disk_total = (double)v.f_blocks * v.f_frsize / 1e9;
            s->disk_used = (double)(v.f_blocks - v.f_bfree) * v.f_frsize / 1e9;
        }
        s->disk_temp = nvme_temp_path[0] ? rdl(nvme_temp_path, 0) / 1000.0 : 0;
    }
}

static void read_fans(HwState *s) {
    s->nfans = nfan_paths;
    for (int i = 0; i < nfan_paths; i++) { g_strlcpy(s->fans[i].label, fan_label[i], 40); s->fans[i].rpm = rdl(fan_path[i], 0); }
}

static void read_battery(HwState *s) {
    glob_t g; s->bat_present = 0;
    if (glob("/sys/class/power_supply/*", 0, NULL, &g)) return;
    for (size_t i = 0; i < g.gl_pathc && !s->bat_present; i++) {
        char p[220], ty[24], sc[24];
        g_snprintf(p, sizeof p, "%s/type", g.gl_pathv[i]); rd(p, ty, sizeof ty);
        g_snprintf(p, sizeof p, "%s/scope", g.gl_pathv[i]); rd(p, sc, sizeof sc);
        if (strcmp(ty, "Battery") || !strcmp(sc, "Device")) continue;
#define BP(n) (g_snprintf(p, sizeof p, "%s/" n, g.gl_pathv[i]), p)
        s->bat_present = 1;
        s->bat_pct = rdl(BP("capacity"), 0);
        rd(BP("status"), s->bat_status, sizeof s->bat_status);
        long full = rdl(BP("charge_full"), 0), design = rdl(BP("charge_full_design"), 0);
        if (!full) { full = rdl(BP("energy_full"), 0); design = rdl(BP("energy_full_design"), 0); }
        s->bat_health = design ? (int)lround(100.0 * full / design) : 0;
        long pw = rdl(BP("power_now"), -1);
        s->bat_watts = pw >= 0 ? pw / 1e6 : fabs((double)rdl(BP("voltage_now"), 0) * rdl(BP("current_now"), 0) / 1e12);
#undef BP
    }
    globfree(&g);
}

/* ------------------------------------------------------------------ profils d'alimentation (D-Bus) */
static const char *PPD[][3] = {
    {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles"},
    {"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles"},
};
static GDBusConnection *sysbus; static int ppd_idx = -1;

static GVariant *ppd_get(const char *prop) {
    if (!sysbus) sysbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!sysbus) return NULL;
    for (int k = 0; k < 2; k++) {
        int i = ppd_idx >= 0 ? ppd_idx : k;
        GVariant *r = g_dbus_connection_call_sync(sysbus, PPD[i][0], PPD[i][1], "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", PPD[i][2], prop), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 800, NULL, NULL);
        if (r) { ppd_idx = i; GVariant *v; g_variant_get(r, "(v)", &v); g_variant_unref(r); return v; }
        if (ppd_idx >= 0) break;
    }
    return NULL;
}

static void read_profiles(HwState *s) {
    s->nprofs = 0; s->prof_backend[0] = 0; s->prof_cur[0] = 0;
    GVariant *cur = ppd_get("ActiveProfile");
    if (cur) {
        g_strlcpy(s->prof_cur, g_variant_get_string(cur, NULL), sizeof s->prof_cur);
        g_variant_unref(cur);
        GVariant *lst = ppd_get("Profiles");
        if (lst) {
            GVariantIter it; g_variant_iter_init(&it, lst);
            GVariant *d;
            while (s->nprofs < MAXCH && (d = g_variant_iter_next_value(&it))) {
                const char *name = NULL;
                if (g_variant_lookup(d, "Profile", "&s", &name) && name) g_strlcpy(s->profs[s->nprofs++], name, 32);
                g_variant_unref(d);
            }
            g_variant_unref(lst);
        }
        if (s->nprofs) { g_strlcpy(s->prof_backend, "power-profiles-daemon (D-Bus)", sizeof s->prof_backend); return; }
    }
    char b[200];
    if (rd("/sys/firmware/acpi/platform_profile_choices", b, sizeof b) && b[0]) {
        split_words(b, s->profs, &s->nprofs, MAXCH);
        rd("/sys/firmware/acpi/platform_profile", s->prof_cur, sizeof s->prof_cur);
        g_strlcpy(s->prof_backend, "ACPI platform_profile", sizeof s->prof_backend);
    }
}

/* ------------------------------------------------------------------ mode GPU hybride */
static void read_gpumode(HwState *s) {
    s->ngm = 0; s->gm_backend[0] = 0; s->gm_cur[0] = 0;
    if (has_cmd("supergfxctl")) {
        gchar *cur = cmd1("supergfxctl", "-g", NULL);
        if (cur && *cur) {
            g_strlcpy(s->gm_cur, cur, sizeof s->gm_cur);
            g_strlcpy(s->gm_backend, "supergfxctl", sizeof s->gm_backend);
            gchar *sup = cmd1("supergfxctl", "-s", NULL);
            const char *all[] = {"Integrated", "Hybrid", "AsusMuxDgpu", "Vfio", NULL};
            for (int i = 0; all[i] && s->ngm < MAXCH; i++)
                if ((sup && strstr(sup, all[i])) || !strcmp(all[i], "Integrated") || !strcmp(all[i], "Hybrid") || !strcmp(all[i], cur))
                    g_strlcpy(s->gms[s->ngm++], all[i], 24);
            g_free(sup);
        }
        g_free(cur);
        if (s->ngm) return;
    }
    if (has_cmd("envycontrol")) {
        gchar *cur = cmd1("envycontrol", "--query", NULL);
        if (cur && *cur) {
            g_strlcpy(s->gm_cur, cur, sizeof s->gm_cur); g_strlcpy(s->gm_backend, "envycontrol", sizeof s->gm_backend);
            g_strlcpy(s->gms[0], "integrated", 24); g_strlcpy(s->gms[1], "hybrid", 24); g_strlcpy(s->gms[2], "nvidia", 24); s->ngm = 3;
        }
        g_free(cur);
    }
}

/* ------------------------------------------------------------------ écran */
static int cmp_desc(const void *a, const void *b) { return *(const int *)b - *(const int *)a; }
static void add_mode(Disp *d, int hz) {
    for (int i = 0; i < d->nmodes; i++) if (d->modes[i] == hz) return;
    if (d->nmodes < 16) d->modes[d->nmodes++] = hz;
}

static gboolean read_display(Disp *d) {
    memset(d, 0, sizeof *d);
    if (has_cmd("hyprctl")) {
        gchar *o = cmd1("hyprctl", "monitors", "-j");
        if (o && *o) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, o, -1, NULL)) {
                JsonNode *root = json_parser_get_root(p);
                JsonArray *arr = root && JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : NULL;
                if (arr && json_array_get_length(arr)) {
                    JsonObject *m = json_array_get_object_element(arr, 0);
                    g_strlcpy(d->backend, "hyprland", sizeof d->backend);
                    g_strlcpy(d->name, json_object_get_string_member(m, "name"), sizeof d->name);
                    d->w = json_object_get_int_member(m, "width"); d->h = json_object_get_int_member(m, "height");
                    d->hz = (int)lround(json_object_get_double_member(m, "refreshRate"));
                    d->x = json_object_get_int_member(m, "x"); d->y = json_object_get_int_member(m, "y");
                    d->scale = json_object_get_double_member(m, "scale");
                    JsonArray *modes = json_object_get_array_member(m, "availableModes");
                    char pre[32]; g_snprintf(pre, sizeof pre, "%dx%d@", d->w, d->h);
                    for (guint i = 0; modes && i < json_array_get_length(modes); i++) {
                        const char *ms = json_array_get_string_element(modes, i);
                        if (g_str_has_prefix(ms, pre)) add_mode(d, (int)lround(g_ascii_strtod(ms + strlen(pre), NULL)));
                    }
                }
            }
            g_object_unref(p);
        }
        g_free(o);
        if (d->backend[0]) { qsort(d->modes, d->nmodes, sizeof(int), cmp_desc); return TRUE; }
    }
    if (has_cmd("xrandr") && g_getenv("DISPLAY")) {
        gchar *o = cmd1("xrandr", "--query", NULL);
        if (o) {
            gchar **ln = g_strsplit(o, "\n", -1);
            for (int i = 0; ln[i] && !d->backend[0]; i++) {
                char nm[40]; int w, h, x, y;
                char *c = strstr(ln[i], " connected");
                if (!c) continue;
                char *g = c + 10; if (!strncmp(g, " primary", 8)) g += 8;
                if (sscanf(g, " %dx%d+%d+%d", &w, &h, &x, &y) != 4) continue;
                sscanf(ln[i], "%39s", nm);
                g_strlcpy(d->backend, "xrandr", sizeof d->backend); g_strlcpy(d->name, nm, sizeof d->name);
                d->w = w; d->h = h; d->x = x; d->y = y; d->scale = 1;
                char want[32]; g_snprintf(want, sizeof want, "%dx%d", w, h);
                for (int j = i + 1; ln[j] && ln[j][0] == ' '; j++) {
                    gchar **t = g_strsplit(g_strstrip(ln[j]), " ", -1);
                    if (t[0] && !strcmp(t[0], want))
                        for (int k = 1; t[k]; k++) if (t[k][0]) { int hz = (int)lround(g_ascii_strtod(t[k], NULL)); if (hz) { add_mode(d, hz); if (strchr(t[k], '*')) d->hz = hz; } }
                    g_strfreev(t);
                }
            }
            g_strfreev(ln); g_free(o);
        }
        if (d->backend[0]) { qsort(d->modes, d->nmodes, sizeof(int), cmp_desc); return TRUE; }
    }
    if (has_cmd("wlr-randr")) {
        gchar *o = cmd1("wlr-randr", "--json", NULL);
        if (o) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, o, -1, NULL)) {
                JsonArray *arr = json_node_get_array(json_parser_get_root(p));
                for (guint i = 0; arr && i < json_array_get_length(arr) && !d->backend[0]; i++) {
                    JsonObject *out = json_array_get_object_element(arr, i);
                    if (!json_object_get_boolean_member(out, "enabled")) continue;
                    JsonArray *ms = json_object_get_array_member(out, "modes");
                    int cw = 0, ch = 0;
                    for (guint k = 0; ms && k < json_array_get_length(ms); k++) {
                        JsonObject *m = json_array_get_object_element(ms, k);
                        if (json_object_has_member(m, "current") && json_object_get_boolean_member(m, "current")) {
                            cw = json_object_get_int_member(m, "width"); ch = json_object_get_int_member(m, "height");
                            d->hz = (int)lround(json_object_get_double_member(m, "refresh"));
                        }
                    }
                    for (guint k = 0; ms && cw && k < json_array_get_length(ms); k++) {
                        JsonObject *m = json_array_get_object_element(ms, k);
                        if (json_object_get_int_member(m, "width") == cw && json_object_get_int_member(m, "height") == ch)
                            add_mode(d, (int)lround(json_object_get_double_member(m, "refresh")));
                    }
                    if (cw) { g_strlcpy(d->backend, "wlr-randr", sizeof d->backend); g_strlcpy(d->name, json_object_get_string_member(out, "name"), sizeof d->name); d->w = cw; d->h = ch; d->scale = 1; }
                }
            }
            g_object_unref(p); g_free(o);
        }
        if (d->backend[0]) { qsort(d->modes, d->nmodes, sizeof(int), cmp_desc); return TRUE; }
    }
    return FALSE;
}

static gboolean set_refresh(int hz, char *msg, size_t n) {
    Disp d;
    if (!read_display(&d) || !d.backend[0]) { g_strlcpy(msg, "Aucun gestionnaire d'écran détecté", n); return FALSE; }
    gboolean ok = FALSE;
    for (int i = 0; i < d.nmodes; i++) ok |= d.modes[i] == hz;
    if (!ok) { g_strlcpy(msg, "Fréquence indisponible", n); return FALSE; }
    char mode[48], arg[256];
    g_snprintf(mode, sizeof mode, "%dx%d", d.w, d.h);
    if (!strcmp(d.backend, "hyprland")) {
        char sc[16]; g_ascii_formatd(sc, sizeof sc, "%g", d.scale);
        g_snprintf(arg, sizeof arg, "hl.monitor({output=\"%s\",mode=\"%s@%d\",position=\"%dx%d\",scale=%s})", d.name, mode, hz, d.x, d.y, sc);
        char *a1[] = {"hyprctl", "eval", arg, NULL};
        gchar *o = run_out(a1, msg, n); g_free(o);
        g_usleep(400000);
        Disp d2; if (read_display(&d2) && d2.hz == hz) { msg[0] = 0; return TRUE; }
        char leg[256];
        g_snprintf(leg, sizeof leg, "%s,%s@%d,%dx%d,%s", d.name, mode, hz, d.x, d.y, sc);
        char *a2[] = {"hyprctl", "keyword", "monitor", leg, NULL};
        o = run_out(a2, msg, n); g_free(o);
        g_usleep(400000);
        if (read_display(&d2) && d2.hz == hz) { msg[0] = 0; return TRUE; }
        if (!msg[0]) g_strlcpy(msg, "Échec du changement de fréquence", n);
        return FALSE;
    }
    if (!strcmp(d.backend, "xrandr")) {
        char r[16]; g_snprintf(r, sizeof r, "%d", hz);
        char *a[] = {"xrandr", "--output", d.name, "--mode", mode, "--rate", r, NULL};
        return run_ok(a, msg, n);
    }
    g_snprintf(arg, sizeof arg, "%s@%dHz", mode, hz);
    char *a[] = {"wlr-randr", "--output", d.name, "--mode", arg, NULL};
    return run_ok(a, msg, n);
}

/* ------------------------------------------------------------------ luminosité / audio / veilleuse */
static int read_brightness(void) {
    if (!backlight_dir[0]) return -1;
    char p[220]; long mx, cur;
    g_snprintf(p, sizeof p, "%s/max_brightness", backlight_dir); mx = rdl(p, 0);
    g_snprintf(p, sizeof p, "%s/brightness", backlight_dir); cur = rdl(p, 0);
    return mx > 0 ? (int)lround(100.0 * cur / mx) : -1;
}

static gboolean set_brightness(int v, char *msg, size_t n) {
    if (!backlight_dir[0]) { g_strlcpy(msg, "Aucun rétroéclairage détecté", n); return FALSE; }
    v = CLAMP(v, 1, 100);
    if (has_cmd("brightnessctl")) {
        char pc[16]; g_snprintf(pc, sizeof pc, "%d%%", v);
        char *a[] = {"brightnessctl", "-q", "-d", strrchr(backlight_dir, '/') + 1, "set", pc, NULL};
        return run_ok(a, msg, n);
    }
    char p[220]; g_snprintf(p, sizeof p, "%s/max_brightness", backlight_dir);
    long mx = rdl(p, 100);
    g_snprintf(p, sizeof p, "%s/brightness", backlight_dir);
    FILE *f = fopen(p, "w");
    if (!f) { g_strlcpy(msg, "Installe brightnessctl pour régler la luminosité", n); return FALSE; }
    fprintf(f, "%ld", lround(mx * v / 100.0)); fclose(f);
    return TRUE;
}

static void read_audio(HwState *s) {
    s->audio_backend[0] = 0;
    if (has_cmd("pactl")) {
        gchar *v = cmd1("pactl", "get-sink-volume", "@DEFAULT_SINK@");
        if (v) {
            char *pc = strchr(v, '%');
            if (pc) {
                char *b = pc; while (b > v && g_ascii_isdigit(b[-1])) b--;
                s->volume = atoi(b);
                g_strlcpy(s->audio_backend, "pactl", sizeof s->audio_backend);
                gchar *m = cmd1("pactl", "get-sink-mute", "@DEFAULT_SINK@");
                s->muted = m && strstr(m, "yes"); g_free(m);
            }
            g_free(v);
            if (s->audio_backend[0]) return;
        }
    }
    if (has_cmd("wpctl")) {
        gchar *v = cmd1("wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@");
        if (v && strstr(v, "Volume:")) {
            s->volume = (int)lround(g_ascii_strtod(strstr(v, "Volume:") + 7, NULL) * 100);
            s->muted = strstr(v, "MUTED") != NULL;
            g_strlcpy(s->audio_backend, "wpctl", sizeof s->audio_backend);
        }
        g_free(v);
    }
}

static const char *night_tool(void) {
    const char *t[] = {"omarchy", "gammastep", "redshift", "wlsunset", NULL};
    for (int i = 0; t[i]; i++) if (has_cmd(t[i])) return t[i];
    return NULL;
}

static GPid wlsunset_pid;
static gboolean toggle_night(gboolean on, char *msg, size_t n) {
    const char *t = night_tool();
    if (!t) { g_strlcpy(msg, "Aucun outil de filtre lumière bleue trouvé", n); return FALSE; }
    gboolean ok = TRUE;
    if (!strcmp(t, "omarchy")) { char *a[] = {"omarchy", "toggle", "nightlight", NULL}; ok = run_ok(a, msg, n); }
    else if (!strcmp(t, "wlsunset")) {
        if (wlsunset_pid) { kill(wlsunset_pid, SIGTERM); g_spawn_close_pid(wlsunset_pid); wlsunset_pid = 0; }
        if (on) { char *a[] = {"wlsunset", "-T", "3501", "-t", "3500", NULL}; ok = g_spawn_async(NULL, a, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &wlsunset_pid, NULL); }
    } else { char *a[] = {(char *)t, on ? "-O" : "-x", "3500", NULL}; if (!on) a[2] = NULL; ok = run_ok(a, msg, n); }
    if (ok) g_atomic_int_set(&night_on, on);
    return ok;
}

/* ------------------------------------------------------------------ appareil */
static void read_identity(HwState *s) {
    const char *D = "/sys/devices/virtual/dmi/id/";
    char p[100], model[80] = "", vendor[48] = "";
    g_snprintf(p, sizeof p, "%sproduct_name", D); rd(p, model, sizeof model);
    const char *bad[] = {"", "System Product Name", "To Be Filled By O.E.M.", "Default string", "Type1ProductConfigId", NULL};
    for (int pass = 0; pass < 2; pass++) {
        gboolean isbad = FALSE; for (int i = 0; bad[i]; i++) isbad |= !strcmp(model, bad[i]);
        if (!isbad) break;
        if (pass == 0) { g_snprintf(p, sizeof p, "%sboard_name", D); rd(p, model, sizeof model); }
        else { rd("/proc/device-tree/model", model, sizeof model); if (!model[0]) g_strlcpy(model, "PC Linux", sizeof model); }
    }
    g_snprintf(p, sizeof p, "%ssys_vendor", D); rd(p, vendor, sizeof vendor);
    char *c = strchr(vendor, ','); if (c) *c = 0;
    if (g_str_has_prefix(vendor, "Micro-Star")) g_strlcpy(vendor, "MSI", sizeof vendor);
    else { char *i = strstr(vendor, " Inc."); if (i) *i = 0; i = strstr(vendor, " Co."); if (i) *i = 0; }
    g_strlcpy(s->model, model, sizeof s->model); g_strlcpy(s->vendor, vendor, sizeof s->vendor);
    struct utsname u; if (!uname(&u)) g_strlcpy(s->kernel, u.release, sizeof s->kernel);
    g_strlcpy(s->session, g_getenv("XDG_SESSION_TYPE") ? g_getenv("XDG_SESSION_TYPE") : "?", sizeof s->session);
    g_strlcpy(s->wm, g_getenv("XDG_CURRENT_DESKTOP") ? g_getenv("XDG_CURRENT_DESKTOP") : "?", sizeof s->wm);
    /* nom du CPU */
    gchar *ci = NULL;
    if (g_file_get_contents("/proc/cpuinfo", &ci, NULL, NULL)) {
        char *m = strstr(ci, "model name");
        if (!m) m = strstr(ci, "Hardware");
        if (m && (m = strchr(m, ':'))) { char *e = strchr(m, '\n'); if (e) *e = 0; g_strlcpy(s->cpu_name, g_strstrip(m + 1), sizeof s->cpu_name); }
        g_free(ci);
    }
    s->threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
}

/* ------------------------------------------------------------------ échantillonneur */
static void push(float *h, float v) { memmove(h, h + 1, (HIST - 1) * sizeof(float)); h[HIST - 1] = v; }

static gpointer sampler(gpointer unused) {
    (void)unused;
    int tick = 0;
    g_mutex_lock(&lock);
    while (running) {
        g_mutex_unlock(&lock);
        gboolean slow = (tick % 2 == 0);
        g_mutex_lock(&lock); if (force_slow) { slow = TRUE; force_slow = FALSE; } g_mutex_unlock(&lock);
        read_cpu(&W);
        read_cpu_tuning(&W);
        read_gpu(&W);
        read_mem_net_disk(&W, tick % 5 == 0);
        read_fans(&W);
        read_battery(&W);
        W.brightness = read_brightness();
        char up[64]; W.uptime = rd("/proc/uptime", up, sizeof up) ? atof(up) : 0;
        if (slow) {
            read_profiles(&W); read_gpumode(&W); read_display(&W.disp); read_audio(&W);
            W.night_supported = night_tool() != NULL;
        }
        if (tick % 5 == 0 && has_cmd("nmcli")) {
            W.ssid[0] = 0; W.signal = 0;
            char *a[] = {"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi", "list", "--rescan", "no", NULL};
            gchar *w = run_out(a, NULL, 0);
            if (w) {
                gchar **ln = g_strsplit(w, "\n", -1);
                for (int i = 0; ln[i]; i++) if (g_str_has_prefix(ln[i], "yes:")) {
                    char *last = strrchr(ln[i], ':');
                    if (last) { W.signal = atoi(last + 1); *last = 0; g_strlcpy(W.ssid, ln[i] + 4, sizeof W.ssid); }
                    break;
                }
                g_strfreev(ln); g_free(w);
            }
        }
        W.night_on = g_atomic_int_get(&night_on);
        push(W.h_cpu, W.cpu_usage); push(W.h_gpu, W.gpu_util); push(W.h_ram, W.ram_pct);
        push(W.h_ctemp, W.cpu_temp); push(W.h_gtemp, W.gpu_temp);
        g_mutex_lock(&lock);
        S = W; ready = TRUE;
        if (notify_cb) g_idle_add(notify_idle, NULL);
        tick++;
        gint64 until = g_get_monotonic_time() + G_USEC_PER_SEC;
        while (running && !force_slow && g_cond_wait_until(&wake, &lock, until)) {}
    }
    g_mutex_unlock(&lock);
    return NULL;
}

void hw_start(void) {
    discover();
    nvml_load();
    memset(&S, 0, sizeof S); memset(&W, 0, sizeof W);
    read_identity(&W);
    read_cpu(&W);                     /* amorce les compteurs delta */
    have_net = FALSE;
    running = TRUE;
    thr = g_thread_new("coreboard-sampler", sampler, NULL);
}

void hw_stop(void) {
    g_mutex_lock(&lock); running = FALSE; g_cond_broadcast(&wake); g_mutex_unlock(&lock);
    if (thr) g_thread_join(thr);
    thr = NULL;
    if (wlsunset_pid) kill(wlsunset_pid, SIGTERM);
}

void hw_snapshot(HwState *out) {
    g_mutex_lock(&lock); *out = S; g_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------ actions */
typedef struct { char key[24], value[48]; HwMessageCb cb; gpointer data; char msg[300]; gboolean ok; } Job;

static gboolean job_done(gpointer p) {
    Job *j = p;
    if (j->cb) j->cb(j->msg, j->ok, j->data);
    g_free(j);
    return G_SOURCE_REMOVE;
}

static gboolean do_apply(const char *key, const char *val, char *msg, size_t n) {
    HwState cur; hw_snapshot(&cur);
    msg[0] = 0;
    if (!strcmp(key, "profile")) {
        gboolean known = FALSE; for (int i = 0; i < cur.nprofs; i++) known |= !strcmp(cur.profs[i], val);
        if (!known) { g_strlcpy(msg, "Profil indisponible", n); return FALSE; }
        if (strstr(cur.prof_backend, "D-Bus") && sysbus && ppd_idx >= 0) {
            GError *e = NULL;
            GVariant *r = g_dbus_connection_call_sync(sysbus, PPD[ppd_idx][0], PPD[ppd_idx][1], "org.freedesktop.DBus.Properties", "Set",
                g_variant_new("(ssv)", PPD[ppd_idx][2], "ActiveProfile", g_variant_new_string(val)), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &e);
            if (r) g_variant_unref(r);
            if (e) { g_strlcpy(msg, e->message, n); g_error_free(e); return FALSE; }
            return TRUE;
        }
        char sc[120]; g_snprintf(sc, sizeof sc, "echo %s > /sys/firmware/acpi/platform_profile", val);
        return priv(sc, msg, n);
    }
    if (!strcmp(key, "refresh")) return set_refresh(atoi(val), msg, n);
    if (!strcmp(key, "brightness")) return set_brightness(atoi(val), msg, n);
    if (!strcmp(key, "volume")) {
        char v[16]; g_snprintf(v, sizeof v, "%d%%", CLAMP(atoi(val), 0, 150));
        if (has_cmd("pactl")) { char *a[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", v, NULL}; return run_ok(a, msg, n); }
        char *a[] = {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v, NULL}; return run_ok(a, msg, n);
    }
    if (!strcmp(key, "mute")) {
        const char *v = atoi(val) ? "1" : "0";
        if (has_cmd("pactl")) { char *a[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", (char *)v, NULL}; return run_ok(a, msg, n); }
        char *a[] = {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", (char *)v, NULL}; return run_ok(a, msg, n);
    }
    if (!strcmp(key, "gpu_mode")) {
        gboolean known = FALSE; for (int i = 0; i < cur.ngm; i++) known |= !strcmp(cur.gms[i], val);
        if (!known) { g_strlcpy(msg, "Mode indisponible", n); return FALSE; }
        if (!strcmp(cur.gm_backend, "supergfxctl")) { char *a[] = {"supergfxctl", "-m", (char *)val, NULL}; return run_ok(a, msg, n); }
        char sc[80]; g_snprintf(sc, sizeof sc, "envycontrol -s %s", val); return priv(sc, msg, n);
    }
    if (!strcmp(key, "nightlight")) return toggle_night(atoi(val), msg, n);
    if (!strcmp(key, "turbo")) {
        if (cur.turbo < 0) { g_strlcpy(msg, "Turbo non contrôlable sur ce CPU", n); return FALSE; }
        char sc[120];
        if (!access("/sys/devices/system/cpu/intel_pstate/no_turbo", R_OK))
            g_snprintf(sc, sizeof sc, "echo %d > /sys/devices/system/cpu/intel_pstate/no_turbo", atoi(val) ? 0 : 1);
        else g_snprintf(sc, sizeof sc, "echo %d > /sys/devices/system/cpu/cpufreq/boost", atoi(val) ? 1 : 0);
        return priv(sc, msg, n);
    }
    if (!strcmp(key, "epp") || !strcmp(key, "governor")) {
        gboolean epp = !strcmp(key, "epp"), known = FALSE;
        if (epp) for (int i = 0; i < cur.nepp; i++) known |= !strcmp(cur.epps[i], val);
        else for (int i = 0; i < cur.ngov; i++) known |= !strcmp(cur.governors[i], val);
        if (!known) { g_strlcpy(msg, "Valeur indisponible", n); return FALSE; }
        char sc[300];
        g_snprintf(sc, sizeof sc, "for f in /sys/devices/system/cpu/cpu*/cpufreq/%s; do echo %s > $f; done",
                   epp ? "energy_performance_preference" : "scaling_governor", val);
        return priv(sc, msg, n);
    }
    g_strlcpy(msg, "Réglage inconnu", n);
    return FALSE;
}

static gpointer job_thread(gpointer p) {
    Job *j = p;
    j->ok = do_apply(j->key, j->value, j->msg, sizeof j->msg);
    g_mutex_lock(&lock); force_slow = TRUE; g_cond_broadcast(&wake); g_mutex_unlock(&lock);
    g_idle_add(job_done, j);
    return NULL;
}

void hw_apply(const char *key, const char *value, HwMessageCb cb, gpointer data) {
    Job *j = g_new0(Job, 1);
    g_strlcpy(j->key, key, sizeof j->key); g_strlcpy(j->value, value, sizeof j->value);
    j->cb = cb; j->data = data;
    g_thread_unref(g_thread_new("coreboard-job", job_thread, j));
}

/* Identité seule (modèle, constructeur) sans démarrer la surveillance — utilisée par --set-image. */
void hw_identity(HwState *s) { read_identity(s); }
