/* settings_test — runs the settings backend against docs/cleanroom-settings/test-vectors.
 *
 *   settings_test [VECTOR_DIR [SCRATCH_DIR]]
 *
 * Every vector file is read at run time (a small JSON reader is built in). Beyond the
 * vectors, the field-layout table tv06 drives a per-field read test and write test against the
 * synthetic profile (tv03): each field is patched in the file bytes and must appear in the
 * parsed state, and each field set in the parsed state must change exactly its own bytes.
 * Registry/file stores, profile files on disk and the option helpers are checked too.
 * Exit code is nonzero if any check fails. See docs/cleanroom-settings/IMPL_DESIGN.md. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "graphics_policy.h"

static int g_checks, g_fail;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    if (g_fail <= 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* ---- files ---------------------------------------------------------------------- */
static const char *g_dir = "docs/cleanroom-settings/test-vectors";
static const char *g_tmp = "build/settings_test_tmp";

static unsigned char *slurp(const char *name, size_t *len) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot read %s\n", path); g_fail++; *len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)n + 1);
    size_t got = fread(d, 1, (size_t)n, f);
    fclose(f);
    d[got] = 0;
    *len = got;
    return d;
}

/* ---- minimal JSON DOM ------------------------------------------------------------ */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct J { int t; double num; char *s; struct J **kid; char **key; int n, cap; } J;
static const char *jp;
static void jws(void) { while (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t') jp++; }
static J *jnew(int t) { J *j = (J *)calloc(1, sizeof *j); j->t = t; return j; }
static void jadd(J *c, char *key, J *v) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->kid = (J **)realloc(c->kid, (size_t)c->cap * sizeof *c->kid);
        c->key = (char **)realloc(c->key, (size_t)c->cap * sizeof *c->key);
    }
    c->kid[c->n] = v; c->key[c->n] = key; c->n++;
}
static char *jstr(void) {
    char *out = (char *)malloc(strlen(jp) + 1), *w = out;
    jp++;
    while (*jp && *jp != '"') {
        if (*jp == '\\' && jp[1]) { jp++; *w++ = *jp == 'n' ? '\n' : *jp == 't' ? '\t' : *jp; jp++; }
        else *w++ = *jp++;
    }
    if (*jp == '"') jp++;
    *w = 0;
    return out;
}
static J *jparse(void) {
    jws();
    if (*jp == '{' || *jp == '[') {
        int obj = *jp == '{';
        J *c = jnew(obj ? J_OBJ : J_ARR);
        char close = obj ? '}' : ']';
        jp++; jws();
        while (*jp && *jp != close) {
            char *k = NULL;
            if (obj) { jws(); k = jstr(); jws(); if (*jp == ':') jp++; }
            jadd(c, k, jparse());
            jws();
            if (*jp == ',') jp++;
            jws();
        }
        if (*jp == close) jp++;
        return c;
    }
    if (*jp == '"') { J *j = jnew(J_STR); j->s = jstr(); return j; }
    if (!strncmp(jp, "null", 4)) { jp += 4; return jnew(J_NULL); }
    if (!strncmp(jp, "true", 4)) { jp += 4; J *j = jnew(J_BOOL); j->num = 1; return j; }
    if (!strncmp(jp, "false", 5)) { jp += 5; return jnew(J_BOOL); }
    J *j = jnew(J_NUM);
    char *e;
    j->num = strtod(jp, &e);
    if (e == jp) e++;                       /* never loop on garbage */
    jp = e;
    return j;
}
static J *jload(const char *name) {
    size_t n;
    unsigned char *d = slurp(name, &n);
    if (!d) return NULL;
    jp = (const char *)d;
    J *j = jparse();
    j->s = (char *)d;                       /* root owns the text buffer (strings are copied) */
    return j;
}
static J *jget(const J *o, const char *k) {
    if (!o || o->t != J_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->key[i], k)) return o->kid[i];
    return NULL;
}
static void jfree(J *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) { jfree(j->kid[i]); free(j->key ? j->key[i] : NULL); }
    free(j->kid); free(j->key); free(j->s); free(j);
}
static unsigned long hexval(const char *s) { return strtoul(s, NULL, 16); }

/* ---- byte helpers ---------------------------------------------------------------- */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static float lef(const uint8_t *p) { uint32_t u = le32(p); float f; memcpy(&f, &u, 4); return f; }
static int unhex(const char *h, uint8_t *out) {
    int n = 0;
    for (; h[0] && h[1]; h += 2) {
        char b[3] = { h[0], h[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
    }
    return n;
}
static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* Independent CRC-32/BZIP2 (bitwise), for the closed-form identity of SPEC 3.4. */
static uint32_t crc_bzip2(const uint8_t *d, size_t n) {
    uint32_t r = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        r ^= (uint32_t)d[i] << 24;
        for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
    }
    return ~r;
}

/* Rebuilds a profile file from payload bytes (A = {version, size}). */
static size_t build_profile(uint8_t *out, size_t cap, const uint8_t *payload, uint32_t ver, uint32_t psize) {
    uint8_t a[8];
    put32(a, ver); put32(a + 4, psize);
    return st_container_build(out, cap, a, 8, payload, ST_PAYLOAD_SIZE);
}

/* ---- tv01 + tv02 -------------------------------------------------------------------- */
static void test_checksum(void) {
    J *j = jload("tv01_checksum.json");
    if (!j) return;
    J *cases = jget(j, "cases");
    CHECK(cases && cases->n >= 7, "tv01 has cases");
    for (int i = 0; cases && i < cases->n; i++) {
        J *c = cases->kid[i];
        uint8_t buf[512];
        int n = unhex(jget(c, "input_hex")->s, buf);
        CHECK(n == (int)jget(c, "length")->num, "tv01 %s length", jget(c, "name")->s);
        uint32_t want = (uint32_t)hexval(jget(c, "checksum")->s);
        CHECK(st_checksum(buf, (size_t)n) == want, "tv01 %s: got %08X want %08X",
              jget(c, "name")->s, st_checksum(buf, (size_t)n), want);
    }
    jfree(j);
    /* closed form: checksum(D) = CRC32_BZIP2(D[0..n-5]) ^ BE32(D[n-4..n-1]) (SPEC 3.4) */
    for (int n = 4; n < 200; n++) {
        uint8_t d[200];
        for (int i = 0; i < n; i++) d[i] = (uint8_t)rnd();
        uint32_t be = (uint32_t)d[n - 4] << 24 | d[n - 3] << 16 | d[n - 2] << 8 | d[n - 1];
        CHECK(st_checksum(d, (size_t)n) == (crc_bzip2(d, (size_t)n - 4) ^ be), "closed form n=%d", n);
    }
}

static void test_container_small(void) {
    size_t n;
    uint8_t *f = slurp("tv02_container_small.bin", &n);
    J *j = jload("tv02_tv03_containers.json");
    if (!f || !j) return;
    J *h = jget(jget(j, "tv02_container_small.bin"), "header");
    StContainerView v;
    CHECK(st_container_parse(f, n, &v) == ST_OK, "tv02 parses");
    CHECK(n == (size_t)jget(h, "total")->num, "tv02 size");
    CHECK(v.size_a == jget(h, "sizeA")->num && v.size_b == jget(h, "sizeB")->num, "tv02 sizes");
    CHECK(le32(f + 16) == hexval(jget(h, "crcA")->s), "tv02 crcA");
    CHECK(le32(f + 20) == hexval(jget(h, "crcB")->s), "tv02 crcB");
    CHECK(le32(f + 24) == hexval(jget(h, "crcHeader")->s), "tv02 crcHeader");
    CHECK(v.size_b == 14 && !memcmp(v.b, "hello-settings", 14), "tv02 block B");
    CHECK(v.size_a == 5 && !memcmp(v.a, "\x01\x02\x03\x04\x05", 5), "tv02 block A");
    uint8_t out[128];
    size_t m = st_container_build(out, sizeof out, v.a, v.size_a, v.b, v.size_b);
    CHECK(m == n && !memcmp(out, f, n), "tv02 rebuild is byte-identical");
    CHECK(st_container_build(out, n - 1, v.a, v.size_a, v.b, v.size_b) == 0, "build rejects small buffer");
    /* every single-bit flip must be rejected */
    int rejected = 0;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            uint8_t g[64];
            memcpy(g, f, n);
            g[i] ^= (uint8_t)(1 << b);
            if (st_container_parse(g, n, &v) != ST_OK) rejected++;
        }
    CHECK(rejected == (int)n * 8, "tv02 single-bit flips rejected (%d/%d)", rejected, (int)n * 8);
    CHECK(st_container_parse(f, n - 1, &v) == ST_E_CORRUPT, "tv02 truncated -> corrupt");
    CHECK(st_container_parse(f, 27, &v) == ST_E_IO, "<28 bytes -> I/O error");
    CHECK(st_container_parse(f, 0, &v) == ST_E_IO, "empty -> I/O error");
    /* sizes that run past the file with consistent header: read failure (3.5 step 4) */
    uint8_t g[ST_CONTAINER_HEADER];
    memcpy(g, "20CM", 4); put32(g + 4, 28); put32(g + 8, 10); put32(g + 12, 0);
    put32(g + 16, 0); put32(g + 20, 0); put32(g + 24, st_checksum(g, 24));
    CHECK(st_container_parse(g, 28, &v) == ST_E_IO, "block sizes beyond file -> I/O error");
    /* empty blocks are valid; their checksum is 0 */
    size_t e = st_container_build(out, sizeof out, NULL, 0, NULL, 0);
    CHECK(e == 28 && st_container_parse(out, e, &v) == ST_OK && v.size_a == 0 && v.size_b == 0, "empty blocks");
    free(f);
    jfree(j);
}

/* ---- tv03 / tv03b / tv06 ---------------------------------------------------------- */
typedef enum { T_U8, T_U32, T_F32 } Ty;
typedef struct { const char *name; size_t off; Ty ty; } Fld;
#define F(n, m, t) { n, offsetof(StProfile, m), t }
#define P(i, n, m, t) { "player" #i "." n, offsetof(StProfile, player[(i) - 1].m), t }
#define PL(i) \
    P(i, "gauges", gauges, T_U8), P(i, "race_position", race_position, T_U8), \
    P(i, "lap_info", lap_info, T_U8), P(i, "score", score, T_U8), P(i, "feedback", feedback, T_U8), \
    P(i, "leaderboard", leaderboard, T_U8), P(i, "ask_for_transmission", ask_transmission, T_U8), \
    P(i, "controller_config_index", controller_config, T_U32), P(i, "unknown_12", unk12, T_U8), \
    P(i, "camera", camera, T_U32), P(i, "split_time", split_time, T_U8), \
    P(i, "transmission", transmission, T_U8), P(i, "stability_control", stability_control, T_U8), \
    P(i, "unknown_23", unk23, T_U8), P(i, "unknown_24", unk24, T_U8), P(i, "unknown_25", unk25, T_U8), \
    P(i, "wheel_range", wheel_range, T_F32), P(i, "road_feedback_stored", road_feedback, T_F32), \
    P(i, "force_strength", force_strength, T_F32)
static const Fld k_fields[] = {
    F("master_volume", audio.master, T_F32), F("speech_movie_volume", audio.speech_movie, T_F32),
    F("menu_music_volume", audio.menu_music, T_F32), F("game_music_volume", audio.game_music, T_F32),
    F("sfx_volume", audio.sfx, T_F32), F("engine_volume", audio.engine, T_F32),
    F("tire_volume", audio.tire, T_F32), F("audio_unknown_28", audio.unk28, T_F32),
    F("audio_unknown_32", audio.unk32, T_F32), F("audio_unknown_36", audio.unk36, T_F32),
    F("audio_mode", audio.mode, T_U32), F("audio_unknown_44", audio.unk44, T_U32),
    F("trax_play_order", audio.trax_order, T_U32), F("audio_unknown_52", audio.unk52, T_U32),
    F("audio_unknown_56", audio.unk56, T_U32),
    F("rearview_mirror", gameplay.rearview_mirror, T_U8), F("gameplay_unknown_1", gameplay.unk1, T_U8),
    F("speedo_units", gameplay.speedo_units, T_U8), F("race_map_mode", gameplay.race_map_mode, T_U8),
    F("explore_map_mode", gameplay.explore_map_mode, T_U8), F("jump_camera", gameplay.jump_camera, T_F32),
    F("gameplay_unknown_12", gameplay.unk12, T_F32),
    PL(1), PL(2),
    F("unknown_block_0", unkblk_f, T_F32), F("unknown_block_4", unkblk_a, T_U32),
    F("unknown_block_8", unkblk_b, T_U32), F("unknown_block_12", unkblk_c, T_U8),
    F("career_difficulty", career_difficulty, T_U32), F("autosave", autosave, T_U8),
    F("save_prompt", save_prompt, T_U8),
    F("show_player_names", online.show_player_names, T_U8),
    F("bad_connection_warning", online.bad_connection_warning, T_U8),
    F("online_unknown_10", online.unk10, T_U8),
};
static int fsize(Ty t) { return t == T_U8 ? 1 : 4; }

static double fld_get(const StProfile *p, const Fld *f) {
    const uint8_t *q = (const uint8_t *)p + f->off;
    if (f->ty == T_U8) return *q;
    if (f->ty == T_U32) { uint32_t u; memcpy(&u, q, 4); return u; }
    float x; memcpy(&x, q, 4); return x;
}
static void fld_set(StProfile *p, const Fld *f, double v) {
    uint8_t *q = (uint8_t *)p + f->off;
    if (f->ty == T_U8) *q = (uint8_t)v;
    else if (f->ty == T_U32) { uint32_t u = (uint32_t)v; memcpy(q, &u, 4); }
    else { float x = (float)v; memcpy(q, &x, 4); }
}
static double raw_get(const uint8_t *file, long off, Ty t) {
    if (t == T_U8) return file[off];
    if (t == T_U32) return le32(file + off);
    return lef(file + off);
}
static Ty ty_of(const char *s) { return !strcmp(s, "u8") ? T_U8 : !strcmp(s, "u32") ? T_U32 : T_F32; }
static double distinct(double v, Ty t) {
    if (t == T_U8) return (double)(((int)v + 0x37) & 0xFF);
    if (t == T_U32) return (double)((uint32_t)v ^ 0x01020304u);
    return v + 0.3125;
}
/* Number of differing bytes outside [lo,hi) ignoring the 28-byte header (checksums). */
static int diff_outside(const uint8_t *a, const uint8_t *b, size_t n, long lo, long hi) {
    int c = 0;
    for (size_t i = ST_CONTAINER_HEADER; i < n; i++)
        if (a[i] != b[i] && ((long)i < lo || (long)i >= hi)) c++;
    return c;
}
static void repack(uint8_t *file) {   /* recompute checksums after patching payload bytes in place */
    uint8_t tmp[ST_PROFILE_FILE_SIZE];
    size_t n = build_profile(tmp, sizeof tmp, file + 36, ST_PROFILE_VERSION, ST_PAYLOAD_SIZE);
    if (n == sizeof tmp) memcpy(file, tmp, n);
}

static void test_profile_vectors(void) {
    size_t n, nb;
    uint8_t *f = slurp("tv03_synthetic_profile_defaults.bin", &n);
    uint8_t *fb = slurp("tv03b_synthetic_profile_bad_payload_crc.bin", &nb);
    J *cj = jload("tv02_tv03_containers.json");
    J *lay = jload("tv06_profile_settings_layout.json");
    if (!f || !fb || !cj || !lay) return;
    static StProfile P0, Q;
    J *c3 = jget(cj, "tv03_synthetic_profile_defaults.bin");

    CHECK(n == ST_PROFILE_FILE_SIZE && n == (size_t)jget(c3, "size")->num, "tv03 size %zu", n);
    CHECK(le32(f + 16) == hexval(jget(c3, "crcA")->s), "tv03 crcA in file");
    CHECK(le32(f + 20) == hexval(jget(c3, "crcB")->s), "tv03 crcB in file");
    CHECK(le32(f + 24) == hexval(jget(c3, "crcHeader")->s), "tv03 crcHeader in file");
    StContainerView v;
    CHECK(st_container_parse(f, n, &v) == ST_OK, "tv03 container valid");
    CHECK(v.size_a == 8 && le32(v.a) == 0x10D && le32(v.a + 4) == 54930, "tv03 block A = {0x10D, 54930}");
    CHECK(st_profile_parse(f, n, &P0) == ST_OK, "tv03 profile parses");
    CHECK(!strcmp(P0.name, "TESTPROFILE"), "profile name '%s'", P0.name);
    CHECK(P0.gameplay.speedo_units == 0, "units MPH");

    /* byte-exact round trip, and defaults + name reproduce the vector */
    static uint8_t out[ST_PROFILE_FILE_SIZE];
    CHECK(st_profile_serialize(&P0, out, sizeof out) == n && !memcmp(out, f, n), "tv03 round trip identical");
    CHECK(st_profile_serialize(&P0, out, n - 1) == 0, "serialize rejects short buffer");
    st_profile_defaults(&Q, 0);
    strcpy(Q.name, "TESTPROFILE");
    CHECK(st_profile_serialize(&Q, out, sizeof out) == n && !memcmp(out, f, n),
          "st_profile_defaults(+name) reproduces tv03 byte for byte");
    st_profile_defaults(&Q, 1);
    CHECK(Q.gameplay.speedo_units == 1, "non-English language defaults to km/h");
    st_profile_defaults(&Q, 0);
    CHECK(Q.gameplay.speedo_units == 0, "English defaults to MPH");

    /* tv03b: only block B checksum fails */
    CHECK(nb == n, "tv03b size");
    CHECK(le32(fb + 24) == st_checksum(fb, 24), "tv03b header checksum ok");
    CHECK(le32(fb + 16) == st_checksum(fb + 28, 8), "tv03b block A checksum ok");
    CHECK(le32(fb + 20) != st_checksum(fb + 36, ST_PAYLOAD_SIZE), "tv03b block B checksum bad");
    CHECK(st_profile_parse(fb, nb, &Q) == ST_E_CORRUPT, "tv03b rejected as corrupt");

    /* layout table: sizes */
    CHECK((size_t)jget(lay, "file_size")->num == n && (uint32_t)jget(lay, "payload_size")->num == ST_PAYLOAD_SIZE, "tv06 sizes");
    CHECK((uint32_t)jget(jget(lay, "block_A"), "version")->num == ST_PROFILE_VERSION, "tv06 version");

    /* layout table: every field */
    J *fl = jget(lay, "fields");
    int mapped = 0, trax = 0, zero_only = 0;
    for (int i = 0; i < fl->n; i++) {
        J *e = fl->kid[i];
        const char *name = jget(e, "name")->s, *tys = jget(e, "type")->s;
        long poff = (long)jget(e, "payload_offset")->num, foff = (long)jget(e, "file_offset")->num;
        J *def = jget(e, "default");
        CHECK(foff == poff + 36, "%s: file offset = payload offset + 36", name);
        if (!strcmp(tys, "bytes[1086]") || !strcmp(tys, "char[32]")) continue;   /* below */
        Ty ty = ty_of(tys);
        double v0 = raw_get(f, foff, ty);
        if (def && def->t == J_NUM) CHECK(v0 == (ty == T_F32 ? (double)(float)def->num : def->num),
                                          "%s: vector value %g vs listed default %g", name, v0, def->num);
        const Fld *fld = NULL;
        Fld tx;
        int ti = -1;
        char which[8];
        if (sscanf(name, "trax[%d].%7s", &ti, which) == 2) {
            CHECK(ti >= 0 && ti < ST_TRAX_COUNT, "%s: index", name);
            tx.name = name;
            tx.ty = !strcmp(which, "track") ? T_U32 : T_U8;
            tx.off = !strcmp(which, "track") ? offsetof(StProfile, trax[0].track) + (size_t)ti * sizeof(StTraxEntry)
                                             : offsetof(StProfile, trax[0].mode) + (size_t)ti * sizeof(StTraxEntry);
            fld = &tx;
            trax++;
        } else {
            for (size_t k = 0; k < sizeof k_fields / sizeof *k_fields; k++)
                if (!strcmp(k_fields[k].name, name)) fld = &k_fields[k];
        }
        if (!fld) {   /* online reserved words: always written as zero, ignored on read */
            CHECK(!strncmp(name, "online_reserved", 15) && v0 == 0, "%s: unmapped field", name);
            zero_only++;
            continue;
        }
        mapped++;
        CHECK(fld->ty == ty, "%s: type %s", name, tys);
        /* read: parsed state matches the vector bytes */
        CHECK(fld_get(&P0, fld) == v0, "%s: parsed %g vs file %g", name, fld_get(&P0, fld), v0);
        /* read: patch the file, state follows */
        static uint8_t g[ST_PROFILE_FILE_SIZE];
        memcpy(g, f, n);
        double nv = distinct(v0, ty);
        if (ty == T_U8) g[foff] = (uint8_t)nv; else if (ty == T_U32) put32(g + foff, (uint32_t)nv);
        else { float x = (float)nv; uint32_t u; memcpy(&u, &x, 4); put32(g + foff, u); }
        repack(g);
        CHECK(st_profile_parse(g, n, &Q) == ST_OK, "%s: patched file parses", name);
        CHECK(fld_get(&Q, fld) == (ty == T_F32 ? (double)(float)nv : nv), "%s: patched value read back", name);
        /* write: setting the field changes exactly its bytes */
        Q = P0;
        fld_set(&Q, fld, nv);
        CHECK(st_profile_serialize(&Q, out, sizeof out) == n, "%s: serialize", name);
        CHECK(diff_outside(out, f, n, foff, foff + fsize(ty)) == 0, "%s: write touches only its own bytes", name);
        CHECK(raw_get(out, foff, ty) == (ty == T_F32 ? (double)(float)nv : nv), "%s: written value", name);
        CHECK(st_profile_parse(out, n, &Q) == ST_OK, "%s: written file parses", name);
    }
    CHECK(trax == 54, "tv06 lists 27 playlist entries (%d fields)", trax);
    CHECK(mapped >= 120 && mapped + zero_only + 2 == fl->n, "tv06 field coverage mapped=%d zero=%d total=%d", mapped, zero_only, fl->n);

    /* opaque regions: arbitrary content survives parse -> change a setting -> serialize */
    J *op = jget(lay, "opaque_regions_payload_offset_size");
    CHECK(op && op->n == 6, "six opaque regions");
    static uint8_t g[ST_PROFILE_FILE_SIZE];
    memcpy(g, f, n);
    for (int i = 0; i < op->n; i++) {
        long off = (long)op->kid[i]->kid[0]->num + 36, len = (long)op->kid[i]->kid[1]->num;
        for (long k = 0; k < len; k++) g[off + k] = (uint8_t)rnd();
    }
    for (int i = 0; i < 40; i++) g[ST_CONTAINER_HEADER + 8 + 50211 + 5 + (i % 3)] = (uint8_t)rnd();  /* trax padding */
    repack(g);
    CHECK(st_profile_parse(g, n, &Q) == ST_OK, "profile with filled opaque regions parses");
    CHECK(st_profile_serialize(&Q, out, sizeof out) == n && !memcmp(out, g, n), "opaque regions + padding preserved byte for byte");
    Q.audio.master = 0.25f;
    st_profile_serialize(&Q, out, sizeof out);
    CHECK(diff_outside(out, g, n, 36 + 50427, 36 + 50431) == 0, "editing a setting leaves opaque bytes alone");

    /* name field and the input-device block */
    st_profile_defaults(&Q, 0);
    strcpy(Q.name, "AB");
    st_profile_serialize(&Q, out, sizeof out);
    CHECK(!memcmp(out + 36 + 53761, "AB", 3) && out[36 + 53761 + 31] == 0 && out[36 + 53761 + 5] == 0, "name NUL-padded");
    for (int s = 0; s < ST_INPUT_SLOTS; s++) {
        StInputDevice *d = &Q.input[s];
        for (int k = 0; k < 16; k++) d->guid[k] = (uint8_t)(s * 16 + k + 1);
        for (int k = 0; k < 4; k++) { d->axis[k] = s * 10 + k; d->dir[k] = (k & 1) ? -1 : 1; d->assigned[k] = (uint8_t)(k & 1); }
        d->combined_mode = (uint8_t)(s & 1);
        for (int k = 0; k < 32; k++) d->binding[k] = s * 1000 + k - 5;
    }
    st_profile_serialize(&Q, out, sizeof out);
    const uint8_t *r5 = out + 36 + 53844 + 5 * 181;
    CHECK(r5[0] == 81 && le32(r5 + 16) == 50 && (int32_t)le32(r5 + 36) == -1 && r5[48] == 0 && r5[49] == 1 &&
          r5[52] == 1 && (int32_t)le32(r5 + 53 + 4 * 31) == 5026, "input record layout (stride 181)");
    CHECK(36 + 53844 + 6 * 181 == (int)n, "input block ends at end of file");
    static StProfile R;
    CHECK(st_profile_parse(out, n, &R) == ST_OK && !memcmp(R.input, Q.input, sizeof Q.input) && !strcmp(R.name, "AB"),
          "input records round trip");
    CHECK(st_input_find(&R, Q.input[3].guid) == 3, "st_input_find matches a stored GUID");
    uint8_t zero[16] = { 0 };
    CHECK(st_input_find(&P0, zero) == -1, "zeroed slots never match");

    /* rejections */
    CHECK(st_profile_parse(f, n - 1, &Q) == ST_E_CORRUPT, "truncated profile -> corrupt");
    uint8_t *ext = (uint8_t *)malloc(n + 1);
    memcpy(ext, f, n); ext[n] = 0;
    CHECK(st_profile_parse(ext, n + 1, &Q) == ST_E_CORRUPT, "extended profile -> corrupt");
    free(ext);
    int bad = 0, tot = 0;
    for (size_t i = 0; i < n; i += (i < 64 ? 1 : 997)) {
        memcpy(g, f, n);
        g[i] ^= 0x10;
        tot++;
        if (st_profile_parse(g, n, &Q) == ST_E_CORRUPT) bad++;
    }
    CHECK(bad == tot, "bit flips rejected as corrupt (%d/%d)", bad, tot);
    {   /* flip inside Master Volume in the payload = tv03b recipe */
        memcpy(g, f, n); g[36 + 50427] ^= 1;
        CHECK(st_profile_parse(g, n, &Q) == ST_E_CORRUPT, "payload flip -> corrupt");
    }
    uint8_t payload[ST_PAYLOAD_SIZE];
    memcpy(payload, f + 36, sizeof payload);
    size_t m = build_profile(g, sizeof g, payload, 0x10E, ST_PAYLOAD_SIZE);
    CHECK(m == n && st_profile_parse(g, m, &Q) == ST_E_INCOMPATIBLE, "version != 0x10D -> incompatible");
    m = build_profile(g, sizeof g, payload, ST_PROFILE_VERSION, ST_PAYLOAD_SIZE - 1);
    CHECK(st_profile_parse(g, m, &Q) == ST_E_INCOMPATIBLE, "payload size field != 54930 -> incompatible");
    uint8_t a8[8];
    put32(a8, ST_PROFILE_VERSION); put32(a8 + 4, ST_PAYLOAD_SIZE);
    m = st_container_build(g, sizeof g, a8, 8, payload, ST_PAYLOAD_SIZE - 4);
    CHECK(st_profile_parse(g, m, &Q) == ST_E_INCOMPATIBLE, "short block B -> incompatible");
    m = st_container_build(g, sizeof g, a8, 4, payload, ST_PAYLOAD_SIZE);
    CHECK(st_profile_parse(g, m, &Q) == ST_E_INCOMPATIBLE, "short block A -> incompatible");

    /* unsaved-changes detection (3.6) */
    static uint8_t snap[ST_PAYLOAD_SIZE];
    st_profile_snapshot(&P0, snap);
    CHECK(!st_profile_modified(&P0, snap), "unchanged -> not modified");
    Q = P0; Q.audio.sfx = 0.5f;
    CHECK(st_profile_modified(&Q, snap), "option change -> modified");

    free(f); free(fb); jfree(cj); jfree(lay);
}

/* ---- option helpers ----------------------------------------------------------------- */
static void test_helpers(void) {
    CHECK(st_jump_camera_toggle(0.0f) == 127.0f && st_jump_camera_toggle(127.0f) == 0.0f &&
          st_jump_camera_toggle(127.5f) == 0.0f, "jump camera toggle");
    CHECK(st_road_feedback_from_slider(0) == 20.0f && st_road_feedback_from_slider(1) == 2.0f &&
          st_road_feedback_from_slider(2) == 2.0f && st_road_feedback_from_slider(-1) == 20.0f, "road feedback write dir + clamp");
    CHECK(fabsf(st_road_feedback_from_slider(0.1f) - 18.2f) < 1e-4f, "road feedback 0.1 -> 18.2");
    CHECK(st_road_feedback_to_slider(20) == 0.0f && st_road_feedback_to_slider(2) == 1.0f &&
          st_road_feedback_to_slider(100) == 0.0f && st_road_feedback_to_slider(-5) == 1.0f, "road feedback read dir + clamp");
    CHECK(fabsf(st_road_feedback_to_slider(12.4f) - (1.0f - 10.4f / 18.0f)) < 1e-5f, "default road feedback maps inside slider");
    CHECK(st_career_difficulty_step(0, 1) == 1 && st_career_difficulty_step(2, 1) == 0 &&
          st_career_difficulty_step(0, -1) == 2 && st_career_difficulty_step(1, -1) == 0, "difficulty wraps");
    CHECK(st_trax_mode_step(0, 1) == 1 && st_trax_mode_step(3, 1) == 0 && st_trax_mode_step(0, -1) == 3 &&
          st_trax_mode_step(2, -1) == 1, "trax mode cycles 0..3");

    StProfile p;
    st_profile_defaults(&p, 0);
    uint32_t fe, ig;
    st_trax_masks(&p, &fe, &ig);
    CHECK(fe == 0x7FFFFFFu && ig == 0x7FFFFFFu, "default masks: all 27 tracks");
    for (int i = 0; i < ST_TRAX_COUNT; i++) p.trax[i].mode = 0;
    p.trax[0].track = 5; p.trax[0].mode = 1;
    p.trax[1].track = 6; p.trax[1].mode = 2;
    p.trax[2].track = 7; p.trax[2].mode = 0;
    st_trax_masks(&p, &fe, &ig);
    CHECK(!(fe & (1u << 7)) && !(ig & (1u << 7)) && (fe & (1u << 5)) && !(ig & (1u << 5)) &&
          !(fe & (1u << 6)) && (ig & (1u << 6)), "masks follow track id and mode bits");

    /* save option (5.8) */
    st_save_set_mode(&p, ST_SAVE_AUTOSAVE);
    CHECK(p.autosave == 1 && p.save_prompt == 1 && st_save_mode(&p) == ST_SAVE_AUTOSAVE, "autosave sets both bytes");
    st_save_set_mode(&p, ST_SAVE_PROMPT);
    CHECK(p.autosave == 0 && p.save_prompt == 1 && st_save_mode(&p) == ST_SAVE_PROMPT, "prompt");
    st_save_set_mode(&p, ST_SAVE_OFF);
    CHECK(p.autosave == 0 && p.save_prompt == 0 && st_save_mode(&p) == ST_SAVE_OFF, "off");
    p.autosave = 1; p.save_prompt = 0;
    CHECK(st_save_mode(&p) == ST_SAVE_AUTOSAVE, "autosave wins whatever prompt says");
    st_save_set_mode(&p, ST_SAVE_OFF);
    CHECK(st_save_cycle(&p, 1, 1) == ST_SAVE_AUTOSAVE && st_save_cycle(&p, 1, 1) == ST_SAVE_PROMPT &&
          st_save_cycle(&p, 1, 1) == ST_SAVE_OFF, "cycle right Off>Autosave>Prompt>Off");
    CHECK(st_save_cycle(&p, -1, 1) == ST_SAVE_PROMPT && st_save_cycle(&p, -1, 1) == ST_SAVE_AUTOSAVE &&
          st_save_cycle(&p, -1, 1) == ST_SAVE_OFF, "cycle left Off>Prompt>Autosave>Off");
    CHECK(st_save_cycle(&p, 1, 0) == ST_SAVE_PROMPT && st_save_cycle(&p, 1, 0) == ST_SAVE_OFF, "right skips autosave");
    CHECK(st_save_cycle(&p, -1, 0) == ST_SAVE_PROMPT && st_save_cycle(&p, -1, 0) == ST_SAVE_OFF, "left skips autosave");

    CHECK(st_profile_name_valid("TESTPROFILE") && st_profile_name_valid("a") && st_profile_name_valid("123456789012345"),
          "valid names");
    CHECK(!st_profile_name_valid("") && !st_profile_name_valid("1234567890123456") && !st_profile_name_valid("a/b") &&
          !st_profile_name_valid("a\\b") && !st_profile_name_valid("..") && !st_profile_name_valid("a:b") &&
          !st_profile_name_valid("a*") && !st_profile_name_valid("x ") && !st_profile_name_valid(NULL),
          "invalid names");
}

/* ---- tv04 / tv05 / graphics logic ------------------------------------------------------ */
static int slot_of(const char *key) {
    char full[96];
    snprintf(full, sizeof full, "g_%s", key);
    for (int s = 0; s < ST_GFX_NSLOTS; s++) {
        const char *n = st_gfx_value_name(s);
        if (n && !strcmp(n, full)) return s;
    }
    return -1;
}

static void test_presets(void) {
    J *j = jload("tv04_video_presets.json");
    if (!j) return;
    StGfxCaps caps;
    for (int i = 0; i < ST_GFX_NSLOTS; i++) caps.cap[i] = ST_NO_CAP;
    for (int i = 0; i < 6; i++) caps.resolution_ok[i] = 1;
    J *fc = jget(j, "FULL_CAPS");
    for (int i = 0; i < fc->n; i++) {
        int s = slot_of(fc->key[i]);
        CHECK(s >= 0, "FULL_CAPS key %s known", fc->key[i]);
        if (s >= 0) caps.cap[s] = (int32_t)fc->kid[i]->num;
    }
    StGfxCaps full;
    st_gfx_caps_full(&full, 5);
    int same = 1;
    for (int i = 0; i < ST_GFX_NSLOTS; i++)
        if (i != ST_GFX_SLOT25 && full.cap[i] != caps.cap[i]) { same = 0; printf("cap mismatch slot %d\n", i); }
    CHECK(same, "st_gfx_caps_full(5 levels) equals FULL_CAPS");
    CHECK(full.cap[ST_GFX_SLOT25] == 1, "slot 25 capped to 1");

    J *cases = jget(j, "cases");
    CHECK(cases->n == 12, "12 preset cases");
    for (int start = 0; start < 2; start++)
        for (int c = 0; c < cases->n; c++) {
            J *cs = cases->kid[c];
            int level = (int)jget(cs, "performance_level")->num;
            double gpu = jget(cs, "gpu_score")->num;
            StGraphics g;
            st_gfx_defaults(&g);
            if (start) for (int i = 2; i < ST_GFX_NSLOTS; i++) g.v[i] = 7;     /* stale values */
            if (start) { g.v[ST_GFX_ANIMATED_TEXTURE] = 1; g.v[ST_GFX_RESOLUTION] = 3; }
            st_gfx_autodetect(&g, &caps, level, gpu);
            J *res = jget(cs, "result");
            for (int k = 0; k < res->n; k++) {
                int s = slot_of(res->key[k]);
                CHECK(s >= 0 && g.v[s] == (int32_t)res->kid[k]->num, "tv04 level %d gpu %.0f start %d: %s = %d want %d",
                      level, gpu, start, res->key[k], s >= 0 ? g.v[s] : -99, (int)res->kid[k]->num);
            }
            CHECK(g.v[ST_GFX_PERF_LEVEL] == level, "autodetect records the level");
        }
    jfree(j);

    j = jload("tv05_autodetect.json");
    if (!j) return;
    cases = jget(j, "cases");
    for (int c = 0; c < cases->n; c++) {
        J *cs = cases->kid[c];
        int mhz = (int)jget(cs, "cpu_mhz")->num;
        double gpu = jget(cs, "gpu_score")->num;
        int want = (int)jget(cs, "performance_level")->num;
        CHECK(st_perf_level(mhz, gpu) == want, "tv05 mhz %d gpu %.1f: got %d want %d", mhz, gpu, st_perf_level(mhz, gpu), want);
    }
    jfree(j);
}

static void test_graphics_logic(void) {
    StGfxCaps caps;
    st_gfx_caps_full(&caps, 5);
    StGraphics g;
    st_gfx_defaults(&g);
    CHECK(g.v[ST_GFX_VERSION] == 1 && g.v[ST_GFX_SIZE] == 120 && g.v[ST_GFX_ANIMATED_TEXTURE] == 1 &&
          g.v[ST_GFX_SLOT25] == 1 && g.v[ST_GFX_TEXTURE_FILTERING] == 1 && g.v[ST_GFX_FOG] == 0, "static defaults");
    /* clamp: lowered, never raised, negatives survive */
    g.v[ST_GFX_FOG] = 5; g.v[ST_GFX_CAR_SHADOW] = -3; g.v[ST_GFX_CAR_LIGHTING] = 1; g.v[ST_GFX_FSAA] = 9;
    st_gfx_clamp(&g, &caps);
    CHECK(g.v[ST_GFX_FOG] == 1 && g.v[ST_GFX_CAR_SHADOW] == -3 && g.v[ST_GFX_CAR_LIGHTING] == 0 && g.v[ST_GFX_FSAA] == 4,
          "clamp lowers only");
    /* resolution step-down */
    caps.resolution_ok[5] = caps.resolution_ok[4] = 0;
    g.v[ST_GFX_RESOLUTION] = 5;
    st_gfx_clamp(&g, &caps);
    CHECK(g.v[ST_GFX_RESOLUTION] == 3, "unsupported resolution steps down to highest supported");
    for (int i = 0; i < 6; i++) caps.resolution_ok[i] = 0;
    g.v[ST_GFX_RESOLUTION] = 2;
    st_gfx_clamp(&g, &caps);
    CHECK(g.v[ST_GFX_RESOLUTION] == 2, "no supported resolution: unchanged");
    st_gfx_caps_full(&caps, 5);
    caps.resolution_ok[1] = 0; caps.resolution_ok[3] = 0;
    g.v[ST_GFX_RESOLUTION] = 2;
    CHECK(st_resolution_step(&g, &caps, 1) && g.v[ST_GFX_RESOLUTION] == 4, "step right skips unsupported");
    CHECK(st_resolution_step(&g, &caps, -1) && g.v[ST_GFX_RESOLUTION] == 2, "step left");
    CHECK(st_resolution_step(&g, &caps, -1) && g.v[ST_GFX_RESOLUTION] == 0, "step left skips unsupported");
    CHECK(!st_resolution_step(&g, &caps, -1) && g.v[ST_GFX_RESOLUTION] == 0, "no wrap at 0");
    g.v[ST_GFX_RESOLUTION] = 5;
    CHECK(!st_resolution_step(&g, &caps, 1) && g.v[ST_GFX_RESOLUTION] == 5, "no wrap at 5");
    int w, h;
    CHECK(st_resolution_size(0, &w, &h) && w == 640 && h == 480 && st_resolution_size(5, &w, &h) && w == 1600 && h == 1200 &&
          st_resolution_size(4, &w, &h) && w == 1280 && h == 1024 && st_resolution_size(3, &w, &h) && h == 960 &&
          !st_resolution_size(6, &w, &h), "resolution table");
    /* slider: level change keeps resolution */
    st_gfx_caps_full(&caps, 5);
    st_gfx_defaults(&g);
    g.v[ST_GFX_RESOLUTION] = 4;
    st_gfx_set_level(&g, &caps, 5, 5.0);
    CHECK(g.v[ST_GFX_PERF_LEVEL] == 5 && g.v[ST_GFX_RESOLUTION] == 1 && g.v[ST_GFX_FSAA] == 4, "level 5 slider + gpu>4 resolution");
    st_gfx_set_level(&g, &caps, 3, 5.0);
    CHECK(g.v[ST_GFX_RESOLUTION] == 1 && g.v[ST_GFX_FSAA] == 0 && g.v[ST_GFX_ROAD_REFLECTION] == 2, "level 3 keeps resolution");
    g.v[ST_GFX_RESOLUTION] = 4;
    st_gfx_set_level(&g, &caps, 4, 3.0);
    CHECK(g.v[ST_GFX_RESOLUTION] == 4, "level 4, gpu<=4: resolution unchanged");
    CHECK(st_audio_rate_hz(0) == 22050 && st_audio_rate_hz(1) == 22050 && st_audio_rate_hz(2) == 44100 &&
          st_audio_rate_hz(5) == 44100, "audio rate follows level");
    char *a1[] = { "game", "/s" }, *a2[] = { "game", "/S" }, *a3[] = { "game", "-s" }, *a4[] = { "game" }, *a5[] = { "game", "x", "/s" };
    CHECK(st_parse_safe_mode(2, a1) && st_parse_safe_mode(2, a2) && !st_parse_safe_mode(2, a3) &&
          !st_parse_safe_mode(1, a4) && !st_parse_safe_mode(3, a5), "safe-mode argument is the first argument");
}

/* ---- stores ---------------------------------------------------------------------------- */
static void test_install_and_language(void) {
    CHECK(st_language_index("English US") == 0 && st_language_index("english uk") == 0 && st_language_index("French") == 1 &&
          st_language_index("GERMAN") == 2 && st_language_index("Italian") == 3 && st_language_index("Spanish") == 4 &&
          st_language_index("Dutch") == 5 && st_language_index("Swedish") == 6 && st_language_index("Danish") == 7 &&
          st_language_index("Korean") == 8 && st_language_index("Chinese (Traditional)") == 9 &&
          st_language_index("English China") == 9 && st_language_index("Japanese") == 10 && st_language_index("Thai") == 11 &&
          st_language_index("Klingon") == -1 && st_language_index("") == -1, "language table");
    StRegStore s;
    CHECK(st_reg_open_memory(&s) == ST_OK, "open memory store");
    StInstall in;
    st_install_read(&s, &in);
    CHECK(!in.not_first_time && in.language_index == 0 && in.streaming_install == 1 && in.cache_size == 0 &&
          !in.install_dir[0], "empty store: first run, language 0, streaming on");
    st_reg_set_string(&s, "Install Dir", "C:\\Games\\NFSU2");
    st_reg_set_string(&s, "CD Drive", "D:\\");
    st_reg_set_string(&s, "CacheSize", "128abc");
    st_reg_set_string(&s, "SwapSize", "junk");
    st_reg_set_string(&s, "Language", "Dutch");
    st_reg_set_string(&s, "StreamingInstall", "OFF");
    st_reg_set_dword(&s, "NotFirstTime", 1);
    st_install_read(&s, &in);
    CHECK(!strcmp(in.install_dir, "C:\\Games\\NFSU2") && !strcmp(in.cd_drive, "D:\\") && in.cache_size == 128 &&
          in.swap_size == 0 && in.language_index == 5 && in.streaming_install == 0 && in.not_first_time, "install values");
    st_reg_set_string(&s, "Language", "Esperanto");
    st_reg_set_string(&s, "StreamingInstall", "on");
    st_install_read(&s, &in);
    CHECK(in.language_index == -1 && in.streaming_install == 1, "unknown language -> -1; other streaming value -> on");
    st_reg_close(&s);
    st_reg_open_memory(&s);
    CHECK(st_install_mark_started(&s) == ST_OK && st_reg_exists(&s, "NotFirstTime"), "mark started");
    uint32_t v = 0;
    CHECK(st_reg_get_dword(&s, "NotFirstTime", &v) && v == 1, "NotFirstTime = 1");
    st_reg_close(&s);
}

static void test_graphics_store(void) {
    StRegStore s;
    StGraphics g, h;
    st_reg_open_memory(&s);
    st_gfx_defaults(&g);
    st_gfx_load(&s, &g);
    st_gfx_defaults(&h);
    CHECK(!memcmp(&g, &h, sizeof g), "absent values keep current");
    g.v[ST_GFX_FOG] = 1; g.v[ST_GFX_RESOLUTION] = 3; g.v[ST_GFX_PERF_LEVEL] = 4; g.v[ST_GFX_VSYNC] = 1;
    g.v[ST_GFX_CAR_SHADOW] = -2; g.v[ST_GFX_SLOT25] = 0; g.v[ST_GFX_VERSION] = 99; g.v[ST_GFX_SIZE] = 5;
    CHECK(st_gfx_save(&s, &g) == ST_OK, "save graphics");
    uint32_t v;
    CHECK(st_reg_get_dword(&s, "VERSION", &v) && v == 1 && st_reg_get_dword(&s, "SIZE", &v) && v == 120,
          "VERSION=1 and SIZE=120 are always written");
    CHECK(st_reg_get_dword(&s, "g_CarShadowEnable", &v) && v == 0xFFFFFFFEu, "negative stored as DWORD");
    CHECK(st_reg_get_dword(&s, "g_VSyncOn", &v) && v == 1 && st_reg_get_dword(&s, "g_PerformanceLevel", &v) && v == 4 &&
          st_reg_get_dword(&s, "g_RacingResolution", &v) && v == 3, "values stored under their names");
    int n = 0;
    for (int i = 0; i < ST_GFX_NSLOTS; i++) if (st_gfx_value_name(i)) n++;
    CHECK(n == 29, "29 persisted values (%d)", n);
    st_gfx_defaults(&h);
    st_gfx_load(&s, &h);
    CHECK(h.v[ST_GFX_FOG] == 1 && h.v[ST_GFX_CAR_SHADOW] == -2 && h.v[ST_GFX_SLOT25] == 1 /* never persisted */ &&
          h.v[ST_GFX_RESOLUTION] == 3 && h.v[ST_GFX_VERSION] == 1 && h.v[ST_GFX_PERF_LEVEL] == 4, "load restores persisted values");
    /* a value of the wrong kind (string) is ignored; a negative value survives the clamp */
    st_reg_set_string(&s, "g_FogEnable", "1");
    st_reg_set_dword(&s, "g_RainEnable", 0xFFFFFFFFu);
    st_gfx_defaults(&h);
    st_gfx_load(&s, &h);
    StGfxCaps caps;
    st_gfx_caps_full(&caps, 5);
    st_gfx_clamp(&h, &caps);
    CHECK(h.v[ST_GFX_FOG] == 0 && h.v[ST_GFX_RAIN] == -1, "unusable value ignored; negative survives clamp");
    st_reg_close(&s);

    /* startup flow (4.1) */
    st_reg_open_memory(&s);
    int first = st_gfx_startup(&s, &g, &caps, 0, 3000, 5.0);
    CHECK(first == 1 && g.v[ST_GFX_PERF_LEVEL] == 4 && g.v[ST_GFX_RESOLUTION] == 1 && g.v[ST_GFX_FSAA] == 1 &&
          g.v[ST_GFX_CROWD] == 1, "first run runs auto-detect (level 4)");
    CHECK(!st_reg_exists(&s, "g_FogEnable"), "first run does not write by itself");
    st_install_mark_started(&s);
    st_gfx_save(&s, &g);
    g.v[ST_GFX_FOG] = 0; g.v[ST_GFX_CAR_SHADOW] = 1;
    st_gfx_save(&s, &g);
    first = st_gfx_startup(&s, &h, &caps, 0, 3000, 5.0);
    CHECK(first == 0 && h.v[ST_GFX_FOG] == 0 && h.v[ST_GFX_CAR_SHADOW] == 1, "later start keeps stored values");
    caps.cap[ST_GFX_CAR_SHADOW] = 0;
    st_gfx_startup(&s, &h, &caps, 0, 3000, 5.0);
    CHECK(h.v[ST_GFX_CAR_SHADOW] == 0, "later start clamps to caps");
    st_gfx_caps_full(&caps, 5);
    first = st_gfx_startup(&s, &h, &caps, 1, 3000, 5.0);
    CHECK(first == 0 && h.v[ST_GFX_PERF_LEVEL] == 0 && h.v[ST_GFX_CROWD] == 0 && h.v[ST_GFX_RESOLUTION] == 0, "safe mode: level 0");
    CHECK(st_reg_get_dword(&s, "g_PerformanceLevel", &v) && v == 0 && st_reg_get_dword(&s, "g_CrowdEnable", &v) && v == 0,
          "safe mode writes the result to the store immediately");
    st_reg_close(&s);
}

static int file_has_in_order(const char *path, const char *const *names, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char text[8192];
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = 0;
    const char *p = text;
    for (int i = 0; i < n; i++) {
        char key[96];
        snprintf(key, sizeof key, "\n%s=", names[i]);
        const char *q = strstr(p, key);
        if (!q) return 0;
        p = q + 1;
    }
    return 1;
}

static void test_ini_store(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/cfg/sub/settings.ini", g_tmp);
    remove(path);
    StRegStore s;
    CHECK(st_reg_open_ini(&s, path) == ST_OK, "open INI (missing file)");
    uint32_t v;
    CHECK(!st_reg_exists(&s, "x"), "missing file = empty store");
    CHECK(st_reg_set_string(&s, "Install Dir", "C:\\Games\\\"NFS\" U2") == ST_OK, "write string");
    CHECK(st_reg_set_dword(&s, "NotFirstTime", 1) == ST_OK, "write dword");
    StGraphics g;
    st_gfx_defaults(&g);
    g.v[ST_GFX_CAR_SHADOW] = -1;
    g.v[ST_GFX_PERF_LEVEL] = 3;
    CHECK(st_gfx_save(&s, &g) == ST_OK, "save graphics into INI");
    st_reg_close(&s);

    CHECK(st_reg_open_ini(&s, path) == ST_OK, "reopen INI");
    char buf[128];
    CHECK(st_reg_get_string(&s, "Install Dir", buf, sizeof buf) && !strcmp(buf, "C:\\Games\\\"NFS\" U2"), "string survives (escapes)");
    CHECK(!st_reg_get_string(&s, "Install Dir", buf, 5), "string longer than buffer is unusable");
    CHECK(st_reg_get_dword(&s, "NotFirstTime", &v) && v == 1, "dword survives");
    CHECK(!st_reg_get_dword(&s, "Install Dir", &v), "string is not readable as dword");
    CHECK(st_reg_exists(&s, "notfirsttime"), "names are case-insensitive");
    StGraphics h;
    st_gfx_defaults(&h);
    st_gfx_load(&s, &h);
    CHECK(h.v[ST_GFX_CAR_SHADOW] == -1 && h.v[ST_GFX_PERF_LEVEL] == 3, "graphics survive INI");
    st_reg_close(&s);

    /* write order of 2.3: idx 0..27, g_PerformanceLevel, g_VSyncOn; and the section header */
    static const char *const order[] = {
        "VERSION", "SIZE", "g_CarEnvironmentMapEnable", "g_CarEnvironmentMapUpdateData", "g_CarShadowEnable",
        "g_CarHeadlightEnable", "g_CarLightingEnable", "g_CarDamageEnable", "g_CrowdEnable",
        "g_RoadReflectionEnable", "g_FogEnable", "g_MotionBlurEnable", "g_LightStreaksEnable",
        "g_LightGlowEnable", "g_AnimatedTextureEnable", "g_ParticleSystemEnable", "g_DepthOfFieldEnable",
        "g_WorldLodLevel", "g_CarLodLevel", "g_OverBrightEnable", "g_BleachByPassEnable", "g_TintingEnable",
        "g_FSAALevel", "g_HorizonFogEnable", "g_RainEnable", "g_TextureFiltering", "g_RacingResolution",
        "g_PerformanceLevel", "g_VSyncOn" };
    /* the INI cache keeps insertion order, and Install Dir/NotFirstTime came first: rebuild fresh */
    snprintf(path, sizeof path, "%s/cfg/order.ini", g_tmp);
    remove(path);
    st_reg_open_ini(&s, path);
    st_gfx_save(&s, &g);
    st_reg_close(&s);
    CHECK(file_has_in_order(path, order, 29), "registry values written in SPEC 2.3 order");
    FILE *f = fopen(path, "rb");
    char first[128] = "";
    if (f) { if (!fgets(first, sizeof first, f)) first[0] = 0; fclose(f); }
    CHECK(!strcmp(first, "[Software\\EA Games\\Need for Speed Underground 2]\n"), "INI section name");

    /* default location resolves on this platform */
    char dflt[1024];
    CHECK(st_default_config_path(dflt, sizeof dflt) == ST_OK && strstr(dflt, "settings.ini"), "default config path");
    CHECK(st_default_profile_root(dflt, sizeof dflt) == ST_OK && strstr(dflt, "NFS Underground 2"), "default profile root");
}

/* ---- profile files ------------------------------------------------------------------------ */
static void test_profile_files(void) {
    char root[1024], path[1200];
    snprintf(root, sizeof root, "%s/profiles/nested", g_tmp);
    static StProfile p, q;
    st_profile_defaults(&p, 0);
    strcpy(p.name, "Tester One");
    p.audio.master = 0.35f;
    p.career_difficulty = 2;
    p.raw[100] = 0xAB;                                        /* career-region byte survives */
    st_profile_delete(root, "Tester One");
    CHECK(st_profile_load(root, "Tester One", &q) == ST_E_NOTFOUND, "missing profile -> not found");
    CHECK(!st_profile_exists(root, "Tester One"), "no profile directory yet");
    CHECK(st_profile_status(root, "Tester One") == ST_E_NOTFOUND, "missing directory is positively absent");
    CHECK(st_profile_status(root, "bad/name") == ST_E_ARG, "invalid status query is not absence");
    CHECK(st_profile_save(root, &p) == ST_OK, "save profile (creates directories)");
    CHECK(st_profile_exists(root, "Tester One") && st_profiles_exist(root), "profile directory exists");
    CHECK(st_profile_status(root, "Tester One") == ST_OK, "occupied directory status");
    CHECK(st_profile_path(root, "Tester One", path, sizeof path) == ST_OK && strstr(path, "Tester One/Tester One"),
          "path is <root>/<NAME>/<NAME>");
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "file at that path");
    if (f) { fseek(f, 0, SEEK_END); CHECK(ftell(f) == (long)ST_PROFILE_FILE_SIZE, "file is 54966 bytes"); fclose(f); }
    CHECK(st_profile_load(root, "Tester One", &q) == ST_OK, "load profile");
    CHECK(q.audio.master == 0.35f && q.career_difficulty == 2 && q.raw[100] == 0xAB && !strcmp(q.name, "Tester One"),
          "loaded state matches saved state");
    p.audio.master = 0.5f;
    CHECK(st_profile_save(root, &p) == ST_OK && st_profile_load(root, "Tester One", &q) == ST_OK && q.audio.master == 0.5f,
          "overwrite");
    /* a truncated file on disk is rejected and left in place (3.5) */
    f = fopen(path, "r+b");
    uint8_t half[1000];
    if (f) { fclose(f); f = fopen(path, "wb"); memset(half, 0, sizeof half); fwrite(half, 1, sizeof half, f); fclose(f); }
    CHECK(st_profile_load(root, "Tester One", &q) == ST_E_CORRUPT, "garbage file -> corrupt");
    f = fopen(path, "wb"); if (f) { fwrite(half, 1, 10, f); fclose(f); }
    CHECK(st_profile_load(root, "Tester One", &q) == ST_E_IO, "10-byte file -> I/O error");
    f = fopen(path, "rb");
    CHECK(f != NULL, "rejected file is not deleted");
    if (f) fclose(f);
    st_profile_name_valid("x");
    strcpy(q.name, "bad/name");
    CHECK(st_profile_save(root, &q) == ST_E_ARG, "invalid name refused");
    CHECK(st_profile_delete(root, "Tester One") == ST_OK, "delete profile");
    CHECK(!st_profile_exists(root, "Tester One") && st_profile_load(root, "Tester One", &q) == ST_E_NOTFOUND, "gone after delete");
    CHECK(st_profile_delete(root, "Tester One") == ST_E_NOTFOUND, "delete twice -> not found");
    CHECK(!st_profiles_exist(root), "root empty again");
}

/* Which lights each detail level draws (graphics_policy.h). */
static void test_lighting_policy(void) {
    StGraphics g; StGfxCaps caps;
    st_gfx_defaults(&g);
    LightingPolicy p = lighting_policy(&g);
    CHECK(!p.original && p.headlight && p.glow, "no saved level keeps the full lighting");
    CHECK(lighting_policy(NULL).headlight, "missing settings keep the headlights");
    st_gfx_caps_full(&caps, 5);
    st_gfx_set_level(&g, &caps, 1, 1.0);
    p = lighting_policy(&g);
    CHECK(p.original && !p.headlight && !p.glow, "level 1: original manner, preset turns headlight and glow off");
    st_gfx_set_level(&g, &caps, 2, 1.0);
    p = lighting_policy(&g);
    CHECK(p.original && p.headlight && p.glow, "level 2: original headlight pool");
    st_gfx_set_level(&g, &caps, 3, 1.0);
    p = lighting_policy(&g);
    CHECK(!p.original && p.headlight && p.glow, "level 3 and up: per-pixel beams");
    g.v[ST_GFX_CAR_HEADLIGHT] = 0; g.v[ST_GFX_LIGHT_GLOW] = 0;
    p = lighting_policy(&g);
    CHECK(!p.headlight && !p.glow, "customized Car Headlight and Light Glow are honoured");
}

/* Car reflection, car shadow and road reflection per detail level. */
static void test_detail_policy(void) {
    StGraphics g; StGfxCaps caps;
    st_gfx_defaults(&g);
    DetailPolicy p = detail_policy(&g);
    CHECK(!p.apply && p.car_reflection_rate == 2 && p.car_shadow && !p.road_reflections,
          "no saved level keeps the built-in vehicle and road detail");
    CHECK(!detail_policy(NULL).apply, "missing settings change nothing");
    st_gfx_caps_full(&caps, 5);
    st_gfx_set_level(&g, &caps, 1, 1.0);
    p = detail_policy(&g);
    CHECK(p.apply && !p.car_reflection_rate && !p.car_shadow && !p.road_reflections,
          "level 1: no capture, square shadow, surface highlights");
    st_gfx_set_level(&g, &caps, 2, 1.0);
    p = detail_policy(&g);
    CHECK(!p.car_reflection_rate && p.car_shadow && !p.road_reflections, "level 2: projected shadows only");
    st_gfx_set_level(&g, &caps, 3, 1.0);
    p = detail_policy(&g);
    CHECK(p.car_reflection_rate == 1 && p.car_reflection_detail && p.road_reflections &&
          p.road_reflection_quality == 1, "level 3: 128 px capture every third frame, scene reflections");
    st_gfx_set_level(&g, &caps, 5, 1.0);
    p = detail_policy(&g);
    CHECK(p.car_reflection_rate == 3 && p.road_reflection_quality == 2, "level 5: every frame, fine trace");
    g.v[ST_GFX_CAR_ENV_MAP] = 9; g.v[ST_GFX_CAR_ENV_MAP_UPDATE] = 0; g.v[ST_GFX_CAR_SHADOW] = 0;
    g.v[ST_GFX_ROAD_REFLECTION] = 1;
    p = detail_policy(&g);
    CHECK(p.car_reflection_rate == 3 && !p.car_reflection_detail && !p.car_shadow && !p.road_reflections,
          "customized rows are honoured and clamped");
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && *argv[1]) g_dir = argv[1];
    if (argc > 2 && argv[2] && *argv[2]) g_tmp = argv[2];
    test_checksum();
    test_container_small();
    test_profile_vectors();
    test_helpers();
    test_presets();
    test_graphics_logic();
    test_lighting_policy();
    test_detail_policy();
    test_install_and_language();
    test_graphics_store();
    test_ini_store();
    test_profile_files();
    printf("settings_test: %d checks, %d failed -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
