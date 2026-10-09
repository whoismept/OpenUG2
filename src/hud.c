/* hud.c — in-game player HUD. See hud.h for the honesty contract.
 *
 * Everything is drawn as transformed unit quads through the existing shader,
 * exactly like the menu overlay already does; the HUD adds no GL objects
 * beyond its textures and no shader of its own. All state it touches is saved
 * and restored, so it cannot leak blending/depth/uniforms into the world pass
 * or the developer UI.
 */
#include "hud.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- helpers */

float hud_angle_delta(float from, float to) {
    float d = to - from;
    /* fmod alone leaves (-2pi, 2pi); fold once more to land in (-pi, pi]. */
    d = (float)fmod((double)d, 2.0 * M_PI);
    if (d <= -(float)M_PI) d += 2.0f * (float)M_PI;
    if (d >   (float)M_PI) d -= 2.0f * (float)M_PI;
    return d;
}

void hud_map_project(float wx, float wy, float cx, float cy,
                     float radius_m, float rot, float *mx, float *my) {
    if (radius_m < 1e-3f) radius_m = 1e-3f;
    float dx = (wx - cx) / radius_m, dy = (wy - cy) / radius_m;
    float c = cosf(rot), s = sinf(rot);
    *mx = dx * c - dy * s;
    *my = dx * s + dy * c;
}

void hud_status_text(const HudState *s, char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!s || !s->racing) return;
    if(s->race_kind==N2_RACE_DRIFT) {
        snprintf(out,(size_t)cap,"SCORE %.0f  +%.0f",s->drift_score,s->drift_chain);
    } else if(s->race_kind==N2_RACE_DRAG) {
        snprintf(out,(size_t)cap,s->engine_failed?"ENGINE BLOWN":"HEAT %d PCT%s",(int)(s->engine_heat*100),s->shift_ready?" SHIFT":"");
    } else if (s->circuit) {
        int lap = s->lap < 1 ? 1 : s->lap, laps = s->laps < 1 ? 1 : s->laps;
        if (lap > laps) lap = laps;
        snprintf(out, (size_t)cap, "LAP %d/%d", lap, laps);
    } else {
        float p = s->sprint_progress;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        snprintf(out, (size_t)cap, "%d PCT", (int)(p * 100.0f + 0.5f));
    }
}

int hud_shows_nos(const HudState *s) { return s && s->have_nos; }

int hud_shows_money(const HudState *s) {
    return s && !s->racing && s->have_money;
}

float hud_art_run_fraction(const unsigned char *rgb, int w, int h) {
    if (!rgb || w < 2 || h < 1) return 1.0f;      /* nothing to judge */
    long runs = 0, cmp = 0;
    for (int y = 0; y < h; y++)
        for (int x = 1; x < w; x++) {
            const unsigned char *a = rgb + (((long)y * w) + x - 1) * 3;
            const unsigned char *b = a + 3;
            /* 5 bits per channel: ignores dithering, keeps real edges */
            if ((a[0] >> 3) == (b[0] >> 3) && (a[1] >> 3) == (b[1] >> 3) &&
                (a[2] >> 3) == (b[2] >> 3)) runs++;
            cmp++;
        }
    return cmp ? (float)runs / (float)cmp : 1.0f;
}

void hud_place(float aspect, float anchor_x, float anchor_y,
               float off_x, float off_y, float *ndc_x, float *ndc_y) {
    if (aspect < 1e-3f) aspect = 1e-3f;
    *ndc_x = anchor_x + off_x / aspect;
    *ndc_y = anchor_y + off_y;
}

/* --------------------------------------------------------------- resources */

/* One art slot. `key` is the TPK record hash; `tex` is 0 when it did not load,
 * which every draw site must tolerate. */
typedef struct HudArc HudArc;
typedef struct { const char *name; uint32_t key; GLuint tex; int w, h;
                 unsigned char rejected; struct {
                     int ok; float cx, cy, r, a0, a1;
                 } arc; } HudArt;

enum {
    ART_BACKING, ART_TACH_BACK, ART_TACH_ARC, ART_TACH_FILL,
    ART_NOS_BAR, ART_NOS_OVERLAY,
    ART_MAP_MASK, ART_MAP_BORDER, ART_MAP_BACK, ART_MAP_CAR, ART_MAP_NORTH,
    ART_MAP_GPS, ART_MAP_START, ART_MAP_SAFEHOUSE, ART_MAP_CIRCUIT,
    ART_ARROW, ART_CINGULAR_LOGO, ART_CINGULAR_SLOGAN,
    ART_ENGAGE_BACKING, ART_ENGAGE_PHONE, ART_ENGAGE_CINGULAR,
    ART_DRAG_BACK,ART_DRAG_ARC7,ART_DRAG_ARC8,ART_DRAG_ARC9,ART_DRAG_ARC10,
    ART_DRAG_NEEDLE,ART_DRAG_HEAT,ART_DRAG_SHIFT,ART_DRIFT_SCORE,
    ART_COUNT
};

/* Arc geometry MEASURED from the shipped TACH_NOS_ALPHA art at load time,
 * in art-normalised coordinates (0..1 across the record, y up from its
 * bottom edge). Guessing that the arc is a semicircle centred on the art's
 * bottom-centre puts the needle pivot visibly outside the dial: the shipped
 * arc is neither centred nor a half-turn. */
struct HudArc {
    int   ok;
    float cx, cy, r;      /* fitted circle, fractions of the art's WIDTH */
    float a0, a1;         /* sweep start/end in radians, a0 = the low end */
};

struct Hud {
    HudArt art[ART_COUNT];
    int    preview;
    char   missing[512];
    /* smoothed display state: a needle that snaps looks broken, and a dial fed
       straight from a 60 Hz sample jitters on every gear change. */
    float  needle_rpm, needle_speed, nos_glow, arrow_angle;
    int    arrow_primed;
};

/* Source containers, tried in order. Every key is looked up in each until one
   answers, so a record moving between the shipped packs is not fatal. */
static const char *HUD_PACKS[] = {
    "GLOBAL/InGameCommon.bun",
    "GLOBAL/InGameRace.bun",
    "GLOBAL/InGameDrag.bun",
    "GLOBAL/InGameDrift.bun",
    "GLOBAL/HUD_CustomTextures_ALL.bin",
};
enum { HUD_NPACK = (int)(sizeof HUD_PACKS / sizeof HUD_PACKS[0]) };

static void hud_art_define(Hud *h, int slot, const char *name, uint32_t key) {
    h->art[slot].name = name; h->art[slot].key = key; h->art[slot].tex = 0;
}

/* Least-squares circle through the arc's opaque texels (Kasa fit), plus the
 * angular sweep taken from the largest empty gap around that circle. Both come
 * from the art, so the needle pivots where the dial actually pivots. */
static void hud_measure_arc(const N2Tex *t, struct HudArc *out) {
    out->ok = 0;
    if (!t->alpha || t->w < 8 || t->h < 8) return;
    double Sxx=0,Sxy=0,Syy=0,Sx=0,Sy=0,Sxz=0,Syz=0,Sz=0; long n=0;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            if (t->alpha[(long)y * t->w + x] < 160) continue;
            /* normalise by WIDTH on both axes so the fit stays circular, and
               flip y so it runs up from the art's bottom edge */
            double px = (x + 0.5) / (double)t->w;
            double py = (t->h - 0.5 - y) / (double)t->w;
            double z = px*px + py*py;
            Sxx += px*px; Sxy += px*py; Syy += py*py;
            Sx += px; Sy += py; Sxz += px*z; Syz += py*z; Sz += z; n++;
        }
    if (n < 64) return;
    double N = (double)n;
    /* normal equations for z + D*px + E*py + F = 0 */
    double A[3][4] = {
        { Sxx, Sxy, Sx, -Sxz },
        { Sxy, Syy, Sy, -Syz },
        { Sx,  Sy,  N,  -Sz  },
    };
    for (int c = 0; c < 3; c++) {                 /* Gaussian elimination */
        int piv = c;
        for (int r2 = c + 1; r2 < 3; r2++)
            if (fabs(A[r2][c]) > fabs(A[piv][c])) piv = r2;
        if (fabs(A[piv][c]) < 1e-12) return;
        if (piv != c) for (int k = 0; k < 4; k++) {
            double tmp = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = tmp; }
        for (int r2 = 0; r2 < 3; r2++) {
            if (r2 == c) continue;
            double f = A[r2][c] / A[c][c];
            for (int k = c; k < 4; k++) A[r2][k] -= f * A[c][k];
        }
    }
    double D = A[0][3] / A[0][0], E = A[1][3] / A[1][1], F = A[2][3] / A[2][2];
    double cx = -D * 0.5, cy = -E * 0.5;
    double rr = cx*cx + cy*cy - F;
    if (rr <= 1e-9) return;
    double r = sqrt(rr);
    if (r < 0.05 || r > 4.0) return;

    /* angular occupancy, 1-degree bins, to find where the arc is NOT */
    unsigned char bin[360]; memset(bin, 0, sizeof bin);
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            if (t->alpha[(long)y * t->w + x] < 160) continue;
            double px = (x + 0.5) / (double)t->w;
            double py = (t->h - 0.5 - y) / (double)t->w;
            double a = atan2(py - cy, px - cx) * 180.0 / M_PI;
            if (a < 0) a += 360.0;
            int b = (int)a; if (b >= 0 && b < 360) bin[b] = 1;
        }
    int best_len = 0, best_start = -1, run = 0, run_start = 0;
    for (int i = 0; i < 720; i++) {          /* two turns: handles the wrap */
        int b = i % 360;
        if (!bin[b]) { if (!run) run_start = b; run++;
                       if (run > best_len && run <= 360) { best_len = run; best_start = run_start; } }
        else run = 0;
    }
    if (best_len < 20 || best_len > 340) return;   /* not an arc */
    /* the sweep is the complement of the gap, travelled clockwise */
    double gap_end = (best_start + best_len) % 360;      /* arc CCW start */
    double gap_start = best_start;                       /* arc CCW end   */
    double a1 = gap_end, a0 = gap_start;
    while (a0 < a1) a0 += 360.0;                 /* a0 (zero RPM) is CW-first */
    out->cx = (float)cx; out->cy = (float)cy; out->r = (float)r;
    out->a0 = (float)(a0 * M_PI / 180.0);
    out->a1 = (float)(a1 * M_PI / 180.0);
    out->ok = 1;
}

/* Upload one decoded record with HUD-appropriate sampling. Clamping matters:
   these are sprites on a transparent field, and REPEAT bleeds the opposite
   edge into the border of every rotated element. */
static GLuint hud_upload(const N2Tex *t) {
    GLuint id = upload_tpk_texture_to_gpu(t);
    if (!id) return 0;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return id;
}

Hud *hud_init(const char *dataroot) {
    Hud *h = (Hud *)calloc(1, sizeof *h);
    if (!h) return NULL;
    if (!dataroot) dataroot = ".";

    hud_art_define(h, ART_BACKING,        "SS_HUDBACKING",           0xe3af0cd4u);
    hud_art_define(h, ART_TACH_BACK,      "TACH_NOS_BACKING",        0xa9e53dbcu);
    hud_art_define(h, ART_TACH_ARC,       "TACH_NOS_ALPHA",          0x04575433u);
    hud_art_define(h, ART_TACH_FILL,      "TACH_FILL_CUSTOM_00",     0x4738197eu);
    hud_art_define(h, ART_NOS_BAR,        "SS_NOS",                  0x7ab54f94u);
    hud_art_define(h, ART_NOS_OVERLAY,    "SS_NOS_OVERLAY",          0x59d6e695u);
    hud_art_define(h, ART_MAP_MASK,       "MINIMAP_MASK",            0x40cdc515u);
    hud_art_define(h, ART_MAP_BORDER,     "MINIMAP_BORDER",          0x92a75587u);
    hud_art_define(h, ART_MAP_BACK,       "MAPBACK",                 0xd86eb8ceu);
    hud_art_define(h, ART_MAP_CAR,        "MINIMAP_ICON_CAR",        0xada85247u);
    hud_art_define(h, ART_MAP_NORTH,      "MINIMAP_NORTH_INDICATOR", 0xf2a54430u);
    hud_art_define(h, ART_MAP_GPS,        "MINIMAP_GPS_SELECTOR",    0xe2848973u);
    hud_art_define(h, ART_MAP_START,      "MINIMAP_ICON_START_LINE", 0x059b3ec6u);
    hud_art_define(h, ART_MAP_SAFEHOUSE,  "MINIMAP_ICON_SAFE_HOUSE", 0x2cfd5273u);
    hud_art_define(h, ART_MAP_CIRCUIT,    "MINIMAP_ICON_CIRCUIT",    0x25559384u);
    hud_art_define(h, ART_ARROW,          "ARROW_MARKERA",           0x985c190cu);
    hud_art_define(h, ART_CINGULAR_LOGO,  "HUD_CINGULAR_LOGO",       0x67a76a24u);
    hud_art_define(h, ART_CINGULAR_SLOGAN,"HUD_CINGULAR_SLOGAN",     0xff4c9fb7u);
    hud_art_define(h, ART_ENGAGE_BACKING, "ENGAGE_BACKING",          0x99f665f4u);
    hud_art_define(h, ART_ENGAGE_PHONE,   "ENGAGE_PHONE_ICON",       0x6fff0767u);
    hud_art_define(h, ART_ENGAGE_CINGULAR,"ENGAGE_CINGULAR_ICON",    0xea570722u);

    hud_art_define(h,ART_DRAG_BACK,"DRAG_RPM_BACKING",0x57046cd9u);
    hud_art_define(h,ART_DRAG_ARC7,"DRAG_RPM_7000_LINES",0x0764b24bu);
    hud_art_define(h,ART_DRAG_ARC8,"DRAG_RPM_8000_LINES",0x0b6f436cu);
    hud_art_define(h,ART_DRAG_ARC9,"DRAG_RPM_9000_LINES",0x0f79d48du);
    hud_art_define(h,ART_DRAG_ARC10,"DRAG_RPM_10000_LINES",0xde634915u);
    hud_art_define(h,ART_DRAG_NEEDLE,"DRAG_RPM_NEEDLE",0x93476737u);
    hud_art_define(h,ART_DRAG_HEAT,"DRAG_HEAT_FILL",0x37bd6804u);
    hud_art_define(h,ART_DRAG_SHIFT,"DRAG_SHIFT_LIGHT",0x87e1ce11u);
    hud_art_define(h,ART_DRIFT_SCORE,"DRIFT_STYLE_POINTS_BACK",0x51052532u);
    for (int p = 0; p < HUD_NPACK; p++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dataroot, HUD_PACKS[p]);
        long len = 0;
        unsigned char *d = n2_read_file(path, &len);
        if (!d) continue;
        N2Tpk tpk = n2_tpk_open(d, len);
        for (int a = 0; a < ART_COUNT; a++) {
            if (h->art[a].tex) continue;             /* an earlier pack had it */
            N2Tex t;
            if (!n2_tpk_decode(d, len, tpk, h->art[a].key, &t)) continue;
            /* A record can decode to garbage -- ARROW_MARKERA in InGameCommon
               does exactly that. Shipping it would put a block of coloured
               noise at the top of the screen and call it a navigation arrow.
               n2_tex_noise is tuned for world textures and is deliberately
               conservative (it must not throw away sign faces again), and this
               noise is too dark to trip it; UI art needs its own, measured
               test. The element then falls back to a plainly non-retail
               drawing and is named in hud_missing(). */
            if (hud_art_run_fraction(t.rgb, t.w, t.h) < HUD_ART_RUN_MIN) {
                free(t.rgb); free(t.alpha); free(t.dxt);
                h->art[a].rejected = 1;
                continue;
            }
            h->art[a].tex = hud_upload(&t);
            h->art[a].w = t.w; h->art[a].h = t.h;
            /* Every dial's geometry comes from its own art: the shipped
               records put the circle CENTRE OUTSIDE the texture (the drag
               tach's is 1.34 widths to the right), so nothing can be placed
               by eye. */
            { struct HudArc fit = {0}; hud_measure_arc(&t, &fit);
              h->art[a].arc.ok = fit.ok; h->art[a].arc.cx = fit.cx;
              h->art[a].arc.cy = fit.cy; h->art[a].arc.r  = fit.r;
              h->art[a].arc.a0 = fit.a0; h->art[a].arc.a1 = fit.a1; }
            free(t.rgb); free(t.alpha); free(t.dxt);
        }
        free(d);
    }
    for (int a = 0; a < ART_COUNT; a++)
        if (!h->art[a].tex) {
            size_t used = strlen(h->missing);
            snprintf(h->missing + used, sizeof h->missing - used,
                     "%s%s%s", used ? " " : "", h->art[a].name,
                     h->art[a].rejected ? "(decodes to noise)" : "(absent)");
        }
    {   static const int dials[1] = { ART_TACH_ARC };
        for (int i = 0; i < 1; i++) {
            const HudArt *a = &h->art[dials[i]];
            if (a->arc.ok)
                printf("player HUD: %-22s centre (%.3f, %.3f)w r %.3fw "
                       "sweep %.0f..%.0f deg\n", a->name, a->arc.cx, a->arc.cy,
                       a->arc.r, a->arc.a0 * 180.0f / (float)M_PI,
                       a->arc.a1 * 180.0f / (float)M_PI);
            else printf("player HUD: %-22s no usable arc fit\n", a->name);
        }
    }
    h->needle_rpm = h->needle_speed = 0.0f;
    return h;
}

void hud_free(Hud **hp) {
    if (!hp || !*hp) return;
    Hud *h = *hp;
    for (int a = 0; a < ART_COUNT; a++)
        if (h->art[a].tex) glDeleteTextures(1, &h->art[a].tex);
    free(h);
    *hp = NULL;
}

void hud_set_preview(Hud *h, int on) { if (h) h->preview = on ? 1 : 0; }
int  hud_preview(const Hud *h) { return h ? h->preview : 0; }
const char *hud_missing(const Hud *h) { return h ? h->missing : ""; }

/* ----------------------------------------------------------------- drawing */

typedef struct { const RProg *rp; GpuMesh *quad; float asp; } HudDC;

/* Place the unit quad (local 0..1) as a rotated rectangle centred at (cx,cy)
 * NDC, sized (w,h) in HUD units. Width is divided by the aspect so a square
 * stays square at 16:9 and at 4:3 alike — the whole reason the layout is
 * authored in units rather than NDC. */
static void hud_xform(float asp, float w, float h, float cx, float cy,
                      float rot, float *M) {
    /* Texture rows run top-down but the quad's v=0 corner is at the BOTTOM, so
       every sprite would render upside down. Negating the height maps v=0 to
       the top instead, which is also the right handedness for rotation: local
       +v then points "down the image", as the art is authored. */
    h = -h;
    float wx = w / asp, c = cosf(rot), s = sinf(rot);
    /* rotation happens in SCREEN space, so the x component is un-squashed,
       rotated, then re-squashed; otherwise a rotated icon shears at 4:3. */
    M[0] = wx * c;        M[1] = w * s;    M[2] = 0; M[3] = 0;
    M[4] = -h * s / asp;  M[5] = h * c;    M[6] = 0; M[7] = 0;
    M[8] = 0; M[9] = 0;   M[10] = 1;       M[11] = 0;
    M[12] = cx - 0.5f * (M[0] + M[4]);
    M[13] = cy - 0.5f * (M[1] + M[5]);
    M[14] = 0; M[15] = 1;
}

/* Two different shader branches, and picking the wrong one is silent:
 * uUnlit outputs uColor and NEVER samples the texture, so a sprite drawn
 * through it is a flat filled rectangle. uEmissiveTex is the textured unlit
 * path -- t.rgb * uColor with alpha t.a * uAlpha -- which is exactly an
 * alpha-blended HUD sprite tinted by uColor. */
static void hud_blit(const HudDC *dc, GLuint tex, float w, float h,
                     float cx, float cy, float rot,
                     float r, float g, float b, float a) {
    if (a <= 0.0f) return;
    float M[16];
    hud_xform(dc->asp, w, h, cx, cy, rot, M);
    glUniformMatrix4fv(dc->rp->uMVP, 1, GL_FALSE, M);
    if (tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1f(dc->rp->uUseTex, 1.0f);
        glUniform1f(dc->rp->uEmissiveTex, 1.0f);
        glUniform1f(dc->rp->uUnlit, 0.0f);
    } else {
        glUniform1f(dc->rp->uUseTex, 0.0f);
        glUniform1f(dc->rp->uEmissiveTex, 0.0f);
        glUniform1f(dc->rp->uUnlit, 1.0f);
    }
    glUniform3f(dc->rp->uColor, r, g, b);
    glUniform1f(dc->rp->uAlpha, a);
    draw_gpumesh(dc->quad);
}

/* `size` is the CAP HEIGHT of the text in HUD units. draw_text's px/py are the
 * size of one font cell and the glyph is 5 cells tall, hence the /5 -- passing
 * the cap height straight through makes every label five times too big. */
static void hud_label(const HudDC *dc, const char *s, float cx, float cy,
                      float size, float r, float g, float b, float a) {
    float py = size / 5.0f, px = py / dc->asp;
    glUniform1f(dc->rp->uUseTex, 0.0f);
    glUniform1f(dc->rp->uEmissiveTex, 0.0f);
    glUniform1f(dc->rp->uUnlit, 1.0f);
    glUniform3f(dc->rp->uColor, r, g, b);
    glUniform1f(dc->rp->uAlpha, a);
    /* draw_text takes a TOP-LEFT origin; centre the run on (cx,cy). */
    draw_text(dc->quad, dc->rp->uMVP, s,
              cx - (float)strlen(s) * 2.0f * px, cy + 2.5f * py, px, py);
}

/* --------------------------------------------------------- gauge cluster */

/* Cluster size, in HUD units (1 unit = half the screen height). The arc art
 * is 2:1, and everything else is derived from its MEASURED circle. */
#define TACH_W   0.84f
#define TACH_H   0.42f


/* Sweep an angle across a measured arc. frac 0 = a0 end, 1 = a1 end. */
static float hud_arc_angle(const HudArt *a, float frac, float fb0, float fb1) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    float a0 = a->arc.ok ? a->arc.a0 : fb0 * (float)M_PI / 180.0f;
    float a1 = a->arc.ok ? a->arc.a1 : fb1 * (float)M_PI / 180.0f;
    return a0 + frac * (a1 - a0);
}


static void hud_draw_cluster(Hud *h, const HudDC *dc, const HudState *s) {
    /* Standard third-person cluster. The rev arc is the anchor; the N2O bar
       hangs below it and disappears entirely when no nitrous is fitted, so
       removing it leaves everything else exactly where it was.

       There is deliberately NO boost dial here. The only turbo art in the game
       is DRAG_TURBO_* inside GLOBAL/InGameDrag.bun, which belongs to the drag
       HUD and is out of scope for this one; and no turbo system exists to
       drive it either. Both reasons point the same way, so the gauge is
       absent rather than dead. */
    float cx, cy;
    hud_place(dc->asp, 1.0f, -1.0f, -0.52f, 0.56f, &cx, &cy);
    const HudArt *arc = &h->art[ART_TACH_ARC];
    float pivx = cx, pivy = cy - TACH_H * 0.5f, arc_r = TACH_W * 0.42f;
    if (arc->arc.ok) {
        pivx = cx + (arc->arc.cx - 0.5f) * TACH_W / dc->asp;
        pivy = cy - TACH_H * 0.5f + arc->arc.cy * TACH_W;
        arc_r = arc->arc.r * TACH_W;
    }

    hud_blit(dc, h->art[ART_TACH_FILL].tex, arc_r * 1.70f, arc_r * 1.70f,
             pivx, pivy, 0.0f, 1.0f, 1.0f, 1.0f, 0.28f);
    hud_blit(dc, h->art[ART_TACH_BACK].tex, TACH_W, TACH_H, cx, cy,
             0.0f, 1.0f, 1.0f, 1.0f, 0.85f);

    float frac = 0.0f;
    if (s->have_rpm && s->rpm_redline > 1.0f) frac = s->rpm / s->rpm_redline;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    h->needle_rpm += (frac - h->needle_rpm) * 0.35f;
    const int SEG = 48;
    for (int i = 0; i < SEG; i++) {
        float f0 = (float)i / SEG;
        if (f0 > h->needle_rpm) break;
        float a = hud_arc_angle(arc, f0, 181.0f, 29.0f), rr = arc_r * 0.80f;
        int hot = f0 > 0.82f;
        hud_blit(dc, 0, arc_r * 0.10f, arc_r * 0.10f,
                 pivx + cosf(a) * rr / dc->asp, pivy + sinf(a) * rr, a,
                 hot ? 1.0f : 0.35f, hot ? 0.18f : 0.82f, hot ? 0.12f : 1.0f, 0.90f);
    }
    hud_blit(dc, arc->tex, TACH_W, TACH_H, cx, cy, 0.0f, 1.0f, 1.0f, 1.0f, 0.95f);

    /* needle: no needle sprite ships in the non-drag packs, so this one is
       drawn. It is the only part of the dial that is not retail art. */
    {   float a = hud_arc_angle(arc, h->needle_rpm, 181.0f, 29.0f);
        float len = arc_r * 0.86f;
        hud_blit(dc, 0, len, arc_r * 0.048f,
                 pivx + cosf(a) * len * 0.5f / dc->asp,
                 pivy + sinf(a) * len * 0.5f, a, 0.95f, 0.16f, 0.12f, 0.95f);
        hud_blit(dc, 0, arc_r * 0.15f, arc_r * 0.15f, pivx, pivy, 0.0f,
                 0.10f, 0.10f, 0.12f, 0.95f);
    }

    char buf[64];
    float kmh = s->speed_kmh < 0.0f ? -s->speed_kmh : s->speed_kmh;
    h->needle_speed += (kmh - h->needle_speed) * 0.40f;
    snprintf(buf, sizeof buf, "%d", (int)(h->needle_speed + 0.5f));
    hud_label(dc, buf, pivx, pivy + arc_r * 0.52f, 0.072f, 0.85f, 0.95f, 1.0f, 1.0f);
    hud_label(dc, "KMH", pivx, pivy + arc_r * 0.22f, 0.026f, 0.55f, 0.70f, 0.80f, 1.0f);
    if (s->have_rpm) {
        if (s->gear < 0)       snprintf(buf, sizeof buf, "R");
        else if (s->gear == 0) snprintf(buf, sizeof buf, "N");
        else                   snprintf(buf, sizeof buf, "%d", s->gear);
        hud_label(dc, buf, pivx + arc_r * 0.62f, pivy + arc_r * 0.20f, 0.070f,
                  1.0f, 0.55f, 0.12f, 1.0f);
    }

    /* ---- N2O bar: present only when nitrous is FITTED ---- */
    if (hud_shows_nos(s)) {
        float bx, by;
        hud_place(dc->asp, 1.0f, -1.0f, -0.52f, 0.235f, &bx, &by);
        hud_blit(dc, h->art[ART_NOS_BAR].tex, 0.62f, 0.078f, bx, by,
                 0.0f, 0.35f, 0.38f, 0.45f, 0.85f);
        float fill = s->have_nitro_tank ? s->nitro_tank
                                        : (s->nitro_active ? 1.0f : 0.0f);
        if (fill < 0.0f) fill = 0.0f;
        if (fill > 1.0f) fill = 1.0f;
        h->nos_glow += ((s->nitro_active ? 1.0f : 0.0f) - h->nos_glow) * 0.25f;
        if (fill > 0.001f) {
            float w = 0.60f * fill;
            hud_blit(dc, h->art[ART_NOS_OVERLAY].tex, w, 0.064f,
                     bx - (0.60f - w) * 0.5f, by, 0.0f,
                     0.35f + 0.6f * h->nos_glow, 0.75f + 0.25f * h->nos_glow,
                     1.0f, 0.55f + 0.45f * h->nos_glow);
        }
        if (!s->have_nitro_tank)
            hud_label(dc, s->nitro_active ? "NOS ON" : "NOS", bx, by,
                      0.030f, 0.60f, 0.78f, 0.90f, 0.90f);
    }
}

/* ---------------------------------------------------------------- minimap */

#define MAP_R     0.30f    /* panel radius, HUD units */
#define MAP_VIEW  260.0f   /* world metres from centre to panel edge */

/* One definition of where the panel sits, shared by the draw and the pick so
   they can never drift apart. */
static void hud_map_center(float asp, float *cx, float *cy) {
    hud_place(asp, -1.0f, -1.0f, 0.38f, 0.38f, cx, cy);
}

/* Drag's tall tach uses its own source art and fitted needle pivot. */
static void hud_draw_drag(Hud *h,const HudDC *dc,const HudState *s) {
    float cx,cy;hud_place(dc->asp,1,-1,-.65f,.62f,&cx,&cy);
    int index=s->rpm_redline<=7000 ? 0 : s->rpm_redline<=8000 ? 1 : s->rpm_redline<=9000 ? 2 : 3;
    const HudArt *arc=&h->art[ART_DRAG_ARC7+index];float width=.45f,height=.9f;
    hud_blit(dc,h->art[ART_DRAG_BACK].tex,width,height,cx,cy,0,1,1,1,.9f);
    hud_blit(dc,arc->tex,width,height,cx,cy,0,1,1,1,1);
    float px=cx+(arc->arc.ok?arc->arc.cx-.5f:.5f)*width/dc->asp;
    float py=cy-height*.5f+(arc->arc.ok?arc->arc.cy:1)*width;
    float radius=arc->arc.ok?arc->arc.r*width:width*.75f;
    float fraction=s->have_rpm?fmaxf(0,fminf(1,s->rpm/(7000+index*1000))):0;
    h->needle_rpm+=(fraction-h->needle_rpm)*.35f;
    float angle=hud_arc_angle(arc,h->needle_rpm,150,30),length=radius*.85f;
    hud_blit(dc,h->art[ART_DRAG_NEEDLE].tex,length,.035f,
        px+cosf(angle)*length*.5f/dc->asp,py+sinf(angle)*length*.5f,angle,1,1,1,1);
    hud_blit(dc,h->art[ART_DRAG_SHIFT].tex,.08f,.08f,cx,cy+.45f,0,
        1,s->shift_ready?.9f:.25f,.1f,s->shift_ready?1:.25f);
    float heat=fmaxf(0,fminf(1,s->engine_heat));
    if(heat>0)hud_blit(dc,h->art[ART_DRAG_HEAT].tex,.08f,.5f*heat,
        cx-.35f/dc->asp,cy-.25f+.25f*heat,0,1,.3f,.1f,1);
    char buf[64];snprintf(buf,sizeof buf,"%d",s->gear);
    hud_label(dc,buf,px,py,.12f,1,1,1,1);
    snprintf(buf,sizeof buf,"%.0f KMH",fabsf(s->speed_kmh));
    hud_label(dc,buf,cx,cy+.52f,.055f,.7f,.9f,1,1);
    if(s->have_nos)hud_label(dc,s->nitro_active?"NOS ACTIVE":"NOS N/A",cx,cy-.53f,.04f,.3f,.8f,1,1);
}

static float hud_map_rot(const HudState *s) {
    return (float)M_PI * 0.5f - s->car_heading;
}

int hud_map_pick(float ndc_x, float ndc_y,
                 int width, int height, const HudState *s,
                 float *wx, float *wy) {
    if (!s || !wx || !wy || width <= 0 || height <= 0) return 0;
    float asp = (float)width / (float)height, cx, cy;
    hud_map_center(asp, &cx, &cy);
    /* NDC -> normalised panel coordinates (the exact inverse of the draw) */
    float mx = (ndc_x - cx) * asp / MAP_R, my = (ndc_y - cy) / MAP_R;
    if (mx * mx + my * my > 1.0f) return 0;          /* outside the round panel */
    float rot = hud_map_rot(s), c = cosf(-rot), sn = sinf(-rot);
    float dx = mx * c - my * sn, dy = mx * sn + my * c;
    *wx = s->car_x + dx * MAP_VIEW;
    *wy = s->car_y + dy * MAP_VIEW;
    return 1;
}

static void hud_draw_minimap(Hud *h, const HudDC *dc, const HudState *s) {
    float cx, cy;
    hud_map_center(dc->asp, &cx, &cy);

    /* Heading-up, like the original: the world rotates so the car's forward
       axis points at the top of the panel. World heading 0 = +X, and "up" on
       the panel is +Y, hence the quarter-turn offset. */
    float rot = hud_map_rot(s);
    const float view = s->map_radius_m > 1.0f ? s->map_radius_m : MAP_VIEW;

    /* MINIMAP_MASK is a filled disc, not a vignette: it IS the panel body, so
       it goes underneath and no square backing is needed (one would show its
       corners outside the ring). MAPBACK is a flat tint tile, laid inside. */
    hud_blit(dc, h->art[ART_MAP_BACK].tex, MAP_R * 1.40f, MAP_R * 1.40f,
             cx, cy, 0.0f, 1.0f, 1.0f, 1.0f, 0.55f);
    hud_blit(dc, h->art[ART_MAP_MASK].tex, MAP_R * 2.0f, MAP_R * 2.0f,
             cx, cy, 0.0f, 0.06f, 0.08f, 0.11f, 0.82f);

    /* Everything inside the panel is projected through the same measured
       transform: world metres -> normalised panel -> NDC. No per-district
       constants; MAP_VIEW is a zoom level, not a fitted offset. */
    #define MAP_PT(wx, wy, outx, outy, inside) do {                         \
        float _mx, _my;                                                     \
        hud_map_project((wx), (wy), s->car_x, s->car_y, view, rot,          \
                        &_mx, &_my);                                        \
        (inside) = (_mx*_mx + _my*_my) < 0.92f * 0.92f;                     \
        (outx) = cx + _mx * MAP_R / dc->asp;                                \
        (outy) = cy + _my * MAP_R;                                          \
    } while (0)

    /* free-roam road network: dots, cheap and cullable */
    if (!s->racing && s->nav_xy && s->nav_n > 0) {
        int drawn = 0;
        for (int i = 0; i < s->nav_n && drawn < 900; i++) {
            float px, py; int in;
            MAP_PT(s->nav_xy[i*2], s->nav_xy[i*2+1], px, py, in);
            if (!in) continue;
            hud_blit(dc, 0, 0.012f, 0.012f, px, py, 0.0f,
                     0.40f, 0.46f, 0.55f, 0.85f);
            drawn++;
        }
    }
    /* race route: an ordered polyline, so draw real segments. A circuit's
       last gate joins back to its first, which is why route_loop exists --
       otherwise the lap has a visible gap across the start line. */
    if (s->route_xy && s->route_n > 1) {
        const float RIM = 0.92f;
        int segs = s->route_loop ? s->route_n : s->route_n - 1;
        for (int i = 0; i < segs; i++) {
            int j = (i + 1) % s->route_n;
            float p0x, p0y, p1x, p1y;
            hud_map_project(s->route_xy[i*2], s->route_xy[i*2+1],
                            s->car_x, s->car_y, view, rot, &p0x, &p0y);
            hud_map_project(s->route_xy[j*2], s->route_xy[j*2+1],
                            s->car_x, s->car_y, view, rot, &p1x, &p1y);
            /* CLIP, do not cull: a leg longer than the window has BOTH ends
               outside while still crossing it, and dropping those is what
               leaves the race minimap blank on a long circuit. */
            float ex = p1x - p0x, ey = p1y - p0y;
            float aq = ex*ex + ey*ey;
            if (aq < 1e-12f) continue;
            float bq = 2.0f * (p0x*ex + p0y*ey);
            float cq = p0x*p0x + p0y*p0y - RIM*RIM;
            float disc = bq*bq - 4.0f*aq*cq;
            if (disc < 0.0f) continue;                  /* misses the panel */
            float sq = sqrtf(disc);
            float t0 = (-bq - sq) / (2.0f*aq), t1 = (-bq + sq) / (2.0f*aq);
            if (t0 < 0.0f) t0 = 0.0f;
            if (t1 > 1.0f) t1 = 1.0f;
            if (t1 <= t0) continue;                     /* outside [0,1] */
            float c0x = p0x + ex*t0, c0y = p0y + ey*t0;
            float c1x = p0x + ex*t1, c1y = p0y + ey*t1;
            float ax = cx + c0x * MAP_R / dc->asp, ay = cy + c0y * MAP_R;
            float bx = cx + c1x * MAP_R / dc->asp, by = cy + c1y * MAP_R;
            float dx = (bx - ax) * dc->asp, dy = by - ay;
            float len = sqrtf(dx*dx + dy*dy);
            if (len < 1e-5f) continue;
            hud_blit(dc, 0, len, 0.020f, (ax+bx)*0.5f, (ay+by)*0.5f,
                     atan2f(dy, dx), 0.30f, 0.80f, 1.0f, 0.95f);
        }
    }
    /* the armed gate: the one checkpoint that actually counts next */
    if (s->route_xy && s->route_next >= 0 && s->route_next < s->route_n) {
        float px, py; int in;
        MAP_PT(s->route_xy[s->route_next*2], s->route_xy[s->route_next*2+1],
               px, py, in);
        if (in) hud_blit(dc, h->art[ART_MAP_START].tex, 0.060f, 0.060f,
                         px, py, 0.0f, 1.0f, 0.92f, 0.35f, 1.0f);
    }
    /* destination marker, only while guidance is live */
    if (s->nav_active) {
        float px, py; int in;
        MAP_PT(s->nav_target_x, s->nav_target_y, px, py, in);
        if (!in) {   /* clamp to the rim so the player still gets a bearing */
            float dx = px - cx, dy = py - cy;
            float l = sqrtf(dx*dx*dc->asp*dc->asp + dy*dy);
            if (l > 1e-5f) { px = cx + dx / l * MAP_R * 0.88f;
                             py = cy + dy / l * MAP_R * 0.88f; }
        }
        hud_blit(dc, h->art[ART_MAP_GPS].tex, 0.075f, 0.075f, px, py, 0.0f,
                 1.0f, 1.0f, 1.0f, 0.95f);
    }
    #undef MAP_PT

    /* MINIMAP_BORDER holds ONE QUADRANT of the ring, with the ring's centre at
       the art's bottom-right corner. The retail ring is that quadrant drawn
       four times, each rotated a further 90 degrees about the panel centre. */
    for (int k = 0; k < 4; k++) {
        float th = -(float)k * (float)M_PI * 0.5f;
        float bx = -MAP_R * 0.5f, by = MAP_R * 0.5f;
        float c = cosf(th), sn = sinf(th);
        float ox = bx * c - by * sn, oy = bx * sn + by * c;
        hud_blit(dc, h->art[ART_MAP_BORDER].tex, MAP_R, MAP_R,
                 cx + ox / dc->asp, cy + oy, th, 1.0f, 1.0f, 1.0f, 0.90f);
    }
    /* the car icon never rotates: the map turns under it */
    hud_blit(dc, h->art[ART_MAP_CAR].tex, 0.070f, 0.070f, cx, cy, 0.0f,
             1.0f, 1.0f, 1.0f, 1.0f);
    /* north sits on the rim in the direction world north currently points */
    hud_blit(dc, h->art[ART_MAP_NORTH].tex, 0.072f, 0.036f,
             cx + cosf(rot + (float)M_PI * 0.5f) * MAP_R * 0.84f / dc->asp,
             cy + sinf(rot + (float)M_PI * 0.5f) * MAP_R * 0.84f,
             0.0f, 1.0f, 1.0f, 1.0f, 0.85f);
}

/* ------------------------------------------------------- navigation arrow */

static void hud_draw_nav_arrow(Hud *h, const HudDC *dc, const HudState *s) {
    if (!s->nav_active) { h->arrow_primed = 0; return; }
    /* Relative bearing: where the next reachable route node lies compared to
       where the car points. Guidance never points at the destination through
       a building — nav_bearing comes from the solved route, not from the
       straight line to the target. */
    float want = hud_angle_delta(s->car_heading, s->nav_bearing);
    if (!h->arrow_primed) { h->arrow_angle = want; h->arrow_primed = 1; }
    else {
        /* ease along the SHORT way round, so a turn through +/-pi does not
           whip the arrow the wrong direction */
        h->arrow_angle += hud_angle_delta(h->arrow_angle, want) * 0.20f;
        h->arrow_angle = hud_angle_delta(0.0f, h->arrow_angle);
    }
    float ax, ay;
    hud_place(dc->asp, 0.0f, 1.0f, 0.0f, -0.20f, &ax, &ay);   /* exact top centre */
    /* Both angles are world bearings (CCW from +X), so a positive delta means
       the target is CCW of the nose = to the LEFT. hud_xform's rot is also
       CCW-positive, so the arrow angle passes straight through: negating it
       here points the arrow at the mirror image of every turn. */
    float rot = h->arrow_angle;
    if (h->art[ART_ARROW].tex) {
        /* the sprite points up at rot 0; a right turn must lean right */
        hud_blit(dc, h->art[ART_ARROW].tex, 0.13f, 0.26f, ax, ay, rot,
                 0.30f, 0.62f, 1.0f, 0.95f);
    } else {
        /* No usable arrow art ships in these packs, so this chevron is drawn,
           not decoded. Two swept bars meeting at the tip -- deliberately plain,
           so it is never mistaken for the original sprite. */
        const float L = 0.115f, T = 0.032f, SPREAD = 0.70f;
        /* forward is "up" at rot 0; screen y is up, so dir = (-sin, cos) */
        float fx = -sinf(rot), fy = cosf(rot);
        float tipx = ax + fx * L * 0.62f / dc->asp, tipy = ay + fy * L * 0.62f;
        for (int side = -1; side <= 1; side += 2) {
            /* each leg runs BACK from the tip, swept +/-SPREAD off the axis */
            float c = cosf((float)side * SPREAD), sn = sinf((float)side * SPREAD);
            float lx = -fx * c - -fy * sn, ly = -fx * sn + -fy * c;
            float mx = tipx + lx * L * 0.5f / dc->asp, my = tipy + ly * L * 0.5f;
            hud_blit(dc, 0, L, T, mx, my, atan2f(ly, lx),
                     0.30f, 0.62f, 1.0f, 0.95f);
        }
        /* shaft, so the glyph reads as an arrow rather than a bare V */
        hud_blit(dc, 0, L * 0.95f, T * 0.82f,
                 ax - fx * L * 0.10f / dc->asp, ay - fy * L * 0.10f,
                 atan2f(fy, fx), 0.30f, 0.62f, 1.0f, 0.95f);
    }
    if (s->nav_dist_m > 0.5f) {
        char buf[32];
        if (s->nav_dist_m >= 1000.0f)
            snprintf(buf, sizeof buf, "%d KM", (int)(s->nav_dist_m / 1000.0f + 0.5f));
        else
            snprintf(buf, sizeof buf, "%d M", (int)(s->nav_dist_m + 0.5f));
        hud_label(dc, buf, ax, ay - 0.175f, 0.040f, 0.55f, 0.80f, 1.0f, 0.95f);
    }
}

/* ------------------------------------------------- race / money / message */

static void hud_draw_status(const HudDC *dc, const HudState *s) {
    char buf[96];
    float x, y;
    hud_place(dc->asp, 1.0f, 1.0f, -0.34f, -0.16f, &x, &y);

    if (s->racing) {
        /* money is hidden during a race, per the original */
        if (s->cars > 1) {
            snprintf(buf, sizeof buf, "POS %d/%d", s->position, s->cars);
            hud_label(dc, buf, x, y, 0.070f, 1.0f, 0.95f, 0.80f, 1.0f);
        }
        hud_status_text(s, buf, sizeof buf);
        if (buf[0])
            hud_label(dc, buf, x, y - 0.095f, 0.052f, 0.85f, 0.92f, 1.0f, 1.0f);
    } else if (hud_shows_money(s)) {
        snprintf(buf, sizeof buf, "%ld", s->money);
        hud_label(dc, "BANK", x, y, 0.040f, 0.60f, 0.72f, 0.85f, 1.0f);
        hud_label(dc, buf, x, y - 0.085f, 0.065f, 1.0f, 0.92f, 0.55f, 1.0f);
    } else {
        /* No career/economy system exists yet. Name the gap instead of
           printing a number that looks like a balance. */
        hud_label(dc, "BANK", x, y, 0.040f, 0.60f, 0.72f, 0.85f, 0.9f);
        hud_label(dc, "N/A",  x, y - 0.085f, 0.055f, 0.45f, 0.50f, 0.55f, 0.9f);
    }
}

static void hud_draw_message(Hud *h, const HudDC *dc, const HudState *s) {
    if (s->racing) return;          /* free roam only, as in the original */
    float x, y;
    hud_place(dc->asp, -1.0f, 1.0f, 0.30f, -0.13f, &x, &y);
    hud_blit(dc, h->art[ART_ENGAGE_BACKING].tex, 0.46f, 0.20f, x, y, 0.0f,
             1.0f, 1.0f, 1.0f, 0.80f);
    hud_blit(dc, h->art[ART_CINGULAR_LOGO].tex, 0.26f, 0.13f, x, y + 0.025f,
             0.0f, 1.0f, 1.0f, 1.0f, 0.95f);
    hud_blit(dc, h->art[ART_CINGULAR_SLOGAN].tex, 0.24f, 0.06f, x, y - 0.055f,
             0.0f, 1.0f, 1.0f, 1.0f, 0.85f);
    /* The phone icon means "you have a message". Nothing in the engine can
       produce one yet, so it is drawn only if something really sets the flag —
       never on a timer to make the screenshot look busier. */
    if (s->message_waiting)
        hud_blit(dc, h->art[ART_ENGAGE_PHONE].tex, 0.075f, 0.075f,
                 x + 0.19f, y + 0.06f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

/* ------------------------------------------------------------------ frame */

void hud_draw(Hud *h, const RProg *rp, GpuMesh *quad,
              const HudState *st, int width, int height) {
    if (!h || !rp || !quad || !st || width <= 0 || height <= 0) return;

    HudState s = *st;
    if (h->preview) {           /* clearly-labelled sample values only */
        s.have_rpm = 1; s.have_nitro_tank = 1; s.have_boost = 1; s.have_money = 1;
        s.have_nos = 1; s.have_turbo = 1;   /* show the full cluster */
        if (s.rpm_redline < 1.0f) s.rpm_redline = 8000.0f;
        if (s.rpm <= 0.0f) s.rpm = 5200.0f;
        if (s.speed_kmh <= 0.0f) s.speed_kmh = 137.0f;
        if (s.gear == 0) s.gear = 4;
        s.nitro_tank = 0.62f; s.boost = 0.4f; s.money = 250000;
    }

    HudDC dc; dc.rp = rp; dc.quad = quad; dc.asp = (float)width / (float)height;

    /* --- enter 2D: save what we change so the world pass is untouched --- */
    GLboolean depth_on = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_on = glIsEnabled(GL_BLEND);
    GLint blend_src = GL_SRC_ALPHA, blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst);
    GLboolean depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(rp->uUnlit, 1.0f);
    glUniform1f(rp->uVColor, 0.0f);
    /* both unlit branches mix toward uFogColor; a HUD quad's vPos is meaningless
       so the only safe fog for it is none. Restored below. */
    GLfloat fog_density = 0.0f;
    glGetUniformfv(rp->prog, rp->uFogDensity, &fog_density);
    glUniform1f(rp->uFogDensity, 0.0f);
    glUniform1f(rp->uAlpha, 1.0f);
    glUniform1f(rp->uDecal, 0.0f);
    glUniform1f(rp->uAlphaTest, 0.0f);
    glUniform1f(rp->uTextureAlpha, 1.0f);   /* HUD art is alpha-blended sprites */
    glUniform1f(rp->uEmissiveTex, 0.0f);
    glUniform1f(rp->uVista, 0.0f);
    glUniform3f(rp->uEmissive, 0.0f, 0.0f, 0.0f);

    hud_draw_minimap(h, &dc, &s);
    if(s.racing && s.race_kind==N2_RACE_DRAG)hud_draw_drag(h,&dc,&s);
    else hud_draw_cluster(h, &dc, &s);
    if(s.racing && s.race_kind==N2_RACE_DRIFT) {
        float x,y;hud_place(dc.asp,0,1,0,-.18f,&x,&y);
        hud_blit(&dc,h->art[ART_DRIFT_SCORE].tex,.65f,.08f,x,y,0,1,1,1,.8f);
        char score[80];snprintf(score,sizeof score,"%.0f  +%.0f",s.drift_score,s.drift_chain);
        hud_label(&dc,score,x,y,.065f,1,.9f,.5f,1);
    }
    hud_draw_status(&dc, &s);
    if (!s.racing) hud_draw_message(h, &dc, &s);
    hud_draw_nav_arrow(h, &dc, &s);

    if (h->preview) {
        float px, py;
        hud_place(dc.asp, 0.0f, -1.0f, 0.0f, 0.045f, &px, &py);
        hud_label(&dc, "PREVIEW SAMPLE VALUES", px, py, 0.040f,
                  1.0f, 0.55f, 0.25f, 1.0f);
    }

    /* --- restore --- */
    glUniform1f(rp->uUseTex, 0.0f);
    glUniform1f(rp->uUnlit, 0.0f);
    glUniform1f(rp->uEmissiveTex, 0.0f);
    glUniform1f(rp->uAlpha, 1.0f);
    glUniform1f(rp->uTextureAlpha, 0.0f);
    glUniform1f(rp->uFogDensity, fog_density);
    glUniform3f(rp->uColor, 1.0f, 1.0f, 1.0f);
    glDepthMask(depth_mask);
    if (depth_on) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blend_on) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc((GLenum)blend_src, (GLenum)blend_dst);
}
