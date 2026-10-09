/* ai.h — shared source roads, free-roam traffic and physics-driven racers. */
#ifndef OPENUG2_AI_H
#define OPENUG2_AI_H

#include "nfsu2.h"
#include "physics.h"
#include "debug.h"

#define N_AI 4
#define N_RACE_AI 5
#define N_TRAFFIC_MAX 16
#define N_ROAM_VISUALS (N_AI + 2) /* reuse four traffic models and two racer models */
#define N_OPENWORLD_AI (N_TRAFFIC_MAX + 2)
#define N_WORLD_AI (N_OPENWORLD_AI + N_RACE_AI)
/* Keep the original racer slots stable as traffic density changes. */
static inline int ai_traffic_is_racer(int k) { return k>=N_AI && k<N_ROAM_VISUALS; }
static inline int ai_traffic_visual(int k) { return k<N_ROAM_VISUALS?k:k%N_AI; }
static inline int ai_event_visual(int k) { return N_AI+k%(N_ROAM_VISUALS-N_AI); }
/* an AI racer following the racing line */
struct AiCar {
    float pos[3], head, spd, wheel_angle, turn_rate, col[3];
    float half_length, half_width, height;
    float mass; /* tonnes from GLOBALB when available; zero uses neutral mass */
    int t, lap, prevrel, braking;
    float vel[2], steer, target_speed;
    PhysVehicle vehicle;
    N2PhysicsAttr powertrain;
    int has_powertrain;float tyre_radius;
    PhysRideState ride;
    PhysRideSupport support;
    int ride_ready;
    int render_visible; /* renderer's last conservative frustum test; never simulation input */
};

/* Render-only pose between the last two ticks; never feed it to simulation.
   before.wheel_angle must be unwrapped relative to after for this tick. */
void ai_render_pose(AiCar *out,const AiCar *before,const AiCar *after,float alpha);

/* Road nodes read from the game's shared RoutesFreeRoam.bin files. A record's
 * points are ordered; next[] joins consecutive points within that record. */
typedef struct {
    float *xy; int *next, n; float *half_width;
    /* Optional query index built by ai_roads_index (zero = linear scans):
       predecessors per node and a uniform node grid. Never changes results. */
    int *pred_start, *pred_list, *cell_start, *cell_list, gw, gh;
    float gx0, gy0;
} AiRoadNet;
int ai_roads_index(AiRoadNet *roads);
int ai_roads_load(AiRoadNet *roads, const char *tracks_root);
void ai_roads_free(AiRoadNet *roads);

/* Free-roam cars follow the authored road paths, independently of circuits. */
typedef struct {
    int prev, from, to, after;
    float along, travelled, cruise_speed;
    int stop_reason, present;
    unsigned respawns, wait_ticks;
    float lane_offset, max_impact;
    unsigned air_ticks;
    /* Driver preview is stable within one immutable resident and body size. */
    const N2Mesh *preview_scene;
    float preview_body[3], preview_z[8];
    int preview_from[8], preview_to[8], preview_after[8];
    unsigned preview_valid;
} AiTraffic;
typedef struct {
    N2Scene *scene;
    const float (*obst)[4], (*obstz)[2];
    const int *obstsrc;
    int nobst;
    float eye[3], view[2];
    int traffic_target; /* 0..N_TRAFFIC_MAX, roaming racers are independent */
    int ambient_only; /* road races retain traffic, without free-roam rivals */
    float race_pace; /* km/h; zero retains the conservative diagnostic pace */
    const AiCar *ambient;
    const AiTraffic *ambient_routes;
    int ambient_count;
} AiTrafficWorld;
static inline int ai_actor_slot(int k,int racing) { return racing?k-N_RACE_AI:k; }
static inline int ai_actor_present(int k,int racing,int racers,const AiTraffic *routes) {
    if(racing && k>=0 && k<racers)return 1;
    int slot=ai_actor_slot(k,racing);
    return slot>=0 && slot<N_OPENWORLD_AI && routes[slot].present;
}
static inline int ai_actor_visual(int k,int racing,int racers,int event) {
    if(racing && k<racers)return event?ai_event_visual(k):ai_traffic_visual(k);
    int slot=ai_actor_slot(k,racing);return ai_traffic_visual(slot>=0?slot:0);
}
/* Next supported, collision-clear node; NULL car uses the default body size. */
int ai_road_next(const AiRoadNet *roads,const AiTrafficWorld *world,const AiCar *car,
                 int at,int previous,float z);
/* Optional read-only observer for behaviour audits; NULL in production. Called
 * once per free-roam simulation tick after contacts and population update. */
typedef void (*AiAuditHook)(const AiCar cars[N_OPENWORLD_AI],
                            const AiTraffic routes[N_OPENWORLD_AI], int n,
                            const AiRoadNet *roads, const AiTrafficWorld *world,
                            const AiCar *player, long tick);
extern AiAuditHook g_ai_audit_hook;
/* Optional observer of actual position corrections; never changes the solver.
 * kind 0: world walls/rails, kind 1: vehicle separation. */
typedef void (*AiContactHook)(const float before[3],const float after[3],int kind);
extern AiContactHook g_ai_contact_hook;
/* Work counters for the optional frame profiler; the caller resets them. */
typedef struct { long passes, pairs, hits, world_fixes, wall_candidates; } AiPerf;
extern AiPerf g_ai_perf;
/* Resolve all live player/AI contacts together; return the player's hit level.
   Pass each car once. player may be NULL for AI-only simulation. */
float ai_car_contacts(AiCar *const cars[], int count, const AiTrafficWorld *world,
                       const AiCar *player);
int ai_traffic_offscreen(const float eye[3], const float view[2],
                         const float pos[3]);
/* Reset/spawn the requested traffic plus two rivals. Returns a slot range,
 * not a live count: inspect routes[k].present when iterating it. */
int ai_traffic_spawn(const AiRoadNet *roads, const AiTrafficWorld *world,
                     AiCar cars[N_OPENWORLD_AI],
                     AiTraffic routes[N_OPENWORLD_AI], const float player[3],
                     float player_heading);
int ai_traffic_respawn(const AiRoadNet *roads, const AiTrafficWorld *world,
                       AiCar cars[N_OPENWORLD_AI],
                       AiTraffic routes[N_OPENWORLD_AI], int k, const float player[3],
                       float player_heading);
/* Reconcile density without removing visible cars. At most one spawn attempt
 * per call; call periodically. Returns the slot range to iterate (holes allowed). */
int ai_traffic_update(const AiRoadNet *roads,const AiTrafficWorld *world,
                       AiCar cars[N_OPENWORLD_AI],AiTraffic routes[N_OPENWORLD_AI],
                       const float player[3],float heading);
void ai_traffic_follow(AiCar cars[N_OPENWORLD_AI], const AiTraffic routes[N_OPENWORLD_AI],
                       const AiRoadNet *roads, int k, int count, const AiCar *player);
void ai_traffic_step(AiCar *car, AiTraffic *route, const AiRoadNet *roads,
                     const AiTrafficWorld *world);

/* (Re)load a racing-line circuit and grid the AI cars on it. Returns the AI
 * count (0 if the file has no usable loop). Frees any previously loaded path,
 * so it is safe to call repeatedly (e.g. when the menu switches circuit).
 * The player spawns at (cx,cy) — the densest built-up spot — facing the line. */
int load_circuit(const char *dataroot, const char *circuit, N2Scene *scene,
                 N2Path *aipath, AiCar *ais, float spawn[3],
                 float *heading0, int *start_idx, float cx, float cy);

/* Use the same shipped circuit line for free-roam rivals without moving or
 * rotating the player. Returns zero in districts with no usable loop. */
int load_roaming_circuit(const char *dataroot, const char *circuit, N2Scene *scene,
                         N2Path *aipath, AiCar *ais, const float player[3],
                         int *start_idx);

/* One tick for one AI: steer toward the next waypoint, pace for the bend
 * ahead, rubber-band toward player_prog (monotonic lap*n+rel progress),
 * follow the ground, count laps. k staggers per-car top speed. */
void ai_step(AiCar *ai, int k, const N2Path *aipath, N2Scene *scene,
             int start_idx, int player_prog);

/* Input-only driver. Borrows a validated open/closed XY polyline; never writes
 * the car pose/velocity or queries/snaps its ground height. Speeds: m/tick. */
typedef struct {
    const N2Path *path;
    int segment, direction, stalled, finished, failed, loop, turns;
    float start, progress, length, checkpoint, error, target[2], target_heading, target_kmh, error_limit;
} AiDrive;
typedef struct { float throttle, steer; int handbrake; } AiDriveInput;
int ai_drive_route_valid(const N2Path *path);
int ai_drive_init(AiDrive *drive, const N2Path *path, const float pos[3], float heading);
/* Authored corridor bound, with normal physical support/collision still required. */
int ai_drive_join(AiDrive *drive,const N2Path *path,const float pos[3],float heading,float corridor);
AiDriveInput ai_drive_step(AiDrive *drive, const float pos[3], float heading, float speed);

/* Event courses come from source lane distances; raw-list jumps are rejected. */
int ai_race_course(const char *tracks_root,const WEvent *event,N2Path *out);
float ai_course_heading(const N2Path *path,const float pos[3]);
float ai_course_lateral(const N2Path *path,const float pos[3]);
float ai_drag_lane_target(const AiTrafficWorld *world,const AiCar *car,const AiDrive *line,
        float current,const float *lanes,int count,int direction);
typedef struct { WRace progress; AiDrive drive; PhysManual gearbox; float offset,finish_at; int blocked,finishing; } AiRace;
/* Returns opponent count. Solo downhill returns zero with its player course
   and a borrowed player driver in drivers[0] for the physics checker. */
int ai_race_prepare(const char *tracks_root,const WEvent *event,WRace *race,
        const AiTrafficWorld *world,N2Path *path,AiCar cars[N_RACE_AI],AiRace drivers[N_RACE_AI],
        const AiCar *player);
void ai_race_step(AiCar cars[N_RACE_AI],AiRace drivers[N_RACE_AI],int k,int count,
        int circuit,const AiTrafficWorld *world,const AiCar *player);

#endif
