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
    }
    /* Each case rewrites the fixture archives under the same key, so the
       process-wide texture cache must not survive into the next one. */
    world_texture_cache_clear();
    /* The loader activates this owner's grid even for an empty scene. Use the
     * ownership teardown so later ground queries cannot see a freed grid. */
    world_neighborhood_free(&w->neighborhood);
    world_city_free(&w->city);
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
    GLuint texture0 = resources.textures[0], texture1 = resources.textures[1];
    GLuint vbo0 = resources.ordinary[0].vbo;
    GLuint ibo0 = resources.ordinary[0].ibo;
    assert(glIsTexture(texture0) && glIsBuffer(vbo0) && glIsBuffer(ibo0));

    world_resident_resources_free(&resources);
    assert(!resources.textures && !resources.texture_keys && !resources.texture_modes &&
           !resources.ordinary && !resources.debug_batches && !resources.obstacles &&
           !resources.obstacle_z && !resources.obstacle_src);
    assert(resources.texture_count == 0 && resources.ordinary_count == 0 &&
           resources.obstacle_count == 0);
    assert(!glIsBuffer(vbo0) && !glIsBuffer(ibo0));
    /* World textures are borrowed from the process-wide key cache and shared
       with the other resident, so freeing one resident must NOT delete them. */
    assert(glIsTexture(texture0));
    GLuint own[2] = {texture0, texture1};
    glDeleteTextures(2, own);   /* this fixture's own, not the cache's */
    world_resident_resources_free(&resources);
}

/* Returns the GL name bound for KEY, so the caller can prove a second build
 * reuses it instead of decoding and uploading the same record again. */
static GLuint test_resident_resource_build(const char *region_path) {
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
    GLuint bound = resources.textures[0];
    world_resident_resources_free(&resources);
    assert(world_ground_grid_build(&neighborhood.grid,&neighborhood.scene,
                                   (const float (*)[4])neighborhood.mbb));
    WorldResident candidate={0};candidate.world=neighborhood;
    candidate.radius=candidate.world.radius=1400;
    for(int leave=0;leave<2;leave++) {
        assert(!world_resident_finish_step(&candidate,1,1,0,1,NULL));
        assert(candidate.resources.upload && !candidate.resources.ordinary);
        /* A supported start is insufficient: the last step must reject a
         * player who has since left this detached candidate's ground. */
        int status=world_resident_finish_step(&candidate,leave?1e8f:1,1,0,1,NULL);
        assert(status==(leave?-1:1));
        world_resident_resources_free(&candidate.resources);
    }
    world_neighborhood_free(&candidate.world);
    return bound;
}

static void test_failed_upload_retry(const char *region_path) {
    N2Mesh mesh={0};mesh.texkey=KEY;
    for(int attempt=0;attempt<2;attempt++) {
        World w={0};w.neighborhood.nreg=1;
        w.neighborhood.scene.meshes=&mesh;w.neighborhood.scene.count=1;
        WRegion *r=&w.neighborhood.rgn[0];r->mesh1=1;
        r->data=n2_read_file(region_path,&r->len);assert(r->data);
        r->tpk=n2_tpk_open(r->data,r->len);
        uint32_t key=0;GLuint id=0;unsigned char mode=0;
        if(!attempt)glEnable(0xdeadbeef); /* deterministic GL failure */
        int n=world_bind_textures(&w,&key,&id,&mode,1);
        assert(n==(attempt?1:-1));
        if(attempt)assert(id && glIsTexture(id) && mode==N2_DRAW_BLEND);
        free(r->tpk.blk);
    }
    world_texture_cache_clear();
}

static void assert_same_buffer(GLenum target, GLuint a, GLuint b) {
    GLint na=0,nb=0;
    glBindBuffer(target,a);glGetBufferParameteriv(target,GL_BUFFER_SIZE,&na);
    glBindBuffer(target,b);glGetBufferParameteriv(target,GL_BUFFER_SIZE,&nb);
    assert(na>0 && na==nb);
    void *pa=malloc((size_t)na),*pb=malloc((size_t)nb);assert(pa && pb);
    glBindBuffer(target,a);glGetBufferSubData(target,0,na,pa);
    glBindBuffer(target,b);glGetBufferSubData(target,0,nb,pb);
    assert(!memcmp(pa,pb,(size_t)na));free(pa);free(pb);
}

static void test_sliced_batches(void) {
    N2Mesh meshes[7]={{0}};N2Scene scene={meshes,7,7};
    float verts[7][15],bounds[7][4];uint16_t indices[3]={0,1,2};
    unsigned char colors[7][12],modes[7]={0};
    GLuint textures[7]={3,3,3,2,3,3,3};int direct_map[7],sliced_map[7];
    const float triangle[15]={0,0,0,0,0, 4,0,0,1,0, 0,4,0,0,1};
    for(int i=0;i<7;i++) {
        memcpy(verts[i],triangle,sizeof triangle);memset(colors[i],i+20,12);
        for(int v=0;v<3;v++)verts[i][v*5+2]=(float)i;
        bounds[i][0]=bounds[i][1]=0;bounds[i][2]=bounds[i][3]=4;
        meshes[i].verts=verts[i];meshes[i].idx=indices;meshes[i].vcol=colors[i];
        meshes[i].nverts=3;meshes[i].nidx=3;meshes[i].cat=N2_ROAD;
        meshes[i].texkey=textures[i];meshes[i].mat_exact=i>=2;
        sliced_map[i]=-99;
    }
    meshes[4].cat=N2_SKY;meshes[5].cat=N2_GLOW;modes[6]=N2_DRAW_CUTOUT;
    N2Batch *direct=NULL,*sliced=NULL;int count=-99;
    int nd=upload_world_batches(&scene,bounds,textures,0,&direct,NULL,direct_map,modes);
    assert(nd==4 && direct_map[0]==direct_map[1] && direct_map[2]!=direct_map[0]);
    assert(direct_map[6]!=direct_map[2] && direct_map[4]==-1 && direct_map[5]==-1);
    WorldBatchUpload *job=upload_world_batches_begin(&scene,bounds,textures,0,NULL,modes);
    assert(job && !upload_world_batches_step(&job,0,&sliced,&count,sliced_map));
    for(int step=1;step<=nd;step++) {
        assert(upload_world_batches_step(&job,1,&sliced,&count,sliced_map)==(step==nd));
        if(step<nd) {
            assert(job && !sliced && count==-99);
            for(int i=0;i<7;i++)assert(sliced_map[i]==-99);
        }
    }
    assert(!job && count==nd && !memcmp(direct_map,sliced_map,sizeof direct_map));
    for(int i=0;i<nd;i++) {
        assert_same_buffer(GL_ARRAY_BUFFER,direct[i].vbo,sliced[i].vbo);
        assert_same_buffer(GL_ELEMENT_ARRAY_BUFFER,direct[i].ibo,sliced[i].ibo);
        N2Batch a=direct[i],b=sliced[i];a.vbo=a.ibo=b.vbo=b.ibo=0;
        assert(!memcmp(&a,&b,sizeof a));
    }
    render_batch_array_free(&direct,&nd);render_batch_array_free(&sliced,&count);
    /* On each partial stage, cancel and prove the latest emitted GL pair died.
     * Immediate GL failure must also cancel and leave complete outputs alone. */
    for(int stop=0;stop<4;stop++) {
        job=upload_world_batches_begin(&scene,bounds,textures,0,NULL,modes);assert(job);
        GLint vbo=0,ibo=0;
        for(int step=0;step<stop;step++)
            assert(!upload_world_batches_step(&job,1,&sliced,&count,NULL));
        if(stop) {
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&vbo);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&ibo);
            assert(glIsBuffer((GLuint)vbo) && glIsBuffer((GLuint)ibo));
        }
        if(stop==2) {
            glEnable(0xdeadbeefu);
            assert(upload_world_batches_step(&job,1,&sliced,&count,NULL)==-1);
            assert(!job && !sliced && count==0);
        } else upload_world_batches_cancel(&job);
        assert(!job && !glIsBuffer((GLuint)vbo) && !glIsBuffer((GLuint)ibo));
        upload_world_batches_cancel(&job);
    }
    assert(glGetError()==GL_NO_ERROR);
}

static void test_sliced_u16_limit(void) {
    N2Mesh meshes[3]={{0}};N2Scene scene={meshes,3,3};
    float bounds[3][4]={{0,0,4,4},{0,0,4,4},{0,0,4,4}};
    GLuint textures[3]={1,1,1};int map[3],counts[3]={40000,25535,3};
    uint16_t indices[3][3];
    for(int i=0;i<3;i++) {
        meshes[i].nverts=counts[i];meshes[i].nidx=3;meshes[i].cat=N2_ROAD;
        meshes[i].verts=calloc((size_t)counts[i]*5,sizeof(float));assert(meshes[i].verts);
        indices[i][0]=0;indices[i][1]=1;indices[i][2]=(uint16_t)(counts[i]-1);
        meshes[i].idx=indices[i];
    }
    WorldBatchUpload *job=upload_world_batches_begin(&scene,bounds,textures,0,NULL,NULL);
    N2Batch *out=NULL;int count=0;assert(job);
    assert(!upload_world_batches_step(&job,1,&out,&count,map));
    assert(upload_world_batches_step(&job,1,&out,&count,map)==1);
    assert(count==2 && map[0]==map[1] && map[2]!=map[0]);
    GLint bytes=0;uint16_t packed[6];
    glBindBuffer(GL_ARRAY_BUFFER,out[map[0]].vbo);
    glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&bytes);
    assert(bytes==65535*(int)sizeof(BatchedVertex));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,out[map[0]].ibo);
    glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER,0,sizeof packed,packed);
    assert(packed[2]==39999 && packed[3]==40000 && packed[5]==65534);
    render_batch_array_free(&out,&count);
    for(int i=0;i<3;i++)free(meshes[i].verts);
}

static void test_incremental_retirement(void) {
    for(int steps=1;steps<=5;steps++) {
        WorldResident *r=calloc(1,sizeof *r);assert(r);
        r->resources.ordinary_count=2;
        r->resources.ordinary=calloc(2,sizeof *r->resources.ordinary);
        r->world.scene.count=2;r->world.vista.count=1;
        r->world.scene.meshes=calloc(2,sizeof *r->world.scene.meshes);
        r->world.vista.meshes=calloc(1,sizeof *r->world.vista.meshes);
        assert(r->resources.ordinary && r->world.scene.meshes && r->world.vista.meshes);
        GLuint ids[2];glGenBuffers(2,ids);
        for(int i=0;i<2;i++) {
            glBindBuffer(GL_ARRAY_BUFFER,ids[i]);glBufferData(GL_ARRAY_BUFFER,4,ids,GL_STATIC_DRAW);
            r->resources.ordinary[i].vbo=ids[i];
            r->world.scene.meshes[i].verts=malloc(4);
            r->world.scene.meshes[i].idx=malloc(2);
            r->world.scene.meshes[i].vcol=malloc(4);
        }
        r->world.vista.meshes[0].verts=malloc(4);
        assert(!world_resident_retire_step(&r,0));
        assert(!world_resident_retire_step(&r,1));
        assert(r->resources.ordinary_count==1 && r->world.scene.count==2);
        assert(glIsBuffer(ids[0]) && !glIsBuffer(ids[1]));
        /* Cancel after every partial stage, including partially freed CPU
         * meshes, or finish all two batches and three meshes in five calls. */
        for(int call=2;call<=steps;call++)
            assert(world_resident_retire_step(&r,1)==(call==5));
        if(steps<5) {assert(r);world_resident_free(r);r=NULL;}
        else assert(!r);
        assert(!glIsBuffer(ids[0]) && !glIsBuffer(ids[1]));
        assert(world_resident_retire_step(&r,1));
    }
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
    test_sliced_batches();
    test_sliced_u16_limit();
    test_incremental_retirement();
    /* No regional match: common supplies exact key, RGBA and draw mode once. */
    write_tpk(region,KEY+1,1);write_tpk(common,KEY,0);
    run_case(tracks,NULL,1,0,0,0);
    run_case(tracks,NULL,1,0,1,0);
    write_tpk(common,N2_TEX_SFX_FLARE_GLOWA,0);run_case(tracks,NULL,1,0,2,0);
    write_tpk(common,KEY,0);
    /* Existing region and master matches must win over the common red image. */
    write_tpk(region,KEY,1);
    test_failed_upload_retry(region);
    /* A district swap rebuilds the same keys: the second build must reuse the
       first build's GL name (no re-decode, no re-upload, no second copy in
       VRAM), and the name must outlive the resident that first requested it
       until the cache itself is cleared. */
    GLuint first=test_resident_resource_build(region);
    assert(glIsTexture(first)); /* verify ownership BEFORE a new name can be reused */
    GLuint again=test_resident_resource_build(region);
    assert(first && first==again && glIsTexture(first));
    world_texture_cache_clear();
    assert(!glIsTexture(first));
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
