/* Tire marks (skid marks): independent implementation of
 * docs/cleanroom-driving-physics/tire-marks/SPEC.md (TM-1 ... TM-13).
 * Design log: docs/cleanroom-driving-physics/tire-marks/IMPL_DESIGN.md.
 *
 * Pure maths and a fixed-capacity strip store: no IO, no GL, no SDL. The host
 * supplies per-wheel quantities (TmWheelIn), a ground-height query and, for
 * drawing, a view description; the module returns vertices grouped by mark
 * texture. World z is up; lengths m, load kN (as Stage5). */
#ifndef OPENUG2_TIRE_MARKS_H
#define OPENUG2_TIRE_MARKS_H

#define TM_MAX_STRIPS 128      /* TM-10.1 (64 for two local players) */
#define TM_MAX_POINTS 8        /* TM-0 strip of 1...8 points */
#define TM_MAX_CELLS  40       /* TM-10.2 */
#define TM_SURFACES   29       /* TM-1.2 ids 0...28 */
#define TM_ORIGIN_RANGE 4.0f   /* TM-7 step 1 */
#define TM_CELL_REFRESH 300    /* TM-10.2: world frames (60 Hz) */
/* TM-2 clause 3: the reserved (global game-state code / venue id) block of the Organic
 * Drag events. Marks are never laid while the current venue id is inside it; free roam
 * (4000) and ordinary events are outside. */
#define TM_VENUE_RESERVED_LO 4400
#define TM_VENUE_RESERVED_HI 4499

/* Mark textures (TM-1.3): index into per-texture arrays. 0 = none. */
enum { TM_TEX_NONE = 0, TM_TEX_ROAD = 1, TM_TEX_DIRT = 2, TM_TEX_GRASS = 3, TM_TEX_COUNT = 4 };
#define TM_HASH_ROAD  0x9076F58Fu
#define TM_HASH_DIRT  0x906F30FCu
#define TM_HASH_GRASS 0x9E904149u

/* ---- TM-1 data -------------------------------------------------------------- */
float tm_surface_width_factor(int surface);   /* k_s (b32(1.4) for 8, 9, 15; else 1) */
int   tm_surface_texture(int surface);        /* TM_TEX_* */
unsigned tm_texture_hash(int texture);        /* 0 for TM_TEX_NONE */
/* Four tyre widths (m) from the 2192-byte car record (TM-1.1, +0x134+0x30*i).
 * Returns 1 on success. */
/* TM-15.5 "pixel bases": texture-pack offsets are relative to the 0x80-aligned start of the pixel
 * payload; the 0x11 bytes before it are padding. `skipped` is the position after a greedy skip
 * of 0x11 bytes (what the shared pack reader returns); when the run crosses the 0x80 boundary
 * because the first palette entry is itself 0x11-led, return the boundary instead. */
long tm_payload_base(const unsigned char *data, long skipped);
int tm_car_read(const unsigned char *data, long length, const char *name, float width[4]);

/* ---- TM-3 / TM-4 / TM-5 / TM-6: one wheel's sample ---------------------------- */
typedef struct {
    int   contact;         /* published contact flag */
    int   masked;          /* wheel bit of the suppression mask (TM-U3; default 0) */
    int   sliding;         /* sliding branch selected by the tyre preparation */
    float u;               /* utilisation (TM-3), published per wheel */
    float N;               /* aligned load, kN */
    float U[3];            /* sampled wheel velocity, world frame */
    float C[3];            /* world wheel centre */
    float l[3];            /* world wheel lateral axis (steered wheel frame) */
    int   surface;         /* ground surface id 0...28 */
    float width;           /* tyre width W_i, m */
} TmWheelIn;
/* Returns 0 (no probe found ground) or 1; *height receives the ground z. */
typedef int (*TmGroundFn)(void *user, int wheel, float x, float y, float z, float *height);
typedef struct { TmGroundFn query; void *user; float offset; } TmGround; /* offset = d_i (0) */

typedef struct {
    float P[3];            /* sample point Pm */
    float A[3];            /* half-width vector Am (unquantised) */
    float w, q, intensity;
    int   surface;
    signed char aq[3];     /* trunc(64*Am), TM-6 */
    unsigned char ib;      /* trunc(255*I) */
} TmSample;

int tm_lays(const TmWheelIn *wheel);                         /* TM-3 predicate */
/* TM-2 clause 3: 1 when the current global game-state code / venue id permits wheel
 * effects, 0 inside the reserved Organic-Drag block [4400, 4499]. */
int tm_venue_lays_marks(int venue_id);
/* TM-2 vehicle-level gate G, evaluated per vehicle: clause 1 (wheel effects enabled for
 * this vehicle) AND clause 2 (not the special mover mode) AND clause 3
 * (tm_venue_lays_marks of the shared venue id). A false G means nothing is sampled for
 * that vehicle this frame. */
int tm_vehicle_gate(int clause1, int clause2, int venue_id);
/* TM-4/5/6. Returns 1, or 0 when no ground is found by either probe
 * (TM-U7 host choice: nothing is laid). */
int tm_sample(const TmWheelIn *wheel, int index, const TmGround *ground, TmSample *out);
/* TM-3 steps 1-4: utilisation published per wheel. magnitude = M as tested by the
 * friction cap (before scaling), limit = Fmax. Header-inline so the drivetrain can
 * publish it without linking this module. */
static inline float tm_utilisation(double magnitude, double limit, int sliding_or_locked, int contact) {
    double u = limit > .001 ? (magnitude / limit < 1 ? magnitude / limit : 1) : 1;
    if (sliding_or_locked) u = 1;
    if (!contact) u = 0;
    return (float)u;
}

/* ---- strips, store, trails (TM-6 ... TM-10) ------------------------------------ */
typedef struct { float P[3]; signed char aq[3]; unsigned char ib; } TmPoint;
struct TmTrail;
typedef struct TmStrip {
    int used, n, surface, flag, creator, cell, open;
    unsigned long recent;          /* recency stamp, larger = more recent */
    float D[3], S;
    float lo[3], hi[3], centre[3]; /* strip bounding box and its centre */
    struct TmTrail *trail;         /* trail holding this strip open, or NULL */
    TmPoint pt[TM_MAX_POINTS];
} TmStrip;
typedef struct { int used, cx, cy; float lo[3], hi[3]; long touched; } TmCell;
typedef struct TmTrail { TmStrip *strip; } TmTrail;   /* strip == NULL: no open strip */
typedef struct {
    int capacity;                  /* 128, or 64 with two local players */
    TmStrip strip[TM_MAX_STRIPS];
    TmCell  cell[TM_MAX_CELLS];
    unsigned long clock;           /* recency counter */
    long frame;                    /* host world-frame counter (60 Hz) */
} TmStore;

/* Event log for sample addition (used by tests and diagnostics). */
typedef enum {
    TM_EV_FAR, TM_EV_NEW, TM_EV_NEW_SEEDED, TM_EV_NEW_CONTINUED,
    TM_EV_COMMIT, TM_EV_REPLACE, TM_EV_END, TM_EV_FULL
} TmEvent;
const char *tm_event_name(TmEvent e);

void tm_init(TmStore *store, int capacity);    /* session start: clears everything */
void tm_set_frame(TmStore *store, long frame); /* world-frame counter */
void tm_cell_key(float x, float y, int *cx, int *cy);   /* TM-10.2 */
/* TM-7: add a sample to a wheel's trail. O = vehicle origin; flag = view flag f.
 * Events (if non-NULL, room for 6) are appended; returns their count. */
int  tm_trail_add(TmStore *store, TmTrail *trail, const float O[3], const TmSample *sample,
                  int flag, int creator, TmEvent *events);
void tm_trail_end(TmTrail *trail);             /* TM-9 */

/* One vehicle: four trails. */
typedef struct { TmTrail trail[4]; int id; } TmVehicle;
/* TM-2/3/7: run the sampler for wheels 0...3 in order. The caller applies the
 * vehicle-level gate G (when false, do not call). */
void tm_vehicle_update(TmStore *store, TmVehicle *vehicle, const TmWheelIn wheel[4],
                       const float O[3], int flag, const TmGround *ground);
void tm_vehicle_destroy(TmVehicle *vehicle);   /* ends every trail (TM-9) */

/* ---- TM-11 rendering ------------------------------------------------------------ */
double tm_view_k(double fov_radians);          /* K = 256/tan(phi/2), 10 if tan 0 */
/* TM-11.3. Returns p; *fade = F (0...255) or -1 when the strip is not drawn. */
int    tm_view_metric(double K, double distance, int *fade);
int    tm_cell_metric(double K, double radius, double distance);   /* TM-11.4 */
int    tm_vertex_alpha(int intensity_byte, int fade);               /* TM-11.2 */

typedef struct {
    float cam[3], fwd[3];          /* camera position and forward unit vector */
    double K;                      /* tm_view_k of this view's field of view */
    /* Optional frustum test on a bounding sphere; NULL = always passes (TM-U12). */
    int (*frustum)(void *user, const float centre[3], float radius);
    void *user;
    unsigned texture_mask;         /* bit TM_TEX_x set when that texture is available */
} TmView;
typedef struct { float x, y, z, u, v; unsigned char rgba[4]; } TmVertex;
#define TM_MAX_VERTICES (TM_MAX_STRIPS * (TM_MAX_POINTS - 1) * 4)
/* Runs the per-view tests of TM-11.3/TM-11.4 (updating strip recency and cell
 * touches), then emits four vertices per segment quad grouped by texture:
 * vertices of texture t occupy out[first[t] ... first[t]+count[t]). Returns the
 * total vertex count. `out` must hold TM_MAX_VERTICES entries. */
int tm_view_build(TmStore *store, const TmView *view, TmVertex *out,
                  int first[TM_TEX_COUNT], int count[TM_TEX_COUNT]);


/* ---- TM-15 render state ------------------------------------------------------------ */
/* Vertex bytes are emitted as is: RGB = TM_VERTEX_RGB (128), alpha = TM-11.2 value
 * (0...127). The draw must realise this pixel law (TM-15.1/15.2/15.6), with T the
 * filtered texel and C the interpolated vertex colour (both 0...1):
 *     out.rgb = clamp(2 * T.rgb * C.rgb, 0, 1)      (colour doubled)
 *     out.a   = T.a * C.a                            (alpha NOT doubled)
 *     result  = out.rgb * out.a + dst * (1 - out.a)  (then world fog on out.rgb) */
#define TM_VERTEX_RGB 128
typedef struct {
    int   vertex_rgb_byte;     /* 128 */
    int   colour_scale;        /* 2 */
    int   alpha_scale;         /* 1 */
    int   blend_src_alpha;     /* 1: src*a + dst*(1-a) */
    int   depth_write;         /* 0 */
    int   depth_lequal;        /* 1: depth test on, pass if <= */
    int   cull;                /* 0: both windings */
    int   alpha_test;          /* 0 */
    int   depth_bias;          /* 0: none (no polygon offset) */
    float z_lift_m;            /* 0.05 (TM-11.1), the only z-fight measure */
    int   wrap_u, wrap_v;      /* 1: repeat addressing */
    int   mip_linear;          /* 1 */
    int   mip_levels;          /* 3 (host may carry more levels, see IMPL_DESIGN.md) */
} TmRenderState;
const TmRenderState *tm_render_state(void);
/* Reference model of the pixel law, used by the tests and by the host shader's contract.
 * texel_rgb 0...1, vertex_rgb bytes -> source colour. */
void  tm_mark_source(const float texel_rgb[3], const unsigned char vertex_rgb[3], float out[3]);
/* Per-pixel opacity: texel alpha (0...1) x vertex alpha byte / 255. */
float tm_mark_opacity(float texel_alpha, int vertex_alpha);
void  tm_mark_blend(const float dst[3], const float src[3], float a, float out[3]);

#endif
