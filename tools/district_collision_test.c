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

static void test_wall_contact_uses_car_height(void) {
    /* Vertical triangular wall: in its local (y,z) plane the vertices are
     * (0,0), (10,10), (0,10). At z=0.2..2.1 the actual face ends at y=2.1,
     * not y=10. Projecting the entire upper edge onto XY invents a wall at
     * y=8, at least 5.9 m beyond any face at car height. Exercise real broad
     * phase, narrow phase AND response, in rotated/translated coordinates
     * and reversed winding. No texture/category exception may mask the bug. */
    const float angles[] = {0.0f, 0.71f, 1.5707963f};
    for (int a = 0; a < 3; a++) for (int winding = 0; winding < 2; winding++) {
        N2Scene scene; N2Mesh mesh;
        float verts[20] = {0}, obst[1][4], obz[1][2];
        uint16_t idx[6]; int src[1];
        make_vertical_panel(&scene, &mesh, verts, idx, N2_SC_WALL, 10.0f);
        const float local[3][2] = {{0,0},{10,10},{0,10}};
        float c=cosf(angles[a]),s=sinf(angles[a]);
        for(int j=0;j<3;j++) {
            verts[j*5]=20.0f-s*local[j][0];
            verts[j*5+1]=-30.0f+c*local[j][0];
            verts[j*5+2]=100.0f+local[j][1];
            idx[j]=(uint16_t)(winding?2-j:j);
        }
        mesh.nverts=3; mesh.nidx=3;
        assert(collect_one(&scene,obst,src,obz)==1);
        float pos[3]={20.0f+0.5f*c-8.0f*s,-30.0f+0.5f*s+8.0f*c,100.0f};
        float vel[2]={-c,-s};
        float before[3]; memcpy(before,pos,sizeof before);
        assert(collide_walls(pos,vel,obst,obz,1,1.0f,100.2f,102.1f,
                             &scene,src,NULL,0)==0);
        assert(memcmp(pos,before,sizeof pos)==0);
        assert(vel[0]==-c && vel[1]==-s);

        /* At y=1 a face really IS present at car height: keep its normal
         * response, even for a very thin car-height interval. The seam limit
         * describes the authored face, not the height of this clipped slice. */
        pos[0]=20.0f+0.5f*c-s; pos[1]=-30.0f+0.5f*s+c;
        vel[0]=-c; vel[1]=-s;
        PhysWallContact hit={0};
        assert(collide_walls(pos,vel,obst,obz,1,1.0f,101.4f,101.5f,
                             &scene,src,&hit,1)==1);
        assert(fabsf(hit.dist-0.5f)<1e-4f && hit.span>9.99f);
        assert(fabsf(vel[0])<1e-4f && fabsf(vel[1])<1e-4f);
        assert(fabsf((pos[0]-20.0f)*c+(pos[1]+30.0f)*s-1.0f)<1e-4f);
    }
}

static void test_five_vertex_height_slice(void) {
    N2Scene scene; N2Mesh mesh;
    float verts[20]={0},obst[1][4],obz[1][2]; uint16_t idx[6]; int src[1];
    make_vertical_panel(&scene,&mesh,verts,idx,N2_SC_WALL,2.0f);
    const float yz[3][2]={{0,0},{0,1},{10,2}};
    for(int j=0;j<3;j++) { verts[j*5+1]=yz[j][0]; verts[j*5+2]=yz[j][1]; idx[j]=(uint16_t)j; }
    mesh.nverts=mesh.nidx=3;
    assert(collect_one(&scene,obst,src,obz)==1);
    /* Both clipping planes cut off a different vertex: five points remain.
     * Their largest Y is 7.5, not the original triangle's Y=10. */
    assert(!cw_probe_contact(&scene,0,0.5f,9.0f,1.0f,0.5f,1.5f));
    assert(cw_probe_contact(&scene,0,0.5f,6.0f,1.0f,0.5f,1.5f));
    /* Boundary-inclusive slabs must also handle a vertex exactly on a plane,
     * including a zero-width height interval, without division by zero. */
    assert(cw_probe_contact(&scene,0,0.5f,2.0f,1.0f,1.0f,1.0f));
    assert(!cw_probe_contact(&scene,0,0.5f,8.0f,1.0f,1.0f,1.0f));
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

    test_wall_contact_uses_car_height();
    test_five_vertex_height_slice();

    puts("district_collision_test: PASS");
    return 0;
}
