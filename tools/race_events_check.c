/* Optional check against locally installed assets; no game files are fixtures.
 * Course decoding is distinct from completing a race under normal collision.
 * Neutral car dimensions test route feasibility, not every fitted car/model.
 * Finishing a course does not certify drift scoring or URL tournaments. */
#include "ai.h"
#include "world.h"

static int drive(const char *root,const WEvent *e,int *spawned,int *finished,
        const RaceAiSource *source,int difficulty,float pace,int laps) {
    World w={0};
    char region[64];snprintf(region,sizeof region,"STREAM%s",e->reg);
    w.neighborhood.nreg=1;snprintf(w.neighborhood.rgn[0].name,64,"%s",region);
    if(!world_city_load(&w.city,&w.neighborhood,root,region)){world_city_free(&w.city);return 0;}
    int ev=-1;for(int k=0;k<w.city.nev;k++)if(w.city.ev[k].id==e->id)ev=k;
    if(!world_race_start(&w,root,ev,laps)){world_city_free(&w.city);return 0;}
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
        AiTrafficWorld world={.scene=&nb.scene,.obst=walls,.obstz=heights,.obstsrc=sources,.nobst=n,
            .race_source=source,.race_source_choice=-1,.race_difficulty=difficulty,.race_pace=pace};
        /* Five URL rivals still need a separate moving player reference. */
        AiCar cars[N_RACE_AI+1]={0};AiRace drivers[N_RACE_AI+1]={0};
        for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f,.mass=1.4f};
        *spawned=ai_race_prepare(root,e,&w.city.race,&world,&course,cars,drivers,&player);
        int wanted=e->info.downhill?1:e->info.kind==N2_RACE_URL?5:3,driven=*spawned;
        if(e->info.downhill && course.n>1 && drivers[0].drive.path==&course) {
            cars[0]=player;cars[0].head=ai_course_heading(&course,player.pos);driven=1;
        }
        int reference=-1;float low[N_RACE_AI],high[N_RACE_AI]={0};
        for(int k=0;k<N_RACE_AI;k++)low[k]=INFINITY;
        if(source && (driven==3 || driven==5) && drivers[0].source){
            reference=driven;cars[reference]=player;cars[reference].head=ai_course_heading(&course,player.pos);
            drivers[reference].progress=w.city.race;
            if(!ai_drive_join(&drivers[reference].drive,&course,player.pos,cars[reference].head,
                    drivers[0].drive.error_limit)){reference=-1;driven=0;}
            else world.race_player=&drivers[reference].progress;
        }
        if(source && driven && drivers[0].source){
            int selected=race_ai_source_select(source,e->id,driven,-1);
            if(selected>=0)printf("SOURCE-AI event=%d setup=%s difficulty=%d moving-reference=%d\n",
                e->id,source->event[selected].name,difficulty,reference>=0);
        }
        if(driven==wanted) {
            int invalid=0;
            for(int frame=0;frame<90000;frame++) {
                AiCar *contacts[N_RACE_AI+1];int done=0,failed=0;
                for(int k=0;k<driven;k++) {
                    int was_finished=drivers[k].progress.finished;
                    ai_race_step(cars,drivers,k,driven,e->circuit,&world,reference>=0?cars+reference:NULL);
                    if(drivers[k].source){float intent=ai_race_pace(cars+k,drivers+k,k,&world,reference>=0?cars+reference:NULL);
                        low[k]=fminf(low[k],intent);high[k]=fmaxf(high[k],intent);}
                    contacts[k]=cars+k;done+=drivers[k].progress.finished;
                    failed+=!was_finished && drivers[k].drive.failed;
                }
                if(reference>=0){
                    int was_finished=drivers[reference].progress.finished;
                    AiTrafficWorld control=world;control.race_source=NULL;
                    control.race_pace=pace>0?fmaxf(40,pace-12):pace;
                    ai_race_step(cars,drivers,reference,driven+1,e->circuit,&control,NULL);
                    contacts[reference]=cars+reference;
                    failed+=!was_finished && drivers[reference].drive.failed;
                }
                ai_car_contacts(contacts,driven+(reference>=0),&world,NULL);
                world_props_step(1/PHYS_TICKRATE);
                *finished=done;
                invalid|=failed;
                if(done==driven || failed)break;
            }
            ok=!invalid && *finished==driven && (reference<0 || drivers[reference].progress.finished || !drivers[reference].drive.failed);
            int applied=0;
            for(int k=0;k<driven;k++)applied+=drivers[k].source!=NULL;
            printf("EVENT-MODE %d kind=%s laps=%d source-drivers=%d/%d\n",
                e->id,n2_race_name(e->info.kind),w.city.race.maxlaps,applied,driven);
            if(reference>=0)printf("EVENT-REFERENCE %d finished=%d failed=%d lap=%d gate=%d error=%.2f\n",
                e->id,drivers[reference].progress.finished,drivers[reference].drive.failed,
                drivers[reference].progress.lap,drivers[reference].progress.next,drivers[reference].drive.error);
            if(e->info.kind==N2_RACE_DRIFT)for(int k=0;k<driven;k++)
                printf("EVENT-DRIFT %d car=%d bank=%.2f chain=%.2f finished=%d\n",
                    e->id,k,drivers[k].progress.drift.bank,drivers[k].progress.drift.chain,
                    drivers[k].progress.finished);
            if(source && drivers[0].source)for(int k=0;k<driven;k++)printf("SOURCE-DRIVER car=%d skill=%d scale=%.4f catchup=%d intent-range=%.2f..%.2f km/h\n",
                k,drivers[k].source_skill,drivers[k].source_scale,drivers[k].source_catch_up,low[k],high[k]);
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
    int simulate=argc>2 && !strcmp(argv[2],"--drive"),only=0,use_source=0,difficulty=1,opt=3,laps=1;
    float pace=0;
    if(argc>3 && strncmp(argv[3],"--",2)){
        char *end=NULL;long id=strtol(argv[3],&end,10);
        if(!end || *end || id<=0 || id>65535)return 2;
        only=(int)id;opt=4;
    }
    for(int j=opt;j<argc;j++){
        if(!strcmp(argv[j],"--source-ai"))use_source=1;
        else if(!strcmp(argv[j],"--difficulty") && j+1<argc){
            char *end=NULL;long n=strtol(argv[++j],&end,10);if(!end || *end || n<0 || n>2)return 2;difficulty=(int)n;
        }else if(!strcmp(argv[j],"--pace") && j+1<argc){
            char *end=NULL;pace=strtof(argv[++j],&end);if(!end || *end || !isfinite(pace))return 2;
        }else if(!strcmp(argv[j],"--laps") && j+1<argc){
            char *end=NULL;long n=strtol(argv[++j],&end,10);if(!end || *end || n<1 || n>99)return 2;laps=(int)n;
        }else {fprintf(stderr,"Usage: race_events_check TRACKS [--drive [event] [--source-ai] [--difficulty 0..2] [--pace km/h] [--laps 1..99]]\n");return 2;}
    }
    RaceAiSource *source=NULL;
    if(use_source){
        char file[1024];snprintf(file,sizeof file,"%s/../GLOBAL/GLOBALB.BUN",root);
        long n=0;unsigned char *data=n2_read_file(file,&n);source=(RaceAiSource *)malloc(sizeof *source);
        int ok=source && race_ai_source_read(data,n,source);free(data);
        if(!ok){free(source);fprintf(stderr,"GLOBALB source AI settings unavailable\n");return 2;}
    }
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
        if(simulate && route)ok=drive(root,e,&spawned,&finished,source,difficulty,pace,laps);
        completed+=ok;
        printf("EVENT-CHECK %d %s %s route=%d spawned=%d finished=%d/%d %s\n",
            e->id,e->reg,n2_race_name(e->info.kind),route,spawned,finished,e->info.downhill?1:spawned,
            simulate?(ok?"PASS":"INCOMPLETE"):(route?"DECODED":"UNAVAILABLE"));
        fflush(stdout);
    }
    printf("EVENT-CHECK SUMMARY events=%d decoded=%d completed=%d simulation=%d\n",count,decoded,completed,simulate);
    free(source);return !count || (simulate?completed!=count:decoded!=count);
}
