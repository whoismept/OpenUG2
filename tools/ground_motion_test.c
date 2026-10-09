/* Synthetic geometry only. A moving contact envelope must not cross a solid
 * ground face between samples. Compile with GROUND_MOTION_BASELINE to reproduce
 * the old point-query-only movement (no sweep) before the M155 fix. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "world.h"
#include "physics.h"
#include "render.h"

#ifdef GROUND_MOTION_BASELINE
static float sweep(const N2Scene *s, const float a[3], const float b[3], WGroundHit *h) {
    (void)s; (void)a; (void)b; (void)h;
    return 1.0f; /* Old XY integrator accepted the full move before gathering. */
}
#else
#define sweep world_ground_sweep
#endif

#ifdef GROUND_LIMIT_BASELINE
static float ground_motion_limit(const N2Scene *s, const PhysRideState *r,
        const PhysRideSupport *sup,const float old[3],float oldh,
        float pos[3],float *heading,float vel[2],WGroundHit *hit) {
    (void)s;(void)r;(void)sup;(void)old;(void)oldh;
    (void)pos;(void)heading;(void)vel;(void)hit;return 1;
}
#elif !defined(GROUND_MOTION_BASELINE)
#include "ground_motion.h"
#endif

static void close_to(float a, float b) {
    if(fabsf(a-b)>=.0001f){fprintf(stderr,"got %.9g expected %.9g\n",a,b);}
    assert(fabsf(a-b)<0.0001f);
}

/* A moving camera target needs the same elapsed-time solution at every
 * cadence. Static-target exponential easing alone changes the high-speed lag. */
static void camera_timing_test(void) {
    const int rates[]={144,30,120,20,60,240,10};
    const float velocities[]={8,61,-12};
    const double lambda=-60*log(1-.22f);
    for(int mode=0;mode<8;mode++)for(int speed=0;speed<3;speed++) {
        float eye[]={-4,0,2},previous[]={-4,0,2};double elapsed=0;
        float worst=0;
        for(int frame=0;elapsed<10;frame++) {
            int rate=rates[mode==7?frame%7:mode];double dt=1.0/rate;elapsed+=dt;
            float desired[]={(float)(velocities[speed]*elapsed-4),
                             (float)(-3*elapsed),(float)(2+.2*elapsed)};
            render_camera_ease(eye,previous,desired,.22f,(float)(dt*60));
            double lag=(1-exp(-lambda*elapsed))/lambda;
            const double expected[]={velocities[speed]*(elapsed-lag)-4,
                                     -3*(elapsed-lag),2+.2*(elapsed-lag)};
            for(int c=0;c<3;c++)worst=fmaxf(worst,(float)fabs(eye[c]-expected[c]));
            memcpy(previous,desired,sizeof previous);
        }
        printf("camera cadence %d speed%g: max path error %.7fm\n",mode,velocities[speed],worst);fflush(stdout);
        assert(worst<.002f);
    }
    float eye[]={0,0,0},previous[]={10,20,30},desired[]={10,20,30};
    render_camera_ease(eye,previous,desired,.22f,1);
    for(int c=0;c<3;c++)close_to(eye[c],.22f*desired[c]); /* Same parked-car tuning. */
    float saved[3];memcpy(saved,eye,sizeof saved);
    render_camera_ease(eye,previous,desired,.22f,0);assert(!memcmp(saved,eye,sizeof eye));
    render_camera_ease(eye,previous,desired,1,1);assert(!memcmp(desired,eye,sizeof eye));
    render_camera_ease(eye,previous,desired,.22f,.000001f);
    for(int c=0;c<3;c++)assert(isfinite(eye[c]));
    puts("camera timing: fixed/changing FPS, forward/reverse speed, static tuning and snap PASS");
}

static void slope_spawn(float grade,float heading) {
    /* A long wheelbase can span more height than static suspension reach.
       Initial placement must use the actual slope, not four level probes. */
    float v[]={-20,-20,0,0,0,20,-20,0,0,0,20,20,0,0,0,-20,20,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    float co=cosf(heading),si=sinf(heading);
    for(int k=0;k<4;k++) {
        float x=v[k*5],y=v[k*5+1];v[k*5+2]=grade*x+.03f*y;
        v[k*5]=co*x-si*y;v[k*5+1]=si*x+co*y;
    }
    N2Mesh mesh={.verts=v,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD};
    N2Scene scene={&mesh,1,1};PhysRideSupport sup={0};
    for(int k=0;k<4;k++){sup.ax[k]=k<2?4:-4;sup.ay[k]=(k&1)?-1.2f:1.2f;}
    float pos[3]={0},vel[2]={0};
    int contacts=world_ride_gather(&scene,pos,heading,vel,heading,NULL,&sup,NULL,NULL,NULL);
    printf("slope spawn grade=%+.2f heading=%.2f contacts=%d\n",grade,heading,contacts);fflush(stdout);
    assert(contacts==4);
    PhysRideState ride;phys_ride_init(&ride,&sup);
    close_to(ride.pitch,grade);close_to(ride.roll,.03f);
    for(int i=0;i<120;i++) {
        assert(world_ride_gather(&scene,pos,heading,vel,heading,&ride,&sup,NULL,NULL,NULL)==4);
        phys_ride_step(&ride,&sup,1.0f/60.0f);pos[2]=ride.z;
        assert(ride.contact_mask==15 && fabsf(ride.z)<.001f);
    }
    /* This is placement only: never rotate a live unsupported body onto road. */
    ride=(PhysRideState){0};pos[2]=0;
    assert(world_ride_gather(&scene,pos,heading,vel,heading,&ride,&sup,NULL,NULL,NULL)<4);
    /* No reachable centre floor: do not recover onto a deck or down from air. */
    for(int side=-1;side<=1;side+=2) {
        pos[2]=side*3;
        assert(world_ride_gather(&scene,pos,heading,vel,heading,NULL,&sup,NULL,NULL,NULL)==0);
    }
    /* A centre plane cannot manufacture road beyond an actual edge. */
    pos[2]=0;
    for(int k=1;k<=2;k++) {
        float x=1,y=k==1?-20:20;
        v[k*5]=co*x-si*y;v[k*5+1]=si*x+co*y;v[k*5+2]=grade*x+.03f*y;
    }
    assert(world_ride_gather(&scene,pos,heading,vel,heading,NULL,&sup,NULL,NULL,NULL)==2);
}

static void slope_contact(float grade, float heading, float step, int reverse) {
    /* A continuous 6% descent at 108 km/h: no edge, seam or missing mesh.
     * A damper must resist suspension travel, not travel down the road. */
    const float dt=1.0f/60.0f, co=cosf(heading), si=sinf(heading);
    float v[]={-10,-10,0,0,0, 400,-10,0,0,0,
                400,10,0,0,0, -10,10,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    for(int k=0;k<4;k++) {
        float x=v[k*5],y=v[k*5+1];v[k*5+2]=grade*x;
        v[k*5]=co*x-si*y;v[k*5+1]=si*x+co*y;
    }
    if(reverse)for(int k=0;k<6;k+=3){uint16_t t=idx[k];idx[k]=idx[k+2];idx[k+2]=t;}
    N2Mesh m={0};m.verts=v;m.idx=idx;m.nverts=4;m.nidx=6;m.cat=N2_ROAD;
    N2Scene sc={&m,1,1};PhysRideSupport s={0};PhysRideState r;
    for(int k=0;k<4;k++) {
        s.ax[k]=k<2?1.2f:-1.2f;s.ay[k]=(k&1)?-.7f:.7f;
        s.z[k]=grade*s.ax[k];s.valid[k]=1;
    }
    s.pitch_limit=fabsf(grade);
    phys_ride_init(&r,&s);r.vz=grade*step/dt;
    int air=0,partial=0,missing=0;float maxerr=0;
    for(int f=1;f<=600;f++) {
        for(int k=0;k<4;k++) {
            WGroundHit h;int why;
            float x=f*step+s.ax[k],y=s.ay[k];
            s.valid[k]=world_wheel_support(&sc,co*x-si*y,si*x+co*y,
                phys_ride_wheel_z(&r,&s,k),PHYS_RIDE_REACH_UP,
                phys_ride_reach_down(&r,dt),&h,NULL,&why)!=WSURF_NONE;
            s.z[k]=h.z;if(!s.valid[k])missing++;
            const float vel[]={co*step,si*step};
            s.vz[k]=s.valid[k]?phys_ride_support_vz(h.normal,vel,heading,heading,s.ax[k],s.ay[k],dt):0;
#ifdef SLOPE_DAMPING_BASELINE
            s.vz[k]=0; /* Original damper used absolute wheel-Z velocity. */
#endif
        }
        s.pitch_limit=fabsf(grade); /* normal of this actual synthetic road */
        phys_ride_step(&r,&s,dt);
        if(!r.contact_mask)air++;
        if(r.contact_mask!=15)partial++;
        maxerr=fmaxf(maxerr,fabsf(r.z-grade*f*step));
    }
    printf("continuous grade=%+.2f heading=%.2f step=%.2f: air=%d partial=%d rejected=%d max body error=%.6f m\n",
           grade,heading,step,air,partial,missing,maxerr);fflush(stdout);
    assert(air==0 && partial==0 && missing==0 && maxerr<fabsf(grade*step)+.005f);
    /* No covering geometry: even a nonzero supplied rate cannot invent force. */
    float vz=r.vz;
    for(int k=0;k<4;k++)s.valid[k]=0;
    phys_ride_step(&r,&s,dt);
    assert(r.contact_mask==0);close_to(r.vz,vz-PHYS_RIDE_G*dt);
}

static void camera_collision_test(void) {
    float v[]={0,-2,-2,0,0, 0,2,-2,0,0, 0,2,2,0,0, 0,-2,2,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh m={.verts=v,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER};
    N2Scene s={&m,1,1};float bounds[1][4]={{0,-2,0,2}};
    for(int side=-1;side<=1;side+=2) {
        float a[]={side*2,0,0},eye[]={-side*4,0,0};
        assert(world_camera_clip(&s,bounds,a,eye,.25f)<1);
        assert(fabsf(eye[0]-side*.25f)<.0002f);
        /* Long movement also catches a zero-thickness, reverse-facing wall. */
        eye[0]=-side*100;world_camera_clip(&s,bounds,a,eye,.25f);
        assert(fabsf(eye[0]-side*.25f)<.0002f);
    }
    float a[]={2,2.1f,0},eye[]={-4,2.1f,0};
    world_camera_clip(&s,bounds,a,eye,.25f);
    assert(eye[0]>.22f && eye[0]<.24f); /* sphere brushes finite wall edge */
    a[1]=eye[1]=3;eye[0]=-4;
    close_to(world_camera_clip(&s,bounds,a,eye,.25f),1);close_to(eye[0],-4);
    /* Road and overhead sheets block from either side; sky/glow do not. */
    for(int i=0;i<4;i++){v[i*5]=v[i*5+2];v[i*5+2]=0;}
    m.cat=N2_ROAD;a[0]=eye[0]=a[1]=eye[1]=0;a[2]=2;eye[2]=-4;
    world_camera_clip(&s,NULL,a,eye,.25f);assert(fabsf(eye[2]-.25f)<.0002f);
    a[2]=-2;eye[2]=4;world_camera_clip(&s,NULL,a,eye,.25f);
    assert(fabsf(eye[2]+.25f)<.0002f);
    m.cat=N2_GLOW;eye[2]=4;close_to(world_camera_clip(&s,NULL,a,eye,.25f),1);
    m.cat=N2_SKY;close_to(world_camera_clip(&s,NULL,a,eye,.25f),1);
    puts("camera sweep: PASS (two-sided walls, edges, clearance, floor/ceiling, long moves)");
}

static void light_effect_collision_test(void) {
    float v[]={-4,-4,0,0,0, 4,-4,0,1,0, 4,4,0,1,1, -4,4,0,0,1,
               -4,-4,10,0,0, 4,-4,10,1,0, 4,4,10,1,1, -4,4,10,0,1};
    uint16_t idx[]={0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7};
    N2Mesh m={.verts=v,.nverts=8,.idx=idx,.nidx=24,.cat=N2_OTHER,.scen=N2_SC_PROP};
    N2Scene scene={&m,1,1};float bounds[][4]={{-4,-4,4,4}},heights[][2]={{0,10}};
    float body[]={-2,-.85f,0,2,.85f,1.5f},saved[40];memcpy(saved,v,sizeof v);
    int src[]={0};
    for(int kind=0;kind<4;kind++) {
        m.mat_exact=kind!=1;
        m.texkey=kind<2?N2_TEX_SFX_LIGHT_BEAMA:kind==2?0:N2_TEX_SFX_FLARE_GLOWA;
        int solid=kind!=0;float obstacles[1][4],z[1][2];int owner[1];
        assert(phys_collect_walls(&scene,obstacles,owner,z,1)==solid);
        assert(cw_probe_contact(&scene,0,-4.1f,0,.5f,.1f,1.5f)==solid);
        float p[]={-5.9f,0,0},vel[]={.1f,0};
        assert(!!collide_body_walls(p,vel,0,body,bounds,heights,1,.1f,1.5f,&scene,src,NULL,0)==solid);
        p[0]=-5.9f;vel[0]=.1f;
        assert(!!collide_body_mesh_wall(p,vel,0,body,.1f,1.5f,&scene,0,0,INFINITY,NULL)==solid);
        float anchor[]={-6,0,1},eye[]={6,0,1};
        assert((world_camera_clip(&scene,bounds,anchor,eye,.25f)<1)==solid);
        assert(!memcmp(saved,v,sizeof v));
    }
    puts("light effects: PASS (exact material, opaque/ambiguous fixtures, broad/narrow collision, camera, unchanged geometry)");
}

static void buried_foundation_test(void) {
    float road[]={-8,-8,0,0,0,8,-8,0,0,0,8,8,0,0,0,-8,8,0,0,0};
    float lower[20];memcpy(lower,road,sizeof lower);
    float wall[]={1.2f,-4,-1,0,0,1.2f,4,-1,0,0,
                  1.2f,4,.15f,0,0,1.2f,-4,.15f,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=road,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
                     {.verts=lower,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
                     {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER}};
    N2Scene scene={meshes,3,3};float bounds[][4]={{-8,-8,8,8},{-8,-8,8,8},{1.2f,-4,1.2f,4}};
    float ob[][4]={{1.2f,-4,1.2f,4}},oz[][2]={{-3,1}};int src[]={2};
    const float bb[]={-2,-.85f,0,2,.85f,1.5f};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));
    world_ground_grid_activate(&grid);
    for(int surface=0;surface<2;surface++)
    for(int winding=0;winding<2;winding++) {
        if(winding)for(int k=0;k<6;k+=3){uint16_t t=idx[k];idx[k]=idx[k+2];idx[k+2]=t;}
        for(int reverse=0;reverse<2;reverse++)
        for(int mode=0;mode<7;mode++) {
            for(int k=0;k<4;k++) {
                road[k*5]=(k==1 || k==2)?(mode==5?1:8):-8;
                lower[k*5]=(k==0 || k==3)?(mode==5?1:-8):8;
                road[k*5+2]=.2f*road[k*5];
                lower[k*5+2]=.2f*lower[k*5]+(mode==5?1:-2);
            }
            float z=mode==2?-2:mode==3?-.35f:0;
            wall[2]=wall[7]=mode==2?-3:-1;
            wall[12]=wall[17]=mode==1?.8f:mode==6?.4f:.15f;
            meshes[0].cat=mode==4?N2_OTHER:surface?N2_TERRAIN:N2_ROAD;
            meshes[1].cat=surface?N2_TERRAIN:N2_ROAD;
            float p[]={0,0,z},vel[]={.1f,0};
            int solid=mode!=0 && mode!=6;
            assert(!!collide_body_walls(p,vel,reverse*3.14159265f,bb,ob,oz,1,
                z+.05f,z+1.5f,&scene,src,NULL,0)==solid);
            p[0]=p[1]=0;vel[0]=.1f;
            assert(!!collide_body_walls_preview(p,vel,reverse*3.14159265f,bb,
                ob,oz,1,z+.05f,z+1.5f,&scene,src)==solid);
            p[0]=p[1]=0;vel[0]=.1f;
            assert(!!collide_body_mesh_wall(p,vel,reverse*3.14159265f,bb,
                z+.05f,z+1.5f,&scene,2,0,INFINITY,NULL)==solid);
            assert(!!cw_probe_contact(&scene,2,0,0,2,z+.05f,z+1.5f)==solid);
            if(!solid)assert(p[0]==0 && vel[0]==.1f);
        }
    }
    /* Without world support, standalone collision keeps the source wall. */
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    assert(cw_probe_contact(&scene,2,0,0,2,.05f,1.5f));
    puts("buried foundations/seams: PASS (ROAD/TERRAIN, both bumpers/windings, exposed wall, lower/disconnected decks, unsupported body, no ground, standalone)");
}

/* A narrow XY projection does not make a vertical skirt supporting pavement. */
static void vertical_support_test(void) {
    float floor[]={-4,-4,0,0,0,4,-4,0,0,0,4,4,0,0,0,-4,4,0,0,0};
    float face[]={0,-4,-1,0,0,0,4,-1,0,0,.0001f,4,1,0,0,.0001f,-4,1,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
                     {.verts=face,.nverts=4,.idx=idx,.nidx=6,.cat=N2_TERRAIN}};
    N2Scene scene={meshes,2,2};WGroundHit h;
    for(int winding=0;winding<2;winding++)for(int order=0;order<2;order++) {
        assert(world_wheel_support(&scene,.0000525f,0,.05f,.25f,.25f,&h,NULL,NULL));
        printf("support near vertical face z%.6f nz%.6f\n",h.z,h.normal[2]);fflush(stdout);
        assert(fabsf(h.z)<.0001f && h.normal[2]>.99f);
        N2Mesh swap=meshes[0];meshes[0]=meshes[1];meshes[1]=swap;
        if(order)for(int t=0;t<6;t+=3){uint16_t q=idx[t];idx[t]=idx[t+2];idx[t+2]=q;}
    }
    N2Scene wall={meshes+1,1,1};
    assert(!world_wheel_support(&wall,.0000525f,0,.05f,.25f,.25f,&h,NULL,NULL));
    float a[]={-.1f,0,.25f},b[]={.1f,0,.25f};
    assert(world_ground_sweep(&wall,a,b,&h)<1); /* still a solid crossed face */
    float pos[]={-.5f,0,0},vel[]={.1f,0},bb[]={-1,-.5f,0,1,.5f,1.5f};
    assert(collide_body_mesh_wall(pos,vel,0,bb,.05f,1.5f,&wall,0,0,INFINITY,NULL));
    puts("near-vertical faces: reject tyre support, preserve swept/body collision PASS");
}

static void overlapping_ramp(float grade) {
    float floor[]={-20,-10,0,0,0,80,-10,0,0,0,80,10,0,0,0,-20,10,0,0,0};
    float ramp[]={0,-10,0,0,0,80,-10,12,0,0,80,10,12,0,0,0,10,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh m[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
                {.verts=ramp,.nverts=4,.idx=idx,.nidx=6,.cat=N2_TERRAIN}};
    N2Scene sc={m,2,2}; WGroundHit h;
    for(int k=0;k<4;k++)ramp[k*5+2]=grade*ramp[k*5];
    for(int order=0;order<2;order++) {
        assert(world_wheel_support(&sc,.15f/grade,0,0,.25f,.3f,&h,NULL,NULL));
        close_to(h.z,.15f);
        N2Mesh tmp=m[0];m[0]=m[1];m[1]=tmp;
    }
    /* Nearby flat stacked layers still choose closest; unreachable ramps are
     * never recovery targets, and a real falling wheel remains unsupported. */
    for(int k=0;k<4;k++)ramp[k*5+2]=.15f;
    assert(world_wheel_support(&sc,1,0,0,.25f,.3f,&h,NULL,NULL));close_to(h.z,0);
    /* A near-vertical retaining face cannot take ramp priority over a floor. */
    for(int k=0;k<4;k++)ramp[k*5+2]=30*ramp[k*5]-29.85f;
    assert(world_wheel_support(&sc,1,0,0,.25f,.3f,&h,NULL,NULL));close_to(h.z,0);
    for(int k=0;k<4;k++)ramp[k*5+2]=3+.15f*ramp[k*5];
    assert(world_wheel_support(&sc,1,0,0,.25f,.3f,&h,NULL,NULL));close_to(h.z,0);
    assert(!world_wheel_support(&sc,1,0,8,.25f,.3f,&h,NULL,NULL));
    for(int k=0;k<4;k++)ramp[k*5+2]=grade*ramp[k*5];
    PhysRideSupport sup={.ax={1.2f,1.2f,-1.2f,-1.2f},.ay={.7f,-.7f,.7f,-.7f}};
    float pos[3]={-3,0,0},vel[2]={.05f,0},heading=0;
    world_ride_gather(&sc,pos,0,vel,0,NULL,&sup,NULL,NULL,NULL);
    PhysRideState r;phys_ride_init(&r,&sup);
    for(int f=0;f<400;f++) {
        float old[3]={pos[0],pos[1],pos[2]};pos[0]+=.05f;vel[0]=.05f;
        ground_motion_limit(&sc,&r,&sup,old,0,pos,&heading,vel,NULL);
        world_ride_gather(&sc,pos,0,vel,0,&r,&sup,NULL,NULL,NULL);
        phys_ride_step(&r,&sup,1.0f/60);pos[2]=r.z;
        assert(r.contact_mask && isfinite(r.z));
    }
    printf("overlapping ramp: x=%.3f z=%.3f\n",pos[0],pos[2]);
    assert(pos[0]>16 && pos[2]>2);
}

static void ramp_edge_wall(void) {
    /* Terrain retaining walls exceed the old low-rail ceiling. Their real
       face, clipped to body height, must stop a car before support is lost. */
    float v[]={0,-20,0,0,0,0,20,0,0,0,0,20,8,0,0,0,-20,8,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh m={.verts=v,.nverts=4,.idx=idx,.nidx=6,.cat=N2_TERRAIN};
    N2Scene sc={&m,1,1};float bounds[1][4]={{0,-20,0,20}};WGroundGrid grid={0};
    assert(world_ground_grid_build(&grid,&sc,bounds));world_ground_grid_activate(&grid);
    const float bb[]={-1.97f,-.94f,0,1.97f,.94f,1.37f};
    for(int side=-1;side<=1;side+=2) {
        float p[]={side*1.8f,0,2},vel[]={-side*.3f,.1f};
        assert(world_body_wall_push(&sc,p,vel,0,bb,2.1f,3.37f,NULL));
        assert(side*p[0]>=1.969f);close_to(vel[0],0);close_to(vel[1],.1f);
        assert(!world_wall_clear_at(&sc,side*.5f,0,2,.75f));
        p[0]=side*1.8f;p[2]=10;vel[0]=-side*.3f;
        assert(!world_body_wall_push(&sc,p,vel,0,bb,10.1f,11.37f,NULL));
    }
    /* A thin curb can gain metres along a hill without becoming a wall. */
    v[7]=v[12]=5;v[17]=.1f;v[12]+=.1f;
    float p[]={.5f,0,2.5f},vel[]={-.3f,0};
    assert(!world_body_wall_push(&sc,p,vel,0,bb,2.5f,3.87f,NULL));
    world_ground_grid_free(&grid);
    puts("ramp edge: tall terrain walls, both sides, height clipping and passable curb PASS");
}

static void wall_across_cell_boundary(void) {
    float floor[]={0,0,0,0,0,128,0,0,0,0,128,128,0,0,0,0,128,0,0,0};
    float wall[]={64.5f,5,0,0,0,64.5f,40,0,0,0,64.5f,40,.55f,0,0,64.5f,5,.55f,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh meshes[]={{.verts=floor,.nverts=4,.idx=idx,.nidx=6,.cat=N2_ROAD},
                    {.verts=wall,.nverts=4,.idx=idx,.nidx=6,.cat=N2_TERRAIN}};
    N2Scene scene={meshes,2,2};float bounds[][4]={{0,0,128,128},{64.5f,5,64.5f,40}};
    WGroundGrid grid={0};assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);
    float bb[]={-1.97f,-.94f,0,1.97f,.94f,1.37f};
    float p[]={63.9f,20,0},vel[]={.2f,.1f};
    assert(world_body_wall_push(&scene,p,vel,0,bb,.1f,1.37f,NULL));
    assert(p[0]<=62.531f && fabsf(vel[0])<1e-6f && vel[1]==.1f);
    /* Keep low surface seams passable; the same region's real wall is solid. */
    wall[12]=wall[17]=.1f;p[0]=63.9f;vel[0]=.2f;
    assert(!world_body_wall_push(&scene,p,vel,0,bb,0,1.37f,NULL));
    world_ground_grid_free(&grid);
    puts("world walls: adjacent-cell body coverage and short terrain wall PASS");
}

static void test_swept_wall_motion(void) {
    float verts[20]={64,-20,0,0,0,64,20,0,0,0,64,20,6,0,0,64,-20,6,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh mesh={.verts=verts,.nverts=4,.idx=idx,.nidx=6,.cat=N2_OTHER,.scen=N2_SC_WALL};
    N2Scene scene={&mesh,1,1};float obst[1][4],obz[1][2];int src[1];
    const float bb[]={-.8f,-.4f,0,.8f,.4f,1.4f};
    for(int terrain=0;terrain<2;terrain++) {
        mesh.cat=terrain?N2_TERRAIN:N2_OTHER;
        mesh.scen=terrain?N2_SC_TERRAIN:N2_SC_WALL;
        float bounds[1][4]={{64,-20,64,20}};WGroundGrid grid={0};
        if(terrain){assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);}
        int no=phys_collect_walls(&scene,obst,src,obz,1);
        for(int side=-1;side<=1;side+=2)for(int air=0;air<2;air++) {
            float old[]={64+4*side,0,air?2.5f:0},p[]={64-4*side,1,old[2]};
            float vel[]={-side*2.0f,.25f},before[]={vel[0],vel[1]};int rails=0;
            int walls=world_body_walls_move(&scene,old,p,vel,0,bb,old[2]+.05f,old[2]+1.4f,
                air?-.5f:0,obst,obz,no,src,NULL,0,NULL,&rails);
            assert(walls+rails>0 && side*(p[0]-64)>=.799f);
            printf("sweep terrain%d side%d air%d p%g,%g vel%.9g,%.9g\n",terrain,side,air,p[0],p[1],vel[0],vel[1]);fflush(stdout);
            assert(fabsf(vel[0])<1e-4f && fabsf(vel[1]-.25f)<1e-4f);
            assert(fabsf(p[1]-1)<1e-4f); /* Full tangential travel retained. */
            PhysRideState ride={.contact_mask=air?0:15};
            phys_ride_wall_response(&ride,before,vel);
            if(air)assert(side*vel[0]>.39f);else assert(fabsf(vel[0])<1e-5f);
        }
        float old[]={60,0,7},p[]={68,1,7},vel[]={2,.25f};int rails=0;
        assert(!world_body_walls_move(&scene,old,p,vel,0,bb,7.05f,8.4f,0,
            obst,obz,no,src,NULL,0,NULL,&rails) && !rails);
        close_to(p[0],68);close_to(p[1],1); /* Above the wall remains free. */
        world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
}

/* Real reverse throttle must use the same world contact path as forward
 * driving: both wall sides, reversed winding, and compact/long body bounds. */
static void reverse_wall_drive_test(void) {
    float verts[20]={0,-100,0,0,0,0,100,0,0,0,0,100,4,0,0,0,-100,4,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh mesh={.verts=verts,.nverts=4,.idx=idx,.nidx=6,
                 .cat=N2_OTHER,.scen=N2_SC_WALL};
    N2Scene scene={&mesh,1,1};float obst[1][4],obz[1][2];int src[1];
    float bounds[1][4]={{0,-100,0,100}};WGroundGrid grid={0};
    for(int terrain=0;terrain<2;terrain++) {
        mesh.cat=terrain?N2_TERRAIN:N2_OTHER;
        mesh.scen=terrain?N2_SC_TERRAIN:N2_SC_WALL;
        int no=phys_collect_walls(&scene,obst,src,obz,1);
        if(terrain){assert(world_ground_grid_build(&grid,&scene,bounds));world_ground_grid_activate(&grid);}
        for(int winding=0;winding<2;winding++) {
            for(int side=-1;side<=1;side+=2)for(int reverse=0;reverse<2;reverse++)
            for(int longbody=0;longbody<2;longbody++) {
                float length=longbody?6:2,width=longbody?1.3f:.9f;
                float bb[]={-length,-width,0,length,width,1.5f};
                float h=(side>0?3.14159265f:0)+.2f+reverse*3.14159265f;
                float extent=width+(length-width)*fabsf(cosf(h));
                float p[]={side*(length+1),0,0},v[]={0,0},speed=0;int hits=0;
                PhysRideState ride={.contact_mask=15};
                for(int t=0;t<600;t++) {
                    float old[3];memcpy(old,p,sizeof old);
                    phys_drive_step(p,v,&h,&speed,reverse?-1:1,0,0,NULL,NULL,&ride);
                    float before[]={v[0],v[1]};int rails=0;
                    int walls=world_body_walls_move(&scene,old,p,v,h,bb,.05f,1.5f,0,
                        obst,obz,no,src,NULL,0,NULL,&rails);
                    hits+=walls+rails;
                    phys_ride_wall_response(&ride,before,v);
                    speed=v[0]*cosf(h)+v[1]*sinf(h);
                    assert(side*p[0]>=extent-.0001f);
                }
                assert(hits>0 && fabsf(p[1])>2); /* Stops inward, keeps wall slide. */
            }
            for(int t=0;t<6;t+=3){uint16_t tmp=idx[t];idx[t]=idx[t+2];idx[t+2]=tmp;}
        }
        world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    }
    puts("forward/reverse drive: both wall sets/sides/windings/body sizes PASS");
}

/* Equal elapsed time must produce equal motion, independent of frame rate.
 * Exercise the real horizontal solver with acceleration, steering and braking. */
static void fixed_time_test(void) {
    const int rates[]={10,30,60,120,144,240};
    float expected[9]={0};
    for(int r=0;r<6;r++) {
        PhysClock clock={0};float pos[3]={0},vel[2]={0},heading=0,speed=0;
        float shake=0,shake_velocity=0;int ticks=0,zero=0,multiple=0;
        for(int frame=0;frame<20*rates[r];frame++) {
            int steps=phys_clock_steps(&clock,1.0/rates[r]);
            zero+=steps==0;multiple+=steps>1;
            for(int step=0;step<steps;step++,ticks++) {
                float throttle=ticks<900?1:-1,steer=ticks>=300 && ticks<600?.25f:0;
                phys_car_step(pos,vel,&heading,&speed,throttle,steer,0,NULL,NULL);
                phys_landing_camera(&shake,&shake_velocity,ticks==100?4:0,1/PHYS_TICKRATE);
            }
        }
        float actual[]={pos[0],pos[1],pos[2],vel[0],vel[1],heading,speed,shake,shake_velocity};
        assert(ticks==1200 && clock.remainder<1e-8);
        if(!r)memcpy(expected,actual,sizeof expected);
        else assert(!memcmp(expected,actual,sizeof expected));
        if(rates[r]>60)assert(zero>0);
        if(rates[r]<60)assert(multiple>0);
        printf("fixed clock %d FPS: %d ticks, pose %.3f %.3f, speed %.3f\n",
               rates[r],ticks,pos[0],pos[1],PHYS_KMH(speed));
    }
    PhysClock clock={0};assert(phys_clock_steps(&clock,1.0/120)==0);
    double remainder=clock.remainder;
    assert(!phys_clock_steps(&clock,NAN) && !phys_clock_steps(&clock,INFINITY));
    assert(!phys_clock_steps(&clock,-1) && !phys_clock_steps(&clock,0));
    assert(clock.remainder==remainder);
    assert(phys_clock_steps(&clock,10)==8); /* bounded stall recovery */
    assert(phys_clock_steps(&clock,1.0/120)==1); /* fractional time retained */
    puts("fixed clock: render independence, fractional ticks and bounded stalls PASS");
}

int main(void) {
    vertical_support_test();
    buried_foundation_test();
    fixed_time_test();
    test_swept_wall_motion();
    reverse_wall_drive_test();
    wall_across_cell_boundary();
    ramp_edge_wall();
    overlapping_ramp(.15f);overlapping_ramp(.40f);
    slope_contact(.40f,0,.05f,0);
    camera_timing_test();
    camera_collision_test();
    light_effect_collision_test();
    slope_spawn(.40f,0);
    slope_spawn(.08f,0);slope_spawn(-.08f,1.2f);
    slope_contact(-.06f,0,.5f,0);
    slope_contact(-.06f,1.2f,.5f,1);
    slope_contact(.06f,-.7f,.5f,0);
    slope_contact(0,0,.5f,0);
    slope_contact(.06f,.3f,0,1);
    /* Height-rate geometry includes accepted yaw and respects winding. */
    const float normal[]={-.1f,-.2f,1}, reversed[]={.1f,.2f,-1}, still[]={0,0};
    close_to(phys_ride_support_vz(normal,still,0,1.5707963f,1,0,1),.1f);
    close_to(phys_ride_support_vz(reversed,still,0,1.5707963f,1,0,1),.1f);
    close_to(phys_ride_support_vz(normal,still,0,0,1,0,1),0);
    close_to(phys_ride_support_vz(normal,still,0,0,1,0,0),0);
    const float vertical[]={1,0,0}, contour[]={.2f,-.1f};
    close_to(phys_ride_support_vz(vertical,contour,0,0,1,0,1),0);
    close_to(phys_ride_support_vz(normal,contour,0,0,1,0,1),0);
    /* z=x, y in [-10,10]. Moving at z=.25 from x=0 to x=.5
     * reaches the face at x=.25, halfway through the step. */
    float v[]={-10,-10,-10,0,0, 10,-10,10,0,0,
                10,10,10,0,0, -10,10,-10,0,0};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh m={0}; m.verts=v; m.idx=idx; m.nverts=4; m.nidx=6; m.cat=N2_TERRAIN;
    N2Scene s={&m,1,1};
    WGroundHit h;
    float a[]={0,0,.25f}, b[]={.5f,0,.25f};
    float f=sweep(&s,a,b,&h);
    printf("rising ground: fraction %.6f (expected .5)\n",f); fflush(stdout);
    close_to(f,.5f); /* RED: old movement returns 1 and tunnels through. */
    assert(h.mesh==0 && h.cat==WSURF_TERRAIN);
    close_to(h.z,.25f); assert(h.normal[0]<-.70f && h.normal[2]>.70f);

    /* Winding must not change a ground query. */
    for(int i=0;i<6;i+=3){uint16_t t=idx[i];idx[i]=idx[i+2];idx[i+2]=t;}
    close_to(sweep(&s,a,b,&h),.5f);
    /* Below the entire sheet: an overhead deck is NOT a recovery target. */
    a[2]=b[2]=-7; close_to(sweep(&s,a,b,&h),1);
    /* Moving downhill / away from the surface stays free. */
    a[0]=.5f; b[0]=0; a[2]=b[2]=.75f; close_to(sweep(&s,a,b,&h),1);
    /* A plane crossing outside the actual triangle must not block. */
    a[0]=0;b[0]=.5f;a[1]=b[1]=20;a[2]=b[2]=.25f;
    close_to(sweep(&s,a,b,&h),1);
    /* No ground and non-ground scenery are not invisible floors. */
    a[1]=b[1]=0;m.cat=N2_OTHER;close_to(sweep(&s,a,b,&h),1);
    m.cat=N2_ROAD;s.count=0;close_to(sweep(&s,a,b,&h),1);s.count=1;
    /* Flat road, zero movement, touching then moving into/out of the face. */
    for(int i=0;i<4;i++)v[i*5+2]=0;
    close_to(sweep(&s,a,b,&h),1);close_to(sweep(&s,a,a,&h),1);
    a[2]=0;b[2]=-.1f;close_to(sweep(&s,a,b,&h),0);
    b[2]=.1f;close_to(sweep(&s,a,b,&h),1);
    /* Swept downward crossing detects a floor, irrespective of step length. */
    a[2]=1;b[2]=-1;close_to(sweep(&s,a,b,&h),.5f);

    /* Real acceleration grid and brute-force must agree. >512 occupants
     * protects this query from the point-query scratch-array capacity cap. */
    N2Mesh many[520];float high[20];memcpy(high,v,sizeof high);
    for(int i=0;i<4;i++)high[i*5+2]=7;
    float bb[520][4];
    for(int i=0;i<520;i++){many[i]=m;many[i].verts=high;
        bb[i][0]=bb[i][1]=-10;bb[i][2]=bb[i][3]=10;}
    many[519].verts=v;
    many[519].cat=N2_ROAD;N2Scene dense={many,520,520};
    WGroundGrid grid={0};
    assert(world_ground_grid_build(&grid,&dense,bb));
    world_ground_grid_activate(&grid);
    close_to(sweep(&dense,a,b,&h),.5f);assert(h.mesh==519);
    world_ground_grid_activate(NULL);world_ground_grid_free(&grid);
    close_to(sweep(&dense,a,b,&h),.5f);assert(h.mesh==519);
#ifndef GROUND_MOTION_BASELINE
    /* Full production movement limiter: the leading axle meets z=x at
     * body x=-.75. An unchecked four-metre move ends at x=2, inside ground.
     * Only into-slope velocity may be removed, never the tangent component. */
    for(int i=0;i<4;i++)v[i*5+2]=v[i*5];
    PhysRideState r={0};PhysRideSupport sup={0};
    for(int i=0;i<4;i++){sup.ax[i]=i<2?1:-1;sup.ay[i]=(i&1)?-.5f:.5f;}
    float old[]={-2,0,0},pos[]={2,1,0},vel[]={4,1},heading=0;
    float mf=ground_motion_limit(&s,&r,&sup,old,0,pos,&heading,vel,&h);
    printf("body movement: fraction %.6f, x %.6f (expected <= -.75)\n",mf,pos[0]);fflush(stdout);
    assert(mf<.313f && pos[0]<=-.75f && pos[0]>-.76f);
    close_to(pos[2],0);close_to(r.z,0);close_to(vel[0],0);close_to(vel[1],1);

    /* Turning wheel endpoints follow an arc, not the chord used to locate a
     * candidate. Reconstructing the accepted yaw must not cross that face. */
    float oh=-atanf(.5f)-.015f, nh=-atanf(.5f)+.005f;
    float x0=cosf(oh)-.5f*sinf(oh),x1=cosf(nh)-.5f*sinf(nh);
    float edge=(x0+x1)*.5f;
    for(int i=0;i<4;i++)v[i*5+2]=10*(v[i*5]-edge)+PHYS_RIDE_REACH_UP;
    old[0]=old[1]=pos[0]=pos[1]=0;heading=nh;vel[0]=vel[1]=0;
    ground_motion_limit(&s,&r,&sup,old,oh,pos,&heading,vel,&h);
    float actual=cosf(heading)-.5f*sinf(heading);
    printf("turning contact: wheel %.9f face %.9f\n",actual,edge);fflush(stdout);
    assert(actual<=edge);
#endif
    puts("ground_motion_test: PASS");return 0;
}
