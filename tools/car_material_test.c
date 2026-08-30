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

/* Track/world vertex buffer, stride 24: pos f32[3]@0, prelight RGBA@12,
 * uv f32[2]@16. This is the production layout n2_walk_meshes passes to
 * n2_add_pair; car fixtures above deliberately use their separate 36B layout. */
static void leaf_world_verts(Buf *o, const float (*pos)[3], int n) {
    Buf p; p.n = 0;
    bfill11(&p, 4);
    for (int i = 0; i < n; i++) {
        bf32(&p, pos[i][0]); bf32(&p, pos[i][1]); bf32(&p, pos[i][2]);
        bu32(&p, 0xffffffffu);
        bf32(&p, 0); bf32(&p, 0);
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

static int category_for_name(const char *name) {
    Buf p; p.n = 0;
    leaf_name(&p, name);
    return n2_mesh_category(p.b, 0, p.n);
}

/* M136 RED/GREEN regression: SKY is a narrow authored family and the shipped
 * dome is a two-material object (dome + alpha cap), not one last-slot mesh. */
static void sky_material_routing_test(void) {
    printf("\n  SKY authored family and two-range material routing\n");
    chk("exact SKYDOME is sky", category_for_name("SKYDOME") == N2_SKY);
    chk("SKY_ family is sky", category_for_name("SKY_NIGHT_TEST") == N2_SKY);
    chk("building containing SKYDOME is ordinary",
        category_for_name("XB_SKYDOMEB_1A_LL_00") == N2_OTHER);
    chk("factory skylight is ordinary",
        category_for_name("XB_FACTORYSKYLIGHTA_1A_00") == N2_OTHER);

    Buf object_payload; object_payload.n = 0;
    leaf_name(&object_payload, "SKYDOME");
    const uint32_t slots[2] = {0x5fb8bcd1u, 0x2414a01eu}; /* cap, dome */
    leaf_slots(&object_payload, 0x00134012u, slots, 2);
    const SubSpec sub[2] = {
        {288, 1, 0,   0}, /* 96 dome triangles */
        {144, 0, 0, 288}, /* 48 cap triangles */
    };
    leaf_submeshes(&object_payload, sub, 2);
    const float pos[3][3] = {{0,0,0},{1,0,0},{0,1,0}};
    uint16_t idx[432];
    for (int i = 0; i < 432; i++) idx[i] = (uint16_t)(i % 3);
    leaf_world_verts(&object_payload, pos, 3);
    leaf_idx(&object_payload, idx, 432);

    Buf file; file.n = 0;
    chunk(&file, 0x80134010u, &object_payload);
    N2Scene scene; memset(&scene, 0, sizeof scene);
    /* The real SKY keys live in shared LOC4, not the region-local TPK key
       inventory passed to the mesh walker. Preserve the authored slots now;
       world_bind_textures resolves them later. */
    n2_walk_meshes(file.b, 0, file.n, &scene, NULL, 0);
    chk("SKYDOME emits dome and cap ranges", scene.count == 2);
    chk("SKYDOME preserves all 432 indices", total_nidx(&scene) == 432);
    chk("dome range selects shipped dome slot",
        scene.count == 2 && scene.meshes[0].texkey == 0x2414a01eu &&
        scene.meshes[0].nidx == 288);
    chk("cap range selects shipped cap slot",
        scene.count == 2 && scene.meshes[1].texkey == 0x5fb8bcd1u &&
        scene.meshes[1].nidx == 144);
    for (int i = 0; i < scene.count; i++) {
        free(scene.meshes[i].verts); free(scene.meshes[i].idx); free(scene.meshes[i].vcol);
    }
    free(scene.meshes);

    chk("night profile parses", n2_sky_profile_parse("night") == N2_SKY_NIGHT);
    chk("invalid sky profile is rejected", n2_sky_profile_parse("noon") < 0);
    chk("night remap selects shipped dome key",
        n2_sky_remap_key(N2_SKY_NIGHT, 0x2414a01eu) == 0x8a9a05cfu);
    chk("night remap selects shipped cap key",
        n2_sky_remap_key(N2_SKY_NIGHT, 0x5fb8bcd1u) == 0xb0eb9302u);
    chk("unknown sky key is unchanged",
        n2_sky_remap_key(N2_SKY_NIGHT, 0x12345678u) == 0x12345678u);
}

/* Hand-built N2Mesh for n2_car_dedupe_lod fixtures (Fixture G): the function
 * only reads tierid/namekey/nverts/verts/nidx, so this drives it directly
 * without going through the chunk parser at all. X varies per call (drives
 * n2_bbox_overlap), Y/Z fixed 0..1 so only X controls overlap ratio. */
static N2Mesh mk_lod_mesh(uint32_t tierid, uint32_t namekey, float x0, float x1, int nidx) {
    N2Mesh m; memset(&m, 0, sizeof m);
    m.tierid = tierid; m.namekey = namekey; m.nidx = nidx; m.nverts = 2;
    m.verts = (float *)malloc(2 * 5 * sizeof(float));
    float v0[5] = {x0, 0, 0, 0, 0}, v1[5] = {x1, 1, 1, 0, 0};
    memcpy(m.verts, v0, sizeof v0); memcpy(m.verts + 5, v1, sizeof v1);
    return m;
}
static void free_lod_scene(N2Scene *s) {
    for (int i = 0; i < s->count; i++) { free(s->meshes[i].verts); free(s->meshes[i].idx); free(s->meshes[i].vcol); }
    free(s->meshes);
}
static int has_tierid(const N2Scene *s, uint32_t tierid) {
    for (int i = 0; i < s->count; i++) if (s->meshes[i].tierid == tierid) return 1;
    return 0;
}

/* Regression for the world-side bug M135 accidentally exposed. A multi-slot
 * building object has an exact 0x134B02 partition, but the old production
 * walker only honoured that linkage for ROAD/TERRAIN. OTHER therefore became
 * one mesh wearing the last slot (often glass/railing), spreading its draw
 * mode over the opaque walls. These fixtures drive n2_walk_meshes itself. */
static void world_material_routing_test(void) {
    printf("\n  W world multi-material routing stays on its exact submesh\n");
    Buf object_payload; object_payload.n = 0;
    leaf_name(&object_payload, "XB_TEST_MULTIMATERIAL_1A_00");
    uint32_t slots[2] = {0x11111101u, 0x22222202u};
    leaf_slots(&object_payload, 0x00134012u, slots, 2);
    SubSpec sub[2] = {{3, 0, 0, 0}, {3, 1, 0, 3}};
    leaf_submeshes(&object_payload, sub, 2);
    float pos[6][3] = {{0,0,0},{1,0,0},{0,1,0},
                       {0,0,1},{1,0,1},{0,1,1}};
    uint16_t idx[6] = {0,1,2, 3,4,5};
    leaf_world_verts(&object_payload, pos, 6);
    leaf_idx(&object_payload, idx, 6);

    Buf file; file.n = 0;
    chunk(&file, 0x80134010u, &object_payload);
    N2Scene scene; memset(&scene, 0, sizeof scene);
    n2_walk_meshes(file.b, 0, file.n, &scene, slots, 2);

    chk("OTHER object emits one mesh per verified material range", scene.count == 2);
    chk("world material partition preserves all six indices", total_nidx(&scene) == 6);
    chk("first range keeps opaque-wall slot 0",
        scene.count == 2 && scene.meshes[0].texkey == slots[0] && scene.meshes[0].nidx == 3);
    chk("second range keeps railing/glass slot 1",
        scene.count == 2 && scene.meshes[1].texkey == slots[1] && scene.meshes[1].nidx == 3);
    chk("both verified ranges may consume their authored draw modes",
        scene.count == 2 && scene.meshes[0].mat_exact && scene.meshes[1].mat_exact &&
        n2_world_draw_mode(&scene.meshes[0], N2_DRAW_OPAQUE) == N2_DRAW_OPAQUE &&
        n2_world_draw_mode(&scene.meshes[1], N2_DRAW_CUTOUT) == N2_DRAW_CUTOUT);
    for (int i = 0; i < scene.count; i++) {
        free(scene.meshes[i].verts); free(scene.meshes[i].idx); free(scene.meshes[i].vcol);
    }
    free(scene.meshes);

    /* A malformed multi-slot partition must keep all geometry but may not
       spread whichever slot n2_mesh_texkey_cat happened to choose over it. */
    object_payload.n = 0;
    leaf_name(&object_payload, "XB_TEST_MALFORMED_1A_00");
    leaf_slots(&object_payload, 0x00134012u, slots, 2);
    SubSpec badsub[2] = {{3, 0, 0, 0}, {3, 1, 0, 4}}; /* gap: not contiguous */
    leaf_submeshes(&object_payload, badsub, 2);
    leaf_world_verts(&object_payload, pos, 6);
    leaf_idx(&object_payload, idx, 6);
    file.n = 0; chunk(&file, 0x80134010u, &object_payload);
    memset(&scene, 0, sizeof scene);
    n2_walk_meshes(file.b, 0, file.n, &scene, slots, 2);
    chk("malformed multi-slot object falls back with full geometry",
        scene.count == 1 && total_nidx(&scene) == 6);
    chk("malformed multi-slot fallback is forced opaque",
        scene.count == 1 && !scene.meshes[0].mat_exact &&
        n2_world_draw_mode(&scene.meshes[0], N2_DRAW_BLEND) == N2_DRAW_OPAQUE);
    for (int i = 0; i < scene.count; i++) {
        free(scene.meshes[i].verts); free(scene.meshes[i].idx); free(scene.meshes[i].vcol);
    }
    free(scene.meshes);

    /* Two positional slots are still a multi-slot object when one is empty.
       The empty entry makes the partition invalid; counting only non-zero
       values used to mislabel this fallback as exact and leak transparency. */
    object_payload.n = 0;
    leaf_name(&object_payload, "XB_TEST_EMPTY_SLOT_1A_00");
    uint32_t one_and_empty[2] = {slots[1], 0};
    leaf_slots(&object_payload, 0x00134012u, one_and_empty, 2);
    SubSpec emptysub[2] = {{3, 0, 0, 0}, {3, 1, 0, 3}};
    leaf_submeshes(&object_payload, emptysub, 2);
    leaf_world_verts(&object_payload, pos, 6);
    leaf_idx(&object_payload, idx, 6);
    file.n = 0; chunk(&file, 0x80134010u, &object_payload);
    memset(&scene, 0, sizeof scene);
    n2_walk_meshes(file.b, 0, file.n, &scene, slots, 2);
    chk("two positional slots with one empty fall back with full geometry",
        scene.count == 1 && total_nidx(&scene) == 6);
    chk("empty-slot multi-material fallback is forced opaque",
        scene.count == 1 && !scene.meshes[0].mat_exact &&
        n2_world_draw_mode(&scene.meshes[0], N2_DRAW_CUTOUT) == N2_DRAW_OPAQUE);
    for (int i = 0; i < scene.count; i++) {
        free(scene.meshes[i].verts); free(scene.meshes[i].idx); free(scene.meshes[i].vcol);
    }
    free(scene.meshes);

    /* A genuinely single-slot object is exact without needing a partition;
       this preserves authored fence/foliage cutouts rather than disabling
       world transparency globally. */
    object_payload.n = 0;
    leaf_name(&object_payload, "XO_TEST_RAILING_1A_00");
    leaf_slots(&object_payload, 0x00134012u, slots + 1, 1);
    leaf_world_verts(&object_payload, pos, 3);
    leaf_idx(&object_payload, idx, 3);
    file.n = 0; chunk(&file, 0x80134010u, &object_payload);
    memset(&scene, 0, sizeof scene);
    n2_walk_meshes(file.b, 0, file.n, &scene, slots, 2);
    chk("single-slot world object keeps its exact authored cutout",
        scene.count == 1 && scene.meshes[0].mat_exact &&
        n2_world_draw_mode(&scene.meshes[0], N2_DRAW_CUTOUT) == N2_DRAW_CUTOUT);
    for (int i = 0; i < scene.count; i++) {
        free(scene.meshes[i].verts); free(scene.meshes[i].idx); free(scene.meshes[i].vcol);
    }
    free(scene.meshes);
}

/* world_dedup used to call meshes identical from only texture+bbox+counts.
 * Split submeshes share the full source vertex pool, so two different index
 * ranges can satisfy all of those coarse fields. The production identity
 * predicate must inspect content, not just dimensions. */
static void world_mesh_identity_test(void) {
    printf("\n  H world dedup identity includes mesh content\n");
    float verts[6 * 5] = {0};
    uint16_t ia[3] = {0,1,2}, ib[3] = {3,4,5}, ic[3] = {0,1,2};
    N2Mesh a, b, c; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    a.verts = b.verts = c.verts = verts;
    a.idx = ia; b.idx = ib; c.idx = ic;
    a.nverts = b.nverts = c.nverts = 6;
    a.nidx = b.nidx = c.nidx = 3;
    a.texkey = b.texkey = c.texkey = 0xABCD1234u;
    a.mat_exact = b.mat_exact = c.mat_exact = 1;
    chk("different index ranges are not duplicate mesh content",
        !n2_mesh_same_content(&a, &b));
    chk("byte-identical geometry and material ownership are duplicates",
        n2_mesh_same_content(&a, &c));
    c.mat_exact = 0;
    chk("exact and conservative-fallback material ownership stay separate",
        !n2_mesh_same_content(&a, &c));
    chk("exact and fallback opaque material ownership use separate batch groups",
        n2_world_batch_material_group(&a, N2_DRAW_OPAQUE) !=
        n2_world_batch_material_group(&c, N2_DRAW_OPAQUE));
    chk("one exact material's opaque and cutout modes use separate batch groups",
        n2_world_batch_material_group(&a, N2_DRAW_OPAQUE) !=
        n2_world_batch_material_group(&a, N2_DRAW_CUTOUT));
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
    unsigned char fmt;                           /* +0x3e format tag (M135-R):
                                                     0x08 P8, 0x22 DXT1, 0x24 DXT3 */
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
        hdr.b[base + 0x3e] = recs[i].fmt;
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
        TexRec r = { "OPAQUE_TEST", 0x1001u, 0, (uint32_t)px.n, 0, 4, 4, 0,0,0,1, 0x22, px.b, px.n, 0 };
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
        TexRec r = { "CUTOUT_TEST", 0x1002u, 0, (uint32_t)px.n, 0, 4, 4, 0,1,0,1, 0x22, px.b, px.n, 0 };
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
        TexRec r = { "BLEND_TEST", 0x1003u, 0, (uint32_t)px.n, 0, 4, 4, 5,2,1,0, 0x22, px.b, px.n, 0 };
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
        TexRec r = { "ADD_TEST", 0x1004u, 0, (uint32_t)px.n, 0, 4, 4, 5,2,2,0, 0x22, px.b, px.n, 0 };
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
        hdr.b[0x3e] = 0x22;   /* DXT1 format tag: +0x3e is well before the +0x4c
                                 truncation point this fixture tests, so it must
                                 be present the same way real truncated records
                                 that still resolve W/H would carry it */
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
        TexRec r = { "BADOFF_TEST", 0x1006u, 0, (uint32_t)px.n, 0, 4, 4, 0,0,0,1, 0x22, px.b, px.n, 0 };
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
       data and fail with less. (Originally built at 5x5 and moved to 13x13
       when the OLD size-heuristic format dispatch -- sz > w*h*9/10 -- picked
       DXT3 instead of DXT1 for a correctly-sized 32-byte 5x5 DXT1 buffer,
       since 32 > 5*5*0.9=22.5; that heuristic is GONE now (M135-R: format
       comes from the record's own +0x3e tag, set explicitly to 0x22 below),
       so the size-vs-heuristic concern that picked 13x13 no longer applies
       -- kept at 13x13 anyway since it already exercises the non-multiple-
       of-4 block-count math this fixture is actually testing.) */
    {
        Buf px; px.n = 0;
        for (int b = 0; b < 16; b++) dxt1_block(&px, 0xFFFFu, 0x0000u, 0u);   /* 4x4 blocks */
        Buf tpk;
        TexRec r = { "DXT1_13X13", 0x1007u, 0, (uint32_t)px.n, 0, 13, 13, 0,0,0,1, 0x22, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1007u, &tex);
        chk("T7 DXT1 13x13 (non-multiple-of-4): decodes with the ceil-to-4 "
            "block count (4x4 blocks, 128 bytes)", ok == 1 && tex.w == 13 && tex.h == 13);
        if (ok) { free(tex.rgb); free(tex.alpha); free(tex.dxt); }

        /* one byte short of the required 128 -> must be rejected, not read
           OOB. Built from px BEFORE the outer block ends. */
        Buf px2; px2.n = 0; bbytes(&px2, px.b, px.n - 1);
        Buf tpk2;
        TexRec r2 = { "DXT1_13X13_SHORT", 0x1008u, 0, (uint32_t)px2.n, 0, 13, 13, 0,0,0,1, 0x22, px2.b, px2.n, 0 };
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
        /* fmt tag (0x24) selects the DXT3 branch directly (M135-R); Size is
           only used for the byte-length bound below, not format selection. */
        TexRec r = { "DXT3_6X6", 0x1009u, 0, (uint32_t)px.n, 0, 6, 6, 0,0,0,1, 0x24, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x1009u, &tex);
        chk("T8 DXT3 6x6 (non-multiple-of-4): decodes with the ceil-to-4 "
            "block count (2x2 blocks, 64 bytes -- not w*h/2=18)",
            ok == 1 && tex.w == 6 && tex.h == 6);
        if (ok) { free(tex.rgb); free(tex.alpha); free(tex.dxt); }

        /* Declared Size (40) is irrelevant to format selection now (the fmt
           tag below picks DXT3 directly), but only 18 bytes physically
           follow in the buffer -- far short of the real 64 needed (2x2
           blocks x 16). The OLD bounds check used a flat w*h/2=18-byte
           estimate here, which this exact input would have satisfied,
           letting n2_dxt3 read 46 bytes past the buffer's end. The
           corrected block-count check must reject it instead. */
        Buf px3; px3.n = 0; for (int i = 0; i < 18; i++) px3.b[px3.n++] = 0xAB;
        Buf tpk3;
        TexRec r3 = { "DXT3_6X6_OLD18", 0x100Au, 0, 40, 0, 6, 6, 0,0,0,1, 0x24, px3.b, 18, 0 };
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
        TexRec r = { "P8_TEST", 0x100Bu, 0, 16, 1024, 4, 4, 0,0,0,1, 0x08, px.b, px.n, 0 };
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
        TexRec r = { "P8_OPAQUE", 0x100Cu, 0, 16, 1024, 4, 4, 0,0,0,1, 0x08, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        patch_field(&tpk, t, 0x100Cu, 0x24, 1024);   /* see T9: Offset past the palette */
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x100Cu, &tex);
        chk("T10 fully-opaque P8 source decodes", ok == 1);
        if (ok) { chk("T10 alpha plane freed (fully opaque, unchanged M132-R2 rule)",
                      tex.alpha == NULL);
                 free(tex.rgb); free(tex.alpha); free(tex.dxt); }
            }

    /* T11: unknown format tag (M135-R) -- not one of the three proven values
       (0x08 P8, 0x22 DXT1, 0x24 DXT3), and not one of the two named-but-
       unsupported ones (0x20 BGRA8, 0x26 DXT5) either. Must be rejected, not
       guessed at via the old size heuristic. */
    {
        Buf px; px.n = 0; dxt1_block(&px, 0xFFFFu, 0x0000u, 0u);
        Buf tpk;
        TexRec r = { "UNKNOWN_FMT", 0x100Du, 0, (uint32_t)px.n, 0, 4, 4, 0,0,0,1, 0x99, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x100Du, &tex);
        chk("T11 unknown format tag (0x99) rejected, not guessed at", ok == 0);
    }

    /* T12: contradictory tag -- claims DXT1 (0x22) but carries a real 1024-
       byte palette (PaletteSize >= 1024, the P8 signal). A genuine DXT1
       record never has a palette; trust neither guess and reject rather
       than decode either interpretation. */
    {
        unsigned char pal[1024]; memset(pal, 0, sizeof pal);
        Buf px; px.n = 0; bbytes(&px, pal, 1024);
        unsigned char ix[16]; memset(ix, 0, 16); bbytes(&px, ix, 16);
        Buf tpk;
        TexRec r = { "CONTRADICT_FMT", 0x100Eu, 0, 16, 1024, 4, 4, 0,0,0,1, 0x22, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        patch_field(&tpk, t, 0x100Eu, 0x24, 1024);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x100Eu, &tex);
        chk("T12 DXT1 tag contradicted by a real P8-sized palette: rejected",
            ok == 0);
    }

    /* T13: the reverse contradiction -- tagged P8 (0x08) but PaletteSize is
       0 (no palette at all, the DXT signal). A genuine P8 record always
       carries a real palette; reject rather than fall through to a DXT
       interpretation of palette-indexed bytes. */
    {
        Buf px; px.n = 0; dxt1_block(&px, 0xFFFFu, 0x0000u, 0u);
        Buf tpk;
        TexRec r = { "CONTRADICT_FMT2", 0x100Fu, 0, (uint32_t)px.n, 0, 4, 4, 0,0,0,1, 0x08, px.b, px.n, 0 };
        N2Tpk t = build_tpk(&tpk, &r, 1);
        N2Tex tex; int ok = n2_tpk_decode(tpk.b, tpk.n, t, 0x100Fu, &tex);
        chk("T13 P8 tag contradicted by PaletteSize=0: rejected", ok == 0);
    }

    return FAIL;
}

/* ---------------------------------------------------------------------
 * n2_sort_back_to_front (M135-R item 2): GL-free, drives the exact function
 * main.c's deferred blend pass calls, on plain index/distance arrays --
 * no N2Batch, no GL, no render.h. At least three batches, including an
 * equal-distance tie, per the review's explicit requirement.
 * ------------------------------------------------------------------- */
static int sort_tests(void) {
    printf("\n  S n2_sort_back_to_front: deterministic back-to-front order\n");

    /* four batches: id 2 farthest (20), id 0 mid (10), ids 1 and 3 TIE at 5.
       Expected order (farthest first, smaller index wins a tie): 2,0,1,3. */
    int idx[4] = {0,1,2,3};
    float dist[4] = {10.0f, 5.0f, 20.0f, 5.0f};   /* indexed by batch id, not position */
    n2_sort_back_to_front(idx, 4, dist);
    chk("farthest batch (id 2, dist 20) drawn first", idx[0] == 2);
    chk("mid batch (id 0, dist 10) drawn second", idx[1] == 0);
    chk("tied pair (ids 1 and 3, both dist 5): smaller index (1) first",
        idx[2] == 1 && idx[3] == 3);

    /* re-run with the SAME input to prove determinism -- no dependence on
       memory layout, uninitialized state, or qsort-style unstable pivoting. */
    int idx2[4] = {0,1,2,3};
    n2_sort_back_to_front(idx2, 4, dist);
    chk("re-run on identical input reproduces the identical order",
        idx2[0]==idx[0] && idx2[1]==idx[1] && idx2[2]==idx[2] && idx2[3]==idx[3]);

    /* already-sorted and reverse-sorted inputs: edge cases for an insertion
       sort (best/worst case shift counts), same expected descending result. */
    int idx3[3] = {0,1,2}; float dist3[3] = {30.0f, 20.0f, 10.0f};   /* already descending */
    n2_sort_back_to_front(idx3, 3, dist3);
    chk("already-descending input stays in order", idx3[0]==0 && idx3[1]==1 && idx3[2]==2);

    int idx4[3] = {0,1,2}; float dist4[3] = {10.0f, 20.0f, 30.0f};   /* ascending: fully reversed */
    n2_sort_back_to_front(idx4, 3, dist4);
    chk("fully-ascending input is reversed to descending", idx4[0]==2 && idx4[1]==1 && idx4[2]==0);

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

    /* -------------------------------------------------------------------
     * Fixture F (M135-R item 4): a single-submesh object (nsub==1) whose
     * sole submesh's material hash is WINDSHIELD, but whose OWN NAME matches
     * the BODY name-heuristic (contains "KIT") rather than GLASS/WINDOW --
     * exactly the real, fleet-wide shape of every car's door-window pane
     * (e.g. HUMMER_KIT00_DOOR_LEFT_B; census: 557 real hits across 28 cars,
     * scratchpad/m135/windshield_census.c -- every one of them previously
     * resolved to N2_CAR_BODY via the "KIT" substring, since a single-
     * submesh object never reached material classification at all before
     * this fix). Splitting never enters into it: nsub==1, nothing to
     * compare against. The only question is whether the whole object's
     * category comes from the proven material hash or the name heuristic.
     * ------------------------------------------------------------------- */
    printf("  F single-submesh WINDSHIELD-material object with a BODY-heuristic name\n");
    {
        Buf f; f.n = 0;
        float pos[3][3] = {{0,0,0},{1,0,0},{0,1,0}};
        uint16_t idx[3] = {0,1,2};
        uint32_t tex[1] = {0xAAAAAAAAu};
        uint32_t mat[1] = { 0x471a1dcau };   /* N2_MAT_WINDSHIELD */
        SubSpec sub[1] = { {3, 0, 0, 0} };   /* count=3, mat=0, matid=0, start=0 */
        object(&f, "TESTCAR_KIT00_DOOR_LEFT_B", tex, 1, mat, 1, sub, 1, pos, 3, idx, 3);

        N2Scene sc; int n = n2_load_car(f.b, f.n, &sc, NULL, 0, NULL);
        printf("    emitted meshes: %d\n", n);
        chk("not split (single submesh, nothing to split from)", n == 1);
        chk("classified GLASS from the proven material hash, not BODY from the name",
            n == 1 && sc.meshes[0].cat == N2_CAR_GLASS);
        for (int i = 0; i < sc.count; i++) { free(sc.meshes[i].verts); free(sc.meshes[i].idx); free(sc.meshes[i].vcol); }
        free(sc.meshes);
    }

    /* -------------------------------------------------------------------
     * Fixture G (M135-R item 4): n2_car_dedupe_lod spatial-anchor hardening.
     * Drives n2_car_dedupe_lod directly (hand-built N2Mesh array, no chunk
     * parsing -- it only reads tierid/namekey/nverts/verts/nidx).
     *
     * G1 is the reviewer's literal scenario: three tiers A/B/C, A overlaps
     * B, B overlaps C, A does NOT overlap C, B has the highest score.
     * VERIFIED (scratchpad/m135/anchor_probe.c, run against both this
     * fixed header and the pre-fix 08a5edb header side by side): both
     * produce the IDENTICAL survivor set {B} for this exact input. That is
     * expected, not a test bug -- B, having beaten A, always reaches its
     * OWN outer-loop turn (nothing drops it before then), where it
     * independently and correctly rediscovers the true B-vs-C overlap
     * either way; `tiers[best].bb` is never a synthesized/wrong box, only
     * ever one tier's own real box, so an "early" comparison via a shifted
     * anchor can never produce a DIFFERENT true/false overlap verdict than
     * the same tier's own later turn would -- only a different TIMING. The
     * fixed anchor (tiers[a].bb) is still the right, defensible design
     * (matches "one stable spatial anchor per candidate family" and removes
     * a real risk for FUTURE edits, e.g. a non-monotonic drop rule), so it
     * is kept; this fixture freezes its actual, verified-safe behaviour on
     * the exact scenario named in review.
     *
     * G2 goes further: a 4-tier chain (A, B, D) plus an entirely
     * unconnected bystander (C, positioned far away) sharing the same
     * namekey. A overlaps B; B overlaps D; A does NOT overlap D; D has the
     * highest score, so within A's own scan `best` is reassigned TWICE
     * (A->B->D) before the scan ends. This is the shape most likely to
     * expose a real divergence if a future change ever makes the anchor
     * choice matter (e.g. a non-monotonic drop rule) -- and confirms today
     * that the always-unrelated bystander C survives untouched regardless,
     * and the true group champion D is the one kept from {A,B,D}.
     * ------------------------------------------------------------------- */
    printf("  G n2_car_dedupe_lod: spatial-anchor hardening (chained tiers)\n");
    {
        N2Scene sc; memset(&sc, 0, sizeof sc);
        sc.count = 3;
        sc.meshes = (N2Mesh *)malloc(3 * sizeof(N2Mesh));
        sc.meshes[0] = mk_lod_mesh(101, 0xB0D1u, 0, 10, 30);   /* A: X[0,10]  score 30 */
        sc.meshes[1] = mk_lod_mesh(102, 0xB0D1u, 5, 15, 60);   /* B: X[5,15]  score 60 (highest) */
        sc.meshes[2] = mk_lod_mesh(103, 0xB0D1u, 10, 20, 45);  /* C: X[10,20] score 45; touches A at x=10 (no overlap) */
        n2_car_dedupe_lod(&sc);
        printf("    G1 (literal reviewer scenario) survivors: %d\n", sc.count);
        chk("G1 only the highest-scoring, mutually-overlapping tier (B) survives",
            sc.count == 1 && has_tierid(&sc, 102));
        free_lod_scene(&sc);
    }
    {
        N2Scene sc; memset(&sc, 0, sizeof sc);
        sc.count = 4;
        sc.meshes = (N2Mesh *)malloc(4 * sizeof(N2Mesh));
        sc.meshes[0] = mk_lod_mesh(201, 0xC4A1u, 0,   10,  20);   /* A: X[0,10]    score 20 */
        sc.meshes[1] = mk_lod_mesh(202, 0xC4A1u, 5,   15,  50);   /* B: X[5,15]    score 50; overlaps A */
        sc.meshes[2] = mk_lod_mesh(203, 0xC4A1u, 100, 110, 15);   /* C: X[100,110] score 15; overlaps NOTHING */
        sc.meshes[3] = mk_lod_mesh(204, 0xC4A1u, 10,  20,  80);   /* D: X[10,20]   score 80 (highest); overlaps B, touches A (no overlap) */
        n2_car_dedupe_lod(&sc);
        printf("    G2 (4-tier chain + unrelated bystander) survivors: %d\n", sc.count);
        chk("G2 exactly two survivors", sc.count == 2);
        chk("G2 the unrelated bystander (C, tierid 203) is untouched",
            has_tierid(&sc, 203));
        chk("G2 the true chain champion (D, tierid 204) wins over A and B",
            has_tierid(&sc, 204) && !has_tierid(&sc, 201) && !has_tierid(&sc, 202));
        free_lod_scene(&sc);
    }

    texture_record_tests();
    sort_tests();
    sky_material_routing_test();
    world_material_routing_test();
    world_mesh_identity_test();

    printf("\n  %d passed, %d failed\n", PASS, FAIL);
    return FAIL ? 1 : 0;
}
