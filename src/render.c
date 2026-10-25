/* render.c — OpenUG2 Renderer module implementation. Everything here was
 * extracted verbatim from the original monolithic main.c; the parsing logic
 * (nfsu2.h) is untouched ground truth. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stddef.h>   /* offsetof */
#include <zlib.h>   /* screenshot PNG (dev only) */

#include "render.h"
#include "tex_override.h"
#include "generated_shaders.h"   /* src/shaders/: make-embedded byte arrays */

/* The world and car pipelines of a shader, for helpers that only get the
 * RProg (pipelines_of, below). */
typedef struct { uint32_t shader; UG2WorldPipelines world; UG2CarPipelines car;
                 UG2EffectPipelines effect; UG2OverlayPipelines overlay; } ShaderPipelines;
static const ShaderPipelines *pipelines_of(const RProg *r);
static RhiPipelineDesc world_pipeline_desc(RhiShader shader);   /* below */

/* World glows use world coordinates, never the preceding car/smoke matrix.
   Additive fog fades to black so sprite rectangles cannot add fog colour. */
int render_world_glows(const RProg *r,const N2Batch *batches,int count,
                        const float mvp[16]) {
    float fog[3];ug2_copy(fog, r->p->uFogColor, 3);
    render_model(r,NULL);
    ug2_copy(r->p->uMVP, mvp, 16);
    r->p->uAlpha = 1;r->p->uSoft = 0;
    ug2_vec3(r->p->uFogColor, 0, 0, 0);
    RhiStateSnapshot saved; rhi_cmd_save_state(&saved);
    rhi_cmd_bind_pipeline(pipelines_of(r)->world.add);   /* additive, depth tested, no write */
    int draws=0;
    for(int i=0;i<count;i++) {
        const N2Batch *b=&batches[i];
        if(b->unresolved)continue; /* no texture-backed flare mask available */
        r->p->uUnlit = b->tex.id?0:1;
        r->p->uUseTex = b->tex.id?1:0;
        r->p->uEmissiveTex = b->tex.id?1:0;
        ug2_vec3(r->p->uColor, 1, b->tex.id?1:.85f, b->tex.id?1:.5f);
        rhi_cmd_bind_texture(0,b->tex);
        render_draw_batch(b);draws++;
    }
    ug2_copy(r->p->uFogColor, fog, 3);
    r->p->uEmissiveTex = 0;r->p->uUnlit = 0;
    rhi_cmd_restore_state(&saved);
    return draws;
}

int render_district_lights(const RProg *r, GpuMesh *quad, RhiTexture texture,
                           const N2LightSrc *lights, int nlights,
                           const float cam[3], const float look[3],
                           const float MVP[16], float viewdist,
                           float halo, float gain) {
    RProg rp = *r;
    int draws = 0;
    float *const scalar_loc[] = {&rp.p->uUnlit, &rp.p->uEmissiveTex, &rp.p->uUseTex,
                                &rp.p->uSoft, &rp.p->uAlpha};
    float scalars[5], color[3], fog_color[3], saved_mvp[16];
    for (int i=0; i<5; i++) scalars[i] = *scalar_loc[i];
    ug2_copy(color, rp.p->uColor, 3);
    ug2_copy(fog_color, rp.p->uFogColor, 3);
    ug2_copy(saved_mvp, rp.p->uMVP, 16);
    RhiStateSnapshot saved; rhi_cmd_save_state(&saved);
    float ll = sqrtf(look[0]*look[0] + look[1]*look[1] + look[2]*look[2]);
    if (ll < 1e-4f) ll = 1.0f;
    float ld[3] = {look[0]/ll, look[1]/ll, look[2]/ll};
    float rt[3] = {ld[1], -ld[0], 0};
    float rl = sqrtf(rt[0]*rt[0] + rt[1]*rt[1]);
    if (rl < 1e-4f) rl = 1.0f;
    rt[0] /= rl; rt[1] /= rl;
    float up[3] = {rt[1]*ld[2], -rt[0]*ld[2],
                   rt[0]*ld[1] - rt[1]*ld[0]};
    rhi_cmd_bind_pipeline(pipelines_of(r)->car.halo);   /* additive, depth tested, no write */
    rp.p->uUnlit = 0.0f; rp.p->uEmissiveTex = 1.0f;
    rp.p->uUseTex = 1.0f; rp.p->uSoft = 0.0f;
    /* Additive emission fades toward zero, not fog RGB: black sprite edges
       must add nothing even at long distance. The sky path is unaffected. */
    ug2_vec3(rp.p->uFogColor, 0.0f, 0.0f, 0.0f);
    rhi_cmd_bind_texture(0,texture);
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
        ug2_copy(rp.p->uMVP, LMVP, 16);
        ug2_vec3(rp.p->uColor, (float)( light->rgba        & 0xffu) / 255.0f, (float)((light->rgba >> 8)  & 0xffu) / 255.0f, (float)((light->rgba >> 16) & 0xffu) / 255.0f);
        float a = (float)((light->rgba >> 24) & 0xffu) / 255.0f * gain * fade;
        if (a > 1.0f) a = 1.0f;
        if (a <= 0.002f) continue;
        rp.p->uAlpha = a;
        render_draw_gpumesh(quad); draws++;
    }
    for (int i=0; i<5; i++) *scalar_loc[i] = scalars[i];
    ug2_copy(rp.p->uColor, color, 3);
    ug2_copy(rp.p->uFogColor, fog_color, 3);
    ug2_copy(rp.p->uMVP, saved_mvp, 16);
    rhi_cmd_restore_state(&saved);
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
    ug2_copy(r->p->uWetLightPos[0], &pos[0][0], 4*8);
    ug2_copy(r->p->uWetLightColor[0], &color[0][0], 4*8);
}

void free_road_reflections(RoadReflections *s) {
    if(!s)return;
    rhi_texture_destroy(s->color);rhi_texture_destroy(s->depth);
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
    int32_t viewport[4];
    rhi_viewport(viewport);
    if(viewport[2]<=0 || viewport[3]<=0)return 0;
    RhiStateSnapshot saved_state; rhi_cmd_save_state(&saved_state);
    /* Copies of the frame drawn so far: colour (linear) on unit 3, depth
       (nearest, the main target's precision) on unit 4. */
    RhiTexture *textures[]={&s->color,&s->depth};
    int ok=1;
    for(int i=0;i<2 && ok;i++) if(!textures[i]->id) {
        *textures[i]=rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D,
            i?RHI_FORMAT_DEPTH:RHI_FORMAT_RGBA8,viewport[2],viewport[3],
            RHI_TEXTURE_SAMPLED|RHI_TEXTURE_COPY_DST});
        RhiFilter f=i?RHI_FILTER_NEAREST:RHI_FILTER_LINEAR;
        ok=textures[i]->id && rhi_texture_set_sampler(*textures[i],rhi_sampler_create(
            &(RhiSamplerDesc){f,f,RHI_MIP_NONE,RHI_WRAP_CLAMP,RHI_WRAP_CLAMP,1.0f}))==RHI_OK;
    }
    for(int i=0;i<2 && ok;i++) {
        rhi_cmd_bind_texture(3+(uint32_t)i,*textures[i]);
        ok=rhi_cmd_copy_main_to_texture(*textures[i],viewport[0],viewport[1],viewport[2],viewport[3])==RHI_OK;
    }
    if(!ok) {
        free_road_reflections(s);s->failed=1;
        rhi_cmd_restore_state(&saved_state);return -1;
    }
    s->width=viewport[2];s->height=viewport[3];
    float *const loc[]={&r->p->uUnlit,&r->p->uVista,&r->p->uEmissiveTex,&r->p->uUVCheck,&r->p->uFlipN,
        &r->p->uFresnel,&r->p->uAlphaTest,&r->p->uWetness,&r->p->uRoadReflection};
    float saved[9],matrix[16],model[16],params[4];
    for(int i=0;i<9;i++)saved[i] = *loc[i];
    ug2_copy(matrix, r->p->uMVP, 16);ug2_copy(model, r->p->uModel, 16);
    ug2_copy(params, r->p->uReflectionParams, 4);
    rhi_cmd_bind_pipeline(pipelines_of(r)->world.reflect);
    for(int i=0;i<7;i++)*loc[i] = 0;
    r->p->uWetness = wetness;r->p->uRoadReflection = 1;
    ug2_vec4(r->p->uReflectionParams, projection[10], projection[14], quality==1?16:32, 0);
    ug2_copy(r->p->uMVP, mvp, 16);render_model(r,NULL);
    int draws=0;
    for(int i=0;i<count;i++) {
        const N2Batch *b=&batches[i];if(!road_reflection_receiver(b,cam))continue;
        rhi_cmd_bind_texture(0,b->tex);
        r->p->uAlphaTest = b->drawmode==N2_DRAW_CUTOUT?1:0;
        render_draw_batch(b);draws++;
    }
    for(int i=0;i<9;i++)*loc[i] = saved[i];
    ug2_copy(r->p->uMVP, matrix, 16);ug2_copy(r->p->uModel, model, 16);
    ug2_copy(r->p->uReflectionParams, params, 4);
    rhi_cmd_restore_state(&saved_state);
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
    rhi_buffer_destroy(s->body);
    rhi_buffer_destroy(s->wheel);
    memset(s,0,sizeof *s);
}

/* Flatten indices once per loaded model, not once per frame/traffic actor.
 * Position-only casters merge material slices into one draw per rigid part. */
static int shadow_vertices(const N2Scene *car,int stock,const N2Scene *rims,
                           int wheel,RhiBuffer *buffer,int *vertices) {
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
    *buffer=rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STATIC, n*3*sizeof *positions, positions});
    free(positions);*vertices=(int)n;return 1;
}

int render_car_shadow(const RProg *r,CarShadow *s,const N2Scene *car,const GpuMesh *gpu,
                      int stock,const N2Scene *rims,const GpuMesh *rim_gpu,
                      const float model[16],const float wheels[4][16],const float mvp[16],
                      const float ground[3],const float normal[3],int quality) {
    if(!car || !car->count || !gpu || quality<=0)return 0;
    uint32_t body_key=gpu[0].vbo.id,wheel_key=rims && rims->count && rim_gpu?rim_gpu[0].vbo.id:
                                      stock>=0?gpu[stock].vbo.id:0;
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
    RhiStateSnapshot saved; rhi_cmd_save_state(&saved);
    rhi_cmd_bind_pipeline(pipelines_of(r)->effect.car_shadow);
    r->p->uUnlit = 1;r->p->uSoft = 0;r->p->uUseTex = 0;
    ug2_vec3(r->p->uColor, 0, 0, 0);r->p->uAlpha = .48f;
    /* Body and wheel coverage share the stencil reference for this actor. */
    for(int part=0;part<5;part++) {
        RhiBuffer buffer=part?s->wheel:s->body;int n=part?s->wheel_count:s->body_count;
        if(!buffer.id || !n)continue;
        float matrix[16];mat_mul(projected,part?wheels[part-1]:model,matrix);
        ug2_copy(r->p->uMVP, matrix, 16);
        rhi_cmd_bind_vertex_buffer(0,buffer,0);
        rhi_cmd_draw((uint32_t)n,0);draws++;
    }
    r->p->uUnlit = 0;r->p->uAlpha = 1;
    rhi_cmd_restore_state(&saved);
    return draws;
}

void free_car_environment(CarEnvironment *s) {
    rhi_texture_destroy(s->depth);
    for(int t=0;t<2;t++)rhi_texture_destroy(s->cube[t]);
    rhi_shader_destroy(s->capture.shader);
    memset(s,0,sizeof *s);
}

int render_car_environment(const RProg *r,CarEnvironment *s,const N2Batch *world,int count,
                           const N2Batch *sky,int nsky,const float pos[3],int quality) {
    r->p->uEnvReady = 0;
    if(quality<=0 || count<=0 || s->failed)return 0;
    if(quality==1 && s->size==64 && s->ready && (++s->ticks&1)) {
        r->p->uEnvReady = 1;ug2_copy(r->p->uEnvOrigin, s->published, 3);return 0;
    }
    RhiStateSnapshot saved; rhi_cmd_save_state(&saved);
    int size=quality==1?64:128,draws=0;
    if(s->size!=size) {
        free_car_environment(s);s->size=size;s->capture=render_program();
        if(!s->capture.shader.id){free_car_environment(s);s->failed=1;draws=-1;goto restore;}
        const RhiSampler linear=rhi_sampler_create(&(RhiSamplerDesc){RHI_FILTER_LINEAR,RHI_FILTER_LINEAR,
            RHI_MIP_NONE,RHI_WRAP_CLAMP,RHI_WRAP_CLAMP,1.0f});
        int ok=1;
        for(int t=0;t<2 && ok;t++) {
            s->cube[t]=rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_CUBE,RHI_FORMAT_RGBA8,size,size,
                RHI_TEXTURE_SAMPLED|RHI_TEXTURE_COLOR_TARGET});
            ok=s->cube[t].id!=0;
            for(int face=0;face<6 && ok;face++)ok=rhi_texture_upload(s->cube[t],face,0,NULL,0)==RHI_OK;
            ok=ok && rhi_texture_set_sampler(s->cube[t],linear)==RHI_OK;
        }
        s->depth=rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D,RHI_FORMAT_D16,size,size,RHI_TEXTURE_DEPTH_TARGET});
        /* depth LESS, two-sided: sky blends without depth write, the world is
           opaque or additive */
        RhiPipelineDesc d=world_pipeline_desc(s->capture.shader);
        d.depth_compare=RHI_COMPARE_LESS;
        s->opaque=rhi_pipeline_create(&d);
        d.depth_write=0;d.blend=1;d.src_rgb=d.src_alpha=RHI_BLEND_SRC_ALPHA;
        d.dst_rgb=d.dst_alpha=RHI_BLEND_ONE_MINUS_SRC_ALPHA;
        s->sky=rhi_pipeline_create(&d);
        d.dst_rgb=d.dst_alpha=RHI_BLEND_ONE;
        s->add=rhi_pipeline_create(&d);
        if(!ok || !s->depth.id || !s->opaque.id || !s->sky.id || !s->add.id) {
            free_car_environment(s);s->failed=1;draws=-1;goto restore;
        }
    }
    if(s->face==0)memcpy(s->origin,pos,sizeof s->origin);
    rhi_cmd_bind_pipeline(s->opaque);
    /* the face clears to what the main view clears to (the fog colour) */
    const float *fog_rgb=r->p->uFogColor;
    if(rhi_cmd_begin_pass(&(RhiPassDesc){{s->cube[1-s->front],s->face},{s->depth,0},
                                         RHI_LOAD_CLEAR,RHI_LOAD_CLEAR,
                                         {fog_rgb[0],fog_rgb[1],fog_rgb[2],1},1.0f})!=RHI_OK) {
        free_car_environment(s);s->failed=1;draws=-1;goto restore;
    }
    rhi_cmd_bind_texture(5,s->cube[s->front]);
    const RProg *cr=&s->capture;float ambient,diffuse,fog,fc[3],sun[3];
    ug2_copy(&ambient, &r->p->uAmbient, 1);ug2_copy(&diffuse, &r->p->uDiffuse, 1);
    ug2_copy(&fog, &r->p->uFogDensity, 1);ug2_copy(fc, r->p->uFogColor, 3);
    ug2_copy(sun, r->p->uLight, 3);
    cr->p->uAmbient = ambient;cr->p->uDiffuse = diffuse;
    ug2_copy(cr->p->uFogColor, fc, 3);ug2_copy(cr->p->uLight, sun, 3);
    ug2_copy(cr->p->uCamPos, s->origin, 3);cr->p->uVColor = 1;render_model(cr,NULL);
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
    ug2_copy(cr->p->uMVP, matrix, 16);
    cr->p->uEmissiveTex = 1;cr->p->uFogDensity = 0;
    for(int i=0;i<nsky;i++)if(sky[i].tex.id) {
        rhi_cmd_bind_pipeline(s->sky);
        cr->p->uUseTex = 1;rhi_cmd_bind_texture(0,sky[i].tex);render_draw_batch(sky+i);draws++;
    }
    rhi_cmd_bind_pipeline(s->opaque);cr->p->uEmissiveTex = 0;
    cr->p->uFogDensity = fog;
    mat_persp(1.570796327f,1,.15f,350,p);mat_mul(p,view,matrix);
    ug2_copy(cr->p->uMVP, matrix, 16);
    for(int i=0;i<count;i++) {
        const N2Batch *b=world+i;
        if(b->unresolved || b->drawmode==N2_DRAW_BLEND || !render_batch_in_view(b,matrix))continue;
        cr->p->uUseTex = b->tex.id?1:0;ug2_vec3(cr->p->uColor, .28f, .29f, .31f);
        cr->p->uAlphaTest = b->drawmode==N2_DRAW_CUTOUT?1:0;
        cr->p->uTextureAlpha = b->drawmode==N2_DRAW_ADD?1:0;
        rhi_cmd_bind_pipeline(b->drawmode==N2_DRAW_ADD?s->add:s->opaque);
        rhi_cmd_bind_texture(0,b->tex);render_draw_batch(b);draws++;
    }
    rhi_cmd_end_pass();
    if(++s->face==6){s->face=0;s->front=1-s->front;s->ready=1;memcpy(s->published,s->origin,sizeof s->published);}
restore:
    /* The published cube is this helper's output (unit 5 for the car shader);
       cube bindings are not part of the snapshot, so it survives the restore. */
    if(s->cube[s->front].id)rhi_cmd_bind_texture(5,s->cube[s->front]);
    rhi_cmd_restore_state(&saved);
    r->p->uEnvReady = s->ready?1:0;ug2_copy(r->p->uEnvOrigin, s->published, 3);
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

int render_rain(const RProg *r,RhiBuffer *vbo,float time,float intensity,int quality,float aspect) {
    if(intensity<=0)return 0;
    RainVertex vertices[RAIN_MAX_DROPS*6];
    int n=render_rain_vertices(time,intensity,quality,aspect,vertices);
    if(!n)return 0;
    if(!vbo->id)*vbo=rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STREAM, 0, NULL});
    if(!vbo->id)return 0;
    float *const loc[]={&r->p->uUnlit,&r->p->uEmissiveTex,&r->p->uUseTex,&r->p->uSoft,&r->p->uAlpha,&r->p->uVista,&r->p->uUVCheck};
    float saved[7],matrix[16];
    for(int i=0;i<7;i++)saved[i] = *loc[i];
    ug2_copy(matrix, r->p->uMVP, 16);
    RhiStateSnapshot saved_state; rhi_cmd_save_state(&saved_state);
    rhi_cmd_bind_pipeline(pipelines_of(r)->effect.rain);
    r->p->uUnlit = 1;r->p->uEmissiveTex = 0;r->p->uUseTex = 0;
    r->p->uVista = 0;r->p->uUVCheck = 0;
    r->p->uSoft = 3;r->p->uAlpha = .8f*fminf(intensity,1);
    const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    ug2_copy(r->p->uMVP, identity, 16);
    rhi_buffer_replace(*vbo,(size_t)n*sizeof *vertices,vertices);
    /* No normal input is consumed by the unlit rain branch: the layout leaves
       it without an array, so a preceding small mesh buffer cannot be indexed
       beyond its allocation. */
    rhi_cmd_bind_vertex_buffer(0,*vbo,0);
    rhi_cmd_draw((uint32_t)n,0);
    for(int i=0;i<7;i++)*loc[i] = saved[i];
    ug2_copy(r->p->uMVP, matrix, 16);
    rhi_cmd_restore_state(&saved_state);
    return 1;
}

int render_collision_walls(const RProg *r,RhiBuffer *vbo,const float *faces,int count,
                          const float color[3],const float mvp[16],int through) {
    if(!faces || count<=0 || count>WALL_DEBUG_MAX_FACES)return 0;
    if(!vbo->id)*vbo=rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STREAM, 0, NULL});
    if(!vbo->id)return 0;
    static float edges[WALL_DEBUG_MAX_FACES*18];
    for(int t=0;t<count;t++)for(int e=0;e<3;e++) {
        memcpy(edges+t*18+e*6,faces+t*9+e*3,3*sizeof(float));
        memcpy(edges+t*18+e*6+3,faces+t*9+(e+1)%3*3,3*sizeof(float));
    }
    float *const loc[]={&r->p->uUnlit,&r->p->uEmissiveTex,&r->p->uUseTex,&r->p->uSoft,&r->p->uAlpha,&r->p->uVista,&r->p->uUVCheck};
    float saved[7],matrix[16],old_color[3];
    for(int i=0;i<7;i++)saved[i] = *loc[i];
    ug2_copy(matrix, r->p->uMVP, 16);ug2_copy(old_color, r->p->uColor, 3);
    RhiStateSnapshot saved_state; rhi_cmd_save_state(&saved_state);
    const UG2EffectPipelines *ep = &pipelines_of(r)->effect;
    r->p->uUnlit = 1;r->p->uEmissiveTex = 0;r->p->uUseTex = 0;
    r->p->uSoft = 0;r->p->uVista = 0;r->p->uUVCheck = 0;
    ug2_copy(r->p->uColor, color, 3);ug2_copy(r->p->uMVP, mvp, 16);
    rhi_buffer_replace(*vbo,(size_t)count*27*sizeof(float),NULL);
    rhi_buffer_update(*vbo,0,(size_t)count*9*sizeof(float),faces);
    rhi_buffer_update(*vbo,(size_t)count*9*sizeof(float),(size_t)count*18*sizeof(float),edges);
    rhi_cmd_bind_vertex_buffer(0,*vbo,0);
    r->p->uAlpha = .10f;rhi_cmd_bind_pipeline(ep->walls[!through][0]);rhi_cmd_draw((uint32_t)count*3,0);
    r->p->uAlpha = .85f;rhi_cmd_bind_pipeline(ep->walls[!through][1]);rhi_cmd_draw((uint32_t)count*6,(uint32_t)count*3);
    for(int i=0;i<7;i++)*loc[i] = saved[i];
    ug2_copy(r->p->uMVP, matrix, 16);ug2_copy(r->p->uColor, old_color, 3);
    rhi_cmd_restore_state(&saved_state);
    return 2;
}

/* The UG2 uber shader, headlight shadow and frontend sources live in
 * src/shaders/ and are embedded by make (generated_shaders.h). */

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
    if (n) crc = crc32(crc, d, n);   /* crc32(crc, NULL, 0) returns 0, not crc */
    fwrite(tag, 1, 4, f); if (n) fwrite(d, 1, n, f);   /* IEND has d == NULL: fwrite(NULL, ...) is undefined even for 0 bytes */
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

int render_read_rgb(int w, int h, unsigned char *rgb) {
    return rhi_read_pixels(0, 0, w, h, RHI_READ_RGB8, rgb, 0) == RHI_OK;
}

/* Prefix an embedded shader body (not NUL-terminated) with this build's GLSL
 * header; the game owns the preamble, the backend gets finished text. */
static char *shader_text(const char *header, const unsigned char *body, unsigned len) {
    size_t n = strlen(header);
    char *t = (char *)malloc(n + len + 1);
    if (!t) return NULL;
    memcpy(t, header, n); memcpy(t + n, body, len); t[n + len] = 0;
    return t;
}
RhiShader render_shader_create(const char *label,
                               const char *vs_header, const unsigned char *vs, unsigned vs_len,
                               const char *fs_header, const unsigned char *fs, unsigned fs_len,
                               const char *const *attributes, uint32_t count,
                               const RhiParamLayout *const *blocks, uint32_t block_count) {
    char *v = shader_text(vs_header, vs, vs_len), *f = shader_text(fs_header, fs, fs_len);
    RhiShader out = {0};
    if (v && f)
        out = rhi_shader_create(&(RhiShaderDesc){ v, f, attributes, count, label,
                                                  blocks, block_count });
    free(v); free(f);
    return out;
}

/* Colour maps plus a depth renderbuffer work without depth-texture extensions.
 * ponytail: fixed 512px world-only maps; add vehicle casters when their renderer
 * supports receiving world-space lights. Alpha-blended surfaces do not cast. */
/* Parameters of src/shaders/headlight_shadow.* (std140 order). */
struct HeadlightShadowParams { float uMVP[16]; float uCutout, uRange; };
static const RhiParamField headlight_shadow_fields[] = {
    {"uMVP", RHI_PARAM_MAT4, offsetof(HeadlightShadowParams, uMVP), 1},
    {"uCutout", RHI_PARAM_FLOAT, offsetof(HeadlightShadowParams, uCutout), 1},
    {"uRange", RHI_PARAM_FLOAT, offsetof(HeadlightShadowParams, uRange), 1},
};
static const RhiParamLayout headlight_shadow_layout = {
    headlight_shadow_fields, 3, (uint32_t)sizeof(HeadlightShadowParams), "HeadlightShadowParams"
};

void free_headlight_shadows(HeadlightShadows *s) {
    for(int h=0;h<2;h++)rhi_texture_destroy(s->map[h]);
    rhi_texture_destroy(s->depth);rhi_texture_destroy(s->white);
    rhi_shader_destroy(s->shader);
    memset(s,0,sizeof *s);
}
static int headlight_shadow_init(HeadlightShadows *s) {
    static const char *const attributes[] = { "aPos", "aUV" };
    static const RhiParamLayout *const blocks[] = { &headlight_shadow_layout };
    s->shader=render_shader_create("headlight shadow",
        GLSL_VS_HEADER, src_shaders_headlight_shadow_vert, src_shaders_headlight_shadow_vert_len,
        GLSL_FS_HEADER, src_shaders_headlight_shadow_frag, src_shaders_headlight_shadow_frag_len,
        attributes, 2, blocks, 1);
    s->params=(HeadlightShadowParams *)rhi_shader_params(s->shader,0);
    if(!s->params)return 0;
    const RhiSampler nearest=rhi_sampler_create(&(RhiSamplerDesc){RHI_FILTER_NEAREST,RHI_FILTER_NEAREST,
        RHI_MIP_NONE,RHI_WRAP_CLAMP,RHI_WRAP_CLAMP,1.0f});
    for(int h=0;h<2;h++) {
        s->map[h]=rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D,RHI_FORMAT_RGBA8,512,512,
            RHI_TEXTURE_SAMPLED|RHI_TEXTURE_COLOR_TARGET});
        if(!s->map[h].id || rhi_texture_upload(s->map[h],0,0,NULL,0)!=RHI_OK ||
           rhi_texture_set_sampler(s->map[h],nearest)!=RHI_OK)return 0;
    }
    unsigned char white[]={255,255,255};N2Tex tex={.w=1,.h=1,.rgb=white};s->white=upload_tex(&tex);
    s->depth=rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D,RHI_FORMAT_D16,512,512,RHI_TEXTURE_DEPTH_TARGET});
    /* casters: BatchedVertex, depth test LESS with write, no blend, two-sided */
    RhiPipelineDesc d=world_pipeline_desc(s->shader);
    d.depth_compare=RHI_COMPARE_LESS;
    s->pipeline=rhi_pipeline_create(&d);
    return s->white.id && s->depth.id && s->pipeline.id;
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
    float gain;ug2_copy(&gain, &r->p->uHeadGain, 1);
    r->p->uHeadShadow = 0.0f;
    if(gain<=0 || s->failed)return s->failed?-1:0;
    float pos[2][4],forward[3],right[3],up[3],shape[4];
    /* Inputs come from the CPU uniform mirror of render_headlights' writes;
       the mirror keeps the 2-element uHeadPos array contiguous. */
    ug2_copy(pos[0], r->p->uHeadPos[0], 8);
    ug2_copy(forward, r->p->uHeadForward, 3);ug2_copy(right, r->p->uHeadRight, 3);
    ug2_copy(up, r->p->uHeadUp, 3);ug2_copy(shape, r->p->uHeadShape, 4);
    RhiStateSnapshot saved; rhi_cmd_save_state(&saved);
    int draws=0;float matrices[2][16];
    if(!s->shader.id && !headlight_shadow_init(s)) {
        free_headlight_shadows(s);s->failed=1;
        fprintf(stderr,"Headlight shadows unavailable; retaining unshadowed beams.\n");
        draws=-1;goto restore;
    }
    rhi_cmd_bind_pipeline(s->pipeline);
    s->params->uRange=shape[3];
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
        /* far = white, depth 1 */
        if(rhi_cmd_begin_pass(&(RhiPassDesc){{s->map[h],0},{s->depth,0},RHI_LOAD_CLEAR,RHI_LOAD_CLEAR,
                                             {1,1,1,1},1.0f})!=RHI_OK) {
            free_headlight_shadows(s);s->failed=1;
            fprintf(stderr,"Headlight shadows unavailable; retaining unshadowed beams.\n");
            draws=-1;goto restore;
        }
        if(pos[h][3]>=.5f) {
            ug2_copy(s->params->uMVP,matrices[h],16);
            for(int i=0;i<count;i++) {
                const N2Batch *b=batches+i;
                if(b->drawmode!=N2_DRAW_OPAQUE && b->drawmode!=N2_DRAW_CUTOUT)continue;
                if(!render_batch_in_view(b,matrices[h]))continue;
                rhi_cmd_bind_texture(0,b->tex.id?b->tex:s->white);
                s->params->uCutout=b->tex.id && b->drawmode==N2_DRAW_CUTOUT?1.0f:0.0f;
                render_draw_batch(b);draws++;
            }
        }
        rhi_cmd_end_pass();
    }
restore:
    if(draws>=0) {
        ug2_copy(r->p->uHeadShadowMVP[0], matrices[0], 16*2);
        /* Units 1 and 2 are reserved for the two maps; ordinary textures use 0.
           The maps are this helper's output: they stay bound after the restore. */
        for(int h=0;h<2;h++)rhi_state_set_texture(&saved,1+(uint32_t)h,s->map[h]);
        r->p->uHeadShadowMap[0] = 1;r->p->uHeadShadowMap[1] = 2;
        r->p->uHeadShadow = 1.0f;
    }
    rhi_cmd_restore_state(&saved);
    return draws;
}

void render_headlights(const RProg *r, const float model[16], const float anchors[4][4],
                       int high, float pitch, float range, float gain) {
    float pos[2][4];
    for(int b=0;b<2;b++) {
        for(int a=0;a<3;a++)pos[b][a]=model[a]*anchors[b][0]+model[4+a]*anchors[b][1]+
                                    model[8+a]*anchors[b][2]+model[12+a];
        pos[b][3]=anchors[b][3];
    }
    ug2_copy(r->p->uHeadPos[0], &pos[0][0], 4*2);
    ug2_copy(r->p->uHeadForward, model, 3);
    ug2_copy(r->p->uHeadRight, model+4, 3);
    ug2_copy(r->p->uHeadUp, model+8, 3);
    ug2_vec4(r->p->uHeadShape, high?.20f:.38f, high?.095f:.12f, tanf(pitch*3.14159265f/180.0f), fmaxf(range,1.0f));
    r->p->uHeadLow = high?0.0f:1.0f;
    r->p->uHeadGain = fmaxf(gain,0.0f);
    r->p->uHeadShadow = 0.0f; /* maps must be refreshed for this pose */
}

void render_tail_lamp(const RProg *r,RhiTexture texture,int running,int braking,
                      int boost,float gain) {
    int on=running || braking,hot=braking || boost;
    float level=on?fmaxf(gain,0.0f):0.0f;
    r->p->uUnlit = 0.0f;
    ug2_vec3(r->p->uColor, .17f, .020f, .016f);
    ug2_vec3(r->p->uEmissive, (hot?1.00f:.62f)*level, (hot?.16f:.05f)*level, (hot?.10f:.04f)*level);
    r->p->uSpec = .26f;r->p->uEnv = .16f;
    r->p->uGloss = 20.0f;r->p->uClearcoat = 0.0f;
    r->p->uDecal = 0.0f;r->p->uUseTex = texture.id?1.0f:0.0f;
    if(texture.id)rhi_cmd_bind_texture(0,texture);
}

void render_model(const RProg *r,const float model[16]) {
    static const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    ug2_copy(r->p->uModel, model?model:identity, 16);
}

static const RhiParamLayout *const ug2_blocks[] = { &ug2_params_layout };

RProg render_program(void) {
    static const char *const attributes[] = { "aPos", "aUV", "aNor", "aColor" };
    RProg r;
    r.shader = render_shader_create("ug2",
        GLSL_VS_HEADER, src_shaders_ug2_vert, src_shaders_ug2_vert_len,
        GLSL_FS_HEADER, src_shaders_ug2_frag, src_shaders_ug2_frag_len, attributes, 4,
        ug2_blocks, 1);
    r.prog = rhi_shader_native_program(r.shader);
    r.p = (UG2Params *)rhi_shader_params(r.shader, 0);
    if (!r.p) {   /* no usable shader: keep writes harmless, draws show nothing */
        static UG2Params unused;
        r.p = &unused;
    }
    render_model(&r,NULL);
    r.p->uReflectionColor = 3; r.p->uReflectionDepth = 4;
    r.p->uEnvCube = 5;r.p->uEnvReady = 0;
    r.p->uHeadShadowMap[0] = 0;r.p->uHeadShadowMap[1] = 0;
    r.p->uHeadShadow = 0.0f;
    r.p->uHeadGain = 0.0f;
    r.p->uAlpha = 1.0f; r.p->uSoft = 0.0f; r.p->uSpec = 0.0f;
    r.p->uDecal = 0.0f; r.p->uRimTint = 0.0f;
    ug2_vec3(r.p->uEmissive, 0.0f, 0.0f, 0.0f);
    ug2_vec3(r.p->uFogColor, 0.06f, 0.07f, 0.11f); r.p->uFogDensity = 0.0f;
    ug2_vec3(r.p->uCamPos, 0, 0, 0); r.p->uEnv = 0.0f;
    r.p->uUVCheck = 0.0f;
    r.p->uGloss = 20.0f;
    r.p->uFlipN = 0.0f;
    r.p->uAlphaTest = 0.0f;
    r.p->uTextureAlpha = 0.0f;
    r.p->uEmissiveTex = 0.0f;
    ug2_vec3(r.p->uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);
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
        rhi_buffer_destroy(gm[i].vbo);
        rhi_buffer_destroy(gm[i].nbo);
        rhi_buffer_destroy(gm[i].cbo);
        rhi_buffer_destroy(gm[i].ibo);
    }
    free(gm);
}

/* Upload meshes with per-vertex normals; any failure releases every buffer and
 * returns NULL. */
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
        gm[i].vbo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STATIC, m->nverts*5*sizeof(float), m->verts});
        gm[i].nbo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STATIC, m->nverts*3*sizeof(float), nor});
        if (m->vcol)
            gm[i].cbo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STATIC, (size_t)m->nverts*4, m->vcol});
        gm[i].ibo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_INDEX, RHI_UPDATE_STATIC, m->nidx*sizeof(uint16_t), m->idx});
        gm[i].nidx = m->nidx; gm[i].cat = m->cat; gm[i].texkey = m->texkey;
        gm[i].trim = m->trim; gm[i].draw_mode = m->draw_mode;
        gm[i].car_material = m->car_material;
        free(nor);
        free(rounded.verts); free(rounded.idx);
        if (!gm[i].vbo.id || !gm[i].nbo.id || !gm[i].ibo.id || (m->vcol && !gm[i].cbo.id)) {
            free_scene_gpu(gm, s->count);
            return NULL;
        }
    }
    return gm;
}

/* ---- static-world batching ---- */

/* key holds the GL texture name so the batch order stays as it was; tex is
 * the handle the batch keeps. */
typedef struct { uint64_t key; int idx; unsigned char group; RhiTexture tex; } BSortEnt;
static int bsort_cmp(const void *a, const void *b) {
    const BSortEnt *aa = (const BSortEnt *)a, *bb = (const BSortEnt *)b;
    if (aa->key != bb->key) return aa->key < bb->key ? -1 : 1;
    return aa->group < bb->group ? -1 : aa->group > bb->group ? 1 : 0;
}
static int btex_cmp(const void *a, const void *b) {   /* final texture order */
    /* native ids, not handles: keeps the order (and so the image) unchanged. */
    uint32_t ta = rhi_texture_native_id(((const N2Batch *)a)->tex),
             tb = rhi_texture_native_id(((const N2Batch *)b)->tex);
    return ta < tb ? -1 : ta > tb ? 1 : 0;
}

#define BATCH_CELL     256.0f   /* grid cell edge, metres */
#define BATCH_MAXVERTS 65535    /* u16 indices (GLES2: no u32 without ext) */

/* Milestone 75: the VBO layout world_pipeline_desc() hardcodes must equal the struct's
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
                                 int bidx, RhiTexture tex, const N2Batch *bref) {
    (void)bref;
    printf("MILESTONE: 79\n");
    printf("batch index   %d\n", bidx);
    printf("texkey        %08x   gl_tex %u   nmesh %d\n",
           s->meshes[ent[i0].idx].texkey, (unsigned)rhi_texture_native_id(tex), i1 - i0);
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
                       RhiTexture tex, N2Batch *b, int bidx, const char *audit,
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
    b->vbo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STATIC, (size_t)nv * sizeof *bv, bv});
    b->ibo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_INDEX, RHI_UPDATE_STATIC, (size_t)ni * 2, bi});
    if (!b->vbo.id || !b->ibo.id) {
        rhi_buffer_destroy(b->vbo); rhi_buffer_destroy(b->ibo);
        memset(b, 0, sizeof *b); free(bv); free(bi); return 0;
    }
    for(int k=i0;k<i1;k++)b->nprops+=s->meshes[ent[k].idx].prop_id!=0;
    if(b->nprops) {
        b->props=calloc((size_t)b->nprops,sizeof *b->props);
        if(!b->props) {
            rhi_buffer_destroy(b->vbo);rhi_buffer_destroy(b->ibo);
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
    b->unresolved = (unsigned char)(!tex.id && named == i1 - i0);
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
                          const float (*mbb)[4], const RhiTexture *mtex,
                          RhiTexture texTerr, const char *audit,
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
        RhiTexture tex = mtex[i];
        if (!tex.id && mesh->cat == N2_TERRAIN) tex = texTerr;   /* fallback baked in */
        float cx = (mbb[i][0]+mbb[i][2])*0.5f, cy = (mbb[i][1]+mbb[i][3])*0.5f;
        uint64_t cell = (uint64_t)(uint32_t)((int)((cy-y0)/BATCH_CELL)*4096
                                           + (int)((cx-x0)/BATCH_CELL));
        ent[m].key = cell << 32 | rhi_texture_native_id(tex); ent[m].idx = i; ent[m].tex = tex;
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
        if (!batch_emit(s, ent, i0, i, ent[i0].tex,
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
                         const RhiTexture *mtex, RhiTexture texTerr, N2Batch **out,
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
int upload_cat_batches(const N2Scene *s, int cat, const RhiTexture *mtex, N2Batch **out,
                       const unsigned char *mtexmode) {
    int n = s->count;
    BSortEnt *ent = (BSortEnt *)malloc((size_t)(n ? n : 1) * sizeof *ent);
    if (!ent) return -1;
    int m = 0;
    for (int i = 0; i < n; i++)
        if (s->meshes[i].cat == cat) {
            ent[m].key = rhi_texture_native_id(mtex[i]); ent[m].idx = i; ent[m].tex = mtex[i];
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
            if (!batch_emit(s, ent, i0, i, ent[i0].tex,
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
            rhi_buffer_destroy((*batches)[i].vbo);
            rhi_buffer_destroy((*batches)[i].ibo);
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
        if(rhi_buffer_update(b->vbo,(size_t)range->vertex*sizeof *v,(size_t)m->nverts*sizeof *v,v)!=RHI_OK)
            {free(v);return -1;}
        for(int q=0;q<m->nverts;q++)for(int a=0;a<3;a++) {
            b->bbox_min[a]=fminf(b->bbox_min[a],v[q].pos[a]);
            b->bbox_max[a]=fmaxf(b->bbox_max[a],v[q].pos[a]);
        }
        free(v);range->revision=m->prop_revision;changed++;
    }
    return changed;
}

/* ---- world pipelines (sky, vista, world passes) ----
 * BatchedVertex: pos @0, uv @12, normal @20, RGBA8 prelight @32, stride 36.
 * All world passes test depth with LEQUAL and never cull. */
/* BatchedVertex over `shader`, depth test LEQUAL with write, no blend, two-sided. */
static RhiPipelineDesc world_pipeline_desc(RhiShader shader) {
    RhiPipelineDesc d;
    rhi_pipeline_desc_init(&d);
    d.shader = shader;
    d.vertex.attribs[0] = (RhiVertexAttrib){ 0, 0, 0,  RHI_VERTEX_FLOAT3 };
    d.vertex.attribs[1] = (RhiVertexAttrib){ 1, 0, 12, RHI_VERTEX_FLOAT2 };
    d.vertex.attribs[2] = (RhiVertexAttrib){ 2, 0, 20, RHI_VERTEX_FLOAT3 };
    d.vertex.attribs[3] = (RhiVertexAttrib){ 3, 0, 32, RHI_VERTEX_UNORM8X4 };
    d.vertex.attrib_count = 4;
    d.vertex.stride[0] = sizeof(BatchedVertex);
    d.depth_test = 1;
    d.depth_compare = RHI_COMPARE_LESS_EQUAL;
    return d;
}

static RhiPipelineDesc world_pipeline(const RProg *r, int depth_write, int blend,
                                      RhiBlendFactor dst) {
    RhiPipelineDesc d = world_pipeline_desc(r->shader);
    d.depth_write = (uint8_t)depth_write;
    d.blend = (uint8_t)blend;
    if (blend) {
        d.src_rgb = d.src_alpha = RHI_BLEND_SRC_ALPHA;
        d.dst_rgb = d.dst_alpha = dst;
    }
    return d;
}

void render_world_pipelines(const RProg *r, UG2WorldPipelines *p) {
    RhiPipelineDesc d;
    d = world_pipeline(r, 0, 0, RHI_BLEND_ZERO);
    p->sky_dome = rhi_pipeline_create(&d);
    p->overlay = p->sky_dome;   /* same state: depth test, no write, no blend */
    d = world_pipeline(r, 0, 1, RHI_BLEND_ONE_MINUS_SRC_ALPHA);
    p->sky_cap = rhi_pipeline_create(&d);
    p->vista = p->sky_cap;
    p->blend = p->sky_cap;
    d = world_pipeline(r, 1, 0, RHI_BLEND_ZERO);
    p->opaque = rhi_pipeline_create(&d);
    d = world_pipeline(r, 0, 1, RHI_BLEND_ONE);
    p->add = rhi_pipeline_create(&d);
    d = world_pipeline(r, 0, 1, RHI_BLEND_ONE_MINUS_SRC_ALPHA);
    d.depth_compare = RHI_COMPARE_EQUAL;   /* exactly the road surfaces already drawn */
    p->reflect = rhi_pipeline_create(&d);
}

/* ---- car pipelines ----
 * GpuMesh streams: 0 = position + uv (20 B), 1 = normal (12 B), 2 = optional
 * RGBA8 prelight. Colour comes first in the layout, the order the hand-written
 * draw set the attributes up in. All car passes test depth with LEQUAL. */
static RhiPipelineDesc car_pipeline(const RProg *r, RhiCullMode cull, int depth_write,
                                    int blend, RhiBlendFactor dst) {
    RhiPipelineDesc d;
    rhi_pipeline_desc_init(&d);
    d.shader = r->shader;
    d.vertex.attribs[0] = (RhiVertexAttrib){ 3, 2, 0,  RHI_VERTEX_UNORM8X4 };
    d.vertex.attribs[1] = (RhiVertexAttrib){ 0, 0, 0,  RHI_VERTEX_FLOAT3 };
    d.vertex.attribs[2] = (RhiVertexAttrib){ 1, 0, 12, RHI_VERTEX_FLOAT2 };
    d.vertex.attribs[3] = (RhiVertexAttrib){ 2, 1, 0,  RHI_VERTEX_FLOAT3 };
    d.vertex.attrib_count = 4;
    d.vertex.stride[0] = 5 * sizeof(float);
    d.vertex.stride[1] = 3 * sizeof(float);
    d.vertex.stride[2] = 4;
    d.cull = cull;
    d.depth_test = 1;
    d.depth_compare = RHI_COMPARE_LESS_EQUAL;
    d.depth_write = (uint8_t)depth_write;
    d.blend = (uint8_t)blend;
    if (blend) {
        d.src_rgb = d.src_alpha = RHI_BLEND_SRC_ALPHA;
        d.dst_rgb = d.dst_alpha = dst;
    }
    return d;
}

void render_car_pipelines(const RProg *r, UG2CarPipelines *p) {
    RhiPipelineDesc d;
    for (int c = RHI_CULL_NONE; c <= RHI_CULL_FRONT; c++) {
        d = car_pipeline(r, (RhiCullMode)c, 1, 0, RHI_BLEND_ZERO);
        p->opaque[c] = rhi_pipeline_create(&d);
    }
    d = car_pipeline(r, RHI_CULL_NONE, 0, 1, RHI_BLEND_ONE_MINUS_SRC_ALPHA);
    p->glass = rhi_pipeline_create(&d);
    d = car_pipeline(r, RHI_CULL_NONE, 1, 1, RHI_BLEND_ONE_MINUS_SRC_ALPHA);
    p->glass_depth = rhi_pipeline_create(&d);
    d = car_pipeline(r, RHI_CULL_NONE, 0, 1, RHI_BLEND_ONE);
    p->halo = rhi_pipeline_create(&d);
}

/* ---- tire-mark pass (SPEC TM-15) ---- */
void render_mark_begin(const RProg *r, RMarkSaved *saved) {
    saved->unlit = r->p->uUnlit; saved->use_tex = r->p->uUseTex; saved->alpha = r->p->uAlpha;
    saved->vcolor = r->p->uVColor; saved->vista = r->p->uVista;
    rhi_cmd_bind_pipeline(pipelines_of(r)->car.glass);   /* src alpha / 1-src alpha, depth <= no write, no cull, no bias */
    render_model(r, NULL);             /* vertices are world space: identity */
    r->p->uUnlit = 0.0f; r->p->uUseTex = 1.0f; r->p->uAlpha = 1.0f; r->p->uVColor = 0.0f;
    r->p->uVista = 2.0f;               /* mark law in the fragment shader */
}
void render_mark_end(const RProg *r, const RMarkSaved *saved) {
    r->p->uVista = saved->vista; r->p->uUnlit = saved->unlit; r->p->uUseTex = saved->use_tex;
    r->p->uAlpha = saved->alpha; r->p->uVColor = saved->vcolor;
}

/* ---- effect pipelines ----
 * Car shadow and collision walls: position only (attribute 0, 12 B). Rain:
 * RainVertex (position, uv, fade). All blend with alpha and never cull or
 * write depth; the shadow and the walls test depth with LEQUAL. */
static RhiPipelineDesc effect_pipeline(const RProg *r, int depth_test, RhiPrimitive primitive) {
    RhiPipelineDesc d;
    rhi_pipeline_desc_init(&d);
    d.shader = r->shader;
    d.vertex.attribs[0] = (RhiVertexAttrib){ 0, 0, 0, RHI_VERTEX_FLOAT3 };
    d.vertex.attrib_count = 1;
    d.vertex.stride[0] = 3 * sizeof(float);
    d.primitive = primitive;
    d.depth_test = (uint8_t)depth_test;
    d.depth_compare = RHI_COMPARE_LESS_EQUAL;
    d.depth_write = 0;
    d.blend = 1;
    d.src_rgb = d.src_alpha = RHI_BLEND_SRC_ALPHA;
    d.dst_rgb = d.dst_alpha = RHI_BLEND_ONE_MINUS_SRC_ALPHA;
    return d;
}

void render_effect_pipelines(const RProg *r, UG2EffectPipelines *p) {
    RhiPipelineDesc d = effect_pipeline(r, 1, RHI_PRIMITIVE_TRIANGLES);
    /* each pixel of an actor's shadow is darkened once: draw where the
       stencil is still 0 and count it up */
    d.stencil_test = 1;
    d.stencil_compare = RHI_COMPARE_EQUAL;
    d.stencil_ref = 0;
    d.stencil_pass = RHI_STENCIL_INCR;
    p->car_shadow = rhi_pipeline_create(&d);
    for (int depth = 0; depth < 2; depth++)
        for (int lines = 0; lines < 2; lines++) {
            d = effect_pipeline(r, depth, lines ? RHI_PRIMITIVE_LINES : RHI_PRIMITIVE_TRIANGLES);
            p->walls[depth][lines] = rhi_pipeline_create(&d);
        }
    d = effect_pipeline(r, 0, RHI_PRIMITIVE_TRIANGLES);
    d.vertex.attribs[1] = (RhiVertexAttrib){ 1, 0, 12, RHI_VERTEX_FLOAT2 };
    d.vertex.attribs[2] = (RhiVertexAttrib){ 3, 0, 20, RHI_VERTEX_FLOAT1 };
    d.vertex.attrib_count = 3;
    d.vertex.stride[0] = sizeof(RainVertex);
    p->rain = rhi_pipeline_create(&d);
}

/* Two shaders use helpers (main view and environment capture). */
static const ShaderPipelines *pipelines_of(const RProg *r) {
    static ShaderPipelines cache[4];
    static int count, next;
    for (int i = 0; i < count; i++)
        if (cache[i].shader == r->shader.id) return &cache[i];
    int slot = count < 4 ? count++ : (next++ & 3);
    cache[slot].shader = r->shader.id;
    render_world_pipelines(r, &cache[slot].world);
    render_car_pipelines(r, &cache[slot].car);
    render_effect_pipelines(r, &cache[slot].effect);
    RhiPipelineDesc d = car_pipeline(r, RHI_CULL_NONE, 1, 0, RHI_BLEND_ZERO);
    d.depth_test = 0;
    cache[slot].overlay.screen = rhi_pipeline_create(&d);
    d = car_pipeline(r, RHI_CULL_NONE, 0, 1, RHI_BLEND_ONE_MINUS_SRC_ALPHA);
    d.depth_test = 0;
    cache[slot].overlay.screen_blend = rhi_pipeline_create(&d);
    return &cache[slot];
}

const UG2OverlayPipelines *render_overlay_pipelines(const RProg *r) {
    return &pipelines_of(r)->overlay;
}

/* A GpuMesh through the bound car pipeline. */
void render_draw_gpumesh(const GpuMesh *g) {
    rhi_cmd_bind_vertex_buffer(0, g->vbo, 0);
    rhi_cmd_bind_vertex_buffer(1, g->nbo, 0);
    rhi_cmd_bind_vertex_buffer(2, g->cbo, 0);
    rhi_cmd_bind_index_buffer(g->ibo);
    rhi_cmd_draw_indexed((uint32_t)g->nidx, 0);
}

/* A world batch through the bound world pipeline. */
void render_draw_batch(const N2Batch *b) {
    rhi_cmd_bind_vertex_buffer(0, b->vbo, 0);
    rhi_cmd_bind_index_buffer(b->ibo);
    rhi_cmd_draw_indexed((uint32_t)b->index_count, 0);
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
RhiTexture make_wheel_tex(void) {
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
    RhiTexture id = upload_tex(&t);
    /* radial, single-sample cap texture (no tiling intended) — clamp so a
       filter footprint near u/v=0 or 1 can't wrap and bleed in colour from
       the opposite edge, same reasoning as the car atlas/vinyl clamps. */
    render_texture_clamp(id);
    return id;
}

/* unit-quad buffers for the 2D HUD / billboards (drawn in NDC via uMVP) */
GpuMesh make_quad(void) {
    GpuMesh quad; memset(&quad, 0, sizeof quad);
    float qv[] = {0,0,0,0,0, 1,0,0,1,0, 1,1,0,1,1, 0,1,0,0,1};
    float qn[] = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
    uint16_t qi[] = {0,1,2, 0,2,3};
    quad.vbo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STATIC, sizeof qv, qv});
    quad.nbo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX, RHI_UPDATE_STATIC, sizeof qn, qn});
    quad.ibo = rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_INDEX, RHI_UPDATE_STATIC, sizeof qi, qi});
    quad.nidx = 6;
    return quad;
}

void render_wheel_mesh(const RProg *r, GpuMesh *mesh, RhiTexture texture, int mode) {
    float *const loc[]={&r->p->uUseTex,&r->p->uAlphaTest,&r->p->uTextureAlpha,&r->p->uAlpha,&r->p->uDecal,
                       &r->p->uRimTint,&r->p->uSpec,&r->p->uEnv,&r->p->uClearcoat,&r->p->uVColor};
    float saved[10];for(int i=0;i<10;i++)saved[i] = *loc[i];
    RhiStateSnapshot saved_state; rhi_cmd_save_state(&saved_state);
    unsigned char amode = mesh->draw_mode ? mesh->draw_mode : (unsigned char)mode;
    int cut=texture.id && amode==N2_DRAW_CUTOUT, translucent=texture.id && amode==N2_DRAW_BLEND;
    /* The wheel INTERIOR slice is the inboard barrel/backing. Its atlas
       rectangle also contains transparent texels from the tread strip, so
       alpha discard breaks the generated round backing into black facets.
       Keep its authored RGB (usually black) but make the backing continuous;
       TIRE and RIM retain their authored cutout textures. */
    /* Prelit traffic wheels use a cutout quad for the visible wheel face. */
    if (mesh->car_material == N2_MAT_INTERIOR && !mesh->cbo.id) cut = 0;
    r->p->uVColor = mesh->cbo.id?2.0f:0.0f;
    rhi_cmd_bind_texture(0,texture);
    r->p->uUseTex = texture.id?1.0f:0.0f;
    r->p->uAlphaTest = cut?1.0f:0.0f;
    r->p->uTextureAlpha = translucent?1.0f:0.0f;
    r->p->uAlpha = 1.0f;r->p->uDecal = 0.0f;
    uint32_t material=mesh->car_material;
    if (!texture.id || (material!=N2_MAT_MAGSILVER && material!=N2_MAT_MAGCHROME))
        r->p->uRimTint = 0.0f;
    if (material==N2_MAT_RUBBER || material==N2_MAT_INTERIOR || material==N2_MAT_DULLPLASTIC) {
        r->p->uSpec = 0.0f;r->p->uEnv = 0.0f;
        r->p->uClearcoat = 0.0f;
    }
    /* two-sided, depth tested; translucent slices blend without depth write */
    const UG2CarPipelines *cp = &pipelines_of(r)->car;
    rhi_cmd_bind_pipeline(translucent ? cp->glass : cp->opaque[RHI_CULL_NONE]);
    render_draw_gpumesh(mesh);
    for(int i=0;i<10;i++)*loc[i] = saved[i];
    rhi_cmd_restore_state(&saved_state);
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

/* S3TC capability flag (rhi_caps()->tex_bc1..3; the formats live in rhi_gl.c). */
int g_tex_s3tc = 0;
int   g_tex_aniso_max = 1;
float g_tex_aniso     = 1.0f;

/* Every texture this renderer owns, so a detail change can re-apply to all of
 * them. Handles are recorded on upload and checked with rhi_texture_is_live
 * before use -- residents and the world cache destroy textures behind our back,
 * and setting a parameter on a deleted name is a GL error, not a no-op. */
static RhiTexture *g_tex_all; static int g_tex_all_n, g_tex_all_cap;
static void tex_track(RhiTexture id) {
    if (!id.id) return;
    if (g_tex_all_n == g_tex_all_cap) {
        int cap = g_tex_all_cap ? g_tex_all_cap * 2 : 256;
        RhiTexture *t = (RhiTexture *)realloc(g_tex_all, (size_t)cap * sizeof *t);
        if (!t) return;                       /* out of memory: just don't track it */
        g_tex_all = t; g_tex_all_cap = cap;
    }
    g_tex_all[g_tex_all_n++] = id;
}
/* Sampler of an uploaded texture: trilinear with the current anisotropy when
 * it has a full mip chain, bilinear on level 0 when it has not. */
static RhiSampler tex_sampler(int mipped) {
    RhiSamplerDesc d = { RHI_FILTER_LINEAR, RHI_FILTER_LINEAR,
                         mipped ? RHI_MIP_LINEAR : RHI_MIP_NONE,
                         RHI_WRAP_REPEAT, RHI_WRAP_REPEAT, mipped ? g_tex_aniso : 1.0f };
    return rhi_sampler_create(&d);
}
static void tex_change_sampler(RhiTexture tex, const RhiSamplerDesc *d) {
    rhi_texture_set_sampler(tex, rhi_sampler_create(d));
}
void render_texture_clamp(RhiTexture tex) {
    const RhiSamplerDesc *cur = rhi_sampler_desc(rhi_texture_sampler(tex));
    if (!cur) return;
    RhiSamplerDesc d = *cur;
    d.wrap_u = d.wrap_v = RHI_WRAP_CLAMP;
    tex_change_sampler(tex, &d);
}
void render_texture_clamp_linear(RhiTexture tex) {
    const RhiSamplerDesc *cur = rhi_sampler_desc(rhi_texture_sampler(tex));
    if (!cur) return;
    RhiSamplerDesc d = *cur;
    d.wrap_u = d.wrap_v = RHI_WRAP_CLAMP;
    d.min_filter = d.mag_filter = RHI_FILTER_LINEAR;
    d.mip_filter = RHI_MIP_NONE;
    tex_change_sampler(tex, &d);
}
void render_texture_detail(float aniso) {
    if (g_tex_aniso_max <= 1) { g_tex_aniso = 1.0f; return; }
    if (aniso < 1.0f) aniso = 1.0f;
    if (aniso > (float)g_tex_aniso_max) aniso = (float)g_tex_aniso_max;
    if (aniso == g_tex_aniso) return;
    g_tex_aniso = aniso;
    RhiStateSnapshot saved; rhi_cmd_save_state(&saved);   /* sampler changes bind */
    int live = 0;
    for (int i = 0; i < g_tex_all_n; i++) {
        if (!rhi_texture_is_live(g_tex_all[i])) continue;   /* freed since upload */
        g_tex_all[live++] = g_tex_all[i];
        RhiSamplerDesc d = *rhi_sampler_desc(rhi_texture_sampler(g_tex_all[i]));
        d.max_anisotropy = g_tex_aniso;
        tex_change_sampler(g_tex_all[i], &d);
    }
    g_tex_all_n = live;                                 /* compact away dead ids */
    rhi_cmd_restore_state(&saved);
}

/* tex_override.c: sampler + tracking for an override it has just uploaded. */
void render_tex_finish(RhiTexture id, int mipped) {
    rhi_texture_set_sampler(id, tex_sampler(mipped));
    if (mipped) tex_track(id);
}

/* Keyed siblings of the two uploaders: try a user HD override first (only when
   --tex-overrides indexed one for this key), else the unchanged original path.
   The caller keeps `t` for draw mode, alpha policy and measurements. */
RhiTexture upload_tex_keyed(uint32_t key, const char *pack, const N2Tex *t) {
    RhiTexture id = tex_override_upload(key, pack, t);
    return id.id ? id : upload_tex(t);
}
RhiTexture upload_tpk_keyed(uint32_t key, const char *pack, const N2Tex *t) {
    RhiTexture id = tex_override_upload(key, pack, t);
    return id.id ? id : upload_tpk_texture_to_gpu(t);
}

RhiTexture upload_tpk_texture_to_gpu(const N2Tex *t) {
    if (g_tex_s3tc && t->dxtfmt && t->dxt && t->dxtlen > 0) {
        RhiFormat fmt = (t->dxtfmt == 3) ? RHI_FORMAT_BC2
                      : (t->dxtfmt == 5) ? RHI_FORMAT_BC3 : RHI_FORMAT_BC1;
        int bpb = (t->dxtfmt == 1) ? 8 : 16;   /* S3TC block bytes (DXT3/DXT5 = 16) */
        RhiTexture id = rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D, fmt, t->w, t->h, RHI_TEXTURE_SAMPLED});
        if (!id.id) return id;
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
            if (rhi_texture_upload(id, 0, level, t->dxt + off, (size_t)sz) != RHI_OK) {
                rhi_texture_destroy(id); return (RhiTexture){0};
            }
            off += sz; level++;
            if (lw == 1 && lh == 1) { complete = 1; break; }
            if (lw > 1) lw /= 2; if (lh > 1) lh /= 2;
        }
        rhi_texture_set_sampler(id, tex_sampler(complete));
        if (complete) tex_track(id);   /* anisotropy needs a mip chain */
        return id;
    }
    return upload_tex(t);   /* portable fallback: CPU-decoded RGBA + mipmaps */
}

RhiTexture upload_tex(const N2Tex *t) {
    RhiTexture id = rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D,
        t->alpha ? RHI_FORMAT_RGBA8 : RHI_FORMAT_RGB8, t->w, t->h, RHI_TEXTURE_SAMPLED});
    int ok;
    if (!id.id) return id;
    if (t->alpha) {   /* interleave the decal-mask plane -> RGBA */
        unsigned char *px = (unsigned char *)malloc((size_t)t->w * t->h * 4);
        if (!px) { rhi_texture_destroy(id); return (RhiTexture){0}; }
        for (long p = 0; p < (long)t->w * t->h; p++) {
            px[p*4]=t->rgb[p*3]; px[p*4+1]=t->rgb[p*3+1];
            px[p*4+2]=t->rgb[p*3+2]; px[p*4+3]=t->alpha[p];
        }
        ok = rhi_texture_upload(id, 0, 0, px, 0) == RHI_OK;
        free(px);
    } else
        ok = rhi_texture_upload(id, 0, 0, t->rgb, 0) == RHI_OK;
    if (!ok || rhi_texture_generate_mips(id) != RHI_OK) {
        rhi_texture_destroy(id); return (RhiTexture){0};
    }
    rhi_texture_set_sampler(id, tex_sampler(1));
    tex_track(id);
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
void draw_text(GpuMesh *quad, const RProg *r, const char *s, float x, float y, float px, float py) {
    for (; *s; s++) {
        const unsigned char *g = glyph3x5(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col)) {
                    float M[16] = { px*0.85f,0,0,0, 0,py*0.85f,0,0, 0,0,1,0,
                                    x + col*px, y - row*py, 0, 1 };
                    ug2_copy(r->p->uMVP, M, 16);
                    render_draw_gpumesh(quad);
                }
        x += 4*px;   /* 3 columns + 1 gap */
    }
}
float text_width(const char *s, float px) { return (float)strlen(s) * 4 * px; }
