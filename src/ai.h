/* ai.h — OpenUG2 AI module: opponents that follow the game's own racing line
 * (the ROUTES Paths .bin waypoints), with corner-aware pacing and mild
 * rubber-banding toward the player. Also loads/grids a circuit. */
#ifndef OPENUG2_AI_H
#define OPENUG2_AI_H

#include "nfsu2.h"
#include "physics.h"

#define N_AI 4
#define N_OPENWORLD_AI (N_AI + 2) /* four traffic cars, two faster roaming racers */
/* an AI racer following the racing line */
struct AiCar {
    float pos[3], head, spd, wheel_angle, turn_rate, col[3];
    float half_length, half_width, height;
    int t, lap, prevrel, braking;
    float vel[2], steer, target_speed;
    PhysVehicle vehicle;
    PhysRideState ride;
    PhysRideSupport support;
    int ride_ready;
};

/* Road nodes read from the game's shared RoutesFreeRoam.bin files. A record's
 * points are ordered; next[] joins consecutive points within that record. */
typedef struct { float *xy; int *next, n; float *half_width; } AiRoadNet;
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
} AiTraffic;
typedef struct {
    N2Scene *scene;
    const float (*obst)[4], (*obstz)[2];
    const int *obstsrc;
    int nobst;
    float eye[3], view[2];
} AiTrafficWorld;
int ai_traffic_offscreen(const float eye[3], const float view[2],
                         const float pos[3]);
int ai_traffic_spawn(const AiRoadNet *roads, const AiTrafficWorld *world,
                     AiCar cars[N_OPENWORLD_AI],
                     AiTraffic routes[N_OPENWORLD_AI], const float player[3],
                     float player_heading);
int ai_traffic_respawn(const AiRoadNet *roads, const AiTrafficWorld *world,
                       AiCar cars[N_OPENWORLD_AI],
                       AiTraffic routes[N_OPENWORLD_AI], int k, const float player[3],
                       float player_heading);
void ai_traffic_follow(AiCar cars[N_OPENWORLD_AI], const AiTraffic routes[N_OPENWORLD_AI],
                       const AiRoadNet *roads, int k, int count);
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

/* Input-only test driver. Borrows a validated, open XY polyline; never writes
 * the car pose/velocity or queries/snaps its ground height. Speeds: m/tick. */
typedef struct {
    const N2Path *path;
    int segment, direction, stalled, finished, failed;
    float start, progress, length, checkpoint, error, target[2], target_kmh;
} AiDrive;
typedef struct { float throttle, steer; int handbrake; } AiDriveInput;
int ai_drive_route_valid(const N2Path *path);
int ai_drive_init(AiDrive *drive, const N2Path *path, const float pos[3], float heading);
AiDriveInput ai_drive_step(AiDrive *drive, const float pos[3], float heading, float speed);

#endif
