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

int world_resident_resources_build(WorldResidentResources *resources,
                                   WorldNeighborhood *neighborhood);
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
                         float player_x, float player_y, float player_z);
void world_resident_activate(WorldResident **active,
                             WorldResident **candidate);
void world_resident_free(WorldResident *resident);

#endif
