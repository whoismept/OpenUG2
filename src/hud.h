/* hud.h — OpenUG2 in-game player HUD (roadmap stage 8).
 *
 * Opt-in via --hud. Draws the retail-style speed/RPM dial, nitrous bar, gear,
 * minimap, navigation arrow, race position/lap and the Cingular message
 * indicator, using the shipped HUD art decoded through the project's existing
 * TPK reader (n2_tpk_open / n2_tpk_decode). No asset is copied into the repo.
 *
 * HONESTY CONTRACT. This module renders what the engine actually knows and
 * nothing else. Several retail readouts have no live system behind them yet
 * (see HudState's availability flags); those draw an explicit inactive/empty
 * state rather than a plausible-looking number. Sample values exist only under
 * hud_set_preview(), which also paints a visible PREVIEW tag, so a screenshot
 * can never be mistaken for working telemetry.
 */
#ifndef OPENUG2_HUD_H
#define OPENUG2_HUD_H

#include "render.h"

/* Everything the HUD draws, sampled by the caller once per frame. The HUD
 * never reaches into engine globals: this struct is the whole contract, which
 * is what makes the layout and transform maths testable without a GL context. */
typedef struct {
    /* --- drivetrain readouts --- */
    float speed_kmh;          /* REAL: PHYS_KMH(speed) */
    float rpm, rpm_redline;   /* PROVISIONAL: the virtual AUDIO gearbox */
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
    float drift_score,drift_chain,engine_heat;
    int shift_ready,engine_failed;
    int   racing;             /* 0 = free roam, 1 = in an event */
    int   circuit;            /* race only: 1 = laps, 0 = sprint (progress) */
    int   lap, laps;          /* circuit only */
    float sprint_progress;    /* sprint only, 0..1 */
    int   position, cars;     /* 0 = unknown/no opponents */

    /* --- minimap geometry: world-space polyline, borrowed, not owned --- */
    const float *route_xy;    /* interleaved x,y */
    int   route_n;
    const float *nav_xy;      /* free-roam road network */
    int   nav_n;
    float map_radius_m;       /* world metres to the panel edge; 0 = default.
                                 A race sets this to fit the whole route. */
    int   route_loop;         /* 1 = circuit: close the polyline */
    int   route_next;         /* index of the armed gate, -1 = none */

    /* --- navigation --- */
    int   nav_active;         /* a destination is set AND a route was solved */
    float nav_target_x, nav_target_y;
    float nav_bearing;        /* radians, world bearing to the next route node */
    float nav_dist_m;

    /* --- message indicator (free roam only) --- */
    int   message_waiting;    /* engine never sets this yet; see HUD_MSG_* */
} HudState;

/* Opaque GL-side resources. */
typedef struct Hud Hud;

/* Load the HUD art from `dataroot`. Returns NULL only if allocation fails; a
 * missing container or texture is NOT fatal — the affected element is skipped
 * and named by hud_missing(). */
Hud *hud_init(const char *dataroot);
void hud_free(Hud **h);

/* One frame. Restores every GL state and uniform it touches. */
void hud_draw(Hud *h, const RProg *rp, GpuMesh *quad,
              const HudState *s, int width, int height);

/* Clearly-labelled sample values for layout review. Off by default. */
void hud_set_preview(Hud *h, int on);
int  hud_preview(const Hud *h);

/* Space-separated names of art that failed to load, "" when all present. */
const char *hud_missing(const Hud *h);

/* ---- pure helpers, unit-tested without GL (see tools/hud_test.c) ---- */

/* Signed angle from `from` to `to`, wrapped to (-pi, +pi]. The navigation
 * arrow rotates by this, so the wrap must be exact or the arrow spins the long
 * way round every time the car crosses the +/-pi seam. */
float hud_angle_delta(float from, float to);

/* World -> minimap transform. The map is a north-up-or-heading-up window of
 * radius `radius_m` around (cx,cy); `rot` is the rotation applied (0 for
 * north-up, -heading-pi/2 for heading-up). Writes normalised map coordinates
 * in [-1,1] where 1 = the window edge; the caller scales into its panel.
 * Points outside the window are returned unclamped so the caller can cull. */
void hud_map_project(float wx, float wy, float cx, float cy,
                     float radius_m, float rot, float *mx, float *my);

/* The secondary status line, as a pure function of the state, because the
 * rule it encodes is a correctness requirement rather than a style choice:
 * a SPRINT has no laps, so printing "LAP 1/2" there would be a fabricated
 * circuit readout. Circuits get laps, sprints get real gate progress, and
 * free roam gets nothing. Always NUL-terminates. */
void hud_status_text(const HudState *s, char *out, int cap);

/* Is the N2O section present at all? Fitted parts are removed, not greyed
 * out: an unfitted car shows no nitrous bar and the cluster closes up around
 * the gap. `have_turbo` is the matching switch for a boost dial; the standard
 * HUD has no turbo art to draw (the only turbo gauge in the game is
 * DRAG_TURBO_* inside InGameDrag.bun, which belongs to the drag HUD), so
 * nothing consumes it yet and an unfitted turbo is simply the whole story. */
int hud_shows_nos(const HudState *s);

/* Money is hidden during a race, and shown in free roam only when a career
 * economy actually exists. Both halves are requirements, so both are tested. */
int hud_shows_money(const HudState *s);

/* Fraction of texels whose quantised colour equals their left neighbour's.
 * HUD art is flat-shaded, so this is high; a mis-decoded record is noise, so
 * it is low. Measured on the shipped packs: every usable sprite scores
 * 0.719..0.996 (HUD_CINGULAR_LOGO 0.992, MINIMAP_BORDER 0.978, the 16x16
 * MINIMAP_ICON_CAR 0.719), while ARROW_MARKERA -- which decodes to coloured
 * noise -- scores 0.192. hud_init refuses anything below HUD_ART_RUN_MIN.
 * rgb is w*h*3, top-down. */
#define HUD_ART_RUN_MIN 0.50f
float hud_art_run_fraction(const unsigned char *rgb, int w, int h);

/* Hit-test a click in NDC against the minimap panel and convert it to world
 * coordinates, so a destination can be chosen from the map with no developer
 * UI present. Returns 0, leaving the outputs alone, when the point is outside
 * the panel. The inverse of the transform hud_draw uses, same frame. */
int hud_map_pick(float ndc_x, float ndc_y,
                 int width, int height, const HudState *s,
                 float *wx, float *wy);

/* Uniform-scale layout: converts a position in HUD units (a 2 x 2/aspect box
 * whose HEIGHT is always 2.0, so a unit is the same on screen at 16:9 and 4:3)
 * into NDC. anchor_x/anchor_y in [-1,1] select the screen corner to hang off.
 * This is what keeps proportions and safe margins identical across aspects. */
void hud_place(float aspect, float anchor_x, float anchor_y,
               float off_x, float off_y, float *ndc_x, float *ndc_y);

#endif
