/* envcube.c -- see envcube.h. */
#include "envcube.h"

/* ---- environment cube map ------------------------------------------------
 * There is no shipped environment texture: the slot exists but nothing fills
 * it, and the graphics options expose a reflection DETAIL and a reflection
 * RATE, which only make sense for something rendered live. So we render it:
 * six faces at low resolution, refreshed one face at a time. */
static GLuint g_env_cube = 0, g_env_fbo = 0, g_env_depth = 0;
static int    g_env_size = 0;

int env_cube_init(int size) {
    if (g_env_cube) return 1;
    g_env_size = size;
    glGenTextures(1, &g_env_cube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_env_cube);
    /* RGBA rather than RGB: an RGB cube face is not a complete framebuffer
       attachment on macOS and the driver substitutes a null texture. */
    for (int f = 0; f < 6; f++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, size, size, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &g_env_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_env_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, size, size);
    glGenFramebuffers(1, &g_env_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_env_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_env_depth);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X, g_env_cube, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) { printf("reflections: incomplete framebuffer\n"); return 0; }
    printf("reflections: %dx%d cube map ready\n", size, size);
    return 1;
}

GLuint env_cube_tex(void) { return g_env_cube; }

/* Cube face directions in GL order: +X -X +Y -Y +Z -Z.
   fwd is where the face looks, up is its vertical. */
void env_cube_face_dirs(int f, float *fwd, float *up) {
    static const float F[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const float U[6][3] = {{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}};
    for (int i = 0; i < 3; i++) { fwd[i] = F[f][i]; up[i] = U[f][i]; }
}

void env_cube_begin(int face) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_env_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, g_env_cube, 0);
    glViewport(0, 0, g_env_size, g_env_size);
}

void env_cube_end(int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
}
