/* Asset-free GL regression for the production wheel material draw. */
#include "render.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    float verts[]={0,0,0,0,0, 0,0,2,0,0};uint16_t idx[]={0,0,0,1,1,1};
    N2Mesh slices[2]={{.verts=verts,.nverts=2,.idx=idx,.nidx=3},
                      {.verts=verts,.nverts=2,.idx=idx+3,.nidx=3}};
    N2Scene scene={.meshes=slices,.count=2};float wmvp[4][16]={{0}};int order[8];
    for(int k=0;k<4;k++){wmvp[k][11]=1;wmvp[k][15]=10-k;}
    render_wheel_order(&scene,wmvp,order);
    const int expected[]={1,3,0,5,2,7,4,6};
    assert(!memcmp(order,expected,sizeof order)); /* own ranges, all hubs, stable ties */
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_Window *w=SDL_CreateWindow("wheel-render-test",0,0,32,32,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    assert(w);SDL_GLContext ctx=SDL_GL_CreateContext(w);assert(ctx);
    RProg r=render_program();GpuMesh q=make_quad();
    float m[16]={2,0,0,0,0,2,0,0,0,0,1,0,-1,-1,0,1};
    glUniformMatrix4fv(r.uMVP,1,GL_FALSE,m);
    glUniform1f(r.uFogDensity,0);glUniform1f(r.uAmbient,1.0f/1.35f);
    glUniform1f(r.uDiffuse,0);glUniform1f(r.uSpec,0);glUniform1f(r.uEnv,0);
    glUniform1f(r.uClearcoat,0);glUniform1f(r.uRimTint,0);
    glUniform3f(r.uColor,0,1,0);glUniform3f(r.uCamPos,0,0,3);
    unsigned char pixels[]={255,0,0,0, 255,0,0,127, 255,0,0,128, 255,0,0,255};
    GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glViewport(0,0,32,32);glDepthFunc(GL_LESS);
    for(int test=0;test<5;test++) {
        int blocked=test==4;
        int mode=test==0||blocked?N2_DRAW_CUTOUT:test==1?N2_DRAW_BLEND:test==2?N2_DRAW_OPAQUE:3;
        int missing=mode==3;
        glDepthMask(GL_TRUE);glClearDepth(blocked?0:1);glClearColor(0,0,1,1);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        int incoming=test&1;
        if(incoming){glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDisable(GL_BLEND);}
        else {glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glEnable(GL_BLEND);}
        glBlendFuncSeparate(GL_ONE,GL_ZERO,GL_ZERO,GL_ONE);
        glBindTexture(GL_TEXTURE_2D,tex);
        glUniform1f(r.uUseTex,.25f);glUniform1f(r.uAlpha,.4f);glUniform1f(r.uDecal,.8f);
        glUniform1f(r.uAlphaTest,.75f);glUniform1f(r.uTextureAlpha,.5f);
        render_wheel_mesh(&r,&q,missing?0:tex,missing?N2_DRAW_CUTOUT:mode);
        unsigned char a[4],b[4],c[4],below[4];float za,zc;
        glReadPixels(4,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,a);
        glReadPixels(12,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,below);
        glReadPixels(20,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,b);
        glReadPixels(28,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,c);
        glReadPixels(4,16,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&za);
        glReadPixels(27,16,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&zc);
        if(blocked)assert(a[2]>245 && c[2]>245 && a[0]<5 && c[0]<5);
        else if(missing) assert(a[1]>245 && c[1]>245 && a[0]<5);
        else if(mode==N2_DRAW_OPAQUE) assert(a[0]>245 && b[0]>245 && c[0]>245);
        else {
            assert(a[0]<5 && a[2]>245); /* transparent corner leaves background */
            assert(c[0]>245 && c[2]<5);
            if(mode==N2_DRAW_CUTOUT) {assert(b[0]>245 && b[2]<5);assert(below[0]<5 && below[2]>245);}
            else assert(b[0]>120 && b[0]<136 && b[2]>120 && b[2]<136);
        }
        if(blocked)assert(za<.01f && zc<.01f);
        else if(mode==N2_DRAW_BLEND) assert(za>.99f && zc>.99f);
        else {assert(zc>.49f && zc<.51f);if(mode==N2_DRAW_CUTOUT)assert(za>.99f);}
        assert(!!glIsEnabled(GL_DEPTH_TEST)==incoming && !!glIsEnabled(GL_BLEND)==!incoming);
        GLboolean mask;glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);assert(!!mask==incoming);
        GLint v;glGetIntegerv(GL_BLEND_SRC_RGB,&v);assert(v==GL_ONE);
        glGetIntegerv(GL_BLEND_DST_RGB,&v);assert(v==GL_ZERO);
        glGetIntegerv(GL_BLEND_SRC_ALPHA,&v);assert(v==GL_ZERO);
        glGetIntegerv(GL_BLEND_DST_ALPHA,&v);assert(v==GL_ONE);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&v);assert((GLuint)v==tex);
        float f;glGetUniformfv(r.prog,r.uUseTex,&f);assert(f==.25f);
        glGetUniformfv(r.prog,r.uAlpha,&f);assert(f==.4f);
        glGetUniformfv(r.prog,r.uDecal,&f);assert(f==.8f);
        glGetUniformfv(r.prog,r.uAlphaTest,&f);assert(f==.75f);
        glGetUniformfv(r.prog,r.uTextureAlpha,&f);assert(f==.5f);
        assert(glGetError()==GL_NO_ERROR);
    }
    /* Scene contract: opaque opponents first, then blended player wheels.
       Draw an actual farther green surface, not just a prefilled depth buffer. */
    glDepthMask(GL_TRUE);glClearDepth(1);glClearColor(0,0,1,1);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    m[14]=.5f;glUniformMatrix4fv(r.uMVP,1,GL_FALSE,m);
    render_wheel_mesh(&r,&q,0,N2_DRAW_OPAQUE);
    m[14]=0;glUniformMatrix4fv(r.uMVP,1,GL_FALSE,m);
    render_wheel_mesh(&r,&q,tex,N2_DRAW_BLEND);
    unsigned char overlap[4];
    glReadPixels(4,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,overlap);
    assert(overlap[1]>245 && overlap[0]<5 && overlap[2]<5);
    glReadPixels(20,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,overlap);
    assert(overlap[0]>120 && overlap[0]<136 && overlap[1]>120 && overlap[1]<136 && overlap[2]<5);
    glReadPixels(28,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,overlap);
    assert(overlap[0]>245 && overlap[1]<5 && overlap[2]<5);
    /* Axial wheel face: rim paint and sheen must not brighten dark rubber,
       plastic or backing texels. Exercise both authored alpha modes. */
    const uint32_t materials[]={N2_MAT_RUBBER,N2_MAT_INTERIOR,N2_MAT_DULLPLASTIC,
                                N2_MAT_MAGSILVER,N2_MAT_MAGCHROME,0,0x12049251u};
    float axial[]={0,1,0, 0,1,0, 0,1,0, 0,1,0};
    glBindBuffer(GL_ARRAY_BUFFER,q.nbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof axial,axial,GL_STATIC_DRAW);
    for(int i=0;i<4;i++)pixels[i*4]=pixels[i*4+1]=pixels[i*4+2]=32;
    glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glUniform1f(r.uAmbient,.42f);glUniform3f(r.uLight,0,0,-1);
    for(int mode=N2_DRAW_CUTOUT;mode<=N2_DRAW_BLEND;mode++)
    for(int material=0;material<7;material++) {
        unsigned char raw[4]={0};q.car_material=materials[material];
        for(int paint=0;paint<2;paint++) {
            glDepthMask(GL_TRUE);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            glUniform1f(r.uRimTint,(float)paint);glUniform1f(r.uSpec,.85f);
            glUniform1f(r.uEnv,.25f);glUniform1f(r.uClearcoat,.4f);
            render_wheel_mesh(&r,&q,tex,mode);
            unsigned char sample[4],corner[4];
            glReadPixels(28,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sample);
            glReadPixels(4,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,corner);
            assert(corner[0]<5 && corner[1]<5 && corner[2]>245);
            if(!paint)memcpy(raw,sample,4);
            else if(material==3 || material==4)assert(sample[1]>sample[0]+10);
            else assert(!memcmp(raw,sample,3)); /* no tint, including unknowns */
            if(material<3)assert(sample[0]<25 && sample[1]<25 && sample[2]<25);
            const GLint loc[]={r.uRimTint,r.uSpec,r.uEnv,r.uClearcoat};
            const float expected[]={(float)paint,.85f,.25f,.4f};
            for(int i=0;i<4;i++){float value;glGetUniformfv(r.prog,loc[i],&value);assert(value==expected[i]);}
        }
    }
    assert(glGetError()==GL_NO_ERROR);
    glDeleteTextures(1,&tex);glDeleteBuffers(1,&q.vbo);glDeleteBuffers(1,&q.nbo);glDeleteBuffers(1,&q.ibo);
    glDeleteProgram(r.prog);SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(w);SDL_Quit();
    puts("wheel_render_test: PASS (alpha, depth, state, sorting, overlap, wheel material paint/sheen isolation)");
}
