#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "resource.h"
#include "world.h"
#include "world_resident.h"

static int nearf(float a, float b) {
    return fabsf(a - b) < 0.001f;
}

static void test_policy(void) {
    WResidentPolicy p = {1400.0f, 933.0f, 67.0f, 400.0f};
    float center[2] = {-1.0f, -1.0f};

    assert(world_resident_policy_valid(&p));
    assert(!world_resident_target(&p, 399.9f, 0.0f, 0.0f, 0.0f, center));
    assert(world_resident_target(&p, 400.1f, 0.0f, 0.0f, 0.0f, center));
    assert(nearf(center[0], 400.0f) && nearf(center[1], 0.0f));
    assert(!world_resident_target(&p, 400.1f, 0.0f,
                                  center[0], center[1], center));

    p.resident_radius = 1200.0f;
    assert(!world_resident_policy_valid(&p));
    p.resident_radius = 1400.0f;
    p.cell_size = 0.0f;
    assert(!world_resident_policy_valid(&p));
    p.cell_size = 400.0f;
    assert(!world_resident_target(&p, NAN, 0.0f, 0.0f, 0.0f, center));
}

static void test_route_points(void) {
    WResidentPolicy policy = {1400.0f, 933.0f, 67.0f, 400.0f};
    WorldResident *active = calloc(1, sizeof *active);
    WorldCity city = {0};
    float visited[3][2] = {{0.0f, 0.0f}};
    float pos[3], center[2];
    assert(active);

    active->center[0] = active->world.center[0] = 0.0f;
    active->center[1] = active->world.center[1] = 0.0f;
    active->radius = active->world.radius = policy.resident_radius;
    active->world.scene.meshes = calloc(1, sizeof *active->world.scene.meshes);
    active->world.mbb = calloc(1, sizeof *active->world.mbb);
    assert(active->world.scene.meshes && active->world.mbb);
    active->world.scene.count = active->world.scene.cap = 1;
    N2Mesh *ground = active->world.scene.meshes;
    ground->verts = calloc(15, sizeof *ground->verts);
    ground->idx = calloc(3, sizeof *ground->idx);
    assert(ground->verts && ground->idx);
    const float verts[15] = {
        -100.0f, -100.0f, 2.0f, 0.0f, 0.0f,
        1300.0f, -100.0f, 2.0f, 1.0f, 0.0f,
        1300.0f, 1300.0f, 2.0f, 1.0f, 1.0f
    };
    memcpy(ground->verts, verts, sizeof verts);
    ground->idx[0] = 0; ground->idx[1] = 1; ground->idx[2] = 2;
    ground->nverts = 3; ground->nidx = 3; ground->cat = N2_ROAD;
    active->world.mbb[0][0] = -100.0f;
    active->world.mbb[0][1] = -100.0f;
    active->world.mbb[0][2] = 1300.0f;
    active->world.mbb[0][3] = 1300.0f;
    assert(world_ground_grid_build(&active->world.grid, &active->world.scene,
                                   (const float (*)[4])active->world.mbb));
    world_ground_grid_activate(&active->world.grid);

    const float nav[] = {100.0f, 0.0f, 450.0f, 0.0f, 850.0f, 0.0f};
    city.nav = malloc(sizeof nav);
    assert(city.nav);
    memcpy(city.nav, nav, sizeof nav);
    city.nnav = 3;

    assert(world_resident_route_point(&policy, active, &city,
                                      0.0f, 0.0f, 2.0f,
                                      (const float (*)[2])visited, 1,
                                      pos, center));
    assert(nearf(pos[0], 450.0f) && nearf(pos[1], 0.0f) && nearf(pos[2], 2.0f));
    assert(nearf(center[0], 400.0f) && nearf(center[1], 0.0f));
    visited[1][0] = center[0]; visited[1][1] = center[1];

    active->center[0] = 400.0f;
    assert(world_resident_route_point(&policy, active, &city,
                                      pos[0], pos[1], pos[2],
                                      (const float (*)[2])visited, 2,
                                      pos, center));
    assert(nearf(pos[0], 850.0f) && nearf(pos[2], 2.0f));
    assert(nearf(center[0], 800.0f) && nearf(center[1], 0.0f));

    world_city_free(&city);
    world_resident_free(active);
}

static void make_ground_scene(N2Scene *scene, N2Mesh *mesh,
                              float verts[15], uint16_t idx[3], float z) {
    const float value[15] = {
        0.0f, 0.0f, z, 0.0f, 0.0f,
        4.0f, 0.0f, z, 1.0f, 0.0f,
        0.0f, 4.0f, z, 0.0f, 1.0f,
    };
    for (int i = 0; i < 15; i++) verts[i] = value[i];
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    *mesh = (N2Mesh){0};
    mesh->verts = verts;
    mesh->idx = idx;
    mesh->nverts = 3;
    mesh->nidx = 3;
    mesh->cat = N2_ROAD;
    scene->meshes = mesh;
    scene->count = 1;
    scene->cap = 1;
}

static void test_ground_grid_ownership(void) {
    N2Scene scene_a, scene_b;
    N2Mesh mesh_a, mesh_b;
    float verts_a[15], verts_b[15];
    uint16_t idx_a[3], idx_b[3];
    float bounds_a[1][4] = {{0.0f, 0.0f, 4.0f, 4.0f}};
    float bounds_b[1][4] = {{0.0f, 0.0f, 4.0f, 4.0f}};
    WGroundGrid grid_a = {0}, grid_b = {0};
    float z = -9.0f;

    make_ground_scene(&scene_a, &mesh_a, verts_a, idx_a, 2.0f);
    make_ground_scene(&scene_b, &mesh_b, verts_b, idx_b, 7.0f);
    assert(world_ground_grid_build(&grid_a, &scene_a, bounds_a));
    world_ground_grid_activate(&grid_a);
    assert(world_ground_at(&scene_a, 1.0f, 1.0f, -9.0f, &z) == WSURF_ROAD);
    assert(nearf(z, 2.0f));

    assert(world_ground_grid_build(&grid_b, &scene_b, bounds_b));
    assert(world_ground_at(&scene_a, 1.0f, 1.0f, -9.0f, &z) == WSURF_ROAD);
    assert(nearf(z, 2.0f));

    world_ground_grid_activate(&grid_b);
    assert(world_ground_at(&scene_b, 1.0f, 1.0f, -9.0f, &z) == WSURF_ROAD);
    assert(nearf(z, 7.0f));

    world_ground_grid_free(&grid_a);
    world_ground_grid_free(&grid_b);
    assert(!grid_a.start && !grid_a.list && !grid_b.start && !grid_b.list);
    assert(grid_a.gw == 0 && grid_a.gh == 0 && grid_b.gw == 0 && grid_b.gh == 0);
}

static void test_cpu_owner_cleanup(void) {
    WorldNeighborhood neighborhood = {0};
    WorldCity city = {0};

    neighborhood.scene.meshes = calloc(1, sizeof *neighborhood.scene.meshes);
    assert(neighborhood.scene.meshes);
    neighborhood.scene.count = neighborhood.scene.cap = 1;
    neighborhood.scene.meshes[0].verts = malloc(15 * sizeof(float));
    neighborhood.scene.meshes[0].idx = malloc(3 * sizeof(uint16_t));
    neighborhood.scene.meshes[0].vcol = malloc(12);
    neighborhood.vista.meshes = calloc(1, sizeof *neighborhood.vista.meshes);
    assert(neighborhood.scene.meshes[0].verts && neighborhood.scene.meshes[0].idx &&
           neighborhood.scene.meshes[0].vcol && neighborhood.vista.meshes);
    neighborhood.vista.count = neighborhood.vista.cap = 1;
    neighborhood.vista.meshes[0].verts = malloc(15 * sizeof(float));
    neighborhood.vista.meshes[0].idx = malloc(3 * sizeof(uint16_t));
    assert(neighborhood.vista.meshes[0].verts && neighborhood.vista.meshes[0].idx);

    neighborhood.nreg = 1;
    neighborhood.rgn[0].data = malloc(1);
    neighborhood.rgn[0].tpk.blk = malloc(sizeof *neighborhood.rgn[0].tpk.blk);
    neighborhood.loc4 = malloc(1);
    neighborhood.master = malloc(1);
    neighborhood.mastertpk.blk = malloc(sizeof *neighborhood.mastertpk.blk);
    neighborhood.common = malloc(1);
    neighborhood.commontpk.blk = malloc(sizeof *neighborhood.commontpk.blk);
    neighborhood.grass.rgb = malloc(3);
    neighborhood.grass.alpha = malloc(1);
    neighborhood.grass.dxt = malloc(8);
    neighborhood.lights = malloc(sizeof *neighborhood.lights);
    neighborhood.mbb = malloc(4 * sizeof(float));
    neighborhood.grid.start = malloc(2 * sizeof(int));
    neighborhood.grid.list = malloc(sizeof(int));
    assert(neighborhood.rgn[0].data && neighborhood.rgn[0].tpk.blk &&
           neighborhood.loc4 && neighborhood.master && neighborhood.mastertpk.blk &&
           neighborhood.common && neighborhood.commontpk.blk &&
           neighborhood.grass.rgb && neighborhood.grass.alpha &&
           neighborhood.grass.dxt && neighborhood.lights && neighborhood.mbb &&
           neighborhood.grid.start && neighborhood.grid.list);

    city.navcomp = malloc(sizeof *city.navcomp);
    city.nav = malloc(2 * sizeof *city.nav);
    city.navedge = malloc(2 * sizeof *city.navedge);
    city.adjstart = malloc(2 * sizeof *city.adjstart);
    city.adjlist = malloc(sizeof *city.adjlist);
    city.navev = malloc(sizeof *city.navev);
    city.navopen = malloc(sizeof *city.navopen);
    assert(city.navcomp && city.nav && city.navedge && city.adjstart &&
           city.adjlist && city.navev && city.navopen);

    world_neighborhood_free(&neighborhood);
    world_city_free(&city);
    assert(!neighborhood.scene.meshes && !neighborhood.vista.meshes &&
           !neighborhood.rgn[0].data && !neighborhood.loc4 &&
           !neighborhood.master && !neighborhood.common &&
           !neighborhood.grass.rgb && !neighborhood.grass.alpha &&
           !neighborhood.grass.dxt && !neighborhood.lights && !neighborhood.mbb &&
           !neighborhood.grid.start && !neighborhood.grid.list);
    assert(!city.navcomp && !city.nav && !city.navedge && !city.adjstart &&
           !city.adjlist && !city.navev && !city.navopen);

    world_neighborhood_free(&neighborhood);
    world_city_free(&city);
}

static void test_mapped_master_cleanup(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/openug2-master-%ld.bin", (long)getpid());
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite("master", 1, 6, file) == 6);
    assert(fclose(file) == 0);

    WorldNeighborhood neighborhood = {0};
    neighborhood.master = res_map_file(path, &neighborhood.masterlen);
    assert(neighborhood.master && neighborhood.masterlen == 6);
    neighborhood.master_mapped = 1;
    assert(unlink(path) == 0);
    world_neighborhood_free(&neighborhood);
    assert(!neighborhood.master && neighborhood.masterlen == 0 &&
           !neighborhood.master_mapped);
}

typedef struct {
    float pos[3], vel[2], heading;
    unsigned char vertical_state[48];
} PlayerSnapshot;

static WorldResident *fixture_resident(unsigned long generation,
                                       float center_x, float center_y,
                                       float support_z, int supported) {
    WorldResident *resident = calloc(1, sizeof *resident);
    assert(resident);
    resident->center[0] = resident->world.center[0] = center_x;
    resident->center[1] = resident->world.center[1] = center_y;
    resident->radius = resident->world.radius = 1400.0f;
    resident->generation = generation;
    resident->world.scene.meshes = calloc(1, sizeof *resident->world.scene.meshes);
    resident->world.mbb = calloc(1, sizeof *resident->world.mbb);
    assert(resident->world.scene.meshes && resident->world.mbb);
    resident->world.scene.count = resident->world.scene.cap = 1;
    resident->world.scene.meshes[0].verts = calloc(15, sizeof(float));
    resident->world.scene.meshes[0].idx = calloc(3, sizeof(uint16_t));
    assert(resident->world.scene.meshes[0].verts &&
           resident->world.scene.meshes[0].idx);
    float *v = resident->world.scene.meshes[0].verts;
    v[0] = 0.0f; v[1] = 0.0f; v[2] = support_z;
    v[5] = 4.0f; v[6] = 0.0f; v[7] = support_z;
    v[10] = 0.0f; v[11] = 4.0f; v[12] = support_z;
    resident->world.scene.meshes[0].idx[0] = 0;
    resident->world.scene.meshes[0].idx[1] = 1;
    resident->world.scene.meshes[0].idx[2] = 2;
    resident->world.scene.meshes[0].nverts = 3;
    resident->world.scene.meshes[0].nidx = 3;
    resident->world.scene.meshes[0].cat = supported ? N2_ROAD : N2_OTHER;
    resident->world.mbb[0][0] = resident->world.mbb[0][1] = 0.0f;
    resident->world.mbb[0][2] = resident->world.mbb[0][3] = 4.0f;
    if (supported)
        assert(world_ground_grid_build(&resident->world.grid,
                                       &resident->world.scene,
                                       (const float (*)[4])resident->world.mbb));
    return resident;
}

static void test_transactional_activation(void) {
    WorldResident *active = fixture_resident(1, 0.0f, 0.0f, 2.0f, 1);
    WorldResident *candidate = fixture_resident(0, 400.0f, 0.0f, 7.0f, 0);
    const N2Mesh *old_meshes = active->world.scene.meshes;
    PlayerSnapshot player = {{1.0f, 1.0f, 2.0f}, {3.0f, 4.0f}, 0.75f, {0}};
    PlayerSnapshot before = player;
    float z = -9.0f;

    world_ground_grid_activate(&active->world.grid);
    assert(world_ground_at(&active->world.scene, 1.0f, 1.0f, -9.0f, &z) ==
           WSURF_ROAD && nearf(z, 2.0f));
    assert(!world_resident_validate_cpu(candidate, 1.0f, 1.0f, 2.0f));
    world_resident_free(candidate);
    candidate = NULL;
    assert(active->world.scene.meshes == old_meshes);
    assert(world_ground_at(&active->world.scene, 1.0f, 1.0f, -9.0f, &z) ==
           WSURF_ROAD && nearf(z, 2.0f));

    candidate = fixture_resident(0, 400.0f, 0.0f, 7.0f, 1);
    assert(world_resident_validate_cpu(candidate, 1.0f, 1.0f, 7.0f));
    world_resident_activate(&active, &candidate);
    assert(active && candidate && active->generation == 2);
    assert(active->world.scene.meshes != old_meshes);
    assert(candidate->world.scene.meshes == old_meshes);
    assert(world_ground_at(&active->world.scene, 1.0f, 1.0f, -9.0f, &z) ==
           WSURF_ROAD && nearf(z, 7.0f));
    assert(!memcmp(&player, &before, sizeof player));

    world_resident_free(candidate);
    world_resident_free(active);
}

int main(void) {
    test_policy();
    test_route_points();
    test_ground_grid_ownership();
    test_cpu_owner_cleanup();
    test_mapped_master_cleanup();
    test_transactional_activation();
    puts("world_resident_test: PASS");
    return 0;
}
