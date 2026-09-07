/* frontend_draw.c — GL drawing adapter for the frontend.
 * GLES2-compatible: no fixed-function, no immediate mode.
 *
 * Uses a minimal shader with position + color uniforms and an embedded
 * 3×5 bitmap font (same glyph set as the main game's render.c, but
 * self-contained — no dependency on render.h). */
#ifdef __APPLE__
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "frontend_draw.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- minimal shader ---- */
/* ponytail: GLES2-compatible, no version directive = desktop GL2 default */
static const char *FE_VS =
    "attribute vec2 aPos;\n"
    "uniform mat4 uMVP;\n"
    "void main() { gl_Position = uMVP * vec4(aPos, 0.0, 1.0); }\n";

static const char *FE_FS =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform vec4 uColor;\n"
    "void main() { gl_FragColor = uColor; }\n";

struct FeDraw {
    unsigned prog;
    int uMVP, uColor;
    unsigned vbo;
};

static unsigned fe_compile(unsigned type, const char *src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, 0, log);
        fprintf(stderr, "frontend shader compile: %s\n", log);
    }
    return s;
}

FeDraw *fed_init(void) {
    FeDraw *d = (FeDraw *)calloc(1, sizeof *d);
    if (!d) return 0;

    unsigned vs = fe_compile(GL_VERTEX_SHADER, FE_VS);
    unsigned fs = fe_compile(GL_FRAGMENT_SHADER, FE_FS);
    d->prog = glCreateProgram();
    glAttachShader(d->prog, vs);
    glAttachShader(d->prog, fs);
    glBindAttribLocation(d->prog, 0, "aPos");
    glLinkProgram(d->prog);
    glDeleteShader(vs);  /* mark for deletion; freed when program is deleted */
    glDeleteShader(fs);
    int ok = 0;
    glGetProgramiv(d->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(d->prog, sizeof log, 0, log);
        fprintf(stderr, "frontend shader link: %s\n", log);
        glDeleteProgram(d->prog);
        free(d);
        return 0;
    }
    d->uMVP   = glGetUniformLocation(d->prog, "uMVP");
    d->uColor = glGetUniformLocation(d->prog, "uColor");

    /* unit quad: two triangles, (0,0)-(1,1) */
    float quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };
    glGenBuffers(1, &d->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return d;
}

/* ---- self-contained 3×5 bitmap font (matches render.c glyph set) ---- */
static const unsigned char FE_FONT[][5] = {
    {0,0,0,0,0},                                     /* space */
    {7,5,5,5,7},{2,2,2,2,2},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1}, /* 0-4 */
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7}, /* 5-9 */
    {7,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,6,4,7}, /* A-E */
    {7,4,6,4,4},{7,4,5,5,7},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,7}, /* F-J */
    {5,5,6,5,5},{4,4,4,4,7},{5,7,5,5,5},{5,7,7,7,5},{7,5,5,5,7}, /* K-O */
    {7,5,7,4,4},{7,5,5,7,1},{7,5,6,5,5},{7,4,7,1,7},{7,2,2,2,2}, /* P-T */
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2}, /* U-Y */
    {7,1,2,4,7},                                     /* Z */
    {0,0,0,0,7},{0,0,7,0,0},{1,1,2,4,4},             /* _ - / */
    {0,0,0,0,2},{7,5,5,5,2},                         /* . ? (40,41) */
};

static const unsigned char *fe_glyph(char c) {
    if (c >= '0' && c <= '9') return FE_FONT[1 + (c-'0')];
    if (c >= 'A' && c <= 'Z') return FE_FONT[11 + (c-'A')];
    if (c >= 'a' && c <= 'z') return FE_FONT[11 + (c-'a')];
    if (c == '_') return FE_FONT[37];
    if (c == '-') return FE_FONT[38];
    if (c == '/') return FE_FONT[39];
    if (c == '.') return FE_FONT[40];
    if (c == '?') return FE_FONT[41];
    return FE_FONT[0];
}

/* ---- internal draw helpers ---- */

/* Draw a filled rect in NDC. x,y = bottom-left corner. */
static void fe_rect(FeDraw *d, float x, float y, float w, float h) {
    float M[16] = { w,0,0,0, 0,h,0,0, 0,0,1,0, x,y,0,1 };
    glUniformMatrix4fv(d->uMVP, 1, GL_FALSE, M);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void fe_slant(FeDraw *d, float x, float y, float w, float h, float skew) {
    float M[16] = { w,0,0,0, skew,h,0,0, 0,0,1,0, x,y,0,1 };
    glUniformMatrix4fv(d->uMVP, 1, GL_FALSE, M);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void fe_color(FeDraw *d, float r, float g, float b, float a) {
    glUniform4f(d->uColor, r, g, b, a);
}

static float fe_text_width(const char *s, float px) {
    int n = 0;
    for (; *s; s++) n++;
    return (float)n * 4.0f * px;
}

/* Draw text at NDC (x,y = top-left), pixel size (px, py). */
static void fe_text(FeDraw *d, const char *s, float x, float y,
                    float px, float py) {
    for (; *s; s++) {
        const unsigned char *g = fe_glyph(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col))
                    fe_rect(d, x + col*px + (4-row)*px*.18f,
                            y - row*py, px*0.85f, py*0.85f);
        x += 4*px;
    }
}

/* Draw text centered horizontally at vertical position cy. */
static void fe_text_center(FeDraw *d, const char *s, float cy,
                           float px, float py) {
    float w = fe_text_width(s, px);
    fe_text(d, s, -w/2.0f, cy, px, py);
}

/* ---- NFSU2 visual reference (verified Underground 2, NOT Underground 1) ----
 * Retail NFSU2 PC main menu (verified from multiple independent sources):
 * - Dark garage/showcase backdrop with GREEN/LIME accent lighting on car
 * - Car showcase (Rachel's 350Z) dominates center-right of frame
 * - Menu items vertically stacked on the LEFT side, left-aligned
 * - Selected item: bright white-green text with a green/lime highlight bar
 * - Unselected items: muted grey-green
 * - Title screen: NFSU2 logo upper area, "PRESS ENTER" pulsing green near bottom
 * - Color palette: near-black (#080C08), lime green (#80FF00), grey-green (#6A886A)
 *
 * U1 contamination removed: teal/cyan palette, right-side menu placement,
 * "MAIN MENU" label, cyan highlight colors. All replaced with verified U2
 * green/lime palette and left-side layout.
 *
 * THIS implementation uses the bitmap font and solid-color rectangles to
 * approximate the retail composition. PROVISIONAL — retail textures from
 * FRONTA.BUN would replace these. Layout geometry and color proportions
 * are matched to verified U2 references (see scratchpad/frontend/U2_REFERENCES.md). */

/* Safe area: keep content within 90% of viewport to avoid TV overscan. */
#define SAFE_L (-0.85f)
#define SAFE_R  (0.85f)
#define SAFE_T  (0.85f)
#define SAFE_B (-0.85f)

void fed_draw(FeDraw *d, const Fe *fe, int vp_w, int vp_h) {
    if (!d || !fe || vp_w <= 0 || vp_h <= 0) return;

    /* Save GL state we change. */
    int prev_prog = 0, prev_blend = 0, prev_depth = 0;
    int prev_vp[4];
    int prev_blend_src = 0, prev_blend_dst = 0;
    int prev_buf = 0, prev_attr0 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_BLEND_SRC, &prev_blend_src);
    glGetIntegerv(GL_BLEND_DST, &prev_blend_dst);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prev_attr0);

    /* Aspect-correct logical viewport: 4:3 area centered in the window. */
    float aspect = (float)vp_w / (float)vp_h;
    int lx = 0, ly = 0, lw = vp_w, lh = vp_h;
    if (aspect > 4.0f/3.0f) {
        lw = (int)(vp_h * 4.0f / 3.0f);
        lx = (vp_w - lw) / 2;
    } else if (aspect < 4.0f/3.0f) {
        lh = (int)(vp_w * 3.0f / 4.0f);
        ly = (vp_h - lh) / 2;
    }

    glViewport(lx, ly, lw, lh);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(d->prog);
    glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    /* Pixel sizes for text (scale with viewport). */
    float px_big   = 0.022f;
    float py_big   = 0.032f;
    float px_small = 0.014f;
    float py_small = 0.022f;
    float px_title = 0.030f;
    float py_title = 0.044f;

    float p = fe_pulse(fe);

    switch (fe->screen) {
    case FE_SCREEN_TITLE: {
        /* Subtle dark overlay — let 3D showcase show through */
        fe_color(d, 0.02f, 0.04f, 0.02f, 0.45f);
        fe_rect(d, -1, -1, 2, 2);

        /* Logo panel — upper-left, dark translucent with green tint */
        fe_color(d, 0.02f, 0.06f, 0.02f, 0.55f);
        fe_slant(d, -1.0f, 0.48f, 1.20f, 0.42f, -0.10f);

        /* "NEED FOR SPEED" — small, muted green-white */
        fe_color(d, 0.60f, 0.78f, 0.55f, 0.90f);
        fe_text(d, "NEED FOR SPEED", SAFE_L+.02f, 0.80f,
                px_small*0.72f, py_small*0.72f);

        /* "UNDERGROUND 2" — larger, bright green-white (approx: U2 logo) */
        fe_color(d, 0.78f, 0.95f, 0.60f, 1.0f);
        fe_text(d, "UNDERGROUND 2", SAFE_L+.02f, 0.64f,
                px_title*0.80f, py_title*0.80f);

        /* Pulsing green "PRESS ENTER" bar near bottom-center */
        fe_color(d, 0.30f, 0.75f, 0.15f, 0.18f + 0.40f*p);
        fe_slant(d, -0.32f, -0.55f, 0.64f, 0.09f, -0.03f);
        fe_color(d, 0.75f, 1.0f, 0.55f, 0.50f + 0.50f*p);
        fe_text_center(d, "PRESS ENTER", -0.47f, px_small, py_small);

        /* Build credit — bottom-left, very small */
        fe_color(d, 0.40f, 0.55f, 0.35f, 0.70f);
        fe_text(d, "OPENUG2", SAFE_L+.02f, -0.80f,
                px_small*0.55f, py_small*0.55f);
        break;
    }

    case FE_SCREEN_MAIN: {
        /* Left-side panel — translucent dark green, covers LEFT half only
         * so the 3D car showcase on the right stays visible */
        fe_color(d, 0.02f, 0.05f, 0.02f, 0.58f);
        fe_slant(d, -1.02f, -0.92f, 1.10f, 1.88f, -0.06f);

        /* Logo — upper-left, smaller than title screen */
        fe_color(d, 0.55f, 0.72f, 0.48f, 0.85f);
        fe_text(d, "NEED FOR SPEED", SAFE_L+.02f, 0.80f,
                px_small*0.52f, py_small*0.52f);
        fe_color(d, 0.72f, 0.90f, 0.55f, 0.95f);
        fe_text(d, "UNDERGROUND 2", SAFE_L+.02f, 0.68f,
                px_big*0.65f, py_big*0.65f);

        /* Menu entries — LEFT-aligned, vertically stacked */
        float y_start = 0.38f;
        float y_step  = 0.18f;
        float menu_x  = SAFE_L + 0.06f;

        for (int i = 0; i < fe->entry_count; i++) {
            float y = y_start - i * y_step;
            int sel = (i == fe->selected);
            int ena = fe->entries[i].enabled;

            if (sel) {
                /* Green highlight glow behind selected entry */
                fe_color(d, 0.25f, 0.65f, 0.10f, 0.14f + 0.14f*p);
                fe_slant(d, menu_x - 0.04f, y - 5*py_big - 0.02f,
                         0.82f, 5*py_big + 0.05f, -0.04f);
                /* Thin green accent bar on left edge */
                fe_color(d, 0.45f, 1.0f, 0.20f, 0.65f + 0.35f*p);
                fe_rect(d, menu_x - 0.04f, y - 5*py_big - 0.01f,
                        0.012f, 5*py_big + 0.04f);
            }

            /* Text color: green-white selected, grey-green unselected,
             * dim for disabled */
            if (!ena)     fe_color(d, 0.28f, 0.35f, 0.25f, 0.50f);
            else if (sel) fe_color(d, 0.88f, 1.0f, 0.78f, 1.0f);
            else          fe_color(d, 0.45f, 0.60f, 0.40f, 0.88f);

            const char *label = fe->entries[i].label
                                ? fe->entries[i].label : "---";
            fe_text(d, label, menu_x, y, px_big, py_big);
        }

        /* Navigation hints — bottom-left, small and peripheral */
        fe_color(d, 0.35f, 0.48f, 0.30f, 0.75f);
        fe_text(d, "UP/DOWN NAVIGATE   ENTER SELECT   ESC BACK",
                SAFE_L+.02f, SAFE_B+0.05f, px_small*0.50f, py_small*0.50f);
        break;
    }

    case FE_SCREEN_QUIT_CONFIRM: {
        /* Dark green-tinted dim overlay */
        fe_color(d, 0.01f, 0.03f, 0.01f, 0.72f);
        fe_rect(d, -1, -1, 2, 2);

        /* Dialog panel — slightly asymmetric slant, dark with green border */
        fe_color(d, 0.03f, 0.08f, 0.03f, 0.94f);
        fe_slant(d, -0.56f, -0.28f, 1.12f, 0.58f, -0.06f);
        /* Green accent borders */
        fe_color(d, 0.35f, 0.80f, 0.25f, 0.80f);
        fe_rect(d, -0.53f, 0.29f, 1.06f, 0.010f);
        fe_rect(d, -0.53f, -0.28f, 1.06f, 0.010f);

        /* Question */
        fe_color(d, 0.80f, 0.95f, 0.70f, 1.0f);
        fe_text_center(d, "QUIT TO DESKTOP?", 0.22f, px_big, py_big);

        /* Options — green for active, dim for inactive */
        float oy = 0.02f;
        int qs = fe->quit_sel;
        /* NO */
        if (!qs) fe_color(d, 0.50f, 1.0f, 0.30f, 1.0f);
        else     fe_color(d, 0.35f, 0.42f, 0.30f, 0.70f);
        fe_text_center(d, "NO", oy, px_big, py_big);
        /* YES */
        oy -= 0.16f;
        if (qs) fe_color(d, 1.0f, 0.45f, 0.25f, 1.0f);
        else    fe_color(d, 0.35f, 0.42f, 0.30f, 0.70f);
        fe_text_center(d, "YES", oy, px_big, py_big);

        /* Hint */
        fe_color(d, 0.30f, 0.40f, 0.28f, 0.65f);
        fe_text_center(d, "ESC  CANCEL", -0.22f, px_small*0.6f, py_small*0.6f);
        break;
    }
    }

    /* ---- restore GL state ---- */
    glDisableVertexAttribArray(0);
    if (prev_attr0) glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, (unsigned)prev_buf);
    glUseProgram((unsigned)prev_prog);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBlendFunc((unsigned)prev_blend_src, (unsigned)prev_blend_dst);
    if (!prev_blend) glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
}

void fed_free(FeDraw *d) {
    if (!d) return;
    if (d->prog) glDeleteProgram(d->prog);
    if (d->vbo) glDeleteBuffers(1, &d->vbo);
    free(d);
}
