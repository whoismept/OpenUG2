/* Asset-free check: authored grid, shared gates and physics-driven opponents. */
#include "ai.h"
#include "world.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

/* Three scrambled segment runs: explicit links order them into one road. */
static void segmented_file(const char *root,int broken) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Paths1.bin",root);
    enum { N=9, M=3, SIZE=16+8+24*N+8+220*M };
    unsigned char bytes[SIZE]={0};
    uint32_t heads[]={0x80034147,SIZE-8,0x80034147,SIZE-16,0x34148,24*N};
    memcpy(bytes,heads,sizeof heads);
    unsigned char *nodes=bytes+24,*segments=nodes+24*N+8;
    uint32_t sh[]={0x34149,220*M};memcpy(segments-8,sh,8);
    for(int k=0;k<N;k++) {
        float x=k<3?k*40:k<6?(k+3)*40:(k-3)*40;
        if(broken==6 && k==4)x=NAN;
        memcpy(nodes+k*24,&x,4);
        uint16_t fields[]={(uint16_t)(k/3),0xffff,0xffff,0xffff,0xffff};
        memcpy(nodes+k*24+8,fields,sizeof fields);
    }
    uint16_t to=broken==1?N:6;memcpy(nodes+2*24+12,&to,2);
    to=3;memcpy(nodes+8*24+12,&to,2);
    if(broken==7){to=0xffff;memcpy(nodes+2*24+12,&to,2);}
    if(broken==3){to=0;memcpy(nodes+4*24+8,&to,2);}
    for(int k=0;k<M;k++) {
        uint16_t range[]={(uint16_t)(3*k),(uint16_t)(3*k+3),3};
        if(broken==2 && k==1)range[2]=2;
        if(broken==4 && k==2){range[0]=3;range[1]=6;}
        memcpy(segments+k*220+28,range,sizeof range);
    }
    if(broken==5){uint32_t size=SIZE;memcpy(bytes+4,&size,4);}
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,SIZE,f)==SIZE);assert(!fclose(f));
}

static void route_file(const char *root,int broken) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Paths1.bin",root);
    unsigned char bytes[8+5*24]={0};uint32_t type=0x34148,size=5*24;
    memcpy(bytes,&type,4);memcpy(bytes+4,&size,4);
    for(int k=0;k<5;k++) {float x=broken && k==2?500:k*50;memcpy(bytes+8+k*24,&x,4);}
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(!fclose(f));
}
/* The same nested road-record format used by traffic, with event distances. */
static void lane_file(const char *root,int broken) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    enum { PAYLOAD=8+128+5*56+4 };
    unsigned char bytes[16+PAYLOAD]={0};
    uint32_t container=0x80034120,size=8+PAYLOAD,type=0x34121,payload=PAYLOAD,pair=5|(5<<16);
    memcpy(bytes,&container,4);memcpy(bytes+4,&size,4);
    memcpy(bytes+8,&type,4);memcpy(bytes+12,&payload,4);memcpy(bytes+24+44,&pair,4);
    float points[][3]={{0,0,0},{35,-9,35},{90,-9,90},{120,0,120},{200,0,200}};
    if(broken==2)points[2][2]=NAN;
    if(broken==3)points[2][2]=1e7f; /* outside the accepted distance range */
    if(broken==5)for(int k=0;k<5;k++)points[k][1]-=20; /* cannot join from the grid */
    for(int k=0;k<5;k++)memcpy(bytes+24+128+k*56+4,points[broken==4?4-k:k],sizeof points[k]);
    FILE *f=fopen(file,"wb");assert(f);
    size_t n=sizeof bytes-(broken==1?8:0);assert(fwrite(bytes,1,n,f)==n);assert(!fclose(f));
}
/* Nearby branch endpoints reset event progress; they cannot replace the
   real 200m course with a 1m interpolation of all intermediate distances. */
static void progress_reset_file(const char *root,int reverse) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    enum { PAYLOAD=8+(140+5*56)+(140+2*56)-8 };
    unsigned char bytes[16+PAYLOAD]={0};
    uint32_t head[]={0x80034120,8+PAYLOAD,0x34121,PAYLOAD};memcpy(bytes,head,16);
    for(int record=0;record<2;record++) {
        int n=record?2:5;unsigned char *b=bytes+24+(record?140+5*56:0);
        uint32_t pair=n|(n<<16);memcpy(b+44,&pair,4);
        for(int k=0;k<n;k++) {
            float point[]={record?100.0f+k:50.0f*k,0,record?200.0f*(reverse?1-k:k):50.0f*k};
            memcpy(b+128+56*k+4,point,sizeof point);
        }
    }
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(!fclose(f));
}

static void loop_file(const char *root,int alternatives) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    enum { N=65, RECORD=140+N*56 };
    int PAYLOAD=8+alternatives*RECORD-8;
    unsigned char bytes[16+PAYLOAD];memset(bytes,0,sizeof bytes);uint32_t head[]={0x80034120,8+PAYLOAD,0x34121,PAYLOAD};
    memcpy(bytes,head,16);uint32_t pair=N|(N<<16);
    for(int lane=0;lane<alternatives;lane++) {
    unsigned char *record=bytes+24+lane*RECORD;memcpy(record+44,&pair,4);
    for(int k=0;k<N;k++) {
        float angle=6.283185307f*k/(N-1),length=251.0f;
        float radius=40+10*lane;
        float point[]={100+radius*cosf(angle),radius*sinf(angle),length*k/(N-1)};
        /* The final edge crosses the authored distance wrap. */
        if(k==N-1)point[2]=0;
        memcpy(record+128+k*56+4,point,sizeof point);
        uint16_t widths[]={16*256,20*256};memcpy(record+128+k*56+24,widths,sizeof widths);
    }
    }
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(!fclose(f));
}
/* Synthetic source lanes, including distinct height layers at the same XY. */
static void tight_street_file(const char *root,float height,int mirror,int alternatives) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    enum { N=65, RECORD=140+N*56 };
    int payload=alternatives*RECORD;unsigned char bytes[16+payload];memset(bytes,0,sizeof bytes);
    uint32_t head[]={0x80034120,8+payload,0x34121,payload},pair=N|(N<<16);
    memcpy(bytes,head,sizeof head);
    for(int lane=0;lane<alternatives;lane++) {
        unsigned char *record=bytes+24+lane*RECORD;memcpy(record+44,&pair,4);
        for(int i=0;i<N;i++) {
            float at=5*i,x=at<=100?at:at<=160?100:at<=260?260-at:0;
            float y=at<=100?0:at<=160?at-100:at<=260?60:320-at;
            float point[]={x,mirror?-y:y,i==N-1?0:at};memcpy(record+128+i*56+4,point,sizeof point);
            uint16_t widths[]={5*256,5*256};memcpy(record+128+i*56+24,widths,sizeof widths);
            uint32_t z=(uint32_t)(uint16_t)(int16_t)((height+5*lane)*8)<<16;memcpy(record+128+i*56+20,&z,4);
        }
    }
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(!fclose(f));
}

static void street_turn_course_test(void) {
    char root[]="/tmp/openug2-street-turn-XXXXXX";assert(mkdtemp(root));char folder[512];
    snprintf(folder,sizeof folder,"%s/ROUTESTEST",root);assert(!mkdir(folder,0700));tight_street_file(root,0,0,1);
    float floor[]={-30,-90,0,0,0,130,-90,0,0,0,130,90,0,0,0,-30,90,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh meshes[2]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD}};
    N2Scene scene={meshes,1,2};float bounds[][4]={{-30,-90,130,90},{80,8,98,8}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    AiTrafficWorld world={.scene=&scene,.race_pace=110};phys_wall_index_build(NULL,NULL,0);
    WEvent event={.id=1,.circuit=1,.reg="TEST",.npoly=5,.poly={{0,0},{100,0},{100,60},{0,60},{0,0}},
        .info={.kind=N2_RACE_STREETX,.length=320}};
    WRace seed={.active=1,.kind=N2_RACE_STREETX,.maxlaps=2,.ngate=4,.ngrid=4,.grid_named=1,.grid_heading=0,
        .gate={{.x=10,.half=12},{.x=100,.y=30,.half=12},{.x=50,.y=60,.half=12},{.y=30,.half=12}},
        .grid={{20,-3,0},{20,0,0},{20,3,0},{20,6,0}}};
    AiCar player={.pos={20,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    AiCar cars[N_RACE_AI];AiRace drivers[N_RACE_AI];N2Path path={0};WRace race=seed;
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    WRace unnamed=seed;unnamed.grid_named=0;unnamed.ngrid=3;
    const float oldgrid[][3]={{8,-3,0},{8,3,0},{2,0,0}};memcpy(unnamed.grid,oldgrid,sizeof oldgrid);
    assert(ai_race_prepare(root,&event,&unnamed,&world,&path,cars,drivers,&player)==3);
    for(int k=0;k<3;k++)assert(!drivers[k].drive.failed); /* the exact grid-entry bound still applies */
    race=seed;player=(AiCar){.pos={20,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    assert(ai_race_prepare(root,&event,&race,&world,&path,cars,drivers,&player)==3);
    float nearest=INFINITY;
    for(int i=0;i<path.n;i++)nearest=fminf(nearest,hypotf(path.xy[2*i]-100,path.xy[2*i+1]));
    fprintf(stderr,"Street X sharp corner replaced: nearest source apex %.3f m\n",nearest);
    assert(nearest>.05f && ai_drive_route_valid(&path));
    int done=0;
    for(int tick=0;tick<18000 && done<3;tick++) {
        done=0;AiCar *contacts[3];
        for(int k=0;k<3;k++){ai_race_step(cars,drivers,k,3,1,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;
            if(drivers[k].drive.failed || cars[k].ride.contact_mask!=15)fprintf(stderr,"Street X failure tick%d car%d xy%.3f,%.3f segment%d gate%d lap%d error%.3f blocked%d wheels%u\n",tick,k,cars[k].pos[0],cars[k].pos[1],drivers[k].drive.segment,drivers[k].progress.next,drivers[k].progress.lap,drivers[k].drive.error,drivers[k].blocked,cars[k].ride.contact_mask);
            assert(!drivers[k].drive.failed && cars[k].ride.contact_mask==15);}
        ai_car_contacts(contacts,3,&world,NULL);
    }
    assert(done==3);
    float wall[]={80,8,0,0,0,98,8,0,0,0,98,8,4,0,0,80,8,4,0,0};
    meshes[1]=(N2Mesh){.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL};scene.count=2;
    float obs[2][4],oz[2][2];int sources[2];world.nobst=phys_collect_walls(&scene,obs,sources,oz,2);
    world.obst=obs;world.obstz=oz;world.obstsrc=sources;phys_wall_index_build(obs,oz,world.nobst);
    race=seed;player=(AiCar){.pos={20,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    assert(ai_race_prepare(root,&event,&race,&world,&path,cars,drivers,&player)==3);
    for(int i=0;i+1<path.n;i++) {
        float dx=path.xy[2*i+2]-path.xy[2*i],dy=path.xy[2*i+3]-path.xy[2*i+1],h=atan2f(dy,dx);
        int steps=(int)ceilf(hypotf(dx,dy)*2);
        for(int q=0;q<=steps;q++) {
            float t=(float)q/steps,pose[]={path.xy[2*i]+t*dx,path.xy[2*i+1]+t*dy,0},vel[2]={0};
            float body[]={-2,-.85f,0,2,.85f,1.5f};
            assert(!collide_body_walls_preview(pose,vel,h,body,obs,oz,world.nobst,.05f,1.5f,&scene,sources));
        }
    }
    /* Opposite turn direction on an upper road: the lower deck must not
       attract source-height samples or the physical field. */
    phys_wall_index_build(NULL,NULL,0);world.nobst=0;world_ground_grid_free(&grid);
    float roof[20];memcpy(roof,floor,sizeof roof);for(int i=0;i<4;i++)roof[5*i+2]=5;
    meshes[1]=(N2Mesh){.verts=roof,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    memcpy(bounds[1],bounds[0],sizeof bounds[0]);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    tight_street_file(root,5,1,1);WEvent mirrored=event;
    for(int i=0;i<mirrored.npoly;i++)mirrored.poly[i][1]=-mirrored.poly[i][1];
    race=seed;for(int i=0;i<race.ngate;i++){race.gate[i].y=-race.gate[i].y;race.gate[i].dy=-race.gate[i].dy;}
    for(int i=0;i<race.ngrid;i++){race.grid[i][1]=-race.grid[i][1];race.grid[i][2]=5;}
    player=(AiCar){.pos={20,0,5},.half_length=2,.half_width=.85f,.height=1.5f};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    assert(ai_race_prepare(root,&mirrored,&race,&world,&path,cars,drivers,&player)==3);
    nearest=INFINITY;for(int i=0;i<path.n;i++)nearest=fminf(nearest,hypotf(path.xy[2*i]-100,path.xy[2*i+1]));
    assert(nearest>.05f);done=0;
    for(int tick=0;tick<18000 && done<3;tick++) {
        done=0;AiCar *contacts[3];
        for(int k=0;k<3;k++){ai_race_step(cars,drivers,k,3,1,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;assert(!drivers[k].drive.failed && cars[k].ride.contact_mask==15 && fabsf(cars[k].pos[2]-5)<.05f);}
        ai_car_contacts(contacts,3,&world,NULL);
    }
    assert(done==3);
    /* Identical XY with conflicting source heights is intentionally left
       unsmoothed; borrowing the nearest visible floor would hide ambiguity. */
    tight_street_file(root,0,0,2);race=seed;
    player=(AiCar){.pos={20,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    assert(ai_race_prepare(root,&event,&race,&world,&path,cars,drivers,&player)==3);
    nearest=INFINITY;for(int i=0;i<path.n;i++)nearest=fminf(nearest,hypotf(path.xy[2*i]-100,path.xy[2*i+1]));
    assert(nearest<.05f);
    /* Slightly separated concentric records must remain ambiguous in every
       record order, even when a nearer record follows a conflicting floor. */
    int order[]={0,1,2,3},permutations=0;
    do {
        tight_street_file(root,0,0,4);
        char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
        long bytes=0;unsigned char *data=n2_read_file(file,&bytes);assert(data);
        const float gaps[]={.015f,.006f,.004f,0},levels[]={0,5,0,0};
        for(int lane=0;lane<4;lane++)for(int q=0;q<65;q++) {
            unsigned char *point=data+24+lane*(140+65*56)+128+q*56;
            float x,y,gap=gaps[order[lane]];memcpy(&x,point+4,4);memcpy(&y,point+8,4);
            if(x==0)x-=gap;else if(x==100)x+=gap;
            if(y==0)y-=gap;else if(y==60)y+=gap;
            memcpy(point+4,&x,4);memcpy(point+8,&y,4);
            uint32_t z=(uint32_t)(uint16_t)(int16_t)(levels[order[lane]]*8)<<16;memcpy(point+20,&z,4);
        }
        FILE *out=fopen(file,"wb");assert(out);assert(fwrite(data,1,(size_t)bytes,out)==(size_t)bytes);assert(!fclose(out));free(data);
        WEvent fallback=event;fallback.info.length=10000; /* exercise the source-topology course */
        race=seed;player=(AiCar){.pos={20,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
        for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
        assert(ai_race_prepare(root,&fallback,&race,&world,&path,cars,drivers,&player)==3);
        nearest=INFINITY;for(int q=0;q<path.n;q++)nearest=fminf(nearest,hypotf(path.xy[2*q]-100,path.xy[2*q+1]));
        fprintf(stderr,"Street X ambiguous order%d%d%d%d apex %.6f\n",order[0],order[1],order[2],order[3],nearest);
        assert(nearest<.05f);permutations++;
        int j=2;while(j>=0 && order[j]>order[j+1])j--;if(j<0)break;
        int k=3;while(order[k]<order[j])k--;int tmp=order[j];order[j]=order[k];order[k]=tmp;
        for(int l=j+1,r=3;l<r;l++,r--){tmp=order[l];order[l]=order[r];order[r]=tmp;}
    } while(1);
    assert(permutations==24);

    free(path.xy);phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    snprintf(folder,sizeof folder,"%s/ROUTESTEST/Routes1F.bin",root);assert(!unlink(folder));
    snprintf(folder,sizeof folder,"%s/ROUTESTEST",root);assert(!rmdir(folder));assert(!rmdir(root));
}


/* Different steering/grip profiles must retain a feasible lane and pace together. */
static void street_turn_pace_test(void) {
    char root[]="/tmp/openug2-street-pace-XXXXXX";assert(mkdtemp(root));char file[512];
    snprintf(file,sizeof file,"%s/ROUTESTEST",root);assert(!mkdir(file,0700));tight_street_file(root,0,0,1);
    float floor[]={-40,-80,0,0,0,140,-80,0,0,0,140,100,0,0,0,-40,100,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};float bounds[][4]={{-40,-80,140,100}};WGroundGrid grid={0};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);phys_wall_index_build(NULL,NULL,0);
    WEvent event={.id=1,.circuit=1,.reg="TEST",.npoly=5,.poly={{0,0},{100,0},{100,60},{0,60},{0,0}},.info={.kind=N2_RACE_STREETX,.length=320}};
    WRace seed={.active=1,.kind=N2_RACE_STREETX,.maxlaps=2,.ngate=4,.ngrid=4,.grid_named=1,.grid_heading=0,
        .gate={{.x=10,.half=12},{.x=100,.y=30,.half=12},{.x=50,.y=60,.half=12},{.y=30,.half=12}},
        .grid={{20,-3,0},{20,0,0},{20,3,0},{20,6,0}}};
    const PhysVehicle profiles[]={{1.25f,1,.95f,1.1f,1,1},{1.1f,1,1.1f,.9f,1,1}};
    const float paces[]={50,110,-1};N2Path path={0};
    for(int variant=0;variant<2;variant++)for(int pace=0;pace<3;pace++) {
        AiCar cars[N_RACE_AI];AiRace drivers[N_RACE_AI];
        for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2.2f,.half_width=.95f,.height=1.4f,.mass=1.4f,.vehicle=profiles[variant]};
        AiCar player={.pos={20,0,0},.half_length=2.2f,.half_width=.95f,.height=1.4f};
        AiTrafficWorld world={.scene=&scene,.race_pace=paces[pace]};WRace race=seed;
        assert(ai_race_prepare(root,&event,&race,&world,&path,cars,drivers,&player)==3);
        int done=0,failed=0,tick;
        for(tick=0;tick<18000 && done<3 && !failed;tick++) {
            done=0;AiCar *contacts[3];
            for(int k=0;k<3;k++){ai_race_step(cars,drivers,k,3,1,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;failed|=drivers[k].drive.failed;assert(cars[k].ride.contact_mask==15);}
            ai_car_contacts(contacts,3,&world,NULL);
        }
        fprintf(stderr,"Street X pace variant%d pace%.0f ticks%d finished%d failed%d\n",variant,paces[pace],tick,done,failed);
        if(failed || done!=3)for(int k=0;k<3;k++)fprintf(stderr,"  car%d xy%.3f,%.3f segment%d gate%d lap%d error%.3f blocked%d\n",k,cars[k].pos[0],cars[k].pos[1],drivers[k].drive.segment,drivers[k].progress.next,drivers[k].progress.lap,drivers[k].drive.error,drivers[k].blocked);
        assert(!failed && done==3);
    }
    free(path.xy);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);assert(!unlink(file));snprintf(file,sizeof file,"%s/ROUTESTEST",root);assert(!rmdir(file));assert(!rmdir(root));
}

/* Independent lane chains form a loop even when their branch distances do
   not form a lap. One join reverses 1m; another needs its recorded 10m width. */
static void junction_loop_file(const char *root) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    enum { RECORD=140+3*56, PAYLOAD=4*RECORD, SIZE=16+PAYLOAD };
    unsigned char bytes[SIZE]={0};uint32_t heads[]={0x80034120,8+PAYLOAD,0x34121,PAYLOAD};
    memcpy(bytes,heads,sizeof heads);
    const float xy[4][3][2]={{{0,0},{50,0},{100,0}},{{99,0},{108,50},{108,100}},
        {{116,100},{50,100},{0,100}},{{0,100},{0,50},{0,0}}};
    for(int r=0;r<4;r++) {
        unsigned char *record=bytes+24+r*RECORD;uint32_t pair=3|(3<<16);memcpy(record+44,&pair,4);
        for(int k=0;k<3;k++) {
            memcpy(record+128+56*k+4,xy[r][k],sizeof xy[r][k]);
            uint16_t widths[]={5*256,5*256};memcpy(record+128+56*k+24,widths,sizeof widths);
        }
    }
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(!fclose(f));
}
static void event_metadata(void) {
    unsigned char bytes[8+296]={0};uint32_t h[]={0x34201,296};memcpy(bytes,h,8);
    unsigned char *b=bytes+8;memcpy(b,"Synthetic course",17);
    uint32_t id=17,length=251;memcpy(b+140,&id,4);memcpy(b+152,&length,4);
    unsigned flags[]={1,4,2,8,2048,8192};
    for(int k=0;k<6;k++) {
        memcpy(b+148,flags+k,4);N2EventInfo info={0};
        assert(n2_event_info(bytes,sizeof bytes,17,&info));
        assert(info.kind==k+1 && info.length==251 && !strcmp(info.name,"Synthetic course"));
    }
    N2EventInfo info={0};assert(!n2_event_info(bytes,sizeof bytes-1,17,&info));
    assert(!n2_event_info(bytes,sizeof bytes,18,&info));
    memset(b,'x',64);assert(!n2_event_info(bytes,sizeof bytes,17,&info));
}
/* Exercise the display contract at mismatched rates. The same fixed ticks
   remain authoritative while rendered movement has no repeated/stair steps. */
static void render_turn_timing(void) {
    const int cadence[]={144,30,240,20,60,120};
    for(int reverse=0;reverse<2;reverse++) {
        const float rate=reverse?-.6f:.6f,slip=reverse?-.4f:.4f;
        PhysClock clock={0};double elapsed=0;int ticks=0,zero=0,multiple=0;
        AiCar now={.head=3.12f},before,draw;
        now.vel[0]=cosf(now.head+slip)*61/PHYS_TICKRATE;
        now.vel[1]=sinf(now.head+slip)*61/PHYS_TICKRATE;before=now;
        float worst=0;
        for(int frame=0;frame<600;frame++) {
            double dt=1.0/cadence[frame%6];elapsed+=dt;
            int steps=phys_clock_steps(&clock,dt);zero+=steps==0;multiple+=steps>1;
            for(int k=0;k<steps;k++) {
                before=now;ticks++;
                float angle=3.12f+rate*ticks/PHYS_TICKRATE;
                now.head=atan2f(sinf(angle),cosf(angle));
                now.vel[0]=cosf(angle+slip)*61/PHYS_TICKRATE;
                now.vel[1]=sinf(angle+slip)*61/PHYS_TICKRATE;
            }
            AiCar saved_before=before,saved_now=now;
            ai_render_pose(&draw,&before,&now,(float)(clock.remainder*PHYS_TICKRATE));
            float aim=draw.head+.6f*atan2f(sinf(atan2f(draw.vel[1],draw.vel[0])-draw.head),
                                         cosf(atan2f(draw.vel[1],draw.vel[0])-draw.head));
            float expected=3.12f+rate*(float)fmax(0,elapsed-1/PHYS_TICKRATE)+.6f*slip;
            worst=fmaxf(worst,fabsf(atan2f(sinf(aim-expected),cosf(aim-expected))));
            assert(!memcmp(&before,&saved_before,sizeof before) && !memcmp(&now,&saved_now,sizeof now));
        }
        printf("drift display mixed20..240FPS turn%d: error%.8frad zero%d multi%d\n",reverse,worst,zero,multiple);fflush(stdout);
        assert(worst<.00001f && zero>0 && multiple>0 && ticks==(int)(elapsed*PHYS_TICKRATE+.000001));
    }
}

static void render_timing(void) {
    const int rates[]={10,30,60,120,144,240};
    for(int r=0;r<6;r++) {
        PhysClock clock={0};AiCar before={0},now={0},draw={0};int ticks=0;
        float last=0;int repeated=0;
        for(int frame=1;frame<=5*rates[r];frame++) {
            int steps=phys_clock_steps(&clock,1.0/rates[r]);
            for(int k=0;k<steps;k++) {
                before=now;ticks++;
                now.pos[0]=(float)(ticks*10.0/PHYS_TICKRATE);
            }
            AiCar a=before,b=now;
            ai_render_pose(&draw,&before,&now,(float)(clock.remainder*PHYS_TICKRATE));
            float expected=(float)(fmax(0,frame/(double)rates[r]-1.0/PHYS_TICKRATE)*10);
            assert(fabsf(draw.pos[0]-expected)<.00002f);
            if(frame>(int)ceilf(rates[r]/PHYS_TICKRATE)+1) {
                assert(fabsf(draw.pos[0]-last-10.0f/rates[r])<.00002f);
                repeated+=draw.pos[0]==last;
            }
            assert(!memcmp(&a,&before,sizeof a) && !memcmp(&b,&now,sizeof b));
            last=draw.pos[0];
        }
        assert(ticks==300 && repeated==0);
        printf("render timing %d FPS: %d fixed ticks, no repeated pose frames\n",rates[r],ticks);
    }
    /* Drift view direction must use velocity at the displayed tick fraction,
       not the future velocity of the latest simulation tick. */
    AiCar va={.vel={.3f,0}},vb={.vel={0,.3f}},vd;
    ai_render_pose(&vd,&va,&vb,.5f);
    assert(fabsf(atan2f(vd.vel[1],vd.vel[0])-.78539816f)<.000001f);
    ai_render_pose(&vd,&va,&vb,0);assert(!memcmp(vd.vel,va.vel,sizeof va.vel));
    ai_render_pose(&vd,&va,&vb,1);assert(!memcmp(vd.vel,vb.vel,sizeof vb.vel));
    vb.vel[0]=-.3f;vb.vel[1]=0;ai_render_pose(&vd,&va,&vb,.5f);
    assert(vd.vel[0]==0 && vd.vel[1]==0); /* a reversal stays finite at rest */
    AiCar a={.head=3.12f,.wheel_angle=-4.25f},b={.head=-3.12f,.wheel_angle=.25f},draw;
    b.pos[2]=b.ride.z=1;for(int k=0;k<4;k++)b.ride.compression[k]=-1;
    a.wheel_steer[0]=.1f;a.wheel_steer[1]=.2f;b.wheel_steer[0]=.3f;b.wheel_steer[1]=.4f;
    ai_render_pose(&draw,&a,&b,.5f);
    assert(fabsf(draw.wheel_steer[0]-.2f)<1e-6f&&fabsf(draw.wheel_steer[1]-.3f)<1e-6f);
    assert(fabsf(fabsf(draw.head)-3.14159265f)<.00001f);
    assert(draw.wheel_angle==-2); /* full angular travel, not a reverse short arc */
    for(int k=0;k<4;k++)assert(draw.pos[2]+draw.ride.compression[k]==0);
    ai_render_pose(&draw,&a,&b,1);assert(!memcmp(&draw,&b,sizeof b));
    ai_render_pose(&draw,&b,&b,.3f);assert(!memcmp(&draw,&b,sizeof b)); /* reset/teleport snaps */
    ai_render_pose(&a,&a,&b,.5f);assert(a.pos[2]==.5f); /* alias-safe output */
    puts("render interpolation: timing, heading wrap, fast wheel phases, contact travel, reset and unchanged simulation PASS");
}

/* A narrow corner must be driven through under normal contact physics.
   The wider case preserves the previously usable straight approach. */
/* Live lookahead must follow the same rising overlap as physical tyre contacts. */
static void rising_overlap_test(void) {
    for(int sign=-1;sign<=1;sign+=2) {
        float floor[]={-30,-25,0,0,0,140,-25,0,0,0,140,25,0,0,0,-30,25,0,0,0};
        float ramp[]={25,-25,0,0,0,35,-25,1.2f,0,0,35,25,1.2f,0,0,25,25,0,0,0};
        float deck[]={35,-25,1.2f,0,0,140,-25,1.2f,0,0,140,25,1.2f,0,0,35,25,1.2f,0,0};
        float skirt[]={35,-25,-1,0,0,35,25,-1,0,0,35,25,1.2f,0,0,35,-25,1.2f,0,0};
        uint16_t idx[]={0,1,2,0,2,3};
        N2Mesh meshes[]={{.verts=floor,.cat=N2_ROAD},{.verts=ramp,.cat=N2_TERRAIN},
                        {.verts=deck,.cat=N2_ROAD},{.verts=skirt,.cat=N2_TERRAIN}};
        for(int m=0;m<4;m++) {
            meshes[m].nverts=4;meshes[m].idx=idx;meshes[m].nidx=6;
            for(int k=0;k<4;k++)meshes[m].verts[5*k]*=sign;
        }
        N2Scene scene={meshes,4,4};
        float bounds[4][4];for(int m=0;m<4;m++) {
            float lo=INFINITY,hi=-INFINITY;
            for(int k=0;k<4;k++){lo=fminf(lo,meshes[m].verts[5*k]);hi=fmaxf(hi,meshes[m].verts[5*k]);}
            bounds[m][0]=lo;bounds[m][1]=-25;bounds[m][2]=hi;bounds[m][3]=25;
        }
        WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
        AiTrafficWorld world={.scene=&scene};
        float xy[]={-20*sign,0,20*sign,0,60*sign,0,100*sign,0,120*sign,0};N2Path path={.xy=xy,.n=5};
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        for(int k=0;k<3;k++) {
            cars[k]=(AiCar){.pos={sign*(float)(6*k),3.f*(k-1),0},.head=sign>0?0:3.14159265f,.half_length=2,.half_width=.85f,.height=1.5f};
            drivers[k].progress=(WRace){.active=1,.maxlaps=1,.ngate=3,.gate={{.x=-10*sign,.dx=sign,.half=25},{.x=50*sign,.dx=sign,.half=25},{.x=95*sign,.dx=sign,.half=25}}};
            assert(ai_drive_init(&drivers[k].drive,&path,cars[k].pos,cars[k].head));
            drivers[k].offset=ai_course_lateral(&path,cars[k].pos);
        }
        for(int tick=0;tick<2400;tick++) {
            int done=0;AiCar *contacts[3];
            for(int k=0;k<3;k++){ai_race_step(cars,drivers,k,3,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;}
            ai_car_contacts(contacts,3,&world,NULL);if(done==3)break;
        }
        for(int k=0;k<3;k++) {
            printf("rising overlap sign%d car%d x%.4f z%.4f complete%d failed%d blocked%d contacts%u\n",sign,k,cars[k].pos[0],cars[k].pos[2],drivers[k].progress.finished,drivers[k].drive.failed,drivers[k].blocked,cars[k].ride.contact_mask);fflush(stdout);
            assert(drivers[k].progress.finished && !drivers[k].drive.failed && cars[k].ride.contact_mask==15 && fabsf(cars[k].pos[2]-1.2f)<.01f);
        }
        world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
}

static void corner_wall_test(void) {
    for(int sign=-1;sign<=1;sign+=2)for(int width=0;width<2;width++) {
        float gap=width?1.6f:1.4f;
        float floor[]={-gap,-10,0,0,0,gap,-10,0,0,0,gap,5,0,0,0,-gap,5,0,0,0, -gap,5,0,0,0,100,5,0,0,0,100,100,0,0,0,-gap,100,0,0,0};
        float wall[]={-gap,-10,0,0,0,-gap,5,0,0,0,-gap,5,4,0,0,-gap,-10,4,0,0};
        uint16_t idx[]={0,1,2,0,2,3},flooridx[]={0,1,2,0,2,3,4,5,6,4,6,7};
        N2Mesh meshes[]={{.verts=floor,.nverts=8,.idx=flooridx,.nidx=12,.cat=N2_ROAD},
            {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
        for(int k=0;k<8;k++)floor[5*k]*=sign;for(int k=0;k<4;k++)wall[5*k]*=sign;
        N2Scene scene={meshes,2,2};WGroundGrid grid={0};float bounds[][4]={{sign>0?-gap:-100,-10,sign>0?100:gap,100},{-sign*gap,-10,-sign*gap,5}};
        assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
        float obs[2][4],oz[2][2];int src[2];int n=phys_collect_walls(&scene,obs,src,oz,2);
        phys_wall_index_build(obs,oz,n);
        AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
        float xy[]={0,0,0,4,4,8,30,8};for(int k=0;k<4;k++)xy[2*k]*=sign;N2Path path={.xy=xy,.n=4};
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={0,3,0},.head=1.5707963f,.half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0].progress=(WRace){.active=1,.maxlaps=1,.ngate=3,.gate={{.dy=1,.half=10},{.x=sign*4,.y=8,.dx=sign,.half=10},{.x=sign*25,.y=8,.dx=sign,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,cars[0].head));
        for(int t=0;t<2400 && !drivers[0].progress.finished && !drivers[0].drive.failed;t++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            float probe[3];memcpy(probe,cars[0].pos,sizeof probe);
            float v[2]={0},bb[]={-2,-.85f,0,2,.85f,1.5f};
            collide_body_walls(probe,v,cars[0].head,bb,obs,oz,n,
                probe[2]+.05f,probe[2]+1.5f,&scene,src,NULL,0);
            assert(hypotf(probe[0]-cars[0].pos[0],probe[1]-cars[0].pos[1])<.001f);
        }
        assert(drivers[0].progress.finished && !drivers[0].drive.failed && cars[0].ride.contact_mask==15);
        phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
}

/* A body beside a wall must translate away while turning, rather than
   being previewed at the final yaw immediately. Wider clearance still works. */
static void wall_turn_recovery_test(void) {
    for(int sign=-1;sign<=1;sign+=2)for(int wider=0;wider<2;wider++) {
        float gap=wider?1.02f:.9f;
        float floor[]={-100,-5,0,0,0,100,-5,0,0,0,100,100,0,0,0,-100,100,0,0,0};
        float wall[]={-100,-gap,0,0,0,10,-gap,0,0,0,10,-gap,4,0,0,-100,-gap,4,0,0};
        uint16_t idx[]={0,1,2,0,2,3};
        N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},{.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
        for(int k=0;k<4;k++){floor[5*k+1]*=sign;wall[5*k+1]*=sign;}
        N2Scene scene={meshes,2,2};WGroundGrid grid={0};float bounds[][4]={{-100,sign>0?-5:-100,100,sign>0?100:5},{-100,-sign*gap,10,-sign*gap}};
        assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
        float obs[2][4],oz[2][2];int src[2];int n=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,n);
        AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
        float xy[]={-10,0,0,0,2,6,20,20,20,60};for(int k=0;k<5;k++)xy[2*k+1]*=sign;N2Path path={.xy=xy,.n=5};AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.head=sign<0?6.2831853f:0,.half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0].progress=(WRace){.active=1,.maxlaps=1,.ngate=3,.gate={{.x=-10,.dx=1,.half=10},{.x=15,.y=15,.dy=1,.half=20},{.x=20,.y=50,.dy=1,.half=10}}};
        for(int k=0;k<3;k++){drivers[0].progress.gate[k].y*=sign;drivers[0].progress.gate[k].dy*=sign;}
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,cars[0].head));
        for(int t=0;t<2400 && !drivers[0].progress.finished && !drivers[0].drive.failed;t++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            float probe[3];memcpy(probe,cars[0].pos,sizeof probe);float v[2]={0},bb[]={-2,-.85f,0,2,.85f,1.5f};
            collide_body_walls(probe,v,cars[0].head,bb,obs,oz,n,probe[2]+.05f,probe[2]+1.5f,&scene,src,NULL,0);
            assert(hypotf(probe[0]-cars[0].pos[0],probe[1]-cars[0].pos[1])<.001f);
        }
        assert(drivers[0].progress.finished && !drivers[0].drive.failed && cars[0].ride.contact_mask==15);
        phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
}

/* Race lanes use the fitted wheels for support. Body overhangs can cross
   a verge, while solid collision still checks the full vehicle. */
static void parallel_lane_file(const char *root) {
    segmented_file(root,0);
    enum { N=5, RECORD=140+56*N, SIZE=16+2*RECORD };
    unsigned char bytes[SIZE]={0};uint32_t heads[]={0x80034120,SIZE-8,0x34121,SIZE-16};memcpy(bytes,heads,16);
    for(int lane=0;lane<2;lane++) {
        unsigned char *record=bytes+24+lane*RECORD;uint32_t pair=N|(N<<16);memcpy(record+44,&pair,4);
        for(int k=0;k<N;k++) {
            float point[]={-20.0f+20*k,20.0f*lane,20.0f*k};memcpy(record+128+k*56+4,point,sizeof point);
            uint16_t widths[]={32*256,32*256};memcpy(record+128+k*56+24,widths,4);
        }
    }
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,SIZE,f)==SIZE);assert(!fclose(f));
}

static void lane_heights(const char *root,float along,float across,float base) {
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    FILE *f=fopen(file,"r+b");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);
    unsigned char *data=malloc((size_t)size);assert(data);rewind(f);assert(fread(data,1,(size_t)size,f)==(size_t)size);
    for(long at=24;at<size;) {
        unsigned count=(unsigned)data[at+44]|(unsigned)data[at+45]<<8;
        for(unsigned j=0;j<count;j++) {
            unsigned char *p=data+at+128+56*j;float x,y;memcpy(&x,p+4,4);memcpy(&y,p+8,4);
            int16_t z=(int16_t)lroundf(8*(along*x+across*y+base));
            p[22]=(unsigned char)z;p[23]=(unsigned char)((uint16_t)z>>8);
        }
        at+=140+56*count;
    }
    rewind(f);assert(fwrite(data,1,(size_t)size,f)==(size_t)size);free(data);assert(!fclose(f));
}

/* Prefer the physically supported route whose authored height matches the
   grid's deck over a closer XY chain authored for an unreachable upper road. */
static void authored_layer_test(const char *root) {
    for(int below=0;below<2;below++) {
    float base=below?-12.25f:0;
    parallel_lane_file(root);lane_heights(root,0,-.5f,base+10);
    float lower[]={-50,-25,0,0,0,110,-25,0,0,0,110,25,0,0,0,-50,25,0,0,0};
    for(int k=0;k<4;k++)lower[5*k+2]=base;
    float upper[20];memcpy(upper,lower,sizeof upper);for(int k=0;k<4;k++)upper[5*k+2]=base+10;
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=lower,.cat=N2_ROAD},{.verts=upper,.cat=N2_ROAD}};
    for(int k=0;k<2;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
    N2Scene scene={meshes,2,2};float bounds[][4]={{-50,-25,110,25},{-50,-25,110,25}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);AiTrafficWorld world={.scene=&scene};
    AiCar cars[N_RACE_AI]={0},player={.pos={-30,0,base},.half_length=2,.half_width=.85f,.height=1.5f};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=player;
    AiRace drivers[N_RACE_AI]={0};N2Path path={0};
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,0},{60,0}},.info={.kind=N2_RACE_SPRINT}};
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=3,
        .gate={{.x=-20,.dx=1,.half=25},{.x=20,.dx=1,.half=25},{.x=60,.y=20,.dx=1,.half=25}},
        .grid={{-10,0,base},{-4,0,base},{2,0,base}}};
    assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player)==3);
    assert(fabsf(path.xy[1]-20)<.01f);
    for(int tick=0;tick<3000;tick++) {
        AiCar *contacts[N_RACE_AI];int done=0;
        for(int k=0;k<3;k++) {
            ai_race_step(cars,drivers,k,3,0,&world,NULL);contacts[k]=cars+k;
            assert(!drivers[k].drive.failed && fabsf(cars[k].pos[2]-base)<.01f);done+=drivers[k].progress.finished;
        }
        ai_car_contacts(contacts,3,&world,NULL);if(done==3)break;
    }
    for(int k=0;k<3;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15);
    free(path.xy);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
    puts("race authored height: matching deck preferred, including negative heights PASS");
}

/* One isolated distance spike must not force a source lane through a divider.
   The same geometry in reverse record order must retain the safe lane too. */
static void progress_outlier_course_test(const char *root,int reverse) {
    segmented_file(root,0);
    enum { N=9, RECORD=140+56*N, SIZE=16+2*RECORD };
    unsigned char bytes[SIZE]={0};uint32_t heads[]={0x80034120,SIZE-8,0x34121,SIZE-16};memcpy(bytes,heads,16);
    for(int lane=0;lane<2;lane++)for(int j=0;j<N;j++){
        unsigned char *record=bytes+24+lane*RECORD;uint32_t pair=N|(N<<16);memcpy(record+44,&pair,4);
        int at=lane && reverse?N-1-j:j;
        float point[]={-20+20.0f*at,20.0f*lane,lane && j==4?(reverse?10:150):20.0f*at};
        memcpy(record+128+56*j+4,point,sizeof point);
        uint16_t widths[]={32*256,32*256};memcpy(record+128+56*j+24,widths,4);
    }
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,SIZE,f)==SIZE);assert(!fclose(f));
    float floor[]={-50,-5,0,0,0,190,-5,0,0,0,190,25,0,0,0,-50,25,0,0,0};
    float wall[]={50,-5,0,0,0,50,5,0,0,0,50,5,4,0,0,50,-5,4,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
        {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    N2Scene scene={meshes,2,2};float bounds[][4]={{-50,-5,190,25},{50,-5,50,5}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obs[2][4],oz[2][2];int src[2],n=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,n);
    AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
    AiCar cars[N_RACE_AI]={0},player={.pos={-30,20,0},.half_length=2,.half_width=.85f,.height=1.5f};
    AiRace drivers[N_RACE_AI]={0};for(int k=0;k<N_RACE_AI;k++)cars[k]=player;
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,20},{140,20}},.info={.kind=N2_RACE_SPRINT}};
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=3,
        .gate={{.x=-20,.y=20,.dx=1,.half=25},{.x=60,.y=20,.dx=1,.half=25},{.x=140,.y=20,.dx=1,.half=25}},
        .grid={{-10,20,0},{-4,20,0},{2,20,0}}};
    N2Path path={0};assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player)==3);
    for(int j=0;j<path.n;j++)assert(fabsf(path.xy[2*j+1]-20)<.01f);
    for(int tick=0;tick<2400;tick++){
        int done=0;AiCar *contacts[N_RACE_AI];
        for(int k=0;k<3;k++){ai_race_step(cars,drivers,k,3,0,&world,NULL);assert(!drivers[k].drive.failed);done+=drivers[k].progress.finished;contacts[k]=cars+k;}
        ai_car_contacts(contacts,3,&world,NULL);if(done==3)break;
    }
    for(int k=0;k<3;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15);
    free(path.xy);phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* Source settings affect intent/pace while the shared physical limits remain. */
static void source_ai_test(const char *root){
    RaceAiSource source={.count=1};
    for(int j=0;j<11;j++){source.catchup[j][0]=-500+100.0f*j;source.catchup[j][1]=.5f-.1f*j;}
    for(int j=0;j<10;j++)source.skill_scale[j]=.5f+.03f*j;
    RaceAiSourceEvent *profile=source.event;strcpy(profile->name,"SYNTHETIC_SOURCE");profile->track[0]=1;profile->count=3;
    for(int j=0;j<3;j++){profile->driver[j].skill[0]=1;profile->driver[j].skill[1]=4;profile->driver[j].skill[2]=8;profile->driver[j].catch_up=1;}
    WRace player_race={.active=1,.ev=7,.maxlaps=1,.lap=1,.next=1,.ngate=3,
        .gate={{.x=0},{.x=200},{.x=400}}};
    AiTrafficWorld w={.race_pace=100,.race_source=&source,.race_source_choice=-1,.race_difficulty=1,.race_player=&player_race};
    AiRace d={.source=&source,.source_skill=4,.source_scale=.8f,.source_catch_up=1,.progress=player_race};
    float route_xy[]={0,0,200,0,400,0};N2Path route={.xy=route_xy,.n=3};
    d.drive.path=&route;d.drive.error_limit=12;
    AiCar car={.pos={0,0,0}},player={.pos={100,0,0}};
    assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-88)<.001f);
    player.pos[1]=1000;
    assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-80)<.001f);
    player.pos[1]=0;

    car.pos[0]=100;player.pos[0]=0;
    assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-72)<.001f);
    assert(fabsf(ai_race_pace(&car,&d,0,&w,NULL)-80)<.001f);
    d.source_catch_up=0;assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-80)<.001f);d.source_catch_up=1;
    player_race.ev=8;assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-80)<.001f);player_race.ev=7;
    player_race.finished=1;assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-80)<.001f);player_race.finished=0;
    player.pos[0]=NAN;assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-80)<.001f);
    d.source=NULL;assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-100)<.001f);d.source=&source;
    d.progress=(WRace){.active=1,.ev=7,.kind=N2_RACE_CIRCUIT,.maxlaps=3,.lap=1,.next=1,.ngate=4,
        .gate={{.x=0,.y=0},{.x=100,.y=0},{.x=100,.y=100},{.x=0,.y=100}}};
    float square_xy[]={0,0,100,0,100,100,0,100,0,0};route=(N2Path){.xy=square_xy,.n=5};
    d.drive.loop=1;player_race=d.progress;player_race.lap=2;car.pos[0]=player.pos[0]=0;
    assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-112)<.001f);
    d.progress.next=0;car.pos[1]=10;
    assert(fabsf(ai_race_pace(&car,&d,0,&w,&player)-80.8f)<.001f);
    /* Validate source selection at real preparation, not only arithmetic. */
    parallel_lane_file(root);
    float verts[]={-50,-30,0,0,0,150,-30,0,0,0,150,30,0,0,0,-50,30,0,0,0};uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh mesh={.verts=verts,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};N2Scene scene={&mesh,1,1};
    WGroundGrid grid={0};float bounds[][4]={{-50,-30,150,30}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);w.scene=&scene;
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,0},{60,0}},.info={.kind=N2_RACE_SPRINT}};
    for(int difficulty=0;difficulty<4;difficulty++){
        WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=3,
            .gate={{.x=-20,.dx=1,.half=10},{.x=20,.dx=1,.half=10},{.x=60,.dx=1,.half=10}},
            .grid={{-10,0,0},{-4,0,0},{2,0,0}}};
        AiCar cars[N_RACE_AI]={0},pl={.pos={-30,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
        AiRace drivers[N_RACE_AI]={0};for(int j=0;j<3;j++)cars[j]=pl;
        w.race_difficulty=difficulty;N2Path path={0};
        assert(ai_race_prepare(root,&event,&seed,&w,&path,cars,drivers,&pl)==3);
        for(int j=0;j<3;j++){
            if(difficulty==3)assert(!drivers[j].source);
            else {assert(drivers[j].source==&source && drivers[j].source_skill==profile->driver[j].skill[difficulty]);
                assert(drivers[j].source_scale==source.skill_scale[drivers[j].source_skill]);}
        }
        free(path.xy);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

static void narrow_course_test(const char *root,int terrain) {
    parallel_lane_file(root);
    float narrow[]={-50,-.9f,0,0,0,110,-.9f,0,0,0,110,.9f,0,0,0,-50,.9f,0,0,0};
    float wide[]={-50,15,0,0,0,110,15,0,0,0,110,25,0,0,0,-50,25,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=narrow,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},{.verts=wide,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD}};
    if(terrain) {
        meshes[0].cat=N2_TERRAIN;
        for(int k=0;k<4;k++)narrow[5*k+2]=.06f*narrow[5*k];
    }
    N2Scene scene={meshes,2,2};WGroundGrid grid={0};float bounds[][4]={{-50,-.9f,110,.9f},{-50,15,110,25}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    AiTrafficWorld world={.scene=&scene};AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=1.2f,.height=1.5f};
    for(int k=0;k<N_RACE_AI;k++)for(int wheel=0;wheel<4;wheel++) {
        cars[k].support.ax[wheel]=wheel<2?1.3f:-1.3f;
        cars[k].support.ay[wheel]=wheel&1?-.8f:.8f;
    }
    AiCar player={.pos={-30,0,0},.half_length=2,.half_width=1.2f,.height=1.5f};
    player.support=cars[0].support;
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,0},{60,0}},.info={.kind=N2_RACE_SPRINT}};
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=4,.gate={{.x=-20,.dx=1,.half=10},{.x=20,.dx=1,.half=10},{.x=60,.dx=1,.half=10}},.grid={{-10,0,0},{-4,0,0},{2,0,0},{8,0,0}}};
    if(terrain) {
        player.pos[2]=.06f*player.pos[0];
        for(int k=0;k<seed.ngrid;k++)seed.grid[k][2]=.06f*seed.grid[k][0];
    }
    N2Path path={0};int count=ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player);
    assert(count==3);
    assert(fabsf(path.xy[1])<.01f);
    for(int t=0;t<2400;t++) {
        AiCar *contacts[N_RACE_AI];int done=0;
        for(int k=0;k<count;k++){ai_race_step(cars,drivers,k,count,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;assert(!drivers[k].drive.failed);}
        ai_car_contacts(contacts,count,&world,NULL);if(done==count)break;
    }
    for(int k=0;k<count;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15);
    free(path.xy);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}


/* Repairing a blocked lane must allow its still-clear approach to move
   onto the other authored lane before terrain-labelled pavement begins. */
static void branch_terrain_test(const char *root) {
    parallel_lane_file(root);
    float shared[]={-50,-5,0,0,0,30,-5,0,0,0,30,25,0,0,0,-50,25,0,0,0};
    float road[]={30,-5,0,0,0,110,-5,0,0,0,110,5,0,0,0,30,5,0,0,0};
    float terrain[]={-50,-5,0,0,0,110,-5,0,0,0,110,25,0,0,0,-50,25,0,0,0};
    float finish[]={55,15,0,0,0,110,15,0,0,0,110,25,0,0,0,55,25,0,0,0};
    float wall[]={50,0,0,0,0,110,0,0,0,0,110,0,4,0,0,50,0,4,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=shared,.cat=N2_ROAD},{.verts=road,.cat=N2_ROAD},
        {.verts=finish,.cat=N2_ROAD},{.verts=terrain,.cat=N2_TERRAIN},{.verts=wall,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    for(int k=0;k<5;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
    N2Scene scene={meshes,5,5};float bounds[][4]={{-50,-5,30,25},{30,-5,110,5},{55,15,110,25},{-50,-5,110,25},{50,0,110,0}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obs[5][4],oz[5][2];int src[5],n=phys_collect_walls(&scene,obs,src,oz,5);phys_wall_index_build(obs,oz,n);
    AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
    AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    AiCar player={.pos={-30,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,0},{60,0}},.info={.kind=N2_RACE_SPRINT}};
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=3,.gate={{.x=-20,.dx=1,.half=25},{.x=20,.dx=1,.half=25},{.x=60,.y=20,.dx=1,.half=25}},.grid={{-10,20,0},{-4,20,0},{2,20,0}}};
    N2Path path={0};int count=ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player);
    assert(count==3 && fabsf(path.xy[1]-20)<.01f);
    for(int tick=0;tick<3000;tick++) {
        AiCar *contacts[N_RACE_AI];int done=0;
        for(int k=0;k<count;k++){ai_race_step(cars,drivers,k,count,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;assert(!drivers[k].drive.failed);}
        ai_car_contacts(contacts,count,&world,NULL);if(done==count)break;
    }
    for(int k=0;k<count;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15);
    free(path.xy);phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* One lane chain ends where the next begins 10 m aside. The decoded course
   must change lanes gradually, never through the wall beside the first chain. */
static void sidestep_course_test(const char *root) {
    segmented_file(root,0);
    const float chains[2][4][3]={{{-40,0,0},{-20,0,20},{0,0,40},{20,0,60}},{{25,10,65},{45,10,85},{65,10,105},{85,10,125}}};
    enum { N=4, RECORD=140+56*N, SIZE=16+2*RECORD };
    unsigned char bytes[SIZE]={0};uint32_t heads[]={0x80034120,SIZE-8,0x34121,SIZE-16};memcpy(bytes,heads,16);
    for(int lane=0;lane<2;lane++) {
        unsigned char *record=bytes+24+lane*RECORD;uint32_t pair=N|(N<<16);memcpy(record+44,&pair,4);
        for(int k=0;k<N;k++) {
            memcpy(record+128+k*56+4,chains[lane][k],sizeof chains[lane][k]);
            uint16_t widths[]={32*256,32*256};memcpy(record+128+k*56+24,widths,4);
        }
    }
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
    FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,SIZE,f)==SIZE);assert(!fclose(f));
    for(int walled=0;walled<2;walled++) {
        float floor[]={-60,-25,0,0,0,110,-25,0,0,0,110,25,0,0,0,-60,25,0,0,0};
        float wall[]={-60,3,0,0,0,10,3,0,0,0,10,3,4,0,0,-60,3,4,0,0};
        uint16_t idx[]={0,1,2,0,2,3};
        N2Mesh meshes[]={{.verts=floor,.cat=N2_ROAD},{.verts=wall,.cat=N2_OTHER,.scen=N2_SC_WALL}};
        for(int k=0;k<2;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
        N2Scene scene={meshes,1+walled,1+walled};float bounds[][4]={{-60,-25,110,25},{-60,3,10,3}};
        WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
        float obs[2][4],oz[2][2];int src[2],n=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,n);
        AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
        AiCar player={.pos={-36,-1,0},.half_length=2,.half_width=.85f,.height=1.5f};
        WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-40,0},{85,10}},.info={.kind=N2_RACE_SPRINT}};
        WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=3,.gate={{.x=-30,.dx=1,.half=25},{.x=20,.dx=1,.half=25},{.x=80,.y=10,.dx=1,.half=25}},
            .grid={{-30,-2,0},{-24,1,0},{-18,-2,0}}};
        N2Path path={0};int count=ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player);
        assert(count==3);
        float sharpest=0;
        for(int j=1;j+1<path.n;j++) {
            float ux=path.xy[2*j]-path.xy[2*j-2],uy=path.xy[2*j+1]-path.xy[2*j-1];
            float vx=path.xy[2*j+2]-path.xy[2*j],vy=path.xy[2*j+3]-path.xy[2*j+1];
            sharpest=fmaxf(sharpest,fabsf(atan2f(ux*vy-uy*vx,ux*vx+uy*vy)));
            /* The car body never straddles the wall line. */
            if(walled && path.xy[2*j]<10+2)assert(path.xy[2*j+1]<3-.85f || path.xy[2*j+1]>3+.85f);
        }
        fprintf(stderr,"sidestep course walled%d nodes%d sharpest%.1fdeg\n",walled,path.n,sharpest*57.2958f);
        assert(sharpest<.35f);
        for(int tick=0;tick<3000;tick++) {
            AiCar *contacts[N_RACE_AI];int done=0;
            for(int k=0;k<count;k++){ai_race_step(cars,drivers,k,count,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;assert(!drivers[k].drive.failed);}
            ai_car_contacts(contacts,count,&world,NULL);if(done==count)break;
        }
        for(int k=0;k<count;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15);
        free(path.xy);phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
}

/* A gore median begins just past the grid. The other lane is cheaper and its
   entrance is clear beside the grid, but the field cannot reach it. */
static void gore_entrance_test(const char *root) {
    parallel_lane_file(root);lane_heights(root,0,.05f,0);
    float floor[]={-50,-25,0,0,0,110,-25,0,0,0,110,25,0,0,0,-50,25,0,0,0};
    float median[]={-2,10,0,0,0,110,10,0,0,0,110,10,4,0,0,-2,10,4,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=floor,.cat=N2_ROAD},{.verts=median,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    for(int k=0;k<2;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
    N2Scene scene={meshes,2,2};float bounds[][4]={{-50,-25,110,25},{-2,10,110,10}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obs[2][4],oz[2][2];int src[2],n=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,n);
    AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
    AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    AiCar player={.pos={-16,20,0},.half_length=2,.half_width=.85f,.height=1.5f};
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,0},{60,0}},.info={.kind=N2_RACE_SPRINT}};
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=3,.gate={{.x=-20,.dx=1,.half=25},{.x=20,.dx=1,.half=25},{.x=60,.dx=1,.half=25}},.grid={{-10,16,0},{-10,24,0},{-4,20,0}}};
    N2Path path={0};int count=ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player);
    fprintf(stderr,"gore entrance count%d course y%.2f\n",count,path.n?path.xy[1]:NAN);
    assert(count==3 && fabsf(path.xy[1]-20)<.01f);
    for(int tick=0;tick<3000;tick++) {
        AiCar *contacts[N_RACE_AI];int done=0;
        for(int k=0;k<count;k++){ai_race_step(cars,drivers,k,count,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;assert(!drivers[k].drive.failed);}
        ai_car_contacts(contacts,count,&world,NULL);if(done==count)break;
    }
    for(int k=0;k<count;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15);
    free(path.xy);phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* Clear upper-deck endpoints cannot join through a lower floor.
   The side bridge is continuous; all grid cars must physically finish there. */
static void connection_layer_test(const char *root,int wide_gap) {
    parallel_lane_file(root);
    lane_heights(root,0,0,10);
    float left[]={-50,-25,10,0,0,26.2f,-25,10,0,0,26.2f,25,10,0,0,-50,25,10,0,0};
    float right[]={29.2f,-25,10,0,0,110,-25,10,0,0,110,25,10,0,0,29.2f,25,10,0,0};
    float lower[]={-50,-25,0,0,0,110,-25,0,0,0,110,25,0,0,0,-50,25,0,0,0};
    float bridge[]={20,15,10,0,0,35,15,10,0,0,35,25,10,0,0,20,25,10,0,0};
    /* A whole sample can fit on the lower deck while both incoming endpoints look supported. */
    if(wide_gap){right[0]=right[15]=45.2f;bridge[5]=bridge[10]=50;}
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=left,.cat=N2_ROAD},{.verts=right,.cat=N2_ROAD},
        {.verts=lower,.cat=N2_ROAD},{.verts=bridge,.cat=N2_ROAD}};
    for(int k=0;k<4;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
    N2Scene scene={meshes,4,4};float bounds[][4]={{-50,-25,26.2f,25},{right[0],-25,110,25},{-50,-25,110,25},{20,15,bridge[5],25}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    AiTrafficWorld world={.scene=&scene};
    AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    AiCar player={.pos={-30,0,10},.half_length=2,.half_width=.85f,.height=1.5f};
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,0},{60,0}},.info={.kind=N2_RACE_SPRINT}};
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=3,.gate={{.x=-20,.dx=1,.half=25},{.x=20,.dx=1,.half=25},{.x=60,.y=20,.dx=1,.half=25}},.grid={{-10,0,10},{-4,0,10},{2,0,10}}};
    N2Path path={0};int count=ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player);
    assert(count==3 && fabsf(path.xy[1]-20)<.01f);
    for(int tick=0;tick<3000;tick++) {
        AiCar *contacts[N_RACE_AI];int done=0;
        for(int k=0;k<count;k++){ai_race_step(cars,drivers,k,count,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;assert(!drivers[k].drive.failed);}
        ai_car_contacts(contacts,count,&world,NULL);if(done==count)break;
    }
    for(int k=0;k<count;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15);
    free(path.xy);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* Rejected terrain must still carry its measured height into the next deck.
   Both authored lane chains climb the same ramp; only one joins the grid.
   The lower road's wall must not displace the reachable upper-deck route. */
static void route_height_test(const char *root) {
    parallel_lane_file(root);
    lane_heights(root,.3f,0,6);
    float ramp[]={-50,-25,-9,0,0,40,-25,18,0,0,40,25,18,0,0,-50,25,-9,0,0};
    float upper[]={40,-3,18,0,0,110,-3,39,0,0,110,3,39,0,0,40,3,18,0,0};
    float side[]={40,15,18,0,0,110,15,39,0,0,110,25,39,0,0,40,25,18,0,0};
    float lower[]={40,-3,0,0,0,110,-3,0,0,0,110,3,0,0,0,40,3,0,0,0};
    float wall[]={50,-3,0,0,0,50,3,0,0,0,50,3,4,0,0,50,-3,4,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=ramp,.cat=N2_TERRAIN},{.verts=upper,.cat=N2_ROAD},
        {.verts=side,.cat=N2_ROAD},{.verts=lower,.cat=N2_ROAD},{.verts=wall,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    for(int k=0;k<5;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
    N2Scene scene={meshes,5,5};float bounds[][4]={{-50,-25,40,25},{40,-3,110,3},{40,15,110,25},{40,-3,110,3},{50,-3,50,3}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obs[5][4],oz[5][2];int src[5],n=phys_collect_walls(&scene,obs,src,oz,5);phys_wall_index_build(obs,oz,n);
    AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
    AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    AiCar player={.pos={-30,0,-3},.half_length=2,.half_width=.85f,.height=1.5f};
    WEvent event={.id=1,.reg="TEST",.npoly=2,.poly={{-20,0},{60,0}},.info={.kind=N2_RACE_SPRINT}};
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=4,.gate={{.x=-20,.dx=1,.half=10},{.x=20,.dx=1,.half=10},{.x=60,.dx=1,.half=10}},.grid={{-10,0,3},{-4,0,4.8f},{2,0,6.6f},{8,0,8.4f}}};
    N2Path path={0};int count=ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player);
    assert(count==3);
    assert(fabsf(path.xy[2*path.n-1])<.01f);
    for(int t=0;t<3000;t++) {
        AiCar *contacts[N_RACE_AI];int done=0;
        for(int k=0;k<count;k++){ai_race_step(cars,drivers,k,count,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;assert(!drivers[k].drive.failed);}
        ai_car_contacts(contacts,count,&world,NULL);if(done==count)break;
    }
    for(int k=0;k<count;k++)assert(drivers[k].progress.finished && cars[k].ride.contact_mask==15 && cars[k].pos[2]>17);
    free(path.xy);phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* Start-grid height belongs to its local deck, not a distant lap seam.
   The lower deck hides an upper-deck wall from a wrongly seeded planner. */
static void grid_layer_loop_test(const char *root) {
    loop_file(root,2);
    lane_heights(root,.3f,0,0);
    float upper[]={-20,-120,-6,0,0,220,-120,66,0,0,220,120,66,0,0,-20,120,-6,0,0};
    float lower[]={-20,-120,0,0,0,220,-120,0,0,0,220,120,0,0,0,-20,120,0,0,0};
    float wall[]={140,-5,40,0,0,140,5,40,0,0,140,5,46,0,0,140,-5,46,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=upper,.cat=N2_ROAD},{.verts=lower,.cat=N2_ROAD},
        {.verts=wall,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    for(int k=0;k<3;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
    N2Scene scene={meshes,3,3};float bounds[][4]={{-20,-120,220,120},{-20,-120,220,120},{140,-5,140,5}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obs[3][4],oz[3][2];int src[3],n=phys_collect_walls(&scene,obs,src,oz,3);phys_wall_index_build(obs,oz,n);
    AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
    WEvent event={.id=1,.reg="TEST",.circuit=1,.info={.kind=N2_RACE_CIRCUIT,.length=251}};
    WRace seed={.active=1,.kind=N2_RACE_CIRCUIT,.maxlaps=1,.ngate=4,.ngrid=1,
        .gate={{.x=60,.dy=-1,.half=15},{.x=100,.y=-40,.dx=1,.half=15},
               {.x=140,.dy=1,.half=15},{.x=100,.y=40,.dx=-1,.half=15}},.grid={{61,8,18.3f}}};
    AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=.85f,.height=1.5f};
    AiCar player={.pos={61,-8,18.3f},.half_length=2,.half_width=.85f,.height=1.5f};
    N2Path path={0};assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player)==1);
    int detoured=0;
    for(int j=0;j<path.n;j++)if(path.xy[2*j]>100 && fabsf(path.xy[2*j+1])<4) {
        assert(path.xy[2*j]>143);detoured=1;
    }
    assert(detoured);
    for(int tick=0;tick<7000 && !drivers[0].progress.finished;tick++) {
        ai_race_step(cars,drivers,0,1,1,&world,NULL);assert(!drivers[0].drive.failed);
    }
    assert(drivers[0].progress.finished && cars[0].ride.contact_mask==15);
    free(path.xy);phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* A short S-bend needs steering anticipation through the exit, while its
   outside guardrail stays solid. Exercise the mirrored bend too. */
static int bend_contacts;
static void bend_contact(const float before[3],const float after[3],int kind) {
    (void)kind;if(hypotf(before[0]-after[0],before[1]-after[1])>1e-5f)bend_contacts++;
}
static void guardrail_bend_test(void) {
    for(int sign=-1;sign<=1;sign+=2) {
        float floor[]={-100,-100,0,0,0,200,-100,0,0,0,200,100,0,0,0,-100,100,0,0,0};
        float wall[]={29,-25,0,0,0,50,-30,0,0,0,50,-30,4,0,0,29,-25,4,0,0};
        for(int k=0;k<4;k++)wall[5*k+1]*=sign;
        uint16_t idx[]={0,1,2,0,2,3};
        N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
            {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
        N2Scene scene={meshes,2,2};WGroundGrid grid={0};float bounds[][4]={{-100,-100,200,100},{29,-30,50,30}};
        assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
        float obs[2][4],oz[2][2];int src[2],n=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,n);
        AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
        float xy[]={-80,0,-40,0,0,0,15,0,22,-14,35,-23,100,-32,170,-32};
        for(int k=0;k<8;k++)xy[2*k+1]*=sign;N2Path path={.xy=xy,.n=8};
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={-40,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0].progress=(WRace){.active=1,.maxlaps=1,.ngate=3,
            .gate={{.x=-40,.dx=1,.half=20},{.x=30,.y=-15*sign,.dx=1,.half=20},{.x=130,.y=-32*sign,.dx=1,.half=20}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        int ticks=0;bend_contacts=0;g_ai_contact_hook=bend_contact;
        for(;ticks<5000 && !drivers[0].progress.finished && !drivers[0].drive.failed;ticks++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
        }
        g_ai_contact_hook=NULL;assert(!bend_contacts);
        assert(drivers[0].progress.finished && !drivers[0].drive.failed && cars[0].ride.contact_mask==15);
        phys_wall_index_build(NULL,NULL,0);world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
}


/* Overlapping lower pavement must not steal the height reference when a
 * corridor preview restarts at the car after sampling its downhill target. */
static void descending_deck_test(void) {
    float upper[]={-20,-2,1.5f,0,0,40,-2,1.5f,0,0,40,2,1.5f,0,0,-20,2,1.5f,0,0};
    float ramp[]={40,-2,1.5f,0,0,50,-2,0,0,0,50,2,0,0,0,40,2,1.5f,0,0};
    float lower[]={-20,-2,-.05f,0,0,120,-2,-.05f,0,0,120,2,-.05f,0,0,-20,2,-.05f,0,0};
    float skirt[]={40,-2,-.05f,0,0,40,2,-.05f,0,0,40,2,1.5f,0,0,40,-2,1.5f,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=upper,.cat=N2_ROAD},{.verts=ramp,.cat=N2_TERRAIN},
        {.verts=lower,.cat=N2_TERRAIN},{.verts=skirt,.cat=N2_TERRAIN}};
    for(int k=0;k<4;k++){meshes[k].nverts=4;meshes[k].idx=idx;meshes[k].nidx=6;}
    N2Scene scene={meshes,4,4};
    float bounds[][4]={{-20,-2,40,2},{40,-2,50,2},{-20,-2,120,2},{40,-2,40,2}};
    WGroundGrid grid={0};
    float xy[]={0,0,20,0,40,0,60,0,80,0,100,0};N2Path path={xy,6};
    AiTrafficWorld world={.scene=&scene};
    for(int reverse=0;reverse<2;reverse++) {
        if(reverse) {
            for(int k=0;k<4;k++) {
                for(int v=0;v<4;v++)meshes[k].verts[5*v]=-meshes[k].verts[5*v];
                float lo=bounds[k][0];bounds[k][0]=-bounds[k][2];bounds[k][2]=-lo;
            }
            for(int k=0;k<path.n;k++)xy[2*k]=-xy[2*k];
        }
        assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
        int sign=reverse?-1:1;
        AiCar cars[N_RACE_AI]={{.pos={sign*20,0,1.5f},
            .head=reverse?3.14159265f:0,.half_length=2,.half_width=.85f,.height=1.5f}};
        AiRace drivers[N_RACE_AI]={{.progress={.active=1,.maxlaps=1,.ngate=2,
            .gate={{.x=sign*20,.dx=sign,.half=2},
                   {.x=sign*80,.dx=sign,.half=2}}}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,cars[0].head));
        int partial=0;
        for(int tick=0;tick<1800 && !drivers[0].progress.finished;tick++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            assert(isfinite(cars[0].pos[2]) && cars[0].pos[2]>-.3f);
            partial+=cars[0].ride.contact_mask!=15;
        }
        printf("deck grade direction%d: x%.3f z%.3f mask%u partial%d finished%d failed%d\n",reverse,cars[0].pos[0],cars[0].pos[2],cars[0].ride.contact_mask,partial,drivers[0].progress.finished,drivers[0].drive.failed);fflush(stdout);
        assert(drivers[0].progress.finished && !drivers[0].drive.failed && cars[0].ride.contact_mask==15);
        world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
}

static void race_testdrive_test(void) {
    assert(race_has_traffic(N2_RACE_CIRCUIT,0) && race_has_traffic(N2_RACE_SPRINT,0));
    assert(race_has_traffic(N2_RACE_DRAG,0) && race_has_traffic(N2_RACE_DRIFT,1));
    assert(!race_has_traffic(N2_RACE_DRIFT,0) && !race_has_traffic(N2_RACE_STREETX,0));
    assert(!race_has_traffic(N2_RACE_URL,0) && !race_has_traffic(N2_RACE_UNKNOWN,1));
    AiTraffic routes[N_OPENWORLD_AI]={0};routes[0].present=routes[17].present=1;
    assert(ai_actor_present(0,1,3,routes) && !ai_actor_present(3,1,3,routes));
    assert(ai_actor_present(N_RACE_AI,1,3,routes) && ai_actor_present(N_WORLD_AI-1,1,3,routes));
    assert(ai_actor_visual(N_RACE_AI,1,3,1)==0 && ai_actor_visual(0,1,3,1)==N_AI);
    assert(ai_actor_slot(N_WORLD_AI-1,1)==17 && ai_actor_present(17,0,0,routes));
    float floor[]={-20,-6,0,0,0,600,-6,0,0,0,600,6,0,0,0,-20,6,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-20,-6,600,6}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    AiTrafficWorld world={.scene=&scene,.race_pace=110};
    float xy[]={0,0,100,0,200,0,300,0,400,0,500,0,600,0},lanes[]={-4,0,4};N2Path path={.xy=xy,.n=7};
    AiCar body={.pos={10,0,0},.head=0,.half_length=2,.half_width=.85f,.height=1.5f};
    AiDrive line;assert(ai_drive_join(&line,&path,body.pos,0,12));
    assert(ai_drag_lane_target(&world,&body,&line,0,lanes,3,1)==4);
    assert(ai_drag_lane_target(&world,&body,&line,4,lanes,3,0)==4); /* held key adds no request */
    assert(ai_drag_lane_target(&world,&body,&line,4,lanes,3,-1)==0);
    assert(ai_drag_lane_target(&world,&body,&line,4,lanes,3,1)==4.5f);
    assert(ai_drag_lane_target(&world,&body,&line,5,lanes,3,1)==5); /* edge support rejects further movement */
    float wall[]={-20,4.9f,0,0,0,600,4.9f,0,0,0,600,4.9f,4,0,0,-20,4.9f,4,0,0};
    N2Mesh wall_meshes[]={mesh,{.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    scene.meshes=wall_meshes;scene.count=scene.cap=2;
    float obs[2][4],oz[2][2];int src[2];
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);world.obst=obs;world.obstz=oz;world.obstsrc=src;
    phys_wall_index_build(obs,oz,world.nobst);
    assert(ai_drag_lane_target(&world,&body,&line,4,lanes,3,1)==4); /* wall blocks the edge nudge */
    scene.meshes=&mesh;scene.count=scene.cap=1;world.nobst=0;phys_wall_index_build(NULL,NULL,0);
    /* A narrowing floor merges onto a clear source lane before its edge. */
    floor[5]=floor[10]=40;bounds[0][2]=40;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    body.pos[0]=30;
    assert(ai_drag_lane_target(&world,&body,&line,4,lanes,3,0)==4); /* no supported forward lane: retain target */
    float narrow[]={40,-1.2f,0,0,0,600,-1.2f,0,0,0,600,1.2f,0,0,0,40,1.2f,0,0,0};
    N2Mesh meshes[]={mesh,{.verts=narrow,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD}};
    scene.meshes=meshes;scene.count=scene.cap=2;float joined[][4]={{-20,-6,40,6},{40,-1.2f,600,1.2f}};
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,joined));world_ground_grid_activate(&grid);
    assert(ai_drag_lane_target(&world,&body,&line,4,lanes,3,0)==0);
    /* A gentle vehicle touch fails drag, but never ends other race modes. */
    AiCar other=body;other.pos[0]+=3.5f;AiCar *pair[]={&body,&other};
    float touch=ai_car_contacts(pair,2,&world,&body);assert(touch>0);
    assert(race_drag_failure(0,0,touch)==2 && !race_drag_failure(0,0,0));
    assert(race_drag_failure(1,20,touch)==1 && race_drag_failure(0,9,0)==2);
    /* A faster racer must physically pass traffic, rather than queue behind it. */
    scene.meshes=&mesh;scene.count=scene.cap=1;floor[5]=floor[10]=600;bounds[0][2]=600;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    AiCar cars[N_RACE_AI]={0},traffic={.pos={55,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    AiRace drivers[N_RACE_AI]={0};AiTraffic ambient_route={.present=1};
    cars[0]=(AiCar){.pos={5,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    drivers[0].progress=(WRace){.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
        .gate={{.dx=1,.half=10},{.x=500,.dx=1,.half=10}}};
    assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
    world.ambient=&traffic;world.ambient_routes=&ambient_route;world.ambient_count=1;
    float peak=0,lateral=0;int overlap=0;
    for(int tick=0;tick<4000 && !drivers[0].progress.finished;tick++) {
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
        peak=fmaxf(peak,PHYS_KMH(cars[0].spd));lateral=fmaxf(lateral,fabsf(cars[0].pos[1]));
        overlap+=phys_ai_overlap(&cars[0],&traffic);
    }
    fprintf(stderr,"race testdrive: peak%.1f lateral%.2f overlap%d finished%d failed%d\n",peak,lateral,overlap,drivers[0].progress.finished,drivers[0].drive.failed);
    assert(peak>80 && lateral>1.8f && !overlap && drivers[0].progress.finished && !drivers[0].drive.failed);
    /* A close stationary grid leader must not prevent starting a pass. */
    cars[0]=(AiCar){.pos={5,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    traffic.pos[0]=9.3f;traffic.pos[1]=0;traffic.vel[0]=traffic.vel[1]=traffic.spd=0;
    drivers[0]=(AiRace){.progress={.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
        .gate={{.dx=1,.half=10},{.x=500,.dx=1,.half=10}}}};
    assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
    for(int tick=0;tick<1200;tick++) {
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
        AiCar *contacts[]={cars,&traffic};ai_car_contacts(contacts,2,&world,NULL);
        assert(!phys_ai_overlap(cars,&traffic));
    }
    fprintf(stderr,"close grid pass: racer%.2f %.2f leader%.2f %.2f failed%d\n",cars[0].pos[0],cars[0].pos[1],traffic.pos[0],traffic.pos[1],drivers[0].drive.failed);
    assert(cars[0].pos[0]>traffic.pos[0]+10 && !drivers[0].drive.failed);
    /* Small movements inside a lead vehicle's lane must not reverse a pass. */
    cars[0]=(AiCar){.pos={5,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
    drivers[0]=(AiRace){.progress={.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
        .gate={{.dx=1,.half=10},{.x=500,.dx=1,.half=10}}}};
    assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
    float previous_offset=0,moving_peak=0;int passing_flips=0;
    for(int tick=0;tick<1200;tick++) {
        traffic.pos[0]=cars[0].pos[0]+40;traffic.pos[1]=.2f*sinf(tick*.1f);
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
        moving_peak=fmaxf(moving_peak,PHYS_KMH(cars[0].spd));
        if(drivers[0].offset*previous_offset<-.1f && PHYS_KMH(cars[0].spd)>30)passing_flips++;
        previous_offset=drivers[0].offset;
        assert(!drivers[0].drive.failed && !phys_ai_overlap(cars,&traffic));
    }
    fprintf(stderr,"moving lead: passing side flips%d peak%.1f\n",passing_flips,moving_peak);
    assert(passing_flips<=1 && moving_peak>80);
    /* Faster trailing racers commit to a pass on a straight, supported road. */
    world.ambient_count=0;world.ambient=NULL;
    int changes[3]={0},flips[3]={0},done=0;
    float previous[3]={0};
    for(int k=0;k<3;k++) {
        cars[k]=(AiCar){.pos={21-k*8,0,0},.half_length=2,.half_width=.85f,.height=1.5f};
        drivers[k]=(AiRace){.progress={.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
            .gate={{.dx=1,.half=10},{.x=500,.dx=1,.half=10}}}};
        assert(ai_drive_init(&drivers[k].drive,&path,cars[k].pos,0));
    }
    for(int tick=0;tick<5000 && done<3;tick++) {
        AiCar *contacts[3];done=0;
        for(int k=0;k<3;k++) {
            ai_race_step(cars,drivers,k,3,0,&world,NULL);contacts[k]=cars+k;
            if(fabsf(drivers[k].offset-previous[k])>1 && PHYS_KMH(cars[k].spd)>30) {
                changes[k]++;flips[k]+=drivers[k].offset*previous[k]<-.1f;
            }
            previous[k]=drivers[k].offset;done+=drivers[k].progress.finished;
            assert(!drivers[k].drive.failed);
        }
        ai_car_contacts(contacts,3,&world,NULL);
    }
    for(int k=0;k<3;k++) {
        fprintf(stderr,"straight race car%d: line changes%d side flips%d finished%d\n",k,changes[k],flips[k],drivers[k].progress.finished);
            assert(drivers[k].progress.finished && changes[k]<=6 && flips[k]<=1);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    puts("race testdrive: road/closed traffic policy, actor partition, drag edge/narrowing/contact and faster physical passing PASS");
}

/* A rotated body occupies its projected length and width, in every actor role. */
static void oriented_rival_test(void) {
    float floor[]={-20,-8,0,0,0,600,-8,0,0,0,600,8,0,0,0,-20,8,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-20,-8,600,8}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    float xy[]={0,0,100,0,200,0,300,0,400,0,500,0,600,0};N2Path path={.xy=xy,.n=7};
    const float headings[]={1.5707963f,-1.5707963f,.7853982f,-.7853982f};
    for(int role=0;role<4;role++)for(int angle=0;angle<(role==3?1:4);angle++) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};AiTraffic route={.present=1};
        cars[0]=(AiCar){.pos={5,0,0},.spd=100/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        cars[1]=(AiCar){.pos={55,0,0},.head=role==3?.1f:headings[angle],.half_length=2,.half_width=.85f,.height=1.5f};
        AiTrafficWorld world={.scene=&scene,.race_pace=110};
        if(role==0){world.ambient=cars+1;world.ambient_routes=&route;world.ambient_count=1;}
        drivers[0].progress=(WRace){.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
            .gate={{.dx=1,.half=10},{.x=500,.dx=1,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        /* Current yaw must not block a future pass along the shared course. */
        if(role==3) {
            floor[1]=floor[6]=-3.9f;floor[11]=floor[16]=3.9f;
            cars[1].spd=60/3.6f/PHYS_TICKRATE;cars[1].vel[0]=cars[1].spd;
        }
        int overlaps=0,passed=0;
        for(int tick=0;tick<3000 && !drivers[0].progress.finished;tick++) {
            if(role==3)cars[1].pos[0]+=cars[1].spd;
            ai_race_step(cars,drivers,0,role>=2?2:1,0,&world,role==1?cars+1:NULL);
            passed|=cars[0].pos[0]>cars[1].pos[0]+4;
            overlaps+=phys_ai_overlap(cars,cars+1);
            assert(!drivers[0].drive.failed && cars[0].ride.contact_mask==15);
        }
        fprintf(stderr,"oriented rival: role%d angle%.2f overlaps%d finished%d\n",role,cars[1].head,overlaps,drivers[0].progress.finished);
        assert(!overlaps && drivers[0].progress.finished && passed);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    puts("race oriented rivals: stopped bodies cleared and moving racer passed on narrow road PASS");
}


/* Kinematic rival pose at route distance s, offset left of the course. */
static void course_pose(const N2Path *p,float s,float lateral,float pos[3],float *head) {
    int i=0;float len=0;
    for(;i<p->n-2;i++,s-=len) {
        len=hypotf(p->xy[2*i+2]-p->xy[2*i],p->xy[2*i+3]-p->xy[2*i+1]);
        if(s<=len)break;
    }
    float dx=p->xy[2*i+2]-p->xy[2*i],dy=p->xy[2*i+3]-p->xy[2*i+1];len=hypotf(dx,dy);
    *head=atan2f(dy,dx);
    pos[0]=p->xy[2*i]+dx*s/len-sinf(*head)*lateral;
    pos[1]=p->xy[2*i+1]+dy*s/len+cosf(*head)*lateral;pos[2]=0;
}

/* Rivals on a bending course: route headings differ between the racer and
   the rival, so a straight projection would mix tangents. */
static void bend_rival_test(void) {
    float floor[]={-50,-400,0,0,0,1100,-400,0,0,0,1100,400,0,0,0,-50,400,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-50,-400,1100,400}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    float xy[42]={0};N2Path path={.xy=xy,.n=21};
    for(int k=1;k<21;k++) {
        float h=.25f*sinf(k*.8f);
        xy[2*k]=xy[2*k-2]+50*cosf(h);xy[2*k+1]=xy[2*k-1]+50*sinf(h);
    }
    float end=hypotf(xy[40]-xy[38],xy[41]-xy[39]);
    AiTraffic route={.present=1};AiCar rival={.half_length=2,.half_width=.85f,.height=1.5f};
    AiTrafficWorld world={.scene=&scene,.race_pace=110,.ambient=&rival,.ambient_routes=&route,.ambient_count=1};
    for(int slower=0;slower<2;slower++) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        float h,rival_s=slower?60:55,rival_kmh=slower?60:125;
        course_pose(&path,5,0,cars[0].pos,&h);
        cars[0].head=h;cars[0].spd=100/3.6f/PHYS_TICKRATE;
        cars[0].half_length=2;cars[0].half_width=.85f;cars[0].height=1.5f;
        drivers[0].progress=(WRace){.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
            .gate={{.dx=1,.half=10},{.x=xy[40],.y=xy[41],.dx=(xy[40]-xy[38])/end,.dy=(xy[41]-xy[39])/end,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,h));
        float previous=0,widest=0;int flips=0,passed=0;
        for(int tick=0;tick<1800 && !passed;tick++) {
            rival_s+=rival_kmh/3.6f/PHYS_TICKRATE;
            course_pose(&path,rival_s,.3f*sinf(tick*.05f),rival.pos,&rival.head);
            rival.spd=rival_kmh/3.6f/PHYS_TICKRATE;
            rival.vel[0]=cosf(rival.head)*rival.spd;rival.vel[1]=sinf(rival.head)*rival.spd;
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            if(drivers[0].offset*previous<-.1f && PHYS_KMH(cars[0].spd)>30)flips++;
            previous=drivers[0].offset;widest=fmaxf(widest,fabsf(drivers[0].offset));
            assert(!drivers[0].drive.failed && !phys_ai_overlap(cars,&rival) && cars[0].ride.contact_mask==15);
            passed=drivers[0].drive.progress>rival_s+15;
        }
        fprintf(stderr,"bend rival %s: side flips%d widest offset%.2f passed%d\n",slower?"slower":"faster",flips,widest,passed);
        /* Faster and pulling away: hold the line. Slower: commit to one pass. */
        if(slower)assert(passed && flips<=1);
        else assert(!passed && !flips && widest<1);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    puts("race bend rivals: faster rival ignored, slower rival passed without side reversals PASS");
}

/* Losing the passed rival must not cancel an unfinished, supported lane change. */
static void held_lane_test(void) {
    float floor[]={-20,-20,0,0,0,600,-20,0,0,0,600,20,0,0,0,-20,20,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-20,-20,600,20}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    float xy[]={0,0,100,0,200,0,300,0,400,0,500,0,600,0};N2Path path={.xy=xy,.n=7};
    AiTrafficWorld world={.scene=&scene,.race_pace=110};
    for(int narrowing=0;narrowing<2;narrowing++)
    for(int direction=-1;direction<=1;direction+=2)for(int side=-1;side<=1;side+=2) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        float heading=(direction>0?0:3.1415926536f)+direction*side*.06f;
        cars[0]=(AiCar){.pos={direction>0?5:505,side*1.1f,0},.head=heading,
            .spd=90/3.6f/PHYS_TICKRATE,.half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0]=(AiRace){.offset=side*2.35f,.pass_side=side,
            .progress={.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
                .gate={{.x=direction>0?0:510,.dx=direction,.half=10},
                    {.x=direction>0?500:10,.dx=direction,.half=10}}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,heading));
        /* The passed rival has left: finish entering the safe selected lane,
           including offsets between the planner's fixed lane samples. */
        for(int tick=0;tick<600;tick++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            assert(fabsf(drivers[0].offset-side*2.35f)<.001f);
            assert(!drivers[0].blocked && !drivers[0].drive.failed && cars[0].ride.contact_mask==15);
        }
        assert(fabsf(cars[0].pos[1]-side*2.35f)<.5f);
        if(narrowing) {
            /* A taper ahead makes this lane unsupported. Leave it before
               reaching the narrow road, keeping all four wheels supported. */
            float taper[40],a=cars[0].pos[0]+direction*20,b=cars[0].pos[0]+direction*60;
            float x[]={-20,fminf(a,b),fmaxf(a,b),600};
            uint16_t strip[]={0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6};
            for(int v=0;v<8;v++) {
                int near=v%4<2;float width=(near==(direction>0))?20:2;
                taper[5*v]=x[v%4];taper[5*v+1]=(v<4?-1:1)*width;
                taper[5*v+2]=taper[5*v+3]=taper[5*v+4]=0;
            }
            mesh.verts=taper;mesh.nverts=8;mesh.idx=strip;mesh.nidx=18;cars[0].t=0;
            for(int tick=0;tick<600;tick++) {
                ai_race_step(cars,drivers,0,1,0,&world,NULL);
                assert(!drivers[0].drive.failed && cars[0].ride.contact_mask==15);
            }
            assert(fabsf(drivers[0].offset)<.001f && fabsf(cars[0].pos[1])<.5f);
            mesh.verts=floor;mesh.nverts=4;mesh.idx=idx;mesh.nidx=6;
            continue;
        }
        /* Holding a lane must still yield it to a stopped body ahead. */
        cars[1]=(AiCar){.pos={cars[0].pos[0]+direction*35,side*2.35f,0},
            .head=direction>0?0:3.1415926536f,.half_length=2,.half_width=.85f,.height=1.5f};
        cars[0].t=0;
        ai_race_step(cars,drivers,0,1,0,&world,cars+1);
        assert(!drivers[0].blocked && !drivers[0].drive.failed);
        assert(fabsf(drivers[0].offset-side*2.35f)>1);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    puts("race held lane: unfinished passes retain their safe lane; occupied or unsupported lanes replan PASS");
}

/* A narrow road still has room to widen a pass around a swaying slower car. */
static void narrow_passing_test(void) {
    float floor[]={-20,-3.9f,0,0,0,600,-3.9f,0,0,0,600,3.9f,0,0,0,-20,3.9f,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-20,-3.9f,600,3.9f}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    float xy[]={0,0,100,0,200,0,300,0,400,0,500,0,600,0};N2Path path={.xy=xy,.n=7};
    AiTrafficWorld world={.scene=&scene,.race_pace=110};
    for(int role=0;role<3;role++)for(int direction=-1;direction<=1;direction+=2)for(int side=-1;side<=1;side++) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        float heading=direction>0?0:3.1415926536f;
        cars[0]=(AiCar){.pos={direction>0?5:505,0,0},.head=heading,.spd=100/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        cars[1]=(AiCar){.pos={direction>0?55:455,0,0},.head=heading+.1f*(side?side:1),.spd=60/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0]=(AiRace){.offset=side*2.05f,.pass_side=side,
            .progress={.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
                .gate={{.x=direction>0?0:510,.dx=direction,.half=10},
                    {.x=direction>0?500:10,.dx=direction,.half=10}}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,heading));
        AiTraffic ambient_route={.present=1};
        world.ambient=role==2?cars+1:NULL;world.ambient_routes=role==2?&ambient_route:NULL;world.ambient_count=role==2;
        float previous=drivers[0].offset;int flips=0,passed=0;
        for(int tick=0;tick<3000 && !drivers[0].progress.finished;tick++) {
            cars[1].pos[0]+=direction*cars[1].spd;cars[1].pos[1]=.3f*sinf(tick*.05f);
            ai_race_step(cars,drivers,0,role?1:2,0,&world,role==1?cars+1:NULL);
            if(drivers[0].offset*previous<-.1f && PHYS_KMH(cars[0].spd)>30)flips++;
            previous=drivers[0].offset;
            if(drivers[0].drive.failed || phys_ai_overlap(cars,cars+1) || cars[0].ride.contact_mask!=15)
                fprintf(stderr,"narrow pass side%d tick%d flips%d offset%.2f overlap%d contacts%d failed%d\n",
                    side,tick,flips,drivers[0].offset,phys_ai_overlap(cars,cars+1),
                    cars[0].ride.contact_mask,drivers[0].drive.failed);
            assert(!drivers[0].drive.failed && !phys_ai_overlap(cars,cars+1) &&
                   cars[0].ride.contact_mask==15);
            for(int w=0;w<4;w++)assert(cars[0].support.valid[w]);
            passed|=direction*(cars[0].pos[0]-cars[1].pos[0])>10;
        }
        fprintf(stderr,"narrow pass role%d direction%d side%d: flips%d passed%d finished%d\n",
            role,direction,side,flips,passed,drivers[0].progress.finished);
        assert(passed && !flips && drivers[0].progress.finished);
    }
    world.ambient=NULL;world.ambient_routes=NULL;world.ambient_count=0;
    /* Entry commitment must not hide oncoming or stopped bodies in any role. */
    for(int role=0;role<6;role++) {
        int actor=role%3,stopped=role>=3;
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};AiTraffic route={.present=1};
        cars[0]=(AiCar){.pos={5,0,0},.head=.05f,.spd=100/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        cars[1]=(AiCar){.pos={55,2.05f,0},.head=stopped?0:3.1415926536f,
            .spd=stopped?0:100/3.6f/PHYS_TICKRATE,.half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0]=(AiRace){.offset=2.05f,.pass_side=1,
            .progress={.active=1,.kind=N2_RACE_SPRINT,.ngate=2,.maxlaps=1,
                .gate={{.dx=1,.half=10},{.x=500,.dx=1,.half=10}}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,cars[0].head));
        AiTrafficWorld blocked_world={.scene=&scene,.race_pace=110};
        if(actor==2){blocked_world.ambient=cars+1;blocked_world.ambient_routes=&route;blocked_world.ambient_count=1;}
        ai_race_step(cars,drivers,0,actor?1:2,0,&blocked_world,actor==1?cars+1:NULL);
        assert(!drivers[0].drive.failed && drivers[0].offset<=0);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    puts("race narrow passing: swaying racer/player/traffic passed in both directions without reversals PASS");
}

/* A reviewed guardrail replacement (model rule with a height) must include
   its tilted W-beam, not only the posts: a car fits between posts 3.3 m apart.
   The 8 m extrusion must not block a deck above the rail's lower road. */
static void rail_replacement_test(void) {
    float rail[]={0,0,0,0,0, .1f,0,0,0,0, .1f,0,.75f,0,0, 0,0,.75f,0,0,
        3.3f,0,0,0,0, 3.4f,0,0,0,0, 3.4f,0,.75f,0,0, 3.3f,0,.75f,0,0,
        0,0,.3f,0,0, 3.4f,0,.3f,0,0, 3.4f,.12f,.5f,0,0, 0,.12f,.5f,0,0};
    uint16_t rail_idx[]={0,1,2,0,2,3, 4,5,6,4,6,7, 8,9,10,8,10,11};
    float road[]={-20,-20,0,0,0,20,-20,0,0,0,20,20,0,0,0,-20,20,0,0,0};
    float deck[]={-20,-20,7,0,0,20,-20,7,0,0,20,20,7,0,0,-20,20,7,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[3]={{.verts=road,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
        {.verts=deck,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
        {.verts=rail,.nverts=12,.idx=rail_idx,.nidx=18,.cat=N2_OTHER,.scen=N2_SC_WALL,
         .wall_policy=2,.wall_height=8,.sname="TEST_GUARDRAIL"}};
    N2Scene scene={meshes,3,3};
    assert(phys_prepare_boundaries(&scene) && meshes[2].wall_verts);
    float bounds[][4]={{-20,-20,20,20},{-20,-20,20,20},{0,0,3.4f,.12f}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obst[3][4],oz[3][2];int src[3];int n=phys_collect_walls(&scene,obst,src,oz,3);
    assert(n==1 && src[0]==2);
    float bb[6]={-2,-.85f,0,2,.85f,1.5f};
    for(int deck_level=0;deck_level<2;deck_level++) {
        float z=deck_level?7:0,pos[3]={1.7f,0,z},vel[2]={0,0};
        int hit=collide_body_walls_preview(pos,vel,1.5707963f,bb,obst,oz,n,z+.05f,z+1.5f,&scene,src);
        fprintf(stderr,"guardrail replacement: %s body between posts hit%d\n",deck_level?"deck":"road",hit);
        assert(hit==!deck_level);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    free(meshes[2].wall_verts);free(meshes[2].wall_idx);
    puts("race guardrail: W-beam between posts solid, deck above the rail clear PASS");
}

/* Slot numbers, not record order or distance to the player, define the field.
   A start-line marker is not a parking slot. The rear player shares the grid. */
static void starting_grid_test(const char *root) {
    float floor[]={-100,-100,0,0,0,300,-100,0,0,0,300,300,0,0,0,-100,300,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};AiTrafficWorld world={.scene=&scene};
    for(int layout=0;layout<6;layout++) {
        route_file(root,0);
        if(layout==5) {
            /* The real grid can extend past a lane record's half-width.
               It is still inside the race driver's supported join envelope. */
            lane_file(root,0);char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);
            FILE *f=fopen(file,"r+b");assert(f);
            for(int k=0;k<5;k++) {
                float p[]={k*50.0f,0,k*50.0f};uint16_t width[]={5*256,5*256};
                assert(!fseek(f,24+128+k*56+4,SEEK_SET));assert(fwrite(p,4,3,f)==3);
                assert(!fseek(f,24+128+k*56+24,SEEK_SET));assert(fwrite(width,2,2,f)==2);
            }
            assert(!fclose(f));
        }
        float slots[8][3];int columns=layout==2?3:layout==1 || layout==3?2:4;
        for(int k=0;k<8;k++) {
            slots[k][0]=20-8*(k/columns);slots[k][1]=(columns-1)*2-4*(k%columns);slots[k][2]=0;
        }
        if(layout==4) {
            for(int k=0;k<8;k++){float x=slots[k][0];slots[k][0]=-slots[k][1];slots[k][1]=x;}
            char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/Paths1.bin",root);
            FILE *f=fopen(file,"r+b");assert(f);
            for(int k=0;k<5;k++){float p[]={0,k*50.0f};assert(!fseek(f,8+24*k,SEEK_SET));assert(fwrite(p,4,2,f)==2);}assert(!fclose(f));
        }
        unsigned char bytes[16+9*48]={0};uint32_t header[]={0x34146,sizeof bytes-8};memcpy(bytes,header,8);
        memset(bytes+8,0x11,8);const int order[]={8,3,7,0,5,1,6,2,4};
        for(int k=0;k<9;k++) {
            unsigned char *rec=bytes+16+48*k;int slot=order[k];
            uint32_t name=slot==8?0x6a56b90au:0x8e55c895u,id=slot,event=1;
            uint32_t heading=0xbeef0000u+(layout==4?16384:0);
            float start[]={22.75f,0,0};
            memcpy(rec+8,&name,4);memcpy(rec+12,&id,4);memcpy(rec+16,slot==8?start:slots[slot],12);
            memcpy(rec+32,&heading,4);memcpy(rec+36,&event,4);
        }
        char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/TrackPosMarkersAll.bin",root);
        FILE *f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(!fclose(f));
        World w={0};w.neighborhood.nreg=1;strcpy(w.neighborhood.rgn[0].name,"STREAMTEST");
        w.city.nev=1;w.city.ev[0]=(WEvent){.id=1,.reg="TEST",.npoly=2,.poly={{0,0},{200,0}},
            .info={.kind=layout==3?N2_RACE_URL:N2_RACE_SPRINT}};
        if(layout==4){w.city.ev[0].poly[1][0]=0;w.city.ev[0].poly[1][1]=200;}
        assert(world_race_start(&w,root,0,1)==2);
        assert(w.city.race.ngrid==8);
        for(int k=0;k<8;k++)assert(!memcmp(w.city.race.grid[k],slots[k],sizeof slots[k]));
        AiCar cars[N_RACE_AI],player={.pos={22.75f,0,0},.half_length=2,.half_width=1,.height=1.5f};
        for(int k=0;k<N_RACE_AI;k++)cars[k]=player;
        AiRace drivers[N_RACE_AI];N2Path course={0};int wanted=layout==3?5:3;
        assert(ai_race_prepare(root,&w.city.ev[0],&w.city.race,&world,&course,cars,drivers,&player)==wanted);
        int player_slot=layout==2?4:wanted;
        assert(!memcmp(player.pos,slots[player_slot],sizeof player.pos));
        assert(fabsf(player.head-(layout==4?1.5707963f:0))<.0001f);
        for(int k=0;k<wanted;k++) {
            int slot=layout==2 && k==2?3:k;
            assert(!memcmp(cars[k].pos,slots[slot],sizeof cars[k].pos));
            assert(fabsf(cars[k].head-player.head)<.0001f && cars[k].ride.contact_mask==15);
            assert(drivers[k].drive.path==&course && !drivers[k].drive.failed);
            assert(!phys_ai_overlap(&cars[k],&player));
            for(int j=0;j<k;j++)assert(!phys_ai_overlap(&cars[k],&cars[j]));
        }
        if(layout==0) {
            /* The outer columns have no pavement: choose the middle 2x2,
               never leave just the player or part of the field unsupported. */
            floor[1]=floor[6]=-4;floor[11]=floor[16]=4;
            assert(ai_race_prepare(root,&w.city.ev[0],&w.city.race,&world,&course,cars,drivers,&player)==3);
            assert(!memcmp(player.pos,slots[6],sizeof player.pos));
            const int chosen[]={1,2,5};
            for(int k=0;k<3;k++)assert(!memcmp(cars[k].pos,slots[chosen[k]],sizeof cars[k].pos));
            AiCar before=player,saved[N_RACE_AI];memcpy(saved,cars,sizeof saved);
            floor[1]=floor[6]=-.5f;floor[11]=floor[16]=.5f;
            assert(!ai_race_prepare(root,&w.city.ev[0],&w.city.race,&world,&course,cars,drivers,&player));
            assert(!memcmp(&before,&player,sizeof player) && !memcmp(saved,cars,sizeof saved));
            floor[1]=floor[6]=-100;floor[11]=floor[16]=300;
        }
        free(course.xy);
        if(layout==5) {
            uint32_t size=56;memcpy(bytes+4,&size,4);
            f=fopen(file,"wb");assert(f);assert(fwrite(bytes,1,64,f)==64);assert(!fclose(f));
            assert(world_race_start(&w,root,0,1)==2 && w.city.race.ngrid==0);
        }
    }
    char file[512];snprintf(file,sizeof file,"%s/ROUTESTEST/TrackPosMarkersAll.bin",root);assert(!unlink(file));
    snprintf(file,sizeof file,"%s/ROUTESTEST/Routes1F.bin",root);assert(!unlink(file));
    puts("source starting grids: four abreast, two by two, three-column subset, URL and rotated headings PASS");
}

/* An apparently clear destination lane is not a clear merge: the body
   alongside us occupies the intermediate lane during the transition. */
static void adjacent_merge_test(void) {
    float floor[]={-50,-20,0,0,0,500,-20,0,0,0,500,20,0,0,0,-50,20,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-50,-20,500,20}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);AiTrafficWorld world={.scene=&scene,.race_pace=100};
    float xy[]={0,0,100,0,200,0,300,0,400,0};N2Path path={.xy=xy,.n=5};
    {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={20,6,0},.spd=100/3.6f/PHYS_TICKRATE,.t=1,
            .half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0]=(AiRace){.blocked=1,.offset=6,.progress={.active=1,.lap=1,.maxlaps=1,.ngate=2,
            .gate={{.dx=1,.half=10},{.x=390,.dx=1,.half=10}}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
        fprintf(stderr,"blocked lane steering%.4f braking%d\n",cars[0].steer,cars[0].braking);
        assert(fabsf(cars[0].steer)<.001f && cars[0].braking);
    }
    /* Centimetres of correction within a clear held lane are not a new
       merge through a faster car in the neighbouring lane. */
    for(int side=-1;side<=1;side+=2) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={20,.01f*side,0},.spd=100/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        cars[1]=cars[0];cars[1].pos[0]-=10;cars[1].pos[1]=-1.95f*side;cars[1].spd=140/3.6f/PHYS_TICKRATE;
        drivers[0]=(AiRace){.progress={.active=1,.lap=1,.maxlaps=1,.ngate=2,
            .gate={{.dx=1,.half=10},{.x=390,.dx=1,.half=10}}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        ai_race_step(cars,drivers,0,2,0,&world,NULL);
        fprintf(stderr,"held lane with rear side%d offset%.3f\n",side,drivers[0].offset);
        assert(fabsf(drivers[0].offset)<.001f && !drivers[0].blocked);
    }
    for(int rear=0;rear<2;rear++)for(int side=-1;side<=1;side+=2) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={20,6*side,0},.spd=100/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        cars[1]=cars[0];cars[1].pos[1]=2*side;
        if(rear){cars[1].pos[0]-=10;cars[1].spd=140/3.6f/PHYS_TICKRATE;}
        drivers[0]=(AiRace){.offset=-2*side,.progress={.active=1,.lap=1,.maxlaps=1,.ngate=2,
            .gate={{.dx=1,.half=10},{.x=390,.dx=1,.half=10}}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        int overlap=0;
        for(int tick=0;tick<120;tick++) {
            ai_race_step(cars,drivers,0,2,0,&world,NULL);
            overlap+=phys_ai_overlap(cars,cars+1);cars[1].pos[0]+=cars[1].spd;
        }
        fprintf(stderr,"adjacent merge side%d rear%d overlap%d\n",side,rear,overlap);assert(!overlap && !drivers[0].drive.failed);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* A fixed lane crossing a source vertex must not jump the steering target
   when the raw segment normal changes. Both mirrored turns are covered. */
static void lane_vertex_test(void) {
    float floor[]={-100,-100,0,0,0,200,-100,0,0,0,200,100,0,0,0,-100,100,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-100,-100,200,100}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);AiTrafficWorld world={.scene=&scene,.race_pace=60};
    for(int side=-1;side<=1;side+=2) {
        float xy[]={-40,0,0,0,20,0,40,10*side,80,10*side};N2Path path={.xy=xy,.n=5};
        float previous=0,jump=0;
        for(int sample=0;sample<=500;sample++) {
            AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
            cars[0]=(AiCar){.pos={10+sample*.01f,3*side,0},.spd=10/PHYS_TICKRATE,.t=1,
                .half_length=2,.half_width=.85f,.height=1.5f};
            drivers[0]=(AiRace){.offset=3*side,.progress={.active=1,.lap=1,.maxlaps=1,.ngate=2,
                .gate={{.dx=1,.half=10},{.x=75,.dx=1,.half=10}}}};
            assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            if(sample)jump=fmaxf(jump,fabsf(cars[0].steer-previous));previous=cars[0].steer;
        }
        fprintf(stderr,"lane vertex side%d steering-step%.5f\n",side,jump);assert(jump<.01f);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* A moving leader at a steady course gap must not be treated as a wall.
   The same geometry with a stopped leader must still command braking. */
static void moving_gap_test(void) {
    float floor[]={-50,-50,0,0,0,500,-50,0,0,0,500,50,0,0,0,-50,50,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-50,-50,500,50}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    float xy[]={0,0,100,0,200,0,300,0,400,0};N2Path path={.xy=xy,.n=5};
    AiTrafficWorld world={.scene=&scene,.race_pace=100};
    for(int moving=0;moving<2;moving++) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={20,0,0},.spd=60/3.6f/PHYS_TICKRATE,.t=1,
            .half_length=2,.half_width=.85f,.height=1.5f};
        cars[1]=cars[0];cars[1].pos[0]+=12;cars[1].spd=moving?cars[0].spd:0;
        drivers[0].progress=(WRace){.active=1,.lap=1,.maxlaps=1,.ngate=2,
            .gate={{.dx=1,.half=10},{.x=390,.dx=1,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        ai_race_step(cars,drivers,0,2,0,&world,NULL);
        fprintf(stderr,"course gap moving%d target%.2f\n",moving,PHYS_KMH(cars[0].target_speed));
        if(moving)assert(PHYS_KMH(cars[0].target_speed)>=50);
        else assert(cars[0].braking && PHYS_KMH(cars[0].target_speed)<50);
    }
    /* A dense route must still reveal stopped traffic inside braking
       distance when weak brakes need more than two seconds of preview. */
    {
        float dense[802];N2Path straight={.xy=dense,.n=401};
        for(int i=0;i<=400;i++){dense[2*i]=(float)i;dense[2*i+1]=0;}
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={20,0,0},.spd=160/3.6f/PHYS_TICKRATE,.t=1,
            .half_length=2,.half_width=.85f,.height=1.5f,
            .vehicle={1,.25f,1,1,1,1}};
        cars[1]=cars[0];cars[1].pos[0]+=180;cars[1].spd=0;
        drivers[0].progress=(WRace){.active=1,.lap=1,.maxlaps=1,.ngate=2,
            .gate={{.dx=1,.half=10},{.x=390,.dx=1,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&straight,cars[0].pos,0));
        world.race_pace=180;
        ai_race_step(cars,drivers,0,2,0,&world,NULL);
        fprintf(stderr,"distant stopped lead target%.2f\n",PHYS_KMH(cars[0].target_speed));
        assert(cars[0].braking && PHYS_KMH(cars[0].target_speed)<100);
        world.race_pace=100;
    }
    /* A collision spin must turn back toward the initialized route, not
       accelerate away because a directly rearward target has zero curvature. */
    for(int side=-1;side<=1;side+=2) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={100,0,0},.spd=40/3.6f/PHYS_TICKRATE,.t=1,
            .half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0].progress=(WRace){.active=1,.lap=1,.maxlaps=1,.ngate=2,
            .gate={{.dx=1,.half=10},{.x=390,.dx=1,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        cars[0].head=side*3.12159265f;
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
        fprintf(stderr,"spun racer side%d steering%.3f target%.2f\n",side,cars[0].steer,PHYS_KMH(cars[0].target_speed));
        assert(cars[0].steer*side<-.1f && PHYS_KMH(cars[0].target_speed)<20 && !drivers[0].drive.failed);
    }
    /* The leader is around a bend, outside our straight body ray. */
    float arc[202];N2Path curve={.xy=arc,.n=101};
    for(int i=0;i<=100;i++){float a=6.283185307f*i/100;arc[2*i]=40*cosf(a);arc[2*i+1]=40*sinf(a);}
    arc[200]=arc[0];arc[201]=arc[1];
    AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
    cars[0]=(AiCar){.pos={40,0,0},.head=1.570796327f,.spd=60/3.6f/PHYS_TICKRATE,.t=1,
        .half_length=2,.half_width=.85f,.height=1.5f};
    cars[1]=cars[0];cars[1].pos[0]=40*cosf(.4f);cars[1].pos[1]=40*sinf(.4f);
    cars[1].head+=.4f;cars[1].spd=30/3.6f/PHYS_TICKRATE;
    drivers[0].progress=(WRace){.active=1,.lap=1,.maxlaps=20,.ngate=2,
        .gate={{.x=40,.dy=1,.half=10},{.x=-40,.dy=-1,.half=10}}};
    assert(ai_drive_init(&drivers[0].drive,&curve,cars[0].pos,cars[0].head));
    ai_race_step(cars,drivers,0,2,1,&world,NULL);
    fprintf(stderr,"curved gap target%.2f\n",PHYS_KMH(cars[0].target_speed));
    assert(PHYS_KMH(cars[0].target_speed)<65 && !drivers[0].drive.failed);
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* A coarse authored bend must not make clear adjacent lanes crawl as if
   occupied. Exercise actual steering/braking, support and body separation. */
static void side_by_side_corner_test(void) {
    float floor[]={-100,-300,0,0,0,600,-300,0,0,0,600,300,0,0,0,-100,300,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-100,-300,600,300}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);AiTrafficWorld world={.scene=&scene,.race_pace=110,.race_difficulty=2};
    for(int side=-1;side<=1;side+=2) {
        float xy[162];N2Path path={.xy=xy,.n=81};
        for(int i=0;i<=80;i++) {
            float s=5.0f*i;
            xy[2*i]=s<=60?s:60+(s-60)*.8660254f;
            xy[2*i+1]=s<=60?0:side*(s-60)*.5f;
        }
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};float lowest=INFINITY;
        for(int k=0;k<2;k++) {
            cars[k]=(AiCar){.pos={0,k?2.3f:-2.3f,0},.spd=80/3.6f/PHYS_TICKRATE,
                .half_length=2,.half_width=.85f,.height=1.5f};
            drivers[k]=(AiRace){.offset=cars[k].pos[1],.progress={.active=1,.lap=1,.maxlaps=1,.ngate=2,
                .gate={{.dx=1,.half=20},{.x=300,.dx=1,.half=300}}}};
            assert(ai_drive_init(&drivers[k].drive,&path,cars[k].pos,0));
        }
        int passed[2]={0};
        for(int tick=0;tick<1200;tick++) {
            for(int k=0;k<2;k++) {
                ai_race_step(cars,drivers,k,2,0,&world,NULL);
                if(cars[k].pos[0]>40 && cars[k].pos[0]<85)lowest=fminf(lowest,PHYS_KMH(cars[k].spd));
                passed[k]|=cars[k].pos[0]>120;
                assert(!drivers[k].drive.failed && cars[k].ride.contact_mask==15);
                assert(fabsf(ai_course_lateral(&path,cars[k].pos)-drivers[k].offset)<3);
            }
            assert(!phys_ai_overlap(cars,cars+1));
            if(passed[0] && passed[1])break;
        }
        fprintf(stderr,"side-by-side corner side%d minimum%.2f passed%d/%d\n",side,lowest,passed[0],passed[1]);
        assert(passed[0] && passed[1] && lowest>40);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* Steady circular lanes expose a bearing-only controller's persistent
   understeer. Test both turn directions and physical integration. */
static void steady_lane_test(void) {
    float floor[]={-100,-100,0,0,0,100,-100,0,0,0,100,100,0,0,0,-100,100,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-100,-100,100,100}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    for(int side=-1;side<=1;side+=2) {
        float xy[202];N2Path path={.xy=xy,.n=101};
        for(int i=0;i<=100;i++){float a=side*6.283185307f*i/100;xy[2*i]=40*cosf(a);xy[2*i+1]=40*sinf(a);}
        xy[200]=xy[0];xy[201]=xy[1];
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={40,0,0},.head=side*1.570796327f,.spd=60/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0].progress=(WRace){.active=1,.lap=1,.maxlaps=20,.ngate=2,
            .gate={{.x=40,.dy=side,.half=10},{.x=-40,.dy=-side,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,cars[0].head));
        AiTrafficWorld world={.scene=&scene,.race_pace=60};float error=0;
        for(int tick=0;tick<1200;tick++) {
            ai_race_step(cars,drivers,0,1,1,&world,NULL);
            if(tick>180)error=fmaxf(error,fabsf(hypotf(cars[0].pos[0],cars[0].pos[1])-40));
            assert(!drivers[0].drive.failed && !drivers[0].blocked && cars[0].ride.contact_mask==15);
        }
        fprintf(stderr,"steady lane side%d peak-error%.3f speed%.1f\n",side,error,PHYS_KMH(cars[0].spd));
        assert(error<1 && PHYS_KMH(cars[0].spd)>55);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* A straight source lane does not make the current recovery turn straight.
   Brake for the actual pursuit demand before accelerating onto its exit. */
static void pursuit_recovery_speed_test(void) {
    float floor[]={-50,-80,0,0,0,450,-80,0,0,0,450,80,0,0,0,-50,80,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-50,-80,450,80}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    float xy[]={0,0,100,0,200,0,300,0,400,0};N2Path path={xy,5};
    for(int side=-1;side<=1;side+=2) {
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={20,0,0},.head=side*.7f,.spd=60/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0].progress=(WRace){.active=1,.lap=1,.maxlaps=1,.ngate=2,.next=1,
            .gate={{.x=20,.dx=1,.half=12},{.x=350,.dx=1,.half=12}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,cars[0].head));
        AiTrafficWorld world={.scene=&scene,.race_pace=180};
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
        fprintf(stderr,"pursuit recovery side%d initial target%.2f speed%.2f\n",side,
            PHYS_KMH(cars[0].target_speed),PHYS_KMH(cars[0].spd));
        assert(PHYS_KMH(cars[0].target_speed)<40 && cars[0].braking);
        float peak=0;
        for(int tick=0;tick<9000 && !drivers[0].progress.finished;tick++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            peak=fmaxf(peak,fabsf(cars[0].pos[1]));
            assert(!drivers[0].drive.failed && !drivers[0].blocked && cars[0].ride.contact_mask==15);
        }
        fprintf(stderr,"pursuit recovery side%d peak error%.3f finished%d\n",side,peak,drivers[0].progress.finished);
        assert(drivers[0].progress.finished && peak<12);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

/* With pace far above it, a steady 40 m corner is driven close to the
   chassis corner speed: the margin, not extra caution, sets the speed. */
static void corner_margin_test(void) {
    float floor[]={-100,-100,0,0,0,100,-100,0,0,0,100,100,0,0,0,-100,100,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};WGroundGrid grid={0};float bounds[][4]={{-100,-100,100,100}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    phys_wall_index_build(NULL,NULL,0);
    float corner=PHYS_KMH(phys_vehicle_corner_speed(NULL,40));
    for(int side=-1;side<=1;side+=2) {
        float xy[202];N2Path path={.xy=xy,.n=101};
        for(int i=0;i<=100;i++){float a=side*6.283185307f*i/100;xy[2*i]=40*cosf(a);xy[2*i+1]=40*sinf(a);}
        xy[200]=xy[0];xy[201]=xy[1];
        AiCar cars[N_RACE_AI]={0};AiRace drivers[N_RACE_AI]={0};
        cars[0]=(AiCar){.pos={40,0,0},.head=side*1.570796327f,.spd=40/3.6f/PHYS_TICKRATE,
            .half_length=2,.half_width=.85f,.height=1.5f};
        drivers[0].progress=(WRace){.active=1,.lap=1,.maxlaps=20,.ngate=2,
            .gate={{.x=40,.dy=side,.half=10},{.x=-40,.dy=-side,.half=10}}};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,cars[0].head));
        AiTrafficWorld world={.scene=&scene,.race_pace=180};float error=0,sum=0;int n=0;
        for(int tick=0;tick<1200;tick++) {
            ai_race_step(cars,drivers,0,1,1,&world,NULL);
            if(tick>600){error=fmaxf(error,fabsf(hypotf(cars[0].pos[0],cars[0].pos[1])-40));sum+=PHYS_KMH(cars[0].spd);n++;}
            assert(!drivers[0].drive.failed && !drivers[0].blocked && cars[0].ride.contact_mask==15);
        }
        fprintf(stderr,"corner margin side%d speed%.1f corner%.1f ratio%.3f peak-error%.3f\n",side,sum/n,corner,sum/n/corner,error);
        /* Measured: .815 of corner speed with the old .85 margin, .905 with .95. */
        assert(sum/n>.87f*corner && sum/n<corner && error<2);
    }
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
}

int main(void) {
    corner_margin_test();
    side_by_side_corner_test();
    adjacent_merge_test();
    lane_vertex_test();
    moving_gap_test();
    street_turn_course_test();
    street_turn_pace_test();
    pursuit_recovery_speed_test();
    steady_lane_test();
    race_testdrive_test();
    oriented_rival_test();
    bend_rival_test();
    held_lane_test();
    narrow_passing_test();
    rail_replacement_test();
    descending_deck_test();
    render_turn_timing();
    render_timing();
    event_metadata();
    RaceDrift drift={0};float slip[]={.3f,.12f},straight[]={.3f,0};
    for(int k=0;k<60;k++)race_drift_step(&drift,slip,0,.3f,1,0,1/60.0f);
    assert(drift.chain>0 && drift.bank==0);
    race_drift_step(&drift,slip,0,.3f,1,1,1/60.0f);assert(drift.chain==0 && drift.bank==0);
    for(int k=0;k<60;k++)race_drift_step(&drift,slip,0,.3f,1,0,1/60.0f);
    for(int k=0;k<62;k++)race_drift_step(&drift,straight,0,.3f,1,0,1/60.0f);
    assert(drift.bank>0 && drift.chain==0);
    float banked=drift.bank;race_drift_step(&drift,slip,0,.3f,0,0,1/60.0f);
    assert(drift.bank==banked && drift.chain==0);
    N2PhysicsAttr motor={.idle_rpm=800,.redline_rpm=6500,.limiter_rpm=7000};
    motor.gearbox[0]=(N2GearboxAttr){.final_drive=4,.forward={3,2,1},.gear_count=3};
    for(int k=0;k<9;k++)motor.torque[k]=1;
    PhysManual manual={0};assert(phys_manual_step(&manual,&motor,0,0,.3f,0,1,0,1/60.0f)>0 && manual.gear==1);
    float first=phys_manual_step(&manual,&motor,0,0,.3f,.3f,1,0,1/60.0f),rpm=manual.rpm;
    assert(phys_manual_step(&manual,&motor,0,0,.3f,.3f,1,1,1/60.0f)==0 && manual.gear==2 && manual.rpm<rpm);
    for(int k=0;k<12;k++)phys_manual_step(&manual,&motor,0,0,.3f,.3f,1,0,1/60.0f);
    assert(phys_manual_step(&manual,&motor,0,0,.3f,.3f,1,0,1/60.0f)<first);
    for(int k=0;k<100;k++)phys_manual_step(&manual,&motor,0,0,.3f,1,1,0,1/60.0f);
    assert(manual.failed && manual.heat==1);
    assert(phys_manual_step(&manual,&motor,0,0,.3f,0,1,0,1/60.0f)==0);
    WRace seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=4,
        .gate={{.x=0,.dx=1,.half=10},{.x=80,.dx=1,.half=10},{.x=180,.dx=1,.half=10}},
        .grid={{12,-2.5f,0},{12,2.5f,0},{4,-2.5f,0},{4,2.5f,0}}};
    WRace r=seed;assert(world_race_progress_value(&r,20,0)==0);
    world_race_begin(&r,20,0);assert(r.lap==1 && r.next==1);
    assert(!world_race_progress_update(&r,0,20,20));
    assert(!world_race_progress_update(&r,0,190,20)); /* miss ordered gate */
    assert(!r.finished && r.cleared==0);
    world_race_progress_update(&r,0,90,0);world_race_progress_update(&r,0,70,0);
    assert(!r.cleared); /* reversing is not a checkpoint */
    assert(world_race_progress_update(&r,0,90,0) && r.next==2);
    float score=world_race_progress_value(&r,90,0);
    assert(world_race_progress_value(&r,150,0)>score);
    assert(world_race_progress_update(&r,0,190,0) && r.finished);
    assert(world_race_progress_value(&r,190,0)==3);
    r=seed;r.maxlaps=2;world_race_begin(&r,20,0);
    for(int lap=0;lap<2;lap++) {
        assert(world_race_progress_update(&r,1,90,0));
        assert(world_race_progress_update(&r,1,190,0));
        world_race_progress_update(&r,1,-10,20);world_race_progress_update(&r,1,-10,0);
        assert(world_race_progress_update(&r,1,10,0));
        assert(r.finished==(lap==1));
    }

    float course_xy[]={0,0,100,0};N2Path source_course={course_xy,2};
    WRace misplaced={.active=1,.maxlaps=1,.ngate=3,
        .gate={{.x=0,.y=30,.dy=1,.half=10},{.x=50,.y=30,.dy=1,.half=10},{.x=100,.y=30,.dy=1,.half=10}}};
    assert(world_race_bind_course(&misplaced,&source_course,10));
    assert(misplaced.gate[1].y==0 && misplaced.gate[1].dx==1 && misplaced.gate[1].dy==0);
    world_race_begin(&misplaced,10,0);
    assert(world_race_progress_update(&misplaced,0,60,0));
    assert(world_race_progress_update(&misplaced,0,110,0) && misplaced.finished);
    assert(!world_race_bind_course(&misplaced,&source_course,10)); /* no mid-race reordering */

    char root[]="/tmp/openug2-race-XXXXXX";assert(mkdtemp(root));
    char dir[512];snprintf(dir,sizeof dir,"%s/ROUTESTEST",root);assert(!mkdir(dir,0700));
    starting_grid_test(root);
    WEvent segmented={.id=1,.reg="TEST",.npoly=3,.poly={{0,0},{160,0},{320,0}}};
    for(int broken=0;broken<=7;broken++) {
        segmented_file(root,broken);N2Path ordered={0};
        int built=ai_race_course(root,&segmented,&ordered);
        if(broken)assert(!built && !ordered.xy);
        else {
            assert(built && ordered.n==9);
            for(int k=0;k<ordered.n;k++)assert(ordered.xy[2*k]==k*40 && ordered.xy[2*k+1]==0);
            free(ordered.xy);
        }
    }
    segmented_file(root,0);segmented.circuit=1;segmented.poly[2][0]=0;
    N2Path joined={0};assert(ai_race_course(root,&segmented,&joined) && joined.n==9);
    assert(joined.xy[0]==joined.xy[2*joined.n-2]);free(joined.xy);
    segmented.circuit=0;segmented.poly[2][0]=NAN;
    assert(!ai_race_course(root,&segmented,&joined));
    route_file(root,1); /* force authored distance traversal */
    WEvent reset_event={.id=1,.reg="TEST",.npoly=2,.poly={{0,0},{200,0}}};
    for(int reverse=0;reverse<2;reverse++) {
        progress_reset_file(root,reverse);N2Path ordered={0};
        assert(ai_race_course(root,&reset_event,&ordered));
        assert(ordered.n==41 && ordered.xy[0]==0 && ordered.xy[2*ordered.n-2]==200);
        for(int k=0;k<ordered.n;k++)assert(fabsf(ordered.xy[2*k]-5*k)<.00002f && ordered.xy[2*k+1]==0);
        free(ordered.xy);
    }
    puts("branch progress resets: both point orders retain the full source course PASS");
    route_file(root,0);
    float floor[]={-100,-100,0,0,0,300,-100,0,0,0,300,100,0,0,0,-100,100,0,0,0};
    float wall[]={60,-100,0,0,0,60,100,0,0,0,60,100,4,0,0,60,-100,4,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
        {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    N2Scene scene={meshes,1,2};WGroundGrid grid={0};
    float bounds[][4]={{-100,-100,300,100},{60,-100,60,100}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    AiTrafficWorld world={.scene=&scene};
    AiCar cars[N_RACE_AI]={0},player={.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f};
    AiRace drivers[N_RACE_AI]={0};N2Path path={0};WEvent event={.id=1,.reg="TEST"};
    for(int k=0;k<N_RACE_AI;k++)cars[k]=(AiCar){.half_length=2,.half_width=1,.height=1.5f};
    float repaired=0,oldgrid[]={20,0,50};
    assert(world_race_grid_ground(&scene,&event,oldgrid,&repaired) && repaired==0);
    float deck[20];memcpy(deck,floor,sizeof deck);for(int k=0;k<4;k++)deck[k*5+2]=10;
    N2Mesh stacked[]={meshes[0],meshes[0]};stacked[1].verts=deck;
    N2Scene layers={stacked,2,2};
    assert(!world_race_grid_ground(&layers,&event,oldgrid,&repaired));
    oldgrid[2]=10;assert(world_race_grid_ground(&layers,&event,oldgrid,&repaired) && repaired==10);
    oldgrid[2]=0;assert(world_race_grid_ground(&layers,&event,oldgrid,&repaired) && repaired==0);
    /* Authored grids can sit on terrain-labelled pavement. Match the shipped
       height and full wheel footprint, keeping stacked/stale terrain ambiguous. */
    meshes[0].cat=N2_TERRAIN;oldgrid[2]=0;
    assert(world_race_grid_ground(&scene,&event,oldgrid,&repaired) && repaired==0);
    oldgrid[2]=.3f;assert(!world_race_grid_ground(&scene,&event,oldgrid,&repaired));
    oldgrid[2]=5;assert(!world_race_grid_ground(&scene,&event,oldgrid,&repaired));
    for(int k=0;k<4;k++)floor[k*5+2]=2*floor[k*5];
    oldgrid[2]=40;assert(!world_race_grid_ground(&scene,&event,oldgrid,&repaired));
    for(int k=0;k<4;k++)floor[k*5+2]=0;
    assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player)==4);
    for(int k=0;k<4;k++)assert(cars[k].ride.contact_mask==15 && !drivers[k].drive.failed);
    for(int tick=0;tick<4000;tick++) {
        int done=0;AiCar *contacts[4];
        for(int k=0;k<4;k++){ai_race_step(cars,drivers,k,4,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;}
        ai_car_contacts(contacts,4,&world,NULL);if(done==4)break;
    }
    for(int k=0;k<4;k++)assert(drivers[k].progress.finished && !drivers[k].drive.failed);
    /* A shallow pavement ramp changes grade under the wheel footprint. */
    float ramp[]={-100,-100,0,0,0, 55,-100,0,0,0, 60,-100,0,0,0, 65,-100,1,0,0, 300,-100,48,0,0,
                  -100,100,0,0,0, 55,100,0,0,0, 60,100,0,0,0, 65,100,1,0,0, 300,100,48,0,0};
    uint16_t rampidx[]={0,1,6,0,6,5,1,2,7,1,7,6,2,3,8,2,8,7,3,4,9,3,9,8};
    N2Mesh saved=meshes[0];meshes[0].verts=ramp;meshes[0].nverts=10;meshes[0].idx=rampidx;meshes[0].nidx=24;
    assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player)==4);
    for(int tick=0;tick<4000;tick++) {
        int done=0;AiCar *contacts[4];
        for(int k=0;k<4;k++){ai_race_step(cars,drivers,k,4,0,&world,NULL);contacts[k]=cars+k;done+=drivers[k].progress.finished;}
        ai_car_contacts(contacts,4,&world,NULL);if(done==4)break;
    }
    for(int k=0;k<4;k++)assert(drivers[k].progress.finished && !drivers[k].drive.failed && cars[k].pos[2]>20);
    meshes[0]=saved;meshes[0].cat=N2_ROAD;
    /* A decoded distance course may lie on another road. Prefer it when it
       joins the grid; otherwise try the existing linked source spine. */
    segmented.poly[2][0]=320;segmented_file(root,0);lane_file(root,5);
    N2Path distant={0};AiDrive entrance;
    assert(ai_race_course(root,&segmented,&distant));
    assert(!ai_drive_join(&entrance,&distant,player.pos,ai_course_heading(&distant,player.pos),8));
    free(distant.xy);
    assert(ai_race_prepare(root,&segmented,&seed,&world,&path,cars,drivers,&player)==4);
    assert(path.n==9 && path.xy[1]==0);
    for(int tick=0;tick<2400;tick++) {
        int done=0;AiCar *contacts[4];
        for(int k=0;k<4;k++) {
            ai_race_step(cars,drivers,k,4,0,&world,NULL);contacts[k]=cars+k;
            done+=drivers[k].progress.finished;
        }
        ai_car_contacts(contacts,4,&world,NULL);if(done==4)break;
    }
    for(int k=0;k<4;k++)assert(drivers[k].progress.finished && !drivers[k].drive.failed);
    segmented_file(root,7); /* disconnected links cannot force a grid join */
    assert(!ai_race_prepare(root,&segmented,&seed,&world,&path,cars,drivers,&player));
    route_file(root,0);
    snprintf(dir,sizeof dir,"%s/ROUTESTEST/Routes1F.bin",root);assert(!unlink(dir));
    int count=ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player);
    assert(count==4 && path.n==5);
    for(int k=0;k<count;k++) {
        assert(!cars[k].spd && cars[k].ride.contact_mask==15 && !drivers[k].drive.failed);
        assert(drivers[k].progress.lap==0 && world_race_progress_value(&drivers[k].progress,cars[k].pos[0],cars[k].pos[1])==0);
        assert(!phys_ai_overlap(&cars[k],&player));
        for(int j=k+1;j<count;j++)assert(!phys_ai_overlap(&cars[k],&cars[j]));
    }
    /* A stationary player blocking a narrow lane is yielded to. */
    floor[1]=floor[6]=-1.1f;floor[11]=floor[16]=1.1f;bounds[0][1]=-1.1f;bounds[0][3]=1.1f;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    cars[0].pos[1]=0;drivers[0].drive.stalled=0;
    for(int tick=0;tick<400;tick++)ai_race_step(cars,drivers,0,1,0,&world,&player);
    assert(cars[0].pos[0]+4<=player.pos[0] && !drivers[0].drive.failed);
    floor[1]=floor[6]=-100;floor[11]=floor[16]=100;bounds[0][1]=-100;bounds[0][3]=100;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    player.pos[0]=300;
    for(int tick=0;tick<1800;tick++)ai_race_step(cars,drivers,0,1,0,&world,&player);
    assert(drivers[0].progress.finished && !drivers[0].drive.failed);
    assert(cars[0].pos[0]>180 && fabsf(cars[0].pos[1])<.5f && cars[0].ride.contact_mask==15);

    /* A reverse event shares the same open path. Read its outline direction
       before placing cars or sorting gates; otherwise intermediate gates are
       behind the grid and cars stop at the wrong end without finishing. */
    WEvent reversed={.id=1,.reg="TEST",.npoly=2,.poly={{200,0},{0,0}},.info={.kind=N2_RACE_DRAG}};
    WRace reverse_seed={.active=1,.maxlaps=1,.ngate=3,.ngrid=1,
        .gate={{.x=200,.dx=-1,.half=10},{.x=100,.dx=-1,.half=10},{.dx=-1,.half=10}},
        .grid={{180,-5,0}}};
    assert(ai_race_prepare(root,&reversed,&reverse_seed,&world,&path,cars,drivers,
        &(AiCar){.pos={180,5,0},.half_length=2,.half_width=1,.height=1.5f})==1);
    assert(path.xy[0]==200 && path.xy[2*path.n-2]<0 && drivers[0].drive.direction==1);
    for(int tick=0;tick<3000 && !drivers[0].progress.finished;tick++)
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(drivers[0].progress.finished && !drivers[0].drive.failed && cars[0].pos[0]<0);
    N2Path invalid={0};reversed.poly[1][0]=NAN;
    assert(!ai_race_course(root,&reversed,&invalid) && !invalid.xy);

    assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f})==4);
    /* After touching a verge, improve partial support back onto the road.
       Requiring four supported corners at the first approach sample deadlocks. */
    cars[0].pos[1]=1.5f;cars[0].head=0;cars[0].ride_ready=0;
    floor[1]=floor[6]=-2;floor[11]=floor[16]=2;
    for(int tick=0;tick<2500 && !drivers[0].progress.finished;tick++)
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(drivers[0].progress.finished && !drivers[0].drive.failed);
    assert(fabsf(cars[0].pos[1])<.5f);
    floor[1]=floor[6]=-100;floor[11]=floor[16]=100;
    assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f})==4);
    world_ground_grid_free(&grid);scene.count=2;
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obs[2][4],oz[2][2];int src[2];world.nobst=phys_collect_walls(&scene,obs,src,oz,2);
    world.obst=obs;world.obstz=oz;world.obstsrc=src;
    phys_wall_index_build(obs,oz,world.nobst);
    for(int tick=0;tick<1500;tick++)ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(cars[0].pos[0]+cars[0].half_length<60.001f && !drivers[0].progress.finished);
    assert(isfinite(cars[0].pos[2]) && cars[0].ride.contact_mask==15);
    /* Seeing a blockage is not a Drift collision. Actual wall contact is. */
    drivers[0].progress.kind=N2_RACE_DRIFT;drivers[0].progress.drift=(RaceDrift){.chain=300};
    ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(drivers[0].blocked && drivers[0].progress.drift.chain==300);
    cars[0].pos[0]=60;cars[0].ride_ready=0;
    ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(drivers[0].progress.drift.chain==0);
    /* Solo downhill keeps a player course/driver, without spawning opponents. */
    scene.count=1;world.nobst=0;phys_wall_index_build(NULL,NULL,0);
    event.info.kind=N2_RACE_DRIFT;event.info.downhill=1;WRace solo=seed;solo.kind=N2_RACE_DRIFT;
    AiCar solo_player={.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f};
    assert(ai_race_prepare(root,&event,&solo,&world,&path,cars,drivers,&solo_player)==0);
    assert(path.n==5 && drivers[0].drive.path==&path && !drivers[0].drive.failed);
    cars[0]=solo_player;
    for(int tick=0;tick<1800 && !drivers[0].progress.finished;tick++)
        ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(drivers[0].progress.finished && !drivers[0].drive.failed);
    event.info=(N2EventInfo){0};scene.count=2;
    /* A short centre divider must be steered around, never made non-solid. */
    for(int k=0;k<4;k++)wall[k*5+1]=(k==0 || k==3)?-1:1;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));
    world_ground_grid_activate(&grid);
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,world.nobst);
    assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f})==4);
    for(int tick=0;tick<2400;tick++)ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(drivers[0].progress.finished && !drivers[0].drive.failed);
    /* A wider search must not replace a clear conservative line with a
       slightly cheaper edge-hugging current offset. It is a fallback only. */
    cars[0]=(AiCar){.pos={48,2.1f,0},.half_length=2,.half_width=1,.height=1.5f};
    drivers[0]=(AiRace){.progress=seed};
    assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
    ai_race_step(cars,drivers,0,1,0,&world,NULL);
    assert(!drivers[0].blocked && fabsf(drivers[0].offset-2.2f)<.001f);
    /* A wide divider needs an early lane change, before braking leaves the
       car pinned at its nose. Use both directions of the source chain. */
    for(int k=0;k<4;k++)wall[k*5+1]=(k==0 || k==3)?-6:6;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));
    world_ground_grid_activate(&grid);
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,world.nobst);
    for(int reverse=0;reverse<2;reverse++) {
        if(reverse)for(int k=0;k<path.n;k++)path.xy[2*k]=200-path.xy[2*k];
        cars[0]=(AiCar){.pos={12,0,0},.half_length=2,.half_width=1,.height=1.5f};
        drivers[0]=(AiRace){.progress=seed};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        assert(drivers[0].drive.direction==(reverse?-1:1));
        int shifted_early=0;
        for(int tick=0;tick<2400;tick++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            if(cars[0].pos[0]<40 && fabsf(drivers[0].offset)>7)shifted_early=1;
            if(fabsf(cars[0].pos[0]-60)<2.1f)assert(fabsf(cars[0].pos[1])>6.9f);
        }
        assert(shifted_early && drivers[0].progress.finished && !drivers[0].drive.failed);
        /* On this wide road the safe passing lane remains valid after the
           divider. Settle into it rather than forcing a return to centre. */
        assert(fabsf(cars[0].pos[1]-(reverse?-1:1)*drivers[0].offset)<.5f);
    }
    /* A source-width grid can be on a narrow parallel lane beyond the fixed
       offsets. Only the current offset fits; its fence must remain solid. */
    float fence[]={-100,12,0,0,0,300,12,0,0,0,300,12,4,0,0,-100,12,4,0,0};
    meshes[1].verts=fence;
    floor[1]=floor[6]=19;floor[11]=floor[16]=21;
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,world.nobst);
    const float centres[]={20,3.5f};
    for(int lane=0;lane<2;lane++) {
        float centre=centres[lane];
        floor[1]=floor[6]=centre-1;floor[11]=floor[16]=centre+1;
        cars[0]=(AiCar){.pos={12,centre,0},.half_length=3,.half_width=lane?1.4f:1,.height=1.5f};
        WRace parallel=seed;for(int k=0;k<parallel.ngate;k++)parallel.gate[k].half=30;
        drivers[0]=(AiRace){.progress=parallel};
        assert(ai_drive_join(&drivers[0].drive,&path,cars[0].pos,0,21));
        for(int tick=0;tick<2400 && !drivers[0].progress.finished;tick++) {
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
            assert(fabsf(cars[0].pos[1]-centre)<.01f && cars[0].ride.contact_mask==15);
        }
        assert(drivers[0].progress.finished && !drivers[0].drive.failed);
        assert(fabsf(fabsf(drivers[0].offset)-centre)<.001f);
    }
    floor[1]=floor[6]=-100;floor[11]=floor[16]=100;
    meshes[1].verts=wall;
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,world.nobst);
    /* Curved source lanes guide the whole field around the planted divider. */
    for(int reverse=0;reverse<2;reverse++) {
        lane_file(root,reverse?4:0);
        assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,
            &(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f})==4);
        assert(path.n>5);int curved=0;
        for(int k=0;k<path.n;k++)if(path.xy[2*k]<80 && path.xy[2*k+1]<-8)curved=1;
        assert(curved);
        for(int tick=0;tick<4000;tick++) {
            for(int k=0;k<4;k++)ai_race_step(cars,drivers,k,4,0,&world,NULL);
            AiCar *contacts[]={&cars[0],&cars[1],&cars[2],&cars[3]};
            ai_car_contacts(contacts,4,&world,NULL);
        }
        for(int k=0;k<4;k++)assert(drivers[k].progress.finished && !drivers[k].drive.failed);
    }
    for(int broken=1;broken<=3;broken++) {
        lane_file(root,broken);
        assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,
            &(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f})==4);
        assert(path.n==5); /* reject the malformed lanes without changing the spine */
    }
    lane_file(root,5);
    assert(ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,
        &(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f})==4);
    assert(path.n==5); /* a valid but unreachable lane must not strand the grid */
    snprintf(dir,sizeof dir,"%s/ROUTESTEST/Routes1F.bin",root);assert(!unlink(dir));
    /* Ending on the finish plane must not stop the field before it crosses,
       and a finished leader must roll out instead of blocking the followers. */
    world_ground_grid_free(&grid);scene.count=1;world.nobst=0;
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    WRace finish;
    for(int projected=0;projected<3;projected++) {
        if(projected==2) {
            floor[5]=floor[10]=214;bounds[0][2]=214;
            world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
        }
        finish=seed;finish.gate[2].x=200+50*(projected!=0);
        assert(ai_race_prepare(root,&event,&finish,&world,&path,cars,drivers,
            &(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f})==4);
        assert(fabsf(finish.gate[2].x-200)<.01f);
        assert(path.n==6);
        printf("finish run-out case%d source%.1f bound%.1f end%.3f\n",projected,projected?250.f:200.f,finish.gate[2].x,path.xy[2*path.n-2]);fflush(stdout);
        if(projected==2)assert(path.xy[2*path.n-2]>201 && path.xy[2*path.n-2]<220);
        else assert(path.xy[2*path.n-2]>220);
        for(int tick=0;tick<4000;tick++) {
            for(int k=0;k<4;k++)ai_race_step(cars,drivers,k,4,0,&world,NULL);
            AiCar *contacts[]={&cars[0],&cars[1],&cars[2],&cars[3]};
            ai_car_contacts(contacts,4,&world,NULL);
        }
        for(int k=0;k<4;k++)assert(drivers[k].progress.finished && !drivers[k].drive.failed && cars[k].pos[0]>200);
    }
    floor[5]=floor[10]=300;bounds[0][2]=300;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    /* An authored course can cross terrain-labelled pavement, including a
       coherent hill. The driver must use its ground plane for the footprint. */
    meshes[0].cat=N2_TERRAIN;
    for(int hill=0;hill<2;hill++) {
        float slope=hill?.3f:0;
        for(int v=0;v<4;v++)floor[5*v+2]=slope*floor[5*v];
        cars[0]=(AiCar){.pos={12,0,12*slope},.half_length=2,.half_width=1,.height=1.5f};
        drivers[0]=(AiRace){.progress=finish};
        assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
        for(int tick=0;tick<4000 && !drivers[0].progress.finished;tick++)
            ai_race_step(cars,drivers,0,1,0,&world,NULL);
        assert(drivers[0].progress.finished && !drivers[0].drive.failed);
        assert(cars[0].ride.contact_mask==15);
    }
    meshes[0].cat=N2_ROAD;for(int v=0;v<4;v++)floor[5*v+2]=0;
    route_file(root,1);assert(!ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player));
    assert(!path.n && !path.xy);
    route_file(root,0);event.circuit=1;
    assert(!ai_race_prepare(root,&event,&seed,&world,&path,cars,drivers,&player));
    /* A wrapped source distance produces a closed, physics-driven course.
       Completing a lap must re-arm gates and continue driving across the seam. */
    loop_file(root,1);event.info.length=251;
    N2Path circle={0};assert(ai_race_course(root,&event,&circle));
    assert(circle.n>40 && circle.xy[0]==circle.xy[2*circle.n-2]);
    AiDrive loopdrive;float start[]={140,0,0};assert(ai_drive_init(&loopdrive,&circle,start,1.5707963f));
    assert(loopdrive.loop);
    for(int lap=0;lap<3;lap++)for(int k=0;k<circle.n-1;k++) {
        float *p=circle.xy+2*k,pose[]={p[0],p[1],0};
        float heading=atan2f(circle.xy[2*k+3]-p[1],circle.xy[2*k+2]-p[0]);
        AiDriveInput input=ai_drive_step(&loopdrive,pose,heading,.1f);
        assert(!loopdrive.failed && !loopdrive.finished && !input.handbrake);
    }
    assert(loopdrive.turns>=2 && loopdrive.progress>2*loopdrive.length);
    WRace loopseed={.active=1,.maxlaps=2,.ngate=4,.ngrid=1,
        .gate={{.x=140,.dy=1,.half=12},{.x=100,.y=40,.dx=-1,.half=12},
               {.x=60,.dy=-1,.half=12},{.x=100,.y=-40,.dx=1,.half=12}},
        .grid={{139,-8,0}}};
    assert(ai_race_prepare(root,&event,&loopseed,&world,&path,cars,drivers,
        &(AiCar){.pos={139,8,0},.half_length=2,.half_width=1,.height=1.5f})==1);
    assert(drivers[0].drive.error_limit==21);
    /* Standalone drivers retain their conservative default; event callers may
       supply the corridor measured from the source, never a pose correction. */
    AiDrive wide;float outside[]={155,0,0};
    assert(!ai_drive_init(&wide,&circle,outside,1.5707963f));
    assert(ai_drive_join(&wide,&circle,outside,1.5707963f,21));
    assert(!ai_drive_join(&wide,&circle,outside,1.5707963f,NAN));
    assert(ai_drive_init(&wide,&circle,start,1.5707963f));ai_drive_step(&wide,outside,1.5707963f,.1f);assert(wide.failed);
    assert(ai_drive_init(&wide,&circle,start,1.5707963f));wide.error_limit=21;
    ai_drive_step(&wide,outside,1.5707963f,.1f);assert(!wide.failed);
    for(int tick=0;tick<5000 && !drivers[0].progress.finished;tick++)
        ai_race_step(cars,drivers,0,1,1,&world,NULL);
    assert(drivers[0].progress.finished && drivers[0].progress.lap==2 && !drivers[0].drive.failed);
    /* Uphill samples must stay on the road above a nearby terrain layer.
       A stale endpoint height can select that lower layer and change lanes. */
    loop_file(root,2);
    lane_heights(root,.12f,0,0);
    float hill[20],under[20];memcpy(hill,floor,sizeof hill);
    for(int k=0;k<4;k++)hill[5*k+2]=hill[5*k]*.12f;
    memcpy(under,hill,sizeof under);for(int k=0;k<4;k++)under[5*k+2]-=1.2f;
    N2Mesh slopes[]={meshes[0],meshes[0]};slopes[0].verts=hill;
    slopes[1].verts=under;slopes[1].cat=N2_TERRAIN;
    N2Scene hillside={slopes,2,2};AiTrafficWorld hillworld={.scene=&hillside};
    world_ground_grid_activate(NULL);
    WRace hillseed=loopseed;hillseed.grid[0][2]=139*.12f;
    assert(ai_race_prepare(root,&event,&hillseed,&hillworld,&path,cars,drivers,
        &(AiCar){.pos={139,8,139*.12f},.half_length=2,.half_width=1,.height=1.5f})==1);
    float radius=0;for(int k=0;k<path.n;k++)radius=fmaxf(radius,hypotf(path.xy[2*k]-100,path.xy[2*k+1]));
    assert(radius<40.1f);
    world_ground_grid_activate(&grid);
    /* A shorter authored lane blocked by event scenery must lose to a
       physically supported alternative, without deleting the barrier. */
    loop_file(root,2);
    float barrier[]={60,-5,0,0,0,60,5,0,0,0,60,5,4,0,0,60,-5,4,0,0};
    meshes[1].verts=barrier;scene.count=2;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));
    world_ground_grid_activate(&grid);
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,world.nobst);
    assert(ai_race_prepare(root,&event,&loopseed,&world,&path,cars,drivers,
        &(AiCar){.pos={139,8,0},.half_length=2,.half_width=1,.height=1.5f})==1);
    int detoured=0;
    for(int k=0;k<path.n;k++)if(fabsf(path.xy[2*k+1])<4 && path.xy[2*k]<100) {
        assert(path.xy[2*k]<57);detoured=1;
    }
    assert(detoured);
    /* A thin divider halfway between clear samples must not be crossed by
       a cheaper connection. Both endpoint poses alone miss this obstacle. */
    int edge=10;float mx=(circle.xy[2*edge]+circle.xy[2*edge+2])/2;
    float my=(circle.xy[2*edge+1]+circle.xy[2*edge+3])/2;
    float dx=circle.xy[2*edge+2]-circle.xy[2*edge],dy=circle.xy[2*edge+3]-circle.xy[2*edge+1];
    float seglen=hypotf(dx,dy),heading=atan2f(dy,dx),between[20];
    for(int v=0;v<4;v++) {
        float side=v==0 || v==3?-1:1;
        between[5*v]=mx-dy/seglen*side*3;between[5*v+1]=my+dx/seglen*side*3;
        between[5*v+2]=v>=2?4:0;between[5*v+3]=between[5*v+4]=0;
    }
    meshes[1].verts=between;
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,world.nobst);
    float bb[]={-2,-1,0,2,1,1.5f},vel[]={0,0};
    for(int q=0;q<2;q++) {
        float pose[]={circle.xy[2*(edge+q)],circle.xy[2*(edge+q)+1],0};
        assert(!collide_body_walls(pose,vel,heading,bb,obs,oz,world.nobst,.05f,1.5f,&scene,src,NULL,0));
    }
    assert(ai_race_prepare(root,&event,&loopseed,&world,&path,cars,drivers,
        &(AiCar){.pos={139,8,0},.half_length=2,.half_width=1,.height=1.5f})==1);
    for(int j=0;j<path.n-1;j++) {
        float pose[]={(path.xy[2*j]+path.xy[2*j+2])/2,(path.xy[2*j+1]+path.xy[2*j+3])/2,0};
        float h=atan2f(path.xy[2*j+3]-path.xy[2*j+1],path.xy[2*j+2]-path.xy[2*j]);
        assert(!collide_body_walls(pose,vel,h,bb,obs,oz,world.nobst,.05f,1.5f,&scene,src,NULL,0));
    }
    /* Choose a joinable authored course before rejecting its start slots.
       The cheaper inner loop cannot reach this grid within source width. */
    scene.count=1;world.nobst=0;phys_wall_index_build(NULL,NULL,0);
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));
    world_ground_grid_activate(&grid);
    AiCar outside_grid={.pos={161,8,0},.half_length=2,.half_width=1,.height=1.5f};
    N2Path cheaper={0};assert(ai_race_course(root,&event,&cheaper));
    assert(!ai_drive_join(&wide,&cheaper,outside_grid.pos,1.5707963f,20));free(cheaper.xy);
    loopseed.grid[0][0]=161;
    assert(ai_race_prepare(root,&event,&loopseed,&world,&path,cars,drivers,&outside_grid)==1);
    assert(ai_drive_join(&wide,&path,outside_grid.pos,ai_course_heading(&path,outside_grid.pos),20));
    /* Both source loops fit the width, but only the outer start lane can be
       reached from this grid without crossing the solid divider. */
    float entrance_wall[]={145,-12,0,0,0,145,12,0,0,0,145,12,4,0,0,145,-12,4,0,0};
    meshes[1].verts=entrance_wall;scene.count=2;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));
    world_ground_grid_activate(&grid);
    world.nobst=phys_collect_walls(&scene,obs,src,oz,2);phys_wall_index_build(obs,oz,world.nobst);
    outside_grid.pos[0]=151;loopseed.grid[0][0]=151;
    assert(ai_race_prepare(root,&event,&loopseed,&world,&path,cars,drivers,&outside_grid)==1);
    assert(path.xy[0]>149); /* not the shorter disconnected start lane at 140 */
    /* Closing a lap must retain the validated endpoint, not snap another
       lane across the divider to the start point. */
    for(int part=0;part<=20;part++) {
        int last=path.n-2;float t=(float)part/20;
        float dx=path.xy[2*last+2]-path.xy[2*last],dy=path.xy[2*last+3]-path.xy[2*last+1];
        float pose[]={path.xy[2*last]+dx*t,path.xy[2*last+1]+dy*t,0};
        assert(!collide_body_walls(pose,vel,atan2f(dy,dx),bb,obs,oz,world.nobst,.05f,1.5f,&scene,src,NULL,0));
    }
    for(int tick=0;tick<7000 && !drivers[0].progress.finished;tick++) {
        ai_race_step(cars,drivers,0,1,1,&world,NULL);
        assert(!drivers[0].drive.failed && cars[0].ride.contact_mask==15);
    }
    assert(drivers[0].progress.finished && drivers[0].progress.lap==2);
    /* Route geometry, not an invented distance period, closes this circuit. */
    junction_loop_file(root);route_file(root,1);free(path.xy);path=(N2Path){0};
    WEvent junction={.id=1,.circuit=1,.reg="TEST",.npoly=5,
        .poly={{0,0},{100,0},{108,100},{0,100},{0,0}}};junction.info.kind=N2_RACE_CIRCUIT;junction.info.length=400;
    assert(ai_race_course(root,&junction,&path));
    assert(path.n==9 && path.xy[0]==path.xy[2*path.n-2] && path.xy[1]==path.xy[2*path.n-1]);
    assert(path.xy[6]==108 && path.xy[7]==50); /* no 100 -> 99 backward seam */
    scene.count=1;world.nobst=0;phys_wall_index_build(NULL,NULL,0);
    floor[11]=floor[16]=200;bounds[0][3]=200;
    world_ground_grid_free(&grid);assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    WRace connected={.active=1,.kind=N2_RACE_CIRCUIT,.maxlaps=2,.ngate=4};
    for(int k=0;k<4;k++)connected.gate[k]=(WGate){.x=junction.poly[k][0],.y=junction.poly[k][1],.half=20};
    assert(world_race_bind_course(&connected,&path,20));
    cars[0]=(AiCar){.pos={20,0,0},.half_length=2,.half_width=1,.height=1.5f};drivers[0]=(AiRace){.progress=connected};
    assert(ai_drive_init(&drivers[0].drive,&path,cars[0].pos,0));
    for(int tick=0;tick<12000 && !drivers[0].progress.finished;tick++)ai_race_step(cars,drivers,0,1,1,&world,NULL);
    fprintf(stderr,"junction finish%d failed%d blocked%d pos%.2f %.2f segment%d error%.2f speed%.2f\n",drivers[0].progress.finished,drivers[0].drive.failed,drivers[0].blocked,cars[0].pos[0],cars[0].pos[1],drivers[0].drive.segment,drivers[0].drive.error,PHYS_KMH(cars[0].spd));
    assert(drivers[0].progress.finished && !drivers[0].drive.failed && cars[0].ride.contact_mask==15);
    free(circle.xy);free(path.xy);
    snprintf(dir,sizeof dir,"%s/ROUTESTEST/Routes1F.bin",root);assert(!unlink(dir));
    snprintf(dir,sizeof dir,"%s/ROUTESTEST/Paths1.bin",root);assert(!unlink(dir));
    narrow_course_test(root,0);
    narrow_course_test(root,1);
    source_ai_test(root);
    authored_layer_test(root);
    progress_outlier_course_test(root,0);
    progress_outlier_course_test(root,1);
    route_height_test(root);
    branch_terrain_test(root);
    gore_entrance_test(root);
    sidestep_course_test(root);
    connection_layer_test(root,0);connection_layer_test(root,1);
    grid_layer_loop_test(root);
    snprintf(dir,sizeof dir,"%s/ROUTESTEST/Paths1.bin",root);assert(!unlink(dir));
    snprintf(dir,sizeof dir,"%s/ROUTESTEST/Routes1F.bin",root);assert(!unlink(dir));
    snprintf(dir,sizeof dir,"%s/ROUTESTEST",root);assert(!rmdir(dir));assert(!rmdir(root));
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    rising_overlap_test();
    corner_wall_test();
    wall_turn_recovery_test();
    guardrail_bend_test();
    puts("race AI: grid, ordered sprint/circuit gates, yielding, finish, early divider avoidance, source-width parallel lanes, authored curved lanes, uphill road layers, grid-local stacked-deck circuit, mirrored guardrail bend without contact, terrain branch approach repair, connection endpoint layer continuity, rising floor overlaps, projected/limited finish run-out, linked segment/grid fallback, malformed lanes, field finish/run-out and wall checks PASS");
}
