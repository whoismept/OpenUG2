/* Synthetic geometry only. A moving contact envelope must not cross a solid
 * ground face between samples. Compile with GROUND_MOTION_BASELINE to reproduce
 * the old point-query-only movement (no sweep) before the M155 fix. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "world.h"
#include "physics.h"

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

int main(void) {
    /* A road closure is a finite segment, not a nine-metre-deep slab.
     * Entering beside its far side must not teleport a car under a slope. */
    World barrier={0};barrier.city.mode=MODE_RACE_EVENT;barrier.city.nbar=1;
    barrier.city.bar[0].dx=1;
    float beside[]={8,9.5f,-.4f};
    int pushed=world_barrier_push(&barrier,beside,1.3f);
    printf("closure side approach: hit=%d x=%.3f (expected no hit, x=8)\n",pushed,beside[0]);fflush(stdout);
    assert(!pushed);close_to(beside[0],8);close_to(beside[2],-.4f);
    float corner[]={1.2f,10.2f,0};assert(!world_barrier_push(&barrier,corner,1.3f));
    for(int side=-1;side<=1;side+=2) {
        float face[]={side*.5f,0,2};assert(world_barrier_push(&barrier,face,1.3f));
        close_to(face[0],side*1.3f);close_to(face[1],0);close_to(face[2],2);
    }
    float end[]={-.5f,9.6f,0};assert(world_barrier_push(&barrier,end,1.3f));
    close_to(hypotf(end[0],end[1]-9),1.3f);
    barrier.city.bar[0].dx=0;barrier.city.bar[0].dy=-1;
    float turned[]={9.5f,-8,0};assert(!world_barrier_push(&barrier,turned,1.3f));
    turned[0]=0;turned[1]=.5f;assert(world_barrier_push(&barrier,turned,1.3f));
    close_to(turned[1],1.3f);
    barrier.city.mode=MODE_FREEROAM;turned[1]=0;
    assert(!world_barrier_push(&barrier,turned,1.3f));
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
