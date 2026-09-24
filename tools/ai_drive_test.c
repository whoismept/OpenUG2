/* Synthetic only: drive through the same steering response and horizontal
 * player physics as gameplay. Live audit separately checks tyre/world contact. */
#include "ai.h"
#include "physics.h"
#include "world.h"
#include <assert.h>

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
    AiRoadNet roads={xy,next,6,NULL};
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

int main(void) {
    phys_selftest();
    traffic_jump(1);traffic_jump(0);
    {
        const float body[6]={-2.0f,-0.9f,0,2.0f,0.9f,1.5f};
        AiCar other={.pos={0,4,0},.head=0,.half_length=2,
                     .half_width=.9f,.height=1.5f};
        float player[3]={0,0,0},vel[2]={0.2f,0};
        assert(phys_car_contacts(player,vel,.2f,0,body,&other,1)==0);
        assert(player[0]==0 && player[1]==0); /* adjacent lane, no contact */
        other.pos[0]=3.5f;other.pos[1]=0;
        assert(phys_car_contacts(player,vel,.2f,0,body,&other,1)>0);
        assert(player[0]<-.49f && other.pos[0]==3.5f); /* route pose unchanged */
        player[0]=0;other.pos[2]=8;
        assert(phys_car_contacts(player,vel,.2f,0,body,&other,1)==0);
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
        float nodes[N_OPENWORLD_AI*5*2],width[N_OPENWORLD_AI*5];
        int next[N_OPENWORLD_AI*5];
        AiRoadNet road={nodes,next,N_OPENWORLD_AI*5,width};
        AiCar cars[N_OPENWORLD_AI]={0}; AiTraffic traffic[N_OPENWORLD_AI]={0};
        float player[3]={0,0,5};
        const float loop[5][2]={{0,0},{30,0},{30,30},{0,30},{0,0}};
        for(int k=0;k<N_OPENWORLD_AI;k++) {
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
        assert(ai_traffic_spawn(&road,NULL,cars,traffic,player,0)==N_OPENWORLD_AI);
        for(int k=0;k<N_OPENWORLD_AI;k++) {
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
        AiRoadNet bend={bend_xy,bend_next,4,bend_width};
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
            ai_traffic_follow(turning,turn_routes,&bend,0,1);
            ai_traffic_step(&turning[0],&turn_routes[0],&bend,NULL);
        }
        assert(turn_routes[0].from==0 && turning[0].spd*PHYS_TICKRATE<9.0f);
        float dead_xy[]={0,0,30,0};int dead_next[]={1,-1};
        AiRoadNet dead={dead_xy,dead_next,2,NULL};
        car=(AiCar){.pos={29.9f,0,0},.head=0,.spd=.2f};
        route=(AiTraffic){.prev=-1,.from=0,.to=1,.after=-1,.along=29.9f};
        ai_traffic_step(&car,&route,&dead,NULL);
        assert(fabsf(car.head)<.03f); /* endpoint cannot force a U-turn */
        AiCar ending[N_OPENWORLD_AI]={0};AiTraffic ends[N_OPENWORLD_AI]={0};
        ending[0]=(AiCar){.pos={0,-2,0},.spd=12.0f/PHYS_TICKRATE};
        ends[0]=(AiTraffic){.prev=-1,.from=0,.to=1,.after=-1,.present=1,
                            .cruise_speed=ending[0].spd};
        for(int f=0;f<900 && ends[0].to>=0;f++) {
            ai_traffic_follow(ending,ends,&dead,0,1);
            ai_traffic_step(&ending[0],&ends[0],&dead,NULL);
            if(ending[0].pos[0]>27.0f)
                assert(ending[0].spd*PHYS_TICKRATE<5.0f);
        }
        assert(ends[0].to<0 && ending[0].pos[0]>27.0f && ending[0].pos[0]<30.0f);
        float reverse_xy[]={-90,0,-60,0,-60,0,-90,1};
        int reverse_next[]={1,-1,3,-1};
        AiRoadNet reverse={reverse_xy,reverse_next,4,NULL};
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
        AiRoadNet roads={xy,next,6,NULL};
        AiCar cars[N_OPENWORLD_AI]={0};AiTraffic routes[N_OPENWORLD_AI]={0};
        float player[3]={0,0,0};
        for(int k=0;k<N_OPENWORLD_AI;k++)routes[k].to=-1;
        assert(ai_traffic_respawn(&roads,NULL,cars,routes,0,player,0));
        assert(cars[0].head>1.4f && cars[0].head<1.8f); /* side street first */
    }
    {
        float xy[]={-120,0,-90,0,-60,0};int next[]={1,2,-1};
        AiRoadNet road={xy,next,3,NULL};
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
        AiRoadNet road={xy,next,2,NULL};
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
            ai_traffic_follow(cars,routes,&road,0,2);
            if(cars[0].spd<routes[0].cruise_speed-0.01f/PHYS_TICKRATE)slowed=1;
            ai_traffic_step(&cars[0],&routes[0],&road,NULL);
            ai_traffic_step(&cars[1],&routes[1],&road,NULL);
            assert(cars[1].pos[0]-cars[0].pos[0]>10.0f);
            assert(!phys_ai_overlap(&cars[0],&cars[1]));
        }
        assert(slowed && cars[0].spd<routes[0].cruise_speed);
        cars[0].pos[0]=0;cars[0].spd=20.0f/PHYS_TICKRATE;
        cars[1].pos[0]=30;cars[1].spd=0;routes[1].to=-1;
        ai_traffic_follow(cars,routes,&road,0,2);
        assert(cars[0].target_speed<cars[0].spd); /* stopped cars hold traffic */
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
    for(int i=0;i<4;i++)for(int r=0;r<2;r++)drive_curve(i*1.570796327f,r);
    puts("ai_drive_test: PASS");return 0;
}
