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

int main(void) {
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
