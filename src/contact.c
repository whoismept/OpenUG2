#include "contact.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
static void cross(const float a[3],const float b[3],float c[3]){float r[3]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};memcpy(c,r,sizeof r);}
static void transform(const ResBasis*b,const float r[3],float o[3]){for(int k=0;k<3;k++)o[k]=(float)((double)b->forward[k]*r[0]+(double)b->lateral[k]*r[1]+(double)b->up[k]*r[2]);}
static const CtSurface surfaces[29]={
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0f},
 {0.97000002861022949f,0.95787501335144043f,1.0f,1.0f,1.0f,0.05000000074505806f},
 {0.99000000953674316f,0.9812999963760376f,1.0f,1.0f,1.0f,0.019999999552965164f},
 {1.0f,1.0f,1.0099999904632568f,1.0f,1.0f,0.019999999552965164f},
 {1.0f,1.0f,1.0199999809265137f,1.0f,3.0f,0.029999999329447746f},
 {1.0f,1.0f,1.0199999809265137f,1.0f,3.0f,0.048000000417232513f},
 {0.95999997854232788f,0.95130002498626709f,1.1000000238418579f,1.0f,1.0f,0.0099999997764825821f},
 {0.95999997854232788f,0.94800001382827759f,1.5f,1.0f,1.0f,0.029999999329447746f},
 {0.95999997854232788f,0.94800001382827759f,1.0499999523162842f,5.0f,1.0f,0.05000000074505806f},
 {1.0f,1.0f,1.0f,1.0f,1.1000000238418579f,0.029999999329447746f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0099999997764825821f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0099999997764825821f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0099999997764825821f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0099999997764825821f},
 {1.0f,1.0f,1.0f,1.0f,1.0f,0.0099999997764825821f},
 {0.99000000953674316f,0.98100000619888306f,1.0f,1.0f,1.0f,0.0f},
 {0.99000000953674316f,0.98100000619888306f,1.0f,1.0f,1.0f,0.0f},
 {0.99000000953674316f,0.98100000619888306f,1.0f,1.0f,1.0f,0.0f},
 {0.99000000953674316f,0.9812999963760376f,1.0f,1.0f,1.0f,0.0f},
 {0.99000000953674316f,0.98100000619888306f,1.0f,1.0f,1.0f,0.0f},
 {0.99000000953674316f,0.98100000619888306f,1.0f,1.0f,1.0f,0.0f},
 {0.99000000953674316f,0.9812999963760376f,1.0f,1.0f,1.0f,0.0f},
};
const CtSurface *ct_surface(int id){return surfaces+id;}
int ct_surface_id(int attribute){return attribute>=CT_SURFACE_ID_MIN && attribute<=CT_SURFACE_ID_MAX ? attribute : CT_SURFACE_ID_INVALID;}
static float ct_record_f32(const unsigned char *p){uint32_t bits=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;float f;memcpy(&f,&bits,4);return f;}
void ct_surface_record(const unsigned char record[CT_SURFACE_RECORD_SIZE],CtSurface *out){
 out->frequency=ct_record_f32(record+CT_SURFACE_OFFSET_FREQUENCY);
 out->amplitude=ct_record_f32(record+CT_SURFACE_OFFSET_AMPLITUDE);
 out->peak=ct_record_f32(record+CT_SURFACE_OFFSET_PEAK);
 out->slide=ct_record_f32(record+CT_SURFACE_OFFSET_SLIDE);
 out->spin=ct_record_f32(record+CT_SURFACE_OFFSET_SPIN);
 out->body_drag=ct_record_f32(record+CT_SURFACE_OFFSET_BODY_DRAG);
}
static float noise(double i,double j){static const float samples[16]={-.5f,-.2f,.5f,.2f,-.4f,-.1f,.2f,.5f,-.2f,-.5f,.4f,-.3f,.3f,0,-.2f,.1f};double index=fmod(-5*i*j-3*(i+1)*(j+1),16);if(index<0)index+=16;return samples[(int)index];}
float ct_bump(float x,float y,float frequency,float amplitude){double px=(double)x*frequency,py=(double)y*frequency,i=floor(px),j=floor(py),u=px-i,w=py-j;return (float)(amplitude*((1-u)*(1-w)*noise(i,j)+u*(1-w)*noise(i+1,j)+(1-u)*w*noise(i,j+1)+u*w*noise(i+1,j+1)));}
CtPreload ct_preload(float mass,float cx,float xf,float xr,int enabled,const ResCar*a,float kA,float v,float enable){double c=enabled?cx:0;float sr=(float)(.5*(xf-c)/((double)xf-xr)),sf=(float)(.5-sr);float total=(float)((double)mass*9.8f+((double)a->front+a->rear)*enable*fmax(v,0)*kA);return (CtPreload){total,(float)((double)total*sf),(float)((double)total*sr),sf,sr};}
void ct_inertia(const CtCar*c,float ordinary[3],float selected[3]){for(int k=0;k<3;k++){double a=c->dimensions[(k+1)%3],b=c->dimensions[(k+2)%3];ordinary[k]=(float)(c->mass*(a*a+b*b)*(1.0f/12));selected[k]=ordinary[k];}selected[2]=(float)((double)selected[2]*c->yaw_scale);}
void ct_change_centre(float p[3],const ResBasis*b,const float old[3],const float changed[3]){float d[3],v[3];for(int k=0;k<3;k++)d[k]=changed[k]-old[k];transform(b,d,v);for(int k=0;k<3;k++)p[k]+=v[k];}
float ct_distance(float dot,float difference,int*surface){if(dot<.001f){*surface=CT_SURFACE_ID_STEEP;return -100;}return (float)((double)difference/dot);}
float ct_ground_distance(const float p[3],const float up[3],const CtGround*g,float bump,int*s){*s=g->override_surface?CT_SURFACE_ID_DEFAULT:g->surface;float difference=(float)(((g->found?(double)g->height+g->height_offset:-10000)+bump-p[2])*g->normal[2]);return ct_distance((float)res_dot(up,g->normal),difference,s);}
void ct_spring(CtWheel*w,const CtAxle*a,float candidate,float pair,float preload,float h){float old=w->travel;w->contact=candidate>=a->lower;w->travel=fmaxf(a->lower,fminf(a->upper,candidate));if(h>.0001f)w->rate=(float)(((double)w->travel-old)/h);double t=w->travel;w->scalar=w->contact?(float)fmax(0,preload+(1+a->progression*fmax(t,0))*a->stiffness*t+(w->rate>0?a->d_positive:a->d_nonpositive)*(double)w->rate+a->antiroll*(t-pair)):0;}
float ct_alignment(float dot){return (float)fmax(.3f,1-4*(double)dot*dot);}
float ct_align(float scalar,const float lateral[3],const float normal[3]){float first[3],second[3];cross(lateral,normal,first);cross(first,lateral,second);return scalar>0?(float)((double)scalar*fmax(.3f,4*res_dot(second,normal)-3)):0;}
void ct_apply(const CtPose*p,CtWheel*w,const ResBasis*axes,int selector,float fx,float fy,float force[3],float moment[3]){if(!w->contact)return;ResBasis a=*axes;float arm[3],f[3];if(selector==2||selector==3){double hn=res_dot(a.forward,w->normal),ln=res_dot(a.lateral,w->normal);for(int k=0;k<3;k++){a.forward[k]=(float)(a.forward[k]-hn*w->normal[k]);a.lateral[k]=(float)(a.lateral[k]-ln*w->normal[k]);}if(selector==3){double length=sqrt(res_dot(a.forward,a.forward));for(int k=0;k<3;k++)a.forward[k]=(float)(a.forward[k]/length);cross(w->normal,a.forward,a.lateral);}}if(selector>=2)memcpy(a.up,w->normal,sizeof a.up);for(int k=0;k<3;k++){f[k]=(float)((double)fx*a.forward[k]+(double)fy*a.lateral[k]+(double)w->load*a.up[k]);arm[k]=w->point[k]-p->position[k];}ResBody application={0};res_point_force(&application,f,arm,NULL);for(int k=0;k<3;k++){force[k]+=application.force[k];moment[k]+=application.moment[k];}}
float ct_bottom(float closing,float mass){return closing<0?0:(float)((double)(float)(.225f*closing)*mass);}
static void cap(float v[3],float limit){double length=sqrt(res_dot(v,v));if(length>limit)for(int k=0;k<3;k++)v[k]=(float)(v[k]*limit/length);}
void ct_impulse(const float n[3],const float arm[3],float request,float lc,float ac,int suppress,float linear[3],float angular[3]){for(int k=0;k<3;k++)linear[k]=suppress?0:request*n[k];cap(linear,lc);cross(arm,linear,angular);cap(angular,ac);}
float ct_blend(float hb,float old,float dt){return (float)(hb<.5f?fmin(1,(double)old+dt):fmax(.5,(double)old-20*dt));}
float ct_eta(float hb,int role,float v,float sigma,float vm){if(v<1)sigma=0;return (float)(hb+((hb>.2f&&(role==0||(fabsf(sigma)>.3f&&vm<80)))?.5:0));}
CtGrip ct_grip(int role,float eta,float sigma,float speed,float tilt,float blend,float b0,float b1,float event,float q,float X,float v0,float s0){double g=q*(role==0?(double).85f:1)*(1-(1-(double).975f)*X),f=1+fabs(tilt);if(!(eta>.5f&&fabs(sigma)<(double)20.0f*.01745329238474369f)){double c=b0+(double)b1*(role==0?1:event),s=s0+(1-(double)s0)*(speed-(double)v0)/(30-(double)v0);f+=fmin(.35f,c*s*fabs(sigma));}return (CtGrip){(float)f,(float)g,(float)(g*(1+(f-1)*blend))};}
float ct_mph(float v){return (float)((double)v*2.236999988555908f);}
void ct_init(CtState*s,const CtCar*c){memset(s,0,sizeof *s);s->car=*c;s->blend=s->grip_front=s->grip_rear=1;for(int i=0;i<4;i++)for(int k=0;k<3;k++)s->mount[i][k]=c->mount[i][k]-c->centre[k];}
void ct_frame(CtState*s,const ResCar*a,float previous,int enabled,const float offset[4]){CtPreload p=ct_preload(s->car.mass,s->car.centre[0],s->car.mount[0][0],s->car.mount[3][0],enabled,a,s->car.aero_preload,s->exported_forward,previous);for(int i=0;i<4;i++){s->preload[i]=i<2?p.front:p.rear;for(int k=0;k<3;k++)s->mount[i][k]=s->car.mount[i][k]-s->car.centre[k]-(k==2&&offset?offset[i]:0);}s->frame_refreshes++;}
void ct_wheel_axes(const ResBasis*b,float angle,ResBasis*w){float c=cosf(angle),s=sinf(angle);for(int k=0;k<3;k++){w->forward[k]=c*b->forward[k]+s*b->lateral[k];w->lateral[k]=-s*b->forward[k]+c*b->lateral[k];w->up[k]=b->up[k];}}
unsigned ct_post(CtState*s,const CtPose*p,const float steering[4],float h,CtQuery query,void*user){unsigned upper=0;s->post_sweeps++;if(h==0)s->zero_sweeps++;for(int i=0;i<4;i++){CtWheel*w=s->wheel+i;float q[3],r[3];transform(&p->axes,s->mount[i],r);for(int k=0;k<3;k++)q[k]=p->position[k]+r[k];CtGround g=query(user,q,i);const CtSurface*sf=ct_surface(g.surface);float bump=ct_bump(q[0],q[1],sf->frequency,sf->amplitude);float distance=ct_ground_distance(q,p->axes.up,&g,bump,&w->surface);memcpy(w->normal,g.normal,sizeof w->normal);ct_spring(w,&s->car.axle[i/2],distance+s->car.radius[i],s->wheel[i^1].travel,s->preload[i],h);if(w->travel==s->car.axle[i/2].upper)upper|=1u<<i;memcpy(r,s->mount[i],sizeof r);r[2]+=w->travel;transform(&p->axes,r,w->arm);for(int k=0;k<3;k++){w->centre[k]=p->position[k]+w->arm[k];if(w->contact)w->point[k]=w->centre[k]-s->car.radius[i]*w->normal[k];}ResBasis axes;ct_wheel_axes(&p->axes,steering[i],&axes);w->load=ct_align(w->scalar,axes.lateral,w->normal);}return upper;}
void ct_export(CtState*s,const CtPose*p){memcpy(s->exported_heading,p->axes.forward,sizeof s->exported_heading);memcpy(s->exported_velocity,p->velocity,sizeof s->exported_velocity);s->exported_mask=0;for(int i=0;i<4;i++){if(s->wheel[i].contact)s->exported_mask|=1u<<i;s->exported_surface[i]=s->wheel[i].surface;}s->exported_forward=(float)res_dot(p->velocity,p->axes.forward);s->exported_speed=(float)sqrt(res_dot(p->velocity,p->velocity));}
