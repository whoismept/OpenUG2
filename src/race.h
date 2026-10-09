#ifndef OPENUG2_RACE_H
#define OPENUG2_RACE_H
#include <stdint.h>
#include <math.h>
enum { N2_RACE_UNKNOWN, N2_RACE_CIRCUIT, N2_RACE_SPRINT, N2_RACE_DRAG,
       N2_RACE_DRIFT, N2_RACE_STREETX, N2_RACE_URL };
typedef struct { char name[64]; uint32_t flags; int length, kind, downhill; } N2EventInfo;
static const char *n2_race_name(int kind) {
    static const char *names[]={"Unknown", "Circuit", "Sprint", "Drag", "Drift", "Street X", "URL"};
    return kind>=0 && kind<=N2_RACE_URL ? names[kind] : names[0];
}
static int race_has_traffic(int kind,int downhill) {
    return kind==N2_RACE_CIRCUIT || kind==N2_RACE_SPRINT || kind==N2_RACE_DRAG ||
           (kind==N2_RACE_DRIFT && downhill);
}
static int race_drag_failure(int blown,float wall,float vehicle) {
    return blown?1:wall>8 || vehicle>0?2:0;
}
typedef struct { float bank,chain,quiet,angle; } RaceDrift;
/* Measured slip and travelled distance, not steering animation. The scoring
   scale/thresholds are provisional tuning; the retail equation is undecoded. */
static void race_drift_step(RaceDrift *d,const float velocity[2],float heading,
                            float metres,int grounded,int collision,float dt) {
    if(!d || !velocity || !isfinite(heading) || !isfinite(metres) ||
       !isfinite(velocity[0]) || !isfinite(velocity[1]) || dt<=0 || !isfinite(dt))return;
    float forward=velocity[0]*cosf(heading)+velocity[1]*sinf(heading);
    float sideways=-velocity[0]*sinf(heading)+velocity[1]*cosf(heading);
    float angle=fabsf(atan2f(sideways,forward));d->angle=angle*57.2957795f;
    if(collision){d->chain=0;d->quiet=0;return;}
    if(grounded && forward>0 && hypotf(velocity[0],velocity[1])*216>35 &&
       angle>.174532925f && angle<1.3962634f && metres>0) {
        d->chain=fminf(1e9f,d->chain+metres*sinf(angle)*100);d->quiet=0;
    } else if((d->quiet+=dt)>=1) {d->bank=fminf(1e9f,d->bank+d->chain);d->chain=0;}
}
static void race_drift_bank(RaceDrift *d) {
    d->bank=fminf(1e9f,d->bank+d->chain);d->chain=0;d->quiet=0;
}
#endif
