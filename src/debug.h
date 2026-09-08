/* OpenUG2 debug tunables — shared between the C engine (main.c) and the optional
 * Dear ImGui panel (debugui.cpp, built only with `make debug` / -DDEBUG_UI).
 * g_dbg is defined once in main.c and exists in every build; without the UI it
 * just holds its defaults, so the engine reads it the same either way. */
#ifndef OPENUG2_DEBUG_H
#define OPENUG2_DEBUG_H
#ifdef __cplusplus
extern "C" {
#endif

/* One decoded scripted-object entity definition (0x39200 chunk in a district
 * companion L4R*.BUN). Local bounding-box extents only; see docs/FORMATS.md. */
typedef struct {
    char name[32];         /* ZCV_/ZCS_ identifier, NUL-terminated */
    unsigned int hash;     /* FNV-32 of the name */
    float w, l, h;         /* local OBB extents (X, Y, Z) */
} ScriptedDef;

#define WORLD_EVPOLY 33

/* One shipped race event (Phase 71), decoded from the 0x3414c leaf that every
 * per-event TRACKS/ROUTES<REG>/Paths<id>.bin carries and PathsFreeRoam.bin does
 * NOT — that absence is the game's own Freeroam/Race split (see world.h). */
typedef struct {
    int   id;                       /* 4001.. — also the Paths<id>.bin number */
    char  reg[8];                   /* owning region stem, e.g. "L4RA" */
    int   circuit;                  /* 1 = closed lap circuit, 0 = point-to-point sprint */
    int   npoly;                    /* valid outline points; poly[npoly-1] == poly[0] */
    int   len100m;                  /* coarse course-length hint, units of 100 m */
    float poly[WORLD_EVPOLY][2];    /* closed track-outline polygon, world XY */
    float bb[4];                    /* x0,x1,y0,y1 of the outline */
    int   node0, node1;             /* [node0,node1) = this event's nav nodes */
} WEvent;

/* A road closed off by the active race event: the point where a drivable link
 * leaves the race corridor, plus the outward direction along that link. */
typedef struct {
    float x, y;      /* midpoint of the severed link */
    float dx, dy;    /* unit vector pointing OUT of the corridor */
    int   a, b;      /* the nav nodes it separated (a inside, b outside) */
} WBarrier;

enum { MODE_FREEROAM = 0, MODE_RACE_EVENT = 1 };

#define WORLD_MAXGATE 40
#define WORLD_MAXGRID 24

/* One checkpoint gate: a line the car must cross, centred on the racing line
 * and square to the direction of travel. Gate 0 is the start/finish. */
typedef struct {
    float x, y;        /* gate centre, on the racing line */
    float dx, dy;      /* unit direction of travel through the gate */
    float half;        /* half-width, metres (the gate spans +-half across dx,dy) */
    int   node;        /* the racing-line node it was snapped to */
} WGate;

/* Live race progression for the active event. */
typedef struct {
    int   active;                    /* 1 = a race is being tracked */
    int   ev;                        /* index into World.ev */
    int   lap, maxlaps;              /* lap 0 = not yet over the start line */
    int   next;                      /* the ONLY armed gate; nothing else counts */
    int   cleared;                   /* checkpoints cleared on the current lap */
    int   finished;
    int   ngate;
    WGate gate[WORLD_MAXGATE];
    float grid[WORLD_MAXGRID][3]; int ngrid;  /* start-grid slots (chunk 0x34146):
                                                 XY at +16/+20, the slot's own Z
                                                 at +24 (M118) */
    float px, py; int havep;         /* previous car XY: gates are crossed, not touched */
} WRace;

/* Explicit per-corner wheel stance for the active car, in the car's own model
   space (X = longitudinal/+forward, Y = lateral, Z = vertical/up). Replaces the
   old AABB-fraction heuristic: values are absolute metres, set once at load from
   a per-car table (or a body-box fallback) and live-editable in the ImGui panel.
   The task's Z-forward / Y-up field names map here as: front_axle_z->front_axle,
   track_width(X)->front/rear_track, ride_height_y->ride_y. */
typedef struct {
    float front_axle;    /* X of the front axle line */
    float rear_axle;     /* X of the rear axle line (negative = behind origin) */
    float front_track;   /* full front track width (lateral wheel-centre spacing) */
    float rear_track;    /* full rear track width */
    float ride_y;        /* hub-centre height (measured from the car's tyre mesh) */
} VehicleWheelConfig;

typedef struct {
    /* --- freecam (works in every build; toggle with F) --- */
    int   freecam;
    float speed;                 /* freecam move units/frame */

    /* --- chase camera (3rd-person follow): ideal pos = car - forward*distance
       + up*height; actual pos and look-target ease toward their ideals by
       `stiffness` each frame (exponential smoothing, fixed timestep) --- */
    float chase_distance, chase_height, chase_stiffness;
    int   auto_drive;   /* 1 = feed steady throttle + sine steer to the physics so
                           the car drives an S-curve hands-free (camera-spring test);
                           interactive only -- never overrides the --shot autopilot */

    /* --- wheel placement: explicit per-car stance (see VehicleWheelConfig) --- */
    VehicleWheelConfig wheel;
    float wheel_scale;           /* rim radius multiplier (fits rim to the arch) */
    int   wheel_demo;            /* 1 = drive spin/steer from a demo clock (free spin +
                                    sine steer) instead of physics, to verify the matrices */

    /* --- lighting (fed to the shader as uniforms) --- */
    int   night_mode;   /* 1 = night: low ambient + emissive light lenses + headlight
                           pools/bloom; 0 = day: raised ambient, lenses lit normally */
    float ambient, diffuse, body_spec;
    float body_env;   /* multiplier on the body/misc env-reflection strength (clearcoat sheen) */
    float vcolor;   /* 0..1 strength of world per-vertex prelight (baked AO/tint) */
    float fog_density;          /* exp^2 fog: f = exp(-(depth*density)^2) */
    float fog_r, fog_g, fog_b;  /* fog + sky-clear colour (kept identical) */

    /* --- car appearance --- */
    int   paint_override;        /* 1 = use paint[] below instead of the per-car hash */
    float paint[3];
    int   show_body, show_glass, show_lights, show_tires, show_misc, show_track;

    /* --- readouts (engine writes, panel displays) --- */
    float cam[3], car[3], heading, kmh;
    int   car_meshes, track_meshes, fps;
    int   drawn;

    /* --- session info + menu HUD (moved off the 3D viewport in debug builds:
       consulted only under DEBUG_UI, so plain builds are unaffected either
       way) --- */
    int  hud_hide_menu;              /* 1 = ALL retro pixel-font viewport text
                                         (menu car/track/pips/prompt AND the
                                         in-race position/lap/speed HUD) is
                                         suppressed; read it in ImGui instead.
                                         Plain builds have no ImGui, so they
                                         never consult this — always drawn. */
    char car_name[32], track_name[64];
    int  sel_car, n_cars, sel_track, n_tracks, sel_circuit, n_circuits;
    int  race_pos, race_cars, race_lap, race_laps;   /* mirrors the in-race HUD */

    /* Session combo boxes: main.c points these at its own fixed-size car/
     * track name arrays (read-only from here) once at startup; they don't
     * move afterward, so the raw pointer is safe for the process lifetime.
     * The panel writes want_car/want_track (else leaves them -1) when the
     * user picks a different entry; main.c polls them once per frame and
     * performs the SAME relaunch() the arrow-key menu already uses. */
    const char (*car_list)[64];
    const char (*track_list)[64];
    int  want_car, want_track;

    /* --- car modification (wheel library); panel writes, main.c acts --- */
    const char (*wheel_brands)[24];  /* brand name table owned by main.c */
    int  wheel_brand_n;              /* entries in it */
    int  wheel_brand;                /* selected index */
    int  wheel_style;                /* STYLEnn within that brand */
    int  wheel_reload;               /* panel sets 1 => main.c re-streams rims */

    /* --- Mesh Inspector (passive: observes/overlays, never alters assets) --- */
    int  insp_count;        /* how many car meshes are inspectable */
    const int *insp_cat;    /* N2_CAR_* per mesh (main.c owns the array) */
    const int *insp_verts;  /* vertex count per mesh */
    int  insp_sel;          /* selected mesh index, -1 = none */
    int  insp_highlight;    /* 1 = force the selection to a neon unlit overlay */
    int  insp_wire;         /* 1 = draw the selection as wireframe */
    int  insp_dump;         /* panel raises, main.c dumps telemetry and clears */
    int  insp_flipn;        /* 1 = negate normals on the selected mesh (uFlipN) */
    int  insp_cull;         /* 0 = no culling (engine default), 1 = cull back, 2 = cull front */
    int  insp_glass_depth;  /* 1 = glass pass writes depth (diagnostic) */

    /* --- live handling tuner (multipliers; top in km/h) --- */
    float tune_accel, tune_brake, tune_turn, tune_top;

    /* --- wheel kinematics telemetry (engine writes, panel displays) --- */
    float wheel_rpm, steer_deg, wheel_radius;

    /* --- navigation graph for the ImGui minimap (main.c owns the arrays) --- */
    const float *nav; int nnav;          /* node XY pairs */
    const int *navedge; int nnavedge;    /* node-index pairs */
    const int *navcomp;                  /* district (component) index per node */
    int   ndist;                         /* fused districts (area codes) */
    char  dist_tok[8][4];                /* their 2-letter codes, for the legend */
    char  dist_name[8][24];              /* canonical UI names (external truth) */
    /* GPS: panel writes gps_want_x/y on right-click, main.c solves and fills
       gps_path/gps_n/gps_dist/gps_ms for the panel to draw. */
    float gps_want_x, gps_want_y; int gps_request;
    const int *gps_path; int gps_n; float gps_dist; int gps_ms;
    float navbb[4];                      /* x0,x1,y0,y1 */

    /* --- Race & Track Manager (Phase 71): engine writes the catalog + the
       active mask, the panel writes want_mode/want_event and main.c applies
       them through world_set_mode(). --- */
    const WEvent *ev; int ev_count;      /* shipped race events (world.ev) */
    const WBarrier *bar; int bar_count;  /* barriers of the active event */
    const char *navopen;                 /* per-node corridor mask, NULL in freeroam */
    int   mode;                          /* MODE_FREEROAM / MODE_RACE_EVENT */
    int   active_ev;                     /* index into ev[], -1 = freeroam */
    int   masked_links;                  /* directed CSR links the barriers disabled */
    int   want_mode, want_event, mode_request;
    const WRace *race;                   /* live checkpoint/lap state (world.race) */
    int   race_maxlaps_want;             /* panel writes, engine picks up on start */
    int   race_start_request, race_stop_request;

    /* --- current city zone (engine writes each frame) --- */
    char  zone_name[24];        /* area code the car is inside, "" if none */
    int   zone_count;           /* zones parsed for this region set */

    /* --- scenery semantics (engine writes each frame; panel displays) --- */
    int   scen_count[8];        /* mesh count per N2_SC_* class */
    int   scen_near_n;          /* named chunks near the car (<=12) */
    char  scen_near[12][40];    /* "NAME  [CLASS]  d=..m" rows */

    /* --- rim paint (recolor the OEM gold rim diffuse; silver by default) --- */
    int   rim_paint;        /* 1 = tint toward rim_color, 0 = raw OEM texture */
    float rim_color[3];     /* rim paint colour */

    int   car_cull;         /* 1 = back-face cull the car shell */
    float body_clearcoat;   /* tight lacquer highlight over the base coat */

    /* --- neon underglow (a real customization, not a diagnostic) --- */
    int   neon_on;          /* 1 = project the underglow pool */
    float neon_col[3];      /* emission colour */
    float neon_str;         /* intensity */

    /* --- diagnostics --- */
    int  show_uv_checker;   /* fed to the shader's uUVCheck uniform each frame */

    /* --- scripted-object entity DEFINITIONS (read-only inspector) ---
       Decoded from the district companion L4R*.BUN (0x39200/0x39201 chunks).
       DEFINITIONS ONLY: the data carries no world placement or mesh, so this
       is a decode readout, not live entities. main.c owns the array. */
    int  scripted_count;
    const ScriptedDef *scripted;
} DbgState;

extern DbgState g_dbg;

/* Dear ImGui bridge — no-ops unless DEBUG_UI is compiled in. Declared always so
 * main.c can call them unconditionally behind a single #ifdef. */
#ifdef DEBUG_UI
struct SDL_Window; union SDL_Event;
void dbgui_init(struct SDL_Window *win, void *glctx);
void dbgui_event(const union SDL_Event *e);
int  dbgui_want_mouse(void);          /* 1 = panel is grabbing the mouse */
int  dbgui_want_keyboard(void);
void dbgui_frame(void);               /* build the panel from g_dbg */
void dbgui_render(void);              /* draw it (call last, before SwapWindow) */
void dbgui_shutdown(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
