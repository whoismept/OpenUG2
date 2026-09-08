/* ai.h — OpenUG2 AI module: opponents that follow the game's own racing line
 * (the ROUTES Paths .bin waypoints), with corner-aware pacing and mild
 * rubber-banding toward the player. Also loads/grids a circuit. */
#ifndef OPENUG2_AI_H
#define OPENUG2_AI_H

#include "nfsu2.h"

#define N_AI 4
/* an AI racer following the racing line */
typedef struct { float pos[3], head, spd, col[3]; int t, lap, prevrel; } AiCar;

/* (Re)load a racing-line circuit and grid the AI cars on it. Returns the AI
 * count (0 if the file has no usable loop). Frees any previously loaded path,
 * so it is safe to call repeatedly (e.g. when the menu switches circuit).
 * The player spawns at (cx,cy) — the densest built-up spot — facing the line. */
int load_circuit(const char *dataroot, const char *circuit, N2Scene *scene,
                 N2Path *aipath, AiCar *ais, float spawn[3],
                 float *heading0, int *start_idx, float cx, float cy);

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
