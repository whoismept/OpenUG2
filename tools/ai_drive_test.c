/* Synthetic only: drive through the same steering response and horizontal
 * player physics as gameplay. Live audit separately checks tyre/world contact. */
#include "ai.h"
#include "physics.h"
#include <assert.h>

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
    PhysVehicle heavy={0.85f,0.85f,0.8f,1.02f}; AiDrive d;
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
    for(int i=0;i<4;i++)for(int r=0;r<2;r++)drive_curve(i*1.570796327f,r);
    puts("ai_drive_test: PASS");return 0;
}
