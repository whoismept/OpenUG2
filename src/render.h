/* render.h — OpenUG2 Renderer module: GL context-level state (shaders, buffers,
 * textures), the tiny column-major matrix library, the 3x5 bitmap font and the
 * dev PNG screenshot writer. Owns every gl* call that creates GPU objects;
 * main.c only binds/draws through GpuMesh + the RProg uniform handles.
 *
 * Also the single home of the GL headers: desktop legacy GL 2.1 + GLSL 120, or
 * OpenGL ES 2.0 + GLSL 100 with -DN2_GLES — every module includes GL via here.
 */
#ifndef OPENUG2_RENDER_H
#define OPENUG2_RENDER_H

#include <SDL.h>
#ifdef N2_GLES
#  include <SDL_opengles2.h>
#elif defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION 1
#  include <OpenGL/gl.h>
#  include <OpenGL/glext.h>
#else
#  define GL_GLEXT_PROTOTYPES 1
#  include <SDL_opengl.h>
#endif

#include "nfsu2.h"

/* per-mesh GPU buffers + computed normals */
typedef struct { GLuint vbo, nbo, ibo; int nidx, cat, trim;
                 uint32_t texkey;
                 uint32_t matkey; /* material record key, 0 = none */ } GpuMesh;

/* ---- static-world batching: meshes merged per (256m grid cell, texture) ----
 * One interleaved VBO per batch kills the per-mesh bind/attrib overhead;
 * batches stay small enough for the XY distance cull to keep working. */
typedef struct {
    float pos[3];
    float uv[2];
    float normal[3];
    unsigned char col[4];     /* per-vertex RGBA prelight from the source stream
                                 (baked AO/lighting + terrain tint); 255=neutral */
} BatchedVertex;              /* 36 B, interleaved */

typedef struct {
    GLuint vbo;               /* unified interleaved VBO */
    GLuint ibo;               /* consolidated u16 IBO (<= 65535 verts/batch) */
    int index_count;
    uint32_t texkey;          /* first member mesh's TPK key (debugging) */
    GLuint tex;               /* resolved GL texture (0 = untextured fallback) */
    int nmesh;                /* source meshes merged in (drawn-mesh metric) */
    int scen_count[8];        /* source N2_SC_* membership (visibility audit) */
    int emit_idx;             /* pre-sort emission index: upload_world_batches
                                 re-sorts by texture, so the draw-time array
                                 index is NOT the index batch_emit saw (M79) */
    float bbox_min[3];        /* culling bounds */
    float bbox_max[3];
} N2Batch;

/* the one shader program + its uniform handles */
typedef struct {
    GLuint prog;
    GLint uMVP, uUseTex, uColor, uUnlit, uAlpha, uSoft, uSpec, uAmbient, uDiffuse,
          uLight,   /* sun direction in the CURRENT object's model space */
          uVColor,  /* 0..1 strength of per-vertex prelight (world geometry only) */
          uDecal,   /* 1 = texture is an alpha-masked decal over uColor paint */
          uVista,                   /* >0.5: alpha-blended backdrop pass */
          uFogColor, uFogDensity,   /* exp^2 distance fog (matches the sky) */
          uCamPos,  /* camera in the current object's model space */
          uEnv,     /* environment-reflection amount (cars only) */
          uUVCheck, /* 1 = show the diagnostic UV-coordinate visualization
                       instead of lighting/texture (toggle lives in the
                       ImGui Session panel, make debug only) */
          uFlipN,   /* 1 = negate the vertex normal (inspector diagnostic) */
          uGloss,   /* specular pow() exponent: high = tight metallic-paint
                       highlight, low = broad plastic/trim sheen (cars only) */
          uRimTint, /* 0 = raw rim texture, 1 = recolor toward uColor (rim paint) */
          /* Material response taken from the shipped material record rather
             than from per-class constants: each term is a Min/Range pair the
             shader interpolates by dot(V,N). uMatOn selects it. */
          /* 1 = discard texels below half alpha. Only for textures the record
             calls cutout: applied to an opaque one it punches holes. */
          uAlphaTest,
          uMatOn, uMatDifMin, uMatDifRange, uMatSE,
          /* live environment cube: 0 = procedural night sphere, 1 = sample the
             cube, 2 = show the reflection alone (diagnostic) */
          uEnvCubeOn, uEnvYaw, uEnvCube;
} RProg;

/* world-space sun direction (night scene key light) */
#define N2_SUN_X 0.4f
#define N2_SUN_Y 0.7f
#define N2_SUN_Z 0.6f

/* ---- tiny 4x4 matrix (column-major) ---- */
void mat_mul(const float *a, const float *b, float *o);
void mat_persp(float fov, float aspect, float znear, float zfar, float *m);
void mat_trans(float x, float y, float z, float *m);
void mat_rotz(float a, float *m);
void mat_car(const float *pos, float heading, const float *up, float rideh, float *m);
/* Look-at with an explicit up vector: the cube's top and bottom faces need
   one, since the usual world-up is degenerate there. */
void mat_lookat_up(const float eye[3], const float fwd[3], const float up[3],
                   float out[16]);
void mat_lookat(const float *eye, const float *fwd, float *m);   /* up = world +Z */

/* ---- GPU objects ---- */
RProg    render_program(void);          /* compile+link the shader, fetch uniforms */
GpuMesh *upload_scene(N2Scene *s);      /* VBO/NBO/IBO per mesh, normals computed */
GpuMesh  make_wheel(float R, float halfW);  /* procedural tyre (see render.c) */
GLuint   make_wheel_tex(void);          /* radial alloy-rim texture for it */
GLuint   make_wheel_blur_tex(void);     /* same rim, angular-averaged (spinning) */
GpuMesh  make_quad(void);               /* unit quad for HUD / billboards */
void     draw_gpumesh(GpuMesh *g);

/* Merge the static world into per-(cell,texture) batches and upload them.
 * mtex = per-mesh resolved GL texture, texTerr = grass fallback for terrain
 * meshes without one. Sorted by texture so binds are rare. The CPU-side scene
 * is left untouched (physics reads it). Returns the batch count. */
/* `audit` (Milestone 75): when non-NULL, the batch that swallows the source
 * mesh of that sname is additionally verified vertex-by-vertex and reported on
 * stdout. Purely additive — the GL upload is identical either way, so the audit
 * observes the production path rather than a parallel reimplementation. */
/* meshbatch (optional, NULL to skip): caller-sized s->count array receiving the
 * FINAL batch index each source mesh ended in, or -1 if it never entered the
 * world partition (sky/glow go to their own passes). Written after the texture
 * re-sort, so the indices are the ones the renderer uses (M133). */
int  upload_world_batches(const N2Scene *s, const float (*mbb)[4],
                          const GLuint *mtex, GLuint texTerr, N2Batch **out,
                          const char *audit, int *meshbatch);
/* Same merge, but for one category (N2_SKY / N2_GLOW) pulled out of the main
 * batching pass above — grouped by texture only, no spatial cell/cull grid,
 * since there are only ever a handful of skybox/neon meshes per city. */
int  upload_cat_batches(const N2Scene *s, int cat, const GLuint *mtex, N2Batch **out);
void draw_batch(const N2Batch *b);
GLuint   upload_tex(const N2Tex *t);
/* Upload a decoded TPK texture, preferring a direct glCompressedTexImage2D of
   its raw S3TC blocks when the driver supports the format; falls back to the
   portable CPU-decoded RGBA path (upload_tex) otherwise. Leaves the texture
   bound (callers may override wrap/filter after). */
GLuint   upload_tpk_texture_to_gpu(const N2Tex *t);
extern int g_tex_s3tc;   /* 1 = GL_EXT_texture_compression_s3tc present; set once at GL init */

/* ---- 3x5 bitmap font (uppercase, digits, _ - /) ---- */
void  draw_text(GpuMesh *quad, GLint uMVP, const char *s,
                float x, float y, float px, float py);
float text_width(const char *s, float px);

/* dev screenshot: rgb is top-left origin, w*h*3 */
void write_png(const char *path, int w, int h, const unsigned char *rgb);


#endif
