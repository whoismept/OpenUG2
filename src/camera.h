/* camera.h — OpenUG2 gameplay (driving) camera.
 *
 * Pure maths: no GL, no SDL, no file IO beyond the optional settings file.
 * Implements the behaviour described in docs/CAMERA.md (docs/cleanroom-camera/SPEC.md):
 * six view types, the per-car record from GLOBALB, restarting-cubic-Hermite
 * smoothers, acceleration-driven offsets, shake and the projection scales.
 *
 * Units: metres, seconds, m/s, m/s^2. World frame +Z up. Car frame +X forward,
 * +Y left, +Z up. Angles in "binary angle units" (BAU, 65536 = 360 deg) where
 * the data stores them; radians elsewhere.
 */
#ifndef OPENUG2_CAMERA_H
#define OPENUG2_CAMERA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_BAU_PER_TURN 65536.0f
#define CAM_BAU_TO_RAD(b) ((float)(b) * (6.28318530718f / CAM_BAU_PER_TURN))

/* ---- views (SPEC 2.1) --------------------------------------------------- */
enum { CAM_FAR = 0, CAM_CLOSE, CAM_BUMPER, CAM_DRIVER, CAM_HOOD, CAM_DRIFT, CAM_NVIEW };
static inline int cam_is_chase(int view) {
    return view == CAM_FAR || view == CAM_CLOSE || view == CAM_DRIFT;
}
const char *cam_view_name(int view);

/* Stored camera option (SPEC 2.2). */
enum { CAM_OPT_BUMPER = 0, CAM_OPT_HOOD, CAM_OPT_CLOSE, CAM_OPT_FAR, CAM_OPT_DRIFT, CAM_NOPT };
#define CAM_OPT_DEFAULT CAM_OPT_FAR
int         cam_view_for_option(int opt);          /* any other value -> FAR */
const char *cam_option_label(int opt);             /* English label */
/* In-race "change camera" (SPEC 2.3). dir > 0 next, dir < 0 previous. */
int cam_option_cycle(int opt, int dir, int drift_allowed);
/* Options screen: 0..3 with wrap-around. */
int cam_option_menu_step(int opt, int dir);
/* Event set-up (SPEC 2.3): drift sets DRIFT; any other event leaves 4 -> 3. */
int cam_option_for_event(int opt, int drift_event);
int cam_drift_allowed(int drift_event, int event_id);

/* Per-player stored option, persisted in a small text file (docs/CAMERA.md D3). */
#define CAM_PLAYERS 2
typedef struct { int opt[CAM_PLAYERS]; int mouse_look; } CamSettings;
void cam_settings_defaults(CamSettings *s);
/* Default location: $OPENUG2_CONFIG_DIR, else $XDG_CONFIG_HOME/openug2, else
 * $HOME/.config/openug2, file camera.cfg. Returns 0 if no location is known. */
int  cam_settings_path(char *out, size_t cap);
void cam_mkdir_parents(const char *path);   /* create the folders above a config file */
int  cam_settings_load(CamSettings *s, const char *path); /* 1 = read; defaults otherwise */
int  cam_settings_save(const CamSettings *s, const char *path);

/* ---- per-car data (SPEC 3) ---------------------------------------------- */
#define CAM_SET_MAX 6
typedef struct { int tag, pitch_bau; float fwd, up, lat; } CamEntry;
typedef struct {
    int valid;            /* 1 = read from a GLOBALB record, 0 = built-in default */
    int count[2];         /* primary, secondary (split-screen) */
    CamEntry e[2][CAM_SET_MAX];
} CamCarData;
/* Parse the record named `car` from a decompressed GLOBALB image. 0 on failure
 * (car absent, ambiguous table, implausible counts); *out is then untouched. */
int  cam_car_data_load(const unsigned char *global, long glen, const char *car, CamCarData *out);
/* Built-in stand-in used when a car has no record (D1). */
void cam_car_data_default(CamCarData *out);
/* First entry in the set whose tag matches, or NULL. */
const CamEntry *cam_car_entry(const CamCarData *d, int set, int view);

/* ---- built-in tuning (SPEC 4) ------------------------------------------- */
typedef struct {
    float t_eye, t_look, t_fov, t_up;
    float up_k[3], up_min[3], up_max[3];
    float eye_k[3], eye_min[3], eye_max[3];
    float look_k[3], look_min[3], look_max[3];
    float t_head[3];
    int   fov_bau;
} CamTuning;
const CamTuning *cam_tuning(int view);

/* ---- smoother (SPEC 5) --------------------------------------------------- */
typedef struct { float p, v, target, T; int active; } CamSmooth;
void cam_smooth_set_target(CamSmooth *s, float x);
void cam_smooth_snap(CamSmooth *s);
void cam_smooth_step(CamSmooth *s, float dt);

/* ---- driving camera (SPEC 6) --------------------------------------------- */
typedef struct { float pos[3], fwd[3], left[3], up[3]; } CamCarFrame;
/* Orthonormal car axes from a heading (radians about +Z) and an up vector. */
void cam_car_axes(float heading, const float up[3], CamCarFrame *f);

/* Optional veto for in-car views (SPEC 6.1 conditions 2 and 4). `eye` is the
 * would-be in-car eye in world space, `lo`/`hi` the car origin and the point
 * 2 m along its up axis. Nonzero = fall back to FAR. */
typedef int (*CamBlockedFn)(void *ctx, const float eye[3], const float lo[3], const float hi[3]);

typedef struct {
    CamCarFrame car;      /* body frame used for in-car views and projections */
    float vel[3];         /* world velocity, m/s */
    float speed;          /* |car speed|, m/s */
    float accel[3];       /* world linear acceleration, m/s^2 */
    int   wheels_down;    /* wheels with ground contact (0..4) */
    double clock;         /* game clock, s */
    int   drift_event;    /* current event is a drift event */
    int   event_e11;      /* SPEC 13.2: unidentified special event type */
    int   vehicle_s2;     /* SPEC 13.3: unidentified vehicle state */
    int   split_screen;   /* use the secondary camera set */
    int   accel_offsets_disabled; /* SPEC 6.8 per-player flag */
    CamBlockedFn blocked; void *blocked_ctx;
} CamInput;

typedef struct {
    int requested, effective;
    int started, snap;
    int look_back, variant_f;   /* variant F is unused in retail (SPEC 6.13) */
    CamCarData car;
    CamSmooth eye[3], look[3], up[3], fov, head[3];
    float acc_hist[4][3]; int acc_n, acc_i;
    double t_ground;
    int airborne;
} CamState;

void cam_init(CamState *s);
void cam_set_car(CamState *s, const CamCarData *d);      /* also requests a cut */
void cam_set_requested(CamState *s, int view);            /* a change requests a cut */
void cam_request_snap(CamState *s);
void cam_set_look_back(CamState *s, int on);
/* One camera update step of dt seconds (SPEC 6.1 - 6.8, 6.12). */
void cam_tick(CamState *s, const CamInput *in, float dt);

/* Pieces of cam_tick exposed for the test vectors. */
float cam_heading_weight(float speed);
/* Base eye offset (fwd, lat, up) and look-at (SPEC 6.7) for an entry. */
void  cam_base_offsets(int view, int pitch_bau, float fwd, float lat, float up,
                       float off[3], float look[3]);
/* SPEC 6.8. `aL` is the acceleration in the heading frame, `acc_world` the same
 * vector in world space (used by the up channels). */
void  cam_targets(const CamTuning *k, int chase, int airborne, const float off[3],
                  const float look[3], const float aL[3], const float acc_world[3],
                  float eye_t[3], float look_t[3], float up_t[3]);

typedef struct {
    float eye[3], look[3], up[3];   /* world */
    int   fov_bau;
    int   view;                     /* effective view */
    int   hide_car;                 /* SPEC 10 */
    float znear;
} CamPose;
/* SPEC 6.14: final eye / look-at / up for a car pose (may be an interpolated
 * render pose, not the tick pose). Shake and collision come afterwards. */
void cam_pose(const CamState *s, const CamCarFrame *car, CamPose *out);
/* Display-only interpolation; snapshots bracket the same tick as the car pose.
 * Cuts and exact current-tick captures bypass interpolation. */
void cam_render_pose(const CamState *before, const CamState *after,
                     const CamCarFrame *car, float alpha, CamPose *out);

/* Optional OpenUG2 mouse look; anchors the displayed pose to the moving car. */
typedef struct { CamPose local; float yaw, pitch; } CamMouseLook;
void cam_mouse_look_begin(CamMouseLook *s, const CamPose *display, const CamCarFrame *car);
void cam_mouse_look_move(CamMouseLook *s, float dx, float dy); /* radians */
void cam_mouse_look_pose(const CamMouseLook *s, const CamCarFrame *car, CamPose *out);

/* ---- projection (SPEC 7) -------------------------------------------------- */
/* Policy (SPEC 7.1, OpenUG2 addition): ORIGINAL is SPEC 7 with the real viewport;
 * WIDE (project default) keeps the 4:3 result as reference and widens the horizontal
 * view (Hor+) when the viewport is wider than 4:3. Selected by --projection. */
enum { CAM_PROJ_ORIGINAL = 0, CAM_PROJ_WIDE = 1 };
#define CAM_PROJ_DEFAULT CAM_PROJ_WIDE
int  cam_projection_policy_by_name(const char *name);   /* "original"/"wide"; -1 if unknown */
void cam_projection_scales(int policy, int fov_bau, int w, int h, int main_view, float *xs, float *ys);
void cam_projection(int policy, int fov_bau, int w, int h, int main_view, float znear, float zfar, float m[16]);
/* Replace the up-vector of a lookAt: view matrix, column-major, GL convention. */
void cam_view_matrix(const float eye[3], const float look[3], const float up[3], float m[16]);

/* ---- shake (SPEC 8) --------------------------------------------------------- */
enum {
    CAM_SURF_NONE = 0, CAM_SURF_ROAD, CAM_SURF_ROAD_WET, CAM_SURF_ROAD_DRIFT,
    CAM_SURF_ROAD_SMOKE_1, CAM_SURF_ROAD_SMOKE_2, CAM_SURF_ROAD_SMOKE_3, CAM_SURF_ROAD_BRIDGE,
    CAM_SURF_DIRT, CAM_SURF_GRAVEL, CAM_SURF_ROUGH_ROAD, CAM_SURF_COBBLESTONE, CAM_SURF_STAIRS,
    CAM_SURF_PUDDLE, CAM_SURF_DEEP_WATER, CAM_SURF_GRASS, CAM_SURF_SIDEWALK, CAM_SURF_WOOD,
    CAM_SURF_PLASTIC, CAM_SURF_GLASS, CAM_SURF_SOLID_WALL, CAM_SURF_SEE_THROUGH_WALL,
    CAM_SURF_PLANT, CAM_SURF_POST, CAM_SURF_PILLAR, CAM_SURF_METAL_GRATE, CAM_SURF_METAL,
    CAM_SURF_CHAINLINK, CAM_SURF_CAR, CAM_NSURF
};
void        cam_surface_params(int surf, float *fq, float *rough);
const char *cam_surface_name(int surf);
int         cam_surface_by_name(const char *name);   /* -1 if unknown */

double cam_lattice(int32_t i);
double cam_noise(double x);
double cam_fbm(double x);

typedef struct { float freq1[4], amp1[4], freq2[4], amp2[4]; } CamShakeParams;
/* `stopped`: car effectively stopped (speed below ~2.2 m/s). */
void cam_shake_params(float speed, float vmax, const int surf[4], int drag_event,
                      int nitrous, int stopped, CamShakeParams *out);
/* c[0],c[1]: metres along camera x (right) / y (down); c[2],c[3]: degrees about
 * camera x (pitch) / y (yaw) before FOV rescale. */
void cam_shake_values(const CamShakeParams *p, double t, float c[4]);
/* Rescale an angle (degrees) for the FOV (SPEC 8), BAU truncation included.
 * Returns radians. */
float cam_shake_angle(float deg, int fov_bau);
/* Left-multiply `view` (GL view matrix) by the shake transform. */
void cam_shake_apply(const float c[4], int fov_bau, float view[16]);

/* ---- collision helpers (SPEC 9, 6.1) ---------------------------------------- */
/* Raise `eye` over the sphere (c, r + margin). Returns 1 if it moved. */
int cam_raise_over_sphere(float eye[3], const float c[3], float r, float margin);
/* Distance test of a point to an upright box centred (x,y) with heading, half
 * extents hl/hw (along/across) and z range [z0,z1]; true if within `margin`. */
int cam_point_near_box(const float p[3], const float box_pos[3], float heading,
                       float hl, float hw, float height, float margin);

#ifdef __cplusplus
}
#endif
#endif
