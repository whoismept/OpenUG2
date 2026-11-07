/* tex_override.c — optional HD texture overrides (see tex_override.h).
 * Original code for OpenUG2 (MIT). DDS is a public Microsoft container format;
 * only the documented header fields are read. Override files are untrusted
 * input: every read is bounded by the file size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "tex_override.h"


/* render.c: sampler + anisotropy tracking for an uploaded override */
void render_tex_finish(RhiTexture id, int mipped);

enum { TO_DXT1 = 1, TO_DXT3 = 3, TO_DXT5 = 5, TO_BGRA = 10, TO_RGBA = 11 };
#define TO_MAX_LEVELS 16
#define TO_PACK 32

typedef struct {
    uint32_t key;
    char pack[TO_PACK];        /* "" = global */
    char *path;
    int w, h, fmt, alpha;      /* alpha: format can carry non-opaque texels */
    int levels;                /* whole levels present in the file */
    long off[TO_MAX_LEVELS], size[TO_MAX_LEVELS];
    int state;                 /* 0 untried, 1 uploaded at least once, 2 rejected */
    int noted;                 /* bit0: pack-mismatch logged, bit1: budget logged */
} TexOvr;

/* Live override uploads, so the budget measures resident bytes rather than a
   session-cumulative total: world cache eviction and car switches delete GL
   names behind our back, and the next upload prunes them with rhi_texture_is_live. A
   recycled name still counts as live -- that can only over-estimate usage. */
typedef struct { RhiTexture id; long long bytes; } TexLive;
static TexLive *g_live; static int g_nlive, g_caplive;

static TexOvr *g_ovr; static int g_novr, g_capovr;
static int    g_enabled;
static long long g_budget, g_used;
static int    g_maxsize = 4096;
static int    g_nused, g_nskip, g_nreduced;
static int    g_force_cpu;   /* OPENUG2_TEX_OVERRIDES_CPU=1: test the no-S3TC path */

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int is_pot(int v) { return v > 0 && (v & (v - 1)) == 0; }
static long level_bytes(int fmt, int w, int h) {
    if (fmt == TO_BGRA || fmt == TO_RGBA) return (long)w * h * 4;
    long bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    return bw * bh * (fmt == TO_DXT1 ? 8 : 16);
}

/* Parse the header of one DDS file into `o` (path/key/pack already set).
   Returns 1 if usable, else 0 with a reason in *why. */
static int dds_parse(TexOvr *o, const char **why) {
    FILE *f = fopen(o->path, "rb");
    if (!f) { *why = "cannot open"; return 0; }
    unsigned char hd[148]; size_t n = fread(hd, 1, sizeof hd, f);
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fclose(f);
    if (n < 128) { *why = "truncated header (shorter than 128 bytes)"; return 0; }
    if (memcmp(hd, "DDS ", 4) || rd32(hd + 4) != 124) { *why = "not a DDS file (bad magic/header size)"; return 0; }
    uint32_t flags = rd32(hd + 8);
    int h = (int)rd32(hd + 12), w = (int)rd32(hd + 16);
    uint32_t mips = (flags & 0x20000) ? rd32(hd + 28) : 1;
    uint32_t pff = rd32(hd + 80), fourcc = rd32(hd + 84), bits = rd32(hd + 88);
    uint32_t rm = rd32(hd + 92), gm = rd32(hd + 96), bm = rd32(hd + 100), am = rd32(hd + 104);
    uint32_t caps2 = rd32(hd + 112);
    long data = 128;
    if (caps2 & 0x200) { *why = "cube map"; return 0; }
    if (caps2 & 0x200000) { *why = "volume texture"; return 0; }
    o->alpha = 1;
    if (pff & 4) {
        if (fourcc == 0x31545844u) { o->fmt = TO_DXT1; }          /* "DXT1" */
        else if (fourcc == 0x33545844u || fourcc == 0x32545844u) o->fmt = TO_DXT3; /* DXT3/DXT2 */
        else if (fourcc == 0x35545844u || fourcc == 0x34545844u) o->fmt = TO_DXT5; /* DXT5/DXT4 */
        else if (fourcc == 0x30315844u) {                           /* "DX10" */
            if (n < 148) { *why = "truncated DX10 header"; return 0; }
            uint32_t dxgi = rd32(hd + 128), dim = rd32(hd + 132), arr = rd32(hd + 140);
            data = 148;
            if (dim != 3 || arr > 1) { *why = "DX10: not a single 2D texture"; return 0; }
            switch (dxgi) {
            case 71: case 72: o->fmt = TO_DXT1; break;
            case 74: case 75: o->fmt = TO_DXT3; break;
            case 77: case 78: o->fmt = TO_DXT5; break;
            case 28: case 29: o->fmt = TO_RGBA; break;
            case 87: case 91: o->fmt = TO_BGRA; break;
            case 88: case 93: o->fmt = TO_BGRA; o->alpha = 0; break;
            case 98: case 99: *why = "BC7 not supported yet"; return 0;
            default: *why = "unsupported DXGI format"; return 0;
            }
        } else { *why = "unsupported FourCC"; return 0; }
    } else if ((pff & 0x40) && bits == 32) {
        if (rm == 0x00FF0000u && gm == 0xFF00u && bm == 0xFFu) o->fmt = TO_BGRA;
        else if (rm == 0xFFu && gm == 0xFF00u && bm == 0x00FF0000u) o->fmt = TO_RGBA;
        else { *why = "unsupported RGB channel masks"; return 0; }
        o->alpha = (pff & 1) && am == 0xFF000000u;
    } else { *why = "unsupported pixel format"; return 0; }
    if (!is_pot(w) || !is_pot(h)) { *why = "not power of two"; return 0; }
    if (w > g_maxsize || h > g_maxsize) { *why = "larger than the device texture limit/4096"; return 0; }
    if (mips < 1) mips = 1;
    if (mips > TO_MAX_LEVELS) mips = TO_MAX_LEVELS;
    o->w = w; o->h = h; o->levels = 0;
    long off = data; int lw = w, lh = h;
    for (uint32_t l = 0; l < mips; l++) {
        long sz = level_bytes(o->fmt, lw, lh);
        if (off + sz > fsz) break;                /* keep only whole levels */
        o->off[l] = off; o->size[l] = sz; o->levels++;
        off += sz;
        if (lw == 1 && lh == 1) break;
        if (lw > 1) lw /= 2;
        if (lh > 1) lh /= 2;
    }
    if (!o->levels) { *why = "file shorter than its base level"; return 0; }
    return 1;
}

/* KEY8HEX[@PACK][_suffix].dds -> key, pack. */
/* TRACKMAP4144.dds: minimaps by their readable record name. The key is the same
   bin hash the record carries (case-folded, start ~0, h*33+c). */
static int parse_map_name(const char *fn, uint32_t *key, char *pack) {
    if (strncasecmp(fn, "TRACKMAP", 8)) return 0;
    int n = 8;
    while (fn[n] >= '0' && fn[n] <= '9') n++;
    if (n == 8 || (fn[n] != '_' && fn[n] != '.')) return 0;
    uint32_t h = UINT32_MAX;
    for (int i = 0; i < n; i++) h = 33 * h + (unsigned char)toupper((unsigned char)fn[i]);
    *key = h;
    pack[0] = 0;
    return 1;
}

static int parse_name(const char *fn, uint32_t *key, char *pack) {
    size_t L = strlen(fn);
    if (L < 12 || strcasecmp(fn + L - 4, ".dds")) return 0;
    if (parse_map_name(fn, key, pack)) return 1;
    uint32_t k = 0;
    for (int i = 0; i < 8; i++) {
        int c = (unsigned char)fn[i], v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return 0;
        k = k << 4 | (uint32_t)v;
    }
    const char *p = fn + 8;
    pack[0] = 0;
    if (*p == '@') {
        p++; int n = 0;
        while (*p && *p != '_' && *p != '.' && n < TO_PACK - 1)
            pack[n++] = (char)toupper((unsigned char)*p++);
        pack[n] = 0;
    }
    if (*p != '_' && *p != '.') return 0;
    *key = k;
    return 1;
}

static void scan_dir(const char *dir, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t pl = strlen(dir) + strlen(e->d_name) + 2;
        char *path = (char *)malloc(pl); if (!path) break;
        snprintf(path, pl, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) { free(path); continue; }
        if (S_ISDIR(st.st_mode)) { scan_dir(path, depth + 1); free(path); continue; }
        TexOvr o; memset(&o, 0, sizeof o);
        if (!S_ISREG(st.st_mode) || !parse_name(e->d_name, &o.key, o.pack)) { free(path); continue; }
        o.path = path;
        const char *why = "";
        if (!dds_parse(&o, &why)) {
            printf("TEXOVR skip %s: %s\n", path, why); free(path); continue;
        }
        if (g_novr == g_capovr) {
            int cap = g_capovr ? g_capovr * 2 : 64;
            TexOvr *t = (TexOvr *)realloc(g_ovr, (size_t)cap * sizeof *t);
            if (!t) { free(path); break; }
            g_ovr = t; g_capovr = cap;
        }
        g_ovr[g_novr++] = o;
    }
    closedir(d);
}

static int ovr_cmp(const void *a, const void *b) {
    const TexOvr *x = (const TexOvr *)a, *y = (const TexOvr *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    int c = strcmp(x->pack, y->pack);
    return c ? c : strcmp(x->path, y->path);
}

int tex_override_init(const char *dir, int budget_mib) {
    tex_override_shutdown();
    if (!dir || !*dir) return 0;
    const char *env = getenv("OPENUG2_TEX_OVERRIDES");
    if (env && !strcmp(env, "0")) { printf("TEXOVR disabled by OPENUG2_TEX_OVERRIDES=0\n"); return 0; }
    const char *cpu = getenv("OPENUG2_TEX_OVERRIDES_CPU");
    g_force_cpu = cpu && !strcmp(cpu, "1");
    int mx = rhi_caps()->max_texture_size;
    g_maxsize = (mx > 0 && mx < 4096) ? mx : 4096;
    g_budget = (long long)(budget_mib > 0 ? budget_mib : 1536) << 20;
    scan_dir(dir, 0);
    if (g_novr) qsort(g_ovr, (size_t)g_novr, sizeof *g_ovr, ovr_cmp);
    /* exact duplicates (same key + pack): first path in sort order wins */
    int out = 0;
    for (int i = 0; i < g_novr; i++) {
        if (out && g_ovr[out-1].key == g_ovr[i].key && !strcmp(g_ovr[out-1].pack, g_ovr[i].pack)) {
            printf("TEXOVR duplicate %08X%s%s ignored: %s\n", g_ovr[i].key,
                   g_ovr[i].pack[0] ? "@" : "", g_ovr[i].pack, g_ovr[i].path);
            free(g_ovr[i].path); continue;
        }
        g_ovr[out++] = g_ovr[i];
    }
    g_novr = out;
    g_enabled = g_novr > 0;
    printf("TEXOVR %d override(s) indexed from %s (%s upload, budget %lld MiB, max %d)\n",
           g_novr, dir, g_tex_s3tc && !g_force_cpu ? "S3TC" : "CPU-decoded", g_budget >> 20, g_maxsize);
    return g_novr;
}

/* KEY@PACK for the caller's pack beats plain KEY; a rejected entry is skipped
   so a bad pack-specific file still lets the global one apply. Pack-bound
   files for a different pack are reported once and never applied. */
static TexOvr *find(uint32_t key, const char *pack) {
    int lo = 0, hi = g_novr;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_ovr[m].key < key) lo = m + 1; else hi = m; }
    TexOvr *global = NULL, *hit = NULL;
    for (int i = lo; i < g_novr && g_ovr[i].key == key; i++) {
        TexOvr *o = &g_ovr[i];
        if (o->state == 2) continue;
        if (!o->pack[0]) { if (!global) global = o; }
        else if (pack && !strcasecmp(o->pack, pack)) { if (!hit) hit = o; }
        else if (!(o->noted & 1)) {
            o->noted |= 1;
            printf("TEXOVR not applied %08X@%s (%s): texture requested from pack %s\n",
                   o->key, o->pack, o->path, pack ? pack : "(unknown/global)");
        }
    }
    return hit ? hit : global;
}

static long long live_bytes(void) {
    long long sum = 0; int out = 0;
    for (int i = 0; i < g_nlive; i++) {
        if (!rhi_texture_is_live(g_live[i].id)) continue;      /* deleted since upload */
        g_live[out++] = g_live[i]; sum += g_live[i].bytes;
    }
    g_nlive = out;
    return sum;
}
static void live_add(RhiTexture id, long long bytes) {
    if (g_nlive == g_caplive) {
        int cap = g_caplive ? g_caplive * 2 : 64;
        TexLive *t = (TexLive *)realloc(g_live, (size_t)cap * sizeof *t);
        if (!t) return;                                 /* untracked: under-counts */
        g_live = t; g_caplive = cap;
    }
    g_live[g_nlive].id = id; g_live[g_nlive].bytes = bytes; g_nlive++;
}

static void reject(TexOvr *o, const char *why) {
    printf("TEXOVR skip %08X%s%s (%s): %s\n", o->key, o->pack[0] ? "@" : "", o->pack, o->path, why);
    o->state = 2; g_nskip++;
}

/* RGBA8 from one level of the file buffer. alpha forced opaque if !keep. */
static unsigned char *decode_level(const TexOvr *o, const unsigned char *src, int w, int h, int keep) {
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px) return NULL;
    if (o->fmt == TO_BGRA || o->fmt == TO_RGBA) {
        for (long p = 0; p < (long)w * h; p++) {
            const unsigned char *s = src + p * 4;
            px[p*4+0] = o->fmt == TO_BGRA ? s[2] : s[0];
            px[p*4+1] = s[1];
            px[p*4+2] = o->fmt == TO_BGRA ? s[0] : s[2];
            px[p*4+3] = keep ? s[3] : 255;
        }
        return px;
    }
    unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
    unsigned char *al = (unsigned char *)malloc((size_t)w * h);
    if (!rgb || !al) { free(rgb); free(al); free(px); return NULL; }
    if (o->fmt == TO_DXT1) n2_dxt1(src, w, h, rgb, al);
    else if (o->fmt == TO_DXT3) n2_dxt3(src, w, h, rgb, al);
    else n2_dxt5(src, w, h, rgb, al);
    for (long p = 0; p < (long)w * h; p++) {
        px[p*4] = rgb[p*3]; px[p*4+1] = rgb[p*3+1]; px[p*4+2] = rgb[p*3+2];
        px[p*4+3] = keep ? al[p] : 255;
    }
    free(rgb); free(al);
    return px;
}

RhiTexture tex_override_upload(uint32_t key, const char *pack, const N2Tex *orig) {
    if (!g_enabled || !orig) return (RhiTexture){0};
    TexOvr *o = find(key, pack);
    if (!o) return (RhiTexture){0};
    if (orig->w <= 0 || orig->h <= 0) return (RhiTexture){0};
    /* scale k in {0,1,2,3}, identical on both axes */
    int k = -1;
    for (int s = 0; s <= 3; s++)
        if (o->w == orig->w << s && o->h == orig->h << s) { k = s; break; }
    if (k < 0) {
        char why[96]; snprintf(why, sizeof why, "size %dx%d is not 1x/2x/4x/8x of original %dx%d",
                               o->w, o->h, orig->w, orig->h);
        reject(o, why); return (RhiTexture){0};
    }
    int needs_alpha = orig->alpha != NULL || orig->usage || orig->blend;
    int smooth_alpha = orig->blend || orig->usage >= 2;
    if (needs_alpha && !o->alpha) { reject(o, "original has alpha, override format has none"); return (RhiTexture){0}; }
    if (smooth_alpha && o->fmt == TO_DXT1) { reject(o, "blended original needs DXT3/DXT5/RGBA, not DXT1"); return (RhiTexture){0}; }

    int compressed = o->fmt <= TO_DXT5 && g_tex_s3tc && !g_force_cpu;
    /* GPU bytes of the levels [b, levels): CPU-decoded DXTn expands to RGBA8 */
    int mul = compressed || o->fmt > TO_DXT5 ? 1 : (o->fmt == TO_DXT1 ? 8 : 4);
    /* budget (resident bytes): when over, start at the level equal to the
       original size; when even that does not fit, use the original. Not
       sticky: memory freed later lets the next request through. */
    int base = 0;
    long long bytes = 0;
    for (int l = 0; l < o->levels; l++) bytes += o->size[l] * mul;
    g_used = live_bytes();
    if (g_used + bytes > g_budget) {
        long long red = -1;
        if (k > 0 && k < o->levels) {
            red = 0;
            for (int l = k; l < o->levels; l++) red += o->size[l] * mul;
        }
        if (red < 0 || g_used + red > g_budget) {
            if (!(o->noted & 2)) {
                o->noted |= 2;
                printf("TEXOVR skip %08X%s%s (%s): over memory budget (%.1f of %lld MiB resident), using original\n",
                       o->key, o->pack[0] ? "@" : "", o->pack, o->path,
                       (double)g_used / 1048576.0, g_budget >> 20);
                g_nskip++;
            }
            return (RhiTexture){0};
        }
        base = k; bytes = red;
        g_nreduced++;
    }

    long end = o->off[o->levels-1] + o->size[o->levels-1];
    FILE *f = fopen(o->path, "rb");
    if (!f) { reject(o, "cannot open"); return (RhiTexture){0}; }
    unsigned char *buf = (unsigned char *)malloc((size_t)end);
    size_t got = buf ? fread(buf, 1, (size_t)end, f) : 0;
    fclose(f);
    if (!buf || got != (size_t)end) { free(buf); reject(o, "short read"); return (RhiTexture){0}; }

    int lw = o->w >> base, lh = o->h >> base;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    int nlev = o->levels - base;
    int last_w = lw, last_h = lh;
    for (int l = 1; l < nlev; l++) { if (last_w > 1) last_w /= 2; if (last_h > 1) last_h /= 2; }
    int complete = last_w == 1 && last_h == 1;
    int keep_alpha = needs_alpha;

    RhiFormat format = !compressed ? RHI_FORMAT_RGBA8
                     : o->fmt == TO_DXT1 ? RHI_FORMAT_BC1
                     : o->fmt == TO_DXT3 ? RHI_FORMAT_BC2 : RHI_FORMAT_BC3;
    RhiTexture id = rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D, format, lw, lh, RHI_TEXTURE_SAMPLED});
    if (!id.id) { free(buf); reject(o, "GL upload error"); return (RhiTexture){0}; }
    int ok = 1, gpu_ok = 1;
    int uplevels = complete ? nlev : 1;
    for (int l = 0; l < uplevels && ok && gpu_ok; l++) {
        const unsigned char *src = buf + o->off[base + l];
        if (compressed) {
            if (rhi_texture_upload(id, 0, l, src, (size_t)o->size[base + l]) != RHI_OK) gpu_ok = 0;
        } else {
            unsigned char *px = decode_level(o, src, lw, lh, keep_alpha);
            if (!px) { ok = 0; break; }
            if (rhi_texture_upload(id, 0, l, px, 0) != RHI_OK) gpu_ok = 0;
            free(px);
        }
        if (lw > 1) lw /= 2;
        if (lh > 1) lh /= 2;
    }
    free(buf);
    /* A partial chain is re-mipped on the RGBA path; a compressed base cannot
       be re-mipped portably, so it stays base-only with LINEAR filtering. */
    if (ok && gpu_ok && !complete && !compressed && rhi_texture_generate_mips(id) != RHI_OK) gpu_ok = 0;
    if (!ok || !gpu_ok) {
        rhi_texture_destroy(id);
        reject(o, ok ? "GL upload error" : "out of memory");
        return (RhiTexture){0};
    }
    int mipped = complete || !compressed;
    render_tex_finish(id, mipped);   /* REPEAT; trilinear + anisotropy when mipped */
    live_add(id, bytes); g_used += bytes;
    if (!o->state) {
        printf("TEXOVR use %08X%s%s %dx%d (orig %dx%d, x%d%s) %s\n", key,
               pack ? " pack " : "", pack ? pack : "", o->w, o->h, orig->w, orig->h, 1 << k,
               base ? ", budget-reduced" : "", o->path);
        g_nused++;
    }
    o->state = 1;
    return id;
}

void tex_override_stats(void) {
    if (!g_enabled) return;
    printf("TEXOVR stats: %d indexed, %d used, %d skipped, %d budget-reduced, %.1f MiB resident\n",
           g_novr, g_nused, g_nskip, g_nreduced, (double)live_bytes() / (1024.0 * 1024.0));
}

void tex_override_shutdown(void) {
    for (int i = 0; i < g_novr; i++) free(g_ovr[i].path);
    free(g_ovr); g_ovr = NULL; g_novr = g_capovr = 0;
    free(g_live); g_live = NULL; g_nlive = g_caplive = 0;   /* GL names stay with their owners */
    g_enabled = 0; g_used = 0; g_nused = g_nskip = g_nreduced = 0;
}
