/* drivetrain_test — runs src/drivetrain.c against docs/cleanroom-driving-physics/test-vectors.
 *
 *   drivetrain_test [VECTOR_DIR]
 *
 * Every Stage 1 vector file is read at run time (CSV, plus a small reader for
 * s1_params.json). The vectors are derived from the spec, not captured from the
 * retail game (spec U-16); they check that the implementation applies the stated
 * formulas. Section "behaviour" at the end holds additional deterministic checks
 * of spec statements that have no vector (marked non-vector in the output); the
 * launch run there is a smoke test only. Exit code is nonzero if any check fails. */
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivetrain.h"

static int g_checks, g_fail;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    if (g_fail <= 80) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static int near_(double a, double b, double tol) { return fabs(a - b) <= tol; }
/* Stored binary32 word of a value, for exact-constant checks (S1-7.7). */
static unsigned float_bits(double v) { float f = (float)v; unsigned b = 0; memcpy(&b, &f, sizeof b); return b; }
/* First "0x........" token of a note field (the stored binary32 pattern). */
static unsigned note_bits(const char *note, int *ok) {
    const char *p = strstr(note, "0x");
    char *end = NULL;
    unsigned long v = p ? strtoul(p, &end, 16) : 0;
    if (ok) *ok = (p && end && end >= p + 3);
    return (unsigned)v;
}

static float test_rand(void *ud) { return *(float *)ud; }
static const char *g_dir = "docs/cleanroom-driving-physics/test-vectors";

/* ---- file + CSV helpers -------------------------------------------------- */
static char *slurp(const char *name) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot read %s\n", path); g_fail++; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); g_fail++; return NULL; }
    s[n] = 0; fclose(f);
    return s;
}
#define CSV_MAXF 16
typedef struct { char *buf; char **line; int n; } Csv;   /* line[0] is the header */
static int csv_open(Csv *c, const char *name) {
    memset(c, 0, sizeof *c);
    c->buf = slurp(name);
    if (!c->buf) return 0;
    int cap = 1024; c->line = (char **)malloc(sizeof(char *) * (size_t)cap);
    for (char *p = c->buf; *p;) {
        if (c->n == cap) { cap *= 2; c->line = (char **)realloc(c->line, sizeof(char *) * (size_t)cap); }
        c->line[c->n++] = p;
        char *e = strchr(p, '\n');
        if (!e) break;
        *e = 0; if (e > p && e[-1] == '\r') e[-1] = 0;
        p = e + 1;
    }
    return 1;
}
static void csv_close(Csv *c) { free(c->buf); free(c->line); }
/* split a data line into fields in a scratch copy */
static int csv_fields(const char *line, char *scratch, char **f) {
    strcpy(scratch, line);
    int n = 0; char *p = scratch;
    f[n++] = p;
    for (; *p; p++) if (*p == ',') { *p = 0; if (n < CSV_MAXF) f[n++] = p + 1; }
    return n;
}

/* ---- tiny JSON reader for s1_params.json ----------------------------------- */
/* Copy the {...} block of top-level key `name`. */
static char *json_block(const char *buf, const char *name) {
    char pat[128]; snprintf(pat, sizeof pat, "\"%s\"", name);
    const char *p = strstr(buf, pat);
    if (!p) return NULL;
    p = strchr(p, '{'); if (!p) return NULL;
    int depth = 0; const char *q = p;
    for (; *q; q++) { if (*q == '{') depth++; else if (*q == '}' && --depth == 0) break; }
    size_t n = (size_t)(q - p) + 1;
    char *out = (char *)malloc(n + 1); memcpy(out, p, n); out[n] = 0;
    return out;
}
/* Numbers of `key` (scalar or array; null -> NAN). Returns count or -1. */
static int json_nums(const char *blk, const char *key, double *out, int max) {
    char pat[128]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(blk, pat);
    if (!p) return -1;
    p = strchr(p, ':'); if (!p) return -1;
    p++; while (*p == ' ' || *p == '\n') p++;
    int n = 0;
    if (*p != '[') { out[0] = !strncmp(p, "true", 4) ? 1 : (!strncmp(p, "false", 5) ? 0 : strtod(p, NULL)); return 1; }
    p++;
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        if (!strncmp(p, "true", 4)) { if (n < max) out[n++] = 1; p += 4; }
        else if (!strncmp(p, "false", 5)) { if (n < max) out[n++] = 0; p += 5; }
        else if (!strncmp(p, "null", 4)) { if (n < max) out[n++] = NAN; p += 4; }
        else { char *e; double v = strtod(p, &e); if (e == p) break; if (n < max) out[n++] = v; p = e; }
    }
    return n;
}
static double json_num(const char *blk, const char *key) {
    double v[1]; return json_nums(blk, key, v, 1) == 1 ? v[0] : NAN;
}

static const char *CARS[4] = { "PEUGOT", "240SX", "SKYLINE", "SYNTH_RWD" };
static char *g_params_json;

/* Build a DrvCar from the JSON record fields (not finalized). Returns the car block (caller frees). */
static char *load_car(const char *name, DrvCar *car) {
    memset(car, 0, sizeof *car);
    char *b = json_block(g_params_json, name);
    if (!b) { CHECK(0, "no params block for %s", name); return NULL; }
    double v[16];
    car->mass = (float)json_num(b, "mass_t");
    car->launch_fraction = (float)json_num(b, "launch_fraction");
    car->final_front = (float)json_num(b, "final_front");
    car->final_rear = (float)json_num(b, "final_rear");
    car->rear_share = (float)json_num(b, "rear_split");
    car->clutch_k = (float)json_num(b, "clutch_k");
    car->gear_count = (int)json_num(b, "gear_count");
    car->clutch_rate = (float)json_num(b, "clutch_rate");
    int n = json_nums(b, "ratios_R_N_1_6", v, 16);
    CHECK(n == 8, "%s ratios n=%d", name, n);
    for (int i = 0; i < 8 && i < n; i++) car->ratio[i] = (float)v[i];
    car->idle_rpm = (float)json_num(b, "idle_rpm");
    car->shift_rpm = (float)json_num(b, "shift_rpm_e1");
    car->max_rpm = (float)json_num(b, "max_rpm_e2");
    car->engine_inertia = (float)json_num(b, "engine_inertia");
    n = json_nums(b, "torque_primary", v, 16);
    for (int i = 0; i < 9 && i < n; i++) car->torque_primary[i] = (float)v[i];
    n = json_nums(b, "torque_secondary", v, 16);
    for (int i = 0; i < 9 && i < n; i++) car->torque_secondary[i] = (float)v[i];
    n = json_nums(b, "engine_brake", v, 16);
    for (int i = 0; i < 3 && i < n; i++) car->engine_brake[i] = (float)v[i];
    n = json_nums(b, "wheel_radius", v, 16);
    for (int i = 0; i < 4 && i < n; i++) car->wheel_radius[i] = (float)v[i];
    n = json_nums(b, "axle_front_slipangle_thr_peak_mu_slide_mu", v, 16);
    if (n == 3) { car->theta_max[0] = (float)v[0]; car->mu_peak[0] = (float)v[1]; car->mu_slide[0] = (float)v[2]; }
    n = json_nums(b, "axle_rear_slipangle_thr_peak_mu_slide_mu", v, 16);
    if (n == 3) { car->theta_max[1] = (float)v[0]; car->mu_peak[1] = (float)v[1]; car->mu_slide[1] = (float)v[2]; }
    return b;
}

/* ---- s1_params.json: S1-1, S1-4.2 -------------------------------------------- */
static void test_params(void) {
    for (int c = 0; c < 4; c++) {
        DrvCar car; char *b = load_car(CARS[c], &car);
        if (!b) continue;
        CHECK(drv_car_finalize(&car) == 0, "%s finalize", CARS[c]);
        double v[16]; int n;
        n = json_nums(b, "torque_composed", v, 16);
        CHECK(n == 9, "%s composed n", CARS[c]);
        for (int i = 0; i < 9 && i < n; i++)
            CHECK(car.torque[i] == (float)v[i], "%s Tq[%d] %.10g vs %.10g", CARS[c], i, car.torque[i], v[i]);
        CHECK(car.drive_class == (int)json_num(b, "class_row"), "%s class %d vs %g", CARS[c], car.drive_class, json_num(b, "class_row"));
        n = json_nums(b, "upshift_table_U", v, 16);
        CHECK(n == 9, "%s U n", CARS[c]);
        for (int i = 0; i < 9 && i < n; i++)
            if (!isnan(v[i])) CHECK(car.up_rpm[i] == (float)v[i], "%s U[%d] %.10g vs %.10g", CARS[c], i, car.up_rpm[i], v[i]);
        n = json_nums(b, "landing_table_D", v, 16);
        CHECK(n == 9, "%s D n", CARS[c]);
        for (int i = 0; i < 9 && i < n; i++)
            if (!isnan(v[i])) CHECK(near_(car.down_rpm[i], v[i], 2e-6 * fabs(v[i])), "%s D[%d] %.10g vs %.10g", CARS[c], i, car.down_rpm[i], v[i]);
        n = json_nums(b, "knots_torque_rpm", v, 16);
        CHECK(n == 9, "%s knots n", CARS[c]);
        for (int i = 0; i < 9 && i < n; i++)
            CHECK(near_(drv_curve_knot_rpm(car.idle_rpm, car.max_rpm, 9, i), v[i], 1e-3), "%s knot %d", CARS[c], i);
        n = json_nums(b, "knots_brake_rpm", v, 16);
        CHECK(n == 3, "%s bknots n", CARS[c]);
        for (int i = 0; i < 3 && i < n; i++)
            CHECK(near_(drv_curve_knot_rpm(car.idle_rpm, car.max_rpm, 3, i), v[i], 1e-3), "%s bknot %d", CARS[c], i);
        free(b);
    }
    /* drive class thresholds and launch scales (S1-1.4) */
    CHECK(drv_drive_class(0.19f) == DRV_CLASS_A && drv_drive_class(0.2f) == DRV_CLASS_B, "class A/B edge");
    CHECK(drv_drive_class(0.8f) == DRV_CLASS_B && drv_drive_class(0.81f) == DRV_CLASS_C, "class B/C edge");
    CHECK(drv_launch_scale(DRV_CLASS_A) == 1.0f && drv_launch_scale(DRV_CLASS_B) == 1.2f && drv_launch_scale(DRV_CLASS_C) == 1.1f, "launch scale");
}

/* ---- s1_engine_torque_<car>.csv: S1-3.1, S1-3.3 ------------------------------- */
static void test_engine_torque(void) {
    for (int c = 0; c < 4; c++) {
        DrvCar car; char *b = load_car(CARS[c], &car); if (!b) continue; free(b);
        CHECK(drv_car_finalize(&car) == 0, "finalize %s", CARS[c]);
        DrvTune tune; drv_tune_default(&tune);
        char name[64]; snprintf(name, sizeof name, "s1_engine_torque_%s.csv", CARS[c]);
        Csv csv; if (!csv_open(&csv, name)) continue;
        CHECK(csv.n > 30, "%s rows %d", name, csv.n);
        for (int r = 1; r < csv.n; r++) {
            char scratch[256]; char *f[CSV_MAXF];
            if (csv_fields(csv.line[r], scratch, f) < 6) continue;
            double rpm = atof(f[0]);
            double e1 = drv_engine_torque_at(&car, &tune, 2, 0, rpm, 1.0);
            double e2 = drv_engine_torque_at(&car, &tune, 2, 0, rpm, 0.5);
            double e3 = drv_engine_torque_at(&car, &tune, 2, 0, rpm, 0.0);
            double e4 = drv_engine_torque_at(&car, &tune, DRV_GEAR_N, 0, rpm, 0.0);
            double e5 = drv_engine_torque_at(&car, &tune, 2, 1, rpm, 1.0);
            CHECK(near_(e1, atof(f[1]), 1e-6), "%s rpm %g u=1: %.7f vs %s", CARS[c], rpm, e1, f[1]);
            CHECK(near_(e2, atof(f[2]), 1e-6), "%s rpm %g u=.5: %.7f vs %s", CARS[c], rpm, e2, f[2]);
            CHECK(near_(e3, atof(f[3]), 1e-6), "%s rpm %g u=0: %.7f vs %s", CARS[c], rpm, e3, f[3]);
            CHECK(near_(e4, atof(f[4]), 1e-6), "%s rpm %g neutral: %.7f vs %s", CARS[c], rpm, e4, f[4]);
            CHECK(near_(e5, atof(f[5]), 1e-6), "%s rpm %g limiter: %.7f vs %s", CARS[c], rpm, e5, f[5]);
        }
        csv_close(&csv);
    }
}

/* ---- s1_substeps.csv: S1-2.2 ------------------------------------------------- */
static void test_substeps(void) {
    Csv csv; if (!csv_open(&csv, "s1_substeps.csv")) return;
    CHECK(csv.n == 13, "substeps rows %d", csv.n);
    for (int r = 1; r < csv.n; r++) {
        char scratch[256]; char *f[CSV_MAXF];
        if (csv_fields(csv.line[r], scratch, f) < 4) continue;
        float dt = (float)atof(f[1]); float h = 0;
        int n = drv_substep_count(dt, &h);
        CHECK(n == atoi(f[2]), "dt %s: N=%d expected %s", f[1], n, f[2]);
        CHECK(h == (float)atof(f[3]), "dt %s: h=%.10g expected %s", f[1], h, f[3]);
    }
    csv_close(&csv);
    /* exact integer quotient still gets the extra substep (S1-2.2) */
    CHECK(drv_substep_count(0.0044f * 2.0f, NULL) >= 3, "integer quotient extra substep");
}

/* ---- s1_clutch_torque.csv: S1-7.3 ---------------------------------------------- */
static void test_clutch_torque(void) {
    Csv csv; if (!csv_open(&csv, "s1_clutch_torque.csv")) return;
    CHECK(csv.n == 25, "clutch rows %d", csv.n);
    for (int r = 1; r < csv.n; r++) {
        char scratch[256]; char *f[CSV_MAXF];
        if (csv_fields(csv.line[r], scratch, f) < 7) continue;
        DrvCar car; char *b = NULL;
        int ci = -1; for (int c = 0; c < 4; c++) if (!strcmp(f[0], CARS[c])) ci = c;
        if (ci < 0) { CHECK(0, "unknown car %s", f[0]); continue; }
        b = load_car(CARS[ci], &car); if (!b) continue; free(b);
        float prev = (float)atof(f[3]);
        double T = drv_clutch_torque(&car, atof(f[1]), atof(f[2]), atof(f[4]), &prev);
        CHECK(near_(T, atof(f[5]), 1e-6), "%s row %d torque %.7f vs %s", f[0], r, T, f[5]);
        CHECK(near_(prev, atof(f[6]), 1e-2), "%s row %d delta %.4f vs %s", f[0], r, prev, f[6]);
    }
    csv_close(&csv);
}

/* ---- s1_axle_split.csv: S1-7.2 step 3 ------------------------------------------ */
static void test_axle_split(void) {
    Csv csv; if (!csv_open(&csv, "s1_axle_split.csv")) return;
    CHECK(csv.n == 7, "axle rows %d", csv.n);
    for (int r = 1; r < csv.n; r++) {
        char scratch[256]; char *f[CSV_MAXF];
        if (csv_fields(csv.line[r], scratch, f) < 6) continue;
        double sa, sb;
        drv_axle_split(atof(f[0]), atof(f[1]), atoi(f[2]), &sa, &sb);
        CHECK(near_(sa, atof(f[4]), 1e-6), "axle row %d share_a %.6f vs %s", r, sa, f[4]);
        CHECK(near_(sb, atof(f[5]), 1e-6), "axle row %d share_b %.6f vs %s", r, sb, f[5]);
    }
    csv_close(&csv);
}

/* ---- s1_perf_class.csv: S1-12 performance class, S1-7.7 kappa-C ---------------- */
static void test_perf_class(void) {
    Csv csv; if (!csv_open(&csv, "s1_perf_class.csv")) return;
    CHECK(csv.n == 66, "perf class rows %d", csv.n);
    int retail = 0, rule = 0, synth = 0, forced = 0, bad_source = 0;
    for (int r = 1; r < csv.n; r++) {
        char scratch[256]; char *f[CSV_MAXF];
        if (csv_fields(csv.line[r], scratch, f) < 6) { CHECK(0, "perf class row %d fields", r); continue; }
        const int flag = atoi(f[2]);
        const int expect = atoi(f[4]);
        CHECK(expect >= 0 && expect <= 3, "perf class row %d class range %s", r, f[4]);
        if (flag != 0) {
            forced++;
            /* S1-12.1: a nonzero forced flag yields class 3 without forming a
             * score; the blank score column is passed as NAN to prove it is unused. */
            CHECK(drv_perf_class_resolve(flag, (float)NAN) == expect,
                  "row %d forced flag %d -> class %d", r, flag, expect);
            continue;
        }
        if (!strcmp(f[1], "retail_stock")) retail++;
        else if (!strcmp(f[1], "rule")) rule++;
        else if (!strcmp(f[1], "synthetic")) synth++;
        else bad_source++;
        const double score = strtod(f[3], NULL);
        /* S1-12.3: strict > thresholds on the stored binary32 score. */
        const int got = drv_perf_class_from_score((float)score);
        CHECK(got == expect, "row %d %s S=%s: class %d expected %d", r, f[1], f[3], got, expect);
        CHECK(drv_perf_class_resolve(0, (float)score) == expect, "row %d unflagged resolve -> %d", r, expect);
    }
    CHECK(bad_source == 0, "perf class sources: unexpected %d", bad_source);
    CHECK(retail == 31 && rule == 8 && synth == 11 && forced == 15,
          "perf class coverage retail %d rule %d synthetic %d forced %d", retail, rule, synth, forced);
    csv_close(&csv);
}

/* ---- s1_kappa_c.csv: S1-7.7 / S1-12.5 coefficient lookup ----------------------- */
static void test_kappa_c(void) {
    Csv csv; if (!csv_open(&csv, "s1_kappa_c.csv")) return;
    CHECK(csv.n == 6, "kappa_c rows %d", csv.n);
    int rows = 0;
    for (int r = 1; r < csv.n; r++) {
        char scratch[256]; char *f[CSV_MAXF];
        if (csv_fields(csv.line[r], scratch, f) < 5) { CHECK(0, "kappa_c row %d fields", r); continue; }
        const int c = atoi(f[1]);
        const double expect = strtod(f[2], NULL);
        const double got = drv_kappa_c(c);
        /* S1-7.7/S1-12.5: the four K entries and the preceding -1 slot are
         * stored binary32 constants. The CSV decimal is the exact real value of
         * that word and the note carries its bit pattern; both are checked. */
        CHECK(got == expect, "row %d %s C=%d: %.17g expected %s", r, f[0], c, got, f[2]);
        int have_bits = 0;
        const unsigned want_bits = note_bits(f[4], &have_bits);
        CHECK(have_bits, "kappa_c row %d note carries no binary32 pattern", r);
        if (have_bits)
            CHECK(float_bits(got) == want_bits, "row %d %s C=%d coefficient bits 0x%08X expected 0x%08X",
                  r, f[0], c, float_bits(got), want_bits);
        rows++;
    }
    CHECK(rows == 5, "kappa_c rows exercised %d", rows);
    csv_close(&csv);
}

/* ---- s1_freerev_neutral_<car>.csv: S1-2, S1-3, S1-4.4 ------------------------------ */
static void test_freerev(void) {
    for (int c = 0; c < 4; c++) {
        DrvCar car; char *b = load_car(CARS[c], &car); if (!b) continue; free(b);
        DrvTune tune; drv_tune_default(&tune);
        Drivetrain d;
        CHECK(drv_init(&d, &car, &tune) == 0, "init %s", CARS[c]);
        DrvEnv env; drv_env_default(&env);
        DrvBodyStraight body = { d.car.mass, 0.0f };
        drv_couple(&d, &env, drv_body_accel_straight, &body);
        d.throttle_mult = 1.0f; /* Vectors explicitly override creation mc=0.7. */
        char name[64]; snprintf(name, sizeof name, "s1_freerev_neutral_%s.csv", CARS[c]);
        Csv csv; if (!csv_open(&csv, name)) continue;
        CHECK(csv.n == 601, "%s rows %d", name, csv.n);
        float frame_dt = 1.0f / 60.0f; float h = 0;
        int N = drv_substep_count(frame_dt, &h);
        CHECK(N == 4, "60 fps -> 4 substeps");
        DrvInput in = { 1.0f, 0.0f, 0.0f, DRV_GEAR_N };
        int shown = 0, lim_bad = 0;
        double max_dw = 0;
        for (int r = 1; r < csv.n; r++) {
            char scratch[256]; char *f[CSV_MAXF];
            if (csv_fields(csv.line[r], scratch, f) < 9) continue;
            int frame = atoi(f[0]), sub = atoi(f[1]);
            if (sub == 0) drv_frame_begin(&d, &in, frame_dt, -1.0);
            drv_integrate(&d, h);
            drv_couple(&d, &env, drv_body_accel_straight, &body);
            double w = d.omega_e, rpm = w * DRV_KR;
            double dw = fabs(w - atof(f[3]));
            if (dw > max_dw) max_dw = dw;
            int ok = dw <= 0.05;
            ok = ok && near_(rpm, atof(f[4]), 0.05 * DRV_KR + 0.01);
            ok = ok && d.mode == atoi(f[6]);
            ok = ok && near_(d.engage, atof(f[7]), 1.1e-5);
            ok = ok && near_(d.throttle_mult, atof(f[8]), 1e-6);
            if (d.limiter != atoi(f[5])) lim_bad++;
            ok = ok && d.limiter == atoi(f[5]);
            g_checks++;
            if (!ok) {
                g_fail++;
                if (shown++ < 5)
                    printf("FAIL %s frame %d sub %d: w=%.4f(%s) lim=%d(%s) mode=%d(%s) c=%.5f(%s) mc=%.3f(%s)\n", CARS[c],
                           frame, sub, w, f[3], d.limiter, f[5], d.mode, f[6], d.engage, f[7], d.throttle_mult, f[8]);
            }
        }
        printf("  freerev %-9s max |d omega| = %.5f rad/s, limiter mismatches %d\n", CARS[c], max_dw, lim_bad);
        csv_close(&csv);
    }
}

/* Isolated supplied-state regression vectors from the current reviewed spec. */
static void constant_accel(void *ud, const float F[4], const float Fy[4], float A[4]) {
    (void)F; (void)Fy;
    for (int i = 0; i < 4; i++) A[i] = *(float *)ud;
}
static void test_question_cases(void) {
    char *buf = slurp("s1_question_cases.json");
    if (!buf) return;
    const double tol = json_num(buf, "numeric_tolerance");
    const char *p = strstr(buf, "\"cases\"");
    CHECK(p != NULL, "question vector cases array");
    if (!p) { free(buf); return; }
    p = strchr(p, '[') + 1;
    int cases = 0;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == ',' || *p == '\r') p++;
        if (*p == ']') break;
        CHECK(*p == '{', "question vector object");
        if (*p != '{') break;
        const char *end = p; int depth = 0;
        do { if (*end == '{') depth++; if (*end == '}') depth--; end++; } while (*end && depth);
        size_t len = (size_t)(end - p);
        char *item = malloc(len + 1); memcpy(item, p, len); item[len] = 0; p = end;
        char *in = json_block(item, "input"), *out = json_block(item, "expected");
        CHECK(in && out, "question vector input and expected objects");
        if (!in || !out) { free(in); free(out); free(item); continue; }
        cases++;
        DrvCar car; char *b = load_car("SYNTH_RWD", &car); free(b);
        Drivetrain d; drv_init(&d, &car, NULL);
        DrvEnv env; drv_env_default(&env); env.body_speed = 30;
        if (strstr(item, "\"axle_share\"")) {
            double a, z;
            drv_axle_split(json_num(in, "omega_a"), json_num(in, "omega_b"), (int)json_num(in, "both_contact"), &a, &z);
            CHECK(near_(z, json_num(out, "share_b"), tol), "Q1 axle share");
        } else if (strstr(item, "\"tyre_branch\"")) {
            d.wheel[0].omega = (float)json_num(in, "omega");
            d.wheel[0].dir = d.wheel[0].omega ? DRV_DIR_FORWARD : DRV_DIR_STOPPED;
            d.car.wheel_radius[0] = (float)json_num(in, "radius");
            env.wheel[0].vx = (float)json_num(in, "vx"); env.wheel[0].vy = (float)json_num(in, "vy");
            env.wheel[0].locked = (int)json_num(in, "locked");
            drv_couple(&d, &env, NULL, NULL);
            CHECK(near_(d.wheel[0].slip_speed, json_num(out, "slip"), tol), "Q3 slip");
            CHECK(d.wheel[0].gripping == !!strstr(out, "\"gripping\""), "Q3 contact translation branch");
        } else if (strstr(item, "\"automatic_downshift\"")) {
            d.gear = d.target_gear = (int)json_num(in, "g"); d.mode = (int)json_num(in, "mode");
            d.car.idle_rpm = (float)json_num(in, "idle_rpm");
            DrvInput input = { (float)json_num(in, "throttle"), 0, 0, DRV_CMD_AUTO };
            drv_frame_begin(&d, &input, 0, json_num(in, "rpm"));
            CHECK((d.target_gear == DRV_GEAR_N) == (int)json_num(out, "requests_neutral"), "Q5 positive idle stays in first");
        } else if (strstr(item, "\"assist_report\"")) {
            d.gear = (int)json_num(in, "g"); d.target_gear = (int)json_num(in, "target");
            d.mode = DRV_CLUTCH_LOCKED; d.sel_gear = (int)json_num(in, "gs");
            drv_assist_supply(&d, d.sel_gear, (float)json_num(in, "elapsed"), (int)json_num(in, "remembered_gear"));
            d.manual_hold_time = (float)json_num(in, "elapsed"); d.car.max_rpm = (float)json_num(in, "max_rpm");
            DrvInput input = { 1, 0, 0, DRV_CMD_MANUAL_ASSIST };
            drv_frame_begin(&d, &input, 0, json_num(in, "rpm"));
            CHECK(d.report_auto == (int)json_num(out, "report_auto"), "Q6 assist report %.1f sec", json_num(in, "elapsed"));
        } else if (strstr(item, "\"slip_sync\"")) {
            double v[4], expected[4]; json_nums(in, "omega", v, 4); json_nums(out, "omega", expected, 4);
            d.car.rear_share = 0.5f; d.gear = d.target_gear = 3; d.mode = DRV_CLUTCH_LOCKED;
            d.car.ratio[3] = (float)json_num(in, "g_ratio"); d.car.final_front = (float)json_num(in, "front_final");
            d.car.final_rear = (float)json_num(in, "rear_final"); d.omega_e = (float)json_num(in, "engine_omega"); d.throttle = 1;
            env.body_speed = (float)json_num(in, "body_speed_mps");
            double contacts[4]; json_nums(in, "contact", contacts, 4);
            for (int i = 0; i < 4; i++) { d.wheel[i].omega = (float)v[i]; env.wheel[i].contact = (int)contacts[i]; }
            CHECK(near_(drv_wheel_side_speed(&d), json_num(out, "old_wheel_side_speed"), tol), "Q7 old combined snapshot");
            drv_couple(&d, &env, NULL, NULL);
            for (int i = 0; i < 4; i++) CHECK(near_(d.wheel[i].omega, expected[i], tol), "Q7 corrected wheel %d", i);
            CHECK(near_(drv_wheel_side_speed(&d), json_num(out, "new_wheel_side_speed"), tol), "Q7 synchronized combined speed");
        } else if (strstr(item, "\"first_gear_feedback\"")) {
            /* S1-12: the per-instance class reaches the block through the
             * creation-time copy, so the vector supplies it at init. */
            DrvTune fb_tune; drv_tune_default(&fb_tune);
            fb_tune.perf_class = (signed char)json_num(in, "class");
            drv_init(&d, &car, &fb_tune);
            d.gear = d.target_gear = (int)json_num(in, "gear"); d.mode = (int)json_num(in, "mode");
            d.feedback = (float)json_num(in, "feedback"); d.throttle = d.throttle_mult = 1;
            for (int i = 0; i < 9; i++) d.car.torque[i] = (float)json_num(in, "engine_torque");
            for (int i = 0; i < 4; i++) env.wheel[i].contact = 0;
            drv_couple(&d, &env, NULL, NULL);
            CHECK(near_(d.feedback, json_num(out, "feedback"), tol), "Q8 independent open first-gear feedback");
        } else if (strstr(item, "\"friction_limit\"")) {
            /* R=1, tau=1 and supplied body A make every force pass return the prescribed Fi. */
            d.gear = d.target_gear = 2; d.mode = DRV_CLUTCH_ENGAGING; d.engage = 1;
            d.car.rear_share = 0; d.car.final_front = 1; d.car.ratio[2] = 2; d.car.clutch_k = 1;
            d.omega_e = 100; d.omega_w = (float)(100 - json_num(in, "drive_torque"));
            d.car.wheel_radius[0] = 1; d.car.mu_peak[0] = 1;
            env.wheel[0].load = (float)json_num(in, "force_limit"); env.wheel[0].vx = (float)json_num(in, "vx");
            env.wheel[0].locked = (int)json_num(in, "locked"); env.wheel[0].roll_drag = 0;
            env.wheel[0].lateral_grip_force = (float)json_num(in, "lateral_force");
            float accel = (float)((json_num(in, "drive_torque") - json_num(in, "longitudinal_force")) / DRV_WHEEL_INERTIA);
            drv_couple(&d, &env, constant_accel, &accel);
            const double expected = json_num(out, "stored_longitudinal_force");
            CHECK(near_(d.wheel[0].force, expected, tol), "Q9 stored force %.5f vs %.5f", d.wheel[0].force, expected);
            if (!isnan(json_num(out, "body_longitudinal_force"))) {
                CHECK(near_(d.force_long[0], json_num(out, "body_longitudinal_force"), tol), "Q9 body uses stored force");
                const double torque = d.wheel[0].tau;
                drv_integrate(&d, 0.0005f);
                CHECK(near_(d.wheel[0].tau_smooth, 0.5 * (torque - json_num(out, "wheel_derivative_force")), tol), "Q9 spin uses stored force");
            }
        } else if (strstr(item, "\"angle_adjustment\"")) {
            d.wheel[0].phi = (float)json_num(in, "phi"); drv_integrate(&d, 0);
            CHECK(near_(d.wheel[0].phi, json_num(out, "phi"), tol), "Q10 angle %g -> %g expected %g", json_num(in, "phi"), d.wheel[0].phi, json_num(out, "phi"));
        } else if (strstr(item, "\"initial_coupling_speed\"")) {
            double spins[2]; json_nums(in, "wheel_omega", spins, 2);
            d.gear = DRV_GEAR_N; d.target_gear = 2; d.mode = DRV_CLUTCH_OPEN; d.pending_shift = 1;
            d.car.rear_share = 1; d.car.ratio[DRV_GEAR_N] = (float)json_num(in, "old_ratio");
            d.car.ratio[2] = (float)json_num(in, "new_ratio"); d.car.final_rear = (float)json_num(in, "axle_final");
            d.wheel[2].omega = (float)spins[0]; d.wheel[3].omega = (float)spins[1];
            d.omega_e = (float)json_num(in, "engine_omega"); d.engage = (float)json_num(in, "engagement"); d.car.idle_rpm = 0;
            drv_integrate(&d, 0); drv_couple(&d, &env, NULL, NULL);
            CHECK(near_(d.omega_w, json_num(out, "stored_wheel_side_speed"), tol), "Q12 keeps clutch-post speed across swap");
            CHECK(near_(-d.delta_prev / DRV_KR, json_num(out, "initial_slip_delta"), tol), "Q12 uses cached slip delta");
            CHECK(near_(d.wheel[2].tau + d.wheel[3].tau, json_num(out, "distributed_slip_torque"), tol), "Q12 zero engagement torque");
        } else if (strstr(item, "\"creation_poststep\"")) {
            CHECK(d.gear == (int)json_num(in, "gear") && d.mode == (int)json_num(in, "mode"), "Q13 creation supplied neutral state");
            CHECK(near_(d.feedback, json_num(out, "engine_feedback"), tol), "Q13 initial neutral feedback");
            CHECK(near_(d.throttle_mult, json_num(out, "throttle_multiplier"), tol), "Q13 creation runs zero-time post-step");
        } else if (strstr(item, "\"planar_body\"")) {
            DrvBodyPlanar bp; memset(&bp, 0, sizeof bp);
            bp.mass = (float)json_num(in, "mass"); bp.inv_iz = (float)json_num(in, "inverse_yaw_inertia");
            double f[2], arm[2]; json_nums(in, "force", f, 2); json_nums(in, "arm", arm, 2);
            bp.pos[0][0] = (float)arm[0]; bp.pos[0][1] = (float)arm[1];
            bp.head[0][0] = bp.lat[0][1] = 1;
            float F[4] = { (float)f[0], 0, 0, 0 }, Fy[4] = { (float)f[1], 0, 0, 0 }, acceleration[3];
            /* BC-2 replaces the old heading-based projection probes. The
             * component vector still supplies the same force sums/moment. */
            drv_body_planar_acceleration(&bp, F, Fy, acceleration);
            CHECK(near_(acceleration[0], json_num(out, "ax"), tol) && near_(acceleration[1], json_num(out, "ay"), tol), "Q15 body translation");
            CHECK(near_(acceleration[2], json_num(out, "yaw_acceleration"), tol), "Q15 positive yaw moment");
        } else if (strstr(item, "\"lock_correction\"")) {
            d.gear = d.target_gear = 2; d.car.rear_share = (float)json_num(in, "rear_share");
            d.car.ratio[2] = (float)json_num(in, "ratio"); d.car.final_front = (float)json_num(in, "front_final");
            d.car.engine_inertia = (float)json_num(in, "engine_inertia"); d.feedback = (float)json_num(in, "engine_feedback");
            d.throttle = d.throttle_mult = 1;
            for (int i = 0; i < 9; i++) d.car.torque[i] = (float)json_num(in, "engine_torque");
            double tau[2], radii[2], forces[2]; json_nums(in, "front_drive_torques", tau, 2);
            json_nums(in, "front_radii", radii, 2); json_nums(in, "updated_front_forces", forces, 2);
            for (int i = 0; i < 2; i++) { d.wheel[i].tau = (float)tau[i]; d.car.wheel_radius[i] = (float)radii[i]; d.wheel[i].force = (float)forces[i]; }
            CHECK(near_(drv_lock_correction(&d), json_num(out, "correction_torque"), tol), "Q4 correction uses current forces");
        } else CHECK(0, "unknown question vector kind");
        free(in); free(out); free(item);
    }
    CHECK(cases == 18, "all 18 question cases exercised, got %d", cases);
    free(buf);
}

/* Independent five-pass oracle: callback observes each pass before force updates. */
typedef struct {
    Drivetrain *d;
    float torque[4], force[4], feedback;
    int passes;
} PassTrace;
static void trace_pass(void *ud, const float F[4], const float Fy[4], float A[4]) {
    PassTrace *t = ud;
    Drivetrain *d = t->d;
    CHECK(t->passes < 5, "exactly five coupling passes");
    CHECK(near_(d->feedback, t->feedback, 2e-6), "pass %d reaction includes prior correction", t->passes);
    DrvBodyStraight body = { 2, 0.3f };
    drv_body_accel_straight(&body, F, Fy, A);
    for (int i = 0; i < 4; i++) {
        CHECK(near_(F[i], t->force[i], 2e-6), "pass %d wheel %d current force", t->passes, i);
        CHECK(near_(d->wheel[i].tau, t->torque[i], 2e-6), "pass %d wheel %d prior correction distributed", t->passes, i);
        t->force[i] = (float)(t->force[i] + (t->torque[i] - 0.5 * t->force[i]) - A[i] * (double)DRV_WHEEL_INERTIA / 0.5);
    }
    const double ae = ((double)0.2f + t->feedback) / (double)0.1f;
    const double aw = 8 * ((t->torque[0] - 0.5 * t->force[0]) + (t->torque[1] - 0.5 * t->force[1])) / (2 * (double)DRV_WHEEL_INERTIA);
    const double denominator = 1 / (double)0.1f + 32 / (double)DRV_WHEEL_INERTIA;
    const double correction = (ae - aw) / denominator;
    t->feedback = (float)(t->feedback - correction);
    for (int i = 0; i < 2; i++) t->torque[i] = (float)(t->torque[i] + 4 * correction);
    t->passes++;
}
static void test_amended_boundaries(void) {
    DrvCar car; char *b = load_car("SYNTH_RWD", &car); free(b);
    Drivetrain d;
    DrvInput assist = { 1, 0, 0, DRV_CMD_MANUAL_ASSIST };
    drv_init(&d, &car, NULL);
    d.mode = DRV_CLUTCH_LOCKED; d.gear = d.target_gear = 3;
    drv_frame_begin(&d, &assist, 12, 5000);
    CHECK(!d.assist_state_supplied && !d.report_auto, "unknown assist initial state remains unimplemented");
    CHECK(drv_assist_supply(&d, 3, 9.5f, 3) == 0, "caller supplies assist state");
    d.mode = DRV_CLUTCH_ENGAGING;
    drv_frame_begin(&d, &assist, 0.5f, 5000);
    CHECK(d.manual_hold_time == 10 && !d.report_auto, "assist clock advances outside Locked mode");
    d.mode = DRV_CLUTCH_LOCKED;
    drv_frame_begin(&d, &assist, 0, 5000);
    CHECK(!d.report_auto && d.manual_hold_time == 10, "assist elapsed ==10 retains assist");
    drv_frame_begin(&d, &assist, 0.25f, 5000);
    CHECK(d.report_auto && d.manual_hold_time == 10.25f, "reporting automatic retains elapsed timer");
    drv_frame_begin(&d, &assist, 0.25f, 5000);
    CHECK(d.report_auto && d.manual_hold_time == 10.5f, "automatic report can persist on next assist call");
    drv_assist_supply(&d, DRV_GEAR_N, 5, 3);
    CHECK(drv_assist_select(&d, 4) == 0 && d.manual_hold_time == 0, "selection from neutral resets clock");
    drv_frame_begin(&d, &assist, 0.25f, 5000);
    CHECK(d.target_gear == 4 && d.manual_last_gear == 3 && d.manual_hold_time == 0, "manual up request remembers pre-change gear and resets timer");
    d.gear = d.target_gear = 4;
    drv_frame_begin(&d, &assist, 0.25f, d.car.max_rpm - 300);
    CHECK(!d.report_auto, "remembered upshift rpm equality retains assist");
    drv_frame_begin(&d, &assist, 0, d.car.max_rpm - 299);
    CHECK(d.report_auto, "gear above remembered pre-change gear reports at high rpm");
    d.gear = d.target_gear = 3; drv_assist_supply(&d, 2, 3, 2);
    const double ceiling = ((double)d.car.ratio[3] / d.car.ratio[2]) * 1.2 * d.car.max_rpm;
    drv_frame_begin(&d, &assist, 0.2f, ceiling + 1);
    CHECK(d.target_gear == 3 && d.sel_gear == 3 && d.manual_last_gear == 3, "assist cancels over-rev down request");
    drv_assist_select(&d, 2); drv_frame_begin(&d, &assist, 0.2f, ceiling);
    CHECK(d.target_gear == 2 && d.manual_hold_time == 0, "assist permits down request at ceiling equality");
    d.gear = d.target_gear = 3; drv_assist_supply(&d, 3, 0, 3);
    drv_frame_begin(&d, &assist, 0.1f, d.car.idle_rpm + 99);
    CHECK(d.target_gear == 2 && d.report_auto, "assist low-rpm fallback downshifts and reports automatic");
    int old_selected = d.sel_gear;
    CHECK(drv_assist_select(&d, 1) == -1 && drv_assist_select(&d, d.car.gear_count + 2) == -1 && d.sel_gear == old_selected,
          "accepted manual requests stay within forward selection range");

    /* Full rear multiplier is independent of orientation assumptions. */
    DrvEnv env; drv_env_default(&env); env.body_speed = 30;
    for (int run = 0; run < 2; run++) {
        drv_init(&d, &car, NULL);
        d.car.mu_slide[1] = d.car.mu_peak[1] = 1;
        d.wheel[2].omega = 100; env.wheel[2].vx = 10; env.wheel[2].load = 2;
        env.rear_grip_multiplier = run ? 0.4f : 1.6f;
        drv_couple(&d, &env, NULL, NULL);
        CHECK(near_(d.force_long[2], 2 * env.rear_grip_multiplier, 1e-5), "supplied rear multiplier %g controls sliding grip", env.rear_grip_multiplier);
    }
    /* Signed lateral force arrives already calculated; only wT applies. */
    drv_init(&d, &car, NULL); drv_env_default(&env);
    env.wheel[0].lateral_grip_force = -2; env.wheel[0].load = 100;
    d.wheel[0].w_lat = 0.5f;
    drv_couple(&d, &env, NULL, NULL);
    CHECK(d.force_lat[0] == -1, "gripping lateral input preserves sign and uses wT");

    drv_init(&d, &car, NULL); drv_env_default(&env);
    d.gear = d.target_gear = 3; d.mode = DRV_CLUTCH_LOCKED;
    d.car.rear_share = 0; d.car.ratio[3] = 2; d.car.final_front = 4; d.car.engine_inertia = 0.1f;
    d.car.clutch_k = 0; d.throttle = d.throttle_mult = 1;
    for (int i = 0; i < 9; i++) d.car.torque[i] = 0.2f;
    PassTrace t; memset(&t, 0, sizeof t); t.d = &d; t.feedback = -0.2f;
    for (int i = 0; i < 4; i++) {
        d.car.wheel_radius[i] = 0.5f; d.wheel[i].omega = 100;
        env.wheel[i].vx = 50; env.wheel[i].load = 100;
        t.torque[i] = i < 2 ? 0.8f : 0;
        t.force[i] = 2 * t.torque[i];
    }
    drv_couple(&d, &env, trace_pass, &t);
    CHECK(t.passes == 5, "lock correction occurs in each of five passes");
    CHECK(near_(d.feedback, t.feedback, 2e-6), "fifth lock correction contributes engine reaction");
    for (int i = 0; i < 4; i++) {
        CHECK(near_(d.wheel[i].tau, t.torque[i], 2e-6), "fifth correction distributed at wheel %d", i);
        CHECK(near_(d.force_long[i], t.force[i], 2e-6), "fifth updated force stored at wheel %d", i);
    }
    /* Cached initial speed survives finishing's independent resample. */
    CHECK(d.omega_w == 0, "non-sync finishing does not overwrite cached initial speed");

    const float angles[] = { 628.3179931640625f, -628.3179931640625f, -8000, 1000 };
    const float expected[] = { 628.3179931640625f, -628.3179931640625f, 0, 371.6820068359375f };
    for (int i = 0; i < 4; i++) {
        drv_init(&d, &car, NULL); d.wheel[0].phi = angles[i]; drv_integrate(&d, 0);
        CHECK(d.wheel[0].phi == expected[i], "angle boundary/one-step %g", angles[i]);
    }
    DrvTune tune; drv_tune_default(&tune); tune.initial_rpm = car.max_rpm;
    drv_init(&d, &car, &tune);
    CHECK(d.limiter && d.throttle_mult == 0.7f && d.feedback == 0, "supplied high-rpm creation runs engine and clutch post-steps");
}

/* ---- behaviour checks of spec statements without vectors (non-vector) ---------------- */
static void test_behaviour(void) {
    DrvCar car; char *b = load_car("SYNTH_RWD", &car); free(b);
    DrvTune tune; drv_tune_default(&tune);
    Drivetrain d;

    /* S1-3.1 curve edges */
    {
        float s[3] = { 1, 2, 4 };
        CHECK(drv_curve(s, 3, 0, 10, DBL_MAX) == 4, "curve huge upper bound");
        CHECK(drv_curve(s, 3, 0, 10, -DBL_MAX) == 1, "curve huge lower bound");
        CHECK(isnan(drv_curve(s, 3, 0, 10, NAN)), "curve invalid query");
        CHECK(isnan(drv_curve(s, 3, 0, 0, 1)), "curve invalid domain");
        CHECK(drv_curve(s, 3, 0, 10, -5) == 1, "curve below");
        CHECK(drv_curve(s, 3, 0, 10, -0.1) == 1, "curve p in (-1,0)");
        CHECK(drv_curve(s, 3, 0, 10, 10) == 4 && drv_curve(s, 3, 0, 10, 99) == 4, "curve above");
        CHECK(near_(drv_curve(s, 3, 0, 10, 7.5), 3.0, 1e-12), "curve interp");
    }
    /* S1-3.3: G and B scale only the drive part */
    {
        DrvTune t2 = tune; t2.torque_scale = 2.0f; t2.torque_bonus = 1;
        DrvCar c2 = car; drv_car_finalize(&c2);
        double base = drv_engine_torque_at(&c2, &tune, 2, 0, 3000, 1.0);
        double scaled = drv_engine_torque_at(&c2, &t2, 2, 0, 3000, 1.0);
        CHECK(near_(scaled, base * 2.0 * 1.05, 1e-9), "G and B scale");
    }
    /* S1-5 input shaping */
    {
        CHECK(drv_init(&d, &car, &tune) == 0, "init");
        DrvInput in = { 0.10f, 0.0f, 0.0f, DRV_GEAR_N };
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.throttle == 0.0f, "dead-zone");
        in.throttle = 0.3f; drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.throttle == 0.3f, "neutral: no anti-stall floor");
        in.throttle = 0.4f; in.brake = 0.6f; drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.throttle == 0.0f && d.brake == 0.6f, "brake wins over small throttle");
        in.throttle = 0.7f; in.brake = 0.6f; in.handbrake = 0.4f; drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.throttle == 0.7f && d.brake == 0.6f && d.handbrake == 0.0f, "throttle zeroes small handbrake only");
        d.gear = 2; d.target_gear = 2;
        in.throttle = 0.3f; in.brake = 0; in.handbrake = 0; drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.throttle == 0.5f, "anti-stall floor in gear below 3000 rpm");
    }
    /* S1-4.5 selector: automatic from neutral selects first gear */
    {
        drv_init(&d, &car, &tune);
        DrvInput in = { 1.0f, 0, 0, DRV_CMD_AUTO };
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 2, "auto from neutral -> first");
        in.gear_cmd = 3; drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 3, "direct command");
    }
    /* S1-4.5 automatic up/down-shift, locked, jitter 0 */
    {
        DrvEnv env; drv_env_default(&env);
        drv_init(&d, &car, &tune);
        d.gear = d.target_gear = 2; d.mode = DRV_CLUTCH_LOCKED; d.engage = 1; d.sel_gear = -1;
        DrvInput in = { 1.0f, 0, 0, DRV_CMD_AUTO };
        d.omega_e = 5800.0f * DRV_KW;           /* U[2] = 5900 */
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 2, "below up-shift rpm");
        d.omega_e = 5950.0f * DRV_KW;
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 3, "up-shift at rpm >= U");
        d.target_gear = 2; d.wheel[1].slip_speed = 5.0f;
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 2, "slipping wheel blocks up-shift");
        d.wheel[1].slip_speed = 0; d.env.wheel[2].contact = 0;
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 2, "airborne wheel blocks up-shift");
        d.env.wheel[2].contact = 1;
        /* down-shift from 3rd: D[3] = 3933.33; second condition (r3/r2)*(U[2]-100)-200 */
        d.gear = d.target_gear = 3; d.omega_e = 3000.0f * DRV_KW;
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 2, "down-shift when rpm <= D and <= d2");
        d.gear = d.target_gear = 3; d.omega_e = 3950.0f * DRV_KW;
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 3, "no down-shift above D");
        /* rpm override (U-10) */
        d.gear = d.target_gear = 3; d.omega_e = 3000.0f * DRV_KW;
        drv_frame_begin(&d, &in, 1.0f / 60, 4500.0);
        CHECK(d.target_gear == 3, "selector uses the override rpm");
        /* not Locked: no change */
        d.mode = DRV_CLUTCH_ENGAGING; d.gear = d.target_gear = 3; d.omega_e = 3000.0f * DRV_KW;
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        CHECK(d.target_gear == 3, "selector frozen unless Locked");
        (void)env;
    }
    /* S1-4.4 clutch mode machine: a shift request opens the clutch, swaps gear, re-engages */
    {
        drv_init(&d, &car, &tune);
        DrvEnv env; drv_env_default(&env);
        DrvBodyStraight body = { d.car.mass, 0 };
        drv_couple(&d, &env, drv_body_accel_straight, &body);
        d.gear = 2; d.target_gear = 3; d.mode = DRV_CLUTCH_LOCKED; d.engage = 1;
        d.omega_e = 3000.0f * DRV_KW;
        float h = 1.0f / 240; int saw0 = 0, saw1 = 0, saw2 = 0, saw3_after = 0;
        DrvInput in = { 1.0f, 0, 0, DRV_CMD_AUTO };
        drv_frame_begin(&d, &in, 1.0f / 60, -1);
        for (int i = 0; i < 600; i++) {
            drv_integrate(&d, h);
            drv_couple(&d, &env, drv_body_accel_straight, &body);
            if (d.mode == DRV_CLUTCH_DISENGAGING) saw0 = 1;
            if (d.mode == DRV_CLUTCH_OPEN) saw1 = 1;
            if (d.mode == DRV_CLUTCH_ENGAGING && d.gear == 3) saw2 = 1;
            if (d.mode == DRV_CLUTCH_LOCKED && d.gear == 3 && saw2) saw3_after = 1;
        }
        CHECK(saw0 && saw1 && saw2, "mode sequence disengage/open/engage (%d %d %d)", saw0, saw1, saw2);
        CHECK(d.gear == 3, "gear swapped");
        (void)saw3_after;
    }
    /* S1-7.2 neutral: no wheel torque and no reaction */
    {
        drv_init(&d, &car, &tune);
        DrvEnv env; drv_env_default(&env);
        DrvBodyStraight body = { d.car.mass, 0 };
        d.omega_e = 3000.0f * DRV_KW; d.engage = 1; d.mode = DRV_CLUTCH_LOCKED;
        drv_couple(&d, &env, drv_body_accel_straight, &body);
        CHECK(d.feedback == 0.0f && d.wheel[2].tau == 0.0f && d.wheel[3].tau == 0.0f, "neutral: no torque, no reaction");
    }
    /* S1-7.3 / 7.2: clutch slip torque reaches the rear wheels of an RWD car, reaction on engine.
     * An explicit computed class with K[0] = 0.0 keeps the independent S1-7.7
     * first-gear term at zero so this fixture isolates the clutch reaction; it
     * does not rely on the unset class (U-17). */
    {
        DrvTune t3 = tune; t3.perf_class = 0;
        drv_init(&d, &car, &t3);
        DrvEnv env; drv_env_default(&env);
        DrvBodyStraight body = { d.car.mass, 0 };
        d.gear = d.target_gear = 2; d.mode = DRV_CLUTCH_ENGAGING; d.engage = 0.5f;
        d.omega_e = 200.0f; d.delta_prev = 0;
        drv_couple(&d, &env, drv_body_accel_straight, &body);
        double T = 0.05 * 0.5 * 200.0;          /* k' c Delta = 5 kN m */
        double Ar = 0.9f * 4.0f * (T * 3.0);    /* s Fr T r[g] */
        double Af = 0.1f * 4.0f * (T * 3.0);
        CHECK(near_(d.feedback, -T, 1e-4), "reaction torque %.5f vs %.5f", d.feedback, -T);
        CHECK(near_(d.wheel[2].tau + d.wheel[3].tau, Ar, 1e-3) && near_(d.wheel[0].tau + d.wheel[1].tau, Af, 1e-3),
              "axle torques rear %.4f/%.4f front %.4f/%.4f", d.wheel[2].tau + d.wheel[3].tau, Ar, d.wheel[0].tau + d.wheel[1].tau, Af);
    }
    /* S1-3.5 post-step: idle clamp and limiter hysteresis */
    {
        drv_init(&d, &car, &tune);
        d.omega_e = 1.0f; d.throttle = 0;
        drv_integrate(&d, 1.0f / 240);
        CHECK(d.omega_e >= 1000.0f * DRV_KW - 1e-4, "clamped to idle");
        d.omega_e = 6990.0f * DRV_KW; d.limiter = 0;
        drv_integrate(&d, 1.0f / 240);
        CHECK(d.limiter == 1, "limiter on above eps2-25");
        d.omega_e = 6900.0f * DRV_KW;
        drv_integrate(&d, 1.0f / 240);
        CHECK(d.limiter == 0, "limiter off below eps2*(1-0.0125)");
    }
    /* S1-2.1 frame-time quantiser; S1-8.2 planar body reduces to the straight-line case */
    {
        DrvTune tq = tune;
        CHECK(drv_quantize_frame_dt_test_adapter(&tq, 0.0170) == (float)(31.0 / 1800.0), "quantise rounds to nearest 1/1800");
        tq.frame_dt_max = 0.05f;
        CHECK(drv_quantize_frame_dt_test_adapter(&tq, 1.0) == 0.05f, "tune clamp");
        DrvBodyStraight bs = { 1.2f, 0.3f };
        DrvBodyPlanar bp; memset(&bp, 0, sizeof bp);
        bp.mass = 1.2f; bp.inv_iz = 1.0f / 1.5f; bp.ex = 0.3f;
        for (int i = 0; i < 4; i++) { bp.head[i][0] = 1; bp.lat[i][1] = 1; }
        const float xs[4] = { 1.3f, 1.3f, -1.2f, -1.2f }, ys[4] = { 0.8f, -0.8f, -0.8f, 0.8f };
        for (int i = 0; i < 4; i++) { bp.pos[i][0] = xs[i]; bp.pos[i][1] = ys[i]; }
        float F[4] = { 0.5f, 0.5f, 1.0f, 1.0f }, Fy[4] = { 0, 0, 0, 0 }, As[4], Ap[4];
        drv_body_accel_straight(&bs, F, Fy, As);
        drv_body_accel_planar(&bp, F, Fy, Ap);
        CHECK(near_(As[0], (3.0 + 0.3) / 1.2, 1e-5), "straight accel");
        for (int i = 0; i < 4; i++) CHECK(near_(As[i], Ap[i], 1e-5), "planar == straight in the symmetric case (%d: %g vs %g)", i, As[i], Ap[i]);
        F[0] = 1.0f; F[1] = 0.0f;                        /* asymmetric: yaw acceleration appears */
        drv_body_accel_planar(&bp, F, Fy, Ap);
        CHECK(Ap[0] != Ap[1], "planar: yaw changes per-wheel acceleration");
    }
    /* S1-4.3 jitter: offsets in [-100,0); suppressed (0) without a generator */
    {
        float u = 0.0f;
        DrvTune tj = tune; tj.rand01 = test_rand; tj.rand_ud = &u;
        drv_init(&d, &car, &tj);
        CHECK(d.jitter[0] == -100.0f && d.jitter[1] == -100.0f && d.jitter[2] == -100.0f, "jitter u=0 -> -100");
        u = 0.5f; drv_init(&d, &car, &tj);
        CHECK(d.jitter[0] == -50.0f, "jitter u=.5 -> -50");
        drv_init(&d, &car, &tune);
        CHECK(d.jitter[0] == 0.0f && d.jitter[1] == 0.0f && d.jitter[2] == 0.0f, "jitter suppressed without generator");
    }
    /* S1-12 performance class storage/classification and S1-7.7 kappa-C */
    {
        DrvTune tp; drv_tune_default(&tp);
        CHECK(tp.perf_class == -1, "physics-parameter class starts unset (-1)");
        CHECK(drv_perf_class_from_score(500.0001f) == 0 && drv_perf_class_from_score(500.0f) == 1 &&
              drv_perf_class_from_score(401.0001f) == 1 && drv_perf_class_from_score(401.0f) == 2 &&
              drv_perf_class_from_score(273.0001f) == 2 && drv_perf_class_from_score(273.0f) == 3 &&
              drv_perf_class_from_score(0.0f) == 3 && drv_perf_class_from_score(-1.0f) == 3,
              "classification uses strict > at 500/401/273");
        CHECK(drv_perf_class_resolve(1, (float)NAN) == 3 && drv_perf_class_resolve(2, (float)NAN) == 3,
              "forced flag short-circuits the score");
        /* S1-7.7/S1-12.5: exact stored binary32 words, not the real decimal
         * literals; test_kappa_c() checks the same table loaded from the CSV. */
        CHECK(float_bits(drv_kappa_c(0)) == 0x00000000u &&
              float_bits(drv_kappa_c(1)) == 0xBE4CCCCDu &&
              float_bits(drv_kappa_c(2)) == 0xBECCCCCDu &&
              float_bits(drv_kappa_c(3)) == 0xBF333333u,
              "K binary32 words in C order (S1-7.7)");
        CHECK(float_bits(drv_kappa_c(-1)) == 0x3E800000u,
              "-1 selects the preceding 0.25 binary32 word (S1-12.5)");
        /* Indices the class scheme never produces are outside the spec; this
         * API rejects them instead of reading a guessed neighbour (S1-12.5). */
        CHECK(isnan(drv_kappa_c(4)) && isnan(drv_kappa_c(-2)),
              "indices outside {-1,0,1,2,3} are rejected");
        tp.perf_class = 2;
        drv_init(&d, &car, &tp);
        CHECK(d.perf_class == 2, "creation-time copy of the physics-parameter class");
        CHECK(sizeof d.perf_class == 1, "class copy is a signed byte");
        d.perf_class = -1;
        CHECK(d.perf_class == -1, "class byte is signed");
        /* S1-7.7 first-gear term reads the block copy taken at creation. Attempt 0
         * is a direct isolated fixture with the unset class copy (the S1-12.5
         * coefficient 0.25 applies there); it makes no claim about whether a
         * simulated drivetrain reaches the clause in that state (U-17). */
        for (int attempt = 0; attempt < 4; attempt++) {
            DrvTune tc; drv_tune_default(&tc);
            if (attempt == 1) tc.perf_class = 1;
            if (attempt == 2) tc.perf_class = 3;
            if (attempt == 3) tc.perf_class = 1;
            drv_init(&d, &car, &tc);
            if (attempt == 3) d.tune.perf_class = 3;   /* after creation: no live recompute (U-11 residual) */
            d.gear = d.target_gear = 2; d.mode = DRV_CLUTCH_OPEN;
            d.feedback = -10.0f; d.throttle = d.throttle_mult = 1.0f;
            for (int i = 0; i < 9; i++) d.car.torque[i] = 0.0f;
            DrvEnv env12; drv_env_default(&env12);
            for (int i = 0; i < 4; i++) env12.wheel[i].contact = 0;
            drv_couple(&d, &env12, NULL, NULL);
            const double expect_fb[4] = { -12.5, -8.0, -3.0, -8.0 };
            CHECK(near_(d.feedback, expect_fb[attempt], 1e-5),
                  "first-gear kappaC attempt %d: feedback %.5f expected %.5f", attempt, d.feedback, expect_fb[attempt]);
        }
    }
    /* Smoke test (NOT a vector): 1-D full-throttle launch with assumed loads. */
    {
        DrvTune t2 = tune; t2.is_player = 0;
        drv_init(&d, &car, &t2);
        DrvEnv env; drv_env_default(&env);
        double weight = d.car.mass * 9.8;
        for (int i = 0; i < 4; i++) env.wheel[i].load = (float)(weight / 4);
        DrvBodyStraight body = { d.car.mass, 0 };
        double v = 0; float frame_dt = 1.0f / 60, h; int N = drv_substep_count(frame_dt, &h);
        drv_couple(&d, &env, drv_body_accel_straight, &body);
        DrvInput in = { 1.0f, 0, 0, DRV_CMD_AUTO };
        int finite = 1, maxg = 0; double v_prev = 0; int regress = 0;
        for (int f = 0; f < 60 * 12; f++) {
            drv_frame_begin(&d, &in, frame_dt, -1);
            for (int s = 0; s < N; s++) {
                drv_integrate(&d, h);
                double F = 0; for (int i = 0; i < 4; i++) F += d.force_long[i];
                body.ex = (float)(-0.0004 * v * v);          /* arbitrary light drag, smoke only */
                v += h * (F + body.ex) / d.car.mass;
                for (int i = 0; i < 4; i++) { env.wheel[i].vx = (float)v; env.wheel[i].vy = 0; }
                env.body_speed = (float)fabs(v);
                drv_couple(&d, &env, drv_body_accel_straight, &body);
                if (!isfinite(d.omega_e) || !isfinite(v)) finite = 0;
            }
            if (d.gear > maxg) maxg = d.gear;
            if (v < v_prev - 0.5) regress++;
            v_prev = v;
        }
        printf("  smoke launch (assumed loads, non-vector): v=%.1f m/s after 12 s, gear %d (max %d), rpm %.0f, regressions %d\n",
               v, d.gear, maxg, d.omega_e * DRV_KR, regress);
        CHECK(finite, "smoke: finite");
        CHECK(v > 25.0, "smoke: car accelerates (%.1f m/s)", v);
        CHECK(maxg >= 4, "smoke: automatic shifted up through the gears (max gear %d)", maxg);
        CHECK(d.omega_e * DRV_KR <= d.car.max_rpm + 1.0, "smoke: rev limit respected");
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0]) g_dir = argv[1];
    g_params_json = slurp("s1_params.json");
    if (!g_params_json) return 1;
    printf("drivetrain_test: vectors in %s\n", g_dir);
    test_params();         printf("params       : %d checks, %d failed so far\n", g_checks, g_fail);
    test_engine_torque();  printf("engine torque: %d checks, %d failed so far\n", g_checks, g_fail);
    test_substeps();       printf("substeps     : %d checks, %d failed so far\n", g_checks, g_fail);
    test_clutch_torque();  printf("clutch torque: %d checks, %d failed so far\n", g_checks, g_fail);
    test_axle_split();     printf("axle split   : %d checks, %d failed so far\n", g_checks, g_fail);
    test_perf_class();     printf("perf class   : %d checks, %d failed so far\n", g_checks, g_fail);
    test_kappa_c();        printf("kappa C      : %d checks, %d failed so far\n", g_checks, g_fail);
    test_freerev();        printf("free rev     : %d checks, %d failed so far\n", g_checks, g_fail);
    test_question_cases(); printf("question cases: %d checks, %d failed so far\n", g_checks, g_fail);
    test_amended_boundaries(); printf("amendment boundaries: %d checks, %d failed so far\n", g_checks, g_fail);
    test_behaviour();      printf("behaviour    : %d checks, %d failed so far\n", g_checks, g_fail);
    free(g_params_json);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
