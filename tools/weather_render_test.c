/* Asset-free checks for wet batch isolation, rain cover, and real GL output. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "render.h"
#include "rhi_gl.h"
#include "legacy_draw.h"

static RainVertex drops[RAIN_MAX_DROPS*6],again[RAIN_MAX_DROPS*6];
static float verts[]={-100,-100,0,0,0, 100,-100,0,1,0,
                       100,100,0,1,1, -100,100,0,0,1};
static uint16_t indices[]={0,1,2,0,2,3};

static void geometry(void) {
    assert(!render_rain_vertices(1,0,2,1.6f,drops));
    assert(!render_rain_vertices(NAN,1,2,1.6f,drops));
    assert(!render_rain_vertices(1,1,2,0,drops));
    int counts[3];
    for(int q=0;q<3;q++) {
        srand(42);int expected=rand();srand(42);
        int n=render_rain_vertices(1,1,q,1.6f,drops);
        assert(rand()==expected);
        assert(n>0 && n<=RAIN_MAX_DROPS*6 && n%6==0);counts[q]=n;
        assert(n==render_rain_vertices(1,1,q,1.6f,again));
        assert(!memcmp(drops,again,n*sizeof *drops));
        for(int i=0;i<n;i++)assert(drops[i].pos[2]==0 && drops[i].fade>=0 && drops[i].fade<=1);
    }
    assert(counts[0]<counts[1] && counts[1]<counts[2]);
    int light=render_rain_vertices(1,.25f,2,1.6f,drops);assert(light>0 && light<counts[2]);
    int n=render_rain_vertices(1,1,1,1,again);
    assert(n==render_rain_vertices(1,1,1,2,drops));
    /* Horizontal width compensates for aspect; height stays unchanged. */
    assert(fabsf((again[1].pos[0]-again[0].pos[0])-2*(drops[1].pos[0]-drops[0].pos[0]))<1e-6f);
    assert(again[2].pos[1]==drops[2].pos[1]);
    assert(n==render_rain_vertices(1.1f,1,1,1,drops));
    assert(memcmp(drops,again,n*sizeof *drops));
    N2Mesh mesh={0};mesh.verts=verts;mesh.idx=indices;mesh.nverts=4;mesh.nidx=6;mesh.cat=N2_ROAD;
    N2Scene scene={0};scene.meshes=&mesh;scene.count=1;
    float bounds[1][4]={{-100,-100,100,100}},cam[]={-2,-3,3};
    assert(render_rain_exposed(&scene,bounds,cam));
    mesh.cat=N2_OTHER;for(int v=0;v<4;v++)verts[v*5+2]=20;
    assert(!render_rain_exposed(&scene,bounds,cam));
    mesh.cat=N2_SKY;assert(render_rain_exposed(&scene,bounds,cam));
    mesh.cat=N2_OTHER;cam[2]=25;assert(render_rain_exposed(&scene,bounds,cam));
    for(int v=0;v<4;v++)verts[v*5+2]=0;
    /* Conservative frustum: keep boxes that enclose/straddle the view, even
       with their centres outside it; reject boxes entirely behind/outside. */
    float m[16];mat_trans(0,0,0,m);
    N2Batch batch={0};
    for(int axis=0;axis<3;axis++)for(int sign=-1;sign<=1;sign+=2) {
        for(int a=0;a<3;a++){batch.bbox_min[a]=-.2f;batch.bbox_max[a]=.2f;}
        batch.bbox_min[axis]=sign>0?2:-3;batch.bbox_max[axis]=sign>0?3:-2;
        assert(!render_batch_in_view(&batch,m));
        if(sign>0)batch.bbox_min[axis]=.99f;else batch.bbox_max[axis]=-.99f;
        assert(render_batch_in_view(&batch,m));
    }
    for(int a=0;a<3;a++){batch.bbox_min[a]=-100;batch.bbox_max[a]=100;}
    assert(render_batch_in_view(&batch,m));
}

static void pixel(const RProg *r,GpuMesh *quad,float wet,float normal_z,unsigned char out[4]) {
    glClear(GL_COLOR_BUFFER_BIT);
    r->p->uWetness = wet;
    float normals[12];
    for(int i=0;i<4;i++){normals[i*3]=sqrtf(1-normal_z*normal_z);normals[i*3+1]=0;normals[i*3+2]=normal_z;}
    rhi_buffer_replace(quad->nbo,sizeof normals,normals);
    draw_gpumesh(quad);
    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,out);
}

static void paint_shine_test(const RProg *r,GpuMesh *quad) {
    N2LightSrc lamp={{.5f,3,4},2,30,0xff2040ffu};
    float camera[]={.5f,-2,4};unsigned char base[4],shine[4],other[4];
    r->p->uSpec = .25f;r->p->uGloss = 32;r->p->uClearcoat = .7f;
    render_wet_lights(r,&lamp,1,camera,1,2);
    r->p->uPaintLights = 0;pixel(r,quad,0,1,base);
    r->p->uPaintLights = 1;pixel(r,quad,0,1,shine);
    assert(shine[0]>base[0]+10 && shine[0]-base[0]>shine[1]-base[1]);
    /* View angle and source position move the glint; no constant brightening. */
    ug2_vec3(r->p->uCamPos, 3, -2, 4);pixel(r,quad,0,1,other);
    assert(other[0]+5<shine[0]);ug2_copy(r->p->uCamPos, camera, 3);
    lamp.pos[0]+=50;render_wet_lights(r,&lamp,1,camera,1,2);
    pixel(r,quad,0,1,other);assert(!memcmp(base,other,4));lamp.pos[0]-=50;
    render_wet_lights(r,&lamp,1,camera,1,2);
    r->p->uPaintLights = 0;pixel(r,quad,0,-1,base);
    r->p->uPaintLights = 1;pixel(r,quad,0,-1,other);
    assert(!memcmp(base,other,4)); /* lamp behind the surface cannot shine */
    /* Uncoated trim/rubber/glass and disabled lamps retain their old pixels. */
    r->p->uClearcoat = 0;pixel(r,quad,0,1,other);
    r->p->uPaintLights = 0;pixel(r,quad,0,1,base);
    assert(!memcmp(base,other,4));
    r->p->uClearcoat = .7f;pixel(r,quad,0,1,base);
    r->p->uPaintLights = 1;
    for(int mode=0;mode<2;mode++) {
        render_wet_lights(r,&lamp,1,camera,mode?0:1,mode?2:0);
        pixel(r,quad,0,1,other);assert(!memcmp(base,other,4));
    }
    r->p->uPaintLights = 0;r->p->uClearcoat = 0;r->p->uSpec = 0;
    puts("paint shine: source color/range, moving view, front-facing coat and off/low checks PASS");
}

static void wet_patch_test(const RProg *r,GpuMesh *quad) {
    /* Grazing view and black asphalt isolate the sky contribution: bright
       pixels indicate standing water rather than the uniform wet sheen. */
    float clip[]={2,0,0,0,0,2,0,0,0,0,1,0,-1,-1,0,1};
    float model[16];mat_trans(-100,-100,0,model);model[0]=model[5]=200;
    ug2_copy(r->p->uMVP, clip, 16);
    render_model(r,model);ug2_vec3(r->p->uCamPos, 0, -100000, 1);
    ug2_vec3(r->p->uColor, 0, 0, 0);ug2_vec3(r->p->uFogColor, 1, 1, 1);
    r->p->uAmbient = 0;r->p->uDiffuse = 0;
    r->p->uFogDensity = 0;r->p->uRainIntensity = 1;
    r->p->uWeatherTime = 0;
    unsigned char first[64*64*4],next[64*64*4],pixel_value[4];
    pixel(r,quad,1,1,pixel_value);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,first);
    r->p->uWeatherTime = 1.0f/60;
    pixel(r,quad,1,1,pixel_value);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,next);
    int pools=0,changes=0;
    for(int i=0;i<64*64;i++) {
        pools+=first[i*4]>66;
        changes+=memcmp(first+i*4,next+i*4,3)!=0;
    }
    printf("wet patches: %d/%d standing-water pixels, %d changed after 1/60 s\n",pools,64*64,changes);
    fflush(stdout);
    assert(pools>0 && pools<64*64/8);
    assert(!memcmp(first,next,sizeof first));
    /* A one-pixel camera pan must sample the same world patch, not regenerate it. */
    clip[12]-=2.0f/64;
    ug2_copy(r->p->uMVP, clip, 16);
    pixel(r,quad,1,1,pixel_value);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,next);
    for(int y=0;y<64;y++)for(int x=0;x<63;x++)
        assert(abs(next[(y*64+x)*4]-first[(y*64+x+1)*4])<=1);
    render_model(r,NULL);clip[12]=-1;
    ug2_copy(r->p->uMVP, clip, 16);
    ug2_vec3(r->p->uFogColor, 0, 0, 0);ug2_vec3(r->p->uColor, .3f, .3f, .3f);
    r->p->uAmbient = .5f;r->p->uDiffuse = .5f;
    r->p->uRainIntensity = 1;r->p->uWeatherTime = 0;r->p->uWetness = 0;
}

static void car_render_test(const RProg *r) {
    float eye[]={3,4,5},move[]={1,1,0},look[3];
    render_free_camera(eye,0,0,move,2,look);
    assert(fabsf(eye[0]-3-sqrtf(2))<1e-5f && fabsf(eye[1]-4+sqrtf(2))<1e-5f);
    assert(eye[2]==5 && look[0]==1 && look[2]==0);
    move[0]=move[1]=0;move[2]=-1;
    render_free_camera(eye,1,.5f,move,3,look);assert(eye[2]==2);
    /* Free flight has no player anchor or ground-height clamp. */
    render_free_camera(eye,1,.5f,move,10,look);assert(eye[2]==-8);
    float gp[]={5,-8,2},normal[]={-.2f,0,.9797959f},projection[16];
    mat_ground_shadow(gp,normal,projection);
    float point[]={6,-9,6},hit[3];
    for(int a=0;a<3;a++)hit[a]=projection[a]*point[0]+projection[4+a]*point[1]+projection[8+a]*point[2]+projection[12+a];
    float distance=0;for(int a=0;a<3;a++)distance+=(hit[a]-gp[a])*normal[a];
    assert(fabsf(distance-.025f)<1e-5f);
    float triangle[]={.15f,.15f,0,0,0, .85f,.15f,0,1,0, .15f,.85f,0,0,1};
    uint16_t twice[]={0,1,2,0,1,2};
    N2Mesh mesh={0};mesh.verts=triangle;mesh.idx=twice;mesh.nverts=3;mesh.nidx=6;
    N2Scene scene={0};scene.meshes=&mesh;scene.count=1;
    GpuMesh *gpu=upload_scene(&scene);CarShadow shadow={0};
    float identity[16],clip[16]={2,0,0,0,0,2,0,0,0,0,1,0,-1,-1,0,1};
    mat_trans(0,0,0,identity);float wheels[4][16];for(int w=0;w<4;w++)memcpy(wheels[w],identity,sizeof identity);
    float ground[]={0,0,0},up[]={0,0,1};
    glViewport(0,0,64,64);glClearColor(1,1,1,1);glStencilMask(0xff);glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glDisable(GL_DEPTH_TEST);
    assert(!render_car_shadow(r,&shadow,&scene,gpu,-1,NULL,NULL,identity,wheels,clip,ground,up,0));
    assert(render_car_shadow(r,&shadow,&scene,gpu,-1,NULL,NULL,identity,wheels,clip,ground,up,1)==1);
    unsigned char inside[4],outside[4];
    glReadPixels(20,20,1,1,GL_RGBA,GL_UNSIGNED_BYTE,inside);
    glReadPixels(48,48,1,1,GL_RGBA,GL_UNSIGNED_BYTE,outside);
    assert(inside[0]>=130 && inside[0]<=135 && outside[0]==255); /* single darkening, true triangle silhouette */
    assert(!glIsEnabled(GL_STENCIL_TEST) && !glIsEnabled(GL_BLEND));
    GLuint caster=rhi_gl_buffer_name(shadow.body);
    mesh.nidx=3;GpuMesh *replacement=upload_scene(&scene);
    glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    assert(render_car_shadow(r,&shadow,&scene,replacement,-1,NULL,NULL,identity,wheels,clip,ground,up,2)==1);
    assert(shadow.body_key==replacement[0].vbo.id && shadow.body_count==3);
    caster=rhi_gl_buffer_name(shadow.body);free_car_shadow(&shadow);assert(!glIsBuffer(caster));
    free_scene_gpu(replacement,1);free_scene_gpu(gpu,1);

    /* Six colored enclosing walls prove actual scene capture, all cube faces,
       complete publication, resizing, moving origin and Low disabling it. */
    N2Batch *batches=NULL;int count=0;N2Mesh walls[6]={{0}};
    float positions[6][20];RhiTexture textures[6];float bounds[6][4];
    for(int face=0;face<6;face++) {
        int axis=face/2,sign=face%2?-1:1;
        for(int v=0;v<4;v++) {
            float *p=positions[face]+v*5;memset(p,0,5*sizeof *p);
            p[axis]=sign*3; p[(axis+1)%3]=(v==0 || v==3)?-3:3;
            p[(axis+2)%3]=v<2?-3:3;p[3]=v==1 || v==2;p[4]=v>=2;
        }
        walls[face].verts=positions[face];walls[face].idx=indices;walls[face].nverts=4;walls[face].nidx=6;
        unsigned char rgb[]={0,0,0};rgb[axis]=sign>0?240:100;
        N2Tex tex={.w=1,.h=1,.rgb=rgb};textures[face]=upload_tex(&tex);
        bounds[face][0]=bounds[face][1]=-3;bounds[face][2]=bounds[face][3]=3;
    }
    scene.meshes=walls;scene.count=6;
    count=upload_world_batches(&scene,bounds,textures,(RhiTexture){0},&batches,NULL,NULL,NULL);
    CarEnvironment env={0};float origin[]={0,0,0};
    r->p->uAmbient = 1;r->p->uDiffuse = 0;r->p->uFogDensity = 0;
    for(int face=0;face<6;face++) {
        assert(render_car_environment(r,&env,batches,count,NULL,0,origin,1)>0);
        assert(env.ready==(face==5));assert(env.face==(face+1)%6);
    }
    assert(env.size==64);glActiveTexture(GL_TEXTURE5);glBindTexture(GL_TEXTURE_CUBE_MAP,rhi_gl_texture_name(env.cube[env.front]));
    unsigned char pixels[64*64*4];
    for(int face=0;face<6;face++) {
        glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        unsigned char *center=pixels+(32*64+32)*4;int axis=face/2;
        assert(center[axis]>=(face%2?90:220));assert(center[(axis+1)%3]==0);
    }
    glActiveTexture(GL_TEXTURE0);GpuMesh panel=make_quad();
    ug2_copy(r->p->uMVP, clip, 16);render_model(r,NULL);
    ug2_vec3(r->p->uCamPos, .5f, .5f, 3);ug2_vec3(r->p->uColor, 0, 0, 0);
    r->p->uEnv = 1;r->p->uSpec = 0;r->p->uUseTex = 0;
    glDisable(GL_DEPTH_TEST);glClear(GL_COLOR_BUFFER_BIT);draw_gpumesh(&panel);
    unsigned char reflected[4],fallback[4];glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,reflected);
    r->p->uEnvReady = 0;glClear(GL_COLOR_BUFFER_BIT);draw_gpumesh(&panel);
    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,fallback);
    assert(reflected[2]>fallback[2]+20 && reflected[2]>reflected[0]+20);
    r->p->uEnv = 0;rhi_buffer_destroy(panel.vbo);rhi_buffer_destroy(panel.nbo);rhi_buffer_destroy(panel.ibo);
    origin[0]=.5f;
    assert(render_car_environment(r,&env,batches,count,NULL,0,origin,2)>0);
    assert(env.size==128 && !env.ready && env.origin[0]==.5f);
    assert(!render_car_environment(r,&env,batches,count,NULL,0,origin,0));
    float ready;ug2_copy(&ready, &r->p->uEnvReady, 1);assert(ready==0);
    CarEnvironment slow={0};
    for(int face=0;face<6;face++)assert(render_car_environment_rate(r,&slow,batches,count,NULL,0,origin,0,1)>0);
    assert(slow.ready && slow.size==64);
    assert(!render_car_environment_rate(r,&slow,batches,count,NULL,0,origin,0,1));
    assert(!render_car_environment_rate(r,&slow,batches,count,NULL,0,origin,0,1));
    assert(render_car_environment_rate(r,&slow,batches,count,NULL,0,origin,0,1)>0);
    for(int i=0;i<3;i++)assert(render_car_environment_rate(r,&slow,batches,count,NULL,0,origin,1,3)>0);
    assert(slow.size==128);free_car_environment(&slow);
    GLuint cube=rhi_gl_texture_name(env.cube[0]);free_car_environment(&env);assert(cube && !glIsTexture(cube));
    render_batch_array_free(&batches,&count);for(int i=0;i<6;i++)rhi_texture_destroy(textures[i]);
    glActiveTexture(GL_TEXTURE0);r->p->uEnvReady = 0;
    ug2_copy(r->p->uMVP, clip, 16);r->p->uAmbient = .5f;r->p->uDiffuse = .5f;
    glClearColor(0,0,0,0);assert(glGetError()==GL_NO_ERROR);
    puts("car rendering: free flight, sloped mesh shadow/stencil and six-face scene reflection PASS");
}

static void reflection_test(const RProg *r) {
    float ground[]={-10,0,0,0,0, 10,0,0,1,0, 10,20,0,1,1, -10,20,0,0,1};
    float wall[]={-3,8,0,0,0, 3,8,0,1,0, 3,8,5,1,1, -3,8,5,0,1};
    N2Mesh meshes[2]={{0},{0}};
    for(int i=0;i<2;i++){meshes[i].verts=i?wall:ground;meshes[i].idx=indices;meshes[i].nverts=4;meshes[i].nidx=6;meshes[i].cat=i?N2_OTHER:N2_ROAD;}
    N2Scene scene={0};scene.meshes=meshes;scene.count=2;
    float bounds[2][4]={{-10,0,10,20},{-3,8,3,8}};
    RhiTexture tex[2]={{0},{0}};N2Batch *batches=NULL;int map[2];
    int count=upload_world_batches(&scene,bounds,tex,(RhiTexture){0},&batches,NULL,map,NULL);
    assert(count==2);
    float cam[]={0,-4,3},look[]={0,1,-.15f},p[16],v[16],mvp[16];
    mat_persp(1.1f,1,.1f,60,p);mat_lookat(cam,look,v);mat_mul(p,v,mvp);
    ug2_copy(r->p->uMVP, mvp, 16);render_model(r,NULL);
    ug2_copy(r->p->uCamPos, cam, 3);r->p->uFogDensity = 0;
    r->p->uUnlit = 1;r->p->uAlpha = 1;r->p->uSoft = 0;
    r->p->uRainIntensity = 0;r->p->uWetness = 0;
    glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);glClearDepth(1);
    glViewport(0,0,64,64);glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    ug2_vec3(r->p->uColor, .12f, .12f, .12f);draw_batch(&batches[map[0]]);
    ug2_vec3(r->p->uColor, 1, .02f, 1);draw_batch(&batches[map[1]]);
    unsigned char before[64*64*4],after[64*64*4];
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,before);
    RoadReflections refl={0};
    assert(!render_road_reflections(r,&refl,batches,count,cam,mvp,p,0,2));
    assert(!refl.color.id && !refl.depth.id);
    assert(!render_road_reflections(r,&refl,batches,count,cam,mvp,p,1,0));
    glActiveTexture(GL_TEXTURE5);
    int draws=render_road_reflections(r,&refl,batches,count,cam,mvp,p,1,2);
    assert(draws==1 && !refl.failed && refl.width==64 && refl.height==64);
    float unlit;ug2_copy(&unlit, &r->p->uUnlit, 1);assert(unlit==1);
    GLint state;glGetIntegerv(GL_ACTIVE_TEXTURE,&state);assert(state==GL_TEXTURE5);
    glGetIntegerv(GL_DEPTH_FUNC,&state);assert(state==GL_LESS);
    assert(!glIsEnabled(GL_BLEND));GLboolean mask;glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);assert(mask);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,after);
    int hits=0;
    for(int i=0;i<64*64;i++) {
        if(before[i*4]>200)assert(!memcmp(before+i*4,after+i*4,3)); /* wall unchanged */
        if(before[i*4]>=29 && before[i*4]<=32 && after[i*4]>before[i*4]+3 && after[i*4+2]>before[i*4+2]+3)hits++;
    }
    printf("road reflection: %d colored receiver pixels\n",hits);assert(hits>10);
    /* Rain belongs to the lens overlay; the same reflected scene must not
       tear into a new pattern as the weather clock advances. */
    glActiveTexture(GL_TEXTURE0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    ug2_vec3(r->p->uColor, .12f, .12f, .12f);draw_batch(&batches[map[0]]);
    ug2_vec3(r->p->uColor, 1, .02f, 1);draw_batch(&batches[map[1]]);
    r->p->uWeatherTime = 1.0f/60;
    assert(render_road_reflections(r,&refl,batches,count,cam,mvp,p,1,2)==1);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,before);
    assert(!memcmp(before,after,sizeof before));
    /* A solid wall's reflection must stay continuous as the camera moves.
       Analytic ray/plane intersections select pixels well inside its edges. */
    for(int frame=0;frame<8;frame++) {
        int quality=1+frame%2;
        cam[0]=frame*.07f;cam[1]=-4+frame*.15f;
        mat_lookat(cam,look,v);mat_mul(p,v,mvp);
        ug2_copy(r->p->uMVP, mvp, 16);ug2_copy(r->p->uCamPos, cam, 3);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        ug2_vec3(r->p->uColor, .12f, .12f, .12f);draw_batch(&batches[map[0]]);
        ug2_vec3(r->p->uColor, 1, .02f, 1);draw_batch(&batches[map[1]]);
        glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,before);
        assert(render_road_reflections(r,&refl,batches,count,cam,mvp,p,1,quality)==1);
        glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,after);
        int expected=0,found=0;
        float length=sqrtf(1+.15f*.15f),ly=1/length,lz=-.15f/length;
        for(int y=0;y<64;y++)for(int x=0;x<64;x++) {
            int i=y*64+x;if(before[i*4]<29 || before[i*4]>32)continue;
            float dx=(2*(x+.5f)/64-1)/p[0],up=(2*(y+.5f)/64-1)/p[5];
            float dy=ly-lz*up,dz=lz+ly*up;if(dz>=0)continue;
            float t=-cam[2]/dz,gx=cam[0]+dx*t,gy=cam[1]+dy*t;
            float hit=(8-gy)/dy,hx=gx+dx*hit,hz=-dz*hit;
            if(hit<=0 || fabsf(hx)>2.5f || hz<.5f || hz>4.5f)continue;
            expected++;found+=after[i*4]>before[i*4]+3 && after[i*4+2]>before[i*4+2]+3;
        }
        printf("moving reflection %d (quality %d): %d/%d interior pixels\n",frame,quality,found,expected);
        fflush(stdout);assert(expected>20 && found>=expected*.95f);
    }
    /* Remove the wall in the next frame. Empty sky must not become a hit,
       and the old wall must not remain in the copied scene textures. */
    glActiveTexture(GL_TEXTURE0);glClearColor(0,1,0,1);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    ug2_vec3(r->p->uColor, .12f, .12f, .12f);draw_batch(&batches[map[0]]);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,before);
    assert(render_road_reflections(r,&refl,batches,count,cam,mvp,p,1,1)==1);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,after);
    assert(!memcmp(before,after,sizeof before));
    /* Resize without stale source storage; repeated calls include no feedback
       because the production caller redraws the scene before each capture. */
    glViewport(0,0,32,32);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    /* The reflection pipeline must not leak into the caller's raster state. */
    glColorMask(1,1,1,0);glEnable(GL_STENCIL_TEST);glStencilFunc(GL_ALWAYS,0,0xff);glEnable(GL_POLYGON_OFFSET_FILL);
    assert(render_road_reflections(r,&refl,batches,count,cam,mvp,p,1,1)==1);
    assert(refl.width==32 && refl.height==32);
    GLboolean cm[4];glGetBooleanv(GL_COLOR_WRITEMASK,cm);
    assert(cm[0] && cm[1] && cm[2] && !cm[3] && glIsEnabled(GL_STENCIL_TEST) && glIsEnabled(GL_POLYGON_OFFSET_FILL));
    glColorMask(1,1,1,1);glDisable(GL_STENCIL_TEST);glDisable(GL_POLYGON_OFFSET_FILL);
    assert(glGetError()==GL_NO_ERROR);
    GLuint color=rhi_gl_texture_name(refl.color),depth=rhi_gl_texture_name(refl.depth);free_road_reflections(&refl);
    assert(!glIsTexture(color) && !glIsTexture(depth) && !refl.width);
    render_batch_array_free(&batches,&count);
    glActiveTexture(GL_TEXTURE0);glViewport(0,0,64,64);
}

static void collision_wall_render_test(const RProg *r) {
    const float faces[]={-.8f,-.8f,0,.8f,-.8f,0,0,.8f,0},color[]={.1f,.85f,1};
    const float identity[]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const float clip[]={2,0,0,0,0,2,0,0,0,0,1,0,-1,-1,0,1};
    RhiBuffer vbo={0};unsigned char pixels[64*64*4];
    for(int through=0;through<2;through++) {
        glViewport(0,0,64,64);glClearColor(0,0,0,0);glClearDepth(0);
        glDepthMask(GL_TRUE);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);glEnable(GL_CULL_FACE);glDisable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE,GL_ZERO,GL_ZERO,GL_ONE);
        r->p->uUnlit = 0;r->p->uSoft = 0;r->p->uAlpha = .6f;
        ug2_vec3(r->p->uColor, .2f, .3f, .4f);ug2_copy(r->p->uMVP, clip, 16);
        assert(render_collision_walls(r,&vbo,faces,1,color,identity,through)==2);
        glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        int lit=0;for(int i=0;i<64*64;i++)lit+=pixels[i*4+2]>0;
        assert(through?lit>100:lit==0);
        assert(glIsEnabled(GL_DEPTH_TEST) && glIsEnabled(GL_CULL_FACE) && !glIsEnabled(GL_BLEND));
        GLboolean mask;glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);assert(mask);
        float matrix[16],value,restored[3];ug2_copy(matrix, r->p->uMVP, 16);
        assert(!memcmp(matrix,clip,sizeof matrix));
        ug2_copy(&value, &r->p->uUnlit, 1);assert(value==0);
        ug2_copy(&value, &r->p->uAlpha, 1);assert(value==.6f);
        ug2_copy(restored, r->p->uColor, 3);assert(restored[0]==.2f && restored[1]==.3f && restored[2]==.4f);
        GLint blend;glGetIntegerv(GL_BLEND_SRC_RGB,&blend);assert(blend==GL_ONE);
        glGetIntegerv(GL_BLEND_DST_ALPHA,&blend);assert(blend==GL_ONE);
        assert(glGetError()==GL_NO_ERROR);
    }
    glDepthFunc(GL_GREATER);glEnable(GL_STENCIL_TEST);glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE);
    assert(render_collision_walls(r,&vbo,faces,1,color,identity,0)==2);
    GLint depth_func;GLboolean cm[4];
    glGetIntegerv(GL_DEPTH_FUNC,&depth_func);glGetBooleanv(GL_COLOR_WRITEMASK,cm);
    assert(depth_func==GL_GREATER && glIsEnabled(GL_STENCIL_TEST));
    assert(!cm[0] && cm[1] && !cm[2] && cm[3]);
    glDepthFunc(GL_LESS);glDisable(GL_STENCIL_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    assert(!render_collision_walls(r,&vbo,faces,0,color,identity,1));
    rhi_buffer_destroy(vbo);glClearDepth(1);
    puts("collision drawing: depth/through views, visible faces/edges and preserved GL state PASS");
}

/* Quality changes and car/district resets must not consume a fixed pipeline budget. */
static void environment_recreation_test(const RProg *r) {
    CarEnvironment env={0};N2Batch hidden={0};hidden.unresolved=1;float pos[3]={0};
    for(int i=0;i<40;i++) {
        assert(render_car_environment(r,&env,&hidden,1,NULL,0,pos,1+(i&1))>=0);
        assert(!env.failed && env.opaque.id && env.sky.id && env.add.id);
        if(i%3==0)free_car_environment(&env);
    }
    free_car_environment(&env);assert(glGetError()==GL_NO_ERROR);
}

int main(void) {
    geometry();
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_Window *win=SDL_CreateWindow("weather test",0,0,64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    assert(win);SDL_GLContext ctx=SDL_GL_CreateContext(win);assert(ctx);
    RProg r=render_program();glUseProgram(r.prog);GLint linked=0;glGetProgramiv(r.prog,GL_LINK_STATUS,&linked);assert(linked);
    N2Mesh meshes[2]={{0},{0}};
    for(int i=0;i<2;i++){meshes[i].verts=verts;meshes[i].idx=indices;meshes[i].nverts=4;meshes[i].nidx=6;}
    meshes[0].cat=N2_ROAD;meshes[1].cat=N2_OTHER;
    N2Scene scene={0};scene.meshes=meshes;scene.count=2;
    float bounds[2][4]={{-100,-100,100,100},{-100,-100,100,100}};
    RhiTexture textures[2]={{0},{0}};N2Batch *batches=NULL;int mapping[2];
    int count=upload_world_batches(&scene,bounds,textures,(RhiTexture){0},&batches,NULL,mapping,NULL);
    assert(count==2 && mapping[0]!=mapping[1]);
    assert(batches[mapping[0]].wettable && !batches[mapping[1]].wettable);
    render_batch_array_free(&batches,&count);

    GpuMesh quad=make_quad();
    float clip[]={2,0,0,0,0,2,0,0,0,0,1,0,-1,-1,0,1};
    glViewport(0,0,64,64);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
    ug2_copy(r.p->uMVP, clip, 16);
    ug2_vec3(r.p->uColor, .3f, .3f, .3f);r.p->uAmbient = .5f;r.p->uDiffuse = .5f;
    ug2_vec3(r.p->uCamPos, .5f, -2, 4);
    unsigned char dry[4],wet[4],wall[4],wall_wet[4];
    pixel(&r,&quad,0,1,dry);pixel(&r,&quad,1,1,wet);
    assert(wet[0]+10<dry[0]);
    pixel(&r,&quad,0,0,wall);pixel(&r,&quad,1,0,wall_wet);
    assert(!memcmp(wall,wall_wet,4));
    /* Authored lamp color and enable state reach only the wet material. */
    N2LightSrc lamp={ {.5f,3,4}, 2,30,0xff2040ffu };
    float camera[]={.5f,-2,4},lamp_data[4];
    render_wet_lights(&r,&lamp,1,camera,1,2);
    ug2_copy(lamp_data, r.p->uWetLightPos[0], 4);
    assert(lamp_data[0]==lamp.pos[0] && lamp_data[2]==4 && lamp_data[3]==30);
    ug2_copy(lamp_data, r.p->uWetLightColor[0], 4);
    assert(lamp_data[0]==1 && lamp_data[1]<.3f && lamp_data[3]>0);
    unsigned char highlight[4];pixel(&r,&quad,1,1,highlight);
    assert(highlight[0]>wet[0]);
    render_wet_lights(&r,&lamp,1,camera,1,0);
    ug2_copy(lamp_data, r.p->uWetLightColor[0], 4);assert(lamp_data[3]==0);
    r.p->uWetness = 0;r.p->uRainIntensity = 1;
    r.p->uWeatherTime = 29;pixel(&r,&quad,0,1,wet);
    assert(!memcmp(dry,wet,4)); /* dry path unaffected by weather time */
    paint_shine_test(&r,&quad);
    wet_patch_test(&r,&quad);

    RhiBuffer rain={0};float cam[]={0,-5,3},look[]={0,1,-.1f},p[16],v[16],mvp[16];
    mat_persp(1.4f,1,.1f,60,p);mat_lookat(cam,look,v);mat_mul(p,v,mvp);
    unsigned char image[64*64*4],visible[64*64*4];int lit=0;
    for(int blocked=0;blocked<2;blocked++) {
        glClearColor(0,0,0,0);glDepthMask(GL_TRUE);glClearDepth(blocked?0:1);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);glEnable(GL_CULL_FACE);glDisable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE,GL_ZERO,GL_ZERO,GL_ONE);
        assert(render_rain(&r,&rain,2,1,2,1)==1);
        assert(!glIsEnabled(GL_DEPTH_TEST) && !glIsEnabled(GL_BLEND) && glIsEnabled(GL_CULL_FACE));
        GLboolean mask;glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);assert(mask);
        float value;ug2_copy(&value, &r.p->uSoft, 1);assert(value==0);
        ug2_copy(&value, &r.p->uUnlit, 1);assert(value==0);
        float saved[16];ug2_copy(saved, r.p->uMVP, 16);assert(!memcmp(saved,clip,sizeof clip));
        GLint blend;glGetIntegerv(GL_BLEND_SRC_RGB,&blend);assert(blend==GL_ONE);
        glGetIntegerv(GL_BLEND_DST_ALPHA,&blend);assert(blend==GL_ONE);
        glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,image);
        if(blocked)assert(!memcmp(visible,image,sizeof image));
        else {memcpy(visible,image,sizeof image);for(int i=0;i<64*64;i++)lit+=image[i*4]>0;}
        assert(glGetError()==GL_NO_ERROR);
    }
    assert(lit>10);
    glEnable(GL_STENCIL_TEST);glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE);
    assert(render_rain(&r,&rain,2,1,2,1)==1);
    GLboolean cm[4];glGetBooleanv(GL_COLOR_WRITEMASK,cm);
    assert(glIsEnabled(GL_STENCIL_TEST) && !cm[0] && cm[1] && !cm[2] && cm[3]);
    glDisable(GL_STENCIL_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    assert(!render_rain(&r,&rain,2,0,2,1));
    rhi_buffer_destroy(rain);rhi_buffer_destroy(quad.vbo);rhi_buffer_destroy(quad.nbo);rhi_buffer_destroy(quad.ibo);
    reflection_test(&r);
    car_render_test(&r);
    collision_wall_render_test(&r);
    environment_recreation_test(&r);
    rhi_shader_destroy(r.shader);SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(win);SDL_Quit();
    puts("weather: screen droplets/aspect, camera cover, conservative culling, wet materials/reflections and GL state passed");
    return 0;
}
