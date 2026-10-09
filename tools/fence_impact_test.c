/* One asset-free check of marked panels, ordinary walls, AI and live GPU ranges. */
#include "world.h"
#include "ai.h"
#include "world_scenery.h"
#include <assert.h>

static void group_role_test(void) {
    unsigned char ov[8]={1,0,2,0,0,0,1,0},g[56]={0};
    memcpy(g+8,"SMOKEABLE",9);uint32_t hash=0xffffffffu;
    for(const char *p="SMOKEABLE";*p;p++)hash=hash*33+(unsigned char)*p;
    memcpy(g+40,&hash,4);uint32_t one=1;memcpy(g+48,&one,4);
    WGTable table;assert(wg_open(ov,sizeof ov,g,sizeof g,&table));
    for(int event=-1;event<=0;event++) {
        WGSelection selection;assert(wg_selection_open(&table,event,&selection));
        const WGSelected *role=wg_selection_find(&selection,1,2);
        assert(role && role->membership&WG_KNOCKDOWN);
        assert(wg_selection_visible(&selection,1,2));free(selection.items);
    }
}

int main(int argc,char **argv) {
    group_role_test();int gpu=argc>1 && !strcmp(argv[1],"--gpu");
    SDL_Window *window=NULL;SDL_GLContext context=NULL;
    if(gpu) {
        assert(SDL_Init(SDL_INIT_VIDEO)==0);
        window=SDL_CreateWindow("Fence impact check",0,0,64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);assert(window);
        context=SDL_GL_CreateContext(window);assert(context);
    }
    float road[]={-30,-10,0,0,0, 100,-10,0,1,0, 100,10,0,1,1, -30,10,0,0,1};
    float panel[]={25,-8,0,0,0, 25,8,0,1,0, 25,8,4,1,1, 25,-8,4,0,1};
    float original[20];memcpy(original,panel,sizeof panel);
    uint16_t triangles[]={0,1,2,0,2,3};unsigned char colors[16]={10,20,30,255,40,50,60,255,70,80,90,255,100,110,120,255};
    N2Mesh meshes[2]={{.verts=road,.nverts=4,.idx=triangles,.nidx=6,.cat=N2_ROAD,.scen=N2_SC_TERRAIN},
        {.verts=panel,.nverts=4,.idx=triangles,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL,.vcol=colors,
         .prop_id=0x4152344c00010002ULL}};
    N2Scene scene={meshes,2,2};float bounds[][4]={{-30,-10,100,10},{25,-8,25,8}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float walls[2][4],heights[2][2];int sources[2],n=phys_collect_walls(&scene,walls,sources,heights,2);assert(n==1);
    phys_wall_index_build(walls,heights,n);
    float body[]={-2,-.85f,0,2,.85f,1.5f},pose[]={23.1f,0,0},vel[]={.02f,0};
    /* Planning is read-only. Low-speed contact still stops against the panel. */
    assert(!collide_body_walls_preview(pose,vel,0,body,walls,heights,n,.05f,1.5f,&scene,sources));
    assert(!meshes[1].prop_broken && !memcmp(panel,original,sizeof panel));
    assert(collide_body_walls(pose,vel,0,body,walls,heights,n,.05f,1.5f,&scene,sources,NULL,0));
    assert(!meshes[1].prop_broken && vel[0]==0);
    /* Even the invalid-body fallback cannot knock anything down while planning. */
    float invalid_body[6]={0},preview_pose[]={24,0,0},preview_vel[]={.2f,0};
    assert(!collide_body_walls_preview(preview_pose,preview_vel,0,invalid_body,walls,heights,n,.05f,1.5f,&scene,sources));
    assert(!meshes[1].prop_broken && preview_vel[0]==.2f);
    /* Same geometry without source membership remains an ordinary wall. */
    uint64_t identity=meshes[1].prop_id;meshes[1].prop_id=0;
    N2Mesh copy=meshes[1];copy.prop_id=identity;
    assert(!n2_mesh_same_content(&meshes[1],&copy));
    pose[0]=23.1f;vel[0]=.02f;
    assert(collide_body_walls_preview(pose,vel,0,body,walls,heights,n,.05f,1.5f,&scene,sources));
    meshes[1].prop_id=identity;
    /* A glancing corner hit must still lay this flat panel on the ground. */
    assert(g_phys_prop_impact_hook(&scene,1,10,-.3f,.9539392f)==1);
    world_props_step(.3f);assert(world_prop_update_mesh(meshes+1));
    for(int v=0;v<4;v++)assert(fabsf(panel[5*v+2])<.001f);
    world_props_reset();memcpy(panel,original,sizeof panel);
    meshes[1].prop_angle=0;meshes[1].prop_revision=0;meshes[1].prop_broken=0;
    N2Batch *batches=NULL;int count=0,map[2];BatchedVertex before[8],after[8];
    int panel_batch=-1,other_batch=-1;
    if(gpu) {
        GLuint textures[2]={0};count=upload_world_batches(&scene,bounds,textures,0,&batches,NULL,map,NULL);assert(count==2);
        /* upload returns count; the scene deliberately separates ROAD and OTHER. */
        assert(batches);panel_batch=map[1];other_batch=map[0];
        assert(panel_batch!=other_batch && batches[panel_batch].nprops==1);
        glBindBuffer(GL_ARRAY_BUFFER,batches[panel_batch].vbo);glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof before[0]*4,before);
    }
    /* A normal physics-driven racer approaches, strikes and finishes. */
    float xy[]={0,0,20,0,40,0,60,0,80,0};N2Path path={xy,5};
    AiCar cars[N_RACE_AI]={{.pos={0,0,0},.half_length=2,.half_width=.85f,.height=1.5f}};
    AiRace drivers[N_RACE_AI]={0};
    drivers[0].progress=(WRace){.active=1,.kind=N2_RACE_SPRINT,.maxlaps=1,.ngate=3,
        .gate={{.x=0,.dx=1,.half=10},{.x=40,.dx=1,.half=10},{.x=70,.dx=1,.half=10}}};
    assert(world_race_bind_course(&drivers[0].progress,&path,12));
    assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
    AiTrafficWorld world={.scene=&scene,.obst=walls,.obstz=heights,.obstsrc=sources,.nobst=n};
    for(int frame=0;frame<2400 && !drivers[0].progress.finished;frame++) {
        ai_race_step(cars,drivers,0,1,0,&world,NULL);world_props_step(1/PHYS_TICKRATE);
        if(gpu)assert(render_world_prop_updates(&scene,batches,count,world_prop_update_mesh)>=0);
        else world_prop_update_mesh(meshes+1);
        assert(!drivers[0].drive.failed);
    }
    assert(drivers[0].progress.finished && cars[0].ride.contact_mask==15);
    assert(meshes[1].prop_broken && meshes[1].prop_angle>1.56f);
    assert(memcmp(panel,original,sizeof panel));
    for(int v=0;v<4;v++){assert(fabsf(panel[5*v+2])<.001f);assert(!memcmp(panel+5*v+3,original+5*v+3,8));}
    if(gpu) {
        assert(!render_world_prop_updates(&scene,batches,count,world_prop_update_mesh));
        glBindBuffer(GL_ARRAY_BUFFER,batches[panel_batch].vbo);glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof after[0]*4,after);
        for(int v=0;v<4;v++) {
            assert(!memcmp(after[v].pos,panel+5*v,12));assert(!memcmp(after[v].uv,before[v].uv,8));
            assert(!memcmp(after[v].col,before[v].col,4));assert(fabsf(after[v].normal[2])>.99f);
        }
        glBindBuffer(GL_ARRAY_BUFFER,batches[other_batch].vbo);glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof after[0]*4,after);
        for(int v=0;v<4;v++)assert(!memcmp(after[v].pos,road+5*v,12));
        assert(glGetError()==GL_NO_ERROR);
    }
    /* A reloaded copy keeps the same fall; another source identity does not. */
    memcpy(panel,original,sizeof panel);meshes[1].prop_angle=0;meshes[1].prop_revision=0;meshes[1].prop_broken=0;
    /* Freeing a failed map candidate must not reset the current world's props. */
    WorldCity candidate={0};world_city_free(&candidate);
    pose[0]=23.1f;vel[0]=.02f;
    assert(!collide_body_walls(pose,vel,0,body,walls,heights,n,.05f,1.5f,&scene,sources,NULL,0));
    assert(vel[0]==.02f && meshes[1].prop_broken);
    if(gpu) {
        render_batch_array_free(&batches,&count);
        GLuint textures[2]={0};count=upload_world_batches(&scene,bounds,textures,0,&batches,NULL,map,NULL);assert(count==2);
        assert(render_world_prop_updates(&scene,batches,count,world_prop_update_mesh)>0);
        glBindBuffer(GL_ARRAY_BUFFER,batches[map[1]].vbo);glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof after[0]*4,after);
        for(int v=0;v<4;v++)assert(!memcmp(after[v].pos,panel+5*v,12));
        render_batch_array_free(&batches,&count);
    } else assert(world_prop_update_mesh(meshes+1));
    assert(meshes[1].prop_broken && meshes[1].prop_angle>1.56f);
    meshes[1].prop_id++;meshes[1].prop_broken=0;meshes[1].prop_revision=0;meshes[1].prop_angle=0;
    memcpy(panel,original,sizeof panel);assert(!world_prop_update_mesh(meshes+1));assert(!meshes[1].prop_broken);
    world_props_reset();meshes[1].prop_id=identity;
    assert(!world_prop_update_mesh(meshes+1));assert(!meshes[1].prop_broken);
    phys_wall_index_build(NULL,NULL,0);world_ground_grid_free(&grid);
    if(gpu){SDL_GL_DeleteContext(context);SDL_DestroyWindow(window);SDL_Quit();}
    puts("fence_impact_test: PASS (source role, solid/slow contacts, AI finish, visible fall, reload/reset, optional GPU)");
}
