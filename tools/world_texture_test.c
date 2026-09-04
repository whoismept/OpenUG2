/* Asset-free integration test: real world loader, decoder and GL upload.
 * Break caught: missing common-library fallback, lost alpha/draw metadata,
 * or a shared texture overriding an existing region/master texture. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "world.h"
#include "world_resident.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint32_t KEY = 0x12345678u;
static void u32(unsigned char *p, uint32_t n) {
    for (int i=0;i<4;i++) p[i]=(unsigned char)(n>>(i*8));
}
/* One 4x4 DXT3 fixture. Even texels transparent, odd texels opaque.
 * RGB endpoint is red for common, green for the overriding source. */
static void write_tpk(const char *path, uint32_t key, int green) {
    unsigned char d[8+124+8+16]={0};
    u32(d,0xb3310000u);u32(d+4,124);
    unsigned char *r=d+8;
    memcpy(r,"TEST_TEXTURE",12);u32(r+0x18,key);u32(r+0x2c,16);
    r[0x38]=r[0x3a]=4;r[0x3e]=0x24;
    r[0x45]=5;r[0x49]=2;r[0x4a]=1;r[0x4b]=0;
    u32(d+132,0x33320002u);u32(d+136,16);
    memset(d+140,0xf0,8);d[148]=green?0xe0:0;d[149]=green?0x07:0xf8;
    FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(d,1,sizeof d,f)==sizeof d);assert(!fclose(f));
}

static void run_case(const char *tracks,const char *masterpath,
                     int want_count,int green,int consumer,int second_region) {
    World *w=calloc(1,sizeof *w);assert(w);
    /* The fixture contains textures, not retail models. Request one texture
     * on a test-owned mesh after the real loader establishes source paths. */
    assert(world_load(w,tracks,"STREAMTEST")==0);
    assert(w->neighborhood.nreg==1 && w->neighborhood.rgn[0].data);
    if(masterpath && !second_region) {
        w->neighborhood.master=n2_read_file(masterpath,&w->neighborhood.masterlen);assert(w->neighborhood.master);
        w->neighborhood.mastertpk=n2_tpk_open(w->neighborhood.master,w->neighborhood.masterlen);
    }
    if(second_region) {
        w->neighborhood.nreg=2;
        w->neighborhood.rgn[1].data=n2_read_file(masterpath,&w->neighborhood.rgn[1].len);assert(w->neighborhood.rgn[1].data);
        w->neighborhood.rgn[1].tpk=n2_tpk_open(w->neighborhood.rgn[1].data,w->neighborhood.rgn[1].len);
    }
    w->neighborhood.scene.meshes=calloc(2,sizeof *w->neighborhood.scene.meshes);assert(w->neighborhood.scene.meshes);
    w->neighborhood.scene.count=consumer==0?2:0;w->neighborhood.rgn[0].mesh1=w->neighborhood.scene.count;
    w->neighborhood.scene.meshes[0].texkey=w->neighborhood.scene.meshes[1].texkey=KEY;
    uint32_t requested=KEY;
    if(consumer==1) {
        w->neighborhood.vista.meshes=calloc(1,sizeof *w->neighborhood.vista.meshes);assert(w->neighborhood.vista.meshes);
        w->neighborhood.vista.count=1;w->neighborhood.vista.meshes[0].texkey=KEY;
    } else if(consumer==2) {w->neighborhood.nlights=1;requested=N2_TEX_SFX_FLARE_GLOWA;}
    uint32_t keys[4]={0};GLuint ids[4]={0};unsigned char modes[4]={0};
    int n=world_bind_textures(w,keys,ids,modes,4);
    assert(n==want_count); /* RED before common-library support: 0 instead of 1. */
    assert(!w->neighborhood.common && !w->neighborhood.commontpk.blk && !w->neighborhood.commonlen && !w->neighborhood.commontpk.nblk);
    if(n) {
        assert(keys[0]==requested && modes[0]==N2_DRAW_BLEND);
        unsigned char rgba[4*4*4];glBindTexture(GL_TEXTURE_2D,ids[0]);
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        assert(glGetError()==GL_NO_ERROR);
        for(int i=0;i<16;i++) {
            assert(rgba[i*4]==(green?0:255));assert(rgba[i*4+1]==(green?255:0));
            assert(rgba[i*4+2]==0);assert(rgba[i*4+3]==((i&1)?255:0));
        }
        glDeleteTextures(n,ids);
    }
    /* Only fixture-owned allocations; no production cleanup API added. */
    for(int i=0;i<w->neighborhood.nreg;i++)free(w->neighborhood.rgn[i].tpk.blk);
    free(w->neighborhood.loc4);free(w->neighborhood.master);free(w->neighborhood.mastertpk.blk);free(w->neighborhood.vista.meshes);
    free(w->neighborhood.scene.meshes);free(w->neighborhood.mbb);free(w->city.nav);free(w->city.navcomp);
    free(w->city.navedge);free(w->city.adjstart);free(w->city.adjlist);free(w->city.navev);free(w->city.navopen);
    free(w);
}

static void test_resident_resource_cleanup(void) {
    WorldResidentResources resources = {0};
    resources.texture_count = 2;
    resources.textures = calloc(2, sizeof *resources.textures);
    resources.texture_keys = calloc(2, sizeof *resources.texture_keys);
    resources.texture_modes = calloc(2, sizeof *resources.texture_modes);
    resources.ordinary_count = 2;
    resources.ordinary = calloc(2, sizeof *resources.ordinary);
    resources.debug_batches = calloc(2, sizeof *resources.debug_batches);
    resources.obstacle_count = 1;
    resources.obstacles = calloc(1, sizeof *resources.obstacles);
    resources.obstacle_z = calloc(1, sizeof *resources.obstacle_z);
    resources.obstacle_src = calloc(1, sizeof *resources.obstacle_src);
    assert(resources.textures && resources.texture_keys && resources.texture_modes &&
           resources.ordinary && resources.debug_batches && resources.obstacles &&
           resources.obstacle_z && resources.obstacle_src);

    glGenTextures(2, resources.textures);
    glGenBuffers(1, &resources.ordinary[0].vbo);
    glGenBuffers(1, &resources.ordinary[0].ibo);
    glGenBuffers(1, &resources.ordinary[1].vbo);
    glGenBuffers(1, &resources.ordinary[1].ibo);
    for (int i = 0; i < 2; i++) {
        unsigned char pixel[4] = {255, 255, 255, 255};
        glBindTexture(GL_TEXTURE_2D, resources.textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindBuffer(GL_ARRAY_BUFFER, resources.ordinary[i].vbo);
        glBufferData(GL_ARRAY_BUFFER, 4, pixel, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, resources.ordinary[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4, pixel, GL_STATIC_DRAW);
    }
    GLuint texture0 = resources.textures[0];
    GLuint vbo0 = resources.ordinary[0].vbo;
    GLuint ibo0 = resources.ordinary[0].ibo;
    assert(glIsTexture(texture0) && glIsBuffer(vbo0) && glIsBuffer(ibo0));

    world_resident_resources_free(&resources);
    assert(!resources.textures && !resources.texture_keys && !resources.texture_modes &&
           !resources.ordinary && !resources.debug_batches && !resources.obstacles &&
           !resources.obstacle_z && !resources.obstacle_src);
    assert(resources.texture_count == 0 && resources.ordinary_count == 0 &&
           resources.obstacle_count == 0);
    assert(!glIsTexture(texture0) && !glIsBuffer(vbo0) && !glIsBuffer(ibo0));
    world_resident_resources_free(&resources);
}

static void test_resident_resource_build(const char *region_path) {
    WorldNeighborhood neighborhood = {0};
    neighborhood.nreg = 1;
    neighborhood.rgn[0].data = n2_read_file(region_path,
                                             &neighborhood.rgn[0].len);
    assert(neighborhood.rgn[0].data);
    neighborhood.rgn[0].tpk = n2_tpk_open(neighborhood.rgn[0].data,
                                           neighborhood.rgn[0].len);
    neighborhood.rgn[0].mesh0 = 0;
    neighborhood.rgn[0].mesh1 = 1;
    neighborhood.scene.meshes = calloc(1, sizeof *neighborhood.scene.meshes);
    neighborhood.scene.count = neighborhood.scene.cap = 1;
    N2Mesh *mesh = neighborhood.scene.meshes;
    mesh->verts = malloc(15 * sizeof *mesh->verts);
    mesh->idx = malloc(3 * sizeof *mesh->idx);
    assert(mesh->verts && mesh->idx);
    const float verts[15] = {
        0,0,0,0,0, 4,0,0,1,0, 0,4,0,0,1
    };
    memcpy(mesh->verts, verts, sizeof verts);
    mesh->idx[0]=0; mesh->idx[1]=1; mesh->idx[2]=2;
    mesh->nverts=3; mesh->nidx=3; mesh->cat=N2_ROAD;
    mesh->texkey=KEY; mesh->mat_exact=1;
    neighborhood.mbb = malloc(sizeof *neighborhood.mbb);
    assert(neighborhood.mbb);
    neighborhood.mbb[0][0]=0; neighborhood.mbb[0][1]=0;
    neighborhood.mbb[0][2]=4; neighborhood.mbb[0][3]=4;

    WorldResidentResources resources = {0};
    assert(world_resident_resources_build(&resources, &neighborhood, NULL));
    assert(resources.texture_count == 1 && resources.textures[0]);
    assert(resources.mesh_count == 1 && resources.mesh_textures[0]);
    assert(resources.ordinary_count > 0 && resources.ordinary[0].vbo &&
           resources.ordinary[0].ibo);
    assert(!neighborhood.rgn[0].data);
    world_resident_resources_free(&resources);
    world_neighborhood_free(&neighborhood);
}

int main(void) {
    char root[]="build/world-texture-XXXXXX";assert(mkdtemp(root));
    char tracks[160],global[160],region[192],common[192],master[192];
    snprintf(tracks,sizeof tracks,"%s/TRACKS",root);
    snprintf(global,sizeof global,"%s/GLOBAL",root);
    assert(!mkdir(tracks,0700)&&!mkdir(global,0700));
    snprintf(region,sizeof region,"%s/STREAMTEST.BUN",tracks);
    snprintf(common,sizeof common,"%s/InGameCommon.bun",global);
    snprintf(master,sizeof master,"%s/master-fixture.bun",root);
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_Window *win=SDL_CreateWindow("world-texture-test",0,0,32,32,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    assert(win);SDL_GLContext ctx=SDL_GL_CreateContext(win);assert(ctx);
    test_resident_resource_cleanup();
    /* No regional match: common supplies exact key, RGBA and draw mode once. */
    write_tpk(region,KEY+1,1);write_tpk(common,KEY,0);
    run_case(tracks,NULL,1,0,0,0);
    run_case(tracks,NULL,1,0,1,0);
    write_tpk(common,N2_TEX_SFX_FLARE_GLOWA,0);run_case(tracks,NULL,1,0,2,0);
    write_tpk(common,KEY,0);
    /* Existing region and master matches must win over the common red image. */
    write_tpk(region,KEY,1);test_resident_resource_build(region);
    run_case(tracks,NULL,1,1,0,0);
    write_tpk(region,KEY+1,1);write_tpk(master,KEY,1);run_case(tracks,master,1,1,0,0);
    /* Common in region A must not preempt the existing region-B vista copy. */
    run_case(tracks,master,1,1,1,1);
    /* A present but unrelated common library must not invent a match. */
    write_tpk(common,KEY+2,0);run_case(tracks,NULL,0,0,0,0);
    /* A TPK-only unrelated region must not change the old source policy. */
    run_case(tracks,master,0,0,0,1);
    assert(!unlink(common));run_case(tracks,NULL,0,0,0,0);
    assert(!unlink(region)&&!unlink(master));
    assert(!rmdir(tracks)&&!rmdir(global)&&!rmdir(root));
    SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(win);SDL_Quit();
    puts("world_texture_test: PASS");return 0;
}
