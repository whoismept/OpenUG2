/* Synthetic only: drive through the same steering response and horizontal
 * player physics as gameplay. Live audit separately checks tyre/world contact. */
#include "ai.h"
#include "physics.h"
#include "world.h"
#include <assert.h>
#include "traffic_heatmap.h"
static TrafficHeatmap test_heat;
static void test_heat_contact(const float a[3],const float b[3],int kind) {
    heatmap_contact(&test_heat,a,b,kind);
}

static void traffic_pileup(int count,float angle,int order,int heavy,int backed,float gap) {
    float floor[]={-100,-100,0,0,0,100,-100,0,0,0,100,100,0,0,0,-100,100,0,0,0};
    float wall[]={2,-40,0,0,0,2,40,0,0,0,2,40,3,0,0,2,-40,3,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
        {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    float co=cosf(angle),sn=sinf(angle);
    for(int k=0;k<4;k++) {
        float x=wall[k*5],y=wall[k*5+1];
        wall[k*5]=co*x-sn*y;wall[k*5+1]=sn*x+co*y;
    }
    N2Scene scene={meshes,backed?2:1,2};
    WGroundGrid grid={0};float bounds[2][4]={{-100,-100,100,100},{-50,-50,50,50}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float obs[2][4],oz[2][2];int src[2];
    int n=phys_collect_walls(&scene,obs,src,oz,2);
    AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
    AiCar cars[N_OPENWORLD_AI+1]={0};AiCar *contacts[N_OPENWORLD_AI+1];
    float momentum[2]={0},energy=0;
    for(int k=0;k<count;k++) {
        float x=(k-count+1)*gap,v=k?0:.5f;
        cars[k]=(AiCar){.pos={x*co,x*sn,0},.head=angle,.spd=v,
            .vel={v*co-.03f*sn,v*sn+.03f*co},
            .half_length=2,.half_width=1,.height=1.5f,
            .mass=heavy && k==order%count?7:1.4f,
            .support={.valid={1,1,1,1},.ax={1,1,-1,-1},.ay={.8f,-.8f,.8f,-.8f}}};
        phys_ride_init(&cars[k].ride,&cars[k].support);
        cars[k].ride_ready=1;cars[k].ride.vz=-.3f;
        momentum[0]+=cars[k].mass*cars[k].vel[0];
        momentum[1]+=cars[k].mass*cars[k].vel[1];
        energy+=cars[k].mass*(v*v+.03f*.03f);
        contacts[k]=&cars[(order+(order&1?count-1-k:k))%count];
    }
    assert(ai_car_contacts(contacts,count,&world,&cars[0])>0);
    float after[2]={0},after_energy=0;
    for(int k=0;k<count;k++) {
        AiCar *c=&cars[k];
        for(int j=k+1;j<count;j++) {
            if(phys_ai_overlap(c,&cars[j])) {
                printf("pile-up overlap: n=%d angle=%.2f order=%d heavy=%d wall=%d pair=%d/%d\n",
                       count,angle,order,heavy,backed,k,j);fflush(stdout);
                assert(!phys_ai_overlap(c,&cars[j]));
            }
        }
        if(backed)assert(c->pos[0]*co+c->pos[1]*sn+2<=2.0001f);
        assert(fabsf(-c->vel[0]*sn+c->vel[1]*co-.03f)<1e-5f);
        assert(c->pos[2]==0 && c->ride.vz==-.3f && c->ride.impact==0);
        assert(c->ride.contact_mask==15);
        after[0]+=c->mass*c->vel[0];after[1]+=c->mass*c->vel[1];
        after_energy+=c->mass*(c->vel[0]*c->vel[0]+c->vel[1]*c->vel[1]);
    }
    if(!backed)assert(hypotf(after[0]-momentum[0],after[1]-momentum[1])<1e-5f);
    assert(after_energy<=energy+1e-5f);
    AiCar settled[N_OPENWORLD_AI+1];memcpy(settled,cars,sizeof cars);
    assert(ai_car_contacts(contacts,count,&world,&cars[0])==0);
    assert(!memcmp(settled,cars,sizeof cars)); /* no contact: no extra simulation */
    world_ground_grid_free(&grid);
}

static void traffic_impact_at_wall(float angle,float mass,int rail) {
    float floor[]={-20,-20,0,0,0,20,-20,0,0,0,20,20,0,0,0,-20,20,0,0,0};
    float wall[]={5.5f,-10,0,0,0,5.5f,10,0,0,0,5.5f,10,3,0,0,5.5f,-10,3,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
        {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    float co=cosf(angle),sn=sinf(angle);
    for(int k=0;k<4;k++) {
        float x=wall[k*5],y=wall[k*5+1];
        wall[k*5]=co*x-sn*y;wall[k*5+1]=sn*x+co*y;
        if(rail)wall[k*5+2]*=.5f;
    }
    if(rail){meshes[1].cat=N2_ROAD;meshes[1].scen=N2_SC_TERRAIN;}
    N2Scene scene={meshes,2,2};float obs[2][4],oz[2][2];int src[2];
    WGroundGrid grid={0};float bounds[2][4]={{-20,-20,20,20},{-20,-20,20,20}};
    assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    int n=phys_collect_walls(&scene,obs,src,oz,2);
    AiTrafficWorld world={.scene=&scene,.obst=obs,.obstz=oz,.obstsrc=src,.nobst=n};
    AiCar a={.pos={0,0,0},.vel={.5f*co-.03f*sn,.5f*sn+.03f*co},.spd=.5f,.head=angle,
             .half_length=2,.half_width=1,.height=1.5f,.mass=mass};
    AiCar b=a;b.pos[0]=3.5f*co;b.pos[1]=3.5f*sn;b.vel[0]=b.vel[1]=b.spd=0;b.mass=1.4f;
    for(int k=0;k<2;k++) {
        AiCar *c=k?&b:&a;
        c->support=(PhysRideSupport){.valid={1,1,1,1},
            .ax={1,1,-1,-1},.ay={.8f,-.8f,.8f,-.8f}};
        phys_ride_init(&c->ride,&c->support);c->ride_ready=1;
    }
    AiCar initial_a=a,initial_b=b;
    AiCar *contacts[]={&a,&b};
    float baseline_hit=ai_car_contacts(contacts,2,&world,&a);assert(baseline_hit>0);
    AiCar expected_a=a,expected_b=b;
    a=initial_a;b=initial_b;heatmap_clear(&test_heat);g_ai_contact_hook=test_heat_contact;
    float observed_hit=ai_car_contacts(contacts,2,&world,&a);g_ai_contact_hook=NULL;
    assert(observed_hit==baseline_hit && !memcmp(&a,&expected_a,sizeof a) &&
           !memcmp(&b,&expected_b,sizeof b));
    int world_hits=0,car_hits=0;
    for(int i=0;i<HEAT_CONTACTS;i++)if(test_heat.contacts[i].used) {
        if(test_heat.contacts[i].kind==HEAT_WORLD)world_hits++;else car_hits++;
    }
    assert(world_hits && car_hits); /* observer sees both production paths */
    float front=b.pos[0]*co+b.pos[1]*sn+2;
    printf("impact at %s: angle %.2f mass %.1f front %.5f (wall 5.5)\n",rail?"rail":"wall",angle,mass,front);
    fflush(stdout);
    assert(front<=5.5001f && !phys_ai_overlap(&a,&b));
    assert(fabsf(-a.vel[0]*sn+a.vel[1]*co-.03f)<1e-5f);
    assert(fabsf(a.vel[0]*co+a.vel[1]*sn)<.001f && fabsf(b.vel[0]*co+b.vel[1]*sn)<.001f);
    assert(a.ride.contact_mask==15 && b.ride.contact_mask==15);
    assert(a.pos[2]==0 && b.pos[2]==0 && a.ride.impact==0 && b.ride.impact==0);
    world_ground_grid_free(&grid);
}

static void traffic_junction(int bridge,int reverse) {
    /* An authored chain ends beside the middle of another edge.
       Neither of that edge's nodes is within the five-metre join radius. */
    float xy[]={-60,0,-30,0,0,0,-15,2.5f,15,2.5f,45,2.5f};
    if(reverse)for(int i=3;i<6;i++)xy[i*2]=30-xy[i*2];
    int next[]={1,2,-1,4,5,-1};AiRoadNet roads={.xy=xy,.next=next,.n=6,.half_width=NULL};
    float ground[]={-100,-10,0,0,0,5,-10,0,0,0,5,10,0,0,0,-100,10,0,0,0};
    float deck[]={-15,-10,8,0,0,100,-10,8,0,0,100,10,8,0,0,-15,10,8,0,0};
    uint16_t indices[]={0,1,2,0,2,3};
    N2Mesh meshes[2]={{.verts=ground,.nverts=4,.idx=indices,.nidx=6,.cat=N2_ROAD},
                      {.verts=deck,.nverts=4,.idx=indices,.nidx=6,.cat=N2_ROAD}};
    N2Scene scene={meshes,2,2};AiTrafficWorld world={.scene=&scene};
    AiCar car={.pos={-29.9f,-1.8f,0},.spd=.12f,.target_speed=.12f};
    AiTraffic route={.prev=-1,.from=0,.to=1,.after=2,.present=1};
    ai_traffic_step(&car,&route,&roads,bridge?&world:NULL);
    assert(route.from==1 && route.to==2);
    assert(route.after==(bridge?-1:4));
    if(!bridge) {
        for(int f=0;f<400;f++) {
            float x=car.pos[0],y=car.pos[1];
            ai_traffic_step(&car,&route,&roads,NULL);
            assert(hypotf(car.pos[0]-x,car.pos[1]-y)<.3f);
        }
        assert(car.pos[0]>10 && route.from>=2 && route.present);
    }
}

static void traffic_impact_at_edge(void) {
    float floor[]={-20,-20,0,0,0,4.3f,-20,0,0,0,4.3f,20,0,0,0,-20,20,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh mesh={.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};AiTrafficWorld world={.scene=&scene};
    AiCar a={.pos={1,0,0},.vel={.5f,0},.spd=.5f,
        .half_length=2,.half_width=1,.height=1.5f,.mass=1.4f};
    AiCar b=a;b.pos[0]=3;b.vel[0]=b.spd=0;
    for(int k=0;k<2;k++) {
        AiCar *c=k?&b:&a;
        c->support=(PhysRideSupport){.valid={1,1,1,1},
            .ax={1,1,-1,-1},.ay={.8f,-.8f,.8f,-.8f}};
        phys_ride_init(&c->ride,&c->support);c->ride_ready=1;c->ride.vz=-.3f;
    }
    AiCar *contacts[]={&a,&b};
    assert(ai_car_contacts(contacts,2,&world,&a)>0);
    assert(!phys_ai_overlap(&a,&b) && b.pos[0]>4);
    assert(fabsf(a.vel[0]+b.vel[0]-.5f)<1e-6f); /* no wall: momentum retained */
    assert(a.ride.contact_mask==15 && b.ride.contact_mask==12);
    assert(b.ride.compression[0]==-PHYS_RIDE_DROOP && b.ride.compression[1]==-PHYS_RIDE_DROOP);
    assert(a.pos[2]==0 && b.pos[2]==0 && a.ride.vz==-.3f && b.ride.vz==-.3f);
}

static void traffic_jump(int drop) {
    /* Flat approach, 15% ramp, then a three-metre drop to a landing road. */
    float verts[3][20];uint16_t idx[]={0,1,2,0,2,3};N2Mesh meshes[3]={0};
    const float bounds[3][4]={{-100,0,0,0},{0,20,0,3},{20,200,drop?0:3,drop?0:3}};
    for(int i=0;i<3;i++) {
        const float *b=bounds[i];
        float v[]={b[0],-15,b[2],0,0,b[1],-15,b[3],0,0,
                   b[1],15,b[3],0,0,b[0],15,b[2],0,0};
        memcpy(verts[i],v,sizeof v);
        meshes[i].verts=verts[i];meshes[i].nverts=4;meshes[i].idx=idx;
        meshes[i].nidx=6;meshes[i].cat=N2_ROAD;
    }
    N2Scene scene={meshes,3,3};AiTrafficWorld world={.scene=&scene};
    float xy[]={-40,0,0,0,20,0,80,0,140,0,200,0};int next[]={1,2,3,4,5,-1};
    AiRoadNet roads={.xy=xy,.next=next,.n=6,.half_width=NULL};
    AiCar car={.pos={-15,-1.8f,0},.spd=25.0f/PHYS_TICKRATE,
               .target_speed=25.0f/PHYS_TICKRATE,.half_length=2,.half_width=.9f,.height=1.5f};
    AiTraffic route={.prev=-1,.from=0,.to=1,.after=2,.present=1,.along=25};
    int air=0,landed=0;float peak=0,impact=0,shake=0,shake_v=0,shake_peak=0;
    for(int f=0;f<420;f++) {
        float oldz=car.pos[2];
        ai_traffic_step(&car,&route,&roads,&world);
        if(car.pos[0]>20 && !car.ride.contact_mask)air++;
        if(air && car.ride.impact>0){landed=1;impact=fmaxf(impact,car.ride.impact);}
        peak=fmaxf(peak,car.pos[2]);
        phys_landing_camera(&shake,&shake_v,car.ride.impact,1.0f/60.0f);
        shake_peak=fmaxf(shake_peak,fabsf(shake));
        assert(fabsf(car.pos[2]-oldz)<.6f && isfinite(car.pos[2]));
        if(car.pos[0]<-2)assert(car.pos[2]<.08f); /* braking cannot launch it */
    }
    printf("traffic jump drop=%d: air=%d peak=%.2f impact=%.2f shake=%.3f settled=%.3f\n",
           drop,air,peak,impact,shake_peak,car.pos[2]);fflush(stdout);
    assert(air>15 && landed && peak>3.1f && impact>(drop?2.0f:1.5f));
    assert(shake_peak>(drop?.01f:.003f));
    assert(fabsf(car.pos[2]-(drop?0:3))<.05f && car.ride.contact_mask==15);
    for(int f=0;f<180;f++)phys_landing_camera(&shake,&shake_v,0,1.0f/60.0f);
    assert(fabsf(shake)<.0001f);
    /* In flight, full steering/braking cannot redirect tyre-free momentum. */
    PhysRideState flying={0};float pos[3]={0,0,5},vel[2]={.3f,.1f},head=0,speed=.3f;
    phys_drive_step(pos,vel,&head,&speed,-1,1,1,NULL,NULL,&flying);
    assert(pos[0]==.3f && pos[1]==.1f && head==0 && vel[0]==.3f && vel[1]==.1f);
}

static void drive_curve(float rotation, int reverse) {
    float xy[24], raw[24]={-60,0,-30,0,0,0};
    for (int i=1;i<=6;i++) {
        float a=i*1.570796327f/6;
        raw[2*(i+2)]=40*sinf(a); raw[2*(i+2)+1]=40*(1-cosf(a));
    }
    raw[18]=40;raw[19]=70;raw[20]=40;raw[21]=100;raw[22]=40;raw[23]=130;
    for(int i=0;i<12;i++) {
        int j=i;
        xy[2*i]=raw[2*j]*cosf(rotation)-raw[2*j+1]*sinf(rotation);
        xy[2*i+1]=raw[2*j]*sinf(rotation)+raw[2*j+1]*cosf(rotation);
    }
    int start=reverse ? 11 : 0, next=reverse ? 10 : 1, end=reverse ? 0 : 11;
    N2Path p={xy,12}; float pos[3]={xy[2*start],xy[2*start+1],7},vel[2]={0};
    float h=atan2f(xy[2*next+1]-pos[1],xy[2*next]-pos[0]),speed=0,steer=0,maxerr=0;
    PhysVehicle heavy={.accel=.85f,.brake=.85f,.steer=.8f,.lat=1.02f,
                       .pitch_load=1,.roll_load=1}; AiDrive d;
    assert(ai_drive_init(&d,&p,pos,h));
    assert(d.direction==(reverse ? -1 : 1));
    for(int f=0;f<9000 && !d.finished && !d.failed;f++) {
        float old[3];memcpy(old,pos,sizeof old);
        AiDriveInput in=ai_drive_step(&d,pos,h,speed);
        assert(!memcmp(old,pos,sizeof old)); /* controller cannot move the car */
        assert(isfinite(in.steer) && fabsf(in.steer)<=1 && fabsf(in.throttle)<=1);
        steer=phys_steer_response(steer,in.steer);
        phys_car_step(pos,vel,&h,&speed,in.throttle,steer,in.handbrake,NULL,&heavy);
        assert(pos[2]==7 && speed>=-0.001f && PHYS_KMH(speed)<51);
        if(d.error>maxerr)maxerr=d.error;
    }
    printf("curve rotation %.2f reverse %d: end=%d fail=%d error=%.3f max=%.3f progress=%.2f/%.2f\n",
           rotation,reverse,d.finished,d.failed,d.error,maxerr,d.progress,d.length);
    assert(d.finished && !d.failed && maxerr<6);
    assert(hypotf(pos[0]-xy[2*end],pos[1]-xy[2*end+1])<3);
}

static void traffic_density_test(void) {
    float xy[N_OPENWORLD_AI*5*2],width[N_OPENWORLD_AI*5];int next[N_OPENWORLD_AI*5];
    AiRoadNet roads={.xy=xy,.next=next,.n=N_OPENWORLD_AI*5,.half_width=width};
    const float loop[5][2]={{0,0},{30,0},{30,30},{0,30},{0,0}};
    for(int k=0;k<N_OPENWORLD_AI;k++)for(int n=0;n<5;n++) {
        int id=k*5+n;xy[id*2]=-100-75*(k%3)+loop[n][0];
        xy[id*2+1]=-175+70*(k/3)+loop[n][1];next[id]=n<4?id+1:-1;width[id]=5;
    }
    AiTrafficWorld world={.view={1,0},.traffic_target=N_AI};
    AiCar cars[N_OPENWORLD_AI]={0};AiTraffic routes[N_OPENWORLD_AI]={0};
    float player[3]={0};
    assert(ai_traffic_spawn(&roads,&world,cars,routes,player,0)==N_ROAM_VISUALS);
    for(int k=0;k<N_OPENWORLD_AI;k++)assert(routes[k].present==(k<N_ROAM_VISUALS));
    world.traffic_target=999; /* engine clamps even direct, non-UI callers */
    for(int round=0;round<40;round++)ai_traffic_update(&roads,&world,cars,routes,player,0);
    for(int k=0;k<N_OPENWORLD_AI;k++) {
        assert(routes[k].present);
        assert(ai_traffic_visual(k)<N_ROAM_VISUALS);
        float kmh=PHYS_KMH(routes[k].cruise_speed);
        assert(ai_traffic_is_racer(k)?kmh>=75:kmh>=26 && kmh<=35.01f);
        for(int j=0;j<k;j++)assert(hypotf(cars[k].pos[0]-cars[j].pos[0],cars[k].pos[1]-cars[j].pos[1])>=22);
    }
    /* Decreasing density must leave a visible car and its pose untouched. */
    cars[0].pos[0]=100;cars[0].pos[1]=0;AiCar visible=cars[0];
    world.traffic_target=-20;
    ai_traffic_update(&roads,&world,cars,routes,player,0);
    for(int k=0;k<N_OPENWORLD_AI;k++)assert(routes[k].present==(k==0 || ai_traffic_is_racer(k)));
    assert(!memcmp(&visible,&cars[0],sizeof visible));
    /* Once the player looks away, it can retire. Racer identities remain. */
    world.view[0]=-1;ai_traffic_update(&roads,&world,cars,routes,player,0);
    assert(!routes[0].present && routes[0].to<0 && cars[0].spd==0);
    for(int k=N_AI;k<N_ROAM_VISUALS;k++)assert(routes[k].present);
    world.view[0]=1;world.traffic_target=3;
    for(int round=0;round<8;round++)ai_traffic_update(&roads,&world,cars,routes,player,0);
    for(int k=0;k<N_OPENWORLD_AI;k++)assert(routes[k].present==(k<3 || ai_traffic_is_racer(k)));
    world.ambient_only=1;world.traffic_target=16;
    assert(ai_traffic_spawn(&roads,&world,cars,routes,player,0)==N_OPENWORLD_AI);
    for(int k=0;k<N_OPENWORLD_AI;k++)assert(routes[k].present==!ai_traffic_is_racer(k));
    /* Front-side streets can spawn outside the view; hiding a car preserves it. */
    float front_xy[]={20,180,20,210,20,240};int front_next[]={1,2,-1};
    AiRoadNet front={.xy=front_xy,.next=front_next,.n=3};
    memset(routes,0,sizeof routes);
    assert(ai_traffic_respawn(&front,&world,cars,routes,0,player,0));
    assert(cars[0].pos[0]>0 && ai_traffic_offscreen(world.eye,world.view,cars[0].pos));
    AiCar hidden=cars[0];cars[0].render_visible=0;
    ai_traffic_update(&front,&world,cars,routes,player,0);
    assert(routes[0].present && !memcmp(hidden.pos,cars[0].pos,sizeof hidden.pos));
    world.ambient_only=0;
    /* An unavailable slot cannot prevent retries for the rest of the pool. */
    player[0]=10000;ai_traffic_spawn(&roads,&world,cars,routes,player,0);
    unsigned attempts=routes[0].respawns;
    for(int round=0;round<10;round++)ai_traffic_update(&roads,&world,cars,routes,player,0);
    assert(routes[0].respawns>attempts && routes[N_AI].respawns>1);
    puts("traffic density: default/increase/zero/decrease/restore, safe visibility, racer roles and retry fairness PASS");
}

static void traffic_authored_terrain(void) {
    float verts[3][20];uint16_t idx[]={0,1,2,0,2,3};
    const float ends[]={-150,-40,40,150};N2Mesh meshes[3]={0};
    for(int m=0;m<3;m++) {
        float quad[]={ends[m],-6,0,0,0,ends[m+1],-6,0,0,0,
                      ends[m+1],6,0,0,0,ends[m],6,0,0,0};
        memcpy(verts[m],quad,sizeof quad);
        meshes[m]=(N2Mesh){.verts=verts[m],.nverts=4,.idx=idx,.nidx=6,
                            .cat=m==1?N2_TERRAIN:N2_ROAD};
    }
    N2Scene scene={meshes,3,3};AiTrafficWorld world={.scene=&scene};
    float xy[]={-90,0,-60,0,-30,0,0,0,30,0,60,0,90,0,-30,0,0,0};
    int next[]={1,2,3,4,5,6,-1,8,-1};
    AiRoadNet roads={.xy=xy,.next=next,.n=9};
    for(int indexed=0;indexed<2;indexed++) {
        if(indexed)assert(ai_roads_index(&roads));
        assert(ai_road_next(&roads,&world,NULL,1,0,0)==2); /* road -> terrain */
        assert(ai_road_next(&roads,&world,NULL,2,1,0)==3); /* terrain -> terrain */
        assert(ai_road_next(&roads,&world,NULL,4,3,0)==5); /* terrain -> road */
        assert(ai_road_next(&roads,&world,NULL,4,5,0)==3); /* reverse traversal */
    }
    free(roads.pred_start);free(roads.pred_list);free(roads.cell_start);free(roads.cell_list);
    roads=(AiRoadNet){.xy=xy,.next=next,.n=9};
    next[2]=-1;
    assert(ai_road_next(&roads,&world,NULL,2,1,0)==-1); /* no terrain junction hop */
    next[2]=3;next[1]=-1;xy[14]=-60;
    assert(ai_road_next(&roads,&world,NULL,1,0,0)==-1); /* road hop cannot cross terrain */
    next[1]=2;xy[14]=-30;
    meshes[1].cat=N2_OTHER;
    assert(ai_road_next(&roads,&world,NULL,1,0,0)==-1); /* unsupported gap */
    meshes[1].cat=N2_TERRAIN;
    for(int v=0;v<4;v++)verts[1][v*5+2]=6;
    assert(ai_road_next(&roads,&world,NULL,1,0,0)==-1); /* different height layer */
    for(int v=0;v<4;v++)verts[1][v*5+2]=0;
    AiCar car={.pos={-60,-1.8f,0},.spd=.12f,.target_speed=.12f,
        .half_length=2,.half_width=.9f,.height=1.5f,
        .support={.ax={1,1,-1,-1},.ay={.8f,-.8f,.8f,-.8f}}};
    AiTraffic route={.prev=-1,.from=0,.to=1,.after=2,.present=1};
    for(int tick=0;tick<1200;tick++) {
        ai_traffic_step(&car,&route,&roads,&world);
        assert(route.to>=0 && route.stop_reason==0);
        assert(fabsf(car.pos[1]+1.8f)<.05f && fabsf(route.lane_offset-1.8f)<.001f);
    }
    assert(car.pos[0]>45); /* drove through the whole terrain strip in-lane */
    /* A narrow ROAD strip with a TERRAIN shoulder must retain its old
       centre fallback, rather than treating the shoulder as a traffic lane. */
    scene.count=2;
    for(int v=0;v<4;v++) {
        verts[0][v*5]=verts[1][v*5]=(v==1 || v==2)?150:-150;
        verts[0][v*5+1]=v<2?-.2f:.2f;
        verts[1][v*5+1]=v<2?-6:6;
    }
    car=(AiCar){.pos={-60,0,0},.spd=.12f,.target_speed=.12f};
    route=(AiTraffic){.prev=-1,.from=0,.to=1,.after=2,.present=1};
    ai_traffic_step(&car,&route,&roads,&world);
    assert(route.lane_offset==0);
    AiCar cars[N_OPENWORLD_AI]={0};AiTraffic routes[N_OPENWORLD_AI]={0};
    meshes[0].cat=meshes[2].cat=N2_TERRAIN;
    assert(!ai_traffic_respawn(&roads,&world,cars,routes,0,(float[3]){200,0,0},0));
    puts("authored terrain: continuation, lanes, gaps, layers and strict spawn/junction gates PASS");
}

static void traffic_closed_road(void) {
    float floor[]={-100,-100,0,0,0,100,-100,0,0,0,100,100,0,0,0,-100,100,0,0,0};
    float wall[]={12,-5,0,0,0,12,5,0,0,0,12,5,3,0,0,12,-5,3,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh m[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
                {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL}};
    N2Scene scene={m,2,2};float ob[2][4],oz[2][2];int src[2];
    AiTrafficWorld w={.scene=&scene,.obst=ob,.obstz=oz,.obstsrc=src};
    w.nobst=phys_collect_walls(&scene,ob,src,oz,2);
    float xy[]={-10,0,0,0,20,0,0,0,0,20};int next[]={1,2,-1,4,-1};
    AiRoadNet roads={.xy=xy,.next=next,.n=5};
    AiCar small={.half_length=2,.half_width=.9f,.height=1.5f},bus=small;bus.half_length=8;
    assert(ai_road_next(&roads,&w,&small,1,0,0)==4); /* turn before closure */
    w.nobst=0;assert(ai_road_next(&roads,&w,&small,1,0,0)==2);
    /* A closure beyond the centreline endpoint still catches a large body. */
    for(int i=0;i<4;i++)wall[i*5]=22.5f;
    w.nobst=phys_collect_walls(&scene,ob,src,oz,2);
    assert(ai_road_next(&roads,&w,&small,1,0,0)==2);
    assert(ai_road_next(&roads,&w,&bus,1,0,0)==4);
    for(int i=0;i<4;i++)wall[i*5+2]+=10;
    w.nobst=phys_collect_walls(&scene,ob,src,oz,2);
    assert(ai_road_next(&roads,&w,&bus,1,0,0)==2); /* upper layer is not this road */
    /* A short blocked spur must be rejected at the upstream junction too. */
    for(int i=0;i<4;i++){wall[i*5]=12;wall[i*5+2]-=10;}
    w.nobst=phys_collect_walls(&scene,ob,src,oz,2);
    float spur[]={-10,0,0,0,6,0,9,0,14,0,0,0,0,20};
    int links[]={1,2,3,4,-1,6,-1};roads.xy=spur;roads.next=links;roads.n=7;
    assert(ai_road_next(&roads,&w,&small,1,0,0)==6);
    /* Real road graphs use indexed candidate lists, including recursive calls. */
    roads.xy=malloc(sizeof spur);memcpy(roads.xy,spur,sizeof spur);
    roads.next=malloc(sizeof links);memcpy(roads.next,links,sizeof links);
    assert(ai_roads_index(&roads));
    assert(ai_road_next(&roads,&w,&small,1,0,0)==6);
    ai_roads_free(&roads);
    puts("traffic closed road: direct/preview/size/layer checks PASS");
}

static void traffic_preview_cache(void) {
    float v[]={-100,-100,0,0,0,100,-100,0,0,0,100,100,0,0,0,-100,100,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh mesh={.verts=v,.idx=idx,.nverts=4,.nidx=6,.cat=N2_ROAD},next_mesh=mesh;
    N2Scene scene={&mesh,1,1};AiTrafficWorld w={.scene=&scene};
    float xy[20];int links[10];
    for(int i=0;i<10;i++){xy[2*i]=i*4;xy[2*i+1]=0;links[i]=i<9?i+1:-1;}
    AiRoadNet roads={.xy=xy,.next=links,.n=10};
    AiCar cached={.pos={0,-1.8f,0},.spd=.08f,.target_speed=.08f,
                 .half_length=2,.half_width=.9f,.height=1.5f},direct=cached;
    AiTraffic a={.prev=-1,.from=0,.to=1,.after=2,.present=1},b=a;
    int populated=0;
    for(int f=0;f<240;f++) {
        if(f==60)scene.meshes=&next_mesh; /* new immutable resident */
        if(f==120){cached.half_length=direct.half_length=3;cached.half_width=direct.half_width=1.2f;}
        b.preview_valid=0;
        ai_traffic_step(&cached,&a,&roads,&w);ai_traffic_step(&direct,&b,&roads,&w);
        populated|=a.preview_valid!=0;
        assert(!memcmp(&cached,&direct,sizeof cached));
        assert(a.from==b.from && a.to==b.to && a.after==b.after && a.travelled==b.travelled);
    }
    assert(populated);
    puts("traffic preview cache: exact driving, resident/body invalidation PASS");
}

int main(void) {
    traffic_preview_cache();
    traffic_closed_road();
    traffic_authored_terrain();
    traffic_density_test();
    phys_selftest();
    for(int count=3;count<=N_OPENWORLD_AI+1;count+=4)
        for(int a=0;a<3;a++)for(int order=0;order<count;order++)
            for(int heavy=0;heavy<2;heavy++)for(int backed=0;backed<2;backed++)
                for(int deep=0;deep<2;deep++)
                    traffic_pileup(count,a*.71f,order,heavy,backed,deep?3.2f:3.7f);
    puts("pile-ups: rotated/order/mass/wall/depth cases up to full pool, no residual overlaps");
    {
        AiCar player={.pos={100,100,0}};
        AiCar a={.vel={.2f,0},.spd=.2f,.half_length=2,.half_width=1,.height=1.5f};
        AiCar b=a;b.pos[0]=2.8f;b.head=1.570796327f;b.vel[0]=b.spd=0;
        AiCar *cars[]={&a,&player,&b};AiCar untouched=player;
        /* Crosswise AI contact must not trigger the distant player's hit sound. */
        assert(phys_ai_overlap(&a,&b));
        assert(ai_car_contacts(cars,3,NULL,&player)==0);
        assert(!phys_ai_overlap(&a,&b) && !memcmp(&untouched,&player,sizeof player));
        assert(ai_car_contacts(cars,0,NULL,NULL)==0);
        assert(ai_car_contacts(cars,1,NULL,NULL)==0);
    }
    for(int angle=0;angle<4;angle++)for(int heavy=0;heavy<2;heavy++)for(int rail=0;rail<2;rail++)
        traffic_impact_at_wall(angle*.71f,heavy?7.0f:1.4f,rail);
    traffic_impact_at_edge();
    {
        /* A nearby straighter chain must not steal an authored bend. */
        float xy[]={-30,0,0,0,30,0,55,15,20,0,50,0,80,0};
        int next[]={1,2,3,-1,5,6,-1};AiRoadNet road={.xy=xy,.next=next,.n=7,.half_width=NULL};
        AiCar car={.pos={.1f,-1.8f,0},.spd=.12f,.target_speed=.12f};
        AiTraffic route={.prev=-1,.from=0,.to=1,.after=2,.present=1};
        ai_traffic_step(&car,&route,&road,NULL);
        assert(route.from==1 && route.to==2 && route.after==3);
    }
    traffic_junction(0,0);traffic_junction(1,0);traffic_junction(0,1);
    traffic_jump(1);traffic_jump(0);
    {
        const float body[6]={-2.0f,-0.9f,0,2.0f,0.9f,1.5f};
        AiCar other={.pos={0,4,0},.head=0,.half_length=2,
                     .half_width=.9f,.height=1.5f};
        float player[3]={0,0,0},vel[2]={0.2f,0};
        assert(phys_car_contacts(player,vel,.2f,0,body,1.4f,&other,1)==0);
        assert(player[0]==0 && player[1]==0); /* adjacent lane, no contact */
        other.pos[0]=3.5f;other.pos[1]=0;
        assert(phys_car_contacts(player,vel,.2f,0,body,1.4f,&other,1)>0);
        assert(player[0]<-.24f && other.pos[0]>3.74f);
        assert(other.vel[0]>.10f && vel[0]>0 && vel[0]<.10f);
        assert(fabsf(vel[0]+other.vel[0]-.2f)<1e-6f); /* equal-mass momentum */
        player[0]=0;other.pos[2]=8;
        assert(phys_car_contacts(player,vel,.2f,0,body,1.4f,&other,1)==0);
        assert(player[0]==0); /* a car on the deck above is not a collision */
        AiCar first={.pos={0,0,0},.head=0,.half_length=2,
                     .half_width=.9f,.height=1.5f};
        other.pos[0]=3.5f;other.pos[2]=0;
        assert(phys_ai_overlap(&first,&other));
        other.pos[1]=4;
        assert(!phys_ai_overlap(&first,&other));
        other.pos[1]=0;other.pos[2]=8;
        assert(!phys_ai_overlap(&first,&other));
    }
    {
        const float bb[]={-2,-1,0,2,1,1.5f};
        for(int heavy=0;heavy<2;heavy++) {
            float p[]={0,0,0},v[]={.3f,0};
            AiCar traffic={.pos={3.8f,0,0},.half_length=2,.half_width=1,.height=1.5f,
                           .mass=heavy?7.0f:1.4f};
            assert(phys_car_contacts(p,v,.3f,0,bb,1.4f,&traffic,1)>0);
            assert(traffic.pos[0]>3.8f && traffic.vel[0]>0);
            assert(fabsf(1.4f*v[0]+traffic.mass*traffic.vel[0]-.42f)<1e-6f);
            if(heavy)assert(traffic.vel[0]<.06f);
            else assert(traffic.vel[0]>.16f);
            /* The driver's next step must retain the impact, not snap to its
               route or overwrite the new momentum with a cruise velocity. */
            PhysRideSupport support={.valid={1,1,1,1},.ax={1,1,-1,-1},.ay={1,-1,1,-1}};
            traffic.support=support;phys_ride_init(&traffic.ride,&support);traffic.ride_ready=1;
            float xy[]={0,1.8f,100,1.8f};int next[]={1,-1};AiRoadNet road={.xy=xy,.next=next,.n=2,.half_width=NULL};
            AiTraffic route={.prev=-1,.from=0,.to=1,.after=-1,.present=1};
            float before=traffic.pos[0];
            ai_traffic_step(&traffic,&route,&road,NULL);
            assert(traffic.pos[0]>before && traffic.pos[0]-before<.2f);
        }
    }
    {
        float nodes[N_ROAM_VISUALS*5*2],width[N_ROAM_VISUALS*5];
        int next[N_ROAM_VISUALS*5];
        AiRoadNet road={.xy=nodes,.next=next,.n=N_ROAM_VISUALS*5,.half_width=width};
        AiCar cars[N_OPENWORLD_AI]={0}; AiTraffic traffic[N_OPENWORLD_AI]={0};
        float player[3]={0,0,5};
        const float loop[5][2]={{0,0},{30,0},{30,30},{0,30},{0,0}};
        for(int k=0;k<N_ROAM_VISUALS;k++) {
            float a=2.0f+k*0.48f;
            for(int n=0;n<5;n++) {
                int id=k*5+n;
                nodes[id*2]=cosf(a)*110+loop[n][0];
                nodes[id*2+1]=sinf(a)*110+loop[n][1];
                next[id]=n<4?id+1:-1;
                width[id]=5.0f;
            }
        }
        float away[3]={1000,1000,5};
        cars[0].spd=.2f;
        assert(ai_traffic_spawn(&road,NULL,cars,traffic,away,0)==0);
        assert(traffic[0].to<0 && traffic[0].stop_reason==7 && cars[0].spd==0);
        assert(ai_traffic_spawn(&road,NULL,cars,traffic,player,0)==N_ROAM_VISUALS);
        for(int k=0;k<N_ROAM_VISUALS;k++) {
            float x=cars[k].pos[0],y=cars[k].pos[1];
            assert(hypotf(x,y)>=75 && x<=0.55f*hypotf(x,y));
            int from=traffic[k].from,to=traffic[k].to;
            float dx=nodes[to*2]-nodes[from*2],dy=nodes[to*2+1]-nodes[from*2+1];
            assert(fabsf(traffic[k].lane_offset-2.0f)<1e-4f);
            assert(dx*(y-nodes[from*2+1])-dy*(x-nodes[from*2])<0);
            assert(PHYS_KMH(traffic[k].cruise_speed)>=26 &&
                   PHYS_KMH(traffic[k].cruise_speed)<=80.01f);
            assert(cars[k].spd<=traffic[k].cruise_speed);
            if(k>=N_AI)assert(PHYS_KMH(traffic[k].cruise_speed)>=75);
            float furthest=0;
            for(int f=0;f<600;f++) {
                ai_traffic_step(&cars[k],&traffic[k],&road,NULL);
                float d=hypotf(cars[k].pos[0]-x,cars[k].pos[1]-y);
                if(d>furthest)furthest=d;
            }
            assert(furthest>35 && furthest<50);
            assert(traffic[k].to>=0 && isfinite(cars[k].head));
            assert(traffic[k].travelled>35.0f);
        }
        /* The right-lane offset must not snap sideways at a 90-degree bend. */
        float bend_xy[]={0,0,30,0,30,30,60,30},bend_width[]={5,5,5,5};
        int bend_next[]={1,2,3,-1};
        AiRoadNet bend={.xy=bend_xy,.next=bend_next,.n=4,.half_width=bend_width};
        AiCar car={.pos={0,-2,0},.spd=.2f,.target_speed=.2f};
        AiTraffic route={.prev=-1,.from=0,.to=1,.after=2,.lane_offset=2};
        float oldhead=car.head,maxturn=0;
        for(int f=0;f<180;f++) {
            float ox=car.pos[0],oy=car.pos[1];
            ai_traffic_step(&car,&route,&bend,NULL);
            assert(hypotf(car.pos[0]-ox,car.pos[1]-oy)<0.5f);
            float turn=fabsf(atan2f(sinf(car.head-oldhead),cosf(car.head-oldhead)));
            if(turn>maxturn)maxturn=turn;
            assert(fabsf(fabsf(car.turn_rate)-turn)<1e-4f);
            oldhead=car.head;
        }
        assert(maxturn>0.001f && maxturn<0.15f); /* rounded heading through node */
        car.pos[1]+=2.0f;
        float displaced_y=car.pos[1];
        ai_traffic_step(&car,&route,&bend,NULL);
        assert(fabsf(car.pos[1]-displaced_y)<.5f); /* steer back, never snap to route */
        AiCar turning[N_OPENWORLD_AI]={0};AiTraffic turn_routes[N_OPENWORLD_AI]={0};
        turning[0]=(AiCar){.pos={0,-2,0},.head=0,.spd=15.8f/PHYS_TICKRATE};
        turn_routes[0]=(AiTraffic){.prev=-1,.from=0,.to=1,.after=2,.present=1,
                                   .cruise_speed=20.0f/PHYS_TICKRATE};
        for(int f=0;f<300 && turn_routes[0].along<25.5f;f++) {
            ai_traffic_follow(turning,turn_routes,&bend,0,1,NULL);
            ai_traffic_step(&turning[0],&turn_routes[0],&bend,NULL);
        }
        assert(turn_routes[0].from==0 && turning[0].spd*PHYS_TICKRATE<9.0f);
        float dead_xy[]={0,0,30,0};int dead_next[]={1,-1};
        AiRoadNet dead={.xy=dead_xy,.next=dead_next,.n=2,.half_width=NULL};
        car=(AiCar){.pos={29.9f,0,0},.head=0,.spd=.2f};
        route=(AiTraffic){.prev=-1,.from=0,.to=1,.after=-1,.along=29.9f};
        ai_traffic_step(&car,&route,&dead,NULL);
        assert(fabsf(car.head)<.03f); /* endpoint cannot force a U-turn */
        AiCar ending[N_OPENWORLD_AI]={0};AiTraffic ends[N_OPENWORLD_AI]={0};
        ending[0]=(AiCar){.pos={0,-2,0},.spd=12.0f/PHYS_TICKRATE};
        ends[0]=(AiTraffic){.prev=-1,.from=0,.to=1,.after=-1,.present=1,
                            .cruise_speed=ending[0].spd};
        for(int f=0;f<900 && ends[0].to>=0;f++) {
            ai_traffic_follow(ending,ends,&dead,0,1,NULL);
            ai_traffic_step(&ending[0],&ends[0],&dead,NULL);
            if(ending[0].pos[0]>27.0f)
                assert(ending[0].spd*PHYS_TICKRATE<5.0f);
        }
        assert(ends[0].to<0 && ending[0].pos[0]>27.0f && ending[0].pos[0]<30.0f);
        float reverse_xy[]={-90,0,-60,0,-60,0,-90,1};
        int reverse_next[]={1,-1,3,-1};
        AiRoadNet reverse={.xy=reverse_xy,.next=reverse_next,.n=4,.half_width=NULL};
        AiCar reverse_cars[N_OPENWORLD_AI]={0};
        AiTraffic reverse_routes[N_OPENWORLD_AI]={0};
        float reverse_player[3]={0,0,0};
        for(int k=0;k<N_OPENWORLD_AI;k++)reverse_routes[k].to=-1;
        assert(ai_traffic_respawn(&reverse,NULL,reverse_cars,reverse_routes,3,
                                  reverse_player,0));
        assert(reverse_routes[3].from==0 && reverse_routes[3].after<0);
        assert(ai_traffic_offscreen(player,(float[2]){1,0},(float[3]){-100,0,0}));
        assert(ai_traffic_offscreen(player,(float[2]){1,0},(float[3]){0,100,0}));
        assert(!ai_traffic_offscreen(player,(float[2]){1,0},(float[3]){100,100,0}));
        assert(!ai_traffic_offscreen(player,(float[2]){-1,0},(float[3]){-100,0,0}));
        assert(!ai_traffic_offscreen(player,(float[2]){1,0},(float[3]){-20,0,0}));
    }
    {
        float xy[]={-100,10,-70,10,-40,10,
                    -120,50,-120,80,-120,110};
        int next[]={1,2,-1,4,5,-1};
        AiRoadNet roads={.xy=xy,.next=next,.n=6,.half_width=NULL};
        AiCar cars[N_OPENWORLD_AI]={0};AiTraffic routes[N_OPENWORLD_AI]={0};
        float player[3]={0,0,0};
        for(int k=0;k<N_OPENWORLD_AI;k++)routes[k].to=-1;
        assert(ai_traffic_respawn(&roads,NULL,cars,routes,0,player,0));
        assert(cars[0].head>1.4f && cars[0].head<1.8f); /* side street first */
    }
    {
        float xy[]={-120,0,-90,0,-60,0};int next[]={1,2,-1};
        AiRoadNet road={.xy=xy,.next=next,.n=3,.half_width=NULL};
        AiCar cars[N_OPENWORLD_AI]={0};AiTraffic routes[N_OPENWORLD_AI]={0};
        AiTrafficWorld view={0};float player[3]={0,0,0};
        for(int k=0;k<N_OPENWORLD_AI;k++)routes[k].to=-1;
        view.view[0]=-1;
        assert(!ai_traffic_respawn(&road,&view,cars,routes,0,player,0));
        assert(!routes[0].present);
        view.view[0]=1;
        assert(ai_traffic_respawn(&road,&view,cars,routes,0,player,0));
        assert(routes[0].present && ai_traffic_offscreen(view.eye,view.view,cars[0].pos));
    }
    {
        float xy[]={0,0,500,0};int next[]={1,-1};
        AiRoadNet road={.xy=xy,.next=next,.n=2,.half_width=NULL};
        AiCar cars[N_OPENWORLD_AI]={0};AiTraffic routes[N_OPENWORLD_AI]={0};
        cars[0]=(AiCar){.pos={0,0,0},.head=0,.spd=20.0f/PHYS_TICKRATE,
                        .half_length=2,.half_width=1,.height=1.5f};
        cars[1]=(AiCar){.pos={60,0,0},.head=0,.spd=8.0f/PHYS_TICKRATE,
                        .target_speed=8.0f/PHYS_TICKRATE,
                        .half_length=2,.half_width=1,.height=1.5f};
        routes[0]=(AiTraffic){.prev=-1,.from=0,.to=1,.after=-1,.present=1,
                              .cruise_speed=cars[0].spd};
        routes[1]=(AiTraffic){.prev=-1,.from=0,.to=1,.after=-1,.present=1,
                              .along=60,.cruise_speed=cars[1].spd};
        int slowed=0;
        for(int f=0;f<600;f++) {
            ai_traffic_follow(cars,routes,&road,0,2,NULL);
            if(cars[0].spd<routes[0].cruise_speed-0.01f/PHYS_TICKRATE)slowed=1;
            ai_traffic_step(&cars[0],&routes[0],&road,NULL);
            ai_traffic_step(&cars[1],&routes[1],&road,NULL);
            assert(cars[1].pos[0]-cars[0].pos[0]>10.0f);
            assert(!phys_ai_overlap(&cars[0],&cars[1]));
        }
        assert(slowed && cars[0].spd<routes[0].cruise_speed);
        cars[0].pos[0]=0;cars[0].spd=20.0f/PHYS_TICKRATE;
        cars[1].pos[0]=30;cars[1].spd=0;routes[1].to=-1;
        ai_traffic_follow(cars,routes,&road,0,2,NULL);
        assert(cars[0].target_speed<cars[0].spd); /* stopped cars hold traffic */
        /* The player blocks traffic even when parked across its lane. */
        AiCar player={.pos={18,0,0},.head=1.5707963f,
                      .half_length=2,.half_width=1,.height=1.5f};
        ai_traffic_follow(cars,routes,&road,0,1,&player);
        assert(cars[0].target_speed==0);
        player.pos[1]=10;
        ai_traffic_follow(cars,routes,&road,0,1,&player);
        assert(cars[0].target_speed==routes[0].cruise_speed);
    }
    float xy[]={0,0,30,0,60,0},pos[]={2,0,0};N2Path p={xy,3};AiDrive d;
    assert(ai_drive_route_valid(&p));
    assert(ai_drive_init(&d,&p,pos,3.14159265f) && d.direction==-1);
    assert(!ai_drive_init(&d,&p,pos,1.570796327f));
    pos[1]=20;assert(!ai_drive_init(&d,&p,pos,0));pos[1]=0;
    xy[2]=NAN;assert(!ai_drive_route_valid(&p));xy[2]=0;
    assert(!ai_drive_route_valid(&p));xy[2]=30;xy[4]=151;
    assert(!ai_drive_route_valid(&p));xy[4]=0;
    assert(!ai_drive_route_valid(&p));xy[4]=60;
    assert(ai_drive_init(&d,&p,pos,0));
    for(int i=0;i<301;i++) ai_drive_step(&d,pos,0,0);
    assert(d.failed && !d.finished); /* stationary pin does not earn coverage */
    AiDriveInput in=ai_drive_step(&d,pos,0,0);assert(in.throttle==0 && in.handbrake);
    assert(ai_drive_init(&d,&p,pos,0));
    for(int i=0;i<301;i++) { pos[0]+=0.01f; ai_drive_step(&d,pos,0,0.01f); }
    assert(d.failed && !d.finished); /* sub-walking-speed scraping is still a pin */
    pos[0]=2;
    assert(ai_drive_init(&d,&p,pos,0));pos[0]=58;pos[1]=20;
    ai_drive_step(&d,pos,0,0);assert(d.failed && !d.finished);
    pos[0]=2;pos[1]=0;assert(ai_drive_init(&d,&p,pos,0));
    ai_drive_step(&d,pos,NAN,0);assert(d.failed);
    assert(ai_drive_init(&d,&p,pos,0));ai_drive_step(&d,pos,3.14159265f,0);
    assert(d.target_kmh<=14.01f); /* facing away must not restore straight-line pace */
    {
        float loopxy[]={0,0,20,0,20,20,0,20,-20,20,-20,0};
        N2Path loop={loopxy,6};N2Scene empty={0};AiCar rival={0};
        rival.pos[0]=0;rival.pos[1]=0;rival.t=1;rival.spd=PHYS_MAXSPD;
        ai_step(&rival,0,&loop,&empty,0,0);
        assert(rival.braking); /* corner deceleration drives the rear lamps */
    }
    /* A lateral race target uses the tangent at the lookahead point, even
       when it has crossed a corner. Exercise both travel directions. */
    {
        float cornerxy[]={0,0,6,0,6,20};N2Path corner={cornerxy,3};AiDrive ahead;
        float start[]={4,0,0};assert(ai_drive_init(&ahead,&corner,start,0));
        ai_drive_step(&ahead,start,0,.1f);
        assert(ahead.target[0]==6 && ahead.target[1]>0);
        assert(fabsf(cosf(ahead.target_heading))<.00001f && sinf(ahead.target_heading)>.99999f);
        start[0]=6;start[1]=2;assert(ai_drive_init(&ahead,&corner,start,-1.570796327f));
        ai_drive_step(&ahead,start,-1.570796327f,.1f);
        assert(ahead.direction==-1 && ahead.target[0]<6 && ahead.target[1]==0);
        assert(cosf(ahead.target_heading)<-.99999f && fabsf(sinf(ahead.target_heading))<.00001f);
    }
    for(int i=0;i<4;i++)for(int r=0;r<2;r++)drive_curve(i*1.570796327f,r);
    {   /* The road query index must return exactly what the linear scan does,
           including junction hops between separate chains and ties. */
        enum { RN = 2400 };
        static float rxy[RN*2]; static int rnext[RN];
        unsigned seed = 12345u;
        for (int i = 0; i < RN; i++) {
            seed = seed*1664525u + 1013904223u;
            int chain = i/12, step = i%12;
            float ang = (float)(seed>>8 & 1023)/1023.0f*6.2831853f;
            if (step == 0) {   /* chains start on a coarse lattice, some within 5 m */
                rxy[i*2] = (float)(chain%20)*37.0f + (seed&3);
                rxy[i*2+1] = (float)(chain/20)*37.0f + (seed>>2&3);
            } else {
                float len = 3.0f + (float)(seed>>20 & 31);   /* 3..34 m, some > 60 via gaps */
                rxy[i*2] = rxy[i*2-2] + cosf(ang*0.2f + chain)*len;
                rxy[i*2+1] = rxy[i*2-1] + sinf(ang*0.2f + chain)*len;
            }
            rnext[i] = step < 11 ? i+1 : -1;
        }
        AiRoadNet lin = {.xy=rxy,.next=rnext,.n=RN,.half_width=NULL}, idx = lin;
        assert(ai_roads_index(&idx) && idx.pred_start && idx.cell_start);
        int checked = 0, junction = 0;
        for (int at = 0; at < RN; at++)
            for (int pv = -1; pv < 2; pv++) {
                int previous = pv < 0 ? -1 : pv == 0 ? (at%12 ? at-1 : -1) : (at+7)%RN;
                int a = ai_road_next(&lin, NULL, NULL, at, previous, 0);
                int b = ai_road_next(&idx, NULL, NULL, at, previous, 0);
                assert(a == b); checked++;
                if (a >= 0 && a != rnext[at]) junction++;
            }
        assert(junction > 50);   /* the junction pass, not only the chain, is covered */
        free(idx.pred_start); free(idx.pred_list); free(idx.cell_start); free(idx.cell_list);
        printf("road index: %d next-node queries identical (%d junction hops)\n", checked, junction);
    }
    puts("ai_drive_test: PASS");return 0;
}
