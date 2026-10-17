/* Tire marks: see tire_marks.h and docs/cleanroom-driving-physics/tire-marks/. */
#include "tire_marks.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "nfsu2.h"

/* b32(x): nearest binary32. Spec thresholds are binary32 values; comparisons
 * below are exact and strict/non-strict as written (never tolerance tests). */
static float b32(double x) { return (float)x; }

/* ---- TM-1 ------------------------------------------------------------------------ */
float tm_surface_width_factor(int s) {
    return s == 8 || s == 9 || s == 15 ? b32(1.4) : 1.0f;
}
int tm_surface_texture(int s) {
    if (s >= 1 && s <= 7) return TM_TEX_ROAD;
    if (s == 8 || s == 9) return TM_TEX_DIRT;
    if (s >= 10 && s <= 13) return TM_TEX_ROAD;      /* 13 puddle: intensity forced to 0 */
    if (s == 15) return TM_TEX_GRASS;
    if (s >= 16 && s <= 19) return TM_TEX_ROAD;
    return TM_TEX_NONE;                              /* 0, 14, 20...28 and out of range */
}
unsigned tm_texture_hash(int t) {
    return t == TM_TEX_ROAD ? TM_HASH_ROAD : t == TM_TEX_DIRT ? TM_HASH_DIRT
         : t == TM_TEX_GRASS ? TM_HASH_GRASS : 0;
}
int tm_car_read(const unsigned char *data, long length, const char *name, float width[4]) {
    if (!data || !name || !width) return 0;
    long row = n2_global_car_record(data, length, name);
    if (row < 0 || row > length - 0x890) return 0;
    float w[4];
    for (int i = 0; i < 4; i++) {
        memcpy(&w[i], data + row + 0x134 + 0x30 * i, sizeof w[i]);
        if (!isfinite(w[i]) || w[i] <= 0) return 0;
    }
    memcpy(width, w, sizeof w);
    return 1;
}

/* ---- TM-2 clause 3 / TM-3 --------------------------------------------------------- */
int tm_venue_lays_marks(int venue_id) {
    return !(venue_id >= TM_VENUE_RESERVED_LO && venue_id <= TM_VENUE_RESERVED_HI);
}
int tm_vehicle_gate(int clause1, int clause2, int venue_id) {
    return clause1 && clause2 && tm_venue_lays_marks(venue_id);
}
int tm_lays(const TmWheelIn *w) {
    if (!w->contact || w->masked) return 0;
    float h = sqrtf(w->U[0] * w->U[0] + w->U[1] * w->U[1]);
    if (!(h > 1.0f)) return 0;                       /* strict; NaN false */
    if (w->sliding) return 1;
    return w->u * w->N > 8.0f;                       /* strict; product compared */
}

/* ---- TM-4 / TM-5 / TM-6 ----------------------------------------------------------- */
static signed char quant_across(float v) {
    float t = 64.0f * v;
    if (!(t > -128.0f)) return -128;                 /* TM-U10: saturate */
    if (t > 127.0f) return 127;
    return (signed char)(int)t;                      /* truncation toward zero */
}
static unsigned char quant_intensity(float i) {
    float t = 255.0f * i;
    if (!(t > 0.0f)) return 0;
    if (t >= 255.0f) return 255;                     /* TM-U10: saturate */
    return (unsigned char)(int)t;
}
int tm_sample(const TmWheelIn *in, int index, const TmGround *g, TmSample *o) {
    /* 1. travel-normal unit vector, left of horizontal travel */
    float hs = sqrtf(in->U[0] * in->U[0] + in->U[1] * in->U[1]);
    if (!(hs > 0)) return 0;
    float a[3] = { -in->U[1] / hs, in->U[0] / hs, 0 };
    /* 2. alignment */
    float q = fabsf(in->l[0] * a[0] + in->l[1] * a[1] + in->l[2] * a[2]);
    /* 3. half-width */
    float ks = tm_surface_width_factor(in->surface);
    float ws = in->width * ks;
    float qw = q >= b32(.5) ? q : b32(.5);
    float w = ws * qw * b32(.5);
    /* 4. probes */
    float A0[3] = { w * a[0], w * a[1], 0 };
    float Lp[3] = { in->C[0] + A0[0], in->C[1] + A0[1], in->C[2] };
    float Rp[3] = { in->C[0] - A0[0], in->C[1] - A0[1], in->C[2] };
    /* 5. ground heights; TM-U7: one missing probe reuses the other, none = no mark */
    float hl = 0, hr = 0;
    int fl = g->query(g->user, index, Lp[0], Lp[1], Lp[2], &hl);
    int fr = g->query(g->user, index, Rp[0], Rp[1], Rp[2], &hr);
    if (!fl && !fr) return 0;
    if (!fl) hl = hr;
    if (!fr) hr = hl;
    float zL = b32((double)hl + g->offset);
    double zR = (double)hr + g->offset;
    /* 6. sample point; 7. half-width vector following the cross-slope */
    o->P[0] = in->C[0]; o->P[1] = in->C[1];
    o->P[2] = b32(((double)zL + zR) / 2);
    double v[3] = { A0[0], A0[1], ((double)zL - zR) / 2 };
    double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0) for (int k = 0; k < 3; k++) o->A[k] = (float)(w * (v[k] / n));
    else { o->A[0] = w; o->A[1] = o->A[2] = 0; }
    o->w = w; o->q = q; o->surface = in->surface;
    /* TM-5 intensity */
    float I = in->sliding ? q : b32(q * b32(1.0 / 3.0));
    if (in->surface == 13) I = 0;
    else if (I < b32(.3)) I = b32(.3);
    o->intensity = I;
    for (int k = 0; k < 3; k++) o->aq[k] = quant_across(o->A[k]);
    o->ib = quant_intensity(I);
    return 1;
}

/* ---- store helpers ---------------------------------------------------------------- */
const char *tm_event_name(TmEvent e) {
    static const char *const name[] = { "far", "new", "new-seeded", "new-continued",
                                        "commit", "replace", "end", "full" };
    return name[e];
}
void tm_init(TmStore *s, int capacity) {
    memset(s, 0, sizeof *s);
    s->capacity = capacity < 1 ? 1 : capacity > TM_MAX_STRIPS ? TM_MAX_STRIPS : capacity;
}
void tm_set_frame(TmStore *s, long frame) { s->frame = frame; }

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t d = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? d - 1 : d;
}
static int cell_axis(float v) {
    if (!isfinite(v)) return 0;
    double t = (double)(65536.0f * v);
    if (t > 9.0e18) t = 9.0e18;
    if (t < -9.0e18) t = -9.0e18;
    int64_t q = floor_div((int64_t)t, (int64_t)1 << 22);   /* trunc, then arithmetic floor */
    return (int)q;
}
void tm_cell_key(float x, float y, int *cx, int *cy) { *cx = cell_axis(x); *cy = cell_axis(y); }

static void strip_remove(TmStore *s, TmStrip *st) {
    if (st->trail) st->trail->strip = NULL;          /* closes any trail holding it open */
    memset(st, 0, sizeof *st);
    (void)s;
}
static void cell_destroy(TmStore *s, int c) {
    for (int i = 0; i < TM_MAX_STRIPS; i++)
        if (s->strip[i].used && s->strip[i].cell == c) strip_remove(s, &s->strip[i]);
    memset(&s->cell[c], 0, sizeof s->cell[c]);
}
static int cell_for(TmStore *s, const float P[3]) {
    int cx, cy; tm_cell_key(P[0], P[1], &cx, &cy);
    for (int i = 0; i < TM_MAX_CELLS; i++)
        if (s->cell[i].used && s->cell[i].cx == cx && s->cell[i].cy == cy) return i;
    int slot = -1;
    for (int i = 0; i < TM_MAX_CELLS; i++) if (!s->cell[i].used) { slot = i; break; }
    if (slot < 0) {                                   /* destroy least-recently-touched cell */
        slot = 0;
        for (int i = 1; i < TM_MAX_CELLS; i++) if (s->cell[i].touched < s->cell[slot].touched) slot = i;
        cell_destroy(s, slot);
    }
    TmCell *c = &s->cell[slot];
    c->used = 1; c->cx = cx; c->cy = cy; c->touched = s->frame;
    for (int k = 0; k < 3; k++) c->lo[k] = c->hi[k] = P[k];
    return slot;
}
static TmStrip *strip_create(TmStore *s, TmTrail *trail, int surface, int flag, int creator,
                             const float first[3]) {
    int used = 0;
    for (int i = 0; i < TM_MAX_STRIPS; i++) used += s->strip[i].used;
    if (used >= s->capacity) {                        /* evict the least-recent strip */
        int victim = -1;
        for (int i = 0; i < TM_MAX_STRIPS; i++)
            if (s->strip[i].used && (victim < 0 || s->strip[i].recent < s->strip[victim].recent)) victim = i;
        if (victim >= 0) strip_remove(s, &s->strip[victim]);
    }
    int c = cell_for(s, first);
    s->cell[c].touched = s->frame;                   /* creation touches the cell */
    TmStrip *st = NULL;
    for (int i = 0; i < TM_MAX_STRIPS; i++) if (!s->strip[i].used) { st = &s->strip[i]; break; }
    if (!st) return NULL;                            /* unreachable while capacity <= slots */
    memset(st, 0, sizeof *st);
    st->used = 1; st->open = 1; st->surface = surface; st->flag = flag; st->creator = creator;
    st->cell = c; st->recent = ++s->clock;
    st->trail = trail; trail->strip = st;
    return st;
}

typedef enum { APPEND_OK, APPEND_ENDED, APPEND_FULL } AppendResult;
static void unit_or_x(const float v[3], float o[3]) {
    double n = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
    if (n > 0) for (int k = 0; k < 3; k++) o[k] = (float)(v[k] / n);
    else { o[0] = 1; o[1] = o[2] = 0; }
}
static void grow_box(TmStore *s, TmStrip *st, const float P[3], float d) {
    if (st->n == 1 && d == 0) {                      /* box initialises to point 0 */
        for (int k = 0; k < 3; k++) st->lo[k] = st->hi[k] = P[k];
    } else {
        for (int k = 0; k < 3; k++) {
            if (P[k] - d < st->lo[k]) st->lo[k] = P[k] - d;
            if (P[k] + d > st->hi[k]) st->hi[k] = P[k] + d;
        }
    }
    TmCell *c = &s->cell[st->cell];
    for (int k = 0; k < 3; k++) {
        st->centre[k] = (st->lo[k] + st->hi[k]) * .5f;
        if (st->lo[k] < c->lo[k]) c->lo[k] = st->lo[k];
        if (st->hi[k] > c->hi[k]) c->hi[k] = st->hi[k];
    }
}
/* TM-8. *event is COMMIT/REPLACE when the result is OK. */
static AppendResult strip_append(TmStore *s, TmStrip *st, const float P[3], const signed char aq[3],
                                 unsigned char ib, TmEvent *event) {
    TmPoint np; memcpy(np.P, P, sizeof np.P); memcpy(np.aq, aq, 3); np.ib = ib;
    *event = TM_EV_COMMIT;
    if (st->n == 0) {
        st->pt[0] = np; st->n = 1; st->S = 0;
        grow_box(s, st, P, 0);
        return APPEND_OK;
    }
    const float *p = st->pt[st->n - 1].P;
    float delta[3] = { P[0] - p[0], P[1] - p[1], P[2] - p[2] };
    float d = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    float e[3]; unit_or_x(delta, e);
    if (st->n == 1) {
        st->pt[1] = np; st->n = 2; st->D[0] = e[0]; st->D[1] = e[1]; st->D[2] = e[2]; st->S = d;
        grow_box(s, st, P, d);
        return APPEND_OK;
    }
    float kappa = 1.0f - (e[0] * st->D[0] + e[1] * st->D[1] + e[2] * st->D[2]);
    float L = d + st->S;
    if (kappa > b32(.2)) {                            /* corner: close the strip */
        st->open = 0;
        if (st->trail) { st->trail->strip = NULL; st->trail = NULL; }
        return APPEND_ENDED;
    }
    if (L <= 3.0f && (L < b32(.25) || kappa < b32(.002))) {
        st->pt[st->n - 1] = np; st->S = L;            /* replace; D unchanged */
        *event = TM_EV_REPLACE;
        grow_box(s, st, P, d);
        return APPEND_OK;
    }
    if (st->n == TM_MAX_POINTS) return APPEND_FULL;
    st->pt[st->n++] = np; st->D[0] = e[0]; st->D[1] = e[1]; st->D[2] = e[2]; st->S = d;
    grow_box(s, st, P, d);
    return APPEND_OK;
}

void tm_trail_end(TmTrail *t) {                      /* TM-9 */
    if (t->strip) { t->strip->open = 0; t->strip->trail = NULL; t->strip = NULL; }
}

int tm_trail_add(TmStore *store, TmTrail *trail, const float O[3], const TmSample *sm,
                 int flag, int creator, TmEvent *ev) {
    int ne = 0;
#define EMIT(e) do { if (ev) ev[ne] = (e); ne++; } while (0)
    float dx = sm->P[0] - O[0], dy = sm->P[1] - O[1], dz = sm->P[2] - O[2];
    if (sqrtf(dx * dx + dy * dy + dz * dz) > TM_ORIGIN_RANGE) { EMIT(TM_EV_FAR); return ne; }
    TmEvent e;
    TmStrip *open = trail->strip;
    TmEvent cont;
    if (!open) {                                       /* step 2 */
        TmStrip *st = strip_create(store, trail, sm->surface, flag, creator, sm->P);
        if (!st) return ne;
        EMIT(TM_EV_NEW);
        strip_append(store, st, sm->P, sm->aq, sm->ib, &e); EMIT(e);
        return ne;
    }
    if (open->surface != sm->surface || open->flag != flag) {   /* step 3 */
        cont = TM_EV_NEW_SEEDED;
    } else {                                           /* step 4 */
        AppendResult r = strip_append(store, open, sm->P, sm->aq, sm->ib, &e);
        if (r == APPEND_OK) { EMIT(e); return ne; }
        if (r == APPEND_ENDED) { EMIT(TM_EV_END); return ne; }
        EMIT(TM_EV_FULL);
        cont = TM_EV_NEW_CONTINUED;
    }
    /* step 5: continue from the old strip's last stored point (stored bytes are
     * carried over unchanged, so decode/encode cannot drift) */
    TmPoint seed = open->pt[open->n - 1];
    open->open = 0; open->trail = NULL; trail->strip = NULL;
    TmStrip *st = strip_create(store, trail, sm->surface, flag, creator, seed.P);
    if (!st) return ne;
    EMIT(cont);
    strip_append(store, st, seed.P, seed.aq, seed.ib, &e); EMIT(e);
    strip_append(store, st, sm->P, sm->aq, sm->ib, &e); EMIT(e);
    return ne;
#undef EMIT
}

void tm_vehicle_update(TmStore *store, TmVehicle *v, const TmWheelIn wheel[4], const float O[3],
                       int flag, const TmGround *ground) {
    for (int i = 0; i < 4; i++) {
        if (tm_lays(&wheel[i])) {
            TmSample s;
            if (tm_sample(&wheel[i], i, ground, &s)) tm_trail_add(store, &v->trail[i], O, &s, flag, v->id, NULL);
        } else tm_trail_end(&v->trail[i]);
    }
}
void tm_vehicle_destroy(TmVehicle *v) { for (int i = 0; i < 4; i++) tm_trail_end(&v->trail[i]); }

/* ---- TM-11 ------------------------------------------------------------------------ */
double tm_view_k(double fov) {
    double t = tan(fov / 2);
    return t == 0 ? 10.0 : 256.0 / t;
}
int tm_view_metric(double K, double dc, int *fade) {
    int p = dc / 2 < 1 ? (int)(K * dc) : (int)(2 * K);
    if (p <= 4) { *fade = -1; return p; }
    if (p > 10) { *fade = 0; return p; }
    int f = (int)(256.0 - (p - 4) * 256.0 / 6.0);
    *fade = f & 255;
    return p;
}
int tm_cell_metric(double K, double r, double dist) {
    if (dist - r <= r) return (int)K;
    return (int)(K * r / (dist - r));
}
int tm_vertex_alpha(int ib, int fade) {
    int v = ib - fade;
    if (v < 0) v = 0;
    return v >> 1;
}

static double box_radius(const float lo[3], const float hi[3]) {
    double a = hi[0] - lo[0], b = hi[1] - lo[1], c = hi[2] - lo[2];
    return .5 * sqrt(a * a + b * b + c * c);
}
static double dist3(const float a[3], const float b[3]) {
    double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrt(x * x + y * y + z * z);
}

int tm_view_build(TmStore *s, const TmView *view, TmVertex *out, int first[TM_TEX_COUNT],
                  int count[TM_TEX_COUNT]) {
    int pass[TM_MAX_STRIPS], npass = 0, fade[TM_MAX_STRIPS];
    unsigned long stamp[TM_MAX_STRIPS];
    for (int c = 0; c < TM_MAX_CELLS; c++) {
        TmCell *cell = &s->cell[c];
        if (!cell->used) continue;
        float ctr[3] = { (cell->lo[0] + cell->hi[0]) * .5f, (cell->lo[1] + cell->hi[1]) * .5f,
                         (cell->lo[2] + cell->hi[2]) * .5f };
        double r = box_radius(cell->lo, cell->hi);
        double dot = (ctr[0] - view->cam[0]) * view->fwd[0] + (ctr[1] - view->cam[1]) * view->fwd[1]
                   + (ctr[2] - view->cam[2]) * view->fwd[2];
        if (dot < -r) continue;                      /* behind the camera */
        if (tm_cell_metric(view->K, r, dist3(ctr, view->cam)) <= 10) continue;
        if (view->frustum && !view->frustum(view->user, ctr, (float)r)) continue;
        if (s->frame - cell->touched > TM_CELL_REFRESH) cell->touched = s->frame;
        for (int i = 0; i < TM_MAX_STRIPS; i++) {
            TmStrip *st = &s->strip[i];
            if (!st->used || st->cell != c) continue;
            if (view->frustum && !view->frustum(view->user, st->centre, (float)box_radius(st->lo, st->hi))) continue;
            int f; tm_view_metric(view->K, dist3(st->centre, view->cam), &f);
            if (f < 0) continue;
            pass[npass] = i; fade[npass] = f; stamp[npass] = st->recent; npass++;
        }
    }
    /* oldest first so newer marks blend on top (draw order is TM-U4) */
    for (int a = 1; a < npass; a++) {
        int p = pass[a], f = fade[a]; unsigned long t = stamp[a]; int b = a - 1;
        while (b >= 0 && stamp[b] > t) { pass[b + 1] = pass[b]; fade[b + 1] = fade[b]; stamp[b + 1] = stamp[b]; b--; }
        pass[b + 1] = p; fade[b + 1] = f; stamp[b + 1] = t;
    }
    for (int a = 0; a < npass; a++) s->strip[pass[a]].recent = ++s->clock;   /* most-recent */
    int total = 0;
    for (int t = 0; t < TM_TEX_COUNT; t++) { first[t] = total; count[t] = 0; }
    for (int t = TM_TEX_ROAD; t < TM_TEX_COUNT; t++) {
        first[t] = total;
        if (!((view->texture_mask >> t) & 1u)) continue;
        for (int a = 0; a < npass; a++) {
            const TmStrip *st = &s->strip[pass[a]];
            if (tm_surface_texture(st->surface) != t || st->n < 2) continue;
            for (int k = 0; k + 1 < st->n; k++) {
                const TmPoint *p0 = &st->pt[k], *p1 = &st->pt[k + 1];
                float A0[3], A1[3];
                for (int j = 0; j < 3; j++) { A0[j] = p0->aq[j] / 64.0f; A1[j] = p1->aq[j] / 64.0f; }
                /* TM-15.2: the vertex alpha byte is the TM-11.2 value as is (0...127);
                 * alpha is not doubled. The colour doubling is in the pixel law. */
                int a0 = tm_vertex_alpha(p0->ib, fade[a]);
                int a1 = tm_vertex_alpha(p1->ib, fade[a]);
                const float lift = b32(.05);
                TmVertex *v = &out[total];
                const float *base[4] = { p0->P, p1->P, p1->P, p0->P };
                const float *arm[4] = { A0, A1, A1, A0 };
                const float sign[4] = { 1, 1, -1, -1 };
                const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 0, 0, 1, 1 };
                const int al[4] = { a0, a1, a1, a0 };
                for (int j = 0; j < 4; j++) {
                    v[j].x = base[j][0] + sign[j] * arm[j][0];
                    v[j].y = base[j][1] + sign[j] * arm[j][1];
                    v[j].z = base[j][2] + sign[j] * arm[j][2] + lift;
                    v[j].u = us[j]; v[j].v = vs[j];
                    v[j].rgba[0] = v[j].rgba[1] = v[j].rgba[2] = TM_VERTEX_RGB;
                    v[j].rgba[3] = (unsigned char)al[j];
                }
                total += 4;
                count[t] += 4;
            }
        }
    }
    return total;
}

/* ---- TM-15 render state and pixel law ---------------------------------------------- */
const TmRenderState *tm_render_state(void) {
    static const TmRenderState state = {
        TM_VERTEX_RGB, 2, 1, 1 /* blend: src alpha, one minus src alpha */, 0, 1 /* lequal */, 0, 0, 0,
        0.05f, 1, 1, 1, 3
    };
    return &state;
}
void tm_mark_source(const float texel_rgb[3], const unsigned char vertex_rgb[3], float out[3]) {
    const TmRenderState *st = tm_render_state();
    for (int j = 0; j < 3; j++) {
        float v = (float)st->colour_scale * texel_rgb[j] * ((float)vertex_rgb[j] / 255.0f);
        out[j] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
}
float tm_mark_opacity(float texel_alpha, int vertex_alpha) {
    return texel_alpha * ((float)vertex_alpha / 255.0f) * (float)tm_render_state()->alpha_scale;
}
void tm_mark_blend(const float dst[3], const float src[3], float a, float out[3]) {
    for (int j = 0; j < 3; j++) out[j] = src[j] * a + dst[j] * (1.0f - a);
}

long tm_payload_base(const unsigned char *data, long skipped) {
    long run = skipped;
    while (run > 0 && data[run - 1] == 0x11) run--;          /* start of the 0x11 run ending here */
    long aligned = (run + 0x7f) & ~0x7fL;
    return (aligned >= run && skipped > aligned) ? aligned : skipped;
}
