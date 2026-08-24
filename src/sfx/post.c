/* post.c -- bloom and tone. See post.h. */
#include "post.h"

#include <stdlib.h>

#ifdef USE_GLES2
#  define GLSL_HEADER "precision mediump float;\n"
#else
#  define GLSL_HEADER "#version 120\n#define lowp\n#define mediump\n#define highp\n"
#endif

/* Local copy of the shader compiler: render.c keeps its own private one, and a
   screen-space pass should not have to reach into it. */
static GLuint pp_compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(sh, sizeof log, NULL, log);
               fprintf(stderr, "post shader: %s\n", log); }
    return sh;
}

/* ---- post-processing: bloom and tone -------------------------------------
 *
 * The TONE CONSTANTS here are taken as a starting point from the noclip.website
 * project, which implements the same era of this engine's post effects and is
 * open source under the MIT licence:
 *
 *     https://github.com/magcius/noclip.website
 *
 * They are: the luminance weights (0.6125, 0.5154, 0.0721), the desaturation
 * of 0.5, the coloured bloom intensity of 1.75 and the tint (0.88, 0.80,
 * 0.44). None of them is read from the shipped data, and none could be: the
 * effect parameter blocks hold handles rather than numbers.
 *
 * The blur kernel -- offsets +-1 and +-3 with weights 0.3333 / 0.1667 -- is
 * the one part measured here rather than borrowed.
 *
 * The tone constants below are NOT read from the shipped data -- none of the
 * asset files carry them, so they are calibrated values rather than exact
 * ones and should be treated as approximations:
 *     luminance weights  (0.6125, 0.5154, 0.0721)
 *     desaturation        0.5
 *     coloured bloom      intensity 1.75, tint (0.88, 0.80, 0.44)
 * The blur kernel is the one thing here that is exact: offsets +-1 and +-3
 * with weights 0.3333 / 0.1667.
 *
 * Shape of the pass: the scene renders into a texture, mip levels are built
 * from it, and a final pass adds the sharp frame to a blurred copy (a mip
 * level standing in for a real blur) and tones the result. */
static GLuint g_pp_fbo = 0, g_pp_tex = 0, g_pp_depth = 0, g_pp_prog = 0;
static int    g_pp_w = 0, g_pp_h = 0;
static GLint  g_pp_uScene, g_pp_uTint, g_pp_uAmount;

static const char *PP_VS =
    GLSL_HEADER
    "attribute vec3 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "void main(){ vUV = aPos.xy; gl_Position = vec4(aPos.xy*2.0-1.0, 0.0, 1.0); }\n";

static const char *PP_FS =
    GLSL_HEADER
    "varying vec2 vUV; uniform sampler2D uScene; uniform vec3 uTint; uniform float uAmount;\n"
    "void main(){\n"
    "  vec3 c = texture2D(uScene, vUV).rgb;\n"
    /* blurred copy sampled from a mip level -- a cheap stand-in for a
       separate blur pass, and roughly the radius the real kernel gives */
    "  vec3 blur = texture2D(uScene, vUV, 3.0).rgb;\n"
    "  float lum = dot(blur, vec3(0.6125, 0.5154, 0.0721));\n"
    /* overbright: whatever passes the threshold blooms in the warm tint */
    "  float over = max(lum - 0.55, 0.0);\n"
    "  vec3 bloom = blur * uTint * over * 1.75;\n"
    "  vec3 outc = c + bloom * uAmount;\n"
    /* slight desaturation toward the tint; this is what warms the frame */
    "  float g = dot(outc, vec3(0.6125, 0.5154, 0.0721));\n"
    "  outc = mix(outc, g * uTint, 0.5 * uAmount);\n"
    "  gl_FragColor = vec4(outc, 1.0);\n"
    "}\n";

int pp_init(int w, int h) {
    if (g_pp_tex && g_pp_w == w && g_pp_h == h) return 1;
    if (g_pp_tex) { glDeleteTextures(1, &g_pp_tex); glDeleteFramebuffers(1, &g_pp_fbo);
                    glDeleteRenderbuffers(1, &g_pp_depth); }
    g_pp_w = w; g_pp_h = h;
    glGenTextures(1, &g_pp_tex);
    glBindTexture(GL_TEXTURE_2D, g_pp_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &g_pp_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_pp_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glGenFramebuffers(1, &g_pp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_pp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_pp_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_pp_depth);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) { printf("post: incomplete framebuffer\n"); return 0; }
    if (!g_pp_prog) {
        g_pp_prog = glCreateProgram();
        glAttachShader(g_pp_prog, pp_compile(GL_VERTEX_SHADER, PP_VS));
        glAttachShader(g_pp_prog, pp_compile(GL_FRAGMENT_SHADER, PP_FS));
        glBindAttribLocation(g_pp_prog, 0, "aPos");
        glBindAttribLocation(g_pp_prog, 1, "aUV");
        glLinkProgram(g_pp_prog);
        g_pp_uScene  = glGetUniformLocation(g_pp_prog, "uScene");
        g_pp_uTint   = glGetUniformLocation(g_pp_prog, "uTint");
        g_pp_uAmount = glGetUniformLocation(g_pp_prog, "uAmount");
    }
    printf("post: %dx%d render target ready\n", w, h);
    return 1;
}

void pp_begin(void) { glBindFramebuffer(GL_FRAMEBUFFER, g_pp_fbo); }

void pp_end_and_draw(GpuMesh *quad, float amount) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, g_pp_tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glUseProgram(g_pp_prog);
    glUniform1i(g_pp_uScene, 0);
    /* warm yellow tint */
    glUniform3f(g_pp_uTint, 0.88f, 0.80f, 0.44f);
    glUniform1f(g_pp_uAmount, amount);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_pp_tex);
    draw_gpumesh(quad);
    glEnable(GL_DEPTH_TEST);
}
