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
static void ai_vehicle_step(AiCar *car,float throttle,float steer,int handbrake,
                             const AiTrafficWorld *world) {
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
        if(world->obst && world->obstsrc)
            collide_body_walls(car->pos,car->vel,car->head,bb,world->obst,
                world->obstz,world->nobst,car->pos[2]+.05f,car->pos[2]+height,
                scene,world->obstsrc,NULL,0);
        world_body_wall_push(scene,car->pos,car->vel,car->head,bb,
                             car->pos[2]+.05f,car->pos[2]+height,NULL);
        world_ride_gather(scene,car->pos,car->head,car->vel,oldh,&car->ride,
                          &car->support,NULL,NULL,NULL);
    }
    float co=cosf(car->head),sn=sinf(car->head);
    car->spd=car->vel[0]*co+car->vel[1]*sn;
    phys_ride_apply_load(&car->ride,vehicle,ax*co+ay*sn,-ax*sn+ay*co,1.0f/60.0f);
    phys_ride_step(&car->ride,&car->support,1.0f/60.0f);
    car->pos[2]=car->ride.z;
    car->turn_rate=atan2f(sinf(car->head-oldh),cosf(car->head-oldh));
}

static float ai_throttle(float target,float speed) {
    float delta=(target-speed)*PHYS_TICKRATE;
    if(delta<0 && speed>0.002f)return fmaxf(-1.0f,delta*.8f);
    return fmaxf(0.0f,fminf(1.0f,delta*.5f));
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
    memset(roads,0,sizeof *roads);
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
        N2Leaf leaf[2];int nl=0;
        n2_find_leaves(data,0,len,0x00034121u,leaf,&nl,2);
        if(nl!=1 || leaf[0].size<8){free(data);continue;}
        long at=leaf[0].off+8,end=leaf[0].off+leaf[0].size;
        int oldn=roads->n,ok=1;
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
                if(!width){ok=0;break;}roads->half_width=width;cap=grown;
            }
            for(int j=0;j<count;j++) {
                float x,y;
                memcpy(&x,data+at+128+56*j+4,4);
                memcpy(&y,data+at+128+56*j+8,4);
                if(!isfinite(x)||!isfinite(y)||fabsf(x)>1e5f||fabsf(y)>1e5f){ok=0;break;}
                int id=roads->n++;
                roads->xy[id*2]=x;roads->xy[id*2+1]=y;
                roads->next[id]=j+1<count?id+1:-1;
                uint32_t widths=n2_u32(data+at+128+56*j+24);
                roads->half_width[id]=fminf((float)(widths&0xffffu),
                                             (float)(widths>>16))/256.0f;
            }
            if(!ok)break;
            at+=140L+56L*count;
            if(at==end+8)at=end; /* the final record omits the next-record header */
            else if(at>end){ok=0;break;}
        }
        if(!ok || at!=end) {
            roads->n=oldn;
            fprintf(stderr,"free-roam road paths: invalid record in %s\n",path);
        } else if(roads->n>oldn)nfiles++;
        free(data);
    }
    for(int f=0;f<nf;f++)free(files[f]);free(files);
    printf("free-roam road paths: %d nodes from %d RoutesFreeRoam.bin files\n",
           roads->n,nfiles);
    return roads->n;
}

static int traffic_edge_supported(N2Scene *scene,float x,float y,float dx,float dy,float z) {
    if(!scene)return 1;
    for(int q=1;q<=4;q++) {
        float nz=z;
        if(world_ground_at(scene,x+dx*q*0.25f,y+dy*q*0.25f,z,&nz)!=WSURF_ROAD ||
           fabsf(nz-z)>2.0f)return 0;
        z=nz;
    }
    return 1;
}

static float traffic_lane_tangent(const AiRoadNet *roads,N2Scene *scene,int from,int to,
                                  float t,float x,float y,float z,float heading,
                                  float *outx,float *outy,float *outz) {
    /* Navigation stays on the road even while the vehicle is airborne. */
    if(scene)world_ground_at(scene,x,y,z,&z);
    float width=roads->half_width
        ? roads->half_width[from]*(1.0f-t)+roads->half_width[to]*t : 4.0f;
    float offset=fminf(2.0f,width*0.45f);
    for(float side=offset;side>=0.45f;side*=0.5f) {
        float px=x+sinf(heading)*side,py=y-cosf(heading)*side,pz=z;
        if(!scene || (world_ground_at(scene,px,py,z,&pz)==WSURF_ROAD &&
                      fabsf(pz-z)<0.75f)) {
            *outx=px;*outy=py;*outz=pz;return side;
        }
    }
    *outx=x;*outy=y;*outz=z;
    return 0;
}

static int traffic_pose_clear(const AiTrafficWorld *world,const AiCar *car,
                              float x,float y,float z,float heading) {
    if(!world || !world->scene)return 1;
    N2Scene *scene=world->scene;
    float hl=car->half_length>1?car->half_length:2.2f;
    float hw=car->half_width>0.5f?car->half_width:1.0f;
    float height=car->height>1?car->height:1.6f;
    float fx=cosf(heading),fy=sinf(heading),sx=fy,sy=-fx;
    for(int a=-1;a<=1;a+=2)for(int b=-1;b<=1;b+=2) {
        float px=x+a*fx*hl+b*sx*hw;
        float py=y+a*fy*hl+b*sy*hw,pz=z;
        if(world_ground_at(scene,px,py,z,&pz)!=WSURF_ROAD ||
           fabsf(pz-z)>1.0f)return 0;
    }
    float bb[6]={-hl,-hw,0,hl,hw,height};
    float pos[3]={x,y,z},vel[2]={0,0};
    if(world->obst && world->obstsrc && world->nobst &&
       collide_body_walls(pos,vel,heading,bb,world->obst,world->obstz,
                          world->nobst,z+0.05f,z+height,scene,world->obstsrc,NULL,0))
        return 0;
    pos[0]=x;pos[1]=y;
    return !world_body_wall_push(scene,pos,vel,heading,bb,z+0.05f,z+height,NULL);
}

/* Keep going along the authored path. At its end, join another directed path
 * whose start is within five metres and best preserves the incoming heading. */
static int traffic_next(const AiRoadNet *roads,N2Scene *scene,int at,int previous,float z) {
    if(!roads || at<0 || at>=roads->n)return -1;
    const float *xy=roads->xy;
    if(scene && world_ground_at(scene,xy[at*2],xy[at*2+1],z,&z)!=WSURF_ROAD)return -1;
    float hx=0,hy=0;
    if(previous>=0) {
        hx=xy[at*2]-xy[previous*2];hy=xy[at*2+1]-xy[previous*2+1];
        float len=hypotf(hx,hy);if(len>0.01f){hx/=len;hy/=len;}
    }
    int best=-1;float score=-1e30f;
    for(int i=0;i<roads->n;i++) {
        int nb=roads->next[i];
        if(nb<0 || nb==previous)continue;
        float gap=hypotf(xy[i*2]-xy[at*2],xy[i*2+1]-xy[at*2+1]);
        if(gap>5.0f)continue;
        float dx=xy[nb*2]-xy[at*2],dy=xy[nb*2+1]-xy[at*2+1];
        float len=hypotf(dx,dy);
        if(len<1.0f || len>60.0f)continue;
        if(previous>=0 && (dx*hx+dy*hy)/len<-0.15f)continue;
        if(!traffic_edge_supported(scene,xy[at*2],xy[at*2+1],dx,dy,z))continue;
        float s=previous<0? -gap : (dx*hx+dy*hy)/len-gap*0.02f;
        if(s>score){score=s;best=nb;}
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
                     .after=traffic_next(roads,world->scene,to,from,z)};
    for(int i=0;i<15;i++) {
        route.along+=1.0f;
        float length=hypotf(roads->xy[2*route.to]-roads->xy[2*route.from],
                             roads->xy[2*route.to+1]-roads->xy[2*route.from+1]);
        if(route.along>=length) {
            if(route.after<0)return 0;
            route.along-=length;route.prev=route.from;route.from=route.to;route.to=route.after;
            route.after=traffic_next(roads,world->scene,route.to,route.from,z);
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
                       const AiRoadNet *roads, int k, int count) {
    /* ponytail: six-car local gap model; use decoded lane/signal rules if found. */
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
    for(int j=0;j<count;j++)if(j!=k && routes[j].present) {
        const AiCar *lead=&cars[j];
        if(fabsf(car->pos[2]-lead->pos[2])>2.5f ||
           fx*cosf(lead->head)+fy*sinf(lead->head)<0.8f)continue;
        float dx=lead->pos[0]-car->pos[0],dy=lead->pos[1]-car->pos[1];
        float ahead=dx*fx+dy*fy;
        if(ahead<=0 || ahead>100.0f ||
           fabsf(dx*fy-dy*fx)>car->half_width+lead->half_width+0.5f)continue;
        float gap=ahead-car->half_length-lead->half_length;
        float relative=fmaxf(0.0f,speed-lead->spd*PHYS_TICKRATE);
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
    const float angle=0.4f+(float)k*6.2831853f/N_OPENWORLD_AI+0.5f*(attempt%6);
    const float radius=85.0f+20.0f*(attempt%3);
    const float tx=player[0]+cosf(angle)*radius,ty=player[1]+sinf(angle)*radius;
    float fx=cosf(player_heading),fy=sinf(player_heading);
    int best=-1,to=-1;float bz=player[2];
    /* ponytail: heading masks the normal chase view; use camera occlusion if
       free-camera traffic spawning is needed. */
    for(int pass=0;pass<2 && best<0;pass++) {
    float score=1e30f;
    for(int i=0;i<roads->n;i++) {
        int next=roads->next[i];if(next<0)continue;
        float x=roads->xy[i*2],y=roads->xy[i*2+1];
        float px=x-player[0],py=y-player[1],d2=px*px+py*py;
        if(d2<75.0f*75.0f || d2>350.0f*350.0f ||
           px*fx+py*fy>-0.25f*sqrtf(d2) ||
           (world && hypotf(world->view[0],world->view[1])>0.5f &&
            !ai_traffic_offscreen(world->eye,world->view,(float[3]){x,y,player[2]})))continue;
        float s=(x-tx)*(x-tx)+(y-ty)*(y-ty);
        if(s>=score)continue;
        float z=player[2];
        if(scene && world_ground_at(scene,x,y,z,&z)!=WSURF_ROAD)continue;
        float dx=roads->xy[next*2]-x,dy=roads->xy[next*2+1]-y;
        float len=hypotf(dx,dy);
        if(len<1.0f || len>60.0f ||
           !traffic_edge_supported(scene,x,y,dx,dy,z))continue;
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
        if(!traffic_pose_clear(world,&cars[k],lx,ly,lz,atan2f(dy,dx)) ||
           !traffic_spawn_run_clear(roads,world,&cars[k],i,next,lz))continue;
        int occupied=0;
        for(int j=0;j<N_OPENWORLD_AI;j++)if(j!=k && routes[j].present &&
           fabsf(lz-cars[j].pos[2])<3.0f) {
            float sep=hypotf(lx-cars[j].pos[0],ly-cars[j].pos[1]);
            float same=cosf(atan2f(dy,dx)-cars[j].head);
            if(sep<(same>0.8f?40.0f:22.0f))occupied=1;
        }
        if(occupied)continue;
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
    routes[k].after=traffic_next(roads,scene,to,best,bz);
    routes[k].lane_offset=traffic_lane_tangent(roads,scene,best,to,0,
        roads->xy[best*2],roads->xy[best*2+1],bz,
        atan2f(dy,dx),
        &car->pos[0],&car->pos[1],&car->pos[2]);
    car->head=atan2f(dy,dx);
    car->spd=(k<N_AI ? 26.0f+3.0f*k : 75.0f+5.0f*(k-N_AI))/3.6f/PHYS_TICKRATE;
    routes[k].cruise_speed=car->spd;
    car->spd=fminf(car->spd,traffic_turn_speed(roads,&routes[k])/PHYS_TICKRATE);
    car->target_speed=car->spd;car->ride_ready=0;
    ai_motion_init(car,world);
    car->wheel_angle=car->turn_rate=0;car->braking=0;car->lap=car->prevrel=car->t=0;
    static const float colors[N_OPENWORLD_AI][3]={{.65f,.7f,.8f},{.2f,.55f,.3f},
        {.8f,.65f,.3f},{.6f,.35f,.3f},{.95f,.2f,.12f},{.1f,.5f,.95f}};
    memcpy(car->col,colors[k],sizeof car->col);
    return 1;
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
        if(!ai_traffic_respawn(roads,world,cars,routes,k,player,player_heading))break;
        count++;
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
        route->after=traffic_next(roads,scene,route->to,route->from,car->pos[2]);
    }
    AiTraffic target=*route;
    target.along+=fminf(10.0f,4.0f+fabsf(car->spd)*PHYS_TICKRATE*.35f);
    for(int hop=0;hop<8 && target.along>length;hop++) {
        if(target.after<0){target.along=length;break;}
        target.along-=length;target.prev=target.from;target.from=target.to;target.to=target.after;
        target.after=traffic_next(roads,scene,target.to,target.from,car->pos[2]);
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
