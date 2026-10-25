/* camera_test — runs the gameplay-camera module against docs/cleanroom-camera/test-vectors.
 *
 *   camera_test [VECTOR_DIR [GAME_DATA_DIR [FULL_TV01_CSV]]]    (empty argument = absent)
 *
 * Every vector file is read at run time (CSV, plus a small JSON reader for
 * tv03). tv01 (three published cars) and tv01b (synthetic records with entry
 * bytes) are checked against synthetic GLOBALB images built from the vector rows.
 * When GAME_DATA_DIR holds GLOBAL/, the user's own GLOBALB is loaded through the
 * engine's loader and checked structurally (every record, 6 entries per set, tags
 * 0..5, finite values). Values are compared only if FULL_TV01_CSV (an optional,
 * local, unpublished table with the tv01 columns) is also given; otherwise that
 * is reported as skipped. tv11 checks the SPEC 7.1 projection policy.
 * Behaviour tests beyond the vectors follow at the end. Exit code is nonzero if
 * any check fails. See docs/CAMERA.md. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "nfsu2.h"

static int g_checks, g_fail;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    if (g_fail <= 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

/* ---- file + CSV helpers ---------------------------------------------------- */
static char *slurp(const char *dir, const char *name) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    long n = 0; unsigned char *d = n2_read_file(path, &n);
    if (!d) { printf("FAIL cannot read %s\n", path); g_fail++; return NULL; }
    char *s = (char *)realloc(d, (size_t)n + 1); s[n] = 0;
    return s;
}
/* Split one line in place; commas inside "..." do not split; quotes removed. */
static int split(char *line, char **f, int max) {
    int n = 0; char *p = line, *w = line;
    f[n++] = w;
    int q = 0;
    for (; *p; p++) {
        if (*p == '"') { q = !q; continue; }
        if (*p == ',' && !q) { *w++ = 0; if (n < max) f[n++] = w; continue; }
        if (*p == '\r' || *p == '\n') break;
        *w++ = *p;
    }
    *w = 0;
    return n;
}
typedef struct { char *buf; char *rows[2048]; int n; } Csv;
static int csv_open(Csv *c, const char *dir, const char *name) {
    memset(c, 0, sizeof *c);
    c->buf = slurp(dir, name);
    if (!c->buf) return 0;
    for (char *p = c->buf; *p && c->n < 2048;) {
        c->rows[c->n++] = p;
        char *e = strchr(p, '\n');
        if (!e) break;
        *e = 0; p = e + 1;
        if (!*p) break;
    }
    return 1;
}
static int csv_col(char **hdr, int nh, const char *name) {
    for (int i = 0; i < nh; i++) if (!strcmp(hdr[i], name)) return i;
    return -1;
}
static int view_by_name(const char *s) {
    for (int v = 0; v < CAM_NVIEW; v++) if (!strcmp(cam_view_name(v), s)) return v;
    return -1;
}
static int parse_floats(const char *s, double *out, int max) {   /* "[1, 2, 3]" or "1 2 3" */
    int n = 0;
    while (*s && n < max) {
        while (*s && !(*s == '-' || *s == '.' || (*s >= '0' && *s <= '9'))) s++;
        if (!*s) break;
        char *e; out[n++] = strtod(s, &e); s = e;
    }
    return n;
}

/* ---- tv02 tuning --------------------------------------------------------------- */
static void test_tuning(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv02_view_tuning_effective.csv")) return;
    char *h[64]; char hb[4096]; strncpy(hb, c.rows[0], sizeof hb - 1); hb[sizeof hb - 1] = 0;
    int nh = split(hb, h, 64);
    for (int r = 1; r < c.n; r++) {
        char *f[64]; split(c.rows[r], f, 64);
        int v = view_by_name(f[0]); CHECK(v >= 0, "tv02 view %s", f[0]); if (v < 0) continue;
        const CamTuning *k = cam_tuning(v);
        const char *axes = "xyz";
        #define COL(n) atof(f[csv_col(h, nh, n)])
        CHECK(near(k->t_eye, COL("T_eye"), 1e-6), "tv02 %s T_eye", f[0]);
        CHECK(near(k->t_look, COL("T_look"), 1e-6), "tv02 %s T_look", f[0]);
        CHECK(near(k->t_fov, COL("T_fov"), 1e-6), "tv02 %s T_fov", f[0]);
        CHECK(near(k->t_up, COL("T_up"), 1e-6), "tv02 %s T_up", f[0]);
        CHECK(k->fov_bau == (int)COL("fov_units"), "tv02 %s fov", f[0]);
        for (int a = 0; a < 3; a++) {
            char n[32];
            #define AX(base, field) snprintf(n, sizeof n, base "_%c", axes[a]); \
                CHECK(near(k->field[a], COL(n), 1e-6), "tv02 %s %s", f[0], n)
            AX("up_k", up_k); AX("up_min", up_min); AX("up_max", up_max);
            AX("eye_k", eye_k); AX("eye_min", eye_min); AX("eye_max", eye_max);
            AX("look_k", look_k); AX("look_min", look_min); AX("look_max", look_max);
            AX("T_head", t_head);
        }
    }
    free(c.buf);
}

/* ---- tv04 smoother ----------------------------------------------------------------- */
static void test_smoother(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv04_smoother_step.csv")) return;
    char prev[64] = ""; CamSmooth s; memset(&s, 0, sizeof s); float dt = 0;
    int rows = 0;
    for (int r = 1; r < c.n; r++) {
        char *f[8]; split(c.rows[r], f, 8);
        if (strcmp(prev, f[0])) {
            strncpy(prev, f[0], sizeof prev - 1);
            memset(&s, 0, sizeof s); s.T = (float)atof(f[1]); dt = (float)atof(f[2]);
        }
        int frame = atoi(f[3]);
        if (frame > 0) { cam_smooth_set_target(&s, 1.0f); cam_smooth_step(&s, dt); }
        CHECK(near(s.p, atof(f[5]), 5e-6), "tv04 %s frame %d value %.9f vs %s", f[0], frame, s.p, f[5]);
        CHECK(near(s.v, atof(f[6]), 5e-5), "tv04 %s frame %d velocity %.9f vs %s", f[0], frame, s.v, f[6]);
        rows++;
    }
    CHECK(rows > 600, "tv04 rows %d", rows);
    free(c.buf);
}

/* ---- tv05 heading weight ------------------------------------------------------------- */
static void test_heading_weight(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv05_heading_weight.csv")) return;
    for (int r = 1; r < c.n; r++) {
        char *f[4]; split(c.rows[r], f, 4);
        CHECK(near(cam_heading_weight((float)atof(f[0])), atof(f[1]), 1e-6), "tv05 speed %s", f[0]);
    }
    free(c.buf);
}

/* ---- tv06 acceleration offsets ------------------------------------------------------------ */
static void test_accel(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv06_accel_offsets.csv")) return;
    for (int r = 1; r < c.n; r++) {
        char *f[8]; split(c.rows[r], f, 8);
        int v = view_by_name(f[0]);
        double aL[3], eye[3], look[3], aw[3], up[3];
        parse_floats(f[1], aL, 3); parse_floats(f[2], eye, 3); parse_floats(f[3], look, 3);
        parse_floats(f[4], aw, 3); parse_floats(f[5], up, 3);
        float a1[3] = {(float)aL[0], (float)aL[1], (float)aL[2]}, a2[3] = {(float)aw[0], (float)aw[1], (float)aw[2]};
        float z[3] = {0, 0, 0}, e[3], l[3], u[3];
        cam_targets(cam_tuning(v), cam_is_chase(v), 0, z, z, a1, a2, e, l, u);
        for (int i = 0; i < 3; i++) {
            CHECK(near(e[i], eye[i], 1e-5), "tv06 row %d %s eye[%d] %g vs %g", r, f[0], i, e[i], eye[i]);
            CHECK(near(l[i], look[i], 1e-5), "tv06 row %d %s look[%d] %g vs %g", r, f[0], i, l[i], look[i]);
            CHECK(near(u[i], up[i], 1e-5), "tv06 row %d %s up[%d] %g vs %g", r, f[0], i, u[i], up[i]);
        }
    }
    free(c.buf);
}

/* ---- tv01 / tv01b car entries (CSV, synthetic images, optional retail) ---------------------------- */
typedef struct { char car[32]; int set, tag, pitch; double fwd, up, lat; } Row01;
typedef struct { Row01 *r; int n; } RowSet;
static RowSet g_tv01;                       /* published subset (three cars) */
#define g_rows g_tv01.r
#define g_nrows g_tv01.n
/* tv01-format file (header: car,set,view_tag,pitch_raw_bau,pitch_deg,forward_m,up_m,lateral_m). */
static int load_rows(RowSet *rs, const char *dir, const char *name) {
    Csv c; if (!csv_open(&c, dir, name)) return 0;
    rs->r = (Row01 *)calloc((size_t)c.n + 1, sizeof *rs->r); rs->n = 0;
    for (int r = 1; r < c.n; r++) {
        char *f[10]; if (split(c.rows[r], f, 10) < 8) continue;
        Row01 *w = &rs->r[rs->n++];
        snprintf(w->car, sizeof w->car, "%s", f[0]);
        w->set = !strcmp(f[1], "secondary"); w->tag = atoi(f[2]); w->pitch = atoi(f[3]);
        w->fwd = atof(f[5]); w->up = atof(f[6]); w->lat = atof(f[7]);
    }
    free(c.buf);
    return 1;
}
static void compare_rows(const RowSet *rs, const char *what, const CamCarData *d, const char *car) {
    for (int i = 0; i < rs->n; i++) {
        const Row01 *w = &rs->r[i];
        if (strcmp(w->car, car)) continue;
        const CamEntry *e = cam_car_entry(d, w->set, w->tag);
        CHECK(e != NULL, "%s %s set %d tag %d missing", what, car, w->set, w->tag);
        if (!e) continue;
        CHECK(e->pitch_bau == w->pitch, "%s %s/%d/%d pitch %d vs %d", what, car, w->set, w->tag, e->pitch_bau, w->pitch);
        CHECK(near(e->fwd, w->fwd, 1e-4) && near(e->up, w->up, 1e-4) && near(e->lat, w->lat, 1e-4),
              "%s %s/%d/%d offsets %g %g %g vs %g %g %g", what, car, w->set, w->tag,
              e->fwd, e->up, e->lat, w->fwd, w->up, w->lat);
    }
}
static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* A synthetic GLOBALB image: chunk 0x34600, eight 0x11 bytes, 0x890-byte records. */
typedef struct { unsigned char *img; long size; } Image;
static Image image_new(int ncars) {
    Image im; im.size = 8 + 8 + (long)ncars * N2_GLOBAL_CAR_STRIDE;
    im.img = (unsigned char *)calloc(1, (size_t)im.size);
    put32(im.img, 0x00034600u); put32(im.img + 4, (uint32_t)(im.size - 8));
    memset(im.img + 8, 0x11, 8);
    return im;
}
static unsigned char *image_record(Image *im, int k, const char *name) {
    unsigned char *rec = im->img + 16 + (long)k * N2_GLOBAL_CAR_STRIDE;
    snprintf((char *)rec, 32, "%.31s", name);
    snprintf((char *)rec + 0x40, 64, "CARS\\%.31s\\GEOMETRY.BIN", name);
    return rec;
}
static unsigned char *entry_ptr(unsigned char *rec, int set, int slot) { return rec + (set ? 0x790 : 0x730) + 16 * slot; }

static void test_car_data_synthetic(void) {
    char names[64][32]; int nn = 0;
    for (int i = 0; i < g_nrows; i++) {
        int seen = 0; for (int k = 0; k < nn; k++) if (!strcmp(names[k], g_rows[i].car)) seen = 1;
        if (!seen && nn < 64) snprintf(names[nn++], 32, "%s", g_rows[i].car);
    }
    CHECK(nn == 3, "tv01 cars %d (expected PEUGOT, NAVIGATOR, SKYLINE)", nn);
    for (int k = 0; k < nn; k++) {
        int ok = 0; const char *want[3] = {"PEUGOT", "NAVIGATOR", "SKYLINE"};
        for (int j = 0; j < 3; j++) if (!strcmp(names[k], want[j])) ok = 1;
        CHECK(ok, "tv01 unexpected car %s", names[k]);
    }
    Image im = image_new(nn);
    for (int k = 0; k < nn; k++) {
        unsigned char *rec = image_record(&im, k, names[k]);
        int cnt[2] = {0, 0};
        for (int i = 0; i < g_nrows; i++) {
            if (strcmp(g_rows[i].car, names[k])) continue;
            const Row01 *w = &g_rows[i];
            unsigned char *e = entry_ptr(rec, w->set, cnt[w->set]++);
            float f[3] = {(float)w->fwd, (float)w->up, (float)w->lat};
            e[0] = (unsigned char)w->tag; e[1] = 0;
            e[2] = (unsigned char)(w->pitch & 255); e[3] = (unsigned char)((w->pitch >> 8) & 255);
            memcpy(e + 4, f, 12);
        }
        put32(rec + 0x720, (uint32_t)cnt[0]); put32(rec + 0x724, (uint32_t)cnt[1]);
    }
    for (int k = 0; k < nn; k++) {
        CamCarData d;
        int ok = cam_car_data_load(im.img, im.size, names[k], &d);
        CHECK(ok, "synthetic load %s", names[k]);
        if (ok) { CHECK(d.valid && d.count[0] == 6 && d.count[1] == 6, "counts %s", names[k]); compare_rows(&g_tv01, "synthetic", &d, names[k]); }
    }
    CamCarData d;
    CHECK(!cam_car_data_load(im.img, im.size, "NOSUCHCAR", &d), "unknown car must fail");
    /* A count above the set size is implausible and must be refused. */
    put32(im.img + 16 + 0x720, 99);
    CHECK(!cam_car_data_load(im.img, im.size, names[0], &d), "oversized count must fail");
    free(im.img);
}

/* tv01b: invented records with exact entry encodings (SPEC 3.2). */
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
typedef struct {
    char rec[32]; int set, declared, slot, tag, pitch; double pitch_deg, fwd, up, lat; unsigned char hex[16];
} Row01b;
static void test_car_data_tv01b(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv01b_synthetic_car_records.csv")) return;
    Row01b rows[64]; int n = 0;
    for (int r = 0; r < c.n; r++) {
        if (c.rows[r][0] == '#' || !strncmp(c.rows[r], "record,", 7)) continue;
        char *f[12]; if (split(c.rows[r], f, 12) < 11) continue;
        Row01b *w = &rows[n]; memset(w, 0, sizeof *w);
        snprintf(w->rec, sizeof w->rec, "%s", f[0]);
        w->set = !strcmp(f[1], "secondary"); w->declared = atoi(f[2]); w->slot = atoi(f[3]); w->tag = atoi(f[4]);
        w->pitch = atoi(f[5]); w->pitch_deg = atof(f[6]); w->fwd = atof(f[7]); w->up = atof(f[8]); w->lat = atof(f[9]);
        int bad = strlen(f[10]) != 32;
        for (int i = 0; i < 16 && !bad; i++) {
            int a = hexval(f[10][2 * i]), b = hexval(f[10][2 * i + 1]);
            if (a < 0 || b < 0) bad = 1; else w->hex[i] = (unsigned char)(a << 4 | b);
        }
        CHECK(!bad, "tv01b row %d bad entry_hex", r);
        if (!bad && n < 64) n++;
    }
    CHECK(n == 18, "tv01b rows %d", n);
    /* The vector's own columns must agree with its byte encoding. */
    for (int i = 0; i < n; i++) {
        const Row01b *w = &rows[i]; float f[3]; memcpy(f, w->hex + 4, 12);
        int tag = w->hex[0] | w->hex[1] << 8, pitch = (int16_t)(w->hex[2] | w->hex[3] << 8);
        CHECK(tag == w->tag && pitch == w->pitch, "tv01b %s/%d/%d tag/pitch vs hex", w->rec, w->set, w->slot);
        CHECK(near(f[0], w->fwd, 1e-6) && near(f[1], w->up, 1e-6) && near(f[2], w->lat, 1e-6), "tv01b %s/%d/%d floats vs hex", w->rec, w->set, w->slot);
        CHECK(near(w->pitch * 360.0 / 65536.0, w->pitch_deg, 5e-5), "tv01b %s/%d/%d pitch degrees", w->rec, w->set, w->slot);
        CHECK(near(CAM_BAU_TO_RAD(w->pitch) * 180.0 / M_PI, w->pitch_deg, 5e-5), "tv01b %s/%d/%d BAU conversion", w->rec, w->set, w->slot);
    }
    char names[8][32]; int nn = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0; for (int k = 0; k < nn; k++) if (!strcmp(names[k], rows[i].rec)) seen = 1;
        if (!seen && nn < 8) snprintf(names[nn++], 32, "%s", rows[i].rec);
    }
    CHECK(nn == 2, "tv01b records %d", nn);
    Image im = image_new(nn);
    for (int k = 0; k < nn; k++) {
        unsigned char *rec = image_record(&im, k, names[k]);
        for (int i = 0; i < n; i++) {
            if (strcmp(rows[i].rec, names[k])) continue;
            put32(rec + (rows[i].set ? 0x724 : 0x720), (uint32_t)rows[i].declared);
            if (rows[i].slot >= 0 && rows[i].slot < 6) memcpy(entry_ptr(rec, rows[i].set, rows[i].slot), rows[i].hex, 16);
        }
    }
    for (int k = 0; k < nn; k++) {
        CamCarData d;
        int ok = cam_car_data_load(im.img, im.size, names[k], &d);
        CHECK(ok, "tv01b load %s", names[k]); if (!ok) continue;
        for (int set = 0; set < 2; set++) {
            int declared = -1;
            for (int i = 0; i < n; i++) if (!strcmp(rows[i].rec, names[k]) && rows[i].set == set) declared = rows[i].declared;
            CHECK(d.count[set] == declared, "tv01b %s set %d count %d vs %d", names[k], set, d.count[set], declared);
            /* Lookup per tag: first match among the declared slots, else not found. */
            for (int tag = 0; tag < 6; tag++) {
                const Row01b *want = NULL;
                for (int i = 0; i < n && !want; i++)
                    if (!strcmp(rows[i].rec, names[k]) && rows[i].set == set && rows[i].slot < rows[i].declared && rows[i].tag == tag) want = &rows[i];
                const CamEntry *e = cam_car_entry(&d, set, tag);
                if (!want) { CHECK(e == NULL, "tv01b %s/%d tag %d must be not found", names[k], set, tag); continue; }
                CHECK(e != NULL, "tv01b %s/%d tag %d missing", names[k], set, tag); if (!e) continue;
                CHECK(e->pitch_bau == want->pitch && near(e->fwd, want->fwd, 1e-5) && near(e->up, want->up, 1e-5) && near(e->lat, want->lat, 1e-5),
                      "tv01b %s/%d tag %d (slot %d) value mismatch", names[k], set, tag, want->slot);
            }
        }
    }
    free(im.img); free(c.buf);
}

/* ---- retail GLOBALB ---------------------------------------------------------------------------------- */
static unsigned char *load_global(const char *root, long *len) {   /* same order as main.c */
    char path[1024]; *len = 0;
    snprintf(path, sizeof path, "%s/GLOBAL/GLOBALB.BUN", root);
    unsigned char *data = n2_read_file(path, len);
    if (data) return data;
    snprintf(path, sizeof path, "%s/GLOBAL/GlobalB.lzc", root);
    long bytes = 0; unsigned char *packed = n2_read_file(path, &bytes);
    if (packed && bytes >= 16 && !memcmp(packed, "JDLZ", 4)) {
        uint32_t size = n2_u32(packed + 8);
        if (size && size < (1u << 28)) {
            data = (unsigned char *)malloc(size);
            *len = n2_jdlz(packed, (int)bytes, data, (int)size);
            if (*len <= 0) { free(data); data = NULL; *len = 0; }
        }
    }
    free(packed);
    return data;
}
/* Retail comparison. Always structural: every record loads, 6 declared entries per set, view
 * tags 0..5, finite values. Values are compared only against the optional local full table
 * (RowSet `full`, may be empty); otherwise the value comparison is reported as skipped. */
static void test_car_data_retail(const char *root, const RowSet *full) {
    long glen = 0; unsigned char *g = (root && *root) ? load_global(root, &glen) : NULL;
    if (!g) {
        printf("note: no GLOBAL data under '%s'; retail comparison skipped entirely\n", root && *root ? root : "(none)");
        return;
    }
    N2GlobalCarTable t;
    CHECK(n2_global_car_table(g, glen, &t), "retail car table not found");
    int records = 0, loaded = 0, compared = 0;
    if (t.valid && t.tables == 1) {
        for (long at = t.beg; at < t.end; at += N2_GLOBAL_CAR_STRIDE) {
            char name[33]; memcpy(name, g + at, 32); name[32] = 0;
            records++;
            CamCarData d;
            if (!cam_car_data_load(g, glen, name, &d)) { CHECK(0, "retail record %s does not load", name); continue; }
            loaded++;
            CHECK(d.count[0] == 6 && d.count[1] == 6, "retail %s declared counts %d/%d", name, d.count[0], d.count[1]);
            for (int set = 0; set < 2; set++) {
                for (int i = 0; i < d.count[set]; i++) {
                    const CamEntry *e = &d.e[set][i];
                    CHECK(e->tag >= 0 && e->tag <= 5, "retail %s/%d slot %d view tag %d", name, set, i, e->tag);
                    CHECK(isfinite(e->fwd) && isfinite(e->up) && isfinite(e->lat), "retail %s/%d slot %d non-finite", name, set, i);
                }
            }
            if (full && full->n) {
                int has = 0; for (int i = 0; i < full->n; i++) if (!strcmp(full->r[i].car, name)) { has = 1; break; }
                if (has) { compare_rows(full, "retail", &d, name); compared++; }
            }
        }
    }
    printf("retail structure: %d records, %d loaded, 6 entries per set checked\n", records, loaded);
    CHECK(records > 0 && loaded == records, "retail records %d loaded %d", records, loaded);
    if (full && full->n) {
        printf("retail values: compared %d cars against the local full table (%d rows)\n", compared, full->n);
        CHECK(compared > 0, "full table given but no car matched");
    } else {
        printf("note: no full per-car table given; retail VALUE comparison skipped (structure only)\n");
    }
    free(g);
}

/* ---- tv03 / tv03b poses ------------------------------------------------------------------------ */
typedef struct JV { int t; double n; char *s; struct JV **v; char **k; int c; } JV;   /* 0 num 1 str 2 arr 3 obj 4 lit */
static const char *jp;
static void jws(void) { while (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t') jp++; }
static JV *jparse(void) {
    jws();
    JV *v = (JV *)calloc(1, sizeof *v);
    if (*jp == '{' || *jp == '[') {
        int obj = *jp == '{'; v->t = obj ? 3 : 2; jp++;
        int cap = 0;
        for (;;) {
            jws(); if (*jp == '}' || *jp == ']') { jp++; break; }
            if (*jp == ',') { jp++; continue; }
            if (v->c == cap) { cap = cap ? cap * 2 : 8; v->v = (JV **)realloc(v->v, (size_t)cap * sizeof *v->v); v->k = (char **)realloc(v->k, (size_t)cap * sizeof *v->k); }
            char *key = NULL;
            if (obj) { JV *k = jparse(); key = k->s; jws(); jp++; /* ':' */ }
            v->k[v->c] = key; v->v[v->c++] = jparse();
        }
    } else if (*jp == '"') {
        const char *e = ++jp; while (*e && *e != '"') e++;
        v->t = 1; v->s = (char *)malloc((size_t)(e - jp) + 1); memcpy(v->s, jp, (size_t)(e - jp)); v->s[e - jp] = 0; jp = e + 1;
    } else if ((*jp >= '0' && *jp <= '9') || *jp == '-') {
        char *e; v->n = strtod(jp, &e); jp = e;
    } else { v->t = 4; v->n = (*jp == 't'); while (*jp && *jp != ',' && *jp != '}' && *jp != ']' && *jp != '\n') jp++; }
    return v;
}
static JV *jget(const JV *o, const char *key) {
    for (int i = 0; i < o->c; i++) if (o->k[i] && !strcmp(o->k[i], key)) return o->v[i];
    return NULL;
}

static void settle(CamState *s, const CamCarFrame *car, float speed, int ticks, const float accel[3], int wheels) {
    CamInput in; memset(&in, 0, sizeof in);
    in.car = *car; in.speed = speed; in.wheels_down = wheels;
    for (int i = 0; i < 3; i++) { in.vel[i] = car->fwd[i] * speed; in.accel[i] = accel ? accel[i] : 0; }
    for (int n = 0; n < ticks; n++) { in.clock = n / 60.0; cam_tick(s, &in, 1.0f / 60.0f); }
}

static void test_static_pose(const char *dir) {
    char *txt = slurp(dir, "tv03_static_pose.json"); if (!txt) return;
    jp = txt; JV *root = jparse(); JV *cases = jget(root, "cases");
    CHECK(cases && cases->c == 90, "tv03 case count %d", cases ? cases->c : -1);
    if (!cases) return;
    for (int i = 0; i < cases->c; i++) {
        JV *cs = cases->v[i];
        const char *view = jget(cs, "view")->s, *pose = jget(cs, "pose")->s;
        int v = view_by_name(view);
        JV *lb = jget(cs, "look_back_mirrored");
        int mirrored = lb && lb->t == 4 && lb->n != 0;
        JV *en = jget(cs, "entry");
        CamCarData d; memset(&d, 0, sizeof d); d.valid = 1; d.count[0] = 1;
        d.e[0][0] = (CamEntry){ v, (int)jget(en, "pitch_units")->n, (float)jget(en, "fwd_m")->n,
                                (float)jget(en, "up_m")->n, (float)jget(en, "lat_m")->n };
        float pos[3] = {0, 0, 0}, yaw = 0;
        if (strstr(pose, "(100,-50,3)")) { pos[0] = 100; pos[1] = -50; pos[2] = 3; yaw = 1.57079632679f; }
        else if (strstr(pose, "yaw_30")) yaw = 30.0f * 3.14159265359f / 180.0f;
        float up[3] = {0, 0, 1};
        CamCarFrame car; cam_car_axes(yaw, up, &car); memcpy(car.pos, pos, sizeof pos);
        CamState s; cam_init(&s); cam_set_car(&s, &d); cam_set_requested(&s, v); cam_set_look_back(&s, mirrored);
        settle(&s, &car, 20.0f, 600, NULL, 4);
        CamPose p; cam_pose(&s, &car, &p);
        JV *ew = jget(cs, "eye_world"), *lw = jget(cs, "look_world"), *uw = jget(cs, "up_world");
        for (int a = 0; a < 3; a++) {
            CHECK(near(p.eye[a], ew->v[a]->n, 2e-4), "tv03 #%d %s %s lb=%d eye[%d] %g vs %g", i, view, pose, mirrored, a, p.eye[a], ew->v[a]->n);
            CHECK(near(p.look[a], lw->v[a]->n, 2e-4), "tv03 #%d %s %s lb=%d look[%d] %g vs %g", i, view, pose, mirrored, a, p.look[a], lw->v[a]->n);
            if (uw->t == 2) CHECK(near(p.up[a], uw->v[a]->n, 1e-4), "tv03 #%d up[%d]", i, a);
            else CHECK(near(p.up[a], up[a], 1e-4), "tv03 #%d in-car up[%d]", i, a);
        }
        CHECK(p.fov_bau == (int)jget(cs, "fov_units")->n, "tv03 #%d fov %d", i, p.fov_bau);
        CHECK(p.hide_car == (v == CAM_BUMPER || (v == CAM_HOOD && mirrored)), "tv03 #%d hide_car", i);
    }
}

static void test_airborne(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv03b_airborne_offsets.csv")) return;
    for (int r = 1; r < c.n; r++) {
        char *f[8]; split(c.rows[r], f, 8);
        int v = view_by_name(f[1]); double g[3], a[3], l[3];
        parse_floats(f[2], g, 3); parse_floats(f[3], a, 3); parse_floats(f[4], l, 3);
        const Row01 *row = NULL;
        for (int i = 0; i < g_nrows; i++) if (!strcmp(g_rows[i].car, f[0]) && !g_rows[i].set && g_rows[i].tag == v) row = &g_rows[i];
        CHECK(row != NULL, "tv03b entry %s %s", f[0], f[1]); if (!row) continue;
        float off[3], look[3], z[3] = {0, 0, 0}, e[3], lt[3], ut[3];
        cam_base_offsets(v, row->pitch, (float)row->fwd, (float)row->lat, (float)row->up, off, look);
        for (int air = 0; air < 2; air++) {
            cam_targets(cam_tuning(v), 1, air, off, look, z, z, e, lt, ut);
            const double *want = air ? a : g;
            for (int i = 0; i < 3; i++) {
                CHECK(near(e[i], want[i], 1e-3), "tv03b %s %s air=%d eye[%d] %g vs %g", f[0], f[1], air, i, e[i], want[i]);
                CHECK(near(lt[i], l[i], 1e-3), "tv03b %s %s air=%d look[%d] %g vs %g", f[0], f[1], air, i, lt[i], l[i]);
            }
        }
    }
    free(c.buf);
}

/* ---- tv07 / tv08 / tv09 / tv10 ------------------------------------------------------------------------- */
static void test_shake(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv07_shake_params.csv")) return;
    for (int r = 1; r < c.n; r++) {
        char *f[10]; split(c.rows[r], f, 10);
        int s = cam_surface_by_name(f[1]); CHECK(s >= 0, "tv07 surface %s", f[1]);
        int surf[4] = {s, s, s, s};
        CamShakeParams p;
        cam_shake_params((float)atof(f[0]), 100.0f, surf, !strcmp(f[2], "True"), !strcmp(f[3], "True"), 0, &p);
        double q[4];
        const float *got[4] = {p.freq1, p.amp1, p.freq2, p.amp2};
        for (int k = 0; k < 4; k++) {
            parse_floats(f[4 + k], q, 4);
            for (int i = 0; i < 4; i++)
                CHECK(near(got[k][i], q[i], 1e-4 * fmax(1.0, fabs(q[i])) + 1e-7), "tv07 row %d col %d[%d] %g vs %g", r, k, i, got[k][i], q[i]);
        }
    }
    free(c.buf);
}
static void test_noise(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv08_noise.csv")) return;
    for (int r = 1; r < c.n; r++) {
        char *f[6]; split(c.rows[r], f, 6);
        double x = atof(f[0]);
        CHECK(near(cam_lattice((int32_t)floor(x)), atof(f[1]), 1e-6), "tv08 lattice x=%s", f[0]);
        CHECK(near(cam_noise(x), atof(f[2]), 1e-6), "tv08 noise x=%s %g vs %s", f[0], cam_noise(x), f[2]);
        CHECK(near(cam_fbm(x), atof(f[3]), 1e-6), "tv08 fbm x=%s %g vs %s", f[0], cam_fbm(x), f[3]);
    }
    free(c.buf);
}
static void test_projection(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv09_projection.csv")) return;
    for (int r = 1; r < c.n; r++) {
        char *f[10]; split(c.rows[r], f, 10);
        float xs, ys;
        cam_projection_scales(CAM_PROJ_ORIGINAL, atoi(f[1]), atoi(f[2]), atoi(f[3]), !strcmp(f[4], "True"), &xs, &ys);
        CHECK(near(xs, atof(f[5]), 2e-6), "tv09 row %d xs %g vs %s", r, xs, f[5]);
        CHECK(near(ys, atof(f[6]), 2e-6), "tv09 row %d ys %g vs %s", r, ys, f[6]);
        CHECK(near(2 * atan(1.0 / xs) * 180 / M_PI, atof(f[7]), 1e-3), "tv09 row %d hfov", r);
        CHECK(near(2 * atan(1.0 / ys) * 180 / M_PI, atof(f[8]), 1e-3), "tv09 row %d vfov", r);
    }
    free(c.buf);
}
/* tv11: SPEC 7.1 projection policy. */
static void test_projection_policy(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv11_projection_policy.csv")) return;
    int rows = 0;
    for (int r = 1; r < c.n; r++) {
        char *f[12]; if (split(c.rows[r], f, 12) < 10) continue;
        int pol = cam_projection_policy_by_name(f[5]);
        CHECK(pol >= 0, "tv11 row %d policy '%s'", r, f[5]); if (pol < 0) continue;
        float xs, ys;
        cam_projection_scales(pol, atoi(f[1]), atoi(f[2]), atoi(f[3]), !strcmp(f[4], "True"), &xs, &ys);
        CHECK(near(xs, atof(f[6]), 2e-6), "tv11 row %d (%s %sx%s %s) xs %g vs %s", r, f[5], f[2], f[3], f[0], xs, f[6]);
        CHECK(near(ys, atof(f[7]), 2e-6), "tv11 row %d (%s %sx%s %s) ys %g vs %s", r, f[5], f[2], f[3], f[0], ys, f[7]);
        CHECK(near(2 * atan(1.0 / xs) * 180 / M_PI, atof(f[8]), 1e-3), "tv11 row %d hfov", r);
        CHECK(near(2 * atan(1.0 / ys) * 180 / M_PI, atof(f[9]), 1e-3), "tv11 row %d vfov", r);
        rows++;
    }
    CHECK(rows == 24, "tv11 rows %d", rows);
    free(c.buf);
    /* Consequences stated in SPEC 7.1, for arbitrary FOVs. */
    CHECK(CAM_PROJ_DEFAULT == CAM_PROJ_WIDE, "wide is the default policy");
    CHECK(cam_projection_policy_by_name("wide") == CAM_PROJ_WIDE && cam_projection_policy_by_name("original") == CAM_PROJ_ORIGINAL &&
          cam_projection_policy_by_name("bogus") < 0 && cam_projection_policy_by_name(NULL) < 0, "policy names");
    static const int fovs[] = {13020, 16384, 9000};
    for (int fi = 0; fi < 3; fi++) for (int mv = 0; mv < 2; mv++) {
        float ox, oy, wx, wy, rx, ry;
        /* 4:3 and narrower: both policies identical. */
        static const int narrow[][2] = {{640, 480}, {1024, 768}, {800, 800}, {600, 800}, {1280, 1024}};
        for (int k = 0; k < 5; k++) {
            cam_projection_scales(CAM_PROJ_ORIGINAL, fovs[fi], narrow[k][0], narrow[k][1], mv, &ox, &oy);
            cam_projection_scales(CAM_PROJ_WIDE, fovs[fi], narrow[k][0], narrow[k][1], mv, &wx, &wy);
            CHECK(ox == wx && oy == wy, "policies differ at %dx%d", narrow[k][0], narrow[k][1]);
        }
        /* 16:9: y unchanged from 4:3, x is 0.75 of the 4:3 x. */
        cam_projection_scales(CAM_PROJ_ORIGINAL, fovs[fi], 640, 480, mv, &rx, &ry);
        cam_projection_scales(CAM_PROJ_WIDE, fovs[fi], 1920, 1080, mv, &wx, &wy);
        CHECK(near(wy, ry, 1e-6) && near(wx, 0.75 * rx, 1e-5), "wide 16:9 fov %d mv %d: %g %g vs %g %g", fovs[fi], mv, wx, wy, rx, ry);
        /* The matrix carries the same scales. */
        float m[16]; cam_projection(CAM_PROJ_WIDE, fovs[fi], 1920, 1080, mv, 0.5f, 1000.0f, m);
        CHECK(m[0] == wx && m[5] == wy && m[11] == -1.0f, "cam_projection matrix scales");
    }
}
/* Very narrow viewports: the vertical half angle must stay below 90 degrees, or the picture flips. */
static void test_projection_narrow(void) {
    static const int sizes[][2] = {{300, 600}, {200, 600}, {120, 600}, {40, 600}, {1, 600}, {1, 100000}};
    for (int mv = 0; mv < 2; mv++) for (int k = 0; k < 6; k++) {
        float xs, ys;
        cam_projection_scales(CAM_PROJ_ORIGINAL, 13020, sizes[k][0], sizes[k][1], mv, &xs, &ys);
        CHECK(xs > 0 && ys > 0 && isfinite(xs) && isfinite(ys), "narrow %dx%d mv %d scales %g %g", sizes[k][0], sizes[k][1], mv, xs, ys);
        cam_projection_scales(CAM_PROJ_WIDE, 13020, sizes[k][0], sizes[k][1], mv, &xs, &ys);
        CHECK(xs > 0 && ys > 0 && isfinite(xs) && isfinite(ys), "narrow wide %dx%d mv %d scales %g %g", sizes[k][0], sizes[k][1], mv, xs, ys);
    }
    /* Narrower never gives a wider vertical view than the limit, and 4:3 is untouched. */
    float x4, y4, xn, yn;
    cam_projection_scales(CAM_PROJ_ORIGINAL, 13020, 640, 480, 1, &x4, &y4);
    CHECK(near(y4, 1.486711, 2e-6), "4:3 unchanged %g", y4);
    cam_projection_scales(CAM_PROJ_ORIGINAL, 13020, 1, 100000, 1, &xn, &yn);
    CHECK(near(2 * atan(1.0 / yn) * 180 / M_PI, 160.0, 0.05), "vertical view limit %g", 2 * atan(1.0 / yn) * 180 / M_PI);
}
static void test_cycle(const char *dir) {
    Csv c; if (!csv_open(&c, dir, "tv10_view_cycle.csv")) return;
    for (int r = 1; r < c.n; r++) {
        char *f[6]; split(c.rows[r], f, 6);
        int before = atoi(f[1]), dir_ = atoi(f[2]), after = atoi(f[3]);
        int got;
        if (!strncmp(f[0], "options", 7)) got = cam_option_menu_step(before, dir_);
        else got = cam_option_cycle(before, dir_, strstr(f[0], "drift camera allowed") != NULL);
        CHECK(got == after, "tv10 row %d (%s) %d dir %d -> %d, want %d", r, f[0], before, dir_, got, after);
        CHECK(!strcmp(cam_view_name(cam_view_for_option(got)), f[4]), "tv10 row %d view name %s", r, f[4]);
    }
    free(c.buf);
}

/* ---- behaviour beyond the vectors ---------------------------------------------------------------------------- */
static int block_all(void *ctx, const float *eye, const float *lo, const float *hi) {
    (void)eye; (void)lo; (void)hi; return *(int *)ctx;
}
static void test_behaviour(void) {
    float up[3] = {0, 0, 1};
    CamCarFrame car = {0}; cam_car_axes(0, up, &car);
    CamCarData d; cam_car_data_default(&d);

    /* Event set-up and drift permission. */
    CHECK(cam_option_for_event(CAM_OPT_FAR, 1) == CAM_OPT_DRIFT, "drift event sets option 4");
    CHECK(cam_option_for_event(CAM_OPT_DRIFT, 0) == CAM_OPT_FAR, "other event leaves 4 -> 3");
    CHECK(cam_option_for_event(CAM_OPT_HOOD, 0) == CAM_OPT_HOOD, "other event keeps option");
    CHECK(cam_drift_allowed(1, 0) && cam_drift_allowed(0, 4174) && cam_drift_allowed(0, 4179) &&
          !cam_drift_allowed(0, 4173) && !cam_drift_allowed(0, 4180), "drift allowed ids");
    CHECK(cam_view_for_option(9) == CAM_FAR && cam_view_for_option(-1) == CAM_FAR, "unknown option -> FAR");

    /* Settings persistence round trip. */
    CamSettings st, rd; cam_settings_defaults(&st);
    CHECK(st.opt[0] == CAM_OPT_FAR && st.opt[1] == CAM_OPT_FAR, "default option is FAR for both players");
    st.opt[0] = CAM_OPT_HOOD; st.opt[1] = CAM_OPT_DRIFT;
    CHECK(cam_settings_save(&st, "build/camera_test_settings/camera.cfg"), "settings save");
    FILE *saved=fopen("build/camera_test_settings/camera.cfg","r");
    char saved_text[256]={0};
    if(saved){fread(saved_text,1,sizeof saved_text-1,saved);fclose(saved);}
    CHECK(strstr(saved_text,"right_mouse_look=0\n")!=NULL,"mouse look is explicitly off by default in saved settings");
    CHECK(cam_settings_load(&rd, "build/camera_test_settings/camera.cfg") && rd.opt[0] == CAM_OPT_HOOD && rd.opt[1] == CAM_OPT_DRIFT, "settings round trip");
    CHECK(!rd.mouse_look,"mouse look defaults off");
    st.mouse_look=1;
    CHECK(cam_settings_save(&st,"build/camera_test_settings/camera.cfg") &&
          cam_settings_load(&rd,"build/camera_test_settings/camera.cfg") && rd.mouse_look,"mouse look persists on");
    saved=fopen("build/camera_test_settings/camera.cfg","w");
    if(saved){fputs("camera_option_p1=1\nright_mouse_look=7\n",saved);fclose(saved);}
    CHECK(cam_settings_load(&rd,"build/camera_test_settings/camera.cfg") && !rd.mouse_look && rd.opt[0]==CAM_OPT_HOOD,"invalid mouse setting stays off");
    saved=fopen("build/camera_test_settings/camera.cfg","w");
    if(saved){fputs("camera_option_p1=2\n",saved);fclose(saved);}
    CHECK(cam_settings_load(&rd,"build/camera_test_settings/camera.cfg") && !rd.mouse_look && rd.opt[0]==CAM_OPT_CLOSE,"old configs keep camera and default mouse look off");
    CHECK(!cam_settings_load(&rd, "build/camera_test_settings/none.cfg") && rd.opt[0] == CAM_OPT_FAR, "missing settings -> defaults");

    /* First tick cuts: no smoothing lag on creation. */
    CamState s; cam_init(&s); cam_set_requested(&s, CAM_FAR);
    settle(&s, &car, 0, 1, NULL, 4);
    CamPose p; cam_pose(&s, &car, &p);
    CHECK(near(p.eye[0], -5.8, 1e-5) && near(p.eye[2], 1.7, 1e-5), "first frame is snapped");

    /* Sustained acceleration converges to the SPEC 6.8 offset (FAR: -0.15 * 5). */
    float acc[3] = {5, 0, 0};
    CamInput in; memset(&in, 0, sizeof in); in.car = car; in.wheels_down = 4; in.speed = 20;
    in.vel[0] = 20; in.accel[0] = 5;
    for (int n = 0; n < 900; n++) { in.clock = n / 60.0; cam_tick(&s, &in, 1.0f / 60.0f); }
    cam_pose(&s, &car, &p);
    CHECK(near(p.eye[0], -5.8 - 0.75, 2e-3) && near(p.look[0], -0.3, 2e-3), "accel offset settles: %g %g", p.eye[0], p.look[0]);
    (void)acc;

    /* A view change is an instant cut. */
    in.accel[0] = 0;
    for (int n = 0; n < 5; n++) { in.clock = 20 + n / 60.0; cam_tick(&s, &in, 1.0f / 60.0f); }   /* flush the 4-sample mean */
    cam_set_requested(&s, CAM_CLOSE);
    in.clock = 21; cam_tick(&s, &in, 1.0f / 60.0f); cam_pose(&s, &car, &p);
    CHECK(p.view == CAM_CLOSE && near(p.eye[0], -4.7, 1e-4), "view change cuts");

    /* BUMPER is rigid and hides the car; look-back mirrors; HOOD hides only in look-back. */
    cam_set_requested(&s, CAM_BUMPER); in.clock = 21; cam_tick(&s, &in, 1.0f / 60.0f); cam_pose(&s, &car, &p);
    CHECK(p.hide_car && p.view == CAM_BUMPER && p.fov_bau == 16384, "bumper hides car, 90 deg");
    CHECK(near(p.up[2], 1, 1e-6), "in-car up is the car up");
    cam_set_look_back(&s, 1); cam_pose(&s, &car, &p);
    CHECK(p.eye[0] < 0 && p.look[0] < p.eye[0], "bumper look-back looks out of the rear");
    cam_set_look_back(&s, 0);
    cam_set_requested(&s, CAM_HOOD); in.clock = 22; cam_tick(&s, &in, 1.0f / 60.0f); cam_pose(&s, &car, &p);
    CHECK(!p.hide_car, "hood shows car");
    cam_set_look_back(&s, 1); cam_pose(&s, &car, &p);
    CHECK(p.hide_car, "hood look-back hides car");
    cam_set_look_back(&s, 0);

    /* In-car fallback to FAR: blocked probe, and airborne with a previous chase view. */
    int blocked = 1;
    cam_set_requested(&s, CAM_BUMPER); in.blocked = block_all; in.blocked_ctx = &blocked;
    in.clock = 23; cam_tick(&s, &in, 1.0f / 60.0f);
    CHECK(s.effective == CAM_FAR, "blocked in-car eye falls back to FAR");
    blocked = 0; in.clock = 24; cam_tick(&s, &in, 1.0f / 60.0f);
    CHECK(s.effective == CAM_BUMPER, "fallback ends when clear");
    in.wheels_down = 2; in.clock = 25;
    cam_set_requested(&s, CAM_FAR); cam_tick(&s, &in, 1.0f / 60.0f);
    cam_set_requested(&s, CAM_BUMPER); in.clock = 26; cam_tick(&s, &in, 1.0f / 60.0f);
    CHECK(s.effective == CAM_FAR, "airborne after a chase view keeps FAR");
    in.vehicle_s2 = 1; in.wheels_down = 4; in.clock = 27; cam_tick(&s, &in, 1.0f / 60.0f);
    CHECK(s.effective == CAM_FAR, "vehicle state S2 forces FAR");
    in.vehicle_s2 = 0; in.blocked = NULL;

    /* Airborne: heading frozen, vertical component fades; ground contact restores. */
    CamState a; cam_init(&a); cam_set_requested(&a, CAM_FAR);
    CamInput ai = in; ai.wheels_down = 4; ai.speed = 20; ai.vel[0] = 20; ai.accel[0] = 0;
    for (int n = 0; n < 60; n++) { ai.clock = n / 60.0; cam_tick(&a, &ai, 1.0f / 60.0f); }
    float hx = a.head[0].target;
    ai.wheels_down = 0; ai.vel[1] = 15;      /* sideways velocity while airborne must be ignored */
    for (int n = 60; n < 120; n++) { ai.clock = n / 60.0; cam_tick(&a, &ai, 1.0f / 60.0f); }
    CHECK(near(a.head[0].target, hx, 1e-6) && near(a.head[1].target, 0, 1e-6), "heading target frozen in the air");
    CHECK(near(a.eye[0].target, -5.8f * 1.3f, 1e-3), "airborne eye offset x1.3 (%g)", a.eye[0].target);
    CHECK(near(a.eye[0].T, 1.0 / 1.3, 1e-4), "airborne T_eye / 1.3");

    /* Reversing mirrors the velocity forward. */
    CamState rv; cam_init(&rv); cam_set_requested(&rv, CAM_FAR);
    CamInput ri; memset(&ri, 0, sizeof ri); ri.car = car; ri.wheels_down = 4; ri.speed = 20; ri.vel[0] = -20;
    cam_tick(&rv, &ri, 1.0f / 60.0f);
    CHECK(near(rv.head[0].target, 1, 1e-5), "reverse velocity mirrored to point forward");

    /* Drift event removes the acceleration eye shift. */
    CamState dr; cam_init(&dr); cam_set_requested(&dr, CAM_DRIFT);
    CamInput di = in; di.accel[0] = 5; di.drift_event = 1; di.blocked = NULL; di.wheels_down = 4;
    for (int n = 0; n < 10; n++) { di.clock = n / 60.0; cam_tick(&dr, &di, 1.0f / 60.0f); }
    CHECK(near(dr.eye[0].target, -5.8, 1e-5), "drift event: eyeK zeroed (%g)", dr.eye[0].target);

    /* Smoother: T = 0 follows exactly; large step completes. */
    CamSmooth sm; memset(&sm, 0, sizeof sm); cam_smooth_set_target(&sm, 3); cam_smooth_step(&sm, 0.016f);
    CHECK(near(sm.p, 3, 1e-6), "T=0 follows exactly");
    memset(&sm, 0, sizeof sm); sm.T = 0.1f; cam_smooth_set_target(&sm, 1); cam_smooth_step(&sm, 5.0f);
    CHECK(near(sm.p, 1, 1e-6) && !sm.active, "u >= 10 completes");

    /* Shake: pitch amplitude 0.14 deg on a smooth road at 100 m/s outside drag. */
    int road[4] = {1, 1, 1, 1}; CamShakeParams sp;
    cam_shake_params(100, 100, road, 0, 0, 0, &sp);
    CHECK(near(sp.amp1[2], 0.14, 1e-5), "shake pitch amplitude");
    float c[4] = {0, 0, 0, 0}, vm[16], base[16];
    float eye[3] = {0, 0, 0}, look[3] = {1, 0, 0};
    cam_view_matrix(eye, look, up, base); memcpy(vm, base, sizeof vm);
    cam_shake_apply(c, 13020, vm);
    int same = 1; for (int i = 0; i < 16; i++) if (!near(vm[i], base[i], 1e-6)) same = 0;
    CHECK(same, "zero shake leaves the view matrix alone");
    c[0] = 0.01f; c[2] = 0.14f; memcpy(vm, base, sizeof vm); cam_shake_apply(c, 13020, vm);
    int moved = 0; for (int i = 0; i < 16; i++) if (!near(vm[i], base[i], 1e-7)) moved = 1;
    CHECK(moved, "non-zero shake changes the view matrix");
    CHECK(cam_shake_angle(0.14f, 13020) > 0.0024f && cam_shake_angle(-0.14f, 13020) < -0.0024f, "FOV rescale of shake angle");

    /* View matrix sends the look point to -z. */
    float e2[3] = {10, 5, 2}, l2[3] = {20, 5, 2}, vmx[16];
    cam_view_matrix(e2, l2, up, vmx);
    float lx = vmx[0]*l2[0]+vmx[4]*l2[1]+vmx[8]*l2[2]+vmx[12];
    float lz = vmx[2]*l2[0]+vmx[6]*l2[1]+vmx[10]*l2[2]+vmx[14];
    CHECK(near(lx, 0, 1e-5) && near(lz, -10, 1e-5), "lookAt places target on -z");

    /* Car-vs-eye helpers. */
    float ev[3] = {0, 0, 1.0f}, cc[3] = {0, 0, 1.0f};
    CHECK(cam_raise_over_sphere(ev, cc, 2.0f, 0.1f) && near(ev[2], 1.0 + 2.1, 1e-5), "eye raised over car sphere");
    float far_eye[3] = {10, 0, 1}; CHECK(!cam_raise_over_sphere(far_eye, cc, 2.0f, 0.1f), "far eye untouched");
    float bp[3] = {0, 0, 0}, pt[3] = {2.3f, 0, 0.5f};
    CHECK(cam_point_near_box(pt, bp, 0, 2.0f, 1.0f, 1.5f, 0.5f) && !cam_point_near_box(pt, bp, 0, 1.5f, 1.0f, 1.5f, 0.5f), "point/box margin");
}

static void test_render_pose(void) {
    CamState before, after; cam_init(&before); cam_init(&after);
    before.started=after.started=1; before.snap=after.snap=0;
    before.head[0].p=1; after.head[1].p=1;
    before.eye[0].p=-4; after.eye[0].p=-8;
    before.eye[2].p=2; after.eye[2].p=4;
    before.look[0].p=1; after.look[0].p=3;
    before.look[2].p=1; after.look[2].p=3;
    before.fov.p=12000; after.fov.p=16000;
    CamState saved_before=before,saved_after=after;
    const float up[3]={0,0,1}; CamCarFrame car;
    cam_car_axes(.2f,up,&car); car.pos[0]=10;car.pos[1]=20;car.pos[2]=5;
    CamPose actual,expected;
    cam_render_pose(&before,&after,&car,.5f,&actual);
    CHECK(near(actual.eye[0],5.7573593,1e-5) && near(actual.eye[1],15.7573593,1e-5) &&
          near(actual.eye[2],8,1e-6),"camera eye uses the displayed heading and offset at the same fraction");
    CHECK(near(actual.look[0],11.4142136,1e-5) && near(actual.look[1],21.4142136,1e-5) &&
          near(actual.look[2],7,1e-6) && actual.fov_bau==14000,"look and FOV share the display fraction");
    CHECK(!memcmp(&before,&saved_before,sizeof before) && !memcmp(&after,&saved_after,sizeof after),
          "camera rendering never changes simulation snapshots");
    cam_pose(&before,&car,&expected);cam_render_pose(&before,&after,&car,0,&actual);
    CHECK(!memcmp(&actual,&expected,sizeof actual),"render alpha zero preserves the previous pose exactly");
    cam_pose(&after,&car,&expected);cam_render_pose(&before,&after,&car,1,&actual);
    CHECK(!memcmp(&actual,&expected,sizeof actual),"deterministic render alpha one preserves the current pose exactly");
    for(int cut=0;cut<5;cut++) {
        before=saved_before;after=saved_after;
        if(cut==0)before.started=0;
        if(cut==1)before.snap=1;
        if(cut==2)after.effective=CAM_HOOD;
        if(cut==3)after.snap=1;
        if(cut==4)after.look_back=1;
        cam_pose(&after,&car,&expected);cam_render_pose(&before,&after,&car,.25f,&actual);
        CHECK(!memcmp(&actual,&expected,sizeof actual),"camera cut %d never blends incompatible history",cut);
    }
}

static void test_render_cadence(void) {
    const int rates[]={10,30,60,90,144,240,0};
    const int mixed[]={144,30,240,20,60,120};
    const double tick=1./60.;const float up[3]={0,0,1};
    for(int direction=-1;direction<=1;direction+=2)for(unsigned r=0;r<sizeof rates/sizeof rates[0];r++) {
        CamState before,after;cam_init(&after);after.started=1;after.snap=0;
        after.eye[0].p=-6;after.eye[2].p=2;after.fov.p=14000;
        after.head[0].p=cosf(3.1f);after.head[1].p=sinf(3.1f);before=after;
        double remainder=0,time=0,previous_yaw=0,max_error=0;int ticks=0,repeated=0,samples=0;
        for(int f=0;time<6;f++) {
            double dt=1./(rates[r]?rates[r]:mixed[f%6]);time+=dt;remainder+=dt;
            while(remainder>=tick-1e-12) {
                before=after;ticks++;remainder-=tick;
                double angle=3.1+direction*ticks*tick;
                after.head[0].p=(float)cos(angle);after.head[1].p=(float)sin(angle);
            }
            float alpha=(float)fmax(0,remainder*60);
            double angle=3.1+direction*fmax(0,(ticks-1+alpha)*tick);
            CamCarFrame car;cam_car_axes((float)angle,up,&car);
            car.pos[0]=(float)(30*sin(angle));car.pos[1]=(float)(30*cos(angle));
            CamPose pose;cam_render_pose(&before,&after,&car,alpha,&pose);
            double yaw=atan2(pose.look[1]-pose.eye[1],pose.look[0]-pose.eye[0]);
            double error=fabs(atan2(sin(yaw-angle),cos(yaw-angle)));
            if(error>max_error)max_error=error;
            if(time>1){samples++;repeated+=fabs(atan2(sin(yaw-previous_yaw),cos(yaw-previous_yaw)))<1e-6;}
            previous_yaw=yaw;
        }
        CHECK(max_error<1e-5,"camera follows displayed turn direction%d FPS%d max angle error %.9f",direction,rates[r],max_error);
        CHECK(samples>0 && !repeated,"camera turns through every display frame direction%d FPS%d repeats%d",direction,rates[r],repeated);
    }
}

static void test_mouse_look(void) {
    CamCarFrame car={0};cam_car_axes(.7f,(float[3]){0,0,1},&car);
    car.pos[0]=12;car.pos[1]=-8;car.pos[2]=3;
    CamPose p={.eye={5,-14,6},.look={12,-8,4},.up={0,0,1},.view=CAM_FAR,.fov_bau=9400,.znear=.5f};
    CamMouseLook s;CamPose o,moved;cam_mouse_look_begin(&s,&p,&car);cam_mouse_look_pose(&s,&car,&o);
    for(int i=0;i<3;i++){
        CHECK(near(o.eye[i],p.eye[i],2e-6),"orbit starts at displayed eye axis %d",i);
        CHECK(near(o.look[i],p.look[i],2e-6),"orbit starts at displayed target axis %d",i);
    }
    float r=0;for(int i=0;i<3;i++)r+=(p.eye[i]-p.look[i])*(p.eye[i]-p.look[i]);
    cam_mouse_look_move(&s,.4f,.2f);cam_mouse_look_pose(&s,&car,&o);
    float rr=0;for(int i=0;i<3;i++){rr+=(o.eye[i]-o.look[i])*(o.eye[i]-o.look[i]);CHECK(near(o.look[i],p.look[i],2e-6),"orbit target fixed %d",i);}
    CHECK(near(rr,r,2e-5),"orbit retains actual camera distance");
    CamMouseLook down=s;cam_mouse_look_move(&down,0,-100);cam_mouse_look_pose(&down,&car,&moved);
    CHECK(moved.eye[2]>=moved.look[2]-1e-5f,"external orbit cannot sink below its car-facing target");
    car.pos[0]+=5;car.pos[2]+=2;cam_mouse_look_pose(&s,&car,&moved);
    CHECK(near(moved.eye[0]-o.eye[0],5,2e-6)&&near(moved.eye[2]-o.eye[2],2,2e-6),"orbit follows moving car");
    for(int view=CAM_BUMPER;view<=CAM_HOOD;view++){
        cam_car_axes(0,(float[3]){0,0,1},&car);memset(car.pos,0,sizeof car.pos);
        p=(CamPose){.eye={1,.2f,1.3f},.look={11,.2f,1.3f},.up={0,0,1},.view=view,.hide_car=1,.fov_bau=9100,.znear=.4f};
        cam_mouse_look_begin(&s,&p,&car);cam_mouse_look_move(&s,.4f,-.2f);cam_mouse_look_pose(&s,&car,&o);
        for(int i=0;i<3;i++)CHECK(o.eye[i]==p.eye[i],"in-car eye fixed view %d axis %d",view,i);
        CHECK(o.look[1]<p.look[1]&&o.look[2]>p.look[2],"in-car mouse right/up looks right/up view %d",view);
        CHECK(o.hide_car==p.hide_car&&o.fov_bau==p.fov_bau&&o.znear==p.znear,"view rendering preserved %d",view);
        cam_car_axes(1.570796327f,(float[3]){0,0,1},&car);cam_mouse_look_pose(&s,&car,&moved);
        CHECK(near(moved.eye[0],-.2f,2e-6)&&near(moved.eye[1],1,2e-6),"mounted eye follows car turn %d",view);
        cam_mouse_look_move(&s,1e5f,1e5f);cam_mouse_look_pose(&s,&car,&o);
        for(int i=0;i<3;i++)CHECK(isfinite(o.eye[i])&&isfinite(o.look[i])&&isfinite(o.up[i]),"bounded pitch finite %d",view);
        CHECK(near(s.yaw,-35*3.141592653589793/180,1e-6)&&near(s.pitch,0,1e-6),"mounted view stops at left limit without looking down %d",view);
        cam_mouse_look_move(&s,-1e5f,-1e5f);
        CHECK(near(s.yaw,35*3.141592653589793/180,1e-6)&&near(s.pitch,15*3.141592653589793/180,1e-6),"mounted view stops at right/up limits %d",view);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 && *argv[1] ? argv[1] : "docs/cleanroom-camera/test-vectors";
    const char *data = argc > 2 && *argv[2] ? argv[2] : NULL;
    const char *full_path = argc > 3 && *argv[3] ? argv[3] : NULL;
    load_rows(&g_tv01, dir, "tv01_car_camera_entries.csv");
    CHECK(g_nrows == 36, "tv01 rows %d", g_nrows);
    RowSet full = {NULL, 0};
    if (full_path) {   /* optional local full per-car table: any readable path, same columns as tv01 */
        char dirbuf[1024], base[1024]; const char *sl = strrchr(full_path, '/');
        if (sl) { snprintf(dirbuf, sizeof dirbuf, "%.*s", (int)(sl - full_path), sl == full_path ? "/" : full_path); snprintf(base, sizeof base, "%s", sl + 1); }
        else { snprintf(dirbuf, sizeof dirbuf, "."); snprintf(base, sizeof base, "%s", full_path); }
        if (!load_rows(&full, dirbuf, base)) printf("note: full table '%s' not readable; value comparison skipped\n", full_path);
    }
    test_tuning(dir);
    test_smoother(dir);
    test_heading_weight(dir);
    test_accel(dir);
    test_car_data_synthetic();
    test_car_data_tv01b(dir);
    test_car_data_retail(data, &full);
    test_static_pose(dir);
    test_airborne(dir);
    test_shake(dir);
    test_noise(dir);
    test_projection(dir);
    test_projection_policy(dir);
    test_projection_narrow();
    test_cycle(dir);
    test_behaviour();
    test_render_pose();
    test_render_cadence();
    test_mouse_look();
    printf("camera_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
