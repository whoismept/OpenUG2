#ifndef OPENUG2_WORLD_RESIDENT_H
#define OPENUG2_WORLD_RESIDENT_H

#include "world.h"
#include "world_mesh.h"

typedef struct {
    float resident_radius;
    float draw_radius;
    float safety_margin;
    float cell_size;
} WResidentPolicy;

typedef struct WorldResident WorldResident;

int world_resident_policy_valid(const WResidentPolicy *policy);
int world_resident_target(const WResidentPolicy *policy,
                          float player_x, float player_y,
                          float active_x, float active_y,
                          float out_center[2]);
int world_resident_route_point(const WResidentPolicy *policy,
                               const WorldResident *active,
                               const WorldCity *city,
                               float from_x, float from_y, float reference_z,
                               const float (*visited)[2], int visited_count,
                               float out_pos[3], float out_center[2]);

typedef struct {
    uint32_t *texture_keys;
    GLuint *textures;
    unsigned char *texture_modes;
    int texture_count;
    GLuint *mesh_textures;
    unsigned char *mesh_modes;
    int *mesh_batch;
    int mesh_count;
    GLuint terrain_texture;
    N2Batch *ordinary, *sky, *glow, *vista;
    int ordinary_count, sky_count, glow_count, vista_count;
    WorldMeshBatch *debug_batches;
    int *vista_mesh;
    float (*obstacles)[4];
    float (*obstacle_z)[2];
    int *obstacle_src;
    int obstacle_count;
} WorldResidentResources;

/* Optional build timing output (M151). Pass NULL to skip. */
typedef struct {
    uint32_t neighborhood_ms;  /* world_neighborhood_load */
    uint32_t validate_ms;      /* world_resident_validate_cpu */
    uint32_t textures_ms;      /* world_bind_textures + mesh tex lookup */
    uint32_t batches_ms;       /* upload_*_batches + debug_batches */
    uint32_t collision_ms;     /* phys_collect_walls */
    uint32_t total_ms;
} WResidentBuildTiming;

int world_resident_resources_build(WorldResidentResources *resources,
                                   WorldNeighborhood *neighborhood,
                                   WResidentBuildTiming *timing);
void world_resident_resources_free(WorldResidentResources *resources);

struct WorldResident {
    WorldNeighborhood world;
    WorldResidentResources resources;
    float center[2];
    float radius;
    unsigned long generation;
};

typedef struct {
    const char *track_root;
    const char *trackname;
    int scenery_event;
    int sky_profile;
    WResidentPolicy policy;
} WResidentBuildArgs;

int world_resident_validate_cpu(const WorldResident *resident,
                                float player_x, float player_y, float player_z);
int world_resident_build(WorldResident *candidate,
                         const WResidentBuildArgs *args,
                         float center_x, float center_y,
                         float player_x, float player_y, float player_z,
                         WResidentBuildTiming *timing);

/* Prepare owns CPU/file data only; candidate must be zero-initialized. Partial
 * failure is still owned by candidate. Finish runs on the GL thread and checks
 * the CURRENT player position, not a pose captured when preparation started. */
int world_resident_prepare(WorldResident *candidate, const WResidentBuildArgs *args,
                           float center_x, float center_y,
                           WResidentBuildTiming *timing);
int world_resident_finish(WorldResident *candidate, float x, float y, float z,
                          WResidentBuildTiming *timing);

/* Single-loader contract: while a job is outstanding, do not invoke another
 * neighborhood/instance load or activate/free the active ground grid. Rendering
 * and ground queries on the immutable active scene may continue. All job APIs
 * are called on the frame thread. Arguments are copied; no active-world pointers
 * are borrowed. Take joins only after completion, then transfers the CPU owner
 * (including partial failed output). Cancel joins and disposes on the caller. */
typedef struct WResidentJob WResidentJob;
int world_resident_job_start(WResidentJob **job, const WResidentBuildArgs *args,
                             float center_x, float center_y);
/* 0 = not ready/no job (outputs untouched), 1 = prepared, -1 = load failed. */
int world_resident_job_take(WResidentJob **job, WorldResident **candidate,
                            WResidentBuildTiming *timing);
void world_resident_job_cancel(WResidentJob **job);
void world_resident_activate(WorldResident **active,
                             WorldResident **candidate);
void world_resident_free(WorldResident *resident);

#endif
