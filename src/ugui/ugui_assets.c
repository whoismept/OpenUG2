/* ugui_assets.c -- run-time loader for the original frontend files (SPEC 14-17). */
#include "ugui_assets.h"
#include <stddef.h>
#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---- case-insensitive path resolution ---- */
static int dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* resolves "A/b/C" below `base` ignoring case; returns 1 and writes the real path */
static int resolve_ci(const char *base, const char *rel, char *out, size_t outlen) {
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", base[0] ? base : ".");
    char part[256];
    const char *p = rel;
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        if (n == 0) { p++; continue; }
        if (n >= sizeof part) return 0;
        memcpy(part, p, n);
        part[n] = 0;
        DIR *d = opendir(cur);
        if (!d) return 0;
        struct dirent *e;
        int found = 0;
        char real[256] = "";
        while ((e = readdir(d)) != NULL) {
            if (!strcasecmp(e->d_name, part)) {
                if (!found || !strcmp(e->d_name, part)) snprintf(real, sizeof real, "%s", e->d_name);
                found = 1;
            }
        }
        closedir(d);
        if (!found) return 0;
        size_t cl = strlen(cur);
        if (cl + 1 + strlen(real) + 1 > sizeof cur) return 0;
        cur[cl] = '/';
        strcpy(cur + cl + 1, real);
        p += n;
    }
    snprintf(out, outlen, "%s", cur);
    return 1;
}

const char *ug_assets_find_dir(const char *explicit_dir, char *out, size_t outlen) {
    const char *cands[4];
    int n = 0;
    if (explicit_dir && *explicit_dir) cands[n++] = explicit_dir;
    const char *e1 = getenv("UGUI_DATA"), *e2 = getenv("NFSU2_DATA");
    if (e1 && *e1) cands[n++] = e1;
    if (e2 && *e2) cands[n++] = e2;
    cands[n++] = ".";
    for (int i = 0; i < n; i++) {
        char t[1024];
        if (dir_exists(cands[i]) && (resolve_ci(cands[i], "FRONTEND", t, sizeof t) ||
                                      resolve_ci(cands[i], "GLOBAL", t, sizeof t))) {
            snprintf(out, outlen, "%s", cands[i]);
            return out;
        }
    }
    return NULL;
}

/* ---- state helpers ---- */
static void logf_(UgAssets *a, const char *fmt, const char *s1, int n1) {
    size_t l = strlen(a->log);
    if (l + 2 >= sizeof a->log) return;
    snprintf(a->log + l, sizeof a->log - l, fmt, s1 ? s1 : "", n1);
}

static int add_file(UgAssets *a, const char *rel_for_log, UgBuf *b) {
    if (a->nfiles >= 8) { ug_buf_free(b); return -1; }
    a->files[a->nfiles] = *b;
    snprintf(a->file_names[a->nfiles], sizeof a->file_names[0], "%s", rel_for_log);
    return a->nfiles++;
}

/* tries the candidates in order; loads (and decompresses) the first that exists */
static int load_first(UgAssets *a, const char *const *cands, int ncand, const char *label,
                      UgBuf *out, char *used, size_t usedlen) {
    for (int i = 0; i < ncand; i++) {
        char path[1100], err[160];
        if (!resolve_ci(a->dir, cands[i], path, sizeof path)) continue;
        if (ug_bundle_read(path, out, err, sizeof err)) {
            snprintf(used, usedlen, "%s", cands[i]);
            return 1;
        }
        logf_(a, "warning: %s: ", cands[i], 0);
        logf_(a, "%s\n", err, 0);
    }
    logf_(a, "note: %s not found\n", label, 0);
    return 0;
}

typedef struct { UgAssets *a; int file; int language_file; } WalkCtx;

static int add_package(UgAssets *a, UgPackageBlob *b, int file) {
    for (int i = 0; i < a->npk; i++)
        if (a->pk[i].key == b->key) { ug_package_free(b); return 0; }       /* first wins */
    if (a->npk == a->pk_cap) {
        int nc = a->pk_cap ? a->pk_cap * 2 : 192;
        UgAssetPackage *np = (UgAssetPackage *)realloc(a->pk, sizeof(UgAssetPackage) * (size_t)nc);
        if (!np) { ug_package_free(b); return 0; }
        a->pk = np; a->pk_cap = nc;
    }
    UgAssetPackage *p = &a->pk[a->npk++];
    p->key = b->key; p->name = b->name; p->data = b->data; p->len = b->len; p->source = file;
    b->name = NULL; b->data = NULL;
    return 1;
}

static int add_face(UgAssets *a, UgFace *f) {
    for (int i = 0; i < a->nface; i++)
        if (!strcmp(a->face[i].name, f->name)) { ug_face_free(f); return 0; }
    if (a->nface == a->face_cap) {
        int nc = a->face_cap ? a->face_cap * 2 : 16;
        UgFace *nf = (UgFace *)realloc(a->face, sizeof(UgFace) * (size_t)nc);
        if (!nf) { ug_face_free(f); return 0; }
        a->face = nf; a->face_cap = nc;
    }
    a->face[a->nface++] = *f;
    return 1;
}

static int visit(void *u, const UgChunkRef *c) {
    WalkCtx *w = (WalkCtx *)u;
    UgAssets *a = w->a;
    char err[128];
    switch (c->id) {
    case UG_CHUNK_PKG_HASHED: case UG_CHUNK_PKG_RAW: {
        UgPackageBlob b;
        if (ug_package_from_chunk(c, &b, err, sizeof err)) add_package(a, &b, w->file);
        else logf_(a, "warning: unreadable package chunk: %s\n", err, 0);
        return 1;
    }
    case 0xB3300000u: {
        int before = a->tex.n;
        if (!ug_texpack_parse(&a->tex, c, err, sizeof err)) logf_(a, "warning: texture pack: %s\n", err, 0);
        for(int i=before;i<a->tex.n;i++)snprintf(a->tex.t[i].source,sizeof a->tex.t[i].source,"%s/%s",a->dir,a->file_names[w->file]);
        if (w->file >= 0 && a->tex.n > before) {
            if (strstr(a->file_names[w->file], "ront")) a->n_front_tex += a->tex.n - before;
            else a->n_global_tex += a->tex.n - before;
        }
        return 1;
    }
    case UG_CHUNK_FONT: {
        UgFace f;
        if (ug_face_parse(&f, c->payload, c->size, err, sizeof err)) add_face(a, &f);
        else logf_(a, "warning: font: %s\n", err, 0);
        return 1;
    }
    case UG_CHUNK_STRINGS:
        /* Shared bundles may contain unrelated or empty tables. Only the
         * requested LANGUAGES file supplies the UI's localized labels. */
        if (w->language_file && !a->have_strings) {
            if (ug_strtab_load(&a->strings, c->hdr, (size_t)(c->payload - c->hdr) + c->size, err, sizeof err)) {
                a->have_strings = a->strings.n > 0;
                if (!a->have_strings)
                    logf_(a, "warning: empty language table in %s\n", a->file_names[w->file], 0);
            }
            else logf_(a, "warning: string table: %s\n", err, 0);
        }
        return 1;
    default: return 0;
    }
}

static void walk_file(UgAssets *a, UgBuf *b, const char *name, int language_file) {
    int fi = add_file(a, name, b);
    if (fi < 0) return;
    WalkCtx w = {a, fi, language_file};
    char err[160];
    if (!ug_chunks_walk(a->files[fi].data, a->files[fi].len, visit, &w, err, sizeof err))
        logf_(a, "warning: bundle walk stopped early: %s\n", err, 0);
}

static int cmp_pkg(const void *x, const void *y) {
    return strcasecmp(((const UgAssetPackage *)x)->name, ((const UgAssetPackage *)y)->name);
}

static int assets_open(UgAssets *a, const char *dir, const char *language, char *msg, size_t msglen, int ingame) {
    memset(a, 0, sizeof *a);
    a->main_face = -1;
    ug_texlib_init(&a->tex);
    ug_strtab_init(&a->strings);
    snprintf(a->dir, sizeof a->dir, "%s", dir && *dir ? dir : ".");
    snprintf(a->language, sizeof a->language, "%s", language && *language ? language : "English");
    if (msg && msglen) msg[0] = 0;

    char used[64], rel[96];
    UgBuf b;

    /* global bundle: original JDLZ file or the same bundle uncompressed (14.2) */
    static const char *GLOBAL_B[] = {"GLOBAL/GLOBALB.LZC.bacc", "GLOBAL/GlobalB.lzc", "GLOBAL/GLOBALB.BUN"};
    static const char *FRONT_B[] = {"FRONTEND/FRONTB.LZC.bacc", "FRONTEND/FrontB.lzc"};
    static const char *GLOBAL_A[] = {"GLOBAL/GLOBALA.BUN.bacc", "GLOBAL/GLOBALA.BUN"};
    if (load_first(a, FRONT_B, 2, "FRONTEND/FrontB.lzc", &b, used, sizeof used)) walk_file(a, &b, used, 0);
    if (load_first(a, GLOBAL_B, 3, "GLOBAL/GlobalB.lzc", &b, used, sizeof used)) walk_file(a, &b, used, 0);
    if (load_first(a, GLOBAL_A, 2, "GLOBAL/GLOBALA.BUN", &b, used, sizeof used)) walk_file(a, &b, used, 0);

    snprintf(rel, sizeof rel, "LANGUAGES/%s.bin", a->language);
    char rel2[110];
    snprintf(rel2, sizeof rel2, "%s.bacc", rel);
    const char *LANG[] = {rel2, rel};
    if (load_first(a, LANG, 2, rel, &b, used, sizeof used)) walk_file(a, &b, used, 1);
    if (!a->have_strings) logf_(a, "warning: requested language table unavailable: %s\n", rel, 0);
    static const char *LT[] = {"LANGUAGES/LanguageTextures.bin"};
    if (load_first(a, LT, 1, "LANGUAGES/LanguageTextures.bin", &b, used, sizeof used)) walk_file(a, &b, used, 0);

    if (ingame) {
        static const char *COMMON[] = {"GLOBAL/InGameCommon.lzc", "GLOBAL/InGameCommon.bun"};
        if (load_first(a, COMMON, 2, "GLOBAL/InGameCommon", &b, used, sizeof used)) walk_file(a, &b, used, 0);
    }

    ug_texlib_index(&a->tex);
    for (int i = 0; i < a->nface; i++) {
        UgFace *f = &a->face[i];
        int ti = ug_texlib_find(&a->tex, ug_hash_upper(f->atlas_name));
        f->tex = ti >= 0 ? ti + 1 : 0;
        if (ti >= 0 && f->atlas_w <= 0) {      /* 0x16E fonts carry no texture block: use the pack's size */
            const uint8_t *px = ug_texlib_pixels(&a->tex, ti);
            if (px) { f->atlas_w = a->tex.t[ti].w; f->atlas_h = a->tex.t[ti].h; }
        }
        if (!strcmp(f->name, "conduitmditc_tt21i")) a->main_face = i;
    }
    if (a->npk > 1) qsort(a->pk, (size_t)a->npk, sizeof(UgAssetPackage), cmp_pkg);

    char sum[200];
    snprintf(sum, sizeof sum, "%d screens, %d textures, %d fonts, %d strings (%s)",
             a->npk, a->tex.n, a->nface, a->strings.n, a->language);
    if (a->npk == 0) {
        if (msg && msglen) snprintf(msg, msglen, "no screen packages found under %s (%s)", a->dir, sum);
        return 0;
    }
    if (msg && msglen) snprintf(msg, msglen, "%s", sum);
    return 1;
}

int ug_assets_open(UgAssets *a, const char *dir, const char *language, char *msg, size_t msglen) {
    return assets_open(a, dir, language, msg, msglen, 0);
}

int ug_assets_open_ingame(UgAssets *a, const char *dir, const char *language, char *msg, size_t msglen) {
    return assets_open(a, dir, language, msg, msglen, 1);
}

void ug_assets_close(UgAssets *a) {
    for (int i = 0; i < a->npk; i++) { free(a->pk[i].name); free(a->pk[i].data); }
    free(a->pk);
    for (int i = 0; i < a->nface; i++) ug_face_free(&a->face[i]);
    free(a->face);
    ug_texlib_free(&a->tex);
    ug_strtab_free(&a->strings);
    for (int i = 0; i < a->nfiles; i++) ug_buf_free(&a->files[i]);
    memset(a, 0, sizeof *a);
}

const UgAssetPackage *ug_assets_package_by_key(const UgAssets *a, uint32_t key) {
    for (int i = 0; i < a->npk; i++) if (a->pk[i].key == key) return &a->pk[i];
    return NULL;
}
const UgAssetPackage *ug_assets_package(const UgAssets *a, const char *name) {
    return ug_assets_package_by_key(a, ug_hash_upper(name));
}
int ug_assets_package_count(const UgAssets *a) { return a->npk; }
const UgAssetPackage *ug_assets_package_at(const UgAssets *a, int i) {
    return (i >= 0 && i < a->npk) ? &a->pk[i] : NULL;
}

const UgFace *ug_assets_main_face(const UgAssets *a) {
    return a->main_face >= 0 ? &a->face[a->main_face] : NULL;
}

const UgFace *ug_assets_face(const UgAssets *a, const char *res_name) {
    char nm[64];
    size_t n = 0;
    const char *base = res_name;
    for (const char *p = res_name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    for (; base[n] && base[n] != '.' && n + 1 < sizeof nm; n++) nm[n] = (char)tolower((unsigned char)base[n]);
    nm[n] = 0;
    for (int i = 0; i < a->nface; i++)
        if (!strcmp(a->face[i].name, nm) && a->face[i].tex > 0) return &a->face[i];
    return NULL; /* missing requested font is unavailable, never another font */
}

int ug_assets_texture_for(const UgAssets *a, const char *res_name) {
    int i=ug_texlib_find(&a->tex,ug_texture_key(res_name));
    return i>=0 && ug_texlib_pixels((UgTexLib *)&a->tex,i) ? i : -1;
}
int ug_assets_texture_by_name(const UgAssets *a, const char *base_name) {
    int i=ug_texlib_find(&a->tex,ug_hash_upper(base_name));
    return i>=0 && ug_texlib_pixels((UgTexLib *)&a->tex,i) ? i : -1;
}

int ug_skin_resolve(UgSkin *sk, const UgAssets *a) {
    memset(sk, 0, sizeof *sk);
    static const struct { size_t off; const char *name; } ROLE[] = {
        {offsetof(UgSkin, box_b), "U2_Menu_Box"}, {offsetof(UgSkin, box_w), "U2_Menu_Box_White"},
        {offsetof(UgSkin, corner_b), "U2_Menu_RoundedCorner"}, {offsetof(UgSkin, corner_w), "U2_Menu_Corner_white"},
        {offsetof(UgSkin, cap), "U2_Menu_Text_Corner"},
        {offsetof(UgSkin, arrow), "U2_Menu_Arrow"}, {offsetof(UgSkin, arrow_w), "U2_Menu_Arrow_White"},
        {offsetof(UgSkin, arrow_hl), "U2_Menu_Arrow_highlight"},
        {offsetof(UgSkin, scr_tab), "U2_Scroller_Tab"}, {offsetof(UgSkin, scr_dot), "U2_Scroller_Corner"},
        {offsetof(UgSkin, icon_hl), "U2_Icon_Highlight"}, {offsetof(UgSkin, icon_back), "U2_CarSelect_icon_backing"},
        {offsetof(UgSkin, slider_bg), "U2_temp_sliderBG"}, {offsetof(UgSkin, slider_fill_corner), "U2_temp_slider_fill_corner"},
        {offsetof(UgSkin, slider_knob), "U2_SliderBar_Tab"}, {offsetof(UgSkin, white), "White16x16_pure_white"},
        {offsetof(UgSkin, pill), "UI_PC_Generic_Button"}, {offsetof(UgSkin, pill_glow), "UI_PC_Generic_Button_Glow"},
        {offsetof(UgSkin, save), "U2_Save_Icon"},
        {offsetof(UgSkin, cursor), UG_CURSOR_TEXTURE},        /* 4.6: key hash("PC_CURSOR") in the PC global pack */
    };
    int n = 0;
    for (size_t i = 0; i < sizeof ROLE / sizeof ROLE[0]; i++) {
        int t = ug_assets_texture_by_name(a, ROLE[i].name);
        if (t >= 0) { *(int *)((char *)sk + ROLE[i].off) = t + 1; n++; }
    }
    /* The slider backing is a soft bar that need not be vertically centred in its texture: find the centroid of its
     * alpha so that fills and knobs can be centred on the bar rather than on the quad. */
    if (sk->slider_bg) {
        const UgTexture *t = &a->tex.t[sk->slider_bg - 1];
        const uint8_t *px = ug_texlib_pixels((UgTexLib *)&a->tex, sk->slider_bg - 1);
        if (px && t->w > 0 && t->h > 0) {
            double sum = 0, wy = 0;
            for (int y = 0; y < t->h; y++) {
                double row = 0;
                for (int x = 0; x < t->w; x++) row += px[((size_t)y * (size_t)t->w + (size_t)x) * 4 + 3];
                sum += row; wy += row * ((double)y + 0.5);
            }
            if (sum > 0) sk->slider_bg_dy = (float)((0.5 * t->h - wy / sum) / t->h);
        }
    }
    return n;
}

int ug_assets_textures_with_prefix(const UgAssets *a, const char *prefix, int *out, int cap) {
    size_t pl = strlen(prefix);
    int n = 0;
    for (int i = 0; i < a->tex.n && n < cap; i++)
        if (!strncasecmp(a->tex.t[i].name, prefix, pl)) out[n++] = i + 1;
    /* insertion sort by stored name */
    for (int i = 1; i < n; i++) {
        int v = out[i], j = i - 1;
        while (j >= 0 && strcasecmp(a->tex.t[out[j] - 1].name, a->tex.t[v - 1].name) > 0) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}
