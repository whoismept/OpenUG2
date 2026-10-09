/* Optional check against locally installed assets; no game files are fixtures.
 * Course decoding is distinct from completing a race under normal collision.
 * Neutral car dimensions test route feasibility, not every fitted car/model. */
#include "ai.h"
#include "world.h"

static int drive(const char *root,const WEvent *e,int *spawned,int *finished) {
    World w={0};
    char region[64];snprintf(region,sizeof region,"STREAM%s",e->reg);
    w.neighborhood.nreg=1;snprintf(w.neighborhood.rgn[0].name,64,"%s",region);
    if(!world_city_load(&w.city,&w.neighborhood,root,region)){world_city_free(&w.city);return 0;}
    int ev=-1;for(int k=0;k<w.city.nev;k++)if(w.city.ev[k].id==e->id)ev=k;
    if(!world_race_start(&w,root,ev,1)){world_city_free(&w.city);return 0;}
    WorldNeighborhood nb={0};
    WLoadOptions opt={1,w.city.race.grid[0][0],w.city.race.grid[0][1],6000,e->id};
    if(!world_neighborhood_load(&nb,root,region,&opt)) {
        world_neighborhood_free(&nb);world_city_free(&w.city);return 0;
    }
    world_ground_grid_activate(&nb.grid);
    AiCar player={.half_length=2,.half_width=.85f,.height=1.5f};
    int grid=-1;float best=INFINITY;
    for(int k=0;k<w.city.race.ngrid;k++) {
        const float *g=w.city.race.grid[k];float z;
        if(!world_race_grid_ground(&nb.scene,e,g,&z))continue;
        float d=hypotf(g[0]-w.city.race.gate[0].x,g[1]-w.city.race.gate[0].y);
        if(d<best){grid=k;best=d;memcpy(player.pos,g,sizeof player.pos);player.pos[2]=z;}
    }
    float (*walls)[4]=calloc(nb.scene.count,sizeof *walls);
    float (*heights)[2]=calloc(nb.scene.count,sizeof *heights);
    int *sources=calloc(nb.scene.count,sizeof *sources),ok=0;
    N2Path course={0};
    if(grid>=0 && walls && heights && sources) {
        int n=phys_collect_walls(&nb.scene,walls,sources,heights,nb.scene.count);
        phys_wall_index_build(walls,heights,n);
        AiTrafficWorld world={.scene=&nb.scene,.obst=walls,.obstz=heights,.obstsrc=sources,.nobst=n};
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f,.mass=1.4f};
        *spawned=ai_race_prepare(root,e,&w.city.race,&world,&course,cars,drivers,&player);
        int wanted=e->info.downhill?1:e->info.kind==N2_RACE_URL?5:3,driven=*spawned;
        if(e->info.downhill && course.n>1 && drivers[0].drive.path==&course) {
            cars[0]=player;cars[0].head=ai_course_heading(&course,player.pos);driven=1;
        }
        if(driven==wanted) {
            for(int frame=0;frame<90000;frame++) {
                AiCar *contacts[N_RACE_AI];int done=0,failed=0;
                for(int k=0;k<driven;k++) {
                    ai_race_step(cars,drivers,k,driven,e->circuit,&world,NULL);
                    contacts[k]=cars+k;done+=drivers[k].progress.finished;failed+=drivers[k].drive.failed;
                }
                ai_car_contacts(contacts,driven,&world,NULL);
                world_props_step(1/PHYS_TICKRATE);
                *finished=done;
                if(done==driven || failed)break;
            }
            ok=*finished==driven;
            for(int k=0;k<driven;k++)if(!drivers[k].progress.finished)
                printf("EVENT-DETAIL %d car=%d pos=(%.2f %.2f %.2f) gate=%d segment=%d error=%.2f blocked=%d failed=%d drift=%.0f\n",
                    e->id,k,cars[k].pos[0],cars[k].pos[1],cars[k].pos[2],drivers[k].progress.next,
                    drivers[k].drive.segment,drivers[k].drive.error,drivers[k].blocked,
                    drivers[k].drive.failed,drivers[k].progress.drift.bank);
        }
    }
    free(course.xy);free(walls);free(heights);free(sources);
    phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);
    world_neighborhood_free(&nb);world_city_free(&w.city);world_props_reset();return ok;
}
int main(int argc,char **argv) {
    const char *root=argc>1?argv[1]:"../TRACKS";
    int simulate=argc>2 && !strcmp(argv[2],"--drive"),only=argc>3?atoi(argv[3]):0;
    if(simulate)world_collision_init();
    World catalog={0};
    const char *regions[]={"L4RA","L4RB","L4RC","L4RD","L4RF","L4RG"};
    catalog.neighborhood.nreg=6;
    for(int k=0;k<6;k++)snprintf(catalog.neighborhood.rgn[k].name,64,"STREAM%s",regions[k]);
    world_load_events(&catalog,root);
    int count=0,decoded=0,completed=0;
    for(int k=0;k<catalog.city.nev;k++) {
        WEvent *e=catalog.city.ev+k;if(only && e->id!=only)continue;
        N2Path p={0};int route=ai_race_course(root,e,&p),spawned=0,finished=0,ok=0;
        count++;decoded+=route;free(p.xy);
        if(simulate && route)ok=drive(root,e,&spawned,&finished);
        completed+=ok;
        printf("EVENT-CHECK %d %s %s route=%d spawned=%d finished=%d/%d %s\n",
            e->id,e->reg,n2_race_name(e->info.kind),route,spawned,finished,e->info.downhill?1:spawned,
            simulate?(ok?"PASS":"INCOMPLETE"):(route?"DECODED":"UNAVAILABLE"));
        fflush(stdout);
    }
    printf("EVENT-CHECK SUMMARY events=%d decoded=%d completed=%d simulation=%d\n",count,decoded,completed,simulate);
    return !count || (simulate?completed!=count:decoded!=count);
}
