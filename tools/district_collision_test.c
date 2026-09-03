#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "physics.h"

static void make_vertical_panel(N2Scene *scene, N2Mesh *mesh,
                                float verts[20], uint16_t idx[6],
                                int scen, float height) {
    memset(scene, 0, sizeof *scene);
    memset(mesh, 0, sizeof *mesh);
    const float points[4][3] = {
        {0.0f, -2.0f, 0.0f}, {0.0f, 2.0f, 0.0f},
        {0.0f,  2.0f, height}, {0.0f, -2.0f, height}
    };
    for (int i = 0; i < 4; i++) {
        verts[i * 5 + 0] = points[i][0];
        verts[i * 5 + 1] = points[i][1];
        verts[i * 5 + 2] = points[i][2];
    }
    const uint16_t triangles[6] = {0, 1, 2, 0, 2, 3};
    memcpy(idx, triangles, sizeof triangles);
    mesh->verts = verts;
    mesh->nverts = 4;
    mesh->idx = idx;
    mesh->nidx = 6;
    mesh->cat = N2_OTHER;
    mesh->scen = (unsigned char)scen;
    scene->meshes = mesh;
    scene->count = scene->cap = 1;
}

static int collect_one(N2Scene *scene, float obst[1][4], int src[1],
                       float obz[1][2]) {
    return phys_collect_walls(scene, obst, src, obz, 1);
}

int main(void) {
    N2Scene scene;
    N2Mesh mesh;
    float verts[20], obst[1][4], obz[1][2];
    uint16_t idx[6];
    int src[1];

    /* The real L4RA XW_SANDSTONEBASE instances are 0.548 m high and carry
     * near-vertical faces. Explicit WALL semantics must reach the geometric
     * narrow phase even though the old 2.5 m heuristic called them "flat". */
    make_vertical_panel(&scene, &mesh, verts, idx, N2_SC_WALL, 0.548f);
    assert(collect_one(&scene, obst, src, obz) == 1);
    assert(src[0] == 0 && fabsf(obz[0][1] - 0.548f) < 1e-6f);

    float pos[3] = {0.5f, 0.0f, 0.0f};
    float vel[2] = {-1.0f, 0.25f};
    PhysWallContact hit = {0};
    assert(collide_walls(pos, vel, obst, obz, 1, 1.0f, 0.28f, 2.10f,
                         &scene, src, &hit, 1) == 1);
    assert(pos[0] >= 0.999f);
    assert(fabsf(vel[0]) < 1e-6f && fabsf(vel[1] - 0.25f) < 1e-6f);
    assert(hit.span >= 0.547f);

    /* Explicit classification does not weaken the proven seam rejection: a
     * 0.10 m panel may enter broad phase, but the face-span narrow phase must
     * still reject it. */
    make_vertical_panel(&scene, &mesh, verts, idx, N2_SC_WALL, 0.10f);
    assert(collect_one(&scene, obst, src, obz) == 1);
    pos[0] = 0.5f; pos[1] = 0.0f; vel[0] = -1.0f; vel[1] = 0.25f;
    assert(collide_walls(pos, vel, obst, obz, 1, 1.0f, 0.0f, 2.10f,
                         &scene, src, NULL, 0) == 0);

    /* The old height heuristic remains valid for non-solid semantic props and
     * unnamed OTHER fallback meshes; this change is not a global lowering. */
    make_vertical_panel(&scene, &mesh, verts, idx, N2_SC_PROP, 0.548f);
    assert(collect_one(&scene, obst, src, obz) == 0);
    make_vertical_panel(&scene, &mesh, verts, idx, N2_SC_NONE, 0.548f);
    assert(collect_one(&scene, obst, src, obz) == 0);
    make_vertical_panel(&scene, &mesh, verts, idx, N2_SC_TERRAIN, 4.0f);
    assert(collect_one(&scene, obst, src, obz) == 0);

    puts("district_collision_test: PASS");
    return 0;
}
