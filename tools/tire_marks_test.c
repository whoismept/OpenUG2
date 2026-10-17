/* tire_marks_test -- runs src/tire_marks.c against
 * docs/cleanroom-driving-physics/tire-marks/test-vectors/tire_marks.json.
 *
 *   tire_marks_test [VECTOR_DIR]
 *
 * Every vector section (lay_predicate, sample_geometry, strip_sequences,
 * render_alpha, view_metric, cell_keys) is read at run time, as is every section
 * of render_state.json (constants, effective_alpha, source_colour, composite; TM-15). The vectors are
 * synthetic consequences of SPEC.md (see test-vectors/README.md): integers,
 * bytes, Booleans and event lists compare exactly, floats with abs 1e-6.
 * The "behaviour" section afterwards checks further spec statements that have
 * no vector (non-vector, marked in the output). Exit status is nonzero on any
 * failure. Pure maths: no SDL/GL. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_dir = "docs/cleanroom-driving-physics/tire-marks/test-vectors";
static int g_fail;
#include "test_json.h"
#include "tire_marks.h"

static int g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    if (g_fail <= 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
#define TOL 1e-6
static int near_(double a, double b) { return fabs(a - b) <= TOL; }

static void vec3(const J *a, float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = (float)(a && i < a->n ? a->kid[i]->num : 0);
}
static double num_at(const J *a, int i) { return a && i < a->n ? a->kid[i]->num : NAN; }

/* ---- lay_predicate ---------------------------------------------------------------- */
static void test_lay(const J *root) {
    const J *list = jget(root, "lay_predicate");
    CHECK(list && list->n > 0, "lay_predicate missing");
    for (int i = 0; list && i < list->n; i++) {
        const J *c = list->kid[i], *in = jget(c, "input");
        TmWheelIn w; memset(&w, 0, sizeof w);
        w.contact = jbool(in, "contact"); w.masked = jbool(in, "masked");
        w.U[0] = (float)jnum(in, "Ux", 0); w.U[1] = (float)jnum(in, "Uy", 0);
        w.sliding = jbool(in, "sliding"); w.u = (float)jnum(in, "u", 0); w.N = (float)jnum(in, "N", 0);
        int got = tm_lays(&w), want = jbool(c, "lay");
        CHECK(got == want, "lay '%s': got %d want %d", jstrv(c, "name"), got, want);
    }
}

/* ---- vehicle_gate (TM-2) ------------------------------------------------------------- */
static void test_vehicle_gate(const J *root) {
    const J *list = jget(root, "vehicle_gate");
    CHECK(list && list->n > 0, "vehicle_gate missing");
    for (int i = 0; list && i < list->n; i++) {
        const J *c = list->kid[i], *in = jget(c, "input");
        int clause1 = jbool(in, "clause1"), clause2 = jbool(in, "clause2");
        int venue = (int)jnum(in, "venue", 0), clause3 = jbool(in, "clause3");
        int want = jbool(c, "lays");
        CHECK(tm_venue_lays_marks(venue) == clause3, "vehicle_gate '%s': clause3 %d want %d",
              jstrv(c, "name"), tm_venue_lays_marks(venue), clause3);
        CHECK(tm_vehicle_gate(clause1, clause2, venue) == want, "vehicle_gate '%s': got %d want %d",
              jstrv(c, "name"), tm_vehicle_gate(clause1, clause2, venue), want);
    }
}

/* ---- sample_geometry -------------------------------------------------------------- */
static int ground_flat(void *u, int wheel, float x, float y, float z, float *h) {
    (void)u; (void)wheel; (void)x; (void)y; (void)z; *h = 0; return 1;
}
static int ground_slope(void *u, int wheel, float x, float y, float z, float *h) {
    (void)u; (void)wheel; (void)x; (void)z; *h = 0.1f * y; return 1;
}
static void test_geometry(const J *root) {
    const J *list = jget(root, "sample_geometry");
    CHECK(list && list->n > 0, "sample_geometry missing");
    for (int i = 0; list && i < list->n; i++) {
        const J *c = list->kid[i], *in = jget(c, "input"), *ex = jget(c, "expected");
        const char *name = jstrv(c, "name");
        TmWheelIn w; memset(&w, 0, sizeof w);
        vec3(jget(in, "C"), w.C); vec3(jget(in, "U"), w.U); vec3(jget(in, "lateral"), w.l);
        w.width = (float)jnum(in, "tyre_width", 0); w.sliding = jbool(in, "sliding");
        w.surface = (int)jnum(in, "surface", 0); w.contact = 1;
        CHECK(near_(tm_surface_width_factor(w.surface), jnum(in, "surface_width_factor", 1)),
              "'%s': width factor %f", name, tm_surface_width_factor(w.surface));
        const char *gs = jstrv(in, "ground");
        TmGround g = { gs && strstr(gs, "0.1") ? ground_slope : ground_flat, NULL, 0 };
        TmSample s;
        int ok = tm_sample(&w, 0, &g, &s);
        CHECK(ok, "'%s': no sample", name);
        if (!ok) continue;
        CHECK(near_(s.q, jnum(ex, "align", NAN)), "'%s': align %f", name, s.q);
        CHECK(near_(s.w, jnum(ex, "half_width", NAN)), "'%s': half_width %f", name, s.w);
        const J *c3 = jget(ex, "centre"), *a3 = jget(ex, "across"), *aq = jget(ex, "across_q");
        const J *ad = jget(ex, "across_decoded");
        for (int k = 0; k < 3; k++) {
            CHECK(near_(s.P[k], num_at(c3, k)), "'%s': centre[%d] %f", name, k, s.P[k]);
            CHECK(near_(s.A[k], num_at(a3, k)), "'%s': across[%d] %f", name, k, s.A[k]);
            CHECK(s.aq[k] == (int)num_at(aq, k), "'%s': across_q[%d] %d want %d", name, k, s.aq[k], (int)num_at(aq, k));
            CHECK(near_(s.aq[k] / 64.0, num_at(ad, k)), "'%s': across_decoded[%d]", name, k);
        }
        CHECK(near_(s.intensity, jnum(ex, "intensity", NAN)), "'%s': intensity %f", name, s.intensity);
        CHECK(s.ib == (int)jnum(ex, "intensity_byte", -1), "'%s': intensity_byte %d", name, s.ib);
    }
}

/* ---- strip_sequences -------------------------------------------------------------- */
static void test_sequences(const J *root) {
    const J *list = jget(root, "strip_sequences");
    CHECK(list && list->n > 0, "strip_sequences missing");
    for (int i = 0; list && i < list->n; i++) {
        const J *c = list->kid[i], *samples = jget(c, "samples"), *events = jget(c, "events"),
                *strips = jget(c, "strips");
        const char *name = jstrv(c, "name");
        int far_case = strstr(name, "farther") != NULL;
        static TmStore store; TmTrail trail = { NULL };
        tm_init(&store, 128);
        TmEvent log[512]; int nlog = 0;
        for (int k = 0; k < samples->n; k++) {
            const J *sj = samples->kid[k];
            TmSample s; memset(&s, 0, sizeof s);
            vec3(jget(sj, "P"), s.P); vec3(jget(sj, "across"), s.A);
            s.surface = (int)jnum(sj, "surface", 1); s.intensity = (float)jnum(sj, "intensity", 1);
            for (int j = 0; j < 3; j++) s.aq[j] = (signed char)(int)(64.0f * s.A[j]);
            s.ib = (unsigned char)(int)(255.0f * s.intensity);
            float O[3] = { s.P[0] + (far_case ? 10.0f : 1.0f), s.P[1], s.P[2] };   /* 1 m inside the gate */
            nlog += tm_trail_add(&store, &trail, O, &s, 0, 0, log + nlog);
        }
        CHECK(nlog == events->n, "'%s': %d events, want %d", name, nlog, events->n);
        for (int k = 0; k < nlog && k < events->n; k++)
            CHECK(!strcmp(tm_event_name(log[k]), events->kid[k]->s), "'%s': event %d '%s' want '%s'",
                  name, k, tm_event_name(log[k]), events->kid[k]->s);
        /* strips in creation order */
        int order[TM_MAX_STRIPS], n = 0;
        for (int k = 0; k < TM_MAX_STRIPS; k++) if (store.strip[k].used) order[n++] = k;
        for (int a = 1; a < n; a++) { int t = order[a], b = a - 1;
            while (b >= 0 && store.strip[order[b]].recent > store.strip[t].recent) { order[b + 1] = order[b]; b--; }
            order[b + 1] = t; }
        int want = strips ? strips->n : 0;
        CHECK(n == want, "'%s': %d strips, want %d", name, n, want);
        for (int k = 0; k < n && k < want; k++) {
            const TmStrip *st = &store.strip[order[k]]; const J *sj = strips->kid[k], *pts = jget(sj, "points");
            CHECK(st->surface == (int)jnum(sj, "surface", -1), "'%s' strip %d surface", name, k);
            CHECK((trail.strip == st) == jbool(sj, "attached"), "'%s' strip %d attached", name, k);
            CHECK(st->n == pts->n, "'%s' strip %d: %d points, want %d", name, k, st->n, pts->n);
            for (int p = 0; p < st->n && p < pts->n; p++) {
                const J *pj = pts->kid[p], *P = jget(pj, "P"), *q = jget(pj, "across_q");
                for (int j = 0; j < 3; j++) {
                    CHECK(near_(st->pt[p].P[j], num_at(P, j)), "'%s' strip %d point %d P[%d] %f want %f",
                          name, k, p, j, st->pt[p].P[j], num_at(P, j));
                    CHECK(st->pt[p].aq[j] == (int)num_at(q, j), "'%s' strip %d point %d across_q[%d]", name, k, p, j);
                }
                CHECK(st->pt[p].ib == (int)jnum(pj, "intensity_byte", -1), "'%s' strip %d point %d intensity_byte", name, k, p);
            }
        }
    }
}

/* ---- render_alpha / view_metric / cell_keys --------------------------------------- */
static void test_render(const J *root) {
    const J *list = jget(root, "render_alpha");
    CHECK(list && list->n > 0, "render_alpha missing");
    for (int i = 0; list && i < list->n; i++) {
        const J *c = list->kid[i];
        int got = tm_vertex_alpha((int)jnum(c, "intensity_byte", 0), (int)jnum(c, "fade", 0));
        CHECK(got == (int)jnum(c, "vertex_alpha", -1), "render_alpha[%d]: %d", i, got);
    }
    const J *vm = jget(root, "view_metric");
    CHECK(vm != NULL, "view_metric missing");
    double K = tm_view_k(71.5 * 3.14159265358979323846 / 180.0);
    CHECK(near_(K, jnum(vm, "K_for_full_fov_71_5_deg", NAN)), "K %.7f", K);
    const J *samples = jget(vm, "samples");
    for (int i = 0; samples && i < samples->n; i++) {
        const J *c = samples->kid[i];
        int fade, p = tm_view_metric(jnum(vm, "K_for_full_fov_71_5_deg", 0), jnum(c, "distance", 0), &fade);
        const J *f = jget(c, "fade");
        int want = f && f->t == J_NUM ? (int)f->num : -1;
        CHECK(p == (int)jnum(c, "p", -1), "view_metric[%d]: p %d", i, p);
        CHECK(fade == want, "view_metric[%d]: fade %d want %d", i, fade, want);
    }
    const J *keys = jget(root, "cell_keys");
    CHECK(keys && keys->n > 0, "cell_keys missing");
    for (int i = 0; keys && i < keys->n; i++) {
        const J *c = keys->kid[i], *cell = jget(c, "cell");
        int cx, cy; tm_cell_key((float)jnum(c, "x", 0), (float)jnum(c, "y", 0), &cx, &cy);
        CHECK(cx == (int)num_at(cell, 0) && cy == (int)num_at(cell, 1), "cell_keys[%d]: (%d,%d)", i, cx, cy);
    }
}

/* ---- behaviour (non-vector) ------------------------------------------------------- */
static TmSample mk(float x, float y, int surface) {
    TmSample s; memset(&s, 0, sizeof s);
    s.P[0] = x; s.P[1] = y; s.surface = surface; s.intensity = 1; s.ib = 255; s.aq[1] = 6;
    return s;
}
static void test_behaviour(void) {
    static TmStore st;
    /* TM-1.2 surface table */
    CHECK(tm_surface_texture(1) == TM_TEX_ROAD && tm_surface_texture(7) == TM_TEX_ROAD &&
          tm_surface_texture(8) == TM_TEX_DIRT && tm_surface_texture(9) == TM_TEX_DIRT &&
          tm_surface_texture(10) == TM_TEX_ROAD && tm_surface_texture(13) == TM_TEX_ROAD &&
          tm_surface_texture(14) == TM_TEX_NONE && tm_surface_texture(15) == TM_TEX_GRASS &&
          tm_surface_texture(19) == TM_TEX_ROAD && tm_surface_texture(20) == TM_TEX_NONE &&
          tm_surface_texture(28) == TM_TEX_NONE && tm_surface_texture(0) == TM_TEX_NONE,
          "surface textures");
    CHECK(tm_surface_width_factor(14) == 1.0f && tm_surface_width_factor(15) == 1.4f &&
          tm_surface_width_factor(8) == 1.4f && tm_surface_width_factor(10) == 1.0f, "width factors");
    CHECK(tm_texture_hash(TM_TEX_ROAD) == 0x9076F58Fu && tm_texture_hash(TM_TEX_DIRT) == 0x906F30FCu &&
          tm_texture_hash(TM_TEX_GRASS) == 0x9E904149u, "texture hashes");
    /* TM-2 clause 3: the reserved Organic-Drag venue block [4400, 4499] suppresses marks */
    CHECK(tm_venue_lays_marks(4000) && tm_venue_lays_marks(4001) && tm_venue_lays_marks(4399) &&
          tm_venue_lays_marks(4500) && !tm_venue_lays_marks(4400) && !tm_venue_lays_marks(4401) &&
          !tm_venue_lays_marks(4499), "venue-id gate [4400,4499]");
    /* TM-3 utilisation */
    CHECK(tm_utilisation(5, 10, 0, 1) == .5f && tm_utilisation(20, 10, 0, 1) == 1.0f &&
          tm_utilisation(1, .001, 0, 1) == 1.0f && tm_utilisation(1, 10, 1, 1) == 1.0f &&
          tm_utilisation(20, 10, 1, 0) == 0.0f, "utilisation");
    /* TM-7 step 1: origin distance exactly 4 passes, beyond rejects */
    { TmTrail t = { NULL }; TmSample s = mk(0, 0, 1); float O[3] = { 4, 0, 0 }; TmEvent e[6];
      tm_init(&st, 128);
      CHECK(tm_trail_add(&st, &t, O, &s, 0, 0, e) == 2 && e[0] == TM_EV_NEW, "distance 4 accepted");
      O[0] = 4.001f; TmTrail t2 = { NULL };
      CHECK(tm_trail_add(&st, &t2, O, &s, 0, 0, e) == 1 && e[0] == TM_EV_FAR && !t2.strip, "distance>4 rejected"); }
    /* TM-9 / TM-13: a wheel that stops laying ends its trail; the strip stays */
    { TmVehicle v; memset(&v, 0, sizeof v); tm_init(&st, 128);
      TmWheelIn w[4]; memset(w, 0, sizeof w);
      for (int i = 0; i < 4; i++) { w[i].contact = 1; w[i].sliding = 1; w[i].U[0] = 10; w[i].l[1] = 1;
          w[i].width = .2f; w[i].surface = 1; w[i].C[0] = (float)i; w[i].C[2] = .3f; }
      TmGround g = { ground_flat, NULL, 0 }; float O[3] = { 0, 0, 0 };
      tm_vehicle_update(&st, &v, w, O, 0, &g);
      CHECK(v.trail[0].strip && v.trail[1].strip && v.trail[2].strip && v.trail[3].strip, "four trails open");
      w[2].contact = 0; w[3].masked = 1;
      tm_vehicle_update(&st, &v, w, O, 0, &g);
      CHECK(v.trail[0].strip && v.trail[1].strip && !v.trail[2].strip && !v.trail[3].strip, "airborne/masked end trail");
      int used = 0; for (int i = 0; i < TM_MAX_STRIPS; i++) used += st.strip[i].used;
      CHECK(used == 4, "ended strips remain in the world (%d)", used);
      tm_vehicle_destroy(&v);
      CHECK(!v.trail[0].strip && !v.trail[1].strip, "destroy ends trails"); }
    /* TM-10.1: capacity evicts the least-recent strip and closes its trail */
    { static TmTrail t[130]; tm_init(&st, 128); memset(t, 0, sizeof t);
      float O[3] = { 0, 0, 0 };
      for (int i = 0; i < 129; i++) { TmSample s = mk(0.01f * (float)i, 0, 1); tm_trail_add(&st, &t[i], O, &s, 0, 0, NULL); }
      int used = 0; for (int i = 0; i < TM_MAX_STRIPS; i++) used += st.strip[i].used;
      CHECK(used == 128, "store holds 128 strips (%d)", used);
      CHECK(!t[0].strip && t[1].strip && t[128].strip, "least-recent strip evicted, its trail closed");
      tm_init(&st, 64); memset(t, 0, sizeof t);
      for (int i = 0; i < 70; i++) { TmSample s = mk(0.01f * (float)i, 0, 1); tm_trail_add(&st, &t[i], O, &s, 0, 0, NULL); }
      used = 0; for (int i = 0; i < TM_MAX_STRIPS; i++) used += st.strip[i].used;
      CHECK(used == 64, "two-player capacity 64 (%d)", used); }
    /* TM-10.2: 41st cell destroys the least-recently-touched cell with its strips */
    { static TmTrail t[45]; tm_init(&st, 128); memset(t, 0, sizeof t);
      for (int i = 0; i < 41; i++) {
          tm_set_frame(&st, i);
          float O[3] = { 64.0f * (float)i + 1, 0, 0 };
          TmSample s = mk(64.0f * (float)i + 1, 0, 1);
          tm_trail_add(&st, &t[i], O, &s, 0, 0, NULL);
      }
      int cells = 0; for (int i = 0; i < TM_MAX_CELLS; i++) cells += st.cell[i].used;
      CHECK(cells == 40, "40 cells (%d)", cells);
      CHECK(!t[0].strip && t[1].strip && t[40].strip, "oldest cell destroyed with its strip"); }
    /* TM-11: geometry, lift, alpha, grouping (a view close enough that the cell test passes) */
    { TmVehicle v; memset(&v, 0, sizeof v); tm_init(&st, 128); tm_set_frame(&st, 0);
      TmTrail *t = &v.trail[0]; float O[3] = { 0, 0, 0 };
      TmSample a = mk(0, 0, 1), b = mk(1, 0, 1); b.ib = 85;
      tm_trail_add(&st, t, O, &a, 0, 0, NULL); tm_trail_add(&st, t, O, &b, 0, 0, NULL);
      static TmVertex out[TM_MAX_VERTICES]; int first[TM_TEX_COUNT], count[TM_TEX_COUNT];
      TmView view; memset(&view, 0, sizeof view);
      view.cam[0] = -3; view.cam[2] = 1; view.fwd[0] = 1; view.K = tm_view_k(1.2);
      view.texture_mask = 1u << TM_TEX_ROAD;
      int total = tm_view_build(&st, &view, out, first, count);
      CHECK(total == 4 && count[TM_TEX_ROAD] == 4 && first[TM_TEX_ROAD] == 0, "one quad (%d)", total);
      if (total == 4) {
          CHECK(near_(out[0].x, 0) && near_(out[0].y, 6 / 64.0) && near_(out[0].z, .05), "v0 position");
          CHECK(near_(out[1].x, 1) && near_(out[1].y, 6 / 64.0), "v1 position");
          CHECK(near_(out[2].x, 1) && near_(out[2].y, -6 / 64.0), "v2 position");
          CHECK(near_(out[3].x, 0) && near_(out[3].y, -6 / 64.0), "v3 position");
          CHECK(out[0].u == 0 && out[0].v == 0 && out[1].u == 1 && out[2].v == 1 && out[3].u == 0 && out[3].v == 1, "uv");
          CHECK(out[0].rgba[0] == 128 && out[0].rgba[3] == 127 && out[3].rgba[3] == 127 &&
                out[1].rgba[3] == 42 && out[2].rgba[3] == 42, "vertex alpha 127/42 (TM-15.2: not doubled)");
          CHECK(out[0].rgba[0] == 128 && out[1].rgba[1] == 128 && out[2].rgba[2] == 128, "vertex rgb 128");
      }
      view.texture_mask = 0;
      CHECK(tm_view_build(&st, &view, out, first, count) == 0, "missing texture draws nothing");
      view.texture_mask = 1u << TM_TEX_ROAD; view.fwd[0] = -1;
      CHECK(tm_view_build(&st, &view, out, first, count) == 0, "behind camera culled"); }
}

/* TM-15.5 pixel base: greedy 0x11 skip must not eat a 0x11-led first palette entry */
static void test_payload_base(void) {
    unsigned char buf[512]; memset(buf, 0, sizeof buf);
    /* payload starts at 0x108 (after an 8-byte header), padding 0x11 up to 0x180, then palette (11 11 11 7f) */
    for (int i = 0x108; i < 0x180; i++) buf[i] = 0x11;
    buf[0x180] = 0x11; buf[0x181] = 0x11; buf[0x182] = 0x11; buf[0x183] = 0x7f; buf[0x184] = 0x18;
    CHECK(tm_payload_base(buf, 0x183) == 0x180, "payload base stops at the 0x80 boundary (%ld)", tm_payload_base(buf, 0x183));
    /* palette not 0x11-led: greedy skip already ends on the boundary */
    buf[0x180] = 0x20; buf[0x181] = 0x20; buf[0x182] = 0x20;
    CHECK(tm_payload_base(buf, 0x180) == 0x180, "boundary unchanged");
    /* filler shorter than the alignment run does not move */
    CHECK(tm_payload_base(buf, 0x140) == 0x140, "inside the run unchanged");
}

/* ---- render_state.json (TM-15) ------------------------------------------------------ */
static const char *sv(const J *o, const char *k) { const char *v = jstrv(o, k); return v ? v : ""; }
static void test_render_state(void) {
    J *root = jload("render_state.json");
    CHECK(root != NULL, "render_state.json missing");
    if (!root) return;
    const TmRenderState *rs = tm_render_state();
    const J *k = jget(root, "constants");
    CHECK(k != NULL, "constants missing");
    CHECK(rs->vertex_rgb_byte == (int)jnum(k, "vertex_rgb_byte", -1) && TM_VERTEX_RGB == 128, "vertex_rgb_byte");
    CHECK(rs->colour_scale == (int)jnum(k, "colour_scale", -1), "colour_scale");
    CHECK(rs->alpha_scale == (int)jnum(k, "alpha_scale", -1) && rs->alpha_scale == 1, "alpha_scale is 1 (TM-15.2)");
    CHECK(!strcmp(sv(k, "blend"), "src*a + dst*(1-a)") && rs->blend_src_alpha, "blend");
    CHECK(jbool(k, "depth_write") == rs->depth_write && !rs->depth_write, "depth write off");
    CHECK(!strcmp(sv(k, "depth_func"), "less_or_equal") && rs->depth_lequal, "depth func");
    CHECK(!strcmp(sv(k, "cull"), "none") && !rs->cull, "no culling");
    CHECK(jbool(k, "alpha_test") == rs->alpha_test && !rs->alpha_test, "no alpha test");
    CHECK((int)jnum(k, "depth_bias", -1) == rs->depth_bias && !rs->depth_bias, "no depth bias");
    CHECK(near_(jnum(k, "z_lift_m", NAN), rs->z_lift_m), "z lift 0.05 m");
    CHECK(!strcmp(sv(k, "address_u"), "wrap") && rs->wrap_u && !strcmp(sv(k, "address_v"), "wrap") && rs->wrap_v,
          "wrap addressing");
    CHECK(!strcmp(sv(k, "mip_filter"), "linear") && rs->mip_linear, "mip filter");
    CHECK((int)jnum(k, "mip_levels", -1) == rs->mip_levels, "mip levels");

    const J *ea = jget(root, "effective_alpha");
    CHECK(ea && ea->n == 9, "effective_alpha rows");
    for (int i = 0; ea && i < ea->n; i++) {
        const J *c = ea->kid[i];
        int vA = tm_vertex_alpha((int)jnum(c, "Ib", 0), (int)jnum(c, "F", 0));
        CHECK(vA == (int)jnum(c, "vertex_alpha", -1), "effective_alpha[%d] vertex alpha %d", i, vA);
        float a = tm_mark_opacity((float)(jnum(c, "texel_alpha", 0) / 255.0), vA);
        CHECK(fabs(a - jnum(c, "effective_alpha", NAN)) <= TOL, "effective_alpha[%d]: %.7f want %.7f", i, a,
              jnum(c, "effective_alpha", NAN));
    }
    /* the TM-15.2 mapping itself: 127 -> 127/255 (not 1), 76 -> 38, 85 -> 42 */
    CHECK(near_(tm_mark_opacity(1.0f, 127), 127.0 / 255.0), "vertex alpha 127 maps to 0.49804");
    CHECK(tm_vertex_alpha(76, 0) == 38 && tm_vertex_alpha(85, 0) == 42 && tm_vertex_alpha(255, 0) == 127, "alpha bytes");

    const J *sc = jget(root, "source_colour");
    CHECK(sc && sc->n == 5, "source_colour rows");
    for (int i = 0; sc && i < sc->n; i++) {
        const J *c = sc->kid[i];
        float t[3], got[3]; const unsigned char vc[3] = { TM_VERTEX_RGB, TM_VERTEX_RGB, TM_VERTEX_RGB };
        for (int j = 0; j < 3; j++) t[j] = (float)(num_at(jget(c, "texel_rgb"), j) / 255.0);
        tm_mark_source(t, vc, got);
        for (int j = 0; j < 3; j++)
            CHECK(fabs(got[j] - num_at(jget(c, "src_rgb"), j)) <= TOL, "source_colour[%d][%d]: %.7f want %.7f", i, j,
                  got[j], num_at(jget(c, "src_rgb"), j));
    }
    { /* 128 is almost exactly colour unity: 2*128/255 = 256/255 (TM-15.2); 1.0 clamps */
      float t[3] = { 0.5f, 0.25f, 1.0f }, got[3]; const unsigned char vc[3] = { 128, 128, 128 };
      tm_mark_source(t, vc, got);
      CHECK(fabs(got[0] - 0.5 * 256.0 / 255.0) < 1e-6 && fabs(got[1] - 0.25 * 256.0 / 255.0) < 1e-6 && got[2] == 1.0f,
            "colour x 256/255, clamped"); }

    const J *cp = jget(root, "composite");
    CHECK(cp && cp->n == 5, "composite rows");
    for (int i = 0; cp && i < cp->n; i++) {
        const J *c = cp->kid[i];
        const unsigned char vc[3] = { TM_VERTEX_RGB, TM_VERTEX_RGB, TM_VERTEX_RGB };
        float t[3], src[3], dst[3], out[3];
        for (int j = 0; j < 3; j++) { t[j] = (float)(num_at(jget(c, "texel_rgb"), j) / 255.0); dst[j] = (float)num_at(jget(c, "dst_rgb"), j); }
        tm_mark_source(t, vc, src);
        float a = tm_mark_opacity((float)(jnum(c, "texel_alpha", 0) / 255.0), tm_vertex_alpha((int)jnum(c, "Ib", 0), 0));
        CHECK(fabs(a - jnum(c, "effective_alpha_per_layer", NAN)) <= TOL, "composite[%d] alpha %.7f", i, a);
        for (int l = 0; l < (int)jnum(c, "layers", 1); l++) { tm_mark_blend(dst, src, a, out); memcpy(dst, out, sizeof dst); }
        for (int j = 0; j < 3; j++)
            CHECK(fabs(dst[j] - num_at(jget(c, "result_rgb"), j)) <= TOL, "composite[%d][%d]: %.7f want %.7f '%s'", i, j,
                  dst[j], num_at(jget(c, "result_rgb"), j), sv(c, "name"));
    }
    /* the vertices a built full-intensity strip emits feed that law unchanged (TM-15.2) */
    { static TmStore st; TmVehicle v; memset(&v, 0, sizeof v); tm_init(&st, 128); tm_set_frame(&st, 0);
      float O[3] = { 0, 0, 0 };
      TmSample a = mk(0, 0, 1), b = mk(1, 0, 1);
      tm_trail_add(&st, &v.trail[0], O, &a, 0, 0, NULL); tm_trail_add(&st, &v.trail[0], O, &b, 0, 0, NULL);
      static TmVertex out[TM_MAX_VERTICES]; int first[TM_TEX_COUNT], count[TM_TEX_COUNT];
      TmView view; memset(&view, 0, sizeof view);
      view.cam[0] = -3; view.cam[2] = 1; view.fwd[0] = 1; view.K = tm_view_k(1.2); view.texture_mask = 1u << TM_TEX_ROAD;
      int n = tm_view_build(&st, &view, out, first, count);
      CHECK(n == 4, "full-intensity quad built (%d)", n);
      for (int j = 0; j < n; j++) {
          const unsigned char vc[3] = { out[j].rgba[0], out[j].rgba[1], out[j].rgba[2] };
          float t[3] = { 8.31f / 255, 8.31f / 255, 8.31f / 255 }, src[3];
          tm_mark_source(t, vc, src);
          CHECK(near_(src[0], 0.032716) && near_(tm_mark_opacity(127.0f / 255.0f, out[j].rgba[3]), 0.248043),
                "built vertex %d gives 0.0327 / 0.2480", j);
      } }
    jfree(root);
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0]) g_dir = argv[1];
    J *root = jload("tire_marks.json");
    if (!root) return 1;
    test_lay(root); test_vehicle_gate(root); test_geometry(root); test_sequences(root); test_render(root);
    test_behaviour();
    test_payload_base();
    test_render_state();
    printf("tire_marks_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
