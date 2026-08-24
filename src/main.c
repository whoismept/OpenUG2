/* main.c — OpenUG2: an open reimplementation of Need for Speed: Underground 2.
 * Loads the original game data directly (no Wine/box64), assembles a track
 * section, decodes its textures, and drives a textured car with AI opponents
 * around a circuit — SDL2 + OpenGL, portable across x86 and ARM.
 *
 * main.c is the orchestrator: setup, the game loop, input, race flow and HUD.
 * The engine proper lives in the modules it drives:
 *   nfsu2.h    — chunk parser (ground truth — do not "optimize")
 *   render.*   — Renderer: GL objects, shaders, matrices, font, screenshot
 *   physics.*  — car kinematics, wall + car-to-car collision
 *   ai.*       — racing-line opponents, circuit loading
 *   audio.*    — procedural engine/road/skid synth
 *   resource.* — file mapping + track/car/circuit discovery
 *
 * Desktop (x86/ARM, Linux/macOS/Windows): legacy GL 2.1 + GLSL 120.
 * Embedded / mobile (ARM): OpenGL ES 2.0 + GLSL 100 — build with -DN2_GLES
 * (links -lGLESv2). The shaders are written to compile on both.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>
#include <unistd.h>   /* execvp: menu track-switch re-launches the process */
#include <SDL.h>

#include "nfsu2.h"
#include "render.h"
#include "sfx/post.h"
#include "sfx/envcube.h"
#include "physics.h"
#include "ai.h"
#include "audio.h"
#include "resource.h"
#include "world.h"
#include "world_mesh.h"   /* F3 prelight/normal/wireframe debug pipeline */
#include "debug.h"
#include "car_setup.h"      /* per-car parameters out of the shipped table */
#include "vehicle_model.h"  /* single-track dynamics driven by those parameters */

/* debug tunables — defaults match the previously hard-coded constants, so a
 * normal build behaves exactly as before; `make debug` adds an ImGui panel. */
DbgState g_dbg = {
    .freecam = 0, .speed = 0.6f,
    .chase_distance = 10.0f, .chase_height = 4.5f, .chase_stiffness = 0.22f,
    .race_maxlaps_want = 2,
    /* .wheel (VehicleWheelConfig) is filled per car at load by wheel_config_for()
       -- an explicit table entry or a body-box fallback -- so no default here. */
    .wheel_scale = 1.0f,
    .insp_sel = -1,
    .neon_on = 1, .neon_col = { 0.15f, 0.45f, 1.0f }, .neon_str = 0.85f,
    .rim_paint = 1, .rim_color = { 0.85f, 0.88f, 0.92f },   /* silver by default */
    .tune_accel = 1.0f, .tune_brake = 1.0f, .tune_turn = 1.0f, .tune_top = 220.0f,
    .night_mode = 1,                                          /* game ships at night */
    .ambient = 0.38f, .diffuse = 0.62f, .body_spec = 0.50f,   /* glossy paint (night) */
    .body_env = 1.0f,                                          /* 1.0 = tuned clearcoat reflection */
    .vcolor = 1.0f,   /* full source prelight on world geometry */
    /* f(700m cull range) ~= 0.07 — far batches dissolve into the sky */
    .fog_density = 0.0023f, .fog_r = 0.06f, .fog_g = 0.07f, .fog_b = 0.11f,
    .paint_override = 0, .paint = { 0.68f, 0.09f, 0.08f },
    .show_body = 1, .show_glass = 1, .show_lights = 1, .show_tires = 1,
    .show_misc = 1, .show_track = 1,
    .hud_hide_menu = 1,   /* only consulted under DEBUG_UI; see debug.h */
    /* Momentary "switch to this car/track" requests. dbgui_frame() clears them
       to -1 at the top of every panel build, so they MUST start at -1: a zero
       default is a valid list index, and with the panel hidden (M122) nothing
       would ever clear it -- main.c would relaunch the process every frame. */
    .want_car = -1, .want_track = -1,
};

/* ---- Per-car wheel stance ------------------------------------------------
 * Primary source is the GLOBALB per-car table (n2_global_wheel_attr): the
 * exact factory axle X and track Y the game itself ships, decoded per car with
 * no hash and no hand-tuned table. Cars with no GLOBAL record (or when GLOBAL is
 * absent) fall back to a one-time seed from their own measured body box. Hub Z
 * stays the exact tyre-mesh measurement either way. The ImGui "Wheel Stance"
 * sliders still edit the active car's live copy. */
#define WHEEL_SEED_FRONTF 0.639f   /* fallback-only: fraction of front body extent */
#define WHEEL_SEED_REARF  0.597f   /* fallback-only: fraction of rear body extent  */
#define WHEEL_SEED_TRACKF 0.760f   /* fallback-only: fraction of half-width -> track */
static VehicleWheelConfig wheel_config_for(const char *name, const N2CarProfile *p,
                                           const unsigned char *gdata, long glen, int *from_global) {
    VehicleWheelConfig c;
    N2WheelAttr wa;
    if (n2_global_wheel_attr(gdata, glen, name, &wa)) {   /* exact, from GLOBALB car table */
        c.front_axle = wa.front_axle; c.rear_axle = wa.rear_axle;
        c.front_track = wa.front_track; c.rear_track = wa.rear_track;
        c.ride_y = p->hub_z;
        if (from_global) *from_global = 1;
        return c;
    }
    c.front_axle  = p->body[1] * WHEEL_SEED_FRONTF;   /* fallback: this car's own body box */
    c.rear_axle   = p->body[0] * WHEEL_SEED_REARF;
    c.front_track = c.rear_track = 2.0f * p->body[3] * WHEEL_SEED_TRACKF;
    c.ride_y      = p->hub_z;
    if (from_global) *from_global = 0;
    return c;
}

/* Stock body-to-wheel suspension trim. The model and wheel data share a hub
 * origin, but NFSU2 also has per-car stance/suspension tuning outside the mesh.
 * That table is not decoded yet; keep the known tune explicit and local rather
 * than moving the tyre off the road or corrupting the asset coordinates. */
static float stock_body_drop(const char *name) {
    return name && !strcmp(name, "MIATA") ? 0.040f : 0.0f;
}

/* Read-only contact audit for the four wheels exactly where the renderer puts
 * their hubs. `mat_car` supplies the chassis basis, then wheelT adds axle/track
 * and local hub Z. Subtract the rendered tyre radius along chassis-up and
 * compare that bottom point with the selected world layer at its own XY. */
static void wheel_contact_residuals(const N2Scene *scene, const float pos[3],
                                    float heading, const float up[3],
                                    const VehicleWheelConfig *wc,
                                    float wheel_ride, float wheel_radius,
                                    float out[4], int cat[4]) {
    float f[3]={cosf(heading),sinf(heading),0};
    float fd=f[0]*up[0]+f[1]*up[1]+f[2]*up[2];
    f[0]-=fd*up[0]; f[1]-=fd*up[1]; f[2]-=fd*up[2];
    float fl=sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
    if (fl<1e-6f) { f[0]=cosf(heading); f[1]=sinf(heading); f[2]=0; fl=1; }
    f[0]/=fl; f[1]/=fl; f[2]/=fl;
    float l[3]={up[1]*f[2]-up[2]*f[1],
                up[2]*f[0]-up[0]*f[2],
                up[0]*f[1]-up[1]*f[0]};
    float ax[4]={wc->front_axle,wc->front_axle,wc->rear_axle,wc->rear_axle};
    float sy[4]={wc->front_track*.5f,-wc->front_track*.5f,
                 wc->rear_track*.5f,-wc->rear_track*.5f};
    for (int i=0;i<4;i++) {
        float hub[3];
        for (int a=0;a<3;a++)
            hub[a]=pos[a]+up[a]*(wheel_ride+wc->ride_y)+f[a]*ax[i]+l[a]*sy[i];
        float bottom[3]={hub[0]-up[0]*wheel_radius,
                         hub[1]-up[1]*wheel_radius,
                         hub[2]-up[2]*wheel_radius};
        WGroundHit h;
        cat[i]=world_ground_hit(scene,bottom[0],bottom[1],pos[2],&h);
        out[i]=cat[i]==WSURF_NONE ? 0.0f : bottom[2]-h.z;
    }
}

/* Switching car or track means a whole new asset load (world batches, car
 * buffers/textures, physics obstacles, AI paths — ~30 pieces of long-lived
 * state with no teardown path), so both the menu's arrow keys and the
 * ImGui car/track combos below trigger the SAME clean re-exec rather than
 * an in-process hot-swap: the OS reclaims everything, so there is nothing
 * to leak or get left dangling, which an untested from-scratch teardown/
 * reload path could easily do quietly. */
/* Load one aftermarket rim style out of a wheel-brand archive and upload it.
 * The archive holds several size/width variants per style; the first mesh is
 * used. Frees any previously uploaded rim. Returns 1 on success. */
/* Drop triangles that weld two submesh runs together.
 *
 * What these triangles ACTUALLY are (Phase 49, re-measured -- two earlier
 * versions of this comment were wrong). They are NOT a grouping artifact:
 *   - The 0x134B02 index runs are clean. Every run's index count is a
 *     multiple of 3 and the runs are chained (start = prev start + count),
 *     so grouping the whole buffer in 3s from offset 0 never crosses a run
 *     boundary. There is no "tail bridges into the next run" bug.
 *   - The long triangles are genuine source geometry: a flat, axis-aligned
 *     quad (2 tris, 4 dedicated verts) at CONSTANT Y = 0.18, spanning the
 *     full rim diameter in X and Z. Verified identical across BBS/ENKEI/VOLK/
 *     OZ/ADVAN/LEXANI/WORK/RACINGHART/GIOVANNA/NFSU. It is the rim's flat
 *     backing/hub plane, hidden behind the spokes and occluded by the tyre
 *     and brake when mounted -- so dropping it is invisible, and keeping it
 *     would z-fight against the brake disc the engine draws separately.
 *     Measured: exactly 2 of 487 tris on BBS style 1 (LEXANI has 4 = two
 *     quads); the rest are clean spoke surface (mean edge 0.065 vs 0.85 diam).
 *
 * So the fix is a genuine-geometry cull, not a parser change: a run-boundary
 * splitter would keep these tris (they sit correctly inside one run), which
 * is why that approach was NOT taken. Dropping them here, before upload_scene,
 * means they never reach a VBO -- the VRAM is already optimal.
 *
 * Kept out of n2_add_pair on purpose: that path is shared by all 57 cars, and
 * this archive is the only place the condition arises. Purely local, and
 * geometric (edge > 0.25*diag) rather than name-based, so it is self-limiting
 * and cannot misfire on a legitimately large triangle in a small mesh. */
static void rim_drop_welded_mesh(N2Mesh *m) {
    float bb[6]; n2_mesh_bbox(m, bb);
    float dx = bb[1]-bb[0], dy = bb[3]-bb[2], dz = bb[5]-bb[4];
    float diag = sqrtf(dx*dx + dy*dy + dz*dz);
    if (diag <= 0.0f) return;
    float lim = 0.25f * diag, lim2 = lim * lim;
    int w = 0;
    for (int t = 0; t + 2 < m->nidx; t += 3) {
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            const float *p = m->verts + m->idx[t+k]*5;
            const float *q = m->verts + m->idx[t+(k+1)%3]*5;
            float ex=p[0]-q[0], ey=p[1]-q[1], ez=p[2]-q[2];
            if (ex*ex+ey*ey+ez*ez > lim2) ok = 0;
        }
        if (ok) { m->idx[w]=m->idx[t]; m->idx[w+1]=m->idx[t+1]; m->idx[w+2]=m->idx[t+2]; w += 3; }
    }
    m->nidx = w;
}
static void rim_drop_welded_tris(N2Scene *s) {
    for (int i = 0; i < s->count; i++) rim_drop_welded_mesh(&s->meshes[i]);
}

static int load_rim_style(const unsigned char *wldata, long wllen,
                          const uint32_t *wkeys, int nwkeys, int style,
                          N2Scene *lib, GpuMesh **gm, int *ngm,
                          const unsigned char *wtdata, long wtlen,
                          GLuint *rimtex, float fitR) {
    if (!wldata) { (void)wllen; return 0; }
    for (int i = 0; i < *ngm; i++) {
        glDeleteBuffers(1, &(*gm)[i].vbo);
        glDeleteBuffers(1, &(*gm)[i].nbo);
        glDeleteBuffers(1, &(*gm)[i].ibo);
    }
    free(*gm); *gm = NULL; *ngm = 0;
    n2_free_scene(lib);
    N2CarConfig wcfg = { 0, style, 0, style };   /* STYLEnn selects the rim */
    int n = n2_load_car(wldata, wllen, lib, wkeys, nwkeys, &wcfg);
    if (n <= 0) return 0;
    rim_drop_welded_tris(lib);
    /* Fit the aftermarket rim to THIS car's wheel. The library rims are one
       fixed tuner size (~0.42 radius); a Hummer's arch is far bigger, so the
       rim floated tiny inside it. Scale every rim submesh (they share the
       origin) to the car's own stock-wheel radius fitR, which is measured from
       the car's N2_CAR_TIRE mesh -- the same size the procedural tyre uses, so
       the two stay consistent when the draw swaps between them at speed. */
    if (fitR > 0.0f && lib->count) {
        float bb[6]; n2_mesh_bbox(&lib->meshes[0], bb);
        float rimR = 0.25f * ((bb[1]-bb[0]) + (bb[5]-bb[4]));   /* mean X-Z radius */
        if (rimR > 1e-4f) {
            float sc = fitR / rimR;
            for (int mi = 0; mi < lib->count; mi++) {
                N2Mesh *mm = &lib->meshes[mi];
                for (int v = 0; v < mm->nverts; v++)
                    for (int a = 0; a < 3; a++) mm->verts[v*5+a] *= sc;
            }
            printf("  rim fit: rimR %.3f -> car wheel %.3f (x%.2f)\n", rimR, fitR, sc);
        }
    }
    *gm = upload_scene(lib); *ngm = n;

    /* Bind the rim's own diffuse out of WHEELS/TEXTURES.BIN. Every submesh of
       a given style carries the SAME key (measured across BBS/ENKEI/VOLK/OZ/
       LEXANI/WORK/ADVAN), so one texture covers the whole rim -- hence a
       single GLuint rather than a per-mesh map like the car body uses. */
    if (*rimtex) { glDeleteTextures(1, rimtex); *rimtex = 0; }
    uint32_t tk = lib->count ? lib->meshes[0].texkey : 0;
    N2Tex rt; memset(&rt, 0, sizeof rt);
    long ar=0, ag=0, ab=0, bmn=255, bmx=0;
    if (tk && wtdata && n2_load_car_tex_by_key(wtdata, wtlen, tk, &rt)) {
        *rimtex = upload_tpk_texture_to_gpu(&rt);
        /* rim sheets are atlases with UVs in [0,1]: clamp so REPEAT wrap +
           mip filtering cannot bleed the opposite border in (same reason as
           the car body atlases above). */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        long np = (long)rt.w * rt.h;
        for (long p = 0; p < np; p++) { int b = rt.rgb[p*3+2];
            ar += rt.rgb[p*3]; ag += rt.rgb[p*3+1]; ab += b;
            if (b < bmn) bmn = b; if (b > bmx) bmx = b; }
        if (np) { ar/=np; ag/=np; ab/=np; }
        free(rt.rgb); free(rt.alpha); free(rt.dxt);
    }
    /* channel telemetry: B min/max spanning 0..255 confirms the decode is NOT
       truncating blue -- the low average is an authentic gold/bronze rim. */
    printf("  rim diffuse: texkey %08x -> %s (%dx%d) avg RGB %ld,%ld,%ld  B[min %ld..max %ld]\n",
           tk, *rimtex ? "bound" : "UNRESOLVED, procedural fallback",
           *rimtex ? rt.w : 0, *rimtex ? rt.h : 0, ar, ag, ab, bmn, bmx);
    return 1;
}

static void relaunch(const char *selfexe, const char *dataroot,
                     const char *car, const char *track) {
    char *na[8]; int a = 0;
    na[a++] = (char *)selfexe; na[a++] = (char *)dataroot;
    na[a++] = "--car";   na[a++] = (char *)car;
    na[a++] = "--track"; na[a++] = (char *)track; na[a] = NULL;
    SDL_Quit(); execvp(selfexe, na);
    _exit(1);   /* only reached if execvp itself failed */
}

/* ---- --carinfo <CAR>: GL-free vehicle geometry + texture inspection ---- */
static const char *car_cat_name(int c) {
    switch (c) {
        case N2_CAR_BODY: return "BODY";  case N2_CAR_GLASS: return "GLASS";
        case N2_CAR_LIGHT: return "LIGHT"; case N2_CAR_TIRE: return "TIRE";
        case N2_CAR_MISC: return "MISC";  case N2_CAR_BRAKELIGHT: return "BRAKELIGHT";
        case N2_CAR_MECH: return "MECH";  default: return "OTHER";
    }
}
static void car_info_walk(const unsigned char *d, long beg, long end,
                          long *nobj, long *cat) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t m = n2_u32(d + o), s = n2_u32(d + o + 4); long ds = o + 8;
        if (m == 0x80134010u) {
            char nm[48]; n2_mesh_name(d, ds, ds + s, nm, sizeof nm);
            int c = n2_car_category(d, ds, ds + s);
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + s, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + s, 0x00134B03u, idx, &ni, 64);
            long verts = 0, tris = 0;
            for (int k = 0; k < nv; k++) {
                int pad = n2_skip_filler(d + vtx[k].off, (int)vtx[k].size);
                int b = (int)vtx[k].size - pad; if (b > 0 && b % 36 == 0) verts += b / 36;
            }
            for (int k = 0; k < ni; k++) {
                const unsigned char *ib = d + idx[k].off; int ip = 0, ib2 = (int)idx[k].size;
                while (ip + 2 <= ib2 && ib[ip] == 0x11 && ib[ip+1] == 0x11) ip += 2;
                tris += (ib2 - ip) / 2 / 3;
            }
            float M[16]; int hasM = n2_obj_matrix(d, ds, ds + s, M);
            /* M111: the material metadata a soft-top rule would have to key on --
               every 0x134012 slot in stored order, and the full 0x134011 name run. */
            uint32_t sl[16]; int nsl = n2_mesh_texslots(d, ds, ds + s, sl, 16);
            char slb[160]; int sp2 = 0; slb[0] = 0;
            for (int k = 0; k < nsl && sp2 < 140; k++)
                sp2 += snprintf(slb + sp2, sizeof slb - sp2, "%08x ", sl[k]);
            printf("  %-34s %-10s verts=%-5ld tris=%-5ld  mat=%d t=(%+.3f %+.3f %+.3f)"
                   "  slots=%d [%s]\n",
                   nm[0] ? nm : "(noname)", car_cat_name(c), verts, tris,
                   hasM, M[12], M[13], M[14], nsl, slb);
            /* Running-gear parts (wheel + brake disc) carry the axle position IF
               they are modelled in place. Print their vertex bbox centre to show
               they are NOT: every one sits at the model origin, so GEOMETRY.BIN
               holds no per-axle wheelbase/track -- see the N2CarProfile audit. */
            if ((strstr(nm, "BRAKE") && !strstr(nm, "LIGHT")) || strstr(nm, "WHEEL")) {
                float b0[3] = {1e30f,1e30f,1e30f}, b1[3] = {-1e30f,-1e30f,-1e30f};
                for (int k = 0; k < nv; k++) {
                    int pad = n2_skip_filler(d + vtx[k].off, (int)vtx[k].size);
                    int nvv = ((int)vtx[k].size - pad) / 36;
                    for (int q = 0; q < nvv; q++) {
                        const unsigned char *vp = d + vtx[k].off + pad + q*36;
                        for (int a = 0; a < 3; a++) { float f; memcpy(&f, vp + a*4, 4);
                            if (f < b0[a]) b0[a] = f; if (f > b1[a]) b1[a] = f; } } }
                printf("      bbox centre (%+.3f %+.3f %+.3f)  <- at origin: no axle offset stored\n",
                       0.5f*(b0[0]+b1[0]), 0.5f*(b0[1]+b1[1]), 0.5f*(b0[2]+b1[2]));
            }
            (*nobj)++; if (c >= 0 && c < 24) cat[c]++;
        } else if (m != 0 && (m >> 28) == 8) {
            car_info_walk(d, ds, ds + s, nobj, cat);
        }
        o = ds + s;
    }
}
static int dump_car_info(const char *dataroot, const char *car) {
    char gp[1024], tp[1024];
    snprintf(gp, sizeof gp, "%s/CARS/%s/GEOMETRY.BIN", dataroot, car);
    snprintf(tp, sizeof tp, "%s/CARS/%s/TEXTURES.BIN", dataroot, car);
    long gn = 0; unsigned char *g = n2_read_file(gp, &gn);
    if (!g) { fprintf(stderr, "--carinfo: cannot read %s\n", gp); return 1; }
    long cat[24] = {0}, nobj = 0;
    printf("=== %s (%ld KB) : vehicle parts ===\n", gp, gn >> 10);
    car_info_walk(g, 0, gn, &nobj, cat);
    printf("  %ld part objects.  by category:", nobj);
    for (int i = 10; i <= 16; i++) if (cat[i]) printf(" %s=%ld", car_cat_name(i), cat[i]);
    printf("\n\n");
    long tn = 0; unsigned char *t = n2_read_file(tp, &tn);
    if (!t) { fprintf(stderr, "--carinfo: cannot read %s\n", tp); free(g); return 0; }
    uint32_t keys[512]; int nk = n2_car_tex_keys(t, tn, keys, 512);
    printf("=== %s (%ld KB) : %d car textures ===\n", tp, tn >> 10, nk);
    for (int i = 0; i < nk; i++) {
        N2Tex tex;
        if (n2_load_car_tex_by_key(t, tn, keys[i], &tex)) {
            printf("  key=0x%08X  %4dx%-4d  %s\n", keys[i], tex.w, tex.h,
                   tex.alpha ? "DXT3 (alpha)" : "DXT1");
            free(tex.rgb); free(tex.alpha); free(tex.dxt);
        } else printf("  key=0x%08X  (decode failed)\n", keys[i]);
    }
    free(g); free(t);
    return 0;
}


/* Named places to start from, measured from world objects rather than chosen:
 * each is the position of an entrance light beam in the shipped scene. The
 * free-roam start is the one exception -- five independent reference
 * recordings all begin at the same position on a heading of -10.2 degrees. */
static const struct { const char *name; float x, y; } N2_SPAWNS[] = {
    { "start",        1695.2f,  -883.6f },
    { "garage",        660.7f,  -120.2f },
    /* Airport: the welcome sign stands at (1718.6, -1002.9); backed off by
       35 m so it sits in frame ahead of the car. */
    { "airport",      1685.0f, -1009.0f },
    { "airport_sign", 1718.6f, -1002.9f },
};

/* ---- static-capture spawn safety (Milestone 80) ---------------------------
 * Used ONLY by --shot-static / --static-spawn-audit. The interactive, menu and
 * race spawn paths are left exactly as they were: this runs after them and
 * overwrites `spawn` only when a static capture asked for it.
 *
 * Coverage is triangle-exact (barycentric, same test as n2_ground_z_ref). The
 * per-mesh XY bounds are used as a broad-phase reject only -- never as the
 * coverage test -- so an overhanging roof whose AABB spans the road cannot
 * fake a hit or a miss. */
#define SS_CLEAR_M   8.0f    /* required open height above the spawn */
#define SS_LIP_M     0.5f    /* ignore surfaces within this of the road itself */

/* ROAD triangle covering (x,y) whose surface is nearest `refz` -- the deck the
 * candidate actually sits on. Taking the highest instead would measure headroom
 * from an overpass the spawn is not on, which is exactly how the old ALL spawn
 * looked "open" while standing under a terrain deck. 0 if no road covers (x,y). */
static int ss_road_z(const N2Scene *s, const float (*mbb)[4], float x, float y,
                     float refz, float *outz) {
    int found = 0; float best = 0, bestd = 1e30f;
    for (int m = 0; m < s->count; m++) {
        if (s->meshes[m].cat != N2_ROAD) continue;
        if (x < mbb[m][0] || x > mbb[m][2] || y < mbb[m][1] || y > mbb[m][3]) continue;
        const N2Mesh *me = &s->meshes[m];
        for (int t = 0; t + 2 < me->nidx; t += 3) {
            const float *a = me->verts + me->idx[t]*5;
            const float *b = me->verts + me->idx[t+1]*5;
            const float *c = me->verts + me->idx[t+2]*5;
            float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
            if (d > -1e-9f && d < 1e-9f) continue;
            float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / d;
            float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / d;
            float w = 1.0f - u - v;
            if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
            float z = u*a[2] + v*b[2] + w*c[2];
            float dz = z - refz; if (dz < 0) dz = -dz;
            if (!found || dz < bestd) { bestd = dz; best = z; found = 1; }
        }
    }
    if (found) *outz = best;
    return found;
}

/* Lowest ROAD/TERRAIN triangle strictly above `z`; 1e30f when the sky is open. */
static float ss_ceiling_above(const N2Scene *s, const float (*mbb)[4],
                              float x, float y, float z) {
    float best = 1e30f;
    for (int m = 0; m < s->count; m++) {
        int cat = s->meshes[m].cat;
        if (cat != N2_ROAD && cat != N2_TERRAIN) continue;
        if (x < mbb[m][0] || x > mbb[m][2] || y < mbb[m][1] || y > mbb[m][3]) continue;
        const N2Mesh *me = &s->meshes[m];
        for (int t = 0; t + 2 < me->nidx; t += 3) {
            const float *a = me->verts + me->idx[t]*5;
            const float *b = me->verts + me->idx[t+1]*5;
            const float *c = me->verts + me->idx[t+2]*5;
            float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
            if (d > -1e-9f && d < 1e-9f) continue;
            float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / d;
            float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / d;
            float w = 1.0f - u - v;
            if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
            float sz = u*a[2] + v*b[2] + w*c[2];
            if (sz > z + SS_LIP_M && sz < best) best = sz;
        }
    }
    return best;
}

/* Footprint patch test (Milestone 82). A candidate is only a real parking spot
 * if the car's whole footprint stands on road, not just the single vertex: the
 * M81 evidence showed a boundary vertex passing every earlier test while the
 * road ended 30 mm away and the neighbouring ground sat 29 m higher. Probes the
 * centre plus the four body corners, oriented by the heading the static camera
 * will actually use, and requires each to land on a road triangle within a
 * conservative Z band of the candidate. Coverage is the same exact barycentric
 * test as everywhere else; mesh XY bounds stay broad-phase only.
 * Fills probe[5] = {x, y, hit, z} for the audit. */
#define SS_PATCH_DZ 0.75f     /* allowed road-Z spread across the footprint */
static int ss_patch(const N2Scene *s, const float (*mbb)[4], float x, float y,
                    float cz, float head, float hl, float hw, float probe[5][4]) {
    float fx = cosf(head), fy = sinf(head);
    float rx = fy,         ry = -fx;          /* same right vector the car uses */
    const float sl[5][2] = { {0,0}, {+1,+1}, {+1,-1}, {-1,+1}, {-1,-1} };
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        float px = x + fx*hl*sl[i][0] + rx*hw*sl[i][1];
        float py = y + fy*hl*sl[i][0] + ry*hw*sl[i][1];
        float pz = 0;
        int hit = ss_road_z(s, mbb, px, py, cz, &pz);
        float dz = hit ? pz - cz : 0.0f;
        probe[i][0] = px; probe[i][1] = py;
        probe[i][2] = hit ? 1.0f : 0.0f; probe[i][3] = dz;
        if (!hit || dz > SS_PATCH_DZ || dz < -SS_PATCH_DZ) ok = 0;
    }
    return ok;
}

/* Every ROAD/TERRAIN triangle covering (x,y), lowest Z first (Milestone 81).
 * Same barycentric coverage test the ground query uses; mesh XY bounds are a
 * broad-phase reject only. Returns the number of covering triangles. */
typedef struct { float z, n[3]; int cat, mesh, tri; } SSHit;
static int ss_stack(const N2Scene *s, const float (*mbb)[4], float x, float y,
                    SSHit *out, int cap) {
    int n = 0;
    for (int m = 0; m < s->count; m++) {
        int cat = s->meshes[m].cat;
        if (cat != N2_ROAD && cat != N2_TERRAIN) continue;
        if (x < mbb[m][0] || x > mbb[m][2] || y < mbb[m][1] || y > mbb[m][3]) continue;
        const N2Mesh *me = &s->meshes[m];
        for (int t = 0; t + 2 < me->nidx && n < cap; t += 3) {
            const float *a = me->verts + me->idx[t]*5;
            const float *b = me->verts + me->idx[t+1]*5;
            const float *c = me->verts + me->idx[t+2]*5;
            float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
            if (d > -1e-9f && d < 1e-9f) continue;
            float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / d;
            float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / d;
            float w = 1.0f - u - v;
            if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
            float e1[3], e2[3], nr[3];
            for (int j = 0; j < 3; j++) { e1[j] = b[j]-a[j]; e2[j] = c[j]-a[j]; }
            nr[0]=e1[1]*e2[2]-e1[2]*e2[1]; nr[1]=e1[2]*e2[0]-e1[0]*e2[2];
            nr[2]=e1[0]*e2[1]-e1[1]*e2[0];
            float L = sqrtf(nr[0]*nr[0]+nr[1]*nr[1]+nr[2]*nr[2]); if (L < 1e-9f) L = 1;
            out[n].z = u*a[2] + v*b[2] + w*c[2];
            out[n].n[0]=nr[0]/L; out[n].n[1]=nr[1]/L; out[n].n[2]=nr[2]/L;
            out[n].cat = cat; out[n].mesh = m; out[n].tri = t/3;
            n++;
        }
    }
    for (int a = 1; a < n; a++) {          /* insertion sort by Z, few hits */
        SSHit k = out[a]; int b = a-1;
        while (b >= 0 && out[b].z > k.z) { out[b+1] = out[b]; b--; }
        out[b+1] = k;
    }
    return n;
}

/* ---- --race-audit collision attribution (Milestone 94) --------------------
 * Every persistent collision response in the M93 trace, attributed to the exact
 * mesh (and triangle, for rails) that produced it. Observation only: the wall
 * probe reads the same rects collide_walls is about to test, before it resolves;
 * the rail record is filled inside world_wall_push's own pass. Nothing here
 * writes carpos or vel. */
typedef struct {
    int kind;                 /* 0 = building AABB, 1 = road/terrain rail face */
    int mesh, tri;
    float bb[6];              /* AABB x0 y0 x1 y1 z0 z1 (buildings) */
    float nz, zlo, zhi, edged;/* rails */
    float cx, cy, cz;         /* car XY/Z at the FIRST hit of this group */
    int   wp; float segd;     /* nearest route waypoint + distance to the line */
    long  first; int count;
} M94Grp;
#define M94_MAXGRP 4096
static M94Grp m94g[M94_MAXGRP]; static int m94n = 0;
typedef struct { int grp; long f; } M94Ev;
#define M94_MAXEV 256
static M94Ev m94ev[M94_MAXEV]; static int m94nev = 0;
static float m94_prex, m94_prey, m94_prez;   /* car pose before this frame's pushes */
/* Developer overlay master switch (M122). Off at launch in EVERY build, so the
 * game opens on a clean world/car view; F1 shows the ImGui panels (debug builds)
 * and the provisional pixel-font viewport HUD (both builds) together. Nothing is
 * deleted -- while it is off the ImGui frame is not built at all, so ImGui takes
 * no mouse or keyboard and gameplay input is untouched. */
static int g_devui = 0;
/* M130: the player's authoritative vertical pose. carpos[0..1] stay owned by
   the XY arcade model; carpos[2], the chassis tilt and every wheel's own height
   now come from here. There is exactly one body-pose filter, not two. */
static PhysRideState  g_ride;
static PhysRideSupport g_sup;
static int   g_ride_ready = 0;
static float g_ride_maxdz = 0.0f;   /* largest single-frame body dZ, audit only */
static long  g_ride_air = 0, g_ride_rejhigh = 0, g_ride_rejlow = 0, g_ride_nocover = 0;
static float g_ride_maxlift = 0.0f;   /* largest penetration correction, audit  */
static float g_ride_maxdelta = 0.0f;  /* largest ACCEPTED contact delta, audit  */
static float g_ride_maxover = 0.0f;   /* worst overshoot of the contact window   */
static unsigned g_pose_t0 = 0; static long g_pose_f0 = 0;   /* pose-shot timing */
static float ra_maxwallcorr = 0.0f;   /* largest single wall correction, audit    */
static float g_ride_maximpact = 0.0f;

/* Fill the four wheel footprints for the current pose and ask the world for
   REACHABLE support under each. Returns the number of supported wheels. */
/* Exact squared distance from a point to a triangle in 3D (Ericson, Real-Time
   Collision Detection). M132-R2 needs this because nearest-VERTEX distance
   cannot see a giant backdrop triangle whose corners are kilometres away while
   its interior sweeps through the foreground -- which is precisely the shape a
   panorama sheet has. */
static float pt_tri_d2(const float p[3], const float a[3],
                       const float b[3], const float c[3]) {
    float ab[3], ac[3], ap[3];
    for (int i = 0; i < 3; i++) { ab[i]=b[i]-a[i]; ac[i]=c[i]-a[i]; ap[i]=p[i]-a[i]; }
    float d1 = ab[0]*ap[0]+ab[1]*ap[1]+ab[2]*ap[2];
    float d2 = ac[0]*ap[0]+ac[1]*ap[1]+ac[2]*ap[2];
    float q[3];
    if (d1 <= 0 && d2 <= 0) { for (int i=0;i<3;i++) q[i]=a[i]; goto done; }
    { float bp[3]; for (int i=0;i<3;i++) bp[i]=p[i]-b[i];
      float d3 = ab[0]*bp[0]+ab[1]*bp[1]+ab[2]*bp[2];
      float d4 = ac[0]*bp[0]+ac[1]*bp[1]+ac[2]*bp[2];
      if (d3 >= 0 && d4 <= d3) { for (int i=0;i<3;i++) q[i]=b[i]; goto done; }
      float vc = d1*d4 - d3*d2;
      if (vc <= 0 && d1 >= 0 && d3 <= 0) {
          float v = d1/(d1-d3); for (int i=0;i<3;i++) q[i]=a[i]+v*ab[i]; goto done; }
      float cp[3]; for (int i=0;i<3;i++) cp[i]=p[i]-c[i];
      float d5 = ab[0]*cp[0]+ab[1]*cp[1]+ab[2]*cp[2];
      float d6 = ac[0]*cp[0]+ac[1]*cp[1]+ac[2]*cp[2];
      if (d6 >= 0 && d5 <= d6) { for (int i=0;i<3;i++) q[i]=c[i]; goto done; }
      float vb = d5*d2 - d1*d6;
      if (vb <= 0 && d2 >= 0 && d6 <= 0) {
          float w = d2/(d2-d6); for (int i=0;i<3;i++) q[i]=a[i]+w*ac[i]; goto done; }
      float va = d3*d6 - d5*d4;
      if (va <= 0 && (d4-d3) >= 0 && (d5-d6) >= 0) {
          float w = (d4-d3)/((d4-d3)+(d5-d6));
          for (int i=0;i<3;i++) q[i]=b[i]+w*(c[i]-b[i]); goto done; }
      float den = 1.0f/(va+vb+vc), v = vb*den, w = vc*den;
      for (int i=0;i<3;i++) q[i]=a[i]+ab[i]*v+ac[i]*w; }
done:
    { float dx=p[0]-q[0], dy=p[1]-q[1], dz=p[2]-q[2]; return dx*dx+dy*dy+dz*dz; }
}

static int ride_gather(const N2Scene *sc, const float pos[3], float heading,
                       const VehicleWheelConfig *wc, PhysRideSupport *sup,
                       WGroundHit hit[4], WGroundHit cand[4], int verdict[4]) {
    float fx = cosf(heading), fy = sinf(heading);
    float lx = -fy, ly = fx;                     /* +y = left, matching ax/ay */
    float ax[4] = { wc->front_axle, wc->front_axle, wc->rear_axle, wc->rear_axle };
    float ay[4] = { wc->front_track*0.5f, -wc->front_track*0.5f,
                    wc->rear_track *0.5f, -wc->rear_track *0.5f };
    /* droop plus this frame's fall: the swept segment, so a fast landing cannot
       tunnel through a floor between two samples. Never a recovery reach. */
    float down = g_ride_ready ? phys_ride_reach_down(&g_ride, 1.0f/60.0f)
                              : PHYS_RIDE_REACH_DOWN;
    int n = 0;
    for (int k = 0; k < 4; k++) {
        sup->ax[k] = ax[k]; sup->ay[k] = ay[k];
        float wx = pos[0] + fx*ax[k] + lx*ay[k];
        float wy = pos[1] + fy*ax[k] + ly*ay[k];
        /* reference = where this wheel's contact sits under the CURRENT body
           pose, so reach is measured from the wheel, not from the car centre */
        float wz = g_ride_ready ? phys_ride_wheel_z(&g_ride, sup, k) : pos[2];
        int why = WWS_NOCOVER;
        int cat = world_wheel_support(sc, wx, wy, wz, PHYS_RIDE_REACH_UP, down,
                                      &hit[k], &cand[k], &why);
        verdict[k] = why;
        sup->valid[k] = cat != WSURF_NONE;
        sup->z[k] = sup->valid[k] ? hit[k].z : wz;
        if (sup->valid[k]) {
            /* how far outside its OWN window this contact was accepted: must
               never be positive, or the selector let in something unreachable */
            float dz = hit[k].z - wz;
            float over = dz > 0 ? dz - PHYS_RIDE_REACH_UP : -dz - down;
            if (over > g_ride_maxover) g_ride_maxover = over;
            float d = dz < 0 ? -dz : dz;
            if (d > g_ride_maxdelta) g_ride_maxdelta = d;
            n++;
        }
        else if (why == WWS_ABOVE) g_ride_rejhigh++;
        else if (why == WWS_BELOW) g_ride_rejlow++;
        else g_ride_nocover++;
    }
    return n;
}
static PhysVehicle g_vehicle = { 1.0f, 1.0f, 1.0f, 1.0f };   /* M121: this car */
/* The dynamics model that runs off the car's own shipped parameters. It sits
   alongside the older kinematic one rather than replacing it: --oldphys puts
   the previous behaviour back, so the two can be driven one after the other
   on the same road. Falls back on its own if the car is not in the table. */
static N2CarSetup g_carsetup;
/* ---- the new chase camera (NFSU2-flavoured) ----
   It follows the direction the car TRAVELS, not the way the nose points.
   Those are the same thing on a straight, slightly apart in a corner -- so
   the body yaws in frame and you see some flank -- and completely different
   mid-doughnut, where the nose sweeps but the travel direction crawls: the
   camera hangs off to the side and you watch the car rotate. Distance
   breathes with speed, tucks in under braking; the FOV opens a touch as the
   speed rises. --oldcam puts the previous rigid follower back. */
static int   g_newcam = 1;
static float g_cam_az, g_cam_dist = 6.0f, g_fov = 0.90f;
static int   g_cam_init = 0;
static VehModel   g_vehmodel;
static int        g_newphys = 1;      /* --oldphys clears it */
static int        g_newphys_ok = 0;   /* parameters actually loaded */
/* --telemetry FILE: one CSV row per frame of the player's car. Driving feel
   is hard to argue about from memory -- this makes it arguable from data. */
static FILE      *g_tel = NULL;
static float      g_tel_t = 0.0f;
static int   g_m107 = 0;          /* M107: three-heading menu capture, audit only */
static float g_m107_h[3];

static void m94_nearest_wp(const N2Path *ap, float x, float y, int *wp, float *segd) {
    *wp = -1; *segd = 0;
    float bd = 1e30f;
    for (int i = 0; i < ap->n; i++) {
        float dx = ap->xy[i*2]-x, dy = ap->xy[i*2+1]-y, d2 = dx*dx+dy*dy;
        if (d2 < bd) { bd = d2; *wp = i; }
    }
    if (*wp >= 0) *segd = sqrtf(bd);
}
static void m94_add(M94Grp *k, long f) {
    for (int i = 0; i < m94n; i++)
        if (m94g[i].kind == k->kind && m94g[i].mesh == k->mesh && m94g[i].tri == k->tri) {
            m94g[i].count++;
            if (m94nev < M94_MAXEV) { m94ev[m94nev].grp = i; m94ev[m94nev].f = f; m94nev++; }
            return;
        }
    if (m94n >= M94_MAXGRP) return;
    k->count = 1; k->first = f; m94g[m94n] = *k;
    if (m94nev < M94_MAXEV) { m94ev[m94nev].grp = m94n; m94ev[m94nev].f = f; m94nev++; }
    m94n++;
}
static void m94_wall(int o, int mesh, const N2Scene *s, const float *car,
                     const N2Path *ap, long f) {
    (void)o;
    M94Grp k; memset(&k, 0, sizeof k);
    k.kind = 0; k.mesh = mesh; k.tri = -1;
    const N2Mesh *m = &s->meshes[mesh];
    k.bb[0]=k.bb[1]=k.bb[4]=1e30f; k.bb[2]=k.bb[3]=k.bb[5]=-1e30f;
    for (int v = 0; v < m->nverts; v++) { float *p = m->verts + v*5;
        if(p[0]<k.bb[0])k.bb[0]=p[0]; if(p[0]>k.bb[2])k.bb[2]=p[0];
        if(p[1]<k.bb[1])k.bb[1]=p[1]; if(p[1]>k.bb[3])k.bb[3]=p[1];
        if(p[2]<k.bb[4])k.bb[4]=p[2]; if(p[2]>k.bb[5])k.bb[5]=p[2]; }
    k.cx=car[0]; k.cy=car[1]; k.cz=car[2];
    m94_nearest_wp(ap, car[0], car[1], &k.wp, &k.segd);
    m94_add(&k, f);
}
static void m94_rail(const WRailHit *rh, const N2Scene *s, float px, float py, float pz,
                     const N2Path *ap, long f) {
    (void)s;
    M94Grp k; memset(&k, 0, sizeof k);
    k.kind = 1; k.mesh = rh->mesh; k.tri = rh->tri;
    k.nz = rh->nz; k.zlo = rh->zlo; k.zhi = rh->zhi; k.edged = rh->edged;
    k.cx=px; k.cy=py; k.cz=pz;
    m94_nearest_wp(ap, px, py, &k.wp, &k.segd);
    m94_add(&k, f);
}

/* M100: locate one 0x80134010 object chunk by its 0x134011 name. Read-only. */
static int m100_nobj = 0;
static int m100_find(const unsigned char *d, long beg, long end, const char *want,
                     long *outds, uint32_t *outsz) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t mg = n2_u32(d + o), sz = n2_u32(d + o + 4);
        long ds = o + 8;
        /* advance exactly like n2_walk_meshes: magic 0 is a sized block too */
        if (mg == 0x80134010u) {
            char anm[40]; n2_mesh_name(d, ds, ds + sz, anm, sizeof anm);
            m100_nobj++;
            if (want[0] == '*') {          /* listing mode: substring filter */
                if (strstr(anm, want + 1)) printf("  [obj] %s\n", anm);
            } else if (!strcmp(anm, want)) { *outds = ds; *outsz = sz; return 1; }
        }
        else if (mg != 0 && (mg >> 28) == 8 &&
                 m100_find(d, ds, ds + sz, want, outds, outsz)) return 1;
        o = ds + sz;
    }
    return 0;
}

/* Squared XY distance from a point to a segment (M112 probe; world.c has its
 * own copy for the rail push -- this one is diagnostic and stays local). */
static float seg_d2_m112(float px,float py,float ax,float ay,float bx,float by,
                         float *ox,float *oy){
    float dx=bx-ax, dy=by-ay, L2=dx*dx+dy*dy;
    float t = L2>1e-9f ? ((px-ax)*dx+(py-ay)*dy)/L2 : 0.0f;
    if (t<0) t=0; if (t>1) t=1;
    *ox = ax+dx*t; *oy = ay+dy*t;
    float qx=px-*ox, qy=py-*oy; return qx*qx+qy*qy;
}

/* Put the player on the armed race's own start grid, facing through the start
 * line. Lifted unchanged from the --event boot path so the menu Enter and the
 * command-line arm place the car identically -- one grid rule, not two.
 * Returns 1 when a race is armed and a grid exists. */
static int race_place_on_grid(const World *w, const N2Scene *sc,
                              float carpos[3], float *heading0) {
    if (!w->race.active || w->race.ngrid <= 0 || w->race.ngate <= 0) return 0;
    /* Stand the car on a SHIPPED grid slot rather than rebuilding a position
       around gate 0. Event 4201 ships two 10-slot grids -- one per race
       direction, as the Phase 72 note recorded -- and the old rule kept only the
       slot's lateral offset, then re-anchored 15 m behind gate 0 at an XY with
       no covering ground at all, so the car started over void.
       Selection is measured, not named: a slot qualifies when the layer under it
       supports its OWN shipped Z (event 4201's usable grid agrees to 6 mm, the
       other cluster's Z sits 6.6 m above its ground), and among those the one
       nearest gate 0 wins -- that is the cluster belonging to this direction of
       travel (260 m from gate 0 versus 1953 m for the far-end grid). */
    const WGate *g0 = &w->race.gate[0];
    int best = -1; float bestd = 1e30f, bestz = 0;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < w->race.ngrid; i++) {
            float gz = w->race.grid[i][2];
            int cat = world_ground_at(sc, w->race.grid[i][0], w->race.grid[i][1],
                                      w->race.grid[i][2], &gz);
            if (cat == WSURF_NONE) continue;
            if (pass == 0) {   /* first pass: the slot must sit on its own layer */
                float dz = gz - w->race.grid[i][2]; if (dz < 0) dz = -dz;
                if (cat != WSURF_ROAD || dz > 1.0f) continue;
            }
            float dx = w->race.grid[i][0] - g0->x, dy = w->race.grid[i][1] - g0->y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestd) { bestd = d2; best = i; bestz = gz; }
        }
    if (best < 0) return 0;
    carpos[0] = w->race.grid[best][0];
    carpos[1] = w->race.grid[best][1];
    carpos[2] = bestz;                       /* the layer under the slot */
        g_ride_ready = 0;   /* M130: re-arm the ride at every placement */
    /* the records carry no orientation (their direction fields are all zero),
       so face along the event's own route: gate 0 towards the next gate. */
    if (w->race.ngate > 1) {
        const WGate *g1 = &w->race.gate[1];
        *heading0 = atan2f(g1->y - g0->y, g1->x - g0->x);
    } else *heading0 = atan2f(g0->dy, g0->dx);
    printf("race start: grid slot %d of %d at (%.3f, %.3f, %.3f) on its own %s "
           "layer, %.1f m from gate 0, heading %+.4f\n",
           best, w->race.ngrid, carpos[0], carpos[1], carpos[2], "ROAD",
           sqrtf(bestd), *heading0);
    return 1;
}

/* Inside any building/wall footprint (same rects collide_walls uses)? */
static int ss_in_wall(const float obst[][4], int nobst, float x, float y, float r) {
    for (int o = 0; o < nobst; o++)
        if (x > obst[o][0]-r && x < obst[o][2]+r &&
            y > obst[o][1]-r && y < obst[o][3]+r) return 1;
    return 0;
}

/* First waypoint at or after `from` (wrapping the closed loop) whose road layer
 * passes the M91 tests: layers come from the exact covering triangles, never
 * from world_ground_z; the whole car footprint must stand on road (ss_patch);
 * the point must be outside the collide_walls footprint (ss_in_wall); and there
 * must be SS_CLEAR_M of headroom (ss_ceiling_above). Heading is the FORWARD path
 * tangent, the direction the car will actually set off in. Returns the waypoint
 * index and writes its accepted road Z + heading, or -1 if the route has none. */
static int sl_first_safe(const N2Scene *s, const float (*mbb)[4],
                         const float *xy, int n, int from,
                         const float obst[][4], int nobst,
                         float hl, float hw, float *outz, float *outhead) {
    static SSHit hit[8192];
    for (int k = 0; k < n; k++) {
        int i = ((from + k) % n + n) % n;
        float x = xy[i*2], y = xy[i*2+1];
        int nx = (i + 1) % n;
        float head = atan2f(xy[nx*2+1] - y, xy[nx*2] - x);   /* forward tangent */
        if (ss_in_wall(obst, nobst, x, y, 1.3f)) continue;
        int nh = ss_stack(s, mbb, x, y, hit, 8192);
        for (int a = 0; a < nh; a++) {
            if (a && hit[a].z - hit[a-1].z < 0.05f) continue;   /* coincident tris */
            if (hit[a].cat != N2_ROAD) continue;
            float rz = hit[a].z, pr[5][4];
            if (!ss_patch(s, mbb, x, y, rz, head, hl, hw, pr)) continue;
            if (ss_ceiling_above(s, mbb, x, y, rz) - rz < SS_CLEAR_M) continue;
            *outz = rz; *outhead = head;
            return i;
        }
    }
    return -1;
}


/* ---- --bundle-census (Milestone 84) --------------------------------------
 * GL-free. Loads every STREAM*.BUN's geometry with the production parser (no
 * dedup, no nav, no textures -- counts are therefore per-bundle raw), then
 * answers elevation questions across bundles. Evidence only: nothing here is
 * consulted by the loader, the batcher or the spawn picker. */
typedef struct { char name[64]; N2Scene sc; float (*bb)[4]; } BCBundle;

static void bc_bounds(BCBundle *b) {
    b->bb = (float (*)[4])malloc((size_t)b->sc.count * 4 * sizeof(float));
    for (int i = 0; i < b->sc.count; i++) {
        const N2Mesh *m = &b->sc.meshes[i];
        float x0=1e30f, y0=1e30f, x1=-1e30f, y1=-1e30f;
        for (int v = 0; v < m->nverts; v++) {
            float *p = m->verts + v*5;
            if (p[0]<x0)x0=p[0]; if (p[0]>x1)x1=p[0];
            if (p[1]<y0)y0=p[1]; if (p[1]>y1)y1=p[1];
        }
        b->bb[i][0]=x0; b->bb[i][1]=y0; b->bb[i][2]=x1; b->bb[i][3]=y1;
    }
}
static void bc_mesh_bb(const N2Mesh *m, float *o) {   /* x0 x1 y0 y1 z0 z1 */
    o[0]=o[2]=o[4]=1e30f; o[1]=o[3]=o[5]=-1e30f;
    for (int v = 0; v < m->nverts; v++) {
        float *p = m->verts + v*5;
        for (int c = 0; c < 3; c++) {
            if (p[c] < o[c*2])   o[c*2]   = p[c];
            if (p[c] > o[c*2+1]) o[c*2+1] = p[c];
        }
    }
}
static int bc_family(const char *n) {          /* 1 = TEMP_, 2 = STARTFINGRAPHIC */
    if (!strncmp(n, "TEMP_", 5)) return 1;
    if (!strncmp(n, "STARTFINGRAPHIC", 15)) return 2;
    return 0;
}
static const char *bc_cat(int c) {
    return c == N2_ROAD ? "ROAD" : c == N2_TERRAIN ? "TERRAIN" :
           c == N2_SKY  ? "SKY"  : c == N2_GLOW ? "GLOW" : "OTHER";
}
/* nearest surface of one category to refz at (x,y); 1e30 if none covers it */
static float bc_surface(const BCBundle *b, int cat, float x, float y, float refz) {
    static SSHit hit[4096];
    int n = ss_stack(&b->sc, (const float (*)[4])b->bb, x, y, hit, 4096);
    float best = 1e30f, bd = 1e30f;
    for (int i = 0; i < n; i++) {
        if (hit[i].cat != cat) continue;
        float d = hit[i].z - refz; if (d < 0) d = -d;
        if (d < bd) { bd = d; best = hit[i].z; }
    }
    return best;
}

/* ---- --ground-conflict (Milestone 85) ------------------------------------
 * Control census: how far apart do two different bundles put "the ground" at
 * the same XY, under normal (non-TEMP) road, versus under L4RC's TEMP_ROAD01_
 * venue strip? Pure measurement -- nothing here is read by the loader, the
 * batcher, the spawn picker or the renderer, and no threshold is decided or
 * encoded. Raw per-bundle parse (no dedup), exactly like --bundle-census, so
 * the byte-identical re-ships between bundles are still present and visible. */
typedef struct { int sb, sm, ob, om; float sz, oz, dz; } GCSample;

/* First XY-nondegenerate triangle of the mesh, in index order -> its centroid.
 * Index order makes the pick deterministic; the centroid is by construction
 * inside the triangle, so the source bundle always covers its own sample. */
static int gc_centroid(const N2Mesh *m, float *cx, float *cy, float *cz) {
    for (int t = 0; t + 2 < m->nidx; t += 3) {
        const float *a = m->verts + m->idx[t]*5;
        const float *b = m->verts + m->idx[t+1]*5;
        const float *c = m->verts + m->idx[t+2]*5;
        float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
        if (d > -1e-9f && d < 1e-9f) continue;
        *cx = (a[0]+b[0]+c[0]) / 3.0f;
        *cy = (a[1]+b[1]+c[1]) / 3.0f;
        *cz = (a[2]+b[2]+c[2]) / 3.0f;
        return 1;
    }
    return 0;
}

/* Nearest ROAD/TERRAIN surface of bundle b to refz at (x,y). ss_stack does the
 * exact barycentric coverage test; mesh XY bounds are broad-phase only. */
static int gc_maxstack = 0;   /* deepest covering stack seen; cap-hit guard */
static int gc_nearest(const BCBundle *b, float x, float y, float refz,
                      float *oz, int *om) {
    static SSHit hit[8192];
    int n = ss_stack(&b->sc, (const float (*)[4])b->bb, x, y, hit, 8192);
    if (n > gc_maxstack) gc_maxstack = n;
    float bd = 1e30f; int found = 0;
    for (int i = 0; i < n; i++) {
        float d = hit[i].z - refz; if (d < 0) d = -d;
        if (d < bd) { bd = d; *oz = hit[i].z; *om = hit[i].mesh; found = 1; }
    }
    return found;
}

static int gc_cmp_f(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static int gc_cmp_dz(const void *a, const void *b) {   /* descending |dZ| */
    float x = ((const GCSample *)a)->dz, y = ((const GCSample *)b)->dz;
    return x > y ? -1 : x < y ? 1 : 0;
}
static float gc_pct(const float *v, int n, float p) {  /* nearest-rank */
    if (n <= 0) return 0;
    int i = (int)ceilf(p * (float)n) - 1;
    if (i < 0) i = 0; if (i >= n) i = n - 1;
    return v[i];
}

/* One cohort. mode 0 = every bundle's non-TEMP_ ROAD meshes; mode 1 = only
 * STREAML4RC's TEMP_ROAD01_ meshes. Identical measurement either way. */
/* The sampling pass, shared by M85 (--ground-conflict) and M86 (--pair-overlap)
 * so both read the identical population: mode 0 = every bundle's non-TEMP_ ROAD
 * meshes, mode 1 = STREAML4RC's TEMP_ROAD01_ meshes only. Caller frees. */
static GCSample *gc_collect(BCBundle *bc, int nb, int mode, int *nsrc,
                            int (*nomiss)[WORLD_MAXREG], int *out_nsm,
                            int *out_nmesh, int *out_nskip) {
    GCSample *sm = NULL; int nsm = 0, cap = 0;
    int nmesh = 0, nskip = 0;
    memset(nsrc, 0, WORLD_MAXREG * sizeof *nsrc);
    memset(nomiss, 0, WORLD_MAXREG * sizeof *nomiss);

    for (int b = 0; b < nb; b++) {
        for (int i = 0; i < bc[b].sc.count; i++) {
            const N2Mesh *m = &bc[b].sc.meshes[i];
            if (mode == 0) {
                if (m->cat != N2_ROAD) continue;
                if (!strncmp(m->sname, "TEMP_", 5)) continue;
            } else {
                if (strcmp(bc[b].name, "STREAML4RC")) continue;
                if (strncmp(m->sname, "TEMP_ROAD01_", 12)) continue;
            }
            nmesh++;
            float cx, cy, cz;
            if (!gc_centroid(m, &cx, &cy, &cz)) { nskip++; continue; }
            nsrc[b]++;
            for (int q = 0; q < nb; q++) {
                if (q == b) continue;
                float oz = 0; int om = -1;
                if (!gc_nearest(&bc[q], cx, cy, cz, &oz, &om)) { nomiss[b][q]++; continue; }
                if (nsm == cap) {
                    cap = cap ? cap * 2 : 4096;
                    sm = (GCSample *)realloc(sm, (size_t)cap * sizeof *sm);
                }
                float d = oz - cz; if (d < 0) d = -d;
                sm[nsm].sb = b; sm[nsm].sm = i; sm[nsm].ob = q; sm[nsm].om = om;
                sm[nsm].sz = cz; sm[nsm].oz = oz; sm[nsm].dz = d;
                nsm++;
            }
        }
    }
    *out_nsm = nsm; *out_nmesh = nmesh; *out_nskip = nskip;
    return sm;
}

static void gc_cohort(BCBundle *bc, int nb, const char *label, int mode) {
    int nsrc[WORLD_MAXREG], nomiss[WORLD_MAXREG][WORLD_MAXREG];
    int nsm = 0, nmesh = 0, nskip = 0;
    GCSample *sm = gc_collect(bc, nb, mode, nsrc, nomiss, &nsm, &nmesh, &nskip);

    printf("%s:\n", label);
    printf("  source meshes %d (sampled %d, skipped %d with no XY-valid triangle), "
           "cross-bundle probes %d\n", nmesh, nmesh - nskip, nskip, nsm);
    if (!nsm) { printf("  (no samples)\n\n"); free(sm); return; }
    printf("  %-11s %-11s %8s %8s %9s %9s %9s %9s %9s %9s %8s %8s %8s\n",
           "source", "other", "samples", "no-grnd", "min", "p50", "p90",
           "p95", "p99", "max", "<=1m", "<=5m", "<=20m");

    float *v = (float *)malloc((size_t)nsm * sizeof *v);
    for (int b = 0; b < nb; b++) {
        if (!nsrc[b]) continue;
        for (int q = 0; q < nb; q++) {
            if (q == b) continue;
            int n = 0, c1 = 0, c5 = 0, c20 = 0;
            for (int k = 0; k < nsm; k++) {
                if (sm[k].sb != b || sm[k].ob != q) continue;
                v[n++] = sm[k].dz;
                if (sm[k].dz <= 1.0f)  c1++;
                if (sm[k].dz <= 5.0f)  c5++;
                if (sm[k].dz <= 20.0f) c20++;
            }
            if (!n && !nomiss[b][q]) continue;
            if (!n) {   /* the other bundle never covers this source at all */
                printf("  %-11s %-11s %8d %8d %9s %9s %9s %9s %9s %9s "
                       "%8s %8s %8s\n", bc[b].name, bc[q].name, nsrc[b],
                       nomiss[b][q], "-","-","-","-","-","-","-","-","-");
                continue;
            }
            qsort(v, (size_t)n, sizeof *v, gc_cmp_f);
            printf("  %-11s %-11s %8d %8d %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f "
                   "%8d %8d %8d\n",
                   bc[b].name, bc[q].name, nsrc[b], nomiss[b][q],
                   v[0], gc_pct(v,n,0.50f), gc_pct(v,n,0.90f),
                   gc_pct(v,n,0.95f), gc_pct(v,n,0.99f), v[n-1],
                   c1, c5, c20);
        }
    }

    /* cohort-wide pooled distribution */
    for (int k = 0; k < nsm; k++) v[k] = sm[k].dz;
    qsort(v, (size_t)nsm, sizeof *v, gc_cmp_f);
    int p1 = 0, p5 = 0, p20 = 0;
    for (int k = 0; k < nsm; k++) {
        if (v[k] <= 1.0f)  p1++;
        if (v[k] <= 5.0f)  p5++;
        if (v[k] <= 20.0f) p20++;
    }
    printf("  %-11s %-11s %8s %8s %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f "
           "%8d %8d %8d\n", "POOLED", "*", "-", "-",
           v[0], gc_pct(v,nsm,0.50f), gc_pct(v,nsm,0.90f), gc_pct(v,nsm,0.95f),
           gc_pct(v,nsm,0.99f), v[nsm-1], p1, p5, p20);
    free(v);

    printf("  deepest covering stack in any probe: %d triangles (cap 8192)\n",
           gc_maxstack);
    printf("  largest disagreements (up to 10):\n");
    qsort(sm, (size_t)nsm, sizeof *sm, gc_cmp_dz);
    for (int k = 0; k < nsm && k < 10; k++) {
        const N2Mesh *a = &bc[sm[k].sb].sc.meshes[sm[k].sm];
        const N2Mesh *o = &bc[sm[k].ob].sc.meshes[sm[k].om];
        printf("    %-11s %-28s %9.3f  ->  %-11s %-28s %9.3f   dZ %9.3f\n",
               bc[sm[k].sb].name, a->sname[0] ? a->sname : "(unnamed)", sm[k].sz,
               bc[sm[k].ob].name, o->sname[0] ? o->sname : "(unnamed)", sm[k].oz,
               sm[k].dz);
    }
    printf("\n");
    free(sm);
}

/* ---- --pair-overlap (Milestone 86) ---------------------------------------
 * Same population, same coverage test and same deterministic centroids as M85;
 * this milestone reorganises them into an ordered bundle-pair matrix. The five
 * |dZ| bins are disjoint and partition the covered samples exactly:
 *   [0,1]  (1,5]  (5,20]  (20,50]  (50,inf)
 * No classification constant, threshold or composition rule is encoded here --
 * the pair labels are assigned in the written report from these numbers. The
 * two example rankings below are monotone scores over the measured pairs, not
 * cutoffs: nothing is included or excluded by them. */
typedef struct {
    int samples, covered, nocov;
    int b1, b5, b20, b50, bhi;
    float p50, dmin, dmax;
} GCPairStat;

/* Ascending |dZ| for one ordered pair -> caller's buffer; returns the count. */
static int gc_pair_dz(const GCSample *sm, int nsm, int b, int q, float *v) {
    int n = 0;
    for (int k = 0; k < nsm; k++)
        if (sm[k].sb == b && sm[k].ob == q) v[n++] = sm[k].dz;
    qsort(v, (size_t)n, sizeof *v, gc_cmp_f);
    return n;
}

/* Five deterministic quantile samples of one ordered pair, named. */
static void gc_examples(BCBundle *bc, const GCSample *sm, int nsm, int b, int q) {
    GCSample *e = (GCSample *)malloc((size_t)nsm * sizeof *e);
    int n = 0;
    for (int k = 0; k < nsm; k++)
        if (sm[k].sb == b && sm[k].ob == q) e[n++] = sm[k];
    if (!n) { free(e); printf("    (no covered samples)\n"); return; }
    qsort(e, (size_t)n, sizeof *e, gc_cmp_dz);       /* descending */
    for (int i = 0; i < n / 2; i++) { GCSample t = e[i]; e[i] = e[n-1-i]; e[n-1-i] = t; }
    const float qs[5] = { 0.0f, 0.25f, 0.50f, 0.75f, 1.0f };
    const char *qn[5] = { "min", "p25", "p50", "p75", "max" };
    for (int i = 0; i < 5 && i < n; i++) {
        int idx = (int)(qs[i] * (float)(n - 1) + 0.5f);
        const GCSample *x = &e[idx];
        const N2Mesh *a = &bc[x->sb].sc.meshes[x->sm];
        const N2Mesh *o = &bc[x->ob].sc.meshes[x->om];
        printf("    %-3s %-11s %-28s %9.3f  ->  %-11s %-28s %9.3f   dZ %9.3f\n",
               qn[i], bc[x->sb].name, a->sname[0] ? a->sname : "(unnamed)", x->sz,
               bc[x->ob].name, o->sname[0] ? o->sname : "(unnamed)", x->oz, x->dz);
    }
    free(e);
}

/* Ordered-pair matrix for one cohort. Fills pst; returns the samples (caller
 * frees) so the example blocks can quote them. */
static GCSample *gc_matrix(BCBundle *bc, int nb, const char *label, int mode,
                           GCPairStat pst[][WORLD_MAXREG], int *out_nsm) {
    int nsrc[WORLD_MAXREG], nomiss[WORLD_MAXREG][WORLD_MAXREG];
    int nsm = 0, nmesh = 0, nskip = 0;
    GCSample *sm = gc_collect(bc, nb, mode, nsrc, nomiss, &nsm, &nmesh, &nskip);
    memset(pst, 0, WORLD_MAXREG * WORLD_MAXREG * sizeof **pst);

    printf("%s:\n", label);
    printf("  source meshes %d (sampled %d, skipped %d), cross-bundle probes %d\n",
           nmesh, nmesh - nskip, nskip, nsm);
    printf("  counts are RAW sample counts; 'cover%%' is a percentage of source samples.\n");
    printf("  bins are disjoint and partition the covered samples: "
           "[0,1] (1,5] (5,20] (20,50] (50,inf)\n");
    printf("  %-11s %-11s %8s %8s %7s | %7s %7s %7s %7s %7s | %7s %9s\n",
           "source", "other", "samples", "covers", "cover%",
           "<=1m", "1-5m", "5-20m", "20-50m", ">50m", "no-cov", "p50|dZ|");

    float *v = (float *)malloc((size_t)(nsm ? nsm : 1) * sizeof *v);
    for (int b = 0; b < nb; b++) {
        if (!nsrc[b]) continue;
        for (int q = 0; q < nb; q++) {
            if (q == b) continue;
            GCPairStat *P = &pst[b][q];
            P->samples = nsrc[b];
            P->nocov   = nomiss[b][q];
            int n = gc_pair_dz(sm, nsm, b, q, v);
            P->covered = n;
            for (int k = 0; k < n; k++) {
                float d = v[k];
                if      (d <= 1.0f)  P->b1++;
                else if (d <= 5.0f)  P->b5++;
                else if (d <= 20.0f) P->b20++;
                else if (d <= 50.0f) P->b50++;
                else                 P->bhi++;
            }
            P->p50  = n ? gc_pct(v, n, 0.50f) : 0;
            P->dmin = n ? v[0] : 0;
            P->dmax = n ? v[n-1] : 0;
            if (!n && !P->nocov) continue;
            printf("  %-11s %-11s %8d %8d %6.1f%% | %7d %7d %7d %7d %7d | %7d ",
                   bc[b].name, bc[q].name, P->samples, P->covered,
                   100.0 * (double)P->covered / (double)P->samples,
                   P->b1, P->b5, P->b20, P->b50, P->bhi, P->nocov);
            if (n) printf("%9.3f\n", P->p50); else printf("%9s\n", "-");
            if (P->b1 + P->b5 + P->b20 + P->b50 + P->bhi != P->covered ||
                P->covered + P->nocov != P->samples)
                printf("      !! partition check FAILED for this row\n");
        }
    }
    free(v);
    printf("\n");
    *out_nsm = nsm;
    return sm;
}

/* ---- --event-refs (Milestone 87) -----------------------------------------
 * Does the shipped event data itself say which bundles belong together?
 * Census only: it drives the PRODUCTION parser (world_load_events) over a World
 * whose region list is filled in from the same res_list_tracks discovery the
 * engine uses, then re-reads the identical 0x3414c leaves at the identical
 * 272-byte stride purely to print the header bytes the parser does not consume.
 * No new chunk schema, no geometry inference, no manifest, no fallback. */
typedef struct {
    char stem[8];              /* region stem, e.g. "L4RA" */
    char cat[256];             /* catalog file the loader would open */
    char cat2[256];            /* next Paths file in the same dir, for cross-check */
    int  nrec, nrec2;          /* 0x3414c record counts in each */
    int  idmatch;              /* 1 = both files' id lists identical */
    const unsigned char *d; long len;
    long off;                  /* first 0x3414c leaf offset in cat */
} ERRegion;

/* The loader's own file search: first existing ROUTES<stem>/Paths<id>.bin. */
static int er_find_paths(const char *troot, const char *stem, int skip, char *out, int cap) {
    int seen = 0;
    for (int fi = 0; fi < 4000; fi++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/ROUTES%s/Paths%04d.bin", troot, stem, 4000 + fi);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fclose(f);
        if (seen++ < skip) continue;
        snprintf(out, (size_t)cap, "%s", p);
        return 4000 + fi;
    }
    out[0] = 0;
    return -1;
}

int main(int argc, char **argv) {
    collide_walls_selftest();
    phys_selftest();
    world_ground_selftest();
    audio_selftest();
    /* Point at your own NFSU2 install/data directory (contains TRACKS/, CARS/).
       Usage: nfsu2 [DATA_DIR] [options]
         --car NAME       car folder under CARS/ (default HUMMER)
         --track NAME     STREAM .BUN under TRACKS/, or ALL = diagnostic union (default).
                          The bundles overlap as route/event supersets; ALL is
                          not a supported playable open-world composition.
         --circuit PATH   circuit Paths .bin under TRACKS/ (default ROUTESL4RF/Paths4602.bin)
         --shot out.png   render one frame and exit
         --carinfo CAR    dump CAR's part list + texture catalog and exit (GL-free) */
    const char *selfexe = argv[0];   /* for the menu's track-switch re-exec */
    const char *dataroot = ".", *shot = NULL, *objdump = NULL, *carinfo = NULL;
    const char *xaudit = NULL;   /* --transform-audit REGION: GL-free placement forensics */
    float shotyaw = 1e9f;        /* --shot-yaw DEG: fixed capture heading (M132) */
    /* M132-R diagnostics. tier: 0 = baseline (old fixed 700 m, no vista),
       1 = ordinary (fog-derived range, no vista), 2 = full (range + vista). */
    int tier = 1;   /* production default: ordinary. full is opt-in (M132-R2) */
    const char *poseshot = NULL; /* --pose-shot PREFIX: freeze the production
                                    start pose and capture four yaws there */
    float posefrac = -1.0f;      /* --pose-frac F: seed the SAME first-safe
                                    start-line search F of the way round the
                                    authored route, so a sample pose is a real
                                    route waypoint on supported road (M133) */
    float poseseed[2] = {0,0};   /* --pose-seed X Y: run the SAME safe-road
                                    selector from an explicit seed, so two
                                    builds can be compared at one pose (M133) */
    int markrepair = 0;          /* --mark-repaired (M133): diagnostic highlight */
    int nansweep = 0;            /* --nan-sweep (M133-R): scan every emitted
                                    mesh in the loaded region(s) for a non-
                                    finite or out-of-N2_VERT_SANE vertex */
    int lodcensus = 0;           /* --lod-census (M133): structural detail-tier census */
    int lsaudit = 0;             /* --local-scene-audit (M133): source->screen
                                    attribution for everything near the car */
    int citypose = 0;            /* --city-pose: seed the existing safe-road
                                    selector at the built-up centroid */
    const char *sshot = NULL;    /* --shot-static: deterministic region-local world capture */
    int passmode = 0;            /* --shot-static-pass: 0 full, 1 opaque, 2 glow, 3 sky */
    int passbatch = -1, passbatch2 = -1;   /* opaque:N or opaque:A-B -- sky + that range (M79) */
    const char *sspawn = NULL;   /* --static-spawn-audit TRACK (M80) */
    const char *sstack = NULL; float stx = 0, sty = 0;   /* --surface-stack TRACK X Y (M81) */
    int bcensus = 0;   /* --bundle-census (M84) */
    int gconf = 0;     /* --ground-conflict (M85): cross-bundle ground control */
    int poverlap = 0;  /* --pair-overlap (M86): ordered bundle-pair overlap matrix */
    int erefs = 0;     /* --event-refs (M87): event -> bundle reference census */
    const char *daudit = NULL;  /* --drive-audit PREFIX (M88): scripted interactive drive */
    const char *raudit = NULL;  /* --race-audit PREFIX (M89): menu -> Enter -> countdown -> race */
    int facecensus = 0;  /* --face-census (M131): wall-face vertical span histogram */
    int slaudit = 0;   /* --startline-audit (M91): every route waypoint x every ROAD layer */
    const char *smaudit = NULL; uint32_t smkey = 0;  /* --smear-audit PREFIX TEXKEYHEX (M98) */
    const char *tpkrec = NULL; uint32_t tpkkey = 0;  /* --tpk-record PREFIX KEYHEX (M99) */
    const char *matdump = NULL, *matmesh = NULL;     /* --mesh-material PREFIX NAME (M100) */
    int fbcensus = 0;   /* --fallback-census (M102): who still takes the old path */
    int camat = 0; float camx = 0, camy = 0;   /* --cam-at X Y: aim a static capture */
    int spawn_set = 0; float spawn_x = 0, spawn_y = 0;   /* --spawn NAME */
    static N2Paint pal[512]; int npal = 0;        /* body paints, from GLOBALB */
    /* (350Z default wired below once carname is final) */
    float paint_rgb[3] = { 0.70f, 0.70f, 0.75f }; /* until the record is read */
    const char *paint_name = NULL;                /* --paint NAME */
    /* Showroom default for the default car: the attribute DB pairs the 350Z's
       stock record with MATERIAL REGPAINTORANGE (145,35,17). Until the per-car
       default-paint chain is parsed end to end, pin the one car we boot into;
       --paint still overrides. */
    const char *paint_default_350z = "REGPAINTORANGE";
    const char *g_sky_name = NULL;     /* --sky NAME */
    int   no_post = 0;                 /* --no-post: skip the tone pass */
    float post_amount = 1.0f;          /* --post N: scale it */
    int   post_on = 0;                 /* the pass is live this frame */
/* Resolving has to happen after everything is drawn and before anything reads
   the pixels, and there are several places that read them. A macro keeps the
   three lines identical at every one of them rather than trusting a single
   spot in a 7000-line frame to be on the live path -- an earlier attempt put
   it in what looked like the right place and it never ran. */
#define POST_RESOLVE() do { if (post_on) { \
        pp_end_and_draw(&quad, post_amount); glUseProgram(rp.prog); post_on = 0; } \
    } while (0)
    int head_set = 0;  float head_deg = 0;               /* --heading DEG */
    const char *b02probe = NULL;   /* --b02-probe NAME (M104): stride/field search */
    const char *shaudit = NULL;    /* --showcase-audit PREFIX [X Y] (M106) */
    int cprobe = 0; float cpx = 0, cpy = 0;   /* --cover-probe X Y (M110v) */
    const char *wprobe = NULL; float wpx = 0, wpy = 0, wpz = 0; int wpzset = 0;
    int gaudit = 0;   /* --grid-audit EVENTID (M118) */
    int rband = 0;    /* --rail-band (M119): whole-region rail-predicate survey */
    int rtrace = 0;   /* --race-trace (M120): gate targeting + route forensics */
    int fleetc = 0;   /* --fleet-census (M121): measured geometry of every car */
    const char *vcmpA = NULL, *vcmpB = NULL;   /* --vehicle-compare A B (M121) */
    int shovr = 0; float shx = 0, shy = 0;
    const char *baudit = NULL, *bmesh = NULL;   /* --batch-audit REGION MESHNAME */
    int vcensus = 0;   /* --vista-census: measure every region's backdrop candidates */
    int mapaudit = 0;  /* --map-audit: texture resolution + production distance-cull census */
    float vthresh = 3000.0f;   /* --vista-census [METRES]: candidate XY-span floor */
    int rendermode = 0, daylight = 0;   /* --rendermode 0..3 / --daylight: headless F3 matrix */
    const char *carname = "350Z", *trackname = "ALL";
    const char *circuit = "ROUTESL4RF/Paths4602.bin"; int explicit_circuit = 0;
    int want_event_id = 0;   /* --event <id>: boot straight into a race event */
    int shotframes = 40;     /* --frames N: how long --shot drives before the grab */
    int want_laps = 2;       /* --laps N: race distance for --event */
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--shot")    && i+1 < argc) shot      = argv[++i];
        else if (!strcmp(argv[i], "--car")     && i+1 < argc) carname   = argv[++i];
        else if (!strcmp(argv[i], "--oldphys")) g_newphys = 0;   /* previous kinematic model */
        else if (!strcmp(argv[i], "--oldcam"))  g_newcam = 0;    /* previous chase camera */
        else if (!strcmp(argv[i], "--telemetry") && i+1 < argc) {
            g_tel = fopen(argv[++i], "w");
            if (g_tel) fprintf(g_tel, "t,x,y,z,speed_kmh,heading_deg,yaw_rate_degs,"
                                      "slip_deg,wheel_slip,throttle,steer,handbrake,gear,rpm,"
                                      "pitch_deg,roll_deg\n");
        }
        else if (!strcmp(argv[i], "--event")   && i+1 < argc) want_event_id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames")  && i+1 < argc) shotframes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--laps")    && i+1 < argc) want_laps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--track")   && i+1 < argc) trackname = argv[++i];
        else if (!strcmp(argv[i], "--circuit") && i+1 < argc) { circuit = argv[++i]; explicit_circuit = 1; }
        else if (!strcmp(argv[i], "--objdump") && i+1 < argc) objdump = argv[++i];
        else if (!strcmp(argv[i], "--carinfo") && i+1 < argc) carinfo = argv[++i];
        else if (!strcmp(argv[i], "--transform-audit") && i+1 < argc) xaudit = argv[++i];
        else if (!strcmp(argv[i], "--batch-audit") && i+2 < argc) { baudit = trackname = argv[++i]; bmesh = argv[++i]; }
        else if (!strcmp(argv[i], "--rendermode") && i+1 < argc) rendermode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--daylight")) daylight = 1;
        else if (!strcmp(argv[i], "--shot-static") && i+1 < argc) sshot = argv[++i];
        else if (!strcmp(argv[i], "--shot-yaw") && i+1 < argc)
            shotyaw = (float)atof(argv[++i]) * 3.14159265f / 180.0f;
        else if (!strcmp(argv[i], "--tier") && i+1 < argc) {
            const char *t = argv[++i];
            if      (!strcmp(t, "baseline")) tier = 0;
            else if (!strcmp(t, "ordinary")) tier = 1;
            else if (!strcmp(t, "full"))     tier = 2;
            else { fprintf(stderr, "unknown --tier '%s' -- expected one of:\n"
                           "  baseline   fixed 700 m world range, no vistas\n"
                           "  ordinary   fog-derived world range (default)\n"
                           "  full       ordinary + backdrop vistas "
                           "[EXPERIMENTAL: known opaque-sheet artifacts]\n", t);
                   return 2; }
        }
        else if (!strcmp(argv[i], "--pose-shot") && i+1 < argc) poseshot = argv[++i];
        else if (!strcmp(argv[i], "--city-pose")) citypose = 1;
        else if (!strcmp(argv[i], "--struct-pose")) citypose = 2;
        else if (!strcmp(argv[i], "--repair-pose")) citypose = 3;
        else if (!strcmp(argv[i], "--pose-seed") && i+2 < argc) {
            citypose = 4; poseseed[0] = (float)atof(argv[++i]);
            poseseed[1] = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--mark-repaired")) markrepair = 1;
        else if (!strcmp(argv[i], "--pose-frac") && i+1 < argc)
            posefrac = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--local-scene-audit")) lsaudit = 1;
        else if (!strcmp(argv[i], "--tex-audit")) g_world_texaudit = 1;
        else if (!strcmp(argv[i], "--lod-census")) lodcensus = 1;
        else if (!strcmp(argv[i], "--nan-sweep")) nansweep = 1;
        else if (!strcmp(argv[i], "--local-scene-objects")) lsaudit = 2;
        else if (!strcmp(argv[i], "--static-spawn-audit") && i+1 < argc) sspawn = trackname = argv[++i];
        else if (!strcmp(argv[i], "--surface-stack") && i+3 < argc) {
            sstack = trackname = argv[++i]; stx = (float)atof(argv[++i]); sty = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--shot-static-pass") && i+2 < argc) {
            const char *pm = argv[++i]; sshot = argv[++i];
            if (!strncmp(pm, "opaque:", 7)) {
                passmode = 1; passbatch = atoi(pm + 7);
                const char *dash = strchr(pm + 7, '-');
                passbatch2 = dash ? atoi(dash + 1) : passbatch;
            }
            else passmode = !strcmp(pm,"opaque") ? 1 : !strcmp(pm,"glow") ? 2 :
                            !strcmp(pm,"sky")    ? 3 : 0;
            if (strcmp(pm,"full") && passmode == 0)
                fprintf(stderr, "shot-static-pass: unknown pass '%s', using full\n", pm);
        }
        else if (!strcmp(argv[i], "--bundle-census")) bcensus = 1;
        else if (!strcmp(argv[i], "--ground-conflict")) gconf = 1;
        else if (!strcmp(argv[i], "--pair-overlap")) poverlap = 1;
        else if (!strcmp(argv[i], "--event-refs")) erefs = 1;
        else if (!strcmp(argv[i], "--drive-audit") && i+1 < argc) daudit = argv[++i];
        else if (!strcmp(argv[i], "--race-audit")  && i+1 < argc) raudit = argv[++i];
            else if (!strcmp(argv[i], "--face-census")) facecensus = 1;
        else if (!strcmp(argv[i], "--startline-audit")) slaudit = 1;
        else if (!strcmp(argv[i], "--fallback-census")) { fbcensus = 1; n2_m102 = 1; }
        else if (!strcmp(argv[i], "--rail-census")) world_rail_census = 1;
        else if (!strcmp(argv[i], "--grid-audit") && i+1 < argc) gaudit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rail-band")) rband = 1;
        else if (!strcmp(argv[i], "--race-trace")) rtrace = 1;
        else if (!strcmp(argv[i], "--fleet-census")) fleetc = 1;
        else if (!strcmp(argv[i], "--vehicle-compare") && i+2 < argc) {
            vcmpA = argv[++i]; vcmpB = argv[++i]; }
        else if (!strcmp(argv[i], "--wall-probe") && i+3 < argc) {
            wprobe = argv[++i]; wpx = (float)atof(argv[++i]); wpy = (float)atof(argv[++i]);
            if (i+1 < argc && (isdigit((unsigned char)argv[i+1][0]) ||
                               ((argv[i+1][0]=='-' || argv[i+1][0]=='+') &&
                                (isdigit((unsigned char)argv[i+1][1]) || argv[i+1][1]=='.')) ||
                               argv[i+1][0]=='.'))
                { wpz = (float)atof(argv[++i]); wpzset = 1; } }
        else if (!strcmp(argv[i], "--cover-probe") && i+2 < argc) {
            cprobe = 1; cpx = (float)atof(argv[++i]); cpy = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--showcase-audit") && i+1 < argc) {
            shaudit = argv[++i];
            if (i+2 < argc && (argv[i+1][0] == '-' ? isdigit((unsigned char)argv[i+1][1])
                                                   : isdigit((unsigned char)argv[i+1][0]))) {
                shovr = 1; shx = (float)atof(argv[++i]); shy = (float)atof(argv[++i]);
            }
            raudit = shaudit;   /* reuse the menu-camera frame at rshot == 2 */
        }
        else if (!strcmp(argv[i], "--b02-probe") && i+1 < argc) {
            b02probe = argv[++i]; fbcensus = 1; n2_m102 = 1; }
        /* --world2: assemble the scene from instance records instead of from
           models with their matrix baked in. See world2.c. */
        else if (!strcmp(argv[i], "--world2")) world2_on = 1;
        /* --no-post / --post N: the bloom-and-tone pass, and how much of it. */
        /* --paint NAME: body paint by material name (METPAINTSILVER, ...). */
        else if (!strcmp(argv[i], "--paint") && i+1 < argc) paint_name = argv[++i];
        /* --sky sunrise|sunset|night: which shipped sky to hang overhead. */
        else if (!strcmp(argv[i], "--sky") && i+1 < argc) g_sky_name = argv[++i];
        else if (!strcmp(argv[i], "--no-post")) no_post = 1;
        else if (!strcmp(argv[i], "--post") && i+1 < argc)
            post_amount = (float)atof(argv[++i]);
        /* --spawn NAME: start at a named place; --spawn list prints them. */
        else if (!strcmp(argv[i], "--spawn") && i+1 < argc) {
            const char *nm = argv[++i];
            int nsp = (int)(sizeof N2_SPAWNS / sizeof N2_SPAWNS[0]);
            if (!strcmp(nm, "list")) {
                printf("spawn points:\n");
                for (int k = 0; k < nsp; k++)
                    printf("  %-13s X=%9.1f  Y=%9.1f\n",
                           N2_SPAWNS[k].name, N2_SPAWNS[k].x, N2_SPAWNS[k].y);
                return 0;
            }
            int found = 0;
            for (int k = 0; k < nsp; k++)
                if (!strcmp(nm, N2_SPAWNS[k].name)) {
                    spawn_set = 1; spawn_x = N2_SPAWNS[k].x;
                    spawn_y = N2_SPAWNS[k].y; found = 1; break;
                }
            if (!found) fprintf(stderr, "--spawn: no such place '%s'"
                                        " (try --spawn list)\n", nm);
        }
        /* --heading DEG: set the starting heading outright. */
        else if (!strcmp(argv[i], "--heading") && i+1 < argc) {
            head_set = 1; head_deg = (float)atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--cam-at") && i+2 < argc) {
            camat = 1; camx = (float)atof(argv[++i]); camy = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--mesh-material") && i+2 < argc) {
            matdump = argv[++i]; matmesh = argv[++i]; }
        else if (!strcmp(argv[i], "--tpk-record") && i+2 < argc) {
            tpkrec = argv[++i]; tpkkey = (uint32_t)strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(argv[i], "--smear-audit") && i+2 < argc) {
            smaudit = argv[++i]; smkey = (uint32_t)strtoul(argv[++i], NULL, 16);
            raudit = smaudit;   /* reuse the M89 menu->Enter->countdown path */
        }
        else if (!strcmp(argv[i], "--vista-census")) { vcensus = 1;
            if (i+1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9') vthresh = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--map-audit")) mapaudit = 1;
        else dataroot = argv[i];
    }
    /* --shot-static (Milestone 77): a STATIC world capture. Reuses --shot's
       capture tail, but every dynamic subsystem below is gated off it — no
       circuit load, no AI, no start-line snap, no autopilot, no throttle, no
       collision, no physics step. The camera is seeded from the final car
       position (never from a stale `spawn`) and settles for a fixed count, so
       two runs of the same command produce the same pixels. */
    if (poseshot) raudit = poseshot;   /* reuse the production menu->Enter->start */
    const int sstatic = sshot != NULL;
    if (daudit) {   /* M88: 2 s idle + 10 s forward + 20 s forward/steer + 5 s brake/coast */
        static char dashot[1024];
        snprintf(dashot, sizeof dashot, "%s_end.png", daudit);
        shot = dashot; shotframes = 2220;
    }
    if (sstatic) { shot = sshot; shotframes = 8; }   /* fixed settle: reproducible */
    if (carinfo) return dump_car_info(dataroot, carinfo);   /* inspect one car, GL-free, exit */
    if (vcmpA) {   /* M121: same fixed input trace, two cars, flat ROAD. GL-free. */
        const char *nmv[2] = { vcmpA, vcmpB };
        PhysVehicle pv[2];
        printf("MILESTONE: 121  vehicle comparison on flat ROAD (identical inputs)\n");
        for (int c = 0; c < 2; c++) {
            char gp2[1024]; snprintf(gp2, sizeof gp2, "%s/CARS/%s/GEOMETRY.BIN", dataroot, nmv[c]);
            long gl2 = 0; unsigned char *gd2 = n2_read_file(gp2, &gl2);
            pv[c] = phys_vehicle_from_geometry(0,0,0,0,0,0);
            if (!gd2) { printf("  %s: cannot read %s\n", nmv[c], gp2); continue; }
            N2Scene cs = {0}; N2CarConfig cc0 = {0,0,0,0}; uint32_t nk2[1]; int dmy = 0;
            if (n2_load_car(gd2, gl2, &cs, nk2, 0, &cc0) > 0) {
                N2CarProfile cp; memset(&cp, 0, sizeof cp);
                n2_car_profile(&cs, nmv[c], WHEEL_SEED_FRONTF, WHEEL_SEED_REARF,
                               WHEEL_SEED_TRACKF, n2_car_brake_radius(gd2, 0, gl2), &cp);
                VehicleWheelConfig wc = wheel_config_for(nmv[c], &cp, NULL, 0, &dmy);
                float bl = cp.body[1]-cp.body[0], bw = cp.body[3]-cp.body[2],
                      bh = cp.body[5]-cp.body[4];
                pv[c] = phys_vehicle_from_geometry(bl, bw, bh,
                          wc.front_axle - wc.rear_axle, wc.front_track, cp.wheel_w);
                printf("  %-10s body %.3f x %.3f x %.3f  wb %.3f track %.3f tyre %.3f\n",
                       nmv[c], bl, bw, bh, wc.front_axle - wc.rear_axle,
                       wc.front_track, cp.wheel_w);
                printf("             factors accel %.4f brake %.4f steer %.4f lat %.4f\n",
                       pv[c].accel, pv[c].brake, pv[c].steer, pv[c].lat);
            }
            for (int k = 0; k < cs.count; k++) { free(cs.meshes[k].verts);
                free(cs.meshes[k].idx); free(cs.meshes[k].vcol); }
            free(cs.meshes); free(gd2);
        }
        printf("  %-10s %10s %10s %12s %12s %10s\n", "car", "0-100 s", "top km/h",
               "turn rad m", "yaw 2s deg", "slide");
        for (int c = 0; c < 2; c++) {
            /* 0-100 and settled top speed, straight line */
            float p[3]={0,0,0}, v[2]={0,0}, h=0, sp=0; int t100=-1;
            for (int t = 1; t <= 60*60; t++) {
                phys_car_step(p, v, &h, &sp, 1.0f, 0, 0, &PHYS_SURF_ROAD, &pv[c]);
                if (t100 < 0 && PHYS_KMH(sp) >= 100.0f) t100 = t;
            }
            float top = PHYS_KMH(sp);
            /* steady 50 km/h, full lock for 2 s: yaw swept and the radius it implies */
            float p2[3]={0,0,0}, v2[2], h2=0, s2=0, slide=0;
            v2[0]=50.0f/3.6f/PHYS_TICKRATE; v2[1]=0;
            float h0 = h2; double dist = 0;
            for (int t = 0; t < 120; t++) {
                float bx = p2[0], by = p2[1];
                slide = phys_car_step(p2, v2, &h2, &s2, 1.0f, 1.0f, 0,
                                      &PHYS_SURF_ROAD, &pv[c]);
                dist += sqrt((double)((p2[0]-bx)*(p2[0]-bx) + (p2[1]-by)*(p2[1]-by)));
            }
            float yaw = h2 - h0;
            float rad = yaw > 1e-4f ? (float)(dist / yaw) : 0.0f;
            printf("  %-10s %10.3f %10.2f %12.2f %12.2f %10.5f\n", nmv[c],
                   t100 < 0 ? -1.0f : t100/60.0f, top, rad, yaw*57.29578f, slide);
        }
        printf("\n");
        return 0;
    }
    if (fleetc) {   /* M121: measured geometry of every drivable car, GL-free, exit */
        enum { FC_MAXCARS = 128 };
        static char cl[FC_MAXCARS][64]; int sel2 = 0;
        int nc = res_list_cars(dataroot, cl, FC_MAXCARS, "", &sel2);
        printf("MILESTONE: 121  fleet geometry census  (%d cars)\n", nc);
        printf("  %-14s %7s %7s %7s %9s %7s %7s %8s %8s\n", "car", "len", "wid", "hgt",
               "volume", "wheelR", "wheelW", "axleWB", "trackF");
        static float vol[FC_MAXCARS], wbv[FC_MAXCARS], trk[FC_MAXCARS], twd[FC_MAXCARS];
        int n = 0;
        for (int i = 0; i < nc; i++) {
            char gp2[1024]; snprintf(gp2, sizeof gp2, "%s/CARS/%s/GEOMETRY.BIN", dataroot, cl[i]);
            long gl2 = 0; unsigned char *gd2 = n2_read_file(gp2, &gl2);
            if (!gd2) continue;
            N2Scene cs = {0}; N2CarConfig cc0 = { 0, 0, 0, 0 };
            uint32_t nk2[1];
            int nmc = n2_load_car(gd2, gl2, &cs, nk2, 0, &cc0);
            if (nmc > 0) {
                N2CarProfile cp; memset(&cp, 0, sizeof cp);
                n2_car_profile(&cs, cl[i], WHEEL_SEED_FRONTF, WHEEL_SEED_REARF,
                               WHEEL_SEED_TRACKF, n2_car_brake_radius(gd2, 0, gl2), &cp);
                float L = cp.body[1]-cp.body[0], W = cp.body[3]-cp.body[2],
                      H = cp.body[5]-cp.body[4];
                VehicleWheelConfig wc = wheel_config_for(cl[i], &cp, NULL, 0, &sel2);
                float wb = wc.front_axle - wc.rear_axle;
                printf("  %-14s %7.3f %7.3f %7.3f %9.3f %7.3f %7.3f %8.3f %8.3f\n",
                       cl[i], L, W, H, L*W*H, cp.wheel_r, cp.wheel_w, wb, wc.front_track);
                vol[n] = L*W*H; wbv[n] = wb; trk[n] = wc.front_track; twd[n] = cp.wheel_w; n++;
            }
            for (int k = 0; k < cs.count; k++) {
                free(cs.meshes[k].verts); free(cs.meshes[k].idx); free(cs.meshes[k].vcol); }
            free(cs.meshes); free(gd2);
        }
        { /* medians: sort copies, take the middle */
            float *a[4] = { vol, wbv, trk, twd };
            const char *nmz[4] = { "body volume", "axle wheelbase", "front track", "tyre width" };
            for (int k = 0; k < 4; k++) {
                for (int p2 = 1; p2 < n; p2++) { float v = a[k][p2]; int q = p2-1;
                    while (q >= 0 && a[k][q] > v) { a[k][q+1] = a[k][q]; q--; } a[k][q+1] = v; }
                printf("  median %-16s %.4f   (min %.4f max %.4f, n=%d)\n",
                       nmz[k], a[k][n/2], a[k][0], a[k][n-1], n);
            }
        }
        return 0;
    }
    char carp[1024], cartexp[1024], pathp[1024], troot[1024];
    snprintf(troot,   sizeof troot,   "%s/TRACKS", dataroot);
    snprintf(carp,    sizeof carp,    "%s/CARS/%s/GEOMETRY.BIN", dataroot, carname);
    snprintf(cartexp, sizeof cartexp, "%s/CARS/%s/TEXTURES.BIN", dataroot, carname);
    snprintf(pathp,   sizeof pathp,   "%s/TRACKS/%s", dataroot, circuit);

    /* --bundle-census (Milestone 84): every TEMP_/STARTFINGRAPHIC mesh in every
       bundle, then the elevation relationship between those footprints and the
       rest of the world. GL-free; exits when done. */
    if (bcensus) {
        static char regs[WORLD_MAXREG][64]; int dummy = 0;
        int nr = res_list_tracks(troot, regs, WORLD_MAXREG, "", &dummy);
        static BCBundle bc[WORLD_MAXREG]; int nb = 0;
        static uint32_t nokeys[1];
        printf("MILESTONE: 84  bundle-census  (raw parser output: no dedup, no textures)\n\n");
        for (int r = 0; r < nr; r++) {
            char rp[1024]; snprintf(rp, sizeof rp, "%s/%s.BUN", troot, regs[r]);
            long rl = 0; unsigned char *rd = n2_read_file(rp, &rl);
            if (!rd) { fprintf(stderr, "bundle-census: cannot read %s\n", rp); continue; }
            snprintf(bc[nb].name, sizeof bc[nb].name, "%s", regs[r]);
            n2_load_scene(rd, rl, &bc[nb].sc, nokeys, 0);
            bc_bounds(&bc[nb]);
            free(rd);
            nb++;
        }

        printf("== family members ==\n");
        printf("%-11s %-30s %-9s %-8s %-34s %8s\n",
               "bundle","name","class","category","XY / Z bounds","tris");
        for (int b = 0; b < nb; b++)
            for (int i = 0; i < bc[b].sc.count; i++) {
                const N2Mesh *m = &bc[b].sc.meshes[i];
                if (!bc_family(m->sname)) continue;
                float o[6]; bc_mesh_bb(m, o);
                char bb[64];
                snprintf(bb, sizeof bb, "[%.0f %.0f][%.0f %.0f][%.1f %.1f]",
                         o[0],o[1], o[2],o[3], o[4],o[5]);
                printf("%-11s %-30s %-9s %-8s %-34s %8d\n",
                       bc[b].name, m->sname, n2_scen_name(m->scen),
                       bc_cat(m->cat), bb, m->nidx/3);
            }

        printf("\n== per-bundle summary ==\n");
        printf("%-11s %6s %6s %8s %8s %8s %8s  %s\n",
               "bundle","TEMP_","START","meshes","ROAD","TERRAIN","BUILD","family combined XY / Z extent");
        for (int b = 0; b < nb; b++) {
            int nt = 0, ns = 0, nroad = 0, nterr = 0, nbld = 0;
            float e[6] = {1e30f,-1e30f,1e30f,-1e30f,1e30f,-1e30f};
            for (int i = 0; i < bc[b].sc.count; i++) {
                const N2Mesh *m = &bc[b].sc.meshes[i];
                if (m->cat == N2_ROAD) nroad++;
                else if (m->cat == N2_TERRAIN) nterr++;
                if (m->scen == N2_SC_BUILDING) nbld++;
                int f = bc_family(m->sname);
                if (!f) continue;
                if (f == 1) nt++; else ns++;
                float o[6]; bc_mesh_bb(m, o);
                for (int c = 0; c < 3; c++) {
                    if (o[c*2]   < e[c*2])   e[c*2]   = o[c*2];
                    if (o[c*2+1] > e[c*2+1]) e[c*2+1] = o[c*2+1];
                }
            }
            char ex[80];
            if (nt + ns) snprintf(ex, sizeof ex, "[%.0f %.0f][%.0f %.0f][%.1f %.1f]",
                                  e[0],e[1], e[2],e[3], e[4],e[5]);
            else snprintf(ex, sizeof ex, "-");
            printf("%-11s %6d %6d %8d %8d %8d %8d  %s\n", bc[b].name, nt, ns,
                   bc[b].sc.count, nroad, nterr, nbld, ex);
        }

        printf("\n== venue footprint vs the rest of the world ==\n"
               "(ROAD/TERRAIN by exact barycentric coverage; BUILDING presence by XY bbox,\n"
               " labelled as such -- a building is not a ground surface)\n");
        int printed = 0;
        for (int b = 0; b < nb && printed < 16; b++)
            for (int i = 0; i < bc[b].sc.count && printed < 16; i++) {
                const N2Mesh *m = &bc[b].sc.meshes[i];
                if (!bc_family(m->sname) || m->cat != N2_ROAD) continue;
                if ((printed % 4) && bc_family(m->sname) == 1) { printed++; continue; }
                float o[6]; bc_mesh_bb(m, o);
                float px = (o[0]+o[1])*0.5f, py = (o[2]+o[3])*0.5f, rz = 0;
                if (!ss_road_z(&bc[b].sc, (const float (*)[4])bc[b].bb, px, py,
                               (o[4]+o[5])*0.5f, &rz)) { printed++; continue; }
                printf("\n%s  %s  at (%.1f, %.1f)\n", bc[b].name, m->sname, px, py);
                printf("   supporting road Z            %.3f\n", rz);
                float ownt = bc_surface(&bc[b], N2_TERRAIN, px, py, rz);
                if (ownt < 1e29f)
                    printf("   terrain in SAME bundle       yes, nearest Z %.3f (%+.3f)\n",
                           ownt, ownt - rz);
                else
                    printf("   terrain in SAME bundle       NO\n");
                for (int q = 0; q < nb; q++) {
                    if (q == b) continue;
                    float orz = bc_surface(&bc[q], N2_ROAD,    px, py, rz);
                    float otz = bc_surface(&bc[q], N2_TERRAIN, px, py, rz);
                    int nbld = 0; float bz0 = 1e30f, bz1 = -1e30f;
                    for (int k = 0; k < bc[q].sc.count; k++) {
                        if (bc[q].sc.meshes[k].scen != N2_SC_BUILDING) continue;
                        if (px < bc[q].bb[k][0] || px > bc[q].bb[k][2] ||
                            py < bc[q].bb[k][1] || py > bc[q].bb[k][3]) continue;
                        float ob[6]; bc_mesh_bb(&bc[q].sc.meshes[k], ob);
                        if (ob[4] < bz0) bz0 = ob[4];
                        if (ob[5] > bz1) bz1 = ob[5];
                        nbld++;
                    }
                    if (orz > 1e29f && otz > 1e29f && !nbld) continue;
                    printf("   other bundle %-11s ", bc[q].name);
                    if (orz < 1e29f) printf("ROAD %.3f (%+.3f)  ", orz, orz - rz);
                    if (otz < 1e29f) printf("TERRAIN %.3f (%+.3f)  ", otz, otz - rz);
                    if (nbld) printf("BUILDING x%d bbox z[%.1f %.1f] (%+.3f)",
                                     nbld, bz0, bz1, bz0 - rz);
                    printf("\n");
                }
                printed++;
            }

        /* aggregate over EVERY family road footprint, not just the printed sample */
        printf("\n== all family road footprints: same-bundle terrain? ==\n");
        for (int b = 0; b < nb; b++) {
            int nfoot = 0, withterr = 0, withother = 0;
            float gmin = 1e30f, gmax = -1e30f;
            for (int i = 0; i < bc[b].sc.count; i++) {
                const N2Mesh *m = &bc[b].sc.meshes[i];
                if (!bc_family(m->sname) || m->cat != N2_ROAD) continue;
                float o2[6]; bc_mesh_bb(m, o2);
                float px = (o2[0]+o2[1])*0.5f, py = (o2[2]+o2[3])*0.5f, rz = 0;
                if (!ss_road_z(&bc[b].sc, (const float (*)[4])bc[b].bb, px, py,
                               (o2[4]+o2[5])*0.5f, &rz)) continue;
                nfoot++;
                if (bc_surface(&bc[b], N2_TERRAIN, px, py, rz) < 1e29f) withterr++;
                for (int q = 0; q < nb; q++) {
                    if (q == b) continue;
                    float t = bc_surface(&bc[q], N2_TERRAIN, px, py, rz);
                    if (t > 1e29f) continue;
                    withother++;
                    if (t - rz < gmin) gmin = t - rz;
                    if (t - rz > gmax) gmax = t - rz;
                    break;
                }
            }
            if (!nfoot) continue;
            printf("%-11s %d footprints: same-bundle terrain %d, other-bundle terrain %d, "
                   "other-bundle terrain dZ range %+.2f .. %+.2f m\n",
                   bc[b].name, nfoot, withterr, withother, gmin, gmax);
        }

        /* M83 follow-up: the condo XY inside L4RA alone */
        printf("\n== L4RA condo stack at (1288.592, 9.076), L4RA ALONE ==\n");
        for (int b = 0; b < nb; b++) {
            if (strcmp(bc[b].name, "STREAML4RA")) continue;
            static SSHit hit[4096];
            int n = ss_stack(&bc[b].sc, (const float (*)[4])bc[b].bb,
                             1288.592f, 9.076f, hit, 4096);
            printf("%d covering ROAD/TERRAIN triangle(s)\n", n);
            for (int a = 0; a < n; a++) {
                const N2Mesh *me = &bc[b].sc.meshes[hit[a].mesh];
                float ob[6]; bc_mesh_bb(me, ob);
                printf("  %10.3f %-8s %-30s mesh %5d tri %5d  n(%5.2f %5.2f %5.2f)"
                       "  src [%.0f %.0f][%.0f %.0f][%.1f %.1f]\n",
                       hit[a].z, bc_cat(hit[a].cat),
                       me->sname[0] ? me->sname : "(unnamed)", hit[a].mesh, hit[a].tri,
                       hit[a].n[0], hit[a].n[1], hit[a].n[2],
                       ob[0],ob[1], ob[2],ob[3], ob[4],ob[5]);
            }
            for (int i = 0; i < bc[b].sc.count; i++) {
                const N2Mesh *me = &bc[b].sc.meshes[i];
                if (strncmp(me->sname, "XB_UC_SMALLCONDOBBASE", 21)) continue;
                float ob[6]; bc_mesh_bb(me, ob);
                printf("  condo mesh %5d %-30s [%.0f %.0f][%.0f %.0f][%.1f %.1f] %d tris\n",
                       i, me->sname, ob[0],ob[1], ob[2],ob[3], ob[4],ob[5], me->nidx/3);
            }
        }
        return 0;
    }

    /* --ground-conflict (Milestone 85): the control measurement. How far apart
       do two bundles place the ground at the same XY under ordinary road, and
       does L4RC's TEMP_ROAD01_ venue strip sit outside that normal spread?
       Census only -- GL-free, exits when done, changes nothing. */
    if (gconf) {
        static char regs[WORLD_MAXREG][64]; int dummy = 0;
        int nr = res_list_tracks(troot, regs, WORLD_MAXREG, "", &dummy);
        static BCBundle bc[WORLD_MAXREG]; int nb = 0;
        static uint32_t nokeys[1];
        for (int r = 0; r < nr; r++) {
            char rp[1024]; snprintf(rp, sizeof rp, "%s/%s.BUN", troot, regs[r]);
            long rl = 0; unsigned char *rd = n2_read_file(rp, &rl);
            if (!rd) { fprintf(stderr, "ground-conflict: cannot read %s\n", rp); continue; }
            snprintf(bc[nb].name, sizeof bc[nb].name, "%s", regs[r]);
            n2_load_scene(rd, rl, &bc[nb].sc, nokeys, 0);
            bc_bounds(&bc[nb]);
            free(rd);
            nb++;
        }
        printf("MILESTONE: 85\n");
        printf("(raw per-bundle parse: no dedup, no textures. One deterministic\n"
               " centroid per source mesh = first XY-nondegenerate triangle in index\n"
               " order. Coverage in the queried bundle is exact barycentric over its\n"
               " ROAD+TERRAIN triangles; mesh XY bounds are broad-phase only.\n"
               " |dZ| in metres, percentiles by nearest rank. %d bundles.)\n\n", nb);
        gc_cohort(bc, nb, "NORMAL-ROAD DISTRIBUTION", 0);
        gc_cohort(bc, nb, "TEMP-ROAD DISTRIBUTION", 1);
        return 0;
    }

    /* --pair-overlap (Milestone 86): the same measurement reorganised per
       ordered bundle pair, because M85 showed |dZ| alone cannot discriminate.
       Census only -- GL-free, exits when done, changes nothing. */
    if (poverlap) {
        static char regs[WORLD_MAXREG][64]; int dummy = 0;
        int nr = res_list_tracks(troot, regs, WORLD_MAXREG, "", &dummy);
        static BCBundle bc[WORLD_MAXREG]; int nb = 0;
        static uint32_t nokeys[1];
        for (int r = 0; r < nr; r++) {
            char rp[1024]; snprintf(rp, sizeof rp, "%s/%s.BUN", troot, regs[r]);
            long rl = 0; unsigned char *rd = n2_read_file(rp, &rl);
            if (!rd) { fprintf(stderr, "pair-overlap: cannot read %s\n", rp); continue; }
            snprintf(bc[nb].name, sizeof bc[nb].name, "%s", regs[r]);
            n2_load_scene(rd, rl, &bc[nb].sc, nokeys, 0);
            bc_bounds(&bc[nb]);
            free(rd);
            nb++;
        }
        printf("MILESTONE: 86\n");
        printf("(raw per-bundle parse: no dedup, no textures. Deterministic centroid per\n"
               " source mesh = first XY-nondegenerate triangle in index order. Coverage in\n"
               " the queried bundle is exact barycentric over its ROAD+TERRAIN triangles;\n"
               " mesh XY bounds are broad-phase only. %d bundles.)\n\n", nb);

        static GCPairStat pn[WORLD_MAXREG][WORLD_MAXREG];
        static GCPairStat pt[WORLD_MAXREG][WORLD_MAXREG];
        int nsmn = 0, nsmt = 0;
        GCSample *smn = gc_matrix(bc, nb, "NORMAL-ROAD PAIR MATRIX", 0, pn, &nsmn);
        GCSample *smt = gc_matrix(bc, nb, "TEMP-ROAD PAIR MATRIX", 1, pt, &nsmt);

        printf("SYMMETRIC PAIR EVIDENCE (normal-road cohort):\n");
        printf("  Interpretation labels are assigned in the written report, not here --\n"
               "  no classification constant exists in this code. These are the numbers\n"
               "  the label is read off: coverage each way, median |dZ| each way, and the\n"
               "  share of covered samples inside 1 m each way.\n");
        printf("  %-25s %9s %9s %9s %9s %9s %9s\n", "pair",
               "A->B cov%", "B->A cov%", "A->B p50", "B->A p50",
               "A->B <=1m%", "B->A <=1m%");
        for (int a = 0; a < nb; a++)
            for (int b = a + 1; b < nb; b++) {
                const GCPairStat *P = &pn[a][b], *Q = &pn[b][a];
                if (!P->samples && !Q->samples) continue;
                char nm[32]; snprintf(nm, sizeof nm, "%s/%s",
                                      bc[a].name + 6, bc[b].name + 6);
                printf("  %-25s %8.1f%% %8.1f%% %9.3f %9.3f %9.1f%% %9.1f%%\n", nm,
                       P->samples ? 100.0*(double)P->covered/(double)P->samples : 0.0,
                       Q->samples ? 100.0*(double)Q->covered/(double)Q->samples : 0.0,
                       P->covered ? (double)P->p50 : 0.0,
                       Q->covered ? (double)Q->p50 : 0.0,
                       P->covered ? 100.0*(double)P->b1/(double)P->covered : 0.0,
                       Q->covered ? 100.0*(double)Q->b1/(double)Q->covered : 0.0);
            }

        /* Example blocks. The two rankings are monotone scores over the measured
           pairs -- they order the pairs, they do not admit or reject any. */
        int ib = -1, iq = -1, ob = -1, oq = -1, tb = -1, tq = -1;
        double bestn = -1, bestd = -1, bestt = -1;
        for (int b = 0; b < nb; b++)
            for (int q = 0; q < nb; q++) {
                const GCPairStat *P = &pn[b][q];
                if (b == q || !P->samples || !P->covered) continue;
                double cov = (double)P->covered / (double)P->samples;
                double sn = cov / (1.0 + (double)P->p50);   /* near-identical */
                double sd = cov * (double)P->p50;           /* overlapping, disagreeing */
                if (sn > bestn) { bestn = sn; ib = b; iq = q; }
                if (sd > bestd) { bestd = sd; ob = b; oq = q; }
                const GCPairStat *T = &pt[b][q];
                if (T->samples && T->covered) {
                    double tc = (double)T->covered / (double)T->samples;
                    if (tc > bestt) { bestt = tc; tb = b; tq = q; }
                }
            }
        printf("\nEXAMPLES (five deterministic quantiles of the pair's |dZ|):\n");
        if (ib >= 0) {
            printf("  1. strongest near-identical pair  %s -> %s  "
                   "(rank score coverage/(1+p50) = %.4f)\n",
                   bc[ib].name, bc[iq].name, bestn);
            gc_examples(bc, smn, nsmn, ib, iq);
        }
        if (ob >= 0) {
            printf("  2. strongest overlapping-but-disagreeing pair  %s -> %s  "
                   "(rank score coverage*p50 = %.3f)\n",
                   bc[ob].name, bc[oq].name, bestd);
            gc_examples(bc, smn, nsmn, ob, oq);
        }
        if (tb >= 0) {
            printf("  3. L4RC TEMP_ROAD01_ pair with greatest coverage  %s -> %s  "
                   "(coverage %.1f%%)\n", bc[tb].name, bc[tq].name, 100.0*bestt);
            gc_examples(bc, smt, nsmt, tb, tq);
        }
        free(smn); free(smt);
        return 0;
    }

    /* --event-refs (Milestone 87): trace the shipped event records to whatever
       they can identify, using the production parser only. GL-free, exits. */
    if (erefs) {
        static char regs[WORLD_MAXREG][64]; int dummy = 0;
        int nr = res_list_tracks(troot, regs, WORLD_MAXREG, "", &dummy);
        static World ew;
        static ERRegion er[WORLD_MAXREG];
        ew.nreg = nr;
        for (int r = 0; r < nr; r++)
            snprintf(ew.rgn[r].name, sizeof ew.rgn[r].name, "%s", regs[r]);

        printf("MILESTONE: 87\n\n");
        printf("PARSER PATH:\n");
        printf("  res_list_tracks(TRACKS/) -> region names STREAM<stem>\n");
        printf("  world_load_events(w, TRACKS/)   [src/world.c:422, unmodified]\n");
        printf("    for each loaded region: stem = name+6\n");
        printf("    open the FIRST existing TRACKS/ROUTES<stem>/Paths<4000+i>.bin\n");
        printf("    n2_find_leaves(..., 0x0003414c, ..., cap 4)\n");
        printf("    each leaf = size/272 records; per record the parser consumes:\n");
        printf("      +0 u16 id  +2 u8 npoly  +3 u8 circuit  +5 u8 len100m\n");
        printf("      +8 .. +272  33 * (f32 x, f32 y) outline polygon\n");
        printf("    reject unless id >= 4000 and 3 <= npoly <= 33\n");
        printf("    e->reg is written from the DIRECTORY STEM, not from any record field\n");
        printf("  consumed downstream: world_set_mode/world_race_start use e->poly,\n");
        printf("    e->bb, e->circuit, e->node0/node1; e->reg is display-only; e->id\n");
        printf("    is matched by ev_by_id() and by --event <id>.\n");
        printf("  bytes in the 272-byte record NOT consumed anywhere: +4 (u8), +6..7 (u16).\n\n");

        /* discovery, mirroring the loader, plus the next file for a cross-check */
        for (int r = 0; r < nr; r++) {
            const char *rn = regs[r];
            const char *stem = strncmp(rn, "STREAM", 6) ? rn : rn + 6;
            snprintf(er[r].stem, sizeof er[r].stem, "%s", stem);
            er_find_paths(troot, stem, 0, er[r].cat,  sizeof er[r].cat);
            er_find_paths(troot, stem, 1, er[r].cat2, sizeof er[r].cat2);
            er[r].nrec = er[r].nrec2 = 0; er[r].idmatch = -1; er[r].off = -1;
            if (er[r].cat[0]) {
                long l = 0; unsigned char *d = n2_read_file(er[r].cat, &l);
                if (d) {
                    N2Leaf lf[4]; int nl = 0;
                    n2_find_leaves(d, 0, l, 0x0003414cu, lf, &nl, 4);
                    for (int L = 0; L < nl; L++) er[r].nrec += (int)lf[L].size / 272;
                    er[r].d = d; er[r].len = l; er[r].off = nl ? lf[0].off : -1;
                }
            }
            if (er[r].cat2[0]) {
                long l2 = 0; unsigned char *d2 = n2_read_file(er[r].cat2, &l2);
                if (d2) {
                    N2Leaf lf[4]; int nl = 0;
                    n2_find_leaves(d2, 0, l2, 0x0003414cu, lf, &nl, 4);
                    for (int L = 0; L < nl; L++) er[r].nrec2 += (int)lf[L].size / 272;
                    if (er[r].d && er[r].nrec == er[r].nrec2 && er[r].off >= 0 && nl) {
                        er[r].idmatch = 1;
                        for (int k = 0; k < er[r].nrec; k++) {
                            const unsigned char *a = er[r].d + er[r].off + k*272;
                            const unsigned char *b = d2 + lf[0].off + k*272;
                            if ((a[0]|(a[1]<<8)) != (b[0]|(b[1]<<8))) { er[r].idmatch = 0; break; }
                        }
                    } else er[r].idmatch = 0;
                    free(d2);
                }
            }
        }

        printf("CATALOG DISCOVERY (per region, exactly what the loader opens):\n");
        printf("  %-6s %-34s %6s  %-34s %6s  %s\n", "stem", "catalog file the loader opens",
               "recs", "next Paths file in that dir", "recs", "same id list?");
        for (int r = 0; r < nr; r++)
            printf("  %-6s %-34s %6d  %-34s %6d  %s\n", er[r].stem,
                   er[r].cat[0]  ? er[r].cat  + (int)strlen(troot) + 1 : "(none)", er[r].nrec,
                   er[r].cat2[0] ? er[r].cat2 + (int)strlen(troot) + 1 : "(none)", er[r].nrec2,
                   er[r].idmatch < 0 ? "-" : er[r].idmatch ? "yes" : "NO");
        printf("\n");

        int nev = world_load_events(&ew, troot);

        printf("\nEVENT REFERENCE TABLE:\n");
        printf("  'resolved bundle' is TRACKS/STREAM<stem>.BUN where <stem> is the\n"
               "  ROUTES<stem>/ directory the catalog was read from -- an exact 1:1\n"
               "  filename mapping, verified below. 'id->Paths' checks that the record's\n"
               "  own id names a file that exists in that same directory.\n");
        printf("  %4s %6s %-8s %-26s %5s %5s %6s %6s %8s %-20s %s\n",
               "idx", "id(+0)", "type(+3)", "source Paths file", "np+2", "ln+5",
               "raw+4", "raw+6", "id->Paths", "resolved bundle", "other id fields");
        int per_stem[WORLD_MAXREG]; memset(per_stem, 0, sizeof per_stem);
        int b4hist[256]; memset(b4hist, 0, sizeof b4hist);
        int b6nonzero = 0, idpaths_ok = 0, idpaths_bad = 0;
        int xtab[2][2]; memset(xtab, 0, sizeof xtab);   /* +4 flag vs +3 circuit */
        int idlo[WORLD_MAXREG], idhi[WORLD_MAXREG];
        for (int k = 0; k < WORLD_MAXREG; k++) { idlo[k] = 1 << 30; idhi[k] = -1; }
        for (int i = 0; i < nev; i++) {
            const WEvent *e = &ew.ev[i];
            int r = -1;
            for (int k = 0; k < nr; k++) if (!strcmp(er[k].stem, e->reg)) { r = k; break; }
            unsigned b4 = 0, b6 = 0;
            if (r >= 0 && er[r].d && er[r].off >= 0) {
                const unsigned char *b = er[r].d + er[r].off + per_stem[r]*272;
                b4 = b[4]; b6 = (unsigned)(b[6] | (b[7] << 8));
            }
            b4hist[b4 & 255]++; if (b6) b6nonzero++;
            if (b4 < 2 && e->circuit < 2) xtab[b4][e->circuit]++;
            if (r >= 0) { if (e->id < idlo[r]) idlo[r] = e->id;
                          if (e->id > idhi[r]) idhi[r] = e->id; }
            char pth[1024], bun[128];
            snprintf(pth, sizeof pth, "%s/ROUTES%s/Paths%04d.bin", troot, e->reg, e->id);
            FILE *f = fopen(pth, "rb"); int ok = f != NULL; if (f) fclose(f);
            ok ? idpaths_ok++ : idpaths_bad++;
            snprintf(bun, sizeof bun, "STREAM%s.BUN", e->reg);
            printf("  %4d %6d %-8s %-26s %5d %5d %6u %6u %8s %-20s %s\n",
                   i, e->id, e->circuit ? "circuit" : "sprint",
                   r >= 0 && er[r].cat[0] ? er[r].cat + (int)strlen(troot) + 1 : "?",
                   e->npoly, e->len100m, b4, b6, ok ? "exists" : "MISSING", bun,
                   "dir stem only");
            if (r >= 0) per_stem[r]++;
        }

        printf("\nRESOLVED BUNDLE-SET GROUPS:\n");
        int one = 0, many = 0, none = 0;
        for (int r = 0; r < nr; r++) {
            if (!per_stem[r]) continue;
            char pth[1024]; snprintf(pth, sizeof pth, "%s/STREAM%s.BUN", troot, er[r].stem);
            FILE *f = fopen(pth, "rb"); int ex = f != NULL; if (f) fclose(f);
            printf("  {STREAM%s.BUN}  %d event(s)   bundle file %s\n",
                   er[r].stem, per_stem[r], ex ? "exists" : "MISSING");
            one += per_stem[r];
        }
        for (int r = 0; r < nr; r++)
            if (!per_stem[r])
                printf("  (no events)    STREAM%s.BUN   catalog file: %s\n",
                       er[r].stem, er[r].cat[0] ? er[r].cat : "none shipped");
        printf("  events resolving to exactly one bundle : %d\n", one);
        printf("  events resolving to multiple bundles   : %d\n", many);
        printf("  events with no resolvable bundle ref   : %d\n", none);
        printf("  id -> ROUTES<stem>/Paths<id>.bin exists: %d, missing: %d\n",
               idpaths_ok, idpaths_bad);

        printf("\nUNRESOLVED CANDIDATE FIELDS (raw values):\n");
        printf("  +4 u8   distinct values seen:");
        for (int k = 0; k < 256; k++) if (b4hist[k]) printf(" %d(x%d)", k, b4hist[k]);
        printf("\n          why unresolved: no shipped table, hash or filename in the data\n"
               "          root takes this as a key; nothing in the engine reads it.\n");
        printf("          cross-tab against the parsed circuit flag (+3):\n");
        printf("            +4=0 & sprint %3d | +4=0 & circuit %3d\n", xtab[0][0], xtab[0][1]);
        printf("            +4=1 & sprint %3d | +4=1 & circuit %3d\n", xtab[1][0], xtab[1][1]);
        printf("          it is binary and correlated with, but not equal to, +3 --\n"
               "          either way it indexes nothing that names a bundle.\n");
        printf("  +6 u16  non-zero in %d of %d records\n", b6nonzero, nev);
        printf("          why unresolved: same -- no lookup exists to resolve it.\n");
        printf("  outline polygon (+8..+272): world XY only. Resolving it to bundles\n"
               "          would be geometry inference, which this milestone forbids.\n");
        printf("\n  id ranges actually observed per region directory:\n");
        for (int r = 0; r < nr; r++)
            if (per_stem[r]) printf("    %-6s %d..%d (%d events)\n",
                                    er[r].stem, idlo[r], idhi[r], per_stem[r]);
        for (int r = 0; r < nr; r++) if (er[r].d) free((void *)er[r].d);
        return 0;
    }

    /* --vista-census: measure large-span candidates in every shipped bundle and
       print the evidence the impostor classifier rests on, then exit. GL-free
       (Milestone 76). */
    if (vcensus) {
        static char regs[WORLD_MAXREG][64]; int dummy = 0;
        int nr = res_list_tracks(troot, regs, WORLD_MAXREG, "", &dummy);
        for (int r = 0; r < nr; r++) {
            char rp[1024]; snprintf(rp, sizeof rp, "%s/%s.BUN", troot, regs[r]);
            long rl = 0; unsigned char *rd = n2_read_file(rp, &rl);
            if (!rd) { fprintf(stderr, "vista-census: cannot read %s\n", rp); continue; }
            n2_vista_census(rd, rl, regs[r], vthresh, r == 0);
            free(rd);
        }
        return 0;
    }

    /* --transform-audit REGION: walk the bundle with the production object
       boundaries and print per-object placement evidence, then exit. GL-free,
       read-only, feeds nothing downstream (Milestone 74). */
    if (xaudit) {
        char xp[1024]; snprintf(xp, sizeof xp, "%s/%s.BUN", troot, xaudit);
        long xlen = 0; unsigned char *xd = n2_read_file(xp, &xlen);
        if (!xd) { fprintf(stderr, "transform-audit: cannot read %s\n", xp); return 1; }
        n2_transform_audit(xd, xlen, xaudit);
        free(xd);
        return 0;
    }

    static World world;
    /* Instance placement only reads the sections within range, so it has to be
       told where that is before the world is loaded. */
    if (spawn_set && world2_on) {
        world_inst_x = spawn_x; world_inst_y = spawn_y; world_inst_r = 600.0f;
    }
    int nm = world_load(&world, troot, trackname);

    /* --objdump: write the exact post-dedup, world-space scene the GPU batches
       are built from to a .obj, then exit. Independent parser-vs-renderer check:
       if this opens coherent in Blender/MeshLab, the assembly is sound and any
       on-screen mangling is strictly the GL path or camera. Runs before any GL. */
    if (objdump) {
        FILE *of = fopen(objdump, "w");
        if (!of) { fprintf(stderr, "objdump: cannot write %s\n", objdump); return 1; }
        const N2Scene *s = &world.scene;
        fprintf(of, "# OpenUG2 scene export  track=%s  meshes=%d (post-dedup, world space)\n",
                trackname, s->count);
        long base = 1, tv = 0, tf = 0;   /* OBJ is 1-indexed */
        for (int mi = 0; mi < s->count; mi++) {
            const N2Mesh *m = &world.scene.meshes[mi];
            fprintf(of, "o m%d_%.31s\n", mi, m->sname[0] ? m->sname : "mesh");
            for (int v = 0; v < m->nverts; v++) {
                const float *p = m->verts + v*5;
                fprintf(of, "v %.3f %.3f %.3f\n", p[0], p[1], p[2]);
                fprintf(of, "vt %.4f %.4f\n", p[3], p[4]);
            }
            for (int t = 0; t + 2 < m->nidx; t += 3) {
                long a = base + m->idx[t], b = base + m->idx[t+1], c = base + m->idx[t+2];
                fprintf(of, "f %ld/%ld %ld/%ld %ld/%ld\n", a,a, b,b, c,c);
            }
            base += m->nverts; tv += m->nverts; tf += m->nidx/3;
        }
        fclose(of);
        printf("objdump: wrote %s  (%ld verts, %ld tris, %d meshes)\n",
               objdump, tv, tf, s->count);
        return 0;
    }
    if (!nm) { fprintf(stderr, "no track data\n  (pass your NFSU2 data dir: nfsu2 /path/to/data)\n"); return 1; }
    N2Scene scene = world.scene;   /* shares world.scene.meshes */
    printf("loaded %d submeshes from %d region(s)\n", nm, world.nreg);
    printf("terrain texture TRN_GRASSC: %s\n", world.have_grass ? "ok" : "-");
    int nroad = 0, nterr = 0;
    for (int i = 0; i < nm; i++) {
        if (scene.meshes[i].cat == N2_ROAD) nroad++;
        else if (scene.meshes[i].cat == N2_TERRAIN) nterr++;
    }
    printf("categories: road=%d terrain=%d other=%d\n", nroad, nterr, nm-nroad-nterr);
    /* scenery semantics from each mesh's own asset name (0x134011) */
    {   int sc[8] = {0}, named = 0;
        for (int i = 0; i < nm; i++) { int c2 = scene.meshes[i].scen;
            if (c2 < 8) sc[c2]++; if (c2) named++; }
        printf("scenery: %d/%d named (%.1f%%)", named, nm, nm ? 100.0*named/nm : 0.0);
        for (int c2 = 1; c2 <= N2_SC_OTHER; c2++)
            if (sc[c2]) printf("  %s=%d", n2_scen_name(c2), sc[c2]);
        printf("\n");
        for (int i = 0, shown = 0; i < nm && shown < 3; i++)
            if (scene.meshes[i].scen == N2_SC_PROP) { float bb[6];
                n2_mesh_bbox(&scene.meshes[i], bb); shown++;
                printf("  join sample: %-26s [%s]  bbox X[%.1f..%.1f] Y[%.1f..%.1f] Z[%.1f..%.1f]\n",
                       scene.meshes[i].sname, n2_scen_name(scene.meshes[i].scen),
                       bb[0],bb[1],bb[2],bb[3],bb[4],bb[5]); }
    }

    /* scene centroid + extent for the camera */
    float cx=0,cy=0,cz=0; long cnt=0;
    float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
    for (int i=0;i<nm;i++) for (int v=0;v<scene.meshes[i].nverts;v++) {
        float *p = scene.meshes[i].verts + v*5;
        cx+=p[0]; cy+=p[1]; cz+=p[2]; cnt++;
        for (int c=0;c<3;c++){ if(p[c]<mn[c])mn[c]=p[c]; if(p[c]>mx[c])mx[c]=p[c]; }
    }
    if (cnt){ cx/=cnt; cy/=cnt; cz/=cnt; }
    float maxr = 1;
    for (int c=0;c<3;c++) if ((mx[c]-mn[c])/2 > maxr) maxr = (mx[c]-mn[c])/2;

    /* densest built-up spot: bin building (N2_OTHER) mesh positions into a grid
       and take the peak cell. The geometry centroid is often an open hole (the
       city spreads in a ring), so this is where to start the car for a view
       that actually frames buildings. Falls back to the centroid if no props. */
    float densx = cx, densy = cy;
    {
        static int hist[40][40]; memset(hist, 0, sizeof hist);
        float gw = (mx[0]-mn[0])/40.0f, gh = (mx[1]-mn[1])/40.0f;
        if (gw > 1e-3f && gh > 1e-3f) {
            for (int i=0;i<nm;i++) if (scene.meshes[i].cat == N2_OTHER && scene.meshes[i].nverts) {
                float *p = scene.meshes[i].verts;          /* first vertex ~ mesh location */
                int gx=(int)((p[0]-mn[0])/gw), gy=(int)((p[1]-mn[1])/gh);
                if (gx<0)gx=0; if (gx>39)gx=39; if (gy<0)gy=0; if (gy>39)gy=39;
                hist[gx][gy]++;
            }
            int best=0, bx=20, by=20;
            for (int gx=0;gx<40;gx++) for (int gy=0;gy<40;gy++)
                if (hist[gx][gy] > best) { best=hist[gx][gy]; bx=gx; by=gy; }
            if (best > 0) { densx = mn[0]+(bx+0.5f)*gw; densy = mn[1]+(by+0.5f)*gh; }
        }
    }
    printf("dense build-up centre: (%.0f,%.0f)\n", densx, densy);

    /* building collision footprints — the car is kept out of these */
    #define MAXOBST 32768   /* whole city worth of building footprints */
    static float obst[MAXOBST][4];
    static int obstsrc[MAXOBST];   /* source mesh per rect, same collection pass */
    static float obstz[MAXOBST][2];/* its Z span, measured in that same pass */
    int nobst = phys_collect_walls(&scene, obst, obstsrc, obstz, MAXOBST);
    printf("collision obstacles: %d buildings\n", nobst);
    if (facecensus) {
        /* M131 evidence: how tall is a near-vertical face on a collision mesh?
           The threshold that separates a seam from a barrier has to come out of
           this distribution, not out of a guess. Counted per triangle, using
           the same |nz| < 0.30 criterion the narrow phase applies. */
        static const float B[] = {0.05f,0.10f,0.20f,0.30f,0.50f,0.75f,0.90f,1.20f,
                                  2.00f,4.00f,8.00f,1e9f};
        long hist[12]; memset(hist, 0, sizeof hist);
        long nface = 0, nmesh = 0;
        for (int o = 0; o < nobst; o++) {
            int mi = obstsrc[o];
            if (mi < 0 || mi >= scene.count) continue;
            const N2Mesh *m = &scene.meshes[mi]; nmesh++;
            for (int t = 0; t + 2 < m->nidx; t += 3) {
                const float *A = m->verts + m->idx[t]*5;
                const float *Bv = m->verts + m->idx[t+1]*5;
                const float *C = m->verts + m->idx[t+2]*5;
                float e1[3], e2[3], n[3];
                for (int a = 0; a < 3; a++) { e1[a]=Bv[a]-A[a]; e2[a]=C[a]-A[a]; }
                n[0]=e1[1]*e2[2]-e1[2]*e2[1]; n[1]=e1[2]*e2[0]-e1[0]*e2[2];
                n[2]=e1[0]*e2[1]-e1[1]*e2[0];
                float L=sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (L<1e-9f) continue;
                if (fabsf(n[2]/L) >= 0.30f) continue;
                float lo=A[2], hi=A[2];
                if (Bv[2]<lo) lo=Bv[2]; if (C[2]<lo) lo=C[2];
                if (Bv[2]>hi) hi=Bv[2]; if (C[2]>hi) hi=C[2];
                float span = hi-lo; nface++;
                for (int b = 0; b < 12; b++) if (span < B[b]) { hist[b]++; break; }
            }
        }
        printf("FACE CENSUS %s: %ld collision meshes, %ld near-vertical faces\n",
               trackname, nmesh, nface);
        float prev = 0;
        for (int b = 0; b < 12; b++) {
            printf("FACE   span %6.2f - %6.2f m : %8ld  (%5.2f%%, cumulative %5.2f%%)\n",
                   prev, B[b] > 1e8f ? 99.99f : B[b], hist[b],
                   nface ? 100.0*hist[b]/nface : 0.0,
                   nface ? 100.0*({ long c=0; for (int q=0;q<=b;q++) c+=hist[q]; c; })/nface : 0.0);
            prev = B[b];
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }

    /* engine sound, most to least authentic: the car's own Gnsu20 sweep
       recordings (matched by name), else an .abk sample bank (name-hash
       pick), else the procedural synth. */
    int nsweep = audio_load_ginsu_sweeps(dataroot, carname);
    int engbank = -1, nloops = 0;
    if (!nsweep) {
        engbank = audio_bank_for_car(carname);
        nloops = audio_load_engine_bank(dataroot, engbank);
        if (!nloops && engbank != 0) { engbank = 0; nloops = audio_load_engine_bank(dataroot, 0); }
    }
    SDL_AudioDeviceID adev = audio_init();
    printf("engine audio: %s (%s)\n", adev ? "on" : "unavailable",
           nsweep ? "Gnsu20 sweeps" : nloops ? "game sample bank" : "procedural synth");
    if (nsweep) printf("engine sweeps: %d for %s (%.0f-%.0f rpm)\n", nsweep, carname,
                       g_engine.gin.accel_sweep.rpm_min, g_engine.gin.accel_sweep.rpm_max);
    else if (nloops) printf("engine bank: CAR_%02d for %s (%d loops incl idle)\n",
                            engbank, carname, nloops);
#ifdef N2_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *win = SDL_CreateWindow("OpenUG2",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 600,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "GL ctx: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(shot ? 0 : 1);   /* raw frame times in shot mode */
    /* Detect S3TC so car/rim TPK textures can upload their DXT blocks directly
       (glCompressedTexImage2D) instead of the CPU-decoded RGBA. Legacy GL 2.1
       and GLES2 both return a valid GL_EXTENSIONS string here. */
    { const char *ext = (const char *)glGetString(GL_EXTENSIONS);
      g_tex_s3tc = ext && (strstr(ext, "GL_EXT_texture_compression_s3tc") ||
                           strstr(ext, "texture_compression_dxt1")); }
    printf("texture path: %s\n", g_tex_s3tc ? "S3TC direct (glCompressedTexImage2D)"
                                            : "CPU DXT decode (no s3tc extension)");
#ifdef DEBUG_UI
    dbgui_init(win, ctx);
#endif

    RProg rp = render_program();
    GLint uMVP = rp.uMVP, uUseTex = rp.uUseTex, uColor = rp.uColor,
          uUnlit = rp.uUnlit, uAlpha = rp.uAlpha, uSoft = rp.uSoft,
          uSpec = rp.uSpec, uAmbient = rp.uAmbient, uDiffuse = rp.uDiffuse,
          uLight = rp.uLight, uGloss = rp.uGloss;

    /* Per-mesh track textures: each mesh's diffuse key (0x134012) resolves to a
       texture in the region's own STREAM TPK (grass/road/props) or the shared
       LOC4DYNTEX pack (sky/facades). Decode each distinct key once, reject any
       that decode to noise (wrong format / swizzled surface), map key->GL tex.
       Generalises the old hard-coded TRN_GRASSC/RDP_PARKING lookup across
       regions (L4RR uses ORG_GRASS_001 etc.). */
    if (smaudit) {
        printf("\nMILESTONE: 98  target texture %08x\n", smkey);
        /* re-decode the same key through the same lookup to report its source form */
        N2Tex tt = {0}; int ok = 0; const char *via = "-";
        for (int r = 0; r < world.nreg && !ok; r++) {
            WRegion *g = &world.rgn[r];
            if (!g->data) continue;
            ok = n2_tpk_decode(g->data, g->len, g->tpk, smkey, &tt);
            if (ok) via = "region STREAM TPK (n2_tpk_decode)";
        }
        if (!ok && world.loc4) { ok = n2_load_car_tex_by_key(world.loc4, world.loc4len, smkey, &tt);
                                 if (ok) via = "LOC4DYNTEX (n2_load_car_tex_by_key)"; }
        if (!ok && world.master) { ok = n2_tpk_decode(world.master, world.masterlen,
                                                      world.mastertpk, smkey, &tt);
                                   if (ok) via = "master TPK (n2_tpk_decode)"; }
        if (!ok) printf("  DECODE FAILED (region buffers already freed after bind)\n");
        else {
            printf("  resolved via          %s\n", via);
            printf("  dimensions            %d x %d\n", tt.w, tt.h);
            printf("  decoded surface       %s%s\n", tt.rgb ? "RGB" : "none",
                   tt.alpha ? " + alpha plane" : "");
            printf("  raw S3TC blob         %s  dxtfmt %d  dxtlen %d\n",
                   tt.dxt ? "present" : "ABSENT", tt.dxtfmt, tt.dxtlen);
            if (tt.dxt && tt.dxtfmt) {   /* same walk upload_tpk_texture_to_gpu uses */
                int bpb = (tt.dxtfmt == 1) ? 8 : 16;
                int lw = tt.w, lh = tt.h, off = 0, level = 0, complete = 0;
                for (;;) { int bw = lw<4?1:lw/4, bh = lh<4?1:lh/4, sz = bw*bh*bpb;
                           if (off + sz > tt.dxtlen) break;
                           off += sz; level++;
                           if (lw==1 && lh==1) { complete = 1; break; }
                           if (lw>1) lw/=2; if (lh>1) lh/=2; }
                printf("  encoded mip levels    %d  chain complete to 1x1: %s\n",
                       level, complete ? "YES" : "NO");
            } else
                printf("  encoded mip levels    0  (world path decodes to RGB and calls "
                       "glGenerateMipmap; the TPK chain is never retained)\n");
            free(tt.rgb); free(tt.alpha); free(tt.dxt);
        }
        printf("\n");
    }
    if (fbcensus) {
        /* Re-run the production walk in THIS translation unit so the census
           statics in nfsu2.h (which each TU owns its own copy of) are the ones
           that get filled. Same function, same buffer, same key set world.c
           assembles: region TPK + LOC4 + master. Throwaway scene; the world the
           engine renders is the one world.c already built and is untouched. */
        static uint32_t ck[16384]; int nck = 0;
        for (int r = 0; r < world.nreg; r++) {
            WRegion *g = &world.rgn[r];
            if (!g->data) continue;
            nck = n2_tpk_keys(g->data, g->tpk, ck, 16384);
            if (world.loc4 && nck < 16384)
                nck += n2_car_tex_keys(world.loc4, world.loc4len, ck + nck, 16384 - nck);
            if (world.master && nck < 16384)
                nck += n2_tpk_keys(world.master, world.mastertpk, ck + nck, 16384 - nck);
            N2Scene tmp = {0};
            n2_walk_meshes(g->data, 0, g->len, &tmp, ck, nck);
            for (int i = 0; i < tmp.count; i++) {
                free(tmp.meshes[i].verts); free(tmp.meshes[i].idx); free(tmp.meshes[i].vcol);
            }
            free(tmp.meshes);
            break;   /* single-region audit */
        }
        if (b02probe) {
            const unsigned char *rd = NULL;
            for (int r = 0; r < world.nreg; r++) if (world.rgn[r].data) { rd = world.rgn[r].data; break; }
            printf("\nMILESTONE: 104  0x134B02 stride/field probe  track=%s\n", trackname);
            printf("Method: for each B02 body, try every 4-aligned stride that divides it\n"
                   "exactly; at each stride try every 4-aligned (start,count) u32 field pair\n"
                   "and require a COMPLETE contiguous partition of the paired B03 usable\n"
                   "index span, every count a positive multiple of 3. Nothing is inferred\n"
                   "from names or textures.\n\n");
            for (int a = 0; a < n2_m102_ns[N2_FB_MALFORMED]; a++) {
                N2FbRec *r = &n2_m102_s[N2_FB_MALFORMED][a];
                if (r->b02off < 0) { printf("%-30s no B02 leaf\n", r->name); continue; }
                const unsigned char *p = rd + r->b02off;
                int detail = !strcmp(r->name, b02probe) || !strcmp(b02probe, "*");
                int pad = n2_skip_filler(p, (int)r->b02size);
                long body = (long)r->b02size - pad;
                const unsigned char *q = p + pad;
                /* n2_skip_filler eats EVERY leading 0x11 byte, including one that
                   is merely the low byte of the first bbox float. Test the leaf
                   at the skipped offset AND at offset 0. */
                /* pair-filler: the index-leaf convention is whole 0x1111 u16
                   words, so a lone trailing 0x11 byte is data, not filler. */
                int ppad = 0;
                while (ppad + 2 <= (int)r->b02size && p[ppad] == 0x11 && p[ppad+1] == 0x11) ppad += 2;
                long bases[3] = { pad, 0, ppad };
                for (int bi = 0; bi < 3; bi++) {
                    long base = bases[bi];
                    if (bi == 1 && base == pad) continue;
                    if (bi == 2 && (base == pad || base == 0)) continue;
                    long b2 = (long)r->b02size - base;
                    if (detail)
                        printf("   base +%-3ld (%s) body %4ld  body%%60 = %ld  -> %ld records\n",
                               base, bi == 0 ? "byte-filler skip" : bi == 1 ? "no skip"
                                                                            : "pair-filler skip",
                               b2, b2 % 60, b2 / 60);
                    if (b2 % 60) continue;
                    const unsigned char *qq = p + base;
                    long n = b2 / 60, chain = 0; int ok = 1;
                    for (long i = 0; i < n && ok; i++) {
                        uint32_t cv = n2_u32(qq + i*60 + 12);
                        uint32_t sv = n2_u32(qq + i*60 + 52);
                        uint32_t mv = n2_u32(qq + i*60 + 28);
                        if ((long)sv != chain || cv == 0 || cv % 3 ||
                            mv >= (uint32_t)r->nslot) ok = 0;
                        else chain += (long)cv;
                    }
                    if (detail || ok)
                        printf("   %-30s base +%-3ld 60B: %-7s chain %ld / avail %ld "
                               "(whole tris %ld, pad %ld) %s\n",
                               r->name, base, ok ? "VALID" : "invalid", chain, r->avail,
                               r->avail - r->avail%3, r->avail%3,
                               (ok && chain == r->avail - r->avail%3)
                                 ? " COVERS EVERY WHOLE TRIANGLE" : "");
                    if (ok && detail)
                        for (long i = 0; i < n; i++)
                            printf("       rec %ld  start %u count %u mat_id %u\n", i,
                                   n2_u32(qq + i*60 + 52), n2_u32(qq + i*60 + 12),
                                   n2_u32(qq + i*60 + 28));
                }
                if (detail)
                    printf("== %s  cat %s  slots %d  B02 size %u filler %d body %ld  "
                           "B03 usable %ld indices (%ld tris, remainder %ld)\n",
                           r->name, bc_cat(r->cat), r->nslot, r->b02size, pad, body,
                           r->avail, r->avail/3, r->avail%3);
                int nhit = 0; long hs = 0, hos = 0, hoc = 0, hn = 0;
                for (long st = 12; st <= 256 && st <= body; st += 4) {
                    if (body % st) continue;
                    long n = body / st;
                    if (n < 1 || n > 256) continue;
                    for (long os = 0; os + 4 <= st; os += 4)
                    for (long oc = 0; oc + 4 <= st; oc += 4) {
                        if (os == oc) continue;
                        long chain = 0; int ok = 1;
                        for (long i = 0; i < n && ok; i++) {
                            uint32_t sv = n2_u32(q + i*st + os);
                            uint32_t cv = n2_u32(q + i*st + oc);
                            if ((long)sv != chain) ok = 0;
                            else if (cv == 0 || cv % 3 || cv > 65535u) ok = 0;
                            else chain += (long)cv;
                        }
                        if (ok && chain == r->avail) {
                            nhit++;
                            if (nhit == 1) { hs = st; hos = os; hoc = oc; hn = n; }
                            if (detail && nhit <= 6)
                                printf("   HIT stride %3ld  n %3ld  start@+%-3ld count@+%-3ld  "
                                       "partition 0..%ld exact\n", st, n, os, oc, chain);
                        }
                    }
                }
                if (!detail) {
                    printf("%-30s body %5ld  avail %5ld  hits %2d", r->name, body, r->avail, nhit);
                    if (nhit) printf("  first: stride %ld n %ld start@+%ld count@+%ld", hs, hn, hos, hoc);
                    printf("\n");
                } else {
                    printf("   total field-layout hits: %d\n", nhit);
                    if (nhit) {
                        /* material-field candidates under the winning stride */
                        printf("   mat_id candidates at stride %ld (all values < %d):",
                               hs, r->nslot);
                        for (long om = 0; om + 4 <= hs; om += 4) {
                            if (om == hos || om == hoc) continue;
                            int ok = 1;
                            for (long i = 0; i < hn && ok; i++)
                                if (n2_u32(q + i*hs + om) >= (uint32_t)r->nslot) ok = 0;
                            if (ok) printf(" +%ld", om);
                        }
                        printf("\n");
                    }
                    printf("\n");
                }
            }
            /* one working 60-byte sibling of the same family, for comparison */
            printf("\nworking 60-byte sibling for comparison:\n");
        }
        static const char *fbn[N2_FB_NCAT] = {
            "1 validated per-submesh", "2 fallback: no 0x134B02",
            "3 fallback: multi-leaf",  "4 fallback: malformed/non-contiguous",
            "5 fallback: bad mat_id / empty or unresolved slot" };
        printf("\nMILESTONE: 102  ROAD/TERRAIN emission census  track=%s\n", trackname);
        long to = 0, tt = 0, ti = 0;
        for (int c = 0; c < N2_FB_NCAT; c++)
            { to += n2_m102_obj[c]; tt += n2_m102_tri[c]; ti += n2_m102_idx[c]; }
        printf("%-50s %8s %8s %10s %8s %8s\n", "category", "objects", "obj%",
               "indices", "tris", "tri%");
        for (int c = 0; c < N2_FB_NCAT; c++)
            printf("%-50s %8ld %7.2f%% %10ld %8ld %7.2f%%\n", fbn[c],
                   n2_m102_obj[c], to ? 100.0*n2_m102_obj[c]/to : 0.0,
                   n2_m102_idx[c], n2_m102_tri[c], tt ? 100.0*n2_m102_tri[c]/tt : 0.0);
        printf("%-50s %8ld %7.2f%% %10ld %8ld %7.2f%%\n", "TOTAL", to, 100.0, ti, tt, 100.0);
        printf("\ncategory 5 split: mat_id out of range %ld, empty slot %ld, "
               "slot key unresolved in this region's TPK set %ld\n",
               n2_m102_bad[0], n2_m102_bad[1], n2_m102_bad[2]);
        printf("emitted submesh ranges: %ld with their own resolved slot key, "
               "%ld falling back to the object's `last` key (%.3f%% of ranges)\n",
               n2_m102_rng_res, n2_m102_rng_unres,
               (n2_m102_rng_res + n2_m102_rng_unres)
                 ? 100.0*n2_m102_rng_unres/(n2_m102_rng_res + n2_m102_rng_unres) : 0.0);
        printf("\nmulti-slot fallback objects where `last` != slot 0 "
               "(the only immediate wrong-material candidates):\n");
        for (int c = 1; c < N2_FB_NCAT; c++)
            if (n2_m102_obj[c])
                printf("  %-48s %4ld of %4ld objects, %6ld tris (%.3f%% of all ROAD/TERRAIN tris)\n",
                       fbn[c], n2_m102_risk[c], n2_m102_obj[c], n2_m102_risktri[c],
                       tt ? 100.0*n2_m102_risktri[c]/tt : 0.0);
        for (int c = 1; c < N2_FB_NCAT; c++) {
            if (!n2_m102_ns[c]) continue;
            printf("\n%s -- first %d objects in load order:\n", fbn[c], n2_m102_ns[c]);
            printf("  %-30s %-8s %5s %5s %6s %9s %9s %-34s %s\n", "asset name", "cat",
                   "slots", "recs", "leaves", "fallback", "slot 0", "world XY / Z bounds", "tris");
            for (int a = 0; a < n2_m102_ns[c]; a++) {
                N2FbRec *r = &n2_m102_s[c][a];
                char bb[64];
                snprintf(bb, sizeof bb, "[%.0f %.0f][%.0f %.0f][%.1f %.1f]",
                         r->bb[0],r->bb[1], r->bb[2],r->bb[3], r->bb[4],r->bb[5]);
                printf("  %-30s %-8s %5d %5d %6d %9.8x %9.8x %-34s %6d%s\n",
                       r->name, bc_cat(r->cat), r->nslot, r->nsub, r->nleaf,
                       r->key, r->slot0, bb, r->ntri,
                       (r->nslot > 1 && r->key != r->slot0) ? "  <-- multi-slot, last != slot0"
                                                            : "");
            }
        }
        printf("\n");
    }
    /* --mesh-material (Milestone 100): re-walk ONE object's original records and
       dump its material linkage verbatim. Read-only; no parser, sampler, UV or
       asset change, and nothing is inferred from a texture name. */
    if (matdump) {
        const unsigned char *rd = NULL; long rl = 0; N2Tpk rtpk; const char *rn = "";
        for (int q = 0; q < world.nreg; q++) if (world.rgn[q].data) {
            rd = world.rgn[q].data; rl = world.rgn[q].len; rtpk = world.rgn[q].tpk;
            rn = world.rgn[q].name; break; }
        if (rd) {
            printf("MILESTONE: 100  object material dump  %s in %s\n\n", matmesh, rn);
            long ds = 0; uint32_t sz = 0;
            if (!m100_find(rd, 0, rl, matmesh, &ds, &sz))
                printf("object \"%s\" not found in %s (%d objects scanned)\n",
                       matmesh, rn, m100_nobj);
            else {
                            int cat = n2_mesh_category(rd, ds, ds + sz);

                            printf("object chunk 0x80134010 payload@%ld size %u  name \"%s\"  cat %s\n",
                                   ds, sz, matmesh, bc_cat(cat));
                            /* --- 0x134012 slot list, stored order --- */
                            N2Leaf t12[4]; int n12 = 0;
                            n2_find_leaves(rd, ds, ds + sz, 0x00134012u, t12, &n12, 4);
                            printf("\n0x134012 leaves: %d\n", n12);
                            uint32_t cand[64]; int ncand = 0;
                            for (int a = 0; a < n12; a++) {
                                printf("  leaf %d @%ld size %u  (stride-8 entries: %ld)\n",
                                       a, t12[a].off, t12[a].size, (long)t12[a].size/8);
                                for (long b = 0; b + 8 <= (long)t12[a].size; b += 8) {
                                    uint32_t k = n2_u32(rd + t12[a].off + b);
                                    uint32_t pad2 = n2_u32(rd + t12[a].off + b + 4);
                                    N2Tex tt = {0};
                                    int ok = k && n2_tpk_decode(rd, rl, rtpk, k, &tt);
                                    char nm[25] = "(unresolved)";
                                    if (ok) {   /* recover the 24-byte record name */
                                        for (int bl = 0; bl < rtpk.nblk && nm[0]=='('; bl++) {
                                            long hb = rtpk.blk[bl].hbeg, he = hb + rtpk.blk[bl].hsize;
                                            for (long z = hb; z + 0x40 < he; z++) {
                                                if (!(rd[z] >= 'A' && rd[z] <= 'Z')) continue;
                                                if (n2_u32(rd + z + 0x18) != k) continue;
                                                memcpy(nm, rd + z, 24); nm[24] = 0; break; }
                                        }
                                    }
                                    printf("    slot %-2ld payload@+%-6ld key %08x pad %08x  "
                                           "%-24s %s",
                                           b/8, b, k, pad2, ok ? nm : "-",
                                           ok ? "" : "UNRESOLVED");
                                    if (ok) printf("%d x %d", tt.w, tt.h);
                                    printf("\n");
                                    if (k && ncand < 64) cand[ncand++] = k;
                                    if (ok) { free(tt.rgb); free(tt.alpha); free(tt.dxt); }
                                }
                            }
                            /* --- what the production rule selects --- */
                            static uint32_t rkeys[16384];
                            int nrk = n2_tpk_keys(rd, rtpk, rkeys, 16384);
                            uint32_t sel = n2_mesh_texkey_cat(rd, ds, ds + sz, cat,
                                                              rkeys, nrk);
                            printf("\nn2_mesh_texkey_cat selects: %08x\n", sel);
                            printf("  rule: cat is %s -> returns `last`, the LAST non-zero u32\n"
                                   "        scanned at stride 4 over every 0x134012 leaf\n"
                                   "        (resolvability is NOT consulted for ROAD/TERRAIN)\n",
                                   bc_cat(cat));
                            /* --- geometry leaves --- */
                            N2Leaf vb[8], ib[8], sm[8]; int nvb=0, nib=0, nsm=0;
                            n2_find_leaves(rd, ds, ds + sz, 0x00134B01u, vb, &nvb, 8);
                            n2_find_leaves(rd, ds, ds + sz, 0x00134B03u, ib, &nib, 8);
                            n2_find_leaves(rd, ds, ds + sz, 0x00134B02u, sm, &nsm, 8);
                            printf("\n0x134B01 vertex leaves: %d, 0x134B03 index leaves: %d, "
                                   "0x134B02 submesh leaves: %d\n", nvb, nib, nsm);
                            for (int a = 0; a < nvb; a++) {
                                int pad3 = n2_skip_filler(rd + vb[a].off, (int)vb[a].size);
                                printf("  0x134B01[%d] @%ld size %u  filler %d  -> %d verts @24B\n",
                                       a, vb[a].off, vb[a].size, pad3,
                                       (int)((vb[a].size - pad3)/24));
                            }
                            for (int a = 0; a < nib; a++) {
                                int pad3 = n2_skip_filler(rd + ib[a].off, (int)ib[a].size);
                                printf("  0x134B03[%d] @%ld size %u  filler %d  -> %d u16 indices\n",
                                       a, ib[a].off, ib[a].size, pad3,
                                       (int)((ib[a].size - pad3)/2));
                            }
                            for (int a = 0; a < nsm; a++) {
                                int pad3 = n2_skip_filler(rd + sm[a].off, (int)sm[a].size);
                                long body = (long)sm[a].size - pad3;
                                printf("  0x134B02[%d] @%ld size %u  filler %d  body %ld  "
                                       "body%%60 = %ld\n", a, sm[a].off, sm[a].size, pad3,
                                       body, body % 60);
                            }
                            N2Sub sub[64];
                            int nsub = n2_mesh_submeshes(rd, ds, ds + sz, sub, 64);
                            printf("n2_mesh_submeshes decoded: %d record(s)%s\n", nsub,
                                   nsub ? "" : "  (returns 0: needs exactly 1 leaf and body%%60==0)");
                            for (int a = 0; a < nsub; a++)
                                printf("  sub %d  start %u count %u mat_id %u\n",
                                       a, sub[a].start, sub[a].count, sub[a].mat);
                            /* --- nearest-filter preview per candidate key --- */
                            printf("\ncandidate previews (1x nearest, no rescale):\n");
                            for (int a = 0; a < ncand; a++) {
                                N2Tex tt = {0};
                                if (!n2_tpk_decode(rd, rl, rtpk, cand[a], &tt)) {
                                    printf("  %08x  no decode\n", cand[a]); continue; }
                                char pp[1024];
                                snprintf(pp, sizeof pp, "%s_%s_slot%d_%08x_%dx%d.png",
                                         matdump, matmesh, a, cand[a], tt.w, tt.h);
                                write_png(pp, tt.w, tt.h, tt.rgb);
                                printf("  %08x %dx%d -> %s\n", cand[a], tt.w, tt.h, pp);
                                free(tt.rgb); free(tt.alpha); free(tt.dxt);
                            }
            }
            printf("\n");
        }
    }

    /* --tpk-record (Milestone 99): every RAW TPK record carrying one key, in the
       exact sources and the exact scan order world_bind_textures uses. Read-only
       -- it re-walks the same 0x7c-byte record layout n2_tpk_decode walks and
       prints the fields verbatim; no decode, sampler, UV or asset change. */
    if (tpkrec) {
        printf("MILESTONE: 99  raw TPK records for key %08x\n", tpkkey);
        printf("record layout (Nikki minus the 0x0C name pad): name@+0x00(24B) "
               "key@+0x18 Offset@+0x24 PaletteOffset@+0x28 Size@+0x2c "
               "PaletteSize@+0x30 W@+0x38(u16) H@+0x3a(u16)\n\n");
        int nfound = 0, firstw = 0, firsth = 0, firstdone = 0;
        char firstsrc[128] = "-"; uint32_t f_off=0, f_pal=0, f_psz=0;
        long f_dbase = 0; const unsigned char *f_d = NULL; long f_len = 0;
        for (int pass = 0; pass < 3; pass++) {
            const unsigned char *d = NULL; long dl = 0; N2Tpk tp; char label[128];
            if (pass == 0) {
                int r = -1;
                for (int q = 0; q < world.nreg; q++) if (world.rgn[q].data) { r = q; break; }
                if (r < 0) continue;
                d = world.rgn[r].data; dl = world.rgn[r].len; tp = world.rgn[r].tpk;
                snprintf(label, sizeof label, "STREAM%s.BUN local TPK", world.rgn[r].name + 6);
            } else if (pass == 1) {
                if (!world.loc4) { printf("[LOC4 fallback: not present]\n"); continue; }
                d = world.loc4; dl = world.loc4len; tp = n2_tpk_open(d, dl);
                snprintf(label, sizeof label, "TRACKS/LOC4DYNTEX.BIN");
            } else {
                if (!world.master) { printf("[master fallback: not present]\n"); continue; }
                d = world.master; dl = world.masterlen; tp = world.mastertpk;
                snprintf(label, sizeof label, "master TPK");
            }
            for (int b = 0; b < tp.nblk; b++) {
                long hbeg = tp.blk[b].hbeg, hend = hbeg + tp.blk[b].hsize;
                long dbase = tp.blk[b].dbase;
                for (long i = hbeg; i + 0x40 < hend; i++) {
                    if (!(d[i] >= 'A' && d[i] <= 'Z')) continue;
                    if (n2_u32(d + i + 0x18) != tpkkey) continue;
                    char nm[25]; memcpy(nm, d + i, 24); nm[24] = 0;
                    for (int c = 0; c < 24; c++) if (nm[c] && (nm[c] < 32 || nm[c] > 126)) nm[c] = '.';
                    uint32_t off = n2_u32(d+i+0x24), paloff = n2_u32(d+i+0x28);
                    uint32_t sz = n2_u32(d+i+0x2c), palsz = n2_u32(d+i+0x30);
                    int w = d[i+0x38] | d[i+0x39]<<8, hh = d[i+0x3a] | d[i+0x3b]<<8;
                    int isp8 = palsz >= 1024;
                    long pixend = dbase + off + (long)w*hh;
                    long palend = dbase + paloff + 1024;
                    printf("record %d\n", nfound);
                    printf("  source file/block     %s, block %d (hdr@%ld size %ld, dbase %ld)\n",
                           label, b, hbeg, (long)tp.blk[b].hsize, dbase);
                    printf("  24-byte name          \"%s\"\n", nm);
                    printf("  key                   %08x\n", n2_u32(d+i+0x18));
                    printf("  Offset / PaletteOffset %u / %u\n", off, paloff);
                    printf("  Size / PaletteSize     %u / %u\n", sz, palsz);
                    printf("  W / H raw u16          %d x %d\n", w, hh);
                    printf("  classification         %s\n",
                           isp8 ? "P8 (PaletteSize >= 1024)"
                                : ((long)sz > (long)w*hh*9/10 ? "DXT3 (Size > W*H*0.9)"
                                                              : "DXT1"));
                    printf("  W*H                    %ld  (Size %u -> %s)\n",
                           (long)w*hh, sz,
                           (long)sz == (long)w*hh ? "EXACTLY W*H (1 byte/texel index plane)"
                           : (long)sz > (long)w*hh ? "larger than W*H" : "smaller than W*H");
                    printf("  pixel range valid      %s (dbase+Offset+W*H = %ld vs len %ld)\n",
                           pixend <= dl ? "yes" : "NO", pixend, dl);
                    printf("  palette range valid    %s (dbase+PaletteOffset+1024 = %ld)\n",
                           isp8 ? (palend <= dl ? "yes" : "NO") : "n/a", palend);
                    if (!firstdone) {
                        firstdone = 1; firstw = w; firsth = hh;
                        snprintf(firstsrc, sizeof firstsrc, "%s block %d", label, b);
                        f_off=off; f_pal=paloff; f_psz=palsz;
                        f_dbase=dbase; f_d=d; f_len=dl;
                    }
                    nfound++;
                    i += 0x7b;
                }
            }
        }
        printf("\ntotal raw records with this key: %d\n", nfound);
        if (firstdone) {
            printf("world_bind_textures resolves FIRST: %s  ->  raw W/H %d x %d\n",
                   firstsrc, firstw, firsth);
            N2Tex chk = {0};
            int r0 = -1;
            for (int q = 0; q < world.nreg; q++) if (world.rgn[q].data) { r0 = q; break; }
            int ok = r0 >= 0 && n2_tpk_decode(world.rgn[r0].data, world.rgn[r0].len,
                                              world.rgn[r0].tpk, tpkkey, &chk);
            printf("n2_tpk_decode returns:            %s  %d x %d   -> raw==decoded: %s\n",
                   ok ? "ok" : "FAILED", chk.w, chk.h,
                   (ok && chk.w == firstw && chk.h == firsth) ? "YES" : "NO");
            /* 1x nearest PNG of the SELECTED record, straight from its own bytes */
            if (ok && f_d && f_psz >= 1024 &&
                f_dbase + f_pal + 1024 <= f_len &&
                f_dbase + f_off + (long)firstw*firsth <= f_len) {
                unsigned char *px = (unsigned char *)malloc((size_t)firstw*firsth*3);
                const unsigned char *pal = f_d + f_dbase + f_pal;
                const unsigned char *ix  = f_d + f_dbase + f_off;
                for (long q = 0; q < (long)firstw*firsth; q++) {
                    const unsigned char *c = pal + (long)ix[q]*4;
                    px[q*3]=c[0]; px[q*3+1]=c[1]; px[q*3+2]=c[2];
                }
                char pp[1024]; snprintf(pp, sizeof pp, "%s_%08x_%dx%d.png",
                                        tpkrec, tpkkey, firstw, firsth);
                write_png(pp, firstw, firsth, px);
                printf("wrote 1x nearest PNG (no rescale, no filter): %s\n", pp);
                free(px);
                /* index-plane sanity: how much of the 256-entry palette is used */
                int used[256]; memset(used, 0, sizeof used); int nu = 0;
                for (long q = 0; q < (long)firstw*firsth; q++)
                    if (!used[ix[q]]) { used[ix[q]] = 1; nu++; }
                printf("index plane: %ld bytes read, %d distinct palette entries used\n",
                       (long)firstw*firsth, nu);
            }
            free(chk.rgb); free(chk.alpha); free(chk.dxt);
        }
        printf("\n");
    }
    static N2LightSrc lsrc[16384]; int nlsrc = 0; /* district light sources */
    GLuint tex_glow = 0;                          /* SFX_FLARE_GLOWA: the lamp halo */
    /* SHARED LIGHT TEXTURES. Headlight and tail-light meshes name texture keys
       that exist in no texture pack at all. They are not meant to be resolved
       that way: the game binds these BY NAME from the global bundle and plugs
       them straight into the shader.
         HEADLIGHTS    128x64   uncompressed BGRA
         BRAKE_GLOBAL  128x128  DXT1, with BRAKE_GLOBAL_LEFT beside it
         HEADLIGHTGLOW          the glow around a lit lamp */
    GLuint tex_headlights = 0, tex_brake = 0, tex_brake_l = 0, tex_hlglow = 0;
    static uint32_t tmapkey[2048]; static GLuint tmaptex[2048];
    static unsigned char tmapalpha[2048];   /* draw mode per texture */
    int ntmap = world_bind_textures(&world, tmapkey, tmaptex, tmapalpha, 2048);
    printf("track textures bound: %d distinct\n", ntmap);

    /* Shared effect textures. Spotlight and shop-front beams all share one
       texture key that no track texture pack carries, which is why they were
       loading untextured: it lives in the common in-game bundle as
       SFX_LIGHT_BEAMA, 32x256 DXT3, drawn in the topmost additive layer. */
    static const char *extra_packs[] = {
        "GLOBAL/InGameCommon.bun",   /* SFX_LIGHT_BEAMA: spotlight beams */
        "TRACKS/LOC4DYNTEX.BIN",     /* sky: SKY_SUNRISE_A/_CAP, SKY_SUNSET_A, SKY_NIGHT_A */
    };
    for (int xf = 0; xf < (int)(sizeof extra_packs / sizeof *extra_packs); xf++)
    {   char cp2[1024];
        snprintf(cp2, sizeof cp2, "%s/%s", dataroot, extra_packs[xf]);
        long cl2 = 0; unsigned char *cd2 = n2_read_file(cp2, &cl2);
        if (cd2) {
            N2Tpk ct2 = n2_tpk_open(cd2, cl2);
            int added = 0;
            for (int i = 0; i < scene.count && ntmap < 2048; i++) {
                uint32_t tk = scene.meshes[i].texkey; if (!tk) continue;
                int have = 0;
                for (int j = 0; j < ntmap; j++) if (tmapkey[j] == tk) { have = 1; break; }
                if (have) continue;
                N2Tex t = {0};
                /* This file is laid out like a car's texture file: a slot
                   table plus compressed blocks, which an ordinary texture-pack
                   walk does not read. */
                if (n2_tpk_decode(cd2, cl2, ct2, tk, &t)
                    || n2_load_car_tex_by_key(cd2, cl2, tk, &t)) {
                    tmapkey[ntmap] = tk; tmaptex[ntmap] = upload_tex(&t);
                    tmapalpha[ntmap] = (unsigned char)n2_tex_mode(&t);
                    ntmap++; added++;
                    free(t.rgb); free(t.alpha); free(t.dxt);
                }
            }
            if (added) printf("%s: %d textures added\n", extra_packs[xf], added);
            free(ct2.blk); free(cd2);
        }
    }

    /* THE LAMP HALO. Its texture is the flare sheet the game ships; the key is
       looked up rather than the texture built, so what is drawn is the game's
       own glow. */
    for (int q = 0; q < ntmap; q++)
        if (tmapkey[q] == 0x17e5ebd2u) { tex_glow = tmaptex[q];
            printf("lamp flare: SFX_FLARE_GLOWA bound (mode %d)\n", tmapalpha[q]); break; }

    /* LIGHT SOURCES drive those halos. Much lamp geometry is a flat quad that
       all but vanishes seen from the side, so at driving height the light
       seemed to switch off while it was still visible from above; the halo is
       drawn from the source record instead, and stays visible from any angle.
       The whole district is kept -- a couple of thousand records is tens of
       kilobytes, and the distance cull runs from the CAMERA every frame, so
       culling once around the spawn point would make halos run out as soon as
       you drove away. */
    if (world2_on && world2_bundle[0]) {
        char lp[1024];
        snprintf(lp, sizeof lp, "%s/TRACKS/%s.BUN", dataroot, world2_bundle);
        long ll = 0; unsigned char *ld = n2_read_file(lp, &ll);
        if (ld) {
            nlsrc = n2_load_lights(ld, ll, lsrc, 16384);
            printf("district light sources: %d\n", nlsrc);
            free(ld);
        }
    }
    if (smaudit) { int slot = -1;
        for (int j = 0; j < ntmap; j++) if (tmapkey[j] == smkey) { slot = j; break; }
        printf("M98 target %08x -> bound slot %d, GL id %u\n\n", smkey, slot,
               slot < 0 ? 0u : tmaptex[slot]); }


    /* GPS self-check: route across the city so a broken graph is loud at load. */
    if (world.nnav > 0 && world.ndist >= 2) {
        int a = -1, b = -1;
        for (int i = 0; i < world.ndist; i++) {
            if (!strcmp(world.dist[i].tok, "UC")) a = i;
            if (!strcmp(world.dist[i].tok, "IP")) b = i;
        }
        if (a < 0) a = 0;
        if (b < 0) b = world.ndist - 1;
        int s0 = world_nav_nearest(&world, world.dist[a].cx, world.dist[a].cy);
        int g0 = world_nav_nearest(&world, world.dist[b].cx, world.dist[b].cy);
        static int path[8192]; float dist = 0;
        uint32_t t0 = SDL_GetTicks();
        int n = world_route(&world, s0, g0, path, 8192, &dist);
        uint32_t t1 = SDL_GetTicks();
        printf("GPS test %s (%s) -> %s (%s): %d nodes, %.0f m, %u ms\n",
               world.dist[a].tok, world_district_name(world.dist[a].tok),
               world.dist[b].tok, world_district_name(world.dist[b].tok),
               n, dist, t1 - t0);
    }

    /* Boot straight into a race event if asked (--event <id>). The Phase 71/72
       load-time A-star and checkpoint self-checks that used to run here were
       removed: they asserted on load and aborted for small events (e.g. 4301,
       where the on-circuit sample finds no reroutable pair) and flooded the
       console -- exactly the automated race telemetry the manual-testing
       protocol drops. The barrier/checkpoint logic in world.c is unchanged. */
    /* A named start point means free roam: the shipped events are not armed
       and the car is not moved to a start grid. Asking for both is a
       contradiction, and the grid would silently win. */
    if (spawn_set && want_event_id) {
        fprintf(stderr, "--spawn and --event are exclusive; ignoring --event %d\n",
                want_event_id);
        want_event_id = 0;
    }
    if (want_event_id && world.nev > 0) {
        int wi = -1;
        for (int i = 0; i < world.nev; i++) if (world.ev[i].id == want_event_id) wi = i;
        if (wi < 0) fprintf(stderr, "--event %d: no such race event\n", want_event_id);
        else {
            world_race_start(&world, troot, wi, want_laps);
            if (rtrace) {
                printf("RT closures: %d\n", world.nbar);
                for (int b=0; b<world.nbar; b++)
                    printf("RT   barrier %d at (%.3f %.3f) dir(%.4f %.4f) nodes %d->%d\n",
                           b, world.bar[b].x, world.bar[b].y,
                           world.bar[b].dx, world.bar[b].dy,
                           world.bar[b].a, world.bar[b].b);
            }
        }
    }

    /* Scripted-object entity DEFINITIONS from the district companion L4R*.BUN.
       Read-only decode for the inspector; the data has no world placement or
       mesh (Phase 50), so these are logged as a table, not instantiated. */
    static ScriptedDef sdefs[256];
    int nsd = world_scripted_defs(&world, troot, sdefs, 256);
    printf("scripted entity defs: %d  [definitions only - no placement in data]\n", nsd);
    for (int i = 0; i < nsd; i++)
        printf("  %-24s %08x  %6.1f x%6.1f x%6.1f\n",
               sdefs[i].name, sdefs[i].hash, sdefs[i].w, sdefs[i].l, sdefs[i].h);
    /* resolve each mesh's texture once — the per-frame key scan was fine for
       one region, not for a whole city of meshes */
    GLuint *mtex = (GLuint *)calloc(nm, sizeof *mtex);
    for (int i = 0; i < nm; i++)
        for (int j = 0; j < ntmap; j++)
            if (tmapkey[j] == scene.meshes[i].texkey) { mtex[i] = tmaptex[j]; break; }
    if (nansweep) {
        long bad = 0, badobj = 0;
        for (int i = 0; i < nm; i++) {
            const N2Mesh *me = &scene.meshes[i];
            int thisbad = 0;
            for (int v = 0; v < me->nverts; v++)
                for (int c = 0; c < 3; c++) {
                    float q = me->verts[v*5+c];
                    if (q != q || q >= N2_VERT_SANE || q <= -N2_VERT_SANE) { bad++; thisbad = 1; }
                }
            if (thisbad) badobj++;
        }
        printf("NAN SWEEP %s: %ld non-finite/out-of-range vertex components "
               "across %ld meshes (of %d) %s\n", trackname, bad, badobj, nm,
               bad == 0 ? "(clean)" : "(FOUND)");
    }
    if (lodcensus) {
        /* M133 STRUCTURAL detail-tier census. No names are consulted: meshes
           are grouped purely by identical rounded XY footprint, and within a
           group we look at triangle counts and vertical offsets. If the shipped
           world really carries several detail tiers of the same object, they
           show up here as same-footprint groups with strictly decreasing
           triangle counts. */
        typedef struct { float x0,y0,x1,y1; int n, tri[8], idx[8]; } LodGrp;
        static LodGrp g[24576]; int ng = 0;
        long grouped = 0, decreasing = 0, zoff = 0, samez = 0;
        for (int i = 0; i < nm; i++) {
            const float *bb = world.mbb[i];
            float q[4] = { roundf(bb[0]*100)/100, roundf(bb[1]*100)/100,
                           roundf(bb[2]*100)/100, roundf(bb[3]*100)/100 };
            int f = -1;
            for (int k = 0; k < ng; k++)
                if (g[k].x0==q[0] && g[k].y0==q[1] && g[k].x1==q[2] && g[k].y1==q[3]) { f = k; break; }
            if (f < 0) { if (ng >= 24576) continue; f = ng++;
                         g[f].x0=q[0]; g[f].y0=q[1]; g[f].x1=q[2]; g[f].y1=q[3]; g[f].n=0; }
            if (g[f].n < 8) { g[f].tri[g[f].n] = scene.meshes[i].nidx/3;
                              g[f].idx[g[f].n] = i; g[f].n++; }
        }
        int shown = 0;
        for (int k = 0; k < ng; k++) {
            if (g[k].n < 2) continue;
            grouped += g[k].n;
            int dec = 1;
            for (int q = 1; q < g[k].n; q++) if (g[k].tri[q] >= g[k].tri[q-1]) dec = 0;
            float zlo[8], zhi[8];
            for (int q = 0; q < g[k].n; q++) {
                const N2Mesh *me = &scene.meshes[g[k].idx[q]];
                zlo[q]=1e30f; zhi[q]=-1e30f;
                for (int v = 0; v < me->nverts; v++) {
                    float z = me->verts[v*5+2];
                    if (z<zlo[q]) zlo[q]=z; if (z>zhi[q]) zhi[q]=z;
                }
            }
            int off = 0;
            for (int q = 1; q < g[k].n; q++) if (fabsf(zlo[q]-zlo[0]) > 0.5f) off = 1;
            if (dec) decreasing += g[k].n;
            if (off) zoff += g[k].n; else samez += g[k].n;
            if (dec && shown < 12) {
                printf("LOD group xy[%9.2f %9.2f %9.2f %9.2f] n=%d  tris", 
                       g[k].x0,g[k].y0,g[k].x1,g[k].y1,g[k].n);
                for (int q = 0; q < g[k].n; q++) printf(" %d", g[k].tri[q]);
                printf("  zlo");
                for (int q = 0; q < g[k].n; q++) printf(" %.2f", zlo[q]);
                printf("  names");
                for (int q = 0; q < g[k].n; q++)
                    printf(" %s", scene.meshes[g[k].idx[q]].sname);
                printf("\n");
                shown++;
            }
        }
        printf("LOD census %s: %d meshes, %d distinct XY footprints, %ld meshes in "
               "multi-member groups, %ld of those in strictly-decreasing-triangle "
               "groups, %ld vertically offset, %ld coincident in Z\n",
               trackname, nm, ng, grouped, decreasing, zoff, samez);
    }
    if (g_world_texaudit) {
        long nokey2 = 0, inmap = 0, notinmap = 0;
        for (int i = 0; i < nm; i++) {
            uint32_t tk = scene.meshes[i].texkey;
            if (!tk) { nokey2++; continue; }
            if (mtex[i]) continue;
            int found = 0;
            for (int j = 0; j < ntmap; j++) if (tmapkey[j] == tk) { found = 1; break; }
            if (found) inmap++; else notinmap++;
            if (notinmap <= 6 && !found)
                printf("TEXMISS mesh %-30s cat %d key %08x  not in the bound map\n",
                       scene.meshes[i].sname, scene.meshes[i].cat, tk);
        }
        printf("TEXMISS summary: %ld meshes carry no key at all, %ld unresolved "
               "but key IS bound (lookup bug), %ld unresolved and key never bound\n",
               nokey2, inmap, notinmap);
    }
    if (mapaudit) {
        int keyed[8]={0}, unresolved[8]={0}, nokey[8]={0};
        for (int i=0;i<nm;i++) {
            int sc=scene.meshes[i].scen;
            if (sc<0 || sc>=8) sc=0;
            if (!scene.meshes[i].texkey) nokey[sc]++;
            else { keyed[sc]++; if (!mtex[i]) unresolved[sc]++; }
        }
        printf("MAP texture resolution (source meshes):\n");
        for (int sc=1;sc<=N2_SC_OTHER;sc++)
            printf("  %-9s keyed=%5d unresolved=%5d no-key=%5d\n",
                   n2_scen_name(sc),keyed[sc],unresolved[sc],nokey[sc]);
    }
    /* load a car and drop it on the track (36-byte vertex format) */
    long clen; unsigned char *cdata = n2_read_file(carp, &clen);
    long ctlen; unsigned char *ctdata = n2_read_file(cartexp, &ctlen);
    /* per-mesh car textures: get the TPK keys, then decode each key referenced
       by a mesh into its own GL texture (body, wheel, brake, ... bound by UVs). */
    uint32_t ckeys[64]; int nck = ctdata ? n2_car_tex_keys(ctdata, ctlen, ckeys, 64) : 0;
    uint32_t mapkey[32]; GLuint maptex[32]; char mapalpha[32]; int nmap = 0;
    N2Scene car; int ncar = 0; GpuMesh *cgm = NULL;
    int stock_wheel = -1;   /* cgm[] index of the car's own highest-LOD stock wheel mesh */
    float wheelT[4][16];                         /* 4 wheel placements (car-local) */
    float wheelTAI[4][16];                       /* same, minus the player's steer (AI cars) */
    GpuMesh wheelmesh; int have_wheel = 0;       /* procedural tyre, built after GL init */
    for (int k=0;k<4;k++){ float I[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; memcpy(wheelT[k],I,sizeof(I)); }
    float carbb[6] = {0,0,0,0,0,0};              /* body AABB min/max, for wheel placement */
    float bloomc[4][4] = {{0}};                  /* light-cluster centroids (car-local) + valid
                                                    flag: 0 front-L, 1 front-R, 2 rear-L, 3 rear-R */
    float carWheelR = 0.0f;                       /* car's stock wheel radius (rim fit) */
    N2CarProfile carprof; memset(&carprof, 0, sizeof carprof);   /* per-car dimensions */
    static N2LightMat lmat[256]; int nlmat = 0;   /* material records, from GLOBALB */
    N2CarConfig carcfg = { 0, 0, 0, 0 };         /* active customization profile (K cycles kits) */
    float spawn[3] = { cx, cy, cz }, heading0 = 0.0f;
    if (cdata) {
        ncar = n2_load_car(cdata, clen, &car, ckeys, nck, &carcfg);
        /* Wheels are modelled once at the origin (the SolidObject transform is
           identity for every part — verified), so a lone wheel mesh renders
           buried in the body. Place it at 4 arch positions derived from the body
           AABB. ponytail: fractions, since no explicit wheel data exists in
           GEOMETRY.BIN / PARTS_ANIMATIONS.bin — tune here if a car sits wrong. */
        float bb0[3]={1e30f,1e30f,1e30f}, bb1[3]={-1e30f,-1e30f,-1e30f};
        float tb0[3]={1e30f,1e30f,1e30f}, tb1[3]={-1e30f,-1e30f,-1e30f}; int ntv=0;
        for (int i=0;i<ncar;i++) {
            int tire = car.meshes[i].cat==N2_CAR_TIRE;
            for (int v=0; v<car.meshes[i].nverts; v++) {
                float *p=car.meshes[i].verts+v*5;
                if (tire) { ntv++; for(int a=0;a<3;a++){ if(p[a]<tb0[a])tb0[a]=p[a]; if(p[a]>tb1[a])tb1[a]=p[a]; } }
                else for (int a=0;a<3;a++){ if(p[a]<bb0[a])bb0[a]=p[a]; if(p[a]>bb1[a])bb1[a]=p[a]; }
            }
        }
        carbb[0]=bb0[0];carbb[1]=bb0[1];carbb[2]=bb0[2];
        carbb[3]=bb1[0];carbb[4]=bb1[1];carbb[5]=bb1[2];  /* wheelT built per-frame from g_dbg */
        /* Prep each stock wheel mesh: (1) the tyre is modelled with its solid
           spoke/cap face inboard (low Y) and the hollow barrel mouth outboard,
           so rotate it 180 deg about the vertical (Z) through its own Y-centre
           -- (x,y)->(-x,-y) -- which flips the cap outward and centres the tyre
           on the AttribSys track line; a rotation (not a mirror) keeps winding
           and normals valid. (2) drop the flat backing/hub-plane quad so the
           rim renders as clean spokes. Then remember the highest-LOD tier. */
        for (int i=0;i<ncar;i++) if (car.meshes[i].cat==N2_CAR_TIRE) {
            N2Mesh *m=&car.meshes[i];
            float ty0=1e30f,ty1=-1e30f;
            for(int v=0;v<m->nverts;v++){ float y=m->verts[v*5+1]; if(y<ty0)ty0=y; if(y>ty1)ty1=y; }
            float ymid=0.5f*(ty0+ty1);
            for(int v=0;v<m->nverts;v++){ m->verts[v*5]=-m->verts[v*5]; m->verts[v*5+1]=ymid-m->verts[v*5+1]; }
            rim_drop_welded_mesh(m);
            if (stock_wheel<0 || m->nverts>car.meshes[stock_wheel].nverts) stock_wheel=i;
        }
        /* Light-bloom clusters: average the lens vertices in each of the 4
           quadrants (front/rear x sign, left/right y sign) to get a halo anchor
           per headlight/taillight group. A vertex threshold keeps a stray single
           lamp from spawning a halo. */
        { double a[4][3]={{0}}; long ln[4]={0,0,0,0};
          for (int i=0;i<ncar;i++){ int c=car.meshes[i].cat;
              if (c!=N2_CAR_LIGHT && c!=N2_CAR_BRAKELIGHT) continue;
              for (int v=0;v<car.meshes[i].nverts;v++){ float *p=car.meshes[i].verts+v*5;
                  int b=(p[0]<0?2:0)+(p[1]<0?1:0);
                  a[b][0]+=p[0]; a[b][1]+=p[1]; a[b][2]+=p[2]; ln[b]++; } }
          for (int b=0;b<4;b++) if (ln[b]>40) {
              bloomc[b][0]=(float)(a[b][0]/ln[b]); bloomc[b][1]=(float)(a[b][1]/ln[b]);
              bloomc[b][2]=(float)(a[b][2]/ln[b]); bloomc[b][3]=1.0f; } }
        cgm = upload_scene(&car);
        /* size the procedural tyre from the car's own wheel mesh (radius in X-Z,
           width in Y) so a Hummer gets big tyres and a compact gets small ones. */
        float wR = 0.33f, wHW = 0.11f;
        if (ntv) { wR = 0.25f*((tb1[0]-tb0[0])+(tb1[2]-tb0[2]));   /* mean disc radius */
                   wHW = 0.5f*(tb1[1]-tb0[1]); if (wHW<0.04f) wHW=0.04f; }
        /* Build this car's dimension profile from its OWN geometry, and drive
           the wheel/ride numbers from it instead of any absolute constant. */
        n2_car_profile(&car, carname, WHEEL_SEED_FRONTF, WHEEL_SEED_REARF,
                       WHEEL_SEED_TRACKF, n2_car_brake_radius(cdata, 0, clen),
                       &carprof);
        /* The GLOBALB per-car table holds each car's factory wheel positions.
           Prefer the pre-decompressed GLOBALB.BUN; if it is absent, decompress
           the shipped GlobalB.lzc in place (it is a JDLZ stream) so the pipeline
           is fully self-contained from the retail files. */
        long globlen = 0; char gp[1024];
        snprintf(gp, sizeof gp, "%s/GLOBAL/GLOBALB.BUN", dataroot);
        unsigned char *globdata = n2_read_file(gp, &globlen);
        if (!globdata) {
            snprintf(gp, sizeof gp, "%s/GLOBAL/GlobalB.lzc", dataroot);
            long clen2 = 0; unsigned char *cz = n2_read_file(gp, &clen2);
            if (cz && clen2 >= 16 && memcmp(cz, "JDLZ", 4) == 0) {
                uint32_t usize = n2_u32(cz + 8);
                if (usize > 0 && usize < (1u << 28)) {
                    globdata = (unsigned char *)malloc(usize);
                    if (globdata) {
                        globlen = n2_jdlz(cz, (int)clen2, globdata, (int)usize);
                        printf("GLOBAL: decompressed GlobalB.lzc (JDLZ) -> %ld bytes\n", globlen);
                    }
                }
            }
            free(cz);
        }
        /* MATERIAL RECORDS. Each car submesh names a material, and that record
           carries the shading it should get -- chrome with no diffuse and a
           strong reflection, metallic paint with its own specular, rubber with
           neither. Reading them here means the car is lit by what the data
           says instead of by per-class constants. */
        nlmat = n2_load_lightmats(globdata, globlen, lmat, 256);
        if (nlmat) printf("material records: %d\n", nlmat);

        /* BODY COLOUR FROM THE DATA. The paint table pairs a material hash
           with the actual RGB the game paints that car in. Without it the body
           keeps a hand-picked near-white, and near-white metallic under a
           specular highlight is what blows out. */
        npal = n2_load_paints(globdata, globlen, pal, 512);
        if (!paint_name && !strcmp(carname, "350Z")) paint_name = paint_default_350z;
        if (paint_name) n2_carskin_mat = n2_str_hash(paint_name);
        {   uint32_t want = n2_carskin_mat ? n2_carskin_mat
                                           : n2_str_hash("METPAINTSILVER");
            for (int q = 0; q < npal; q++) if (pal[q].mat == want) {
                paint_rgb[0] = pal[q].r / 255.0f;
                paint_rgb[1] = pal[q].g / 255.0f;
                paint_rgb[2] = pal[q].b / 255.0f;
                printf("paint: %s RGB=(%d,%d,%d)\n",
                       paint_name ? paint_name : "METPAINTSILVER",
                       pal[q].r, pal[q].g, pal[q].b);
                break;
            }
            printf("paints in the palette: %d\n", npal);
        }

        {   N2Tpk gt = n2_tpk_open(globdata, globlen);
            struct { uint32_t key; GLuint *dst; const char *nm; } need[] = {
                { 0x28eefa9cu, &tex_headlights, "HEADLIGHTS"        },
                { 0x17f9f794u, &tex_brake,      "BRAKE_GLOBAL"      },
                { 0x85e9c79eu, &tex_brake_l,    "BRAKE_GLOBAL_LEFT" },
                { 0x3394fe62u, &tex_hlglow,     "HEADLIGHTGLOW"     },
            };
            for (int q = 0; q < 4; q++) {
                N2Tex t; memset(&t, 0, sizeof t);
                if (n2_tpk_decode(globdata, globlen, gt, need[q].key, &t)) {
                    *need[q].dst = upload_tex(&t);
                    printf("texture %s: %dx%d\n", need[q].nm, t.w, t.h);
                    free(t.rgb); free(t.alpha); free(t.dxt);
                } else printf("texture %s failed to decode\n", need[q].nm);
            }
            free(gt.blk);
        }

        int wheel_from_global = 0;
        g_dbg.wheel = wheel_config_for(carname, &carprof, globdata, globlen, &wheel_from_global);
        /* The new dynamics run entirely off this car's own shipped record. */
        if (n2_car_setup_load(&g_carsetup, globdata, globlen, carname)) {
            veh_model_init(&g_vehmodel, &g_carsetup);
            g_newphys_ok = 1;
            printf("car setup %-12s %.0f kg  %s  %d-speed  peak %.0f N*m  "
                   "tyre mu %.2f->%.2f at %.0f deg\n", carname,
                   g_carsetup.mass * 1000.0f,
                   g_carsetup.drive.split_rear > 0.9f ? "RWD" :
                   g_carsetup.drive.split_rear < 0.1f ? "FWD" : "AWD",
                   g_carsetup.drive.num_gears,
                   g_carsetup.motor.torque[4] * 1000.0f,
                   g_carsetup.tyre[1].mu_static, g_carsetup.tyre[1].mu_slide,
                   g_carsetup.tyre[1].slip_angle_deg);
            printf("physics: %s (--oldphys for the previous one)\n",
                   g_newphys ? "new single-track model" : "old kinematic model");

            /* WHEEL GEOMETRY COMES FROM THE RECORD TOO. wheel_config_for()
               took the axle lines and track from the car table but left the
               hub HEIGHT, the radius and the width to whatever could be
               measured off the tyre mesh -- and the mesh is the rim plus a
               tyre of unknown section, so the car ends up standing on stilts
               with wheels too wide for their arches. The record carries all
               four hub positions, each wheel's radius and its width outright,
               so use them: the 350Z's hubs sit at 0.171 with 0.328 front and
               0.334 rear wheels, and nothing about that needs measuring.
               A tighter, correctly-seated wheel also lets the springs show:
               with the body sitting where it should, pitch and roll have room
               to be visible instead of looking bolted solid. */
            {   VehicleWheelConfig *w = &g_dbg.wheel;
                printf("wheels: mesh gave hub %.3f r %.3f w %.3f  ->  "
                       "record gives hub %.3f r %.3f/%.3f w %.3f/%.3f\n",
                       w->ride_y, carprof.wheel_r, carprof.wheel_w,
                       g_carsetup.wheel[0].pos[2],
                       g_carsetup.wheel[0].radius, g_carsetup.wheel[2].radius,
                       g_carsetup.wheel[0].width,  g_carsetup.wheel[2].width);
                w->front_axle  = g_carsetup.wheel[0].pos[0];
                w->rear_axle   = g_carsetup.wheel[2].pos[0];
                w->front_track = fabsf(g_carsetup.wheel[0].pos[1]
                                     - g_carsetup.wheel[1].pos[1]);
                w->rear_track  = fabsf(g_carsetup.wheel[2].pos[1]
                                     - g_carsetup.wheel[3].pos[1]);
                w->ride_y      = g_carsetup.wheel[0].pos[2];
                carprof.wheel_r = 0.5f * (g_carsetup.wheel[0].radius
                                        + g_carsetup.wheel[2].radius);
                carprof.wheel_w = g_carsetup.wheel[0].width;
            }
        } else {
            printf("car setup: %s is not in the table -- staying on the old model\n", carname);
        }
        free(globdata);
        wR = carprof.wheel_r; wHW = 0.5f * carprof.wheel_w;
        if (wHW < 0.04f) wHW = 0.04f;
        carWheelR = carprof.wheel_r;               /* aftermarket rims fit to this */
        /* M121: this car's handling comes from its own measurements -- body AABB,
           GLOBALB axle line and the tyre mesh -- normalised against the fleet
           medians, never from a hand-tuned per-car handling table. */
        {
            float bl = carbb[3]-carbb[0], bw = carbb[4]-carbb[1], bh = carbb[5]-carbb[2];
            float wb = g_dbg.wheel.front_axle - g_dbg.wheel.rear_axle;
            g_vehicle = phys_vehicle_from_geometry(bl, bw, bh, wb,
                                                   g_dbg.wheel.front_track, carprof.wheel_w);
            printf("vehicle profile %-12s body %.3f x %.3f x %.3f = %.3f m3  wb %.3f  "
                   "track %.3f  tyre %.3f\n", carname, bl, bw, bh, bl*bw*bh, wb,
                   g_dbg.wheel.front_track, carprof.wheel_w);
            printf("  fleet-normalised  volume %.4f  wheelbase %.4f  track %.4f  tyre %.4f\n",
                   (bl*bw*bh)/PHYS_FLEET_VOLUME, wb/PHYS_FLEET_WHEELBASE,
                   g_dbg.wheel.front_track/PHYS_FLEET_TRACK, carprof.wheel_w/PHYS_FLEET_TYREW);
            printf("  active factors    accel %.4f  brake %.4f  steer %.4f  lat %.4f\n",
                   g_vehicle.accel, g_vehicle.brake, g_vehicle.steer, g_vehicle.lat);
        }
        printf("wheel stance %-12s axle F%+.3f R%+.3f (wb %.3f)  track F%.3f R%.3f  ride %+.3f  [%s]\n",
               carprof.name, g_dbg.wheel.front_axle, g_dbg.wheel.rear_axle,
               g_dbg.wheel.front_axle - g_dbg.wheel.rear_axle,
               g_dbg.wheel.front_track, g_dbg.wheel.rear_track, g_dbg.wheel.ride_y,
               wheel_from_global ? "GLOBALB car table" : "body-box fallback");
        printf("car profile %-12s wheel R %.3f (X %.3f Z %.3f) W %.3f%s  hubZ %+.3f  ride %.3f  "
               "wheelbase %.2f  track %.2f  clearance %.3f\n",
               carprof.name, carprof.wheel_r, carprof.wheel_rx, carprof.wheel_rz,
               carprof.wheel_w,
               carprof.has_tire ? "" : " (no tyre mesh: derived from body length)",
               carprof.hub_z, carprof.ride, carprof.wheelbase, carprof.track_f,
               carprof.clearance);
        wheelmesh = make_wheel(wR, wHW); have_wheel = 1;
        /* decode + upload each distinct texture actually bound by a mesh */
        for (int i = 0; i < ncar; i++) {
            uint32_t tk = car.meshes[i].texkey; if (!tk) continue;
            int seen = 0; for (int j = 0; j < nmap; j++) if (mapkey[j]==tk) seen = 1;
            if (seen || nmap >= 32) continue;
            N2Tex ct;
            if (n2_load_car_tex_by_key(ctdata, ctlen, tk, &ct)) {
                mapkey[nmap] = tk; maptex[nmap] = upload_tpk_texture_to_gpu(&ct);
                mapalpha[nmap] = ct.alpha != NULL;   /* DXT3 = decal mask */
                nmap++;
                /* car textures are atlases (UVs in [0,1]): clamp so REPEAT
                   wrap + mip filtering can't bleed the opposite border in. */
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                free(ct.rgb); free(ct.alpha); free(ct.dxt);
            }
        }
        printf("car textures bound: %d distinct\n", nmap);
        /* sponsor vinyl layer: VINYLS.BIN is one big TPK whose offset-slots
           point at EA "HUFF" (Huffman) blobs, not JDLZ — no open decoder
           exists (Nikki et al call EA's closed LZCompressLib.dll), so every
           decode below fails cleanly today and the car keeps its badge
           atlas. The compositing (paint -> vinyl -> badges, one texture)
           lights up as soon as a HUFF decoder lands in nfsu2.h.
           ponytail: first-fit vinyl choice — a vinyl-select menu can replace
           the pick without touching the compositing. */
        {
            char vpath[512];
            snprintf(vpath, sizeof vpath, "%s/CARS/%s/VINYLS.BIN", dataroot, carname);
            long vlen; unsigned char *vdata = n2_read_file(vpath, &vlen);
            uint32_t bodykey = 0;
            for (int i = 0; i < ncar && !bodykey; i++) {
                int c = car.meshes[i].cat;
                if ((c == N2_CAR_BODY || c == N2_CAR_MISC) && car.meshes[i].texkey)
                    for (int j = 0; j < nmap; j++)
                        if (mapkey[j] == car.meshes[i].texkey && mapalpha[j])
                            bodykey = mapkey[j];
            }
            static uint32_t vkeys[512];
            int nvk = vdata ? n2_car_tex_keys(vdata, vlen, vkeys, 512) : 0;
            N2Tex vt; int got = 0; uint32_t gotkey = 0;
            for (int k = 0; k < nvk && !got; k++) {
                if (!n2_load_car_tex_by_key(vdata, vlen, vkeys[k], &vt)) continue;
                if (vt.alpha) {
                    long n = (long)vt.w * vt.h, op = 0;
                    for (long p = 0; p < n; p++) if (vt.alpha[p] > 128) op++;
                    float f = (float)op / (float)n;
                    if (f > 0.04f && f < 0.55f) { got = 1; gotkey = vkeys[k]; break; }
                }
                free(vt.rgb); free(vt.alpha); free(vt.dxt);
            }
            N2Tex bt;
            if (got && bodykey && n2_load_car_tex_by_key(ctdata, ctlen, bodykey, &bt)) {
                int W = bt.w > vt.w ? bt.w : vt.w, H = bt.h > vt.h ? bt.h : vt.h;
                N2Tex out = { W, H, (unsigned char *)malloc((long)W*H*3),
                                    (unsigned char *)malloc((long)W*H), NULL, 0, 0, 0 };
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                    long o  = (long)y*W + x;
                    long bo = (long)(y*bt.h/H)*bt.w + x*bt.w/W;   /* nearest */
                    long vo = (long)(y*vt.h/H)*vt.w + x*vt.w/W;
                    int ba = bt.alpha[bo];                        /* badge over vinyl */
                    for (int ch = 0; ch < 3; ch++)
                        out.rgb[o*3+ch] = (unsigned char)
                            ((bt.rgb[bo*3+ch]*ba + vt.rgb[vo*3+ch]*(255-ba)) / 255);
                    out.alpha[o] = ba > vt.alpha[vo] ? (unsigned char)ba : vt.alpha[vo];
                }
                for (int j = 0; j < nmap; j++) if (mapkey[j] == bodykey) {
                    glDeleteTextures(1, &maptex[j]);
                    maptex[j] = upload_tex(&out);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    /* updates the badge atlas's OWN GL texture in place, so
                       any mesh that references this key directly (its own
                       0x134012 slot list, e.g. GOLF's BASE_A) picks up the
                       vinyl-under-badge composite automatically in the draw
                       loop — no separate fallback texture needed. */
                    break;
                }
                printf("vinyl layer: key %08x (%dx%d) composited under the badge atlas\n",
                       gotkey, vt.w, vt.h);
                free(out.rgb); free(out.alpha); free(bt.rgb); free(bt.alpha); free(bt.dxt);
            }
            if (got) { free(vt.rgb); free(vt.alpha); free(vt.dxt); }
            free(vdata);
        }
        /* spawn on the road mesh nearest the track centre; aim inward so a
           straight run stays on the populated track (user steers in play). */
        float bestd = 1e30f;
        for (int i = 0; i < nm; i++) if (scene.meshes[i].cat == N2_ROAD) {
            float vx = scene.meshes[i].verts[0], vy = scene.meshes[i].verts[1];
            float d = (vx-cx)*(vx-cx) + (vy-cy)*(vy-cy);
            if (d < bestd) { bestd = d;
                spawn[0]=vx; spawn[1]=vy; spawn[2]=scene.meshes[i].verts[2]; }
        }
        heading0 = atan2f(cy - spawn[1], cx - spawn[0]);
        printf("car: %d meshes, spawn (%.1f,%.1f) heading %.2f\n",
               ncar, spawn[0], spawn[1], heading0);
    }

    /* Static-capture spawn (Milestone 80). Everything above is the shipped
       interactive/menu/race spawn and stays exactly as it was; this only runs
       for --shot-static / --static-spawn-audit and, when it finds a valid
       candidate, replaces `spawn` before the camera is seeded. */
    if (sstatic || sspawn || sstack) {
        const float (*mbb)[4] = (const float (*)[4])world.mbb;
        float oldsp[3] = { spawn[0], spawn[1], spawn[2] };
        if (citypose) {
            /* --city-pose: seed the SAME safe-road selector at the built-up
               centroid instead of the shipped spawn, so the pose it returns is
               the nearest road candidate to the dense city/airport content that
               still passes road/patch/wall/headroom. No arbitrary placement:
               only the seed moves, every acceptance test below is unchanged. */
            double cx = 0, cy = 0, cz = 0; long cn = 0;
            if (citypose == 4) {
                oldsp[0] = poseseed[0]; oldsp[1] = poseseed[1];
                densx = oldsp[0]; densy = oldsp[1];
                printf("city pose: seeding the safe-road selector at the given "
                       "seed (%.1f, %.1f)\n", oldsp[0], oldsp[1]);
            } else
            for (int i = 0; i < nm; i++) {
                int sc2 = scene.meshes[i].scen;
                if (citypose == 3) { if (!scene.meshes[i].vrepair) continue; }
                else if (citypose == 2) { if (sc2 != N2_SC_STRUCT) continue; }
                else if (sc2 != N2_SC_BUILDING && sc2 != N2_SC_STRUCT) continue;
                for (int v = 0; v < scene.meshes[i].nverts; v++) {
                    const float *p = scene.meshes[i].verts + v*5;
                    cx += p[0]; cy += p[1]; cz += p[2]; cn++;
                }
            }
            if (cn) {
                oldsp[0] = (float)(cx/cn); oldsp[1] = (float)(cy/cn); oldsp[2] = (float)(cz/cn);
                printf("city pose: seeding the safe-road selector at the "
                       "%s centroid (%.1f, %.1f, %.1f) over %ld vertices\n",
                       citypose == 3 ? "vertex-repaired-object" :
                       citypose == 2 ? "STRUCT" : "BUILDING/STRUCT",
                       oldsp[0], oldsp[1], oldsp[2], cn);
                densx = oldsp[0]; densy = oldsp[1];   /* the selector orders by THIS */
            } else printf("city pose: no BUILDING/STRUCT geometry; seed unchanged\n");
        }
        /* candidates: road-mesh vertices, nearest the dense build-up centre
           first, so the first one that passes every test IS the nearest valid */
        /* Sample road vertices across the WHOLE map: count first, then stride so
           the cap can never truncate to the meshes that happen to load first
           (that left the true nearest candidates untested). x, y, z, dist2. */
        #define SS_MAXCAND 65536
        static float cand[SS_MAXCAND][4];
        long totalrv = 0;
        for (int i = 0; i < nm; i++)
            if (scene.meshes[i].cat == N2_ROAD) totalrv += scene.meshes[i].nverts;
        int step = (int)(totalrv / SS_MAXCAND) + 1;
        int ncand = 0;
        for (int i = 0; i < nm && ncand < SS_MAXCAND; i++) {
            if (scene.meshes[i].cat != N2_ROAD) continue;
            for (int v = 0; v < scene.meshes[i].nverts && ncand < SS_MAXCAND; v += step) {
                float *p = scene.meshes[i].verts + v*5;
                cand[ncand][0] = p[0]; cand[ncand][1] = p[1]; cand[ncand][2] = p[2];
                cand[ncand][3] = (p[0]-densx)*(p[0]-densx) + (p[1]-densy)*(p[1]-densy);
                ncand++;
            }
        }
        /* nearest-first by selection, not a sort: the first candidate that passes
           every test is by construction the nearest valid one, and typically only
           a handful are ever probed. */
        static unsigned char used[SS_MAXCAND];
        memset(used, 0, (size_t)ncand);
        /* car footprint from the loaded body AABB; modest fallback if no car */
        float half_l = (carbb[3]-carbb[0]) * 0.5f, half_w = (carbb[4]-carbb[1]) * 0.5f;
        if (half_l < 0.5f) half_l = 2.20f;
        if (half_w < 0.5f) half_w = 0.90f;
        int chosen = -1; float czl = 0, cclear = 0, chead = 0;
        static float cprobe[5][4];
        int tried = 0, rej_road = 0, rej_wall = 0, rej_low = 0, rej_patch = 0;
        for (int pass = 0; pass < ncand; pass++) {
            int a = -1; float bd = 1e30f;
            for (int q = 0; q < ncand; q++)
                if (!used[q] && cand[q][3] < bd) { bd = cand[q][3]; a = q; }
            if (a < 0) break;
            used[a] = 1;
            float x = cand[a][0], y = cand[a][1], vz = cand[a][2], rz;
            tried++;
            if (!ss_road_z(&scene, mbb, x, y, vz, &rz))        { rej_road++; continue; }
            if (ss_in_wall(obst, nobst, x, y, 1.3f))           { rej_wall++; continue; }
            float ceil = ss_ceiling_above(&scene, mbb, x, y, rz);
            if (ceil - rz < SS_CLEAR_M)                        { rej_low++;  continue; }
            /* the whole car footprint must stand on road, at the heading the
               static camera will use (M82) */
            float hd = atan2f(densy - y, densx - x);
            float pr[5][4];
            if (!ss_patch(&scene, mbb, x, y, rz, hd, half_l, half_w, pr)) { rej_patch++; continue; }
            chosen = a; czl = rz; cclear = ceil - rz; chead = hd;
            memcpy(cprobe, pr, sizeof cprobe);
            break;
        }
        if (chosen >= 0) {
            spawn[0] = cand[chosen][0]; spawn[1] = cand[chosen][1]; spawn[2] = czl;
            heading0 = atan2f(densy - spawn[1], densx - spawn[0]);
        }
        if (camat) {   /* diagnostic: put the same static capture at a given XY */
            spawn[0] = camx; spawn[1] = camy;
            spawn[2] = world_ground_z(&scene, camx, camy, 0.0f);
            printf("cam-at: static capture moved to (%.3f, %.3f, %.3f)\n",
                   spawn[0], spawn[1], spawn[2]);
        }
        if (sspawn) {
            float orz = 0, oclear;
            int oroad = ss_road_z(&scene, mbb, oldsp[0], oldsp[1], oldsp[2], &orz);
            float obase = oroad ? orz : oldsp[2];
            oclear = ss_ceiling_above(&scene, mbb, oldsp[0], oldsp[1], obase);
            printf("\nMILESTONE: 80  static-spawn-audit  track=%s\n", trackname);
            printf("dense build-up centre   (%.1f, %.1f)\n", densx, densy);
            printf("candidates             %d road vertices, %d tested\n", ncand, tried);
            printf("  rejected: no road tri %d   in wall footprint %d   headroom < %.0f m %d"
                   "   incomplete road patch %d\n",
                   rej_road, rej_wall, (double)SS_CLEAR_M, rej_low, rej_patch);
            printf("footprint              half-length %.3f  half-width %.3f  (from %s body AABB)\n",
                   half_l, half_w, carname);
            printf("OLD candidate          (%.3f, %.3f, %.3f)  road_tri=%s  ground_z=%.3f\n"
                   "                       overhead=%s  wall=%s  dist_to_dense=%.1f m\n",
                   oldsp[0], oldsp[1], oldsp[2], oroad ? "yes" : "NO",
                   oroad ? orz : oldsp[2],
                   oclear > 1e29f ? "open" : "BLOCKED",
                   ss_in_wall(obst, nobst, oldsp[0], oldsp[1], 1.3f) ? "INSIDE" : "clear",
                   sqrtf((oldsp[0]-densx)*(oldsp[0]-densx) + (oldsp[1]-densy)*(oldsp[1]-densy)));
            if (oclear > 1e29f)
                printf("                       overhead clearance: open (no surface above)\n");
            else
                printf("                       overhead clearance: %.2f m (surface at z=%.2f) -> %s\n",
                       oclear - obase, oclear,
                       (oclear - obase) < SS_CLEAR_M ? "FAILS the 8 m test" : "passes");
            if (chosen >= 0)
                printf("NEW candidate          (%.3f, %.3f, %.3f)  road_tri=yes  ground_z=%.3f\n"
                       "                       wall=clear  dist_to_dense=%.1f m\n",
                       spawn[0], spawn[1], spawn[2], czl, sqrtf(cand[chosen][3]));
            else
                printf("NEW candidate          FALLBACK -- no road vertex passed all tests; "
                       "static spawn left at the old point\n");
            if (chosen >= 0) {
                if (cclear > 1e29f)
                    printf("                       overhead clearance: open (no surface above) -> passes\n");
                else
                    printf("                       overhead clearance: %.2f m -> passes\n", cclear);
                printf("                       candidate road Z %.3f   heading %.4f rad\n", czl, chead);
                static const char *pl[5] = { "centre", "front-left", "front-right",
                                             "rear-left", "rear-right" };
                printf("  %-12s %12s %12s  %-8s %9s\n", "probe","X","Y","road","dZ");
                for (int q = 0; q < 5; q++)
                    printf("  %-12s %12.3f %12.3f  %-8s %+9.3f\n", pl[q],
                           cprobe[q][0], cprobe[q][1],
                           cprobe[q][2] > 0.5f ? "yes" : "NO", cprobe[q][3]);
            }
            /* same five probes on the OLD point, for comparison */
            {
                float ohd = atan2f(densy - oldsp[1], densx - oldsp[0]);
                float opr[5][4];
                int opass = ss_patch(&scene, mbb, oldsp[0], oldsp[1],
                                     obase, ohd, half_l, half_w, opr);
                printf("OLD footprint patch    %s (heading %.4f rad)\n",
                       opass ? "complete" : "INCOMPLETE", ohd);
                static const char *pl2[5] = { "centre", "front-left", "front-right",
                                              "rear-left", "rear-right" };
                for (int q = 0; q < 5; q++)
                    printf("  %-12s %12.3f %12.3f  %-8s %+9.3f\n", pl2[q],
                           opr[q][0], opr[q][1],
                           opr[q][2] > 0.5f ? "yes" : "NO", opr[q][3]);
            }
            return 0;
        }

        /* --surface-stack (Milestone 81): every covering ROAD/TERRAIN triangle
           at an XY, plus what each layer query answers there and where the car
           body actually sits. Read-only; nothing here feeds the renderer. */
        if (sstack) {
            static SSHit hit[4096];
            float ride = carprof.ride;
            float probe[9][2]; int np = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                probe[np][0] = stx + dx*2.0f; probe[np][1] = sty + dy*2.0f; np++;
            }
            printf("\nMILESTONE: 81  surface-stack  track=%s at (%.3f, %.3f)\n",
                   trackname, stx, sty);
            printf("static spawn (M80)  (%.3f, %.3f, %.3f)\n", spawn[0], spawn[1], spawn[2]);
            printf("car profile %-10s wheelR/ride %.3f  body z [%.3f .. %.3f]\n",
                   carname, ride, carbb[2], carbb[5]);
            printf("  chassis bottom = spawnZ + ride + body_z0 = %.3f\n", spawn[2] + ride + carbb[2]);
            printf("  chassis top    = spawnZ + ride + body_z1 = %.3f\n", spawn[2] + ride + carbb[5]);
            printf("  tyre contact   = spawnZ                  = %.3f\n", spawn[2]);
            for (int q = 0; q < np; q++) {
                float px = probe[q][0], py = probe[q][1];
                int n = ss_stack(&scene, mbb, px, py, hit, 4096);
                printf("\n-- (%.3f, %.3f)  %s  %d covering triangle(s)\n", px, py,
                       (q == 4) ? "[CENTRE]" : "", n);
                if (n) printf("%10s %-8s %-30s %6s %7s  %-22s %s\n",
                              "Z","category","asset name","mesh","tri","normal","source mesh bounds");
                for (int a = 0; a < n; a++) {
                    const N2Mesh *me = &scene.meshes[hit[a].mesh];
                    float b0[3]={1e30f,1e30f,1e30f}, b1[3]={-1e30f,-1e30f,-1e30f};
                    for (int v = 0; v < me->nverts; v++)
                        for (int c = 0; c < 3; c++) { float pp = me->verts[v*5+c];
                            if (pp<b0[c]) b0[c]=pp; if (pp>b1[c]) b1[c]=pp; }
                    char nb[32], bb[64];
                    snprintf(nb, sizeof nb, "(%5.2f %5.2f %5.2f)",
                             hit[a].n[0], hit[a].n[1], hit[a].n[2]);
                    snprintf(bb, sizeof bb, "[%.0f %.0f][%.0f %.0f][%.1f %.1f]",
                             b0[0],b1[0], b0[1],b1[1], b0[2],b1[2]);
                    printf("%10.3f %-8s %-30s %6d %7d  %-22s %s\n",
                           hit[a].z, hit[a].cat == N2_ROAD ? "ROAD" : "TERRAIN",
                           me->sname[0] ? me->sname : "(unnamed)",
                           hit[a].mesh, hit[a].tri, nb, bb);
                }
                /* what each query answers here, referenced to the static spawn Z */
                float gz = world_ground_z(&scene, px, py, spawn[2]);
                float nr = 1e30f, nt = 1e30f;
                for (int a = 0; a < n; a++) {
                    float d = hit[a].z - spawn[2]; if (d < 0) d = -d;
                    if (hit[a].cat == N2_ROAD) { float b = nr - spawn[2]; if (b<0) b=-b;
                        if (nr > 1e29f || d < b) nr = hit[a].z; }
                    else { float b = nt - spawn[2]; if (b<0) b=-b;
                        if (nt > 1e29f || d < b) nt = hit[a].z; }
                }
                printf("   world_ground_z(ref=spawnZ %.3f) = %.3f\n", spawn[2], gz);
                if (nr < 1e29f) printf("   nearest ROAD    = %.3f  (%+.3f vs spawnZ)\n", nr, nr - spawn[2]);
                else            printf("   nearest ROAD    = none\n");
                if (nt < 1e29f) printf("   nearest TERRAIN = %.3f  (%+.3f vs spawnZ)\n", nt, nt - spawn[2]);
                else            printf("   nearest TERRAIN = none\n");
            }
            return 0;
        }
    }

    /* Enumerate the selectable assets for the pre-race menu. Switching car or
       track means a whole new load, so those re-launch the process (below);
       circuit is a cheap in-place reload. */
    #define MAXTRACK WORLD_MAXREG
    #define MAXCARS  64
    #define MAXCIRC  24
    char tracklist[MAXTRACK][64]; int seltrack = 0;
    int ntrack = res_list_tracks(troot, tracklist, MAXTRACK, trackname, &seltrack);
    if (ntrack < MAXTRACK) {   /* city mode is selectable from the menu too */
        snprintf(tracklist[ntrack], sizeof tracklist[0], "ALL");
        if (!strcmp(trackname, "ALL")) seltrack = ntrack;
        ntrack++;
    }
    char carlist[MAXCARS][64]; int selcar = 0;
    int ncars = res_list_cars(dataroot, carlist, MAXCARS, carname, &selcar);
    char circlist[MAXCIRC][256]; int selcirc = 0;
    int ncirc = res_list_circuits(troot, trackname, circlist, MAXCIRC);
    /* honour an explicit --circuit only when it belongs to THIS region's own
       catalog; a foreign one is ignored rather than flinging the car into
       another bundle (M89). */
    if (explicit_circuit) {
        int found = 0;
        for (int i=0;i<ncirc;i++) if(!strcmp(circlist[i], circuit)) { selcirc=i; found=1; }
        if (!found)
            printf("--circuit %s is not in %s's route catalog (ROUTES%s) - ignored\n",
                   circuit, trackname,
                   strncmp(trackname,"STREAM",6) ? trackname : trackname+6);
    }
    printf("circuits available: %d (menu: Left/Right)\n", ncirc);
    /* Sprint events (Milestone 117). A region can ship race events but no closed
       circuit -- L4RB has 9 sprints and 0 loops -- and Enter only knew about the
       aipath circuit, so those regions fell through to the free-roam showcase.
       Collect this region's own events (world_load_events already tagged each
       with its ROUTES<stem> directory) so the same [ / ] selector can cycle them
       when there is no circuit list. */
    static int sprintev[WORLD_MAXEVENT]; int nsprint = 0, selsprint = 0;
    {
        const char *stem = strncmp(trackname, "STREAM", 6) ? trackname : trackname + 6;
        for (int i = 0; i < world.nev && nsprint < WORLD_MAXEVENT; i++)
            if (!strcmp(world.ev[i].reg, stem)) sprintev[nsprint++] = i;
        if (!ncirc && nsprint)
            printf("sprint events available: %d (menu: [ / ]) -- selected id %d "
                   "(%s, %d outline points)\n", nsprint,
                   world.ev[sprintev[0]].id,
                   world.ev[sprintev[0]].circuit ? "circuit" : "sprint",
                   world.ev[sprintev[0]].npoly);
    }
    if (raudit) { printf("RA circuit list for %s (bbox filter mn %.0f %.0f mx %.0f %.0f):\n",
                         trackname, mn[0], mn[1], mx[0], mx[1]);
                  for (int i = 0; i < ncirc; i++)
                      printf("RA   [%d]%s %s\n", i, i == selcirc ? "*" : " ", circlist[i]); }

    N2Path aipath = {0};
    AiCar ais[N_AI]; int start_idx = 0;
    /* load_circuit also REWRITES spawn/heading0 to the circuit start; a static
       capture must keep the region's own showcase spawn, so skip it entirely. */
    int nai = (ncirc && !sstatic) ? load_circuit(dataroot, circlist[selcirc], &scene, &aipath,
                                   ais, spawn, &heading0, &start_idx, densx, densy) : 0;
    if (nai) printf("circuit: %d-waypoint loop; %d AI racers, lap system on\n",
                    aipath.n, nai);

    /* --rail-band (M119): every near-vertical ROAD/TERRAIN face in the region,
       bucketed by height, with what the rail band admits. Static and geometry
       only -- it does not need the car to drive anywhere. */
    if (rband) {
        static const char *bn[8] = { "<0.5","0.5-1","1-1.5","1.5-2","2-3","3-5","5-10",">=10" };
        long cand[2][8] = {{0}}, keep[2][8] = {{0}};
        float kmin[2] = { 1e30f, 1e30f }, kmax[2] = { -1e30f, -1e30f };
        for (int i = 0; i < nm; i++) {
            const N2Mesh *m = &scene.meshes[i];
            if (m->cat != N2_ROAD && m->cat != N2_TERRAIN) continue;
            int ci = (m->cat == N2_ROAD) ? 0 : 1;
            for (int t = 0; t + 2 < m->nidx; t += 3) {
                const float *A = m->verts + m->idx[t]*5;
                const float *B = m->verts + m->idx[t+1]*5;
                const float *C = m->verts + m->idx[t+2]*5;
                float e1[3], e2[3], n3[3];
                for (int a = 0; a < 3; a++) { e1[a]=B[a]-A[a]; e2[a]=C[a]-A[a]; }
                n3[0]=e1[1]*e2[2]-e1[2]*e2[1]; n3[1]=e1[2]*e2[0]-e1[0]*e2[2];
                n3[2]=e1[0]*e2[1]-e1[1]*e2[0];
                float L = sqrtf(n3[0]*n3[0]+n3[1]*n3[1]+n3[2]*n3[2]); if (L<1e-9f) continue;
                if (fabsf(n3[2]/L) >= 0.30f) continue;
                float zlo=A[2], zhi=A[2];
                if(B[2]<zlo)zlo=B[2]; if(C[2]<zlo)zlo=C[2];
                if(B[2]>zhi)zhi=B[2]; if(C[2]>zhi)zhi=C[2];
                float zs = zhi - zlo;
                int b = zs<0.5f?0: zs<1.0f?1: zs<1.5f?2: zs<2.0f?3:
                        zs<3.0f?4: zs<5.0f?5: zs<10.0f?6:7;
                cand[ci][b]++;
                if (zs >= 0.75f && zs <= 2.5f) {     /* the shipped rail band */
                    keep[ci][b]++;
                    if (zs < kmin[ci]) kmin[ci] = zs;
                    if (zs > kmax[ci]) kmax[ci] = zs;
                }
            }
        }
        printf("\nMILESTONE: 119  rail-band survey  track=%s  (band 0.75 .. 2.50 m)\n",
               trackname);
        printf("  %-8s %-10s", "source", "");
        for (int b = 0; b < 8; b++) printf(" %10s", bn[b]);
        printf("   (near-vertical face Z span)\n");
        for (int c = 0; c < 2; c++) {
            printf("  %-8s %-10s", c ? "TERRAIN" : "ROAD", "faces");
            for (int b = 0; b < 8; b++) printf(" %10ld", cand[c][b]);
            printf("\n  %-8s %-10s", "", "in band");
            for (int b = 0; b < 8; b++) printf(" %10ld", keep[c][b]);
            printf("   kept Zspan %.3f .. %.3f\n",
                   kmin[c] > 1e29f ? 0.0f : kmin[c], kmax[c] < -1e29f ? 0.0f : kmax[c]);
        }
        printf("\n");
    }

    /* --grid-audit (M118): every shipped 0x34146 start-grid record for one event,
       decoded field by field, with the supporting layer under each slot. Same
       file, leaf and 48-byte stride race_load_grid walks. Read-only. */
    if (gaudit) {
        const char *stem = strncmp(trackname, "STREAM", 6) ? trackname : trackname + 6;
        char gp[1024]; snprintf(gp, sizeof gp, "%s/ROUTES%s/TrackPosMarkersAll.bin",
                                troot, stem);
        long glen = 0; unsigned char *gd = n2_read_file(gp, &glen);
        printf("\nMILESTONE: 118  start-grid audit  event %d  %s\n", gaudit, gp);
        if (!gd) printf("  cannot read the marker file\n");
        else {
            N2Leaf lf[4]; int nl = 0;
            n2_find_leaves(gd, 0, glen, 0x00034146u, lf, &nl, 4);
            int slot = 0;
            for (int L = 0; L < nl; L++) {
                long off = lf[L].off + 8, end = lf[L].off + lf[L].size;
                for (; off + 48 <= end; off += 48) {
                    unsigned tid; memcpy(&tid, gd + off + 36, 4);
                    if ((int)tid != gaudit) continue;
                    float f[12];
                    for (int k = 0; k < 12; k++) memcpy(&f[k], gd + off + k*4, 4);
                    float sx = f[4], sy = f[5];
                    float gz2 = 0;
                    int cat = world_ground_at(&scene, sx, sy, f[6], &gz2);
                    printf("  slot %-2d  XY(%10.3f %10.3f)  rec+24 %9.3f  "
                           "support %-7s z %9.3f  dir(%+.4f %+.4f) hdg %+7.4f\n",
                           slot, sx, sy, f[6],
                           cat == WSURF_ROAD ? "ROAD" : cat == WSURF_TERRAIN ? "TERRAIN"
                                                                             : "NONE",
                           gz2, f[0], f[1], atan2f(f[1], f[0]));
                    slot++;
                }
            }
            printf("  %d slots for event %d\n", slot, gaudit);
            free(gd);
            if (world.race.active) {
                printf("  armed race gates (%d):\n", world.race.ngate);
                for (int g = 0; g < world.race.ngate; g++) {
                    const WGate *G = &world.race.gate[g];
                    float gz3 = 0;
                    int c3 = world_ground_at(&scene, G->x, G->y, 0.0f, &gz3);
                    printf("    gate %-2d (%10.3f %10.3f) dir(%+.4f %+.4f) hdg %+7.4f "
                           "half %.1f  support %-7s z %9.3f\n", g, G->x, G->y,
                           G->dx, G->dy, atan2f(G->dy, G->dx), G->half,
                           c3 == WSURF_ROAD ? "ROAD" : c3 == WSURF_TERRAIN ? "TERRAIN"
                                                                           : "NONE", gz3);
                }
            } else printf("  (no race armed: run with --event %d to see its gates)\n", gaudit);
        }
        printf("\n");
    }

    /* --wall-probe (M112): is a building collider a real wall at this XY, or only
       the coarse full-XY AABB that phys_collect_walls stores? Read-only. */
    if (wprobe) {
        printf("\nM112 wall-probe  mesh \"%s\"  at (%.3f, %.3f)  track=%s\n",
               wprobe, wpx, wpy, trackname);
        for (int i = 0; i < nm; i++) {
            const N2Mesh *m = &scene.meshes[i];
            if (strcmp(m->sname, wprobe)) continue;
            float bb[6] = {1e30f,-1e30f,1e30f,-1e30f,1e30f,-1e30f};
            for (int q = 0; q < m->nverts; q++) { const float *p2 = m->verts + q*5;
                if(p2[0]<bb[0])bb[0]=p2[0]; if(p2[0]>bb[1])bb[1]=p2[0];
                if(p2[1]<bb[2])bb[2]=p2[1]; if(p2[1]>bb[3])bb[3]=p2[1];
                if(p2[2]<bb[4])bb[4]=p2[2]; if(p2[2]>bb[5])bb[5]=p2[2]; }
            printf("  mesh %d  class %s  cat %s  verts %d  tris %d  texkey %08x\n",
                   i, n2_scen_name(m->scen), bc_cat(m->cat), m->nverts, m->nidx/3, m->texkey);
            printf("  AABB XY[%.1f %.1f][%.1f %.1f] Z[%.1f %.1f]  = %.1f x %.1f x %.1f m\n",
                   bb[0],bb[1], bb[2],bb[3], bb[4],bb[5],
                   bb[1]-bb[0], bb[3]-bb[2], bb[5]-bb[4]);
            /* nearest triangle in XY, and whether its face is a wall */
            float bnear = 1e30f, bnz = 0, bz0 = 0, bz1 = 0; int btri = -1;
            float bnearw = 1e30f; int btriw = -1; float bwz0 = 0, bwz1 = 0;
            for (int t = 0; t + 2 < m->nidx; t += 3) {
                const float *A = m->verts + m->idx[t]*5;
                const float *B = m->verts + m->idx[t+1]*5;
                const float *C = m->verts + m->idx[t+2]*5;
                float e1[3], e2[3], n3[3];
                for (int a = 0; a < 3; a++) { e1[a]=B[a]-A[a]; e2[a]=C[a]-A[a]; }
                n3[0]=e1[1]*e2[2]-e1[2]*e2[1]; n3[1]=e1[2]*e2[0]-e1[0]*e2[2];
                n3[2]=e1[0]*e2[1]-e1[1]*e2[0];
                float L = sqrtf(n3[0]*n3[0]+n3[1]*n3[1]+n3[2]*n3[2]); if (L<1e-9f) continue;
                float nz = n3[2]/L;
                float ox, oy, d2 = 1e30f, o2;
                o2 = seg_d2_m112(wpx,wpy,A[0],A[1],B[0],B[1],&ox,&oy); if(o2<d2)d2=o2;
                o2 = seg_d2_m112(wpx,wpy,B[0],B[1],C[0],C[1],&ox,&oy); if(o2<d2)d2=o2;
                o2 = seg_d2_m112(wpx,wpy,C[0],C[1],A[0],A[1],&ox,&oy); if(o2<d2)d2=o2;
                float zlo=A[2],zhi=A[2];
                if(B[2]<zlo)zlo=B[2]; if(C[2]<zlo)zlo=C[2];
                if(B[2]>zhi)zhi=B[2]; if(C[2]>zhi)zhi=C[2];
                if (d2 < bnear) { bnear = d2; btri = t/3; bnz = nz; bz0 = zlo; bz1 = zhi; }
                if (fabsf(nz) < 0.30f && d2 < bnearw) { bnearw = d2; btriw = t/3;
                                                       bwz0 = zlo; bwz1 = zhi; }
            }
            if (btri >= 0)
                printf("  nearest triangle       %.3f m  tri %d  nz %+.3f  triZ[%.2f %.2f]\n",
                       sqrtf(bnear), btri, bnz, bz0, bz1);
            if (wpzset) {   /* the decisive test: wall faces AT THE CAR'S HEIGHT */
                float cz0 = wpz + carprof.ride + carbb[2];
                float cz1 = wpz + carprof.ride + carbb[5];
                float bz = 1e30f; int bt = -1; float b0 = 0, b1 = 0;
                for (int t = 0; t + 2 < m->nidx; t += 3) {
                    const float *A = m->verts + m->idx[t]*5;
                    const float *B = m->verts + m->idx[t+1]*5;
                    const float *C = m->verts + m->idx[t+2]*5;
                    float e1[3], e2[3], n3[3];
                    for (int a = 0; a < 3; a++) { e1[a]=B[a]-A[a]; e2[a]=C[a]-A[a]; }
                    n3[0]=e1[1]*e2[2]-e1[2]*e2[1]; n3[1]=e1[2]*e2[0]-e1[0]*e2[2];
                    n3[2]=e1[0]*e2[1]-e1[1]*e2[0];
                    float L = sqrtf(n3[0]*n3[0]+n3[1]*n3[1]+n3[2]*n3[2]); if (L<1e-9f) continue;
                    if (fabsf(n3[2]/L) >= 0.30f) continue;
                    float zlo=A[2], zhi=A[2];
                    if(B[2]<zlo)zlo=B[2]; if(C[2]<zlo)zlo=C[2];
                    if(B[2]>zhi)zhi=B[2]; if(C[2]>zhi)zhi=C[2];
                    if (zhi < cz0 || zlo > cz1) continue;
                    float ox, oy, d2 = 1e30f, o2;
                    o2 = seg_d2_m112(wpx,wpy,A[0],A[1],B[0],B[1],&ox,&oy); if(o2<d2)d2=o2;
                    o2 = seg_d2_m112(wpx,wpy,B[0],B[1],C[0],C[1],&ox,&oy); if(o2<d2)d2=o2;
                    o2 = seg_d2_m112(wpx,wpy,C[0],C[1],A[0],A[1],&ox,&oy); if(o2<d2)d2=o2;
                    if (d2 < bz) { bz = d2; bt = t/3; b0 = zlo; b1 = zhi; }
                }
                printf("  car envelope Z[%.3f %.3f] (contact %.3f + ride %.3f + body)\n",
                       cz0, cz1, wpz, carprof.ride);
                if (bt >= 0)
                    printf("  nearest wall face AT CAR HEIGHT  %.3f m  tri %d  triZ[%.2f %.2f]"
                           "  -> %s\n", sqrtf(bz), bt, b0, b1,
                           sqrtf(bz) <= 1.3f ? "REAL CONTACT (within r=1.3)"
                                             : "no contact at r=1.3");
                else
                    printf("  nearest wall face AT CAR HEIGHT  none -- the rect here is a "
                           "FALSE POSITIVE\n");
            }
            if (btriw >= 0)
                printf("  nearest WALL face      %.3f m  tri %d  triZ[%.2f %.2f]  (|nz| < 0.30)\n",
                       sqrtf(bnearw), btriw, bwz0, bwz1);
            else printf("  nearest WALL face      none in this mesh\n");
            /* how much of the stored AABB footprint the geometry actually occupies */
            const int G = 64; int hit = 0;
            for (int gy = 0; gy < G; gy++) for (int gx = 0; gx < G; gx++) {
                float sx = bb[0] + (bb[1]-bb[0]) * (gx+0.5f)/G;
                float sy = bb[2] + (bb[3]-bb[2]) * (gy+0.5f)/G;
                for (int t = 0; t + 2 < m->nidx; t += 3) {
                    const float *A = m->verts + m->idx[t]*5;
                    const float *B = m->verts + m->idx[t+1]*5;
                    const float *C = m->verts + m->idx[t+2]*5;
                    float d = (B[1]-C[1])*(A[0]-C[0]) + (C[0]-B[0])*(A[1]-C[1]);
                    if (d > -1e-9f && d < 1e-9f) continue;
                    float u = ((B[1]-C[1])*(sx-C[0]) + (C[0]-B[0])*(sy-C[1])) / d;
                    float v = ((C[1]-A[1])*(sx-C[0]) + (A[0]-C[0])*(sy-C[1])) / d;
                    if (u < 0 || v < 0 || 1.0f-u-v < 0) continue;
                    hit++; break;
                }
            }
            printf("  AABB footprint occupancy %.1f%% (%d of %d sample cells contain "
                   "projected geometry)\n", 100.0*hit/(G*G), hit, G*G);
            printf("  probe point is %s the stored rect (r=0 test)\n",
                   (wpx > bb[0] && wpx < bb[1] && wpy > bb[2] && wpy < bb[3])
                     ? "INSIDE" : "outside");
        }
        printf("\n");
    }

    /* --cover-probe (M110 visual recovery): every mesh with a triangle covering
       one XY, regardless of category -- so ground that exists but is not
       classified as ground is visible in the evidence. Read-only. */
    if (cprobe) {
        printf("\nM110 cover-probe at (%.3f, %.3f)  track=%s\n", cpx, cpy, trackname);
        printf("  %-30s %-8s %-8s %10s %9s %-34s\n",
               "asset name", "cat", "class", "texkey", "z", "world XY / Z bounds");
        int nfound = 0;
        for (int i = 0; i < nm; i++) {
            const N2Mesh *m = &scene.meshes[i];
            if (cpx < world.mbb[i][0] || cpx > world.mbb[i][2] ||
                cpy < world.mbb[i][1] || cpy > world.mbb[i][3]) continue;
            for (int t = 0; t + 2 < m->nidx; t += 3) {
                const float *A = m->verts + m->idx[t]*5;
                const float *B = m->verts + m->idx[t+1]*5;
                const float *C = m->verts + m->idx[t+2]*5;
                float d = (B[1]-C[1])*(A[0]-C[0]) + (C[0]-B[0])*(A[1]-C[1]);
                if (d > -1e-9f && d < 1e-9f) continue;
                float u = ((B[1]-C[1])*(cpx-C[0]) + (C[0]-B[0])*(cpy-C[1])) / d;
                float v = ((C[1]-A[1])*(cpx-C[0]) + (A[0]-C[0])*(cpy-C[1])) / d;
                float w = 1.0f - u - v;
                if (u < 0 || v < 0 || w < 0) continue;
                float z = u*A[2] + v*B[2] + w*C[2];
                float bb[6] = {1e30f,-1e30f,1e30f,-1e30f,1e30f,-1e30f};
                for (int q = 0; q < m->nverts; q++) { const float *pv = m->verts + q*5;
                    if(pv[0]<bb[0])bb[0]=pv[0]; if(pv[0]>bb[1])bb[1]=pv[0];
                    if(pv[1]<bb[2])bb[2]=pv[1]; if(pv[1]>bb[3])bb[3]=pv[1];
                    if(pv[2]<bb[4])bb[4]=pv[2]; if(pv[2]>bb[5])bb[5]=pv[2]; }
                char bs[64]; snprintf(bs, sizeof bs, "[%.0f %.0f][%.0f %.0f][%.1f %.1f]",
                                      bb[0],bb[1], bb[2],bb[3], bb[4],bb[5]);
                printf("  %-30s %-8s %-8s %10.8x %9.3f %-34s\n",
                       m->sname[0] ? m->sname : "(unnamed)", bc_cat(m->cat),
                       n2_scen_name(m->scen), m->texkey, z, bs);
                nfound++;
                break;
            }
        }
        printf("  total covering meshes: %d   world_ground_z(ref -12.087) = %.3f\n",
               nfound, world_ground_z(&scene, cpx, cpy, -12.087f));
        printf("\n");
    }

    /* Showcase/menu pose (Milestone 108). L4RA's shipped showcase spot is the
       dense build-up centre, and that XY has NO covering ROAD/TERRAIN triangle
       at all (M106): world_ground_z returns its own reference there, so the car
       hangs at whatever Z the previous spawn happened to have. Re-pick it with
       the same triangle tests --shot-static uses, take Z from the chosen
       candidate's OWN supporting road layer, and face the car along the local
       road (reversed tangent, M107) instead of at a point hundreds of metres
       away. Menu/showcase only: the Enter branch, sl_first_safe, the race route
       start and the --shot-static selector are all downstream and untouched, and
       if nothing passes, the shipped pose is left exactly as it was. */
    if (!sstatic && !sspawn && !sstack &&
        (citypose || !strcmp(trackname, "STREAML4RA"))) {
        const float (*wmbb)[4] = (const float (*)[4])world.mbb;
        float hl = (carbb[3]-carbb[0]) * 0.5f, hw = (carbb[4]-carbb[1]) * 0.5f;
        if (hl < 0.5f) hl = 2.20f;
        if (hw < 0.5f) hw = 0.90f;
        float oldsp[3] = { spawn[0], spawn[1], spawn[2] };
        if (citypose) {
            /* --city-pose: seed the SAME safe-road selector at the built-up
               centroid instead of the shipped spawn, so the pose it returns is
               the nearest road candidate to the dense city/airport content that
               still passes road/patch/wall/headroom. No arbitrary placement:
               only the seed moves, every acceptance test below is unchanged. */
            double cx = 0, cy = 0, cz = 0; long cn = 0;
            if (citypose == 4) {
                oldsp[0] = poseseed[0]; oldsp[1] = poseseed[1];
                densx = oldsp[0]; densy = oldsp[1];
                printf("city pose: seeding the safe-road selector at the given "
                       "seed (%.1f, %.1f)\n", oldsp[0], oldsp[1]);
            } else
            for (int i = 0; i < nm; i++) {
                int sc2 = scene.meshes[i].scen;
                if (citypose == 3) { if (!scene.meshes[i].vrepair) continue; }
                else if (citypose == 2) { if (sc2 != N2_SC_STRUCT) continue; }
                else if (sc2 != N2_SC_BUILDING && sc2 != N2_SC_STRUCT) continue;
                for (int v = 0; v < scene.meshes[i].nverts; v++) {
                    const float *p = scene.meshes[i].verts + v*5;
                    cx += p[0]; cy += p[1]; cz += p[2]; cn++;
                }
            }
            if (cn) {
                oldsp[0] = (float)(cx/cn); oldsp[1] = (float)(cy/cn); oldsp[2] = (float)(cz/cn);
                printf("city pose: seeding the safe-road selector at the "
                       "%s centroid (%.1f, %.1f, %.1f) over %ld vertices\n",
                       citypose == 3 ? "vertex-repaired-object" :
                       citypose == 2 ? "STRUCT" : "BUILDING/STRUCT",
                       oldsp[0], oldsp[1], oldsp[2], cn);
                densx = oldsp[0]; densy = oldsp[1];   /* the selector orders by THIS */
            } else printf("city pose: no BUILDING/STRUCT geometry; seed unchanged\n");
        }
        #define SC_MAXC 65536
        static float sc[SC_MAXC][4]; long trv = 0;
        for (int i = 0; i < nm; i++)
            if (scene.meshes[i].cat == N2_ROAD) trv += scene.meshes[i].nverts;
        int stp = (int)(trv / SC_MAXC) + 1, nc = 0;
        for (int i = 0; i < nm && nc < SC_MAXC; i++) {
            if (scene.meshes[i].cat != N2_ROAD) continue;
            for (int v = 0; v < scene.meshes[i].nverts && nc < SC_MAXC; v += stp) {
                float *p = scene.meshes[i].verts + v*5;
                sc[nc][0]=p[0]; sc[nc][1]=p[1]; sc[nc][2]=p[2];
                sc[nc][3]=(p[0]-oldsp[0])*(p[0]-oldsp[0]) + (p[1]-oldsp[1])*(p[1]-oldsp[1]);
                nc++;
            }
        }
        static unsigned char scu[SC_MAXC]; memset(scu, 0, (size_t)nc);
        int rj_road=0, rj_wall=0, rj_low=0, rj_patch=0, tried=0, found=0;
        float px=0, py=0, pz=0, phd=0;
        for (int pass = 0; pass < nc && !found; pass++) {
            int a = -1; float bd = 1e30f;
            for (int q = 0; q < nc; q++) if (!scu[q] && sc[q][3] < bd) { bd = sc[q][3]; a = q; }
            if (a < 0) break;
            scu[a] = 1; tried++;
            float x = sc[a][0], y = sc[a][1], vz = sc[a][2], rz;
            /* Z comes from this candidate's own vertex as the layer reference */
            if (!ss_road_z(&scene, wmbb, x, y, vz, &rz))    { rj_road++; continue; }
            if (ss_in_wall(obst, nobst, x, y, 1.3f))        { rj_wall++; continue; }
            float ceil = ss_ceiling_above(&scene, wmbb, x, y, rz);
            if (ceil - rz < SS_CLEAR_M)                     { rj_low++;  continue; }
            /* local road tangent from the supporting triangle's longest XY edge,
               reversed so the car presents its front three-quarter (M107) */
            static SSHit sh[8192];
            int nh = ss_stack(&scene, wmbb, x, y, sh, 8192);
            float tx = 1, ty = 0; int gotT = 0;
            for (int k = 0; k < nh && !gotT; k++) {
                if (sh[k].cat != N2_ROAD) continue;
                float dz = sh[k].z - rz; if (dz < 0) dz = -dz;
                if (dz > 0.05f) continue;
                const N2Mesh *me = &scene.meshes[sh[k].mesh];
                int t = sh[k].tri * 3;
                const float *A = me->verts + me->idx[t]*5;
                const float *B = me->verts + me->idx[t+1]*5;
                const float *C = me->verts + me->idx[t+2]*5;
                const float *e[3][2] = { {A,B}, {B,C}, {C,A} };
                float best = -1;
                for (int j = 0; j < 3; j++) {
                    float ex = e[j][1][0]-e[j][0][0], ey = e[j][1][1]-e[j][0][1];
                    float L = ex*ex + ey*ey;
                    if (L > best) { best = L; tx = ex; ty = ey; }
                }
                gotT = 1;
            }
            if (!gotT) { rj_patch++; continue; }
            float ht = atan2f(ty, tx);
            ht = ht > 0 ? ht - 3.14159265f : ht + 3.14159265f;   /* reversed tangent */
            float pr[5][4];
            if (!ss_patch(&scene, wmbb, x, y, rz, ht, hl, hw, pr)) { rj_patch++; continue; }
            px = x; py = y; pz = rz; phd = ht; found = 1;
        }
        if (found) {
            spawn[0] = px; spawn[1] = py; spawn[2] = pz; heading0 = phd;
            printf("showcase pose: (%.3f, %.3f, %.3f) heading %+.4f  "
                   "[nearest road candidate passing road/patch/wall/headroom; "
                   "was (%.3f, %.3f, %.3f), %.1f m away]\n",
                   px, py, pz, phd, oldsp[0], oldsp[1], oldsp[2],
                   sqrtf((px-oldsp[0])*(px-oldsp[0]) + (py-oldsp[1])*(py-oldsp[1])));
            printf("  probed %d: rejected road %d, wall %d, headroom %d, patch/tangent %d\n",
                   tried, rj_road, rj_wall, rj_low, rj_patch);
        } else
            printf("showcase pose: no valid road candidate; keeping the shipped pose "
                   "(%.3f, %.3f, %.3f)\n", oldsp[0], oldsp[1], oldsp[2]);
    }

    /* --showcase-audit (Milestone 106): is the shipped menu/showcase pose a safe,
       coherent place to park the car, and what is the nearest road that is?
       Read-only measurement using the SAME triangle tests --shot-static uses; it
       does not run the M80 selector, does not touch the Enter branch or the route
       start, and does not alter --shot-static's own spawn. */
    if (shaudit) {
        const float (*smbb)[4] = (const float (*)[4])world.mbb;
        float hl = (carbb[3]-carbb[0]) * 0.5f, hw = (carbb[4]-carbb[1]) * 0.5f;
        if (hl < 0.5f) hl = 2.20f;
        if (hw < 0.5f) hw = 0.90f;
        printf("\nMILESTONE: 106  showcase spawn audit  track=%s\n", trackname);
        printf("car footprint half_l %.3f half_w %.3f  SS_PATCH_DZ %.2f  SS_CLEAR_M %.2f\n",
               hl, hw, SS_PATCH_DZ, SS_CLEAR_M);
        printf("shipped showcase pose (%.3f, %.3f, %.3f)  heading %+.4f\n",
               spawn[0], spawn[1], spawn[2], heading0);
        printf("dense build-up centre  (%.3f, %.3f)\n", densx, densy);

        static SSHit hit[8192];
        int nh = ss_stack(&scene, smbb, spawn[0], spawn[1], hit, 8192);
        printf("\ncovering ROAD/TERRAIN triangles at the showcase XY: %d\n", nh);
        for (int a = 0; a < nh; a++)
            printf("   %-8s z %9.3f  %-28s n(%.2f %.2f %.2f)\n",
                   bc_cat(hit[a].cat), hit[a].z,
                   scene.meshes[hit[a].mesh].sname[0] ?
                   scene.meshes[hit[a].mesh].sname : "(unnamed)",
                   hit[a].n[0], hit[a].n[1], hit[a].n[2]);
        float srz = 0;
        int hasroad = ss_road_z(&scene, smbb, spawn[0], spawn[1], spawn[2], &srz);
        int inwall = ss_in_wall(obst, nobst, spawn[0], spawn[1], 1.3f);
        float sceil = ss_ceiling_above(&scene, smbb, spawn[0], spawn[1],
                                       hasroad ? srz : spawn[2]);
        float pr[5][4];
        int spatch = hasroad && ss_patch(&scene, smbb, spawn[0], spawn[1], srz,
                                         heading0, hl, hw, pr);
        printf("   road support          %s%s\n", hasroad ? "yes, z " : "NO", "");
        if (hasroad) printf("     nearest road z      %.3f (%+.3f vs pose z)\n", srz, srz - spawn[2]);
        printf("   car-footprint patch   %s\n", spatch ? "ok" : "FAIL");
        printf("   collide_walls foot    %s\n", inwall ? "INSIDE a building rect" : "clear");
        printf("   overhead clearance    %.3f m %s\n",
               sceil > 1e29f ? 99999.0 : sceil - (hasroad ? srz : spawn[2]),
               (sceil - (hasroad ? srz : spawn[2])) >= SS_CLEAR_M ? "" : "(BELOW SS_CLEAR_M)");
        { int nb = 0; float bd = 1e30f; const char *bn = "-";
          for (int o = 0; o < nobst; o++) {
              float cxr = 0.5f*(obst[o][0]+obst[o][2]), cyr = 0.5f*(obst[o][1]+obst[o][3]);
              float dx = cxr-spawn[0], dy = cyr-spawn[1], d2 = dx*dx+dy*dy;
              if (spawn[0] > obst[o][0] && spawn[0] < obst[o][2] &&
                  spawn[1] > obst[o][1] && spawn[1] < obst[o][3]) nb++;
              if (d2 < bd) { bd = d2; bn = "(rect)"; }
          }
          printf("   building rects containing this XY: %d   nearest rect centre %.2f m away %s\n",
                 nb, sqrtf(bd), bn); }

        /* nearest valid road candidates, same tests, ranked by displacement */
        #define SH_MAXC 65536
        static float shc[SH_MAXC][4]; long trv = 0;
        for (int i = 0; i < nm; i++)
            if (scene.meshes[i].cat == N2_ROAD) trv += scene.meshes[i].nverts;
        int stp = (int)(trv / SH_MAXC) + 1, nc = 0;
        for (int i = 0; i < nm && nc < SH_MAXC; i++) {
            if (scene.meshes[i].cat != N2_ROAD) continue;
            for (int v = 0; v < scene.meshes[i].nverts && nc < SH_MAXC; v += stp) {
                float *p = scene.meshes[i].verts + v*5;
                shc[nc][0]=p[0]; shc[nc][1]=p[1]; shc[nc][2]=p[2];
                shc[nc][3]=(p[0]-spawn[0])*(p[0]-spawn[0]) + (p[1]-spawn[1])*(p[1]-spawn[1]);
                nc++;
            }
        }
        static unsigned char shu[SH_MAXC]; memset(shu, 0, (size_t)nc);
        int rj_road=0, rj_wall=0, rj_low=0, rj_patch=0, tried=0, got=0;
        printf("\nnearest valid road candidates (%d road vertices sampled, stride %d):\n", nc, stp);
        printf("  %-4s %10s %10s %9s %9s %9s %s\n", "rank", "X", "Y", "Z", "heading",
               "dist m", "clearance");
        for (int pass = 0; pass < nc && got < 3; pass++) {
            int a = -1; float bd2 = 1e30f;
            for (int q = 0; q < nc; q++) if (!shu[q] && shc[q][3] < bd2) { bd2 = shc[q][3]; a = q; }
            if (a < 0) break;
            shu[a] = 1; tried++;
            float x = shc[a][0], y = shc[a][1], vz = shc[a][2], rz;
            if (!ss_road_z(&scene, smbb, x, y, vz, &rz)) { rj_road++; continue; }
            if (ss_in_wall(obst, nobst, x, y, 1.3f))     { rj_wall++; continue; }
            float ceil = ss_ceiling_above(&scene, smbb, x, y, rz);
            if (ceil - rz < SS_CLEAR_M)                  { rj_low++;  continue; }
            float hd = atan2f(densy - y, densx - x);
            float p2[5][4];
            if (!ss_patch(&scene, smbb, x, y, rz, hd, hl, hw, p2)) { rj_patch++; continue; }
            /* report DISTINCT places: road meshes share vertices, so the same
               XY can be sampled many times over. Purely a reporting rule. */
            static float shown[3][2]; int dup = 0;
            for (int q = 0; q < got; q++) {
                float dx2 = shown[q][0]-x, dy2 = shown[q][1]-y;
                if (dx2*dx2 + dy2*dy2 < 25.0f) dup = 1;
            }
            if (dup) continue;
            shown[got][0] = x; shown[got][1] = y;
            got++;
            printf("  %-4d %10.3f %10.3f %9.3f %9.4f %9.2f %9.3f\n", got, x, y, rz, hd,
                   sqrtf(shc[a][3]), ceil > 1e29f ? 99999.0 : ceil - rz);
        }
        printf("  probed %d candidates: rejected road %d, wall %d, headroom %d, patch %d\n",
               tried, rj_road, rj_wall, rj_low, rj_patch);
        if (shovr) {
            /* M107: resolve Z from the candidate's OWN supporting road layer --
               nearest road vertex sets the reference, never world_ground_z(...,0)
               which resolves to whatever deck sits closest to zero. */
            float vz = 0; float bd3 = 1e30f;
            for (int i = 0; i < nm; i++) {
                if (scene.meshes[i].cat != N2_ROAD) continue;
                for (int v = 0; v < scene.meshes[i].nverts; v++) {
                    float *p = scene.meshes[i].verts + v*5;
                    float dx2 = p[0]-shx, dy2 = p[1]-shy, d2 = dx2*dx2+dy2*dy2;
                    if (d2 < bd3) { bd3 = d2; vz = p[2]; }
                }
            }
            float oz = 0;
            int okz = ss_road_z(&scene, smbb, shx, shy, vz, &oz);
            printf("\nM107 candidate Z: nearest road vertex z %.3f (%.2f m away) -> "
                   "ss_road_z = %.3f  [%s]\n", vz, sqrtf(bd3), oz,
                   okz ? "supported" : "NO ROAD");
            printf("   for comparison, world_ground_z(x,y,0) = %.3f  (the M106 mistake)\n",
                   world_ground_z(&scene, shx, shy, 0.0f));
            /* local road tangent from the supporting triangle's longest XY edge */
            int nh2 = ss_stack(&scene, smbb, shx, shy, hit, 8192);
            float tx = 1, ty = 0; int gotT = 0;
            for (int a = 0; a < nh2 && !gotT; a++) {
                if (hit[a].cat != N2_ROAD) continue;
                float d3 = hit[a].z - oz; if (d3 < 0) d3 = -d3;
                if (d3 > 0.05f) continue;              /* the layer we stand on */
                const N2Mesh *me = &scene.meshes[hit[a].mesh];
                int t = hit[a].tri * 3;
                const float *A = me->verts + me->idx[t]*5;
                const float *B = me->verts + me->idx[t+1]*5;
                const float *C = me->verts + me->idx[t+2]*5;
                const float *e[3][2] = { {A,B}, {B,C}, {C,A} };
                float best = -1;
                for (int k = 0; k < 3; k++) {
                    float dx2 = e[k][1][0]-e[k][0][0], dy2 = e[k][1][1]-e[k][0][1];
                    float L = dx2*dx2 + dy2*dy2;
                    if (L > best) { best = L; tx = dx2; ty = dy2; }
                }
                float L = sqrtf(tx*tx + ty*ty); if (L > 1e-6f) { tx /= L; ty /= L; }
                gotT = 1;
                printf("   supporting triangle: mesh %d %s tri %d  longest XY edge "
                       "(%.4f, %.4f)  length %.2f m\n", hit[a].mesh,
                       me->sname[0] ? me->sname : "(unnamed)", hit[a].tri, tx, ty, sqrtf(best));
            }
            float hdens = atan2f(densy - shy, densx - shx);
            float htan  = atan2f(ty, tx);
            printf("   heading A dense-centre  %+.4f rad (%+.1f deg)\n", hdens, hdens*57.29578f);
            printf("   heading B road tangent  %+.4f rad (%+.1f deg)\n", htan,  htan*57.29578f);
            printf("   heading C tangent + pi  %+.4f rad (%+.1f deg)\n",
                   htan > 0 ? htan-3.14159265f : htan+3.14159265f,
                   (htan > 0 ? htan-3.14159265f : htan+3.14159265f)*57.29578f);
            printf("\nTEMPORARY capture override: showcase pose -> (%.3f, %.3f, %.3f)\n",
                   shx, shy, oz);
            spawn[0] = shx; spawn[1] = shy; spawn[2] = oz;
            heading0 = hdens;
            g_m107_h[0] = hdens; g_m107_h[1] = htan;
            g_m107_h[2] = htan > 0 ? htan-3.14159265f : htan+3.14159265f;
            g_m107 = 1;
        }
        printf("\n");
    }



    /* --startline-audit (Milestone 91): why does the route line start below its
       own ground layer, and is there a safe start on this route at all?
       Every waypoint, oriented by its LOCAL PATH TANGENT, is opened up into all
       of its exact covering ROAD/TERRAIN triangles -- no current-spawn Z is used
       to pick a layer. Each distinct ROAD level is then put through the three
       existing production-side tests, unchanged and unrelaxed: ss_patch (car
       footprint within SS_PATCH_DZ), ss_in_wall (collide_walls footprint) and
       ss_ceiling_above (SS_CLEAR_M headroom). Diagnostic only. */
    if (slaudit && aipath.n > 0) {
        float half_l = (carbb[3]-carbb[0]) * 0.5f, half_w = (carbb[4]-carbb[1]) * 0.5f;
        if (half_l < 0.5f) half_l = 2.20f;
        if (half_w < 0.5f) half_w = 0.90f;
        printf("MILESTONE: 91\n");
        printf("track=%s circuit=%s waypoints=%d start_idx=%d\n",
               trackname, circlist[selcirc], aipath.n, start_idx);
        printf("car footprint half_l=%.3f half_w=%.3f  SS_PATCH_DZ=%.2f "
               "SS_CLEAR_M=%.2f  wall radius 1.3 (same as collide_walls)\n",
               half_l, half_w, SS_PATCH_DZ, SS_CLEAR_M);
        printf("layers are distinct covering triangles grouped only where their Z "
               "coincide within 0.05 m (coincident-triangle de-dup, not a policy)\n\n");

        /* cumulative path length, for path-distance (not straight-line) reporting */
        double *cum = (double *)malloc((size_t)aipath.n * sizeof *cum);
        double total = 0; cum[0] = 0;
        for (int i = 1; i < aipath.n; i++) {
            float dx = aipath.xy[i*2]-aipath.xy[(i-1)*2];
            float dy = aipath.xy[i*2+1]-aipath.xy[(i-1)*2+1];
            total += sqrt((double)(dx*dx+dy*dy)); cum[i] = total;
        }
        { float dx = aipath.xy[0]-aipath.xy[(aipath.n-1)*2];
          float dy = aipath.xy[1]-aipath.xy[(aipath.n-1)*2+1];
          total += sqrt((double)(dx*dx+dy*dy)); }

        int nroadlayer = 0, neligible = 0;
        int *safe = (int *)malloc((size_t)aipath.n * sizeof *safe);
        float *safez = (float *)malloc((size_t)aipath.n * sizeof *safez);
        int nsafe = 0;
        static SSHit hit[8192];

        for (int i = 0; i < aipath.n; i++) {
            float x = aipath.xy[i*2], y = aipath.xy[i*2+1];
            int ip = (i - 1 + aipath.n) % aipath.n, in = (i + 1) % aipath.n;
            float head = atan2f(aipath.xy[in*2+1]-aipath.xy[ip*2+1],
                                aipath.xy[in*2]  -aipath.xy[ip*2]);
            int n = ss_stack(&scene, (const float (*)[4])world.mbb, x, y, hit, 8192);
            int wall = ss_in_wall(obst, nobst, x, y, 1.3f);
            int printed = 0, isafe = 0; float bestz = 0;
            for (int a = 0; a < n; a++) {
                if (a && hit[a].z - hit[a-1].z < 0.05f) continue;   /* coincident */
                if (hit[a].cat != N2_ROAD) continue;
                nroadlayer++;
                float rz = hit[a].z;
                float pr[5][4];
                int patch = ss_patch(&scene, (const float (*)[4])world.mbb, x, y, rz,
                                     head, half_l, half_w, pr);
                float ceil = ss_ceiling_above(&scene, (const float (*)[4])world.mbb, x, y, rz);
                float clear = ceil - rz;
                int ok = patch && !wall && clear >= SS_CLEAR_M;
                if (ok) { neligible++; if (!isafe) { isafe = 1; bestz = rz; } }
                if (i == start_idx || ok || !printed) {
                    printf("wp %-4d (%9.3f %9.3f) tan %+7.4f  ROAD z %9.3f  "
                           "patch %s wall %s clear %8.3f  => %s\n",
                           i, x, y, head, rz, patch ? "ok " : "FAIL",
                           wall ? "IN " : "out", clear > 1e29f ? 99999.0 : clear,
                           ok ? "ELIGIBLE" : "rejected");
                    printed = 1;
                }
            }
            if (!printed)
                printf("wp %-4d (%9.3f %9.3f) tan %+7.4f  no covering ROAD "
                       "(%d ROAD/TERRAIN tris total, wall %s)\n",
                       i, x, y, head, n, wall ? "IN" : "out");
            if (isafe) { safe[nsafe] = i; safez[nsafe] = bestz; nsafe++; }
            if (i == start_idx) {
                printf("  --- start_idx %d full layer stack (%d covering tris) ---\n", i, n);
                for (int a = 0; a < n; a++)
                    printf("      %-8s z %9.3f  mesh %-28s n(%.2f %.2f %.2f)\n",
                           hit[a].cat == N2_ROAD ? "ROAD" : "TERRAIN", hit[a].z,
                           scene.meshes[hit[a].mesh].sname[0] ?
                           scene.meshes[hit[a].mesh].sname : "(unnamed)",
                           hit[a].n[0], hit[a].n[1], hit[a].n[2]);
                printf("      in collide_walls footprint (r=1.3): %s\n", wall ? "YES" : "no");
                for (int a = 0; a < n; a++) {
                    if (hit[a].cat != N2_ROAD) continue;
                    printf("      world_ground_z(ref=%9.3f) = %9.3f\n",
                           hit[a].z, world_ground_z(&scene, x, y, hit[a].z));
                }
                printf("      world_ground_z(ref=%9.3f showcase spawn Z) = %9.3f\n",
                       spawn[2], world_ground_z(&scene, x, y, spawn[2]));
            }
        }

        printf("\neligible route-road layers: %d / %d  (waypoints with >=1 safe "
               "layer: %d / %d)\n", neligible, nroadlayer, nsafe, aipath.n);
        printf("route closed length %.2f m over %d waypoints\n", total, aipath.n);
        if (!nsafe) printf("nearest safe candidate: NONE on this route\n");
        else {
            printf("nearest safe candidates from start_idx %d, by PATH distance:\n", start_idx);
            for (int k = 0; k < 5 && k < nsafe; k++) {
                int bi = -1; double bd = 1e30;
                for (int q = 0; q < nsafe; q++) {
                    if (safe[q] < 0) continue;
                    double f = cum[safe[q]] - cum[start_idx];
                    if (f < 0) f += total;
                    double b = total - f;
                    double dmin = f < b ? f : b;
                    if (dmin < bd) { bd = dmin; bi = q; }
                }
                if (bi < 0) break;
                int wp = safe[bi]; safe[bi] = -1;
                double f = cum[wp] - cum[start_idx]; if (f < 0) f += total;
                printf("  wp %-4d (%9.3f %9.3f) z %9.3f  path-distance %8.2f m (%s)\n",
                       wp, aipath.xy[wp*2], aipath.xy[wp*2+1], safez[bi], bd,
                       f <= total - f ? "forward" : "backward");
            }
        }
        free(cum); free(safe); free(safez);
        return 0;
    }

    GLuint texTerr = world.have_grass ? upload_tex(&world.grass) : 0;
    GLuint texWheel = make_wheel_tex();   /* radial alloy-rim look for the tyres */
    /* Above ~40 km/h the 5-spoke pattern is turning faster than the frame rate
       can sample, so crisp spokes alias into a strobing mess. Swap to the
       angular-averaged rim then — a stand-in for the pre-baked blur asset
       retail used, which is NOT present in this data: no _BLUR key exists in
       any per-car TEXTURES.BIN. */
    GLuint texWheelBlur = make_wheel_blur_tex();
    #define WHEEL_BLUR_KMH 40.0f

    /* Aftermarket rim library. NOT CARS/WHEELS/GEOMETRY.BIN -- that file is a
       64-byte stub; the real geometry is per brand (GEOMETRY_BBS.BIN,
       GEOMETRY_ENKEI.BIN, ...) in the same chunk format as a car, so the
       ordinary car walker reads it. Each brand file holds its styles under
       STYLEnn tokens, which is the SAME token hoods use inside a car, so the
       style selector here is wheel_style_id rather than hood_style. */
    /* NFSU first: GEOMETRY_NFSU.BIN is the game's STOCK rim set (the wheels a
       car ships with), so it is the authentic default; the rest are the
       aftermarket brands, still selectable in the debug panel. */
    static const char wheel_brands[][24] = {
        "NFSU",
        "BBS","ENKEI","VOLK","OZ","MOMO","ADVAN","AVUS","KONIG",
        "LOWENHART","RACINGHART","ROTA","WORK","5ZIGEN","LEXANI"
    };
    const int n_wheel_brands = (int)(sizeof wheel_brands / sizeof wheel_brands[0]);
    (void)n_wheel_brands;   /* only read by the ImGui panel (debug builds) */
    N2Scene wheellib; memset(&wheellib, 0, sizeof wheellib);
    GpuMesh *wheelgm = NULL; int nwheelgm = 0, wheel_style = 1;
    long wllen = 0; unsigned char *wldata = NULL;
    int wheel_brand = 0;
    {   char wlp[1024];
        snprintf(wlp, sizeof wlp, "%s/CARS/WHEELS/GEOMETRY_%s.BIN",
                 dataroot, wheel_brands[wheel_brand]);
        wldata = n2_read_file(wlp, &wllen);
    }
    /* WHEELS/TEXTURES.BIN holds 274 texture slots (measured). This array used
       to be 64 entries, which silently truncated the table at slot 63: only a
       rim whose diffuse key happened to land in that first 64 ever resolved
       (LEXANI did, by luck), and every other brand fell back to flat colour.
       Sized well clear of 274 so a larger table still fits; n2_car_tex_keys
       stops at maxk, so an undersized array fails quietly rather than loudly. */
    uint32_t wkeys[512]; int nwkeys = 0;
    long wtlen = 0; unsigned char *wtdata = NULL;   /* kept resident: the rim
                       diffuse is decoded out of it on every style/brand swap */
    {   char wtp[1024];
        snprintf(wtp, sizeof wtp, "%s/CARS/WHEELS/TEXTURES.BIN", dataroot);
        wtdata = n2_read_file(wtp, &wtlen);
        if (wtdata) nwkeys = n2_car_tex_keys(wtdata, wtlen, wkeys,
                                             (int)(sizeof wkeys / sizeof wkeys[0]));
    }
    GLuint rimtex = 0;    /* real rim diffuse; 0 => fall back to procedural */
    if (load_rim_style(wldata, wllen, wkeys, nwkeys, wheel_style,
                       &wheellib, &wheelgm, &nwheelgm,
                       wtdata, wtlen, &rimtex, carWheelR))
        printf("rims: %s style %d, %d mesh(es)\n", wheel_brands[wheel_brand], wheel_style, nwheelgm);
    else
        printf("rims: library unavailable, using procedural wheels\n");


    /* static world -> per-(cell,texture) interleaved batches; the CPU-side
       scene stays alive for the physics/ground queries */
    /* Phase 21: skybox + neon/glow are pulled into their own batch lists so
       they can get their own draw pass (camera-locked/depth-off for the
       sky, additive-blended at frame end for neon) instead of blending into
       the ordinary opaque city batches. Print a note if a region genuinely
       has no SKYDOME mesh — the shader just falls back to the flat fog
       clear colour, which is correct but worth knowing about. */
    N2Batch *skybatch = NULL; int nsky = upload_cat_batches(&scene, N2_SKY, mtex, &skybatch);
    N2Batch *glowbatch = NULL; int nglow = upload_cat_batches(&scene, N2_GLOW, mtex, &glowbatch);
    /* SKY TEXTURE. The dome mesh names a key that the regional packs do not
       carry -- the skies live in the shared dynamic-texture file, one dome and
       one cap per time of day. Without this the dome draws untextured and the
       top of the frame is flat black. Default is the evening sky the game
       ships free roam in; --sky picks another. */
    {   const char *want = g_sky_name ? g_sky_name : "sunset";
        struct { const char *nm; uint32_t dome, cap; } sk[] = {
            { "sunrise", 0x2414a01eu, 0x5fb8bcd1u },
            { "sunset",  0x27f186b7u, 0x3e6947eau },
            { "night",   0x8a9a05cfu, 0xb0eb9302u },
        };
        for (int a = 0; a < 3 && nsky; a++) {
            if (strcmp(want, sk[a].nm)) continue;
            char dp2[1024]; snprintf(dp2, sizeof dp2, "%s/TRACKS/LOC4DYNTEX.BIN", dataroot);
            long dl2 = 0; unsigned char *dd2 = n2_read_file(dp2, &dl2);
            if (!dd2) { printf("sky: LOC4DYNTEX.BIN did not open\n"); break; }
            N2Tpk dt = n2_tpk_open(dd2, dl2);
            for (int q = 0; q < nsky; q++) {
                uint32_t key = (q == 0) ? sk[a].dome : sk[a].cap;
                N2Tex t; memset(&t, 0, sizeof t);
                if (n2_tpk_decode(dd2, dl2, dt, key, &t)
                    || n2_load_car_tex_by_key(dd2, dl2, key, &t)) {
                    skybatch[q].tex = upload_tex(&t);
                    printf("sky %s: batch %d -> %#x (%dx%d)\n",
                           sk[a].nm, q, key, t.w, t.h);
                    free(t.rgb); free(t.alpha); free(t.dxt);
                } else printf("sky %s: key %#x failed to decode\n", sk[a].nm, key);
            }
            free(dt.blk); free(dd2);
            break;
        }
    }
    printf("sky: %d batch(es)%s, neon/glow: %d batch(es)\n", nsky,
           nsky ? "" : " (no SKYDOME mesh found in this region set)", nglow);

    /* M132: authored backdrop impostors, batched into their own list. They come
       from world.vista, which no ground / collision / nav / spawn query can see,
       so this is purely a rendering tier. */
    N2Batch *vbatch = NULL; int nvista = 0; long vista_tris = 0; int *vmesh = NULL;
    float vista_far = 2000.0f;
    /* Ordinary and baseline never batch or draw vista content. The shared
       world_bind_textures pass may already have resolved a vista key into
       tmap; this gate prevents vista-specific upload and per-frame work. The
       separate scene keeps exclusion from ground/collision/nav/spawn provable. */
    if (tier == 2 && world.vista.count) {
        printf("vista: EXPERIMENTAL tier requested (--tier full); known "
               "opaque-sheet artifacts remain\n");
        GLuint *vtex = (GLuint *)calloc((size_t)world.vista.count, sizeof *vtex);
        for (int i = 0; i < world.vista.count; i++)
            for (int j = 0; j < ntmap; j++)
                if (tmapkey[j] == world.vista.meshes[i].texkey) { vtex[i] = tmaptex[j]; break; }
        /* ONE batch per vista mesh, and the source mesh index kept alongside.
           The foreground decision below needs REAL geometry distance, and a
           merged batch has no per-mesh vertices left to measure. Vista scenes
           are small (188 and 29 meshes), so the extra draw calls are noise. */
        /* vmesh grows WITH vbatch: upload_cat_batches is free to emit more
           than one batch for a mesh (it splits on BATCH_MAXVERTS), so sizing
           this array by mesh count would under-allocate the moment a backdrop
           sheet exceeds the vertex limit. */
        for (int i = 0; i < world.vista.count; i++) {
            N2Scene one = { &world.vista.meshes[i], 1, 1 };
            GLuint t1 = vtex[i];
            N2Batch *part = NULL;
            int np = upload_cat_batches(&one, world.vista.meshes[i].cat, &t1, &part);
            if (np) {
                vbatch = (N2Batch *)realloc(vbatch, (size_t)(nvista+np) * sizeof *vbatch);
                vmesh  = (int *)realloc(vmesh,      (size_t)(nvista+np) * sizeof *vmesh);
                memcpy(vbatch + nvista, part, (size_t)np * sizeof *part);
                for (int q = 0; q < np; q++) vmesh[nvista+q] = i;
                nvista += np;
            }
            free(part);
        }
        /* deterministic proof that every batch has a valid source mesh */
        for (int k = 0; k < nvista; k++)
            assert(vmesh[k] >= 0 && vmesh[k] < world.vista.count);
        int textured = 0;
        for (int i = 0; i < world.vista.count; i++) {
            vista_tris += world.vista.meshes[i].nidx/3;
            if (vtex[i]) textured++;
        }
        /* the pass needs a frustum that actually reaches the backdrop */
        for (int k = 0; k < nvista; k++)
            for (int c = 0; c < 2; c++) {
                float a = fabsf(vbatch[k].bbox_min[c]), b2 = fabsf(vbatch[k].bbox_max[c]);
                if (a > vista_far) vista_far = a;
                if (b2 > vista_far) vista_far = b2;
            }
        vista_far *= 2.5f;      /* the camera can stand on the far side of them */
        printf("vista batched: %d meshes (%ld tris, %d textured) -> %d batches, "
               "far plane %.0f m\n", world.vista.count, vista_tris, textured,
               nvista, vista_far);
        free(vtex);
    }

    /* --mark-repaired: the vertex-repaired meshes batched on their own, drawn
       after the world in flat magenta. Diagnostic image only; never enabled in
       a production capture. */
    N2Batch *rbatch = NULL; int nrep = 0;
    if (markrepair) {
        static N2Mesh rm[4096]; int nrm = 0;
        for (int i = 0; i < nm && nrm < 4096; i++)
            if (scene.meshes[i].vrepair) rm[nrm++] = scene.meshes[i];
        static GLuint rtx[4096]; memset(rtx, 0, sizeof rtx);
        for (int c = 0; c <= N2_GLOW; c++) {
            N2Scene rs = { rm, nrm, 4096 };
            N2Batch *part = NULL;
            int np = upload_cat_batches(&rs, c, rtx, &part);
            if (np) {
                rbatch = (N2Batch *)realloc(rbatch, (size_t)(nrep+np) * sizeof *rbatch);
                memcpy(rbatch + nrep, part, (size_t)np * sizeof *part);
                nrep += np;
            }
            free(part);
        }
        printf("mark-repaired: %d repaired meshes -> %d batches\n", nrm, nrep);
        for (int i = 0, shown = 0; i < nm && shown < 61; i++) {
            if (!scene.meshes[i].vrepair) continue;
            const float *bb = world.mbb[i];
            float best = 1e30f;
            for (int j = 0; j < nm; j++) {
                if (scene.meshes[j].cat != N2_ROAD) continue;
                const float *rb = world.mbb[j];
                float dx = (bb[0]+bb[2])*0.5f, dy = (bb[1]+bb[3])*0.5f;
                float qx = dx < rb[0] ? rb[0]-dx : (dx > rb[2] ? dx-rb[2] : 0);
                float qy = dy < rb[1] ? rb[1]-dy : (dy > rb[3] ? dy-rb[3] : 0);
                float d = qx*qx+qy*qy; if (d < best) best = d;
            }
            printf("REPAIRMESH %-30s xy(%.1f %.1f) tris %d  nearest road %.1f m\n",
                   scene.meshes[i].sname, (bb[0]+bb[2])*0.5f, (bb[1]+bb[3])*0.5f,
                   scene.meshes[i].nidx/3, sqrtf(best));
            shown++;
        }
    }

    N2Batch *wbatch = NULL;
    static int *meshbatch = NULL;
    meshbatch = (int *)malloc((size_t)(nm ? nm : 1) * sizeof *meshbatch);
    int nbatch = upload_world_batches(&scene, (const float (*)[4])world.mbb,
                                      mtex, texTerr, &wbatch, bmesh, meshbatch);
    /* the local-scene audit reads per-mesh texture resolution every frame, so
       the table has to outlive batching when it is enabled */
    if (!lsaudit) { free(mtex); mtex = NULL; }
    printf("world batched: %d meshes -> %d batches\n", nm, nbatch);
    if (baudit) return 0;   /* --batch-audit: report printed above, nothing to draw */

    /* F3 debug pipeline: wrap the existing world VBOs/IBOs in WorldMeshBatch so
       render_world_map can draw them with the prelight/normal/wireframe shader.
       Shared GL handles (owned by wbatch) -> no extra VRAM, no separate upload. */
    GLuint dbgprog = world_debug_program_load("src/world_debug_120.vert",
                                              "src/world_debug_120.frag");
    WorldMeshBatch *wmbatch = (WorldMeshBatch *)calloc((size_t)(nbatch > 0 ? nbatch : 1),
                                                       sizeof *wmbatch);
    for (int k = 0; k < nbatch; k++) {
        wmbatch[k].vbo = wbatch[k].vbo; wmbatch[k].ibo = wbatch[k].ibo;
        wmbatch[k].index_count = wbatch[k].index_count; wmbatch[k].chunk_id = (uint32_t)k;
    }
    int g_debug_mode = rendermode;   /* F3 cycles: 0 default, 1 prelight, 2 normals, 3 wireframe */
    if (daylight) g_dbg.night_mode = 0;   /* --daylight: headless mode matrix needs light */
    if (!dbgprog) fprintf(stderr, "world_debug shaders failed to load; F3 disabled\n");

    /* unit-quad for the 2D HUD (drawn in NDC via uMVP) */
    GpuMesh quad = make_quad();
    /* Live reflection cube. 128 px a face is enough for a clear coat: it is
       reflected, blurred by the mip chain and never seen directly. */
    int g_envcube_ready = env_cube_init(128);
    if (g_envcube_ready) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, env_cube_tex());
        glActiveTexture(GL_TEXTURE0);
        glUseProgram(rp.prog);
        glUniform1i(rp.uEnvCube, 1);       /* the cube lives on unit 1 */
        printf("reflections: 128x128 cube map ready\n");
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);   /* coplanar re-draws (last write wins) pass cleanly */
    /* Stock showroom paint: metallic silver (the files carry no chosen
       colour; badges/vinyls overlay as decals). The debug pane's paint
       override still allows any colour live. */
    float paint[3] = { paint_rgb[0], paint_rgb[1], paint_rgb[2] };
    /* A named start point has the last word. Several rules ahead of this one
       move the car -- the density spawn, the static-capture picker, the
       showcase pose, an armed race grid -- and each is right for its own mode,
       so rather than guarding every one of them the explicit request is simply
       applied after them all. Ground Z is taken here because by now the scene,
       its instances and the ground grid are all final. */
    /* Free roam starts at the free-roam start. With no place asked for, no
       event and no capture mode running, the car would otherwise be put
       wherever the density picker landed -- which is somewhere different on
       every region and is not a place anyone drives from. N2_SPAWNS[0] is that
       start; if the loaded region does not cover it, nothing changes. */
    if (!spawn_set && !shot && !want_event_id && !sstatic && !daudit && !raudit) {
        float gz = world_ground_z(&scene, N2_SPAWNS[0].x, N2_SPAWNS[0].y,
                                  N2_GROUND_HIGHEST);
        if (gz > -2000.0f && gz < 4000.0f) {
            spawn_set = 1;
            spawn_x = N2_SPAWNS[0].x; spawn_y = N2_SPAWNS[0].y;
            if (!head_set) { head_set = 1; head_deg = -10.2f; }
            printf("free roam: starting at '%s'\n", N2_SPAWNS[0].name);
        }
    }

    if (spawn_set) {
        spawn[0] = spawn_x; spawn[1] = spawn_y;
        float gz = world_ground_z(&scene, spawn_x, spawn_y, N2_GROUND_HIGHEST);
        if (gz > -2000.0f && gz < 4000.0f) spawn[2] = gz;
        else fprintf(stderr, "--spawn: no ground at (%.1f, %.1f) -- is the "
                             "region loaded that covers it?\n", spawn_x, spawn_y);
        printf("--spawn: (%.1f, %.1f, %.3f)\n", spawn[0], spawn[1], spawn[2]);
        /* Re-arm the sprung ride: it holds the wheel heights from wherever the
           car was placed before, and moving the body without telling it leaves
           the wheels reaching for ground that is no longer under them. The car
           then falls through the world -- which looks like the lighting going
           out, because what you are seeing is the city from underneath. */
        g_ride_ready = 0;
    }
    if (head_set) {
        heading0 = head_deg * 0.0174533f;
        printf("--heading: %.1f deg\n", head_deg);
    }

    float carpos[3] = { spawn[0], spawn[1], spawn[2] };
    float car_up[3] = { 0, 0, 1 };   /* chassis up, lerped toward the ground normal */
    if (shot && !sstatic && !daudit && !spawn_set && aipath.n > 0) {
        /* --shot skips the menu (and its Enter-key start-line snap), so the
           showcase density-spawn would leave the car parked off-circuit in
           the void on proxy regions. Snap to the start line like a race. */
        carpos[0] = aipath.xy[start_idx*2]; carpos[1] = aipath.xy[start_idx*2+1];
        carpos[2] = world_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
        int nx = (start_idx+1) % aipath.n;
        heading0 = atan2f(aipath.xy[nx*2+1]-carpos[1], aipath.xy[nx*2]-carpos[0]);
    }
    /* an armed race wins: start on its own grid, facing through the start line */
    if (!sstatic && !spawn_set) race_place_on_grid(&world, &scene, carpos, &heading0);
    float heading = heading0, speed = 0.0f, vel[2] = {0,0};
    float steer_filtered = 0.0f;
    float cam[3] = { spawn[0], spawn[1], spawn[2]+5 };
    if (sstatic) {
        /* Documented static view: the ordinary chase pose, placed analytically
           at its converged position so the settle loop has nothing left to ease
           — eye = car - heading*chase_distance, raised by chase_height. */
        carpos[2] = world_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
        cam[0] = carpos[0] - cosf(heading0)*g_dbg.chase_distance;
        cam[1] = carpos[1] - sinf(heading0)*g_dbg.chase_distance;
        cam[2] = carpos[2] + g_dbg.chase_height;
    }
    const float car_body_drop = stock_body_drop(carname);
    if (car_body_drop > 0.0f)
        printf("stock suspension %-12s body drop %.3f m (wheels remain on contact)\n",
               carname, car_body_drop);
    float fc[3] = { spawn[0]-6, spawn[1], spawn[2]+3 };   /* freecam eye */
    float fyaw = 0.0f, fpitch = -0.25f;                   /* freecam look angles */
    int   mlook = 0;                                      /* right-drag mouse look active */
    int p_lap = 0, p_prev = 0;   /* player lap + previous loop-progress */
    /* race flow: 3 = pre-race menu, 0 = countdown, 1 = racing, 2 = finished */
    const int COUNTDOWN = 180, LAP_TARGET = 2;
    int race_state = shot ? 1 : 3, racetimer = 0, finish_place = 0;
    int gear = 1; float shift_t = 0.0f;   /* virtual gearbox (engine audio) */
    float menuspin = 0.0f;   /* orbit-camera angle on the menu screen */
    int running = 1, shotframe = 0;
    uint32_t t0 = SDL_GetTicks();   /* --shot prints avg ms/frame at exit */
    /* tyre skid marks: a ring buffer of oriented ground SEGMENTS (ax,ay,az,
       bx,by,bz, life) — life starts at 1 and decays so marks fade over time.
       bx,by,bz) forming continuous ribbons; plus drift smoke particles. */
    /* A minute of rubber. Two segments a frame at 60 Hz is 7200 for sixty
       seconds; at 28 bytes each that is 200 kB, which is nothing, and it
       means a doughnut leaves a full circle you can still see when you drive
       back past it rather than a few metres that vanish while you watch. */
    #define MAXSKID 7200
    static float skid[MAXSKID][7]; int skidn = 0, skidhead = 0;
    float prevL[3] = {0,0,0}, prevR[3] = {0,0,0}; int skidpen = 0;  /* pen-down state */
    #define MAXSMOKE 512
    static struct { float p[3], v[3], life, size; } smoke[MAXSMOKE]; int smoken = 0;
    /* Contact-forensics accumulators. Audit-only reads; no production state is
       fed back from these measurements. Index is WSURF_ROAD / TERRAIN. */
    long ca_frames[3]={0}, ca_patch_ok[3]={0}, ca_patch_bad[3]={0};
    long ca_missing[3]={0}, ca_mixed[3]={0};
    float ca_fmin[3]={1e30f,1e30f,1e30f}, ca_fmax[3]={-1e30f,-1e30f,-1e30f};
    float ca_rmin[3]={1e30f,1e30f,1e30f}, ca_rmax[3]={-1e30f,-1e30f,-1e30f};
    float ca_axlemax[3]={0,0,0};
    while (running) {
        /* M89 race audit: one synthetic RETURN at 1 s, delivered through SDL so
           the production race_state==3 Enter branch runs exactly as written. */
        static long ra_f = 0; static int ra_sent = 0, ra_start = -1;
        static double ra_dist = 0; static float ra_peak = 0, ra_px = 0, ra_py = 0;
        static int ra_walls = 0, ra_rails = 0, ra_clamp = 0, ra_stall = -1, ra_bad = 0;
        static double ra_clampsum = 0; static int ra_cd = -1; static int ra_done = 0;
        static float ra_maxresid = 0, ra_maxpre = 0;
        static int ra_prev_mask = -1;
        static WGroundHit ra_prev_ground; static int ra_have_ground = 0, ra_ground_spikes = 0;
        if (raudit) {
            if (ra_f == 0)
                printf("RA menu showcase pos(%.3f %.3f %.3f) hdg %+.4f  race_state %d\n",
                       carpos[0], carpos[1], carpos[2], heading, race_state);
            if (ra_f == 60 && !ra_sent) {
                SDL_Event ke; memset(&ke, 0, sizeof ke);
                ke.type = SDL_KEYDOWN; ke.key.state = SDL_PRESSED;
                ke.key.keysym.sym = SDLK_RETURN; ke.key.keysym.scancode = SDL_SCANCODE_RETURN;
                SDL_PushEvent(&ke); ra_sent = 1;
                printf("RA Enter pushed at frame 60\n");
            }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F1 &&
                !e.key.repeat) {
                g_devui = !g_devui;
                printf("developer overlay: %s\n", g_devui ? "on (F1)" : "off (F1)");
                continue;
            }
#ifdef DEBUG_UI
            if (g_devui) {
                dbgui_event(&e);
                if ((e.type==SDL_KEYDOWN||e.type==SDL_KEYUP) && dbgui_want_keyboard()) continue;
            }
#endif
            if (e.type == SDL_QUIT) running = 0;
            /* freecam mouse-look: hold right button to rotate (keeps the cursor
               free for the ImGui panel the rest of the time). */
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT && g_dbg.freecam) {
                SDL_SetRelativeMouseMode(SDL_TRUE); mlook = 1;
            }
            else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                SDL_SetRelativeMouseMode(SDL_FALSE); mlook = 0;
            }
            else if (e.type == SDL_MOUSEMOTION && mlook) {
                fyaw   -= e.motion.xrel * 0.005f;
                fpitch -= e.motion.yrel * 0.005f;
                if (fpitch> 1.5f) fpitch= 1.5f; if (fpitch<-1.5f) fpitch=-1.5f;
            }
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) running = 0;
                else if (k == SDLK_f && race_state != 3) {
                    /* In the pre-race menu F starts free-roam below. Once
                       driving, the same key remains the existing freecam
                       toggle. Keeping the menu out of this branch fixes the
                       old shadowing that made the free-roam action unreachable. */
                    g_dbg.freecam ^= 1;
                    if (g_dbg.freecam) {
                        fc[0]=cam[0]; fc[1]=cam[1]; fc[2]=cam[2];
                        fyaw = atan2f(carpos[1]-cam[1], carpos[0]-cam[0]);
                        fpitch = -0.2f;
                    }
                }
                else if (k == SDLK_F3) {   /* cycle world render debug view */
                    if (dbgprog) {
                        g_debug_mode = (g_debug_mode + 1) & 3;
                        static const char *dm[4] = { "default", "prelight",
                                                     "normals", "wireframe" };
                        printf("render mode %d: %s\n", g_debug_mode, dm[g_debug_mode]);
                    }
                }
                else if (k == SDLK_w && wldata) {
                    wheel_style = wheel_style % 8 + 1;   /* 1..8 */
                    if (load_rim_style(wldata, wllen, wkeys, nwkeys, wheel_style,
                                       &wheellib, &wheelgm, &nwheelgm,
                                       wtdata, wtlen, &rimtex, carWheelR))
                        printf("rims -> BBS style %d (%d mesh(es))\n", wheel_style, nwheelgm);
                }
                else if (k == SDLK_k && cdata) {
                    /* cycle body kit 0 -> 1 -> 2 -> 0 and re-stream the car in
                       place. Unlike car/track (a whole new world + audio load,
                       hence relaunch()), this only touches the car's own
                       buffers, so it is cheap and safe to do live. */
                    carcfg.body_kit = (carcfg.body_kit + 1) % 3;
                    for (int i = 0; i < ncar; i++) {
                        glDeleteBuffers(1, &cgm[i].vbo);
                        glDeleteBuffers(1, &cgm[i].nbo);
                        glDeleteBuffers(1, &cgm[i].ibo);
                    }
                    free(cgm); cgm = NULL;
                    n2_free_scene(&car);
                    ncar = n2_load_car(cdata, clen, &car, ckeys, nck, &carcfg);
                    stock_wheel = -1;   /* re-orient + re-cull wheels, re-find stock index */
                    for (int i=0;i<ncar;i++) if (car.meshes[i].cat==N2_CAR_TIRE) {
                        N2Mesh *m=&car.meshes[i];
                        float ty0=1e30f,ty1=-1e30f;
                        for(int v=0;v<m->nverts;v++){ float y=m->verts[v*5+1]; if(y<ty0)ty0=y; if(y>ty1)ty1=y; }
                        float ymid=0.5f*(ty0+ty1);
                        for(int v=0;v<m->nverts;v++){ m->verts[v*5]=-m->verts[v*5]; m->verts[v*5+1]=ymid-m->verts[v*5+1]; }
                        rim_drop_welded_mesh(m);
                        if (stock_wheel<0 || m->nverts>car.meshes[stock_wheel].nverts) stock_wheel=i;
                    }
                    cgm = upload_scene(&car);
                    printf("body kit -> KIT%02d (%d meshes)\n", carcfg.body_kit, ncar);
                }
                else if (race_state == 3) {   /* pre-race menu navigation */
                    /* car and track each mean a whole new load, so they re-launch
                       the process (see relaunch()); circuit is a cheap in-place
                       reload. */
                    if (k==SDLK_f) {
                        /* Free-roam is a gameplay mode, not a debug camera: keep
                           the supported showcase road pose, open the whole loaded
                           bundle's nav graph, and enable ordinary WASD physics. */
                        world_set_mode(&world, MODE_FREEROAM, -1);
                        world.race.active = 0;
                        vel[0]=vel[1]=0; speed=0; p_lap=p_prev=0;
                        race_state = 1; racetimer = 0;
                        printf("freeroam: whole %s bundle driveable from (%.3f %.3f %.3f)\n",
                               trackname, carpos[0], carpos[1], carpos[2]);
                    } else if ((k==SDLK_LEFT || k==SDLK_RIGHT) && ncars > 1) {
                        selcar = (selcar + (k==SDLK_RIGHT?1:ncars-1)) % ncars;
                        relaunch(selfexe, dataroot, carlist[selcar], trackname);
                    } else if ((k==SDLK_UP || k==SDLK_DOWN) && ntrack > 1) {
                        seltrack = (seltrack + (k==SDLK_DOWN?1:ntrack-1)) % ntrack;
                        relaunch(selfexe, dataroot, carname, tracklist[seltrack]);
                    } else if ((k==SDLK_LEFTBRACKET || k==SDLK_RIGHTBRACKET) &&
                               !ncirc && nsprint > 1) {
                        selsprint = (selsprint + (k==SDLK_RIGHTBRACKET?1:nsprint-1)) % nsprint;
                        printf("sprint selected: event %d (%d outline points)\n",
                               world.ev[sprintev[selsprint]].id,
                               world.ev[sprintev[selsprint]].npoly);
                    } else if ((k==SDLK_LEFTBRACKET || k==SDLK_RIGHTBRACKET) && ncirc > 1) {
                        selcirc = (selcirc + (k==SDLK_RIGHTBRACKET?1:ncirc-1)) % ncirc;
                        nai = load_circuit(dataroot, circlist[selcirc], &scene,
                                           &aipath, ais, spawn, &heading0, &start_idx, densx, densy);
                        carpos[0]=spawn[0]; carpos[1]=spawn[1]; carpos[2]=spawn[2];
        g_ride_ready = 0;   /* M130: re-arm the ride at every placement */
                        heading=heading0; vel[0]=vel[1]=0; speed=0; p_lap=p_prev=0;
                    } else if ((k==SDLK_RETURN || k==SDLK_SPACE) && spawn_set) {
                        /* Free roam: the start was asked for explicitly, so
                           this only drops the countdown and lets the car go.
                           Snapping to a start line here is what teleported the
                           player away from the place they had chosen. */
                        race_state = 0; racetimer = 0;
                    } else if (k==SDLK_RETURN || k==SDLK_SPACE) {
                        /* Snap the player from the showcase spot to the circuit
                           start line so the race itself is fair. start_idx is
                           only the waypoint nearest the showcase spot, and it is
                           not necessarily standable: on L4RA/Paths4175 it has no
                           covering ground at all and sits inside a building
                           footprint (M91). Take the first waypoint AHEAD of it
                           whose own road layer passes the footprint, wall and
                           headroom tests, and start the lap there. */
                        if (!aipath.n && nsprint > 0 && !spawn_set) {
                            /* No closed circuit here: arm the selected shipped
                               event through the same world_race_start() the
                               --event path uses, then place the player with the
                               same grid rule. Gates, corridor, barriers, HUD and
                               world_race_update all come from that call. */
                            int ev = sprintev[selsprint];
                            int ng = world_race_start(&world, troot, ev, want_laps);
                            if (ng > 0 && race_place_on_grid(&world, &scene, carpos, &heading)) {
                                printf("sprint armed: event %d, %d gates\n",
                                       world.ev[ev].id, ng);
                            } else
                                printf("sprint event %d could not be armed (%d gates) - "
                                       "starting from the showcase pose\n",
                                       world.ev[ev].id, ng);
                            vel[0]=vel[1]=0; speed=0;
                        }
                        else if (aipath.n > 0) {
                            float hl = (carbb[3]-carbb[0])*0.5f, hw = (carbb[4]-carbb[1])*0.5f;
                            if (hl < 0.5f) hl = 2.20f;
                            if (hw < 0.5f) hw = 0.90f;
                            float sz = 0, sh = 0;
                            int seed = start_idx + 1;
                            if (posefrac >= 0.0f)
                                seed = start_idx + 1 + (int)(posefrac * aipath.n);
                            int si = sl_first_safe(&scene, (const float (*)[4])world.mbb,
                                                   aipath.xy, aipath.n, seed,
                                                   obst, nobst, hl, hw, &sz, &sh);
                            if (si >= 0) {
                                start_idx = si;              /* lap logic uses the real start */
                                carpos[0]=aipath.xy[si*2]; carpos[1]=aipath.xy[si*2+1];
                                carpos[2]=sz; heading=sh;
                            g_ride_ready = 0;   /* M130: re-arm the ride at every placement */
                                printf("start line: waypoint %d (%.3f, %.3f, %.3f) "
                                       "heading %+.4f [first safe forward candidate]\n",
                                       si, carpos[0], carpos[1], carpos[2], heading);
                            } else {
                                printf("start line: no waypoint on this route passes the "
                                       "footprint/wall/headroom tests - using start_idx %d "
                                       "unchanged\n", start_idx);
                                carpos[0]=aipath.xy[start_idx*2]; carpos[1]=aipath.xy[start_idx*2+1];
                                carpos[2]=world_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
                                g_ride_ready = 0;
                                int nx=(start_idx+1)%aipath.n;
                                heading=atan2f(aipath.xy[nx*2+1]-carpos[1], aipath.xy[nx*2]-carpos[0]);
                            }
                            vel[0]=vel[1]=0; speed=0;
                        }
                        race_state = 0; racetimer = 0;   /* -> 3-2-1 countdown */
                    }
                }
            }
        }
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        if (g_dbg.freecam) {                 /* fly the camera; WASD move, arrows look */
            float sp = g_dbg.speed * (ks[SDL_SCANCODE_LSHIFT]?4.0f:1.0f);
            float cp=cosf(fpitch);
            float dir[3]={cp*cosf(fyaw), cp*sinf(fyaw), sinf(fpitch)};
            float rt[3]={sinf(fyaw), -cosf(fyaw), 0};   /* strafe axis */
            if (ks[SDL_SCANCODE_W]){ fc[0]+=dir[0]*sp; fc[1]+=dir[1]*sp; fc[2]+=dir[2]*sp; }
            if (ks[SDL_SCANCODE_S]){ fc[0]-=dir[0]*sp; fc[1]-=dir[1]*sp; fc[2]-=dir[2]*sp; }
            if (ks[SDL_SCANCODE_D]){ fc[0]+=rt[0]*sp;  fc[1]+=rt[1]*sp; }
            if (ks[SDL_SCANCODE_A]){ fc[0]-=rt[0]*sp;  fc[1]-=rt[1]*sp; }
            if (ks[SDL_SCANCODE_E]||ks[SDL_SCANCODE_SPACE]) fc[2]+=sp;
            if (ks[SDL_SCANCODE_Q]||ks[SDL_SCANCODE_LCTRL]) fc[2]-=sp;
            if (ks[SDL_SCANCODE_LEFT])  fyaw   += 0.03f;
            if (ks[SDL_SCANCODE_RIGHT]) fyaw   -= 0.03f;
            if (ks[SDL_SCANCODE_UP])    fpitch += 0.02f;
            if (ks[SDL_SCANCODE_DOWN])  fpitch -= 0.02f;
            if (fpitch> 1.5f) fpitch= 1.5f; if (fpitch<-1.5f) fpitch=-1.5f;
        }
        /* driving inputs -> Physics module (velocity-vector arcade kinematics:
           throttle along the heading, steering rotates it, tyres scrub the
           sideways component so hard cornering at speed slides/drifts). */
        float throttle = 0.0f;
        if (!g_dbg.freecam && race_state == 1) {
            if      (ks[SDL_SCANCODE_W] || (shot && !sstatic)) throttle =  1.0f;
            else if (ks[SDL_SCANCODE_S])         throttle = -1.0f;
        }
        float steer = g_dbg.freecam ? 0.f
                    : (ks[SDL_SCANCODE_A]?1.f:0.f) - (ks[SDL_SCANCODE_D]?1.f:0.f);
        int handbrake = (race_state==1 && ks[SDL_SCANCODE_SPACE]);
        /* Auto-drive (camera-spring test): steady throttle + smooth sine steer fed
           straight into the physics, so the car throws itself through an S-curve
           hands-free. Interactive only -- gated on !shot so it never fights the
           headless --shot A* autopilot below. */
        if (g_dbg.auto_drive && !shot) {
            static float adt = 0.0f; adt += 0.02f;      /* ~5 s period at 60 Hz */
            throttle = 1.0f;
            steer = sinf(adt) * 0.35f;                  /* gentle S -- hard lock scrubs
                                                           off all the speed and it creeps */
        }
        /* M88 drive audit: keyboard-equivalent scripted input. Observes only --
           it replaces the key state, nothing downstream is altered or tuned. */
        static long da_f = 0;
        static double da_dist = 0; static float da_peak = 0, da_prevx = 0, da_prevy = 0;
        static int da_walls = 0, da_rails = 0, da_clamp = 0, da_stall = -1, da_bad = 0;
        static double da_clampsum = 0;
        if (daudit) {
            if      (da_f < 120)  { throttle =  0.0f; steer = 0.0f; }
            else if (da_f < 720)  { throttle =  1.0f; steer = 0.0f; }
            else if (da_f < 1920) { throttle =  1.0f;
                                    steer = ((da_f - 720) / 120) % 2 ? -1.0f : 1.0f; }
            else if (da_f < 2070) { throttle = -1.0f; steer = 0.0f; }   /* brake */
            else                  { throttle =  0.0f; steer = 0.0f; }   /* coast */
            handbrake = 0;
        }
        if (poseshot && race_state == 1) {   /* frozen: the start pose, untouched */
            throttle = 0.0f; steer = 0.0f; handbrake = 1;
            vel[0] = 0.0f; vel[1] = 0.0f; speed = 0.0f;   /* capture mode: no drift */
        } else if (raudit && race_state == 1) {   /* keyboard-equivalent, race only */
            long r = ra_start < 0 ? 0 : ra_f - ra_start;
            if      (r < 600)  { throttle = 1.0f; steer = 0.0f; }
            else if (r < 1800) { throttle = 1.0f; steer = ((r-600)/120) % 2 ? -1.0f : 1.0f; }
            else if (r < 1950) { throttle = -1.0f; steer = 0.0f; }
            else               { throttle =  0.0f; steer = 0.0f; }
            handbrake = 0;
        }
        /* ONE steering filter, not two. The old model had no rack of its own,
           so this exponential smoother stood in for one. The new model has a
           real rate-limited rack (140 deg/s at the road wheels, quicker back
           to centre), and running both in series just makes the wheel vague:
           full lock arrived a third of a second late, which is a long time
           when you are trying to break the back loose. Pass the input
           straight through and let the rack shape it. */
        steer_filtered = (g_newphys && g_newphys_ok)
                       ? steer : phys_steer_response(steer_filtered, steer);
        /* Surface-aware handling (M114): query the selected centre contact layer,
           then EASE the active profile toward that surface's
           profile over ~0.15 s. Blending the profile (not the velocity) means
           crossing a road edge cannot snap speed, heading or camera; nothing
           here moves the car or invents a height. WSURF_NONE keeps the road
           profile, so an unsupported XY is never penalised. */
        static PhysSurface surf_now = { 1.00f, 1.00f, 0.99886f, 0.86f, 1.00f };
        static int surf_id = WSURF_ROAD, surf_prev = WSURF_ROAD;
        {
            float surface_z=carpos[2];
            int sid=world_ground_at(&scene,carpos[0],carpos[1],carpos[2],&surface_z);
            surf_prev = surf_id;
            if (sid != WSURF_NONE) surf_id = sid;
            const PhysSurface *tgt = (surf_id == WSURF_TERRAIN) ? &PHYS_SURF_TERRAIN
                                                                : &PHYS_SURF_ROAD;
            const float SURF_BLEND = 1.0f / 9.0f;   /* ~0.15 s at 60 Hz */
            float *d = (float *)&surf_now; const float *t = (const float *)tgt;
            for (int c = 0; c < 5; c++) d[c] += (t[c] - d[c]) * SURF_BLEND;
        }
        /* push the ImGui handling sliders into the physics tune (stock when untouched) */
        g_phys_tune.accel = g_dbg.tune_accel; g_phys_tune.brake = g_dbg.tune_brake;
        g_phys_tune.turn = g_dbg.tune_turn;   g_phys_tune.top_kmh = g_dbg.tune_top;
        /* Race autopilot (--shot only): walk the car along the corridor-masked
           A* route to the armed gate so a headless run drives the real course
           and the checkpoint logic sees real positions. Kinematic on purpose —
           the same role as the --circuit demo autopilot below, and the physics
           drive is separately pinned on this track (the default --circuit start
           snap lands the car inside geometry, so collide_walls holds it at
           0 km/h; pre-existing, not the race system). */
        static int rpath[8192]; static int rpn = 0, rp_gate = -1, rp_at = 0;
        int race_auto = shot && !sstatic && world.race.active && !world.race.finished;
        if (race_auto) {
            if (rp_gate != world.race.next) {
                int s = world_nav_nearest(&world, carpos[0], carpos[1]);
                int gnode = world.race.gate[world.race.next].node;
                rpn = world_route(&world, s, gnode, rpath, 8192, NULL);
                if (rpn <= 1 && world.mode == MODE_RACE_EVENT) {
                    /* the corridor mask can disconnect the spawn/previous node
                       from this gate; without a route the car would beeline
                       straight through the city. Re-route on the UNMASKED road
                       graph (fully connected) so it always stays on asphalt. */
                    int mode = world.mode; world.mode = MODE_FREEROAM;
                    rpn = world_route(&world, s, gnode, rpath, 8192, NULL);
                    world.mode = mode;
                }
                if (rtrace) {
                    const WGate *G = &world.race.gate[world.race.next];
                    float gz4 = 0;
                    int c4 = world_ground_at(&scene, G->x, G->y, carpos[2], &gz4);
                    printf("RT retarget: next=%d/%d  gate(%.3f %.3f) node %d  "
                           "car(%.3f %.3f) start-node %d  route %d nodes  "
                           "gate-support %s z %.3f  lap %d cleared %d\n",
                           world.race.next, world.race.ngate, G->x, G->y, gnode,
                           carpos[0], carpos[1], s, rpn,
                           c4 == WSURF_ROAD ? "ROAD" : c4 == WSURF_TERRAIN ? "TERRAIN"
                                                                           : "NONE", gz4,
                           world.race.lap, world.race.cleared);
                    if (rpn > 1)
                        printf("RT   route ends at node %d (%.3f %.3f), gate node "
                               "%d (%.3f %.3f)\n", rpath[rpn-1],
                               world.nav[rpath[rpn-1]*2], world.nav[rpath[rpn-1]*2+1],
                               gnode, world.nav[gnode*2], world.nav[gnode*2+1]);
                }
                rp_gate = world.race.next; rp_at = 0;
            }
            const WGate *ng = &world.race.gate[world.race.next];
            float gdx = ng->x - carpos[0], gdy = ng->y - carpos[1];
            float dx, dy;
            if (gdx*gdx + gdy*gdy < 9.0f*9.0f) {
                /* almost on the gate: drive straight through on the current
                   heading so the crossing always registers (no node stalling). */
                dx = cosf(heading); dy = sinf(heading);
            } else {
                float tx = ng->x, ty = ng->y;
                if (rpn > 1) {                     /* follow the A* route in */
                    /* hug each node (advance at 3 m) so the aim tracks the road
                       polyline rather than cutting toward the distant gate */
                    if (rp_at < rpn - 1) {
                        float ax = world.nav[rpath[rp_at]*2]   - carpos[0];
                        float ay = world.nav[rpath[rp_at]*2+1] - carpos[1];
                        if (ax*ax + ay*ay < 9.0f) rp_at++;
                    }
                    tx = world.nav[rpath[rp_at]*2]; ty = world.nav[rpath[rp_at]*2+1];
                }
                dx = tx - carpos[0]; dy = ty - carpos[1];
            }
            float d = sqrtf(dx*dx + dy*dy);
            if (d > 1e-3f) {
                float step = 1.2f;                 /* ~72 m/s at 60 Hz */
                carpos[0] += dx/d*step; carpos[1] += dy/d*step;
                heading = atan2f(dy, dx);
                speed = step * 60.0f;
            }
            /* clamp Z to the nearest road/terrain surface, eased so a gap in an
               elevated deck descends smoothly instead of teleporting */
            { float gz = world_ground_z(&scene, carpos[0], carpos[1], carpos[2]);
              float dz = gz - carpos[2];
              if (dz >  1.5f) dz =  1.5f; if (dz < -1.5f) dz = -1.5f;
              carpos[2] += dz; }
            world_race_update(&world, carpos[0], carpos[1]);
        }
        else if (shot && !sstatic && !daudit && aipath.n > 0) {   /* screenshot autopilot: follow the racing
               line (chasing an AI used to drift off small proxy regions into
               the empty void — black screenshots) */
            int nearest = 0; float bd = 1e30f;
            for (int i = 0; i < aipath.n; i++) {
                float dx = aipath.xy[i*2]-carpos[0], dy = aipath.xy[i*2+1]-carpos[1];
                float d2 = dx*dx + dy*dy;
                if (d2 < bd) { bd = d2; nearest = i; }
            }
            int tgt = (nearest + 3) % aipath.n;      /* aim a few waypoints ahead */
            float da = atan2f(aipath.xy[tgt*2+1]-carpos[1],
                              aipath.xy[tgt*2]-carpos[0]) - heading;
            while (da> 3.14159f) da-=6.28318f;
            while (da<-3.14159f) da+=6.28318f;
            if (da> 0.06f) da= 0.06f; if (da<-0.06f) da=-0.06f;
            heading += da;
        }
        float dmag = sstatic ? 0.0f
                   : race_auto ? speed/60.0f
                   : (g_newphys && g_newphys_ok)
                   ? veh_bridge_step(carpos, vel, &heading, &speed,
                                     throttle, steer_filtered, handbrake ? 1 : 0, &g_vehmodel)
                   : phys_car_step(carpos, vel, &heading, &speed,
                                   throttle, steer_filtered, handbrake ? 1 : 0, &surf_now, &g_vehicle);
        if (g_tel && !sstatic) {
            g_tel_t += 1.0f/60.0f;
            fprintf(g_tel, "%.3f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.3f,"
                           "%.2f,%.2f,%d,%d,%.0f,%.2f,%.2f\n",
                    g_tel_t, carpos[0], carpos[1], carpos[2],
                    speed * 60.0f * 3.6f, heading * 57.29578f,
                    (g_newphys && g_newphys_ok) ? veh_bridge_yaw_rate()*57.29578f : 0.0f,
                    (g_newphys && g_newphys_ok) ? veh_bridge_slip_deg() : 0.0f,
                    (g_newphys && g_newphys_ok) ? veh_bridge_slip_ratio() : 0.0f,
                    throttle, steer_filtered, handbrake ? 1 : 0,
                    (g_newphys && g_newphys_ok) ? veh_bridge_gear() : gear,
                    (g_newphys && g_newphys_ok) ? veh_bridge_rpm() : 0.0f,
                    (g_newphys && g_newphys_ok) ? veh_bridge_pitch()*57.29578f : 0.0f,
                    (g_newphys && g_newphys_ok) ? veh_bridge_roll()*57.29578f : 0.0f);
        }
        float nf[2] = { cosf(heading), sinf(heading) }, nr[2] = { nf[1], -nf[0] };
        /* engine note: 6-speed virtual gearbox drives RPM + load; shifts cut
           the throttle for 150ms and let the revs sag (idles during the
           countdown, since throttle is locked out until GO). */
        { float sp = (speed < 0 ? -speed : speed) / PHYS_MAXSPD;
          /* THE ENGINE NOTE HAS TO COME FROM THE ENGINE. eng_gearbox_step
             runs a virtual six-speed off road speed alone, which was fine
             when the physics had no engine at all -- but now there is a real
             one, with the car's own ratios and its own rev counter, and the
             two disagreed audibly: the model would hold a gear through a
             slide while the sound shifted up and down anyway. Worse, in a
             doughnut you could HEAR the revs climb and fall while the car
             was in fact sitting on one gear the whole time. Drive the note
             from the model and the ear matches the car. */
          if (g_newphys && g_newphys_ok) {
              g_engine.target_rpm = veh_bridge_rpm();
              g_engine.load = throttle > 0.0f ? throttle : 0.0f;
              gear = veh_bridge_gear() - 1;      /* model counts neutral as 1 */
              if (gear < 1) gear = 1;
          } else
          eng_gearbox_step(sp, throttle, 1.0f/60.0f, &gear, &shift_t);
          g_engine.master_volume = (race_state==0 ? 0.16f : 0.16f + sp*0.5f);
          g_road_vol = sp*sp*0.35f; }   /* tyre/wind roar rises with speed */
        float fwd[3] = { nf[0], nf[1], 0 };
        /* building collision: push the car out of any wall footprint it entered.*/
        /* Body collision envelope at the exact gameplay contact. The renderer
           uses the same contact and body ride; wheels use car_ride unchanged so
           a per-car body drop never moves the tyre off the road. */
        float car_ride = carprof.ride
                       * (g_dbg.wheel_scale > 0.05f ? g_dbg.wheel_scale : 1.0f);
        float car_body_ride = car_ride - car_body_drop;
        if (car_body_ride < 0.05f) car_body_ride = 0.05f;
        float car_z0 = carpos[2] + car_body_ride + carbb[2];
        float car_z1 = carpos[2] + car_body_ride + carbb[5];
        if (car_z1 - car_z0 < 0.5f) car_z1 = car_z0 + 1.5f;   /* no car body loaded */
        m94_prex = carpos[0]; m94_prey = carpos[1]; m94_prez = carpos[2];
        PhysWallContact wc[8]; int nwc = 0;
        float vpre[2] = { vel[0], vel[1] };
        if (raudit && race_state == 1 && !race_auto && !sstatic) {
            /* read-only: the same rects collide_walls is about to test, before it
               moves anything. Nothing here writes carpos or vel. */
            for (int o = 0; o < nobst; o++) {
                const float R = 1.3f;
                if (carpos[0] <= obst[o][0]-R || carpos[0] >= obst[o][2]+R ||
                    carpos[1] <= obst[o][1]-R || carpos[1] >= obst[o][3]+R) continue;
                float z0 = car_z0, z1 = car_z1;
                if (obstz[o][1] < z0 || obstz[o][0] > z1) continue;   /* same gate */
                /* mirror the narrow phase too, so the attribution counts real
                   responses and not broad-phase rect overlaps (M112) */
                if (!cw_probe_contact(&scene, obstsrc[o], carpos[0], carpos[1],
                                      R, z0, z1)) continue;
                m94_wall(o, obstsrc[o], &scene, carpos, &aipath, ra_f);
            }
        }

        if (race_state == 1 && !race_auto && !sstatic &&
            (nwc = collide_walls(carpos, vel, obst, obstz, nobst, 1.3f,
                                 car_z0, car_z1, &scene, obstsrc, wc, 8)) > 0) {
            g_hit = 0.5f; da_walls++; ra_walls++;
            veh_bridge_adopt();      /* the resolved velocity is the truth now */
            if (raudit) {
                float dx = carpos[0]-m94_prex, dy = carpos[1]-m94_prey;
                float corr = sqrtf(dx*dx+dy*dy);
                if (corr > ra_maxwallcorr) ra_maxwallcorr = corr;
                for (int q = 0; q < nwc && q < 8; q++)
                    printf("RA WALL f%-6ld mesh %5d tri %5d %-20s normal (%+.3f %+.3f) "
                           "pen %.4f dist %.4f span %6.2f  vel (%+.4f %+.4f) -> (%+.4f %+.4f)"
                           "  correction %.4f m\n", ra_f, wc[q].mesh, wc[q].tri,
                           wc[q].mesh >= 0 && wc[q].mesh < scene.count
                               ? scene.meshes[wc[q].mesh].sname : "?",
                           wc[q].nx, wc[q].ny, wc[q].pen, wc[q].dist, wc[q].span,
                           vpre[0], vpre[1], vel[0], vel[1], corr);
            } }
        /* guardrail/fence collision: push out of near-vertical road/terrain faces */
        { WRailHit rh; rh.mesh = -1;
          int rpushed = (race_state == 1 && !race_auto && !sstatic) &&
                        world_wall_push(&scene, carpos, 1.3f, raudit ? &rh : NULL);
          if (rpushed) veh_bridge_adopt();
          if (raudit && rpushed && rh.mesh >= 0)
              m94_rail(&rh, &scene, m94_prex, m94_prey, m94_prez, &aipath, ra_f);
          if (rpushed) {
            vel[0]*=0.3f; vel[1]*=0.3f; g_hit = 0.5f;   /* rebound: bleed speed */
            da_rails++; ra_rails++;
          }
        }
        /* race blockades: only solid while a race event is active (Phase 71) */
        /* KERB STOP. The wall probes look for tall near-vertical faces and
           deliberately skip short seams -- which is what a kerb face is, so
           the car drove straight into kerbs, half-sank, and sat there tilted.
           A kerb is not a wall, it is a STEP in the ground: measured on this
           map, the steepest drivable ramp gains 0.09 m per 0.25 m sample and
           the lowest kerb jumps 0.16, so a rise above 0.12 per quarter-metre
           step is a kerb face. Probe just past the bumper along the travel
           direction; hitting one removes the into-step velocity and nudges
           the car back, like a wheel striking the stone does. */
        if (race_state == 1 && !race_auto && !sstatic && fabsf(speed) > 0.02f) {
            /* A kerb is a JUMP inside one quarter-metre step; a ramp is a
               SLOPE spread over all of them. The first cut of this also
               compared the ground 2.2 m ahead against the ground under the
               car -- and over 2.2 m even a drivable ramp legitimately gains
               most of a metre, so the car nosed at every on-ramp convinced
               it was a wall. Walk a ladder of quarter-metre samples and
               judge only the per-step jumps; the total climb proves nothing. */
            float sgn = (speed >= 0.0f) ? 1.0f : -1.0f;
            float dirx = cosf(heading) * sgn, diry = sinf(heading) * sgn;
            float step = 0.0f, gprev = 0.0f;
            for (int q = 0; q < 4; q++) {
                float d = 1.6f + 0.25f * (float)q;
                float g = world_ground_z(&scene, carpos[0] + dirx * d,
                                         carpos[1] + diry * d, carpos[2]);
                if (q && g - gprev > step) step = g - gprev;
                gprev = g;
            }
            if (step > 0.12f && step < 1.4f) {
                float into = vel[0] * dirx + vel[1] * diry;
                if (into > 0.0f) {
                    vel[0] -= dirx * into * 0.9f;
                    vel[1] -= diry * into * 0.9f;
                    carpos[0] -= dirx * 0.06f; carpos[1] -= diry * 0.06f;
                    g_hit = 0.3f;
                    veh_bridge_adopt();
                }
            }
        }
        if (race_state == 1 && !race_auto && !sstatic && world_barrier_push(&world, carpos, 1.3f)) {
            vel[0]*=0.2f; vel[1]*=0.2f; g_hit = 0.5f;
        }
        /* checkpoint / lap tracking, after the pushes so it sees the final XY */
        if (race_state == 1 && !race_auto && !sstatic) world_race_update(&world, carpos[0], carpos[1]);
        /* Stable contact pose: select ONE triangle using the same reference-Z
           rule for height, surface class and normal. The previous four-wheel
           experiment could combine four different stacked layers, making the
           body float, sink or invent a ramp. Suspension remains a later,
           visual/per-wheel system; the gameplay collision reference stays
           exactly on the selected road/terrain layer. */
        float pre_vert_z=carpos[2], gz=carpos[2];
        WGroundHit ground_hit;
        int ground_cat=world_ground_hit(&scene,carpos[0],carpos[1],carpos[2],
                                        &ground_hit);
        if (ground_cat != WSURF_NONE) {
            gz = ground_hit.z;
        }
        float da_dz=gz-pre_vert_z;
        /* M130: the centre query above still classifies the surface and feeds
           the audit, but it no longer sets height. Four wheel footprints ask for
           REACHABLE support; the sprung integrator owns carpos[2], pitch and
           roll. Reach is a CONTACT window in centimetres, not a search band:
           measured on L4RB at f840 the deck 7.53 m overhead is refused outright
           and the car keeps falling, where the 10 m reach used to accept it and
           climb toward it at 0.5 m per frame. */
        static WGroundHit ride_hit[4], ride_cand[4]; static int ride_reason[4];
        int ride_nsup = 0;
        if (!sstatic && !race_auto) {
            ride_nsup = ride_gather(&scene, carpos, heading, &g_dbg.wheel,
                                    &g_sup, ride_hit, ride_cand, ride_reason);
            /* A CAR SPANS A HOLE (Nik's hardcode, and a sound one: the car
               only ever jumps vertically and never rolls over, so nothing is
               lost by refusing crazy supports). If some wheels stand on road
               and another's query fell into a gap in the mesh -- or onto a
               surface a storey below -- the body would tip nose-first into
               the hole and stick there. A real car bridges it on the wheels
               that DO touch: any support more than 0.7 m below the highest
               contacted one is not the ground this car is standing on. */
            {   float zhi = -1e9f; int any = 0;
                for (int w = 0; w < 4; w++)
                    if (g_sup.valid[w] && g_sup.z[w] > zhi) { zhi = g_sup.z[w]; any = 1; }
                if (any)
                    for (int w = 0; w < 4; w++)
                        if (g_sup.valid[w] && g_sup.z[w] < zhi - 0.7f)
                            g_sup.z[w] = zhi - 0.7f;
            }
            if (!g_ride_ready) { phys_ride_init(&g_ride, &g_sup); g_ride_ready = 1; }
            float zprev = g_ride.z;
            phys_ride_step(&g_ride, &g_sup, 1.0f/60.0f);
            /* Upright, always: vertical travel is free, the BODY never tips
               past what suspension geometry could give. Anything beyond a
               ~7 degree ride tilt is a glitch state, not a stance. */
            if (g_ride.pitch >  0.12f) { g_ride.pitch =  0.12f; g_ride.pitch_rate = 0.0f; }
            if (g_ride.pitch < -0.12f) { g_ride.pitch = -0.12f; g_ride.pitch_rate = 0.0f; }
            if (g_ride.roll  >  0.14f) { g_ride.roll  =  0.14f; g_ride.roll_rate  = 0.0f; }
            if (g_ride.roll  < -0.14f) { g_ride.roll  = -0.14f; g_ride.roll_rate  = 0.0f; }
            carpos[2] = g_ride.z;
            { static int n=0; if (n<8 && getenv("N2_RIDE_TRACE")) { n++;
                printf("ride %d: z=%.3f contacts=0x%x sup z=%.3f/%.3f/%.3f/%.3f\n",
                       n, g_ride.z, g_ride.contact_mask,
                       g_sup.z[0], g_sup.z[1], g_sup.z[2], g_sup.z[3]); } }
            float dzf = g_ride.z - zprev; if (dzf < 0) dzf = -dzf;
            if (dzf > g_ride_maxdz) g_ride_maxdz = dzf;
            if (!g_ride.contact_mask) g_ride_air++;
            if (g_ride.impact > g_ride_maximpact) g_ride_maximpact = g_ride.impact;
        } else if (ground_cat != WSURF_NONE && !race_auto) carpos[2] = gz;
        (void)ride_nsup;
        if (raudit) {   /* observe the production state, after every push */
            float adz = da_dz < 0 ? -da_dz : da_dz;
            if (ground_cat != WSURF_NONE && adz > 0.10f && ra_ground_spikes < 80) {
                const N2Mesh *gm = ground_hit.mesh >= 0 && ground_hit.mesh < scene.count
                                 ? &scene.meshes[ground_hit.mesh] : NULL;
                const N2Mesh *pm = ra_have_ground && ra_prev_ground.mesh >= 0 &&
                                   ra_prev_ground.mesh < scene.count
                                 ? &scene.meshes[ra_prev_ground.mesh] : NULL;
                printf("RA GROUND STEP f%-6ld dz=%+.4f at (%.3f %.3f) "
                       "prev mesh=%d tri=%d %-28s z=%.3f -> "
                       "mesh=%d tri=%d %-28s z=%.3f cat=%s\n",
                       ra_f, da_dz, carpos[0], carpos[1],
                       ra_have_ground ? ra_prev_ground.mesh : -1,
                       ra_have_ground ? ra_prev_ground.tri : -1,
                       pm && pm->sname[0] ? pm->sname : "(none)",
                       ra_have_ground ? ra_prev_ground.z : pre_vert_z,
                       ground_hit.mesh, ground_hit.tri,
                       gm && gm->sname[0] ? gm->sname : "(unnamed)",
                       ground_hit.z,
                       ground_hit.cat == WSURF_ROAD ? "ROAD" : "TERRAIN");
                ra_ground_spikes++;
            }
            if (ground_cat != WSURF_NONE) { ra_prev_ground = ground_hit; ra_have_ground = 1; }
            if (race_state == 1 && ra_start < 0) {
                ra_start = (int)ra_f; ra_cd = (int)ra_f - 60;
                printf("RA countdown complete at frame %ld (%.2f s after Enter); "
                       "race_state=1  start-line pos(%.3f %.3f %.3f) hdg %+.4f gz %.3f\n",
                       ra_f, ra_cd/60.0, carpos[0], carpos[1], carpos[2], heading, gz);
            }
            float d = da_dz < 0 ? -da_dz : da_dz;   /* pre-projection, not post */
            if (d > 0.01f) { ra_clamp++; ra_clampsum += d; }
            float sk = speed < 0 ? -speed : speed;
            if (race_state == 1) {
                if (sk > ra_peak) ra_peak = sk;
                if (ra_start >= 0 && ra_f > ra_start) {
                    float ddx = carpos[0]-ra_px, ddy = carpos[1]-ra_py;
                    ra_dist += sqrtf(ddx*ddx + ddy*ddy);
                }
                if (throttle != 0.0f && PHYS_KMH(sk) < 1.0f && ra_stall < 0) ra_stall = (int)ra_f;
            }
            ra_px = carpos[0]; ra_py = carpos[1];
            if (isnan(carpos[0])||isnan(carpos[1])||isnan(carpos[2])||isnan(speed)||
                isnan(heading)||isnan(vel[0])||isnan(vel[1])) ra_bad |= 1;
            if (carpos[2] < -500.0f || carpos[2] > 500.0f) ra_bad |= 2;
            if (carpos[0]<-8000||carpos[0]>8000||carpos[1]<-8000||carpos[1]>8000) ra_bad |= 4;
            if (ra_start >= 0 && ra_f == ra_start)
                printf("RA ENVELOPE contact_z %.4f  body_ride %.4f "
                       "(wheel ride %.4f, stock drop %.4f)  body local Z [%.4f %.4f]"
                       "  ->  world collision Z [%.4f %.4f]  (= contact + ride + local)\n",
                       carpos[2], car_body_ride, car_ride, car_body_drop,
                       carbb[2], carbb[5], car_z0, car_z1);
            float check_z=carpos[2], check_n[3];
            int check_cat=world_ground_pose(&scene,carpos[0],carpos[1],carpos[2],
                                            &check_z,check_n);
            float resid=check_cat==WSURF_NONE ? 0.0f : check_z-carpos[2];
            { float ar = resid < 0 ? -resid : resid;
              if (ar > ra_maxresid) ra_maxresid = ar;
              float ad = da_dz < 0 ? -da_dz : da_dz;
              if (ad > ra_maxpre) ra_maxpre = ad; }
            if (g_ride_ready && (ra_f % 30 == 0 || g_ride.impact > 0.0f ||
                                 (unsigned)ra_prev_mask != g_ride.contact_mask)) {
                printf("RA RIDE f%-6ld z %9.4f vz %+7.3f  pitch %+6.4f/%+6.3f "
                       "roll %+6.4f/%+6.3f  mask 0x%x air %d impact %5.3f  "
                       "sup[%9.3f %9.3f %9.3f %9.3f] comp[%+.3f %+.3f %+.3f %+.3f] "
                       "cat[%d %d %d %d] mesh[%d %d %d %d] tri[%d %d %d %d]\n",
                       ra_f, g_ride.z, g_ride.vz, g_ride.pitch, g_ride.pitch_rate,
                       g_ride.roll, g_ride.roll_rate, g_ride.contact_mask,
                       g_ride.air_frames, g_ride.impact,
                       g_sup.z[0],g_sup.z[1],g_sup.z[2],g_sup.z[3],
                       g_ride.compression[0],g_ride.compression[1],
                       g_ride.compression[2],g_ride.compression[3],
                       ride_hit[0].cat,ride_hit[1].cat,ride_hit[2].cat,ride_hit[3].cat,
                       ride_hit[0].mesh,ride_hit[1].mesh,ride_hit[2].mesh,ride_hit[3].mesh,
                       ride_hit[0].tri,ride_hit[1].tri,ride_hit[2].tri,ride_hit[3].tri);
                for (int k = 0; k < 4; k++) {
                    float pz = phys_ride_wheel_z(&g_ride, &g_sup, k);
                    static const char *WHY[4] = {"contact","above","below","nocover"};
                    printf("RA WHEEL f%-6ld w%d predicted %9.3f candidate %9.3f "
                           "delta %+8.3f mesh %5d tri %5d cat %d  %-7s contact %d "
                           "body_z %9.3f vz %+7.3f lift %+.4f\n",
                           ra_f, k, pz,
                           ride_cand[k].mesh >= 0 ? ride_cand[k].z : pz,
                           ride_cand[k].mesh >= 0 ? ride_cand[k].z - pz : 0.0f,
                           ride_cand[k].mesh, ride_cand[k].tri, ride_cand[k].cat,
                           WHY[ride_reason[k] & 3],
                           (g_ride.contact_mask >> k) & 1,
                           g_ride.z, g_ride.vz, g_ride.lift);
                }
                ra_prev_mask = (int)g_ride.contact_mask;
            }
            if (g_ride.lift > g_ride_maxlift) g_ride_maxlift = g_ride.lift;
            if (surf_id != surf_prev)
                printf("RA %-6ld t=%6.2fs SURFACE %s -> %s at (%.3f %.3f %.3f) "
                       "spd %.2f km/h\n", ra_f, ra_f/60.0,
                       surf_prev == WSURF_TERRAIN ? "TERRAIN" : "ROAD",
                       surf_id  == WSURF_TERRAIN ? "TERRAIN" : "ROAD",
                       carpos[0], carpos[1], carpos[2], PHYS_KMH(sk));
            if (ra_f % 30 == 0 || (ra_start >= 0 && ra_f == ra_start))
                printf("RA %-6ld t=%6.2fs st=%d thr=%+.0f str=%+.0f pos(%9.3f %9.3f %8.3f) "
                       "hdg %+7.4f spd %7.2f km/h gz %8.3f pre_dz %+8.3f resid %+8.4f "
                       "walls %d rails %d surf %-7s a%.2f v%.2f g%.2f\n",
                       ra_f, ra_f/60.0, race_state, throttle, steer,
                       carpos[0], carpos[1], carpos[2], heading, PHYS_KMH(sk), gz, da_dz, resid,
                       ra_walls, ra_rails,
                       surf_id == WSURF_TERRAIN ? "TERRAIN" : "ROAD",
                       surf_now.accel, surf_now.topfrac, surf_now.lat);
            if (ra_start >= 0 && ra_f - ra_start == 2100) {
                printf("RA SUMMARY track=%s showcase-spawn=(%.3f %.3f %.3f)\n",
                       trackname, spawn[0], spawn[1], spawn[2]);
                printf("RA SUMMARY peak=%.2f km/h final=%.2f km/h travelled=%.2f m\n",
                       PHYS_KMH(ra_peak), PHYS_KMH(sk), ra_dist);
                printf("RA SUMMARY vertical-target delta frames=%d (sum |pre_dz| %.2f m) "
                       "wall=%d rail=%d\n",
                       ra_clamp, ra_clampsum, ra_walls, ra_rails);
                printf("RA SUMMARY max wall correction=%.4f m (face-local; the AABB "
                       "least-penetration path is broad phase only)\n", ra_maxwallcorr);
                printf("RA SUMMARY ride: max body dZ/frame=%.4f m  airborne frames=%ld  "
                       "max landing impact=%.3f m/s\n", g_ride_maxdz, g_ride_air, g_ride_maximpact);
                printf("RA SUMMARY ride rejects: too-high layer=%ld too-low(air)=%ld "
                       "no-cover=%ld\n", g_ride_rejhigh, g_ride_rejlow, g_ride_nocover);
                printf("RA SUMMARY ride: max positional correction=%.4f m "
                       "(bound %.4f m)  max accepted contact delta=%.4f m "
                       "(window +%.2f/-%.2f m + swept fall)  worst window "
                       "overshoot=%+.6f m\n", g_ride_maxlift, PHYS_RIDE_PEN_MAX,
                       g_ride_maxdelta, PHYS_RIDE_REACH_UP, PHYS_RIDE_REACH_DOWN,
                       g_ride_maxover);
                printf("RA SUMMARY max |support delta|=%.4f m  "
                       "max |post-contact residual|=%.4f m\n", ra_maxpre, ra_maxresid);
                printf("RA SUMMARY first throttle-active <1km/h frame=%d (%.2f s after start) "
                       "status: nan=%d oob_z=%d oob_xy=%d\n", ra_stall,
                       ra_stall < 0 ? -1.0 : (ra_stall - ra_start)/60.0,
                       !!(ra_bad&1), !!(ra_bad&2), !!(ra_bad&4));
                for (int sc=WSURF_ROAD; sc<=WSURF_TERRAIN; sc++) if (ca_frames[sc]) {
                    printf("CONTACT %-7s frames=%ld patch-ok=%ld patch-held=%ld "
                           "missing-probes=%ld mixed-layer-probes=%ld\n",
                           sc==WSURF_ROAD?"ROAD":"TERRAIN",ca_frames[sc],
                           ca_patch_ok[sc],ca_patch_bad[sc],ca_missing[sc],ca_mixed[sc]);
                    if (ca_fmin[sc]<1e20f)
                        printf("CONTACT %-7s rendered tyre-bottom residual: "
                               "front[%+.3f..%+.3f] rear[%+.3f..%+.3f] "
                               "max axle mismatch %.3f m  (negative=sunk)\n",
                               sc==WSURF_ROAD?"ROAD":"TERRAIN",
                               ca_fmin[sc],ca_fmax[sc],ca_rmin[sc],ca_rmax[sc],
                               ca_axlemax[sc]);
                }
                if (world_rail_census) {
                    static const char *bn[8] = { "<0.5","0.5-1","1-1.5","1.5-2",
                                                 "2-3","3-5","5-10",">=10" };
                    printf("\nM113 RAIL CANDIDATE CENSUS (triangles passing |nz|<0.30 and the "
                           "car-height test)\n  %-8s %-8s", "source", "");
                    for (int b = 0; b < 8; b++) printf(" %9s", bn[b]);
                    printf("   (triangle Z span, m)\n");
                    for (int c = 0; c < 2; c++) {
                        printf("  %-8s %-8s", c ? "TERRAIN" : "ROAD", "candidates");
                        for (int b = 0; b < 8; b++) printf(" %9ld", world_rc_cand[c][b]);
                        printf("\n  %-8s %-8s", "", "pushed");
                        for (int b = 0; b < 8; b++) printf(" %9ld", world_rc_push[c][b]);
                        printf("     Zspan min %.3f max %.3f\n",
                               world_rc_min[c] > 1e29f ? 0.0f : world_rc_min[c],
                               world_rc_max[c] < -1e29f ? 0.0f : world_rc_max[c]);
                    }
                }
                /* ---- M94 attribution report ---- */
                printf("\nM94 COLLISION ATTRIBUTION  (%d distinct sources, %d recorded events)\n",
                       m94n, m94nev);
                printf("dominant groups (up to 12, by response count):\n");
                for (int k = 0; k < 12; k++) {
                    int b = -1, bc = 0;
                    for (int i = 0; i < m94n; i++)
                        if (m94g[i].count > bc) { bc = m94g[i].count; b = i; }
                    if (b < 0) break;
                    M94Grp *g = &m94g[b];
                    const N2Mesh *m = &scene.meshes[g->mesh];
                    if (g->kind == 0)
                        printf("  BUILDING x%-5d mesh %-6d %-30s %-8s "
                               "AABB XY[%.1f %.1f][%.1f %.1f] Z[%.1f %.1f] "
                               "car(%.2f %.2f %.2f) wp %d d %.2f m first f%ld\n",
                               g->count, g->mesh, m->sname[0]?m->sname:"(unnamed)",
                               n2_scen_name(m->scen), g->bb[0],g->bb[2], g->bb[1],g->bb[3],
                               g->bb[4],g->bb[5], g->cx,g->cy,g->cz, g->wp, g->segd, g->first);
                    else
                        printf("  RAIL     x%-5d mesh %-6d %-30s %-8s tri %-6d "
                               "nz %+.3f triZ[%.2f %.2f] edge %.3f m "
                               "car(%.2f %.2f %.2f) wp %d d %.2f m first f%ld\n",
                               g->count, g->mesh, m->sname[0]?m->sname:"(unnamed)",
                               bc_cat(m->cat), g->tri, g->nz, g->zlo, g->zhi, g->edged,
                               g->cx,g->cy,g->cz, g->wp, g->segd, g->first);
                    g->count = -g->count;   /* mark printed */
                }
                for (int i = 0; i < m94n; i++) if (m94g[i].count < 0) m94g[i].count = -m94g[i].count;
                printf("first 20 chronological responses:\n");
                for (int k = 0; k < 20 && k < m94nev; k++) {
                    M94Grp *g = &m94g[m94ev[k].grp];
                    const N2Mesh *m = &scene.meshes[g->mesh];
                    printf("  f%-6ld %-8s mesh %-6d %-30s tri %-6d wp %d\n",
                           m94ev[k].f, g->kind ? "RAIL" : "BUILDING", g->mesh,
                           m->sname[0]?m->sname:"(unnamed)", g->tri, g->wp);
                }
                /* M91 wall-rejected waypoints: what actually sits there? */
                printf("\nM91 wall-rejected waypoints, classified:\n");
                { float hl = (carbb[3]-carbb[0])*0.5f, hw = (carbb[4]-carbb[1])*0.5f;
                  if (hl < 0.5f) hl = 2.20f; if (hw < 0.5f) hw = 0.90f; (void)hl; (void)hw;
                  int nb=0, nr=0, nboth=0, nnone=0;
                  for (int i = 0; i < aipath.n; i++) {
                    float x = aipath.xy[i*2], y = aipath.xy[i*2+1];
                    if (!ss_in_wall(obst, nobst, x, y, 1.3f)) continue;
                    float rz = 0;
                    int hasroad = ss_road_z(&scene, (const float (*)[4])world.mbb, x, y,
                                            carpos[2], &rz);
                    /* probe the production rail test on a COPY: no side effects */
                    WRailHit rh; rh.mesh = -1;
                    float tmp[3] = { x, y, hasroad ? rz : carpos[2] };
                    int rail = world_wall_push(&scene, tmp, 1.3f, &rh);
                    int hitduring = 0;
                    for (int g = 0; g < m94n; g++) if (m94g[g].wp == i) hitduring = 1;
                    const char *cls = rail ? "BOTH (AABB proxy + exact rail face)"
                                           : "building AABB proxy only";
                    if (rail) nboth++; else nb++;
                    if (!hitduring) nnone++;
                    printf("  wp %-4d (%9.3f %9.3f) road %s  %-38s  hit during trace: %s",
                           i, x, y, hasroad ? "yes" : "NO ", cls, hitduring ? "yes" : "no");
                    if (rail) printf("  [rail mesh %d tri %d nz %+.3f]", rh.mesh, rh.tri, rh.nz);
                    printf("\n");
                    if (rail) nr++;
                  }
                  printf("  totals: AABB-proxy-only %d, both %d, rail-bearing %d, "
                         "never hit during the trace %d\n", nb, nboth, nr, nnone);
                }
                ra_done = 1;
            }
            ra_f++;
        }
        if (daudit) {   /* observe the production state, after every push */
            float d = da_dz < 0 ? -da_dz : da_dz;
            if (d > 0.01f) { da_clamp++; da_clampsum += d; }
            float sk = speed < 0 ? -speed : speed;
            if (sk > da_peak) da_peak = sk;
            if (da_f) { float ddx = carpos[0]-da_prevx, ddy = carpos[1]-da_prevy;
                        da_dist += sqrtf(ddx*ddx + ddy*ddy); }
            da_prevx = carpos[0]; da_prevy = carpos[1];
            if (isnan(carpos[0])||isnan(carpos[1])||isnan(carpos[2])||
                isnan(speed)||isnan(heading)||isnan(vel[0])||isnan(vel[1])) da_bad |= 1;
            if (carpos[2] < -500.0f || carpos[2] > 500.0f) da_bad |= 2;
            if (carpos[0] < -8000 || carpos[0] > 8000 ||
                carpos[1] < -8000 || carpos[1] > 8000) da_bad |= 4;
            if (throttle != 0.0f && PHYS_KMH(sk) < 1.0f && da_stall < 0) da_stall = (int)da_f;
            if (da_f % 30 == 0 || da_f == 119 || da_f == 120 || da_f == 719 ||
                da_f == 720 || da_f == 1919 || da_f == 1920 || da_f == 2219)
                printf("DA %-6ld t=%6.2fs thr=%+.0f str=%+.0f  pos(%9.3f %9.3f %8.3f) "
                       "hdg %+7.4f  spd %7.2f km/h  vel(%8.3f %8.3f)  gz %8.3f dz %+7.3f  "
                       "walls %d rails %d\n", da_f, da_f/60.0, throttle, steer,
                       carpos[0], carpos[1], carpos[2], heading, PHYS_KMH(sk),
                       vel[0], vel[1], gz, da_dz, da_walls, da_rails);
            if (da_f == shotframes - 1) {
                float sk2 = speed < 0 ? -speed : speed;
                printf("DA SUMMARY track=%s spawn=(%.3f %.3f %.3f) hdg0=%.4f\n",
                       trackname, spawn[0], spawn[1], spawn[2], heading0);
                printf("DA SUMMARY peak=%.2f km/h final=%.2f km/h travelled=%.2f m\n",
                       PHYS_KMH(da_peak), PHYS_KMH(sk2), da_dist);
                printf("DA SUMMARY vertical-target delta frames=%d (sum |dz| %.2f m) "
                       "wall-response=%d rail-response=%d\n",
                       da_clamp, da_clampsum, da_walls, da_rails);
                printf("DA SUMMARY first throttle-active <1km/h frame=%d (%.2f s)  "
                       "status: nan=%d oob_z=%d oob_xy=%d\n", da_stall,
                       da_stall < 0 ? -1.0 : da_stall/60.0,
                       !!(da_bad&1), !!(da_bad&2), !!(da_bad&4));
            }
            da_f++;
        }
        /* Gameplay contact stays the exact centre triangle, but the chassis is
           wider than one triangle. Derive its visual plane from supported
           front/rear/left/right footprint probes on the SAME layer. If the
           patch straddles a deck edge or has a missing probe, keep the last
           stable pose instead of snapping to whichever tiny triangle happens
           to cover the centre this frame. This is rigid-body road alignment,
           not the later per-wheel suspension system. */
        int chassis_patch_ok=0;
        if (!sstatic && !race_auto && g_ride_ready) {
            /* Body tilt IS the ride pitch/roll -- no second smoothing filter.
               up = world Z tilted back by pitch and toward the low side by roll. */
            float fx=cosf(heading), fy=sinf(heading), lx=-fy, ly=fx;
            /* The ride model answers to the GROUND -- bumps, kerbs, a wheel
               dropping off a deck. It knows nothing about acceleration, so
               the car never squatted, never dived under braking and never
               leaned in a corner: the springs looked welded solid. The
               dynamics model carries exactly that missing part, so the two
               add: terrain from below, weight transfer from the driving. */
            float body_pitch = g_ride.pitch, body_roll = g_ride.roll;
            if (g_newphys && g_newphys_ok) {
                body_pitch += veh_bridge_pitch();
                body_roll  += veh_bridge_roll();
            }
            float sp=sinf(body_pitch), sr=sinf(body_roll);
            float u[3] = { -fx*sp - lx*sr, -fy*sp - ly*sr, 1.0f };
            float ul=sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
            if (ul>1e-6f){ car_up[0]=u[0]/ul; car_up[1]=u[1]/ul; car_up[2]=u[2]/ul; }
            chassis_patch_ok = g_ride.contact_mask == 0xF;
        } else
        { float gn[3] = {car_up[0],car_up[1],car_up[2]};
          float k = 0.040f;
          float halftrack=0.25f*(g_dbg.wheel.front_track+g_dbg.wheel.rear_track);
          int patch_ok = ground_cat != WSURF_NONE &&
              world_ground_patch_normal(&scene,carpos[0],carpos[1],heading,
                                        g_dbg.wheel.front_axle,
                                        g_dbg.wheel.rear_axle,halftrack,
                                        &ground_hit,gn);
          chassis_patch_ok=patch_ok;
          if (!patch_ok && ground_cat == WSURF_NONE) {
              gn[0]=0; gn[1]=0; gn[2]=1; k=0.015f;
          }
          if (patch_ok) {
              float gh=sqrtf(gn[0]*gn[0]+gn[1]*gn[1]);
              float maxgrade=(ground_cat==WSURF_TERRAIN ? 0.4663f : 0.2679f)
                            * (gn[2] > 0.1f ? gn[2] : 0.1f); /* tan(25/15 deg) */
              if (gh > maxgrade && gh > 1e-6f) {
                  float q=maxgrade/gh; gn[0]*=q; gn[1]*=q;
                  float gl=sqrtf(gn[0]*gn[0]+gn[1]*gn[1]+gn[2]*gn[2]);
                  gn[0]/=gl; gn[1]/=gl; gn[2]/=gl;
              }
          }
          for (int a = 0; a < 3; a++) car_up[a] += (gn[a] - car_up[a]) * k;
          float l = sqrtf(car_up[0]*car_up[0]+car_up[1]*car_up[1]+car_up[2]*car_up[2]);
          if (l > 1e-6f) { car_up[0]/=l; car_up[1]/=l; car_up[2]/=l; } }

        if ((raudit || daudit) && (ground_cat==WSURF_ROAD || ground_cat==WSURF_TERRAIN)) {
            float wr=carprof.wheel_r*(g_dbg.wheel_scale>0.05f?g_dbg.wheel_scale:1.0f);
            float res[4]; int wcat[4]; int coherent=1;
            wheel_contact_residuals(&scene,carpos,heading,car_up,&g_dbg.wheel,
                                    car_ride,wr,res,wcat);
            ca_frames[ground_cat]++;
            if (chassis_patch_ok) ca_patch_ok[ground_cat]++;
            else ca_patch_bad[ground_cat]++;
            for (int i=0;i<4;i++) {
                if (wcat[i]==WSURF_NONE) { ca_missing[ground_cat]++; coherent=0; }
                else if (wcat[i]!=ground_cat) { ca_mixed[ground_cat]++; coherent=0; }
            }
            if (coherent) {
                float fr=.5f*(res[0]+res[1]), rr=.5f*(res[2]+res[3]);
                if (fr<ca_fmin[ground_cat]) ca_fmin[ground_cat]=fr;
                if (fr>ca_fmax[ground_cat]) ca_fmax[ground_cat]=fr;
                if (rr<ca_rmin[ground_cat]) ca_rmin[ground_cat]=rr;
                if (rr>ca_rmax[ground_cat]) ca_rmax[ground_cat]=rr;
                float ad=fabsf(fr-rr); if(ad>ca_axlemax[ground_cat])ca_axlemax[ground_cat]=ad;
            }
        }

        /* drifting? extend the tyre ribbons + puff smoke at the rear wheels */
        /* Sliding sideways OR spinning the wheels both lay rubber -- a
           standing burnout leaves the blackest marks of all, and until now
           only sideways motion counted, so a doughnut drew nothing. */
        int drifting = (dmag > PHYS_MAXSPD*0.2f && speed > PHYS_MAXSPD*0.22f)
                     || ((g_newphys && g_newphys_ok)
                         && fabsf(veh_bridge_slip_ratio()) > 0.35f);
        /* tyre screech scales with how hard the back is sliding */
        g_skid = (race_state==1 && drifting)
               ? (0.12f + 0.45f*(dmag - PHYS_MAXSPD*0.2f)/PHYS_MAXSPD) : 0.0f;
        if (g_skid > 0.35f) g_skid = 0.35f;
        if (drifting) {
            /* The marks come off the ACTUAL rear wheels. They were drawn at a
               hard-coded 1.6 m behind the origin and a full metre out each
               side -- outside the 350Z's real track, so the rubber floated
               beside the tyres. The wheel config now carries the record's own
               axle line and track; use them. */
            float rax = -g_dbg.wheel.rear_axle;          /* record X is negative */
            float rht = 0.5f * g_dbg.wheel.rear_track;
            if (rax < 0.5f || rax > 3.0f) rax = 1.6f;    /* sane fallback */
            if (rht < 0.4f || rht > 1.2f) rht = 0.8f;
            float rx = carpos[0]-nf[0]*rax, ry = carpos[1]-nf[1]*rax;
            float rz = carpos[2]+0.05f;
            float curL[3]={rx+nr[0]*rht, ry+nr[1]*rht, rz},
                  curR[3]={rx-nr[0]*rht, ry-nr[1]*rht, rz};
            /* A segment is only a segment if the wheel actually rolled there:
               after a respawn or a teleport the previous point is somewhere
               else entirely, and connecting it drew a single black band tens
               of metres long across the car park. Lift the pen instead. */
            if (skidpen && (fabsf(curL[0]-prevL[0]) + fabsf(curL[1]-prevL[1]) > 2.0f))
                skidpen = 0;
            if (skidpen) {   /* connect last frame's wheel points into ribbon segments */
                float *seg;
                seg=skid[skidhead]; seg[0]=prevL[0];seg[1]=prevL[1];seg[2]=prevL[2];
                seg[3]=curL[0];seg[4]=curL[1];seg[5]=curL[2];seg[6]=1.0f;
                skidhead=(skidhead+1)%MAXSKID; if(skidn<MAXSKID)skidn++;
                seg=skid[skidhead]; seg[0]=prevR[0];seg[1]=prevR[1];seg[2]=prevR[2];
                seg[3]=curR[0];seg[4]=curR[1];seg[5]=curR[2];seg[6]=1.0f;
                skidhead=(skidhead+1)%MAXSKID; if(skidn<MAXSKID)skidn++;
            }
            memcpy(prevL,curL,sizeof curL); memcpy(prevR,curR,sizeof curR); skidpen=1;
            /* dense puffs at both wheels every frame; soft = many low-alpha billboards */
            float wheels[2][2] = {{curL[0],curL[1]},{curR[0],curR[1]}};
            for (int wi = 0; wi < 2; wi++)
                for (int pc = 0; pc < 3 && smoken < MAXSMOKE; pc++) {
                    int si = smoken++;
                    smoke[si].p[0]=wheels[wi][0]+(rand()%100-50)*0.012f;
                    smoke[si].p[1]=wheels[wi][1]+(rand()%100-50)*0.012f;
                    smoke[si].p[2]=rz+0.2f+(rand()%30)*0.01f;
                    smoke[si].v[0]=(rand()%100-50)*0.0025f; smoke[si].v[1]=(rand()%100-50)*0.0025f;
                    smoke[si].v[2]=0.045f+(rand()%40)*0.0015f;
                    smoke[si].life=1.0f; smoke[si].size=0.55f+(rand()%40)*0.01f;
                }
        } else skidpen = 0;   /* lift the pen so the next drift starts a fresh ribbon */
        /* advance smoke particles (rise, expand, fade) */
        for (int i = 0; i < smoken; i++) {
            smoke[i].p[0]+=smoke[i].v[0]; smoke[i].p[1]+=smoke[i].v[1]; smoke[i].p[2]+=smoke[i].v[2];
            smoke[i].v[2]*=0.98f; smoke[i].size+=0.05f; smoke[i].life-=0.012f;
            if (smoke[i].life <= 0) smoke[i]=smoke[--smoken], i--;
        }

        /* AI opponents: each steers toward its next racing-line waypoint */
        for (int k = 0; race_state == 1 && k < nai; k++)
            ai_step(&ais[k], k, &aipath, &scene, start_idx,
                    p_lap*aipath.n + p_prev);
        if (race_state == 1 && aipath.n > 1) {   /* same lap logic for the player */
            int prel = (n2_nearest_wp(&aipath, carpos[0], carpos[1]) - start_idx
                        + aipath.n) % aipath.n;
            if (p_prev > aipath.n*3/4 && prel < aipath.n/4) p_lap++;
            p_prev = prel;
        }

        /* race state machine: countdown -> racing -> finished */
        racetimer++;
        if (race_state == 0 && racetimer >= COUNTDOWN) { race_state = 1; racetimer = 0; }
        if (race_state == 1 && p_lap >= LAP_TARGET) {
            int ahead = 0, pp = p_lap*aipath.n + p_prev;
            for (int k = 0; k < nai; k++)
                if (ais[k].lap*aipath.n + ais[k].prevrel > pp) ahead++;
            finish_place = ahead + 1;
            race_state = 2;
        }

        /* car-to-car collision: push overlapping cars apart (circle test) */
        { float thud = phys_car_contacts(carpos, vel, speed, ais, nai);
          if (thud > g_hit) g_hit = thud; }

        /* M132-R capture freeze: latch the position production placed at the
           start line and hold it. Zeroing throttle and velocity is not enough --
           the ride settles and the wall push nudges over the following frames --
           and the requirement is a capture AT the placed pose, not near it.
           Capture mode only; nothing here runs in play. */
        if (poseshot && race_state != 3) {
            /* pinned at the PLACEMENT frame -- the frame the start-line snap /
               grid slot put the car down -- not at the countdown end 180 frames
               later, by which point it has already drifted (2.27 m on L4RA). */
            static float posepin[3]; static int pinned = 0;
            if (!pinned) { posepin[0]=carpos[0]; posepin[1]=carpos[1]; posepin[2]=carpos[2]; pinned=1;
                           g_pose_t0 = SDL_GetTicks(); g_pose_f0 = ra_f;
                           printf("POSE pinned at placement frame %ld -> "
                                  "(%.3f, %.3f, %.3f)\n",
                                  ra_f, carpos[0], carpos[1], carpos[2]); }
            carpos[0]=posepin[0]; carpos[1]=posepin[1]; carpos[2]=posepin[2];
        }
        /* camera: menu = slow orbit around the parked car; else chase cam */
        float want[3];
        if (race_state == 3) {              /* orbit the parked car, framing the city around it */
            menuspin += 0.006f;
            want[0] = carpos[0] + cosf(menuspin)*16.0f;
            want[1] = carpos[1] + sinf(menuspin)*16.0f;
            want[2] = carpos[2] + 8.0f;
        } else if (shotyaw < 1e8f) {        /* --shot-yaw: fixed capture heading */
            want[0] = carpos[0]-cosf(shotyaw)*g_dbg.chase_distance;
            want[1] = carpos[1]-sinf(shotyaw)*g_dbg.chase_distance;
            want[2] = carpos[2]+g_dbg.chase_height;
        } else if (g_newcam) {              /* chase, NFSU2-flavoured */
            /* Which way is the car actually going? Below walking pace the
               velocity is noise, so ease back onto the nose; in reverse stay
               behind the nose too (a backing car is watched over its boot). */
            float vwx = vel[0]*60.0f, vwy = vel[1]*60.0f;
            float vg = sqrtf(vwx*vwx + vwy*vwy);
            float along = vwx*fwd[0] + vwy*fwd[1];
            float az_want = (vg > 2.5f && along > 0.0f)
                          ? atan2f(vwy, vwx) : heading;
            if (!g_cam_init) { g_cam_az = heading; g_cam_init = 1; }
            /* ease the azimuth with wrap; the lag IS the side view */
            float d_az = az_want - g_cam_az;
            while (d_az >  3.14159f) d_az -= 6.28318f;
            while (d_az < -3.14159f) d_az += 6.28318f;
            g_cam_az += d_az * 0.085f;

            /* distance breathes: further with speed, tucked in on the brakes */
            float dwant = 5.8f * (1.0f + 0.38f * fminf(vg / 50.0f, 1.0f));
            if (throttle < -0.05f) dwant = 5.2f;
            g_cam_dist += (dwant - g_cam_dist) * 0.06f;

            /* FOV opens with speed -- the cheap way to make speed feel fast */
            float fov_want = 0.88f + 0.24f * fminf(vg / 50.0f, 1.0f);
            g_fov += (fov_want - g_fov) * 0.05f;

            want[0] = carpos[0] - cosf(g_cam_az) * g_cam_dist;
            want[1] = carpos[1] - sinf(g_cam_az) * g_cam_dist;
            want[2] = carpos[2] + 2.35f + 0.35f * fminf(vg / 50.0f, 1.0f);
        } else {                            /* chase: behind + above, tunable */
            want[0] = carpos[0]-fwd[0]*g_dbg.chase_distance;
            want[1] = carpos[1]-fwd[1]*g_dbg.chase_distance;
            want[2] = carpos[2]+g_dbg.chase_height;
        }
        /* exponential smoothing (fixed timestep): actual pos AND look-target each
           ease toward their ideal by `stiffness`/frame -- the target lag is what
           gives the spring/swing feel through corners. */
        float k = g_dbg.chase_stiffness; if (k < 0.02f) k = 0.02f; if (k > 1.0f) k = 1.0f;
        if (shotyaw < 1e8f) k = 1.0f;   /* capture pose must be exact, not eased */
        for (int c=0;c<3;c++) cam[c] += (want[c]-cam[c])*k;
        static float camtgt[3]; static int camtgt_init = 0;
        float idealtgt[3] = { carpos[0], carpos[1],
                              carpos[2] + (g_newcam ? 1.15f : 1.5f) };  /* car centre, slightly up */
        if (!camtgt_init) { camtgt[0]=idealtgt[0]; camtgt[1]=idealtgt[1]; camtgt[2]=idealtgt[2]; camtgt_init=1; }
        for (int c=0;c<3;c++) camtgt[c] += (idealtgt[c]-camtgt[c])*k;
        float look[3] = { camtgt[0]-cam[0], camtgt[1]-cam[1], camtgt[2]-cam[2] };
        if (g_dbg.freecam) {                 /* fly-through overrides the chase/orbit cam */
            cam[0]=fc[0]; cam[1]=fc[1]; cam[2]=fc[2];
            float cp=cosf(fpitch);
            look[0]=cp*cosf(fyaw); look[1]=cp*sinf(fyaw); look[2]=sinf(fpitch);
        }

        int W, H; SDL_GL_GetDrawableSize(win, &W, &H);
        glViewport(0, 0, W, H);
        /* LIVE REFLECTION. There is no environment texture in the data -- the
           slot exists and nothing fills it, which is why the game's options
           expose a reflection rate at all. Six faces are rendered around the
           car instead, low resolution, one face per frame. */
        {   static int envtick = 0;
            /* ONE FACE PER FRAME. Refreshing all six at once costs the same
               overall but visibly jumps: the map holds for five frames and
               then changes completely. One face per frame takes the same six
               frames and spreads the change out. */
            if (g_envcube_ready && ncar) {
                /* Unbind the map from unit 1 while rendering into it: it
                   cannot be a shader source and a framebuffer target at the
                   same time -- the driver marks such a texture unloadable and
                   substitutes a null one. */
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
                glActiveTexture(GL_TEXTURE0);
                float eye[3] = { carpos[0], carpos[1], carpos[2] + 1.0f };
                {   int f = envtick++ % 6;
                    float fwd[3], upv[3];
                    env_cube_face_dirs(f, fwd, upv);
                    float Vf[16], Pf[16], MVPf[16];
                    mat_lookat_up(eye, fwd, upv, Vf);
                    mat_persp(1.5708f, 1.0f, 1.0f, 30000.0f, Pf);
                    mat_mul(Pf, Vf, MVPf);
                    env_cube_begin(f);
                    glClearColor(g_dbg.fog_r, g_dbg.fog_g, g_dbg.fog_b, 1);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPf);
                    glUniform1f(rp.uEnv, 0.0f); glUniform1f(rp.uMatOn, 0.0f);
                    glUniform1f(uSpec, 0.0f);
                    /* sky: no fog, no depth writes */
                    glUniform1f(uUnlit, 1.0f); glUniform1f(rp.uFogDensity, 0.0f);
                    glUniform3f(uColor, 1.0f, 1.0f, 1.0f); glUniform1f(uAlpha, 1.0f);
                    glDepthMask(GL_FALSE);
                    for (int k = 0; k < nsky; k++) {
                        if (!skybatch[k].tex) continue;
                        glUniform1f(uUseTex, 1.0f);
                        glBindTexture(GL_TEXTURE_2D, skybatch[k].tex);
                        draw_batch(&skybatch[k]);
                    }
                    glDepthMask(GL_TRUE); glUniform1f(uUnlit, 0.0f);
                    /* the world around the car: what actually reflects */
                    glUniform1f(rp.uFogDensity, g_dbg.fog_density);
                    glUniform1f(rp.uVColor, g_dbg.vcolor);
                    for (int k = 0; k < nbatch; k++) {
                        N2Batch *b = &wbatch[k];
                        float dx = eye[0] < b->bbox_min[0] ? b->bbox_min[0]-eye[0]
                                 : (eye[0] > b->bbox_max[0] ? eye[0]-b->bbox_max[0] : 0);
                        float dy = eye[1] < b->bbox_min[1] ? b->bbox_min[1]-eye[1]
                                 : (eye[1] > b->bbox_max[1] ? eye[1]-b->bbox_max[1] : 0);
                        if (dx*dx + dy*dy > 250.0f*250.0f) continue;
                        if (b->tex) { glUniform1f(uUseTex, 1.0f);
                                      glBindTexture(GL_TEXTURE_2D, b->tex); }
                        else { glUniform1f(uUseTex, 0.0f);
                               glUniform3f(uColor, 0.28f, 0.29f, 0.31f); }
                        draw_batch(b);
                    }
                    glUniform1f(rp.uVColor, 0.0f);
                }
                env_cube_end(W, H);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_CUBE_MAP, env_cube_tex());
                glGenerateMipmap(GL_TEXTURE_CUBE_MAP);   /* smooths the sampling */
                glActiveTexture(GL_TEXTURE0);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_CUBE_MAP, env_cube_tex());
                glActiveTexture(GL_TEXTURE0);
            }
        }

        /* The frame is drawn into a texture so it can be toned afterwards;
           see src/sfx/post.c. Without it the picture is the raw scene, which
           reads cold and flat. */
        if (!no_post && pp_init(W, H)) { post_on = 1; pp_begin(); }
        else post_on = 0;
        /* the sky is cleared to the fog colour: distant geometry dissolves
           into exactly what the horizon shows */
        glClearColor(g_dbg.fog_r, g_dbg.fog_g, g_dbg.fog_b, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        /* The material model belongs to the car: leave it on and the world is
           multiplied by whatever the last panel's record said, which turns the
           road black. Off at the top of every frame, on only where a submesh
           names a material. */
        glUniform1f(rp.uMatOn, 0.0f);
        /* And the flat-colour flags, for the same reason: the billboard passes
           (shadow, lamp halos, glow quads, HUD) all raise them deliberately,
           and any one of them left raised sends the whole next frame down the
           shader's unlit early-return -- no texturing, no specular, no
           reflection. It reads as "the lighting turned off". */
        /* FULL RESET OF THE SHADER STATE, every frame.
         *
         * A dozen passes raise these deliberately -- shadows, halos, tail
         * lights, headlight pools, tyre smoke, skid marks, the HUD -- and any
         * one of them that forgets to lower a flag corrupts everything drawn
         * after it. Worse, several of those passes only run while DRIVING, so
         * the corruption appears the moment the menu is left and never in the
         * menu itself: "the lights go out when the chase camera starts".
         * Rather than audit every pass for what it restores, the frame begins
         * from a known state. */
        glUniform1f(rp.uUnlit, 0.0f);
        glUniform1f(rp.uSoft, 0.0f);
        glUniform1f(rp.uAlpha, 1.0f);
        glUniform1f(rp.uUseTex, 0.0f);
        glUniform1f(rp.uVColor, 0.0f);
        glUniform1f(rp.uEnv, 0.0f);
        glUniform1f(rp.uSpec, 0.0f);
        glUniform1f(rp.uDecal, 0.0f);
        glUniform1f(rp.uAlphaTest, 0.0f);
        glUniform1f(rp.uVista, 0.0f);
        glUniform1f(rp.uEnvCubeOn, 0.0f);
        glUniform3f(rp.uColor, 1.0f, 1.0f, 1.0f);
        glUniform3f(rp.uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
        glDisable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        /* Culling OFF: the car, glass and the glow billboards are all
           single-sided geometry meant to be seen from either face. It is
           enabled only for the world pass, and only on request. Turning it on
           here made the lamp halos vanish as soon as the camera moved behind
           them -- which is every frame once the chase camera starts. */
        glDisable(GL_CULL_FACE);
        glUniform3f(rp.uFogColor, g_dbg.fog_r, g_dbg.fog_g, g_dbg.fog_b);
        glUniform1f(rp.uFogDensity, g_dbg.fog_density);
        glUniform1f(rp.uUVCheck, (float)g_dbg.show_uv_checker);

        float P[16], V[16], MVP[16];
        /* menu orbits ~9m from the car, so use a close near-plane there; the
           driving near scales with region size but is clamped so a whole-city
           maxr can't push it past the car and clip it. */
        float znear = race_state==3 ? 0.2f : (maxr*0.01f < 1.0f ? maxr*0.01f : 1.0f);
        /* zfar used to be maxr*30 (~290000) — a ~300000:1 range that destroys
           24-bit depth precision and z-fights coplanar surfaces (worse on GPUs
           with a shallower depth buffer). Nothing past VIEW_DIST (700 m) is drawn,
           so clamp the world far plane to a realistic 2000 m: znear/zfar ~= 2000:1
           gives ample resolution. The skydome shell spans ~16 km and would clip
           at 2000, so it gets its own deep frustum (Psky) below. */
        /* Far plane from the same fog policy: nothing beyond the 1%-contribution
           distance is drawn, so the depth range only has to cover it (plus 20%
           headroom for batches straddling the gate). Vistas get their own deep
           frustum in the background pass below, exactly as the sky does. */
        float zfar = g_dbg.fog_density > 1e-5f
                   ? 1.2f * sqrtf(-logf(0.01f)) / g_dbg.fog_density : 2000.0f;
        if (zfar < 500.0f) zfar = 500.0f;
        mat_persp(g_fov, (float)W/H, znear, zfar, P);
        mat_lookat(cam, look, V);
        mat_mul(P, V, MVP);
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
        glUniform1f(uUnlit, 0.0f); glUniform1f(uSpec, 0.0f);   /* world is matte */
        glUniform1f(uAmbient, g_dbg.ambient); glUniform1f(uDiffuse, g_dbg.diffuse);
        glUniform3f(uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);   /* track = world space */

        /* M132-R: ONE reset for the whole frame, before any pass. The old reset
           sat between the vista and world passes and silently discarded every
           sky and vista draw from the total. */
        g_dbg.drawn = 0;
        int skydraws = 0, vistadraws = 0;
        /* skybox: drawn first, camera-locked (view built from a zero eye so
           translation drops out — the classic "at infinity" trick) and with
           depth-write off so every real batch below still overdraws it via
           the depth test alone. Falls back to nothing (flat fog clear
           colour shows through) when the region has no SKYDOME mesh. */
        if (nsky && (passmode == 0 || passmode == 3 || passbatch >= 0)) {   /* M78/M79 */
            /* The dome is seen from INSIDE, so back-face culling must be
               disabled explicitly: GL state is global and may still be on from
               the previous pass. */
            glDisable(GL_CULL_FACE);
            /* THE DOME SITS IN WORLD COORDINATES, not around the origin: it
               spans the whole map, centred near (-842, 285), a hemisphere some
               9700 units across and 8000 tall. The usual "camera at the
               origin" trick moves it aside and the sky misses the frame
               entirely. It is an ordinary world object with no camera
               attachment, so it is drawn with the ordinary camera -- only the
               far plane has to be widened to fit it, and depth writes are
               off. */
            float Vsky[16], MVPsky[16], Psky[16];
            mat_lookat(cam, look, Vsky);
            mat_persp(g_fov, (float)W/H, znear, 30000.0f, Psky);
            mat_mul(Psky, Vsky, MVPsky);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPsky);
            glUniform1f(uUnlit, 1.0f);
            /* No fog on the sky. The dome is thousands of metres away and
               exp(-(depth*density)^2) is zero at that range, so the whole dome
               paints in the fog colour -- which is the clear colour, and looks
               exactly like having no sky at all. */
            glUniform1f(rp.uFogDensity, 0.0f);
            glDepthMask(GL_FALSE);
            /* The sky is textured: the emissive shader path samples its
               texture (see render.c), and the images come from the shared
               dynamic texture file by name. The main texture covers the dome,
               the _CAP one the zenith. An untextured batch falls back to the
               fog colour so it dissolves into the horizon rather than
               inheriting whatever uColor the previous pass left behind -- that
               used to paint a red block across the top of the frame. */
            glUniform3f(uColor, 1.0f, 1.0f, 1.0f);
            glUniform1f(uAlpha, 1.0f);
            for (int k = 0; k < nsky; k++) {
                if (skybatch[k].tex) { glUniform1f(uUseTex, 1.0f);
                                       glBindTexture(GL_TEXTURE_2D, skybatch[k].tex); }
                else { glUniform1f(uUseTex, 0.0f);
                       glUniform3f(uColor, g_dbg.fog_r, g_dbg.fog_g, g_dbg.fog_b); }
                draw_batch(&skybatch[k]);
                g_dbg.drawn++; skydraws++;
            }
            glUniform1f(uUseTex, 0.0f);
            glDepthMask(GL_TRUE); glUniform1f(uUnlit, 0.0f);
            glUniform1f(rp.uFogDensity, g_dbg.fog_density);   /* fog back on */
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);   /* restore the real camera */
        }

        /* M132: the ordinary-world cutoff is DERIVED from the active exp^2 fog
           rather than a hard 700 m. fog = exp(-(d*density)^2), so a batch can
           still contribute VIEW_MINCONTRIB of its own colour out to
              d = sqrt(-ln(contrib)) / density
           At the default density 0.0023 that is 933 m; the old 700 m gate threw
           away everything from 700 m out to where the fog itself had not yet
           reached 1%. Density 0 (fog off) has no such distance, so it falls back
           to the world far plane. The test still uses batch AABBs, never
           centres, so a long batch is kept while any part of it is in range. */
        #define VIEW_MINCONTRIB 0.01f
        float view_dist = g_dbg.fog_density > 1e-5f
                        ? sqrtf(-logf(VIEW_MINCONTRIB)) / g_dbg.fog_density
                        : zfar;
        if (view_dist > zfar) view_dist = zfar;
        if (tier == 0) view_dist = 700.0f;      /* --tier baseline: the old gate */
        #define VIEW_DIST view_dist
        int vistadrawn = 0, vistanear = 0;      /* batches */
        int vistamesh = 0, vistanearmesh = 0;   /* their ACTUAL mesh counts */
        float vistamind = 1e30f, vistakeptmin = 1e30f;  /* measured surface distances */
        int vistaburied = 0;
        #define VISTA_BURIED_M 300.0f
        static int vlist = -1;
        if (vlist < 0) vlist = getenv("OPENUG2_VISTA_LIST") ? 1 : 0;
        /* M132 vista pass: authored backdrop impostors, drawn after the sky and
           before any ordinary geometry.
             - world space, authored transforms, nothing re-placed;
             - its own deep frustum, because the backdrops span kilometres and
               the ordinary far plane is derived from fog;
             - depth TEST on, depth WRITE off, so every real road, building and
               car drawn afterwards overwrites them unconditionally -- a
               backdrop can never occlude the city or cut through the road;
             - textured through the normal lit path (uUnlit would discard the
               texture and flat-fill them with the fog colour);
             - its own fog density, because ordinary fog reaches 1% at 933 m and
               would erase a backdrop standing kilometres out. Derived, not
               chosen: the density that leaves the FURTHEST vista vertex at
               VISTA_MINCONTRIB of its own colour, so the backdrop fades into
               the same horizon haze instead of being deleted by it. */
        if (nvista && tier == 2 && g_dbg.show_track && (passmode == 0 || passmode == 1)) {
            const float VISTA_MINCONTRIB = 0.35f;
            float Pv[16], MVPv[16];
            mat_persp(g_fov, (float)W/H, znear, vista_far, Pv);
            mat_mul(Pv, V, MVPv);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPv);
            float vd = vista_far > 1.0f
                     ? sqrtf(-logf(VISTA_MINCONTRIB)) / vista_far : 0.0f;
            glUniform1f(rp.uFogDensity, vd);
            glUniform1f(rp.uVista, 1.0f);
            glUniform1f(rp.uVColor, 1.0f);   /* source vertex alpha is the fade */
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            GLuint vlast = (GLuint)-1;
            for (int k = 0; k < nvista; k++) {
                N2Batch *b = &vbatch[k];
                /* A backdrop is by definition BEHIND the ordinary world, and
                   whether it is depends on where its SURFACE is, not where its
                   bounding box is. A hollow panorama ring legitimately encloses
                   the camera while every one of its vertices stands kilometres
                   away, so the old AABB test refused an entire valid family.
                   Measure the real thing: the closest vertex of this mesh, in
                   3D, to the camera. Cheap enough to do per frame at this scale
                   (188 meshes / ~20k vertices), and it discards only geometry
                   that genuinely comes nearer than the ordinary horizon. */
                const N2Mesh *vm = &world.vista.meshes[vmesh[k]];
                /* A second measured rejection: a backdrop buried far below the
                   ground the player is standing on can only ever be seen from
                   beneath, and is never a horizon. The drawn set splits cleanly
                   into a surface cluster (top z +13 .. +1441 m) and a buried
                   cluster (top z -551 .. -895 m, the retail _Z duplicates); the
                   564 m gap between them is what this threshold sits in, with
                   over 250 m of margin on both sides at both audited poses. */
                /* diagnostic only (M132-R2): the below-world _Z duplicates are
                   still counted, but height relative to the player no longer
                   decides anything -- it was never safe for a high viewpoint. */
                { float vtop = -1e30f;
                  for (int q = 0; q < vm->nverts; q++)
                      if (vm->verts[q*5+2] > vtop) vtop = vm->verts[q*5+2];
                  if (vtop < carpos[2] - VISTA_BURIED_M) vistaburied++; }
                /* TRUE surface distance: closest point on any triangle of this
                   mesh to the camera. The vertex-only test this replaces could
                   not reject a sheet whose corners are far away but whose
                   middle sweeps the foreground. AABB and vertex distance are
                   kept only as the cheap pre-pass. */
                float near2 = 1e30f;
                { float vdx = cam[0] < b->bbox_min[0] ? b->bbox_min[0]-cam[0]
                            : (cam[0] > b->bbox_max[0] ? cam[0]-b->bbox_max[0] : 0);
                  float vdy = cam[1] < b->bbox_min[1] ? b->bbox_min[1]-cam[1]
                            : (cam[1] > b->bbox_max[1] ? cam[1]-b->bbox_max[1] : 0);
                  float vdz = cam[2] < b->bbox_min[2] ? b->bbox_min[2]-cam[2]
                            : (cam[2] > b->bbox_max[2] ? cam[2]-b->bbox_max[2] : 0);
                  float box2 = vdx*vdx+vdy*vdy+vdz*vdz;
                  if (box2 >= VIEW_DIST*VIEW_DIST) near2 = box2;   /* whole box is far */
                  else for (int t = 0; t + 2 < vm->nidx; t += 3) {
                      const float *A = vm->verts + vm->idx[t]*5;
                      const float *B = vm->verts + vm->idx[t+1]*5;
                      const float *C = vm->verts + vm->idx[t+2]*5;
                      float d2 = pt_tri_d2(cam, A, B, C);
                      if (d2 < near2) { near2 = d2; if (near2 < 1.0f) break; }
                  } }
                if (near2 < VIEW_DIST*VIEW_DIST) {
                    vistanear++; vistanearmesh += b->nmesh;
                    if (vistamind > sqrtf(near2)) vistamind = sqrtf(near2);
                    /* diagnostic escape hatch: OPENUG2_VISTA_NOCLIP=1 draws the
                       rejected batches so a family can be photographed and
                       attributed instead of merely counted. Never set in play. */
                    static int noclip = -1;   /* only reachable under --tier full */
                    if (noclip < 0) noclip = getenv("OPENUG2_VISTA_NOCLIP") ? 1 : 0;
                    if (!noclip) continue;
                }
                if (near2 < vistakeptmin*vistakeptmin) vistakeptmin = sqrtf(near2);
                if (vlist) {
                    float hi = -1e30f;
                    for (int q = 0; q < vm->nverts; q++)
                        if (vm->verts[q*5+2] > hi) hi = vm->verts[q*5+2];
                    printf("VDRAW %-28s tris %5d  nearest %8.1f m  top z %8.1f "
                           "(camera z %.1f)\n", vm->sname[0]?vm->sname:"?",
                           b->index_count/3, sqrtf(near2), hi, cam[2]);
                }
                if (b->tex != vlast) {
                    if (b->tex) { glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, b->tex); }
                    else { glUniform1f(uUseTex, 0.0f); glUniform3f(uColor, 0.30f, 0.32f, 0.38f); }
                    vlast = b->tex;
                }
                draw_batch(b);
                g_dbg.drawn++; vistadraws++; vistadrawn++; vistamesh += b->nmesh;
            }
            /* restore every piece of state the pass touched */
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glUniform1f(rp.uVista, 0.0f);
            glUniform1f(rp.uVColor, 0.0f);
            glUniform1f(rp.uFogDensity, g_dbg.fog_density);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
        }

        /* track: one draw per visible (cell,texture) batch, texture-sorted so
           binds are rare; terrain fallback + untextured-gray are baked into
           the batch key at build time.
           cull: XY distance only — the old per-mesh size cull is gone, a
           batch's tiny meshes are ~free once merged. ponytail: no frustum
           test — add one if the batch count becomes the bottleneck. */
        int ndrawn = 0;
        int wbdrawn = 0;   /* world batches only (g_dbg.drawn also counts car/HUD) */
        int vis_scen[8] = {0};
        int far_scen[8] = {0}, far_meshes = 0, far_batches = 0;
        /* M132 band census: every world batch by AABB distance, whether drawn
           or not, so the visibility policy can be judged against the source. */
        static const float BAND[6] = {250,700,1000,1500,2000,1e9f};
        static long band_b[6], band_m[6], band_sc[6][8];
        if (ra_f == 0) { memset(band_b,0,sizeof band_b); memset(band_m,0,sizeof band_m);
                         memset(band_sc,0,sizeof band_sc); }
        static int viskept[4096]; int nviskept = 0;   /* M78: opaque batches drawn */
        /* DRAW MODE PER BATCH, taken from its texture's record. Drawing
           everything opaque is what leaves foliage as solid rectangles and lit
           windows black: the record already says which is which. */
        static unsigned char *bmode = NULL; static int bmode_n = -1;
        if (bmode_n != nbatch) {
            bmode = (unsigned char *)realloc(bmode, (size_t)(nbatch > 0 ? nbatch : 1));
            for (int k = 0; k < nbatch; k++) {
                bmode[k] = 0;
                for (int q = 0; q < ntmap; q++)
                    if (tmapkey[q] == wbatch[k].texkey) { bmode[k] = tmapalpha[q]; break; }
            }
            bmode_n = nbatch;
        }
        static int blendlist[8192]; int nblend = 0;
        if (g_debug_mode == 0 || !dbgprog) {   /* --- default textured world pass --- */
        GLuint lasttex = (GLuint)-1;
        glUniform1f(rp.uVColor, g_dbg.vcolor);   /* apply source prelight to world geom */
        for (int k = 0; g_dbg.show_track && (passmode == 0 || passmode == 1) && k < nbatch; k++) {
            if (passbatch >= 0 && (k < passbatch || k > passbatch2)) continue;   /* M79 */
            N2Batch *b = &wbatch[k];
            float dx = cam[0] < b->bbox_min[0] ? b->bbox_min[0]-cam[0]
                     : (cam[0] > b->bbox_max[0] ? cam[0]-b->bbox_max[0] : 0);
            float dy = cam[1] < b->bbox_min[1] ? b->bbox_min[1]-cam[1]
                     : (cam[1] > b->bbox_max[1] ? cam[1]-b->bbox_max[1] : 0);
            { float dd = sqrtf(dx*dx+dy*dy); int bd = 0;
              while (bd < 5 && dd >= BAND[bd]) bd++;
              band_b[bd]++; band_m[bd] += b->nmesh;
              for (int sc=0;sc<8;sc++) band_sc[bd][sc] += b->scen_count[sc]; }
            if (dx*dx + dy*dy > VIEW_DIST*VIEW_DIST) {
                far_meshes += b->nmesh; far_batches++;
                for (int sc=0;sc<8;sc++) far_scen[sc] += b->scen_count[sc];
                continue;
            }
            if (bmode[k] >= N2_DRAW_BLEND) {      /* glass and neon: second pass */
                if (nblend < 8192) blendlist[nblend++] = k;
                continue;
            }
            ndrawn += b->nmesh;
            if (b->tex != lasttex) {
                if (b->tex) { glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, b->tex); }
                else { glUniform1f(uUseTex, 0.0f); glUniform3f(uColor, 0.28f, 0.29f, 0.31f); }
                /* alpha test only where the record says cutout -- foliage,
                   railings, frames. Applied to an opaque texture it punches
                   holes in the road. */
                glUniform1f(rp.uAlphaTest, bmode[k] == N2_DRAW_CUTOUT ? 1.0f : 0.0f);
                lasttex = b->tex;
            }
            draw_batch(b);
            for (int sc=0; sc<8; sc++) vis_scen[sc] += b->scen_count[sc];
            g_dbg.drawn++; wbdrawn++;
            if (nviskept < 4096) viskept[nviskept++] = k;
        }
        if (nrep) {   /* diagnostic overlay: where the restored geometry is */
            glUniform1f(uUseTex, 0.0f);
            glUniform1f(uUnlit, 1.0f);
            glUniform3f(uColor, 1.0f, 0.15f, 0.85f);
            glDepthMask(GL_FALSE);
            for (int k = 0; k < nrep; k++) { draw_batch(&rbatch[k]); g_dbg.drawn++; }
            glDepthMask(GL_TRUE);
            glUniform1f(uUnlit, 0.0f);
        }
        /* SECOND PASS: translucent geometry. These textures carry a draw order
           of 5..7 and do not write depth. Additive ones are the neon and the
           lit building windows -- what makes the night city glow; without this
           pass the facades stay black. */
        if (nblend) {
            glEnable(GL_BLEND); glDepthMask(GL_FALSE);
            glUniform1f(rp.uAlphaTest, 0.0f); glUniform1f(uUseTex, 1.0f);
            int lastmode = -1; lasttex = (GLuint)-1;
            for (int q = 0; q < nblend; q++) {
                N2Batch *b = &wbatch[blendlist[q]];
                int md = bmode[blendlist[q]];
                if (md != lastmode) {
                    glBlendFunc(GL_SRC_ALPHA, md == N2_DRAW_ADD ? GL_ONE
                                                                : GL_ONE_MINUS_SRC_ALPHA);
                    lastmode = md;
                }
                if (b->tex != lasttex) { glBindTexture(GL_TEXTURE_2D, b->tex); lasttex = b->tex; }
                draw_batch(b);
                ndrawn += b->nmesh; g_dbg.drawn++; wbdrawn++;
            }
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }
        glUniform1f(rp.uAlphaTest, 0.0f);
        glUniform1f(rp.uVColor, 0.0f);   /* off for everything else (cars carry no
                                            prelight; their attrib-3 default is black) */
        } else if (g_dbg.show_track) {   /* --- F3 debug view: prelight/normals/wire --- */
            /* draw every world batch with the debug shader (no distance cull),
               then restore the main program for the car/glow/HUD passes below. */
            render_world_map(wmbatch, (uint32_t)nbatch, dbgprog, MVP, g_debug_mode - 1);
            glUseProgram(rp.prog);
            glUniform1f(rp.uVColor, 0.0f);
            ndrawn += nbatch; g_dbg.drawn += nbatch; wbdrawn += nbatch;
        }

        /* Active race road closures. The navigation/collision barriers existed
           since Phase 71 but production rendered nothing at their coordinates;
           only the debug minimap showed a red tick, so a real collision looked
           like an invisible wall. Draw a lightweight neon barricade across each
           nearby closed road. Geometry comes from the exact WBarrier used by
           world_barrier_push -- no second placement rule. */
        if (world.mode == MODE_RACE_EVENT && world.nbar > 0 && race_state != 3) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit,1.0f); glUniform1f(uUseTex,0.0f); glUniform1f(uSoft,0.0f);
            glUniform3f(uColor, 1.0f, 0.10f, 0.03f); glUniform1f(uAlpha,0.78f);
            for (int bi=0; bi<world.nbar; bi++) {
                const WBarrier *b=&world.bar[bi];
                float qx=b->x-carpos[0], qy=b->y-carpos[1];
                if (qx*qx+qy*qy > 180.0f*180.0f) continue;
                float px=-b->dy, py=b->dx;
                float bz=world_ground_z(&scene,b->x,b->y,carpos[2]);
                /* two luminous cross-bars, 18 m wide */
                for (int h=0; h<2; h++) {
                    float z=bz+(h?1.05f:0.35f), H=0.18f, Wb=18.0f;
                    float M[16]={px*Wb,py*Wb,0,0, 0,0,H,0, 0,0,1,0,
                                 b->x-px*Wb*0.5f,b->y-py*Wb*0.5f,z,1};
                    float MV[16]; mat_mul(MVP,M,MV);
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,MV); draw_gpumesh(&quad); g_dbg.drawn++;
                }
                /* five posts make the closure readable from oblique angles */
                for (int p=-2; p<=2; p++) {
                    float off=p*4.0f, Wp=0.18f, Hp=1.45f;
                    float M[16]={px*Wp,py*Wp,0,0, 0,0,Hp,0, 0,0,1,0,
                                 b->x+px*off-px*Wp*0.5f,
                                 b->y+py*off-py*Wp*0.5f,bz,1};
                    float MV[16]; mat_mul(MVP,M,MV);
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,MV); draw_gpumesh(&quad); g_dbg.drawn++;
                }
            }
            glUniform1f(uAlpha,1.0f); glUniform1f(uUnlit,0.0f);
            glUniformMatrix4fv(uMVP,1,GL_FALSE,MVP);
            glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* car shadows: a soft dark blob on the ground under each car, so they
           sit on the road instead of floating (darkens, so it reads on any
           surface unlike the additive glows). */
        if (ncar) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit,1.0f); glUniform1f(uUseTex,0.0f); glUniform1f(uSoft,1.0f);
            glUniform3f(uColor, 0.0f, 0.0f, 0.0f); glUniform1f(uAlpha, 0.5f);
            for (int c=0; c<=nai; c++) {
                float *cp = c==0 ? carpos : ais[c-1].pos;
                float gz2 = world_ground_z(&scene, cp[0], cp[1], cp[2]) + 0.03f;
                /* footprint from the loaded body AABB rather than a fixed
                   square, so a Miata and a Hummer do not cast the same blob.
                   Falls back to the old 3.2 if no car geometry is loaded. */
                float sl = carbb[3]-carbb[0], sw = carbb[4]-carbb[1];
                float sx = sl > 0.5f ? sl*1.10f : 3.2f;
                float sy = sw > 0.5f ? sw*1.35f : 3.2f;
                float M[16]={sx,0,0,0, 0,sy,0,0, 0,0,1,0, cp[0]-sx*0.5f, cp[1]-sy*0.5f, gz2, 1};
                float MV[16]; mat_mul(MVP,M,MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV); draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            glUniform1f(uAlpha,1.0f); glUniform1f(uSoft,0.0f);
            glDepthMask(GL_TRUE); glDisable(GL_BLEND);

            /* neon underglow: an additive coloured pool on the asphalt under
               the chassis. Additive (not alpha) so it reads as emitted light
               on dark tarmac rather than paint, uSoft so it falls off round
               the edges, and sized from the same body AABB the shadow uses so
               it tracks the car's real footprint. Drawn after the shadow so it
               lights the ground the shadow just darkened. */
            if (g_dbg.neon_on && g_dbg.neon_str > 0.001f) {
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                glDepthMask(GL_FALSE);
                glUniform1f(uUnlit,1.0f); glUniform1f(uUseTex,0.0f); glUniform1f(uSoft,1.0f);
                glUniform3f(uColor, g_dbg.neon_col[0], g_dbg.neon_col[1], g_dbg.neon_col[2]);
                float nl = carbb[3]-carbb[0], nw = carbb[4]-carbb[1];
                float nx = nl > 0.5f ? nl*1.45f : 4.2f, ny = nw > 0.5f ? nw*1.9f : 4.2f;
                float nz = world_ground_z(&scene, carpos[0], carpos[1], carpos[2]) + 0.02f;
                float M[16]={nx,0,0,0, 0,ny,0,0, 0,0,1,0,
                             carpos[0]-nx*0.5f, carpos[1]-ny*0.5f, nz, 1};
                float MV[16]; mat_mul(MVP,M,MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV);
                glUniform1f(uAlpha, g_dbg.neon_str);
                draw_gpumesh(&quad); g_dbg.drawn++;
                glUniform1f(uAlpha,1.0f); glUniform1f(uSoft,0.0f);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MVP);
                glDepthMask(GL_TRUE); glDisable(GL_BLEND);
            }
        }

        /* headlights: warm soft light-pools cast on the road ahead (it's night),
           three overlapping ground glows that widen + fade with distance to read
           as a headlight cone. Additive blend. Night only. */
        /* Likewise the headlight ground pools: three additive quads laid on
           the road at fixed distances ahead. Same objection -- the shape came
           from constants, not from the car or its lamps. */


        /* tyre skid marks: flat dark quads on the ground, alpha-blended */
        if (skidn > 0) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f);
            glUniform3f(uColor, 0.04f, 0.04f, 0.05f);
            for (int i = 0; i < skidn; i++) {
                float *s = skid[i];                          /* ax,ay,az, bx,by,bz, life */
                if (race_state != 3) s[6] -= 0.00028f;       /* ~60 s to fade (frozen on menu) */
                if (s[6] <= 0.0f) continue;
                float dx=s[3]-s[0], dy=s[4]-s[1];            /* segment direction */
                float len2=sqrtf(dx*dx+dy*dy); if(len2<1e-4f) continue;
                glUniform1f(uAlpha, 0.4f*s[6]);              /* fade with age */
                float ux=dx, uy=dy;                          /* along-travel axis (u) */
                const float SW=0.45f; float px=-dy/len2*SW, py=dx/len2*SW;  /* width axis (v) */
                /* unit quad (u,v) -> A + u*(B-A) + v*perp*W, centred across width */
                float M[16]={ ux,uy,0,0,  px,py,0,0,  0,0,1,0,
                              s[0]-px*0.5f, s[1]-py*0.5f, s[2], 1 };
                float MV[16]; mat_mul(MVP, M, MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV); draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            glUniform1f(uAlpha, 1.0f); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* car: solid-shaded, positioned + banked to the road (up = car_up) */
        float player_body_model[16]; int player_body_model_valid = 0;
        if (ncar) {
            float Model[16], MVPc[16], WheelModel[16], MVPwheel[16];
            /* Wheel contact ride is this car's own measured tyre radius. Body
               ride may sit slightly lower for a per-car stock suspension tune;
               both use the exact gameplay contact with no render-only Z lag. */
            float ride = car_body_ride;   /* body collision envelope uses this ride */
            float bodypos[3] = { carpos[0], carpos[1], carpos[2] };
            mat_car(bodypos, heading, car_up, ride, Model);
            memcpy(player_body_model, Model, sizeof player_body_model);
            player_body_model_valid = 1;
            mat_mul(MVP, Model, MVPc);
            /* Wheels never inherit the body spring/drop: their hub stays one
               scaled tyre radius above the exact gameplay contact. */
            mat_car(carpos, heading, car_up, car_ride, WheelModel);
            mat_mul(MVP, WheelModel, MVPwheel);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPc);
            /* normals stay model-space, so counter-rotate the sun into car
               space — otherwise the lit side turns with the car's heading.
               The camera goes into model space the same way, for the
               shader's reflection vector. */
            { float ch=cosf(heading), sh=sinf(heading);
              float dx=cam[0]-bodypos[0], dy=cam[1]-bodypos[1], dz=cam[2]-(bodypos[2]+ride);
              glUniform3f(uLight, ch*N2_SUN_X + sh*N2_SUN_Y,
                                 -sh*N2_SUN_X + ch*N2_SUN_Y, N2_SUN_Z);
              glUniform3f(rp.uCamPos, ch*dx + sh*dy, -sh*dx + ch*dy, dz); }
            /* rebuild the 4 wheel placements from the (live-tunable) fractions;
               the discs spin with road speed (visible via the radial rim tex) */
            { static float wang = 0.0f;
              /* rolling radius = this car's profile radius times the user scale,
                 so w=v/r and the RPM readout are correct per vehicle (a Hummer
                 rolls slower than a compact at the same speed). */
              float wR = carprof.wheel_r
                       * (g_dbg.wheel_scale > 0.05f ? g_dbg.wheel_scale : 1.0f);
              /* w = v/r per tick: speed is m/tick and this runs once per tick, so
                 the angle step is exactly speed/wR rad (the earlier *1/60 made the
                 tread crawl ~60x too slow -- ~3 rad/s instead of ~205 at 220 km/h). */
              static float vsteer = 0.0f;
              /* Visual steer follows the same filtered input as the physics and
                 eases it once more toward a ~28 degree wheel lock. Rotation is
                 about the wheel's own centre (the mesh is modelled at the origin and
                 the arch position lives in the translation column), so steering and
                 spin can't swing it out of the arch. Demo mode drives both from a
                 clock (free spin + sine steer) so the matrices can be verified with
                 the car parked. */
              if (g_dbg.wheel_demo) {
                  static float dt = 0.0f; dt += 0.03f;
                  wang += 0.18f;                                   /* free continuous spin */
                  vsteer += (sinf(dt) * 0.5f - vsteer) * 0.25f;    /* +/-0.5 rad oscillation */
              } else {
                  wang += speed / wR;
                  vsteer += (steer_filtered*0.50f - vsteer) * 0.25f;
              }
              wang = fmodf(wang, 6.2831853f);
              float c = cosf(wang), sn = sinf(wang);
              float sc = cosf(vsteer), ss = sinf(vsteer);
              /* telemetry: wheel RPM from w=v/r (rad/s -> RPM) and steer angle */
              g_dbg.wheel_rpm = fabsf(speed / wR) * 60.0f * 9.5493f;
              g_dbg.steer_deg = vsteer * 57.2958f;
              g_dbg.wheel_radius = wR;
              /* Explicit per-car stance (g_dbg.wheel): absolute axle X, per-axle
                 track and hub height, set at load from the table/fallback and
                 live-edited by the ImGui stance sliders. No fraction math here. */
              float fht = g_dbg.wheel.front_track*0.5f, rht = g_dbg.wheel.rear_track*0.5f,
                    s=g_dbg.wheel_scale, wz=g_dbg.wheel.ride_y;
              float wp[4][2]={{g_dbg.wheel.front_axle, fht},{g_dbg.wheel.front_axle,-fht},
                              {g_dbg.wheel.rear_axle,  rht},{g_dbg.wheel.rear_axle, -rht}};
              for(int k=0;k<4;k++){ float sy=(k&1)?-s:s;
                  /* M130: each wheel carries its OWN suspension travel, so a
                     grounded wheel stays on its contact while the sprung body
                     heaves, pitches and rolls above it. A single shared height
                     would float or sink tyres on every bump. */
                  float wzk = wz + (g_ride_ready && !sstatic ? g_ride.compression[k] : 0.0f);
                  /* rear axle: plain scale * rotY(wang) */
                  float M[16]={s*c,0,-s*sn,0, 0,sy,0,0, s*sn,0,s*c,0,
                               wp[k][0],wp[k][1],wzk,1};
                  { float Mai[16]; memcpy(Mai,M,sizeof Mai); Mai[14]=wz;
                    memcpy(wheelTAI[k],Mai,sizeof Mai); }  /* AI: no player travel */
                  if (k < 2) {   /* front axle: rotZ(steer) * that, per column */
                      float m2[16]={ s*c*sc,  s*c*ss,  -s*sn, 0,
                                     -sy*ss,  sy*sc,    0,    0,
                                     s*sn*sc, s*sn*ss,  s*c,  0,
                                     wp[k][0], wp[k][1], wzk, 1 };
                      memcpy(M, m2, sizeof M);
                  }
                  memcpy(wheelT[k],M,sizeof(M)); } }
            /* Car reflections come from the cube rendered around the car
               itself. uEnvYaw turns the reflected ray from car axes into world
               axes: normals and positions here are model-space, the map is
               world-space. */
            if (g_envcube_ready) {
                glUniform1f(rp.uEnvCubeOn, 1.0f);
                glUniform1f(rp.uEnvYaw, heading);
            }
            /* uUnlit/uSoft are left ON (1.0) by the shadow, headlight-glow
               and skid-mark billboards drawn just above -- all three
               deliberately use the unlit flat-colour path for those quads.
               Without resetting them here every car mesh below hits the
               shader's unlit early-return and skips texture sampling, decal
               blending, specular AND environment mapping entirely. One leaked
               uniform is enough to keep every lit-path feature off the screen:
               gloss, badges and reflections all vanish at once, which looks
               exactly like "the lighting switched off". */
            glUniform1f(uUnlit, 0.0f); glUniform1f(uSoft, 0.0f);
            float *pnt = g_dbg.paint_override ? g_dbg.paint : paint;
            /* uUnlit/uSoft were left ON (1.0) by the shadow/headlight-glow/
               skid-mark billboards drawn just above (all three intentionally
               use the unlit flat-colour shader path for those FX quads) —
               without this reset every car mesh below hits the shader's
               unlit early-return and skips texture sampling, decal blending,
               specular AND environment mapping entirely. This one leaked
               uniform is the actual root cause behind "the potato car" seen
               since Phase 6: none of the lit-path work (gloss, badges,
               reflections) was ever reaching the screen. */
            glUniform1f(uUnlit, 0.0f); glUniform1f(uSoft, 0.0f);
            /* per-mesh: each part wears its own bound texture (body/wheel/...);
               parts with no in-TPK texture get a sensible flat colour by class. */
            for (int i = 0; i < ncar; i++) {
                int c = cgm[i].cat;
                int is_light = (c==N2_CAR_LIGHT || c==N2_CAR_BRAKELIGHT);
                if ((c==N2_CAR_BODY && !g_dbg.show_body) ||
                    (is_light       && !g_dbg.show_lights)|| (c==N2_CAR_TIRE && !g_dbg.show_tires) ||
                    ((c==N2_CAR_MISC||c==N2_CAR_MECH) && !g_dbg.show_misc)) continue;
                if (c == N2_CAR_GLASS) continue;   /* translucent: blended pass below */
                /* Emissive lenses: light parts carry no diffuse texture (verified),
                   so the unlit path (uColor out, no shadow darkening) IS the
                   emissive channel -- uColor = light colour, its magnitude = the
                   emissive intensity. Headlights glow at night; tail/brake lenses
                   glow red (brighter on the brakes). Everything else stays lit. */
                glUniform1f(uUnlit, (is_light && g_dbg.night_mode) ? 1.0f : 0.0f);
                /* plastic trim (bumpers/skirts, real per-part name tokens —
                   see n2_car_is_trim): duller and broader than the metallic
                   paint around it, so it doesn't read as the same "sticker"
                   material as the door/hood/fender panels. */
                float specv = (c==N2_CAR_BODY||c==N2_CAR_MISC)?g_dbg.body_spec
                            : is_light?0.45f : c==N2_CAR_MECH?0.05f : 0.0f;
                if (cgm[i].trim) specv *= 0.4f;
                float glossv = cgm[i].trim ? 6.0f : 20.0f;
                float envv = (c==N2_CAR_BODY||c==N2_CAR_MISC)?0.50f*g_dbg.body_env
                           : is_light?0.55f : c==N2_CAR_MECH?0.0f : 0.15f;

                /* MATERIAL FROM THE DATA. When a submesh names a material, its
                   diffuse, specular and reflection come from that record rather
                   than from per-class constants: chrome has no diffuse and a
                   1.14 reflection, metallic paint 1.12 specular with 0.372
                   reflection, rubber 0.248 specular and no reflection at all.
                   The curves go to the shader unchanged -- it evaluates
                   Min + dot(V,N)*Range. Averaging them into one number and
                   adding hand-made edge darkening instead is what makes paint
                   read flat. The exponent multiplier of 6 is our own scale fit:
                   the stored exponent belongs to a different lighting model and
                   used directly gives a highlight the size of a body panel. */
                const N2LightMat *lm = (nlmat && cgm[i].matkey)
                                     ? n2_find_lightmat(lmat, nlmat, cgm[i].matkey) : NULL;
                if (lm) {
                    float dr[3], se[4];
                    for (int q = 0; q < 3; q++) dr[q] = lm->dif_max[q] - lm->dif_min[q];
                    se[0] = (lm->spec_min[0]+lm->spec_min[1]+lm->spec_min[2]) / 3.0f;
                    se[1] = (lm->spec_max[0]+lm->spec_max[1]+lm->spec_max[2]) / 3.0f - se[0];
                    se[2] = (lm->env_min[0] +lm->env_min[1] +lm->env_min[2])  / 3.0f;
                    se[3] = (lm->env_max[0] +lm->env_max[1] +lm->env_max[2])  / 3.0f - se[2];
                    glUniform1f(rp.uMatOn, 1.0f);
                    glUniform3fv(rp.uMatDifMin, 1, lm->dif_min);
                    glUniform3fv(rp.uMatDifRange, 1, dr);
                    glUniform4fv(rp.uMatSE, 1, se);
                    specv = c==N2_CAR_BODY ? g_dbg.body_spec * 1.2f : 0.5f;
                    envv  = c==N2_CAR_BODY ? g_dbg.body_env  * 2.0f : 1.5f;
                    if (lm->spec_pow > 0.01f) glossv = 6.0f * lm->spec_pow;
                } else {
                    glUniform1f(rp.uMatOn, 0.0f);
                }
                glUniform1f(uSpec, specv);
                glUniform1f(uGloss, glossv);
                /* no diffuse texture exists for any light part (verified
                   exhaustively against the data, see n2_car_category) — chrome
                   housing + coloured lens read entirely through reflection.
                   Mechanical compartment parts (engine/exhaust) are unpainted
                   metal/plastic when they have no texture of their own — no
                   body-paint gloss or reflection either. */
                glUniform1f(rp.uEnv, envv);
                glUniform1f(rp.uDecal, 0.0f);   /* body branch may re-enable */
                GLuint tex = 0; int hasalpha = 0;
                for (int j = 0; j < nmap; j++) if (mapkey[j]==cgm[i].texkey) {
                    tex = maptex[j]; hasalpha = mapalpha[j]; break; }
                /* SHARED LAMP TEXTURE, bound by name. The key these parts
                   carry resolves nowhere, and is not meant to.
                   It belongs to the LENS, and only the lens is unwrapped for
                   it. A light part also contains its housing and the body
                   slice around it, which carry the same class but ordinary
                   body UVs -- putting the lamp texture on those samples it at
                   meaningless coordinates and the panel comes out dark. So it
                   is bound only where the material says glass; everything else
                   takes the flat emissive colour.
                   The sheets are greyscale masks -- BRAKE_GLOBAL averages
                   60/60/59 -- so they carry the SHAPE of the lens, not its
                   colour: the emissive colour has to survive and the texture
                   multiplies it. */
                int lens = (cgm[i].matkey == n2_str_hash("HEADLIGHTGLASS") ||
                            cgm[i].matkey == n2_str_hash("BRAKELIGHTGLASS"));
                if (!tex && lens && c == N2_CAR_LIGHT)      tex = tex_headlights;
                if (!tex && lens && c == N2_CAR_BRAKELIGHT) tex = tex_brake;
                if (c == N2_CAR_BODY || c == N2_CAR_MISC) {
                    /* glossy paint; a mesh that references the badge/vinyl
                       atlas in its OWN 0x134012 slot list (a real per-mesh
                       data reference, e.g. GOLF's BASE_A) still decal-blends
                       it — that's verified correct (renders the actual VW
                       roundel). Panels with NO texture reference of their
                       own render as pure metallic paint, full stop: no
                       vinyl/badge fallback. (Removed: substituting the
                       shared composite onto any texture-less panel "because
                       it probably shares the same UV sheet" — false for
                       most of them; on the Miata almost every body/misc
                       mesh has no texkey at all, so the fallback painted
                       the composite's stretched hook-shape/checker pattern
                       across large panels like the engine bay, reading as
                       a solid mismatched block. TODO: the data actually
                       supports per-submesh materials via the 0x134B02
                       submesh table (mat_id -> its own 0x134011/0x134012) —
                       n2_walk_car currently assigns ONE texkey per whole
                       mesh object from the first material found. Modeling
                       submesh-level materials would let genuinely-textured
                       sub-regions (if any exist) resolve correctly instead
                       of an all-or-nothing per-object key. Not implemented. */
                    /* Roof panels are ordinary painted body: the data carries
                       no soft-top marker (M111), so they take the same paint and
                       their own texture like every other body mesh. */
                    glUniform3f(uColor, pnt[0], pnt[1], pnt[2]);
                    if (tex && !hasalpha) {
                        glUniform1f(uUseTex, 1.0f);
                        glBindTexture(GL_TEXTURE_2D, tex);
                    } else {
                        if (tex) {
                            glUniform1f(uUseTex, 1.0f); glUniform1f(rp.uDecal, 1.0f);
                            glBindTexture(GL_TEXTURE_2D, tex);
                        } else glUniform1f(uUseTex, 0.0f);
                    }
                } else {
                    /* Emissive colour FIRST, texture second: the shared lamp
                       textures are greyscale masks (BRAKE_GLOBAL averages
                       60/60/59), carrying the shape of the lens, not its
                       colour. The old order bound the mask and left uColor at
                       whatever the previous mesh set -- a red tail lamp came
                       out grey. And a mask averaging a quarter of full scale
                       dims the lens four-fold, so a textured lens gets the
                       colour scaled back up to read at the same brightness as
                       a plain one. */
                    int braking = (throttle < -0.1f && speed > 0.01f) || handbrake;
                    if      (c == N2_CAR_LIGHT)      glUniform3f(uColor, 1.0f, 0.94f, 0.78f);
                    else if (c == N2_CAR_BRAKELIGHT) {
                        if (braking) glUniform3f(uColor, 1.0f, 0.16f, 0.10f);
                        else         glUniform3f(uColor, 0.62f, 0.05f, 0.04f);
                    }
                    else if (c == N2_CAR_TIRE)       glUniform3f(uColor, 0.05f, 0.05f, 0.06f);
                    else if (c == N2_CAR_MECH)       glUniform3f(uColor, 0.05f, 0.05f, 0.05f);  /* unpainted metal/plastic */
                    else if (!tex)                   glUniform3f(uColor, pnt[0], pnt[1], pnt[2]);

                    if (tex) {
                        if (is_light) {
                            float bo = 3.5f;
                            if (c == N2_CAR_LIGHT)  glUniform3f(uColor, 1.0f*bo, 0.94f*bo, 0.78f*bo);
                            else if (braking)       glUniform3f(uColor, 1.0f*bo, 0.16f*bo, 0.10f*bo);
                            else                    glUniform3f(uColor, 0.62f*bo, 0.05f*bo, 0.04f*bo);
                        }
                        glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, tex);
                    } else glUniform1f(uUseTex, 0.0f);
                }
#ifdef DEBUG_UI
                /* Mesh Inspector overlay: purely a draw-state override on the
                   selected mesh -- no asset, parser or transform is touched. */
                int insp_on = (g_dbg.insp_sel == i);
                if (insp_on && g_dbg.insp_highlight) {
                    glUniform1f(uUseTex, 0.0f); glUniform1f(rp.uDecal, 0.0f);
                    glUniform1f(uSpec, 0.0f); glUniform1f(rp.uEnv, 0.0f);
                    glUniform1f(uUnlit, 1.0f);
                    glUniform3f(uColor, 1.0f, 0.08f, 0.85f);   /* neon magenta */
                }
                if (insp_on && g_dbg.insp_wire)
                    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                if (insp_on && g_dbg.insp_flipn) glUniform1f(rp.uFlipN, 1.0f);
                if (insp_on && g_dbg.insp_cull) {
                    /* culling is OFF engine-wide, so glFrontFace alone would be
                       inert; enable it here so winding actually has an effect
                       and the inversion question becomes testable. */
                    glEnable(GL_CULL_FACE);
                    glCullFace(g_dbg.insp_cull == 1 ? GL_BACK : GL_FRONT);
                }
#endif
                if (c != N2_CAR_TIRE) { draw_gpumesh(&cgm[i]); g_dbg.drawn++; }   /* tyres = procedural, below */
#ifdef DEBUG_UI
                if (insp_on && g_dbg.insp_wire) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                if (insp_on && g_dbg.insp_highlight) glUniform1f(uUnlit, 0.0f);
                if (insp_on && g_dbg.insp_flipn) glUniform1f(rp.uFlipN, 0.0f);
                if (insp_on && g_dbg.insp_cull) glDisable(GL_CULL_FACE);
#endif
            }
            glUniform1f(uUnlit, 0.0f);   /* emissive lenses left it on; glass/wheels below are lit */
            /* glass pass: translucent tint, blended over the finished body,
               depth-write off (no self-occlusion), spec kept by the shader's
               uAlpha output. State restored before anything else draws. */
            if (g_dbg.show_glass) {
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#ifdef DEBUG_UI
                glDepthMask(g_dbg.insp_glass_depth ? GL_TRUE : GL_FALSE);
#else
                glDepthMask(GL_FALSE);
#endif
                glUniform1f(rp.uDecal, 0.0f); glUniform1f(uUseTex, 0.0f);
                glUniform1f(uSpec, 0.6f); glUniform1f(uGloss, 20.0f); glUniform1f(uAlpha, 0.55f);
                glUniform1f(rp.uEnv, 0.8f);   /* glass reflects hardest */
                glUniform3f(uColor, 0.10f, 0.13f, 0.17f);
                for (int i = 0; i < ncar; i++)
                    if (cgm[i].cat == N2_CAR_GLASS) {
#ifdef DEBUG_UI
                        /* the opaque loop skips glass, so the inspector overlay
                           has to be applied here too or selecting a window did
                           nothing at all. */
                        int gi = (g_dbg.insp_sel == i);
                        if (gi && g_dbg.insp_highlight) {
                            glUniform1f(uUnlit, 1.0f); glUniform1f(uAlpha, 1.0f);
                            glUniform3f(uColor, 1.0f, 0.08f, 0.85f);
                        }
                        if (gi && g_dbg.insp_wire) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
#endif
                        draw_gpumesh(&cgm[i]); g_dbg.drawn++;
#ifdef DEBUG_UI
                        if (gi && g_dbg.insp_wire) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                        if (gi && g_dbg.insp_highlight) {
                            glUniform1f(uUnlit, 0.0f); glUniform1f(uAlpha, 0.55f);
                            glUniform3f(uColor, 0.10f, 0.13f, 0.17f);
                        }
#endif
                    }
                glUniform1f(uAlpha, 1.0f);
                glDepthMask(GL_TRUE); glDisable(GL_BLEND);
            }
            /* Headlight/taillight bloom: a soft camera-facing additive halo over
               each lens cluster -- night-time light diffusion the flat lens mesh
               can't give on its own. Front clusters glow warm, rear red. Blend is
               SRC_ALPHA/ONE (additive but alpha-shaped by uSoft's radial falloff,
               so the halo has a soft edge; plain ONE/ONE would be a hard disc).
               Depth-tested (occluded by nearer body) but no depth write; nudged
               slightly toward the camera so it sits over its own lens. */
            /* The lens-bloom discs that used to sit here are gone. They were
               drawn at cluster centroids derived from the geometry, which put
               a soft disc in the middle of each light unit -- over the glass
               rather than in it. The lens now carries its own emissive colour
               and the shared lamp texture, so it lights up as a shape instead
               of wearing a sticker. */

            /* procedural tyres at the 4 arches (the game rims render as urchins);
               the radial rim texture gives them a hub + spokes instead of a void */
            if (have_wheel && g_dbg.show_tires) {
                glUniform1f(rp.uDecal, 0.0f);
                /* Authentic stock wheel: the car's OWN FRONT_WHEEL mesh (rim +
                   tyre) from its GEOMETRY.BIN, with the flat backing-plane quad
                   culled at load, instanced at all four AttribSys corners. This
                   is the real factory wheel, so it wins over the shared rim
                   library below whenever the car ships one (all but the 2 tyre-
                   less cars). At blur speed the procedural disc still takes over. */
                if (stock_wheel >= 0 && PHYS_KMH(speed) <= WHEEL_BLUR_KMH) {
                    GLuint stex=0; for(int j=0;j<nmap;j++) if(mapkey[j]==cgm[stock_wheel].texkey){stex=maptex[j];break;}
                    glUniform1f(uUseTex, stex?1.0f:0.0f); glUniform1f(rp.uEnv, 0.25f);
                    glUniform1f(uSpec, 0.5f); glUniform3f(uColor,0.6f,0.6f,0.62f);
                    if (stex) glBindTexture(GL_TEXTURE_2D, stex);
                    for (int k=0;k<4;k++){ float MVPw[16]; mat_mul(MVPwheel, wheelT[k], MVPw);
                        glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPw); draw_gpumesh(&cgm[stock_wheel]); }
                    g_dbg.drawn+=4; glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPc);
                    goto wheels_drawn;
                }
                /* Geometric rim below the blur threshold, else the procedural
                   disc (its angular-averaged sheet sells the motion blur).
                   The WHEELS/TEXTURES.BIN rim diffuse is AUTHENTIC (blue spans
                   the full 0..255; the low average is just a gold/bronze rim,
                   not a decode bug), so it is bound directly and shows the real
                   manufacturer colour. uEnv=0 keeps the warm env sphere off it
                   (that, not the texture, was the old green cast) and a strong
                   specular adds the chrome highlight over the diffuse.
                   uColor is ignored on the textured path (base = t.rgb).
                   PHYS_KMH(speed), not g_dbg.kmh, which lags a frame here. */
                int geo = nwheelgm > 0 && PHYS_KMH(speed) <= WHEEL_BLUR_KMH;
                if (geo) {
                    /* Rim paint: bind the authentic OEM diffuse and recolor it
                       toward the chosen rim colour (silver default) via uRimTint,
                       keeping the spoke detail. rim_paint=0 shows the raw gold. */
                    glUniform1f(uUseTex, rimtex ? 1.0f : 0.0f);
                    glUniform1f(rp.uEnv, 0.0f); glUniform1f(uSpec, 0.85f);
                    glUniform3fv(uColor, 1, g_dbg.rim_color);
                    glUniform1f(rp.uRimTint, rimtex && g_dbg.rim_paint ? 1.0f : 0.0f);
                    if (rimtex) glBindTexture(GL_TEXTURE_2D, rimtex);
                } else {
                    glUniform1f(uUseTex, 1.0f); glUniform1f(rp.uEnv, 0.3f);
                    glUniform1f(uSpec, 0.4f);
                    glBindTexture(GL_TEXTURE_2D,
                        PHYS_KMH(speed) > WHEEL_BLUR_KMH ? texWheelBlur : texWheel);
                }
                for (int k=0;k<4;k++){ float MVPw[16]; mat_mul(MVPwheel, wheelT[k], MVPw);
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPw);
                    draw_gpumesh(geo ? &wheelgm[0] : &wheelmesh); }
                g_dbg.drawn += 4;
                glUniform1f(rp.uRimTint, 0.0f);   /* rim paint is rim-only */
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPc);
                wheels_drawn: ;
            }
            glUniform1f(uUseTex, 0.0f);
            glUniform1f(uSpec, 0.3f);     /* AIs: flat colour but glossy paint */
            glUniform1f(rp.uEnv, 0.35f);
            /* AI opponents — same body, each in its own colour */
            for (int k = 0; k < nai; k++) {
                float aup[3], aiz=ais[k].pos[2];
                float AIWheelModel[16], AIWheelMVP[16];
                world_ground_pose(&scene,ais[k].pos[0],ais[k].pos[1],ais[k].pos[2],&aiz,aup);
                mat_car(ais[k].pos, ais[k].head, aup, ride, Model);   /* AI: same per-car ride */
                mat_mul(MVP, Model, MVPc);
                mat_car(ais[k].pos, ais[k].head, aup, car_ride, AIWheelModel);
                mat_mul(MVP, AIWheelModel, AIWheelMVP);
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVPc);
                { float ch=cosf(ais[k].head), sh=sinf(ais[k].head);
                  float dx=cam[0]-ais[k].pos[0], dy=cam[1]-ais[k].pos[1];
                  float dz=cam[2]-(ais[k].pos[2]+ride);
                  glUniform3f(uLight, ch*N2_SUN_X + sh*N2_SUN_Y,
                                     -sh*N2_SUN_X + ch*N2_SUN_Y, N2_SUN_Z);
                  glUniform3f(rp.uCamPos, ch*dx + sh*dy, -sh*dx + ch*dy, dz); }
                glUniform3f(uColor, ais[k].col[0], ais[k].col[1], ais[k].col[2]);
                for (int i = 0; i < ncar; i++)
                    if (cgm[i].cat != N2_CAR_TIRE) { draw_gpumesh(&cgm[i]); g_dbg.drawn++; }
                if (have_wheel && g_dbg.show_tires) {     /* procedural tyres */
                    glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, texWheel);
                    for (int w=0;w<4;w++){ float MVPw[16]; mat_mul(AIWheelMVP, wheelTAI[w], MVPw);
                        glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPw); draw_gpumesh(&wheelmesh); }
                    g_dbg.drawn += 4;
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,MVPc);
                    glUniform1f(uUseTex, 0.0f);
                    glUniform3f(uColor, ais[k].col[0], ais[k].col[1], ais[k].col[2]);
                }
            }
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
            glUniform3f(uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);   /* back to world */
            glUniform1f(rp.uEnv, 0.0f);   /* reflections are cars-only */
            glUniform1f(rp.uEnvCubeOn, 0.0f);
        }
        /* And once more OUTSIDE that block. The restore above only runs when a
           car was drawn, and which passes run at all depends on the mode --
           which is how the world lights ended up visible in one camera mode and
           missing in the other, twice, in opposite directions. */
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
        glUniform3f(uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);

        /* tail lights: red camera-facing glows at each car's rear (night) */
        /* The painted-on tail-light glows are gone. They were two additive
           discs placed by fixed offsets from the car's centre -- 1.9 m back,
           0.6 m out, 0.5 m up -- which lands on a low coupe's lamps and misses
           on anything taller: on the Hummer they sat under the bumper. The
           lens lights itself through its material and the shared lamp texture;
           real light sources for the car belong here later, placed from the
           data rather than from guessed offsets. */


        /* drift smoke: camera-facing billboards, light + fading, additive-ish */
        if (smoken > 0) {
            float lz = sqrtf(look[0]*look[0]+look[1]*look[1]+look[2]*look[2]); if(lz<1e-4f)lz=1;
            float ld[3]={look[0]/lz,look[1]/lz,look[2]/lz};
            float rt[3];                                   /* look x up(0,0,1) */
            rt[0]=ld[1]; rt[1]=-ld[0]; rt[2]=0;
            float rl=sqrtf(rt[0]*rt[0]+rt[1]*rt[1]); if(rl<1e-4f)rl=1; rt[0]/=rl;rt[1]/=rl;
            float up[3]={rt[1]*ld[2]-0, 0-rt[0]*ld[2], rt[0]*ld[1]-rt[1]*ld[0]}; /* right x look */
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f); glUniform1f(uSoft, 1.0f);
            glUniform3f(uColor, 0.78f, 0.78f, 0.80f);
            for (int i = 0; i < smoken; i++) {
                float s = smoke[i].size, *p = smoke[i].p;
                float cxp = p[0]-(rt[0]+up[0])*s*0.5f, cyp = p[1]-(rt[1]+up[1])*s*0.5f,
                      czp = p[2]-(rt[2]+up[2])*s*0.5f;
                float M[16]={ rt[0]*s,rt[1]*s,rt[2]*s,0,  up[0]*s,up[1]*s,up[2]*s,0,
                              0,0,1,0,  cxp,cyp,czp,1 };
                float MV[16]; mat_mul(MVP, M, MV);
                glUniformMatrix4fv(uMVP,1,GL_FALSE,MV);
                glUniform1f(uAlpha, smoke[i].life*smoke[i].life*0.22f);
                draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            glUniform1f(uAlpha,1.0f); glUniform1f(uSoft,0.0f); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* neon signs / bulbs / lens flares: additive pass at the very end of
           the 3D frame, using each mesh's own texture as its emissive colour
           (these never receive diffuse lighting in-game — they ARE the
           light). Depth test stays on (a sign behind a building must still
           be occluded); only the write is off, so overlapping glows blend
           into each other instead of fighting on depth. */
        if (nglow && g_dbg.show_track && (passmode == 0 || passmode == 2)) {   /* M78 */
            /* BACK TO WORLD SPACE FIRST. Everything between the car and this
               pass draws billboards -- tail lights, headlight pools, tyre
               smoke, skid marks -- and each sets its own matrix; the last one
               does not put the world matrix back. Without this the whole glow
               pass is drawn in the coordinates of a quad stuck to the car,
               i.e. off screen. And since those billboards only run while
               DRIVING, the lights are there in the menu orbit and gone the
               moment the chase camera starts. */
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
            glUniform1f(uUnlit, 1.0f);
            GLuint lastglowtex = (GLuint)-1;
            for (int k = 0; k < nglow; k++) {
                N2Batch *b = &glowbatch[k];
                if (b->tex != lastglowtex) {
                    /* White, because the unlit path MULTIPLIES the texture by
                       uColor: left as it was, the glow inherits whatever the
                       last pass set -- the tail lights leave it red, and every
                       street lamp and lit window comes out orange. */
                    if (b->tex) { glUniform1f(uUseTex, 1.0f); glBindTexture(GL_TEXTURE_2D, b->tex);
                                  glUniform3f(uColor, 1.0f, 1.0f, 1.0f); }
                    else { glUniform1f(uUseTex, 0.0f); glUniform3f(uColor, 1.0f, 0.85f, 0.5f); }
                    lastglowtex = b->tex;
                }
                draw_batch(b);
                g_dbg.drawn++;
            }
            glUniform1f(uUnlit, 0.0f); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* LAMP HALOS, drawn from the district's LIGHT SOURCES rather than from
           lamp geometry: many lamp meshes are flat quads that all but vanish
           seen from the side, so at driving height the light seemed to switch
           off while it was still visible from above. Additive, billboarded to
           the camera. The halo RADIUS is not in the data -- the record carries
           falloff radii only -- so the 0.35 factor on the inner radius is ours;
           what is in the data is the difference between a street lamp and a car
           park floodlight, and that comes through. */
        if (nlsrc && tex_glow && g_dbg.night_mode && g_dbg.show_track) {
            float vx = camtgt[0]-cam[0], vy = camtgt[1]-cam[1], vz = camtgt[2]-cam[2];
            float vl = sqrtf(vx*vx+vy*vy+vz*vz); if (vl < 1e-4f) vl = 1.0f;
            vx/=vl; vy/=vl; vz/=vl;
            float rx = vy, ry = -vx, rz = 0.0f;                  /* screen right */
            float rl = sqrtf(rx*rx+ry*ry); if (rl < 1e-4f) { rx=1; ry=0; rl=1; }
            rx/=rl; ry/=rl;
            float ux = ry*vz - rz*vy, uy = rz*vx - rx*vz, uz = rx*vy - ry*vx;
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
            /* The halo is the game's own flare sheet. Its alpha survives now
               that the decoder keeps a plane whenever the texture record says
               the texture is not opaque -- the flare is additive, so it is
               kept even though its alpha is close to uniform. */
            glUniform1f(uUnlit, 1.0f);
            glUniform1f(uUseTex, 1.0f); glUniform1f(rp.uSoft, 0.0f);
            glBindTexture(GL_TEXTURE_2D, tex_glow);
            for (int q = 0; q < nlsrc; q++) {
                const N2LightSrc *L = &lsrc[q];
                float dx = L->x-cam[0], dy = L->y-cam[1], dz = L->z-cam[2];
                float d2 = dx*dx+dy*dy+dz*dz;
                if (d2 > VIEW_DIST*VIEW_DIST) continue;   /* same range as the world */
                float HALO = L->r_in * 0.35f; if (HALO < 1.0f) HALO = 1.0f;
                float M[16] = {
                    rx*HALO, ry*HALO, rz*HALO, 0,
                    ux*HALO, uy*HALO, uz*HALO, 0,
                    0,0,1,0,
                    L->x - (rx+ux)*HALO*0.5f,
                    L->y - (ry+uy)*HALO*0.5f,
                    L->z - (rz+uz)*HALO*0.5f, 1 };
                float MV[16]; mat_mul(MVP, M, MV);
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, MV);
                glUniform3f(uColor, L->cr/255.0f, L->cg/255.0f, L->cb/255.0f);
                float fade = 1.0f - sqrtf(d2)/VIEW_DIST; if (fade < 0) fade = 0;
                glUniform1f(rp.uAlpha, fade*fade + 0.25f*fade);
                draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            glUniform1f(rp.uAlpha, 1.0f); glUniform1f(uUnlit, 0.0f);
            glUniform1f(rp.uSoft, 0.0f);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, MVP);
            glDepthMask(GL_TRUE); glDisable(GL_BLEND);
        }

        /* HUD: race-position leaderboard — one colour bar per car, ordered by
           progress along the racing line (leader on top); player bar wider. */
        /* race telemetry (position/lap/speed) always mirrored to g_dbg for
           the ImGui panel; the viewport HUD drawing itself is additionally
           gated so debug builds can hide it (same pattern as the menu HUD
           above — plain builds have no ImGui, so they always draw it). */
        if (nai > 0 && race_state != 3) {
            int myprog = p_lap*aipath.n + p_prev, ppos_mirror = 1;
            for (int k = 0; k < nai; k++)
                if (ais[k].lap*aipath.n + ais[k].prevrel > myprog) ppos_mirror++;
            g_dbg.race_pos = ppos_mirror; g_dbg.race_cars = nai + 1;
            g_dbg.race_lap = p_lap<LAP_TARGET?p_lap+1:LAP_TARGET; g_dbg.race_laps = LAP_TARGET;
        } else g_dbg.race_cars = 0;   /* not racing: ImGui readout hides itself */
#ifdef DEBUG_UI
        int draw_race_hud = g_devui && !g_dbg.hud_hide_menu;
#else
        int draw_race_hud = g_devui;
#endif
        if (nai > 0 && race_state != 3 && draw_race_hud) {
            glDisable(GL_DEPTH_TEST);
            glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f);
            /* rank by monotonic progress = lap*loop + progress-along-loop */
            int nc = nai + 1, ord[N_AI+1], prog[N_AI+1], pl[N_AI+1];
            float col[N_AI+1][3];
            prog[0] = p_lap*aipath.n + p_prev; pl[0]=1;
            col[0][0]=0.85f; col[0][1]=0.12f; col[0][2]=0.12f;
            for (int k=0;k<nai;k++){
                prog[k+1]=ais[k].lap*aipath.n + ais[k].prevrel;
                memcpy(col[k+1], ais[k].col, sizeof col[0]); pl[k+1]=0;
            }
            for (int i=0;i<nc;i++) ord[i]=i;
            for (int i=0;i<nc;i++) for (int j=i+1;j<nc;j++)
                if (prog[ord[j]] > prog[ord[i]]) { int t=ord[i]; ord[i]=ord[j]; ord[j]=t; }
            for (int i=0;i<nc;i++){
                int c=ord[i];
                float w = pl[c]?0.10f:0.06f, x=-0.97f, y=0.90f-i*0.10f, h=0.075f;
                float M[16]={ w,0,0,0, 0,h,0,0, 0,0,1,0, x,y,0,1 };
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, M);
                glUniform3f(uColor, col[c][0], col[c][1], col[c][2]);
                draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            /* lap counter (green pips, one per completed lap) + lap-progress bar */
            for (int i=0;i<p_lap && i<8;i++){
                float x=-0.97f+i*0.05f, y=-0.93f;
                float M[16]={0.035f,0,0,0, 0,0.05f,0,0, 0,0,1,0, x,y,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                glUniform3f(uColor,0.2f,0.9f,0.3f); draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            {   /* thin bar: fraction of the current lap completed */
                float frac = (float)p_prev / (float)aipath.n;
                float bg[16]={1.5f,0,0,0, 0,0.03f,0,0, 0,0,1,0, -0.75f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,bg);
                glUniform3f(uColor,0.15f,0.15f,0.18f); draw_gpumesh(&quad);
                g_dbg.drawn++;
                float fg[16]={1.5f*frac,0,0,0, 0,0.03f,0,0, 0,0,1,0, -0.75f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,fg);
                glUniform3f(uColor,0.9f,0.8f,0.2f); draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            {   /* speedometer: fraction of top speed (bottom-right) */
                float sf = speed / PHYS_MAXSPD; if (sf < 0) sf = -sf; if (sf > 1) sf = 1;
                float bg[16]={0.35f,0,0,0, 0,0.03f,0,0, 0,0,1,0, 0.58f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,bg);
                glUniform3f(uColor,0.15f,0.15f,0.18f); draw_gpumesh(&quad);
                g_dbg.drawn++;
                float fg[16]={0.35f*sf,0,0,0, 0,0.03f,0,0, 0,0,1,0, 0.58f,-0.9f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,fg);
                glUniform3f(uColor,0.3f,0.85f,1.0f); draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            {   /* text labels: position (top-right), lap (bottom-left), speed (kph) */
                char buf[32];
                int ppos = 1; for (int i=0;i<nc;i++) if (ord[i]==0) { ppos=i+1; break; }
                snprintf(buf,sizeof buf,"P%d/%d", ppos, nc);
                glUniform3f(uColor,1,1,1);
                draw_text(&quad, uMVP, buf, -text_width(buf,0.024f)/2, 0.96f, 0.024f, 0.036f);
                snprintf(buf,sizeof buf,"LAP %d/%d", p_lap<LAP_TARGET?p_lap+1:LAP_TARGET, LAP_TARGET);
                draw_text(&quad, uMVP, buf, -0.97f, -0.80f, 0.018f, 0.028f);
                int kph = (int)PHYS_KMH(speed<0?-speed:speed);
                snprintf(buf,sizeof buf,"%d", kph);
                glUniform3f(uColor,0.3f,0.85f,1.0f);
                draw_text(&quad, uMVP, buf, 0.58f, -0.80f, 0.02f, 0.03f);
            }
            if (aipath.n > 1) {   /* minimap (top-right): racing line + car dots */
                float bx0=0.66f, by0=0.52f, bw=0.30f, bh=0.36f;
                float px0=1e30f,py0=1e30f,px1=-1e30f,py1=-1e30f;
                for (int i=0;i<aipath.n;i++){ float x=aipath.xy[i*2],y=aipath.xy[i*2+1];
                    if(x<px0)px0=x; if(x>px1)px1=x; if(y<py0)py0=y; if(y>py1)py1=y; }
                float asp=(float)W/H, span=(px1-px0)>(py1-py0)?(px1-px0):(py1-py0);
                if (span < 1e-3f) span = 1e-3f;
                float scy = bh*0.9f/span, scx = scy/asp;   /* uniform on screen */
                float ccx=(px0+px1)*0.5f, cyw=(py0+py1)*0.5f;
                float mmx0 = bx0+bw*0.5f, mmy0 = by0+bh*0.5f;
                /* dark panel */
                float PM[16]={bw,0,0,0, 0,bh,0,0, 0,0,1,0, bx0,by0,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,PM); glUniform3f(uColor,0.04f,0.05f,0.08f); draw_gpumesh(&quad);
                g_dbg.drawn++;
                /* draw a centred dot of half-size hs at world (wx,wy) */
                #define MMDOT(wx,wy,hs,r,g,b) do{ \
                    float mmx=mmx0+((wx)-ccx)*scx, mmy=mmy0+((wy)-cyw)*scy; \
                    float M[16]={(hs)*2/asp,0,0,0, 0,(hs)*2,0,0, 0,0,1,0, mmx-(hs)/asp,mmy-(hs),0,1}; \
                    glUniformMatrix4fv(uMVP,1,GL_FALSE,M); glUniform3f(uColor,r,g,b); draw_gpumesh(&quad); \
                    g_dbg.drawn++; }while(0)
                for (int i=0;i<aipath.n;i++) MMDOT(aipath.xy[i*2],aipath.xy[i*2+1], 0.004f, 0.4f,0.4f,0.46f);
                for (int k=0;k<nai;k++) MMDOT(ais[k].pos[0],ais[k].pos[1], 0.010f, ais[k].col[0],ais[k].col[1],ais[k].col[2]);
                MMDOT(carpos[0],carpos[1], 0.013f, 0.95f,0.15f,0.15f);
                #undef MMDOT
            }
            glEnable(GL_DEPTH_TEST);
        }

        /* race banners: 3-2-1 countdown, then a finish banner + place pips */
        glDisable(GL_DEPTH_TEST);
        glUniform1f(uUnlit, 1.0f); glUniform1f(uUseTex, 0.0f);
#ifdef DEBUG_UI
        /* the retro pixel-font menu overlay is off by default under the debug
           build (session info lives in the ImGui panel instead, see below) —
           re-enable it live with the panel's "show 3D menu HUD" checkbox.
           Plain (non-debug) builds always draw it: it is their ONLY UI. */
        int draw_menu_hud = g_devui && !g_dbg.hud_hide_menu;
#else
        int draw_menu_hud = g_devui;
#endif
        if (race_state == 3 && draw_menu_hud) {
            /* car selector (Left/Right): a tight row of pips, chosen lit white */
            float cw = 0.02f, cx0 = -((ncars-1)*cw)/2.0f;
            for (int i = 0; i < ncars; i++) {
                int s = (i == selcar);
                float M[16]={0.014f,0,0,0, 0,s?0.05f:0.028f,0,0, 0,0,1,0, cx0+i*cw,0.44f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.95f,0.95f,0.98f);
                else   glUniform3f(uColor,0.30f,0.32f,0.38f);
                draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            /* track selector (Up/Down): one blue pip per STREAM*.BUN found */
            float tx0 = -((ntrack-1)*0.05f)/2.0f;
            for (int i = 0; i < ntrack; i++) {
                int s = (i == seltrack);
                float M[16]={0.035f,0,0,0, 0,s?0.05f:0.03f,0,0, 0,0,1,0, tx0+i*0.05f,0.36f,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.3f,0.7f,1.0f);
                else   glUniform3f(uColor,0.25f,0.3f,0.4f);
                draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            /* circuit selector ([ / ]): one yellow pip per circuit, chosen lit + tall */
            float x0 = -((ncirc-1)*0.05f)/2.0f;
            for (int i = 0; i < ncirc; i++) {
                int s = (i == selcirc);
                float h = s?0.05f:0.03f, y = 0.29f - (s?0.01f:0);
                float M[16]={0.035f,0,0,0, 0,h,0,0, 0,0,1,0, x0+i*0.05f,y,0,1};
                glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
                if (s) glUniform3f(uColor,0.95f,0.8f,0.2f);
                else   glUniform3f(uColor,0.3f,0.3f,0.35f);
                draw_gpumesh(&quad);
                g_dbg.drawn++;
            }
            /* labels: car name (big, white) above its pips; track name below */
            glUniform3f(uColor, 0.95f, 0.95f, 0.98f);
            draw_text(&quad, uMVP, carname, -text_width(carname,0.017f)/2, 0.56f, 0.017f, 0.026f);
            glUniform3f(uColor, 0.45f, 0.7f, 1.0f);
            draw_text(&quad, uMVP, trackname, -text_width(trackname,0.012f)/2, 0.23f, 0.012f, 0.02f);
            /* "press ENTER" prompt: a gently pulsing green bar */
            float pulse = 0.55f + 0.45f*sinf(menuspin*6.0f);
            float M[16]={0.5f,0,0,0, 0,0.06f,0,0, 0,0,1,0, -0.25f,-0.25f,0,1};
            glUniformMatrix4fv(uMVP,1,GL_FALSE,M);
            glUniform3f(uColor,0.15f*pulse,0.85f*pulse,0.3f*pulse);
            draw_gpumesh(&quad);
            g_dbg.drawn++;
        } else if (draw_menu_hud && race_state == 0) {  /* 3-2-1 / GO, big and centred */
            if (racetimer >= COUNTDOWN-24) {
                glUniform3f(uColor,0.2f,0.95f,0.3f);
                draw_text(&quad, uMVP, "GO", -text_width("GO",0.13f)/2, 0.44f, 0.13f, 0.17f);
            } else {
                char b[4]; snprintf(b,sizeof b,"%d", 3 - racetimer/60);
                glUniform3f(uColor,0.95f,0.25f,0.15f);
                draw_text(&quad, uMVP, b, -text_width(b,0.13f)/2, 0.44f, 0.13f, 0.17f);
            }
        } else if (draw_menu_hud && race_state == 2) {  /* finished: FINISH + place */
            glUniform3f(uColor,0.95f,0.8f,0.2f);
            draw_text(&quad, uMVP, "FINISH", -text_width("FINISH",0.07f)/2, 0.36f, 0.07f, 0.10f);
            char b[16]; snprintf(b,sizeof b,"P%d/%d", finish_place, nai+1);
            glUniform3f(uColor,1,1,1);
            draw_text(&quad, uMVP, b, -text_width(b,0.05f)/2, 0.14f, 0.05f, 0.075f);
        }
        glEnable(GL_DEPTH_TEST);

        g_dbg.nav = world.nav; g_dbg.nnav = world.nnav;
        g_dbg.navedge = world.navedge; g_dbg.nnavedge = world.nnavedge;
        g_dbg.navcomp = world.navcomp; g_dbg.ndist = world.ndist;
        for (int i = 0; i < world.ndist && i < 8; i++) {
            snprintf(g_dbg.dist_tok[i], 4, "%s", world.dist[i].tok);
            snprintf(g_dbg.dist_name[i], 24, "%s", world_district_name(world.dist[i].tok));
        }
        /* Race & Track Manager: mirror the catalog, apply panel mode switches */
        g_dbg.ev = world.ev;  g_dbg.ev_count  = world.nev;
        g_dbg.bar = world.bar; g_dbg.bar_count = world.nbar;
        g_dbg.mode = world.mode; g_dbg.active_ev = world.active_ev;
        g_dbg.masked_links = world.nmasked;
        g_dbg.navopen = world.mode == MODE_RACE_EVENT ? world.navopen : NULL;
        g_dbg.race = &world.race;
        if (g_dbg.mode_request) {
            g_dbg.mode_request = 0;
            world_race_stop(&world);
            world_set_mode(&world, g_dbg.want_mode, g_dbg.want_event);
            g_dbg.gps_n = 0;             /* the old route may cross a new barrier */
        }
        if (g_dbg.race_start_request) {
            g_dbg.race_start_request = 0;
            if (world.active_ev >= 0)
                world_race_start(&world, troot, world.active_ev, g_dbg.race_maxlaps_want);
        }
        if (g_dbg.race_stop_request) { g_dbg.race_stop_request = 0; world_race_stop(&world); }
        /* GPS: solve a fresh route whenever the panel asks for a destination */
        {   static int gpath[8192];
            if (g_dbg.gps_request) {
                g_dbg.gps_request = 0;
                int s0 = world_nav_nearest(&world, carpos[0], carpos[1]);
                int g0 = world_nav_nearest(&world, g_dbg.gps_want_x, g_dbg.gps_want_y);
                float gd = 0; uint32_t ta = SDL_GetTicks();
                int gn = world_route(&world, s0, g0, gpath, 8192, &gd);
                g_dbg.gps_ms = (int)(SDL_GetTicks() - ta);
                g_dbg.gps_path = gpath; g_dbg.gps_n = gn; g_dbg.gps_dist = gd;
                printf("GPS route: %d nodes, %.0f m, %d ms\n", gn, gd, g_dbg.gps_ms);
            }
        }
        for (int i = 0; i < 4; i++) g_dbg.navbb[i] = world.navbb[i];
        /* live district tracking: log every boundary crossing */
        {   static int lastzone = -2;
            int zi = world_district_at(&world, carpos[0], carpos[1], 150.0f);
            static char zbuf[24];
            if (zi >= 0 && zi < world.ndist) snprintf(zbuf, sizeof zbuf, "%s", world.dist[zi].tok);
            else snprintf(zbuf, sizeof zbuf, "-");
            const char *zn = zbuf;
            if (zi != lastzone) {
                if (lastzone != -2)
                    printf("Transition: %s -> %s   at (%.0f, %.0f)\n",
                           (lastzone >= 0 && lastzone < world.ndist) ? world.dist[lastzone].tok : "-", zn,
                           carpos[0], carpos[1]);
                lastzone = zi;
            }
            snprintf(g_dbg.zone_name, sizeof g_dbg.zone_name, "%s", zn);
            g_dbg.zone_count = world.ndist;
        }
#ifdef DEBUG_UI     /* debug readouts + ImGui overlay, drawn on top of everything */
        g_dbg.cam[0]=cam[0]; g_dbg.cam[1]=cam[1]; g_dbg.cam[2]=cam[2];
        g_dbg.car[0]=carpos[0]; g_dbg.car[1]=carpos[1]; g_dbg.car[2]=carpos[2];
        g_dbg.heading=heading; g_dbg.kmh=PHYS_KMH(speed);
        g_dbg.car_meshes=ncar; g_dbg.track_meshes=nm;
        snprintf(g_dbg.car_name, sizeof g_dbg.car_name, "%s", carname);
        snprintf(g_dbg.track_name, sizeof g_dbg.track_name, "%s", trackname);
        g_dbg.sel_car=selcar; g_dbg.n_cars=ncars;
        g_dbg.sel_track=seltrack; g_dbg.n_tracks=ntrack;
        g_dbg.sel_circuit=selcirc; g_dbg.n_circuits=ncirc;
        {   static int icat[512], ivts[512];
            int ni = ncar < 512 ? ncar : 512;
            for (int i = 0; i < ni; i++) { icat[i]=cgm[i].cat; ivts[i]=car.meshes[i].nverts; }
            g_dbg.insp_count = ni; g_dbg.insp_cat = icat; g_dbg.insp_verts = ivts;
        }
        g_dbg.scripted = sdefs; g_dbg.scripted_count = nsd;
        /* scenery semantics: class census + the named chunks nearest the car */
        {   for (int i = 0; i < 8; i++) g_dbg.scen_count[i] = 0;
            for (int i = 0; i < nm; i++) {
                int sc = scene.meshes[i].scen; if (sc < 8) g_dbg.scen_count[sc]++; }
            int nn = 0;
            for (int i = 0; i < nm && nn < 12; i++) {
                if (!scene.meshes[i].scen || !scene.meshes[i].sname[0]) continue;
                float *bb = world.mbb[i];
                float dx = carpos[0]<bb[0]?bb[0]-carpos[0]:(carpos[0]>bb[2]?carpos[0]-bb[2]:0);
                float dy = carpos[1]<bb[1]?bb[1]-carpos[1]:(carpos[1]>bb[3]?carpos[1]-bb[3]:0);
                float dd = sqrtf(dx*dx+dy*dy);
                if (dd > 60.0f) continue;
                snprintf(g_dbg.scen_near[nn], sizeof g_dbg.scen_near[0], "%-24s %-8s %4.0fm",
                         scene.meshes[i].sname, n2_scen_name(scene.meshes[i].scen), dd);
                nn++;
            }
            g_dbg.scen_near_n = nn;
        }
        g_dbg.wheel_brands = wheel_brands;
        g_dbg.wheel_brand_n = n_wheel_brands;
        if (g_dbg.wheel_style < 1) g_dbg.wheel_style = wheel_style;
        g_dbg.car_list = (const char (*)[64])carlist;
        g_dbg.track_list = (const char (*)[64])tracklist;
        if (g_devui) { dbgui_frame(); dbgui_render(); }
        else { g_dbg.want_car = -1; g_dbg.want_track = -1;
               g_dbg.mode_request = 0; g_dbg.race_start_request = 0;
               g_dbg.race_stop_request = 0; }
        /* the Session panel's car/track combos ask for a switch the same
           way the arrow keys do: a clean process relaunch (see relaunch()
           above) — there's no in-place teardown/reload path for the ~30
           pieces of long-lived world/car state, and building one blind
           (no way to interactively test repeated swaps right now) risks
           a leak or dangling handle that a single screenshot can't catch. */
        if (g_dbg.insp_dump) {
            g_dbg.insp_dump = 0;
            int i = g_dbg.insp_sel;
            if (i >= 0 && i < ncar) {
                float bb[6]; n2_mesh_bbox(&car.meshes[i], bb);
                printf("\n[inspector] car mesh %d  cat=%d  verts=%d  tris=%d\n",
                       i, cgm[i].cat, car.meshes[i].nverts, car.meshes[i].nidx/3);
                printf("  local bbox  x[%.4f,%.4f] y[%.4f,%.4f] z[%.4f,%.4f]\n",
                       bb[0],bb[1],bb[2],bb[3],bb[4],bb[5]);
                printf("  car world pos (%.2f,%.2f,%.2f) heading %.3f rad  %.1f km/h\n",
                       carpos[0],carpos[1],carpos[2], heading, PHYS_KMH(speed));
                printf("  wheel scale %.3f  stance: axle F%+.3f R%+.3f  track F%.3f R%.3f  ride %+.3f\n",
                       g_dbg.wheel_scale, g_dbg.wheel.front_axle, g_dbg.wheel.rear_axle,
                       g_dbg.wheel.front_track, g_dbg.wheel.rear_track, g_dbg.wheel.ride_y);
                for (int w = 0; w < 4; w++) {
                    printf("  wheelT[%d] (column-major):\n", w);
                    for (int r = 0; r < 4; r++)
                        printf("    % .4f % .4f % .4f % .4f\n",
                               wheelT[w][r*4+0], wheelT[w][r*4+1],
                               wheelT[w][r*4+2], wheelT[w][r*4+3]);
                }
                if (nwheelgm > 0) {
                    float rb[6]; n2_mesh_bbox(&wheellib.meshes[0], rb);
                    printf("  active rim: %d meshes, mesh0 %d verts, bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]\n",
                           nwheelgm, wheellib.meshes[0].nverts, rb[0],rb[1],rb[2],rb[3],rb[4],rb[5]);
                }
            } else printf("[inspector] no mesh selected\n");
        }
        if (g_dbg.wheel_reload) {          /* panel picked a brand/style */
            g_dbg.wheel_reload = 0;
            if (g_dbg.wheel_brand >= 0 && g_dbg.wheel_brand < n_wheel_brands &&
                g_dbg.wheel_brand != wheel_brand) {
                char wlp[1024];
                snprintf(wlp, sizeof wlp, "%s/CARS/WHEELS/GEOMETRY_%s.BIN",
                         dataroot, wheel_brands[g_dbg.wheel_brand]);
                unsigned char *nd = n2_read_file(wlp, &wllen);
                if (nd) { free(wldata); wldata = nd; wheel_brand = g_dbg.wheel_brand; }
            }
            wheel_style = g_dbg.wheel_style < 1 ? 1 : g_dbg.wheel_style;
            if (load_rim_style(wldata, wllen, wkeys, nwkeys, wheel_style,
                               &wheellib, &wheelgm, &nwheelgm,
                               wtdata, wtlen, &rimtex, carWheelR))
                printf("rims -> %s style %d (%d mesh(es))\n",
                       wheel_brands[wheel_brand], wheel_style, nwheelgm);
        }
        if (g_dbg.want_car >= 0 && g_dbg.want_car < ncars)
            relaunch(selfexe, dataroot, carlist[g_dbg.want_car], trackname);
        if (g_dbg.want_track >= 0 && g_dbg.want_track < ntrack)
            relaunch(selfexe, dataroot, carname, tracklist[g_dbg.want_track]);
#endif
        if (smaudit) {
            /* Four sampler variants on the TARGET TEXTURE ONLY, all captured
               during the countdown while the car is motionless, so the camera
               pose is byte-identical across variants. Sampler state is restored
               to variant 0 before the run ends; nothing else is touched. */
            static const char *vn[4] = { "0_genmips_trilinear", "1_base_linear",
                                         "2_genmips_aniso", "3_tpk_chain" };
            static int done_v = 0;
            long f = ra_f - 1;
            int v = (f == 100) ? 0 : (f == 110) ? 1 : (f == 120) ? 2 : (f == 130) ? 3 : -1;
            if (v >= 0 && v < 4 && !(done_v & (1 << v))) {
                done_v |= 1 << v;
                GLuint tid = 0;
                for (int j = 0; j < ntmap; j++) if (tmapkey[j] == smkey) tid = tmaptex[j];
                if (tid) {
                    glBindTexture(GL_TEXTURE_2D, tid);
                    float amax = 1.0f;
                    glGetFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY*/, &amax);
                    if (v == 0) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                                  GL_LINEAR_MIPMAP_LINEAR);
                                  glTexParameterf(GL_TEXTURE_2D, 0x84FE, 1.0f); }
                    else if (v == 1) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                                       GL_LINEAR);
                                       glTexParameterf(GL_TEXTURE_2D, 0x84FE, 1.0f); }
                    else if (v == 2) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                                       GL_LINEAR_MIPMAP_LINEAR);
                                       glTexParameterf(GL_TEXTURE_2D, 0x84FE, amax);
                                       printf("SM max anisotropy reported: %.1f\n", amax); }
                    else { printf("SM variant 3 (original TPK mip chain): NOT CAPTURED -- "
                                  "the world path never retains an encoded chain for this "
                                  "key (see inventory above); repeating variant 0\n");
                           glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                           GL_LINEAR_MIPMAP_LINEAR);
                           glTexParameterf(GL_TEXTURE_2D, 0x84FE, 1.0f); }
                }
            }
            /* capture one frame AFTER the state took effect */
            int cap = (f == 101) ? 0 : (f == 111) ? 1 : (f == 121) ? 2 : (f == 131) ? 3 : -1;
            if (cap >= 0) {
                char sp[1024]; snprintf(sp, sizeof sp, "%s_%s.png", smaudit, vn[cap]);
                unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
                POST_RESOLVE();
                POST_RESOLVE();
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
                for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
                write_png(sp, W, H, fl); free(px); free(fl);
                printf("SM captured %s (frame %ld, car stationary during countdown)\n", sp, f);
            }
            if (f == 132) {   /* restore normal sampler state, then stop */
                GLuint tid = 0;
                for (int j = 0; j < ntmap; j++) if (tmapkey[j] == smkey) tid = tmaptex[j];
                if (tid) { glBindTexture(GL_TEXTURE_2D, tid);
                           glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                           GL_LINEAR_MIPMAP_LINEAR);
                           glTexParameterf(GL_TEXTURE_2D, 0x84FE, 1.0f); }
                printf("SM sampler state restored to the production default\n");
                running = 0;
            }
        }
        if (raudit) {   /* deterministic evidence frames, then stop */
            static int rshot = 0; rshot++;
            const char *tag = NULL;
            if (rshot == 2)   tag = "_menu";
            else if (ra_start >= 0 && ra_f - 1 == ra_start) tag = "_startline";
            else if (ra_done) tag = "_end";
            if (g_m107) {
                /* Freeze the orbit so all three captures share one camera pose;
                   the camera FORMULA is untouched -- eye = car + (cos,sin)*16, +8,
                   target = car + 1.5z -- and it never reads heading. */
                menuspin = 0.0f;
                int v = (rshot >= 20 && rshot < 30) ? 0 : (rshot >= 30 && rshot < 40) ? 1
                      : (rshot >= 40) ? 2 : -1;
                if (v >= 0) heading = g_m107_h[v];
                static int shot107 = 0;
                if ((rshot == 29 || rshot == 39 || rshot == 49) && shot107 < 3) {
                    static const char *hn[3] = { "A_dense_centre", "B_road_tangent",
                                                 "C_tangent_reversed" };
                    char sp[1024]; snprintf(sp, sizeof sp, "%s_%s.png", shaudit, hn[shot107]);
                    unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
                    POST_RESOLVE();
                POST_RESOLVE();
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
                    for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
                    write_png(sp, W, H, fl); free(px); free(fl);
                    printf("M107 capture %s  heading %+.4f  car(%.3f %.3f %.3f) "
                           "cam(%.3f %.3f %.3f) menuspin %.4f\n", sp, heading,
                           carpos[0], carpos[1], carpos[2], cam[0], cam[1], cam[2], menuspin);
                    shot107++;
                }
                if (shot107 >= 3) { printf("M107: three headings captured at one camera pose\n");
                                    running = 0; }
                tag = NULL;   /* suppress the ordinary menu frame */
            }
            /* M132-R: four yaws from the EXACT frozen production start pose.
               The car is held by the handbrake with zero throttle from the
               moment world_race_start placed it, so every capture shares one
               position; only the camera yaw changes. Pose, support and camera
               are printed with the first capture so the frame can be verified
               against the required coordinates rather than assumed. */
            if (poseshot && ra_start >= 0) {
                long r = ra_f - ra_start;
                static const int YAW[4] = {0, 90, 180, 270};
                /* the yaw must be settled BEFORE the frame is drawn, so it is a
                   function of r alone and holds for four frames per capture */
                int idx = r < 6 ? 0 : (int)((r - 6) / 4);
                if (idx > 3) idx = 3;
                shotyaw = YAW[idx] * 3.14159265f/180.0f;
                int slot = (r >= 6 && (r - 6) % 4 == 3) ? idx : -1;
                if (slot >= 0) {
                    if (slot == 0) {
                        WGroundHit ph; int pc = world_ground_hit(&scene, carpos[0],
                                                                 carpos[1], carpos[2], &ph);
                        printf("POSE track=%s %s\n", trackname,
                               world.active_ev >= 0 ? "event" : "circuit");
                        printf("POSE car   (%.3f, %.3f, %.3f)  heading %+.4f  frozen %ld "
                               "frames after start\n", carpos[0], carpos[1], carpos[2],
                               heading, r);
                        printf("POSE support cat=%s mesh=%d tri=%d z=%.3f name=%s\n",
                               pc==WSURF_ROAD?"ROAD":pc==WSURF_TERRAIN?"TERRAIN":"NONE",
                               ph.mesh, ph.tri, ph.z,
                               pc!=WSURF_NONE && ph.mesh>=0 && ph.mesh<scene.count
                                   ? scene.meshes[ph.mesh].sname : "-");
                        if (pc != WSURF_ROAD)
                            printf("POSE INVALID: support is not real ROAD\n");
                    }
                    if (slot == 0) {
                        printf("TIER %s  cutoff %.1f m  far %.1f m\n",
                               tier==0?"baseline (700 m, no vista)":
                               tier==1?"ordinary (fog-derived, no vista)":
                                       "full (fog-derived + vista) EXPERIMENTAL",
                               (double)VIEW_DIST, zfar);
                        printf("COUNT sky      draws %4d\n", skydraws);
                        printf("COUNT vista    meshes %5d/%-5d batches %4d/%-4d draws %4d "
                               "(skipped %d meshes in %d batches)\n",
                               vistamesh, world.vista.count, vistadrawn, nvista, vistadraws,
                               vistanearmesh, vistanear);
                        printf("COUNT vista    of the skipped, %d were buried more "
                               "than %.0f m below the player's ground\n",
                               vistaburied, (double)VISTA_BURIED_M);
                        printf("COUNT vista    nearest DRAWN surface %.1f m, nearest "
                               "REJECTED surface %.1f m (cutoff %.1f m)\n",
                               vistakeptmin > 1e29f ? -1.0 : (double)vistakeptmin,
                               vistamind    > 1e29f ? -1.0 : (double)vistamind,
                               (double)VIEW_DIST);
                        printf("COUNT ordinary meshes %5d/%-5d batches %4d/%-4d draws %4d\n",
                               ndrawn, nm, wbdrawn, nbatch, wbdrawn);
                        printf("COUNT car/glow/HUD draws %4d\n",
                               g_dbg.drawn - skydraws - vistadraws - wbdrawn);
                        /* render cost since the pose was pinned -- the old
                           figure divided total elapsed (including the scene
                           upload) by every frame and read ~17 ms for a scene
                           that renders in under 2 */
                        long pf = ra_f - g_pose_f0;
                        printf("COUNT total    draws %4d   frame %.2f ms "
                               "(wall clock, vsync-bound; see --shot for render cost)\n",
                               g_dbg.drawn,
                               pf > 0 ? (SDL_GetTicks()-g_pose_t0)/(float)pf : 0.0f);
                    }
                    if (slot == 0 && lsaudit) {
                        /* M133 source-to-screen attribution. Every source mesh
                           whose real triangle surface reaches within the radius
                           gets exactly one outcome; the outcomes are summed and
                           checked against the population so the census closes.
                           Read-only: nothing here influences a draw. */
                        static const float RAD[2] = {150.0f, 300.0f};
                        for (int rr = 0; rr < 2; rr++) {
                            float R = RAD[rr];
                            /* outcome: 0 drawn, 1 far-cull, 2 no batch (sky/glow),
                               3 unresolved texture (still drawn, grey), 4 below
                               the supporting layer */
                            long cls[8][5]; memset(cls, 0, sizeof cls);
                            long clsobj[8]; memset(clsobj, 0, sizeof clsobj);
                            long clsbat[8]; memset(clsbat, 0, sizeof clsbat);
                            static unsigned char batseen[65536];
                            memset(batseen, 0, sizeof batseen);
                            long pop = 0, acct = 0;
                            for (int i = 0; i < nm; i++) {
                                const N2Mesh *me = &scene.meshes[i];
                                const float *bb = world.mbb[i];
                                float bx = carpos[0] < bb[0] ? bb[0]-carpos[0]
                                         : (carpos[0] > bb[2] ? carpos[0]-bb[2] : 0);
                                float by = carpos[1] < bb[1] ? bb[1]-carpos[1]
                                         : (carpos[1] > bb[3] ? carpos[1]-bb[3] : 0);
                                if (bx*bx + by*by > R*R) continue;   /* AABB pre-pass */
                                float near2 = 1e30f;
                                for (int t = 0; t + 2 < me->nidx && near2 > 1.0f; t += 3) {
                                    const float *A = me->verts + me->idx[t]*5;
                                    const float *B = me->verts + me->idx[t+1]*5;
                                    const float *C = me->verts + me->idx[t+2]*5;
                                    float d2 = pt_tri_d2(carpos, A, B, C);
                                    if (d2 < near2) near2 = d2;
                                }
                                if (near2 > R*R) continue;
                                int sc2 = me->scen; if (sc2 < 0 || sc2 > 7) sc2 = 0;
                                pop++; clsobj[sc2]++;
                                int b = meshbatch[i];
                                int outcome;
                                if (b < 0) outcome = 2;
                                else {
                                    if (b < 65536 && !batseen[b]) { batseen[b]=1; clsbat[sc2]++; }
                                    const N2Batch *bt = &wbatch[b];
                                    float dx2 = cam[0] < bt->bbox_min[0] ? bt->bbox_min[0]-cam[0]
                                              : (cam[0] > bt->bbox_max[0] ? cam[0]-bt->bbox_max[0] : 0);
                                    float dy2 = cam[1] < bt->bbox_min[1] ? bt->bbox_min[1]-cam[1]
                                              : (cam[1] > bt->bbox_max[1] ? cam[1]-bt->bbox_max[1] : 0);
                                    if (dx2*dx2 + dy2*dy2 > VIEW_DIST*VIEW_DIST) outcome = 1;
                                    else if (!mtex[i] && me->cat != N2_TERRAIN) outcome = 3;
                                    else outcome = 0;
                                }
                                if (outcome == 0) {
                                    /* is it under the road the car stands on? */
                                    float top = -1e30f;
                                    for (int v = 0; v < me->nverts; v++)
                                        if (me->verts[v*5+2] > top) top = me->verts[v*5+2];
                                    if (top < carpos[2] - 3.0f && me->cat != N2_ROAD
                                        && me->cat != N2_TERRAIN) outcome = 4;
                                }
                                cls[sc2][outcome]++; acct++;
                                if (rr == 1 && lsaudit > 1 &&
                                    (sc2 != N2_SC_TERRAIN || !mtex[i] || outcome != 0)) {
                                    /* per-object detail: where does this thing sit
                                       relative to the road under its OWN extent,
                                       sampled at the AABB corners and centre, not
                                       at its centre alone (long walls span layers) */
                                    static const char *OUT[5] =
                                        {"drawn","far-cull","no-batch","no-texture","below-layer"};
                                    float lo = 1e30f, hi = -1e30f;
                                    for (int v = 0; v < me->nverts; v++) {
                                        float z = me->verts[v*5+2];
                                        if (z < lo) lo = z; if (z > hi) hi = z;
                                    }
                                    float px2[5] = {bb[0],bb[2],bb[0],bb[2],(bb[0]+bb[2])*0.5f};
                                    float py2[5] = {bb[1],bb[1],bb[3],bb[3],(bb[1]+bb[3])*0.5f};
                                    float gmin = 1e30f, gmax = -1e30f; int ghit = 0;
                                    for (int q = 0; q < 5; q++) {
                                        float gz2 = lo;
                                        if (world_ground_at(&scene, px2[q], py2[q], hi, &gz2) != WSURF_NONE) {
                                            ghit++;
                                            if (gz2 < gmin) gmin = gz2;
                                            if (gz2 > gmax) gmax = gz2;
                                        }
                                    }
                                    printf("LSAOBJ %-9s %-30s tris %5d dist %6.1f  "
                                           "xy[%9.2f %9.2f %9.2f %9.2f] "
                                           "z[%8.2f %8.2f]  ground %d/5 [%8.2f %8.2f]  "
                                           "base-vs-ground %+8.2f  tex %s  %s\n",
                                           n2_scen_name(sc2), me->sname[0]?me->sname:"?",
                                           me->nidx/3, sqrtf(near2),
                                           bb[0], bb[1], bb[2], bb[3], lo, hi,
                                           ghit, ghit?gmin:0.0f, ghit?gmax:0.0f,
                                           ghit ? lo - gmin : 0.0f,
                                           mtex[i] ? "ok" : "MISSING", OUT[outcome]);
                                    if (!mtex[i])
                                        printf("LSAKEY %-30s texkey %08x cat %d "
                                               "verts %d tris %d\n", me->sname, me->texkey,
                                               me->cat, me->nverts, me->nidx/3);
                                }
                            }
                            printf("LSA r=%.0f m  pose %s\n", R, trackname);
                            printf("LSA %-9s %6s %6s %6s | %6s %6s %6s %6s %6s\n",
                                   "class","objs","meshes","batch",
                                   "drawn","farcul","nobatc","notex","below");
                            for (int c = 0; c <= N2_SC_OTHER; c++) {
                                if (!clsobj[c]) continue;
                                printf("LSA %-9s %6ld %6ld %6ld | %6ld %6ld %6ld %6ld %6ld\n",
                                       n2_scen_name(c), clsobj[c], clsobj[c], clsbat[c],
                                       cls[c][0], cls[c][1], cls[c][2], cls[c][3], cls[c][4]);
                            }
                            printf("LSA TOTAL population %ld accounted %ld %s\n",
                                   pop, acct, pop==acct ? "(closed)" : "(LEAK)");
                        }
                    }
                    printf("POSE yaw %3d cam (%.3f, %.3f, %.3f) look (%.3f, %.3f, %.3f)\n",
                           YAW[slot], cam[0], cam[1], cam[2],
                           carpos[0], carpos[1], carpos[2]+1.5f);
                    char sp[1024];
                    snprintf(sp, sizeof sp, "%s_yaw%d.png", poseshot, YAW[slot]);
                    unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
                    POST_RESOLVE();
                POST_RESOLVE();
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
                    for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
                    write_png(sp, W, H, fl); free(px); free(fl);
                    printf("POSE frame written: %s\n", sp);
                }
                if (r > 6 + 3*4 + 3) running = 0;
                tag = NULL;
            }
            if (tag && shaudit && rshot > 2) tag = NULL;   /* menu frame only */
            if (tag) {
                char sp[1024]; snprintf(sp, sizeof sp, "%s%s.png", raudit, tag);
                unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
                POST_RESOLVE();
                POST_RESOLVE();
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
                for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
                write_png(sp, W, H, fl); free(px); free(fl);
                printf("RA frame written: %s\n", sp);
            }
            if (ra_done) running = 0;
            if (shaudit && !g_m107 && rshot >= 3) {
                printf("showcase audit: menu frame captured; Enter branch, route start "
                       "and --shot-static spawn were never invoked\n");
                running = 0;
            }
        }
        if (world_rail_census && shot && shotframe + 1 >= shotframes) {
            static const char *bn2[8] = { "<0.5","0.5-1","1-1.5","1.5-2",
                                          "2-3","3-5","5-10",">=10" };
            printf("\nM113 RAIL CANDIDATE CENSUS  %-8s", "");
            for (int b = 0; b < 8; b++) printf(" %9s", bn2[b]);
            printf("\n");
            for (int c = 0; c < 2; c++) {
                printf("  %-8s %-8s", c ? "TERRAIN" : "ROAD", "candidates");
                for (int b = 0; b < 8; b++) printf(" %9ld", world_rc_cand[c][b]);
                printf("\n  %-8s %-8s", "", "pushed");
                for (int b = 0; b < 8; b++) printf(" %9ld", world_rc_push[c][b]);
                printf("     Zspan min %.3f max %.3f\n",
                       world_rc_min[c] > 1e29f ? 0.0f : world_rc_min[c],
                       world_rc_max[c] < -1e29f ? 0.0f : world_rc_max[c]);
            }
        }
        if (daudit && shotframe == 2) {   /* one normal rendered frame at spawn */
            char sp[1024]; snprintf(sp, sizeof sp, "%s_spawn.png", daudit);
            unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
            POST_RESOLVE();
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
            for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
            write_png(sp, W, H, fl); free(px); free(fl);
            printf("DA spawn frame written: %s\n", sp);
        }
        if (shot && ++shotframe >= shotframes) {
            unsigned char *px = malloc((size_t)W*H*3), *fl = malloc((size_t)W*H*3);
            POST_RESOLVE();
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
            for (int y = 0; y < H; y++) memcpy(fl+(size_t)y*W*3, px+(size_t)(H-1-y)*W*3, W*3);
            write_png(shot, W, H, fl);
            free(px); free(fl);
            printf("frame avg: %.1f ms (vsync on), %d/%d world meshes drawn, %d draw calls\n",
                   (SDL_GetTicks()-t0)/(float)shotframe, ndrawn, nm, g_dbg.drawn);
            printf("TIER %s  cutoff %.1f m  far %.1f m\n",
                   tier==0?"baseline (700 m, no vista)":
                   tier==1?"ordinary (fog-derived, no vista)":
                           "full (fog-derived + vista) EXPERIMENTAL",
                   (double)VIEW_DIST, zfar);
            printf("COUNT sky      draws %4d\n", skydraws);
            printf("COUNT vista    meshes %5d/%-5d batches %4d/%-4d draws %4d "
                   "(skipped %d meshes in %d batches)\n",
                   vistamesh, world.vista.count, vistadrawn, nvista, vistadraws,
                   vistanearmesh, vistanear);
            printf("COUNT vista    nearest DRAWN surface %.1f m, nearest REJECTED "
                   "surface %.1f m (cutoff %.1f m)\n",
                   vistakeptmin > 1e29f ? -1.0 : (double)vistakeptmin,
                   vistamind    > 1e29f ? -1.0 : (double)vistamind, (double)VIEW_DIST);
            printf("COUNT ordinary meshes %5d/%-5d batches %4d/%-4d draws %4d\n",
                   ndrawn, nm, wbdrawn, nbatch, wbdrawn);
            printf("COUNT car/glow/HUD draws %4d\n",
                   g_dbg.drawn - skydraws - vistadraws - wbdrawn);
            printf("COUNT total    draws %4d\n", g_dbg.drawn);
            printf("visible scenery:");
            for (int sc=1; sc<=N2_SC_OTHER; sc++)
                if (vis_scen[sc]) printf("  %s=%d", n2_scen_name(sc), vis_scen[sc]);
            printf("\n");
            if (mapaudit) {
                printf("BAND policy: fog density %.5f -> ordinary cutoff %.1f m "
                       "(1%% contribution), far plane %.1f m\n",
                       g_dbg.fog_density, (double)VIEW_DIST, zfar);
                static const char *BN[6] = {"0-250","250-700","700-1000",
                                            "1000-1500","1500-2000",">2000"};
                for (int bd = 0; bd < 6; bd++) {
                    printf("BAND %-10s batches %5ld  meshes %6ld ", BN[bd],
                           band_b[bd], band_m[bd]);
                    for (int sc=1;sc<=N2_SC_OTHER;sc++)
                        if (band_sc[bd][sc]) printf(" %s=%ld",n2_scen_name(sc),band_sc[bd][sc]);
                    printf("\n");
                }
                printf("MAP rejected by %.0f m XY distance: %d meshes in %d batches",
                       (double)VIEW_DIST,far_meshes,far_batches);
                for (int sc=1;sc<=N2_SC_OTHER;sc++)
                    if (far_scen[sc]) printf("  %s=%d",n2_scen_name(sc),far_scen[sc]);
                printf("\n");
            }
            printf("camera XYZ: (%.1f, %.1f, %.1f)  looking at car (%.1f, %.1f, %.1f)\n",
                   cam[0], cam[1], cam[2], carpos[0], carpos[1], carpos[2]);
            if (sstatic) {
                static const char *pn[4] = { "full", "opaque", "glow", "sky" };
                char pbuf[32]; const char *pl = pn[passmode];
                if (passbatch >= 0) { snprintf(pbuf, sizeof pbuf, "opaque:%d-%d", passbatch, passbatch2); pl = pbuf; }
                printf("static: track=%s pass=%s car=(%.3f, %.3f, %.3f) cam=(%.3f, %.3f, %.3f) "
                       "heading=%.4f frames=%d world_batches_drawn=%d/%d sky=%d glow=%d\n",
                       trackname, pl, carpos[0], carpos[1], carpos[2],
                       cam[0], cam[1], cam[2], heading, shotframe, wbdrawn, nbatch,
                       (passmode == 0 || passmode == 3 || passbatch >= 0) ? nsky : 0,
                       (passmode == 0 || passmode == 2) ? nglow : 0);
                if (passmode == 1) {   /* M79: also lists the members of a range */
                    /* visible opaque batches at this exact camera, widest first */
                    for (int a = 0; a < nviskept; a++)      /* insertion sort: <=4096 */
                        for (int b2 = a+1; b2 < nviskept; b2++) {
                            N2Batch *A = &wbatch[viskept[a]], *B = &wbatch[viskept[b2]];
                            float sa = A->bbox_max[0]-A->bbox_min[0];
                            float ta = A->bbox_max[1]-A->bbox_min[1]; if (ta > sa) sa = ta;
                            float sb = B->bbox_max[0]-B->bbox_min[0];
                            float tb = B->bbox_max[1]-B->bbox_min[1]; if (tb > sb) sb = tb;
                            if (sb > sa) { int t = viskept[a]; viskept[a] = viskept[b2]; viskept[b2] = t; }
                        }
                    printf("visible opaque batches: %d (widest 20)\n", nviskept);
                    printf("%5s %5s %9s  %-34s %-34s %8s %8s\n",
                           "batch","nmesh","texkey","bbox min XYZ","bbox max XYZ","XYspan","Zspan");
                    for (int a = 0; a < nviskept && a < 20; a++) {
                        N2Batch *b3 = &wbatch[viskept[a]];
                        float sx = b3->bbox_max[0]-b3->bbox_min[0];
                        float sy = b3->bbox_max[1]-b3->bbox_min[1];
                        char lo[40], hi[40];
                        snprintf(lo, sizeof lo, "%10.1f %10.1f %9.1f",
                                 b3->bbox_min[0], b3->bbox_min[1], b3->bbox_min[2]);
                        snprintf(hi, sizeof hi, "%10.1f %10.1f %9.1f",
                                 b3->bbox_max[0], b3->bbox_max[1], b3->bbox_max[2]);
                        printf("%5d %5d  %08x  %-34s %-34s %8.1f %8.1f\n",
                               viskept[a], b3->nmesh, b3->texkey, lo, hi,
                               sx > sy ? sx : sy, b3->bbox_max[2]-b3->bbox_min[2]);
                    }
                }
            }
            if (raudit || daudit) {
                for (int sc=WSURF_ROAD; sc<=WSURF_TERRAIN; sc++) if (ca_frames[sc]) {
                    printf("CONTACT %-7s frames=%ld patch-ok=%ld patch-held=%ld "
                           "missing-probes=%ld mixed-layer-probes=%ld\n",
                           sc==WSURF_ROAD?"ROAD":"TERRAIN",ca_frames[sc],
                           ca_patch_ok[sc],ca_patch_bad[sc],ca_missing[sc],ca_mixed[sc]);
                    if (ca_fmin[sc]<1e20f)
                        printf("CONTACT %-7s rendered tyre-bottom residual: "
                               "front[%+.3f..%+.3f] rear[%+.3f..%+.3f] "
                               "max axle mismatch %.3f m  (negative=sunk)\n",
                               sc==WSURF_ROAD?"ROAD":"TERRAIN",
                               ca_fmin[sc],ca_fmax[sc],ca_rmin[sc],ca_rmax[sc],
                               ca_axlemax[sc]);
                }
            }
            { float f[3]={cosf(heading),sinf(heading),0};   /* pitch/roll from car_up */
              float d=f[0]*car_up[0]+f[1]*car_up[1]+f[2]*car_up[2];
              f[0]-=d*car_up[0]; f[1]-=d*car_up[1]; f[2]-=d*car_up[2];
              float fl2=sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); if(fl2>1e-6f){f[0]/=fl2;f[1]/=fl2;f[2]/=fl2;}
              float lft[3]={car_up[1]*f[2]-car_up[2]*f[1], car_up[2]*f[0]-car_up[0]*f[2], car_up[0]*f[1]-car_up[1]*f[0]};
              printf("chassis: up(%.3f,%.3f,%.3f) pitch=%+.1fdeg roll=%+.1fdeg  (ground grade %.1fdeg)\n",
                     car_up[0],car_up[1],car_up[2], asinf(f[2])*57.2958f, asinf(lft[2])*57.2958f,
                     acosf(car_up[2]>1?1:car_up[2])*57.2958f); }
            printf("wheels: radius %.3f m, %.0f RPM, steer %+.1f deg, %.0f km/h\n",
                   g_dbg.wheel_radius, g_dbg.wheel_rpm, g_dbg.steer_deg, g_dbg.kmh);
            {   float sc = g_dbg.wheel_scale > 0.05f ? g_dbg.wheel_scale : 1.0f;
                float rd = carprof.ride * sc;
                printf("profile %-11s R %.3f%s  ride %.3f  hubZ %+.3f  wheelbase %.2f"
                       "  track %.2f  tyre bottom %+.3f (0=flush)  clearance %.3f\n",
                       carprof.name, carprof.wheel_r, carprof.has_tire ? "" : "*",
                       rd, carprof.hub_z, carprof.wheelbase, carprof.track_f,
                       rd + g_dbg.wheel.ride_y - g_dbg.wheel_radius,
                       rd + carprof.body[4]); }
            printf("wrote %s (%dx%d) after driving to (%.0f,%.0f)\n", shot, W, H, carpos[0], carpos[1]);
            running = 0;
        }
        POST_RESOLVE();
        SDL_GL_SwapWindow(win);
    }

#ifdef DEBUG_UI
    dbgui_shutdown();
#endif
    n2_free_scene(&scene);   /* region buffers already freed after texture upload */
    free(wmbatch);           /* wraps wbatch's GL handles; frees the array only */
    if (dbgprog) glDeleteProgram(dbgprog);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
