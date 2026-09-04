/* Ground crossing guard for the existing XY + sprung-ride split.
 * No invented support or Z correction: stop the XY/yaw move before a wheel's
 * reachable envelope crosses a solid face, then let suspension absorb it. */
#ifndef OPENUG2_GROUND_MOTION_H
#define OPENUG2_GROUND_MOTION_H
#include <math.h>
#include "physics.h"
#include "world.h"

static float ground_motion_limit(const N2Scene *sc, const PhysRideState *ride,
        const PhysRideSupport *sup, const float old[3], float oldh,
        float pos[3], float *heading, float vel[2], WGroundHit *contact) {
    float f=1, dh=*heading-oldh;
    float dx=pos[0]-old[0],dy=pos[1]-old[1];
    while(dh>3.14159265f)dh-=6.2831853f;
    while(dh<-3.14159265f)dh+=6.2831853f;
    float co=cosf(oldh),so=sinf(oldh);
    WGroundHit best={0};
    /* Recheck reconstructed wheel endpoints after truncating yaw: a wheel's
     * arc is not its chord. Bounded retries fail closed to the old pose. This
     * protects the sampled pose, not a full rigid-body angular CCD volume. */
    for(int pass=0;pass<8;pass++) {
      float cn=cosf(oldh+dh*f),sn=sinf(oldh+dh*f),qmin=1,longest=0;
      for(int k=0;k<4;k++) {
        float z=phys_ride_wheel_z(ride,sup,k)+PHYS_RIDE_REACH_UP;
        float a[]={old[0]+co*sup->ax[k]-so*sup->ay[k],
                   old[1]+so*sup->ax[k]+co*sup->ay[k],z};
        float b[]={old[0]+dx*f+cn*sup->ax[k]-sn*sup->ay[k],
                   old[1]+dy*f+sn*sup->ax[k]+cn*sup->ay[k],z};
        float len=hypotf(b[0]-a[0],b[1]-a[1]);if(len>longest)longest=len;
        WGroundHit h;float q=world_ground_sweep(sc,a,b,&h);
        if(q<qmin){qmin=q;best=h;}
      }
      if(qmin>=1)break;
      if(longest>0)qmin=fmaxf(0,qmin-.002f/longest);
      f*=qmin;
      if(pass==7)f=0;
    }
    if(f>=1)return 1;
    pos[0]=old[0]+dx*f;
    pos[1]=old[1]+dy*f;
    *heading=oldh+dh*f;
    float nx=best.normal[0],ny=best.normal[1],nn=nx*nx+ny*ny;
    if(nn>1e-9f) {
        float vn=vel[0]*nx+vel[1]*ny;
        if(vn<0){vel[0]-=nx*vn/nn;vel[1]-=ny*vn/nn;}
    }
    if(contact)*contact=best;
    return f;
}
#endif
