/* ai.c — OpenUG2 AI module implementation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#ifdef _WIN32
#include "dir_scan.h"
#endif

#include "ai.h"
#include "physics.h"   /* PHYS_MAXSPD paces the AI against the player's cap */
#include "world.h"     /* grid-accelerated ground query */
#include "ground_motion.h"

AiPerf g_ai_perf;
AiAuditHook g_ai_audit_hook;
AiContactHook g_ai_contact_hook;

void ai_render_pose(AiCar *out,const AiCar *before,const AiCar *after,float alpha) {
    AiCar pose=*after;
    float t=fmaxf(0,fminf(1,alpha));
    if(t==1){*out=pose;return;} /* deterministic captures keep the exact tick */
    for(int k=0;k<3;k++)pose.pos[k]=before->pos[k]+t*(after->pos[k]-before->pos[k]);
    for(int k=0;k<2;k++)pose.vel[k]=before->vel[k]+t*(after->vel[k]-before->vel[k]);
    pose.head=before->head+t*atan2f(sinf(after->head-before->head),cosf(after->head-before->head));
    pose.wheel_angle=before->wheel_angle+t*(after->wheel_angle-before->wheel_angle);
    pose.steer=before->steer+t*(after->steer-before->steer);
    for(int k=0;k<2;k++)pose.wheel_steer[k]=before->wheel_steer[k]+t*(after->wheel_steer[k]-before->wheel_steer[k]);
    pose.ride.z=before->ride.z+t*(after->ride.z-before->ride.z);
    pose.ride.pitch=before->ride.pitch+t*(after->ride.pitch-before->ride.pitch);
    pose.ride.roll=before->ride.roll+t*(after->ride.roll-before->ride.roll);
    pose.ride.body_roll=before->ride.body_roll+t*(after->ride.body_roll-before->ride.body_roll);
    for(int k=0;k<4;k++)pose.ride.compression[k]=before->ride.compression[k]+t*(after->ride.compression[k]-before->ride.compression[k]);
    *out=pose;
}

static void ai_motion_init(AiCar *car,const AiTrafficWorld *world) {
    car->vel[0]=cosf(car->head)*car->spd;car->vel[1]=sinf(car->head)*car->spd;
    car->steer=0;
    if(car->support.ax[0]-car->support.ax[2]<0.1f) {
        for(int k=0;k<4;k++) {
            car->support.ax[k]=k<2?1.3f:-1.3f;
            car->support.ay[k]=(k&1)?-.8f:.8f;
        }
    }
    if(world && world->scene) {
        world_ride_gather(world->scene,car->pos,car->head,car->vel,car->head,
                          NULL,&car->support,NULL,NULL,NULL);
    } else for(int k=0;k<4;k++) {
        car->support.z[k]=car->pos[2];car->support.valid[k]=1;car->support.vz[k]=0;
    }
    phys_ride_init(&car->ride,&car->support);
    car->pos[2]=car->ride.z;
    for(int k=0;k<4;k++)car->ride.vz+=car->support.vz[k]*.25f;
    car->ride_ready=1;
}

/* Only forces/collision move the live car. A route supplies driver inputs. */
static int ai_vehicle_step(AiCar *car,float throttle,float steer,int handbrake,
                             const AiTrafficWorld *world) {
    int collision=0;
    if(!car->ride_ready)ai_motion_init(car,world);
    float old[3]={car->pos[0],car->pos[1],car->pos[2]},oldh=car->head;
    float vx=car->vel[0],vy=car->vel[1];
    N2Scene *scene=world?world->scene:NULL;
    float gz=car->pos[2];
    int cat=scene?world_ground_at(scene,old[0],old[1],old[2],&gz):WSURF_ROAD;
    const PhysSurface *surface=cat==WSURF_TERRAIN?&PHYS_SURF_TERRAIN:&PHYS_SURF_ROAD;
    const PhysVehicle *vehicle=car->vehicle.accel>0?&car->vehicle:NULL;
    car->steer=phys_steer_response(car->steer,steer);
    car->braking=throttle<0 || handbrake;
    phys_drive_step(car->pos,car->vel,&car->head,&car->spd,throttle,car->steer,
                    handbrake,surface,vehicle,&car->ride);
    float ax=(car->vel[0]-vx)*PHYS_TICKRATE*PHYS_TICKRATE;
    float ay=(car->vel[1]-vy)*PHYS_TICKRATE*PHYS_TICKRATE;
    if(scene) {
        ground_motion_limit(scene,&car->ride,&car->support,old,oldh,
                            car->pos,&car->head,car->vel,NULL);
        float hl=car->half_length>1?car->half_length:2.2f;
        float hw=car->half_width>.5f?car->half_width:1.0f;
        float height=car->height>1?car->height:1.6f;
        float bb[6]={-hl,-hw,0,hl,hw,height};
        float contact_before[3];memcpy(contact_before,car->pos,sizeof contact_before);
        float wall_before[2]={car->vel[0],car->vel[1]};
        if(world->obst && world->obstsrc)g_ai_perf.wall_candidates+=world->nobst;
        float dz=!car->ride.contact_mask
                 ?(car->ride.vz-PHYS_RIDE_G/PHYS_TICKRATE)/PHYS_TICKRATE:0;
        int rails=0;
        collision=world_body_walls_move(scene,old,car->pos,car->vel,car->head,bb,
            car->pos[2]+.05f,car->pos[2]+height,dz,world->obst,world->obstz,
            world->nobst,world->obstsrc,NULL,0,NULL,&rails);
        collision|=rails;
        phys_ride_wall_response(&car->ride,wall_before,car->vel);
        if(g_ai_contact_hook)g_ai_contact_hook(contact_before,car->pos,0);
        world_ride_gather(scene,car->pos,car->head,car->vel,oldh,&car->ride,
                          &car->support,NULL,NULL,NULL);
    }
    float co=cosf(car->head),sn=sinf(car->head);
    car->spd=car->vel[0]*co+car->vel[1]*sn;
    phys_ride_lean(&car->ride,vehicle,-ax*sn+ay*co,1.0f/60.0f);
    phys_ride_step(&car->ride,&car->support,1.0f/60.0f);
    car->pos[2]=car->ride.z;
    car->turn_rate=atan2f(sinf(car->head-oldh),cosf(car->head-oldh));
    return collision!=0;
}

enum { RACE_PLAN_TICKS=15 };
#define RACE_BRAKE_SHARE .50f /* braking planned by racers, of the car's full brake */
static float ai_throttle(float target,float speed) {
    float delta=(target-speed)*PHYS_TICKRATE;
    if(delta<0 && speed>0.002f)return fmaxf(-1.0f,delta*.8f);
    return fmaxf(0.0f,fminf(1.0f,delta*.5f));
}

static void ai_contact_bounds(const AiCar *car,float bb[6]) {
    float hl=car->half_length>1?car->half_length:2.2f;
    float hw=car->half_width>.5f?car->half_width:1.0f;
    float height=car->height>1?car->height:1.6f;
    bb[0]=-hl;bb[1]=-hw;bb[2]=0;bb[3]=hl;bb[4]=hw;bb[5]=height;
}

static void ai_contact_world(AiCar *car,const float old[3],const AiTrafficWorld *world) {
    if(!world || !world->scene)return;
    float bb[6];ai_contact_bounds(car,bb);
    float dx=car->pos[0]-old[0],dy=car->pos[1]-old[1];
    int steps=1+(int)ceilf(hypotf(dx,dy)/.25f);
    car->pos[0]=old[0];car->pos[1]=old[1];
    /* Short correction steps cannot skip across a thin wall to its far side.
       This constrains penetration correction, not the normal driving tick. */
    for(int i=0;i<steps;i++) {
        float before[3]={car->pos[0],car->pos[1],car->pos[2]},head=car->head;
        car->pos[0]+=dx/steps;car->pos[1]+=dy/steps;
        if(car->ride_ready)
            ground_motion_limit(world->scene,&car->ride,&car->support,before,head,
                                car->pos,&head,car->vel,NULL);
        float contact_before[3];memcpy(contact_before,car->pos,sizeof contact_before);
        g_ai_perf.world_fixes++;
        if(world->obst && world->obstsrc) g_ai_perf.wall_candidates+=world->nobst;
        if(world->obst && world->obstsrc)
            collide_body_walls(car->pos,car->vel,car->head,bb,world->obst,world->obstz,
                world->nobst,car->pos[2]+.05f,car->pos[2]+bb[5],
                world->scene,world->obstsrc,NULL,0);
        world_body_wall_push(world->scene,car->pos,car->vel,car->head,bb,
                             car->pos[2]+.05f,car->pos[2]+bb[5],NULL);
        if(g_ai_contact_hook)g_ai_contact_hook(contact_before,car->pos,0);
    }
}

static float ai_car_contact(AiCar *a,AiCar *b,const AiTrafficWorld *world) {
    float bb[6];ai_contact_bounds(a,bb);
    float thud=0;
    /* Recheck this pair after the world blocks a separation correction. */
    for(int pass=0;pass<8;pass++) {
        float olda[3]={a->pos[0],a->pos[1],a->pos[2]};
        float oldb[3]={b->pos[0],b->pos[1],b->pos[2]};
        float hit=phys_car_contacts(a->pos,a->vel,a->spd,a->head,bb,a->mass,b,1);
        g_ai_perf.pairs++;
        if(!hit)break;
        g_ai_perf.hits++;
        if(g_ai_contact_hook) {
            g_ai_contact_hook(olda,a->pos,1);g_ai_contact_hook(oldb,b->pos,1);
        }
        thud=fmaxf(thud,hit);
        float targeta[3]={a->pos[0],a->pos[1],a->pos[2]};
        float targetb[3]={b->pos[0],b->pos[1],b->pos[2]};
        float nx=targetb[0]-oldb[0],ny=targetb[1]-oldb[1],len=hypotf(nx,ny);
        ai_contact_world(a,olda,world);ai_contact_world(b,oldb,world);
        if(len>1e-6f) {
            nx/=len;ny/=len;
            float blocked_a=fmaxf(0,(a->pos[0]-targeta[0])*nx+(a->pos[1]-targeta[1])*ny);
            float blocked_b=fmaxf(0,(targetb[0]-b->pos[0])*nx+(targetb[1]-b->pos[1])*ny);
            /* A wall-backed car cannot accept its share of the correction.
               Give the remainder to the other body, independent of mass. */
            memcpy(olda,a->pos,sizeof olda);memcpy(oldb,b->pos,sizeof oldb);
            a->pos[0]-=blocked_b*nx;a->pos[1]-=blocked_b*ny;
            b->pos[0]+=blocked_a*nx;b->pos[1]+=blocked_a*ny;
            if(g_ai_contact_hook) {
                g_ai_contact_hook(olda,a->pos,1);g_ai_contact_hook(oldb,b->pos,1);
            }
            float closing=(a->vel[0]-b->vel[0])*nx+(a->vel[1]-b->vel[1])*ny;
            if(closing>0) {
                if(blocked_b>.00001f){a->vel[0]-=closing*nx;a->vel[1]-=closing*ny;}
                else if(blocked_a>.00001f){b->vel[0]+=closing*nx;b->vel[1]+=closing*ny;}
            }
            if(blocked_b>0)ai_contact_world(a,olda,world);
            if(blocked_a>0)ai_contact_world(b,oldb,world);
        }
    }
    if(!thud)return 0;
    a->spd=a->vel[0]*cosf(a->head)+a->vel[1]*sinf(a->head);
    b->spd=b->vel[0]*cosf(b->head)+b->vel[1]*sinf(b->head);
    return thud;
}

float ai_car_contacts(AiCar *const cars[],int count,const AiTrafficWorld *world,
                       const AiCar *player) {
    float thud=0;int touched=0;
    /* ponytail: bounded all-pairs relaxation for the local vehicle pool. Use
       contact islands if traffic grows; impossible enclosures stay bounded. */
    int passes=8*count*count; /* longer queues need more separation sweeps */
    if(passes<256)passes=256;
    for(int pass=0;pass<passes;pass++) {
        int contacts=0;g_ai_perf.passes++;
        for(int i=0;i<count;i++)for(int j=i+1;j<count;j++) {
            float hit=ai_car_contact(cars[i],cars[j],world);
            if(hit) {
                contacts=1;touched=1;
                if(cars[i]==player || cars[j]==player)thud=fmaxf(thud,hit);
            }
        }
        if(!contacts)break;
    }
    if(!touched)return 0;
    for(int k=0;k<count;k++) {
        AiCar *car=cars[k];
        car->spd=car->vel[0]*cosf(car->head)+car->vel[1]*sinf(car->head);
        if(!world || !world->scene || !car->ride_ready)continue;
        world_ride_gather(world->scene,car->pos,car->head,car->vel,car->head,
                         &car->ride,&car->support,NULL,NULL,NULL);
        /* Refresh the displaced wheel footprint without a second gravity or
           spring tick. Newly reached support is resolved on the next tick. */
        for(int w=0;w<4;w++) {
            float c=car->support.z[w]-phys_ride_wheel_z(&car->ride,&car->support,w);
            if(!car->support.valid[w]) {
                car->ride.contact_mask&=~(1u<<w);c=-PHYS_RIDE_DROOP;
            }
            car->ride.compression[w]=fmaxf(-PHYS_RIDE_DROOP,fminf(PHYS_RIDE_BUMP,c));
        }
    }
    return thud;
}

static int ai_load_loop(const char *dataroot, const char *circuit, N2Path *aipath) {
    free(aipath->xy); aipath->xy = NULL; aipath->n = 0;
    char pathp[1024];
    snprintf(pathp, sizeof pathp, "%s/TRACKS/%s", dataroot, circuit);
    long plen; unsigned char *pdata = n2_read_file(pathp, &plen);
    if (!pdata) return 0;
    int ok = n2_load_path(pdata, plen, aipath) > 4;
    free(pdata);
    if (!ok) { free(aipath->xy); aipath->xy = NULL; aipath->n = 0; }
    return ok;
}

static int ai_nearest(const N2Path *path, float x, float y) {
    int best = 0; float bestd = 1e30f;
    for (int i = 0; i < path->n; i++) {
        float dx=path->xy[i*2]-x,dy=path->xy[i*2+1]-y,dd=dx*dx+dy*dy;
        if (dd < bestd) { bestd=dd; best=i; }
    }
    return best;
}

static void ai_grid(const N2Path *path,N2Scene *scene,AiCar *ais,int start,float z) {
    static const float AICOL[N_AI][3] = {
        {0.15f,0.4f,0.95f}, {0.2f,0.8f,0.35f}, {0.95f,0.8f,0.15f}, {0.85f,0.2f,0.8f} };
    for (int k=0;k<N_AI;k++) {
        int t=(start+2+k*2)%path->n,nx=(t+1)%path->n;
        ais[k].t=t;ais[k].lap=0;ais[k].prevrel=(t-start+path->n)%path->n;
        ais[k].braking=0;ais[k].wheel_angle=ais[k].turn_rate=0.0f;
        ais[k].ride_ready=0;
        ais[k].pos[0]=path->xy[t*2];ais[k].pos[1]=path->xy[t*2+1];
        ais[k].pos[2]=world_ground_z(scene,ais[k].pos[0],ais[k].pos[1],z);
        ais[k].head=atan2f(path->xy[nx*2+1]-ais[k].pos[1],
                           path->xy[nx*2]-ais[k].pos[0]);
        ais[k].spd=PHYS_MAXSPD*(0.66f+k*0.027f);
        memcpy(ais[k].col,AICOL[k],sizeof AICOL[k]);
    }
}

int load_circuit(const char *dataroot, const char *circuit, N2Scene *scene,
                 N2Path *aipath, AiCar *ais, float spawn[3],
                 float *heading0, int *start_idx, float cx, float cy) {
    if (!ai_load_loop(dataroot,circuit,aipath)) return 0;
    /* Spawn the PLAYER at the densest built-up spot (cx,cy passed in) so the
       opening view frames the city — even if that's off the racing line (the
       lap logic tracks the nearest waypoint, so the race still works). The AI
       grid on the line, nearest that spot. */
    int best=ai_nearest(aipath,cx,cy);
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
    ai_grid(aipath,scene,ais,*start_idx,spawn[2]);
    return N_AI;
}

int load_roaming_circuit(const char *dataroot,const char *circuit,N2Scene *scene,
                         N2Path *aipath,AiCar *ais,const float player[3],
                         int *start_idx) {
    if (!player || !ai_load_loop(dataroot,circuit,aipath)) return 0;
    *start_idx=ai_nearest(aipath,player[0],player[1]);
    ai_grid(aipath,scene,ais,*start_idx,player[2]);
    return N_AI;
}

void ai_step(AiCar *ai, int k, const N2Path *aipath, N2Scene *scene,
             int start_idx, int player_prog) {
    float ax=aipath->xy[ai->t*2]-ai->pos[0], ay=aipath->xy[ai->t*2+1]-ai->pos[1];
    if (ax*ax+ay*ay < 36.0f) {
        ai->t = (ai->t+1) % aipath->n;
        ax=aipath->xy[ai->t*2]-ai->pos[0];ay=aipath->xy[ai->t*2+1]-ai->pos[1];
    }
    float da = atan2f(ay, ax) - ai->head;
    while (da >  3.14159f) da -= 6.28318f;
    while (da < -3.14159f) da += 6.28318f;
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
    AiTrafficWorld world={.scene=scene};
    ai_vehicle_step(ai,ai_throttle(target,ai->spd),
                     fmaxf(-1,fminf(1,da/.5f)),0,&world);
    /* lap: count when loop-progress wraps past the start/finish */
    int rel = (n2_nearest_wp(aipath, ai->pos[0], ai->pos[1]) - start_idx
               + aipath->n) % aipath->n;
    if (ai->prevrel > aipath->n*3/4 && rel < aipath->n/4) ai->lap++;
    ai->prevrel = rel;
}

void ai_roads_free(AiRoadNet *roads) {
    if(!roads)return;
    free(roads->xy);free(roads->next);free(roads->half_width);
    free(roads->join);free(roads->join_start);free(roads->join_known);
    free(roads->pred_start);free(roads->pred_list);
    free(roads->cell_start);free(roads->cell_list);
    memset(roads,0,sizeof *roads);
}

/* Shared recursive chunk/record reader for free-roam and event lanes.
   Event callers also request the authored distance along each lane chain. */
static int ai_roads_read(AiRoadNet *roads,const unsigned char *data,long len,
                          int *capacity,float **distance,float **height,float *widest) {
    N2Leaf leaf[2];int nl=0;
    n2_find_leaves(data,0,len,0x00034121u,leaf,&nl,2);
    if(nl!=1 || leaf[0].size<8)return 0;
    long at=leaf[0].off+8,end=leaf[0].off+leaf[0].size;
    int oldn=roads->n,ok=1,cap=*capacity;
    while(at<end) {
        if(at+128>end){ok=0;break;}
        uint32_t pair=n2_u32(data+at+44);
        int count=(int)(pair&0xffffu);
        if(count<1 || count>4096 || (int)(pair>>16)!=count ||
           at+128L+56L*count>end){ok=0;break;}
        if(roads->n+count>cap) {
            int grown=cap?cap*2:4096;
            while(grown<roads->n+count)grown*=2;
            float *xy=(float *)realloc(roads->xy,(size_t)grown*2*sizeof(float));
            if(!xy){ok=0;break;}roads->xy=xy;
            int *next=(int *)realloc(roads->next,(size_t)grown*sizeof(int));
            if(!next){ok=0;break;}roads->next=next;
            float *width=(float *)realloc(roads->half_width,(size_t)grown*sizeof(float));
            if(!width){ok=0;break;}roads->half_width=width;
            if(distance) {
                float *values=realloc(*distance,(size_t)grown*sizeof *values);
                if(!values){ok=0;break;}*distance=values;
            }
            if(height) {
                float *values=realloc(*height,(size_t)grown*sizeof *values);
                if(!values){ok=0;break;}*height=values;
            }
            cap=grown;
        }
        for(int j=0;j<count;j++) {
            float x,y;
            memcpy(&x,data+at+128+56*j+4,4);
            memcpy(&y,data+at+128+56*j+8,4);
            if(!isfinite(x)||!isfinite(y)||fabsf(x)>1e5f||fabsf(y)>1e5f){ok=0;break;}
            float value=0;
            if(distance) {
                memcpy(&value,data+at+128+56*j+12,4);
                if(!isfinite(value) || fabsf(value)>1e6f){ok=0;break;}
            }
            int id=roads->n++;
            if(distance)(*distance)[id]=value;
            /* Event point Z is a signed fixed-point value in eighths of a metre. */
            if(height)(*height)[id]=(int16_t)(n2_u32(data+at+128+56*j+20)>>16)/8.0f;
            roads->xy[id*2]=x;roads->xy[id*2+1]=y;
            roads->next[id]=j+1<count?id+1:-1;
            uint32_t widths=n2_u32(data+at+128+56*j+24);
            float width=fmaxf((float)(widths&0xffffu),(float)(widths>>16))/256.0f;
            if(widest && width<=60)*widest=fmaxf(*widest,width);
            roads->half_width[id]=fminf((float)(widths&0xffffu),
                                         (float)(widths>>16))/256.0f;
        }
        if(!ok)break;
        at+=140L+56L*count;
        if(at==end+8)at=end; /* the final record omits the next-record header */
        else if(at>end){ok=0;break;}
    }
    *capacity=cap;
    if(!ok || at!=end){roads->n=oldn;return 0;}
    return roads->n>oldn;
}

static int ai_roads_joins(AiRoadNet *roads,const unsigned char *data,long len,int base) {
    N2Leaf links[2],routes[2];int nl=0,nr=0;
    N2LeafWalk walk={0x34122,links,&nl,2};
    if(!asset_chunks_walk(data,0,len,n2_leaf_chunk,&walk))return 0;
    void *known=realloc(roads->join_known,(size_t)roads->n);
    if(!known)return 0;
    roads->join_known=known;
    memset(roads->join_known+base,0,(size_t)(roads->n-base));
    if(!nl)return 1; /* Legacy paths without join metadata retain geometric joins. */
    if(nl!=1 || links[0].size<4 || (links[0].size-4)%32 || n2_u32(data+links[0].off))return 0;
    n2_find_leaves(data,0,len,0x34121,routes,&nr,2);if(nr!=1)return 0;
    int *map=malloc(2*65536*sizeof *map);if(!map)return 0;
    for(int i=0;i<65536;i++)map[2*i]=-1;
    long at=routes[0].off+8,end=routes[0].off+routes[0].size;int node=base,ok=1;
    while(at<end) {
        int id=(int)(n2_u32(data+at)>>16),count=(int)(n2_u32(data+at+44)&65535);
        if(map[2*id]>=0){ok=0;break;}
        map[2*id]=node;map[2*id+1]=count;node+=count;at+=140L+56L*count;
    }
    long count=(links[0].size-4)/32;
    if(count>INT_MAX-roads->njoin)ok=0;
    int old=roads->njoin;
    if(ok && count) {
        void *grown=realloc(roads->join,(size_t)(old+count)*sizeof *roads->join);
        if(!grown)ok=0;else roads->join=grown;
    }
    for(long j=0;ok && j<count;j++) {
        const unsigned char *p=data+links[0].off+4+32*j;
        uint32_t points=n2_u32(p+4),ids=n2_u32(p+8);
        int a=ids&65535,b=ids>>16,ia=points&65535,ib=points>>16;
        if(n2_u32(p) || n2_u32(p+20) || map[2*a]<0 || map[2*b]<0 ||
           ia>=map[2*a+1] || ib>=map[2*b+1]){ok=0;break;}
        roads->join[old+j][0]=map[2*a]+ia;roads->join[old+j][1]=map[2*b]+ib;
    }
    free(map);
    if(ok){
        roads->njoin+=count;roads->joins_present=1;
        memset(roads->join_known+base,1,(size_t)(roads->n-base));
    }
    return ok;
}

int ai_roads_load(AiRoadNet *roads,const char *troot) {
    if(!roads || !troot)return 0;
    ai_roads_free(roads);
    struct dirent **files=NULL;
#ifdef _WIN32
    int nf=n2_dir_scan(troot,&files),cap=0,nfiles=0;
#else
    int nf=scandir(troot,&files,NULL,alphasort),cap=0,nfiles=0;
#endif
    if(nf<0)return 0;
    for(int f=0;f<nf;f++) {
        if(strncmp(files[f]->d_name,"ROUTES",6))continue;
        char path[1024];
        snprintf(path,sizeof path,"%s/%s/RoutesFreeRoam.bin",troot,files[f]->d_name);
        long len=0;unsigned char *data=n2_read_file(path,&len);
        if(!data)continue;
        int oldn=roads->n;
        if(ai_roads_read(roads,data,len,&cap,NULL,NULL,NULL) && ai_roads_joins(roads,data,len,oldn))nfiles++;
        else {roads->n=oldn;fprintf(stderr,"free-roam road paths: invalid record in %s\n",path);}
        free(data);
    }
    for(int f=0;f<nf;f++)free(files[f]);free(files);
    printf("free-roam road paths: %d nodes, %d authored joins from %d RoutesFreeRoam.bin files\n",
           roads->n,roads->njoin,nfiles);
    if(roads->n>0 && !ai_roads_index(roads))printf("road query index: unavailable, linear scans\n");
    return roads->n;
}

/* ai_road_next only considers edges whose start is `at` (authored pass) or
 * whose endpoint lies within 65 m of `at` (junction pass: gap <= 5 m on an
 * edge <= 60 m). The grid cell is wider than that, so a 3x3 neighbourhood is
 * a superset; candidates are visited in ascending index like the linear scan. */
#define AI_ROAD_CELL 70.0f
static int ai_join_cmp(const void *a,const void *b) {
    const int *x=a,*y=b;
    return x[0]!=y[0]?(x[0]>y[0])-(x[0]<y[0]):(x[1]>y[1])-(x[1]<y[1]);
}
int ai_roads_index(AiRoadNet *r) {
    if(!r || r->n<=0 || !r->xy || !r->next)return 0;
    float x0=1e30f,y0=1e30f,x1=-1e30f,y1=-1e30f;
    for(int i=0;i<r->n;i++) {
        x0=fminf(x0,r->xy[i*2]);x1=fmaxf(x1,r->xy[i*2]);
        y0=fminf(y0,r->xy[i*2+1]);y1=fmaxf(y1,r->xy[i*2+1]);
    }
    int gw=1+(int)((x1-x0)/AI_ROAD_CELL),gh=1+(int)((y1-y0)/AI_ROAD_CELL);
    if(gw<=0||gh<=0||(long)gw*gh>(1L<<22))return 0;
    int *ps=calloc((size_t)r->n+1,sizeof *ps),*pl=malloc((size_t)r->n*sizeof *pl);
    int *cs=calloc((size_t)gw*gh+1,sizeof *cs),*cl=malloc((size_t)r->n*sizeof *cl);
    if(!ps||!pl||!cs||!cl){free(ps);free(pl);free(cs);free(cl);return 0;}
    for(int i=0;i<r->n;i++) {
        int nx=r->next[i];if(nx>=0 && nx<r->n)ps[nx+1]++;
        int cx=(int)((r->xy[i*2]-x0)/AI_ROAD_CELL),cy=(int)((r->xy[i*2+1]-y0)/AI_ROAD_CELL);
        cs[cy*gw+cx+1]++;
    }
    for(int i=0;i<r->n;i++)ps[i+1]+=ps[i];
    for(int c=0;c<gw*gh;c++)cs[c+1]+=cs[c];
    int *pf=malloc((size_t)r->n*sizeof *pf),*cf=malloc((size_t)gw*gh*sizeof *cf);
    if(!pf||!cf){free(pf);free(cf);free(ps);free(pl);free(cs);free(cl);return 0;}
    memcpy(pf,ps,(size_t)r->n*sizeof *pf);memcpy(cf,cs,(size_t)gw*gh*sizeof *cf);
    for(int i=0;i<r->n;i++) {   /* ascending i keeps every list sorted */
        int nx=r->next[i];if(nx>=0 && nx<r->n)pl[pf[nx]++]=i;
        int cx=(int)((r->xy[i*2]-x0)/AI_ROAD_CELL),cy=(int)((r->xy[i*2+1]-y0)/AI_ROAD_CELL);
        cl[cf[cy*gw+cx]++]=i;
    }
    free(pf);free(cf);
    free(r->pred_start);free(r->pred_list);free(r->cell_start);free(r->cell_list);
    r->pred_start=ps;r->pred_list=pl;r->cell_start=cs;r->cell_list=cl;
    r->gw=gw;r->gh=gh;r->gx0=x0;r->gy0=y0;
    free(r->join_start);r->join_start=NULL;
    if(r->joins_present) {
        r->join_start=calloc((size_t)r->n+1,sizeof *r->join_start);
        if(r->join_start) {
            if(r->njoin)qsort(r->join,(size_t)r->njoin,sizeof *r->join,ai_join_cmp);
            for(int j=0;j<r->njoin;j++)r->join_start[r->join[j][0]+1]++;
            for(int i=0;i<r->n;i++)r->join_start[i+1]+=r->join_start[i];
        }
    }
    return 1;
}

static int ai_int_cmp(const void *a,const void *b) {
    int x=*(const int *)a,y=*(const int *)b;return (x>y)-(x<y);
}
static int ai_road_has_joins(const AiRoadNet *r,int at) {
    return r->joins_present && (!r->join_known || r->join_known[at]);
}
/* Sorted candidate node indices for ai_road_next's pass, or -1 = scan all. */
static int ai_road_candidates(const AiRoadNet *r,int at,int pass,int *out,int cap) {
    if(pass==1 && ai_road_has_joins(r,at)) {
        int n=0,begin=r->join_start?r->join_start[at]:0,end=r->join_start?r->join_start[at+1]:r->njoin;
        for(int j=begin;j<end;j++)if(r->join[j][0]==at) {
            int to=r->join[j][1];
            if(hypotf(r->xy[2*to]-r->xy[2*at],r->xy[2*to+1]-r->xy[2*at+1])>=1) {
                if(n>=cap)return 0;out[n++]=to;
            } else {
                if(r->next[to]>=0){if(n>=cap)return 0;out[n++]=r->next[to];}
                int begin=r->pred_start?r->pred_start[to]:0;
                int end=r->pred_start?r->pred_start[to+1]:r->n;
                for(int p=begin;p<end;p++) {
                    int i=r->pred_start?r->pred_list[p]:p;
                    if(r->next[i]==to){if(n>=cap)return 0;out[n++]=i;}
                }
            }
        }
        qsort(out,(size_t)n,sizeof *out,ai_int_cmp);
        int u=0;for(int i=0;i<n;i++)if(!u || out[i]!=out[u-1])out[u++]=out[i];
        return u;
    }
    if(!r->pred_start || !r->cell_start)return -1;
    int n=0;
    if(pass==0) {
        out[n++]=at;
        for(int k=r->pred_start[at];k<r->pred_start[at+1];k++) {
            if(n>=cap)return -1;
            out[n++]=r->pred_list[k];
        }
    } else {
        int cx=(int)((r->xy[at*2]-r->gx0)/AI_ROAD_CELL),cy=(int)((r->xy[at*2+1]-r->gy0)/AI_ROAD_CELL);
        for(int y=cy-1;y<=cy+1;y++)for(int x=cx-1;x<=cx+1;x++) {
            if(x<0||y<0||x>=r->gw||y>=r->gh)continue;
            int c=y*r->gw+x;
            for(int k=r->cell_start[c];k<r->cell_start[c+1];k++) {
                if(n>=cap)return -1;
                out[n++]=r->cell_list[k];
            }
        }
    }
    qsort(out,(size_t)n,sizeof *out,ai_int_cmp);
    int u=0;for(int k=0;k<n;k++)if(!u||out[k]!=out[u-1])out[u++]=out[k];
    return u;
}

static int traffic_edge_supported(N2Scene *scene,float x,float y,float dx,float dy,float z,
                                  int road_only) {
    if(!scene)return 1;
    for(int q=1;q<=4;q++) {
        float nz=z;
        int surface=world_ground_at(scene,x+dx*q*0.25f,y+dy*q*0.25f,z,&nz);
        if(surface==WSURF_NONE || (road_only && surface!=WSURF_ROAD) ||
           fabsf(nz-z)>2.0f)return 0;
        z=nz;
    }
    return 1;
}


static int traffic_body_clear(const AiTrafficWorld *world,const AiCar *car,
                               float x,float y,float z,float heading) {
    if(!world || !world->scene)return 1;
    N2Scene *scene=world->scene;
    float hl=car && car->half_length>1?car->half_length:2.2f;
    float hw=car && car->half_width>0.5f?car->half_width:1.0f;
    float height=car && car->height>1?car->height:1.6f;
    float bb[6]={-hl,-hw,0,hl,hw,height};
    float pos[3]={x,y,z},vel[2]={0,0};
    if(world->obst && world->obstsrc && world->nobst &&
       collide_body_walls_preview(pos,vel,heading,bb,world->obst,world->obstz,
                          world->nobst,z+0.05f,z+height,scene,world->obstsrc))
        return 0;
    pos[0]=x;pos[1]=y;
    return !world_body_wall_push(scene,pos,vel,heading,bb,z+0.05f,z+height,NULL);
}

static int traffic_supported_corners(const AiTrafficWorld *world,const AiCar *car,
                              float x,float y,float z,float heading,int terrain) {
    if(!world || !world->scene)return 4;
    N2Scene *scene=world->scene;
    float hl=car->half_length>1?car->half_length:2.2f;
    float hw=car->half_width>0.5f?car->half_width:1.0f;
    float fx=cosf(heading),fy=sinf(heading);
    /* Race support uses the fitted wheel footprint and selected ground plane.
       Body overhangs can cross a verge; wall clearance still uses the full body. */
    float centre=z,normal[3]={0,0,1};
    int surface=terrain?world_ground_pose(scene,x,y,z,&centre,normal):WSURF_ROAD;
    if(terrain && (surface==WSURF_NONE || fabsf(centre-z)>1 || normal[2]<.7f))return 0;
    /* Preserve a valid selected plane before averaging the footprint: a
       centre-height probe uphill can choose pavement underneath the ramp. */
    int supported=0;
    for(int pass=0;pass<(terrain && surface==WSURF_TERRAIN?2:1);pass++) {
        if(pass) {
            /* Pavement can change grade within the wheel footprint; one face's
               normal cannot describe both sides of that coherent transition. */
            WGroundHit hit={.cat=surface,.z=centre};
            int fitted=car->support.ax[0]-car->support.ax[2]>.1f;
            world_ground_patch_normal(scene,x,y,heading,fitted?car->support.ax[0]:1.3f,
                fitted?car->support.ax[2]:-1.3f,fitted?fabsf(car->support.ay[0]):.8f,&hit,normal);
            if(normal[2]<.7f)return 0;
        }
        supported=0;
        for(int a=-1;a<=1;a+=2)for(int b=-1;b<=1;b+=2) {
            float ax=a*hl,ay=-b*hw;
            if(terrain) {
                int wheel=(a>0?0:2)+(b>0?1:0);
                int fitted=car->support.ax[0]-car->support.ax[2]>.1f;
                ax=fitted?car->support.ax[wheel]:a*1.3f;
                ay=fitted?car->support.ay[wheel]:-b*.8f;
            }
            float px=x+fx*ax-fy*ay;
            float py=y+fy*ax+fx*ay;
            float expected=centre-(normal[0]*(px-x)+normal[1]*(py-y))/normal[2],pz=expected;
            int cat=world_ground_at(scene,px,py,expected,&pz);
            supported+=(cat==WSURF_ROAD || (terrain && cat==WSURF_TERRAIN)) &&
                        fabsf(pz-expected)<=(terrain && cat==WSURF_TERRAIN?.2f:1.0f);
        }
        if(supported==4)return supported;
    }
    return supported;
}

static int traffic_pose_clear(const AiTrafficWorld *world,const AiCar *car,
                              float x,float y,float z,float heading) {
    if(!world || !world->scene)return 1;
    return traffic_supported_corners(world,car,x,y,z,heading,0)==4 &&
           traffic_body_clear(world,car,x,y,z,heading);
}

static int race_pose_clear(const AiTrafficWorld *world,const AiCar *car,
                              float x,float y,float z,float heading) {
    if(!world || !world->scene)return 1;
    return traffic_supported_corners(world,car,x,y,z,heading,1)==4 &&
           traffic_body_clear(world,car,x,y,z,heading);
}

static int traffic_lane_clear(const AiTrafficWorld *world,const AiCar *car,
                               float x,float y,float z,float heading) {
    if(!world || !world->scene)return 1;
    AiCar fallback={.half_length=2.2f,.half_width=1,.height=1.6f};
    if(!car)car=&fallback;
    float bb[6];ai_contact_bounds(car,bb);
    AiCar body=*car;body.half_length=bb[3];body.half_width=bb[4]+.15f;
    /* Support may straddle an authored road/terrain seam; lane centres may not. */
    for(int i=0;i<4;i++) {
        body.support.ax[i]=(i<2?1:-1)*body.half_length;
        body.support.ay[i]=(i%2?-1:1)*body.half_width;
    }
    float ground=z;int surface=world_ground_at(world->scene,x,y,z,&ground);
    return surface!=WSURF_NONE && fabsf(ground-z)<1 &&
        traffic_supported_corners(world,&body,x,y,ground,heading,1)==4 &&
        traffic_body_clear(world,&body,x,y,ground,heading);
}

static float traffic_lane_tangent(const AiRoadNet *roads,const AiTrafficWorld *world,const AiCar *car,int from,int to,
                                  float t,float x,float y,float z,float heading,
                                  float *outx,float *outy,float *outz) {
    N2Scene *scene=world?world->scene:NULL;
    /* Navigation stays on the road even while the vehicle is airborne. */
    int surface=scene?world_ground_at(scene,x,y,z,&z):WSURF_ROAD;
    float width=roads->half_width
        ? roads->half_width[from]*(1.0f-t)+roads->half_width[to]*t : 4.0f;
    float offset=fminf(2.0f,width*0.45f);
    for(float side=offset;side>=0.45f;side*=0.5f) {
        float px=x+sinf(heading)*side,py=y-cosf(heading)*side,pz=z;
        int side_surface=scene?world_ground_at(scene,px,py,z,&pz):WSURF_ROAD;
        /* A terrain-labelled authored road still has lanes. A ROAD centre
           must not use this allowance to steer onto a terrain shoulder. */
        if((side_surface==WSURF_ROAD ||
            (surface==WSURF_TERRAIN && side_surface==WSURF_TERRAIN)) && fabsf(pz-z)<0.75f &&
           traffic_lane_clear(world,car,px,py,pz,heading)) {
            *outx=px;*outy=py;*outz=pz;return side;
        }
    }
    *outx=x;*outy=y;*outz=z;
    return 0;
}

/* Ground support alone does not make a road open: check the authored lane
 * against the same static collision used by the driver, with this car's size. */
static int traffic_edge_clear(const AiRoadNet *roads,const AiTrafficWorld *world,
                              const AiCar *car,int from,int to,float z) {
    if(!world || !world->scene)return 1;
    float x=roads->xy[2*from],y=roads->xy[2*from+1];
    float dx=roads->xy[2*to]-x,dy=roads->xy[2*to+1]-y;
    float heading=atan2f(dy,dx);
    int steps=(int)ceilf(hypotf(dx,dy)/2.0f);
    for(int q=0;q<=steps;q++) {
        float t=steps?(float)q/steps:0,px=x+dx*t,py=y+dy*t,pz=z;
        if(world_ground_at(world->scene,px,py,z,&pz)==WSURF_NONE)return 0;
        float offset=traffic_lane_tangent(roads,world,car,from,to,t,px,py,pz,heading,&px,&py,&pz);
        if(!offset && !traffic_body_clear(world,car,px,py,pz,heading))return 0;
        z=pz;
    }
    return 1;
}

/* Join nearby road segments, including the middle of an edge. Stored point
 * order is not a decoded one-way rule; incoming heading selects travel direction.
 * shortcut: heading selects direction until packed source direction flags are decoded. */
static int traffic_next(const AiRoadNet *roads,const AiTrafficWorld *world,const AiCar *car,
                         int at,int previous,float z,int preview,
                         const int (*avoid)[2],int navoid,const float *goal) {
    N2Scene *scene=world?world->scene:NULL;
    if(!roads || at<0 || at>=roads->n)return -1;
    const float *xy=roads->xy;
    int surface=scene?world_ground_at(scene,xy[at*2],xy[at*2+1],z,&z):WSURF_ROAD;
    if(surface==WSURF_NONE)return -1;
    float hx=0,hy=0;
    if(previous>=0) {
        hx=xy[at*2]-xy[previous*2];hy=xy[at*2+1]-xy[previous*2+1];
        float len=hypotf(hx,hy);if(len>0.01f){hx/=len;hy/=len;}
    }
    int best=-1,fallback=-1;float score=-1e30f;
    /* Preserve the authored chain before considering nearby junctions. A
       straighter overlapping branch must not steal a car halfway round a bend. */
    int cand[4096]; /* Recursive previews must not overwrite their caller's candidates. */
    /* Authored joins describe travel along a chain. A car driving a chain
       backwards reaches its start with no authored exit (STREAML4RA 8496,
       overlapping 10515) and parked every free-roam racer there, so pass 2
       searches geometrically for it, keeping the backwards sense. Forward
       stubs keep their authored dead end: a fallback at 9757 fed a narrow
       two-way chain (87xx) that deadlocked head-on from density 12. */
    int against=previous>=0 && roads->next[at]==previous;
    /* A chase (goal) weighs junction exits against the own chain too. */
    for(int pass=0;pass<3 && (best<0 || (goal && pass==1));pass++) {
      /* Authored chains can cross terrain-classified ground. Joining a new
         chain still needs ROAD support; mesh labels alone cannot invent roads. */
      if(pass>0 && surface!=WSURF_ROAD)break;
      if(pass==2 && (!against || !ai_road_has_joins(roads,at) || fallback>=0))break;
      int nc=ai_road_candidates(roads,at,pass,cand,4096);
      int authored=pass==1 && ai_road_has_joins(roads,at);
      for(int ci=0;ci<(nc<0?roads->n:nc);ci++) {
        int i=nc<0?ci:cand[ci];
        int next=roads->next[i];if(!authored && next<0)continue;
        for(int reverse=0;reverse<(authored?1:2);reverse++) {
            int start=authored?at:reverse?next:i,nb=authored?i:reverse?i:next;
            if(pass==0 && start!=at)continue;
            if(pass==2 && !reverse)continue;
            if(nb==at || nb==previous)continue;
            int banned=0;
            for(int a=0;a<navoid;a++)banned|=avoid[a][0]==at && avoid[a][1]==nb;
            if(banned)continue;
            float ex=xy[nb*2]-xy[start*2],ey=xy[nb*2+1]-xy[start*2+1];
            float edge=hypotf(ex,ey);
            if(edge<1.0f || edge>60.0f)continue;
            if(previous>=0 && (ex*hx+ey*hy)/edge<-0.15f)continue;
            float t=((xy[at*2]-xy[start*2])*ex+(xy[at*2+1]-xy[start*2+1])*ey)/(edge*edge);
            t=fmaxf(0,fminf(1,t));
            float gap=hypotf(xy[start*2]+ex*t-xy[at*2],xy[start*2+1]+ey*t-xy[at*2+1]);
            if(gap>5.0f)continue;
            float dx=xy[nb*2]-xy[at*2],dy=xy[nb*2+1]-xy[at*2+1];
            float len=hypotf(dx,dy);
            if(len<1.0f || len>60.0f)continue;
            if(previous>=0 && (dx*hx+dy*hy)/len<-0.15f)continue;
            if(!traffic_edge_supported(scene,xy[at*2],xy[at*2+1],dx,dy,z,pass>0))continue;
            if(!traffic_edge_clear(roads,world,car,at,nb,z))continue;
            /* Bounded recursive preview follows open junction alternatives,
               rather than rejecting the approach to a junction merely because
               a later authored branch is closed. Terminal/reverse chains keep
               their existing endpoint behavior. */
            if(preview>0 && scene && roads->next[nb]>=0 && roads->next[nb]!=at) {
                float nz=z;world_ground_at(scene,xy[nb*2],xy[nb*2+1],z,&nz);
                if(traffic_next(roads,world,car,nb,at,nz,preview-1,NULL,0,NULL)<0)continue;
            }
            float s=previous<0? -gap : (dx*hx+dy*hy)/len-gap*0.02f;
            if(goal) { /* a chasing racer prefers the exit toward its quarry */
                float gx=goal[0]-xy[at*2],gy=goal[1]-xy[at*2+1],gl=fmaxf(1,hypotf(gx,gy));
                s=(dx*gx+dy*gy)/(len*gl)+.2f*s;
            }
            if(s>score){score=s;best=nb;}
        }
      }
      /* A stored exit can precede the chain's last short stub. Keep the stub
         as fallback, but do not drive past a safe exit into a dead end. */
      if(pass==0 && best>=0 && preview>0 && ai_road_has_joins(roads,at)) {
          int begin=roads->join_start?roads->join_start[at]:0;
          int end=roads->join_start?roads->join_start[at+1]:roads->njoin,linked=0;
          for(int j=begin;j<end;j++)if(roads->join[j][0]==at){linked=1;break;}
          int from=at,to=best,ends=0;float nz=z;
          for(int hop=0;linked && hop<preview;hop++) {
              if(scene)world_ground_at(scene,xy[2*to],xy[2*to+1],nz,&nz);
              int next=traffic_next(roads,world,car,to,from,nz,0,NULL,0,NULL);
              if(next<0){ends=1;break;}
              from=to;to=next;
          }
          if(ends) {
              fallback=best;best=-1;score=-1e30f;
          }
      }
    }
    return best>=0?best:fallback; /* no continuation: stop, then retire off-screen */
}

static float traffic_corner_trim(const AiRoadNet *roads,int before,int corner,int after) {
    if(before<0 || after<0)return 0;
    const float *xy=roads->xy;
    float ax=xy[corner*2]-xy[before*2],ay=xy[corner*2+1]-xy[before*2+1];
    float bx=xy[after*2]-xy[corner*2],by=xy[after*2+1]-xy[corner*2+1];
    float al=hypotf(ax,ay),bl=hypotf(bx,by);
    if(al<1.0f || bl<1.0f)return 0;
    float cosine=(ax*bx+ay*by)/(al*bl);
    if(cosine>0.995f || cosine<-0.85f)return 0;
    float width=roads->half_width?roads->half_width[corner]:6.0f;
    return fminf(fminf(8.0f,fminf(al,bl)*0.35f),fmaxf(2.0f,width*0.9f));
}

int ai_road_next(const AiRoadNet *roads,const AiTrafficWorld *world,const AiCar *car,
                 int at,int previous,float z) {
    return traffic_next(roads,world,car,at,previous,z,3,NULL,0,NULL);
}

/* Racers drive their node plan (roam_racer_limit), which detours around
   dead ends; past the plan, or for traffic, the usual choice applies. */
static int route_next(const AiTraffic *route,const AiRoadNet *roads,const AiTrafficWorld *world,
                      const AiCar *car,int at,int previous,float z) {
    if(!route->racer)return ai_road_next(roads,world,car,at,previous,z);
    for(int i=0;i+1<route->nplan;i++)
        if(route->plan[i]==at && (i?route->plan[i-1]:route->from)==previous)return route->plan[i+1];
    return traffic_next(roads,world,car,at,previous,z,3,route->avoid,route->navoid,route->chase?route->goal:NULL);
}

static void traffic_path_pose(const AiRoadNet *roads,const AiTraffic *route,
                              float *x,float *y,float *heading) {
    const float *xy=roads->xy;
    int from=route->from,to=route->to;
    float dx=xy[to*2]-xy[from*2],dy=xy[to*2+1]-xy[from*2+1];
    float len=hypotf(dx,dy),s=route->along;
    *x=xy[from*2]+dx*s/len;*y=xy[from*2+1]+dy*s/len;
    *heading=atan2f(dy,dx);
    int before=-1,corner=-1,after=-1;float trim=0,q=0;
    if(route->prev>=0 &&
       (trim=traffic_corner_trim(roads,route->prev,from,to))>0 && s<trim) {
        before=route->prev;corner=from;after=to;q=0.5f+0.5f*s/trim;
    } else if(route->after>=0 &&
              (trim=traffic_corner_trim(roads,from,to,route->after))>0 && s>len-trim) {
        before=from;corner=to;after=route->after;
        q=0.5f*(s-(len-trim))/trim;
    }
    if(corner<0)return;
    float ux=xy[corner*2]-xy[before*2],uy=xy[corner*2+1]-xy[before*2+1];
    float vx=xy[after*2]-xy[corner*2],vy=xy[after*2+1]-xy[corner*2+1];
    float al=hypotf(ux,uy),bl=hypotf(vx,vy);
    ux/=al;uy/=al;vx/=bl;vy/=bl;
    float one=1.0f-q;
    *x=one*one*(xy[corner*2]-ux*trim)+2*one*q*xy[corner*2]+q*q*(xy[corner*2]+vx*trim);
    *y=one*one*(xy[corner*2+1]-uy*trim)+2*one*q*xy[corner*2+1]+q*q*(xy[corner*2+1]+vy*trim);
    *heading=atan2f(one*uy+q*vy,one*ux+q*vx);
}

static float traffic_turn_speed(const AiRoadNet *roads,const AiTraffic *route) {
    if(!roads || route->after<0)return 1e30f;
    float trim=traffic_corner_trim(roads,route->from,route->to,route->after);
    if(trim<=0)return 1e30f;
    const float *xy=roads->xy;
    float ax=xy[route->to*2]-xy[route->from*2];
    float ay=xy[route->to*2+1]-xy[route->from*2+1];
    float bx=xy[route->after*2]-xy[route->to*2];
    float by=xy[route->after*2+1]-xy[route->to*2+1];
    float len=hypotf(ax,ay),next=hypotf(bx,by);
    float cosine=fmaxf(-1.0f,fminf(1.0f,(ax*bx+ay*by)/(len*next)));
    float corner=fmaxf(6.0f,13.0f-4.0f*acosf(cosine));
    float remaining=fmaxf(0.0f,len-route->along-trim);
    return sqrtf(corner*corner+2.0f*4.0f*remaining);
}

static int traffic_spawn_run_clear(const AiRoadNet *roads,const AiTrafficWorld *world,
                                   const AiCar *car,int from,int to,float z) {
    if(!world || !world->scene)return 1;
    AiTraffic route={.prev=-1,.from=from,.to=to,
                     .after=ai_road_next(roads,world,car,to,from,z)};
    for(int i=0;i<15;i++) {
        route.along+=1.0f;
        float length=hypotf(roads->xy[2*route.to]-roads->xy[2*route.from],
                             roads->xy[2*route.to+1]-roads->xy[2*route.from+1]);
        if(route.along>=length) {
            if(route.after<0)return 0;
            route.along-=length;route.prev=route.from;route.from=route.to;route.to=route.after;
            route.after=ai_road_next(roads,world,car,route.to,route.from,z);
            length=hypotf(roads->xy[2*route.to]-roads->xy[2*route.from],
                           roads->xy[2*route.to+1]-roads->xy[2*route.from+1]);
        }
        float x,y,h,nz=z;
        traffic_path_pose(roads,&route,&x,&y,&h);
        if(world_ground_at(world->scene,x,y,z,&nz)!=WSURF_ROAD)return 0;
        traffic_lane_tangent(roads,world,car,route.from,route.to,route.along/length,
                             x,y,nz,h,&x,&y,&nz);
        if(!traffic_pose_clear(world,car,x,y,nz,h))return 0;
        z=nz;
    }
    return 1;
}

int ai_traffic_offscreen(const float eye[3],const float view[2],const float pos[3]) {
    float dx=pos[0]-eye[0],dy=pos[1]-eye[1],distance=hypotf(dx,dy);
    float facing=hypotf(view[0],view[1]);
    return distance>50.0f && facing>0.5f &&
           dx*view[0]+dy*view[1]<0.15f*distance*facing;
}

/* Roaming racers cruise briskly until engaged, then race flat out
   (staggered just under the car's own flat-out speed). m/tick. */
static float roam_racer_pace(const AiCar *car,int k,int encounter) {
    float top=phys_vehicle_top_speed(car->vehicle.accel>0?&car->vehicle:NULL);
    return encounter==AI_ENCOUNTER_RACING?(.92f-.04f*(k-N_AI))*top:.45f*top;
}

/* Outrun, per the shipped rules text: the player challenges a nearby cruising
   rival with the Enter Race key; one outrun at a time. The leader may take any
   route; the trailer must pass the leader to take the lead; a leader who gets
   AI_OUTRUN_LEAD_M away wins. A trailing racer chases the player (route->goal).
   WON/LOST racers never re-engage until they respawn. */
int ai_outrun_challengeable(const AiCar cars[],const AiTraffic routes[],int count,const AiCar *player) {
    if(!player)return -1;
    int best=-1;float nearest=AI_OUTRUN_ENGAGE_M;
    for(int k=0;k<count;k++)if(routes[k].present && routes[k].racer) {
        if(routes[k].encounter==AI_ENCOUNTER_RACING)return -1;
        if(routes[k].encounter!=AI_ENCOUNTER_CRUISE || routes[k].to<0)continue;
        const AiCar *car=&cars[k];
        float d=hypotf(player->pos[0]-car->pos[0],player->pos[1]-car->pos[1]);
        if(d<nearest && fabsf(player->pos[2]-car->pos[2])<2.5f && cosf(player->head-car->head)>.7f)
            {nearest=d;best=k;}
    }
    return best;
}

void ai_outrun_begin(AiCar cars[],AiTraffic routes[],int k,const AiCar *player) {
    AiTraffic *route=&routes[k];const AiCar *car=&cars[k];
    route->encounter=AI_ENCOUNTER_RACING;route->outrun_ticks=0;
    route->result_ticks=0;
    route->cruise_speed=roam_racer_pace(car,k,AI_ENCOUNTER_RACING);
    route->player_leads=(player->pos[0]-car->pos[0])*cosf(car->head)+
                        (player->pos[1]-car->pos[1])*sinf(car->head)>0;
    route->lead_changes=0;
}

static void roam_encounter(AiCar cars[],AiTraffic routes[],int k,const AiCar *player) {
    AiTraffic *route=&routes[k];
    route->chase=0;
    if(!player || route->encounter!=AI_ENCOUNTER_RACING)return;
    const AiCar *car=&cars[k];
    const AiCar *leader=route->player_leads?player:car,*trailer=route->player_leads?car:player;
    float dx=trailer->pos[0]-leader->pos[0],dy=trailer->pos[1]-leader->pos[1],distance=hypotf(dx,dy);
    /* Passing takes the lead: close, and a car length ahead along the leader. */
    if(distance<20 && dx*cosf(leader->head)+dy*sinf(leader->head)>leader->half_length+trailer->half_length)
        {route->player_leads=!route->player_leads;route->lead_changes++;}
    route->outrun_ticks++;
    route->outrun_lead=route->player_leads?distance:-distance;
    if(distance>=AI_OUTRUN_LEAD_M) {
        route->encounter=route->player_leads?AI_ENCOUNTER_WON:AI_ENCOUNTER_LOST;
        route->result_ticks=4*PHYS_TICKRATE;
        route->cruise_speed=roam_racer_pace(car,k,AI_ENCOUNTER_CRUISE);
        return;
    }
    if(route->player_leads) {
        route->chase=1;route->goal[0]=player->pos[0];route->goal[1]=player->pos[1];
    }
}

void ai_traffic_follow(AiCar cars[N_OPENWORLD_AI], AiTraffic routes[N_OPENWORLD_AI],
                       const AiRoadNet *roads, int k, int count, const AiCar *player) {
    /* ponytail: local gap model; use decoded lane/signal rules if found. */
    if(k<0 || k>=count)return;
    if(routes[k].result_ticks)routes[k].result_ticks--;
    roam_encounter(cars,routes,k,player);
    if(routes[k].to<0){cars[k].target_speed=0;return;}
    AiCar *car=&cars[k];
    float fx=cosf(car->head),fy=sinf(car->head);
    float speed=car->spd*PHYS_TICKRATE;
    float target=fminf(routes[k].cruise_speed*PHYS_TICKRATE,
        routes[k].racer?routes[k].racer_limit:traffic_turn_speed(roads,&routes[k]));
    /* Racers: braking share and gap of the circuit racers, not traffic's. */
    float decel=4.0f,margin=6.0f,headway=1.5f;
    if(routes[k].racer) {
        decel=RACE_BRAKE_SHARE*phys_vehicle_brake_decel(car->vehicle.accel>0?&car->vehicle:NULL)
              *PHYS_TICKRATE*PHYS_TICKRATE;
        margin=3.0f;headway=.8f;
    }
    if(routes[k].after<0) {
        int from=routes[k].from,to=routes[k].to;
        float length=hypotf(roads->xy[2*to]-roads->xy[2*from],
                            roads->xy[2*to+1]-roads->xy[2*from+1]);
        target=fminf(target,sqrtf(8.0f*fmaxf(0.0f,length-routes[k].along-1.0f)));
    }
    /* Racers overtake: a slower same-direction car in the lane moves the racer
       one car width left while the shifted line is wall-clear (step) and no
       opposing car is in that band; it returns once nothing slower remains
       beside or just ahead in its own lane. Committed, a passed car limits
       speed only when contact is imminent. */
    int racer=routes[k].racer,passable=0,occupied=0,oncoming=0,merge_clear=1;
    float free_target=target,pass_target=target,shift=routes[k].pass_shift;
    float width=2*car->half_width+1.0f;
    for(int j=0;j<count+(player!=NULL);j++)if(j!=k && (j==count || routes[j].present)) {
        const AiCar *lead=j==count?player:&cars[j];
        if(fabsf(car->pos[2]-lead->pos[2])>2.5f)continue;
        float alignment=cosf(lead->head-car->head),cross=sinf(lead->head-car->head);
        float lead_length=lead->half_length*fabsf(alignment)+lead->half_width*fabsf(cross);
        float lead_width=lead->half_width*fabsf(alignment)+lead->half_length*fabsf(cross);
        float dx=lead->pos[0]-car->pos[0],dy=lead->pos[1]-car->pos[1];
        float ahead=dx*fx+dy*fy;
        /* Predict crossing traffic before it enters the current lane aperture.
           Stable slot priority prevents two approaching cars yielding forever. */
        float lateral=dy*fx-dx*fy,side_speed=lead->spd*PHYS_TICKRATE*cross;
        if((j<k || j==count) && fabsf(cross)>.5f && lateral*side_speed<0) {
            float time=-lateral/side_speed;
            float station=ahead+lead->spd*PHYS_TICKRATE*alignment*time;
            float approach=fmaxf(speed,routes[k].cruise_speed*PHYS_TICKRATE);
            float window=.8f+(car->half_length+lead->half_length)/fmaxf(3,approach);
            if(time<4 && station>0 && station<100 &&
               fabsf(time-station/fmaxf(1,approach))<window) {
                float gap=fmaxf(0,station-car->half_length-lead->half_width-6);
                target=fminf(target,fminf(sqrtf(8*gap),gap/1.5f));
            }
        }
        float band=car->half_width+lead_width+0.5f;
        /* Leave a gap for cars already in the passing lane, including a
           faster car that would reach us while the lateral move completes. */
        float closing=fmaxf(0,lead->spd*alignment*PHYS_TICKRATE-speed);
        if(racer && alignment>.7f && fabsf(lateral-width)<band &&
           ahead>-(car->half_length+lead_length+3+closing) &&
           ahead<car->half_length+lead_length+3+fmaxf(0,speed-lead->spd*PHYS_TICKRATE))merge_clear=0;
        int slower=racer && alignment>.7f && lead->spd*alignment*PHYS_TICKRATE<free_target-3;
        if(racer && alignment<-.5f && ahead>-10 && ahead<150 &&
           lateral>-band && lateral<width+band)oncoming=1;
        if(slower && fabsf(lateral+shift)<band &&
           ahead>-(car->half_length+lead_length+4) && ahead<40)occupied=1;
        if(ahead<=0 || ahead>100.0f ||
           fabsf(dx*fy-dy*fx)>car->half_width+lead_width+0.5f)continue;
        /* Opposing cars inside each other's margin both stopped for good
           (STREAML4RA 87xx, 1.9 m apart). Slot priority, as for crossings:
           the lower slot passes when the bodies clear; the other still waits. */
        if(alignment<-.5f && j>k && j<count &&
           fabsf(dx*fy-dy*fx)>car->half_width+lead_width)continue;
        float gap=ahead-car->half_length-lead_length;
        float relative=fmaxf(0.0f,speed-lead->spd*alignment*PHYS_TICKRATE);
        float braking=relative*relative/(2.0f*decel);
        float safe=fmaxf(0.0f,(gap-margin-braking)/headway);
        if(safe<target)target=safe;
        if(slower && ahead<40)passable=1;
        if(!slower || gap<1.5f+braking)pass_target=fminf(pass_target,safe);
    }
    if(racer) {
        float want=routes[k].pass_want;
        if(want>0 && (!occupied || (passable && (oncoming || !routes[k].pass_ok))))want=0;
        else if(!want && passable && !oncoming && merge_clear && routes[k].pass_ok)want=width;
        routes[k].pass_want=want;
        if(want>0)target=pass_target;
    }
    car->target_speed=target/PHYS_TICKRATE;
}

typedef struct { int node; float score; } TrafficSpawnCandidate;
static int traffic_spawn_compare(const void *a,const void *b) {
    const TrafficSpawnCandidate *x=a,*y=b;
    if(x->score<y->score)return -1;
    if(x->score>y->score)return 1;
    return (x->node>y->node)-(x->node<y->node);
}

int ai_traffic_respawn(const AiRoadNet *roads, const AiTrafficWorld *world,
                       AiCar cars[N_OPENWORLD_AI],
                       AiTraffic routes[N_OPENWORLD_AI], int k, const float player[3],
                       float player_heading) {
    if(!roads || !roads->xy || !roads->next || !player || k<0 || k>=N_OPENWORLD_AI)return 0;
    N2Scene *scene=world?world->scene:NULL;
    unsigned attempt=routes[k].respawns;
    /* Population placement must not repeat with the six reused vehicle models. */
    const float angle=0.4f+(float)k*2.3999632f+0.5f*(attempt%6);
    const float radius=85.0f+20.0f*(attempt%3);
    const float tx=player[0]+cosf(angle)*radius,ty=player[1]+sinf(angle)*radius;
    float fx=cosf(player_heading),fy=sinf(player_heading);
    int best=-1,to=-1;float bz=player[2];
    TrafficSpawnCandidate *ranked=NULL;int nranked=0;
    if(roads->n>0 && (size_t)roads->n<=SIZE_MAX/sizeof *ranked)
        ranked=malloc((size_t)roads->n*sizeof *ranked);
    if(ranked) {
        for(int i=0;i<roads->n;i++)if(roads->next[i]>=0) {
            float x=roads->xy[2*i],y=roads->xy[2*i+1];
            float px=x-player[0],py=y-player[1],d2=px*px+py*py;
            if(d2<75.0f*75.0f || d2>350.0f*350.0f)continue;
            ranked[nranked++]=(TrafficSpawnCandidate){i,(x-tx)*(x-tx)+(y-ty)*(y-ty)};
        }
        qsort(ranked,(size_t)nranked,sizeof *ranked,traffic_spawn_compare);
    }
    /* Same nearest-safe choice and index ties, without sweeping farther valid
       nodes first. Allocation failure retains the original linear search. */
    /* Use the actual view; side streets can populate ahead outside the view. */
    for(int pass=0;pass<2 && best<0;pass++) {
    float score=1e30f;
    for(int ci=0;ci<(ranked?nranked:roads->n);ci++) {
        int i=ranked?ranked[ci].node:ci;
        int next=roads->next[i];if(next<0)continue;
        float x=roads->xy[i*2],y=roads->xy[i*2+1];
        float px=x-player[0],py=y-player[1],d2=px*px+py*py;
        if(d2<75.0f*75.0f || d2>350.0f*350.0f ||
           ((!world || hypotf(world->view[0],world->view[1])<=.5f) && px*fx+py*fy>-0.25f*sqrtf(d2)) ||
           (world && hypotf(world->view[0],world->view[1])>0.5f &&
            !ai_traffic_offscreen(world->eye,world->view,(float[3]){x,y,player[2]})))continue;
        float s=(x-tx)*(x-tx)+(y-ty)*(y-ty);
        if(s>=score)continue;
        float z=player[2];
        if(scene && world_ground_at(scene,x,y,z,&z)!=WSURF_ROAD)continue;
        float dx=roads->xy[next*2]-x,dy=roads->xy[next*2+1]-y;
        float len=hypotf(dx,dy);
        if(len<1.0f || len>60.0f ||
           !traffic_edge_supported(scene,x,y,dx,dy,z,1))continue;
        if(pass==0 && (fabsf(px*fy-py*fx)<20.0f ||
           fabsf((dx*fx+dy*fy)/len)>0.85f))continue;
        float run=0;int at=i;
        for(int hop=0;hop<16 && roads->next[at]>=0 && run<25.0f;hop++) {
            int n=roads->next[at];
            run+=hypotf(roads->xy[n*2]-roads->xy[at*2],
                        roads->xy[n*2+1]-roads->xy[at*2+1]);
            at=n;
        }
        if(run<25.0f)continue;
        float lx,ly,lz;
        traffic_lane_tangent(roads,world,&cars[k],i,next,0,x,y,z,atan2f(dy,dx),&lx,&ly,&lz);
        if(world && hypotf(world->view[0],world->view[1])>.5f &&
           !ai_traffic_offscreen(world->eye,world->view,(float[3]){lx,ly,lz}))continue;
        /* Pure filters: the cheap separation test runs before the costly
           clearance sweeps; the accepted candidate is unchanged. */
        int occupied=0;
        for(int j=0;j<N_OPENWORLD_AI;j++)if(j!=k && routes[j].present &&
           fabsf(lz-cars[j].pos[2])<3.0f) {
            float sep=hypotf(lx-cars[j].pos[0],ly-cars[j].pos[1]);
            float same=cosf(atan2f(dy,dx)-cars[j].head);
            if(sep<(same>0.8f?40.0f:22.0f))occupied=1;
        }
        if(occupied)continue;
        g_ai_perf.spawn_checks++;
        if(!traffic_pose_clear(world,&cars[k],lx,ly,lz,atan2f(dy,dx)) ||
           !traffic_spawn_run_clear(roads,world,&cars[k],i,next,lz))continue;
        best=i;to=next;bz=z;score=s;
        if(ranked)break;
    }
    }
    free(ranked);
    routes[k]=(AiTraffic){.prev=-1,.from=best,.to=to,.after=-1,.respawns=attempt+1,
                          .present=best>=0,
                          .stop_reason=best<0?7:0};
    if(best<0){cars[k].spd=0;return 0;}
    AiCar *car=&cars[k];
    float dx=roads->xy[to*2]-roads->xy[best*2];
    float dy=roads->xy[to*2+1]-roads->xy[best*2+1];
    routes[k].after=ai_road_next(roads,world,car,to,best,bz);
    routes[k].lane_offset=traffic_lane_tangent(roads,world,car,best,to,0,
        roads->xy[best*2],roads->xy[best*2+1],bz,
        atan2f(dy,dx),
        &car->pos[0],&car->pos[1],&car->pos[2]);
    car->head=atan2f(dy,dx);
    routes[k].racer=ai_traffic_is_racer(k);
    car->spd=(26.0f+3.0f*(k%N_AI))/3.6f/PHYS_TICKRATE;
    if(routes[k].racer)car->spd=roam_racer_pace(car,k,AI_ENCOUNTER_CRUISE);
    routes[k].cruise_speed=car->spd;
    routes[k].racer_limit=traffic_turn_speed(roads,&routes[k]);
    routes[k].plan_end=-1;
    car->spd=fminf(car->spd,routes[k].racer_limit/PHYS_TICKRATE);
    car->target_speed=car->spd;car->ride_ready=0;
    ai_motion_init(car,world);
    car->wheel_angle=car->turn_rate=0;car->braking=0;car->lap=car->prevrel=car->t=0;
    static const float colors[N_ROAM_VISUALS][3]={{.65f,.7f,.8f},{.2f,.55f,.3f},
        {.8f,.65f,.3f},{.6f,.35f,.3f},{.95f,.2f,.12f},{.1f,.5f,.95f}};
    memcpy(car->col,colors[ai_traffic_visual(k)],sizeof car->col);
    return 1;
}

static int traffic_slot_wanted(int k,const AiTrafficWorld *world) {
    int target=world?world->traffic_target:N_AI;
    if(target<0)target=0;if(target>N_TRAFFIC_MAX)target=N_TRAFFIC_MAX;
    return ai_traffic_is_racer(k)?!(world && world->ambient_only):(k<N_AI?k:k-2)<target;
}

int ai_traffic_update(const AiRoadNet *roads,const AiTrafficWorld *world,
                       AiCar cars[N_OPENWORLD_AI],AiTraffic routes[N_OPENWORLD_AI],
                       const float player[3],float heading) {
    int candidate=-1;
    for(int k=0;k<N_OPENWORLD_AI;k++) {
        int wanted=traffic_slot_wanted(k,world);
        /* Keep the challenged rival until the lead rule produces a result,
           even if its route ends offscreen. follow still advances the race. */
        if((routes[k].present && routes[k].encounter==AI_ENCOUNTER_RACING) || routes[k].result_ticks)continue;
        int offscreen=!world || ai_traffic_offscreen(world->eye,world->view,cars[k].pos);
        if(routes[k].present && offscreen && (!wanted || routes[k].to<0 ||
           hypotf(cars[k].pos[0]-player[0],cars[k].pos[1]-player[1])>420)) {
            routes[k].present=0;routes[k].to=-1;
            cars[k].spd=cars[k].vel[0]=cars[k].vel[1]=0;
        }
        /* Fewest attempts first prevents an impossible slot starving others. */
        if(wanted && !routes[k].present && (candidate<0 ||
           routes[k].respawns<routes[candidate].respawns))candidate=k;
    }
    if(candidate>=0)ai_traffic_respawn(roads,world,cars,routes,candidate,player,heading);
    return N_OPENWORLD_AI;
}

int ai_traffic_spawn(const AiRoadNet *roads, const AiTrafficWorld *world,
                     AiCar cars[N_OPENWORLD_AI],
                     AiTraffic routes[N_OPENWORLD_AI], const float player[3],
                     float player_heading) {
    memset(routes,0,N_OPENWORLD_AI*sizeof *routes);
    for(int k=0;k<N_OPENWORLD_AI;k++)
        routes[k].prev=routes[k].from=routes[k].to=routes[k].after=-1;
    int count=0;
    for(int k=0;k<N_OPENWORLD_AI;k++) {
        if(traffic_slot_wanted(k,world) &&
           ai_traffic_respawn(roads,world,cars,routes,k,player,player_heading))count=k+1;
    }
    return count;
}

/* Steering and clearance share the same bounded route walk. */
static float traffic_lookahead(AiTraffic *route,const AiRoadNet *roads,
        const AiTrafficWorld *world,const AiCar *car,float ahead,AiTraffic *out) {
    const float *xy=roads->xy;N2Scene *scene=world?world->scene:NULL;
    AiTraffic target=*route;target.along+=ahead;
    float length=hypotf(xy[2*target.to]-xy[2*target.from],xy[2*target.to+1]-xy[2*target.from+1]);
    for(int hop=0;hop<64 && target.along>length;hop++) {
        if(target.after<0){target.along=length;break;}
        target.along-=length;target.prev=target.from;target.from=target.to;target.to=target.after;
        float pz=car->pos[2];
        if(scene)world_ground_at(scene,xy[2*target.to],xy[2*target.to+1],pz,&pz);
        if(hop<8) {
            unsigned bit=1u<<hop;
            if(!(route->preview_valid&bit) || route->preview_from[hop]!=target.from ||
               route->preview_to[hop]!=target.to || route->preview_z[hop]!=pz) {
                route->preview_after[hop]=route_next(route,roads,world,car,target.to,target.from,pz);
                route->preview_from[hop]=target.from;route->preview_to[hop]=target.to;
                route->preview_z[hop]=pz;route->preview_valid|=bit;
            }
            target.after=route->preview_after[hop];
        } else {
            target.after=route_next(route,roads,world,car,target.to,target.from,pz);
        }
        length=hypotf(xy[2*target.to]-xy[2*target.from],xy[2*target.to+1]-xy[2*target.from+1]);
    }
    target.along=fminf(target.along,length);*out=target;return length;
}

/* Net heading change over the last 20 m of the plan if `next` followed it.
   Joins allow 99 degrees per node, but short hops can add up to a hairpin
   tighter than any car's full-lock radius (~18 m): a detour at STREAML4RA
   7955-7954 ran a racer wide into an embankment it fell through. */
#define AI_RACER_HAIRPIN 2.1f /* rad, ~120 degrees within 20 m */
static float roam_plan_turn(const AiTraffic *route,const AiRoadNet *roads,
                            const int *plan,int n,int next) {
    const float *xy=roads->xy;
    int last=plan[n-1];
    float h=atan2f(xy[2*next+1]-xy[2*last+1],xy[2*next]-xy[2*last]),turn=0,span=0;
    for(int i=n-1;i>=0 && span<20;i--) {
        int from=i?plan[i-1]:route->from;
        if(from<0)break;
        float dx=xy[2*plan[i]]-xy[2*from],dy=xy[2*plan[i]+1]-xy[2*from+1],len=hypotf(dx,dy);
        if(len<.5f)continue;
        float g=atan2f(dy,dx);
        turn+=atan2f(sinf(h-g),cosf(h-g));h=g;span+=len;
    }
    return fabsf(turn);
}

/* The plan ends in a dead end, or `tail` would make a hairpin: from the
   latest plan junction back, ban the branch taken and take another drivable
   exit, if one exists. Returns 1 with the shortened plan when it detoured;
   bans that found no exit are dropped. */
static int roam_racer_detour(AiTraffic *route,const AiRoadNet *roads,
                             const AiTrafficWorld *world,const AiCar *car,int *n,int tail) {
    int *plan=route->plan;
    for(int j=tail>=0?*n-1:*n-2;j>=0;j--) {
        int at=plan[j],prev=j?plan[j-1]:route->from;
        float z=car->pos[2];
        if(world && world->scene)world_ground_at(world->scene,roads->xy[2*at],roads->xy[2*at+1],z,&z);
        int fresh=route->navoid<AI_RACER_AVOID;
        int slot=fresh?route->navoid++:(int)(route->avoid_next%AI_RACER_AVOID);
        int saved[2]={route->avoid[slot][0],route->avoid[slot][1]};
        route->avoid[slot][0]=at;route->avoid[slot][1]=j==*n-1?tail:plan[j+1];
        int alt=traffic_next(roads,world,car,at,prev,z,3,route->avoid,route->navoid,route->chase?route->goal:NULL);
        if(alt>=0 && roam_plan_turn(route,roads,plan,j+1,alt)<=AI_RACER_HAIRPIN) {
            plan[j+1]=alt;*n=j+2;
            if(!j)route->after=alt;
            if(!fresh)route->avoid_next++;
            route->preview_valid=0;
            return 1;
        }
        if(fresh)route->navoid--;
        else {route->avoid[slot][0]=saved[0];route->avoid[slot][1]=saved[1];}
    }
    return 0;
}

/* Free-roam racers brake for the road ahead as circuit racers do
   (race_corner_limit): physical corner speed from the radius through three
   route samples 8 m apart, within a RACE_BRAKE_SHARE stopping distance. A
   route end ahead is a stop. The node plan is extended a few nodes per call,
   so each node costs one ai_road_next, not one per sample. Returns m/s. */
static float roam_racer_limit(AiTraffic *route,const AiRoadNet *roads,
                              const AiTrafficWorld *world,const AiCar *car) {
    const PhysVehicle *vh=car->vehicle.accel>0?&car->vehicle:NULL;
    const float *xy=roads->xy;
    float brake2=2*RACE_BRAKE_SHARE*phys_vehicle_brake_decel(vh)*PHYS_TICKRATE*PHYS_TICKRATE;
    float speed=fabsf(car->spd)*PHYS_TICKRATE,reach=fminf(250,speed*speed/brake2+30);
    int *plan=route->plan,n=route->nplan,j=0;
    while(j<n && plan[j]!=route->to)j++;
    if(j>=n || (route->after>=0 && j+1<n && plan[j+1]!=route->after) ||
       (route->chase && car->t%30==0))n=j=0;
    if(!n){plan[n++]=route->to;if(route->after>=0)plan[n++]=route->after;}
    else {memmove(plan,plan+j,(size_t)(n-j)*sizeof *plan);n-=j;}
    if(route->after<0)n=1;
    float ahead=hypotf(xy[2*plan[0]]-car->pos[0],xy[2*plan[0]+1]-car->pos[1]);
    for(int i=1;i<n;i++)ahead+=hypotf(xy[2*plan[i]]-xy[2*plan[i-1]],xy[2*plan[i]+1]-xy[2*plan[i-1]+1]);
    int ended=route->after<0 || route->plan_end==plan[n-1];
    /* Look 150 m ahead even when slow, so dead ends show before their junction. */
    for(int added=0;!ended && added<12 && ahead<fmaxf(reach,150) && n<AI_RACER_PLAN;added++) {
        int at=plan[n-1],prev=n>1?plan[n-2]:route->from;
        float z=car->pos[2];
        if(world && world->scene)world_ground_at(world->scene,xy[2*at],xy[2*at+1],z,&z);
        int next=traffic_next(roads,world,car,at,prev,z,3,route->avoid,route->navoid,route->chase?route->goal:NULL);
        int sharp=next>=0 && roam_plan_turn(route,roads,plan,n,next)>AI_RACER_HAIRPIN;
        /* Detoured: the plan now ends in the new branch. A hairpin with no
           other exit is still driven; a dead end with none is a stop. */
        if((next<0 || sharp) && roam_racer_detour(route,roads,world,car,&n,sharp?next:-1))continue;
        if(next<0){route->plan_end=at;ended=1;break;}
        ahead+=hypotf(xy[2*next]-xy[2*at],xy[2*next+1]-xy[2*at+1]);
        plan[n++]=next;
    }
    if(!ended)route->plan_end=-1;
    route->nplan=n;
    /* Corners are the net heading change over a 20 m window of the plan, so
       zig-zags between nearby nodes cancel; a three-point radius through
       samples 8 m apart braked for them (97 -> 69 km/h on a straight). */
    const float window=20;
    float start[AI_RACER_PLAN],head[AI_RACER_PLAN],total=-route->along;
    int prev=route->from;
    for(int i=0;i<n;i++) {
        float dx=xy[2*plan[i]]-xy[2*prev],dy=xy[2*plan[i]+1]-xy[2*prev+1],len=hypotf(dx,dy);
        float h=len>.5f?atan2f(dy,dx):i?head[i-1]:0;
        if(i)h=head[i-1]+atan2f(sinf(h-head[i-1]),cosf(h-head[i-1]));
        start[i]=total;head[i]=h;total+=len;prev=plan[i];
    }
    float limit=ended?sqrtf(brake2*fmaxf(0,total-car->half_length-2)):INFINITY;
    for(int at_seg=0,end_seg=0,i=0;i*4.0f+window<=fminf(reach,total);i++) {
        float at=i*4.0f;
        while(at_seg+1<n && start[at_seg+1]<=at)at_seg++;
        while(end_seg+1<n && start[end_seg+1]<=at+window)end_seg++;
        float turn=fabsf(head[end_seg]-head[at_seg]);
        if(turn<.02f)continue;
        /* Brake for the first node of the turn, not the window's start. */
        float turn_at=at_seg+1<n?fmaxf(at,start[at_seg+1]):at;
        float corner=fmaxf(14/3.6f,.95f*phys_vehicle_corner_speed(vh,window/turn)*PHYS_TICKRATE);
        limit=fminf(limit,sqrtf(corner*corner+brake2*fmaxf(0,turn_at)));
    }
    return limit;
}

void ai_traffic_step(AiCar *car, AiTraffic *route, const AiRoadNet *roads,
                     const AiTrafficWorld *world) {
    if(!car || !route || !roads)return;
    if(route->to<0 || route->from<0) {
        if(route->present)ai_vehicle_step(car,ai_throttle(0,car->spd),0,1,world);
        return;
    }
    N2Scene *scene=world?world->scene:NULL;
    const float *xy=roads->xy;
    float length=0;
    for(int hop=0;hop<8;hop++) {
        int from=route->from,to=route->to;
        float dx=xy[2*to]-xy[2*from],dy=xy[2*to+1]-xy[2*from+1];
        length=hypotf(dx,dy);
        if(length<.01f){route->to=-1;route->stop_reason=1;return;}
        route->along=fmaxf(0,fminf(length,
            ((car->pos[0]-xy[2*from])*dx+(car->pos[1]-xy[2*from+1])*dy)/length));
        if(route->after<0)break;
        AiTraffic end=*route;end.along=length;
        float x,y,h,z=car->pos[2];
        traffic_path_pose(roads,&end,&x,&y,&h);
        traffic_lane_tangent(roads,world,car,from,to,1,x,y,z,h,&x,&y,&z);
        if((car->pos[0]-x)*cosf(h)+(car->pos[1]-y)*sinf(h)<0)break;
        route->prev=from;route->from=to;route->to=route->after;
        route->after=route_next(route,roads,world,car,route->to,route->from,car->pos[2]);
    }
    const N2Mesh *owner=scene?scene->meshes:NULL;
    float body[3]={car->half_length,car->half_width,car->height};
    if(route->preview_scene!=owner || memcmp(route->preview_body,body,sizeof body)) {
        route->preview_valid=0;route->clearance_valid=0;route->preview_scene=owner;
        memcpy(route->preview_body,body,sizeof body);
    }
    if(!route->clearance_valid || car->t%30==0) {
        route->lane_limit=2;route->clearance_valid=1;
        if(scene) {
            float speed=fabsf(car->spd)*PHYS_TICKRATE;
            float reach=fminf(60,4+2*speed+speed*speed/8),pz=car->pos[2];
            AiTraffic sample=*route;
            for(float ahead=0;ahead<=reach;ahead+=4) {
                float span=traffic_lookahead(&sample,roads,world,car,ahead>0?4:0,&sample);
                float px,py,ph;traffic_path_pose(roads,&sample,&px,&py,&ph);
                float offset=traffic_lane_tangent(roads,world,car,sample.from,sample.to,
                    sample.along/span,px,py,pz,ph,&px,&py,&pz);
                /* Clearance planning ends at unsupported jumps/height seams;
                   physical ground following retains responsibility there. */
                if(!offset && !traffic_lane_clear(world,car,px,py,pz,ph))break;
                route->lane_limit=fminf(route->lane_limit,offset);
                if(sample.after<0 && sample.along>=span)break;
            }
        }
    }
    if(route->racer && car->t%5==0)route->racer_limit=roam_racer_limit(route,roads,world,car);
    car->t++;
    AiTraffic target;length=traffic_lookahead(route,roads,world,car,
        fminf(route->racer?20.0f:10.0f,4.0f+fabsf(car->spd)*PHYS_TICKRATE*.35f),&target);
    float x,y,h,z=car->pos[2];
    traffic_path_pose(roads,&target,&x,&y,&h);
    route->lane_offset=traffic_lane_tangent(roads,world,car,target.from,target.to,
        target.along/length,x,y,z,h,&x,&y,&z);
    int clear=1;
    if(route->lane_offset>route->lane_limit) {
        float reduce=route->lane_offset-route->lane_limit;
        float px=x-sinf(h)*reduce,py=y+cosf(h)*reduce;
        clear=traffic_lane_clear(world,car,px,py,z,h);
        if(clear){x=px;y=py;route->lane_offset=route->lane_limit;}
    }
    if(route->racer) {
        float want=route->pass_want>0?route->pass_want:2*car->half_width+1.0f;
        route->pass_ok=clear && traffic_lane_clear(world,car,x-sinf(h)*want,y+cosf(h)*want,z,h);
        float step=4.0f/PHYS_TICKRATE;
        route->pass_shift+=fmaxf(-step,fminf(step,route->pass_want-route->pass_shift));
        if(route->pass_shift>0 && (route->pass_ok || route->pass_want<=0)) {
            x-=sinf(h)*route->pass_shift;y+=cosf(h)*route->pass_shift;
        }
    }
    float error=atan2f(y-car->pos[1],x-car->pos[0])-car->head;
    error=atan2f(sinf(error),cosf(error));
    float desired=fminf(car->target_speed,
        (route->racer?route->racer_limit:traffic_turn_speed(roads,route))/PHYS_TICKRATE);
    float settled=route->racer?fmaxf(20.0f,route->cruise_speed*PHYS_TICKRATE):20.0f;
    desired=fminf(desired,(6.0f+(settled-6.0f)*fmaxf(0,cosf(error*2)))/PHYS_TICKRATE);
    if(!clear)desired=0;
    float oldx=car->pos[0],oldy=car->pos[1];
    ai_vehicle_step(car,ai_throttle(desired,car->spd),
                     fmaxf(-1,fminf(1,error/.5f)),desired<.001f,world);
    if(!car->ride.contact_mask)route->air_ticks++;
    /* A car run wide through a terrain seam (STREAML4RA 7954 embankment)
       falls forever below the world, out of sight: retire it so the slot
       respawns. A racer that falls out of an outrun loses it. */
    float road=car->pos[2];
    if(!car->ride.contact_mask && scene &&
       world_ground_at(scene,xy[2*route->to],xy[2*route->to+1],car->pos[2]+50,&road)!=WSURF_NONE &&
       car->pos[2]<road-8) {
        if(route->encounter==AI_ENCOUNTER_RACING) {
            route->encounter=AI_ENCOUNTER_WON;
            route->result_ticks=4*PHYS_TICKRATE;
        }
        route->present=0;route->to=-1;route->stop_reason=8;
        car->spd=car->vel[0]=car->vel[1]=0;
        return;
    }
    route->max_impact=fmaxf(route->max_impact,car->ride.impact);
    route->travelled+=hypotf(car->pos[0]-oldx,car->pos[1]-oldy);
    float remaining=hypotf(xy[2*route->to]-car->pos[0],xy[2*route->to+1]-car->pos[1]);
    if(route->after<0 && remaining<4 && fabsf(car->spd)<.01f) {
        route->to=-1;route->stop_reason=2;
    }
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
    float seam=hypotf(p->xy[0]-p->xy[2*p->n-2],p->xy[1]-p->xy[2*p->n-1]);
    return seam > 12.0f || (p->n>3 && seam<.01f);
}

static float ai_projection(const N2Path *p, int i, const float pos[3], float *error) {
    float dx=p->xy[2*i+2]-p->xy[2*i], dy=p->xy[2*i+3]-p->xy[2*i+1];
    float t=((pos[0]-p->xy[2*i])*dx+(pos[1]-p->xy[2*i+1])*dy)/(dx*dx+dy*dy);
    t=fmaxf(0,fminf(1,t));
    *error=hypotf(pos[0]-p->xy[2*i]-t*dx,pos[1]-p->xy[2*i+1]-t*dy);
    return t;
}

int ai_drive_join(AiDrive *d, const N2Path *p, const float pos[3], float heading,float corridor) {
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
    if(!isfinite(corridor) || corridor<0 || corridor>60)return 0;
    if (best > corridor || fabsf(dot) < 0.5f) return 0;
    d->error_limit=fmaxf(12,corridor);
    d->path=p; d->direction=dot>0 ? 1 : -1; d->length=along;
    d->loop=hypotf(p->xy[0]-p->xy[2*p->n-2],p->xy[1]-p->xy[2*p->n-1])<.01f;
    d->start=d->progress=d->checkpoint=dot>0 ? at : along-at;
    d->error=best; d->failed=0;
    return 1;
}

int ai_drive_init(AiDrive *d,const N2Path *p,const float pos[3],float heading) {
    return ai_drive_join(d,p,pos,heading,8);
}

/* Sample the validated chain by distance in either travel direction. */
static void ai_route_point(const N2Path *p,int i,float t,int direction,float distance,
                           float point[2],float *heading) {
    float len=ai_segment(p,i),part=direction>0?1-t:t;
    while(distance>part*len) {
        distance-=part*len;
        int next=i+direction;
        if(next<0 || next>=p->n-1) {
            if(hypotf(p->xy[0]-p->xy[2*p->n-2],p->xy[1]-p->xy[2*p->n-1])<.01f)
                next=direction>0?0:p->n-2;
            else {distance=part*len;break;}
        }
        i=next;len=ai_segment(p,i);part=1;t=direction>0?0:1;
    }
    t+=direction*distance/len;
    float dx=p->xy[2*i+2]-p->xy[2*i],dy=p->xy[2*i+3]-p->xy[2*i+1];
    point[0]=p->xy[2*i]+t*dx;point[1]=p->xy[2*i+1]+t*dy;
    if(heading)*heading=atan2f(dy*direction,dx*direction);
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
    int wrap=next<0 || next>=p->n-1;
    if(wrap && d->loop)next=d->direction>0?0:p->n-2;
    if (next>=0 && next<p->n-1) {
        float ne, nt=ai_projection(p,next,pos,&ne);
        if (ne < err) { i=next; t=nt; err=ne;if(wrap)d->turns++; }
    }
    d->segment=i; d->error=err;
    if (err>(isfinite(d->error_limit) && d->error_limit>=12 && d->error_limit<=60 ? d->error_limit : 12.0f)) { d->failed=1; return out; }
    float at=0;
    for (int k=0; k<i; k++) at+=ai_segment(p,k);
    at+=t*ai_segment(p,i);
    if (d->direction<0) at=d->length-at;
    at+=d->turns*d->length;
    if (at>d->progress) d->progress=at;
    if (d->progress>d->checkpoint+0.5f && fabsf(PHYS_KMH(speed))>=3.0f) {
        d->checkpoint=d->progress; d->stalled=0;
    }
    else if (++d->stalled>300) { d->failed=1; return out; }
    float remaining=d->length-at;
    if (!d->loop && remaining<2.0f && err<3.0f && fabsf(PHYS_KMH(speed))<2.0f) {
        d->finished=1; return out;
    }
    /* Keep straight-road targets short. Anticipate sharp bends and retain
       that distance through the exit, so turn-in does not start at the apex.
       ponytail: bounded geometric horizon; fitted turn-radius rollouts would
       replace it if changed handling needs a different cornering line. */
    float look=fminf(10.0f,4.0f+fabsf(speed)*PHYS_TICKRATE*0.35f);
    float extended=fminf(20.0f,10.0f+fabsf(speed)*PHYS_TICKRATE*0.7f);
    float incoming=atan2f(d->direction*(p->xy[2*i+3]-p->xy[2*i+1]),
                         d->direction*(p->xy[2*i+2]-p->xy[2*i]));
    for(float ahead=-extended;ahead<=extended;ahead+=2) {
        float point[2],h;ai_route_point(p,i,t,ahead<0?-d->direction:d->direction,fabsf(ahead),point,&h);
        if(ahead<0)h+=3.1415926536f;
        if(cosf(h-incoming)<.8f){look=extended;break;}
    }
    ai_route_point(p,i,t,d->direction,look,d->target,&d->target_heading);
    float angle=atan2f(d->target[1]-pos[1],d->target[0]-pos[0])-heading;
    angle=atan2f(sinf(angle),cosf(angle));
    out.steer=fmaxf(-1,fminf(1,angle/0.5f));
    /* Conservative test pace; brake before the endpoint, never command reverse
     * as a substitute for stopping. No handling/suspension parameters change. */
    float target=50.0f/3.6f;
    if(!d->loop)target=fminf(target,sqrtf(2*3.0f*fmaxf(0,remaining-1.0f)));
    target=fminf(target,(14.0f+36.0f*fmaxf(0,
                        cosf(fminf(fabsf(angle)*2,1.570796327f))))/3.6f);
    d->target_kmh=target*3.6f;
    float delta=target-speed*PHYS_TICKRATE;
    out.throttle=delta < -0.5f && speed>0.01f ? -1.0f : fmaxf(0,fminf(1,delta*0.5f));
    out.handbrake=target<0.1f;
    return out;
}

/* One event course, not the raw-list loop used by the old showcase AI. */
static int race_route_load(const char *root,const WEvent *event,N2Path *out) {
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Paths%d.bin",root,event->reg,event->id);
    long len=0;unsigned char *data=n2_read_file(file,&len);if(!data)return 0;
    N2Leaf leaf[8];int nl=0;n2_find_leaves(data,0,len,0x34148u,leaf,&nl,8);
    N2Path path={0};int ok=nl==1 && leaf[0].size%24==0 && leaf[0].size/24<=4096 &&
        n2_load_path(data,len,&path)==(int)(leaf[0].size/24) && path.n>1 && path.n<=4096;
    free(data);
    /* Only unsegmented open Paths use the coarse-spine lane refinement.
       Segmented courses are ordered from the event lane records below. */
    ok=ok && !event->circuit && ai_drive_route_valid(&path);
    if(!ok){free(path.xy);return 0;}
    /* Open Paths can be shared by forward/reverse events. The event outline,
       not storage order or the event id, determines start-to-finish order. */
    if(event->npoly>=2) {
        if(event->npoly>WORLD_EVPOLY){free(path.xy);return 0;}
        const float *a=event->poly[0],*b=event->poly[event->npoly-1];
        if(!isfinite(a[0]) || !isfinite(a[1]) || !isfinite(b[0]) || !isfinite(b[1])) {
            free(path.xy);return 0;
        }
        const float *first=path.xy,*last=path.xy+2*(path.n-1);
        float forward=hypotf(first[0]-a[0],first[1]-a[1])+hypotf(last[0]-b[0],last[1]-b[1]);
        float reverse=hypotf(last[0]-a[0],last[1]-a[1])+hypotf(first[0]-b[0],first[1]-b[1]);
        if(reverse<forward)for(int k=0;k<path.n/2;k++)for(int axis=0;axis<2;axis++) {
            float tmp=path.xy[2*k+axis];path.xy[2*k+axis]=path.xy[2*(path.n-1-k)+axis];path.xy[2*(path.n-1-k)+axis]=tmp;
        }
    }
    *out=path;return 1;
}

/* A branch can reset race progress while its road geometry stays nearby.
   Such an edge is not a continuous span of intermediate event distances.
   ponytail: use the existing 3x connection tolerance; decoded branch progress
   semantics would replace this geometric bound. */
static float race_lane_span(const AiRoadNet *lanes,const float *distance,int at,float length) {
    int next=lanes->next[at];if(next<0)return 0;
    float span=distance[next]-distance[at];
    if(length>0) {
        if(span>length*.5f)span-=length;
        if(span<-length*.5f)span+=length;
    }
    float step=hypotf(lanes->xy[2*next]-lanes->xy[2*at],lanes->xy[2*next+1]-lanes->xy[2*at+1]);
    return step>=.01f && step<=120 && fabsf(span)<=3*step+3 ? span : 0;
}

/* Straight body sweep from a grid pose onto the course, on one road layer. */
static int race_join_clear(const AiTrafficWorld *world,const AiCar *body,const float from[3],
                           const float to[2],float heading,int terrain) {
    float z=from[2];int steps=fmaxf(1,ceilf(hypotf(to[0]-from[0],to[1]-from[1])));
    for(int s=1;s<=steps;s++) {
        float t=(float)s/steps,x=from[0]+(to[0]-from[0])*t,y=from[1]+(to[1]-from[1])*t;
        int cat=world_ground_at(world->scene,x,y,z,&z);
        if(cat==WSURF_NONE || (!terrain && cat!=WSURF_ROAD) ||
           !traffic_body_clear(world,body,x,y,z,heading))return 0;
    }
    return 1;
}

/* One DP step can hop sideways between unlinked lane chains where one chain
   ends beside the next. Spread a hop steeper than 45 degrees between parallel
   neighbours into a smoothstep lane change: the gentlest ramp (1:8, 1:4, 1:2)
   whose moved samples keep their layer, support and body clearance, with every
   1 m between them clear. Otherwise keep the decoded samples.
   z holds each sample's DP layer height. The seam samples never move.
   ponytail: isolated hops on straight-ish stretches; hops in corners stay. */
static void race_course_ease(const AiTrafficWorld *world,const AiCar *body,N2Path *p,const float *z) {
    float *xy=p->xy,moved[2*64];
    for(int j=1;j+2<p->n-1;j++) {
        float ix=xy[2*j]-xy[2*j-2],iy=xy[2*j+1]-xy[2*j-1],ox=xy[2*j+4]-xy[2*j+2],oy=xy[2*j+5]-xy[2*j+3];
        float il=hypotf(ix,iy),ol=hypotf(ox,oy);if(il<.5f || ol<.5f)continue;
        float dx=ix/il+ox/ol,dy=iy/il+oy/ol,dl=hypotf(dx,dy);
        if(dl<1.95f)continue; /* neighbours within ~25 degrees of each other */
        dx/=dl;dy/=dl;
        float jx=xy[2*j+2]-xy[2*j],jy=xy[2*j+3]-xy[2*j+1],along=jx*dx+jy*dy;
        float lx=jx-along*dx,ly=jy-along*dy,lateral=hypotf(lx,ly);
        if(along<=0 || lateral<=along)continue;
        for(int slope=8;slope>=2;slope/=2) {
            float half=slope*lateral*.5f,width=2*half;
            int first=j,last=j+1;float back=along*.5f,ahead=along*.5f;
            while(first>1 && back+ai_segment(p,first-1)<=half){back+=ai_segment(p,first-1);first--;}
            while(last<p->n-2 && ahead+ai_segment(p,last)<=half){ahead+=ai_segment(p,last);last++;}
            if(back+ai_segment(p,first-1)<=half || ahead+ai_segment(p,last)<=half || last-first+1>64)continue;
            int clear=1;float s=along*.5f;
            for(int k=j;k>=first && clear;k--) {
                float t=(half-s)/width,f=t*t*(3-2*t);
                moved[2*(k-first)]=xy[2*k]+lx*f;moved[2*(k-first)+1]=xy[2*k+1]+ly*f;
                if(k>first)s+=ai_segment(p,k-1);
            }
            s=along*.5f;
            for(int k=j+1;k<=last;k++) {
                float t=(half+s)/width,f=t*t*(3-2*t);
                moved[2*(k-first)]=xy[2*k]-lx*(1-f);moved[2*(k-first)+1]=xy[2*k+1]-ly*(1-f);
                s+=ai_segment(p,k);
            }
            float heading=atan2f(dy,dx);
            for(int k=first;k<=last && clear;k++) {
                float height=z[k],original=z[k];int cat=world_ground_at(world->scene,xy[2*k],xy[2*k+1],original,&original);
                clear=world_ground_at(world->scene,moved[2*(k-first)],moved[2*(k-first)+1],height,&height)==cat &&
                      cat!=WSURF_NONE && fabsf(height-z[k])<=1 &&
                      race_pose_clear(world,body,moved[2*(k-first)],moved[2*(k-first)+1],height,heading);
            }
            /* Sweep the new polyline, including both unchanged neighbours. */
            for(int k=first-1;k<=last && clear;k++) {
                const float *a=k<first?xy+2*k:moved+2*(k-first),*b=k+1>last?xy+2*(k+1):moved+2*(k+1-first);
                float h=atan2f(b[1]-a[1],b[0]-a[0]),height=z[k];
                clear=race_join_clear(world,body,(float[3]){a[0],a[1],height},b,h,0);
            }
            if(!clear)continue;
            memcpy(xy+2*first,moved,(size_t)(last-first+1)*2*sizeof *moved);
            j=last;break;
        }
    }
}

/* Round only mechanically sharp Street X joins. Source tangents, source height,
   road support and full-body clearance constrain every replacement. */
static void race_course_round(const AiTrafficWorld *world,const AiCar *body,N2Path *p,float *z) {
    float v=14/3.6f/PHYS_TICKRATE,pos[3]={0},vel[2]={v,0},head=0,speed=v;
    phys_stage1_chassis_step(pos,vel,&head,&speed,1,0,&PHYS_SURF_ROAD,body->vehicle.accel>0?&body->vehicle:NULL);
    if(head<=0)return;
    float minimum=v/head,radius=minimum/.95f;
    float bound=12-body->half_width;
    for(int i=1;i+1<p->n;i++) {
        float *xy=p->xy;
        float ux=xy[2*i]-xy[2*i-2],uy=xy[2*i+1]-xy[2*i-1];
        float vx=xy[2*i+2]-xy[2*i],vy=xy[2*i+3]-xy[2*i+1];
        float area=ux*vy-uy*vx;
        float product=hypotf(ux,uy)*hypotf(vx,vy)*hypotf(ux+vx,uy+vy);
        if(fabsf(area)<.001f || product>=2*fabsf(area)*minimum)continue;
        int first=i-1,last=i+1;float back=0,ahead=0;
        while(first>0 && back+ai_segment(p,first-1)<3.141592654f*radius){back+=ai_segment(p,first-1);first--;}
        while(last<p->n-1 && ahead+ai_segment(p,last)<3.141592654f*radius){ahead+=ai_segment(p,last);last++;}
        float best=INFINITY,best_xy[128],best_z[64];int ba=-1,bb=-1;
        for(int a=first;a<i;a++)for(int b=i+1;b<=last && b-a<=63;b++) {
            int n=b-a-1;if(n<4)continue;
            if(!isfinite(z[a]) || !isfinite(z[b]))continue;
            float ax=xy[2*a],ay=xy[2*a+1],bx=xy[2*b],by=xy[2*b+1];
            float ix=xy[2*a+2]-ax,iy=xy[2*a+3]-ay,ox=bx-xy[2*b-2],oy=by-xy[2*b-1];
            float il=hypotf(ix,iy),ol=hypotf(ox,oy);if(il<.01f || ol<.01f)continue;
            ix/=il;iy/=il;ox/=ol;oy/=ol;
            float cross=ix*oy-iy*ox;if(cross*area<=.001f)continue;
            float angle=atan2f(cross,ix*ox+iy*oy),tanhalf=tanf(fabsf(angle)*.5f);
            if(tanhalf<.01f)continue;
            float da=((bx-ax)*oy-(by-ay)*ox)/cross;
            float tx=ax+ix*da,ty=ay+iy*da,db=(bx-tx)*ox+(by-ty)*oy;
            float low=fmaxf(0,fmaxf(da-il,db-ol)),high=fminf(da,db);
            float r=fmaxf(radius,low/tanhalf),trim=r*tanhalf;
            if(trim>high || !isfinite(trim))continue;
            float sx=tx-ix*trim,sy=ty-iy*trim,sign=copysignf(1,cross);
            float cx=sx-sign*iy*r,cy=sy+sign*ix*r;
            float begin=atan2f(sy-cy,sx-cx),moved[128],heights[64],cost=0,height=z[a];int clear=1;
            for(int q=0;q<n && clear;q++) {
                float phi=begin+angle*q/(n-1),x=cx+r*cosf(phi),y=cy+r*sinf(phi),error=INFINITY;
                for(int j=a;j<b;j++) {
                    float ex=xy[2*j+2]-xy[2*j],ey=xy[2*j+3]-xy[2*j+1],len2=ex*ex+ey*ey;
                    if(len2<.0001f)continue;
                    float t=fmaxf(0,fminf(1,((x-xy[2*j])*ex+(y-xy[2*j+1])*ey)/len2));
                    error=fminf(error,hypotf(x-xy[2*j]-t*ex,y-xy[2*j+1]-t*ey));
                }
                cost=fmaxf(cost,error);
                float h=phi+sign*1.570796327f;
                clear=cost<best && error<bound &&
                    world_ground_at(world->scene,x,y,height,&height)==WSURF_ROAD &&
                    fabsf(height-z[a+1+q])<=1 && race_pose_clear(world,body,x,y,height,h);
                moved[2*q]=x;moved[2*q+1]=y;heights[q]=height;
            }
            for(int q=0;q<=n && clear;q++) {
                const float *a0=q?moved+2*(q-1):xy+2*a,*b0=q<n?moved+2*q:xy+2*b;
                float h=atan2f(b0[1]-a0[1],b0[0]-a0[0]);
                clear=race_join_clear(world,body,(float[3]){a0[0],a0[1],q?heights[q-1]:z[a]},b0,h,0);
            }
            if(clear && cost<best){best=cost;ba=a;bb=b;memcpy(best_xy,moved,(size_t)n*2*sizeof(float));memcpy(best_z,heights,(size_t)n*sizeof(float));}
        }
        if(ba>=0) {
            int n=bb-ba-1;memcpy(xy+2*(ba+1),best_xy,(size_t)n*2*sizeof(float));memcpy(z+ba+1,best_z,(size_t)n*sizeof(float));
            i=bb-1;
        }
    }
}

/* Height stays tied to the authored lane even on overlapping decks. Ambiguous
   source layers and unavailable resident ground leave the old course alone. */
static void race_street_course(const char *root,const WEvent *event,const AiTrafficWorld *world,
        const AiCar *body,const AiCar *cars,int count,const AiCar *player,N2Path *path,const WRace *race,float widest) {
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Routes%dF.bin",root,event->reg,event->id);
    long bytes=0;unsigned char *data=n2_read_file(file,&bytes);if(!data)return;
    AiRoadNet lanes={0};float *height=NULL;int capacity=0;
    int ok=ai_roads_read(&lanes,data,bytes,&capacity,NULL,&height,NULL);free(data);
    float *z=ok?malloc((size_t)path->n*sizeof *z):NULL;
    float *original=z?malloc((size_t)path->n*2*sizeof *original):NULL;
    if(!z || !original){free(z);free(original);ai_roads_free(&lanes);free(height);return;}
    int oldn=path->n;memcpy(original,path->xy,(size_t)oldn*2*sizeof *original);
    float corridor=fmaxf(12,fminf(60,widest+body->half_width));
    for(int i=0;i<path->n;i++) {
        float best=INFINITY,authored=NAN;int ambiguous=0;
        for(int pass=0;pass<2;pass++)for(int j=0;j<lanes.n;j++) {
            int next=lanes.next[j];if(next<0)continue;
            float dx=lanes.xy[2*next]-lanes.xy[2*j],dy=lanes.xy[2*next+1]-lanes.xy[2*j+1],len2=dx*dx+dy*dy;
            if(len2<.0001f)continue;
            float t=fmaxf(0,fminf(1,((path->xy[2*i]-lanes.xy[2*j])*dx+(path->xy[2*i+1]-lanes.xy[2*j+1])*dy)/len2));
            float error=hypotf(path->xy[2*i]-lanes.xy[2*j]-t*dx,path->xy[2*i+1]-lanes.xy[2*j+1]-t*dy);
            float h=height[j]+t*(height[next]-height[j]);
            if(!pass && error<best){best=error;authored=h;}
            if(pass && error<=best+.01f && fabsf(h-authored)>1)ambiguous=1;
        }
        z[i]=authored;
        if(ambiguous || best>corridor || !isfinite(authored) ||
           world_ground_at(world->scene,path->xy[2*i],path->xy[2*i+1],authored,z+i)!=WSURF_ROAD ||
           fabsf(z[i]-authored)>1)z[i]=NAN;
    }
    AiCar envelope=*body;float worst=envelope.vehicle.accel>0?envelope.vehicle.steer:1;
    for(int k=-1;k<count;k++) {
        const AiCar *actor=k<0?player:cars+k;
        float steer=actor->vehicle.accel>0?actor->vehicle.steer:1;
        if(steer<worst){worst=steer;envelope.vehicle=actor->vehicle;}
    }
    race_course_round(world,&envelope,path,z);
    int kept=1;
    for(int i=1;i<path->n;i++)if(hypotf(path->xy[2*i]-path->xy[2*kept-2],path->xy[2*i+1]-path->xy[2*kept-1])>=.01f) {
        path->xy[2*kept]=path->xy[2*i];path->xy[2*kept+1]=path->xy[2*i+1];kept++;
    }
    path->n=kept;ok=ai_drive_route_valid(path);
    for(int k=-1;k<count && ok;k++) {
        const AiCar *actor=k<0?player:cars+k;AiDrive join;
        float entry=race->grid_named?fmaxf(12,fminf(60,widest+actor->half_width)):(widest>0?widest:8);
        ok=ai_drive_join(&join,path,actor->pos,actor->head,entry);
    }
    if(!ok){path->n=oldn;memcpy(path->xy,original,(size_t)oldn*2*sizeof *original);}
    free(z);free(original);ai_roads_free(&lanes);free(height);
}

/* Order lane samples by their authored distance, including a circuit's
   start-line wrap. Dynamic programming keeps the entire chain continuous
   instead of greedily jumping to a different road at a branch.
   Loaded support checks carry height along the chosen predecessor; probing
   every hill from grid height can select the ceiling beneath its road instead.
   ponytail: up to 64 lane choices per sample, one measured height per prefix. */
static int race_ground_at(N2Scene *scene,float x,float y,float ref,float *z);

static int race_course_read_mode(const char *root,const WEvent *event,N2Path *out,int full,
        const AiTrafficWorld *world,const AiCar *body,float *widest,int lane_terrain,
        int repair_distance,int *unsupported_out) {
    if(unsupported_out)*unsupported_out=0;
    if(!root || !event || !out)return 0;
    N2Path raw={0};
    if(race_route_load(root,event,&raw)){*out=raw;return 1;}
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Routes%dF.bin",root,event->reg,event->id);
    long bytes=0;unsigned char *data=n2_read_file(file,&bytes);if(!data)return 0;
    AiRoadNet lanes={0};float *distance=NULL,*height=NULL;int capacity=0;
    int ok=ai_roads_read(&lanes,data,bytes,&capacity,&distance,&height,widest);free(data);
    /* Retry isolated interior progress spikes using the same chain's geometry.
       Endpoint resets and consistent forward/reverse records stay untouched. */
    if(ok && repair_distance){
        int changed=0;
        float *fixed=malloc((size_t)lanes.n*sizeof *fixed);
        if(fixed){
            memcpy(fixed,distance,(size_t)lanes.n*sizeof *fixed);
            for(int i=2;i<lanes.n-2;i++){
                if(lanes.next[i-2]!=i-1 || lanes.next[i-1]!=i ||
                   lanes.next[i]!=i+1 || lanes.next[i+1]!=i+2)continue;
                float a=hypotf(lanes.xy[2*i]-lanes.xy[2*i-2],lanes.xy[2*i+1]-lanes.xy[2*i-1]);
                float b=hypotf(lanes.xy[2*i+2]-lanes.xy[2*i],lanes.xy[2*i+3]-lanes.xy[2*i+1]);
                float span=distance[i+1]-distance[i-1];
                if(a>.01f && b>.01f && a<=120 && b<=120 &&
                   (distance[i]-distance[i-1])*(distance[i+1]-distance[i])<0 &&
                   (distance[i-1]-distance[i-2])*span>0 && (distance[i+2]-distance[i+1])*span>0 &&
                   fabsf(span)<=3*(a+b)+3 &&
                   (fabsf(distance[i]-distance[i-1])>3*a+3 || fabsf(distance[i+1]-distance[i])>3*b+3)){
                    fixed[i]=distance[i-1]+span*a/(a+b);changed++;
                }
            }
            memcpy(distance,fixed,(size_t)lanes.n*sizeof *fixed);free(fixed);
        }
        if(!changed){ai_roads_free(&lanes);free(distance);free(height);return 0;}
    }
    float low=INFINITY,high=-INFINITY,length=event->info.length;
    for(int k=0;ok && k<lanes.n;k++){low=fminf(low,distance[k]);high=fmaxf(high,distance[k]);}
    int reverse=0;
    if(!event->circuit && ok && !full && event->npoly>=2) {
        float begin=0,end=0,db=INFINITY,de=INFINITY;
        for(int k=0;k<lanes.n;k++) {
            int next=lanes.next[k];if(next<0)continue;
            float dx=lanes.xy[2*next]-lanes.xy[2*k],dy=lanes.xy[2*next+1]-lanes.xy[2*k+1];
            float len2=dx*dx+dy*dy,span=race_lane_span(&lanes,distance,k,0);
            if(fabsf(span)<.01f)continue;
            for(int endpoint=0;endpoint<2;endpoint++) {
                const float *p=event->poly[endpoint?event->npoly-1:0];
                float t=fmaxf(0,fminf(1,((p[0]-lanes.xy[2*k])*dx+(p[1]-lanes.xy[2*k+1])*dy)/len2));
                float error=hypotf(lanes.xy[2*k]+t*dx-p[0],lanes.xy[2*k+1]+t*dy-p[1]);
                if(error<(endpoint?de:db)) {
                    float d=distance[k]+t*span;
                    if(endpoint){de=error;end=d;}else{db=error;begin=d;}
                }
            }
        }
        ok=isfinite(db) && isfinite(de) && fabsf(end-begin)>20;
        reverse=end<begin;
        low=fmaxf(low,fminf(begin,end)-60);high=fminf(high,fmaxf(begin,end)+60);
    }
    if(event->circuit){low=0;high=length;ok=ok && length>20;}
    int samples=ok && high>low ? (int)ceilf((high-low)/5)+1 : 0;
    enum { CHOICES=64 };
    typedef struct { float x,y,z,authored_z,along,penalty; double cost; int parent,root,edge,linked; } Choice;
    Choice *choices=samples>1 && samples<4096?calloc((size_t)samples*CHOICES,sizeof *choices):NULL;
    int *counts=choices?calloc((size_t)samples,sizeof *counts):NULL;
    ok=choices && counts;int layers=0;float previous=low;
    for(int j=0;ok && j<samples;j++) {
        float along=low+(high-low)*j/(samples-1);
        Choice *row=choices+layers*CHOICES;int n=0;
        for(int k=0;k<lanes.n && ok;k++) {
            int next=lanes.next[k];if(next<0)continue;
            float begin=distance[k],span=race_lane_span(&lanes,distance,k,event->circuit?length:0);
            if(fabsf(span)<.01f)continue;
            for(int copy=event->circuit?-1:0;copy<=(event->circuit?1:0);copy++) {
                float d=along+copy*length,t=(d-begin)/span;
                if(t<-.0001f || t>1.0001f)continue;
                float x=lanes.xy[2*k]+t*(lanes.xy[2*next]-lanes.xy[2*k]);
                float y=lanes.xy[2*k+1]+t*(lanes.xy[2*next+1]-lanes.xy[2*k+1]);
                float source_z=height[k]+t*(height[next]-height[k]);
                int duplicate=0;for(int q=0;q<n;q++)if(hypotf(x-row[q].x,y-row[q].y)<.1f &&
                    (!world || !body || fabsf(source_z-row[q].authored_z)<.01f))duplicate=1;
                if(duplicate)continue;
                if(n==CHOICES){ok=0;break;}
                row[n++]=(Choice){.x=x,.y=y,.z=body?body->pos[2]:0,
                    .authored_z=source_z,
                    .along=along,.cost=INFINITY,.parent=-1,.root=-1,.edge=k};
            }
        }
        if(!n){if(along-previous>15)ok=0;continue;}
        counts[layers++]=n;previous=along;
    }
    /* Seed height beside the starting body, not at a distant lap seam.
       Rotate the closed source sequence; gate binding keeps the finish line. */
    if(ok && world && body && event->circuit && layers>2) {
        int start=0;float nearest=INFINITY;
        for(int j=0;j<layers-1;j++)for(int q=0;q<counts[j];q++) {
            Choice *c=choices+j*CHOICES+q;
            float d=hypotf(c->x-body->pos[0],c->y-body->pos[1]);
            if(d<nearest){nearest=d;start=j;}
        }
        if(start) {
            Choice *rotated=calloc((size_t)layers*CHOICES,sizeof *rotated);
            int *ordered=calloc((size_t)layers,sizeof *ordered);
            if(rotated && ordered) {
                float origin=choices[start*CHOICES].along;
                for(int j=0;j<layers-1;j++) {
                    int at=(start+j)%(layers-1);ordered[j]=counts[at];
                    memcpy(rotated+j*CHOICES,choices+at*CHOICES,(size_t)counts[at]*sizeof *choices);
                    for(int q=0;q<ordered[j];q++) {
                        Choice *c=rotated+j*CHOICES+q;c->along-=origin;
                        if(c->along<0)c->along+=length;
                    }
                }
                ordered[layers-1]=ordered[0];
                memcpy(rotated+(layers-1)*CHOICES,rotated,(size_t)ordered[0]*sizeof *choices);
                for(int q=0;q<ordered[0];q++)rotated[(layers-1)*CHOICES+q].along=length;
                free(choices);choices=rotated;free(counts);counts=ordered;
            } else {free(rotated);free(ordered);}
        }
    }
    /* Each start-lane seed revisits the same geometry. Reuse connection checks
       only at exactly the same incoming height; stacked layers stay distinct.
       ponytail: 64 MB cache ceiling, larger layouts fall back to direct checks. */
    typedef struct { float from_z,z,penalty; int valid; } RoadCheck;
    size_t *check_offsets=world && body && layers>1?calloc((size_t)layers,sizeof *check_offsets):NULL;
    size_t nchecks=0;
    if(check_offsets)for(int j=1;j<layers;j++) {
        check_offsets[j]=nchecks;nchecks+=(size_t)counts[j]*counts[j-1];
    }
    RoadCheck *checks=nchecks && nchecks<=4194304?calloc(nchecks,sizeof *checks):NULL;
    int best=-1;double cost=INFINITY;N2Path path={0};int unsupported=0,anchored=0;
    /* Preserve the start lane when closing a lap. A cheapest prefix from a
       different lane must not discard the only prefix that joins the seam. */
    /* A terrain-labelled span can still be authored pavement. Retry a
       rejected course while strongly preferring its validated ROAD samples.
       The total preference cost stays below one rejected connection, allowing
       an approach to change when repair needs a different branch. A course
       disconnected from the grid has no validated anchors to prefer. */
    Choice *reference=world && body && layers>1?calloc((size_t)layers,sizeof *reference):NULL;
    for(int terrain=0;terrain<=1;terrain++) {
        if(terrain && (!reference || best<0 || (!unsupported && anchored) || !world || !body))break;
        if(terrain && checks)memset(checks,0,nchecks*sizeof *checks);
        for(int seed=event->circuit?0:-1;ok && layers>1 && seed<counts[0];seed++) {
            for(int q=0;q<counts[0];q++) {
                Choice *c=choices+q;c->penalty=0;
                if(world && body) {
                    int edge=c->edge,next=lanes.next[edge];float z=body->pos[2];
                    float h=atan2f(lanes.xy[2*next+1]-lanes.xy[2*edge+1],lanes.xy[2*next]-lanes.xy[2*edge]);
                    int surface=world_ground_at(world->scene,c->x,c->y,z,&z);
                    if((surface==WSURF_NONE || (!terrain && surface!=WSURF_ROAD)) ||
                       !race_pose_clear(world,body,c->x,c->y,z,h))c->penalty=1e8f;
                    c->z=z;
                }
                choices[q].cost=seed<0 || q==seed ? choices[q].penalty : INFINITY;
                if(world && body){float dz=c->z-c->authored_z;choices[q].cost+=dz*dz;}
                if(terrain && anchored && reference[0].penalty==0 && hypotf(c->x-reference[0].x,c->y-reference[0].y)>.1f)c->cost+=1e8/(layers+1);
                choices[q].root=q;choices[q].parent=-1;
            }
            for(int j=1;j<layers;j++) {
                Choice *row=choices+j*CHOICES,*prev=row-CHOICES;
                for(int q=0;q<counts[j];q++) {
                    row[q].cost=INFINITY;row[q].parent=row[q].root=-1;row[q].linked=0;
                    double anchor=terrain && anchored && reference[j].penalty==0 &&
                        hypotf(row[q].x-reference[j].x,row[q].y-reference[j].y)>.1f ? 1e8/(layers+1) : 0;
                    for(int r=0;r<counts[j-1];r++) {
                        if(!isfinite(prev[r].cost))continue;
                        float step=hypotf(row[q].x-prev[r].x,row[q].y-prev[r].y);
                        if(step>120)continue;
                        if(step>3*(row[q].along-prev[r].along)+3) {
                            int connected=prev[r].edge==row[q].edge;
                            int forward=prev[r].edge,backward=row[q].edge;
                            for(int hop=0;hop<16 && !connected;hop++) {
                                if(forward>=0)forward=lanes.next[forward];
                                if(backward>=0)backward=lanes.next[backward];
                                connected=forward==row[q].edge || backward==prev[r].edge;
                            }
                            if(!connected)continue;
                        }
                        float penalty=0,z=prev[r].z;
                        /* A lane point keeps one height. A prefix along its own
                           source chain must not lose it to a cheaper prefix that
                           reached the same XY on another layer (an overpass lane
                           sampled on the road beneath it). */
                        int linked=prev[r].edge==row[q].edge || lanes.next[prev[r].edge]==row[q].edge;
                        if(prev[r].cost+step*step>=row[q].cost && !(linked && !row[q].linked))continue;
                        /* Optionally treat terrain-labelled pavement on an authored
                           lane that advances with the race as road; the strict
                           pass otherwise prefers a clean detour around it. */
                        int along_lane=terrain;
                        if(!along_lane && lane_terrain && linked && step<=3*(row[q].along-prev[r].along)+3) {
                            int e=row[q].edge,en=lanes.next[e];
                            float span=race_lane_span(&lanes,distance,e,event->circuit?length:0);
                            float len=hypotf(lanes.xy[2*en]-lanes.xy[2*e],lanes.xy[2*en+1]-lanes.xy[2*e+1]);
                            along_lane=fabsf(span)>=.5f*len;
                        }
                        if(world && body) {
                            RoadCheck *check=checks?checks+check_offsets[j]+q*counts[j-1]+r:NULL;
                            /* Reuse an already validated ROAD connection only
                               on the same source edges and incoming height.
                               Admitting terrain must not reselect a lower deck. */
                            if(terrain && reference[j].penalty==0 &&
                               row[q].edge==reference[j].edge && prev[r].edge==reference[j-1].edge &&
                               z==reference[j-1].z) {
                                z=reference[j].z;
                            } else if(check && check->valid && check->from_z==z) {
                                z=check->z;penalty=check->penalty;
                            } else {
                                int edge=row[q].edge,next=lanes.next[edge];
                                float h=atan2f(lanes.xy[2*next+1]-lanes.xy[2*edge+1],lanes.xy[2*next]-lanes.xy[2*edge]);
                                /* Resolve the authored deck, then require the incoming sweep to reach it. */
                                int surface=world_ground_at(world->scene,row[q].x,row[q].y,row[q].authored_z,&z);
                                /* Retry a lost road layer by carrying height uphill.
                                   Keep successful endpoint probes and body sweeps unchanged. */
                                if(surface==WSURF_NONE || (!along_lane && surface!=WSURF_ROAD)) {
                                    float height=prev[r].z;
                                    if(world_ground_at(world->scene,prev[r].x,prev[r].y,height,&height)==WSURF_ROAD) {
                                        int steps=fmaxf(1,ceilf(step)),part=1;
                                        for(;part<=steps;part++) {
                                            float t=(float)part/steps,x=prev[r].x+(row[q].x-prev[r].x)*t;
                                            float y=prev[r].y+(row[q].y-prev[r].y)*t;
                                            int cat=world_ground_at(world->scene,x,y,height,&height);
                                            if(cat==WSURF_NONE || (!along_lane && cat!=WSURF_ROAD))break;
                                        }
                                        if(part>steps){surface=WSURF_ROAD;z=height;}
                                    }
                                }
                                /* The .7 support-normal limit bounds driving grade; a lower deck is not a ramp. */
                                float maxrise=step*sqrtf(1-.7f*.7f)/.7f+PHYS_RIDE_REACH_UP;
                                if(surface==WSURF_NONE || (!along_lane && surface!=WSURF_ROAD) ||
                                   fabsf(z-prev[r].z)>maxrise ||
                                   !race_pose_clear(world,body,row[q].x,row[q].y,z,h)) {
                                    penalty=1e8f;
                                    /* Keep the measured surface height even when
                                       its category or body clearance is rejected.
                                       Only a missing surface carries the old height. */
                                    if(surface==WSURF_NONE)z=prev[r].z;
                                } else {
                                    /* Clear endpoints can connect across a divider.
                                       Sweep the body between them. Footprint support
                                       uses the authored tangent at each endpoint;
                                       the driver checks its curved approach too. */
                                    h=atan2f(row[q].y-prev[r].y,row[q].x-prev[r].x);
                                    int steps=fmaxf(1,ceilf(step/2));float midz=prev[r].z;
                                    for(int s=1;s<steps;s++) {
                                        float t=(float)s/steps,x=prev[r].x+(row[q].x-prev[r].x)*t;
                                        float y=prev[r].y+(row[q].y-prev[r].y)*t;
                                        int cat=race_ground_at(world->scene,x,y,midz,&midz);
                                        if(cat==WSURF_NONE || (!along_lane && cat!=WSURF_ROAD) ||
                                           !traffic_body_clear(world,body,x,y,midz,h)) {
                                            penalty=1e8f;break;
                                        }
                                    }
                                    /* The sweep and endpoint must finish on the
                                       same layer, not opposite sides of a deck. */
                                    if(!penalty) {
                                        float end=midz;
                                        race_ground_at(world->scene,row[q].x,row[q].y,end,&end);
                                        if(fabsf(end-z)>.01f)penalty=1e8f;
                                    }
                                }
                                if(check)*check=(RoadCheck){.from_z=prev[r].z,.z=z,.penalty=penalty,.valid=1};
                            }
                        }
                        /* Favor connections along both source road tangents.
                           ponytail: tangent-weighted distance, not a turn-radius planner. */
                        int a=prev[r].edge,b=row[q].edge,an=lanes.next[a],bn=lanes.next[b];
                        float ax=lanes.xy[2*an]-lanes.xy[2*a],ay=lanes.xy[2*an+1]-lanes.xy[2*a+1];
                        float bx=lanes.xy[2*bn]-lanes.xy[2*b],by=lanes.xy[2*bn+1]-lanes.xy[2*b+1];
                        float px=row[q].x-prev[r].x,py=row[q].y-prev[r].y;
                        float ad=fabsf(px*ax+py*ay)/fmaxf(.01f,step*hypotf(ax,ay));
                        float bd=fabsf(px*bx+py*by)/fmaxf(.01f,step*hypotf(bx,by));
                        /* Prefer the authored 3D route, allowing interpolation
                           error on grades without bypassing physical checks. */
                        float dz=world && body?z-row[q].authored_z:0;
                        double c=prev[r].cost+step*step*(3-ad-bd)+dz*dz+penalty+anchor;
                        int other_layer=isfinite(row[q].cost) && fabsf(z-row[q].z)>2.5f;
                        if(other_layer?linked>row[q].linked || (linked==row[q].linked && c<row[q].cost):c<row[q].cost) {
                            row[q].cost=c;row[q].parent=r;row[q].root=prev[r].root;
                            row[q].z=z;row[q].penalty=penalty;row[q].linked=linked;
                        }
                    }
                }
            }
            for(int q=0;q<counts[layers-1];q++) {
                Choice *c=choices+(layers-1)*CHOICES+q;
                if(!isfinite(c->cost) || c->root<0)continue;
                Choice *first=choices+c->root;
                float seam=hypotf(c->x-first->x,c->y-first->y);
                /* Snapping a different lane to the start invents an unchecked connection. */
                if(event->circuit && seam>.01f)continue;
                if(c->cost+seam*seam*(event->circuit!=0)<cost) {
                    /* Layer heights ride after the points for race_course_ease. */
                    N2Path candidate={.n=layers,.xy=malloc((size_t)layers*3*sizeof(float))};
                    if(!candidate.xy)continue;
                    int parent=q,bad=0;
                    for(int j=layers-1;j>=0;j--) {
                        Choice *node=choices+j*CHOICES+parent;
                        candidate.xy[2*j]=node->x;candidate.xy[2*j+1]=node->y;candidate.xy[2*layers+j]=node->z;
                        bad+=node->penalty>0;parent=node->parent;
                    }
                    if(event->circuit) {
                        candidate.xy[2*layers-2]=candidate.xy[0];candidate.xy[2*layers-1]=candidate.xy[1];
                    }
                    int joins=!world || !body;
                    float bound=widest && *widest>0?*widest:8,nearest=INFINITY;
                    float entrance[2]={0},heading=0;
                    for(int j=0;world && body && j<candidate.n-1;j++) {
                        if(ai_segment(&candidate,j)<.01f)continue;
                        float error,t=ai_projection(&candidate,j,body->pos,&error);
                        if(error<nearest) {
                            nearest=error;joins=error<=bound;
                            ai_route_point(&candidate,j,t,1,0,entrance,&heading);
                        }
                    }
                    if(!joins){free(candidate.xy);continue;}
                    /* Nearby lanes across a divider are not reachable from the grid.
                       A divider can also begin just past the grid (a gore): walk the
                       grid heading over the planner's minimum 20 m preview and require
                       the same join from each step, until that line itself leaves the
                       road or meets a solid. */
                    double entrance_penalty=0;
                    if(world && body) {
                        if(!race_join_clear(world,body,body->pos,entrance,heading,terrain))entrance_penalty=1e8;
                        float from[3]={0,0,body->pos[2]};
                        for(int ahead=1;!entrance_penalty && isfinite(body->head) && ahead<=20;ahead++) {
                            from[0]=body->pos[0]+cosf(body->head)*ahead;from[1]=body->pos[1]+sinf(body->head)*ahead;
                            int cat=world_ground_at(world->scene,from[0],from[1],from[2],&from[2]);
                            if(cat==WSURF_NONE || (!terrain && cat!=WSURF_ROAD) ||
                               !traffic_body_clear(world,body,from[0],from[1],from[2],body->head))break;
                            float near=INFINITY,point[2]={0},h=0;
                            for(int j=0;j<candidate.n-1;j++) {
                                if(ai_segment(&candidate,j)<.01f)continue;
                                float error,t=ai_projection(&candidate,j,from,&error);
                                if(error<near){near=error;ai_route_point(&candidate,j,t,1,0,point,&h);}
                            }
                            if(!race_join_clear(world,body,from,point,h,terrain))entrance_penalty=1e8;
                        }
                    }
                    double candidate_cost=c->cost+seam*seam*(event->circuit!=0)+entrance_penalty;
                    if(candidate_cost>=cost){free(candidate.xy);continue;}
                    free(path.xy);path=candidate;
                    best=q;cost=candidate_cost;unsupported=bad;
                    if(!terrain && reference) {
                        anchored=entrance_penalty==0;
                        int parent=q;
                        for(int j=layers-1;j>=0;j--) {
                            Choice *node=choices+j*CHOICES+parent;reference[j]=*node;parent=node->parent;
                        }
                    }
                }
            }
            if(seed<0)break;
        }
    }
    free(reference);
    if(best>=0) {
        if(path.xy) {
            if(unsupported_out)*unsupported_out=unsupported;
            if(world && body)race_course_ease(world,body,&path,path.xy+2*layers);
            if(event->circuit){path.xy[2*layers-2]=path.xy[0];path.xy[2*layers-1]=path.xy[1];}
            int n=1;
            for(int j=1;j<path.n;j++)if(hypotf(path.xy[2*j]-path.xy[2*n-2],path.xy[2*j+1]-path.xy[2*n-1])>=.01f) {
                path.xy[2*n]=path.xy[2*j];path.xy[2*n+1]=path.xy[2*j+1];n++;
            }
            path.n=n;
            if(reverse)for(int j=0;j<n/2;j++)for(int axis=0;axis<2;axis++) {
                float v=path.xy[2*j+axis];path.xy[2*j+axis]=path.xy[2*(n-1-j)+axis];path.xy[2*(n-1-j)+axis]=v;
            }
        }
    }
    ok=ai_drive_route_valid(&path);
    free(checks);free(check_offsets);free(choices);free(counts);ai_roads_free(&lanes);free(distance);free(height);
    if(!ok){free(path.xy);return 0;}
    *out=path;return 1;
}

/* Direction reversals sharper than 120 degrees between consecutive samples.
   A real bend turns ~18 degrees per 5 m even on a 16 m loop; a reversal is two
   lane chains meeting end to end with the turn between them missing. */
static int race_course_reversals(const N2Path *p) {
    int n=0;
    for(int j=1;p && j+1<p->n;j++) {
        float ux=p->xy[2*j]-p->xy[2*j-2],uy=p->xy[2*j+1]-p->xy[2*j-1];
        float vx=p->xy[2*j+2]-p->xy[2*j],vy=p->xy[2*j+3]-p->xy[2*j+1];
        float ul=hypotf(ux,uy),vl=hypotf(vx,vy);
        n+=ul>.5f && vl>.5f && (ux*vx+uy*vy)<-.5f*ul*vl;
    }
    return n;
}

/* With loaded geometry, decode strictly and with lane terrain admitted, and keep
   the course with fewer unvalidated samples, then fewer reversals; a tie keeps
   the lane decode, which follows authored lanes through labelled terrain. Neither mode
   alone is safe: each DP keeps one prefix per lane point, so admitting more
   samples can strand a different section.
   shortcut: up to three decodes per course attempt; use a multi-state DP if start latency matters. */
static int race_course_read(const char *root,const WEvent *event,N2Path *out,int full,
        const AiTrafficWorld *world,const AiCar *body,float *widest) {
    int bad=0,ok=race_course_read_mode(root,event,out,full,world,body,widest,0,0,&bad);
    if(world && body) {
        N2Path lane={0};int lane_bad=0;
        if(race_course_read_mode(root,event,&lane,full,world,body,widest,1,0,&lane_bad) &&
           (!ok || lane_bad*1000+race_course_reversals(&lane)<=bad*1000+race_course_reversals(out))) {
            if(ok)free(out->xy);
            *out=lane;bad=lane_bad;ok=1;
        } else free(lane.xy);
        if(ok && bad){
            N2Path repaired={0};int repaired_bad=0;
            if(race_course_read_mode(root,event,&repaired,full,world,body,widest,1,1,&repaired_bad) && repaired_bad<bad){
                free(out->xy);*out=repaired;bad=repaired_bad;
            }else free(repaired.xy);
        }
    }
    if(ok && bad)printf("race course: %d/%d samples need support/collision validation\n",bad,out->n);
    return ok;
}

/* Last-resort event spine from measured Paths segment ranges and node links.
   Reuse city routing; never join raw records across a segment boundary.
   ponytail: shortest source graph legs between outline points; retail branch
   priorities/direction flags remain undecoded. Physical driving still checks
   the actual road and body corridor. */
static int race_route_topology(const char *root,const WEvent *event,N2Path *out) {
    if(!root || !event || !out || event->npoly<2 || event->npoly>WORLD_EVPOLY)return 0;
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Paths%d.bin",root,event->reg,event->id);
    long bytes=0;unsigned char *data=n2_read_file(file,&bytes);if(!data)return 0;
    N2Leaf nodes[2],segments[2];int nn=0,ns=0;
    N2LeafWalk nw={0x34148u,nodes,&nn,2},sw={0x34149u,segments,&ns,2};
    int ok=asset_chunks_walk(data,0,bytes,n2_leaf_chunk,&nw) &&
        asset_chunks_walk(data,0,bytes,n2_leaf_chunk,&sw) && nn==1 && ns==1;
    int n=ok?(int)(nodes[0].size/24):0,m=ok?(int)(segments[0].size/220):0;
    ok=ok && nodes[0].size%24==0 && segments[0].size%220==0 &&
        n>1 && n<=4096 && m>0 && m<=n;
    World graph={0};N2Path path={0};int *edges=NULL,*fill=NULL,*route=NULL,ne=0;
    if(ok) {
        graph.city.nnav=n;graph.city.nav=malloc((size_t)n*2*sizeof(float));
        graph.city.adjstart=calloc((size_t)n+1,sizeof(int));
        graph.city.adjlist=malloc((size_t)n*8*sizeof(int));
        edges=malloc((size_t)n*8*sizeof(int));fill=calloc((size_t)n,sizeof(int));
        route=malloc((size_t)n*sizeof(int));path.xy=malloc(4096*2*sizeof(float));
        ok=graph.city.nav && graph.city.adjstart && graph.city.adjlist && edges && fill && route && path.xy;
    }
    for(int k=0;ok && k<n;k++) {
        const unsigned char *p=data+nodes[0].off+24*k;
        float x,y;memcpy(&x,p,4);memcpy(&y,p+4,4);
        if(!isfinite(x) || !isfinite(y) || fabsf(x)>1e6f || fabsf(y)>1e6f){ok=0;break;}
        graph.city.nav[2*k]=x;graph.city.nav[2*k+1]=y;
        for(int j=0;j<3;j++) {
            unsigned link=p[12+2*j]|(unsigned)p[13+2*j]<<8;
            if(link==0xffffu)continue;
            if(link>=(unsigned)n){ok=0;break;}
            edges[2*ne]=k;edges[2*ne++ +1]=(int)link;
        }
    }
    for(int k=0;ok && k<m;k++) {
        const unsigned char *s=data+segments[0].off+220*k;
        unsigned begin=s[28]|(unsigned)s[29]<<8,end=s[30]|(unsigned)s[31]<<8;
        unsigned count=s[32]|(unsigned)s[33]<<8;
        if(begin>=end || end>(unsigned)n || end-begin!=count){ok=0;break;}
        for(unsigned j=begin;j<end;j++) {
            const unsigned char *p=data+nodes[0].off+24*j;
            if((p[8]|(unsigned)p[9]<<8)!=(unsigned)k || fill[j]++){ok=0;break;}
            if(j+1<end){edges[2*ne]=(int)j;edges[2*ne++ +1]=(int)j+1;}
        }
    }
    for(int k=0;ok && k<n;k++)if(fill[k]!=1)ok=0;
    for(int k=0;ok && k<ne;k++) {
        int a=edges[2*k],b=edges[2*k+1];
        float d=hypotf(graph.city.nav[2*a]-graph.city.nav[2*b],graph.city.nav[2*a+1]-graph.city.nav[2*b+1]);
        if(a==b || d>120){edges[2*k]=-1;continue;}
        graph.city.adjstart[a+1]++;graph.city.adjstart[b+1]++;
    }
    if(ok) {
        for(int k=0;k<n;k++)graph.city.adjstart[k+1]+=graph.city.adjstart[k];
        graph.city.nadj=graph.city.adjstart[n];memcpy(fill,graph.city.adjstart,(size_t)n*sizeof(int));
        for(int k=0;k<ne;k++)if(edges[2*k]>=0) {
            int a=edges[2*k],b=edges[2*k+1];graph.city.adjlist[fill[a]++]=b;graph.city.adjlist[fill[b]++]=a;
        }
    }
    int previous=-1;
    for(int k=0;ok && k<event->npoly;k++) {
        const float *point=event->poly[k];
        if(!isfinite(point[0]) || !isfinite(point[1])){ok=0;break;}
        int goal=world_nav_nearest(&graph,point[0],point[1]);
        if(goal<0 || hypotf(graph.city.nav[2*goal]-point[0],graph.city.nav[2*goal+1]-point[1])>120){ok=0;break;}
        int count=previous<0?1:world_route(&graph,previous,goal,route,n,NULL);
        if(previous<0)route[0]=goal;
        if(!count || route[count-1]!=goal){ok=0;break;}
        for(int j=0;ok && j<count;j++) {
            const float *p=graph.city.nav+2*route[j];
            if(path.n && hypotf(p[0]-path.xy[2*path.n-2],p[1]-path.xy[2*path.n-1])<.01f)continue;
            if(path.n==4096){ok=0;break;}
            path.xy[2*path.n]=p[0];path.xy[2*path.n++ +1]=p[1];
        }
        previous=goal;
    }
    ok=ok && ai_drive_route_valid(&path) &&
        (!event->circuit || hypotf(path.xy[0]-path.xy[2*path.n-2],path.xy[1]-path.xy[2*path.n-1])<.01f);
    free(data);free(edges);free(fill);free(route);free(graph.city.nav);
    free(graph.city.adjstart);free(graph.city.adjlist);
    if(!ok){free(path.xy);return 0;}
    *out=path;return 1;
}

/* Branch distances need not describe one continuous lap. Join the authored
   lane geometry where recorded road extents overlap, then reuse city routing
   through the ordered event outline. Coarse Paths remains the final fallback.
   ponytail: bounded quadratic junction census (4096 points, 16 edges/point);
   use the existing road index if larger event files require it. */
static int race_lane_topology(const char *root,const WEvent *event,N2Path *out) {
    if(!root || !event || !out || event->npoly<2 || event->npoly>WORLD_EVPOLY)return 0;
    for(int k=0;k<event->npoly;k++)if(!isfinite(event->poly[k][0]) || !isfinite(event->poly[k][1]))return 0;
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Routes%dF.bin",root,event->reg,event->id);
    long bytes=0;unsigned char *data=n2_read_file(file,&bytes);if(!data)return 0;
    AiRoadNet lanes={0};int cap=0,ok=ai_roads_read(&lanes,data,bytes,&cap,NULL,NULL,NULL);free(data);
    int n=lanes.n;ok=ok && n>1 && n<=4096;
    World graph={0};int *ends=ok?calloc(n,sizeof *ends):NULL,*edges=ok?malloc(n*32*sizeof *edges):NULL;
    int *fill=ok?malloc(n*sizeof *fill):NULL,*route=ok?malloc(n*sizeof *route):NULL;
    N2Path path={.xy=ok?malloc(4096*2*sizeof(float)):NULL};int ne=0;
    graph.city.nnav=n;graph.city.nav=lanes.xy;
    graph.city.adjstart=ok?calloc(n+1,sizeof(int)):NULL;
    graph.city.adjlist=ok?malloc(n*32*sizeof(int)):NULL;
    ok=ok && ends && edges && fill && route && path.xy && graph.city.adjstart && graph.city.adjlist;
    for(int i=0;ok && i<n;i++){if(i==0 || lanes.next[i-1]!=i)ends[i]=1;if(lanes.next[i]<0)ends[i]=1;}
    for(int i=0;ok && i<n;i++)for(int j=i+1;j<n;j++) {
        float d=hypotf(lanes.xy[2*i]-lanes.xy[2*j],lanes.xy[2*i+1]-lanes.xy[2*j+1]);
        if(lanes.next[i]!=j && (!(ends[i] || ends[j]) || d>fmaxf(5,lanes.half_width[i]+lanes.half_width[j])))continue;
        if(d>120 || ne>=n*16){ok=0;break;}
        edges[2*ne]=i;edges[2*ne++ +1]=j;graph.city.adjstart[i+1]++;graph.city.adjstart[j+1]++;
    }
    if(ok){for(int i=0;i<n;i++)graph.city.adjstart[i+1]+=graph.city.adjstart[i];graph.city.nadj=graph.city.adjstart[n];memcpy(fill,graph.city.adjstart,n*sizeof(int));
        for(int i=0;i<ne;i++){int a=edges[2*i],c=edges[2*i+1];graph.city.adjlist[fill[a]++]=c;graph.city.adjlist[fill[c]++]=a;}}
    int previous=-1;
    for(int k=0;ok && k<event->npoly;k++) {
        int goal=world_nav_nearest(&graph,event->poly[k][0],event->poly[k][1]);
        if(goal<0 || hypotf(lanes.xy[2*goal]-event->poly[k][0],lanes.xy[2*goal+1]-event->poly[k][1])>120){ok=0;break;}
        int count=previous<0?1:world_route(&graph,previous,goal,route,n,NULL);if(previous<0)route[0]=goal;
        if(!count || route[count-1]!=goal){ok=0;break;}
        for(int j=0;j<count;j++){int at=route[j];float x=lanes.xy[2*at],y=lanes.xy[2*at+1];
            /* A welded endpoint can lie just behind the incoming point.
               Keep one seam point so the driver cannot pin on that backstep. */
            if(j && lanes.next[route[j-1]]!=at && lanes.next[at]!=route[j-1] &&
               !(k==event->npoly-1 && j==count-1))continue;
            if(path.n && hypotf(x-path.xy[2*path.n-2],y-path.xy[2*path.n-1])<.01f)continue;
            if(path.n==4096){ok=0;break;}path.xy[2*path.n]=x;path.xy[2*path.n++ +1]=y;}
        previous=goal;
    }
    ok=ok && ai_drive_route_valid(&path) && (!event->circuit || hypotf(path.xy[0]-path.xy[2*path.n-2],path.xy[1]-path.xy[2*path.n-1])<.01f);
    free(ends);free(edges);free(fill);free(route);free(graph.city.adjstart);free(graph.city.adjlist);ai_roads_free(&lanes);
    if(!ok){free(path.xy);return 0;}*out=path;return 1;
}

int ai_race_course(const char *root,const WEvent *event,N2Path *out) {
    if(race_course_read(root,event,out,0,NULL,NULL,NULL))return 1;
    if(event && !event->circuit && race_course_read(root,event,out,1,NULL,NULL,NULL))return 1;
    if(event && event->circuit && race_lane_topology(root,event,out))return 1;
    return race_route_topology(root,event,out);
}

/* Paths is a coarse event spine. RoutesF contains the curved lane chains;
   use those samples where the loaded road and body collision support them.
   ponytail: greedy lane continuity, not an overtaking/global route planner. */
static void race_route_refine(const char *root,const WEvent *event,
        const AiTrafficWorld *world,const AiCar *car,const AiCar *grid,int count,N2Path *path,float *widest) {
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Routes%dF.bin",root,event->reg,event->id);
    long bytes=0;unsigned char *data=n2_read_file(file,&bytes);if(!data)return;
    AiRoadNet lanes={0};float *distance=NULL,*height=NULL;int capacity=0;
    int ok=ai_roads_read(&lanes,data,bytes,&capacity,&distance,&height,widest);free(data);
    if(!ok){ai_roads_free(&lanes);free(distance);free(height);return;}
    float length=0;for(int i=0;i<path->n-1;i++)length+=ai_segment(path,i);
    int points=(int)ceilf(length/5)+1;
    float *xy=points<=4095?malloc((size_t)points*2*sizeof *xy):NULL;
    if(!xy){ai_roads_free(&lanes);free(distance);free(height);return;}
    float before=0,lastz=car->pos[2];int segment=0,samples=0;
    for(int j=0;j<points;j++) {
        float along=fminf(length,j*5),error=along-before;
        while(segment<path->n-2 && error>ai_segment(path,segment)) {
            before+=ai_segment(path,segment);segment++;error=along-before;
        }
        float raw[2];ai_route_point(path,segment,error/ai_segment(path,segment),1,0,raw,NULL);
        float best=INFINITY,bx=raw[0],by=raw[1],bz=lastz;
        for(int k=0;k<lanes.n;k++) {
            int next=lanes.next[k];if(next<0)continue;
            float span=race_lane_span(&lanes,distance,k,0);
            if(fabsf(span)<.01f || along<fminf(distance[k],distance[next]) ||
               along>fmaxf(distance[k],distance[next]))continue;
            float t=(along-distance[k])/span;
            float ax=lanes.xy[2*k],ay=lanes.xy[2*k+1];
            float dx=lanes.xy[2*next]-ax,dy=lanes.xy[2*next+1]-ay;
            float x=ax+dx*t,y=ay+dy*t,z=lastz,deviation=hypotf(x-raw[0],y-raw[1]);
            if(deviation>30 || hypotf(dx,dy)<.01f)continue;
            float direction=span>0?1:-1,heading=atan2f(dy*direction,dx*direction);
            if(world_ground_at(world->scene,x,y,z,&z)!=WSURF_ROAD ||
               !race_pose_clear(world,car,x,y,z,heading))continue;
            float cost=hypotf(deviation,z-(height[k]+t*(height[next]-height[k])));
            if(j) {
                float px=xy[2*j-2],py=xy[2*j-1],zz=lastz;
                cost+=2*hypotf(x-px,y-py);int clear=1;
                float h=atan2f(y-py,x-px);
                int steps=(int)ceilf(hypotf(x-px,y-py));
                for(int q=1;q<=steps && clear;q++) {
                    float sx=px+(x-px)*q/steps,sy=py+(y-py)*q/steps;
                    clear=world_ground_at(world->scene,sx,sy,zz,&zz)==WSURF_ROAD &&
                          race_pose_clear(world,car,sx,sy,zz,h);
                }
                if(!clear)continue;
            }
            if(cost<best){best=cost;bx=x;by=y;bz=z;}
        }
        if(isfinite(best))samples++;
        else world_ground_at(world->scene,bx,by,lastz,&bz);
        xy[2*j]=bx;xy[2*j+1]=by;lastz=bz;
    }
    if(event->circuit){xy[2*points-2]=xy[0];xy[2*points-1]=xy[1];}
    N2Path refined={xy,points};ok=samples && ai_drive_route_valid(&refined);
    for(int k=0;k<count && ok;k++) {
        AiDrive drive;ok=ai_drive_init(&drive,&refined,grid[k].pos,grid[k].head);
    }
    if(ok) {
        free(path->xy);*path=refined;
        printf("race lanes: %d/%d samples from authored event corridors\n",samples,points);
    } else free(xy);
    ai_roads_free(&lanes);free(distance);free(height);
}

static int race_nearest_segment(const N2Path *p,const float pos[3]) {
    float best=INFINITY;int segment=0;
    for(int i=0;i<p->n-1;i++) {
        float error;ai_projection(p,i,pos,&error);
        if(error<best){best=error;segment=i;}
    }
    return segment;
}

float ai_course_heading(const N2Path *path,const float pos[3]) {
    if(!path || path->n<2 || !path->xy || !pos)return 0;
    int i=race_nearest_segment(path,pos);
    return atan2f(path->xy[2*i+3]-path->xy[2*i+1],path->xy[2*i+2]-path->xy[2*i]);
}

float ai_course_lateral(const N2Path *path,const float pos[3]) {
    if(!path || path->n<2 || !path->xy || !pos)return 0;
    int i=race_nearest_segment(path,pos);float error,t=ai_projection(path,i,pos,&error);
    float x=path->xy[2*i]+t*(path->xy[2*i+2]-path->xy[2*i]);
    float y=path->xy[2*i+1]+t*(path->xy[2*i+3]-path->xy[2*i+1]);
    float h=ai_course_heading(path,pos);
    return -(pos[0]-x)*sinf(h)+(pos[1]-y)*cosf(h);
}

static float race_lane_width(const char *root,const WEvent *event) {
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Routes%dF.bin",root,event->reg,event->id);
    long n=0;unsigned char *data=n2_read_file(file,&n);if(!data)return 0;
    AiRoadNet lanes={0};int capacity=0;float widest=0;
    int ok=ai_roads_read(&lanes,data,n,&capacity,NULL,NULL,&widest);
    free(data);ai_roads_free(&lanes);return ok?widest:0;
}

/* Numbered source slots describe rows; select a whole formation before moving
   anyone. This also excludes a clear front row whose rear player lacks support. */
static int race_grid_try(const WEvent *event,const WRace *race,const AiTrafficWorld *world,
        const N2Path *course,float corridor,AiCar *cars,AiCar *player,
        const int *slots,int count,int columns) {
    float forward[N_RACE_AI+1],side[N_RACE_AI+1];
    float c=cosf(race->grid_heading),s=sinf(race->grid_heading);
    const float *origin=race->grid[slots[0]];
    for(int k=0;k<count;k++) {
        const float *p=race->grid[slots[k]];
        float x=p[0]-origin[0],y=p[1]-origin[1];
        forward[k]=x*c+y*s;side[k]=-x*s+y*c;
        int row=k/columns,col=k%columns;
        /* Authored rows can be slightly skewed, but not a single-file queue. */
        if(fabsf(forward[k]-forward[row*columns])>.75f ||
           (row && (forward[row*columns]>=forward[(row-1)*columns]-.75f ||
                    fabsf(side[k]-side[col])>.75f)))return 0;
    }
    AiCar field[N_RACE_AI+1];
    for(int k=0;k<count;k++) {
        field[k]=k==count-1?*player:cars[k];
        memcpy(field[k].pos,race->grid[slots[k]],sizeof field[k].pos);
        field[k].head=race->grid_heading;field[k].spd=0;field[k].ride_ready=0;
        float z=field[k].pos[2];AiDrive join;
        if(!world_race_grid_ground(world->scene,event,field[k].pos,&z))return 0;
        field[k].pos[2]=z;
        /* Use the live race driver's entry envelope: an authored outer slot
           can lie beyond the lane centre's half-width on supported pavement. */
        float entry=fmaxf(12,fminf(60,corridor+field[k].half_width));
        if(!race_pose_clear(world,&field[k],field[k].pos[0],field[k].pos[1],z,field[k].head) ||
           !ai_drive_join(&join,course,field[k].pos,field[k].head,entry))return 0;
        for(int j=0;j<k;j++)if(phys_ai_overlap(&field[k],&field[j]))return 0;
        ai_motion_init(&field[k],world);if(field[k].ride.contact_mask!=15)return 0;
    }
    memcpy(cars,field,(count-1)*sizeof *cars);*player=field[count-1];
    printf("race grid: %d rows x %d, player source slot %d at (%.3f %.3f %.3f), heading %.4f\n",
        count/columns,columns,slots[count-1],player->pos[0],player->pos[1],player->pos[2],player->head);
    return 1;
}

static int race_grid_search(const WEvent *event,const WRace *race,const AiTrafficWorld *world,
        const N2Path *course,float corridor,AiCar *cars,AiCar *player,
        int *slots,int count,int columns,int depth,int first) {
    if(depth==count)return race_grid_try(event,race,world,course,corridor,cars,player,slots,count,columns);
    for(int k=first;k<=race->ngrid-(count-depth);k++) {
        slots[depth]=k;
        if(race_grid_search(event,race,world,course,corridor,cars,player,slots,count,columns,depth+1,k+1))return 1;
    }
    return 0;
}

/* Match the grid's observed plane to nearby authored lanes, including road bank. */
static int race_corridor_grid_layer(const AiRoadNet *lanes,const float *height,N2Scene *scene,
        float x,float y,float z,float margin) {
    float ground=z,normal[3];
    if(!world_ground_pose(scene,x,y,z,&ground,normal) || ground-z>PHYS_RIDE_REACH_UP ||
       z-ground>PHYS_RIDE_REACH_DOWN || normal[2]<.7f)return 0;
    for(int i=0;i<lanes->n;i++){
        int next=lanes->next[i];if(next<0)continue;
        float dx=lanes->xy[2*next]-lanes->xy[2*i],dy=lanes->xy[2*next+1]-lanes->xy[2*i+1],len2=dx*dx+dy*dy;if(len2<.0001f)continue;
        float t=fmaxf(0,fminf(1,((x-lanes->xy[2*i])*dx+(y-lanes->xy[2*i+1])*dy)/len2));
        float sx=lanes->xy[2*i]+t*dx,sy=lanes->xy[2*i+1]+t*dy,extent=lanes->half_width[i]+t*(lanes->half_width[next]-lanes->half_width[i]);
        if(hypotf(x-sx,y-sy)>extent+margin)continue;
        float authored=height[i]+t*(height[next]-height[i]);
        float projected=ground-(normal[0]*(sx-x)+normal[1]*(sy-y))/normal[2];
        if(fabsf(projected-authored)<=1)return 1;
    }
    return 0;
}

/* Recover a source spine whose branch-progress records cannot form a course.
   Lane extents bound the search; actual supports and solids validate each join. */
static int race_corridor_course(const char *root,const WEvent *event,const AiTrafficWorld *world,
        const AiCar *body,const AiCar *cars,int count,const AiCar *player,const WRace *race,N2Path *path,float widest) {
    N2Path progress={0};
    int decoded=race_course_read(root,event,&progress,0,NULL,NULL,NULL) ||
        (!event->circuit && race_course_read(root,event,&progress,1,NULL,NULL,NULL));
    free(progress.xy);
    if(decoded)return 0; /* A sound source course rejected by the world is not a progress-map failure. */
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Routes%dF.bin",root,event->reg,event->id);
    long bytes=0;unsigned char *data=n2_read_file(file,&bytes);if(!data)return 0;
    AiRoadNet lanes={0};float *height=NULL;int cap=0,ok=ai_roads_read(&lanes,data,bytes,&cap,NULL,&height,NULL);free(data);
    for(int k=-1;ok && k<count;k++){
        const AiCar *actor=k<0?player:cars+k;
        ok=race_corridor_grid_layer(&lanes,height,world->scene,actor->pos[0],actor->pos[1],actor->pos[2],actor->half_width);
    }
    float length=0;for(int i=0;i+1<path->n;i++)length+=ai_segment(path,i);
    float spacing=body->half_width+1.2f;int sides=(int)floorf(fminf(30,widest)/spacing),cols=2*sides+1,rows=(int)ceilf(length/5)+1;
    typedef struct {float xy[2],z;double cost;int parent;} Cell;
    Cell *cells=ok && sides>0 && cols<=63 && rows>1 && rows<=4096?calloc((size_t)rows*cols,sizeof *cells):NULL;
    float *xy=cells?malloc((size_t)rows*3*sizeof *xy):NULL;ok=cells && xy;
    float grid_offset=ai_course_lateral(path,player->pos);for(int i=0;i<count;i++)grid_offset+=ai_course_lateral(path,cars[i].pos);grid_offset/=count+1;
    for(int j=0;ok && j<rows;j++){
        float along=fminf(length,j*5),point[2],heading;ai_route_point(path,0,0,1,along,point,&heading);
        float best=INFINITY;
        for(int i=0;i<lanes.n;i++){int next=lanes.next[i];if(next<0)continue;
            float dx=lanes.xy[2*next]-lanes.xy[2*i],dy=lanes.xy[2*next+1]-lanes.xy[2*i+1],len2=dx*dx+dy*dy;if(len2<.0001f)continue;
            float t=fmaxf(0,fminf(1,((point[0]-lanes.xy[2*i])*dx+(point[1]-lanes.xy[2*i+1])*dy)/len2));
            float error=hypotf(point[0]-lanes.xy[2*i]-t*dx,point[1]-lanes.xy[2*i+1]-t*dy);
            if(error<best)best=error;
        }
        if(best>30){ok=0;break;}
        int reachable=0;
        for(int c=0;c<cols;c++){
            Cell *q=cells+j*cols+c;float offset=(c-sides)*spacing;
            q->xy[0]=point[0]-sinf(heading)*offset;q->xy[1]=point[1]+cosf(heading)*offset;q->z=player->pos[2];q->cost=INFINITY;q->parent=-1;
            if(!j){
                if(!race_ground_at(world->scene,q->xy[0],q->xy[1],player->pos[2],&q->z) || !race_pose_clear(world,body,q->xy[0],q->xy[1],q->z,heading))continue;
                q->cost=(offset-grid_offset)*(offset-grid_offset);reachable++;continue;
            }
            for(int c0=c-1;c0<=c+1;c0++)if(c0>=0 && c0<cols){
                Cell *a=cells+(j-1)*cols+c0;if(!isfinite(a->cost))continue;
                float dx=q->xy[0]-a->xy[0],dy=q->xy[1]-a->xy[1],distance=hypotf(dx,dy),h=atan2f(dy,dx),z=a->z;
                double cost=a->cost+distance+.01f*offset*offset+.2f*(c-c0)*(c-c0);if(cost>=q->cost)continue;
                int steps=fmaxf(1,ceilf(distance)),clear=1;
                for(int k=0;k<=steps && clear;k++){
                    float t=(float)k/steps,x=a->xy[0]+dx*t,y=a->xy[1]+dy*t;
                    float oldz=z;clear=race_ground_at(world->scene,x,y,z,&z)!=WSURF_NONE && fabsf(z-oldz)<=distance/steps*sqrtf(1-.7f*.7f)/.7f+PHYS_RIDE_REACH_UP && race_pose_clear(world,body,x,y,z,h);
                }
                if(clear){q->cost=cost;q->parent=c0;q->z=z;}
            }
            reachable+=isfinite(q->cost);
        }
        if(!reachable)ok=0;
    }
    int end=-1;double best=INFINITY;
    if(ok)for(int c=0;c<cols;c++){Cell *q=cells+(rows-1)*cols+c;if(q->cost<best){best=q->cost;end=c;}}
    ok=ok && end>=0;
    for(int j=rows-1;ok && j>=0;j--){Cell *q=cells+j*cols+end;xy[2*j]=q->xy[0];xy[2*j+1]=q->xy[1];xy[2*rows+j]=q->z;end=q->parent;}
    N2Path candidate={xy,rows};
    AiCar envelope=*body;float worst=envelope.vehicle.accel>0?envelope.vehicle.steer:1;
    for(int k=-1;k<count;k++){const AiCar *actor=k<0?player:cars+k;float steer=actor->vehicle.accel>0?actor->vehicle.steer:1;if(steer<worst){worst=steer;envelope.vehicle=actor->vehicle;}}
    if(ok)race_course_round(world,&envelope,&candidate,xy+2*rows);
    int kept=1;for(int j=1;ok && j<rows;j++)if(hypotf(xy[2*j]-xy[2*kept-2],xy[2*j+1]-xy[2*kept-1])>=.01f){xy[2*kept]=xy[2*j];xy[2*kept+1]=xy[2*j+1];kept++;}candidate.n=kept;
    ok=ok && ai_drive_route_valid(&candidate);
    for(int k=-1;ok && k<count;k++){
        const AiCar *actor=k<0?player:cars+k;AiDrive join;
        float entry=race->grid_named?fmaxf(12,fminf(60,widest+actor->half_width)):(widest>0?widest:8);
        ok=ai_drive_join(&join,&candidate,actor->pos,actor->head,entry);
    }
    if(ok){free(path->xy);*path=candidate;xy=NULL;printf("race corridor: %d supported source-width samples\n",path->n);}
    free(xy);free(cells);ai_roads_free(&lanes);free(height);return ok;
}

int ai_race_prepare(const char *root,const WEvent *event,WRace *race,
        const AiTrafficWorld *world,N2Path *path,AiCar cars[N_RACE_AI],AiRace drivers[N_RACE_AI],
        AiCar *player) {
    free(path->xy);*path=(N2Path){0};
    memset(drivers,0,N_RACE_AI*sizeof *drivers);
    N2Path course={0};
    int rawcourse=event && root && race_route_load(root,event,&course);
    if(!player || !event || !race || !race->active || race->ngate<2 || !world || !world->scene ||
       (!rawcourse && !ai_race_course(root,event,&course))) {
        free(course.xy);printf("race opponents: continuous course unavailable; route topology needs decoding\n");return 0;
    }
    int source_supported=rawcourse;
    float widest=race_lane_width(root,event);
    int wanted=event->info.kind==N2_RACE_URL ? 5 :
        event->info.kind==N2_RACE_UNKNOWN ? N_AI : 3;
    if(!rawcourse) {
        AiCar body=*player;
        /* The field leaves along the authored grid heading. */
        if(race->grid_named && isfinite(race->grid_heading))body.head=race->grid_heading;
        for(int k=0;k<wanted;k++) {
            body.half_length=fmaxf(body.half_length,cars[k].half_length);
            body.half_width=fmaxf(body.half_width,cars[k].half_width);
            body.height=fmaxf(body.height,cars[k].height);
        }
        N2Path supported={0};
        if(race_course_read(root,event,&supported,0,world,&body,&widest) ||
           (!event->circuit && race_course_read(root,event,&supported,1,world,&body,&widest))) {
            free(course.xy);course=supported;source_supported=1;
        }
    }
    if(event->info.downhill) {
        *path=course;
        if(!world_race_bind_course(race,path,fmaxf(22,widest+player->half_width)) ||
           !ai_drive_join(&drivers[0].drive,path,player->pos,ai_course_heading(path,player->pos),widest>0?widest:8)) {
            free(path->xy);*path=(N2Path){0};
        } else {
            /* Zero opponents. This borrowed player driver lets the same
               physics/checkpoint checker exercise solo downhill layouts. */
            drivers[0].progress=*race;
            drivers[0].drive.error_limit=fmaxf(12,fminf(60,widest+player->half_width));
        }
        return 0;
    }
    int used[WORLD_MAXGRID]={0},count=0;
    for(int attempt=0;attempt<2;attempt++) {
        if(race->grid_named && race->ngrid>0 && race->ngrid<=WORLD_MAXGRID &&
           isfinite(race->grid_heading)) {
            int slots[N_RACE_AI+1],total=wanted+1;
            if((total==4 && race_grid_search(event,race,world,&course,widest>0?widest:8,
                    cars,player,slots,total,4,0,0)) ||
               ((total==4 || total==6) && race_grid_search(event,race,world,&course,widest>0?widest:8,
                    cars,player,slots,total,2,0,0)))count=wanted;
            for(int k=0;k<count;k++) {
                cars[k].t=cars[k].lap=cars[k].prevrel=0;cars[k].braking=0;
                drivers[k]=(AiRace){.progress=*race};
                cars[k].col[k%3]=.85f;cars[k].col[(k+1)%3]=.25f;
            }
        }
        for(int k=0;!race->grid_named && k<wanted;k++) {
            AiCar candidate;int best=-1;float bestd=INFINITY;
            for(int slot=0;slot<race->ngrid;slot++) {
                if(used[slot])continue;
                const float *grid=race->grid[slot];
                float d=hypotf(grid[0]-player->pos[0],grid[1]-player->pos[1]);
                if(d>60 || d<3 || d>=bestd)continue;
                float z=grid[2];
                if(!world_race_grid_ground(world->scene,event,grid,&z))continue;
                candidate=cars[k];memcpy(candidate.pos,grid,sizeof candidate.pos);candidate.pos[2]=z;
                int at=race_nearest_segment(&course,candidate.pos),next=(at+1)%course.n;
                candidate.head=atan2f(course.xy[2*next+1]-course.xy[2*at+1],course.xy[2*next]-course.xy[2*at]);
                candidate.spd=0;candidate.ride_ready=0;
                AiDrive ready;if(!ai_drive_join(&ready,&course,candidate.pos,candidate.head,widest>0?widest:8))continue;
                if(!race_pose_clear(world,&candidate,candidate.pos[0],candidate.pos[1],z,candidate.head))continue;
                int overlaps=0;
                if(phys_ai_overlap(&candidate,player))overlaps=1;
                for(int j=0;j<count;j++)if(phys_ai_overlap(&candidate,&cars[j]))overlaps=1;
                if(overlaps)continue;
                ai_motion_init(&candidate,world);if(candidate.ride.contact_mask!=15)continue;
                cars[k]=candidate;best=slot;bestd=d;
            }
            if(best<0)break;
            used[best]=1;cars[k].t=cars[k].lap=cars[k].prevrel=0;cars[k].braking=0;
            drivers[k]=(AiRace){.progress=*race};
            cars[k].col[k%3]=.85f;cars[k].col[(k+1)%3]=.25f;
            count++;
        }
        if(count || attempt)break;
        /* Keep a course with usable grid slots. A distance-decoded course on
           another road may instead join through the existing Paths graph.
           Retry the same support, body, overlap and wheel-contact checks. */
        N2Path linked={0};AiDrive join;
        if(!race_route_topology(root,event,&linked))break;
        if(!ai_drive_join(&join,&linked,player->pos,ai_course_heading(&linked,player->pos),widest>0?widest:8)) {
            free(linked.xy);break;
        }
        free(course.xy);course=linked;rawcourse=1;
    }
    if(!count){*path=course;printf("race opponents: no supported clear grid slots\n");return 0;}
    *path=course;
    AiCar envelope=cars[0];
    for(int k=1;k<count;k++) {
        envelope.half_length=fmaxf(envelope.half_length,cars[k].half_length);
        envelope.half_width=fmaxf(envelope.half_width,cars[k].half_width);
        envelope.height=fmaxf(envelope.height,cars[k].height);
    }
    if(!source_supported && !event->circuit && event->info.kind!=N2_RACE_UNKNOWN &&
       race_corridor_course(root,event,world,&envelope,cars,count,player,race,path,widest))rawcourse=0;
    if(rawcourse)race_route_refine(root,event,world,&envelope,cars,count,path,&widest);
    if(event->info.kind==N2_RACE_STREETX)
        race_street_course(root,event,world,&envelope,cars,count,player,path,race,widest);
    if(!world_race_bind_course(race,path,fmaxf(22,widest+envelope.half_width))) {
        printf("race opponents: no usable checkpoint sequence on the course\n");return 0;
    }
    /* The spine can end ON the finish plane. Reserve room for the field to
       roll through it before braking; normal support/collision still apply. */
    const WGate *finish=&race->gate[race->ngate-1];int last=path->n-1;
    float run=count*(2*envelope.half_length+1.2f)+2,gl=hypotf(finish->dx,finish->dy);
    float dx=path->xy[2*last]-path->xy[2*last-2],dy=path->xy[2*last+1]-path->xy[2*last-1];
    if(!event->circuit && path->n<4096 && gl>.01f && dx*finish->dx+dy*finish->dy>0 &&
       hypotf(path->xy[2*last]-finish->x,path->xy[2*last+1]-finish->y)<envelope.half_length+2) {
        /* A projected finish can sit beside the map edge: keep only supported run-out. */
        float len=hypotf(dx,dy),safe=run;
        if(world && world->scene) {
            float z=envelope.pos[2];safe=0;
            world_ground_at(world->scene,path->xy[2*last],path->xy[2*last+1],z,&z);
            for(float ahead=.5f;ahead<=run+.5f;ahead+=.5f) {
                float step=fminf(ahead,run),x=path->xy[2*last]+dx/len*step,y=path->xy[2*last+1]+dy/len*step;
                if(!world_ground_at(world->scene,x,y,z,&z) || !race_pose_clear(world,&envelope,x,y,z,atan2f(dy,dx)))break;
                safe=step;if(step==run)break;
            }
        }
        float *xy=safe>.5f?realloc(path->xy,(size_t)(path->n+1)*2*sizeof *xy):NULL;
        if(xy) {
            path->xy=xy;xy[2*path->n]=xy[2*last]+dx/len*safe;
            xy[2*path->n+1]=xy[2*last+1]+dy/len*safe;path->n++;
        }
    }
    int source_profile=world && world->race_difficulty>=0 && world->race_difficulty<3
        ?race_ai_source_select(world->race_source,event->id,wanted,world->race_source_choice):-1;
    for(int k=0;k<count;k++) {
        drivers[k].progress=*race;
        if(source_profile>=0){
            const RaceAiSourceDriver *v=world->race_source->event[source_profile].driver+k;
            drivers[k].source=world->race_source;
            drivers[k].source_skill=v->skill[world->race_difficulty];
            drivers[k].source_scale=world->race_source->skill_scale[drivers[k].source_skill];
            drivers[k].source_catch_up=v->catch_up;
        }
        drivers[k].offset=ai_course_lateral(path,cars[k].pos);
        float entry=race->grid_named?fmaxf(12,fminf(60,widest+cars[k].half_width)):(widest>0?widest:8);
        ai_drive_join(&drivers[k].drive,path,cars[k].pos,cars[k].head,entry);
        /* ponytail: widest authored side for this event; local widths can
           replace this bound when per-segment corridor planning is added.
           Physical road/body checks remain authoritative on the narrower side. */
        drivers[k].drive.error_limit=fmaxf(12,fminf(60,widest+cars[k].half_width));
    }
    printf("race opponents: event %d, %d cars at rest on authored grid, %d continuous route nodes\n",event->id,count,path->n);
    return count;
}

/* Follow the reachable rising ramp that the tyres use, rather than its baked floor. */
static int race_ground_at(N2Scene *scene,float x,float y,float ref,float *z) {
    WGroundHit hit;
    int cat=world_ground_at(scene,x,y,ref,z);
    int reachable=world_wheel_support(scene,x,y,ref,PHYS_RIDE_REACH_UP,PHYS_RIDE_REACH_DOWN,&hit,NULL,NULL);
    if(reachable && hit.z>ref && hit.normal[2]>=.7f && fabsf(hit.normal[0])+fabsf(hit.normal[1])>hit.normal[2]*.005f) {
        *z=hit.z;return reachable;
    }
    return cat;
}

float ai_drag_lane_target(const AiTrafficWorld *world,const AiCar *car,const AiDrive *line,
        float current,const float *lanes,int count,int direction) {
    if(!car || !line || !line->path || line->path->n<2 || !isfinite(current) ||
       line->segment<0 || line->segment>=line->path->n-1)return current;
    count=count<0?0:count>WORLD_MAXGRID?WORLD_MAXGRID:count;
    direction=direction>0?1:direction<0?-1:0;
    float desired=current,best=INFINITY;
    for(int k=0;lanes && k<count;k++) {
        float delta=(lanes[k]-current)*direction;
        if(isfinite(lanes[k]) && delta>.5f && delta<best){best=delta;desired=lanes[k];}
    }
    if(direction && !isfinite(best))desired=current+.5f*direction;
    float candidates[WORLD_MAXGRID+2]={desired,current};int n=2;
    for(int k=0;lanes && k<count && n<WORLD_MAXGRID+2;k++)
        if(isfinite(lanes[k]))candidates[n++]=lanes[k];
    float error,t=ai_projection(line->path,line->segment,car->pos,&error),answer=current;best=INFINITY;
    for(int k=0;k<n;k++) {
        float cost=fabsf(candidates[k]-desired);
        if(cost>=best || fabsf(candidates[k])>line->error_limit)continue;
        float z=car->pos[2];int clear=1;
        float preview=fmaxf(12,fabsf(car->spd)*PHYS_TICKRATE*1.5f);
        for(float ahead=0;clear && ahead<=preview;ahead+=2) {
            float point[2],h;
            ai_route_point(line->path,line->segment,t,line->direction,ahead,point,&h);
            point[0]-=sinf(h)*line->direction*candidates[k];
            point[1]+=cosf(h)*line->direction*candidates[k];
            clear=!world || !world->scene ||
                (race_ground_at(world->scene,point[0],point[1],z,&z)!=WSURF_NONE &&
                 race_pose_clear(world,car,point[0],point[1],z,h));
        }
        if(clear){best=cost;answer=candidates[k];}
    }
    return answer;
}

/* Rival station on the racer's own course, searched only near our station:
   route distance ahead of us and signed lateral (path-left, like offsets),
   both on the rival's local tangent so bends cannot mix frames.
   Returns the distance from the course; INFINITY when nothing was searched. */
static float race_course_relative(const N2Path *p,int at,float t,int direction,float back,
        float range,const float pos[3],float *ahead,float *lateral,float *heading) {
    int loop=hypotf(p->xy[0]-p->xy[2*p->n-2],p->xy[1]-p->xy[2*p->n-1])<.01f,i=at;
    float s=-(direction>0?t:1-t)*ai_segment(p,at),best=INFINITY;
    while(s>-back) {
        int prev=i-direction;
        if(prev<0 || prev>p->n-2){if(!loop)break;prev=direction>0?p->n-2:0;}
        i=prev;s-=ai_segment(p,i);
    }
    for(int steps=0;s<=range && steps<p->n;steps++) {
        float len=ai_segment(p,i),error,u=ai_projection(p,i,pos,&error);
        if(error<best) {
            float dx=p->xy[2*i+2]-p->xy[2*i],dy=p->xy[2*i+3]-p->xy[2*i+1];
            best=error;*ahead=s+(direction>0?u:1-u)*len;
            *lateral=((pos[1]-p->xy[2*i+1])*dx-(pos[0]-p->xy[2*i])*dy)/len;
            *heading=atan2f(dy*direction,dx*direction);
        }
        s+=len;i+=direction;
        if(i<0 || i>p->n-2){if(!loop)break;i=direction>0?0:p->n-2;}
    }
    return best;
}

/* A negative race pace drives each car at its own flat-out speed; a positive
   one is the old uniform test pace, staggered per racer. Zero stays the
   conservative diagnostic pace. */
/* Checkpoint-distance proxy keeps lap/armed-gate order without route searches.
   shortcut: distances between gates are chords; use cached course arcs for precise gaps. */
static float race_remaining(const WRace *r,const float pos[3],int loop){
    if(!r || !r->active || r->ngate<2 || r->ngate>WORLD_MAXGATE ||
       r->next<0 || r->next>=r->ngate || !isfinite(pos[0]) || !isfinite(pos[1]) ||
       r->maxlaps<1 || r->lap<0 || r->lap>r->maxlaps)return NAN;
    float left=hypotf(pos[0]-r->gate[r->next].x,pos[1]-r->gate[r->next].y),length=0;
    for(int j=0;j<r->ngate;j++){
        int next=(j+1)%r->ngate;
        if(!loop && next==0)break;
        float d=hypotf(r->gate[next].x-r->gate[j].x,r->gate[next].y-r->gate[j].y);
        length+=d;if((!loop || r->next!=0) && j>=r->next)left+=d;
    }
    if(loop)left+=(r->maxlaps-r->lap)*length;
    return left;
}
static int race_source_near_course(const AiDrive *d,const float pos[3]){
    const N2Path *p=d->path;
    if(!p || !p->xy || p->n<2 || p->n>4096 || !isfinite(pos[0]) || !isfinite(pos[1]))return 0;
    float limit=isfinite(d->error_limit) && d->error_limit>=12 && d->error_limit<=60?d->error_limit:12;
    for(int j=0;j<p->n-1;j++){
        float x=p->xy[2*j],y=p->xy[2*j+1],dx=p->xy[2*j+2]-x,dy=p->xy[2*j+3]-y;
        float len2=dx*dx+dy*dy;if(len2<.0001f)continue;
        float t=fmaxf(0,fminf(1,((pos[0]-x)*dx+(pos[1]-y)*dy)/len2));
        float ex=pos[0]-x-t*dx,ey=pos[1]-y-t*dy;
        if(ex*ex+ey*ey<=limit*limit)return 1;
    }
    return 0;
}
float ai_race_pace(const AiCar *car,const AiRace *driver,int k,
        const AiTrafficWorld *world,const AiCar *player){
    float pace=world && isfinite(world->race_pace) && world->race_pace>0
        ?fmaxf(40,fminf(180,world->race_pace+4*k)):50;
    if(world && isfinite(world->race_pace) && world->race_pace<0 && car)
        pace=PHYS_KMH(phys_vehicle_top_speed(car->vehicle.accel>0?&car->vehicle:NULL));
    if(!driver || !world || !driver->source || driver->source!=world->race_source ||
       !isfinite(driver->source_scale) || driver->source_scale<=0 || driver->source_scale>2)return pace;
    pace*=driver->source_scale;
    const WRace *r=world->race_player;
    if(car && player && driver->source_catch_up && r && r->active && !r->finished &&
       r->ev==driver->progress.ev && r->ngate==driver->progress.ngate && r->maxlaps==driver->progress.maxlaps &&
       race_source_near_course(&driver->drive,player->pos) && race_source_near_course(&driver->drive,car->pos)){
        float gap=race_remaining(r,player->pos,driver->drive.loop)-
                  race_remaining(&driver->progress,car->pos,driver->drive.loop);
        pace*=1+race_ai_source_curve(driver->source,gap);
    }
    return pace;
}

/* Source route points supply the trajectory. This distance-window tangent
   keeps a held lane continuous across polyline vertices (no normal jump).
   Control equations below are OpenUG2 adapters, not recovered retail PID. */
static void race_line_point(const AiDrive *d,float t,float distance,float offset,
                            float point[2],float *heading) {
    float a[2],b[2],h;
    ai_route_point(d->path,d->segment,t,d->direction,distance,point,&h);
    float before=distance-5;
    ai_route_point(d->path,d->segment,t,before<0?-d->direction:d->direction,fabsf(before),a,NULL);
    ai_route_point(d->path,d->segment,t,d->direction,distance+5,b,NULL);
    if(hypotf(b[0]-a[0],b[1]-a[1])>.01f)h=atan2f(b[1]-a[1],b[0]-a[0]);
    point[0]-=sinf(h)*d->direction*offset;
    point[1]+=cosf(h)*d->direction*offset;
    if(heading)*heading=h;
}

/* Pure-pursuit curvature uses the actual target distance, then the existing
   chassis supplies its yaw authority. The probe touches only local copies. */
static float race_line_steer(const AiCar *car,const float point[2]) {
    float dx=point[0]-car->pos[0],dy=point[1]-car->pos[1],d2=dx*dx+dy*dy;
    if(d2<.01f)return 0;
    float speed=fmaxf(.01f,hypotf(car->vel[0],car->vel[1]));
    if(!car->ride_ready)speed=fmaxf(.01f,fabsf(car->spd));
    float across=cosf(car->head)*dy-sinf(car->head)*dx;
    /* A spin can put the target directly behind us, where pursuit curvature
       vanishes. Turn back explicitly; the caller limits recovery speed. */
    if(cosf(car->head)*dx+sinf(car->head)*dy<0)return across>=0?1:-1;
    float curvature=2*across/d2;
    float pos[3]={0},vel[2]={speed,0},heading=0,forward=speed;
    phys_stage1_chassis_step(pos,vel,&heading,&forward,1,0,&PHYS_SURF_ROAD,
                             car->vehicle.accel>0?&car->vehicle:NULL);
    return heading>1e-6f?fmaxf(-1,fminf(1,speed*curvature/heading)):0;
}

/* Preview the steering demand on the selected lane. An authored polyline
   vertex has no drivable instantaneous radius: use the same pursuit target
   distance as steering, rather than braking for its unsmoothed vertex. */
static float race_corner_limit(const AiDrive *d,const AiCar *car,float offset,float look) {
    const PhysVehicle *vh=car->vehicle.accel>0?&car->vehicle:NULL;
    float error,t=ai_projection(d->path,d->segment,car->pos,&error);
    float brake2=2*RACE_BRAKE_SHARE*phys_vehicle_brake_decel(vh),speed=fabsf(car->spd);
    float reach=fminf(300,speed*speed/brake2+20),limit=INFINITY;
    /* Cap recovery toward the current lane tangent, not normal turn-in toward
       its future bend. A straight exit can still need a tight recovery turn. */
    float point[2],heading;race_line_point(d,t,0,offset,point,&heading);
    race_line_point(d,t,look,offset,point,NULL);
    float dx=point[0]-car->pos[0],dy=point[1]-car->pos[1];
    float across=cosf(car->head)*dy-sinf(car->head)*dx;
    if(across*sinf(heading-car->head)>0 && fabsf(across)>.001f) {
        float radius=(dx*dx+dy*dy)/(2*fabsf(across));
        limit=fmaxf(14/3.6f/PHYS_TICKRATE,.95f*phys_vehicle_corner_speed(vh,radius));
    }
    for(float ahead=0;ahead<=reach;ahead+=4) {
        float a[2],b[2],heading;
        race_line_point(d,t,ahead,offset,a,&heading);
        race_line_point(d,t,ahead+look,offset,b,NULL);
        float dx=b[0]-a[0],dy=b[1]-a[1],distance2=dx*dx+dy*dy;
        float cross=fabsf(cosf(heading)*dy-sinf(heading)*dx);
        if(cross<.001f || distance2<.001f)continue;
        float radius=distance2/(2*cross);
        /* Leave braking/control margin, without shrinking the road radius.
           .95 measured across 15 Hard/Normal/Easy events: 3.6% faster, all
           finishing; raising RACE_BRAKE_SHARE instead overshot corners. */
        float corner=fmaxf(14/3.6f/PHYS_TICKRATE,.95f*phys_vehicle_corner_speed(vh,radius));
        limit=fminf(limit,sqrtf(corner*corner+brake2*ahead));
    }
    return limit*PHYS_TICKRATE;
}



/* Gravity-bound speed on turning crests, using the authored ground profile. */
static float race_crest_limit(const AiTrafficWorld *world,const AiDrive *d,const AiCar *car,float offset,float pace) {
    typedef struct {float xy[2],z,along,heading;} Sample;
    Sample samples[77];int n=0;float error,t=ai_projection(d->path,d->segment,car->pos,&error),z=car->pos[2];
    /* One height walk keeps the active road layer through overlapping decks. */
    for(float ahead=0;ahead<=300 && n<77;ahead+=4) {
        Sample q={.z=z};race_line_point(d,t,ahead,offset,q.xy,&q.heading);
        if(race_ground_at(world->scene,q.xy[0],q.xy[1],z,&q.z)==WSURF_NONE || !isfinite(q.z))break;
        if(n){float step=hypotf(q.xy[0]-samples[n-1].xy[0],q.xy[1]-samples[n-1].xy[1]);
            if(step<.01f)break;
            q.along=samples[n-1].along+step;}
        samples[n++]=q;z=q.z;
    }
    const PhysVehicle *vh=car->vehicle.accel>0?&car->vehicle:NULL;
    float speed=fabsf(car->spd)*PHYS_TICKRATE;
    float launch=fmaxf(speed,fminf(pace/3.6f,phys_vehicle_top_speed(vh)*PHYS_TICKRATE));
    float brake2=2*RACE_BRAKE_SHARE*phys_vehicle_brake_decel(vh)*PHYS_TICKRATE*PHYS_TICKRATE;
    float reach=fminf(300,speed*speed/brake2+20),limit=INFINITY;
    for(int i=1;i+1<n && samples[i].along<=reach;i++) {
        Sample *a=samples+i-1,*b=samples+i,*c=samples+i+1;
        float ab=b->along-a->along,bc=c->along-b->along;
        float grade=(b->z-a->z)/ab,curvature=2*((c->z-b->z)/bc-grade)/(ab+bc);
        if(curvature>=0)continue;
        float crest=.95f*sqrtf(PHYS_RIDE_G/-curvature);if(crest>=launch)continue;
        for(int j=i+1;j<n;j++) {
            float distance=samples[j].along-b->along,seconds=distance/launch;
            float ballistic=b->z+grade*distance-.5f*PHYS_RIDE_G*seconds*seconds;
            /* Inspect the whole flight: an S-bend can finish at its starting heading. */
            if(fabsf(sinf(samples[j].heading-b->heading))>.001f) {
                limit=fminf(limit,sqrtf(crest*crest+brake2*fmaxf(0,b->along-car->half_length)));break;
            }
            if(ballistic<=samples[j].z)break;
        }
    }
    return limit;
}

/* Only tight Street X turns need this bounded flat-road mechanical screen.
   Candidate support/body sweeps still validate the actual scene afterwards. */
static int race_turn_needs_preview(const AiDrive *d,float t,const AiCar *car,float *reach) {
    float x[3]={0},v[2]={.001f,0},h=0,speed=.001f;
    phys_stage1_chassis_step(x,v,&h,&speed,1,0,&PHYS_SURF_ROAD,car->vehicle.accel>0?&car->vehicle:NULL);
    float radius=h>0?.001f/h:0,turn=3.141592654f*radius;
    float metres=fabsf(car->spd)*PHYS_TICKRATE;
    float brake=phys_vehicle_brake_decel(car->vehicle.accel>0?&car->vehicle:NULL)*PHYS_TICKRATE*PHYS_TICKRATE;
    float search=fminf(100,turn+metres*metres/fmaxf(.01f,2*brake));
    for(int dir=1;dir>=-1;dir-=2) {
        AiDrive line=*d;line.direction*=dir;
        for(float ahead=0;ahead<=search;ahead+=4) {
            float a[2],b[2],ha,hb;
            race_line_point(&line,t,ahead,0,a,&ha);race_line_point(&line,t,ahead+4,0,b,&hb);
            if(fabsf(atan2f(sinf(hb-ha),cosf(hb-ha)))*(radius+12-car->half_width)>4) {
                *reach=fminf(100,(dir>0?ahead:0)+turn);return 1;
            }
        }
    }
    return 0;
}
/* Mirror lane steering and speed in local copies. shortcut: flat ROAD and
   100 ms distant controls; extend surface sampling before using this off-road. */
static int race_turn_preview(const AiDrive *d,const AiCar *car,float offset,float reach,float pace,float initial_look) {
    AiDrive line=*d;AiCar probe=*car;line.stalled=0;
    float minimum=14/3.6f/PHYS_TICKRATE,travel=0;
    if(!probe.ride_ready) {probe.vel[0]=probe.spd*cosf(probe.head);probe.vel[1]=probe.spd*sinf(probe.head);}
    float steering=probe.steer;
    int ticks=(int)ceilf(fminf(100,reach)/minimum)+180;
    float point[2]={0},steer=0,target=pace/3.6f/PHYS_TICKRATE;
    for(int tick=0;tick<ticks;tick++) {
        if(tick)ai_drive_step(&line,probe.pos,probe.head,probe.spd);
        if(line.failed)return 0;
        if(line.finished)return 1;
        if(tick<RACE_PLAN_TICKS || tick%6==0) {
            float err,t=ai_projection(line.path,line.segment,probe.pos,&err),base[2];
            ai_route_point(line.path,line.segment,t,line.direction,0,base,NULL);
            float look=fmaxf(fminf(24,4+fabsf(probe.spd)*PHYS_TICKRATE*.5f),hypotf(line.target[0]-base[0],line.target[1]-base[1]));
            if(!tick)look=initial_look;
            race_line_point(&line,t,look,offset,point,NULL);
            steer=race_line_steer(&probe,point);
            target=fminf(pace/3.6f,race_corner_limit(&line,&probe,offset,look));
            if(cosf(probe.head)*(point[0]-probe.pos[0])+sinf(probe.head)*(point[1]-probe.pos[1])<0)
                target=fminf(target,14/3.6f);
            if(!line.loop)target=fminf(target,sqrtf(6*fmaxf(0,line.length-line.progress-1)));
            target/=PHYS_TICKRATE;
        }
        float before[2]={probe.pos[0],probe.pos[1]};
        steering=phys_steer_response(steering,steer);
        phys_car_step(probe.pos,probe.vel,&probe.head,&probe.spd,ai_throttle(target,probe.spd),steering,0,&PHYS_SURF_ROAD,
            probe.vehicle.accel>0?&probe.vehicle:NULL);
        float length=hypotf(probe.vel[0],probe.vel[1]);
        if(length<=.0001f)return 0;

        travel+=hypotf(probe.pos[0]-before[0],probe.pos[1]-before[1]);
        if(travel>=reach)return 1;
    }
    return 0;
}

/* Share the existing drag gearbox and chassis step with read-only forecasts. */
static int race_vehicle_step(AiCar *car,AiRace *driver,AiDriveInput input,
        const AiTrafficWorld *world) {
    PhysVehicle stock=car->vehicle;
    if(driver->progress.kind==N2_RACE_DRAG && car->has_powertrain) {
        int shift=driver->gearbox.rpm>=car->powertrain.redline_rpm &&
                  driver->gearbox.gear<car->powertrain.gearbox[0].gear_count;
        float gain=phys_manual_step(&driver->gearbox,&car->powertrain,0,0,
            car->tyre_radius,car->spd,input.throttle,shift,1/PHYS_TICKRATE);
        car->vehicle.accel*=gain;if(gain<=0 && input.throttle>0)input.throttle=0;
        if(driver->gearbox.failed)driver->drive.failed=1;
    }
    int collision=ai_vehicle_step(car,input.throttle,input.steer,input.handbrake,world);car->vehicle=stock;
    return collision;
}

/* A ramp lip can have a lower landing instead of four simultaneous contacts.
   Accept only a short, collision-free physical rollout in the held lane. */
static int race_drop_clear(const AiTrafficWorld *world,const AiCar *car,
        const AiRace *driver,float offset,float pace) {
    if(!world || !world->scene || !car->ride_ready || driver->drive.failed)return 0;
    float z=car->pos[2],normal[3];
    if(world_ground_pose(world->scene,car->pos[0],car->pos[1],z,&z,normal)==WSURF_NONE)return 0;
    if(car->ride.contact_mask==15 &&
       -(normal[0]*cosf(car->head)+normal[1]*sinf(car->head))<.02f)return 0;
    AiCar probe=*car;AiRace forecast=*driver;
    AiDrive *drive=&forecast.drive;drive->stalled=0;
    AiPerf perf=g_ai_perf;AiContactHook contact=g_ai_contact_hook;
    PhysPropImpactHook impact=g_phys_prop_impact_hook;
    g_ai_contact_hook=NULL;g_phys_prop_impact_hook=NULL;
    int left=car->ride.contact_mask==0,stable=0,ok=0,air_ticks=0;
    float peak=car->pos[2];
    /* shortcut: four-second local rollout, not general airborne route planning. */
    for(int tick=0;tick<4*PHYS_TICKRATE;tick++) {
        ai_drive_step(drive,probe.pos,probe.head,probe.spd);
        if(drive->failed)break;
        float error,t=ai_projection(drive->path,drive->segment,probe.pos,&error);
        float point[2],look=fminf(24,4+fabsf(probe.spd)*PHYS_TICKRATE*.5f);
        float base[2];ai_route_point(drive->path,drive->segment,t,drive->direction,0,base,NULL);
        look=fmaxf(look,hypotf(drive->target[0]-base[0],drive->target[1]-base[1]));
        race_line_point(drive,t,look,offset,point,NULL);
        float steer=race_line_steer(&probe,point);
        float target=fminf(pace/3.6f,race_corner_limit(drive,&probe,offset,look)/3.6f);
        if(!drive->loop)target=fminf(target,sqrtf(6*fmaxf(0,drive->length-drive->progress-1)));
        AiDriveInput input={.throttle=ai_throttle(target/PHYS_TICKRATE,probe.spd),.steer=steer};
        if(race_vehicle_step(&probe,&forecast,input,world) || drive->failed)break;
        peak=fmaxf(peak,probe.pos[2]);
        if(peak-probe.pos[2]>2*car->half_length ||
           fabsf(ai_course_lateral(drive->path,probe.pos)-offset)>car->half_width)break;
        if(probe.ride.contact_mask!=15){left|=probe.ride.contact_mask==0;stable=0;air_ticks+=probe.ride.contact_mask==0;}
        else if(left && ++stable>=6) {
            ok=race_pose_clear(world,&probe,probe.pos[0],probe.pos[1],probe.pos[2],probe.head);break;
        }
        if(air_ticks>PHYS_TICKRATE)break;
    }
    g_ai_perf=perf;g_ai_contact_hook=contact;g_phys_prop_impact_hook=impact;
    return ok;
}

void ai_race_step(AiCar cars[N_RACE_AI],AiRace drivers[N_RACE_AI],int k,int count,
        int circuit,const AiTrafficWorld *world,const AiCar *player) {
    AiCar *car=&cars[k];AiRace *driver=&drivers[k];
    if(driver->progress.lap<1)world_race_begin(&driver->progress,car->pos[0],car->pos[1]);
    /* A passing lane need not converge on the centreline after the finish. */
    if(driver->progress.finished && !driver->drive.loop &&
       driver->drive.length-driver->drive.progress<2 && fabsf(PHYS_KMH(car->spd))<2)
        driver->drive.finished=1;
    if(driver->progress.finished && (driver->drive.finished || driver->drive.failed ||
        (driver->drive.loop && driver->finishing && driver->drive.progress-driver->finish_at>
         count*(2*car->half_length+1.2f)+2))) {
        ai_vehicle_step(car,car->spd>.002f?-1:0,0,1,world);return;
    }
    if(driver->drop_phase) {
        if(--driver->drop_ticks<=0)driver->drop_phase=0;
        else if(!car->ride.contact_mask)driver->drop_phase=2;
        else if(driver->drop_phase==2 && car->ride.contact_mask==15 &&
                race_pose_clear(world,car,car->pos[0],car->pos[1],car->pos[2],car->head))
            driver->drop_phase=0;
    }
    AiDriveInput input=ai_drive_step(&driver->drive,car->pos,car->head,car->spd);
    if(driver->drive.failed || driver->drive.finished)driver->drop_phase=0;
    float pace=ai_race_pace(car,driver,k,world,player);
    float target=driver->drive.target_kmh/3.6f;
    if(!driver->drive.failed && !driver->drive.finished && world && world->scene) {
        const N2Path *p=driver->drive.path;int at=driver->drive.segment;
        float dx=p->xy[2*at+2]-p->xy[2*at],dy=p->xy[2*at+3]-p->xy[2*at+1];
        float len=hypotf(dx,dy),best=INFINITY;
        float old_offset=driver->offset,best_offset=old_offset,best_pace=0;
        float error,t=ai_projection(p,at,car->pos,&error);
        float look=fminf(24,4+fabsf(car->spd)*PHYS_TICKRATE*.5f);
        /* The generic driver already anticipates sharp source corners. Keep
           that turn-in reach when a slow car still needs its full radius. */
        float base[2];ai_route_point(p,at,t,driver->drive.direction,0,base,NULL);
        look=fmaxf(look,hypotf(driver->drive.target[0]-base[0],driver->drive.target[1]-base[1]));
        race_line_point(&driver->drive,t,look,0,driver->drive.target,&driver->drive.target_heading);
        /* Check beyond braking distance so a divider is seen before turn-in. */
        int tick=car->t++;
        /* Spread costly Street X plans across frames; every actor plans at spawn. */
        int phase=driver->progress.kind==N2_RACE_STREETX?RACE_PLAN_TICKS*k/(count>0?count:1):0;
        if((!tick || tick%RACE_PLAN_TICKS==phase) && !driver->drop_phase) {
            float speed=fabsf(car->spd)*PHYS_TICKRATE;
            float preview=fmaxf(20,car->half_length+speed*.5f+speed*speed/6);
            /* Preserve a clear conservative line. If none exists, search the
               source corridor and exact current offset, even for a nearby lane.
               Every approach and preview still checks support and solids.
               ponytail: local corridors only; dynamic obstacle routing is separate. */
            float spacing=car->half_width+1.2f;
            float current_offset=((car->pos[1]-p->xy[2*at+1])*dx-(car->pos[0]-p->xy[2*at])*dy)/len;
            int sides=(int)fmaxf(5,floorf(driver->drive.error_limit/spacing));
            /* A rival occupies a line only when we would actually reach it:
               course-relative gap against closing speed, or already touching.
               A faster rival pulling away is not a reason to change sides.
               Closing includes a bounded share of our unrealised pace so a
               slower rival we are queued behind is still passed. */
            int total=count+(player!=NULL)+(world->ambient && world->ambient_routes?world->ambient_count:0);
            float rival_lateral[total>0?total:1],rival_width[total>0?total:1];
            float rival_ahead[total>0?total:1],rival_closing[total>0?total:1],rival_length[total>0?total:1];
            int rival_distant[total>0?total:1],rival_conflict[total>0?total:1];
            int rivals=0,engaged=0;
            for(int j=0;j<total;j++)if(j!=k) {
                int slot=j-count-(player!=NULL);
                if(slot>=0 && !world->ambient_routes[slot].present)continue;
                const AiCar *lead=j<count?&cars[j]:slot>=0?world->ambient+slot:player;
                float lengths=car->half_length+lead->half_length,ahead,lateral,h;
                if(fabsf(lead->pos[2]-car->pos[2])>=2)continue;
                float distance=race_course_relative(p,at,t,driver->drive.direction,lengths,preview+lengths,
                                                    lead->pos,&ahead,&lateral,&h);
                /* Search the rear only for a body behind our local station.
                   Widening every projection can select a nearby earlier bend
                   for a front rival and reverse its passing-side estimate. */
                int rear=ahead<=-lengths;
                if(rear) {
                    distance=race_course_relative(p,at,t,driver->drive.direction,2*look+lengths,0,
                                                 lead->pos,&ahead,&lateral,&h);
                    if(ahead>=-lengths)continue;
                }
                if(distance>driver->drive.error_limit || ahead<=-2*look-lengths || ahead>=preview+lengths)continue;
                /* Moving racers follow this shared course; stopped bodies retain their angle. */
                float angle=j<count && fabsf(PHYS_KMH(lead->spd))>=5?0:lead->head-h;
                float alignment=fabsf(cosf(angle)),cross=fabsf(sinf(angle));
                lengths=car->half_length+lead->half_length*alignment+lead->half_width*cross;
                float width=lead->half_width*alignment+lead->half_length*cross;
                float own=car->spd*PHYS_TICKRATE*cosf(car->head-h);
                float rival=lead->spd*PHYS_TICKRATE*cosf(lead->head-h);
                /* A racer paced at least as fast as us is not waiting to be
                   passed; only real closing speed creates a conflict. */
                float intent=j<count && ai_race_pace(&cars[j],&drivers[j],j,world,player)>=pace?0:fminf(pace/3.6f-rival,3);
                float closing=fmaxf(own-rival,intent);
                float gap=ahead-lengths;
                int conflict=gap<1.2f || (closing>0 && gap<1.2f+.6f*fabsf(own)+4*closing+closing*closing/6);
                /* A committed pass ends once the rival is behind us or
                   really pulling away, not when its gap estimate wobbles. */
                if(ahead>-lengths)
                    engaged|=conflict || ahead<=lengths+2 || (driver->pass_side && own-rival>-1);
                rival_lateral[rivals]=lateral;rival_width[rivals]=width;
                rival_ahead[rivals]=ahead;rival_closing[rivals]=own-rival;rival_length[rivals]=lengths;
                rival_conflict[rivals]=!rear && ahead>0 && conflict;
                rival_distant[rivals++]=rival>=5/3.6f && cosf(lead->head-h)>.9f &&
                    gap>1.2f+.6f*fmaxf(fabsf(own),closing);
            }
            if(!engaged)driver->pass_side=0;
            float course_heading=atan2f(dy*driver->drive.direction,dx*driver->drive.direction);
            float heading_error=driver->drive.direction*atan2f(sinf(car->head-course_heading),cosf(car->head-course_heading));
            /* Finish turn-in before chasing distant lateral corrections. */
            int entering=driver->pass_side && heading_error*driver->pass_side>=0 &&
                ((old_offset-current_offset)*driver->pass_side>.3f || heading_error*driver->pass_side>.03f);
            float passing_offset=old_offset;
            if(driver->pass_side && !entering)for(int r=0;r<rivals;r++)if(rival_conflict[r]) {
                float width=car->half_width+rival_width[r];
                if(fabsf(passing_offset-rival_lateral[r])<width+.3f)
                    passing_offset=rival_lateral[r]+driver->pass_side*(width+.35f);
            }
            /* Keep proven straight corridors. Only if all are blocked,
               test a curve, then a straight recovery with gradual yaw.
               An instant target heading can falsely clip a nearby wall.
               The derivative bound keeps samples at most one metre apart;
               wheel support and full-body walls apply to every pose. */
            /* All three approach shapes share the same destination corridor.
               Cache only within this plan, so world/vehicle changes cannot stale it. */
            int corridor_state[2*sides+4];memset(corridor_state,0,sizeof corridor_state);
            int initial_support=traffic_supported_corners(world,car,car->pos[0],car->pos[1],car->pos[2],car->head,1);
            float turn_reach=0;
            int tight=driver->progress.kind==N2_RACE_STREETX && race_turn_needs_preview(&driver->drive,t,car,&turn_reach);
            float turn_pace[2*sides+4];memset(turn_pace,0,sizeof turn_pace);
            for(int approach=0;approach<3 && !isfinite(best);approach++) {
                /* Retain a safe selected lane after a pass instead of
                   cancelling turn-in as soon as the rival falls behind. */
                for(int option=-1;option<=2*sides+2;option++) {
                    if(option==12 && isfinite(best))break;
                    int sample=option>11?option-1:option;
                    float offset=sample?((sample+1)/2)*spacing*(sample&1?1:-1):0;
                    if(option<0)offset=old_offset;
                    /* Fixed lane spacing can miss a clear, slightly wider pass. */
                    if(option==11) {
                        if(!driver->pass_side || (best<50 && passing_offset!=old_offset))continue;
                        offset=passing_offset;
                    }
                    if(option==2*sides+2)
                        offset=current_offset;
                    if(option<12 && fabsf(offset)+car->half_width>=12)continue;
                    if(option>=12 && fabsf(offset)>=driver->drive.error_limit)continue;
                    float cost=.1f*fabsf(offset-old_offset);
                    /* Hold the committed passing side; only an occupied or
                       physically rejected side outweighs this and replans. */
                    if(driver->pass_side && offset*driver->pass_side<spacing*.5f)cost+=50;
                    /* The held line keeps a smaller margin, so small rival
                       corrections inside its lane do not price it out. */
                    int held=fabsf(offset-old_offset)<.01f;
                    float margin=held?.1f:.3f;
                    int merge_clear=1;
                    for(int r=0;r<rivals;r++) {
                        float width=car->half_width+rival_width[r];
                        float from=current_offset-rival_lateral[r],to=offset-rival_lateral[r];
                        /* A destination beyond an adjacent body is not a free
                           lane. Check the time spent crossing its lateral band;
                           an already overlapping body may still be escaped.
                           A held lane needs body clearance, not the extra
                           entry margin that would reject tiny corrections. */
                        if(fabsf(from)>width+.05f && from*(offset-current_offset)<0 &&
                           (from*to<=0 || fabsf(to)<width+(held?.05f:.3f))) {
                            float duration=2*look/fmaxf(2,speed);
                            float fraction=fminf(1,fabsf(from)/(fabsf(offset-current_offset)+.001f));
                            float gap=rival_ahead[r]-rival_closing[r]*duration*fraction;
                            float sweep=fabsf(rival_closing[r])*duration*fminf(1,width/(fabsf(offset-current_offset)+.001f));
                            if(fabsf(gap)<rival_length[r]+sweep+1.2f)merge_clear=0;
                        }
                        if(!rival_conflict[r] || (entering && held && rival_distant[r]))continue;
                        if(fabsf(to)<width+margin)cost+=100;
                    }
                    if(!merge_clear || cost>=best || corridor_state[option+1]<0)continue;
                    if(tight) {
                        /* Slowing also shortens pursuit; test the live pace before the minimum. */
                        if(!turn_pace[option+1]) {
                            float cap=pace;
                            if(!race_turn_preview(&driver->drive,car,offset,turn_reach,pace,look)) {
                                float lo=fminf(14,pace);
                                if(!race_turn_preview(&driver->drive,car,offset,turn_reach,lo,look))cap=-1;
                                else cap=lo;
                            }
                            turn_pace[option+1]=cap;
                        }
                        if(turn_pace[option+1]<0)continue;
                    }
                    float destination[2];race_line_point(&driver->drive,t,look,offset,destination,NULL);
                    float x=destination[0],y=destination[1],z=car->pos[2];int clear=1;
                    int steps=(int)ceilf(hypotf(x-car->pos[0],y-car->pos[1]));
                    float heading=atan2f(y-car->pos[1],x-car->pos[0]);
                    float reach=hypotf(x-car->pos[0],y-car->pos[1])/3;
                    float cx=car->pos[0]+cosf(car->head)*reach,cy=car->pos[1]+sinf(car->head)*reach;
                    float ex=x-cosf(driver->drive.target_heading)*reach,ey=y-sinf(driver->drive.target_heading)*reach;
                    if(approach==1)steps=(int)ceilf(3*fmaxf(reach,hypotf(ex-cx,ey-cy)));
                    int support=initial_support;
                    int recovering=support>=2 && support<4;
                    for(int q=1;q<=steps && clear;q++) {
                        float u=(float)q/steps,v=1-u;
                        float px=car->pos[0]+(x-car->pos[0])*u;
                        float py=car->pos[1]+(y-car->pos[1])*u;
                        float approach_heading=approach==2
                            ?car->head+u*atan2f(sinf(heading-car->head),cosf(heading-car->head)):heading;
                        if(approach==1) {
                            px=v*v*v*car->pos[0]+3*v*v*u*cx+3*v*u*u*ex+u*u*u*x;
                            py=v*v*v*car->pos[1]+3*v*v*u*cy+3*v*u*u*ey+u*u*u*y;
                            approach_heading=atan2f(v*v*(cy-car->pos[1])+2*v*u*(ey-cy)+u*u*(y-ey),
                                                   v*v*(cx-car->pos[0])+2*v*u*(ex-cx)+u*u*(x-ex));
                        }
                        clear=race_ground_at(world->scene,px,py,z,&z)!=WSURF_NONE;
                        if(clear && recovering) {
                            /* Returning from a verge may begin with two supported
                               corners. Never lose further support or bypass walls;
                               the target and future corridor still need all four. */
                            int next=traffic_supported_corners(world,car,px,py,z,approach_heading,1);
                            clear=next>=support && traffic_body_clear(world,car,px,py,z,approach_heading);support=next;
                        } else if(clear)clear=race_pose_clear(world,car,px,py,z,approach_heading);
                    }
                    if(clear)clear=race_pose_clear(world,car,x,y,z,approach==1?driver->drive.target_heading:heading);
                    /* This sweep restarts at the car, not at the approach's
                       downhill endpoint. Keep its layer reference local too. */
                    if(clear && !corridor_state[option+1]) {
                        z=car->pos[2];
                        for(float ahead=0;clear && ahead<=preview;ahead+=2) {
                            float point[2];race_line_point(&driver->drive,t,ahead,offset,point,&heading);
                            clear=race_ground_at(world->scene,point[0],point[1],z,&z)!=WSURF_NONE &&
                                  race_pose_clear(world,car,point[0],point[1],z,heading);
                        }
                        corridor_state[option+1]=clear?1:-1;
                    }
                    if(clear){best=cost;best_offset=offset;best_pace=tight?turn_pace[option+1]:0;}
                }
            }
            /* Only recover a stalled driver; moving racers keep their normal plan.
               At a lip, an unfinished lane change may need to continue
               straight from its actual offset instead of chasing that lane. */
            for(int option=0;option<2 && !isfinite(best) && driver->drive.stalled>PHYS_TICKRATE/2;option++) {
                float offset=option?current_offset:old_offset;
                if(fabsf(current_offset-offset)>.3f)continue;
                int occupied=0;
                for(int r=0;r<rivals;r++)
                    if(rival_conflict[r] && fabsf(offset-rival_lateral[r])<car->half_width+rival_width[r]+.1f)occupied=1;
                if(!occupied && race_drop_clear(world,car,driver,offset,pace)) {
                    best=0;best_offset=offset;driver->drop_phase=1;driver->drop_ticks=4*PHYS_TICKRATE;
                }
            }
            driver->blocked=!isfinite(best);
            /* Lane cost ignores pace, so refine only the selected candidate. */
            if(!driver->blocked && best_pace>0 && best_pace<pace) {
                float hi=pace;
                for(int q=0;q<5;q++) {
                    float mid=.5f*(best_pace+hi);
                    if(race_turn_preview(&driver->drive,car,best_offset,turn_reach,mid,look))best_pace=mid;else hi=mid;
                }
            }
            driver->turn_speed=best_pace>0?best_pace/3.6f:0;
            if(!driver->blocked && driver->progress.kind==N2_RACE_URL) {
                float crest=race_crest_limit(world,&driver->drive,car,best_offset,pace);
                if(isfinite(crest))driver->turn_speed=driver->turn_speed>0?fminf(driver->turn_speed,crest):crest;
            }
            if(!driver->blocked) {
                driver->offset=best_offset;
                driver->pass_side=engaged && fabsf(best_offset)>=spacing*.5f?(best_offset>0?1:-1):0;
            }
        }
        /* A blocked plan brakes in the held lane. Falling back to the
           generic centreline steering would merge through adjacent bodies. */
        float destination[2];race_line_point(&driver->drive,t,look,driver->offset,destination,NULL);
        input.steer=race_line_steer(car,destination);
        if(!driver->blocked) {
            target=pace/3.6f;
            if(!driver->drive.loop)target=fminf(target,sqrtf(6*fmaxf(0,driver->drive.length-driver->drive.progress-1)));
            target=fminf(target,race_corner_limit(&driver->drive,car,driver->offset,look));
            if(driver->turn_speed>0)target=fminf(target,driver->turn_speed);
            if(cosf(car->head)*(destination[0]-car->pos[0])+
               sinf(car->head)*(destination[1]-car->pos[1])<0)
                target=fminf(target,14/3.6f);
        } else target=0;
    }
    if(driver->progress.finished)target=fminf(target,25/3.6f);
    /* Yield to bodies ahead; shared contacts resolve simultaneous impacts. */
    int ambient=world && world->ambient && world->ambient_routes?world->ambient_count:0;
    float brake=RACE_BRAKE_SHARE*phys_vehicle_brake_decel(car->vehicle.accel>0?&car->vehicle:NULL)*PHYS_TICKRATE*PHYS_TICKRATE;
    for(int j=0;j<count+(player!=NULL)+ambient;j++)if(j!=k) {
        int slot=j-count-(player!=NULL);
        if(slot>=0 && !world->ambient_routes[slot].present)continue;
        const AiCar *lead=j<count?&cars[j]:slot>=0?world->ambient+slot:player;
        if(fabsf(lead->pos[2]-car->pos[2])>2)continue;
        float fx=cosf(car->head),fy=sinf(car->head),lx=lead->pos[0]-car->pos[0],ly=lead->pos[1]-car->pos[1];
        float along=lx*fx+ly*fy,across=fabsf(lx*fy-ly*fx);
        float angle=lead->head-car->head;
        float lengths=car->half_length+lead->half_length*fabsf(cosf(angle))+lead->half_width*fabsf(sinf(angle));
        float widths=car->half_width+lead->half_width*fabsf(cosf(angle))+lead->half_length*fabsf(sinf(angle));
        float gap=along-lengths,lead_speed=fmaxf(0,lead->spd*PHYS_TICKRATE*cosf(angle));
        int body_ahead=along>0 && across<=widths+.05f;
        int following=0;
        const AiDrive *d=&driver->drive;
        if(!d->failed && d->path) {
            float err,t=ai_projection(d->path,d->segment,car->pos,&err),ahead,lateral,h;
            float speed=fabsf(car->spd)*PHYS_TICKRATE;
            /* Search far enough to stop, including reaction and both bodies.
               A fixed time horizon misses stopped traffic with weak brakes. */
            float range=fmaxf(20,speed*.6f+speed*speed/(2*fmaxf(.01f,brake))+lengths+1.2f);
            if(race_course_relative(d->path,d->segment,t,d->direction,0,range,
                                   lead->pos,&ahead,&lateral,&h)<=d->error_limit) {
                float dx=d->path->xy[2*d->segment+2]-d->path->xy[2*d->segment];
                float dy=d->path->xy[2*d->segment+3]-d->path->xy[2*d->segment+1];
                float own_lateral=((car->pos[1]-d->path->xy[2*d->segment+1])*dx-
                                   (car->pos[0]-d->path->xy[2*d->segment])*dy)/ai_segment(d->path,d->segment);
                float relative=lead->head-h;
                float length=lead->half_length*fabsf(cosf(relative))+lead->half_width*fabsf(sinf(relative));
                float width=lead->half_width*fabsf(cosf(relative))+lead->half_length*fabsf(sinf(relative));
                if(ahead>0 && fabsf(lateral-own_lateral)<=car->half_width+width+.05f) {
                    gap=ahead-car->half_length-length;
                    lead_speed=fmaxf(0,lead->spd*PHYS_TICKRATE*cosf(relative));
                    following=1;
                }
            }
            /* Keep an immediate body safeguard for crosswise traffic and
               converging passes, without projecting a straight ray through bends. */
            if(!following && !(body_ahead && along<lengths+fmaxf(2,fabsf(car->spd)*PHYS_TICKRATE*.3f)))continue;
        } else if(!body_ahead)continue;
        float reaction=brake*.6f;
        float limit=fmaxf(0,sqrtf(reaction*reaction+lead_speed*lead_speed+2*brake*fmaxf(0,gap-1.2f))-reaction);
        /* Stopped or overlapping bodies keep the short-range stop constraint. */
        if(lead_speed<.1f || gap<1.2f)limit=fminf(limit,fmaxf(0,(gap-1.2f)/.6f));
        if(world && world->scene && !driver->blocked && !driver->drive.failed &&
           fabsf(driver->offset-ai_course_lateral(driver->drive.path,car->pos))>.15f &&
           fabsf(input.steer)>.05f)
            limit=fmaxf(limit,3/3.6f); /* edge into a clear passing line; contacts remain physical */
        if(limit<target){target=limit;driver->drive.stalled=0;}
    }
    car->target_speed=target/PHYS_TICKRATE;
    if(!driver->drive.failed && !driver->drive.finished)
        input.throttle=ai_throttle(car->target_speed,car->spd);
    float before[3]={car->pos[0],car->pos[1],car->pos[2]};
    int collision=race_vehicle_step(car,driver,input,world);
    if(driver->progress.kind==N2_RACE_DRIFT)race_drift_step(&driver->progress.drift,
        car->vel,car->head,hypotf(car->pos[0]-before[0],car->pos[1]-before[1]),
        car->ride.contact_mask!=0,collision,1/PHYS_TICKRATE);
    world_race_progress_update(&driver->progress,circuit,car->pos[0],car->pos[1]);
    if(driver->progress.finished && !driver->finishing){driver->finishing=1;driver->finish_at=driver->drive.progress;}
    car->lap=fmaxf(0,driver->progress.lap-1);car->prevrel=driver->drive.segment;
}
