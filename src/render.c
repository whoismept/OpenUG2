/* render.c — OpenUG2 Renderer module implementation. Everything here was
 * extracted verbatim from the original monolithic main.c; the parsing logic
 * (nfsu2.h) is untouched ground truth. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <zlib.h>   /* screenshot PNG (dev only) */

#include "render.h"

#if defined(__APPLE__) && !defined(N2_GLES)
#define glGenFramebuffers glGenFramebuffersEXT
#define glBindFramebuffer glBindFramebufferEXT
#define glFramebufferTexture2D glFramebufferTexture2DEXT
#define glCheckFramebufferStatus glCheckFramebufferStatusEXT
#define glDeleteFramebuffers glDeleteFramebuffersEXT
#define glGenRenderbuffers glGenRenderbuffersEXT
#define glBindRenderbuffer glBindRenderbufferEXT
#define glRenderbufferStorage glRenderbufferStorageEXT
#define glFramebufferRenderbuffer glFramebufferRenderbufferEXT
#define glDeleteRenderbuffers glDeleteRenderbuffersEXT
#endif

#ifdef N2_GLES
#  define GLSL_HEADER "precision mediump float;\n"
#else
#  define GLSL_HEADER "#version 120\n#define lowp\n#define mediump\n#define highp\n"
#endif

/* World glows use world coordinates, never the preceding car/smoke matrix.
   Additive fog fades to black so sprite rectangles cannot add fog colour. */
int render_world_glows(const RProg *r,const N2Batch *batches,int count,
                        const float mvp[16]) {
    float fog[3];glGetUniformfv(r->prog,r->uFogColor,fog);
    render_model(r,NULL);
    glUniformMatrix4fv(r->uMVP,1,GL_FALSE,mvp);
    glUniform1f(r->uAlpha,1);glUniform1f(r->uSoft,0);
    glUniform3f(r->uFogColor,0,0,0);
    glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);
    int draws=0;
    for(int i=0;i<count;i++) {
        const N2Batch *b=&batches[i];
        if(b->unresolved)continue; /* no texture-backed flare mask available */
        glUniform1f(r->uUnlit,b->tex?0:1);
        glUniform1f(r->uUseTex,b->tex?1:0);
        glUniform1f(r->uEmissiveTex,b->tex?1:0);
        glUniform3f(r->uColor,1,b->tex?1:.85f,b->tex?1:.5f);
        glBindTexture(GL_TEXTURE_2D,b->tex);
        draw_batch(b);draws++;
    }
    glUniform3fv(r->uFogColor,1,fog);
    glUniform1f(r->uEmissiveTex,0);glUniform1f(r->uUnlit,0);
    glDepthMask(GL_TRUE);glDisable(GL_BLEND);
    return draws;
}

int render_district_lights(const RProg *r, GpuMesh *quad, GLuint texture,
                           const N2LightSrc *lights, int nlights,
                           const float cam[3], const float look[3],
                           const float MVP[16], float viewdist,
                           float halo, float gain) {
    RProg rp = *r;
    int draws = 0;
    const GLint scalar_loc[] = {rp.uUnlit, rp.uEmissiveTex, rp.uUseTex,
                                rp.uSoft, rp.uAlpha};
    float scalars[5], color[3], fog_color[3], saved_mvp[16];
    for (int i=0; i<5; i++) glGetUniformfv(rp.prog,scalar_loc[i],scalars+i);
    glGetUniformfv(rp.prog,rp.uColor,color);
    glGetUniformfv(rp.prog,rp.uFogColor,fog_color);
    glGetUniformfv(rp.prog,rp.uMVP,saved_mvp);
    GLint texture_before, blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture_before);
    glGetIntegerv(GL_BLEND_SRC_RGB,&blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB,&blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA,&blend_dst_a);
    GLboolean blend_before=glIsEnabled(GL_BLEND), depth_before=glIsEnabled(GL_DEPTH_TEST);
    GLboolean depth_mask; glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_mask);
    float ll = sqrtf(look[0]*look[0] + look[1]*look[1] + look[2]*look[2]);
    if (ll < 1e-4f) ll = 1.0f;
    float ld[3] = {look[0]/ll, look[1]/ll, look[2]/ll};
    float rt[3] = {ld[1], -ld[0], 0};
    float rl = sqrtf(rt[0]*rt[0] + rt[1]*rt[1]);
    if (rl < 1e-4f) rl = 1.0f;
    rt[0] /= rl; rt[1] /= rl;
    float up[3] = {rt[1]*ld[2], -rt[0]*ld[2],
                   rt[0]*ld[1] - rt[1]*ld[0]};
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glUniform1f(rp.uUnlit, 0.0f); glUniform1f(rp.uEmissiveTex, 1.0f);
    glUniform1f(rp.uUseTex, 1.0f); glUniform1f(rp.uSoft, 0.0f);
    /* Additive emission fades toward zero, not fog RGB: black sprite edges
       must add nothing even at long distance. The sky path is unaffected. */
    glUniform3f(rp.uFogColor,0.0f,0.0f,0.0f);
    glBindTexture(GL_TEXTURE_2D, texture);
    float maxd2 = viewdist * viewdist;
    for (int i = 0; i < nlights; i++) {
        const N2LightSrc *light = &lights[i];
        float dx = light->pos[0]-cam[0], dy = light->pos[1]-cam[1],
              dz = light->pos[2]-cam[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > maxd2) continue;
        float d = sqrtf(d2); if (d < 0.5f) d = 0.5f;
        /* r_in (10 m on the shipped records) is the light's influence radius,
           NOT a sprite size. Drawing the quad that large buries it in its own
           lamp post and intersects the ground and nearby walls, and the depth
           test then slices it into hard-edged polygons -- the "solid shapes"
           dotted around the night city. A flare is a camera-facing halo: keep
           it at a near-constant angular size, bounded by the authored radius. */
        float s = d * 0.070f * halo;
        float smax = (light->r_in > 1.0f ? light->r_in : 10.0f) * 0.35f * halo;
        if (s > smax) s = smax;
        if (s < 0.45f) s = 0.45f;
        /* Keep the halo at its authored fixture. Scaling this depth bias with
           sprite size pulled distant lights metres through nearby buildings. */
        float ox = -ld[0]*0.02f, oy = -ld[1]*0.02f, oz = -ld[2]*0.02f;
        /* Fade over the last quarter of the view range instead of popping. */
        float fade = (viewdist - d) / (viewdist * 0.25f);
        if (fade > 1.0f) fade = 1.0f; else if (fade < 0.0f) fade = 0.0f;
        float M[16] = {
            rt[0]*s,rt[1]*s,rt[2]*s,0,
            up[0]*s,up[1]*s,up[2]*s,0,
            0,0,1,0,
            light->pos[0]+ox-(rt[0]+up[0])*s*0.5f,
            light->pos[1]+oy-(rt[1]+up[1])*s*0.5f,
            light->pos[2]+oz-(rt[2]+up[2])*s*0.5f,1
        };
        float LMVP[16]; mat_mul(MVP, M, LMVP);
        glUniformMatrix4fv(rp.uMVP, 1, GL_FALSE, LMVP);
        glUniform3f(rp.uColor,
            (float)( light->rgba        & 0xffu) / 255.0f,
            (float)((light->rgba >> 8)  & 0xffu) / 255.0f,
            (float)((light->rgba >> 16) & 0xffu) / 255.0f);
        float a = (float)((light->rgba >> 24) & 0xffu) / 255.0f * gain * fade;
        if (a > 1.0f) a = 1.0f;
        if (a <= 0.002f) continue;
        glUniform1f(rp.uAlpha, a);
        draw_gpumesh(quad); draws++;
    }
    for (int i=0; i<5; i++) glUniform1f(scalar_loc[i],scalars[i]);
    glUniform3fv(rp.uColor,1,color);
    glUniform3fv(rp.uFogColor,1,fog_color);
    glUniformMatrix4fv(rp.uMVP,1,GL_FALSE,saved_mvp);
    glBindTexture(GL_TEXTURE_2D,(GLuint)texture_before);
    glBlendFuncSeparate((GLenum)blend_src_rgb,(GLenum)blend_dst_rgb,
                        (GLenum)blend_src_a,(GLenum)blend_dst_a);
    if (blend_before) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth_before) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(depth_mask);
    return draws;
}

void render_wet_lights(const RProg *r,const N2LightSrc *lights,int count,
                       const float cam[3],float gain,int quality) {
    float pos[8][4]={{0}},color[8][4]={{0}},dist[8];
    for(int i=0;i<8;i++)dist[i]=3600;
    int cap=quality<=0?0:quality==1?4:8;
    for(int i=0;cap>0 && gain>0 && i<count;i++) {
        const N2LightSrc *l=&lights[i];
        if(!(l->rgba>>24))continue;
        float d=0;for(int a=0;a<3;a++){float v=l->pos[a]-cam[a];d+=v*v;}
        int k=0;while(k<cap && d>=dist[k])k++;
        if(k==cap)continue;
        for(int j=cap-1;j>k;j--){dist[j]=dist[j-1];memcpy(pos[j],pos[j-1],sizeof pos[j]);memcpy(color[j],color[j-1],sizeof color[j]);}
        dist[k]=d;
        memcpy(pos[k],l->pos,3*sizeof(float));pos[k][3]=fminf(l->r_out,60);
        for(int a=0;a<3;a++)color[k][a]=((l->rgba>>(8*a))&255)/255.0f;
        color[k][3]=(l->rgba>>24)/255.0f*gain*fminf(1,(60-sqrtf(d))/10);
    }
    glUniform4fv(r->uWetLightPos,8,&pos[0][0]);
    glUniform4fv(r->uWetLightColor,8,&color[0][0]);
}

void free_road_reflections(RoadReflections *s) {
    if(!s)return;
    glDeleteTextures(1,&s->color);glDeleteTextures(1,&s->depth);
    memset(s,0,sizeof *s);
}

static int road_reflection_receiver(const N2Batch *b,const float cam[3]) {
    if(!b->wettable || b->unresolved || b->drawmode>N2_DRAW_CUTOUT)return 0;
    float d=0;
    for(int a=0;a<3;a++) {
        float v=fmaxf(b->bbox_min[a]-cam[a],fmaxf(0,cam[a]-b->bbox_max[a]));d+=v*v;
    }
    return d<80*80;
}

int render_road_reflections(const RProg *r,RoadReflections *s,
                           const N2Batch *batches,int count,const float cam[3],
                           const float mvp[16],const float projection[16],
                           float wetness,int quality) {
    if(wetness<=0 || quality<=0 || count<=0)return 0;
    if(s->failed)return -1;
#ifdef N2_GLES
    /* ES2 does not guarantee depth-buffer copies; keep the wet sky/lamp
       fallback. Add an offscreen depth-texture target for a GLES backend. */
    s->failed=1;return -1;
#else
    int nearby=0;
    for(int i=0;i<count;i++)nearby+=road_reflection_receiver(&batches[i],cam);
    if(!nearby)return 0;
    GLint viewport[4],active,bindings[3];
    glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    if(viewport[2]<=0 || viewport[3]<=0)return 0;
    const GLenum units[]={GL_TEXTURE0,GL_TEXTURE3,GL_TEXTURE4};
    for(int i=0;i<3;i++){glActiveTexture(units[i]);glGetIntegerv(GL_TEXTURE_BINDING_2D,&bindings[i]);}
    GLuint *textures[]={&s->color,&s->depth};
    int resize=s->width!=viewport[2] || s->height!=viewport[3];
    for(int i=0;i<2;i++) {
        glActiveTexture(units[i+1]);
        if(!*textures[i])glGenTextures(1,textures[i]);
        glBindTexture(GL_TEXTURE_2D,*textures[i]);
        if(resize) {
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,i?GL_NEAREST:GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,i?GL_NEAREST:GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glCopyTexImage2D(GL_TEXTURE_2D,0,i?GL_DEPTH_COMPONENT:GL_RGBA,
                             viewport[0],viewport[1],viewport[2],viewport[3],0);
        } else glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,viewport[0],viewport[1],viewport[2],viewport[3]);
    }
    if(glGetError()!=GL_NO_ERROR) {
        free_road_reflections(s);s->failed=1;
        for(int i=0;i<3;i++){glActiveTexture(units[i]);glBindTexture(GL_TEXTURE_2D,bindings[i]);}
        glActiveTexture(active);return -1;
    }
    s->width=viewport[2];s->height=viewport[3];
    const GLint loc[]={r->uUnlit,r->uVista,r->uEmissiveTex,r->uUVCheck,r->uFlipN,
        r->uFresnel,r->uAlphaTest,r->uWetness,r->uRoadReflection};
    float saved[9],matrix[16],model[16],params[4];
    for(int i=0;i<9;i++)glGetUniformfv(r->prog,loc[i],saved+i);
    glGetUniformfv(r->prog,r->uMVP,matrix);glGetUniformfv(r->prog,r->uModel,model);
    glGetUniformfv(r->prog,r->uReflectionParams,params);
    GLint df,sr,dr,sa,da;GLboolean mask;
    GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),cull=glIsEnabled(GL_CULL_FACE);
    GLboolean offset=glIsEnabled(GL_POLYGON_OFFSET_FILL);
    glGetIntegerv(GL_DEPTH_FUNC,&df);glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);
    glGetIntegerv(GL_BLEND_SRC_RGB,&sr);glGetIntegerv(GL_BLEND_DST_RGB,&dr);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&sa);glGetIntegerv(GL_BLEND_DST_ALPHA,&da);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_EQUAL);glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    for(int i=0;i<7;i++)glUniform1f(loc[i],0);
    glUniform1f(r->uWetness,wetness);glUniform1f(r->uRoadReflection,1);
    glUniform4f(r->uReflectionParams,projection[10],projection[14],quality==1?16:32,0);
    glUniformMatrix4fv(r->uMVP,1,GL_FALSE,mvp);render_model(r,NULL);
    glActiveTexture(GL_TEXTURE0);
    int draws=0;
    for(int i=0;i<count;i++) {
        const N2Batch *b=&batches[i];if(!road_reflection_receiver(b,cam))continue;
        glBindTexture(GL_TEXTURE_2D,b->tex);
        glUniform1f(r->uAlphaTest,b->drawmode==N2_DRAW_CUTOUT?1:0);
        draw_batch(b);draws++;
    }
    for(int i=0;i<9;i++)glUniform1f(loc[i],saved[i]);
    glUniformMatrix4fv(r->uMVP,1,GL_FALSE,matrix);glUniformMatrix4fv(r->uModel,1,GL_FALSE,model);
    glUniform4fv(r->uReflectionParams,1,params);
    glDepthFunc(df);glDepthMask(mask);if(!depth)glDisable(GL_DEPTH_TEST);
    if(!blend)glDisable(GL_BLEND);if(cull)glEnable(GL_CULL_FACE);if(offset)glEnable(GL_POLYGON_OFFSET_FILL);
    glBlendFuncSeparate(sr,dr,sa,da);
    for(int i=0;i<3;i++){glActiveTexture(units[i]);glBindTexture(GL_TEXTURE_2D,bindings[i]);}
    glActiveTexture(active);
    return draws;
#endif
}

void render_camera_ease(float eye[3],const float previous[3],const float desired[3],
                        float stiffness,float ticks) {
    if(!isfinite(ticks) || ticks<=0)return;
    if(stiffness>=1){memcpy(eye,desired,3*sizeof *eye);return;}
    double x=-log1p(-fmax(.02,stiffness))*ticks;
    double blend=-expm1(-x);
    /* Integrate the moving target too: endpoint-only easing makes following
       distance change with FPS, most visibly at high vehicle speeds. */
    double travel=x<1e-5?x*.5-x*x/6:1-blend/x;
    for(int c=0;c<3;c++)eye[c]+=(float)(blend*(previous[c]-eye[c])+
                                             travel*(desired[c]-previous[c]));
}

void render_free_camera(float eye[3],float yaw,float pitch,const float move[3],float speed,float look[3]) {
    look[0]=cosf(yaw)*cosf(pitch);look[1]=sinf(yaw)*cosf(pitch);look[2]=sinf(pitch);
    float delta[3]={look[0]*move[0]+sinf(yaw)*move[1],
                    look[1]*move[0]-cosf(yaw)*move[1],look[2]*move[0]+move[2]};
    float length=sqrtf(delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2]);
    if(!isfinite(length) || !isfinite(speed))return;
    float step=fmaxf(0,speed)/fmaxf(1,length);
    for(int a=0;a<3;a++)eye[a]+=step*delta[a];
}

/* Affine projection along the same key-light direction as the lit materials.
 * ponytail: one receiving ground plane per car; a terrain shadow map is needed
 * if silhouettes must wrap stairs or nearby walls instead of the road plane. */
void mat_ground_shadow(const float ground[3],const float normal[3],float m[16]) {
    float light[3]={N2_SUN_X,N2_SUN_Y,N2_SUN_Z};
    float d=0,plane=.025f;
    for(int a=0;a<3;a++){d+=normal[a]*light[a];plane+=normal[a]*ground[a];}
    if(d<.15f){for(int a=0;a<3;a++)light[a]=normal[a];d=1;}
    memset(m,0,16*sizeof *m);m[15]=1;
    for(int a=0;a<3;a++) {
        for(int b=0;b<3;b++)m[b*4+a]=(a==b?1:0)-light[a]*normal[b]/d;
        m[12+a]=light[a]*plane/d;
    }
}

void free_car_shadow(CarShadow *s) {
    if(s->body)glDeleteBuffers(1,&s->body);
    if(s->wheel)glDeleteBuffers(1,&s->wheel);
    memset(s,0,sizeof *s);
}

/* Flatten indices once per loaded model, not once per frame/traffic actor.
 * Position-only casters merge material slices into one draw per rigid part. */
static int shadow_vertices(const N2Scene *car,int stock,const N2Scene *rims,
                           int wheel,GLuint *buffer,int *vertices) {
    const N2Scene *source=wheel && rims && rims->count?rims:car;
    size_t count=0;
    for(int i=0;i<source->count;i++) {
        const N2Mesh *m=source->meshes+i;
        if(wheel) {
            if(source==car && (stock<0 || m->car_mount!=N2_MOUNT_WHEEL ||
               m->tierid!=car->meshes[stock].tierid))continue;
        }else if(m->car_mount!=N2_MOUNT_BODY)continue;
        count+=(size_t)m->nidx;
    }
    if(!count){*vertices=0;return 1;}
    if(count>INT_MAX || count>SIZE_MAX/(3*sizeof(float)))return 0;
    float *positions=malloc(count*3*sizeof *positions);if(!positions)return 0;
    size_t n=0;
    for(int i=0;i<source->count;i++) {
        const N2Mesh *m=source->meshes+i;
        if(wheel) {
            if(source==car && (stock<0 || m->car_mount!=N2_MOUNT_WHEEL ||
               m->tierid!=car->meshes[stock].tierid))continue;
        }else if(m->car_mount!=N2_MOUNT_BODY)continue;
        for(int j=0;j+2<m->nidx;j+=3) {
            if(m->idx[j]>=m->nverts || m->idx[j+1]>=m->nverts || m->idx[j+2]>=m->nverts)continue;
            for(int k=0;k<3;k++){memcpy(positions+n*3,m->verts+m->idx[j+k]*5,3*sizeof(float));n++;}
        }
    }
    glGenBuffers(1,buffer);glBindBuffer(GL_ARRAY_BUFFER,*buffer);
    glBufferData(GL_ARRAY_BUFFER,n*3*sizeof *positions,positions,GL_STATIC_DRAW);
    free(positions);*vertices=(int)n;return 1;
}

int render_car_shadow(const RProg *r,CarShadow *s,const N2Scene *car,const GpuMesh *gpu,
                      int stock,const N2Scene *rims,const GpuMesh *rim_gpu,
                      const float model[16],const float wheels[4][16],const float mvp[16],
                      const float ground[3],const float normal[3],int quality) {
    if(!car || !car->count || !gpu || quality<=0)return 0;
    GLuint body_key=gpu[0].vbo,wheel_key=rims && rims->count && rim_gpu?rim_gpu[0].vbo:
                                    stock>=0?gpu[stock].vbo:0;
    if(s->body_key!=body_key || s->wheel_key!=wheel_key) {
        free_car_shadow(s);
        if(!shadow_vertices(car,stock,rims,0,&s->body,&s->body_count) ||
           !shadow_vertices(car,stock,rims,1,&s->wheel,&s->wheel_count)) {
            free_car_shadow(s);return -1;
        }
        s->body_key=body_key;s->wheel_key=wheel_key;
    }
    float projection[16],projected[16];mat_ground_shadow(ground,normal,projection);
    mat_mul(mvp,projection,projected);
    int draws=0;
    GLboolean cull=glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);glEnable(GL_STENCIL_TEST);glStencilMask(0xff);
    glStencilFunc(GL_EQUAL,0,0xff);glStencilOp(GL_KEEP,GL_KEEP,GL_INCR);
    glUniform1f(r->uUnlit,1);glUniform1f(r->uSoft,0);glUniform1f(r->uUseTex,0);
    glUniform3f(r->uColor,0,0,0);glUniform1f(r->uAlpha,.48f);
    glEnableVertexAttribArray(0);
    for(int a=1;a<4;a++)glDisableVertexAttribArray(a);
    /* Body and wheel coverage share the stencil reference for this actor. */
    for(int part=0;part<5;part++) {
        GLuint buffer=part?s->wheel:s->body;int n=part?s->wheel_count:s->body_count;
        if(!buffer || !n)continue;
        float matrix[16];mat_mul(projected,part?wheels[part-1]:model,matrix);
        glUniformMatrix4fv(r->uMVP,1,GL_FALSE,matrix);
        glBindBuffer(GL_ARRAY_BUFFER,buffer);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,0);
        glDrawArrays(GL_TRIANGLES,0,n);draws++;
    }
    glDisable(GL_STENCIL_TEST);glDepthMask(GL_TRUE);glDisable(GL_BLEND);
    glUniform1f(r->uUnlit,0);glUniform1f(r->uAlpha,1);
    if(cull)glEnable(GL_CULL_FACE);
    return draws;
}

void free_car_environment(CarEnvironment *s) {
    if(s->fbo)glDeleteFramebuffers(1,&s->fbo);
    if(s->depth)glDeleteRenderbuffers(1,&s->depth);
    glDeleteTextures(2,s->cube);
    if(s->capture.prog)glDeleteProgram(s->capture.prog);
    memset(s,0,sizeof *s);
}

int render_car_environment(const RProg *r,CarEnvironment *s,const N2Batch *world,int count,
                           const N2Batch *sky,int nsky,const float pos[3],int quality) {
    glUniform1f(r->uEnvReady,0);
    if(quality<=0 || count<=0 || s->failed)return 0;
    if(quality==1 && s->size==64 && s->ready && (++s->ticks&1)) {
        glUniform1f(r->uEnvReady,1);glUniform3fv(r->uEnvOrigin,1,s->published);return 0;
    }
    GLint program,fbo,rb,viewport[4],active,texture,depthfunc,sr,dr,sa,da;
    GLfloat clear[4];GLboolean depthmask,color_mask[4];
    GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),cull=glIsEnabled(GL_CULL_FACE);
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST),stencil=glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING,&rb);glGetIntegerv(GL_VIEWPORT,viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(GL_DEPTH_FUNC,&depthfunc);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);glGetBooleanv(GL_DEPTH_WRITEMASK,&depthmask);
    glGetBooleanv(GL_COLOR_WRITEMASK,color_mask);
    glGetIntegerv(GL_BLEND_SRC_RGB,&sr);glGetIntegerv(GL_BLEND_DST_RGB,&dr);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&sa);glGetIntegerv(GL_BLEND_DST_ALPHA,&da);
    int size=quality==1?64:128,draws=0;
    if(s->size!=size) {
        free_car_environment(s);s->size=size;s->capture=render_program();
        GLint linked=0;glGetProgramiv(s->capture.prog,GL_LINK_STATUS,&linked);
        if(!linked){free_car_environment(s);s->failed=1;draws=-1;goto restore;}
        glGenTextures(2,s->cube);glActiveTexture(GL_TEXTURE5);
        for(int t=0;t<2;t++) {
            glBindTexture(GL_TEXTURE_CUBE_MAP,s->cube[t]);
            for(int face=0;face<6;face++)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,
                GL_RGBA,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        glGenFramebuffers(1,&s->fbo);glGenRenderbuffers(1,&s->depth);
        glBindRenderbuffer(GL_RENDERBUFFER,s->depth);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,size,size);
    }
    if(s->face==0)memcpy(s->origin,pos,sizeof s->origin);
    glBindFramebuffer(GL_FRAMEBUFFER,s->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,s->depth);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_CUBE_MAP_POSITIVE_X+s->face,s->cube[1-s->front],0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {
        free_car_environment(s);s->failed=1;draws=-1;goto restore;
    }
    glViewport(0,0,size,size);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glUseProgram(s->capture.prog);
    glActiveTexture(GL_TEXTURE5);glBindTexture(GL_TEXTURE_CUBE_MAP,s->cube[s->front]);
    glActiveTexture(GL_TEXTURE0);
    const RProg *cr=&s->capture;float ambient,diffuse,fog,fc[3],sun[3];
    glGetUniformfv(r->prog,r->uAmbient,&ambient);glGetUniformfv(r->prog,r->uDiffuse,&diffuse);
    glGetUniformfv(r->prog,r->uFogDensity,&fog);glGetUniformfv(r->prog,r->uFogColor,fc);
    glGetUniformfv(r->prog,r->uLight,sun);
    glUniform1f(cr->uAmbient,ambient);glUniform1f(cr->uDiffuse,diffuse);
    glUniform3fv(cr->uFogColor,1,fc);glUniform3fv(cr->uLight,1,sun);
    glUniform3fv(cr->uCamPos,1,s->origin);glUniform1f(cr->uVColor,1);render_model(cr,NULL);
    static const float directions[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const float ups[6][3]={{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}};
    const float *f=directions[s->face],*up=ups[s->face];
    float right[3]={f[1]*up[2]-f[2]*up[1],f[2]*up[0]-f[0]*up[2],f[0]*up[1]-f[1]*up[0]};
    float vertical[3]={right[1]*f[2]-right[2]*f[1],right[2]*f[0]-right[0]*f[2],right[0]*f[1]-right[1]*f[0]};
    float view[16]={0},p[16],matrix[16];view[15]=1;
    for(int a=0;a<3;a++) {
        view[a*4]=right[a];view[a*4+1]=vertical[a];view[a*4+2]=-f[a];
        view[12]-=right[a]*s->origin[a];view[13]-=vertical[a]*s->origin[a];view[14]+=f[a]*s->origin[a];
    }
    mat_persp(1.570796327f,1,.15f,30000,p);mat_mul(p,view,matrix);
    glUniformMatrix4fv(cr->uMVP,1,GL_FALSE,matrix);
    glUniform1f(cr->uEmissiveTex,1);glUniform1f(cr->uFogDensity,0);glDepthMask(GL_FALSE);
    for(int i=0;i<nsky;i++)if(sky[i].tex) {
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(cr->uUseTex,1);glBindTexture(GL_TEXTURE_2D,sky[i].tex);draw_batch(sky+i);draws++;
    }
    glDepthMask(GL_TRUE);glDisable(GL_BLEND);glUniform1f(cr->uEmissiveTex,0);
    glUniform1f(cr->uFogDensity,fog);
    mat_persp(1.570796327f,1,.15f,350,p);mat_mul(p,view,matrix);
    glUniformMatrix4fv(cr->uMVP,1,GL_FALSE,matrix);
    for(int i=0;i<count;i++) {
        const N2Batch *b=world+i;
        if(b->unresolved || b->drawmode==N2_DRAW_BLEND || !render_batch_in_view(b,matrix))continue;
        glUniform1f(cr->uUseTex,b->tex?1:0);glUniform3f(cr->uColor,.28f,.29f,.31f);
        glUniform1f(cr->uAlphaTest,b->drawmode==N2_DRAW_CUTOUT?1:0);
        glUniform1f(cr->uTextureAlpha,b->drawmode==N2_DRAW_ADD?1:0);
        if(b->drawmode==N2_DRAW_ADD){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);}
        else {glDisable(GL_BLEND);glDepthMask(GL_TRUE);}
        glBindTexture(GL_TEXTURE_2D,b->tex);draw_batch(b);draws++;
    }
    if(++s->face==6){s->face=0;s->front=1-s->front;s->ready=1;memcpy(s->published,s->origin,sizeof s->published);}
restore:
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);glBindRenderbuffer(GL_RENDERBUFFER,rb);
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glUseProgram(program);
    glClearColor(clear[0],clear[1],clear[2],clear[3]);glColorMask(color_mask[0],color_mask[1],color_mask[2],color_mask[3]);
    glDepthFunc(depthfunc);glDepthMask(depthmask);glBlendFuncSeparate(sr,dr,sa,da);
    if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    if(cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
    if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    if(stencil)glEnable(GL_STENCIL_TEST);else glDisable(GL_STENCIL_TEST);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
    glActiveTexture(GL_TEXTURE5);glBindTexture(GL_TEXTURE_CUBE_MAP,s->cube[s->front]);
    glActiveTexture(active);
    glUniform1f(r->uEnvReady,s->ready?1:0);glUniform3fv(r->uEnvOrigin,1,s->published);
    return draws;
}

/* Stateless lens droplets: no gameplay RNG, simulation steps or per-drop draws. */
static uint32_t rain_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; return x ^ (x >> 16);
}
static float rain_unit(uint32_t x) { return (rain_hash(x) & 0xffffffu)/16777216.0f; }

/* One camera cover query, sampled by the caller at 4 Hz. No per-drop world scan. */
int render_rain_exposed(const N2Scene *scene,const float (*bounds)[4],const float cam[3]) {
    for(int mi=0;scene && mi<scene->count;mi++) {
        const N2Mesh *m=&scene->meshes[mi];
        if(m->cat==N2_SKY || m->cat==N2_GLOW)continue;
        if(bounds && (cam[0]<bounds[mi][0] || cam[0]>bounds[mi][2] ||
                      cam[1]<bounds[mi][1] || cam[1]>bounds[mi][3]))continue;
        for(int t=0;t+2<m->nidx;t+=3) {
            const float *a=m->verts+5*m->idx[t],*b=m->verts+5*m->idx[t+1],*c=m->verts+5*m->idx[t+2];
            if(fmaxf(a[2],fmaxf(b[2],c[2]))<=cam[2]+.1f)continue;
            float d=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1]);
            if(fabsf(d)<1e-7f)continue;
            float u=((b[1]-c[1])*(cam[0]-c[0])+(c[0]-b[0])*(cam[1]-c[1]))/d;
            float v=((c[1]-a[1])*(cam[0]-c[0])+(a[0]-c[0])*(cam[1]-c[1]))/d;
            if(u>=0 && v>=0 && u+v<=1 && u*a[2]+v*b[2]+(1-u-v)*c[2]>cam[2]+.1f)return 0;
        }
    }
    return 1;
}

int render_rain_vertices(float time,float intensity,int quality,float aspect,RainVertex *out) {
    if(!out || !isfinite(time) || time<0 || !isfinite(intensity) || intensity<=0 ||
       !isfinite(aspect) || aspect<=0)return 0;
    if(intensity>1)intensity=1;
    if(quality<0)quality=0; if(quality>2)quality=2;
    int n=0,count=12<<quality;
    static const unsigned char corner[6][2]={{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for(int i=0;i<count;i++) {
        uint32_t seed=rain_hash((uint32_t)i+71);
        if(rain_unit(seed+1)>intensity)continue;
        float life=3+rain_unit(seed+2)*4,phase=time/life+rain_unit(seed+3);
        float age=phase-floorf(phase);
        seed=rain_hash(seed+(uint32_t)floorf(phase));
        float cx=rain_unit(seed+4)*1.9f-.95f;
        float cy=rain_unit(seed+5)*1.9f-.8f-age*age*.28f;
        float h=.022f+rain_unit(seed+6)*.04f,w=h/(aspect*(1+age*.8f));
        float fade=fminf(1,age*10)*fminf(1,(1-age)*5);
        for(int q=0;q<6;q++) {
            RainVertex *o=&out[n++];float u=corner[q][0],v=corner[q][1];
            o->pos[0]=cx+(u-.5f)*w;o->pos[1]=cy+(v-.5f)*h;o->pos[2]=0;
            o->uv[0]=u;o->uv[1]=v;o->fade=fade;
        }
    }
    return n;
}

int render_rain(const RProg *r,GLuint *vbo,float time,float intensity,int quality,float aspect) {
    if(intensity<=0)return 0;
    RainVertex vertices[RAIN_MAX_DROPS*6];
    int n=render_rain_vertices(time,intensity,quality,aspect,vertices);
    if(!n)return 0;
    if(!*vbo)glGenBuffers(1,vbo);
    if(!*vbo)return 0;
    const GLint loc[]={r->uUnlit,r->uEmissiveTex,r->uUseTex,r->uSoft,r->uAlpha,r->uVista,r->uUVCheck};
    float saved[7],matrix[16];
    for(int i=0;i<7;i++)glGetUniformfv(r->prog,loc[i],saved+i);
    glGetUniformfv(r->prog,r->uMVP,matrix);
    GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),cull=glIsEnabled(GL_CULL_FACE),mask;
    GLint sr,dr,sa,da;
    glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);
    glGetIntegerv(GL_BLEND_SRC_RGB,&sr);glGetIntegerv(GL_BLEND_DST_RGB,&dr);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&sa);glGetIntegerv(GL_BLEND_DST_ALPHA,&da);
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(r->uUnlit,1);glUniform1f(r->uEmissiveTex,0);glUniform1f(r->uUseTex,0);
    glUniform1f(r->uVista,0);glUniform1f(r->uUVCheck,0);
    glUniform1f(r->uSoft,3);glUniform1f(r->uAlpha,.8f*fminf(intensity,1));
    const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    glUniformMatrix4fv(r->uMVP,1,GL_FALSE,identity);
    glBindBuffer(GL_ARRAY_BUFFER,*vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(n*sizeof *vertices),vertices,GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(RainVertex),(void*)0);
    glEnableVertexAttribArray(1);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(RainVertex),(void*)12);
    glEnableVertexAttribArray(3);glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,sizeof(RainVertex),(void*)20);
    /* No normal input is consumed by the unlit rain branch. Disable it so a
       preceding small mesh buffer cannot be indexed beyond its allocation. */
    glDisableVertexAttribArray(2);glVertexAttrib3f(2,0,0,1);
    glDrawArrays(GL_TRIANGLES,0,n);
    for(int i=0;i<7;i++)glUniform1f(loc[i],saved[i]);
    glUniformMatrix4fv(r->uMVP,1,GL_FALSE,matrix);
    glDepthMask(mask);if(depth)glEnable(GL_DEPTH_TEST);
    if(!blend)glDisable(GL_BLEND);if(cull)glEnable(GL_CULL_FACE);
    glBlendFuncSeparate(sr,dr,sa,da);
    return 1;
}

int render_collision_walls(const RProg *r,GLuint *vbo,const float *faces,int count,
                          const float color[3],const float mvp[16],int through) {
    if(!faces || count<=0 || count>WALL_DEBUG_MAX_FACES)return 0;
    if(!*vbo)glGenBuffers(1,vbo);
    if(!*vbo)return 0;
    static float edges[WALL_DEBUG_MAX_FACES*18];
    for(int t=0;t<count;t++)for(int e=0;e<3;e++) {
        memcpy(edges+t*18+e*6,faces+t*9+e*3,3*sizeof(float));
        memcpy(edges+t*18+e*6+3,faces+t*9+(e+1)%3*3,3*sizeof(float));
    }
    const GLint loc[]={r->uUnlit,r->uEmissiveTex,r->uUseTex,r->uSoft,r->uAlpha,r->uVista,r->uUVCheck};
    float saved[7],matrix[16],old_color[3];
    for(int i=0;i<7;i++)glGetUniformfv(r->prog,loc[i],saved+i);
    glGetUniformfv(r->prog,r->uMVP,matrix);glGetUniformfv(r->prog,r->uColor,old_color);
    GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),cull=glIsEnabled(GL_CULL_FACE),mask;
    GLint sr,dr,sa,da;
    glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);
    glGetIntegerv(GL_BLEND_SRC_RGB,&sr);glGetIntegerv(GL_BLEND_DST_RGB,&dr);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&sa);glGetIntegerv(GL_BLEND_DST_ALPHA,&da);
    if(through)glDisable(GL_DEPTH_TEST);else glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(r->uUnlit,1);glUniform1f(r->uEmissiveTex,0);glUniform1f(r->uUseTex,0);
    glUniform1f(r->uSoft,0);glUniform1f(r->uVista,0);glUniform1f(r->uUVCheck,0);
    glUniform3fv(r->uColor,1,color);glUniformMatrix4fv(r->uMVP,1,GL_FALSE,mvp);
    glBindBuffer(GL_ARRAY_BUFFER,*vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)count*27*sizeof(float),NULL,GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)count*9*sizeof(float),faces);
    glBufferSubData(GL_ARRAY_BUFFER,(GLintptr)count*9*sizeof(float),(GLsizeiptr)count*18*sizeof(float),edges);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,0);
    for(int a=1;a<4;a++)glDisableVertexAttribArray(a);
    glUniform1f(r->uAlpha,.10f);glDrawArrays(GL_TRIANGLES,0,count*3);
    glUniform1f(r->uAlpha,.85f);glDrawArrays(GL_LINES,count*3,count*6);
    for(int i=0;i<7;i++)glUniform1f(loc[i],saved[i]);
    glUniformMatrix4fv(r->uMVP,1,GL_FALSE,matrix);glUniform3fv(r->uColor,1,old_color);
    glDepthMask(mask);if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    if(!blend)glDisable(GL_BLEND);if(cull)glEnable(GL_CULL_FACE);
    glBlendFuncSeparate(sr,dr,sa,da);
    return 2;
}

static const char *VS =
    GLSL_HEADER
    "attribute vec3 aPos; attribute vec2 aUV; attribute vec3 aNor; attribute vec4 aColor;\n"
    "uniform mat4 uMVP; uniform mat4 uModel; varying vec2 vUV; varying vec3 vN; varying float vDepth;\n"
    "varying vec3 vPos; varying vec4 vColor;\n"
    /* clip.w == view-space depth under a perspective projection, for world
       (P*V) and car (P*V*M) alike — no view matrix or extra uniforms needed.
       NDC-drawn HUD quads have w==1, so fog leaves them alone. */
    "void main(){ vUV=aUV; vN=(uModel*vec4(aNor,0.0)).xyz;\n"
    "  vPos=(uModel*vec4(aPos,1.0)).xyz; vColor=aColor;\n"
    "  gl_Position=uMVP*vec4(aPos,1.0); vDepth=gl_Position.w; }\n";

static const char *FS =
    GLSL_HEADER
    "varying vec2 vUV; varying vec3 vN; varying float vDepth; varying vec3 vPos;\n"
    "varying vec4 vColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uVColor;\n"   /* 0 off, 1 world MODULATE2X, 2 car diffuse */
    "uniform float uUseTex; uniform vec3 uColor; uniform float uUnlit; uniform float uAlpha; uniform float uSoft; uniform float uSpec; uniform float uDecal;\n"
    "uniform float uAmbient; uniform float uDiffuse; uniform vec3 uLight;\n"
    "uniform vec3 uFogColor; uniform float uFogDensity;\n"
    "uniform vec3 uCamPos; uniform float uEnv; uniform float uUVCheck;\n"
    "uniform samplerCube uEnvCube; uniform float uEnvReady; uniform vec3 uEnvOrigin;\n"
    "uniform float uGloss; uniform float uFlipN;\n"
    "uniform float uWetness, uWeatherTime, uRainIntensity;\n"
    "uniform vec4 uWetLightPos[8],uWetLightColor[8];\n"
    "uniform float uPaintLights;\n"
    "uniform float uRoadReflection; uniform vec4 uReflectionParams;\n"
    "uniform sampler2D uReflectionColor,uReflectionDepth; uniform mat4 uMVP;\n"
    "uniform float uRimTint;\n"   /* >0: recolor the rim diffuse toward uColor */
    "uniform vec3 uEmissive;\n"   /* lamp emission added on top of the lit result */
    "uniform float uVista;\n"     /* >0.5: authored backdrop pass, alpha-blended */
    "uniform float uAlphaTest;\n" /* >0.5: discard below 0.5 texture alpha */
    "uniform float uTextureAlpha;\n" /* >0.5: output alpha = texture2D(...).a *
        uAlpha instead of plain uAlpha (M135-R: N2_DRAW_BLEND/ADD world
        batches only -- everything else, including CUTOUT, cars, and every
        HUD/debug uUnlit pass, leaves this at its default 0.0 and keeps its
        existing uAlpha-only alpha output unchanged) */
    "uniform float uEmissiveTex;\n" /* authored texture-backed unlit pass */
    "uniform float uFresnel;\n"   /* >0.5: glass -- fresnel drives alpha + reflection */
    "uniform float uClearcoat;\n" /* >0: second tight specular lobe over the base coat */
    "uniform vec4 uHeadPos[2];\n"
    "uniform vec3 uHeadForward, uHeadRight, uHeadUp;\n"
    "uniform vec4 uHeadShape;\n" /* horizontal/vertical spread, downward slope, range */
    "uniform float uHeadGain, uHeadLow;\n"
    "uniform float uHeadShadow;\n"
    "uniform sampler2D uHeadShadowMap0, uHeadShadowMap1;\n"
    "uniform mat4 uHeadShadowMVP[2];\n"
    "float headVisibility(int h,vec3 position,vec3 normal,float along){\n"
    "  if(uHeadShadow<0.5)return 1.0;\n"
    "  float offset=0.005+along*max(uHeadShape.x,uHeadShape.y)*2.0/512.0;\n"
    "  vec4 clip=uHeadShadowMVP[h]*vec4(position+normal*offset,1.0);\n"
    "  vec2 uv=clip.xy/clip.w*0.5+0.5;\n"
    "  if(clip.w<=0.0 || uv.x<0.0 || uv.x>1.0 || uv.y<0.0 || uv.y>1.0)return 1.0;\n"
    "  float depth=(clip.w-0.025-0.001*along)/uHeadShape.w;\n"
    "  float visibility=0.0;\n"
    "  for(int x=0;x<2;x++)for(int y=0;y<2;y++){\n"
    "    vec2 at=uv+(vec2(float(x),float(y))-0.5)/512.0;\n"
    "    vec2 depthRG=h==0?texture2D(uHeadShadowMap0,at).rg:texture2D(uHeadShadowMap1,at).rg;\n"
    "    visibility+=step(depth,dot(depthRG,vec2(1.0,1.0/255.0)))*0.25;\n"
    "  }\n"
    "  return visibility;\n"
    "}\n"
    "float wetHash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}\n"
    "float wetNoise(vec2 p){\n"
    "  vec2 i=floor(p),f=fract(p); f=f*f*(3.0-2.0*f);\n"
    "  return mix(mix(wetHash(i),wetHash(i+vec2(1,0)),f.x),mix(wetHash(i+vec2(0,1)),wetHash(i+vec2(1,1)),f.x),f.y);\n"
    "}\n"
    /* Screen-space reflection: bounded trace against this frame's depth.
       Missing/offscreen geometry keeps the ordinary wet material underneath. */
    "vec4 roadReflection(vec3 N,vec3 V,float wet,float puddle){\n"
    "  if(wet<0.001 || distance(vPos,uCamPos)>80.0)return vec4(0.0);\n"
    "  vec3 direction=reflect(-V,N),origin=vPos+N*0.04;\n"
    "  float previous=0.0;\n"
    "  for(int i=0;i<32;i++){\n"
    "    if(float(i)>=uReflectionParams.z)break;\n"
    "    float stepSize=32.0/uReflectionParams.z;\n"
    "    float t=0.3+pow((float(i)+1.0)*stepSize,2.0)*0.045;\n"
    "    vec4 clip=uMVP*vec4(origin+direction*t,1.0);\n"
    "    vec2 uv=clip.xy/clip.w*0.5+0.5;\n"
    "    if(clip.w<=0.1 || min(uv.x,uv.y)<0.002 || max(uv.x,uv.y)>0.998)break;\n"
    "    float depth=texture2D(uReflectionDepth,uv).r;\n"
    "    float surface=uReflectionParams.y/(depth*2.0-1.0+uReflectionParams.x);\n"
    "    if(depth<0.999999 && clip.w>surface){\n"
    "      float lo=previous,hi=t;\n"
    "      for(int j=0;j<5;j++){\n"
    "        float mid=(lo+hi)*0.5;\n"
    "        clip=uMVP*vec4(origin+direction*mid,1.0); uv=clip.xy/clip.w*0.5+0.5;\n"
    "        depth=texture2D(uReflectionDepth,uv).r;\n"
    "        surface=uReflectionParams.y/(depth*2.0-1.0+uReflectionParams.x);\n"
    "        if(clip.w>surface)hi=mid;else lo=mid;\n"
    "      }\n"
    "      clip=uMVP*vec4(origin+direction*hi,1.0); uv=clip.xy/clip.w*0.5+0.5;\n"
    "      depth=texture2D(uReflectionDepth,uv).r;\n"
    "      surface=uReflectionParams.y/(depth*2.0-1.0+uReflectionParams.x);\n"
    "      float gap=clip.w-surface;\n"
    "      if(hi<0.6 || depth>=0.999999 || gap<0.0 || gap>0.4+surface*0.003)return vec4(0.0);\n"
    "      float edge=smoothstep(0.0,0.08,min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y)));\n"
    "      float fres=0.04+0.96*pow(1.0-max(dot(N,V),0.0),3.0);\n"
    "      float fade=(1.0-smoothstep(30.0,46.0,hi))*(1.0-smoothstep(60.0,80.0,distance(vPos,uCamPos)));\n"
    "      return vec4(texture2D(uReflectionColor,uv).rgb,edge*fade*fres*(0.18*wet+0.72*puddle));\n"
    "    }\n"
    "    previous=t;\n"
    "  }\n"
    "  return vec4(0.0);\n"
    "}\n"
    /* exp^2 distance fog: fades far batches into the sky colour (which is
       cleared to uFogColor, so the horizon and the haze always agree) */
    "void main(){\n"
    /* diagnostic UV visualization: bypasses lighting/texture/fog entirely.
       R=u, G=v (raw, unclamped -- a mesh whose UVs run outside [0,1] shows
       as a flat saturated patch instead of a gradient, exposing tiling
       regions at a glance) with a darkened 10x10 grid overlaid so uneven
       cell spacing reveals stretching and grid-line direction reveals
       flips/mirrors. Checked first so it overrides every other path. */
    "  if(uUVCheck>0.5){\n"
    "    vec2 g=abs(fract(vUV*10.0-0.5)-0.5);\n"
    "    float grid=smoothstep(0.0,0.04,min(g.x,g.y));\n"
    "    gl_FragColor=vec4(vec3(vUV.x,vUV.y,0.2)*mix(0.35,1.0,grid),1.0); return;\n"
    "  }\n"
    "  float fog = clamp(exp(-pow(vDepth*uFogDensity, 2.0)), 0.0, 1.0);\n"
    /* M132-R2 vista path. The backdrops are authored as cut-out sheets: their
       DXT1 blocks carry one-bit transparency (up to 57.7% of texels fully
       clear) and their DXT3 blocks carry real gradients, all of which the
       decoder used to throw away -- which is exactly why a panorama rendered
       as an opaque black-edged slab. Alpha here comes only from those two
       proven sources, texture alpha and source vertex-colour alpha; nothing is
       keyed off black pixels. Fully clear texels are discarded so they cannot
       write depth-adjacent artefacts or fringe. */
    "  if(uVista>0.5){\n"
    "    vec4 t = texture2D(uTex, vUV);\n"
    "    vec3 c = uUseTex>0.5 ? t.rgb : uColor;\n"
    "    float a = (uUseTex>0.5 ? t.a : 1.0) * vColor.a;\n"
    "    if(a < 0.02) discard;\n"
    "    gl_FragColor = vec4(mix(uFogColor, c, fog), a);\n"
    "    return;\n"
    "  }\n"
    "  if(uEmissiveTex>0.5){\n"
    "    vec4 t=texture2D(uTex,vUV);\n"
    "    float a=t.a*uAlpha; if(a<0.02) discard;\n"
    "    gl_FragColor=vec4(mix(uFogColor,t.rgb*uColor,fog),a); return;\n"
    "  }\n"
    "  if(uUnlit>0.5){ float a=uAlpha;\n"
    /* Shadow footprints need a filled rounded rectangle; lamp/neon sprites
       retain their radial falloff. Both fade to zero at the quad boundary. */
    "    if(uSoft>0.5){\n"
    /* ponytail: transparent lens highlights, no refraction copy. Add image
       distortion only if its measured cost fits the frame budget. */
    "      if(uSoft>2.5){\n"
    "        vec2 p=vUV*2.0-1.0; p.x*=1.0+0.12*p.y; float d=dot(p,p); if(d>1.0)discard;\n"
    "        float rim=smoothstep(0.48,0.83,d)*(1.0-smoothstep(0.83,1.0,d));\n"
    "        float glint=pow(max(0.0,1.0-length(p-vec2(-0.25,0.35))*3.0),3.0);\n"
    "        float shade=clamp(0.35+p.y*0.65+glint,0.0,1.0);\n"
    "        vec3 drop=mix(vec3(0.015,0.025,0.035),vec3(0.85,0.92,1.0),shade);\n"
    "        gl_FragColor=vec4(drop,vColor.r*uAlpha*(0.04*(1.0-d)+0.26*rim+0.28*glint));return;\n"
    "      }\n"
    "      else if(uSoft>1.5){ float d=length(max(abs(vUV-vec2(0.5))-vec2(0.4),vec2(0.0))); a*=1.0-smoothstep(0.02,0.1,d); }\n"
    "      else { float d=length(vUV-vec2(0.5)); a*=clamp(1.0-d*2.0,0.0,1.0); } a*=a; }\n"
    "    gl_FragColor=vec4(mix(uFogColor,uColor,fog),a); return; }\n"
    /* Positions, normals, light and camera share world space. The model
       includes road tilt and each wheel/brake hub's own transform. */
    "  vec3 L=normalize(uLight); vec3 N=normalize(vN);\n"
    "  if(uFlipN>0.5) N = -N;\n"
    "  vec3 V=normalize(uCamPos - vPos);\n"
    /* Two-sided glass reflects the viewer-facing side; a backwards normal
       otherwise forces Fresnel to 1 and makes the far window fully opaque. */
    "  if(uFresnel>0.5 && dot(N,V)<0.0) N=-N;\n"
    /* Wet asphalt keeps a broad sheen; only sparse, nearly level patches
       collect standing water. Keep the reflection normal steady: unfiltered
       fine rain ripples alias at road distance and jump screen-space hits.
       ponytail: analytic sky/key-light fallback for offscreen geometry; use
       environment probes if reflections must survive outside the camera. */
    "  float wet=uWetness*smoothstep(0.55,0.9,N.z);\n"
    "  float puddle=0.0;\n"
    "  if(wet>0.001){\n"
    "    float pools=0.7*wetNoise(vPos.xy*0.24)+0.3*wetNoise(vPos.xy*0.73);\n"
    "    puddle=0.45*wet*smoothstep(0.66,0.85,pools)*smoothstep(0.94,0.99,N.z);\n"
    "  }\n"
    "  if(uRoadReflection>0.5){\n"
    "    if(uAlphaTest>0.5 && texture2D(uTex,vUV).a<0.5)discard;\n"
    "    vec4 reflected=roadReflection(N,V,wet,puddle); reflected.a*=fog;\n"
    "    gl_FragColor=reflected;return;\n"
    "  }\n"
    "  float nl=max(dot(N,L),0.0);\n"
    "  float d=uAmbient+uDiffuse*nl;\n"   /* directional; reveals body form */
    /* uAmbient/uDiffuse are one shared per-frame pair (world+cars both read
       them), so retuning the defaults to fix a car panel would also retune
       every building and road. Cars get their own lower ambient floor /
       sharper diffuse swing right here instead, gated the same way as the
       cavity term above — world batches always set uSpec=0, so they never
       take this branch. */
    /* Paint was clipping to white: at 0.55/1.2 the diffuse term alone reached
       ~0.95, which the shared *1.35 exposure below pushed past 1.0 before the
       specular, rim and environment terms were even added -- so every panel
       saturated and the highlights had no headroom left to show against.
       Pulling the body's own diffuse back leaves that headroom for them. */
    "  if(uSpec>0.001) d=uAmbient*0.35+uDiffuse*0.80*nl;\n"
    /* uDecal: paint under an alpha-masked decal atlas (badges/vinyls) —
       texture RGB shows only where its alpha says so, paint elsewhere */
    "  vec4 t = texture2D(uTex,vUV);\n"
    /* N2_DRAW_CUTOUT world batches and authored wheel textures. Threshold 0.5
       matches the authored railing/fence texture's own 1-bit DXT1 alpha
       (fully 0 or 255, no partial value to tune against). */
    "  if(uAlphaTest>0.5 && t.a<0.5) discard;\n"
    "  vec3 base = uUseTex>0.5 ? (uDecal>0.5 ? mix(uColor,t.rgb,t.a) : t.rgb) : uColor;\n"
    /* Rim paint: recolor the diffuse toward uColor while KEEPING its detail.
       Luminance drives the shade, uColor picks the metal, so a gold OEM sheet
       becomes any painted colour (silver by default) with spokes/shadows
       intact. uRimTint 0 = raw OEM texture, 1 = fully painted. */
    "  if(uRimTint>0.001){ float y=dot(t.rgb, vec3(0.299,0.587,0.114));\n"
    "    base = mix(base, uColor*(0.25+1.5*y), uRimTint); }\n"
    /* cheap view-angle cavity darkening (cars only, uSpec>0 — the world
       batches always set uSpec=0): panel gaps/creases have no texture data
       to show them (verified: body meshes carry no diffuse map at all), so
       this fakes the early-2000s baked-AO look by darkening paint where the
       surface turns away from the camera, instead of claiming detail that
       isn't in the asset. Narrowed from pow4/0.6 to pow6/0.78: at pow4 the
       falloff reached far onto flat panels and fought the fresnel edge
       brightening below, which is what flattened the paint into matte clay.
       It now only bites in the last few degrees, i.e. actual creases. */
    "  if(uSpec>0.001){\n"
    "    float edge=pow(1.0-clamp(dot(N,V),0.0,1.0), 6.0);\n"
    "    base *= mix(1.0, 0.78, edge);\n"
    "  }\n"
    /* Phong: reflect the light about the normal and test it against the VIEW
       vector. The old form used dot(N,L) with no V term at all, so it was a
       sharpened diffuse -- the highlight could not travel across a panel as
       the camera moved, which is what made the paint read flat/matte. */
    "  float rl = max(dot(reflect(-L,N), V), 0.0);\n"
    "  float sp = pow(rl, uGloss)*uSpec;\n"
    /* clear coat: automotive paint is a coloured base coat under a clear
       lacquer, so it carries TWO highlights -- the broad soft one from the
       pigment (sp above) and a small hard one from the lacquer surface. One
       lobe alone is what makes painted metal read as moulded plastic. */
    "  sp += pow(rl, 160.0)*uClearcoat;\n"
    "  float rim = pow(1.0-abs(N.z), 3.0)*uSpec*0.4;\n"        /* fresnel-ish edge sheen */
    /* The city capture supplies the clear-coat shape; the night key light
       must not clip the hood to white and hide those reflected buildings. */
    "  if(uEnvReady>0.5 && uClearcoat>0.001){sp*=0.30;rim*=0.25;}\n"
    "  vec3 lit = base*d*1.35 + sp + rim;\n"
    /* World prelight uses MODULATE2X (0.5 neutral); textureless vehicle assets
       use the same byte slot as direct diffuse color for windows and trim. */
    "  if(uVColor>1.5) lit *= vColor.rgb;\n"
    "  else if(uVColor>0.001) lit *= clamp(vColor.rgb*2.0, 0.0, 1.6);\n"
    "  if(wet>0.001){\n"
    "    lit*=1.0-0.32*wet;\n"
    "    float wf=0.025+0.975*pow(1.0-max(dot(N,V),0.0),5.0);\n"
    "    vec3 sky=mix(uFogColor,vec3(0.16,0.20,0.28),0.5);\n"
    "    lit=mix(lit,sky,clamp(wf*(0.3*wet+0.65*puddle),0.0,0.85));\n"
    "    lit+=vec3(0.65,0.72,0.85)*pow(rl,mix(36.0,120.0,puddle))*wet*0.35;\n"
    "  }\n"
    /* ponytail: camera-local unshadowed district highlights. Add per-car light
       selection for distant cars, light shadows for leakage; headlights use shadows. */
    "  if(wet>0.001 || uPaintLights*uClearcoat>0.001)for(int i=0;i<8;i++){\n"
    "    vec3 delta=uWetLightPos[i].xyz-vPos; float distance=length(delta);\n"
    "    if(uWetLightColor[i].a>0.0 && distance<uWetLightPos[i].w){\n"
    "      vec3 toLight=delta/max(distance,0.001);\n"
    "      float reflected=max(dot(reflect(-toLight,N),V),0.0);\n"
    "      float attenuation=1.0-smoothstep(0.0,uWetLightPos[i].w,distance);\n"
    "      vec3 radiance=uWetLightColor[i].rgb*uWetLightColor[i].a*attenuation;\n"
    "      if(wet>0.001 && delta.z>0.0)lit+=radiance*pow(reflected,mix(18.0,85.0,puddle))*wet*(0.3+1.7*puddle);\n"
    /* Existing clear-coat classification excludes rubber, cabin and plastic.
       No new scene pass or reflection copy: source lamps, normal and view
       determine a moving paint highlight in this same material draw. */
    "      if(uPaintLights*uClearcoat>0.001){\n"
    "        float glint=0.3*uSpec*pow(reflected,max(uGloss,24.0))+0.65*uClearcoat*pow(reflected,96.0);\n"
    "        lit+=radiance*glint*uPaintLights*max(dot(N,toLight),0.0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    /* Surface lighting and per-lamp world-geometry shadows. */
    "  if(uHeadGain>0.0) for(int h=0;h<2;h++){\n"
    "    vec3 ray=vPos-uHeadPos[h].xyz; float dist=length(ray);\n"
    "    float along=dot(ray,uHeadForward);\n"
    "    if(uHeadPos[h].w>0.5 && along>0.05 && dist<uHeadShape.w){\n"
    "      float lateral=dot(ray,uHeadRight)/along;\n"
    "      float elevation=dot(ray,uHeadUp)/along;\n"
    "      vec2 cone=vec2(lateral/uHeadShape.x,(elevation+uHeadShape.z)/uHeadShape.y);\n"
    "      float edge=1.0-smoothstep(0.55,1.0,length(cone));\n"
    "      float cutoff=mix(1.0,1.0-smoothstep(-0.018,-0.008,elevation),uHeadLow);\n"
    "      float reach=1.0-smoothstep(uHeadShape.w*0.65,uHeadShape.w,dist);\n"
    "      float lambert=max(dot(N,-ray/max(dist,0.001)),0.0);\n"
    "      if(edge*cutoff*lambert>0.0)lit+=base*vec3(1.0,0.94,0.82)*edge*cutoff*reach*lambert*uHeadGain*5.0*headVisibility(h,vPos,N,along)/(1.0+dist*dist/350.0);\n"
    "      if(wet>0.001 && edge*cutoff>0.0){\n"
    "        float highlight=pow(max(dot(reflect(ray/max(dist,0.001),N),V),0.0),mix(24.0,100.0,puddle));\n"
    "        lit+=vec3(1.0,0.94,0.82)*highlight*wet*edge*cutoff*reach*uHeadGain*headVisibility(h,vPos,N,along)/(1.0+dist*dist/350.0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    /* Local scene cube, sampled in world space so reflections stay anchored
       while the body banks. Fresnel adds clear-coat reflection without washing
       the base paint into the old constant warm horizon band. */
    "  float fres = 0.18 + 0.82*pow(1.0-clamp(dot(N,V),0.0,1.0), 3.0);\n"
    "  if(uEnv>0.001){\n"
    "    vec3 R = reflect(-V, N);\n"
    "    float up = clamp(R.z*0.5+0.5, 0.0, 1.0);\n"
    "    vec3 env = mix(vec3(0.025,0.025,0.035), vec3(0.10,0.14,0.24), up);\n"
    "    float local=uEnvReady*(1.0-smoothstep(35.0,100.0,length(vPos-uEnvOrigin)));\n"
    "    env=mix(env,textureCube(uEnvCube,R).rgb,local);\n"
    "    lit=mix(lit,env,clamp(uEnv*fres,0.0,0.85));\n"
    "  }\n"
    /* lit alpha = uAlpha (1 everywhere but the blended glass pass), so
       translucent glass keeps its specular highlight. M135-R: authored
       BLEND/ADD world batches and blended wheels multiply in the TEXTURE's own alpha
       (uTextureAlpha>0.5) -- otherwise a cutout-shaped blend/additive
       texture (e.g. a lit-window sheet with transparent gaps) would draw
       fully opaque/full-strength through those gaps, since uAlpha alone
       carries no per-texel information. */
    /* Lamp emission. Added after every lighting term so a lit lens keeps its
       highlight, rim sheen and environment reflection and still reads as
       switched on. The dome weight is the same dot(N,V) the fresnel uses: a
       curved lens is hottest where its surface faces the camera and falls off
       toward the rim, which is the difference between a lens and a sticker.
       The 0.55 floor keeps the lamp clearly lit all over. */
    "  if(dot(uEmissive,uEmissive)>0.0) lit += uEmissive*(0.55+0.45*clamp(dot(N,V),0.0,1.0));\n"
    "  float outA = uTextureAlpha>0.5 ? t.a*uAlpha : uAlpha;\n"
    /* Glass (uFresnel, the car glass pass only). A flat per-pass alpha is what
       made the cabin read as a still pool: every window sat at one constant
       opacity regardless of angle, so it looked like a filled surface rather
       than a pane. Real glass is nearly clear head-on and a mirror at grazing
       incidence, so alpha rides the same fresnel term the reflection does. */
    "  if(uFresnel>0.5) outA = clamp(mix(uAlpha, 1.0, fres), 0.0, 1.0);\n"
    "  gl_FragColor=vec4(mix(uFogColor, lit, fog), outA);\n"
    "}\n";

/* ---- tiny 4x4 matrix (column-major) ---- */
void mat_mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k*4+j]*b[i*4+k]; o[i*4+j] = s;
    }
}
void mat_persp(float f, float ar, float n, float fa, float *m) {
    float t = 1.0f/tanf(f/2);
    memset(m, 0, 16*sizeof(float));
    m[0]=t/ar; m[5]=t; m[10]=(fa+n)/(n-fa); m[11]=-1; m[14]=2*fa*n/(n-fa);
}
static void vnorm(float *v){ float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l<1e-6f)l=1;
    v[0]/=l; v[1]/=l; v[2]/=l; }
void mat_trans(float x,float y,float z,float *m){
    float r[16]={1,0,0,0,0,1,0,0,0,0,1,0,x,y,z,1}; memcpy(m,r,sizeof r); }
void mat_rotz(float a, float *m){ float c=cosf(a),s=sinf(a);
    float r[16]={c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1}; memcpy(m,r,sizeof r); }
/* Car model matrix: local +X = forward (heading), +Z = up (the ground normal),
   translated to pos + rideh along up. Forward is projected onto the plane
   perpendicular to up so the chassis banks with the road instead of staying
   level. up must be unit; a flat up=(0,0,1) reproduces mat_trans*mat_rotz. */
void mat_car(const float *pos, float heading, const float *up, float rideh, float *m){
    float f[3]={cosf(heading),sinf(heading),0};
    float d=f[0]*up[0]+f[1]*up[1]+f[2]*up[2];
    f[0]-=d*up[0]; f[1]-=d*up[1]; f[2]-=d*up[2]; vnorm(f);   /* forward in the plane */
    float l[3]={up[1]*f[2]-up[2]*f[1], up[2]*f[0]-up[0]*f[2], up[0]*f[1]-up[1]*f[0]}; /* left = up x fwd */
    float r[16]={ f[0],f[1],f[2],0,  l[0],l[1],l[2],0,  up[0],up[1],up[2],0,
        pos[0]+up[0]*rideh, pos[1]+up[1]*rideh, pos[2]+up[2]*rideh, 1 };
    memcpy(m,r,sizeof r);
}
void mat_car_footprint(const float pos[3],float heading,const float up[3],
                       const float bb[6],float length_scale,float width_scale,
                       float lift,float m[16]) {
    mat_car(pos,heading,up,lift,m);
    float sx=fmaxf(.5f,bb[3]-bb[0])*length_scale;
    float sy=fmaxf(.5f,bb[4]-bb[1])*width_scale;
    float x=(bb[0]+bb[3]-sx)*.5f,y=(bb[1]+bb[4]-sy)*.5f;
    for(int a=0;a<3;a++) {
        m[12+a]+=m[a]*x+m[4+a]*y;
        m[a]*=sx;m[4+a]*=sy;
    }
}

/* right-handed lookAt, column-major, up = world +Z */
void mat_lookat(const float *eye, const float *fwd, float *m) {
    float f[3]={fwd[0],fwd[1],fwd[2]}; vnorm(f);
    float up[3]={0,0,1};
    float s[3]={ f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0] }; vnorm(s);
    float u[3]={ s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0] };
    float r[16]={ s[0],u[0],-f[0],0,  s[1],u[1],-f[1],0,  s[2],u[2],-f[2],0,
        -(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]),
        -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]),
          f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2], 1 };
    memcpy(m, r, sizeof r);
}

/* minimal PNG writer (dev screenshot). rgb is top-left origin, w*h*3. */
static void png_chunk(FILE *f, const char *tag, const unsigned char *d, uLong n) {
    unsigned char len[4] = { n>>24, n>>16, n>>8, n };
    fwrite(len, 1, 4, f);
    uLong crc = crc32(0, (const Bytef*)tag, 4);
    crc = crc32(crc, d, n);
    fwrite(tag, 1, 4, f); fwrite(d, 1, n, f);
    unsigned char c[4] = { crc>>24, crc>>16, crc>>8, crc };
    fwrite(c, 1, 4, f);
}
void write_png(const char *path, int w, int h, const unsigned char *rgb) {
    /* raw scanlines with filter byte 0 */
    uLong raw_n = (uLong)h * (1 + w*3);
    unsigned char *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        raw[y*(1+w*3)] = 0;
        memcpy(raw + y*(1+w*3) + 1, rgb + (size_t)y*w*3, w*3);
    }
    uLong comp_n = compressBound(raw_n);
    unsigned char *comp = malloc(comp_n);
    compress2(comp, &comp_n, raw, raw_n, 9);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        unsigned char ihdr[13] = { w>>24,w>>16,w>>8,w, h>>24,h>>16,h>>8,h, 8,2,0,0,0 };
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", comp, comp_n);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw); free(comp);
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
        fprintf(stderr, "shader: %s\n", log); }
    return s;
}

/* Colour maps plus a depth renderbuffer work without depth-texture extensions.
 * ponytail: fixed 512px world-only maps; add vehicle casters when their renderer
 * supports receiving world-space lights. Alpha-blended surfaces do not cast. */
void free_headlight_shadows(HeadlightShadows *s) {
    glDeleteTextures(2,s->texture);glDeleteTextures(1,&s->white);
    glDeleteRenderbuffers(1,&s->depth);glDeleteFramebuffers(1,&s->fbo);
    if(s->program)glDeleteProgram(s->program);
    memset(s,0,sizeof *s);
}

static int headlight_shadow_init(HeadlightShadows *s) {
    const char *vs=GLSL_HEADER
        "attribute vec3 aPos;attribute vec2 aUV;uniform mat4 uMVP;"
        "varying vec2 uv;varying float distance;"
        "void main(){uv=aUV;gl_Position=uMVP*vec4(aPos,1.0);distance=gl_Position.w;}";
    const char *fs=GLSL_HEADER
        "uniform sampler2D uTex;uniform float uCutout,uRange;"
        "varying vec2 uv;varying float distance;"
        "void main(){if(uCutout>0.5 && texture2D(uTex,uv).a<0.5)discard;"
        "vec2 enc=fract(clamp(distance/uRange,0.0,0.99999)*vec2(1.0,255.0));"
        "enc.x-=enc.y/255.0;gl_FragColor=vec4(enc,0.0,1.0);}";
    GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs);
    s->program=glCreateProgram();glAttachShader(s->program,v);glAttachShader(s->program,f);
    glBindAttribLocation(s->program,0,"aPos");glBindAttribLocation(s->program,1,"aUV");
    glLinkProgram(s->program);glDeleteShader(v);glDeleteShader(f);
    GLint linked=0;glGetProgramiv(s->program,GL_LINK_STATUS,&linked);if(!linked)return 0;
    s->mvp=glGetUniformLocation(s->program,"uMVP");
    s->cutout=glGetUniformLocation(s->program,"uCutout");
    s->range=glGetUniformLocation(s->program,"uRange");
    glGenTextures(2,s->texture);
    for(int h=0;h<2;h++) {
        glBindTexture(GL_TEXTURE_2D,s->texture[h]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    unsigned char white[]={255,255,255};N2Tex tex={.w=1,.h=1,.rgb=white};s->white=upload_tex(&tex);
    glGenFramebuffers(1,&s->fbo);glBindFramebuffer(GL_FRAMEBUFFER,s->fbo);
    glGenRenderbuffers(1,&s->depth);glBindRenderbuffer(GL_RENDERBUFFER,s->depth);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,512,512);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,s->depth);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->texture[0],0);
    return s->white && glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
}

int render_batch_in_view(const N2Batch *b,const float m[16]) {
    for(int axis=0;axis<3;axis++)for(int sign=-1;sign<=1;sign+=2) {
        float plane[4];for(int a=0;a<4;a++)plane[a]=m[a*4+3]+sign*m[a*4+axis];
        float best=plane[3];
        for(int a=0;a<3;a++)best+=plane[a]*(plane[a]>=0?b->bbox_max[a]:b->bbox_min[a]);
        if(best<-.01f)return 0;
    }
    return 1;
}

int render_headlight_shadows(const RProg *r,HeadlightShadows *s,const N2Batch *batches,int count) {
    float gain;glGetUniformfv(r->prog,r->uHeadGain,&gain);
    glUniform1f(r->uHeadShadow,0.0f);
    if(gain<=0 || s->failed)return s->failed?-1:0;
    float pos[2][4],forward[3],right[3],up[3],shape[4];
    /* glGetUniformfv returns one array element, not the whole array. */
    glGetUniformfv(r->prog,r->uHeadPos,pos[0]);
    glGetUniformfv(r->prog,glGetUniformLocation(r->prog,"uHeadPos[1]"),pos[1]);
    glGetUniformfv(r->prog,r->uHeadForward,forward);glGetUniformfv(r->prog,r->uHeadRight,right);
    glGetUniformfv(r->prog,r->uHeadUp,up);glGetUniformfv(r->prog,r->uHeadShape,shape);
    GLint program,fbo,rb,viewport[4],active,texture,depthfunc;
    GLfloat clear[4],clear_depth;GLboolean mask[4],depthmask;
    GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),cull=glIsEnabled(GL_CULL_FACE);
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST),dither=glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING,&rb);glGetIntegerv(GL_VIEWPORT,viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(GL_DEPTH_FUNC,&depthfunc);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK,&depthmask);glGetFloatv(GL_DEPTH_CLEAR_VALUE,&clear_depth);
    int draws=0;float matrices[2][16];
    if(!s->fbo && !headlight_shadow_init(s)) {
        free_headlight_shadows(s);s->failed=1;
        fprintf(stderr,"Headlight shadows unavailable; retaining unshadowed beams.\n");
        draws=-1;goto restore;
    }
    glBindFramebuffer(GL_FRAMEBUFFER,s->fbo);glViewport(0,0,512,512);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);glDisable(GL_DITHER);
#ifdef N2_GLES
    glClearDepthf(1.0f);
#else
    glClearDepth(1.0);
#endif
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(1,1,1,1);
    glUseProgram(s->program);glUniform1f(s->range,shape[3]);
    for(int h=0;h<2;h++) {
        float dir[3],vertical[3],view[16]={0},projection[16];
        for(int a=0;a<3;a++){dir[a]=forward[a]-up[a]*shape[2];vertical[a]=up[a]+forward[a]*shape[2];}
        vnorm(dir);vnorm(vertical);view[15]=1;
        for(int a=0;a<3;a++) {
            view[a*4]=right[a];view[a*4+1]=vertical[a];view[a*4+2]=-dir[a];
            view[12]-=right[a]*pos[h][a];view[13]-=vertical[a]*pos[h][a];view[14]+=dir[a]*pos[h][a];
        }
        mat_persp(2*atanf(shape[1]*1.15f),shape[0]/shape[1],.05f,shape[3],projection);
        mat_mul(projection,view,matrices[h]);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->texture[h],0);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        if(pos[h][3]<.5f)continue;
        glUniformMatrix4fv(s->mvp,1,GL_FALSE,matrices[h]);
        for(int i=0;i<count;i++) {
            const N2Batch *b=batches+i;
            if(b->drawmode!=N2_DRAW_OPAQUE && b->drawmode!=N2_DRAW_CUTOUT)continue;
            if(!render_batch_in_view(b,matrices[h]))continue;
            glBindTexture(GL_TEXTURE_2D,b->tex?b->tex:s->white);
            glUniform1f(s->cutout,b->tex && b->drawmode==N2_DRAW_CUTOUT?1.0f:0.0f);
            draw_batch(b);draws++;
        }
    }
restore:
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)fbo);glBindRenderbuffer(GL_RENDERBUFFER,(GLuint)rb);
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glUseProgram((GLuint)program);
    glClearColor(clear[0],clear[1],clear[2],clear[3]);glColorMask(mask[0],mask[1],mask[2],mask[3]);
    glDepthFunc((GLenum)depthfunc);glDepthMask(depthmask);
#ifdef N2_GLES
    glClearDepthf(clear_depth);
#else
    glClearDepth(clear_depth);
#endif
    if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    if(cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
    if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    if(dither)glEnable(GL_DITHER);else glDisable(GL_DITHER);
    glBindTexture(GL_TEXTURE_2D,(GLuint)texture);
    if(draws>=0) {
        glUniformMatrix4fv(r->uHeadShadowMVP,2,GL_FALSE,matrices[0]);
        /* Units 1 and 2 are reserved for the two maps; ordinary textures use 0. */
        for(int h=0;h<2;h++){glActiveTexture(GL_TEXTURE1+h);glBindTexture(GL_TEXTURE_2D,s->texture[h]);}
        glUniform1i(r->uHeadShadowMap[0],1);glUniform1i(r->uHeadShadowMap[1],2);
        glUniform1f(r->uHeadShadow,1.0f);
    }
    glActiveTexture((GLenum)active);return draws;
}

void render_headlights(const RProg *r, const float model[16], const float anchors[4][4],
                       int high, float pitch, float range, float gain) {
    float pos[2][4];
    for(int b=0;b<2;b++) {
        for(int a=0;a<3;a++)pos[b][a]=model[a]*anchors[b][0]+model[4+a]*anchors[b][1]+
                                    model[8+a]*anchors[b][2]+model[12+a];
        pos[b][3]=anchors[b][3];
    }
    glUniform4fv(r->uHeadPos,2,&pos[0][0]);
    glUniform3fv(r->uHeadForward,1,model);
    glUniform3fv(r->uHeadRight,1,model+4);
    glUniform3fv(r->uHeadUp,1,model+8);
    glUniform4f(r->uHeadShape,high?.20f:.38f,high?.095f:.12f,
                tanf(pitch*3.14159265f/180.0f),fmaxf(range,1.0f));
    glUniform1f(r->uHeadLow,high?0.0f:1.0f);
    glUniform1f(r->uHeadGain,fmaxf(gain,0.0f));
    glUniform1f(r->uHeadShadow,0.0f); /* maps must be refreshed for this pose */
}

void render_tail_lamp(const RProg *r,GLuint texture,int running,int braking,
                      int boost,float gain) {
    int on=running || braking,hot=braking || boost;
    float level=on?fmaxf(gain,0.0f):0.0f;
    glUniform1f(r->uUnlit,0.0f);
    glUniform3f(r->uColor,.17f,.020f,.016f);
    glUniform3f(r->uEmissive,(hot?1.00f:.62f)*level,
                            (hot?.16f:.05f)*level,(hot?.10f:.04f)*level);
    glUniform1f(r->uSpec,.26f);glUniform1f(r->uEnv,.16f);
    glUniform1f(r->uGloss,20.0f);glUniform1f(r->uClearcoat,0.0f);
    glUniform1f(r->uDecal,0.0f);glUniform1f(r->uUseTex,texture?1.0f:0.0f);
    if(texture)glBindTexture(GL_TEXTURE_2D,texture);
}

void render_model(const RProg *r,const float model[16]) {
    static const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    glUniformMatrix4fv(r->uModel,1,GL_FALSE,model?model:identity);
}

RProg render_program(void) {
    RProg r;
    r.prog = glCreateProgram();
    glAttachShader(r.prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(r.prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(r.prog, 0, "aPos");
    glBindAttribLocation(r.prog, 1, "aUV");
    glBindAttribLocation(r.prog, 2, "aNor");
    glBindAttribLocation(r.prog, 3, "aColor");
    glLinkProgram(r.prog); glUseProgram(r.prog);
    r.uMVP     = glGetUniformLocation(r.prog, "uMVP");
    r.uModel   = glGetUniformLocation(r.prog, "uModel");
    render_model(&r,NULL);
    r.uUseTex  = glGetUniformLocation(r.prog, "uUseTex");
    r.uColor   = glGetUniformLocation(r.prog, "uColor");
    r.uUnlit   = glGetUniformLocation(r.prog, "uUnlit");
    r.uAlpha   = glGetUniformLocation(r.prog, "uAlpha");
    r.uSoft    = glGetUniformLocation(r.prog, "uSoft");
    r.uSpec    = glGetUniformLocation(r.prog, "uSpec");
    r.uDecal   = glGetUniformLocation(r.prog, "uDecal");
    r.uVista      = glGetUniformLocation(r.prog, "uVista");
    r.uAlphaTest  = glGetUniformLocation(r.prog, "uAlphaTest");
    r.uTextureAlpha = glGetUniformLocation(r.prog, "uTextureAlpha");
    r.uEmissiveTex = glGetUniformLocation(r.prog, "uEmissiveTex");
    r.uFogColor   = glGetUniformLocation(r.prog, "uFogColor");
    r.uFogDensity = glGetUniformLocation(r.prog, "uFogDensity");
    r.uCamPos = glGetUniformLocation(r.prog, "uCamPos");
    r.uRoadReflection=glGetUniformLocation(r.prog,"uRoadReflection");
    r.uReflectionParams=glGetUniformLocation(r.prog,"uReflectionParams");
    glUniform1i(glGetUniformLocation(r.prog,"uReflectionColor"),3);
    glUniform1i(glGetUniformLocation(r.prog,"uReflectionDepth"),4);
    r.uWetLightPos = glGetUniformLocation(r.prog,"uWetLightPos[0]");
    r.uWetLightColor = glGetUniformLocation(r.prog,"uWetLightColor[0]");
    r.uPaintLights = glGetUniformLocation(r.prog,"uPaintLights");
    r.uWetness = glGetUniformLocation(r.prog,"uWetness");
    r.uWeatherTime = glGetUniformLocation(r.prog,"uWeatherTime");
    r.uRainIntensity = glGetUniformLocation(r.prog,"uRainIntensity");
    r.uEnv    = glGetUniformLocation(r.prog, "uEnv");
    r.uEnvCube = glGetUniformLocation(r.prog,"uEnvCube");
    r.uEnvReady = glGetUniformLocation(r.prog,"uEnvReady");
    r.uEnvOrigin = glGetUniformLocation(r.prog,"uEnvOrigin");
    glUniform1i(r.uEnvCube,5);glUniform1f(r.uEnvReady,0);
    r.uUVCheck = glGetUniformLocation(r.prog, "uUVCheck");
    r.uAmbient = glGetUniformLocation(r.prog, "uAmbient");
    r.uDiffuse = glGetUniformLocation(r.prog, "uDiffuse");
    r.uLight   = glGetUniformLocation(r.prog, "uLight");
    r.uVColor  = glGetUniformLocation(r.prog, "uVColor");
    r.uGloss   = glGetUniformLocation(r.prog, "uGloss");
    r.uFlipN   = glGetUniformLocation(r.prog, "uFlipN");
    r.uRimTint = glGetUniformLocation(r.prog, "uRimTint");
    r.uEmissive = glGetUniformLocation(r.prog, "uEmissive");
    r.uFresnel = glGetUniformLocation(r.prog, "uFresnel");
    r.uClearcoat = glGetUniformLocation(r.prog, "uClearcoat");
    r.uHeadPos = glGetUniformLocation(r.prog, "uHeadPos[0]");
    r.uHeadForward = glGetUniformLocation(r.prog, "uHeadForward");
    r.uHeadRight = glGetUniformLocation(r.prog, "uHeadRight");
    r.uHeadUp = glGetUniformLocation(r.prog, "uHeadUp");
    r.uHeadShape = glGetUniformLocation(r.prog, "uHeadShape");
    r.uHeadGain = glGetUniformLocation(r.prog, "uHeadGain");
    r.uHeadLow = glGetUniformLocation(r.prog, "uHeadLow");
    r.uHeadShadow = glGetUniformLocation(r.prog, "uHeadShadow");
    r.uHeadShadowMap[0] = glGetUniformLocation(r.prog, "uHeadShadowMap0");
    r.uHeadShadowMap[1] = glGetUniformLocation(r.prog, "uHeadShadowMap1");
    r.uHeadShadowMVP = glGetUniformLocation(r.prog, "uHeadShadowMVP[0]");
    glUniform1i(r.uHeadShadowMap[0],0);glUniform1i(r.uHeadShadowMap[1],0);
    glUniform1f(r.uHeadShadow,0.0f);
    glUniform1f(r.uHeadGain,0.0f);
    glUniform1f(r.uAlpha, 1.0f); glUniform1f(r.uSoft, 0.0f); glUniform1f(r.uSpec, 0.0f);
    glUniform1f(r.uDecal, 0.0f); glUniform1f(r.uRimTint, 0.0f);
    glUniform3f(r.uEmissive, 0.0f, 0.0f, 0.0f);
    glUniform3f(r.uFogColor, 0.06f, 0.07f, 0.11f); glUniform1f(r.uFogDensity, 0.0f);
    glUniform3f(r.uCamPos, 0, 0, 0); glUniform1f(r.uEnv, 0.0f);
    glUniform1f(r.uUVCheck, 0.0f);
    glUniform1f(r.uGloss, 20.0f);
    glUniform1f(r.uFlipN, 0.0f);
    glUniform1f(r.uAlphaTest, 0.0f);
    glUniform1f(r.uTextureAlpha, 0.0f);
    glUniform1f(r.uEmissiveTex, 0.0f);
    glUniform3f(r.uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);
    return r;
}

/* Authored car normals preserve smoothing across material/UV seams. Generated
 * geometry and invalid source normals use area-weighted face accumulation;
 * nor must hold nverts*3 floats, zeroed by the caller. */
static void mesh_normals(const N2Mesh *m, float *nor) {
    for (int t = 0; t + 2 < m->nidx; t += 3) {
        int a=m->idx[t], b=m->idx[t+1], c=m->idx[t+2];
        float *pa=m->verts+a*5,*pb=m->verts+b*5,*pc=m->verts+c*5;
        float ux=pb[0]-pa[0],uy=pb[1]-pa[1],uz=pb[2]-pa[2];
        float vx=pc[0]-pa[0],vy=pc[1]-pa[1],vz=pc[2]-pa[2];
        float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
        int ii[3]={a,b,c};
        for(int e=0;e<3;e++){ nor[ii[e]*3]+=nx; nor[ii[e]*3+1]+=ny; nor[ii[e]*3+2]+=nz; }
    }
    for (int v=0;v<m->nverts;v++){ float*np=nor+v*3;
        if(m->authored_normals) {
            const float *source=m->verts+m->nverts*5+v*3;
            float len2=source[0]*source[0]+source[1]*source[1]+source[2]*source[2];
            if(isfinite(len2) && len2>1e-12f)memcpy(np,source,3*sizeof(float));
        }
        float l=sqrtf(np[0]*np[0]+np[1]*np[1]+np[2]*np[2]); if(l<1e-6f)l=1;
        np[0]/=l; np[1]/=l; np[2]/=l; }
}

void free_scene_gpu(GpuMesh *gm, int count) {
    if (!gm) return;
    for (int i = 0; i < count; i++) {
        glDeleteBuffers(1, &gm[i].vbo);
        glDeleteBuffers(1, &gm[i].nbo);
        glDeleteBuffers(1, &gm[i].cbo);
        glDeleteBuffers(1, &gm[i].ibo);
    }
    free(gm);
}

/* Upload meshes with per-vertex normals; allocation failure releases partial buffers. */
GpuMesh *upload_scene(N2Scene *s) {
    GpuMesh *gm = (GpuMesh *)calloc(s->count, sizeof(GpuMesh));
    if (!gm) return NULL;
    for (int i = 0; i < s->count; i++) {
        N2Mesh *m = &s->meshes[i];
        if (m->cat == N2_COLLISION) continue;
        N2Mesh rounded={0};
        if (n2_round_wheel_tyre(m,&rounded)) m=&rounded;
        float *nor = (float *)calloc(m->nverts * 3, sizeof(float));
        if (!nor) {
            free(rounded.verts); free(rounded.idx);
            free_scene_gpu(gm, s->count);
            return NULL;
        }
        mesh_normals(m, nor);
        glGenBuffers(1,&gm[i].vbo); glBindBuffer(GL_ARRAY_BUFFER,gm[i].vbo);
        glBufferData(GL_ARRAY_BUFFER, m->nverts*5*sizeof(float), m->verts, GL_STATIC_DRAW);
        glGenBuffers(1,&gm[i].nbo); glBindBuffer(GL_ARRAY_BUFFER,gm[i].nbo);
        glBufferData(GL_ARRAY_BUFFER, m->nverts*3*sizeof(float), nor, GL_STATIC_DRAW);
        if (m->vcol) {
            glGenBuffers(1,&gm[i].cbo); glBindBuffer(GL_ARRAY_BUFFER,gm[i].cbo);
            glBufferData(GL_ARRAY_BUFFER, m->nverts*4, m->vcol, GL_STATIC_DRAW);
        }
        glGenBuffers(1,&gm[i].ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,gm[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m->nidx*sizeof(uint16_t), m->idx, GL_STATIC_DRAW);
        gm[i].nidx = m->nidx; gm[i].cat = m->cat; gm[i].texkey = m->texkey;
        gm[i].trim = m->trim; gm[i].draw_mode = m->draw_mode;
        gm[i].car_material = m->car_material;
        free(nor);
        free(rounded.verts); free(rounded.idx);
    }
    return gm;
}

/* ---- static-world batching ---- */

typedef struct { uint64_t key; int idx; unsigned char group; } BSortEnt;
static int bsort_cmp(const void *a, const void *b) {
    const BSortEnt *aa = (const BSortEnt *)a, *bb = (const BSortEnt *)b;
    if (aa->key != bb->key) return aa->key < bb->key ? -1 : 1;
    return aa->group < bb->group ? -1 : aa->group > bb->group ? 1 : 0;
}
static int btex_cmp(const void *a, const void *b) {   /* final texture order */
    GLuint ta = ((const N2Batch *)a)->tex, tb = ((const N2Batch *)b)->tex;
    return ta < tb ? -1 : ta > tb ? 1 : 0;
}

#define BATCH_CELL     256.0f   /* grid cell edge, metres */
#define BATCH_MAXVERTS 65535    /* u16 indices (GLES2: no u32 without ext) */

/* Milestone 75: the VBO layout draw_batch() hardcodes must equal the struct's
 * real layout. Compile-time (C99: negative array size, not C11 _Static_assert),
 * so a field reorder becomes a build error instead of scrambled attributes. */
typedef char n2_batchvertex_layout_check[
    (offsetof(BatchedVertex, pos)    ==  0 &&
     offsetof(BatchedVertex, uv)     == 12 &&
     offsetof(BatchedVertex, normal) == 20 &&
     offsetof(BatchedVertex, col)    == 32 &&
     sizeof(BatchedVertex)           == 36) ? 1 : -1];

/* Milestone 79: dump one batch's member list, in the exact order
 * upload_world_batches partitioned it -- this runs inside batch_emit, so the
 * (ent, i0, i1) run IS the production run; nothing is re-sorted or re-derived. */
static void batch_members_report(const N2Scene *s, const BSortEnt *ent, int i0, int i1,
                                 int bidx, GLuint tex, const N2Batch *bref) {
    (void)bref;
    printf("MILESTONE: 79\n");
    printf("batch index   %d\n", bidx);
    printf("texkey        %08x   gl_tex %u   nmesh %d\n",
           s->meshes[ent[i0].idx].texkey, (unsigned)tex, i1 - i0);
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    for (int k = i0; k < i1; k++) {
        const N2Mesh *m = &s->meshes[ent[k].idx];
        for (int v = 0; v < m->nverts; v++)
            for (int c = 0; c < 3; c++) {
                float p = m->verts[v*5+c];
                if (p < mn[c]) mn[c] = p; if (p > mx[c]) mx[c] = p;
            }
    }
    printf("bounds        x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]\n",
           mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    printf("%5s  %-30s %-9s %-44s %8s\n",
           "mesh", "asset name", "class", "bounds x/y/z", "tris");
    for (int k = i0; k < i1; k++) {
        const N2Mesh *m = &s->meshes[ent[k].idx];
        float a[3] = {1e30f,1e30f,1e30f}, b[3] = {-1e30f,-1e30f,-1e30f};
        for (int v = 0; v < m->nverts; v++)
            for (int c = 0; c < 3; c++) {
                float p = m->verts[v*5+c];
                if (p < a[c]) a[c] = p; if (p > b[c]) b[c] = p;
            }
        char bb[64];
        snprintf(bb, sizeof bb, "[%.0f %.0f][%.0f %.0f][%.1f %.1f]",
                 a[0], b[0], a[1], b[1], a[2], b[2]);
        printf("%5d  %-30s %-9s %-44s %8d\n",
               ent[k].idx, m->sname[0] ? m->sname : "(unnamed)",
               n2_scen_name(m->scen), bb, m->nidx / 3);
    }
}

/* Verify one source mesh survived the merge into `bv`/`bi` byte-exactly. */
static void batch_audit_report(const N2Scene *s, const BSortEnt *ent, int i0, int i1,
                               const BatchedVertex *bv, const uint16_t *bi,
                               int bidx, const char *want) {
    int vo = 0, io = 0;
    for (int k = i0; k < i1; k++) {
        const N2Mesh *m = &s->meshes[ent[k].idx];
        if (strcmp(m->sname, want)) { vo += m->nverts; io += m->nidx; continue; }

        int bad_pos = 0, bad_idx = 0, first_bad = -1;
        for (int v = 0; v < m->nverts; v++) {
            const float *p = m->verts + v*5;
            const BatchedVertex *o = &bv[vo + v];
            if (memcmp(o->pos, p, 3*sizeof(float))) {
                bad_pos++; if (first_bad < 0) first_bad = v;
            }
        }
        for (int t = 0; t < m->nidx; t++)
            if (bi[io + t] != (uint16_t)(m->idx[t] + vo)) bad_idx++;

        printf("MILESTONE: 75\n");
        printf("mesh name                        %s\n", m->sname);
        printf("source mesh index                %d\n", ent[k].idx);
        printf("source vertex count / index count %d / %d\n", m->nverts, m->nidx);
        printf("source first 3 position vertices ");
        for (int v = 0; v < 3 && v < m->nverts; v++)
            printf("(%.4f %.4f %.4f) ", m->verts[v*5], m->verts[v*5+1], m->verts[v*5+2]);
        printf("\nsource first 3 indices           ");
        for (int t = 0; t < 3 && t < m->nidx; t++) printf("%u ", m->idx[t]);
        printf("\nselected batch index             %d\n", bidx);
        printf("batch source-mesh count          %d\n", i1 - i0);
        printf("batched vertex offset            %d\n", vo);
        printf("batched first 3 corresponding position vertices ");
        for (int v = 0; v < 3 && v < m->nverts; v++)
            printf("(%.4f %.4f %.4f) ", bv[vo+v].pos[0], bv[vo+v].pos[1], bv[vo+v].pos[2]);
        printf("\nbatched remapped first 3 indices ");
        for (int t = 0; t < 3 && t < m->nidx; t++) printf("%u ", bi[io + t]);
        printf("\nvertex layout   pos@%d uv@%d normal@%d col@%d stride %d\n",
               (int)offsetof(BatchedVertex, pos), (int)offsetof(BatchedVertex, uv),
               (int)offsetof(BatchedVertex, normal), (int)offsetof(BatchedVertex, col),
               (int)sizeof(BatchedVertex));
        /* M98: UVs travel in the same vertex, so verify them the same way and
           report the ranges the sampler will actually see. Diagnostic only. */
        { int bad_uv = 0;
          float su0=1e30f, sv0=1e30f, su1=-1e30f, sv1=-1e30f;
          float bu0=1e30f, bv0=1e30f, bu1=-1e30f, bv1=-1e30f;
          float x0=1e30f, y0=1e30f, z0=1e30f, x1=-1e30f, y1=-1e30f, z1=-1e30f;
          for (int v = 0; v < m->nverts; v++) {
              const float *p = m->verts + v*5;
              const BatchedVertex *o = &bv[vo + v];
              if (memcmp(o->uv, p + 3, 2*sizeof(float))) bad_uv++;
              if (p[3]<su0)su0=p[3]; if (p[3]>su1)su1=p[3];
              if (p[4]<sv0)sv0=p[4]; if (p[4]>sv1)sv1=p[4];
              if (o->uv[0]<bu0)bu0=o->uv[0]; if (o->uv[0]>bu1)bu1=o->uv[0];
              if (o->uv[1]<bv0)bv0=o->uv[1]; if (o->uv[1]>bv1)bv1=o->uv[1];
              if (p[0]<x0)x0=p[0]; if (p[0]>x1)x1=p[0];
              if (p[1]<y0)y0=p[1]; if (p[1]>y1)y1=p[1];
              if (p[2]<z0)z0=p[2]; if (p[2]>z1)z1=p[2];
          }
          printf("texkey                           %08x\n", m->texkey);
          printf("source UV  min/max               u[%.4f %.4f] v[%.4f %.4f]  span %.3f x %.3f\n",
                 su0, su1, sv0, sv1, su1-su0, sv1-sv0);
          printf("batched UV min/max               u[%.4f %.4f] v[%.4f %.4f]\n",
                 bu0, bu1, bv0, bv1);
          printf("world span                       X %.3f Y %.3f Z %.3f  (m)\n",
                 x1-x0, y1-y0, z1-z0);
          printf("texels per metre (u,v vs X,Y)    %.4f / %.4f  [UV span / world span]\n",
                 (x1-x0) > 1e-6f ? (su1-su0)/(x1-x0) : 0.0f,
                 (y1-y0) > 1e-6f ? (sv1-sv0)/(y1-y0) : 0.0f);
          printf("UVs mismatched                   %d/%d\n", bad_uv, m->nverts);
        }
        printf("positions mismatched %d/%d   indices mismatched %d/%d",
               bad_pos, m->nverts, bad_idx, m->nidx);
        if (first_bad >= 0) printf("   (first bad vertex %d)", first_bad);
        printf("\nmax remapped index %d vs batch vertex ceiling %d\n",
               (int)(m->idx[0] + vo), BATCH_MAXVERTS);
        printf("RESULT: %s\n", (bad_pos || bad_idx) ? "MISMATCH" : "MATCH");
        return;
    }
}

static int batch_mesh_vertices(const N2Mesh *m,BatchedVertex *out) {
    float *nor=calloc((size_t)m->nverts*3,sizeof *nor);if(!nor)return 0;
    mesh_normals(m, nor);          /* per source mesh: no cross-mesh smoothing */
    for (int v = 0; v < m->nverts; v++) {
        BatchedVertex *o = &out[v]; const float *p = m->verts + v*5;
        o->pos[0]=p[0]; o->pos[1]=p[1]; o->pos[2]=p[2];
        o->uv[0]=p[3];  o->uv[1]=p[4];
        o->normal[0]=nor[v*3]; o->normal[1]=nor[v*3+1]; o->normal[2]=nor[v*3+2];
        if (m->vcol) { o->col[0]=m->vcol[v*4]; o->col[1]=m->vcol[v*4+1];
                       o->col[2]=m->vcol[v*4+2]; o->col[3]=m->vcol[v*4+3]; }
        else { o->col[0]=o->col[1]=o->col[2]=o->col[3]=255; }  /* neutral */
    }
    free(nor);return 1;
}

/* merge meshes [i0,i1) of the sort array into one uploaded batch */
static int batch_emit(const N2Scene *s, const BSortEnt *ent, int i0, int i1,
                       GLuint tex, N2Batch *b, int bidx, const char *audit,
                       const unsigned char *mtexmode) {
    int nv = 0, ni = 0;
    for (int k = i0; k < i1; k++) {
        nv += s->meshes[ent[k].idx].nverts; ni += s->meshes[ent[k].idx].nidx;
    }
    BatchedVertex *bv = (BatchedVertex *)malloc((size_t)nv * sizeof *bv);
    uint16_t *bi = (uint16_t *)malloc((size_t)ni * sizeof *bi);
    if (!bv || !bi) { free(bv); free(bi); return 0; }
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    int vo = 0, io = 0;
    for (int k = i0; k < i1; k++) {
        const N2Mesh *m = &s->meshes[ent[k].idx];
        if(!batch_mesh_vertices(m,bv+vo)){free(bv);free(bi);return 0;}
        for(int v=0;v<m->nverts;v++)for(int c=0;c<3;c++) {
            float p=m->verts[v*5+c];
            mn[c]=fminf(mn[c],p);mx[c]=fmaxf(mx[c],p);
        }
        for (int t = 0; t < m->nidx; t++) bi[io + t] = (uint16_t)(m->idx[t] + vo);
        vo += m->nverts; io += m->nidx;
    }
    if (audit && audit[0] != '#') batch_audit_report(s, ent, i0, i1, bv, bi, bidx, audit);
    memset(b, 0, sizeof *b);
    glGenBuffers(1, &b->vbo); glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, (long)nv * (long)sizeof *bv, bv, GL_STATIC_DRAW);
    glGenBuffers(1, &b->ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)ni * 2, bi, GL_STATIC_DRAW);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(1, &b->vbo); glDeleteBuffers(1, &b->ibo);
        memset(b, 0, sizeof *b); free(bv); free(bi); return 0;
    }
    for(int k=i0;k<i1;k++)b->nprops+=s->meshes[ent[k].idx].prop_id!=0;
    if(b->nprops) {
        b->props=calloc((size_t)b->nprops,sizeof *b->props);
        if(!b->props) {
            glDeleteBuffers(1,&b->vbo);glDeleteBuffers(1,&b->ibo);
            memset(b,0,sizeof *b);free(bv);free(bi);return 0;
        }
        int vertex=0,prop=0;
        for(int k=i0;k<i1;k++) {
            const N2Mesh *m=s->meshes+ent[k].idx;
            if(m->prop_id)b->props[prop++]=(PropBatchRange){ent[k].idx,vertex,m->prop_revision};
            vertex+=m->nverts;
        }
    }
    b->index_count = ni; b->tex = tex; b->nmesh = i1 - i0; b->emit_idx = bidx;
    b->wettable = s->meshes[ent[i0].idx].cat == N2_ROAD;
    b->texkey = s->meshes[ent[i0].idx].texkey;
    b->drawmode = mtexmode ? mtexmode[ent[i0].idx] : N2_DRAW_OPAQUE;
    int named = 0;
    for (int k = i0; k < i1; k++) {
        int sc = s->meshes[ent[k].idx].scen;
        if (sc >= 0 && sc < 8) b->scen_count[sc]++;
        if (s->meshes[ent[k].idx].texkey) named++;
    }
    /* Only a batch with no GL texture can be "missing art", and only when every
       member asked for one. Measured over five shipped bundles: 68 batches are
       wholly unresolved and NOT ONE mixes the two kinds, so this changes no
       shipped frame -- it removes the dependence on which member happened to
       sort first, which is what `texkey` reported. */
    b->unresolved = (unsigned char)(!tex && named == i1 - i0);
    for (int c = 0; c < 3; c++) { b->bbox_min[c] = mn[c]; b->bbox_max[c] = mx[c]; }
    free(bv); free(bi);
    return 1;
}

struct WorldBatchUpload {
    const N2Scene *scene;
    const unsigned char *modes;
    const char *audit;
    BSortEnt *ent;
    N2Batch *batches;
    int *run0, *run1;
    int meshes, next, count;
};

void upload_world_batches_cancel(WorldBatchUpload **slot) {
    if (!slot || !*slot) return;
    WorldBatchUpload *j = *slot;
    render_batch_array_free(&j->batches, &j->count);
    free(j->ent); free(j->run0); free(j->run1); free(j);
    *slot = NULL;
}

WorldBatchUpload *upload_world_batches_begin(const N2Scene *s,
                          const float (*mbb)[4], const GLuint *mtex,
                          GLuint texTerr, const char *audit,
                          const unsigned char *mtexmode) {
    if (!s || s->count < 0 || (s->count && (!s->meshes || !mbb || !mtex)))
        return NULL;
    WorldBatchUpload *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    int n = s->count;
    size_t cap = (size_t)(n ? n : 1);
    /* ponytail: at most one batch per source mesh; temporary upper-bound
     * arrays avoid reallocating partial GL owners. Compact if memory dominates. */
    j->ent = malloc(cap * sizeof *j->ent);
    j->batches = malloc(cap * sizeof *j->batches);
    j->run0 = malloc(cap * sizeof *j->run0);
    j->run1 = malloc(cap * sizeof *j->run1);
    if (!j->ent || !j->batches || !j->run0 || !j->run1) {
        upload_world_batches_cancel(&j); return NULL;
    }
    j->scene = s; j->modes = mtexmode; j->audit = audit;
    BSortEnt *ent = j->ent;
    /* world extent -> grid coords */
    float x0 = 1e30f, y0 = 1e30f;
    for (int i = 0; i < n; i++) {
        if (mbb[i][0] < x0) x0 = mbb[i][0];
        if (mbb[i][1] < y0) y0 = mbb[i][1];
    }
    /* sort meshes by (cell, resolved texture) — sky/glow meshes are pulled by
       upload_cat_batches instead: batching them in here would let an ordinary
       cell+texture run silently absorb a skybox shell or a neon sign, so
       they'd draw with the wrong depth/blend state at the wrong time. */
    int m = 0;
    for (int i = 0; i < n; i++) {
        const N2Mesh *mesh = &s->meshes[i];
        if (mesh->cat == N2_SKY || mesh->cat == N2_GLOW || mesh->cat == N2_COLLISION) continue;
        GLuint tex = mtex[i];
        if (!tex && mesh->cat == N2_TERRAIN) tex = texTerr;   /* fallback baked in */
        float cx = (mbb[i][0]+mbb[i][2])*0.5f, cy = (mbb[i][1]+mbb[i][3])*0.5f;
        uint64_t cell = (uint64_t)(uint32_t)((int)((cy-y0)/BATCH_CELL)*4096
                                           + (int)((cx-x0)/BATCH_CELL));
        ent[m].key = cell << 32 | tex; ent[m].idx = i;
        int mode = mtexmode ? mtexmode[i] : N2_DRAW_OPAQUE;
        /* Keep wettable roads separate even when walls share their texture. */
        ent[m].group = (unsigned char)n2_world_batch_material_group(
            &s->meshes[i], mode) | (mesh->cat == N2_ROAD ? 128 : 0);
        m++;
    }
    qsort(ent, (size_t)m, sizeof *ent, bsort_cmp);
    j->meshes = m;
    return j;
}

int upload_world_batches_step(WorldBatchUpload **slot, int max_batches,
                             N2Batch **out, int *count, int *meshbatch) {
    if (!slot || !*slot || !out || !count) return -1;
    if (max_batches <= 0) return 0;
    WorldBatchUpload *j = *slot;
    const N2Scene *s = j->scene;
    BSortEnt *ent = j->ent;
    N2Batch *bat = j->batches;
    int *run0 = j->run0, *run1 = j->run1;
    const char *audit = j->audit;
    while (j->next < j->meshes && max_batches-- > 0) {
        int i0 = j->next, i = i0 + 1;
        int verts = s->meshes[ent[i0].idx].nverts;
        while (i < j->meshes && ent[i].key == ent[i0].key &&
               ent[i].group == ent[i0].group &&
               verts + s->meshes[ent[i].idx].nverts <= BATCH_MAXVERTS)
            verts += s->meshes[ent[i++].idx].nverts;
        if (!batch_emit(s, ent, i0, i, (GLuint)ent[i0].key,
                        &bat[j->count], j->count, audit, j->modes)) {
            upload_world_batches_cancel(slot); return -1;
        }
        run0[j->count] = i0; run1[j->count] = i;
        j->count++; j->next = i;
    }
    if (j->next < j->meshes) return 0;
    int nb = j->count;
    qsort(bat, (size_t)nb, sizeof *bat, btex_cmp);   /* minimise texture binds */
    /* mesh -> final batch, replayed from each batch's own emission run so it is
       the production partition rather than a second derivation (M133) */
    if (meshbatch) for (int k = 0; k < s->count; k++) meshbatch[k] = -1;
    if (meshbatch)
        for (int b = 0; b < nb; b++) {
            int e = bat[b].emit_idx;
            for (int k = run0[e]; k < run1[e]; k++) meshbatch[ent[k].idx] = b;
        }
    /* "#N" audits the batch the RENDERER calls N, i.e. wbatch[N] after this
       sort. Membership is replayed from that batch's own recorded emission run,
       so it is the production partition, not a second derivation. */
    if (audit && audit[0] == '#') {
        int want = atoi(audit + 1);
        if (want >= 0 && want < nb) {
            int e = bat[want].emit_idx;
            batch_members_report(s, ent, run0[e], run1[e], want, bat[want].tex, &bat[want]);
        } else fprintf(stderr, "batch audit: #%d out of range (0..%d)\n", want, nb-1);
    }
    *out = bat; *count = nb;
    j->batches = NULL; j->count = 0;
    upload_world_batches_cancel(slot);
    return 1;
}

int upload_world_batches(const N2Scene *s, const float (*mbb)[4],
                         const GLuint *mtex, GLuint texTerr, N2Batch **out,
                         const char *audit, int *meshbatch,
                         const unsigned char *mtexmode) {
    WorldBatchUpload *j = upload_world_batches_begin(s, mbb, mtex, texTerr,
                                                    audit, mtexmode);
    int count = 0;
    if (!j || upload_world_batches_step(&j, INT_MAX, out, &count, meshbatch) < 0) {
        upload_world_batches_cancel(&j); return -1;
    }
    return count;
}

/* Same merge as above but for exactly one category, grouped by texture only
 * (no spatial grid — a city has a handful of skybox/neon meshes, not tens of
 * thousands, so there's nothing for a cell split to buy here). */
int upload_cat_batches(const N2Scene *s, int cat, const GLuint *mtex, N2Batch **out,
                       const unsigned char *mtexmode) {
    int n = s->count;
    BSortEnt *ent = (BSortEnt *)malloc((size_t)(n ? n : 1) * sizeof *ent);
    if (!ent) return -1;
    int m = 0;
    for (int i = 0; i < n; i++)
        if (s->meshes[i].cat == cat) {
            ent[m].key = mtex[i]; ent[m].idx = i;
            int mode = mtexmode ? mtexmode[i] : N2_DRAW_OPAQUE;
            ent[m].group = (unsigned char)n2_world_batch_material_group(
                &s->meshes[i], mode);
            m++;
        }
    qsort(ent, (size_t)m, sizeof *ent, bsort_cmp);
    int cap = 8, nb = 0;
    N2Batch *bat = (N2Batch *)malloc((size_t)cap * sizeof *bat);
    if (!bat) { free(ent); return -1; }
    int i0 = 0, verts = 0;
    for (int i = 0; i <= m; i++) {
        int flush = (i == m) || (i > i0 && (ent[i].key != ent[i0].key ||
                                            ent[i].group != ent[i0].group)) ||
                    (i > i0 && verts + s->meshes[ent[i].idx].nverts > BATCH_MAXVERTS);
        if (flush && i > i0) {
            if (nb == cap) {
                cap *= 2;
                N2Batch *grown = realloc(bat, (size_t)cap * sizeof *bat);
                if (!grown) goto fail;
                bat = grown;
            }
            if (!batch_emit(s, ent, i0, i, (GLuint)ent[i0].key,
                            &bat[nb], nb, NULL, mtexmode)) goto fail;
            nb++;
            i0 = i; verts = 0;
        }
        if (i < m) verts += s->meshes[ent[i].idx].nverts;
    }
    free(ent);
    *out = bat;
    return nb;
fail:
    free(ent); render_batch_array_free(&bat, &nb); return -1;
}

void render_batch_array_free(N2Batch **batches, int *count) {
    if (!batches) return;
    if (*batches) {
        int n = count && *count > 0 ? *count : 0;
        for (int i = 0; i < n; i++) {
            if ((*batches)[i].vbo) glDeleteBuffers(1, &(*batches)[i].vbo);
            if ((*batches)[i].ibo) glDeleteBuffers(1, &(*batches)[i].ibo);
            free((*batches)[i].props);
        }
        free(*batches);
    }
    *batches = NULL;
    if (count) *count = 0;
}

int render_world_prop_updates(const N2Scene *scene,N2Batch *batches,int count,
                             int (*update_mesh)(N2Mesh *)) {
    int changed=0;
    for(int k=0;k<count;k++)for(int j=0;j<batches[k].nprops;j++) {
        N2Batch *b=batches+k;PropBatchRange *range=b->props+j;
        if(range->mesh<0 || range->mesh>=scene->count)return -1;
        N2Mesh *m=scene->meshes+range->mesh;
        if(update_mesh)update_mesh(m);
        if(range->revision==m->prop_revision)continue;
        BatchedVertex *v=malloc((size_t)m->nverts*sizeof *v);
        if(!v)return -1;
        if(!batch_mesh_vertices(m,v)){free(v);return -1;}
        glBindBuffer(GL_ARRAY_BUFFER,b->vbo);
        glBufferSubData(GL_ARRAY_BUFFER,(long)range->vertex*sizeof *v,(long)m->nverts*sizeof *v,v);
        if(glGetError()!=GL_NO_ERROR){free(v);return -1;}
        for(int q=0;q<m->nverts;q++)for(int a=0;a<3;a++) {
            b->bbox_min[a]=fminf(b->bbox_min[a],v[q].pos[a]);
            b->bbox_max[a]=fmaxf(b->bbox_max[a],v[q].pos[a]);
        }
        free(v);range->revision=m->prop_revision;changed++;
    }
    return changed;
}

void draw_batch(const N2Batch *b) {
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchedVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BatchedVertex), (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(BatchedVertex), (void*)20);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(BatchedVertex), (void*)32);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    glDrawElements(GL_TRIANGLES, b->index_count, GL_UNSIGNED_SHORT, 0);
}

/* Build a clean procedural wheel (short cylinder, axle along Y, disc in X-Z) —
 * the game's rim meshes are sparse spoke shells that read as spiky urchins when
 * drawn solid, so we substitute a simple tyre. Modelled at the origin like the
 * game wheel, so the same wheelT placement transforms apply. */
GpuMesh make_wheel(float R, float halfW) {
    const int N = 24;
    int nv = N*2 + 2, nt = N*4;         /* 2 rings + 2 hub centres; sides + 2 caps */
    N2Mesh m; memset(&m, 0, sizeof m);
    m.nverts = nv; m.verts = (float *)malloc(nv*5*sizeof(float));
    for (int i = 0; i < N; i++) {
        float a = 2.0f*(float)M_PI*i/N, cx = R*cosf(a), cz = R*sinf(a);
        float u = cosf(a)*0.5f+0.5f, v = sinf(a)*0.5f+0.5f;
        float *p0 = m.verts + i*5;     p0[0]=cx; p0[1]= halfW; p0[2]=cz; p0[3]=u; p0[4]=v;
        float *p1 = m.verts + (N+i)*5; p1[0]=cx; p1[1]=-halfW; p1[2]=cz; p1[3]=u; p1[4]=v;
    }
    int c0 = 2*N, c1 = 2*N+1;
    float *h0=m.verts+c0*5; h0[0]=0;h0[1]= halfW;h0[2]=0;h0[3]=0.5f;h0[4]=0.5f;
    float *h1=m.verts+c1*5; h1[0]=0;h1[1]=-halfW;h1[2]=0;h1[3]=0.5f;h1[4]=0.5f;
    m.idx = (uint16_t *)malloc(nt*3*sizeof(uint16_t)); int k=0;
    for (int i = 0; i < N; i++) {
        int j = (i+1)%N;
        m.idx[k++]=i;    m.idx[k++]=j;    m.idx[k++]=N+j;      /* tread quad */
        m.idx[k++]=i;    m.idx[k++]=N+j;  m.idx[k++]=N+i;
        m.idx[k++]=c0;   m.idx[k++]=j;    m.idx[k++]=i;         /* +Y face */
        m.idx[k++]=c1;   m.idx[k++]=N+i;  m.idx[k++]=N+j;       /* -Y face */
    }
    m.nidx = k; m.cat = N2_CAR_TIRE;
    N2Scene s; s.meshes = &m; s.count = 1; s.cap = 1;
    GpuMesh *g = upload_scene(&s); GpuMesh out = *g;
    free(g); free(m.verts); free(m.idx);
    return out;
}

/* Procedural alloy-rim texture for the wheel caps: the cap UVs are radial
 * (centre 0.5,0.5, ring at uv-radius 0.5), so paint by relative radius —
 * bright hub disc, spoked metal mid, dark rubber edge. The tread quads
 * sample the outer ring = rubber. */
GLuint make_wheel_tex(void) {
    enum { S = 64 };
    static unsigned char px[S*S*3];
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        float dx = (x+0.5f)/S - 0.5f, dy = (y+0.5f)/S - 0.5f;
        float r = sqrtf(dx*dx + dy*dy) * 2.0f;      /* 0 centre .. 1 ring */
        unsigned char v;
        if (r > 0.78f)      v = 14;                                  /* rubber */
        else if (r > 0.30f) {                                        /* spokes */
            float spoke = 0.5f + 0.5f*cosf(5.0f*atan2f(dy, dx));
            v = (unsigned char)(38 + 70.0f*spoke*spoke);
        } else               v = r < 0.10f ? 150 : 105;              /* hub */
        unsigned char *o = px + (y*S + x)*3;
        o[0] = v; o[1] = v; o[2] = (unsigned char)(v + v/16);        /* cool metal */
    }
    N2Tex t = { S, S, px, NULL, NULL, 0, 0, 0, 0,0,0,0 };
    GLuint id = upload_tex(&t);
    /* radial, single-sample cap texture (no tiling intended) — clamp so a
       filter footprint near u/v=0 or 1 can't wrap and bleed in colour from
       the opposite edge, same reasoning as the car atlas/vinyl clamps. */
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}

/* unit-quad buffers for the 2D HUD / billboards (drawn in NDC via uMVP) */
GpuMesh make_quad(void) {
    GpuMesh quad; memset(&quad, 0, sizeof quad);
    float qv[] = {0,0,0,0,0, 1,0,0,1,0, 1,1,0,1,1, 0,1,0,0,1};
    float qn[] = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
    uint16_t qi[] = {0,1,2, 0,2,3};
    glGenBuffers(1,&quad.vbo); glBindBuffer(GL_ARRAY_BUFFER,quad.vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof qv,qv,GL_STATIC_DRAW);
    glGenBuffers(1,&quad.nbo); glBindBuffer(GL_ARRAY_BUFFER,quad.nbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof qn,qn,GL_STATIC_DRAW);
    glGenBuffers(1,&quad.ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,quad.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof qi,qi,GL_STATIC_DRAW);
    quad.nidx = 6;
    return quad;
}

void draw_gpumesh(GpuMesh *g) {
    if (g->cbo) {
        glBindBuffer(GL_ARRAY_BUFFER,g->cbo);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3,4,GL_UNSIGNED_BYTE,GL_TRUE,0,(void*)0);
    } else glDisableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, g->nbo);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
    glDrawElements(GL_TRIANGLES, g->nidx, GL_UNSIGNED_SHORT, 0);
}

void render_wheel_mesh(const RProg *r, GpuMesh *mesh, GLuint texture, int mode) {
    const GLint loc[]={r->uUseTex,r->uAlphaTest,r->uTextureAlpha,r->uAlpha,r->uDecal,
                       r->uRimTint,r->uSpec,r->uEnv,r->uClearcoat,r->uVColor};
    float saved[10];for(int i=0;i<10;i++)glGetUniformfv(r->prog,loc[i],saved+i);
    GLint oldtex,src,dst,srca,dsta;
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldtex);
    glGetIntegerv(GL_BLEND_SRC_RGB,&src);glGetIntegerv(GL_BLEND_DST_RGB,&dst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&srca);glGetIntegerv(GL_BLEND_DST_ALPHA,&dsta);
    GLboolean blend=glIsEnabled(GL_BLEND),depth=glIsEnabled(GL_DEPTH_TEST),mask;
    glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);
    unsigned char amode = mesh->draw_mode ? mesh->draw_mode : (unsigned char)mode;
    int cut=texture && amode==N2_DRAW_CUTOUT, translucent=texture && amode==N2_DRAW_BLEND;
    /* The wheel INTERIOR slice is the inboard barrel/backing. Its atlas
       rectangle also contains transparent texels from the tread strip, so
       alpha discard breaks the generated round backing into black facets.
       Keep its authored RGB (usually black) but make the backing continuous;
       TIRE and RIM retain their authored cutout textures. */
    /* Prelit traffic wheels use a cutout quad for the visible wheel face. */
    if (mesh->car_material == N2_MAT_INTERIOR && !mesh->cbo) cut = 0;
    glUniform1f(r->uVColor,mesh->cbo?2.0f:0.0f);
    glBindTexture(GL_TEXTURE_2D,texture);
    glUniform1f(r->uUseTex,texture?1.0f:0.0f);
    glUniform1f(r->uAlphaTest,cut?1.0f:0.0f);
    glUniform1f(r->uTextureAlpha,translucent?1.0f:0.0f);
    glUniform1f(r->uAlpha,1.0f);glUniform1f(r->uDecal,0.0f);
    uint32_t material=mesh->car_material;
    if (!texture || (material!=N2_MAT_MAGSILVER && material!=N2_MAT_MAGCHROME))
        glUniform1f(r->uRimTint,0.0f);
    if (material==N2_MAT_RUBBER || material==N2_MAT_INTERIOR || material==N2_MAT_DULLPLASTIC) {
        glUniform1f(r->uSpec,0.0f);glUniform1f(r->uEnv,0.0f);
        glUniform1f(r->uClearcoat,0.0f);
    }
    glEnable(GL_DEPTH_TEST);glDepthMask(translucent?GL_FALSE:GL_TRUE);
    if(translucent){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);}
    else glDisable(GL_BLEND);
    draw_gpumesh(mesh);
    for(int i=0;i<10;i++)glUniform1f(loc[i],saved[i]);
    glBindTexture(GL_TEXTURE_2D,(GLuint)oldtex);
    glBlendFuncSeparate((GLenum)src,(GLenum)dst,(GLenum)srca,(GLenum)dsta);
    if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    glDepthMask(mask);
}

/* A split material slice shares its vertex pool with other surfaces. Only
 * indexed vertices describe the centre that should determine its draw depth. */
static void mesh_indexed_center(const N2Mesh *m,float center[3]) {
    float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f};
    for(int j=0;j<m->nidx;j++)for(int a=0;a<3;a++) {
        float v=m->verts[m->idx[j]*5+a];
        if(v<lo[a])lo[a]=v;if(v>hi[a])hi[a]=v;
    }
    for(int a=0;a<3;a++)center[a]=(lo[a]+hi[a])*.5f;
}

void render_wheel_order(const N2Scene *scene, const float mvp[4][16], int *order) {
    if(!scene || scene->count<=0 || !order)return;
    int n=scene->count;float depth[4*n];
    for(int i=0;i<n;i++) {
        float center[3];mesh_indexed_center(scene->meshes+i,center);
        for(int k=0;k<4;k++) {
            int at=k*n+i;order[at]=at;depth[at]=mvp[k][15];
            for(int a=0;a<3;a++)depth[at]+=mvp[k][a*4+3]*center[a];
        }
    }
    /* ponytail: source-slice centres give painter order, not per-triangle
       transparency. Intersecting translucent surfaces still need finer sorting. */
    n2_sort_back_to_front(order,4*n,depth);
}

int render_car_glass_order(const N2Scene *scene,const float mvp[16],int *order) {
    if(!scene || scene->count<=0 || !order)return 0;
    float depth[scene->count];int n=0;
    for(int i=0;i<scene->count;i++) {
        const N2Mesh *m=scene->meshes+i;
        if(m->car_mount!=N2_MOUNT_BODY || (m->cat!=N2_CAR_GLASS &&
           !n2_lamp_clear_cover(m->cat,m->car_material)))continue;
        float center[3];mesh_indexed_center(m,center);
        order[n++]=i;depth[i]=mvp[15];
        for(int a=0;a<3;a++)depth[i]+=mvp[a*4+3]*center[a];
    }
    /* ponytail: pane centres, as for wheels; intersecting panes need finer sorting. */
    n2_sort_back_to_front(order,n,depth);return n;
}

/* S3TC format enums + capability flag. glext.h / SDL_opengl.h define these on
   desktop; guard so the -DN2_GLES build (where the flag stays 0 unless the GPU
   advertises the extension) still compiles. */
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#  define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#  define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#  define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
int g_tex_s3tc = 0;
int   g_tex_aniso_max = 1;
float g_tex_aniso     = 1.0f;
#define GL_TEXTURE_MAX_ANISOTROPY_EXT_    0x84FE

/* Every texture this renderer owns, so a detail change can re-apply to all of
 * them. Ids are recorded on upload and validated with glIsTexture before use --
 * residents and the world cache delete textures behind our back, and setting a
 * parameter on a deleted name is a GL error, not a no-op. */
static GLuint *g_tex_all; static int g_tex_all_n, g_tex_all_cap;
static void tex_track(GLuint id) {
    if (!id) return;
    if (g_tex_all_n == g_tex_all_cap) {
        int cap = g_tex_all_cap ? g_tex_all_cap * 2 : 256;
        GLuint *t = (GLuint *)realloc(g_tex_all, (size_t)cap * sizeof *t);
        if (!t) return;                       /* out of memory: just don't track it */
        g_tex_all = t; g_tex_all_cap = cap;
    }
    g_tex_all[g_tex_all_n++] = id;
}
/* Caller has the texture bound; the id is only for symmetry with tex_track. */
static void tex_apply_aniso(void) {
    if (g_tex_aniso_max <= 1) return;
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT_, g_tex_aniso);
}
void render_texture_detail(float aniso) {
    if (g_tex_aniso_max <= 1) { g_tex_aniso = 1.0f; return; }
    if (aniso < 1.0f) aniso = 1.0f;
    if (aniso > (float)g_tex_aniso_max) aniso = (float)g_tex_aniso_max;
    if (aniso == g_tex_aniso) return;
    g_tex_aniso = aniso;
    GLint bound = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    int live = 0;
    for (int i = 0; i < g_tex_all_n; i++) {
        if (!glIsTexture(g_tex_all[i])) continue;      /* freed since upload */
        g_tex_all[live++] = g_tex_all[i];
        glBindTexture(GL_TEXTURE_2D, g_tex_all[i]);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT_, g_tex_aniso);
    }
    g_tex_all_n = live;                                 /* compact away dead ids */
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
}

GLuint upload_tpk_texture_to_gpu(const N2Tex *t) {
    if (g_tex_s3tc && t->dxtfmt && t->dxt && t->dxtlen > 0) {
        GLenum fmt = (t->dxtfmt == 3) ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                   : (t->dxtfmt == 5) ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
                                      : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        int bpb = (t->dxtfmt == 1) ? 8 : 16;   /* S3TC block bytes (DXT3/DXT5 = 16) */
        GLuint id = 0; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
        /* Replay every complete mip level in the blob (level 0 = base .. 1x1).
           Per-level block count matches n2_mipbytes2 exactly so the offsets line
           up. Only whole levels are uploaded; a chain that reaches 1x1 is a
           complete pyramid (mipmap filtering legal), otherwise fall back to a
           base-only LINEAR filter -- which only ever samples level 0, so a short
           chain can never leave the texture incomplete/black. */
        int lw = t->w, lh = t->h, off = 0, level = 0, complete = 0;
        for (;;) {
            int bw = lw < 4 ? 1 : lw/4, bh = lh < 4 ? 1 : lh/4;
            int sz = bw * bh * bpb;
            if (off + sz > t->dxtlen) break;
            glCompressedTexImage2D(GL_TEXTURE_2D, level, fmt, lw, lh, 0, sz, t->dxt + off);
            off += sz; level++;
            if (lw == 1 && lh == 1) { complete = 1; break; }
            if (lw > 1) lw /= 2; if (lh > 1) lh /= 2;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        complete ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (complete) { tex_apply_aniso(); tex_track(id); }   /* needs a mip chain */
        return id;
    }
    return upload_tex(t);   /* portable fallback: CPU-decoded RGBA + mipmaps */
}

GLuint upload_tex(const N2Tex *t) {
    GLuint id = 0; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    if (t->alpha) {   /* interleave the decal-mask plane -> RGBA */
        unsigned char *px = (unsigned char *)malloc((size_t)t->w * t->h * 4);
        if (!px) { glDeleteTextures(1, &id); return 0; }
        for (long p = 0; p < (long)t->w * t->h; p++) {
            px[p*4]=t->rgb[p*3]; px[p*4+1]=t->rgb[p*3+1];
            px[p*4+2]=t->rgb[p*3+2]; px[p*4+3]=t->alpha[p];
        }
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t->w,t->h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
        free(px);
    } else
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,t->w,t->h,0,GL_RGB,GL_UNSIGNED_BYTE,t->rgb);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    tex_apply_aniso(); tex_track(id);
    return id;
}

/* Minimal 3x5 bitmap font (uppercase, digits, _ and -), 5 rows x 3 bits each
   (bit 2 = left column). Rendered as one unit-quad per lit pixel — fine for the
   handful of short labels in the menu. Index: 0=space, 1-10='0'-'9', 11-36=A-Z,
   37='_', 38='-'. */
static const unsigned char FONT3x5[][5] = {
    {0,0,0,0,0},                                     /* space */
    {7,5,5,5,7},{2,2,2,2,2},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1}, /* 0 1 2 3 4 */
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7}, /* 5 6 7 8 9 */
    {7,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,6,4,7}, /* A B C D E */
    {7,4,6,4,4},{7,4,5,5,7},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,7}, /* F G H I J */
    {5,5,6,5,5},{4,4,4,4,7},{5,7,5,5,5},{5,7,7,7,5},{7,5,5,5,7}, /* K L M N O */
    {7,5,7,4,4},{7,5,5,7,1},{7,5,6,5,5},{7,4,7,1,7},{7,2,2,2,2}, /* P Q R S T */
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2}, /* U V W X Y */
    {7,1,2,4,7},                                     /* Z */
    {0,0,0,0,7},{0,0,7,0,0},{1,1,2,4,4},             /* _ - / */
};
static const unsigned char *glyph3x5(char c) {
    if (c >= '0' && c <= '9') return FONT3x5[1 + (c-'0')];
    if (c >= 'A' && c <= 'Z') return FONT3x5[11 + (c-'A')];
    if (c == '_') return FONT3x5[37];
    if (c == '-') return FONT3x5[38];
    if (c == '/') return FONT3x5[39];
    return FONT3x5[0];
}
/* Draw an uppercase string at NDC (x,y = top-left), pixel size (px,py). Colour +
   uUnlit/uUseTex are set by the caller; this only sets uMVP and draws pixels. */
void draw_text(GpuMesh *quad, GLint uMVP, const char *s, float x, float y, float px, float py) {
    for (; *s; s++) {
        const unsigned char *g = glyph3x5(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col)) {
                    float M[16] = { px*0.85f,0,0,0, 0,py*0.85f,0,0, 0,0,1,0,
                                    x + col*px, y - row*py, 0, 1 };
                    glUniformMatrix4fv(uMVP, 1, GL_FALSE, M);
                    draw_gpumesh(quad);
                }
        x += 4*px;   /* 3 columns + 1 gap */
    }
}
float text_width(const char *s, float px) { return (float)strlen(s) * 4 * px; }
