/* Independent bounded Stage5 contacts. SI with tonnes/kN. External world and
 * body trajectory are explicit inputs, never inferred material/angular solvers. */
#ifndef OPENUG2_CONTACT_H
#define OPENUG2_CONTACT_H
#include "resistance.h"
#include "braking.h"
typedef struct { float progression,stiffness,d_positive,d_nonpositive,antiroll,upper,lower,reference; } CtAxle;
typedef struct {
 float centre[3],mount[4][3],radius[4],mass,dimensions[3],yaw_scale,aero_preload,bonus[2];
 CtAxle axle[2];
} CtCar;
typedef struct { float peak,slide,spin,body_drag,frequency,amplitude; } CtSurface;
/* S5-11 surface ID: the collision polygon's one-byte attribute is the numeric
 * ID verbatim over the 29-row channel domain (identity, no remap or clamp);
 * above the domain no row is defined. S5-11.5 fixes the fallback IDs. */
#define CT_SURFACE_ID_MIN 0
#define CT_SURFACE_ID_MAX 28
#define CT_SURFACE_ID_COUNT 29
#define CT_SURFACE_ID_INVALID (-1)
#define CT_SURFACE_ID_DEFAULT 1 /* no pack / no polygon / forced override */
#define CT_SURFACE_ID_STEEP 0   /* failed steep-normal test */
/* S5-11.7 source channel records: 29 fixed 152-byte rows. */
#define CT_SURFACE_RECORD_COUNT 29
#define CT_SURFACE_RECORD_SIZE 152
#define CT_SURFACE_OFFSET_FREQUENCY 0x18
#define CT_SURFACE_OFFSET_AMPLITUDE 0x1C
#define CT_SURFACE_OFFSET_PEAK 0x24
#define CT_SURFACE_OFFSET_SLIDE 0x28
#define CT_SURFACE_OFFSET_SPIN 0x2C
#define CT_SURFACE_OFFSET_BODY_DRAG 0x30
typedef struct { float travel,rate,scalar,load,point[3],centre[3],arm[3],normal[3]; int contact,surface; } CtWheel;
typedef struct { float position[3],velocity[3],omega[3]; ResBasis axes; } CtPose;
typedef struct { float height,normal[3],height_offset;int found,surface,override_surface; } CtGround;
typedef CtGround (*CtQuery)(void *user,const float point[3],int wheel);
typedef struct { float total,front,rear,front_share,rear_share; } CtPreload;
typedef struct { float factor,front,rear; } CtGrip;
typedef struct {
 CtCar car;CtWheel wheel[4];float mount[4][3],preload[4],blend,grip_front,grip_rear;
 unsigned exported_mask;int exported_surface[4];float exported_forward,exported_speed;
 float bottom_request[4]; /* reported even when host suppresses impulses */
 float exported_heading[3],exported_velocity[3];
 unsigned frame_refreshes,blend_refreshes,post_sweeps,zero_sweeps;
} CtState;
const CtSurface *ct_surface(int id);
int ct_surface_id(int attribute);
void ct_surface_record(const unsigned char record[CT_SURFACE_RECORD_SIZE],CtSurface *out);
float ct_bump(float x,float y,float frequency,float amplitude);
CtPreload ct_preload(float mass,float cx,float xf,float xr,int centre_switch,const ResCar *a,float kA,float v,float previous_enable);
void ct_inertia(const CtCar *car,float ordinary[3],float selected[3]);
void ct_change_centre(float position[3],const ResBasis *basis,const float old[3],const float changed[3]);
float ct_distance(float up_dot,float difference,int *surface);
float ct_ground_distance(const float point[3],const float up[3],const CtGround *ground,float bump,int *surface);
void ct_spring(CtWheel *wheel,const CtAxle *axle,float candidate,float pair,float preload,float h);
float ct_alignment(float lateral_dot_normal);
float ct_align(float scalar,const float lateral[3],const float normal[3]);
void ct_apply(const CtPose *pose,CtWheel *wheel,const ResBasis *axes,int selector,float fx,float fy,float force[3],float moment[3]);
float ct_bottom(float closing,float mass);
void ct_impulse(const float normal[3],const float arm[3],float requested,float linear_cap,float angular_cap,int suppressed,float linear[3],float angular[3]);
float ct_blend(float hb,float old,float dt);
float ct_eta(float hb,int role,float v,float sigma,float vm);
CtGrip ct_grip(int role,float eta,float sigma,float speed,float tilt,float blend,float b0,float b1,float event,float q,float X,float v0,float s0);
float ct_mph(float signed_forward);
void ct_init(CtState *state,const CtCar *car);
void ct_frame(CtState *state,const ResCar *aero,float previous_enable,int centre_switch,const float ride_offset[4]);
/* Ordered post-step; returns upper-limit mask. Caller applies capped impulses
 * then calls this again at h=0 once if any upper-limit test occurred. */
unsigned ct_post(CtState *state,const CtPose *pose,const float steering[4],float h,CtQuery query,void *user);
void ct_wheel_axes(const ResBasis *body,float steering,ResBasis *wheel);
void ct_export(CtState *state,const CtPose *pose);
#endif
