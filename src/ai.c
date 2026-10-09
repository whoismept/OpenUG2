/* ai.c — OpenUG2 AI module implementation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

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
    free(roads->pred_start);free(roads->pred_list);
    free(roads->cell_start);free(roads->cell_list);
    memset(roads,0,sizeof *roads);
}

/* Shared recursive chunk/record reader for free-roam and event lanes.
   Event callers also request the authored distance along each lane chain. */
static int ai_roads_read(AiRoadNet *roads,const unsigned char *data,long len,
                          int *capacity,float **distance,float *widest) {
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

int ai_roads_load(AiRoadNet *roads,const char *troot) {
    if(!roads || !troot)return 0;
    ai_roads_free(roads);
    struct dirent **files=NULL;
    int nf=scandir(troot,&files,NULL,alphasort),cap=0,nfiles=0;
    if(nf<0)return 0;
    for(int f=0;f<nf;f++) {
        if(strncmp(files[f]->d_name,"ROUTES",6))continue;
        char path[1024];
        snprintf(path,sizeof path,"%s/%s/RoutesFreeRoam.bin",troot,files[f]->d_name);
        long len=0;unsigned char *data=n2_read_file(path,&len);
        if(!data)continue;
        if(ai_roads_read(roads,data,len,&cap,NULL,NULL))nfiles++;
        else fprintf(stderr,"free-roam road paths: invalid record in %s\n",path);
        free(data);
    }
    for(int f=0;f<nf;f++)free(files[f]);free(files);
    printf("free-roam road paths: %d nodes from %d RoutesFreeRoam.bin files\n",
           roads->n,nfiles);
    if(roads->n>0 && !ai_roads_index(roads))printf("road query index: unavailable, linear scans\n");
    return roads->n;
}

/* ai_road_next only considers edges whose start is `at` (authored pass) or
 * whose endpoint lies within 65 m of `at` (junction pass: gap <= 5 m on an
 * edge <= 60 m). The grid cell is wider than that, so a 3x3 neighbourhood is
 * a superset; candidates are visited in ascending index like the linear scan. */
#define AI_ROAD_CELL 70.0f
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
    return 1;
}

static int ai_int_cmp(const void *a,const void *b) {
    int x=*(const int *)a,y=*(const int *)b;return (x>y)-(x<y);
}
/* Sorted candidate node indices for ai_road_next's pass, or -1 = scan all. */
static int ai_road_candidates(const AiRoadNet *r,int at,int pass,int *out,int cap) {
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

static float traffic_lane_tangent(const AiRoadNet *roads,N2Scene *scene,int from,int to,
                                  float t,float x,float y,float z,float heading,
                                  float *outx,float *outy,float *outz) {
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
            (surface==WSURF_TERRAIN && side_surface==WSURF_TERRAIN)) && fabsf(pz-z)<0.75f) {
            *outx=px;*outy=py;*outz=pz;return side;
        }
    }
    *outx=x;*outy=y;*outz=z;
    return 0;
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
        traffic_lane_tangent(roads,world->scene,from,to,t,px,py,pz,heading,&px,&py,&pz);
        if(!traffic_body_clear(world,car,px,py,pz,heading))return 0;
        z=pz;
    }
    return 1;
}

/* Join nearby road segments, including the middle of an edge. Stored point
 * order is not a decoded one-way rule; incoming heading selects travel direction.
 * ponytail: geometric junction policy until source lane/direction flags are known. */
static int traffic_next(const AiRoadNet *roads,const AiTrafficWorld *world,const AiCar *car,
                         int at,int previous,float z,int preview) {
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
    int best=-1;float score=-1e30f;
    /* Preserve the authored chain before considering nearby junctions. A
       straighter overlapping branch must not steal a car halfway round a bend. */
    int cand[4096]; /* Recursive previews must not overwrite their caller's candidates. */
    for(int pass=0;pass<2 && best<0;pass++) {
      /* Authored chains can cross terrain-classified ground. Joining a new
         chain still needs ROAD support; mesh labels alone cannot invent roads. */
      if(pass==1 && surface!=WSURF_ROAD)break;
      int nc=ai_road_candidates(roads,at,pass,cand,4096);
      for(int ci=0;ci<(nc<0?roads->n:nc);ci++) {
        int i=nc<0?ci:cand[ci];
        int next=roads->next[i];if(next<0)continue;
        for(int reverse=0;reverse<2;reverse++) {
            int start=reverse?next:i,nb=reverse?i:next;
            if(pass==0 && start!=at)continue;
            if(nb==at || nb==previous)continue;
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
            if(!traffic_edge_supported(scene,xy[at*2],xy[at*2+1],dx,dy,z,pass==1))continue;
            if(!traffic_edge_clear(roads,world,car,at,nb,z))continue;
            /* Bounded recursive preview follows open junction alternatives,
               rather than rejecting the approach to a junction merely because
               a later authored branch is closed. Terminal/reverse chains keep
               their existing endpoint behavior. */
            if(preview>0 && scene && roads->next[nb]>=0 && roads->next[nb]!=at) {
                float nz=z;world_ground_at(scene,xy[nb*2],xy[nb*2+1],z,&nz);
                if(traffic_next(roads,world,car,nb,at,nz,preview-1)<0)continue;
            }
            float s=previous<0? -gap : (dx*hx+dy*hy)/len-gap*0.02f;
            if(s>score){score=s;best=nb;}
        }
      }
    }
    return best; /* no authored continuation: despawn off-screen instead of a U-turn */
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
    return traffic_next(roads,world,car,at,previous,z,3);
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
        traffic_lane_tangent(roads,world->scene,route.from,route.to,route.along/length,
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

void ai_traffic_follow(AiCar cars[N_OPENWORLD_AI], const AiTraffic routes[N_OPENWORLD_AI],
                       const AiRoadNet *roads, int k, int count, const AiCar *player) {
    /* ponytail: local gap model; use decoded lane/signal rules if found. */
    if(k<0 || k>=count || routes[k].to<0)return;
    AiCar *car=&cars[k];
    float fx=cosf(car->head),fy=sinf(car->head);
    float speed=car->spd*PHYS_TICKRATE;
    float target=fminf(routes[k].cruise_speed*PHYS_TICKRATE,
                       traffic_turn_speed(roads,&routes[k]));
    if(routes[k].after<0) {
        int from=routes[k].from,to=routes[k].to;
        float length=hypotf(roads->xy[2*to]-roads->xy[2*from],
                            roads->xy[2*to+1]-roads->xy[2*from+1]);
        target=fminf(target,sqrtf(8.0f*fmaxf(0.0f,length-routes[k].along-1.0f)));
    }
    for(int j=0;j<count+(player!=NULL);j++)if(j!=k && (j==count || routes[j].present)) {
        const AiCar *lead=j==count?player:&cars[j];
        if(fabsf(car->pos[2]-lead->pos[2])>2.5f)continue;
        float alignment=cosf(lead->head-car->head),cross=sinf(lead->head-car->head);
        float lead_length=lead->half_length*fabsf(alignment)+lead->half_width*fabsf(cross);
        float lead_width=lead->half_width*fabsf(alignment)+lead->half_length*fabsf(cross);
        float dx=lead->pos[0]-car->pos[0],dy=lead->pos[1]-car->pos[1];
        float ahead=dx*fx+dy*fy;
        if(ahead<=0 || ahead>100.0f ||
           fabsf(dx*fy-dy*fx)>car->half_width+lead_width+0.5f)continue;
        float gap=ahead-car->half_length-lead_length;
        float relative=fmaxf(0.0f,speed-lead->spd*alignment*PHYS_TICKRATE);
        float braking=relative*relative/(2.0f*4.0f);
        float safe=fmaxf(0.0f,(gap-6.0f-braking)/1.5f);
        if(safe<target)target=safe;
    }
    car->target_speed=target/PHYS_TICKRATE;
}

int ai_traffic_respawn(const AiRoadNet *roads, const AiTrafficWorld *world,
                       AiCar cars[N_OPENWORLD_AI],
                       AiTraffic routes[N_OPENWORLD_AI], int k, const float player[3],
                       float player_heading) {
    if(!roads || !roads->xy || !roads->next || !player || k<0 || k>=N_OPENWORLD_AI)return 0;
    N2Scene *scene=world?world->scene:NULL;
    unsigned attempt=routes[k].respawns;
    const float angle=0.4f+(float)k*6.2831853f/N_ROAM_VISUALS+0.5f*(attempt%6);
    const float radius=85.0f+20.0f*(attempt%3);
    const float tx=player[0]+cosf(angle)*radius,ty=player[1]+sinf(angle)*radius;
    float fx=cosf(player_heading),fy=sinf(player_heading);
    int best=-1,to=-1;float bz=player[2];
    /* Use the actual view; side streets can populate ahead outside the view. */
    for(int pass=0;pass<2 && best<0;pass++) {
    float score=1e30f;
    for(int i=0;i<roads->n;i++) {
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
        traffic_lane_tangent(roads,scene,i,next,0,x,y,z,atan2f(dy,dx),&lx,&ly,&lz);
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
        if(!traffic_pose_clear(world,&cars[k],lx,ly,lz,atan2f(dy,dx)) ||
           !traffic_spawn_run_clear(roads,world,&cars[k],i,next,lz))continue;
        best=i;to=next;bz=z;score=s;
    }
    }
    routes[k]=(AiTraffic){.prev=-1,.from=best,.to=to,.after=-1,.respawns=attempt+1,
                          .present=best>=0,
                          .stop_reason=best<0?7:0};
    if(best<0){cars[k].spd=0;return 0;}
    AiCar *car=&cars[k];
    float dx=roads->xy[to*2]-roads->xy[best*2];
    float dy=roads->xy[to*2+1]-roads->xy[best*2+1];
    routes[k].after=ai_road_next(roads,world,car,to,best,bz);
    routes[k].lane_offset=traffic_lane_tangent(roads,scene,best,to,0,
        roads->xy[best*2],roads->xy[best*2+1],bz,
        atan2f(dy,dx),
        &car->pos[0],&car->pos[1],&car->pos[2]);
    car->head=atan2f(dy,dx);
    car->spd=(ai_traffic_is_racer(k) ? 75.0f+5.0f*(k-N_AI) : 26.0f+3.0f*(k%N_AI))/3.6f/PHYS_TICKRATE;
    routes[k].cruise_speed=car->spd;
    car->spd=fminf(car->spd,traffic_turn_speed(roads,&routes[k])/PHYS_TICKRATE);
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
        traffic_lane_tangent(roads,scene,from,to,1,x,y,z,h,&x,&y,&z);
        if((car->pos[0]-x)*cosf(h)+(car->pos[1]-y)*sinf(h)<0)break;
        route->prev=from;route->from=to;route->to=route->after;
        route->after=ai_road_next(roads,world,car,route->to,route->from,car->pos[2]);
    }
    const N2Mesh *owner=scene?scene->meshes:NULL;
    float body[3]={car->half_length,car->half_width,car->height};
    if(route->preview_scene!=owner || memcmp(route->preview_body,body,sizeof body)) {
        route->preview_valid=0;route->preview_scene=owner;
        memcpy(route->preview_body,body,sizeof body);
    }
    AiTraffic target=*route;
    target.along+=fminf(10.0f,4.0f+fabsf(car->spd)*PHYS_TICKRATE*.35f);
    for(int hop=0;hop<8 && target.along>length;hop++) {
        if(target.after<0){target.along=length;break;}
        target.along-=length;target.prev=target.from;target.from=target.to;target.to=target.after;
        float pz=car->pos[2];
        if(scene)world_ground_at(scene,xy[2*target.to],xy[2*target.to+1],pz,&pz);
        unsigned bit=1u<<hop;
        if(!(route->preview_valid&bit) || route->preview_from[hop]!=target.from ||
           route->preview_to[hop]!=target.to || route->preview_z[hop]!=pz) {
            route->preview_after[hop]=ai_road_next(roads,world,car,target.to,target.from,pz);
            route->preview_from[hop]=target.from;route->preview_to[hop]=target.to;
            route->preview_z[hop]=pz;route->preview_valid|=bit;
        }
        target.after=route->preview_after[hop];
        length=hypotf(xy[2*target.to]-xy[2*target.from],xy[2*target.to+1]-xy[2*target.from+1]);
    }
    float x,y,h,z=car->pos[2];
    traffic_path_pose(roads,&target,&x,&y,&h);
    route->lane_offset=traffic_lane_tangent(roads,scene,target.from,target.to,
        target.along/length,x,y,z,h,&x,&y,&z);
    float error=atan2f(y-car->pos[1],x-car->pos[0])-car->head;
    error=atan2f(sinf(error),cosf(error));
    float desired=fminf(car->target_speed,traffic_turn_speed(roads,route)/PHYS_TICKRATE);
    desired=fminf(desired,(6.0f+14.0f*fmaxf(0,cosf(error*2)))/PHYS_TICKRATE);
    float oldx=car->pos[0],oldy=car->pos[1];
    ai_vehicle_step(car,ai_throttle(desired,car->spd),
                     fmaxf(-1,fminf(1,error/.5f)),desired<.001f,world);
    if(!car->ride.contact_mask)route->air_ticks++;
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

/* Order lane samples by their authored distance, including a circuit's
   start-line wrap. Dynamic programming keeps the entire chain continuous
   instead of greedily jumping to a different road at a branch.
   Loaded support checks carry height along the chosen predecessor; probing
   every hill from grid height can select the ceiling beneath its road instead.
   ponytail: up to 64 XY lane choices per sample, one height per prefix; stacked
   routes sharing identical XY/distance would need separate height states. */
static int race_course_read(const char *root,const WEvent *event,N2Path *out,int full,
        const AiTrafficWorld *world,const AiCar *body,float *widest) {
    if(!root || !event || !out)return 0;
    N2Path raw={0};
    if(race_route_load(root,event,&raw)){*out=raw;return 1;}
    char file[1024];snprintf(file,sizeof file,"%s/ROUTES%s/Routes%dF.bin",root,event->reg,event->id);
    long bytes=0;unsigned char *data=n2_read_file(file,&bytes);if(!data)return 0;
    AiRoadNet lanes={0};float *distance=NULL;int capacity=0;
    int ok=ai_roads_read(&lanes,data,bytes,&capacity,&distance,widest);free(data);
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
    typedef struct { float x,y,z,along,penalty; double cost; int parent,root,edge; } Choice;
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
                int duplicate=0;for(int q=0;q<n;q++)if(hypotf(x-row[q].x,y-row[q].y)<.1f)duplicate=1;
                if(duplicate)continue;
                if(n==CHOICES){ok=0;break;}
                row[n++]=(Choice){.x=x,.y=y,.z=body?body->pos[2]:0,.along=along,.cost=INFINITY,.parent=-1,.root=-1,.edge=k};
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
                if(terrain && anchored && reference[0].penalty==0 && hypotf(c->x-reference[0].x,c->y-reference[0].y)>.1f)c->cost+=1e8/(layers+1);
                choices[q].root=q;choices[q].parent=-1;
            }
            for(int j=1;j<layers;j++) {
                Choice *row=choices+j*CHOICES,*prev=row-CHOICES;
                for(int q=0;q<counts[j];q++) {
                    row[q].cost=INFINITY;row[q].parent=row[q].root=-1;
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
                        if(prev[r].cost+step*step>=row[q].cost)continue;
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
                                int surface=world_ground_at(world->scene,row[q].x,row[q].y,z,&z);
                                /* Retry a lost road layer by carrying height uphill.
                                   Keep successful endpoint probes and body sweeps unchanged. */
                                if(surface==WSURF_NONE || (!terrain && surface!=WSURF_ROAD)) {
                                    float height=prev[r].z;
                                    if(world_ground_at(world->scene,prev[r].x,prev[r].y,height,&height)==WSURF_ROAD) {
                                        int steps=fmaxf(1,ceilf(step)),part=1;
                                        for(;part<=steps;part++) {
                                            float t=(float)part/steps,x=prev[r].x+(row[q].x-prev[r].x)*t;
                                            float y=prev[r].y+(row[q].y-prev[r].y)*t;
                                            int cat=world_ground_at(world->scene,x,y,height,&height);
                                            if(cat==WSURF_NONE || (!terrain && cat!=WSURF_ROAD))break;
                                        }
                                        if(part>steps){surface=WSURF_ROAD;z=height;}
                                    }
                                }
                                if(surface==WSURF_NONE || (!terrain && surface!=WSURF_ROAD) || !race_pose_clear(world,body,row[q].x,row[q].y,z,h)) {
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
                                        int cat=world_ground_at(world->scene,x,y,midz,&midz);
                                        if(cat==WSURF_NONE || (!terrain && cat!=WSURF_ROAD) ||
                                           !traffic_body_clear(world,body,x,y,midz,h)) {
                                            penalty=1e8f;break;
                                        }
                                    }
                                    /* The sweep and endpoint must finish on the
                                       same layer, not opposite sides of a deck. */
                                    if(!penalty) {
                                        float end=midz;
                                        world_ground_at(world->scene,row[q].x,row[q].y,end,&end);
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
                        double c=prev[r].cost+step*step*(3-ad-bd)+penalty+anchor;
                        if(c<row[q].cost) {
                            row[q].cost=c;row[q].parent=r;row[q].root=prev[r].root;
                            row[q].z=z;row[q].penalty=penalty;
                        }
                    }
                }
            }
            for(int q=0;q<counts[layers-1];q++) {
                Choice *c=choices+(layers-1)*CHOICES+q;
                if(!isfinite(c->cost) || c->root<0)continue;
                Choice *first=choices+c->root;
                float seam=hypotf(c->x-first->x,c->y-first->y);
                if(event->circuit && seam>12)continue;
                if(c->cost+seam*seam*(event->circuit!=0)<cost) {
                    N2Path candidate={.n=layers,.xy=malloc((size_t)layers*2*sizeof(float))};
                    if(!candidate.xy)continue;
                    int parent=q,bad=0;
                    for(int j=layers-1;j>=0;j--) {
                        Choice *node=choices+j*CHOICES+parent;
                        candidate.xy[2*j]=node->x;candidate.xy[2*j+1]=node->y;
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
                    /* Nearby lanes across a divider are not reachable from the grid. */
                    double entrance_penalty=0;
                    if(world && body) {
                        float z=body->pos[2];int steps=fmaxf(1,ceilf(nearest));
                        for(int s=1;s<=steps;s++) {
                            float t=(float)s/steps,x=body->pos[0]+(entrance[0]-body->pos[0])*t;
                            float y=body->pos[1]+(entrance[1]-body->pos[1])*t;
                            int cat=world_ground_at(world->scene,x,y,z,&z);
                            if(cat==WSURF_NONE || (!terrain && cat!=WSURF_ROAD) ||
                               !traffic_body_clear(world,body,x,y,z,heading)){entrance_penalty=1e8;break;}
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
            if(unsupported)printf("race course: %d/%d samples need support/collision validation\n",unsupported,layers);
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
    free(checks);free(check_offsets);free(choices);free(counts);ai_roads_free(&lanes);free(distance);
    if(!ok){free(path.xy);return 0;}
    *out=path;return 1;
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
    AiRoadNet lanes={0};int cap=0,ok=ai_roads_read(&lanes,data,bytes,&cap,NULL,NULL);free(data);
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
    AiRoadNet lanes={0};float *distance=NULL;int capacity=0;
    int ok=ai_roads_read(&lanes,data,bytes,&capacity,&distance,widest);free(data);
    if(!ok){ai_roads_free(&lanes);free(distance);return;}
    float length=0;for(int i=0;i<path->n-1;i++)length+=ai_segment(path,i);
    int points=(int)ceilf(length/5)+1;
    float *xy=points<=4095?malloc((size_t)points*2*sizeof *xy):NULL;
    if(!xy){ai_roads_free(&lanes);free(distance);return;}
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
            float cost=deviation;
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
    ai_roads_free(&lanes);free(distance);
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
    int ok=ai_roads_read(&lanes,data,n,&capacity,NULL,&widest);
    free(data);ai_roads_free(&lanes);return ok?widest:0;
}

int ai_race_prepare(const char *root,const WEvent *event,WRace *race,
        const AiTrafficWorld *world,N2Path *path,AiCar cars[N_RACE_AI],AiRace drivers[N_RACE_AI],
        const AiCar *player) {
    free(path->xy);*path=(N2Path){0};
    memset(drivers,0,N_RACE_AI*sizeof *drivers);
    N2Path course={0};
    int rawcourse=event && root && race_route_load(root,event,&course);
    if(!player || !event || !race || !race->active || race->ngate<2 || !world || !world->scene ||
       (!rawcourse && !ai_race_course(root,event,&course))) {
        free(course.xy);printf("race opponents: continuous course unavailable; route topology needs decoding\n");return 0;
    }
    float widest=race_lane_width(root,event);
    int wanted=event->info.kind==N2_RACE_URL ? 5 :
        event->info.kind==N2_RACE_UNKNOWN ? N_AI : 3;
    if(!rawcourse) {
        AiCar body=*player;
        for(int k=0;k<wanted;k++) {
            body.half_length=fmaxf(body.half_length,cars[k].half_length);
            body.half_width=fmaxf(body.half_width,cars[k].half_width);
            body.height=fmaxf(body.height,cars[k].height);
        }
        N2Path supported={0};
        if(race_course_read(root,event,&supported,0,world,&body,&widest) ||
           (!event->circuit && race_course_read(root,event,&supported,1,world,&body,&widest))) {
            free(course.xy);course=supported;
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
        for(int k=0;k<wanted;k++) {
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
    if(rawcourse)race_route_refine(root,event,world,&envelope,cars,count,path,&widest);
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
    for(int k=0;k<count;k++) {
        drivers[k].progress=*race;
        drivers[k].offset=ai_course_lateral(path,cars[k].pos);
        ai_drive_join(&drivers[k].drive,path,cars[k].pos,cars[k].head,widest>0?widest:8);
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
    AiDriveInput input=ai_drive_step(&driver->drive,car->pos,car->head,car->spd);
    float pace=world && isfinite(world->race_pace) && world->race_pace>0
        ?fmaxf(40,fminf(180,world->race_pace+4*k)):50;
    float target=driver->drive.target_kmh/3.6f;
    if(!driver->drive.failed && !driver->drive.finished && world && world->scene) {
        const N2Path *p=driver->drive.path;int at=driver->drive.segment;
        float dx=p->xy[2*at+2]-p->xy[2*at],dy=p->xy[2*at+3]-p->xy[2*at+1];
        float len=hypotf(dx,dy),best=INFINITY,tx=0,ty=0;
        float old_offset=driver->offset,best_offset=old_offset;
        /* The lookahead target can already be beyond a bend. Offset it using
           that road tangent, consistently with the corridor preview below. */
        float ox=sinf(driver->drive.target_heading)*driver->drive.direction;
        float oy=cosf(driver->drive.target_heading)*driver->drive.direction;
        /* Check beyond braking distance so a divider is seen before turn-in. */
        if(car->t++%15==0) {
            float error,t=ai_projection(p,at,car->pos,&error);
            float speed=fabsf(car->spd)*PHYS_TICKRATE;
            float preview=fmaxf(20,car->half_length+speed*.5f+speed*speed/6);
            /* Preserve a clear conservative line. If none exists, search the
               source corridor and exact current offset, even for a nearby lane.
               Every approach and preview still checks support and solids.
               ponytail: local corridors only; dynamic obstacle routing is separate. */
            float spacing=car->half_width+1.2f;
            float current_offset=((car->pos[1]-p->xy[2*at+1])*dx-(car->pos[0]-p->xy[2*at])*dy)/len;
            /* Keep a straight passing line through small lead-car movements;
               bends retain the full clearance. */
            int straight=cosf(driver->drive.target_heading-atan2f(dy,dx))>.9999f;
            int sides=(int)fmaxf(5,floorf(driver->drive.error_limit/spacing));
            /* Keep proven straight corridors. Only if all are blocked,
               test a curve, then a straight recovery with gradual yaw.
               An instant target heading can falsely clip a nearby wall.
               The derivative bound keeps samples at most one metre apart;
               wheel support and full-body walls apply to every pose. */
            for(int approach=0;approach<3 && !isfinite(best);approach++) {
                for(int option=0;option<=2*sides+1;option++) {
                    if(option==11 && isfinite(best))break;
                    float offset=option?((option+1)/2)*spacing*(option&1?1:-1):0;
                    if(option==2*sides+1)
                        offset=current_offset;
                    if(option<11 && fabsf(offset)+car->half_width>=12)continue;
                    if(option>=11 && fabsf(offset)>=driver->drive.error_limit)continue;
                    float cost=.1f*fabsf(offset-old_offset)+.2f*fabsf(offset);
                    for(int j=0;j<count+(player!=NULL)+(world->ambient && world->ambient_routes?world->ambient_count:0);j++)if(j!=k) {
                        int slot=j-count-(player!=NULL);
                        if(slot>=0 && !world->ambient_routes[slot].present)continue;
                        const AiCar *lead=j<count?&cars[j]:slot>=0?world->ambient+slot:player;
                        float lx=lead->pos[0]-car->pos[0],ly=lead->pos[1]-car->pos[1];
                        float along=lx*cosf(car->head)+ly*sinf(car->head);
                        float lateral=ai_course_lateral(p,lead->pos);
                        /* At a grid seam, a close stopped body must use our tangent. */
                        if(along<car->half_length+lead->half_length+2 && fabsf(PHYS_KMH(lead->spd))<5)
                            lateral=((lead->pos[1]-p->xy[2*at+1])*dx-
                                (lead->pos[0]-p->xy[2*at])*dy)/len;
                        if(along>0 && along<preview+car->half_length+lead->half_length &&
                           fabsf(lead->pos[2]-car->pos[2])<2 &&
                           fabsf(offset-lateral)<car->half_width+lead->half_width+
                             (straight && cosf(lead->head-driver->drive.target_heading)>.9999f &&
                              fabsf(offset-old_offset)<.1f?.1f:.3f))
                            cost+=100;
                    }
                    if(cost>=best)continue;
                    float x=driver->drive.target[0]-ox*offset;
                    float y=driver->drive.target[1]+oy*offset,z=car->pos[2];int clear=1;
                    int steps=(int)ceilf(hypotf(x-car->pos[0],y-car->pos[1]));
                    float heading=atan2f(y-car->pos[1],x-car->pos[0]);
                    float reach=hypotf(x-car->pos[0],y-car->pos[1])/3;
                    float cx=car->pos[0]+cosf(car->head)*reach,cy=car->pos[1]+sinf(car->head)*reach;
                    float ex=x-cosf(driver->drive.target_heading)*reach,ey=y-sinf(driver->drive.target_heading)*reach;
                    if(approach==1)steps=(int)ceilf(3*fmaxf(reach,hypotf(ex-cx,ey-cy)));
                    int support=traffic_supported_corners(world,car,car->pos[0],car->pos[1],z,car->head,1);
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
                    z=car->pos[2];
                    for(float ahead=0;clear && ahead<=preview;ahead+=2) {
                        float point[2];ai_route_point(p,at,t,driver->drive.direction,ahead,point,&heading);
                        point[0]-=sinf(heading)*driver->drive.direction*offset;
                        point[1]+=cosf(heading)*driver->drive.direction*offset;
                        clear=race_ground_at(world->scene,point[0],point[1],z,&z)!=WSURF_NONE &&
                              race_pose_clear(world,car,point[0],point[1],z,heading);
                    }
                    if(clear){best=cost;best_offset=offset;}
                }
            }
            driver->blocked=!isfinite(best);
            if(!driver->blocked)driver->offset=best_offset;
        }
        if(!driver->blocked) {
            tx=driver->drive.target[0]-ox*driver->offset;
            ty=driver->drive.target[1]+oy*driver->offset;
            float angle=atan2f(ty-car->pos[1],tx-car->pos[0])-car->head;
            angle=atan2f(sinf(angle),cosf(angle));
            input.steer=fmaxf(-1,fminf(1,angle/.5f));
            /* Pace follows the selected corridor, not the blocked centreline. */
            target=pace/3.6f;
            if(!driver->drive.loop)target=fminf(target,sqrtf(6*fmaxf(0,driver->drive.length-driver->drive.progress-1)));
            target=fminf(target,(14+(pace-14)*fmaxf(0,cosf(fminf(fabsf(angle)*2,1.5707963f))))/3.6f);
        } else target=0;
    }
    if(driver->progress.finished)target=fminf(target,25/3.6f);
    /* Yield to bodies ahead; shared contacts resolve simultaneous impacts. */
    int ambient=world && world->ambient && world->ambient_routes?world->ambient_count:0;
    for(int j=0;j<count+(player!=NULL)+ambient;j++)if(j!=k) {
        int slot=j-count-(player!=NULL);
        if(slot>=0 && !world->ambient_routes[slot].present)continue;
        const AiCar *lead=j<count?&cars[j]:slot>=0?world->ambient+slot:player;
        float fx=cosf(car->head),fy=sinf(car->head),lx=lead->pos[0]-car->pos[0],ly=lead->pos[1]-car->pos[1];
        float along=lx*fx+ly*fy,across=fabsf(lx*fy-ly*fx);
        if(along<=0 || across>car->half_width+lead->half_width+.05f || fabsf(lead->pos[2]-car->pos[2])>2)continue;
        float gap=along-car->half_length-lead->half_length;
        float limit=fmaxf(0,(gap-1.2f)/.6f);
        if(world && world->scene && !driver->blocked && !driver->drive.failed &&
           fabsf(driver->offset-ai_course_lateral(driver->drive.path,car->pos))>.15f &&
           fabsf(input.steer)>.05f)
            limit=fmaxf(limit,3/3.6f); /* edge into a clear passing line; contacts remain physical */
        if(limit<target){target=limit;driver->drive.stalled=0;}
    }
    car->target_speed=target/PHYS_TICKRATE;
    if(!driver->drive.failed && !driver->drive.finished)
        input.throttle=ai_throttle(car->target_speed,car->spd);
    PhysVehicle stock=car->vehicle;
    if(driver->progress.kind==N2_RACE_DRAG && car->has_powertrain) {
        int shift=driver->gearbox.rpm>=car->powertrain.redline_rpm &&
                  driver->gearbox.gear<car->powertrain.gearbox[0].gear_count;
        float gain=phys_manual_step(&driver->gearbox,&car->powertrain,0,0,
            car->tyre_radius,car->spd,input.throttle,shift,1/PHYS_TICKRATE);
        car->vehicle.accel*=gain;if(gain<=0 && input.throttle>0)input.throttle=0;
        if(driver->gearbox.failed)driver->drive.failed=1;
    }
    float before[3]={car->pos[0],car->pos[1],car->pos[2]};
    int collision=ai_vehicle_step(car,input.throttle,input.steer,input.handbrake,world);car->vehicle=stock;
    if(driver->progress.kind==N2_RACE_DRIFT)race_drift_step(&driver->progress.drift,
        car->vel,car->head,hypotf(car->pos[0]-before[0],car->pos[1]-before[1]),
        car->ride.contact_mask!=0,collision,1/PHYS_TICKRATE);
    world_race_progress_update(&driver->progress,circuit,car->pos[0],car->pos[1]);
    if(driver->progress.finished && !driver->finishing){driver->finishing=1;driver->finish_at=driver->drive.progress;}
    car->lap=fmaxf(0,driver->progress.lap-1);car->prevrel=driver->drive.segment;
}
