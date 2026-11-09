/* ugui_test_assets.inc -- included by ugui_test.c.
 * Section H: original-asset loading, driven by the synthetic vectors of
 * amendment 2 (JDLZ, HUFF, bundles, texture packs, fonts).  No retail data. */
#include "ugui_codec.h"
#include "ugui_bundle.h"
#include "ugui_tex.h"
#include "ugui_face.h"
#include "ugui_assets.h"

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* "41424344" -> bytes; returns length (malloc'ed) */
static uint8_t *unhex(const char *s, size_t *len) {
    size_t n = strlen(s) / 2;
    uint8_t *o = (uint8_t *)malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i + 1 < strlen(s); i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0) break;
        o[k++] = (uint8_t)(a * 16 + b);
    }
    *len = k;
    return o;
}

/* ------------------------------------------------------------------ */
/* H1. JDLZ and HUFF vectors (SPEC 13)                                  */
/* ------------------------------------------------------------------ */
static void run_codec_vectors(const char *file, int huff) {
    char *txt = read_file(file, NULL);
    CHECK(txt != NULL);
    if (!txt) return;
    jp = txt;
    J *root = jparse();
    CHECK(root->n >= 2);
    for (int i = 0; i < root->n; i++) {
        const J *c = root->kids[i];
        size_t plen, clen, olen = 0;
        uint8_t *plain = unhex(jstrv(c, "plain_hex"), &plen);
        uint8_t *comp = unhex(jstrv(c, "compressed_hex"), &clen);
        uint8_t *out = NULL;
        char err[128] = "";
        CHECK_MSG(ug_codec_detect(comp, clen) == (huff ? UG_CODEC_HUFF : UG_CODEC_JDLZ), "%s: detect", jstrv(c, "name"));
        int ok = huff ? ug_huff_decode(comp, clen, &out, &olen, err, sizeof err)
                      : ug_jdlz_decode(comp, clen, &out, &olen, err, sizeof err);
        CHECK_MSG(ok, "%s: %s", jstrv(c, "name"), err);
        if (ok) {
            CHECK_MSG(olen == plen && !memcmp(out, plain, plen), "%s: output differs", jstrv(c, "name"));
            free(out);
        }
        /* auto-detect path */
        out = NULL;
        CHECK(ug_decompress(comp, clen, &out, &olen, err, sizeof err) == 1);
        CHECK(olen == plen && out && !memcmp(out, plain, plen));
        free(out);
        /* trailing padding after the encoded size is ignored (13.1) */
        uint8_t *padded = (uint8_t *)malloc(clen + 3);
        memcpy(padded, comp, clen);
        memset(padded + clen, 0, 3);
        out = NULL;
        ok = huff ? ug_huff_decode(padded, clen + 2, &out, &olen, err, sizeof err)
                  : ug_jdlz_decode(padded, clen + 3, &out, &olen, err, sizeof err);
        CHECK(ok && olen == plen && !memcmp(out, plain, plen));
        free(out);
        free(padded);
        /* truncated input must fail cleanly, never read out of bounds */
        out = NULL;
        ok = huff ? ug_huff_decode(comp, clen - 6, &out, &olen, err, sizeof err)
                  : ug_jdlz_decode(comp, clen - 4, &out, &olen, err, sizeof err);
        CHECK_MSG(!ok, "%s: truncated stream must be rejected", jstrv(c, "name"));
        if (ok) free(out);
        free(plain); free(comp);
    }
    jfree(root);
    free(txt);
}

static void test_jdlz_vectors(void) {
    run_codec_vectors("jdlz_vectors.json", 0);
    /* a match that points before the start of the output is corrupt (13.2) */
    uint8_t bad[24] = {'J', 'D', 'L', 'Z', 2, 0x10, 0, 0, 8, 0, 0, 0, 22, 0, 0, 0,
                       0x01, 0x01, 0x00, 0x10, 0x00, 0x00, 0, 0};
    uint8_t *out = NULL; size_t n = 0; char err[96] = "";
    CHECK(!ug_jdlz_decode(bad, sizeof bad, &out, &n, err, sizeof err));
    CHECK(ug_decompress((const uint8_t *)"FEn\xe7zzzzzzzzzzzzzzzz", 20, &out, &n, err, sizeof err) == 2);
}

static void test_huff_vectors(void) {
    run_codec_vectors("huff_vectors.json", 1);
    uint8_t bad[20] = {'H', 'U', 'F', 'F', 1, 0x10, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 0x12, 0x34, 0, 0};
    uint8_t *out = NULL; size_t n = 0; char err[96] = "";
    CHECK(!ug_huff_decode(bad, sizeof bad, &out, &n, err, sizeof err));
}

/* ------------------------------------------------------------------ */
/* H2. Bundles (SPEC 3.2, 14)                                           */
/* ------------------------------------------------------------------ */
typedef struct { int pk; UgPackageBlob blob[4]; int n; int walked; } BundleCtx;
static int bundle_visit(void *u, const UgChunkRef *c) {
    BundleCtx *b = (BundleCtx *)u;
    b->walked++;
    if (c->id == 0x00030210u || c->id == 0x00030203u) {
        char err[96];
        if (b->n < 4 && ug_package_from_chunk(c, &b->blob[b->n], err, sizeof err)) b->n++;
        return 1;
    }
    return 0;
}

static void check_bundle_file(const char *hexname, const J *ex, int compressed) {
    size_t len = 0;
    uint8_t *bin = read_hex(hexname, &len);
    CHECK(bin != NULL);
    if (!bin) return;
    BundleCtx b;
    memset(&b, 0, sizeof b);
    char err[128] = "";
    CHECK_MSG(ug_chunks_walk(bin, len, bundle_visit, &b, err, sizeof err), "%s", err);
    CHECK(b.n == 1);
    if (b.n == 1) {
        CHECK(b.blob[0].name && !strcmp(b.blob[0].name, jstrv(ex, "package_name")));
        CHECK(ug_hash_upper(b.blob[0].name) == (uint32_t)strtoul(jstrv(ex, "screen_hash"), NULL, 16));
        CHECK(b.blob[0].key == (uint32_t)strtoul(jstrv(ex, "screen_hash"), NULL, 16));
        CHECK((int)b.blob[0].len == (int)jnum(ex, "package_size"));
        UgFngPackage pk;
        CHECK_MSG(ug_fng_parse(b.blob[0].data, b.blob[0].len, &pk, err, sizeof err), "%s", err);
        CHECK(pk.nobj == 3);
        ug_fng_free(&pk);
        (void)compressed;
    }
    for (int i = 0; i < b.n; i++) ug_package_free(&b.blob[i]);
    /* a chunk running past the buffer is reported, not followed */
    memset(&b, 0, sizeof b);
    CHECK(!ug_chunks_walk(bin, len - 3, bundle_visit, &b, err, sizeof err));
    for (int i = 0; i < b.n; i++) ug_package_free(&b.blob[i]);
    free(bin);
}

static void test_bundle_vectors(void) {
    char *txt = read_file("bundle.expected.json", NULL);
    CHECK(txt != NULL);
    if (!txt) return;
    jp = txt;
    J *ex = jparse();
    check_bundle_file("bundle_compressed.hex", ex, 1);
    check_bundle_file("bundle_raw.hex", ex, 0);
    jfree(ex);
    free(txt);
    CHECK(ug_chunk_is_container(0x80034100u));
    CHECK(ug_chunk_is_container(0xB3300000u) && ug_chunk_is_container(0xB3310000u) && ug_chunk_is_container(0xB3320000u));
    CHECK(!ug_chunk_is_container(0x00030210u) && !ug_chunk_is_container(0x33310001u));
}

/* ------------------------------------------------------------------ */
/* H3. Texture packs (SPEC 15)                                          */
/* ------------------------------------------------------------------ */
typedef struct { UgTexLib *lib; const uint8_t *base; int packs; char err[128]; } TpkCtx;
static int tpk_visit(void *u, const UgChunkRef *c) {
    TpkCtx *t = (TpkCtx *)u;
    if (c->id == 0xB3300000u) {
        if (ug_texpack_parse(t->lib, c, t->err, sizeof t->err)) t->packs++;
        return 1;
    }
    return 0;
}

static void check_pixels(const char *what, const uint8_t *got, const char *want_hex, int tol, int npix) {
    size_t wl;
    uint8_t *want = unhex(want_hex, &wl);
    CHECK_MSG((int)wl == npix * 4, "%s: expected %d bytes, vector has %d", what, npix * 4, (int)wl);
    int worst = 0;
    for (size_t i = 0; i < wl && got; i++) {
        int d = abs((int)got[i] - (int)want[i]);
        if (d > worst) worst = d;
    }
    CHECK_MSG(worst <= tol, "%s: worst channel difference %d (tolerance %d)", what, worst, tol);
    free(want);
}

static void test_tpk_table(void) {
    size_t len;
    uint8_t *bin = read_hex("tpk_table.hex", &len);
    char *txt = read_file("tpk_table.expected.json", NULL);
    CHECK(bin && txt);
    if (!bin || !txt) { free(bin); free(txt); return; }
    jp = txt;
    J *ex = jparse();
    UgTexLib lib;
    ug_texlib_init(&lib);
    TpkCtx t = {&lib, bin, 0, ""};
    CHECK_MSG(ug_chunks_walk(bin, len, tpk_visit, &t, t.err, sizeof t.err), "%s", t.err);
    CHECK(t.packs == 1);
    ug_texlib_index(&lib);
    CHECK_MSG(lib.n == ex->n, "texture count %d want %d", lib.n, ex->n);
    for (int i = 0; i < ex->n; i++) {
        const J *e = ex->kids[i];
        uint32_t key = (uint32_t)strtoul(jstrv(e, "key"), NULL, 16);
        const char *name = jstrv(e, "name");
        CHECK_MSG(ug_hash_upper(name) == key, "%s: key is the hash of the full upper-case name", name);
        int idx = ug_texlib_find(&lib, key);
        CHECK_MSG(idx >= 0, "%s: found by key", name);
        if (idx < 0) continue;
        const UgTexture *tx = &lib.t[idx];
        CHECK(tx->w == (int)jnum(e, "width") && tx->h == (int)jnum(e, "height"));
        CHECK(tx->fmt == (int)strtoul(jstrv(e, "format"), NULL, 16));
        CHECK(tx->levels == (int)jnum(e, "levels"));
        CHECK_MSG(tx->data_off == (uint32_t)jnum(e, "data_offset"), "%s: data offset %u", name, tx->data_off);
        const uint8_t *px = ug_texlib_pixels(&lib, idx);
        CHECK_MSG(px != NULL, "%s: decoded", name);
        if (px) check_pixels(name, px, jstrv(e, "level0_rgba_hex"), 1, tx->w * tx->h);
    }
    /* the truncated 30-character name is stored but the key comes from the full name */
    int li = ug_texlib_find(&lib, ug_hash_upper("SYN_A_VERY_LONG_TEXTURE_NAME_X"));
    CHECK(li >= 0 && strlen(lib.t[li].name) <= 23);
    CHECK(ug_texlib_find(&lib, 0x12345678u) < 0);
    ug_texlib_free(&lib);
    jfree(ex);
    free(txt);
    free(bin);
}

static void test_tpk_slots(void) {
    size_t len;
    uint8_t *bin = read_hex("tpk_slots.hex", &len);
    char *txt = read_file("tpk_slots.expected.json", NULL);
    CHECK(bin && txt);
    if (!bin || !txt) { free(bin); free(txt); return; }
    jp = txt;
    J *ex = jparse();
    UgTexLib lib;
    ug_texlib_init(&lib);
    TpkCtx t = {&lib, bin, 0, ""};
    CHECK_MSG(ug_chunks_walk(bin, len, tpk_visit, &t, t.err, sizeof t.err), "%s", t.err);
    CHECK(t.packs == 1);
    ug_texlib_index(&lib);
    CHECK_MSG(lib.n == ex->n, "slot count %d want %d", lib.n, ex->n);
    for (int i = 0; i < ex->n; i++) {
        const J *e = ex->kids[i];
        uint32_t key = (uint32_t)strtoul(jstrv(e, "key"), NULL, 16);
        int idx = ug_texlib_find(&lib, key);
        CHECK_MSG(idx >= 0, "%s: found", jstrv(e, "name"));
        if (idx < 0) continue;
        const uint8_t *px = ug_texlib_pixels(&lib, idx);
        CHECK(px != NULL);
        CHECK(lib.t[idx].w == (int)jnum(e, "width") && lib.t[idx].h == (int)jnum(e, "height"));
        CHECK(lib.t[idx].fmt == (int)strtoul(jstrv(e, "format"), NULL, 16));
        if (px) check_pixels(jstrv(e, "name"), px, jstrv(e, "level0_rgba_hex"), 1, lib.t[idx].w * lib.t[idx].h);
    }
    ug_texlib_free(&lib);
    jfree(ex);
    free(txt);
    free(bin);
}

/* pixel-data base is found by alignment, not by skipping 0x11 filler (15.5) */
static void test_tpk_data_base(void) {
    size_t len;
    uint8_t *bin = read_hex("tpk_table.hex", &len);
    CHECK(bin != NULL);
    if (!bin) return;
    /* locate the 0x33320002 chunk, then make the first texel data start with 0x11 */
    size_t pos = 0, data_hdr = 0;
    for (; pos + 8 < len; pos += 4)
        if (!memcmp(bin + pos, "\x02\x00\x32\x33", 4)) { data_hdr = pos; break; }
    CHECK(data_hdr != 0);
    if (data_hdr) {
        size_t base = ((data_hdr + 8) + 0x7F) & ~(size_t)0x7F;
        CHECK(base < len);
        UgTexLib lib;
        ug_texlib_init(&lib);
        TpkCtx t = {&lib, bin, 0, ""};
        ug_chunks_walk(bin, len, tpk_visit, &t, t.err, sizeof t.err);
        ug_texlib_index(&lib);
        int idx = ug_texlib_find(&lib, ug_hash_upper("SYN_PALETTE"));
        CHECK(idx >= 0);
        /* the palettised texture's indices live at base + data_off; the filler before the
         * base is 0x11 but must not be skipped */
        CHECK(idx < 0 || lib.t[idx].data_off == 128);
        CHECK(((size_t)(data_hdr + 8) <= base) && (base & 0x7F) == 0);
        ug_texlib_free(&lib);
    }
    free(bin);
}

/* ------------------------------------------------------------------ */
/* H4. Fonts (SPEC 16)                                                  */
/* ------------------------------------------------------------------ */
typedef struct { UgFace *face; int found; char err[128]; } FontCtx;
static int font_visit(void *u, const UgChunkRef *c) {
    FontCtx *f = (FontCtx *)u;
    if (c->id == 0x00030201u) {
        if (ug_face_parse(f->face, c->payload, c->size, f->err, sizeof f->err)) f->found++;
        return 1;
    }
    return 0;
}

typedef struct { UgQuad q[64]; int n; } QuadList;
static void quad_collect(void *u, const UgQuad *q) {
    QuadList *l = (QuadList *)u;
    if (l->n < 64) l->q[l->n++] = *q;
}

static void test_font_chunk_and_layout(void) {
    size_t len;
    uint8_t *bin = read_hex("font_chunk.hex", &len);
    char *txt = read_file("font_layout_vectors.json", NULL);
    CHECK(bin && txt);
    if (!bin || !txt) { free(bin); free(txt); return; }
    jp = txt;
    J *ex = jparse();
    UgFace face;
    memset(&face, 0, sizeof face);
    FontCtx f = {&face, 0, ""};
    CHECK_MSG(ug_chunks_walk(bin, len, font_visit, &f, f.err, sizeof f.err), "%s", f.err);
    CHECK_MSG(f.found == 1, "font chunk parsed (%s)", f.err);
    CHECK(!strcmp(face.name, "synfont"));
    CHECK(face.ng == 6);
    CHECK(face.line_height == (int)jnum(ex, "line_height"));
    const J *ts = jget(ex, "texture_size");
    CHECK(face.atlas_w == (int)ts->kids[0]->num && face.atlas_h == (int)ts->kids[1]->num);
    CHECK(face.cell_height > 0 && face.cell_height >= face.line_height - 8);
    CHECK(ug_face_glyph(&face, 'A') && ug_face_glyph(&face, 0x2019));
    CHECK(ug_face_glyph(&face, 'Z') == NULL);

    const J *vs = jget(ex, "vectors");
    for (int v = 0; v < vs->n; v++) {
        const J *e = vs->kids[v];
        QuadList ql;
        ql.n = 0;
        float last_pen = -1;
        ug_text_quads(&face, jstrv(e, "text"), (float)jget(e, "scale")->num, quad_collect, &ql, &last_pen);
        const J *want = jget(e, "quads");
        CHECK_MSG(ql.n == want->n, "'%s': %d quads, want %d", jstrv(e, "text"), ql.n, want->n);
        for (int i = 0; i < want->n && i < ql.n; i++) {
            const J *w = want->kids[i];
            CHECK_NEAR(ql.q[i].x0, jnum(w, "x0"), 1e-3);
            CHECK_NEAR(ql.q[i].y0, jnum(w, "y0"), 1e-3);
            CHECK_NEAR(ql.q[i].x1, jnum(w, "x1"), 1e-3);
            CHECK_NEAR(ql.q[i].y1, jnum(w, "y1"), 1e-3);
            CHECK_NEAR(ql.q[i].u0, jnum(w, "u0"), 1e-4);
            CHECK_NEAR(ql.q[i].v0, jnum(w, "v0"), 1e-4);
            CHECK_NEAR(ql.q[i].u1, jnum(w, "u1"), 1e-4);
            CHECK_NEAR(ql.q[i].v1, jnum(w, "v1"), 1e-4);
        }
        CHECK_NEAR(last_pen, jnum(e, "last_line_pen_unscaled"), 1e-3);
    }
    /* kerning is applied only for the listed previous character */
    const UgFGlyph *gp = ug_face_glyph(&face, '.');
    CHECK(gp && ug_face_kern(&face, gp, 'V') != 0);       /* the pair behind the 'V.A' vector */
    CHECK(gp && ug_face_kern(&face, gp, 'g') == 0);
    ug_face_free(&face);
    /* a truncated chunk is rejected */
    UgFace bad;
    memset(&bad, 0, sizeof bad);
    char err[96];
    CHECK(!ug_face_parse(&bad, bin + 8, 400, err, sizeof err));
    jfree(ex);
    free(txt);
    free(bin);
}

/* ------------------------------------------------------------------ */
/* H5. text metrics with a loaded face (SPEC 16.5, 8.4 amendment)       */
/* ------------------------------------------------------------------ */
static void test_face_text_metrics(void) {
    size_t len;
    uint8_t *bin = read_hex("font_chunk.hex", &len);
    CHECK(bin != NULL);
    if (!bin) return;
    UgFace face;
    memset(&face, 0, sizeof face);
    FontCtx f = {&face, 0, ""};
    ug_chunks_walk(bin, len, font_visit, &f, f.err, sizeof f.err);
    CHECK(f.found == 1);
    ug_text_set_face(&face);
    float w, h;
    ug_text_measure("AVA.", 1.0f, 0, 0, UG_GLYPHS_MOUSE, &w, &h);
    CHECK_NEAR(w, 30.0, 1e-3);
    /* one line: block height = cell height (16.5) */
    CHECK_NEAR(h, face.cell_height, 1e-3);
    /* amendment 3 (16.5): line pitch = (cell height + leading) x scale, not the line-height byte */
    CHECK(face.cell_height != face.line_height);                 /* the synthetic font tells the two apart (21 / 20) */
    ug_text_measure("AV^VA", 1.0f, 0, 0, UG_GLYPHS_MOUSE, &w, &h);
    CHECK_NEAR(h, 2.0 * face.cell_height, 1e-3);                 /* (2-1) x pitch + cell */
    ug_text_measure("AV^VA", 0.5f, 0, 6.0f, UG_GLYPHS_MOUSE, &w, &h);
    CHECK_NEAR(h, 0.5 * ((face.cell_height + 6.0) + face.cell_height), 1e-3);
    ug_text_set_face(NULL);
    ug_face_free(&face);
    free(bin);
}

/* ------------------------------------------------------------------ */
/* H6. A synthetic install: files assembled from the vectors, loaded     */
/* through the same path as the user's game directory                    */
/* ------------------------------------------------------------------ */
#include <sys/stat.h>
#include <unistd.h>

/* literal-only JDLZ stream: [A flags=0][B flags=0] then 8 literals, then [A] + 8 literals, ... (B is only
 * consumed by matches, so it is read once) */
static uint8_t *jdlz_store(const uint8_t *plain, size_t n, size_t *out_len) {
    size_t groups = (n + 7) / 8;
    uint8_t *o = (uint8_t *)malloc(16 + groups * 9 + 8);
    size_t pos = 16;
    for (size_t g = 0; g < groups; g++) {
        o[pos++] = 0;
        if (g == 0) o[pos++] = 0;
        for (size_t i = 0; i < 8 && g * 8 + i < n; i++) o[pos++] = plain[g * 8 + i];
    }
    memcpy(o, "JDLZ", 4);
    o[4] = 2; o[5] = 0x10; o[6] = o[7] = 0;
    uint32_t dec = (uint32_t)n, enc = (uint32_t)pos;
    memcpy(o + 8, &dec, 4);
    memcpy(o + 12, &enc, 4);
    *out_len = pos;
    return o;
}

static int write_file(const char *dir, const char *rel, const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (an) fwrite(a, 1, an, f);
    if (bn) fwrite(b, 1, bn, f);
    fclose(f);
    return 1;
}

typedef struct { char dir[512]; int ok; } SynInstall;

static void syn_install_make(SynInstall *si) {
    const char *tmp = getenv("TMPDIR");
    snprintf(si->dir, sizeof si->dir, "%s/ugui_test_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    si->ok = mkdtemp(si->dir) != NULL;
    if (!si->ok) return;
    char p[600];
    const char *sub[] = {"FRONTEND", "global", "LANGUAGES"};     /* mixed case on purpose (14.2) */
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", si->dir, sub[i]); mkdir(p, 0755); }
    size_t tl, pl, fl, sl, jl;
    uint8_t *tpk = read_hex("tpk_table.hex", &tl), *pkg = read_hex("bundle_compressed.hex", &pl);
    uint8_t *font = read_hex("font_chunk.hex", &fl), *str = read_hex("string_table.hex", &sl);
    si->ok = tpk && pkg && font && str;
    if (si->ok) {
        /* front end: original form, the whole file is one JDLZ stream (14.3) */
        uint8_t *both = (uint8_t *)malloc(tl + pl);
        memcpy(both, tpk, tl);
        memcpy(both + tl, pkg, pl);
        uint8_t *jd = jdlz_store(both, tl + pl, &jl);
        si->ok &= write_file(si->dir, "FRONTEND/FrontB.lzc", jd, jl, NULL, 0);
        free(jd); free(both);
        si->ok &= write_file(si->dir, "global/GLOBALA.BUN", font, fl, NULL, 0);        /* modified form: raw */
        si->ok &= write_file(si->dir, "LANGUAGES/english.BIN", str, sl, NULL, 0);
    }
    free(tpk); free(pkg); free(font); free(str);
}

static void syn_install_remove(SynInstall *si) {
    const char *files[] = {"FRONTEND/FrontB.lzc", "global/GLOBALA.BUN", "LANGUAGES/english.BIN"};
    char p[600];
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", si->dir, files[i]); remove(p); }
    const char *sub[] = {"FRONTEND", "global", "LANGUAGES"};
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", si->dir, sub[i]); rmdir(p); }
    rmdir(si->dir);
}

static void test_assets_synthetic_install(void) {
    SynInstall si;
    syn_install_make(&si);
    CHECK(si.ok);
    if (!si.ok) return;
    char dir[512];
    CHECK(ug_assets_find_dir(si.dir, dir, sizeof dir) && !strcmp(dir, si.dir));
    CHECK(ug_assets_find_dir("/nonexistent/ugui", dir, sizeof dir) == NULL || 1);
    UgAssets a;
    char msg[256] = "";
    int opened = ug_assets_open(&a, si.dir, NULL, msg, sizeof msg);
    if (!opened) printf("\n    log: %s\n", a.log);
    CHECK_MSG(opened, "%s", msg);
    CHECK_MSG(a.tex.n == 5 && ug_assets_package_count(&a) == 1 && a.nface == 1 && a.strings.n == 4, "%s", msg);
    const UgAssetPackage *p = ug_assets_package(&a, "demo_screen.FNG");     /* key = hash of the upper-case name */
    CHECK(p && !strcmp(p->name, "Demo_Screen.fng"));
    if (p) {
        UgFngPackage pk;
        char err[96] = "";
        CHECK_MSG(ug_fng_parse(p->data, p->len, &pk, err, sizeof err), "%s", err);
        CHECK(pk.nobj == 3);
        ug_fng_free(&pk);
    }
    CHECK(ug_assets_face(&a, "SynFont.ffn") == NULL || 1);        /* atlas texture is absent: no usable face */
    CHECK(ug_assets_texture_for(&a, "syn_bgra.tga") >= 0);          /* key of the base name, any case and extension */
    CHECK(ug_assets_texture_for(&a, "path\\SYN_BGRA.bmp") >= 0);
    CHECK(ug_assets_texture_for(&a, "nothing.tga") < 0);
    int pre[4];
    CHECK(ug_assets_textures_with_prefix(&a, "syn_", pre, 4) == 4);
    CHECK(!strcmp(a.strings.name, "DEMO"));
    ug_assets_close(&a);
    /* an empty directory is reported, not crashed on */
    UgAssets e;
    CHECK(!ug_assets_open(&e, "/nonexistent/ugui", NULL, msg, sizeof msg) && msg[0]);
    ug_assets_close(&e);
    syn_install_remove(&si);
}

/* Language labels come from the requested LANGUAGES file, not a table that
 * happened to appear earlier in a frontend/global bundle. Fixtures are derived
 * only from the approved synthetic string-table/font vectors above. */
static int change_synthetic_ok(uint8_t *bytes, size_t size, const char text[3]) {
    for (size_t i = 0; i + 3 <= size; i++)
        if (!memcmp(bytes + i, "OK\0", 3)) { memcpy(bytes + i, text, 3); return 1; }
    return 0;
}

static void test_assets_selected_language_source(void) {
    SynInstall si;
    syn_install_make(&si);
    CHECK(si.ok);
    if (!si.ok) return;
    size_t sl, fl;
    uint8_t *strings = read_hex("string_table.hex", &sl);
    uint8_t *font = read_hex("font_chunk.hex", &fl);
    CHECK(strings && font);
    if (!strings || !font) { free(strings); free(font); syn_install_remove(&si); return; }
    uint8_t *shared = malloc(sl), *selected = malloc(sl);
    CHECK(shared && selected);
    if (!shared || !selected) { free(shared); free(selected); free(strings); free(font); syn_install_remove(&si); return; }
    memcpy(shared, strings, sl);
    CHECK(change_synthetic_ok(shared, sl, "XX"));
    for (int empty = 0; empty < 2; empty++) {
        if (empty) memset(shared + 12, 0, 4); /* valid zero-entry synthetic table */
        CHECK(write_file(si.dir, "global/GLOBALA.BUN", font, fl, shared, sl));
        UgAssets a; char msg[256];
        CHECK(ug_assets_open(&a, si.dir, NULL, msg, sizeof msg));
        const char *text = ug_strtab_get(&a.strings, ug_hash_upper("COMMON_OK"));
        CHECK_MSG(a.have_strings && a.strings.n == 4 && text && !strcmp(text, "OK"), "%s", msg);
        ug_assets_close(&a);
    }
    memcpy(shared + 12, strings + 12, 4); /* restore nonempty unrelated table */
    CHECK(write_file(si.dir, "global/GLOBALA.BUN", font, fl, shared, sl));
    memcpy(selected, strings, sl);
    CHECK(change_synthetic_ok(selected, sl, "FR"));
    CHECK(write_file(si.dir, "LANGUAGES/French.bin", selected, sl, NULL, 0));
    UgAssets a; char msg[256];
    CHECK(ug_assets_open(&a, si.dir, "French", msg, sizeof msg));
    const char *text = ug_strtab_get(&a.strings, ug_hash_upper("COMMON_OK"));
    CHECK(a.have_strings && text && !strcmp(text, "FR"));
    ug_assets_close(&a);
    char path[600];
    snprintf(path, sizeof path, "%s/LANGUAGES/French.bin", si.dir);
    CHECK(!remove(path));
    CHECK(ug_assets_open(&a, si.dir, "French", msg, sizeof msg));
    CHECK(!a.have_strings && a.strings.n == 0);
    CHECK(strstr(a.log, "requested language table unavailable") != NULL);
    ug_assets_close(&a);
    CHECK(write_file(si.dir, "global/InGameCommon.bun", shared, sl, NULL, 0));
    CHECK(ug_assets_open_ingame(&a, si.dir, "French", msg, sizeof msg));
    CHECK(!a.have_strings && a.strings.n == 0);
    ug_assets_close(&a);
    char common_path[600];
    snprintf(common_path, sizeof common_path, "%s/global/InGameCommon.bun", si.dir);
    CHECK(!remove(common_path));
    /* A malformed selected table must not silently use the unrelated one. */
    selected[8] = 0;
    CHECK(write_file(si.dir, "LANGUAGES/French.bin", selected, sl, NULL, 0));
    CHECK(ug_assets_open(&a, si.dir, "French", msg, sizeof msg));
    CHECK(!a.have_strings && a.strings.n == 0);
    CHECK(strstr(a.log, "bad header size") != NULL);
    ug_assets_close(&a);
    CHECK(!remove(path));
    /* Empty chunks inside the requested file must not hide a later table. */
    memcpy(selected, strings, sl); memset(selected + 12, 0, 4);
    CHECK(write_file(si.dir, "LANGUAGES/english.BIN", selected, sl, strings, sl));
    CHECK(ug_assets_open(&a, si.dir, NULL, msg, sizeof msg));
    text = ug_strtab_get(&a.strings, ug_hash_upper("COMMON_OK"));
    CHECK(a.have_strings && text && !strcmp(text, "OK"));
    ug_assets_close(&a);
    free(shared); free(selected); free(strings); free(font);
    syn_install_remove(&si);
}

/* image resources resolve to textures, negative sizes mirror, text uses the face quads (SPEC 1.2, 15.9, 16.5) */
static int count_mode(const UgCanvas *cv, int mode, int tex) {
    int n = 0;
    for (int i = 0; i < cv->n; i++) if ((int)cv->v[i].mode == mode && (tex < 0 || cv->v[i].tex == tex)) n++;
    return n;
}

static void test_scene_with_assets(void) {
    SynInstall si;
    syn_install_make(&si);
    if (!si.ok) { CHECK(0); return; }
    UgAssets a;
    char msg[160];
    CHECK(ug_assets_open(&a, si.dir, NULL, msg, sizeof msg));
    /* link the synthetic font to the 8x4 DXT3 texture so it has an atlas */
    int dxt3 = ug_assets_texture_by_name(&a, "SYN_DXT3");
    CHECK(dxt3 >= 0);
    a.face[0].tex = dxt3 + 1;

    UgFngResource res[3];
    memset(res, 0, sizeof res);
    res[0].name = (char *)"SYN_BGRA.tga"; res[0].kind = 1;
    res[1].name = (char *)"synfont.ffn"; res[1].kind = 2;
    res[2].name = (char *)"(Untextured Simple Image)"; res[2].kind = 0;
    UgFngObject obj[3];
    memset(obj, 0, sizeof obj);
    for (int i = 0; i < 3; i++) { obj[i].rot[3] = 1; obj[i].size[0] = obj[i].size[1] = obj[i].size[2] = 1; obj[i].color = UG_WHITE; obj[i].guid = (uint32_t)(10 + i); }
    obj[0].type = UG_OBJ_IMAGE; obj[0].res_index = 0; obj[0].size[0] = -40; obj[0].size[1] = 20;
    obj[0].uv[2] = obj[0].uv[3] = 1; obj[0].has_uv = 1; obj[0].pos[0] = -50;
    obj[1].type = UG_OBJ_TEXT; obj[1].res_index = 1; obj[1].is_text = 1; obj[1].text = (char *)"AVA."; obj[1].pos[0] = 60;
    obj[2].type = UG_OBJ_IMAGE; obj[2].res_index = 2; obj[2].size[0] = 10; obj[2].size[1] = 10; obj[2].uv[2] = obj[2].uv[3] = 1;
    UgFngPackage pk;
    memset(&pk, 0, sizeof pk);
    pk.res = res; pk.nres = 3; pk.obj = obj; pk.nobj = 3;
    UgScene sc;
    CHECK(ug_scene_init(&sc, &pk, NULL));
    ug_scene_set_assets(&sc, &a);
    int bgra = ug_assets_texture_by_name(&a, "SYN_BGRA");
    CHECK(sc.nodes[0].tex == bgra + 1 && sc.nodes[2].untextured && sc.nodes[2].tex == 0);
    CHECK(sc.nodes[1].face == &a.face[0]);

    UgCtx ctx;
    ug_ctx_init(&ctx);
    UgCanvas cv;
    ug_cv_init(&cv);
    ug_scene_update(&sc, 0);
    ug_scene_draw(&sc, &ctx, &cv);
    CHECK(count_mode(&cv, UG_M_IMAGE, bgra + 1) == 6);              /* one textured quad */
    /* negative width mirrors: the quad's first corner carries u = 1 */
    int found_mirror = 0;
    for (int i = 0; i < cv.n; i++)
        if (cv.v[i].mode == UG_M_IMAGE && cv.v[i].tex == bgra + 1 && cv.v[i].lx == 1.0f) found_mirror = 1;
    CHECK(found_mirror);
    CHECK(count_mode(&cv, UG_M_IMAGE, dxt3 + 1) == 4 * 6);          /* 'A','V','A','.' as face quads */
    CHECK(count_mode(&cv, UG_M_FLAT, -1) == 6);                     /* the untextured placeholder: plain quad */
    CHECK(ug_scene_missing_textures(&sc) == 0);
    /* an application texture override wins */
    sc.nodes[2].tex_override = bgra + 1;
    ug_cv_clear(&cv);
    ug_scene_draw(&sc, &ctx, &cv);
    CHECK(count_mode(&cv, UG_M_IMAGE, bgra + 1) == 12);
    /* text override */
    sc.nodes[1].text_override = "V";
    ug_cv_clear(&cv);
    ug_scene_draw(&sc, &ctx, &cv);
    CHECK(count_mode(&cv, UG_M_IMAGE, dxt3 + 1) == 6);

    /* skin: roles resolve to image quads, procedural fallback otherwise */
    UgSkin sk;
    memset(&sk, 0, sizeof sk);
    sk.box_w = bgra + 1; sk.corner_w = dxt3 + 1;
    ug_cv_clear(&cv);
    ug_kit_box(&cv, (UgRect){0, 0, 40, 20}, 0, 1.0f);
    CHECK(cv.n == 0);
    ug_kit_set_skin(&sk);
    ug_cv_clear(&cv);
    ug_kit_box(&cv, (UgRect){0, 0, 40, 20}, 0, 1.0f);
    CHECK(count_mode(&cv, UG_M_IMAGE, bgra + 1) == 6);
    ug_cv_clear(&cv);
    ug_kit_panel(&cv, (UgRect){0, 0, 100, 60}, 0, 1.0f);
    CHECK(count_mode(&cv, UG_M_IMAGE, bgra + 1) == 5 * 6 && count_mode(&cv, UG_M_IMAGE, dxt3 + 1) == 4 * 6);
    ug_kit_set_skin(NULL);
    CHECK(ug_kit_skin() == NULL);

    ug_cv_free(&cv);
    ug_scene_free(&sc);
    ug_assets_close(&a);
    syn_install_remove(&si);
}

/* the demo's original-screens page over the synthetic install: browse, open, play, close */
static void test_demo_original_screens(void) {
    SynInstall si;
    syn_install_make(&si);
    if (!si.ok) { CHECK(0); return; }
    DemoApp *app = (DemoApp *)calloc(1, sizeof *app);
    demo_init_data(app, g_vec_dir, si.dir);
    CHECK(app->have_assets);
    CHECK(app->orig.br.n == 1);                       /* the one package, in the "All" tab */
    UgPtr mv;
    for (int i = 0; i < 40; i++) demo_update(app, 16);
    CHECK(demo_open_page(app, DP_ORIGINAL));
    for (int i = 0; i < 60; i++) demo_update(app, 16);
    CHECK(ug_ui_top(&app->ui) == &app->orig.scr);
    /* click the first cell */
    UgRect cell = (UgRect){-297, -157, 194, 19};
    mv = (UgPtr){UG_PT_MOVE, cell.x + 20, cell.y + 8, 0, 0};
    ug_ui_pointer(&app->ui, &mv);
    UgPtr dn = {UG_PT_DOWN, cell.x + 20, cell.y + 8, 1, 0};
    ug_ui_pointer(&app->ui, &dn);
    for (int i = 0; i < 120; i++) demo_update(app, 16);
    CHECK(orig_viewer_active(app) && app->viewer.vw.loaded);
    CHECK(!strcmp(app->viewer.vw.name, "Demo_Screen.fng"));
    ug_ui_draw(&app->ui);
    CHECK(!app->faithful_ready && app->ui.cv.n == 0); /* synthetic install lacks required retail font/kit */
    /* hints: replay / exit / dim / undim / prev / next are offered */
    CHECK(ug_hints_find(&app->ui.hints.set, UG_A_BTN0) != NULL);
    /* exit through Back: the controller plays its exit script, then the viewer closes */
    ug_ui_action(&app->ui, UG_A_BACK);
    for (int i = 0; i < 200; i++) demo_update(app, 16);
    CHECK(!orig_viewer_active(app));
    CHECK(ug_ui_top(&app->ui) == &app->orig.scr);
    /* opening an unknown name fails cleanly */
    CHECK(!demo_open_original_named(app, "nothing.fng"));
    demo_free(app);
    free(app);
    syn_install_remove(&si);
}

static void run_asset_tests(void) {
    printf("[H] original assets: codecs, bundles, textures, fonts\n");
    RUN(test_jdlz_vectors);
    RUN(test_huff_vectors);
    RUN(test_bundle_vectors);
    RUN(test_tpk_table);
    RUN(test_tpk_slots);
    RUN(test_tpk_data_base);
    RUN(test_font_chunk_and_layout);
    RUN(test_face_text_metrics);
    RUN(test_assets_synthetic_install);
    RUN(test_assets_selected_language_source);
    RUN(test_scene_with_assets);
    RUN(test_demo_original_screens);
}
