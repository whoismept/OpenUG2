/* render.h — OpenUG2 Renderer module: GL context-level state (shaders, buffers,
 * textures), the tiny column-major matrix library, the 3x5 bitmap font and the
 * dev PNG screenshot writer. Owns every gl* call that creates GPU objects;
 * main.c only binds/draws through GpuMesh + the RProg uniform handles.
 *
 * Backend-neutral: it pulls in no GL headers. GPU work goes through rhi.h, and
 * the GL headers (desktop legacy GL 2.1 + GLSL 120, or OpenGL ES 2.0 + GLSL 100
 * with -DN2_GLES) live only in the GL backend (rhi_gl*.c). The GLSL_*_HEADER
 * macros below are shader-source prologue strings, not GL headers. */
#ifndef OPENUG2_RENDER_H
#define OPENUG2_RENDER_H

#include <SDL.h>
#include "rhi.h"
#include "render_params.h"

#include "nfsu2.h"

/* Shader prologues. Desktop: GLSL 1.20, precision qualifiers compile away.
 * GLES: vertex shaders need highp (world coordinates are kilometres, mediump
 * loses near geometry); fragment highp is optional in ES2, so use it only
 * where the driver has it. */
#ifdef N2_GLES
#  define GLSL_VS_HEADER "precision highp float;\n"
#  define GLSL_FS_HEADER "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n" \
                         "#else\nprecision mediump float;\n#endif\n"
#else
#  define GLSL_VS_HEADER "#version 120\n#define lowp\n#define mediump\n#define highp\n"
#  define GLSL_FS_HEADER GLSL_VS_HEADER
#endif

/* per-mesh GPU buffers + computed normals */
typedef struct {
    RhiBuffer vbo, nbo, cbo, ibo;
    int nidx, cat, trim;
    uint32_t texkey, car_material;
    unsigned char draw_mode;  /* Optional override; zero uses the caller's mode. */
} GpuMesh;

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
typedef struct { int mesh,vertex;unsigned revision; } PropBatchRange;

typedef struct {
    RhiBuffer vbo;            /* unified interleaved VBO */
    RhiBuffer ibo;            /* consolidated u16 IBO (<= 65535 verts/batch) */
    int index_count;
    uint32_t texkey;          /* first member mesh's TPK key -- DIAGNOSTIC ONLY.
                             Do not gate drawing on it: a batch's members can
                             disagree about whether they named a texture, and
                             this reports one of them. Use `unresolved`. */
    unsigned char unresolved; /* 1 only when EVERY member named a TPK key and the
                             resolver produced nothing, i.e. the batch really is
                             missing art rather than being authored untextured.
                             A batch that mixes the two draws (flat colour) --
                             an untextured mesh must not vanish because a
                             neighbour in its cell lost its texture. */
    RhiTexture tex;           /* resolved texture ({0} = untextured fallback) */
    unsigned char wettable;  /* road-only batch; upward faces receive wet shading */
    int nmesh;                /* source meshes merged in (drawn-mesh metric) */
    int scen_count[8];        /* source N2_SC_* membership (visibility audit) */
    int emit_idx;             /* pre-sort emission index: upload_world_batches
                                 re-sorts by texture, so the draw-time array
                                 index is NOT the index batch_emit saw (M79) */
    float bbox_min[3];        /* culling bounds */
    float bbox_max[3];
    unsigned char drawmode;   /* N2_DRAW_* (M135): authored from the first
                                 member mesh's resolved texture record, via
                                 n2_tex_mode. N2_DRAW_OPAQUE (0) when no
                                 per-mesh mode table was supplied -- sky/glow/
                                 vista/diagnostic batches keep their own,
                                 separate, unrelated render treatment. */
    PropBatchRange *props;
    int nprops;
} N2Batch;

/* the one shader program + its parameter block (render_params.h) */
typedef struct {
    RhiShader shader;
    uint32_t prog;   /* native program id (GL name), for tools that draw by hand */
    UG2Params *p;    /* the shader's parameters; the next draw uses what is here */
} RProg;

typedef struct HeadlightShadowParams HeadlightShadowParams;
typedef struct {
    RhiTexture map[2];   /* 512x512 RGBA8 colour targets: distance packed into RG */
    RhiTexture depth;    /* shared D16 depth target */
    RhiTexture white;    /* 1x1 for untextured casters */
    RhiShader shader;
    RhiPipeline pipeline;
    HeadlightShadowParams *params;
    int failed;
} HeadlightShadows;
/* Returns caster draws, or -1 if offscreen rendering is unavailable. */
int render_headlight_shadows(const RProg *r, HeadlightShadows *s, const N2Batch *batches, int count);
void free_headlight_shadows(HeadlightShadows *s);

/* Original-style headlight pool: a ROWS x COLS grid laid on the road. corners
 * holds (ROWS+1)*(COLS+1) world points, row-major from the near edge, sampled at
 * origin + fwd*size[0]*row/ROWS + right*size[1]*(col/COLS - 0.5). */
#define RENDER_POOL_ROWS 12
#define RENDER_POOL_COLS 8
#define RENDER_POOL_BAND 3.0f
#define RENDER_POOL_LIFT 0.08f
/* Low-beam pool on the road: length ahead and width in metres, distance of its
 * near edge from the car origin, light colour and brightness. High beam is 1.4x
 * longer. */
typedef struct { float length, width, lead, tint[3], strength; } RenderPool;
extern RenderPool render_pool;
void render_headlight_pool_frame(const float model[16], int high, float origin[3],
                                 float fwd[3], float right[3], float size[2]);
int render_headlight_pool(const RProg *r, const float model[16], const float mvp[16],
                          RhiTexture texture, float gain, int high, const float *corners);
void render_headlights(const RProg *r, const float model[16], const float anchors[4][4],
                       int high, float pitch, float range, float gain);

/* Select the material for a proven rear lens. Texture presence must not
 * disable emission. Brake lamps work independently of night running lights. */
void render_tail_lamp(const RProg *r,RhiTexture texture,int running,int braking,
                      int boost,float gain);

typedef struct { RhiTexture color, depth; int width, height, failed; } RoadReflections;   /* frame copies */
/* Copy the current world color/depth before rain/HUD, then reflect visible
 * geometry onto wet roads. 0 = disabled/no receivers, -1 = unavailable.
 * Restores pass state and texture bindings; next draw establishes mesh inputs. */
int render_road_reflections(const RProg *r,RoadReflections *s,
                           const N2Batch *batches,int count,const float cam[3],
                           const float mvp[16],const float projection[16],
                           float wetness,int quality);
void free_road_reflections(RoadReflections *s);

typedef struct {
    RhiTexture cube[2];   /* RGBA8 cube maps: one shown, one being captured */
    RhiTexture depth;     /* D16 depth target, one face's size */
    RProg capture;        /* a second UG2 shader for the capture views */
    RhiPipeline sky, opaque, add;   /* capture pipelines (depth LESS) */
    int size, face, front, ready, failed, ticks;
    float origin[3], published[3];
} CarEnvironment;
/* At most one face per call (Medium every second frame after warmup).
 * Publish only complete six-face captures. Low skips capture.
 * Shared local probe: far traffic fades to the sky, no per-car scene passes. */
int render_car_environment(const RProg *r,CarEnvironment *s,const N2Batch *world,int count,
                           const N2Batch *sky,int nsky,const float pos[3],int quality);
/* The same capture with the menu's two controls: detail 0 is 64 px faces and 1 is
 * 128 px; rate 0 skips capture, 1..3 refresh a face every third, second or frame. */
int render_car_environment_rate(const RProg *r,CarEnvironment *s,const N2Batch *world,int count,
                                const N2Batch *sky,int nsky,const float pos[3],int detail,int rate);
void free_car_environment(CarEnvironment *s);

typedef struct { RhiBuffer body, wheel; uint32_t body_key, wheel_key; int body_count, wheel_count; } CarShadow;
/* Caller clears the frame stencil and binds r. Leaves ordinary opaque state;
 * next draw establishes its material/model/attributes. Stencil prevents
 * overlapping caster triangles from darkening the same pixel twice. */
int render_car_shadow(const RProg *r,CarShadow *s,const N2Scene *car,const GpuMesh *gpu,
                      int stock,const N2Scene *rims,const GpuMesh *rim_gpu,
                      const float model[16],const float wheels[4][16],const float mvp[16],
                      const float ground[3],const float normal[3],int quality);
void free_car_shadow(CarShadow *s);
void mat_ground_shadow(const float ground[3],const float normal[3],float m[16]);
/* One simulation tick, normalized movement; independent of the player pose. */
void render_free_camera(float eye[3],float yaw,float pitch,const float move[3],float speed,float look[3]);
/* Exact exponential follow for a target moving previous..desired this frame.
   stiffness is the existing 60Hz tuning; ticks is elapsed seconds * 60. */
void render_camera_ease(float eye[3],const float previous[3],const float desired[3],
                        float stiffness,float ticks);

/* Bounded highlights from authored district lamps; no scene mirror pass. */
void render_wet_lights(const RProg *r,const N2LightSrc *lights,int count,
                       const float cam[3],float gain,int quality);
/* Shared conservative bounds test for camera draws and headlight casters. */
int render_batch_in_view(const N2Batch *b,const float mvp[16]);
/* One lens overlay draw; no scene copy or depth reads. Caller owns/deletes vbo.
 * vertices returns a count, out has room for RAIN_MAX_DROPS * 6 vertices.
 * Restores uniforms/depth/blend/cull state; next draw establishes mesh inputs. */
#define RAIN_MAX_DROPS 48
typedef struct { float pos[3], uv[2], fade; } RainVertex;
int render_rain_exposed(const N2Scene *scene,const float (*bounds)[4],const float cam[3]);
int render_rain_vertices(float time,float intensity,int quality,float aspect,RainVertex *out);
int render_rain(const RProg *r,RhiBuffer *vbo,float time,float intensity,int quality,float aspect);

/* Diagnostic triangle fill + edges; caller supplies XYZ triples and owns vbo. */
#define WALL_DEBUG_MAX_FACES 4096
int render_collision_walls(const RProg *r,RhiBuffer *vbo,const float *faces,int count,
                          const float color[3],const float mvp[16],int through);

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
/* Unit-quad footprint centred on the model bounds, aligned to heading/ground.
 * pos is on the receiving ground, up is its unit normal. */
void mat_car_footprint(const float pos[3],float heading,const float up[3],
                       const float bb[6],float length_scale,float width_scale,
                       float lift,float m[16]);
void mat_lookat(const float *eye, const float *fwd, float *m);   /* up = world +Z */

/* ---- GPU objects ---- */
/* Shading transform only; uMVP still supplies clip-space placement. NULL
 * selects world geometry. Models must be rigid or uniformly scaled (mirrors
 * allowed), as supplied by mat_car and the wheel/brake hub transforms. */
void render_model(const RProg *r,const float model[16]);
RProg    render_program(void);          /* compile+link the shader, fetch uniforms */
/* Build a shader from embedded source bodies (make: generated_shaders.h), each
   prefixed with a header (GLSL_VS_HEADER / GLSL_FS_HEADER, or ""). Attribute i
   is attributes[i]; blocks are its parameter layouts (may be NULL, 0).
   Returns {0} on failure. */
RhiShader render_shader_create(const char *label,
                               const char *vs_header, const unsigned char *vs, unsigned vs_len,
                               const char *fs_header, const unsigned char *fs, unsigned fs_len,
                               const char *const *attributes, uint32_t count,
                               const RhiParamLayout *const *blocks, uint32_t block_count);
GpuMesh *upload_scene(N2Scene *s);      /* VBO/NBO/IBO per mesh, normals computed */
void free_scene_gpu(GpuMesh *gm, int count);
GpuMesh  make_wheel(float R, float halfW);  /* procedural tyre (see render.c) */
RhiTexture make_wheel_tex(void);          /* radial alloy-rim texture for it */
GpuMesh  make_quad(void);               /* unit quad for HUD / billboards */
/* Wheel material draw: paint is restricted to proven metal; rubber, backing
 * and plastic suppress metallic lighting. All touched uniforms, texture,
 * alpha, blend and depth state are restored. Missing textures stay opaque. */
void render_wheel_mesh(const RProg *r, GpuMesh *mesh, RhiTexture texture, int mode);
/* Sort the four instances of a library's material slices by view depth.
 * order has space for 4 * scene->count entries (hub * count + slice). */
void render_wheel_order(const N2Scene *scene, const float mvp[4][16], int *order);
/* Caller provides scene->count indices; returns glass/cover count in draw order. */
int render_car_glass_order(const N2Scene *scene,const float mvp[16],int *order);

/* Sets mesh attributes/buffers; pass uniforms, texture/blend/depth state are restored. */
/* halo: sprite size multiplier (1 = tuned default); gain: emission multiplier. */
/* Establish world transform/depth/additive state; restore ordinary depth writes
 * and fog on exit. Caller binds the program and texture unit zero. */
int render_world_glows(const RProg *r,const N2Batch *batches,int count,
                        const float mvp[16]);
int render_district_lights(const RProg *r, GpuMesh *quad, RhiTexture texture,
                           const N2LightSrc *lights, int nlights,
                           const float cam[3], const float look[3],
                           const float mvp[16], float viewdist,
                           float halo, float gain);

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
/* mtexmode (optional, NULL to skip): per-mesh N2_DRAW_* (M135), mirroring
 * mtex -- the authored draw mode of the mesh's own resolved texture record.
 * Every mesh a batch merges shares one texture, hence one mode; batch_emit
 * takes it from the first member. NULL leaves every batch N2_DRAW_OPAQUE,
 * unchanged from before this field existed. */
int  upload_world_batches(const N2Scene *s, const float (*mbb)[4],
                          const RhiTexture *mtex, RhiTexture texTerr, N2Batch **out,
                          const char *audit, int *meshbatch,
                          const unsigned char *mtexmode);
/* Same partition, resumed on the GL thread. Scene and mode arrays are borrowed
 * and must stay immutable until completion/cancellation. Step returns 0 while
 * pending, 1 after transferring complete output, or -1 on failure. Pending and
 * failed steps leave outputs untouched. A quota is not a hard time deadline. */
typedef struct WorldBatchUpload WorldBatchUpload;
WorldBatchUpload *upload_world_batches_begin(const N2Scene *s,
                          const float (*mbb)[4], const RhiTexture *mtex,
                          RhiTexture texTerr, const char *audit,
                          const unsigned char *mtexmode);
int upload_world_batches_step(WorldBatchUpload **job, int max_batches,
                          N2Batch **out, int *count, int *meshbatch);
void upload_world_batches_cancel(WorldBatchUpload **job);
/* Same merge, but for one category (N2_SKY / N2_GLOW) pulled out of the main
 * batching pass above — grouped by texture only, no spatial cell/cull grid,
 * since there are only ever a handful of skybox/neon meshes per city. Sky,
 * glow, vista and diagnostic-marker batches keep their own dedicated render
 * treatment, so their callers pass mtexmode = NULL (-> N2_DRAW_OPAQUE). */
int  upload_cat_batches(const N2Scene *s, int cat, const RhiTexture *mtex, N2Batch **out,
                        const unsigned char *mtexmode);
void render_batch_array_free(N2Batch **batches, int *count);
/* Update only struck panels' existing VBO ranges; all passes share those buffers. */
int render_world_prop_updates(const N2Scene *scene,N2Batch *batches,int count,
                             int (*update_mesh)(N2Mesh *));
/* Pipelines of the sky, vista and ordinary world passes (one RProg). Several
   names share a pipeline when their state is the same. */
typedef struct {
    RhiPipeline sky_dome,   /* opaque, depth test LEQUAL, no depth write */
                sky_cap,    /* alpha blend, no depth write */
                vista,      /* = sky_cap */
                opaque,     /* depth write, no blend */
                blend,      /* authored BLEND batches: alpha blend, no depth write */
                add,        /* authored ADD batches: src alpha + one, no depth write */
                reflect,    /* wet-road reflections: depth EQUAL, alpha blend, no write */
                overlay;    /* restored-geometry diagnostic: = sky_dome */
} UG2WorldPipelines;
void render_world_pipelines(const RProg *r, UG2WorldPipelines *p);
/* BatchedVertex batch through the bound pipeline (rhi_cmd_bind_pipeline) */
void render_draw_batch(const N2Batch *b);
/* Pipelines of the car passes (GpuMesh layout). */
typedef struct {
    RhiPipeline opaque[3],   /* by RhiCullMode: none, back, front; depth write */
                glass,       /* alpha blend, no depth write, no culling */
                glass_depth, /* the same with depth write (Mesh Inspector option) */
                halo;        /* lamp halos: src alpha + one, no depth write */
} UG2CarPipelines;
void render_car_pipelines(const RProg *r, UG2CarPipelines *p);

/* Tire-mark pass (tire-marks SPEC TM-15). begin binds the glass pipeline (source-alpha
   blend, depth test <=, no depth write, no culling, no bias) and sets the shader to the
   mark law: out.rgb = clamp(2*texel.rgb*vertex.rgb), out.a = texel.a*vertex.a, world fog
   on the colour, no lighting, tint or alpha test. The caller binds GpuMesh streams, the
   texture and draws; end restores the shader state begin changed. */
typedef struct { float unlit, use_tex, alpha, vcolor, vista; } RMarkSaved;
void render_mark_begin(const RProg *r, RMarkSaved *saved);
void render_mark_end(const RProg *r, const RMarkSaved *saved);
/* Screen-space quads (GpuMesh layout, no depth test, no culling): the
   diagnostic HUD, menu overlay and text, and the HUD art / nitro overlay. */
typedef struct {
    RhiPipeline screen,         /* opaque; depth write stays on (inert without the test) */
                screen_blend;   /* alpha blend, no depth write */
} UG2OverlayPipelines;
/* The overlay pipelines of r's shader (created on first use). */
const UG2OverlayPipelines *render_overlay_pipelines(const RProg *r);
/* Pipelines of the overlay effects: position-only car shadow and collision
   walls, and the screen-space rain. */
typedef struct {
    RhiPipeline car_shadow,      /* blend, no depth write, stencil EQUAL 0 / INCR */
                walls[2][2],     /* [depth test][lines]: blend, no depth write */
                rain;            /* no depth test, blend, RainVertex layout */
} UG2EffectPipelines;
void render_effect_pipelines(const RProg *r, UG2EffectPipelines *p);
/* GpuMesh through the bound pipeline */
void render_draw_gpumesh(const GpuMesh *g);
RhiTexture upload_tex(const N2Tex *t);
/* Upload a decoded TPK texture, preferring a direct glCompressedTexImage2D of
   its raw S3TC blocks when the driver supports the format; falls back to the
   portable CPU-decoded RGBA path (upload_tex) otherwise. Both give the texture
   a REPEAT sampler (trilinear + anisotropy with a full mip chain) and leave it
   bound. */
RhiTexture upload_tpk_texture_to_gpu(const N2Tex *t);
/* Same as upload_tex / upload_tpk_texture_to_gpu, but first try a user HD
   override for `key` (tex_override.h; `pack` = source stem or NULL). With no
   override directory these are exactly the original uploaders. */
RhiTexture upload_tex_keyed(uint32_t key, const char *pack, const N2Tex *t);
RhiTexture upload_tpk_keyed(uint32_t key, const char *pack, const N2Tex *t);
/* Atlases and sprites: keep the texture's filtering but clamp both axes, so a
   filter footprint at u/v = 0 or 1 cannot wrap in the opposite edge. The
   _linear variant also samples level 0 only (bilinear), for 2D overlays. */
void render_texture_clamp(RhiTexture tex);
void render_texture_clamp_linear(RhiTexture tex);
void render_texture_clamp_edges(RhiTexture tex);   /* clamp only: mipmaps stay */
/* Anisotropic filtering, the one texture-detail knob this renderer was missing.
 * Trilinear alone samples a square footprint, so any surface seen at a grazing
 * angle -- which is most of a road, a kerb and a car's own flanks -- drops to a
 * far coarser mip than it needs and smears. Measured on a race frame: 6.2% of
 * pixels change between 1x and 16x. `render_texture_detail` re-applies the
 * setting to every texture already uploaded, so the in-game slider takes effect
 * immediately instead of on the next load. */
extern int   g_tex_aniso_max;   /* 1 = extension absent; else the GL maximum */
extern float g_tex_aniso;       /* current setting, clamped to [1, max] */
void render_texture_detail(float aniso);
extern int g_tex_s3tc;   /* 1 = GL_EXT_texture_compression_s3tc present; set once at GL init */

/* ---- 3x5 bitmap font (uppercase, digits, _ - /) ---- */
void  draw_text(GpuMesh *quad, const RProg *r, const char *s,
                float x, float y, float px, float py);
float text_width(const char *s, float px);

/* dev screenshot: rgb is top-left origin, w*h*3 */
void write_png(const char *path, int w, int h, const unsigned char *rgb);

/* Read the top-left w*h pixels of the current frame into rgb (w*h*3, top-left
 * origin, ready for write_png) through rhi_read_pixels. Call it between
 * rhi_frame_begin and rhi_frame_end. Returns 0 on failure. */
int  render_read_rgb(int w, int h, unsigned char *rgb);

#endif
