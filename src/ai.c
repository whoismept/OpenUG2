/* ai.c — OpenUG2 AI module implementation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "physics.h"   /* PHYS_MAXSPD paces the AI against the player's cap */
#include "world.h"     /* grid-accelerated ground query */

int load_circuit(const char *dataroot, const char *circuit, N2Scene *scene,
                 N2Path *aipath, AiCar *ais, float spawn[3],
                 float *heading0, int *start_idx, float cx, float cy) {
    static const float AICOL[N_AI][3] = {
        {0.15f,0.4f,0.95f}, {0.2f,0.8f,0.35f}, {0.95f,0.8f,0.15f}, {0.85f,0.2f,0.8f} };
    free(aipath->xy); aipath->xy = NULL; aipath->n = 0;
    char pathp[1024];
    snprintf(pathp, sizeof pathp, "%s/TRACKS/%s", dataroot, circuit);
    long plen; unsigned char *pdata = n2_read_file(pathp, &plen);
    if (!pdata) return 0;
    int ok = n2_load_path(pdata, plen, aipath) > 4;
    free(pdata);
    if (!ok) { free(aipath->xy); aipath->xy = NULL; aipath->n = 0; return 0; }
    /* Spawn the PLAYER at the densest built-up spot (cx,cy passed in) so the
       opening view frames the city — even if that's off the racing line (the
       lap logic tracks the nearest waypoint, so the race still works). The AI
       grid on the line, nearest that spot. */
    int best = 0; float bestd = 1e30f;
    for (int i = 0; i < aipath->n; i++) {
        float dx = aipath->xy[i*2]-cx, dy = aipath->xy[i*2+1]-cy, dd = dx*dx+dy*dy;
        if (dd < bestd) { bestd = dd; best = i; }
    }
    *start_idx = best;
    spawn[0]=cx; spawn[1]=cy;
    spawn[2]=world_ground_z(scene, cx, cy, spawn[2]);
    float dwx = aipath->xy[best*2]-cx, dwy = aipath->xy[best*2+1]-cy;
    if (dwx*dwx+dwy*dwy < 9.0f) {                 /* already on the line: face along it */
        int nx = (best+1) % aipath->n;
        *heading0 = atan2f(aipath->xy[nx*2+1]-cy, aipath->xy[nx*2]-cx);
    } else {                                       /* face toward the racing line */
        *heading0 = atan2f(dwy, dwx);
    }
    for (int k = 0; k < N_AI; k++) {
        int t = (*start_idx + 2 + k*2) % aipath->n;
        ais[k].t = t; ais[k].lap = 0; ais[k].prevrel = t;
        ais[k].pos[0]=aipath->xy[t*2]; ais[k].pos[1]=aipath->xy[t*2+1];
        ais[k].pos[2]=spawn[2]; ais[k].head=*heading0;
        ais[k].spd = PHYS_MAXSPD*(0.66f + k*0.027f);
        memcpy(ais[k].col, AICOL[k], sizeof AICOL[k]);
    }
    return N_AI;
}

/* steering lock per tick — AI respects the same turn-rate constraint idea as
 * the player (a real car can't snap its heading) */
#define AI_STEER_LOCK 0.06f

void ai_step(AiCar *ai, int k, const N2Path *aipath, N2Scene *scene,
             int start_idx, int player_prog) {
    float ax=aipath->xy[ai->t*2]-ai->pos[0], ay=aipath->xy[ai->t*2+1]-ai->pos[1];
    if (ax*ax+ay*ay < 36.0f) ai->t = (ai->t+1) % aipath->n;
    float da = atan2f(ay, ax) - ai->head;
    while (da >  3.14159f) da -= 6.28318f;
    while (da < -3.14159f) da += 6.28318f;
    if (da >  AI_STEER_LOCK) da =  AI_STEER_LOCK;
    if (da < -AI_STEER_LOCK) da = -AI_STEER_LOCK;
    ai->head += da;
    /* pace: ease off for the bend ahead (angle between the approach and
       the next segment) and rubber-band mildly toward the player. */
    int nx = (ai->t+1) % aipath->n;
    float ox=aipath->xy[nx*2]-aipath->xy[ai->t*2], oy=aipath->xy[nx*2+1]-aipath->xy[ai->t*2+1];
    float li=sqrtf(ax*ax+ay*ay), lo=sqrtf(ox*ox+oy*oy);
    float cosang = (li>1e-3f && lo>1e-3f) ? (ax*ox+ay*oy)/(li*lo) : 1.0f;
    float corner = 0.4f + 0.6f*(cosang>0?cosang:0);   /* 1 straight, 0.4 sharp */
    int aiprog = ai->lap*aipath->n + ai->prevrel;
    float gap = (float)(player_prog-aiprog)/(aipath->n*0.4f);
    if (gap>1) gap=1; if (gap<-1) gap=-1;             /* + = AI behind -> faster */
    float target = PHYS_MAXSPD*(0.68f + k*0.015f) * corner * (1.0f + 0.18f*gap);
    ai->spd += (target - ai->spd) * 0.06f;
    if (ai->spd < PHYS_MAXSPD*0.11f) ai->spd = PHYS_MAXSPD*0.11f;
    ai->pos[0] += cosf(ai->head)*ai->spd;
    ai->pos[1] += sinf(ai->head)*ai->spd;
    float agz=ai->pos[2];
    if (world_ground_at(scene,ai->pos[0],ai->pos[1],ai->pos[2],&agz)!=WSURF_NONE)
        ai->pos[2]=agz;  /* same stable contact rule as the player */
    /* lap: count when loop-progress wraps past the start/finish */
    int rel = (n2_nearest_wp(aipath, ai->pos[0], ai->pos[1]) - start_idx
               + aipath->n) % aipath->n;
    if (ai->prevrel > aipath->n*3/4 && rel < aipath->n/4) ai->lap++;
    ai->prevrel = rel;
}

static float ai_segment(const N2Path *p, int i) {
    return hypotf(p->xy[2*i+2]-p->xy[2*i], p->xy[2*i+3]-p->xy[2*i+1]);
}

int ai_drive_route_valid(const N2Path *p) {
    if (!p || !p->xy || p->n < 2 || p->n > 4096) return 0;
    for (int i=0; i<p->n*2; i++)
        if (!isfinite(p->xy[i]) || fabsf(p->xy[i]) >= 1e6f) return 0;
    for (int i=0; i<p->n-1; i++) {
        float len=ai_segment(p,i);
        if (len < 0.01f || len > 120.0f) return 0;
    }
    /* ponytail: only open, unsegmented XY chains. Decode topology/elevation
     * before accepting circuits, raw-list jumps or general city routing. */
    return hypotf(p->xy[0]-p->xy[2*p->n-2],p->xy[1]-p->xy[2*p->n-1]) > 12.0f;
}

static float ai_projection(const N2Path *p, int i, const float pos[3], float *error) {
    float dx=p->xy[2*i+2]-p->xy[2*i], dy=p->xy[2*i+3]-p->xy[2*i+1];
    float t=((pos[0]-p->xy[2*i])*dx+(pos[1]-p->xy[2*i+1])*dy)/(dx*dx+dy*dy);
    t=fmaxf(0,fminf(1,t));
    *error=hypotf(pos[0]-p->xy[2*i]-t*dx,pos[1]-p->xy[2*i+1]-t*dy);
    return t;
}

int ai_drive_init(AiDrive *d, const N2Path *p, const float pos[3], float heading) {
    if (!d) return 0;
    memset(d,0,sizeof *d); d->failed=1;
    if (!ai_drive_route_valid(p) || !pos || !isfinite(pos[0]) ||
        !isfinite(pos[1]) || !isfinite(heading)) return 0;
    float best=1e30f, along=0, at=0;
    for (int i=0; i<p->n-1; i++) {
        float err, t=ai_projection(p,i,pos,&err), len=ai_segment(p,i);
        if (err < best) { best=err; d->segment=i; at=along+t*len; }
        along+=len;
    }
    int i=d->segment;
    float dot=((p->xy[2*i+2]-p->xy[2*i])*cosf(heading)+
               (p->xy[2*i+3]-p->xy[2*i+1])*sinf(heading))/ai_segment(p,i);
    if (best > 8.0f || fabsf(dot) < 0.5f) return 0;
    d->path=p; d->direction=dot>0 ? 1 : -1; d->length=along;
    d->start=d->progress=d->checkpoint=dot>0 ? at : along-at;
    d->error=best; d->failed=0;
    return 1;
}

AiDriveInput ai_drive_step(AiDrive *d, const float pos[3], float heading, float speed) {
    AiDriveInput out={0,0,1};
    if (!d || d->failed || d->finished) return out;
    if (!d->path || !pos || !isfinite(pos[0]) || !isfinite(pos[1]) ||
        !isfinite(heading) || !isfinite(speed)) { d->failed=1; return out; }
    const N2Path *p=d->path;
    int i=d->segment;
    float err, t=ai_projection(p,i,pos,&err);
    int next=i+d->direction;
    if (next>=0 && next<p->n-1) {
        float ne, nt=ai_projection(p,next,pos,&ne);
        if (ne < err) { i=next; t=nt; err=ne; }
    }
    d->segment=i; d->error=err;
    if (err>12.0f) { d->failed=1; return out; }
    float at=0;
    for (int k=0; k<i; k++) at+=ai_segment(p,k);
    at+=t*ai_segment(p,i);
    if (d->direction<0) at=d->length-at;
    if (at>d->progress) d->progress=at;
    if (d->progress>d->checkpoint+0.5f && fabsf(PHYS_KMH(speed))>=3.0f) {
        d->checkpoint=d->progress; d->stalled=0;
    }
    else if (++d->stalled>300) { d->failed=1; return out; }
    float remaining=d->length-at;
    if (remaining<2.0f && err<3.0f && fabsf(PHYS_KMH(speed))<2.0f) {
        d->finished=1; return out;
    }
    /* Short arc-length preview, not a leap to a distant node across a bend. */
    float look=fminf(10.0f,4.0f+fabsf(speed)*PHYS_TICKRATE*0.35f);
    float len=ai_segment(p,i), part=d->direction>0 ? 1-t : t;
    float advance=look;
    while (advance>part*len) {
        advance-=part*len;
        next=i+d->direction;
        if (next<0 || next>=p->n-1) { advance=part*len; break; }
        i=next; len=ai_segment(p,i); part=1; t=d->direction>0 ? 0 : 1;
    }
    t+=d->direction*advance/len;
    d->target[0]=p->xy[2*i]+t*(p->xy[2*i+2]-p->xy[2*i]);
    d->target[1]=p->xy[2*i+1]+t*(p->xy[2*i+3]-p->xy[2*i+1]);
    float angle=atan2f(d->target[1]-pos[1],d->target[0]-pos[0])-heading;
    angle=atan2f(sinf(angle),cosf(angle));
    out.steer=fmaxf(-1,fminf(1,angle/0.5f));
    /* Conservative test pace; brake before the endpoint, never command reverse
     * as a substitute for stopping. No handling/suspension parameters change. */
    float target=50.0f/3.6f;
    target=fminf(target,sqrtf(2*3.0f*fmaxf(0,remaining-1.0f)));
    target=fminf(target,(14.0f+36.0f*fmaxf(0,
                        cosf(fminf(fabsf(angle)*2,1.570796327f))))/3.6f);
    d->target_kmh=target*3.6f;
    float delta=target-speed*PHYS_TICKRATE;
    out.throttle=delta < -0.5f && speed>0.01f ? -1.0f : fmaxf(0,fminf(1,delta*0.5f));
    out.handbrake=target<0.1f;
    return out;
}
