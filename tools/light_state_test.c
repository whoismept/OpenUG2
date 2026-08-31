/* Real GL regression: a light pass must neither inherit disabled depth testing
 * nor leak its texture, blend factors or uniforms into the next draw. No assets. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "render.h"

static void eq_uniform(const RProg *r, GLint loc, const float *want, int n) {
    float got[16]; glGetUniformfv(r->prog, loc, got);
    for (int i=0; i<n; i++) assert(fabsf(got[i]-want[i]) < 1e-6f);
}

int main(void) {
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window *win = SDL_CreateWindow("light-state-test", 0, 0, 32, 32,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    assert(win);
    SDL_GLContext ctx = SDL_GL_CreateContext(win); assert(ctx);
    RProg r = render_program(); GpuMesh quad = make_quad();
    GLuint tex[2]; glGenTextures(2, tex);
    const unsigned char pixel[4] = {128,64,32,255};
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    float cam[3]={0,-3,0}, look[3]={0,1,0}, P[16], V[16], mvp[16], saved[16];
    mat_persp(0.9f,1,0.1f,20,P); mat_lookat(cam,look,V); mat_mul(P,V,mvp);
    mat_trans(2,3,4,saved);
    N2LightSrc lights[2] = {{{0,0,0},2,30,0xff4080ffu},
                            {{1000,1000,1000},2,30,0xffffffffu}};
    const float color[3]={0.2f,0.4f,0.6f};
    const GLint scalars[]={r.uAlpha,r.uUnlit,r.uEmissiveTex,r.uUseTex,r.uSoft};
    const float values[]={0.3f,0.4f,0.2f,0.3f,0.4f};
    glViewport(0,0,32,32); glClearColor(0,0,0,0);
    for (int mode=0; mode<3; mode++) {
        int blocked = mode==1, black_in_fog = mode==2;
        const float fog_color[3]={0.3f,0.2f,0.1f};
        glUniform3fv(r.uFogColor,1,fog_color);
        glUniform1f(r.uFogDensity,black_in_fog ? 1.0f : 0.0f);
        if (black_in_fog) {
            const unsigned char black[4]={0,0,0,255};
            glBindTexture(GL_TEXTURE_2D,tex[0]);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,black);
        }
        glDepthMask(GL_TRUE); glClearDepth(blocked ? 0.0 : 1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST); /* the pass must establish depth testing */
        if (blocked) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE,GL_ZERO,GL_ZERO,GL_ONE);
        glBindTexture(GL_TEXTURE_2D,tex[1]);
        glUniform3fv(r.uColor,1,color);
        glUniformMatrix4fv(r.uMVP,1,GL_FALSE,saved);
        for (int i=0;i<5;i++) glUniform1f(scalars[i],values[i]);
        assert(render_district_lights(&r,&quad,tex[0],lights,2,cam,look,mvp,10)==1);
        eq_uniform(&r,r.uColor,color,3);
        eq_uniform(&r,r.uMVP,saved,16);
        eq_uniform(&r,r.uFogColor,fog_color,3);
        for (int i=0;i<5;i++) eq_uniform(&r,scalars[i],values+i,1);
        GLint v; glGetIntegerv(GL_TEXTURE_BINDING_2D,&v); assert((GLuint)v==tex[1]);
        glGetIntegerv(GL_BLEND_SRC_RGB,&v); assert(v==GL_ONE);
        glGetIntegerv(GL_BLEND_DST_RGB,&v); assert(v==GL_ZERO);
        glGetIntegerv(GL_BLEND_SRC_ALPHA,&v); assert(v==GL_ZERO);
        glGetIntegerv(GL_BLEND_DST_ALPHA,&v); assert(v==GL_ONE);
        assert(!glIsEnabled(GL_DEPTH_TEST));
        assert(!!glIsEnabled(GL_BLEND)==blocked);
        GLboolean mask; glGetBooleanv(GL_DEPTH_WRITEMASK,&mask); assert(mask);
        unsigned char out[4]; glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,out);
        if (blocked || black_in_fog) assert(out[0]==0 && out[1]==0 && out[2]==0);
        else assert(out[0]>0 && out[1]>0 && out[2]>0);
        assert(glGetError()==GL_NO_ERROR);
    }
    glDeleteTextures(2,tex); glDeleteProgram(r.prog);
    glDeleteBuffers(1,&quad.vbo); glDeleteBuffers(1,&quad.nbo); glDeleteBuffers(1,&quad.ibo);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    puts("light_state_test: PASS (state isolation, depth occlusion, distance culling, additive fog)");
    return 0;
}
