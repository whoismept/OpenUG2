/* M135 RED/GREEN regression: authored car material routing and complete
 * detail-tier LOD selection. Builds synthetic 0x80134010 objects byte-for-
 * byte in memory (no real game assets) and drives them through the actual
 * production parser (n2_load_car / n2_walk_car / n2_car_dedupe_lod), exactly
 * as the real GEOMETRY.BIN loader does. GL-free: links only nfsu2.h.
 *
 *   make car-material-test
 */
#include <stdio.h>
#include <string.h>
#include "../src/nfsu2.h"

static int PASS = 0, FAIL = 0;
static void chk(const char *what, int ok) {
    printf("    %-64s %s\n", what, ok ? "PASS" : "FAIL");
    ok ? PASS++ : FAIL++;
}

/* ---- tiny byte-buffer builder --------------------------------------- */
#define MAXBUF (1 << 16)
typedef struct { unsigned char b[MAXBUF]; long n; } Buf;
static void bu32(Buf *o, uint32_t v) { memcpy(o->b + o->n, &v, 4); o->n += 4; }
static void bf32(Buf *o, float v)    { memcpy(o->b + o->n, &v, 4); o->n += 4; }
static void bbytes(Buf *o, const void *p, long n) { memcpy(o->b + o->n, p, n); o->n += n; }
static void bstr(Buf *o, const char *s) { bbytes(o, s, (long)strlen(s)); }
static void bfill11(Buf *o, int n) { for (int i = 0; i < n; i++) o->b[o->n++] = 0x11; }

/* Wrap `payload` (already-built bytes) as one chunk: magic, size, payload. */
static void chunk(Buf *o, uint32_t magic, const Buf *payload) {
    bu32(o, magic); bu32(o, (uint32_t)payload->n); bbytes(o, payload->b, payload->n);
}

/* 0x134011: name leaf. n2_car_category/n2_car_name_key/n2_car_is_variant all
 * scan the raw leaf bytes for an uppercase A-Z run of length >= 5; no filler
 * skip is applied to the name scan itself, so a plain ASCII name is enough. */
static void leaf_name(Buf *o, const char *name) {
    Buf p; p.n = 0; bstr(&p, name); bu32(&p, 0);   /* pad so length checks are lenient */
    chunk(o, 0x00134011u, &p);
}

/* 0x134012 / 0x134013: flat 8-byte positional entries (first u32 kept). */
static void leaf_slots(Buf *o, uint32_t magic, const uint32_t *vals, int n) {
    Buf p; p.n = 0;
    for (int i = 0; i < n; i++) { bu32(&p, vals[i]); bu32(&p, 0); }
    chunk(o, magic, &p);
}

/* 0x134B01: car vertex buffer, stride 36 (pos f32[3]@0, normal f32[3]@12,
 * colour u32@24, uv f32[2]@28), single-0x11-byte filler prefix. */
static void leaf_verts(Buf *o, const float (*pos)[3], int n) {
    Buf p; p.n = 0;
    bfill11(&p, 3);   /* odd count: proves the skip is single-byte, not paired */
    for (int i = 0; i < n; i++) {
        bf32(&p, pos[i][0]); bf32(&p, pos[i][1]); bf32(&p, pos[i][2]);
        bf32(&p, 0); bf32(&p, 0); bf32(&p, 1);      /* normal */
        bu32(&p, 0xffffffffu);                       /* colour */
        bf32(&p, 0); bf32(&p, 0);                     /* uv */
    }
    chunk(o, 0x00134B01u, &p);
}

/* 0x134B03: u16 triangle index buffer, PAIRED 0x1111 filler prefix. */
static void leaf_idx(Buf *o, const uint16_t *idx, int n) {
    Buf p; p.n = 0;
    bu32(&p, 0x11111111u);   /* two paired 0x1111 words: proves paired-skip */
    for (int i = 0; i < n; i++) { p.b[p.n++] = (unsigned char)(idx[i] & 0xff);
                                  p.b[p.n++] = (unsigned char)(idx[i] >> 8); }
    chunk(o, 0x00134B03u, &p);
}

typedef struct { uint32_t count, mat, matid, start; } SubSpec;

/* 0x134B02: 60-byte submesh records, PAIRED 0x1111 filler prefix. Only the
 * fields the parser actually reads (+12 count, +28 mat, +32 matid, +52
 * start) carry meaningful values; the rest is zero, matching real records'
 * unused bbox/reserved bytes for these synthetic fixtures. */
static void leaf_submeshes(Buf *o, const SubSpec *sub, int n) {
    Buf p; p.n = 0;
    bu32(&p, 0x11111111u);
    for (int i = 0; i < n; i++) {
        long base = p.n;
        for (int z = 0; z < 15; z++) bu32(&p, 0);   /* 60 zero bytes, patched below */
        memcpy(p.b + base + 12, &sub[i].count, 4);
        memcpy(p.b + base + 28, &sub[i].mat,   4);
        memcpy(p.b + base + 32, &sub[i].matid, 4);
        memcpy(p.b + base + 52, &sub[i].start, 4);
    }
    chunk(o, 0x00134B02u, &p);
}

/* One complete 0x80134010 object: name + optional texslots/matslots/submesh
 * table + one vertex/index pair. `tris` triangles, 3 indices each, all
 * referencing vertices 0..nverts-1. */
static void object(Buf *o, const char *name,
                   const uint32_t *texslots, int ntex,
                   const uint32_t *matslots, int nmat,
                   const SubSpec *sub, int nsub,
                   const float (*pos)[3], int nverts,
                   const uint16_t *idx, int nidx) {
    Buf p; p.n = 0;
    leaf_name(&p, name);
    if (texslots) leaf_slots(&p, 0x00134012u, texslots, ntex);
    if (matslots) leaf_slots(&p, 0x00134013u, matslots, nmat);
    if (sub) leaf_submeshes(&p, sub, nsub);
    leaf_verts(&p, pos, nverts);
    leaf_idx(&p, idx, nidx);
    chunk(o, 0x80134010u, &p);
}

static const char *catname(int c) {
    switch (c) {
    case N2_CAR_BODY: return "BODY"; case N2_CAR_GLASS: return "GLASS";
    case N2_CAR_LIGHT: return "LIGHT"; case N2_CAR_TIRE: return "TIRE";
    case N2_CAR_MISC: return "MISC"; case N2_CAR_BRAKELIGHT: return "BRAKELIGHT";
    case N2_CAR_MECH: return "MECH"; default: return "?";
    }
}
static long total_nidx(const N2Scene *s) {
    long t = 0; for (int i = 0; i < s->count; i++) t += s->meshes[i].nidx; return t;
}
static int find_by_cat(const N2Scene *s, int cat) {
    for (int i = 0; i < s->count; i++) if (s->meshes[i].cat == cat) return i;
    return -1;
}
static int count_by_cat(const N2Scene *s, int cat) {
    int n = 0; for (int i = 0; i < s->count; i++) if (s->meshes[i].cat == cat) n++;
    return n;
}

/* ---------------------------------------------------------------------
 * Texture-record fixtures (Phase 1.4): explicit format/draw-mode bytes,
 * block-count bounds validation, and authored-alpha preservation, built as
 * a real synthetic TPK buffer and driven through n2_tpk_open/n2_tpk_decode.
 * ------------------------------------------------------------------- */

/* One 0x7c-byte texture header record. `body` supplies the compressed/
 * palette pixel bytes appended to the shared pixel block; the record's
 * Offset field is filled in by the caller once every record's placement in
 * that shared block is known. */
typedef struct {
    const char *name; uint32_t hash;
    uint32_t paloff, size, palsize; int w, h;
    unsigned char order, usage, blend, wz;
    const unsigned char *pixels; long pixlen;   /* appended to the pixel block */
    uint32_t off;                                /* filled by build_tpk() */
} TexRec;

/* Assemble one synthetic TPK: a header block (0xb3310000, N x 0x7c records)
 * immediately followed by one pixel block (0x33320002) holding every
 * record's pixel bytes back to back, in order. Mirrors n2_tpk_open's own
 * marker bytes and offset conventions exactly. */
static N2Tpk build_tpk(Buf *o, TexRec *recs, int n) {
    Buf hdr; hdr.n = 0;
    long pixoff = 0;
    for (int i = 0; i < n; i++) {
        long base = hdr.n;
        for (int z = 0; z < 0x1f; z++) bu32(&hdr, 0);   /* 0x7c zero bytes, patched below */
        recs[i].off = (uint32_t)pixoff;
        memcpy(hdr.b + base, recs[i].name, strlen(recs[i].name) < 24 ? strlen(recs[i].name) : 23);
        memcpy(hdr.b + base + 0x18, &recs[i].hash, 4);
        memcpy(hdr.b + base + 0x24, &recs[i].off, 4);
        memcpy(hdr.b + base + 0x28, &recs[i].paloff, 4);
        memcpy(hdr.b + base + 0x2c, &recs[i].size, 4);
        memcpy(hdr.b + base + 0x30, &recs[i].palsize, 4);
        uint16_t w16 = (uint16_t)recs[i].w, h16 = (uint16_t)recs[i].h;
        memcpy(hdr.b + base + 0x38, &w16, 2);
        memcpy(hdr.b + base + 0x3a, &h16, 2);
        hdr.b[base + 0x45] = recs[i].order; hdr.b[base + 0x49] = recs[i].usage;
        hdr.b[base + 0x4a] = recs[i].blend; hdr.b[base + 0x4b] = recs[i].wz;
        pixoff += recs[i].pixlen;
    }

    Buf pix; pix.n = 0;
    for (int i = 0; i < n; i++) bbytes(&pix, recs[i].pixels, recs[i].pixlen);

    o->n = 0;
    bu32(o, 0xb3310000u); bu32(o, (uint32_t)hdr.n); bbytes(o, hdr.b, hdr.n);
    bu32(o, 0x33320002u); bu32(o, (uint32_t)pix.n); bbytes(o, pix.b, pix.n);
    return n2_tpk_open(o->b, o->n);
}

/* Locate the already-built record for `hash` and overwrite one field at
 * `field_off` (the header offset constant, e.g. 0x24 for Offset, 0x28 for
 * PaletteOffset) with `val`. Needed whenever one record's pixel blob packs
 * more than one logical region (e.g. P8's palette followed by its index
 * bytes) at a sub-offset build_tpk's own single-blob placement can't know
 * about on its own. */
static void patch_field(Buf *tpk, N2Tpk t, uint32_t hash, int field_off, uint32_t val) {
    for (int b = 0; b < t.nblk; b++) {
        long hbeg = t.blk[b].hbeg, hend = hbeg + t.blk[b].hsize;
        for (long i = hbeg; i + 0x7c <= hend; i++) {
            if (!(tpk->b[i] >= 'A' && tpk->b[i] <= 'Z')) continue;
            if (n2_u32(tpk->b + i + 0x18) != hash) continue;
            memcpy(tpk->b + i + field_off, &val, 4);
            return;
        }
    }
}

/* One 8-byte DXT1/DXT3-style colour endpoint pair + 4-byte index word. */
static void dxt1_block(Buf *o, uint16_t c0, uint16_t c1, uint32_t idxbits) {
    bu32(o, (uint32_t)c0 | ((uint32_t)c1 << 16)); bu32(o, idxbits);
}
static void dxt3_block(Buf *o, uint64_t alpha4, uint16_t c0, uint16_t c1, uint32_t idxbits) {
    bu32(o, (uint32_t)(alpha4 & 0xffffffffu)); bu32(o, (uint32_t)(alpha4 >> 32));
    bu32(o, (uint32_t)c0 | ((uint32_t)c1 << 16)); bu32(o, idxbits);
}

int texture_record_tests(void) {
    printf("\n  T texture-record decode: authored format/draw-mode bytes\n");

    /* T1: complete opaque DXT1 record (order,usage,blend,wz)=(0,0,0,1),
       c0>c1 (opaque interpolation mode -- no index-3 transparency). */
    {
        Buf px; px.n = 0; dxt1_block(&px, 0xFFFFu, 0x0000u, 0x00000000u);   /* 4x4, one block */
        Buf tpk;
        TexRec r = { "OPAQUE_TEST", 0x1001u, 0, (uint32_t)px.n, 0, 4, 4, 0,0,0,1, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1001u, &tex);
        chk("T1 complete opaque record decodes", ok == 1);
        if (ok) {
            chk("T1 dimensions correct (4x4)", tex.w == 4 && tex.h == 4);
            chk("T1 n2_tex_mode reports N2_DRAW_OPAQUE", n2_tex_mode(&tex) == N2_DRAW_OPAQUE);
            chk("T1 alpha not retained (fully opaque source)", tex.alpha == NULL);
            free(tex.rgb); free(tex.alpha); free(tex.dxt);
        }
            }

    /* T2: complete cutout DXT1 record, usage=1, c0<=c1 (1-bit alpha mode),
       with at least one pixel forced to index 3 (transparent). */
    {
        Buf px; px.n = 0;
        dxt1_block(&px, 0x0000u, 0xFFFFu, 0x00000003u);   /* pixel 0 = index 3 (transparent) */
        Buf tpk;
        TexRec r = { "CUTOUT_TEST", 0x1002u, 0, (uint32_t)px.n, 0, 4, 4, 0,1,0,1, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1002u, &tex);
        chk("T2 complete cutout record decodes", ok == 1);
        if (ok) {
            chk("T2 n2_tex_mode reports N2_DRAW_CUTOUT", n2_tex_mode(&tex) == N2_DRAW_CUTOUT);
            chk("T2 authored alpha preserved (transparent texel present)",
                tex.alpha != NULL && tex.alpha[0] == 0);
            chk("T2 authored alpha preserved byte-for-byte (opaque texel = 255, "
                "not renormalised)", tex.alpha != NULL && tex.alpha[1] == 255);
            free(tex.rgb); free(tex.alpha); free(tex.dxt);
        }
            }

    /* T3: complete blended record, usage=2 blend=1 -> N2_DRAW_BLEND. */
    {
        Buf px; px.n = 0; dxt1_block(&px, 0x0000u, 0xFFFFu, 0u);
        Buf tpk;
        TexRec r = { "BLEND_TEST", 0x1003u, 0, (uint32_t)px.n, 0, 4, 4, 5,2,1,0, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1003u, &tex);
        chk("T3 complete blended record decodes", ok == 1);
        if (ok) { chk("T3 n2_tex_mode reports N2_DRAW_BLEND", n2_tex_mode(&tex) == N2_DRAW_BLEND);
                 free(tex.rgb); free(tex.alpha); free(tex.dxt); }
            }

    /* T4: complete additive record, blend=2 -> N2_DRAW_ADD. */
    {
        Buf px; px.n = 0; dxt1_block(&px, 0x0000u, 0xFFFFu, 0u);
        Buf tpk;
        TexRec r = { "ADD_TEST", 0x1004u, 0, (uint32_t)px.n, 0, 4, 4, 5,2,2,0, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1004u, &tex);
        chk("T4 complete additive record decodes", ok == 1);
        if (ok) { chk("T4 n2_tex_mode reports N2_DRAW_ADD", n2_tex_mode(&tex) == N2_DRAW_ADD);
                 free(tex.rgb); free(tex.alpha); free(tex.dxt); }
            }

    /* T5: truncated record header -- the record ends before +0x4c (the last
       field, wz, is at +0x4b). order/usage/blend/wz must stay at their
       memset-0 default (-> N2_DRAW_OPAQUE) rather than reading past the
       block, and W/H (decoded before the +0x4c gate) must still resolve. */
    {
        Buf px; px.n = 0; dxt1_block(&px, 0xFFFFu, 0x0000u, 0u);
        Buf hdr; hdr.n = 0;
        bbytes(&hdr, "TRUNC_TEST", 10);
        for (int z = 10; z < 0x44; z++) hdr.b[hdr.n++] = 0;   /* record ends at +0x44, short of +0x4c */
        uint32_t hash = 0x1005u, off = 0, paloff = 0, size = (uint32_t)px.n, palsize = 0;
        memcpy(hdr.b + 0x18, &hash, 4); memcpy(hdr.b + 0x24, &off, 4);
        memcpy(hdr.b + 0x28, &paloff, 4); memcpy(hdr.b + 0x2c, &size, 4);
        memcpy(hdr.b + 0x30, &palsize, 4);
        uint16_t w16 = 4, h16 = 4; memcpy(hdr.b + 0x38, &w16, 2); memcpy(hdr.b + 0x3a, &h16, 2);
        Buf tpk; tpk.n = 0;
        bu32(&tpk, 0xb3310000u); bu32(&tpk, (uint32_t)hdr.n); bbytes(&tpk, hdr.b, hdr.n);
        bu32(&tpk, 0x33320002u); bu32(&tpk, (uint32_t)px.n); bbytes(&tpk, px.b, px.n);
        N2Tpk t = n2_tpk_open(tpk.b, tpk.n);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, hash, &tex);
        chk("T5 truncated header (< 0x4c bytes): still decodes, no OOB read "
            "(verified clean under -fsanitize=address,undefined)", ok == 1);
        if (ok) { chk("T5 falls back to N2_DRAW_OPAQUE (order/usage/blend/wz unset)",
                      n2_tex_mode(&tex) == N2_DRAW_OPAQUE);
                 free(tex.rgb); free(tex.alpha); free(tex.dxt); }
            }

    /* T6: invalid/oversized data offset -- must fail the decode, not read
       past the buffer. */
    {
        Buf px; px.n = 0; dxt1_block(&px, 0xFFFFu, 0x0000u, 0u);
        Buf tpk;
        TexRec r = { "BADOFF_TEST", 0x1006u, 0, (uint32_t)px.n, 0, 4, 4, 0,0,0,1, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        /* patch a huge/invalid offset directly into the already-built record */
        for (long i = 0; i + 0x7c <= t.blk[0].hsize; i++)
            if (tpk.b[t.blk[0].hbeg + i] >= 'A' && tpk.b[t.blk[0].hbeg + i] <= 'Z' &&
                n2_u32(tpk.b + t.blk[0].hbeg + i + 0x18) == 0x1006u) {
                uint32_t badoff = 0xFFFFFFF0u;
                memcpy(tpk.b + t.blk[0].hbeg + i + 0x24, &badoff, 4);
                break;
            }
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1006u, &tex);
        chk("T6 huge/invalid data offset rejected, no crash", ok == 0);
            }

    /* T7: DXT1 dimensions not divisible by 4 (13x13) -- ceil-to-4 block
       count is 4x4 blocks = 128 bytes; must succeed with exactly that much
       data and fail with less. (5x5 was tried first and rejected as a
       fixture: at that size the UNCHANGED size heuristic that picks DXT1 vs
       DXT3 -- sz > w*h*9/10 -- itself selects DXT3 for a correctly-sized
       32-byte DXT1 buffer, since 32 > 5*5*0.9=22.5. That is a pre-existing
       property of the untouched heuristic, not a defect this milestone
       introduces or is trying to prove; 13x13 keeps 128 comfortably under
       13*13*0.9=152.1, so the heuristic picks DXT1 as intended and this
       fixture actually exercises the new block-count bounds math.) */
    {
        Buf px; px.n = 0;
        for (int b = 0; b < 16; b++) dxt1_block(&px, 0xFFFFu, 0x0000u, 0u);   /* 4x4 blocks */
        Buf tpk;
        TexRec r = { "DXT1_13X13", 0x1007u, 0, (uint32_t)px.n, 0, 13, 13, 0,0,0,1, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1007u, &tex);
        chk("T7 DXT1 13x13 (non-multiple-of-4): decodes with the ceil-to-4 "
            "block count (4x4 blocks, 128 bytes)", ok == 1 && tex.w == 13 && tex.h == 13);
        if (ok) { free(tex.rgb); free(tex.alpha); free(tex.dxt); }

        /* one byte short of the required 128 -> must be rejected, not read
           OOB. Built from px BEFORE the outer block ends. */
        Buf px2; px2.n = 0; bbytes(&px2, px.b, px.n - 1);
        Buf tpk2;
        TexRec r2 = { "DXT1_13X13_SHORT", 0x1008u, 0, (uint32_t)px2.n, 0, 13, 13, 0,0,0,1, px2.b, px2.n, 0 };
        N2Tpk t2 = build_tpk(&tpk2, &r2, 1);
        N2Tex tex2; int ok2 = n2_tpk_decode(tpk2.b, tpk2.n, t2, 0x1008u, &tex2);
        chk("T7b one byte short of the required DXT1 block length is rejected",
            ok2 == 0);
    }

    /* T8: DXT3 dimensions not divisible by 4 (6x6) -- ceil-to-4 is 2x2
       blocks = 64 bytes (16/block, double DXT1's rate at this size). The
       OLD bounds check used a flat w*h/2 estimate (18 bytes for 6x6) --
       far short of the real 64 needed, which is exactly the latent
       out-of-bounds read this fixture guards against. */
    {
        Buf px; px.n = 0;
        for (int b = 0; b < 4; b++) dxt3_block(&px, 0xffffffffffffffffull, 0xFFFFu, 0x0000u, 0u);
        Buf tpk;
        /* Size chosen large enough to select the DXT3 branch's heuristic
           (sz > w*h*9/10); w*h=36, so > 32.4 selects DXT3 over DXT1. */
        TexRec r = { "DXT3_6X6", 0x1009u, 0, (uint32_t)px.n, 0, 6, 6, 0,0,0,1, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1009u, &tex);
        chk("T8 DXT3 6x6 (non-multiple-of-4): decodes with the ceil-to-4 "
            "block count (2x2 blocks, 64 bytes -- not w*h/2=18)",
            ok == 1 && tex.w == 6 && tex.h == 6);
        if (ok) { free(tex.rgb); free(tex.alpha); free(tex.dxt); }
        
        /* Declared Size (40, > w*h*9/10=32.4) still selects the DXT3 branch
           via the unchanged heuristic, but only 18 bytes physically follow
           in the buffer -- far short of the real 64 needed (2x2 blocks x 16).
           The OLD bounds check used a flat w*h/2=18-byte estimate here, which
           this exact input would have satisfied, letting n2_dxt3 read 46
           bytes past the buffer's end. The corrected block-count check must
           reject it instead. */
        Buf px3; px3.n = 0; for (int i = 0; i < 18; i++) px3.b[px3.n++] = 0xAB;
        Buf tpk3;
        TexRec r3 = { "DXT3_6X6_OLD18", 0x100Au, 0, 40, 0, 6, 6, 0,0,0,1, px3.b, 18, 0 };
        N2Tpk t3 = build_tpk(&tpk3, &r3, 1);
        N2Tex tex3; int ok3 = n2_tpk_decode(tpk3.b, tpk3.n, t3, 0x100Au, &tex3);
        chk("T8b the old w*h/2 estimate (18 B) is correctly rejected for DXT3 "
            "(needs 64 B) -- this was a latent OOB read before M135", ok3 == 0);
            }

    /* T9: P8 palette with meaningful, non-uniform authored alpha. */
    {
        unsigned char pal[1024]; memset(pal, 0, sizeof pal);
        pal[0]=255; pal[1]=0;   pal[2]=0;   pal[3]=64;    /* index 0: red,   alpha 64  */
        pal[4]=0;   pal[5]=255; pal[6]=0;   pal[7]=200;   /* index 1: green, alpha 200 */
        Buf px; px.n = 0;
        bbytes(&px, pal, 1024);                           /* palette first */
        unsigned char ix[16] = {0,1,0,1, 1,0,1,0, 0,1,0,1, 1,0,1,0};
        bbytes(&px, ix, 16);                               /* then 4x4 index bytes */
        Buf tpk;
        TexRec r = { "P8_TEST", 0x100Bu, 0, 16, 1024, 4, 4, 0,0,0,1, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        /* build_tpk only knows one placement offset per record (the blob's
           own start); P8 needs Offset (index bytes) to land 1024 bytes past
           PaletteOffset (the palette), inside this same blob -- patch it. */
        patch_field(&tpk, t, 0x100Bu, 0x24, 1024);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x100Bu, &tex);
        chk("T9 P8 record decodes", ok == 1);
        if (ok) {
            chk("T9 afmt reports P8 (8)", tex.afmt == 8);
            chk("T9 authored alpha preserved exactly (64, not renormalised "
                "toward 255)", tex.alpha != NULL && tex.alpha[0] == 64);
            chk("T9 second palette entry's alpha also exact (200)",
                tex.alpha != NULL && tex.alpha[1] == 200);
            free(tex.rgb); free(tex.alpha); free(tex.dxt);
        }
            }

    /* T10: fully-opaque source alpha -- the plane must be freed (NULL), the
       pre-existing M132-R2 behaviour, unaffected by the new order/usage/
       blend/wz fields. */
    {
        unsigned char pal[1024]; memset(pal, 0, sizeof pal);
        for (int i = 0; i < 256; i++) pal[i*4+3] = 255;   /* every entry fully opaque */
        Buf px; px.n = 0; bbytes(&px, pal, 1024);
        unsigned char ix[16]; memset(ix, 0, 16); bbytes(&px, ix, 16);
        Buf tpk;
        TexRec r = { "P8_OPAQUE", 0x100Cu, 0, 16, 1024, 4, 4, 0,0,0,1, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        patch_field(&tpk, t, 0x100Cu, 0x24, 1024);   /* see T9: Offset past the palette */
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x100Cu, &tex);
        chk("T10 fully-opaque P8 source decodes", ok == 1);
        if (ok) { chk("T10 alpha plane freed (fully opaque, unchanged M132-R2 rule)",
                      tex.alpha == NULL);
                 free(tex.rgb); free(tex.alpha); free(tex.dxt); }
            }

    return FAIL;
}

int main(void) {
    printf("MILESTONE 135  car material routing / complete-tier LOD regression\n\n");

    /* -------------------------------------------------------------------
     * Fixture A: one object, one shared/unresolved texture, two contiguous
     * 0x134B02 ranges whose matid selects CARSKIN then WINDSHIELD.
     * Expected: two emitted slices, N2_CAR_BODY then N2_CAR_GLASS, index
     * counts sum to the raw partition (12), no dropped/duplicated face.
     * ------------------------------------------------------------------- */
    printf("  A same texture, different material class (body + windshield)\n");
    {
        Buf f; f.n = 0;
        float pos[6][3] = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,0,1},{0,1,1}};
        uint16_t idx[6] = {0,1,2, 3,4,5};             /* two triangles */
        uint32_t tex[1] = {0xAAAAAAAAu};              /* one shared/unresolved slot */
        uint32_t mat[2] = {N2_MAT_CARSKIN, N2_MAT_WINDSHIELD};
        SubSpec sub[2] = {
            { 3, 0, 0, 0 },   /* tri 0: texslot 0, matslot 0 (CARSKIN) */
            { 3, 0, 1, 3 },   /* tri 1: same texslot 0, matslot 1 (WINDSHIELD) */
        };
        object(&f, "TESTCAR_KIT00_BODY_A", tex, 1, mat, 2, sub, 2, pos, 6, idx, 6);

        N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
        printf("    emitted meshes: %d, total nidx: %ld (raw partition: 6)\n", n, total_nidx(&sc));
        chk("two slices emitted", n == 2);
        int ib = find_by_cat(&sc, N2_CAR_BODY), ig = find_by_cat(&sc, N2_CAR_GLASS);
        chk("a BODY slice exists",  ib >= 0);
        chk("a GLASS slice exists", ig >= 0);
        chk("index-count sum equals the raw partition (no drop/dup)", total_nidx(&sc) == 6);
        if (ib >= 0) chk("BODY slice has 3 indices (one triangle)", sc.meshes[ib].nidx == 3);
        if (ig >= 0) chk("GLASS slice has 3 indices (one triangle)", sc.meshes[ig].nidx == 3);
        for (int i = 0; i < sc.count; i++) { free(sc.meshes[i].verts); free(sc.meshes[i].idx); free(sc.meshes[i].vcol); }
        free(sc.meshes);
    }

    /* -------------------------------------------------------------------
     * Fixture B: two spatially-overlapping detail tiers of one part family.
     * Lower tier ("_A"): one BODY slice only, 12 indices.
     * Higher tier ("_B"): BODY (30 idx) + an unmatched GLASS slice (6 idx),
     * 36 total -- more triangles, so it must win entire.
     * Expected: exactly the higher tier's slices survive (BODY + GLASS),
     * the lower tier's whole slice is gone, unmatched glass is NOT dropped.
     * ------------------------------------------------------------------- */
    printf("  B unmatched glass slice across two detail tiers\n");
    {
        Buf f; f.n = 0;
        /* lower tier: one whole-object BODY mesh, no split. Real (non-flat)
           bbox volume in all 3 axes -- n2_bbox_overlap treats a degenerate
           (zero-volume) box as never overlapping, by design (the safe
           direction for a genuinely flat quad), so a synthetic fixture needs
           real volume or the tier-competition this fixture exists to test
           never triggers at all. */
        float posA[6][3] = {{0,0,0},{2,0,0},{0,2,0},{2,0,0.5f},{2,2,0.5f},{0,2,0.5f}};
        uint16_t idxA[6] = {0,1,2, 3,4,5};
        object(&f, "TESTPART_KIT00_ROOF_A", NULL, 0, NULL, 0, NULL, 0, posA, 6, idxA, 6);

        /* higher tier: identical bbox footprint (guarantees full overlap),
           split BODY(10 tri)+GLASS(2 tri) */
        float posB[36][3];
        for (int i = 0; i < 36; i++) { posB[i][0] = (float)(i % 2) * 2.0f;
                                       posB[i][1] = (float)((i/2) % 2) * 2.0f;
                                       posB[i][2] = (float)((i/4) % 2) * 0.5f; }
        uint16_t idxB[36]; for (int i = 0; i < 36; i++) idxB[i] = (uint16_t)i;
        uint32_t texB[1] = {0xBBBBBBBBu};
        uint32_t matB[2] = {N2_MAT_CARSKIN, N2_MAT_WINDSHIELD};
        SubSpec subB[2] = { {30, 0, 0, 0}, {6, 0, 1, 30} };
        object(&f, "TESTPART_KIT00_ROOF_B", texB, 1, matB, 2, subB, 2, posB, 36, idxB, 36);

        N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
        printf("    emitted meshes after LOD dedup: %d, total nidx: %ld\n", n, total_nidx(&sc));
        chk("winning tier's two slices survive, losing tier's one slice does not",
            n == 2);
        chk("a GLASS slice survives (the winning tier's unmatched slice)",
            count_by_cat(&sc, N2_CAR_GLASS) == 1);
        chk("a BODY slice survives", count_by_cat(&sc, N2_CAR_BODY) == 1);
        chk("retained index total equals the winning tier's raw total (36)",
            total_nidx(&sc) == 36);
        for (int i = 0; i < sc.count; i++) { free(sc.meshes[i].verts); free(sc.meshes[i].idx); free(sc.meshes[i].vcol); }
        free(sc.meshes);
    }

    /* -------------------------------------------------------------------
     * Fixture C: two distinct, spatially SEPARATE source objects whose
     * fixed-width truncated part names collide (same first N chars).
     * Expected: tierid keeps them apart; neither deletes the other's slices,
     * even though both hash to the same namekey.
     * ------------------------------------------------------------------- */
    printf("  C truncated identical names on distinct, separate objects\n");
    {
        Buf f; f.n = 0;
        /* Names identical after the parser's own uppercase-run scan (both
           read as "TESTLONGPARTNAME_HEADLIGHT_" -- no _A.._D tier suffix, so
           n2_car_name_key hashes them identically) but placed FAR apart. */
        float posL[3][3] = {{0,0,0},{1,0,0},{0,1,0}};
        uint16_t idxL[3] = {0,1,2};
        object(&f, "TESTLONGPARTNAME_HEADLIGHT_", NULL, 0, NULL, 0, NULL, 0, posL, 3, idxL, 3);
        float posR[3][3] = {{100,100,100},{101,100,100},{100,101,100}};
        uint16_t idxR[3] = {0,1,2};
        object(&f, "TESTLONGPARTNAME_HEADLIGHT_", NULL, 0, NULL, 0, NULL, 0, posR, 3, idxR, 3);

        N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
        printf("    emitted meshes: %d (two unrelated, non-overlapping objects)\n", n);
        chk("both distinct objects survive (no cross-object tier collision)", n == 2);
        chk("both carry distinct tierids",
            n == 2 && sc.meshes[0].tierid != sc.meshes[1].tierid);
        for (int i = 0; i < sc.count; i++) { free(sc.meshes[i].verts); free(sc.meshes[i].idx); free(sc.meshes[i].vcol); }
        free(sc.meshes);
    }

    /* -------------------------------------------------------------------
     * Fixture D: malformed submesh partitions. Each must fall back to the
     * whole-object path: one mesh, full raw geometry retained, no crash.
     * ------------------------------------------------------------------- */
    printf("  D malformed submesh partitions fall back to the whole object\n");
    {
        float pos[6][3] = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,0,1},{0,1,1}};
        uint16_t idx[6] = {0,1,2, 3,4,5};
        uint32_t tex[1] = {0xCCCCCCCCu};
        uint32_t mat[1] = {N2_MAT_WINDSHIELD};

        /* D1: non-zero first start */
        { Buf f; f.n = 0;
          SubSpec sub[2] = { {3, 0, 0, 3}, {3, 0, 0, 0} };   /* starts at 3, not 0 */
          object(&f, "TESTD1_KIT00_BODY_A", tex, 1, mat, 1, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
          chk("D1 non-zero first start: whole object retained, no crash",
              n == 1 && total_nidx(&sc) == 6);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }

        /* D2: non-contiguous ranges (gap) */
        { Buf f; f.n = 0;
          SubSpec sub[2] = { {3, 0, 0, 0}, {3, 0, 0, 5} };   /* gap: 3..5 unclaimed */
          object(&f, "TESTD2_KIT00_BODY_A", tex, 1, mat, 1, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
          chk("D2 non-contiguous ranges: whole object retained, no crash",
              n == 1 && total_nidx(&sc) == 6);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }

        /* D3: zero/short count */
        { Buf f; f.n = 0;
          SubSpec sub[2] = { {0, 0, 0, 0}, {6, 0, 0, 0} };   /* zero-count range */
          object(&f, "TESTD3_KIT00_BODY_A", tex, 1, mat, 1, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
          chk("D3 zero-count range: whole object retained, no crash",
              n == 1 && total_nidx(&sc) == 6);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }

        /* D4: index overrun (chain exceeds the decoded index buffer) */
        { Buf f; f.n = 0;
          SubSpec sub[2] = { {3, 0, 0, 0}, {6, 0, 0, 3} };   /* claims 3..9, buffer is only 6 */
          object(&f, "TESTD4_KIT00_BODY_A", tex, 1, mat, 1, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
          chk("D4 index overrun: whole object retained, no crash",
              n == 1 && total_nidx(&sc) == 6);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }

        /* D5: incomplete final chain (does not end at the buffer's own end) */
        { Buf f; f.n = 0;
          SubSpec sub[2] = { {3, 0, 0, 0}, {2, 0, 0, 3} };   /* ends at 5, buffer is 6 */
          object(&f, "TESTD5_KIT00_BODY_A", tex, 1, mat, 1, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
          chk("D5 incomplete final chain: whole object retained, no crash",
              n == 1 && total_nidx(&sc) == 6);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }

        /* D6: out-of-range texture slot -- must not crash; falls back safely
           per-slice (subtex[k]=0) rather than indexing out of bounds. Still a
           STRUCTURALLY valid partition, so this legitimately may split; the
           requirement is safety, not necessarily whole-object fallback. */
        { Buf f; f.n = 0;
          SubSpec sub[2] = { {3, 99, 0, 0}, {3, 0, 0, 3} };   /* mat=99, only 1 texslot */
          object(&f, "TESTD6_KIT00_BODY_A", tex, 1, mat, 1, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
          chk("D6 out-of-range texture slot: no crash, full geometry retained",
              n >= 1 && total_nidx(&sc) == 6);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }

        /* D7: out-of-range material slot -- must fall back to the object
           category for that slice, not crash or misclassify from garbage. */
        { Buf f; f.n = 0;
          uint32_t tex2[2] = {0xCCCCCCCCu, 0xDDDDDDDDu};
          SubSpec sub[2] = { {3, 0, 77, 0}, {3, 1, 0, 3} };   /* matid=77, only 1 matslot */
          object(&f, "TESTD7_KIT00_BODY_A", tex2, 2, mat, 1, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
          chk("D7 out-of-range material slot: no crash, full geometry retained",
              n >= 1 && total_nidx(&sc) == 6);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }

        /* D8: missing 0x134013 entirely -- matid must fall back to the
           object-level category (0 slots -> every matid out of bounds).
           Needs a REAL resolved-texture difference to force the split at
           all (a car's own TPK keys[]/nkeys, not the empty table the other
           fixtures use, since clsdiffer is false here by construction). */
        { Buf f; f.n = 0;
          uint32_t tex2[2] = {0xCCCCCCCCu, 0xDDDDDDDDu};
          uint32_t keys[1] = {0xCCCCCCCCu};   /* only slot 0 resolves */
          SubSpec sub[2] = { {3, 0, 0, 0}, {3, 1, 0, 3} };   /* differing tex -> still splits */
          object(&f, "TESTD8_KIT00_BODY_A", tex2, 2, NULL, 0, sub, 2, pos, 6, idx, 6);
          N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, keys, 1, NULL);
          chk("D8 missing 0x134013: no crash, full geometry retained, "
              "both slices fall back to the whole-object category",
              n == 2 && total_nidx(&sc) == 6 &&
              sc.meshes[0].cat == N2_CAR_BODY && sc.meshes[1].cat == N2_CAR_BODY);
          for (int i=0;i<sc.count;i++){free(sc.meshes[i].verts);free(sc.meshes[i].idx);free(sc.meshes[i].vcol);}
          free(sc.meshes); }
    }

    /* -------------------------------------------------------------------
     * Fixture E: a tire/rim object with several material hashes attached
     * (none of them CARSKIN/WINDSHIELD). Expected: neither hash is
     * misclassified into BODY or GLASS; the whole tire's coverage survives,
     * undamaged, whatever the split outcome.
     * ------------------------------------------------------------------- */
    printf("  E tire/rim object with several unrelated material hashes\n");
    {
        Buf f; f.n = 0;
        float pos[9][3] = {{0,0,0},{1,0,0},{0,1,0}, {1,0,0},{1,1,0},{0,1,0},
                           {0,1,0},{1,1,0},{0,0,1}};
        uint16_t idx[9] = {0,1,2, 3,4,5, 6,7,8};
        uint32_t tex[1] = {0xEEEEEEEEu};
        uint32_t mat[3] = {0x54949afdu /* CHROME */, 0x2e65e067u /* ALUMINUM */, 0xdeadbeefu};
        SubSpec sub[3] = { {3, 0, 0, 0}, {3, 0, 1, 3}, {3, 0, 2, 6} };
        object(&f, "TESTWHEEL_KIT00_TIRE_A", tex, 1, mat, 3, sub, 3, pos, 9, idx, 9);

        N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
        printf("    emitted meshes: %d, total nidx: %ld (raw: 9)\n", n, total_nidx(&sc));
        chk("no coverage loss: every index still represented exactly once",
            total_nidx(&sc) == 9);
        chk("no accidental BODY/GLASS conversion of tire geometry",
            count_by_cat(&sc, N2_CAR_BODY) == 0 && count_by_cat(&sc, N2_CAR_GLASS) == 0);
        for (int i = 0; i < sc.count; i++) { free(sc.meshes[i].verts); free(sc.meshes[i].idx); free(sc.meshes[i].vcol); }
        free(sc.meshes);
    }

    texture_record_tests();

    printf("\n  %d passed, %d failed\n", PASS, FAIL);
    return FAIL ? 1 : 0;
}
