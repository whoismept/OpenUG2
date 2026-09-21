/* Real GL shader regression, no retail assets. Run: make headlight-render-test. */
#include "render.h"
#include <assert.h>
#include <stdio.h>

static unsigned char pixels[128*64*4];
static HeadlightShadows shadows;
static N2Batch *casters;
static int shadow_enabled, caster_count, shadow_draws;
static void wall(N2Batch *b,float x,float ymin) {
    BatchedVertex v[4]={0};
    const float pos[4][3]={{x,ymin,0},{x,3,0},{x,3,3},{x,ymin,3}};
    const uint16_t idx[]={0,1,2,0,2,3};
    for(int i=0;i<4;i++)memcpy(v[i].pos,pos[i],sizeof pos[i]);
    if(!b->vbo)glGenBuffers(1,&b->vbo);
    if(!b->ibo)glGenBuffers(1,&b->ibo);
    glBindBuffer(GL_ARRAY_BUFFER,b->vbo);glBufferData(GL_ARRAY_BUFFER,sizeof v,v,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,b->ibo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof idx,idx,GL_STATIC_DRAW);
    b->index_count=6;
    b->bbox_min[0]=b->bbox_max[0]=x;b->bbox_min[1]=ymin;b->bbox_max[1]=3;
    b->bbox_min[2]=0;b->bbox_max[2]=3;
}
static int sample(const RProg *r,GpuMesh *road,const float model[16],int high,float gain,int x) {
    const float anchors[4][4]={{0,.65f,.7f,1},{0,-.65f,.7f,1},{0},{0}};
    render_headlights(r,model,anchors,high,high?1:4,high?85:35,gain);
    if(shadow_enabled) {
        shadow_draws=render_headlight_shadows(r,&shadows,casters,caster_count);
        assert(shadow_draws>=0);
        GLint viewport[4],program,active;glGetIntegerv(GL_VIEWPORT,viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        assert(viewport[2]==128 && viewport[3]==64 && program==(GLint)r->prog && active==GL_TEXTURE0);
    }
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    draw_gpumesh(road);glFinish();
    glReadPixels(0,0,128,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    return pixels[(32*128+x)*4];
}
int main(void) {
    /* Window and lamp-cover order uses each slice's indices, not the shared
       pool (which also contains an unrelated far vertex). Equal depths are stable. */
    float glass_verts[]={0,0,0,0,0, 0,0,2,0,0, 0,0,999,0,0};
    uint16_t glass_idx[]={0,0,0,1,1,1};
    N2Mesh panes[5]={0};
    for(int i=0;i<5;i++) {
        panes[i].verts=glass_verts;panes[i].nverts=3;
        panes[i].idx=glass_idx+(i?3:0);panes[i].nidx=3;
        panes[i].cat=N2_CAR_GLASS;
    }
    panes[1].cat=N2_CAR_LIGHT;panes[1].car_material=N2_MAT_HEADLIGHTGLASS;
    panes[2].cat=N2_CAR_BODY;panes[4].car_mount=N2_MOUNT_WHEEL;
    N2Scene glass_scene={.meshes=panes,.count=5};
    float view[16]={0};view[11]=1;view[15]=10;int order[5];
    assert(render_car_glass_order(&glass_scene,view,order)==3);
    assert(order[0]==1 && order[1]==3 && order[2]==0);
    view[11]=-1;
    assert(render_car_glass_order(&glass_scene,view,order)==3);
    assert(order[0]==0 && order[1]==1 && order[2]==3);
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_Window *window=SDL_CreateWindow("Headlight shader test",0,0,128,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    assert(window);SDL_GLContext context=SDL_GL_CreateContext(window);assert(context);
    RProg r=render_program();GLint linked=0;glGetProgramiv(r.prog,GL_LINK_STATUS,&linked);assert(linked);
    unsigned char white[]={255,255,255};N2Tex tex={.w=1,.h=1,.rgb=white};
    GLuint texture=upload_tex(&tex); /* shader sampler remains valid even for flat colour */
    float verts[]={0,-20,0,0,0,100,-20,0,1,0,100,20,0,1,1,0,20,0,0,1};
    uint16_t idx[]={0,1,2,0,2,3};
    N2Mesh mesh={.verts=verts,.idx=idx,.nverts=4,.nidx=6};N2Scene scene={.meshes=&mesh,.count=1};
    GpuMesh *road=upload_scene(&scene);assert(road);
    float projection[16]={.02f,0,0,0,0,.05f,0,0,0,0,.01f,0,-1,0,0,1};
    float model[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    glViewport(0,0,128,64);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    glUniformMatrix4fv(r.uMVP,1,GL_FALSE,projection);
    glUniform3f(r.uColor,.3f,.3f,.3f);glUniform3f(r.uCamPos,0,0,10);
    glUniform1f(r.uAmbient,.1f);glUniform1f(r.uDiffuse,0);
    int off=sample(&r,road,model,0,0,25);
    int low=sample(&r,road,model,0,1,25);
    int low_far=sample(&r,road,model,0,1,64);
    int high_far=sample(&r,road,model,1,1,64);
    assert(low>off+5 && low_far==off && high_far>low_far);
    model[0]=model[5]=-1;
    assert(sample(&r,road,model,1,1,25)==off); /* heading turns illumination away */
    model[0]=model[5]=1;
    N2Batch blocker={0};wall(&blocker,8,-3);
    casters=&blocker;caster_count=1;shadow_enabled=1;
    int blocked=sample(&r,road,model,0,1,25);
    assert(blocked==off && shadow_draws==2);
    assert(sample(&r,road,model,1,1,64)==off); /* high beam also stops */
    wall(&blocker,8,0);
    int one_lamp=sample(&r,road,model,0,1,25);
    assert(one_lamp>off && one_lamp<low); /* independently shadowed lamps */
    wall(&blocker,8,-3);
    blocker.drawmode=N2_DRAW_CUTOUT;
    unsigned char alpha=0;tex.alpha=&alpha;blocker.tex=upload_tex(&tex);
    assert(sample(&r,road,model,0,1,25)==low);
    glDeleteTextures(1,&blocker.tex);alpha=255;blocker.tex=upload_tex(&tex);
    assert(sample(&r,road,model,0,1,25)==off);
    blocker.drawmode=N2_DRAW_BLEND;
    assert(sample(&r,road,model,0,1,25)==low && shadow_draws==0);
    blocker.drawmode=N2_DRAW_ADD;
    assert(sample(&r,road,model,0,1,25)==low && shadow_draws==0);
    blocker.drawmode=N2_DRAW_OPAQUE;
    wall(&blocker,-8,-3); /* behind lamps: frustum rejects it */
    assert(sample(&r,road,model,0,1,25)==low && shadow_draws==0);
    wall(&blocker,100,-3);
    assert(sample(&r,road,model,0,1,25)==low && shadow_draws==0);
    wall(&blocker,8,-3);
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,128,64);glEnable(GL_DITHER);
    glDepthMask(GL_FALSE);glClearDepth(.25);
    assert(sample(&r,road,model,0,1,25)==off);
    GLfloat depth_clear;GLboolean depth_mask;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE,&depth_clear);glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_mask);
    assert(depth_clear==.25f && !depth_mask && glIsEnabled(GL_DITHER) && glIsEnabled(GL_SCISSOR_TEST));
    glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);glClearDepth(1);
    caster_count=0;
    assert(sample(&r,road,model,0,1,25)==low); /* no stale occluder after reload */
    assert(sample(&r,road,model,0,0,25)==off && shadow_draws==0);
    /* The road also casts: a surface must not shadow itself. */
    BatchedVertex surface[4]={0};
    for(int v=0;v<4;v++)memcpy(surface[v].pos,verts+v*5,3*sizeof(float));
    glBindBuffer(GL_ARRAY_BUFFER,blocker.vbo);glBufferData(GL_ARRAY_BUFFER,sizeof surface,surface,GL_STATIC_DRAW);
    blocker.bbox_min[0]=0;blocker.bbox_max[0]=100;
    blocker.bbox_min[1]=-20;blocker.bbox_max[1]=20;blocker.bbox_max[2]=0;
    caster_count=1;
    assert(sample(&r,road,model,0,1,25)==low);
    assert(sample(&r,road,model,1,1,64)==high_far);
    shadow_enabled=0;
    glDeleteTextures(1,&blocker.tex);glBindTexture(GL_TEXTURE_2D,texture);
    glDeleteBuffers(1,&blocker.vbo);glDeleteBuffers(1,&blocker.ibo);
    printf("PASS: world shadows blocked=%d single lamp=%d clear=%d; cutouts, culling, refresh and GL state\n",blocked,one_lamp,low);
    free_scene_gpu(road,1);
    /* Underside above lamp height: low-beam cutoff must reject it. */
    for(int v=0;v<4;v++)verts[v*5+2]=1.7f;
    const uint16_t down[]={0,2,1,0,3,2};mesh.idx=(uint16_t *)down;
    road=upload_scene(&scene);assert(road);
    assert(sample(&r,road,model,0,1,25)==off);
    assert(sample(&r,road,model,1,1,25)>off);
    assert(glGetError()==GL_NO_ERROR);
    printf("PASS: low road=%d off=%d; far low=%d high=%d; heading and upper cutoff\n",low,off,low_far,high_far);
    /* Two-sided car glass must not become opaque when seen from inside.
       Same pane and camera, opposite index winding; inspect the shader alpha. */
    glUniform1f(r.uHeadGain,0);glUniform1f(r.uFresnel,1);glUniform1f(r.uAlpha,.2f);
    glUniform1f(r.uSpec,.5f);glUniform1f(r.uGloss,40);glUniform1f(r.uEnv,.4f);
    glUniform1f(r.uFogDensity,0);glUniform3f(r.uLight,0,0,1);
    glUniform3f(r.uCamPos,20,0,10);glUniform3f(r.uColor,.05f,.05f,.05f);
    glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);
    unsigned char glass[2][4];
    for(int side=0;side<2;side++) {
        for(int v=0;v<4;v++)verts[v*5+2]=0;
        mesh.idx=side?(uint16_t *)down:idx;
        GpuMesh *pane=upload_scene(&scene);assert(pane);
        glClear(GL_COLOR_BUFFER_BIT);draw_gpumesh(pane);glFinish();
        glReadPixels(25,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,glass[side]);
        free_scene_gpu(pane,1);
    }
    printf("glass alpha front=%u back=%u\n",glass[0][3],glass[1][3]);fflush(stdout);
    assert(glass[0][3]>50 && glass[0][3]<240 && abs(glass[0][3]-glass[1][3])<=1);
    for(int c=0;c<3;c++)assert(abs(glass[0][c]-glass[1][c])<=1);
    assert(glGetError()==GL_NO_ERROR);
    puts("PASS: car glass tint/reflection/opacity agree from both sides");
    free_headlight_shadows(&shadows);
    free_scene_gpu(road,1);glDeleteTextures(1,&texture);glDeleteProgram(r.prog);
    SDL_GL_DeleteContext(context);SDL_DestroyWindow(window);SDL_Quit();return 0;
}
