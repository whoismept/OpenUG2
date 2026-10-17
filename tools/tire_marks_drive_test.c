/* Integration check of the tire-mark adapter (src/tire_marks_drive.h) against the
 * coupled Stage1-5 driving state on synthetic host fixtures: gripping cruise lays
 * nothing; a handbrake slide lays marks from the locked wheels; published
 * utilisation stays within 0..1. Non-vector behaviour checks; no retail data. */
#define STAGE5_TEST_NO_MAIN
#include "drive_stage5_test.c"
#include "tire_marks_drive.h"
static int flat_ground(void *u,int wheel,float x,float y,float z,float *h){(void)u;(void)wheel;(void)x;(void)y;(void)z;*h=0;return 1;}
static int used_strips(const TmStore *s){int n=0;for(int i=0;i<TM_MAX_STRIPS;i++)n+=s->strip[i].used;return n;}
static void tick_marks(Trace *t,TmStore *store,TmVehicle *v,const float width[4],float pedal,float steer,int hb){
    static const PhysRideState ride={.contact_mask=15};
    tick5(t,pedal,steer,hb,&ride);
    for(int i=0;i<4;i++){float u=t->drive.drivetrain.wheel[i].utilisation;assert(u>=0&&u<=1);}
    float m=t->drive.drivetrain.utilisation_mean;assert(m>=0&&m<=1);
    if(!tm_drive_gate(&t->drive,4001))return;
    TmWheelIn in[4];float O[3];tm_drive_inputs(&t->drive,width,0,in,O);
    TmGround g={flat_ground,NULL,0};tm_vehicle_update(store,v,in,O,0,&g);
}
int main(void){
    static TmStore store;TmVehicle v;Trace t;const float width[4]={.2f,.2f,.22f,.22f};
    memset(&v,0,sizeof v);tm_init(&store,128);contact_init(&t,20);
    /* the fixture starts the wheels from rest at speed: let that spin-up transient pass */
    for(int k=0;k<100;k++)tick_marks(&t,&store,&v,width,.3f,0,0);
    tm_vehicle_destroy(&v);tm_init(&store,128);
    for(int k=0;k<20;k++)tick_marks(&t,&store,&v,width,.3f,0,0);
    assert(tm_drive_gate(&t.drive,4001));
    printf("cruise: strips %d\n",used_strips(&store));
    assert(used_strips(&store)==0);
    for(int k=0;k<60;k++)tick_marks(&t,&store,&v,width,0,.5f,1);
    int strips=used_strips(&store);
    printf("handbrake slide: strips %d, sliding wheels %d%d%d%d\n",strips,!t.drive.drivetrain.wheel[0].gripping,
           !t.drive.drivetrain.wheel[1].gripping,!t.drive.drivetrain.wheel[2].gripping,!t.drive.drivetrain.wheel[3].gripping);
    assert(strips>0);
    for(int i=0;i<TM_MAX_STRIPS;i++)if(store.strip[i].used)assert(store.strip[i].surface==1&&store.strip[i].n>=1);
    t.drive.alternate_movement=1;assert(!tm_drive_gate(&t.drive,4001));
    /* TM-2 clause 3: only the reserved Organic-Drag venue block [4400,4499] drops the gate */
    t.drive.alternate_movement=0;
    assert(tm_drive_gate(&t.drive,4000));   /* free roam id (spec TM-2) */
    assert(tm_drive_gate(&t.drive,4001));   /* ordinary circuit event */
    assert(tm_drive_gate(&t.drive,4399)&&tm_drive_gate(&t.drive,4500));
    assert(!tm_drive_gate(&t.drive,4400)&&!tm_drive_gate(&t.drive,4401)&&!tm_drive_gate(&t.drive,4499));
    /* TM-2 clause 1 (amended): the pre-race start phase INCLUDING the countdown suspends
       wheel effects for every car in the field, so G is false there; it is lifted when the
       cars are put into play at race start (ACTIVE). */
    t.drive.session=(DriveSession){DRIVE_SESSION_COUNTDOWN,0};
    assert(!tm_drive_gate(&t.drive,4001));
    t.drive.session=(DriveSession){DRIVE_SESSION_SUSPENDED,0};
    assert(!tm_drive_gate(&t.drive,4001));
    t.drive.session=(DriveSession){DRIVE_SESSION_ACTIVE,0};
    assert(tm_drive_gate(&t.drive,4001));
    puts("tire-mark drive adapter: gate, venue block [4400,4499], countdown suspended, lay predicate on locked wheels, utilisation range passed");
    return 0;
}
