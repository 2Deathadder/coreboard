/* Coreboard — analyseur PE (lecture des en-têtes, tables d'imports, recherche de chaînes, fichiers voisins) */
#include "pe.h"
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RD16(b, o) ((guint16)((b)[o] | ((b)[(o) + 1] << 8)))
#define RD32(b, o) ((guint32)((b)[o] | ((b)[(o) + 1] << 8) | ((b)[(o) + 2] << 16) | ((guint32)(b)[(o) + 3] << 24)))

typedef struct { guint32 va, vsize, raw, rsize; } Sec;

static gboolean rd(int fd, gint64 off, void *buf, size_t n) { return pread(fd, buf, n, off) == (ssize_t)n; }

static gboolean rva2off(const Sec *s, int ns, guint32 rva, gint64 *off) {
    for (int i = 0; i < ns; i++) {
        guint32 span = MAX(s[i].vsize, s[i].rsize);
        if (rva >= s[i].va && rva < s[i].va + span) { *off = (gint64)s[i].raw + (rva - s[i].va); return TRUE; }
    }
    return FALSE;
}

static void read_cstr(int fd, gint64 off, char *out, size_t n) {
    ssize_t r = pread(fd, out, n - 1, off);
    if (r < 0) r = 0;
    out[r] = 0;
    for (size_t i = 0; out[i]; i++) if (!isprint((unsigned char)out[i])) { out[i] = 0; break; }
}

static void add_import(PeInfo *p, const char *dll) {
    char l[40]; size_t i = 0;
    for (; dll[i] && i < sizeof l - 1; i++) l[i] = g_ascii_tolower(dll[i]);
    l[i] = 0;
    if (!l[0]) return;
    if (p->nimports < 16) g_strlcpy(p->imports[p->nimports++], l, sizeof p->imports[0]);
    if (strstr(l, "d3d9")) p->dx9 = TRUE;
    if (strstr(l, "d3d10")) p->dx10 = TRUE;
    if (strstr(l, "d3d11")) p->dx11 = TRUE;
    if (strstr(l, "d3d12")) p->dx12 = TRUE;
    if (strstr(l, "vulkan-1")) p->vulkan = TRUE;
    if (strstr(l, "opengl32")) p->opengl = TRUE;
    if (strstr(l, "nvapi")) p->nvapi = TRUE;
    if (strstr(l, "nvngx") || strstr(l, "dlss")) p->dlss = TRUE;
    if (strstr(l, "xinput")) p->xinput = TRUE;
    if (strstr(l, "steam_api")) p->steam_api = TRUE;
    if (strstr(l, "eossdk")) p->eos = TRUE;
}

/* les jeux (Unreal Engine surtout) chargent souvent Direct3D à l'exécution : on cherche aussi les noms dans le fichier */
static void scan_strings(int fd, gint64 size, PeInfo *p) {
    static const struct { const char *needle; int id; } N[] = {
        {"d3d12.dll", 12}, {"d3d11.dll", 11}, {"d3d10", 10}, {"d3d9.dll", 9}, {"vulkan-1.dll", 20}, {"nvapi64.dll", 30}, {"nvngx_dlss", 31}, {"nvngx.dll", 31},
        {"easyanticheat", 40}, {"battleye", 41}, {"vgk.sys", 42}, {"riot vanguard", 42}, {"ricochet", 43}, {"punkbuster", 44}, {"xigncode", 45},
    };
    const gsize CH = 4 << 20; guchar *buf = g_malloc(CH + 64);
    gint64 limit = MIN(size, (gint64)400 << 20), off = 0;
    while (off < limit) {
        ssize_t r = pread(fd, buf, CH + 32, off);
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) buf[i] = g_ascii_tolower(buf[i]);
        for (guint k = 0; k < G_N_ELEMENTS(N); k++) {
            if (!memmem(buf, r, N[k].needle, strlen(N[k].needle))) continue;
            switch (N[k].id) {
            case 12: p->dx12 = TRUE; break; case 11: p->dx11 = TRUE; break; case 10: p->dx10 = TRUE; break; case 9: p->dx9 = TRUE; break;
            case 20: p->vulkan = TRUE; break; case 30: p->nvapi = TRUE; break; case 31: p->dlss = TRUE; break;
            case 40: if (!p->ac_soft[0]) g_strlcpy(p->ac_soft, "EasyAntiCheat", sizeof p->ac_soft); break;
            case 41: if (!p->ac_soft[0]) g_strlcpy(p->ac_soft, "BattlEye", sizeof p->ac_soft); break;
            case 42: g_strlcpy(p->ac_kernel, "Riot Vanguard", sizeof p->ac_kernel); break;
            case 43: g_strlcpy(p->ac_kernel, "Ricochet", sizeof p->ac_kernel); break;
            case 44: if (!p->ac_soft[0]) g_strlcpy(p->ac_soft, "PunkBuster", sizeof p->ac_soft); break;
            default: if (!p->ac_soft[0]) g_strlcpy(p->ac_soft, "XIGNCODE3", sizeof p->ac_soft);
            }
        }
        if (r <= (ssize_t)CH) break;
        off += CH;                                                   /* recouvrement de 32 octets entre deux blocs */
    }
    g_free(buf);
}

static gboolean has(const char *dir, const char *name) {
    gchar *p = g_build_filename(dir, name, NULL); gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS); g_free(p); return ok;
}

/* fichiers du jeu autour de l'exécutable (jusqu'à 4 niveaux au-dessus) : anti-triche, moteur, DRM */
static void scan_neighbours(const char *exe, PeInfo *p) {
    gchar *dir = g_path_get_dirname(exe);
    for (int lvl = 0; lvl < 5 && dir && strlen(dir) > 1; lvl++) {
        if (has(dir, "EasyAntiCheat") || has(dir, "EasyAntiCheat_EOS") || has(dir, "EasyAntiCheat_Setup.exe")) g_strlcpy(p->ac_soft, "EasyAntiCheat", sizeof p->ac_soft);
        if (has(dir, "BattlEye") || has(dir, "BEService.exe") || has(dir, "BEService_x64.exe")) g_strlcpy(p->ac_soft, "BattlEye", sizeof p->ac_soft);
        if (has(dir, "vgc.exe") || has(dir, "vgk.sys") || has(dir, "Riot Vanguard")) g_strlcpy(p->ac_kernel, "Riot Vanguard", sizeof p->ac_kernel);
        if (has(dir, "Ricochet") || has(dir, "cod.exe") || has(dir, "ModernWarfare.exe")) { if (has(dir, "Ricochet") || has(dir, "cod.exe")) g_strlcpy(p->ac_kernel, "Ricochet", sizeof p->ac_kernel); }
        if (has(dir, "faceit.exe") || has(dir, "FACEITClient")) g_strlcpy(p->ac_kernel, "FACEIT AC", sizeof p->ac_kernel);
        if (!p->engine[0]) {
            if (has(dir, "UnityPlayer.dll") || has(dir, "UnityCrashHandler64.exe")) g_strlcpy(p->engine, "Unity", sizeof p->engine);
            else if (has(dir, "Engine") && (has(dir, "Binaries") || has(dir, "Content"))) g_strlcpy(p->engine, "Unreal Engine", sizeof p->engine);
            else if (has(dir, "CrySystem.dll") || has(dir, "Bin64")) g_strlcpy(p->engine, "CryEngine", sizeof p->engine);
        }
        if (has(dir, "steam_api64.dll") || has(dir, "steam_api.dll")) p->steam_api = TRUE;
        if (has(dir, "EOSSDK-Win64-Shipping.dll")) p->eos = TRUE;
        gchar *up = g_path_get_dirname(dir); g_free(dir); dir = up;
    }
    g_free(dir);
    if (!p->engine[0] && strstr(exe, "/Binaries/Win64/")) g_strlcpy(p->engine, "Unreal Engine", sizeof p->engine);
    if (!p->engine[0]) { gchar *l = g_ascii_strdown(exe, -1); if (strstr(l, "godot")) g_strlcpy(p->engine, "Godot", sizeof p->engine); g_free(l); }
}

/* version du fichier (ressource VS_VERSION_INFO) : « 310.2.1.0 » pour nvngx_dlss.dll */
gboolean pe_file_version(const char *path, char *out, size_t n) {
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    guchar dos[64], hdr[24 + 240];
    if (!rd(fd, 0, dos, 64) || dos[0] != 'M' || dos[1] != 'Z') { close(fd); return FALSE; }
    guint32 pe = RD32(dos, 0x3c);
    if (!rd(fd, pe, hdr, sizeof hdr) || memcmp(hdr, "PE\0\0", 4)) { close(fd); return FALSE; }
    int nsec = MIN(RD16(hdr, 6), 96), optsz = RD16(hdr, 20);
    gboolean is64 = RD16(hdr, 24) == 0x20b;
    int dd_off = 24 + (is64 ? 112 : 96);
    guint32 res_rva = RD32(hdr, dd_off + 2 * 8);
    Sec secs[96]; guchar sh[40 * 96];
    if (!res_rva || nsec <= 0 || !rd(fd, (gint64)pe + 24 + optsz, sh, 40 * nsec)) { close(fd); return FALSE; }
    for (int i = 0; i < nsec; i++) { secs[i].vsize = RD32(sh, i * 40 + 8); secs[i].va = RD32(sh, i * 40 + 12); secs[i].rsize = RD32(sh, i * 40 + 16); secs[i].raw = RD32(sh, i * 40 + 20); }
    gint64 base;
    if (!rva2off(secs, nsec, res_rva, &base)) { close(fd); return FALSE; }
    /* arbre des ressources : racine (type 16 = RT_VERSION) → nom → langue → entrée de données */
    guint32 dir_off = 0; gboolean found = FALSE;
    for (int level = 0; level < 3; level++) {
        guchar d[16];
        if (!rd(fd, base + dir_off, d, 16)) { close(fd); return FALSE; }
        int cnt = RD16(d, 12) + RD16(d, 14);
        guchar e[8]; guint32 next = 0; gboolean ok = FALSE;
        for (int i = 0; i < cnt && i < 64; i++) {
            if (!rd(fd, base + dir_off + 16 + i * 8, e, 8)) break;
            guint32 id = RD32(e, 0), off = RD32(e, 4);
            if (level == 0 && id != 16) continue;
            next = off; ok = TRUE; break;
        }
        if (!ok) { close(fd); return FALSE; }
        if (level < 2) { if (!(next & 0x80000000u)) { close(fd); return FALSE; } dir_off = next & 0x7fffffffu; }
        else { dir_off = next; found = TRUE; }
    }
    if (!found) { close(fd); return FALSE; }
    guchar de[16];
    if (!rd(fd, base + dir_off, de, 16)) { close(fd); return FALSE; }
    guint32 drva = RD32(de, 0), dsize = MIN(RD32(de, 4), 2048u); gint64 doff;
    if (!rva2off(secs, nsec, drva, &doff)) { close(fd); return FALSE; }
    guchar *buf = g_malloc(dsize + 1); gboolean ok = FALSE;
    if (pread(fd, buf, dsize, doff) == (ssize_t)dsize) {
        for (guint32 i = 0; i + 16 <= dsize; i++)
            if (buf[i] == 0xBD && buf[i + 1] == 0x04 && buf[i + 2] == 0xEF && buf[i + 3] == 0xFE) {
                guint32 ms = RD32(buf, i + 8), ls = RD32(buf, i + 12);
                g_snprintf(out, n, "%u.%u.%u.%u", ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff); ok = TRUE; break;
            }
    }
    g_free(buf); close(fd);
    return ok;
}

gboolean pe_analyze(const char *path, PeInfo *p) {
    memset(p, 0, sizeof *p);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    struct stat sb; fstat(fd, &sb);
    guchar dos[64];
    if (!rd(fd, 0, dos, 64) || dos[0] != 'M' || dos[1] != 'Z') { close(fd); return FALSE; }
    guint32 pe = RD32(dos, 0x3c);
    guchar hdr[24 + 240];
    if (pe > (guint32)sb.st_size || !rd(fd, pe, hdr, sizeof hdr) || memcmp(hdr, "PE\0\0", 4)) { close(fd); return FALSE; }
    p->machine = RD16(hdr, 4);
    int nsec = RD16(hdr, 6), optsz = RD16(hdr, 20);
    guint16 magic = RD16(hdr, 24);
    if (magic != 0x10b && magic != 0x20b) { close(fd); return FALSE; }
    p->is64 = magic == 0x20b;
    int sub = RD16(hdr, 24 + 68);
    p->console = sub == 3;
    int dd_off = 24 + (p->is64 ? 112 : 96), ndd = RD32(hdr, 24 + (p->is64 ? 108 : 92));
    if (nsec > 96) nsec = 96;
    Sec secs[96]; guchar sh[40 * 96];
    if (nsec > 0 && rd(fd, (gint64)pe + 24 + optsz, sh, 40 * nsec))
        for (int i = 0; i < nsec; i++) { secs[i].vsize = RD32(sh, i * 40 + 8); secs[i].va = RD32(sh, i * 40 + 12); secs[i].rsize = RD32(sh, i * 40 + 16); secs[i].raw = RD32(sh, i * 40 + 20); }
    else nsec = 0;
    p->valid = TRUE;
    if (ndd > 14 && dd_off + 15 * 8 <= (int)sizeof hdr) {
        if (RD32(hdr, dd_off + 14 * 8) != 0) p->dotnet = TRUE;                                     /* en-tête CLR */
        for (int pass = 0; pass < 2; pass++) {                                                       /* imports, puis imports différés */
            guint32 rva = RD32(hdr, dd_off + (pass ? 13 : 1) * 8); gint64 off;
            if (!rva || !rva2off(secs, nsec, rva, &off)) continue;
            guint step = pass ? 32 : 20, name_at = pass ? 4 : 12;
            for (int i = 0; i < 300; i++) {
                guchar d[32];
                if (!rd(fd, off + (gint64)i * step, d, step)) break;
                guint32 nrva = RD32(d, name_at); gint64 noff;
                if (!nrva) break;
                if (!rva2off(secs, nsec, nrva, &noff)) continue;
                char dll[64]; read_cstr(fd, noff, dll, sizeof dll);
                add_import(p, dll);
            }
        }
    }
    scan_strings(fd, sb.st_size, p);
    close(fd);
    scan_neighbours(path, p);
    return TRUE;
}

void pe_summary(const PeInfo *p, char *buf, size_t n) {
    GString *s = g_string_new(NULL);
    g_string_append(s, p->machine == 0xaa64 ? "ARM64" : p->is64 ? "64 bits" : "32 bits");
    if (p->dotnet) g_string_append(s, " · .NET");
    if (p->dx12) g_string_append(s, " · DirectX 12"); else if (p->dx11) g_string_append(s, " · DirectX 11"); else if (p->dx10) g_string_append(s, " · DirectX 10"); else if (p->dx9) g_string_append(s, " · DirectX 9");
    if (p->vulkan) g_string_append(s, " · Vulkan");
    if (p->engine[0]) g_string_append_printf(s, " · %s", p->engine);
    if (p->dlss) g_string_append(s, " · DLSS"); else if (p->nvapi) g_string_append(s, " · NVAPI");
    if (p->console) g_string_append(s, " · console");
    g_strlcpy(buf, s->str, n); g_string_free(s, TRUE);
}

void pe_advice(const PeInfo *p, char *buf, size_t n) {
    buf[0] = 0;
    if (p->machine == 0xaa64) g_strlcpy(buf, "Exécutable ARM64 : ne fonctionnera pas sur ce processeur x86_64.", n);
    else if (p->ac_kernel[0]) g_snprintf(buf, n, "Anti-triche %s (noyau Windows) : ne fonctionne pas sous Linux, risque de blocage ou de bannissement.", p->ac_kernel);
    else if (p->ac_soft[0]) g_snprintf(buf, n, "%s détecté : ne marche sous Proton que si l'éditeur l'a activé pour Linux (à vérifier).", p->ac_soft);
    else if (p->dotnet) g_strlcpy(buf, "Application .NET : Proton fournit Mono ; si le jeu exige .NET Framework, installe-le dans le préfixe.", n);
}
