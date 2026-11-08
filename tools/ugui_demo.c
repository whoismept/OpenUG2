#include "ugui_glyph_folder.h"
/* ugui_demo.c -- SDL2 + OpenGL demo window for the ugui widget library.
 *
 * Everything is operable with the mouse alone:
 *   hover  = focus      left click = Accept     right click = Back
 *   wheel  = scroll / cycle values              drag = sliders and scroll thumbs
 * Keyboard (arrows, Enter, Esc, F1, [ ], typing) is a fallback.
 *
 * Usage: ugui_demo [--data DIR] [--lang NAME] [--size W H] [--vectors DIR] [--page N]
 *                  [--screen FILE.fng] [--list] [--check|--demo-check] [--pillbox] [--shots DIR] [--frames N]
 *   --data    the game install (default: $UGUI_DATA, $NFSU2_DATA, ./). With data the
 *             "Original screens" page shows the real screens with the real art and
 *             fonts. Missing required resources produce a diagnostic and exit.
 *   --screen  open one original screen straight away (e.g. UI_Main.fng)
 *   --list    print the screens found in the install and exit (no window)
 *   --check   deterministic headless-style run: visits every page and dialog and
 *             every original screen, verifies there are no GL errors, writes BMP
 *             screenshots and original_report.txt to --shots.
 *   --pillbox render a non-4:3 window and verify that everything outside the centred 4:3 viewport
 *             is pure black, the pointer cursor included (SPEC 1.1, R); use with a non-4:3 --size.
 *   --frames  run N frames then exit (smoke test).
 *
 * The virtual screen is shown in a centred 4:3 viewport at any window size (scale min(W/640, H/480));
 * the side bars are black and nothing, not even the cursor, is drawn into them. */
#include "../src/rhi_gl_state.h"

#include "ugui_demo_app.h"
#include "ugui_gl.h"
#include "ugui_tuning.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Window *win;
    SDL_GLContext gl;
    UgGL *ugl;
    DemoApp app;
    int w, h;
    int gl_errors, frames;
    const char *shot_dir;
    int check, demo_only;
} Env;

/* ---- screenshots ---- */
static float g_last_ink;     /* fraction of pixels clearly brighter than the backdrop in the last shot */

static int save_shot(Env *e, const char *name) {
    int w = e->w, h = e->h;
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
    uint8_t *flip = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
    if (!px || !flip) { free(px); free(flip); return 0; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++) memcpy(flip + (size_t)y * w * 3, px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
    {
        size_t ink = 0;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            if (flip[3 * i] > 70 || flip[3 * i + 1] > 70 || flip[3 * i + 2] > 70) ink++;
        g_last_ink = (float)ink / (float)((size_t)w * (size_t)h);
    }
    if (!e->shot_dir) { free(px); free(flip); return 0; }
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom(flip, w, h, 24, w * 3, 0x0000FF, 0x00FF00, 0xFF0000, 0);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.bmp", e->shot_dir, name);
    int ok = s && SDL_SaveBMP(s, path) == 0;
    if (s) SDL_FreeSurface(s);
    free(px); free(flip);
    if (ok) printf("  screenshot %s\n", path);
    return ok;
}

/* ---- one frame ---- */
static void render(Env *e) {
    SDL_GL_GetDrawableSize(e->win, &e->w, &e->h);
    demo_draw(&e->app);
    glViewport(0, 0, e->w, e->h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ug_gl_render(e->ugl, &e->app.ui.cv, e->w, e->h);
    e->gl_errors += ug_gl_errors("frame");
    e->frames++;
}

static void map_mouse(Env *e, int mx, int my, float *x, float *y) {
    int ww, wh;
    SDL_GetWindowSize(e->win, &ww, &wh);
    /* window coords -> drawable coords (hi-dpi) -> virtual screen */
    float sx = e->w > 0 && ww > 0 ? (float)e->w / (float)ww : 1.0f;
    float sy = e->h > 0 && wh > 0 ? (float)e->h / (float)wh : 1.0f;
    UgView v = ug_view_fit(e->w, e->h);
    ug_view_to_virtual(&v, (int)(mx * sx), (int)(my * sy), x, y);
}

static void ptr(Env *e, int type, float x, float y, int button, int wheel) {
    /* the black bars are not part of the screen: clicks and wheel turns there do nothing, the cursor is hidden */
    int vis = ug_virtual_visible(x, y);
    if (!vis && (type == UG_PT_DOWN || type == UG_PT_WHEEL)) return;
    UgPtr p = {type, x, y, button, wheel};
    ug_ui_pointer(&e->app.ui, &p);
}

/* ---- scripted tour used by --check ---- */
static void step(Env *e, int frames, int shot_every) {
    (void)shot_every;
    for (int i = 0; i < frames; i++) {
        demo_update(&e->app, 16.0f);
        render(e);
        /* the last frame of a step stays in the back buffer so a screenshot reads it */
        if (i + 1 < frames || !e->check) SDL_GL_SwapWindow(e->win);
    }
}

/* Amendment 8 final-frame glyph oracle. Compare the actual fully composited
 * framebuffer with the same canvas omitting only the checked texture. This
 * detects invisible/covered glyphs despite successful resource status. No
 * original pixels are exported separately or used as golden artwork. */
static int glyph_frame_ink(Env *e,int texture,const char *context,uint32_t key,const char *capture) {
    const UgCanvas *cv=&e->app.ui.cv;size_t bytes=(size_t)e->w*e->h*3;
    uint8_t *full=malloc(bytes),*without=malloc(bytes);
    UgCanvas reduced=*cv;reduced.n=0;reduced.v=malloc((size_t)cv->n*sizeof *cv->v);
    if(!full||!without||!reduced.v){free(full);free(without);free(reduced.v);return 0;}
    glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,e->w,e->h,GL_RGB,GL_UNSIGNED_BYTE,full);
    for(int i=0;i<cv->n;i++)if(cv->v[i].tex!=texture)reduced.v[reduced.n++]=cv->v[i];
    glDisable(GL_SCISSOR_TEST);glClear(GL_COLOR_BUFFER_BIT);ug_gl_render(e->ugl,&reduced,e->w,e->h);
    glReadPixels(0,0,e->w,e->h,GL_RGB,GL_UNSIGNED_BYTE,without);
    int x0=e->w,y0=e->h,x1=-1,y1=-1;size_t count=0;
    for(int y=0;y<e->h;y++)for(int x=0;x<e->w;x++){
        size_t at=((size_t)y*e->w+x)*3;int changed=0;
        for(int ch=0;ch<3;ch++)if(abs((int)full[at+ch]-(int)without[at+ch])>1)changed=1;
        if(changed){int top=e->h-1-y;if(x<x0)x0=x;if(x>x1)x1=x;if(top<y0)y0=top;if(top>y1)y1=top;count++;}
    }
    glDisable(GL_SCISSOR_TEST);glClear(GL_COLOR_BUFFER_BIT);ug_gl_render(e->ugl,cv,e->w,e->h);
    e->gl_errors+=ug_gl_errors("gui final-frame glyph check");
    printf("gui glyph provenance: family=%s key=%08x source_kind=%s source=%s\n",e->app.controller.family==UG_CONTROLLER_PS2?"PS2":"Xbox",key,e->app.controller.folder_active?"TEST-ONLY PNG folder":"ISO",texture>0&&texture<=e->app.assets.tex.n?e->app.assets.tex.t[texture-1].source:"unavailable");
    printf("gui ink: %dx%d family=%s context=%s key=%08x pixels=%zu bounds=%d,%d..%d,%d capture=%s status=%s\n",
        e->w,e->h,e->app.controller.family==UG_CONTROLLER_PS2?"PS2":"Xbox",context,key,count,x0,y0,x1,y1,capture,count?"visible":"FAIL invisible");
    free(full);free(without);free(reduced.v);return count>0;
}

static int run_gui_check(Env *e) {
    DemoApp *a=&e->app;int failed=0;
    printf("gui source kind: %s\n",a->controller.folder_active?"TEST-ONLY PNG folder; does not pass final ISO acceptance":"ISO; final console acceptance requires each family ISO");
    step(e,90,0);while(a->ui.sp>1){ug_ui_action(&a->ui,UG_A_BACK);step(e,90,0);}
    const UgControllerFamily families[]={UG_CONTROLLER_PC,UG_CONTROLLER_PS2,UG_CONTROLLER_XBOX,UG_CONTROLLER_PC};
    for(int f=0;f<4;f++) {
        UgControllerFamily family=families[f];demo_select_controller(a,family);
        demo_open_page(a,DP_GLYPHS);step(e,90,0);
        char shot[64];snprintf(shot,sizeof shot,"gui_gallery_%d_%d",family,f);
        if(!save_shot(e,shot))failed++;
        printf("gui profile: family=%d mode=%d status=%s capture=%s\n",family,a->ui.hints.mode,a->controller.status,shot);
        if(family==UG_CONTROLLER_PC){if(a->ui.hints.mode!=UG_HINTS_PC)failed++;}
        else if(!ug_controller_family_available(&a->controller,family)) {
            printf("gui resources: %s unavailable; resource-backed acceptance NOT PASSED\n",family==UG_CONTROLLER_PS2?"PS2":"Xbox");failed++;
        } else {
            int n=ug_controller_family_icon_count(family);
            for(int i=0;i<n;i++){
                int tex=ug_controller_family_icon_texture(&a->controller,family,i);
                if(!tex){printf("gui gallery: family=%d key=%08x status=missing/unsupported\n",family,ug_controller_family_icon_key(family,i));continue;}
                if(!glyph_frame_ink(e,tex,"gallery",ug_controller_family_icon_key(family,i),shot))failed++;
            }
            /* Explicit test-only D maps, never asserted as retail bindings. */
            const uint32_t keys[2]={family==UG_CONTROLLER_PS2?0xe795717du:0x8522e0a0u,family==UG_CONTROLLER_PS2?0xd9a22505u:0x8522e0a1u};
            const char *tokens[2]={"ACCEPT","CANCEL"};
            for(int i=0;i<2;i++)ug_controller_bind(&a->controller,tokens[i],keys[i]);
            ug_ui_action(&a->ui,UG_A_BACK);step(e,90,0);
            demo_open_page(a,DP_CARS);step(e,90,0);
            snprintf(shot,sizeof shot,"gui_strip_%d",family);if(!save_shot(e,shot))failed++;
            for(int i=0;i<2;i++){
                int tex=0;UgVisualStatus status=ug_controller_token(&a->controller,tokens[i],&tex);
                printf("gui mapping: family=%d action=%s key=%08x status=%d (explicit D test map)\n",family,tokens[i],keys[i],status);
                if(status==UG_VIS_OK&&!glyph_frame_ink(e,tex,tokens[i],keys[i],shot))failed++;
                else if(status!=UG_VIS_OK)failed++;
            }
        }
        ug_ui_action(&a->ui,UG_A_BACK);step(e,90,0);
    }
    printf("gui %scheck: %d failures, %d GL errors; captures require native-size inspection%s\n",a->controller.folder_active?"TEST-ONLY folder ":"ISO ",failed,e->gl_errors,a->controller.folder_active?"; not final ISO acceptance":"");
    return e->gl_errors?2:failed?1:0;
}

static void click_at(Env *e, UgRect r) {
    float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
    ptr(e, UG_PT_MOVE, x, y, 0, 0);
    step(e, 2, 0);
    ptr(e, UG_PT_DOWN, x, y, 1, 0);
    ptr(e, UG_PT_UP, x, y, 1, 0);
}

/* ---- every original screen: open, let the enter animation play, shoot, close ---- */
static void safe_name(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; *src && n + 1 < cap; src++) {
        char c = *src;
        if (c == '.') break;
        dst[n++] = (char)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_');
    }
    dst[n] = 0;
}

static int run_original_tour(Env *e) {
    DemoApp *a = &e->app;
    if (!a->have_assets) {
        printf("check: no game data: original screens skipped (%s)\n", a->assets_msg);
        return 0;
    }
    char path[600];
    FILE *rep = NULL;
    if (e->shot_dir) {
        snprintf(path, sizeof path, "%s/original_report.txt", e->shot_dir);
        rep = fopen(path, "w");
    }
    int n = ug_assets_package_count(&a->assets), bad = 0, suppressed = 0, blank = 0, withmissing = 0;
    printf("check: %d original screens (%s)\n", n, a->assets_msg);
    if (rep) fprintf(rep, "# screen objects missing_textures ink%% status\n");
    ptr(e, UG_PT_MOVE, 0, -134, 0, 0);
    demo_open_page(a, DP_ORIGINAL);
    step(e, 70, 0);
    save_shot(e, "orig_browser");
    for (int i = 0; i < n; i++) {
        const UgAssetPackage *p = ug_assets_package_at(&a->assets, i);
        if (!demo_open_original(a, i)) { step(e, 30, 0); demo_open_original(a, i); }
        step(e, 100, 0);
        DemoViewer *v = &a->viewer.vw;
        char nm[96], shot[128];
        safe_name(nm, sizeof nm, p->name);
        snprintf(shot, sizeof shot, "orig_%03d_%s", i, nm);
        save_shot(e, shot);
        const char *status = "ok";
        if (v->suppressed) { status = "intentionally suppressed (console profile)"; suppressed++; }
        else if (!v->loaded) { status = "LOAD FAILED"; bad++; }
        else if (g_last_ink < 0.002f) { status = "blank"; blank++; }
        else if (g_last_ink < 0.03f) status = "sparse (little beyond the hint bar: movie / program-supplied content)";
        if (v->loaded && v->missing_tex) withmissing++;
        if (rep) fprintf(rep, "%-34s %5d %4d %6.2f  %s%s%s\n", p->name, v->objects, v->missing_tex, (double)g_last_ink * 100.0,
                         status, v->loaded ? "" : "  ", v->loaded ? "" : v->err);
        if (v->suppressed) printf("  SUPPRESSED %s: %s\n", p->name, v->err);
        else if (!v->loaded) printf("  FAIL %s: %s\n", p->name, v->err);
        /* leave through the exit animation (Back) */
        ptr(e, UG_PT_DOWN, 0, 0, 2, 0);
        ptr(e, UG_PT_UP, 0, 0, 2, 0);
        step(e, 80, 0);
        if (orig_viewer_active(a)) step(e, 60, 0);
    }
    printf("check: original screens: %d loaded, %d intentionally suppressed, %d failed to load, %d blank, %d with unresolved textures\n",
           n - bad - suppressed, suppressed, bad, blank, withmissing);
    if (rep) fclose(rep);

    /* animations of one real screen: exit, dim, undim */
    const char *probe = "UI_Main.fng";
    if (ug_assets_package(&a->assets, probe) && demo_open_original_named(a, probe)) {
        step(e, 100, 0);
        for (int k = 0; k < 3; k++) { ug_ui_action(&a->ui, UG_A_RIGHT); step(e, 2, 0); }
        step(e, 40, 0);
        save_shot(e, "orig_UI_Main_right3");
        ptr(e, UG_PT_MOVE, 128, -134, 0, 0);          /* hover the next item, then click it */
        step(e, 20, 0);
        ptr(e, UG_PT_MOVE, 150, -134, 0, 0);
        step(e, 40, 0);
        save_shot(e, "orig_UI_Main_hover_next");
        ug_ui_action(&a->ui, UG_A_BTN2);              /* dim request to the screen's controller */
        step(e, 3, 0);
        save_shot(e, "orig_UI_Main_dim_mid");
        step(e, 30, 0);
        save_shot(e, "orig_UI_Main_dimmed");
        ug_ui_action(&a->ui, UG_A_BTN3);
        step(e, 40, 0);
        ug_ui_action(&a->ui, UG_A_BTN1);              /* exit request */
        step(e, 4, 0);
        save_shot(e, "orig_UI_Main_exit_mid");
        step(e, 60, 0);
        save_shot(e, "orig_UI_Main_exited");
        ug_ui_action(&a->ui, UG_A_BTN0);              /* replay the enter animation */
        step(e, 5, 0);
        save_shot(e, "orig_UI_Main_enter_mid");
        step(e, 80, 0);
        ptr(e, UG_PT_DOWN, 0, 0, 2, 0);
        ptr(e, UG_PT_UP, 0, 0, 2, 0);
        step(e, 100, 0);
    }
    /* real buttons of a real dialog: hover (focus script), click (pressed -> application message) */
    if (ug_assets_package(&a->assets, "GenericDialog_SMALL.fng") && demo_open_original_named(a, "GenericDialog_SMALL.fng")) {
        step(e, 100, 0);
        ptr(e, UG_PT_MOVE, 150, 74, 0, 0);
        step(e, 30, 0);
        save_shot(e, "orig_dialog_hover_no");
        ptr(e, UG_PT_MOVE, -150, 74, 0, 0);
        step(e, 30, 0);
        save_shot(e, "orig_dialog_hover_yes");
        ptr(e, UG_PT_DOWN, -150, 74, 1, 0);
        ptr(e, UG_PT_UP, -150, 74, 1, 0);
        step(e, 40, 0);
        save_shot(e, "orig_dialog_click_yes");
        printf("check: dialog click -> last application message %08x (%d messages)\n", a->viewer.vw.last_msg, a->viewer.vw.n_msgs);
        ptr(e, UG_PT_DOWN, 0, 0, 2, 0);
        ptr(e, UG_PT_UP, 0, 0, 2, 0);
        step(e, 100, 0);
    }
    /* back to the root page */
    ptr(e, UG_PT_DOWN, 0, 0, 2, 0);
    ptr(e, UG_PT_UP, 0, 0, 2, 0);
    step(e, 80, 0);
    return bad || withmissing ? 3 : 0;
}

/* SPEC 1.1 (R): in a non-4:3 window the frame is drawn into a centred 4:3 viewport; the bars are pure black
 * (0,0,0) and nothing, not even the pointer cursor, is drawn into them. Draws the root page with the cursor
 * at the right-hand edge of the screen, reads the frame back and checks every pixel outside the viewport. */
static int run_pillbox_check(Env *e) {
    DemoApp *a = &e->app;
    step(e, 20, 0);
    SDL_GL_GetDrawableSize(e->win, &e->w, &e->h);
    UgView v = ug_view_fit(e->w, e->h);
    printf("pillbox: window %dx%d, viewport %dx%d at (%d,%d), scale %.4f\n", e->w, e->h, v.w, v.h, v.x0, v.y0, (double)v.scale);
    if (v.x0 == 0 && v.y0 == 0) {
        printf("pillbox: the window is 4:3, nothing to check (use a non-4:3 --size)\n");
        return 0;
    }
    ptr(e, UG_PT_MOVE, 312, 40, 0, 0);                      /* the cursor straddles the right edge of the viewport */
    a->ui.ctx.px = 312; a->ui.ctx.py = 40; a->ui.ctx.pointer_visible = 1;
    step(e, 3, 0);
    int w = e->w, h = e->h;
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
    if (!px) return 1;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    size_t outside = 0, nonblack = 0, inside_ink = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int fy = h - 1 - y;                              /* GL rows start at the bottom */
            const uint8_t *q = px + ((size_t)y * (size_t)w + (size_t)x) * 3;
            int in = ug_view_contains(&v, x, fy);
            if (!in) { outside++; if (q[0] || q[1] || q[2]) nonblack++; }
            else if (q[0] > 70 || q[1] > 70 || q[2] > 70) inside_ink++;
        }
    free(px);
    printf("pillbox: %zu pixels outside the viewport, %zu not black; %zu bright pixels inside\n", outside, nonblack, inside_ink);
    int ok = nonblack == 0 && inside_ink > 100;
    printf("pillbox: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 4;
}

static int run_check(Env *e) {
    DemoApp *a = &e->app;
    char name[64];
    step(e, 50, 0);
    save_shot(e, "00_root");
    for (int i = 0; i < DP_COUNT - 1; i++) {
        /* select the icon with the mouse (hover), then click it */
        ptr(e, UG_PT_MOVE, 0, -134, 0, 0);
        if (i > 0) ptr(e, UG_PT_WHEEL, 0, -134, 0, -1);      /* wheel = next item */
        step(e, 30, 0);
        ptr(e, UG_PT_DOWN, 0, -134, 1, 0);
        ptr(e, UG_PT_UP, 0, -134, 1, 0);
        step(e, 70, 0);
        snprintf(name, sizeof name, "%02d_page_%s", i + 1, demo_page_name(i));
        for (char *c = name; *c; c++) if (*c == ' ' || *c == '&' || *c == '(' || *c == ')' || *c == '.') *c = '_';
        save_shot(e, name);
        /* page specific interaction shots */
        if(i==DP_ORIGINAL) {
            DemoBrowser *b=&a->orig.br;
            for(int k=0;k<b->n;k++)ug_ui_action(&a->ui,UG_A_DOWN);
            step(e,30,0);save_shot(e,"browser_last");
            for(int k=0;k<b->n;k++)ug_ui_action(&a->ui,UG_A_UP);
            step(e,30,0);save_shot(e,"browser_first");
        }
        if(i==DP_CONTROLS) {
            click_at(e,a->controls.rand_btn.w.bounds);step(e,30,0);save_shot(e,"meters_changed");
            click_at(e,a->controls.pill_btn.w.bounds);step(e,10,0);save_shot(e,"action_result");
        }
        if(i==DP_ENTRY) {
            ug_ui_text(&a->ui,"Demo");step(e,10,0);save_shot(e,"keyboard_typed");
            click_at(e,a->entry.kb.keys[0].r);step(e,10,0);save_shot(e,"keyboard_clicked");
        }
        if(i==DP_DATE) {
            ug_ui_action(&a->ui,UG_A_UP);step(e,10,0);save_shot(e,"date_changed");
            ug_ui_action(&a->ui,UG_A_ACCEPT);step(e,10,0);save_shot(e,"date_accepted");
        }
        if(i==DP_TABLE) {
            ug_ui_action(&a->ui,UG_A_DOWN);step(e,20,0);save_shot(e,"table_selection");
        }
        if(i==DP_CARS) {
            for(int k=0;k<10;k++)ug_ui_action(&a->ui,UG_A_RIGHT);
            step(e,40,0);save_shot(e,"car_last");
            for(int k=0;k<10;k++)ug_ui_action(&a->ui,UG_A_LEFT);
            step(e,40,0);save_shot(e,"car_first");
        }
        if(i==DP_DIALOGS) {
            for(int k=0;k<5;k++) {
                click_at(e,a->dialogs.b[k].w.bounds);step(e,30,0);
                char shot[64];snprintf(shot,sizeof shot,"dialog_variant_%d_rest",k);save_shot(e,shot);
                ug_ui_action(&a->ui,UG_A_LEFT);step(e,1,0);
                snprintf(shot,sizeof shot,"dialog_variant_%d_focus_start",k);save_shot(e,shot);
                step(e,75,0);snprintf(shot,sizeof shot,"dialog_variant_%d_focus_end",k);save_shot(e,shot);
                ug_ui_action(&a->ui,k&1?UG_A_BACK:UG_A_ACCEPT);step(e,60,0);
            }
            save_shot(e,"dialogs_chrome_restored");
            click_at(e,a->dialogs.b[5].w.bounds);step(e,40,0);save_shot(e,"help_popup");
            ug_ui_action(&a->ui,UG_A_BACK);step(e,60,0);
        }
        if(i==DP_ANIMS) {
            ug_ui_action(&a->ui,UG_A_RIGHT);step(e,20,0);save_shot(e,"animation_selected");
        }
        if(i==DP_PACKAGE) {
            click_at(e,a->package.example.w.bounds);step(e,20,0);save_shot(e,"package_button_result");
            printf("demo example: %d observable activations\n",a->package.sw.total);
        }
        if(i==DP_GLYPHS) {
            UgControllerFamily saved=a->controller.family;
            const UgControllerFamily profiles[]={UG_CONTROLLER_PC,UG_CONTROLLER_PS2,UG_CONTROLLER_XBOX,UG_CONTROLLER_PC};
            const char *names[]={"family_pc","family_ps2","family_xbox","family_pc_restored"};
            for(int k=0;k<4;k++){demo_select_controller(a,profiles[k]);step(e,15,0);save_shot(e,names[k]);printf("demo family: %s\n",a->controller.status);}
            demo_select_controller(a,saved);
        }
        if(i==DP_PCCTL) {
            ug_ui_action(&a->ui,UG_A_RIGHT);step(e,10,0);save_shot(e,"controls_binding_changed");
            ug_ui_action(&a->ui,UG_A_BTN1);step(e,10,0);save_shot(e,"controls_binding_reset");
        }
        if (i == DP_OPTIONS) {
            ptr(e, UG_PT_MOVE, -100, 40, 0, 0);
            step(e, 20, 0);
            ptr(e, UG_PT_WHEEL, 150, 40, 0, 1);
            step(e, 20, 0);
            save_shot(e, "options_hover_wheel");
        }
        ptr(e, UG_PT_DOWN, 0, 0, 2, 0);          /* right click = Back */
        ptr(e, UG_PT_UP, 0, 0, 2, 0);
        step(e, 70, 0);
    }
    int orig_rc = e->demo_only?0:run_original_tour(e);
    printf("check: %d frames, %d GL errors\n", e->frames, e->gl_errors);
    if (e->gl_errors) return 2;
    return orig_rc;
}

int main(int argc, char **argv) {
    int w = 960, h = 720, check = 0, max_frames = -1, page = -1, list = 0, pillbox = 0, demo_only=0,gui_check=0;
    const char *dump = NULL, *xbox_iso=NULL, *ps2_iso=NULL, *family="pc", *capture=NULL, *test_glyph_folder=NULL;
    const char *bindings[32];int nbindings=0, audit=0, diagnostics=0;
    const char *vectors = "tools/ugui_vectors";
    const char *shots = NULL, *data = NULL, *lang = NULL, *screen = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 2 < argc) { w = atoi(argv[++i]); h = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--vectors") && i + 1 < argc) vectors = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if(!strcmp(argv[i],"--demo-check")){check=1;demo_only=1;}
        else if(!strcmp(argv[i],"--gui-check")){check=1;gui_check=1;}
        else if (!strcmp(argv[i], "--pillbox")) { check = 1; pillbox = 1; }
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) lang = argv[++i];
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) screen = argv[++i];
        else if(!strcmp(argv[i],"--diagnostics"))diagnostics=1;
        else if(!strcmp(argv[i],"--glyph-folder")){fprintf(stderr,"PNG folder loading requires explicit test-only enablement: --test-glyph-folder DIR\n");return 1;}
        else if(!strcmp(argv[i],"--test-glyph-folder")&&i+1<argc)test_glyph_folder=argv[++i];
        else if(!strcmp(argv[i],"--xbox-iso")&&i+1<argc)xbox_iso=argv[++i];
        else if (!strcmp(argv[i], "--ps2-iso") && i+1<argc) ps2_iso=argv[++i];
        else if (!strcmp(argv[i], "--controller") && i+1<argc) family=argv[++i];
        else if (!strcmp(argv[i], "--bind") && i+1<argc && nbindings<32) bindings[nbindings++]=argv[++i];
        else if (!strcmp(argv[i], "--capture") && i+1<argc) {capture=argv[++i];check=1;}
        else if (!strcmp(argv[i], "--audit")) audit=1;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--data DIR] [--lang NAME] [--size W H] [--vectors DIR] [--page N] [--screen FILE.fng]\n"
                            "          [--list] [--check|--demo-check|--gui-check] [--shots DIR] [--frames N] [--capture NAME]\n"
                            "          [--ps2-iso FILE] [--xbox-iso FILE] [--test-glyph-folder DIR (testing only; never final ISO acceptance)] [--diagnostics] [--controller pc|ps2|xbox] [--bind TOKEN=ICON_NAME] [--audit]\n", argv[0]);
            return 1;
        }
    }

    if(strcmp(family,"pc") && strcmp(family,"ps2") && strcmp(family,"xbox")){fprintf(stderr,"controller must be pc, ps2 or xbox\n");return 1;}
    if (dump) {          /* diagnostic: object table of one package as the scene runtime sees it */
        char dir[512], msg[256];
        const char *found = ug_assets_find_dir(data, dir, sizeof dir);
        UgAssets as;
        if (!found || !ug_assets_open(&as, found, lang, msg, sizeof msg)) { fprintf(stderr, "no game data\n"); return 1; }
        const UgAssetPackage *p = ug_assets_package(&as, dump);
        UgFngPackage pk;
        char err[160];
        if (!p || !ug_fng_parse(p->data, p->len, &pk, err, sizeof err)) { fprintf(stderr, "cannot load %s\n", dump); return 1; }
        UgScene sc;
        ug_scene_init(&sc, &pk, &as.strings);
        ug_scene_set_assets(&sc, &as);
        for (int i = 0; i < 400; i++) ug_scene_update(&sc, 16);
        printf("%s: %d objects, %d resources\n", p->name, pk.nobj, pk.nres);
        for (int i = 0; i < pk.nobj; i++) {
            const UgFngObject *o = &pk.obj[i];
            const UgNode *n = &sc.nodes[i];
            const char *rn = o->res_index >= 0 && o->res_index < pk.nres ? pk.res[o->res_index].name : "";
            printf("%3d t%-2u g%-5u p%-5d f%08x w(%7.1f,%7.1f) sz(%6.1f,%6.1f) ws(%5.2f,%5.2f) z%6.1f a%3d vis%d tex%d uv(%.2f,%.2f,%.2f,%.2f) %s%s%s\n", i, o->type, o->guid,
                   n->parent >= 0 ? (int)pk.obj[n->parent].guid : -1, o->flags, n->we, n->wf,
                   n->props.w[UG_P_SIZE], n->props.w[UG_P_SIZE + 1],
                   sqrtf(n->wa * n->wa + n->wb * n->wb), sqrtf(n->wc * n->wc + n->wd * n->wd), n->wz, n->wcolor.a, n->visible, n->tex,
                   n->props.w[UG_P_UV0], n->props.w[UG_P_UV0 + 1], n->props.w[UG_P_UV1], n->props.w[UG_P_UV1 + 1], rn,
                   o->text ? "  \"" : "", o->text ? o->text : "");
        }
        for (int i = 0; i < pk.nobj; i++) {
            const UgFngObject *o = &pk.obj[i];
            for (int r = 0; r < o->nresp; r++) {
                printf("  resp obj %d g%u msg %08x:", i, o->guid, o->resp[r].msg);
                for (int k = 0; k < o->resp[r].ncmds; k++)
                    printf(" [%x %x t%u]", o->resp[r].cmds[k].id, o->resp[r].cmds[k].arg, o->resp[r].cmds[k].target);
                printf("\n");
            }
            for (int sc2 = 0; sc2 < o->nscripts; sc2++)
                printf("  script obj %d id %08x len %u flags %u chain %d tracks %d events %d\n", i, o->scripts[sc2].id,
                       o->scripts[sc2].length, o->scripts[sc2].flags, o->scripts[sc2].has_chain, o->scripts[sc2].ntracks, o->scripts[sc2].nevents);
        }
        for (int r = 0; r < pk.npkg_resp; r++) {
            printf("  pkg resp msg %08x:", pk.pkg_resp[r].msg);
            for (int k = 0; k < pk.pkg_resp[r].ncmds; k++)
                printf(" [%x %x t%u]", pk.pkg_resp[r].cmds[k].id, pk.pkg_resp[r].cmds[k].arg, pk.pkg_resp[r].cmds[k].target);
            printf("\n");
        }
        return 0;
    }
    if (list) {          /* no window: just exercise the loader */
        char dir[512], msg[256];
        const char *found = ug_assets_find_dir(data, dir, sizeof dir);
        if (!found) { fprintf(stderr, "no game data found (use --data DIR or $UGUI_DATA)\n"); return 1; }
        UgAssets as;
        int ok = ug_assets_open(&as, found, lang, msg, sizeof msg);
        printf("%s: %s\n", found, msg);
        if (as.log[0]) printf("%s", as.log);
        for (int i = 0; ok && i < ug_assets_package_count(&as); i++) {
            const UgAssetPackage *p = ug_assets_package_at(&as, i);
            UgFngPackage pk;
            char err[160];
            int pok = ug_fng_parse(p->data, p->len, &pk, err, sizeof err);
            int missing = 0, images = 0;
            if (pok) {
                for (int k = 0; k < pk.nobj; k++) {
                    const UgFngObject *o = &pk.obj[k];
                    if ((o->type == UG_OBJ_IMAGE || o->type == UG_OBJ_SIMPLEIMAGE) && o->res_index >= 0 && o->res_index < pk.nres &&
                        pk.res[o->res_index].kind != 0) {
                        images++;
                        if (ug_assets_texture_for(&as, pk.res[o->res_index].name) < 0) missing++;
                    }
                }
            }
            printf("  %-34s %7zu bytes  %s  objects %4d  images %4d  unresolved %3d%s%s\n", p->name, p->len, pok ? "parsed" : "PARSE FAILED",
                   pok ? pk.nobj : 0, images, missing, pok ? "" : "  ", pok ? "" : err);
            if (pok) ug_fng_free(&pk);
        }
        ug_assets_close(&as);
        return ok ? 0 : 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    Env *e = (Env *)calloc(1, sizeof *e);
    e->shot_dir = shots;
    e->check = check;e->demo_only=demo_only;
    e->win = SDL_CreateWindow("OpenUG2 ugui widget demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!e->win) {      /* retry without multisampling */
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        e->win = SDL_CreateWindow("OpenUG2 ugui widget demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    }
    if (!e->win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    e->gl = SDL_GL_CreateContext(e->win);
    if (!e->gl) { fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(check ? 0 : 1);
    printf("GL: %s | %s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));

    e->ugl = ug_gl_create();
    if (!e->ugl) { fprintf(stderr, "ug_gl_create failed\n"); return 1; }
    e->gl_errors += ug_gl_errors("init");
    SDL_ShowCursor(SDL_DISABLE);
    SDL_StartTextInput();
    demo_set_language(lang);
    demo_init_data(&e->app, vectors, data);
    if(!e->app.faithful_ready) {
        fprintf(stderr,"%s\n",e->app.assets_msg[0]?e->app.assets_msg:"Faithful demo unavailable: provide original game resources with --data");
        demo_free(&e->app);ug_gl_destroy(e->ugl);SDL_GL_DeleteContext(e->gl);SDL_DestroyWindow(e->win);SDL_Quit();free(e);return 1;
    }
    e->app.diagnostics=diagnostics;
    if(xbox_iso)ug_controller_load_xbox(&e->app.controller,&e->app.assets.tex,xbox_iso);
    if(ps2_iso)ug_controller_load_ps2(&e->app.controller,&e->app.assets.tex,ps2_iso);
    if(test_glyph_folder){
        fprintf(stderr,"controller PNG folder: TEST-ONLY runtime source; final controller sources use --ps2-iso/--xbox-iso\n");
        ug_controller_load_folder(&e->app.controller,&e->app.assets.tex,test_glyph_folder);
        UgController *p=&e->app.controller;
        for(size_t i=0;i<p->folder_count;i++)fprintf(stderr,"test glyph folder: family=%s key=%08x status=%s\n",p->folder_records[i].family==UG_CONTROLLER_PS2?"ps2":"xbox",p->folder_records[i].key,ug_glyph_folder_status(p->folder_records[i].status));
        if(p->folder_index_failed)fprintf(stderr,"test glyph folder: index configuration failure\n");
    }
    demo_select_controller(&e->app,!strcmp(family,"pc")?UG_CONTROLLER_PC:!strcmp(family,"xbox")?UG_CONTROLLER_XBOX:UG_CONTROLLER_PS2);
    fprintf(stderr,"controller: %s\n",e->app.controller.status);
    for(int i=0;i<nbindings;i++) {
        char token[40];const char *eq=strchr(bindings[i],'=');size_t n=eq?(size_t)(eq-bindings[i]):0;
        if(!eq||n==0||n>=sizeof token){fprintf(stderr,"invalid --bind, expected TOKEN=ICON_NAME\n");return 1;}
        memcpy(token,bindings[i],n);token[n]=0;
        const char *value=eq+1;uint32_t key;
        if((value[0]=='0'&&(value[1]=='x'||value[1]=='X'))||(strlen(value)==8&&strspn(value,"0123456789abcdefABCDEF")==8)){
            char *end=NULL;unsigned long parsed=strtoul(value,&end,16);
            if(!*value||*end||parsed>UINT32_MAX){fprintf(stderr,"invalid --bind numeric resource key\n");return 1;}key=(uint32_t)parsed;
        }else key=ug_hash_upper(value);
        UgVisualStatus status=ug_controller_bind(&e->app.controller,token,key);
        fprintf(stderr,"demo binding: %s -> %s (%s; unobserved retail mapping)\n",token,eq+1,status==UG_VIS_OK?"configured":"unavailable");
    }
    if(audit) {
        printf("Runtime visual provenance (user-supplied assets; no asset bytes packaged):\n");
        for(int i=0;i<e->app.assets.nfiles;i++)printf("source %s/%s\n",e->app.assets.dir,e->app.assets.file_names[i]);
        for(int i=0;i<e->app.assets.tex.n;i++) {
            UgTexture *t=&e->app.assets.tex.t[i];
            printf("visual %08x format %02x %dx%d source %s status %s\n",t->key,t->fmt,t->w,t->h,t->source,ug_texlib_pixels(&e->app.assets.tex,i)?"available":"unavailable");
        }
        for(int i=0;i<e->app.assets.nface;i++) {
            UgFace *f=&e->app.assets.face[i];int at=ug_texlib_find(&e->app.assets.tex,ug_hash_upper(f->atlas_name));
            printf("font %s atlas-key %08x source %s status %s\n",f->name,ug_hash_upper(f->atlas_name),at>=0?e->app.assets.tex.t[at].source:"unavailable",f->tex>0?"available":"unavailable");
        }
    }
    ug_gl_set_texlib(e->ugl, &e->app.assets.tex);
    printf("assets: %s\n", e->app.have_assets ? e->app.assets_msg : e->app.assets_msg[0] ? e->app.assets_msg : "none");
    int rc = 0;
    if (page >= 0) demo_open_page(&e->app, page);
    if (screen) {
        if (!demo_open_original_named(&e->app, screen)) {
            fprintf(stderr, "original screen '%s' unavailable: %s\n", screen,
                    e->app.viewer.vw.err[0] ? e->app.viewer.vw.err : "not found or cannot be opened");
            rc = 1;
            goto cleanup;
        }
    }

    if(gui_check) {rc=run_gui_check(e);
    } else if(capture) {
        step(e,max_frames>0?max_frames:120,0);
        if (screen && (e->gl_errors || !orig_viewer_active(&e->app) || !e->app.viewer.vw.loaded ||
                       e->app.viewer.vw.missing_tex || !e->app.ui.cv.n ||
                       ug_cv_visual_status(&e->app.ui.cv) != UG_VIS_OK)) {
            fprintf(stderr, "original screen '%s': incomplete frame; capture not written\n", screen);
            rc = 1;
        } else {
            int shot_ok=save_shot(e,capture);
            rc=e->gl_errors?2:shot_ok?0:1;
        }
    } else if (pillbox) {
        rc = run_pillbox_check(e);
    } else if (check) {
        rc = run_check(e);
    } else {
        int running = 1, held = 0;
        float held_t = 0;
        int first = 1;
        Uint32 last = SDL_GetTicks();
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                UgAction act = UG_A_NONE;
                float x, y;
                switch (ev.type) {
                case SDL_QUIT: running = 0; break;
                case SDL_MOUSEMOTION:
                    map_mouse(e, ev.motion.x, ev.motion.y, &x, &y);
                    ptr(e, UG_PT_MOVE, x, y, 0, 0);
                    break;
                case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT || ev.button.button == SDL_BUTTON_RIGHT) {
                        map_mouse(e, ev.button.x, ev.button.y, &x, &y);
                        ptr(e, ev.type == SDL_MOUSEBUTTONDOWN ? UG_PT_DOWN : UG_PT_UP, x, y,
                            ev.button.button == SDL_BUTTON_LEFT ? 1 : 2, 0);
                    }
                    break;
                case SDL_MOUSEWHEEL: {
                    int mx, my, dy = ev.wheel.y;
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
                    SDL_GetMouseState(&mx, &my);
                    map_mouse(e, mx, my, &x, &y);
                    if (dy) ptr(e, UG_PT_WHEEL, x, y, 0, dy > 0 ? 1 : -1);
                    break;
                }
                case SDL_TEXTINPUT: ug_ui_text(&e->app.ui, ev.text.text); break;
                case SDL_KEYDOWN:
                    switch (ev.key.keysym.sym) {
                    case SDLK_UP: act = UG_A_UP; break;
                    case SDLK_DOWN: act = UG_A_DOWN; break;
                    case SDLK_LEFT: act = UG_A_LEFT; break;
                    case SDLK_RIGHT: act = UG_A_RIGHT; break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: if (!ev.key.repeat) act = UG_A_ACCEPT; break;
                    case SDLK_ESCAPE: if (!ev.key.repeat) act = UG_A_BACK; break;
                    case SDLK_F1: if (!ev.key.repeat) act = UG_A_HELP; break;
                    case SDLK_LEFTBRACKET: if (!ev.key.repeat) act = UG_A_LTRIG; break;
                    case SDLK_RIGHTBRACKET: if (!ev.key.repeat) act = UG_A_RTRIG; break;
                    case SDLK_BACKSPACE: if (!ev.key.repeat) ug_ui_text(&e->app.ui, "\b"); break;
                    case SDLK_F2:
                        if (!ev.key.repeat) demo_cycle_hint_mode(&e->app);
                        break;
                    case SDLK_F3: if(!ev.key.repeat)e->app.diagnostics=!e->app.diagnostics; break;
                    case SDLK_F12: save_shot(e, "ugui_demo"); break;
                    default: break;
                    }
                    if (act != UG_A_NONE && !ev.key.repeat) {
                        ug_ui_action(&e->app.ui, act);
                        if (act >= UG_A_UP && act <= UG_A_RIGHT) { held = act; held_t = 0; first = 1; }
                    }
                    /* Q C D R = the four extra buttons (the viewer uses them for replay / exit / dim / undim) */
                    if (!ev.key.repeat && !(ev.key.keysym.mod & (KMOD_CTRL | KMOD_ALT))) {
                        int b = -1;
                        if (ev.key.keysym.sym == SDLK_q) b = UG_A_BTN0;
                        else if (ev.key.keysym.sym == SDLK_c) b = UG_A_BTN1;
                        else if (ev.key.keysym.sym == SDLK_d) b = UG_A_BTN2;
                        else if (ev.key.keysym.sym == SDLK_r) b = UG_A_BTN3;
                        if (b >= 0 && orig_viewer_active(&e->app)) ug_ui_action(&e->app.ui, (UgAction)b);
                    }
                    break;
                case SDL_KEYUP:
                    if (held && ((ev.key.keysym.sym == SDLK_UP && held == UG_A_UP) || (ev.key.keysym.sym == SDLK_DOWN && held == UG_A_DOWN) ||
                                 (ev.key.keysym.sym == SDLK_LEFT && held == UG_A_LEFT) || (ev.key.keysym.sym == SDLK_RIGHT && held == UG_A_RIGHT)))
                        held = 0;
                    break;
                default: break;
                }
            }
            Uint32 now = SDL_GetTicks();
            float dt = (float)(now - last);
            last = now;
            if (dt > 100) dt = 100;
            if (held) {                  /* key repeat: 400 ms delay, then every 110 ms (SPEC 4.5) */
                held_t += dt;
                float limit = first ? UG_TUNE_REPEAT_DELAY_MS : UG_TUNE_REPEAT_PERIOD_MS;
                if (held_t >= limit) { ug_ui_action(&e->app.ui, (UgAction)held); held_t = 0; first = 0; }
            }
            demo_update(&e->app, dt);
            render(e);
            SDL_GL_SwapWindow(e->win);
            if (max_frames > 0 && e->frames >= max_frames) running = 0;
            if (e->app.quit_requested) running = 0;          /* "Quit Game" pill, answered Yes */
        }
        printf("run: %d frames, %d GL errors\n", e->frames, e->gl_errors);
        rc = e->gl_errors ? 2 : 0;
    }

cleanup:
    demo_free(&e->app);
    ug_gl_destroy(e->ugl);
    SDL_GL_DeleteContext(e->gl);
    SDL_DestroyWindow(e->win);
    SDL_Quit();
    free(e);
    return rc;
}
