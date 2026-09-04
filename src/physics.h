/* physics.h — OpenUG2 Physics module: arcade car kinematics (heading-frame
 * velocity with lateral tyre scrub → drift), wall AABB collision, and
 * car-to-car circle separation. No GL, no SDL, no file IO. */
#ifndef OPENUG2_PHYSICS_H
#define OPENUG2_PHYSICS_H

#include "nfsu2.h"
#include "ai.h"

/* driving constants — car length axis = local X; world +Z up.
 * Real units: world coordinates are metres, physics ticks at 60 Hz, so
 * speeds are metres/tick. Tuned to NFSU2 driving: ~220 km/h top speed,
 * 0-100 km/h in ~4 s, ~100-0 braking in ~3 s, long pull to top speed. */
#define PHYS_TICKRATE 60.0f
#define PHYS_MAXSPD   (61.0f/PHYS_TICKRATE)   /* 220 km/h cap (m/tick) */
#define PHYS_ACCEL    (7.0f/(PHYS_TICKRATE*PHYS_TICKRATE)) /* 7 m/s^2 peak thrust */
#define PHYS_FRICTION 0.99886f /* rolling+air drag; equilibrium lands at MAXSPD */
#define PHYS_TURN     0.024f   /* full-lock yaw gain; ~105 deg/2s at 50 km/h */
#define PHYS_GRIP     0.86f    /* lateral scrub per tick (lower = grippier) */
/* km/h for the HUD from a m/tick forward speed */
#define PHYS_KMH(v)   ((v) * PHYS_TICKRATE * 3.6f)

/* Live handling tuning (ImGui sliders). accel/brake/turn are multipliers on the
 * constants above (1.0 = stock); top_kmh is a hard forward-speed cap. Defaults
 * reproduce the tuned NFSU2 feel exactly, so phys_selftest still holds. */
typedef struct { float accel, brake, turn, top_kmh; } PhysTune;
extern PhysTune g_phys_tune;

/* One tick of car kinematics: throttle in [-1..1] along the heading, steering
 * in [-1..1] rotates it, tyres scrub the sideways velocity (handbrake keeps
 * it, so the car slides), position integrates. Returns the post-grip lateral
 * speed magnitude — the drift signal for skid marks / smoke / screech. */
/* What the surface under the car does to the arcade model. Multipliers on the
 * constants above, except `drag` and `lat` which REPLACE PHYS_FRICTION and
 * PHYS_GRIP outright. The road profile is exactly the tuned NFSU2 feel, so
 * passing it (or NULL) reproduces the previous behaviour bit for bit. */
typedef struct {
    float accel;    /* thrust multiplier */
    float topfrac;  /* fraction of the selected top speed the surface allows */
    float drag;     /* per-tick velocity retention (replaces PHYS_FRICTION) */
    float lat;      /* lateral retention per tick (replaces PHYS_GRIP) */
    float steer;    /* steering-authority multiplier */
} PhysSurface;
extern const PhysSurface PHYS_SURF_ROAD;
extern const PhysSurface PHYS_SURF_TERRAIN;

/* What the CAR's own measured geometry does to the arcade model, as bounded
 * multipliers on the constants above. Built by phys_vehicle_from_geometry();
 * NULL is a neutral car (all 1.0), which is what the self-tests use. */
typedef struct {
    float accel;   /* thrust multiplier      (mass proxy: body volume)      */
    float brake;   /* braking multiplier     (same proxy)                   */
    float steer;   /* steering authority     (axle wheelbase)               */
    float lat;     /* lateral-retention mult (tyre width, track width)      */
} PhysVehicle;

/* Fleet medians measured over all 44 drivable cars (--fleet-census, M121).
 * They are the normalisation basis, not per-car values. */
#define PHYS_FLEET_VOLUME   11.8908f   /* m^3, body AABB L*W*H */
#define PHYS_FLEET_WHEELBASE 2.7896f   /* m, front axle - rear axle */
#define PHYS_FLEET_TRACK     1.5004f   /* m, front track */
#define PHYS_FLEET_TYREW     0.2243f   /* m, tyre width */

/* Derive one car's profile from measurements only: body volume as the mass and
 * inertia proxy, axle wheelbase for steering authority, tyre and track width for
 * lateral grip. Every factor is clamped, so a bus cannot invert the model. */
PhysVehicle phys_vehicle_from_geometry(float body_len, float body_wid, float body_hgt,
                                       float wheelbase, float track, float tyre_w);

/* Keyboard/gamepad steering response: fast enough to catch a corner, gradual
 * enough that one A/D frame cannot command instant full lock. */
float phys_steer_response(float current, float target);

/* ---- sprung ride / four-wheel contact (M130) --------------------------------
 * The player used to be pinned to one centre triangle: carpos[2] = gz every
 * frame. That copied every seam exactly, teleported the car 12.96 m when the
 * selected layer changed on L4RB, and made airborne motion impossible.
 *
 * This is the replacement foundation: four independent wheel contacts drive one
 * sprung body with heave, pitch and roll. It is deliberately pure -- no scene,
 * no GL, no SDL, no hidden statics -- so the world query and the integrator can
 * be tested apart.
 *
 * UNITS. Everything here is metres, seconds and radians, and dt is passed in
 * explicitly. The horizontal arcade model above uses m/tick; do NOT mix them.
 */

/* Travel and response. Justified by the acceptance targets rather than taste:
 *   PHYS_RIDE_FREQ / ZETA give a settle time of 4/(zeta*omega) = 0.32 s, so a
 *   0.25 m displacement is inside 2 cm well within the 1.5 s budget, and a
 *   0.20 m step cannot be absorbed instantly because BUMP caps the compression
 *   the spring can see in one frame. */
#define PHYS_RIDE_BUMP     0.12f   /* max compression, m                        */
#define PHYS_RIDE_DROOP    0.18f   /* max extension before the wheel hangs, m   */

/* CONTACT REACH (M130-R). This is the window in which a covering triangle is a
 * suspension CONTACT. It is not a search band: the world query may look as far
 * as it likes for attribution, but only a candidate inside this window may set
 * valid[k], carry spring force, or seed the initial pose.
 *
 * UP is bounded by what the wheel can physically climb, in centimetres:
 * BUMP (0.12 m) is what the spring absorbs, and the remaining 0.13 m is tyre
 * roll-over -- still under half the smallest fleet tyre radius (~0.30 m), so it
 * is a lip a tyre can mount rather than a wall. Cross-checked against speed: at
 * the 220 km/h cap the car covers 61.1/60 = 1.018 m per frame, so 0.25 m is a
 * 24.6% grade (13.8 deg) -- steeper than any drivable authored road, and the
 * allowance only grows as a share of the frame step as the car slows.
 *
 * DOWN is suspension droop, plus the distance the wheel actually falls during
 * the frame (phys_ride_reach_down). The swept term is not a widened band: it is
 * the segment the contact point sweeps, so a fast landing cannot tunnel through
 * a floor between two samples. At the 35 m/s worst case measured on L4RA it is
 * 0.58 m; at rest it is exactly DROOP.
 *
 * There is deliberately NO multi-metre reach and no recovery lift. A body under
 * a deck stays under it and remains observable; climbing out is a respawn or
 * collision problem, not a suspension one. */
#define PHYS_RIDE_REACH_UP    0.25f   /* bump travel + tyre roll-over, m       */
#define PHYS_RIDE_REACH_DOWN  PHYS_RIDE_DROOP  /* static part; see reach_down  */
/* Penetration guard bound: one frame can push a valid contact at most this far
 * past full bump, so the correction can never exceed the travel that produced
 * it. Applies to reachable contacts only. */
#define PHYS_RIDE_PEN_MAX  (PHYS_RIDE_BUMP + 0.001f)
/* Largest per-wheel RESIDUAL a fitted spawn plane may leave. Judged on the
 * residual rather than the raw spread, because a car on an 8% grade
 * legitimately sees 0.25 m across a 3.1 m wheelbase and that IS one plane,
 * while a wheel on another layer leaves an error no plane can absorb. */
#define PHYS_RIDE_PLANE_SPREAD (PHYS_RIDE_BUMP + PHYS_RIDE_DROOP)
#define PHYS_RIDE_FREQ     2.25f   /* undamped natural frequency, Hz            */
#define PHYS_RIDE_ZETA     0.90f   /* damping ratio: near-critical at rest      */
#define PHYS_RIDE_G        9.81f   /* m/s^2                                     */
#define PHYS_RIDE_MAXTILT  0.35f   /* pitch/roll clamp, rad (~20 deg)           */

typedef struct {
    float z, vz;                 /* sprung body reference plane, m and m/s      */
    float pitch, pitch_rate;     /* + = nose up,     rad, rad/s                 */
    float roll,  roll_rate;      /* + = left side up, rad, rad/s                */
    float compression[4];        /* m, + = compressed, - = drooping             */
    unsigned contact_mask;       /* bit k set = wheel k has reachable support   */
    int   air_frames;            /* consecutive frames with no contact at all   */
    float impact;                /* |vz| at the frame contact was regained, m/s */
    float lift;                  /* penetration guard applied this frame, m     */
} PhysRideState;

/* One frame of support, gathered by the caller from the world. ax/ay are the
 * wheel offsets in body space (+x forward, +y left); the integrator re-centres
 * them on their own centroid so a car at rest generates exactly zero torque. */
typedef struct {
    float z[4];      /* world support height under each wheel, m */
    int   valid[4];  /* 1 = reachable support this frame          */
    float ax[4], ay[4];
} PhysRideSupport;

/* Static equilibrium on the given support. Solves heave, pitch and roll from
 * the valid wheel heights (least squares on the re-centred offsets), so a car
 * spawned on a slope starts ON that slope with zero residual, zero velocity,
 * zero impact and no first-frame bump-stop correction. Needs three coherent
 * contacts: with fewer, or with a support spread wider than
 * PHYS_RIDE_PLANE_SPREAD (unrelated stacked layers), no plane is invented and
 * the pose starts level. With no valid support it starts airborne. */
void phys_ride_init(PhysRideState *r, const PhysRideSupport *s);
/* Downward contact reach for the NEXT gather: droop plus the distance the body
 * falls during one step, so a fast landing cannot tunnel past a floor. */
float phys_ride_reach_down(const PhysRideState *r, float dt);
/* Advance one fixed step. dt in seconds (the game passes 1.0f/60.0f). */
void phys_ride_step(PhysRideState *r, const PhysRideSupport *s, float dt);
/* World Z of wheel k's contact point under the current body pose. */
float phys_ride_wheel_z(const PhysRideState *r, const PhysRideSupport *s, int k);

/* sf == NULL is the road profile; vh == NULL is a neutral car. */
float phys_car_step(float pos[3], float vel[2], float *heading, float *speed,
                    float throttle, float steer, int handbrake,
                    const PhysSurface *sf, const PhysVehicle *vh);

/* Push (pos.xy) out of any wall AABB (expanded by r) it penetrates, along the
 * least-penetration axis; zero the into-wall velocity so the car slides along
 * the face. Returns the number of walls resolved.
 *
 * obz is the per-obstacle {z0,z1} span from the same phys_collect_walls pass,
 * and [cz0,cz1] is the car's own world-space vertical extent: an obstacle whose
 * Z span does not overlap the car's is skipped, so a deck or overhang the car
 * is under (or over) no longer blocks it in XY (M94: a 6.1 m slab 5.3 m ABOVE
 * the car produced 68 responses). Pass obz = NULL to disable the Z test and get
 * the original XY-only behaviour.
 *
 * scene/src (optional, NULL to skip) turn the rect into a broad phase only: a
 * hit is confirmed against the SOURCE MESH's own geometry -- some near-vertical
 * face clipped to the car's Z interval and then within r in XY. Testing just
 * the face's Z bounds is insufficient: an upper edge can project near the car
 * even when the face at car height is metres away. The stored rect is the
 * mesh's full XY extent at every height, which is not the building's footprint
 * (measured: XB_HTECHTOWERQ_1B_00 occupies 37% of its 40x50 m rect, and its
 * nearest wall face to the pinned car was 5.6 m away and 42 m up). Same
 * near-vertical criterion world_wall_push already uses; no class, name or size
 * is consulted, so every obstacle is treated identically. */
/* What the narrow phase actually touched (M131). The old code returned a bare
 * boolean and then resolved against the mesh's expanded AABB, which is why an
 * X-facing inner wall of a concave building pushed the car 4.300 m along +Y:
 * the rect's least-penetration axis has nothing to do with the face that was
 * hit. This record carries the feature itself, so the response is local. */
typedef struct {
    int   mesh, tri;      /* source mesh and triangle that owns the feature   */
    float cx, cy;         /* closest point on that feature, XY                */
    float nx, ny;         /* unit XY normal, closest point -> car centre      */
    float dist;           /* XY distance from the car centre to it            */
    float pen;            /* r - dist, the depth to resolve                   */
    float span;           /* union vertical span of this mesh's contacting
                             faces: a 0.10 m seam is not a wall              */
} PhysWallContact;

/* Minimum vertical span, measured. Census over the shipped collision meshes
 * (--face-census): near-vertical faces under 0.20 m are 2.03% of L4RA's 255678
 * and 1.83% of L4RB's 70061, and the cumulative share below 0.30 m is 3.45% /
 * 3.27%. 0.30 m rejects the proven 0.10 m seam with 3x margin and keeps the
 * proven 0.9 m barrier with 3x margin, leaving 96.6% of authored faces
 * untouched. The test is applied to the UNION of the contacting faces of one
 * mesh, not to a single triangle, so a tessellated wall built from short strips
 * still spans its true height. No asset name or class is consulted. */
#define WALL_MIN_FACE_SPAN 0.30f

/* Narrow phase. Returns 1 and fills *out (may be NULL) with the CLOSEST
 * contacted feature on mesh mi, 0 if that mesh presents no wall here. */
int cw_probe_contact(const N2Scene *s, int mi, float px, float py,
                     float r, float cz0, float cz1);
int cw_mesh_feature(const N2Scene *s, int mi, float px, float py,
                    float r, float cz0, float cz1, PhysWallContact *out);

int collide_walls(float *pos, float *vel, const float obst[][4],
                  const float obz[][2], int nobst, float r, float cz0, float cz1,
                  const N2Scene *scene, const int *src,
                  PhysWallContact *log, int maxlog);
void collide_walls_selftest(void);
void phys_selftest(void);   /* asserts the NFSU2 velocity tuning targets */

/* Collect building collision footprints: the 2D (XY) bounding box of every
 * tall N2_OTHER mesh. Flat props/signs/road paint are skipped so they don't
 * block the road. Returns the number of AABBs written. */
/* Same selection pass, plus an optional parallel array of the SOURCE mesh index
 * each rect came from (pass NULL to ignore). Selection semantics are unchanged;
 * src exists so a diagnostic can name the mesh behind a collision response
 * without re-implementing the predicate. */
int phys_collect_walls(const N2Scene *s, float (*obst)[4], int *src,
                       float (*obz)[2], int max);

/* Circle-separate the player from each AI and the AIs from each other.
 * Player is pushed at half weight each way; a bump scrubs a little player
 * speed. Returns the collision thud amplitude for the audio (0 = no hit). */
float phys_car_contacts(float carpos[3], float vel[2], float speed,
                        AiCar *ais, int nai);

#endif
