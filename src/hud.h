/* Native asset-driven HUD, independently implemented from the approved
 * bounded interoperability specification. Numeric telemetry comes from the
 * host; authoring samples and unavailable telemetry are hidden.
 * Font baseline, needle direction and minimap projection remain provisional. */
#ifndef OPENUG2_HUD_H
#define OPENUG2_HUD_H

#include "render.h"

/* Everything the HUD draws, sampled by the caller once per frame. The HUD
 * never reaches into engine globals: this struct is the whole contract, which
 * is what makes the layout and transform maths testable without a GL context. */
typedef struct {
    /* Approved bounded HUD inputs; map axes are translated at this boundary. */
    int skin_index, hide_map, hide_gauge;
    int career_context, suppress_cash;
    int map_heading_explicit;
    double map_heading_clockwise, map_window_world;
    /* --- drivetrain readouts --- */
    float speed_kmh;          /* REAL: PHYS_KMH(speed) */
    int imperial_units;      /* display only: 0 = KMH, nonzero = MPH */
    float rpm, rpm_redline;   /* PROVISIONAL: the virtual AUDIO gearbox */
    float rpm_maximum;        /* limiter/upper bound, separate from redline;
                                0 retains the provisional redline fallback */
    int   gear;               /* <0 reverse, 0 neutral, 1..n forward */
    int   nitro_active;       /* REAL, but binary: there is no tank model */

    /* --- availability. 0 = no live system; draw the empty state. --- */
    unsigned char have_rpm;      /* provisional source present */
    unsigned char have_nos;      /* a nitrous system is FITTED to this car.
                                    0 removes the N2O arc entirely. */
    unsigned char have_turbo;    /* a turbo is FITTED. 0 removes the boost
                                    dial entirely (not a greyed-out dial). */
    unsigned char have_nitro_tank;
    unsigned char have_boost;
    unsigned char have_money;
    float nitro_tank;         /* 0..1, meaningful only if have_nitro_tank */
    float boost;              /* 0..1, meaningful only if have_boost */
    long  money;              /* meaningful only if have_money */

    /* --- world placement --- */
    float car_x, car_y;
    float car_heading;        /* radians, world +X = 0, CCW */

    /* --- mode --- */
    int   race_kind;
    int   race_event_id;      /* positive for an authored event, 0 otherwise */
    float drift_score,drift_chain,engine_heat;
    int shift_ready,engine_failed;
    int   racing;             /* 0 = free roam, 1 = in an event */
    int   circuit;            /* race only: 1 = laps, 0 = sprint (progress) */
    int   lap, laps;          /* circuit only */
    float sprint_progress;    /* sprint only, 0..1 */
    int   position, cars;     /* 0 = unknown/no opponents */
    int race_phase;           /* HUD_RACE_*; zero withholds phase/banner */
    unsigned char have_race_time;
    double elapsed_seconds, countdown_seconds;
    float drift_angle;

    /* --- minimap geometry: world-space polyline, borrowed, not owned --- */
    const float *route_xy;    /* interleaved x,y */
    int   route_n;
    const float *nav_xy;      /* free-roam road network */
    int   nav_n;
    float map_radius_m;       /* retained baseline helper radius; asset maps use map_window_world */
    const float *opponent_xy; /* interleaved live rival positions, borrowed */
    int opponent_n;
    int   route_loop;         /* 1 = circuit: close the polyline */
    int   route_next;         /* index of the armed gate, -1 = none */

    /* --- navigation --- */
    int   nav_active;         /* a destination is set AND a route was solved */
    float nav_target_x, nav_target_y;
    float nav_bearing;        /* radians, world bearing to the next route node */
    float nav_dist_m;

    /* --- message indicator (free roam only) --- */
    int   message_waiting;    /* engine never sets this yet; see HUD_MSG_* */
    const char *notice;       /* short banner text (outrun prompt/result), NULL = none */

    /* --- capture-amendment inputs (approved SPEC capture sets 1-3) ---
       Explicit "supplied" flags: an absent value hides the opt-in element
       rather than being read as zero. */
    unsigned char have_redline_rpm; /* rpm_redline is a real host value */
    unsigned char have_shift_rpm;   /* shift_rpm is a real host value */
    float shift_rpm;                /* host shift-up RPM for the indicator */
    /* Capture set 6 (F1). Optional host input "gear change in progress",
       true for the host's own gear-change interval; zero (the default, i.e.
       absent) means not in progress. With it set, forward gears use the
       gear-colour setting; gear 1 at standstill uses it regardless.
       Capture set 5: clock_ms is the monotonic host clock (ms) shared with
       hud_nos_glow_event(); read while the opt-in glow sequence runs and
       while the shift icon is shown (set 7, H3 item 5). */
    unsigned char gear_change_in_progress;
    double clock_ms;
    /* Capture set 7 (H3/H4): clock_ms also times the shift-icon flash.
       Optional transmission mode, HA_TRANSMISSION_* (0 = absent). */
    unsigned char transmission;
} HudState;

/* Named HUD settings from the capture amendments. Gear and shift colours
 * replace RGB only (authored alpha kept; the shift icon's alpha also follows
 * its flash); the highlight replaces RGBA. The inferred redline, NOS level and
 * N2O glow are opt-in and default off; the shift icon is default on (set 7,
 * H3 item 1) and still needs a host shift RPM (hud_default_settings). */
typedef struct {
    unsigned char gear_colour[3];      /* C1-3/E1, F1: gear change in progress, or gear 1 at standstill: 255,128,64 */
    unsigned char highlight_colour[4]; /* D2 right-arrow highlight RGBA: 255,190,50,255 */
    unsigned char shift_colour[3];     /* H3 item 6 shift-up icon RGB: 202,244,138 */
    int inferred_redline;              /* C1-5 trimmed redline, default 0 */
    int inferred_nos_level;            /* C3-4 NOS level arc, default 0 */
    int shift_indicator;               /* H3 item 1 shift-up icon, default 1 */
    int shift_flash;                   /* H3 item 5 flash, default 1 (0 = steady lit) */
    int shift_icon_automatic;          /* H4 icon with an automatic transmission, default 1 (guessed) */
    double nos_a0_degrees, nos_a1_degrees; /* C3-4 dial span: 91, 239 */
    double gear_standstill_mps;        /* F1 gear-1 standstill threshold: 0.5 (guessed) */
    int nos_glow;                      /* E3 N2O glow label, default 0 (hidden) */
    double glow_ms_per_unit;           /* E3 fitted time unit: 0.82 ms per unit; also the H1 shift flash unit */
    int glyph_half_texel_inset;        /* set 8 G6 item 4 compatibility setting, default 0:
                                          glyphs are sampled edge to edge (no inset) */
    int fit_policy;                    /* project widescreen policy (not original behaviour),
                                          HA_FIT_WIDE (default, 0) or HA_FIT_43; see hud_asset.h */
} HudSettings;

/* Opaque GL-side resources. */
typedef struct Hud Hud;

/* Load the HUD art from `dataroot`. Returns NULL only if allocation fails; a
 * missing container or texture is NOT fatal — the affected element is skipped
 * and named by hud_missing(). */
Hud *hud_init(const char *dataroot);
/* Sharp minimaps. The size follows the detail setting; 0 keeps the retail
 * images. The HUD reads the world only inside hud_set_map_source and when a race
 * in another map frame starts. Everything else runs on a worker thread, and a
 * finished map replaces the retail image on a later frame. Pointers are borrowed. */
void hud_set_map_source(Hud *h, const N2Scene *scene, const float *nav, int nnav,
                        const int *start, const int *list, int size);
/* Request the free-roam map, from the source above. */
void hud_request_city_map(Hud *h);
/* Block until a requested map is ready: for captures, which need one fixed frame. */
void hud_set_map_sync(Hud *h, int on);
void hud_free(Hud **h);
#ifdef HUD_TEST_HOOKS
/* Test only: a HUD with one white image node shown as the career widget backing; needs a GL context, no game data. */
Hud *hud_test_white_widget(void);
#endif

/* One frame. Restores every GL state and uniform it touches. */
void hud_draw(Hud *h, const RProg *rp, GpuMesh *quad,
              const HudState *s, int width, int height);

void hud_default_settings(HudSettings *out);
void hud_configure(Hud *h, const HudSettings *settings);

/* E3: host "NOS glow" event. Starts the opt-in glow sequence at `now_ms` (the
 * same clock as HudState.clock_ms). Returns 1 when a sequence started; 0 when
 * the setting is off, the clock or time unit is invalid, or one is already
 * running (a new event while running is ignored; guessed). */
int hud_nos_glow_event(Hud *h, double now_ms);

/* Retained preview request flag; telemetry is never fabricated by this HUD. */
void hud_set_preview(Hud *h, int on);
int  hud_preview(const Hud *h);

/* Space-separated names of art that failed to load, "" when all present. */
const char *hud_missing(const Hud *h);

/* Independent verification report contains counts and resource status only. */
typedef struct { unsigned nodes,images,texts,groups,resources,fonts,original_atlases; int gauge_targets,map_targets,track_metadata; unsigned resource_resolved,resource_drawn; } HudAudit;
int hud_pick(Hud *h,float ndc_x,float ndc_y,float *world_x,float *world_y);
void hud_audit(const Hud *h,HudAudit *out);

/* Legacy helper API retained for engine compatibility. The standard driving
 * renderer uses the asset contracts in hud_asset.h and race roles in hud_race.h. */
float hud_angle_delta(float from,float to);
int hud_speed_value(float speed_kmh,int imperial);
float hud_tach_fraction(const HudState *s,float dial_scale);
void hud_map_project(float wx,float wy,float cx,float cy,float radius_m,float rot,float *mx,float *my);
void hud_status_text(const HudState *s,char *out,int cap);
int hud_shows_nos(const HudState *s);
int hud_shows_money(const HudState *s);
#define HUD_ART_RUN_MIN 0.50f
int hud_catalog_has(const unsigned char *data,size_t len,uint32_t key);
float hud_art_run_fraction(const unsigned char *rgb,int w,int h);
int hud_map_pick(float x,float y,int width,int height,const HudState *s,float *wx,float *wy);
void hud_place(float aspect,float anchor_x,float anchor_y,float off_x,float off_y,float *x,float *y);
#endif
