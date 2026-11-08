/* ugui_demo_orig.c -- "Original screens" page of the ugui demo: a mouse-
 * navigable browser of every screen package found in the user's install and a
 * viewer that runs a package through the scene runtime with the real textures
 * and fonts (SPEC 14-17).  GL-free; the same code runs in the unit tests.
 *
 * The viewer plays what the package itself contains: the enter animation at
 * creation, the exit / dim / undim animations on request (SPEC 4.2).  Hint
 * *content* is the library's mouse-navigation hint set (SPEC 7.6); only the
 * bottom strip pieces come from the data. */
#include "ugui_demo_app.h"
#include "ugui_tuning.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define R(cx, cy, w, h) UG_RECT_C((cx), (cy), (w), (h))

/* ------------------------------------------------------------------ */
/* browser                                                             */
/* ------------------------------------------------------------------ */
#define BR_COLS 3
#define BR_ROWS 11
#define BR_CELL_W 186.0f
#define BR_CELL_H 21.0f
#define BR_X0 (-288.0f)
#define BR_Y0 (-157.0f)
#define BR_NTABS 5

static const char *TAB_NAME[BR_NTABS] = {"All", "Menus", "Dialogs", "Help", "Other"};

static int pkg_class(const char *name) {
    char up[96];
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof up; n++) up[n] = (char)toupper((unsigned char)name[n]);
    up[n] = 0;
    if (strstr(up, "DIALOG")) return 2;
    if (strstr(up, "HELP")) return 3;
    if (!strncmp(up, "UI_", 3)) return 1;
    return 4;
}

static float clampf01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

static int rows_total(const DemoBrowser *b) { return (b->n + BR_COLS - 1) / BR_COLS; }

static void br_rebuild(DemoBrowser *b) {
    b->n = 0;
    int total = b->app->have_assets ? ug_assets_package_count(&b->app->assets) : 0;
    for (int i = 0; i < total && b->n < BR_MAX; i++) {
        const UgAssetPackage *p = ug_assets_package_at(&b->app->assets, i);
        if (b->tab == 0 || pkg_class(p->name) == b->tab) b->list[b->n++] = i;
    }
    if (b->sel >= b->n) b->sel = b->n ? b->n - 1 : 0;
    int rows = rows_total(b), maxfirst = rows > BR_ROWS ? rows - BR_ROWS : 0;
    if (b->first_row > maxfirst) b->first_row = maxfirst;
    if (b->first_row < 0) b->first_row = 0;
    ug_scroller_set(&b->sb, b->first_row, maxfirst + 1);
}

static void br_show_sel(DemoBrowser *b) {
    int row = b->sel / BR_COLS;
    if (row < b->first_row) b->first_row = row;
    if (row >= b->first_row + BR_ROWS) b->first_row = row - BR_ROWS + 1;
    int rows = rows_total(b), maxfirst = rows > BR_ROWS ? rows - BR_ROWS : 0;
    ug_scroller_set(&b->sb, b->first_row, maxfirst + 1);
}

static UgRect br_cell(const DemoBrowser *b, int i) {
    int row = i / BR_COLS - b->first_row, col = i % BR_COLS;
    return (UgRect){BR_X0 + (float)col * BR_CELL_W, BR_Y0 + (float)row * BR_CELL_H, BR_CELL_W - 4, BR_CELL_H - 2};
}

static UgRect br_tab(int t) { return R(-244 + (float)t * 112, -178, 106, 20); }

static int br_hit_cell(const DemoBrowser *b, float x, float y) {
    for (int i = b->first_row * BR_COLS; i < b->n && i < (b->first_row + BR_ROWS) * BR_COLS; i++)
        if (ug_rect_hit(br_cell(b, i), x, y)) return i;
    return -1;
}

static void br_set_tab(DemoBrowser *b, UgCtx *c, int t) {
    t = (t % BR_NTABS + BR_NTABS) % BR_NTABS;
    if (t == b->tab) return;
    ug_cue(c, t > b->tab ? UG_CUE_TRIGGER_R : UG_CUE_TRIGGER_L);
    b->tab = t;
    b->sel = 0;
    b->first_row = 0;
    br_rebuild(b);
}

static void br_open(DemoBrowser *b, UgCtx *c, int i) {
    if (i < 0 || i >= b->n) return;
    ug_cue(c, UG_CUE_SELECT);
    ug_post(c, UG_EV_ACTIVATE, b->w.id, b->list[i], 0);
}

static void br_update(UgWidget *w, UgCtx *c, float dt) {
    (void)c;
    DemoBrowser *b = (DemoBrowser *)w;
    ug_scroller_update(&b->sb, dt);
    ug_anim_update(&b->pulse, dt);
}

static int br_action(UgWidget *w, UgCtx *c, UgAction a) {
    DemoBrowser *b = (DemoBrowser *)w;
    int ns = b->sel;
    switch (a) {
    case UG_A_LEFT: ns--; break;
    case UG_A_RIGHT: ns++; break;
    case UG_A_UP: ns -= BR_COLS; break;
    case UG_A_DOWN: ns += BR_COLS; break;
    case UG_A_LTRIG: br_set_tab(b, c, b->tab - 1); return 1;
    case UG_A_RTRIG: br_set_tab(b, c, b->tab + 1); return 1;
    case UG_A_ACCEPT: br_open(b, c, b->sel); return 1;
    default: return 0;
    }
    if (ns < 0 || ns >= b->n) return 0;
    b->sel = ns;
    br_show_sel(b);
    ug_cue(c, UG_CUE_BROWSER);
    return 1;
}

static int br_pointer(UgWidget *w, UgCtx *c, const UgPtr *p) {
    DemoBrowser *b = (DemoBrowser *)w;
    int ni;
    if (p->type != UG_PT_MOVE || b->sb.dragging) {
        if (ug_scroller_pointer(&b->sb, p, &ni)) {
            b->first_row = ni;
            int rows = rows_total(b), maxfirst = rows > BR_ROWS ? rows - BR_ROWS : 0;
            if (b->first_row > maxfirst) b->first_row = maxfirst;
            return 1;
        }
    }
    if (p->type == UG_PT_WHEEL) {
        int rows = rows_total(b), maxfirst = rows > BR_ROWS ? rows - BR_ROWS : 0;
        int nf = b->first_row + (p->wheel > 0 ? -1 : 1) * 2;
        if (nf < 0) nf = 0;
        if (nf > maxfirst) nf = maxfirst;
        if (nf != b->first_row) {
            b->first_row = nf;
            ug_scroller_set(&b->sb, nf, maxfirst + 1);
            ug_cue(c, UG_CUE_SCROLLBAR);
        }
        return 1;
    }
    if (p->type == UG_PT_DOWN && p->button == 1) {
        for (int t = 0; t < BR_NTABS; t++)
            if (ug_rect_hit(br_tab(t), p->x, p->y)) {
                br_set_tab(b, c, t);
                return 1;
            }
    }
    int i = br_hit_cell(b, p->x, p->y);
    if (i < 0) return 0;
    if (p->type == UG_PT_MOVE) {
        if (i != b->sel) { b->sel = i; ug_cue(c, UG_CUE_BROWSER); }
        return 1;
    }
    if (p->type == UG_PT_DOWN && p->button == 1) {
        b->sel = i;
        br_open(b, c, i);
        return 1;
    }
    return 0;
}

static void br_draw(UgWidget *w, UgCtx *c, UgCanvas *cv) {
    DemoBrowser *b = (DemoBrowser *)w;
    DemoApp *a = b->app;
    ug_kit_panel(cv,(UgRect){-304,-228,608,40},0,1);
    ug_kit_title(c,cv,"Original Screens");
    if (!a->have_assets) {
        ug_kit_panel(cv, (UgRect){-250, -90, 500, 150}, 0, 0.8f);
        ug_kit_text(c, cv, "No game data found", 0, -60, 0.8f, UG_ACCENT, UG_J_CENTER | UG_J_VCENTER);
        ug_kit_textw(c, cv, a->assets_msg[0] ? a->assets_msg : "Start the demo with --data DIR (the game install), or set $UGUI_DATA.",
                     0, -25, 0.55f, UG_TEXT_LIGHT, UG_J_CENTER, 440 / 0.55f);
        ug_kit_text(c, cv, "Required original resources are unavailable.", 0, 35, 0.5f, UG_TEXT_HINT, UG_J_CENTER | UG_J_VCENTER);
        return;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "%d screens  |  %d textures  |  %d fonts  |  %s", ug_assets_package_count(&a->assets),
             a->assets.tex.n, a->assets.nface, a->assets.language);
    if(a->diagnostics)ug_kit_text(c,cv,buf,-288,110,0.5f,UG_TEXT_DARK,0);
    ug_kit_panel(cv,(UgRect){-300,-190,600,28},0,1);
    for (int t = 0; t < BR_NTABS; t++) {
        UgRect r = br_tab(t);
        int on = t == b->tab;
        ug_kit_pill(cv, r, UG_RGBA(0, 0, 0, on ? 190 : 120), on ? UG_ACCENT : UG_RGBA(255, 255, 255, 120), 1.4f);
        ug_kit_text(c, cv, TAB_NAME[t], r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.5f, on ? UG_ACCENT : UG_TEXT_LIGHT,
                    UG_J_CENTER | UG_J_VCENTER);
    }
    ug_kit_panel(cv, (UgRect){-300, -162, 600, 266}, 1, 1.0f);
    for (int i = b->first_row * BR_COLS; i < b->n && i < (b->first_row + BR_ROWS) * BR_COLS; i++) {
        UgRect r = br_cell(b, i);
        const UgAssetPackage *p = ug_assets_package_at(&a->assets, b->list[i]);
        int sel = i == b->sel;
        if (sel) {
            ug_cv_fill(cv, r, UG_RGBA(0, 0, 0, 220), 5);
            ug_cv_outline(cv, r, w->focused ? UG_ACCENT : UG_RGBA(197, 218, 165, 160), 5, 1.4f);
        }
        ug_kit_text(c, cv, p->name, r.x + 6, r.y + r.h * 0.5f, 0.45f, sel ? UG_ACCENT : UG_TEXT_DARK, UG_J_VCENTER);
    }
    ug_scroller_draw(&b->sb, cv, 1.0f);
    if (b->n == 0) ug_kit_text(c, cv, "(no screens in this group)", 0, -40, 0.6f, UG_TEXT_HINT, UG_J_CENTER | UG_J_VCENTER);
    if (b->n > 0) {
        const UgAssetPackage *p = ug_assets_package_at(&a->assets, b->list[b->sel]);
        if(a->diagnostics)snprintf(buf,sizeof buf,"%s / %.1f KB / %s",p->name,(double)p->len/1024.0,a->assets.file_names[p->source]);
        else snprintf(buf,sizeof buf,"%s (%d of %d)",p->name,b->sel+1,b->n);
        ug_kit_text(c,cv,buf,-288,124,0.5f,UG_TEXT_HINT,UG_J_VCENTER);
        ug_kit_text(c,cv,"Select a screen to open",-288,150,0.55f,UG_TEXT_HINT,UG_J_VCENTER);
    }
}

static void br_hints(UgWidget *w, UgHintSet *h) {
    (void)w;
    ug_hints_add(h, UG_A_ACCEPT, "Open Screen", 90);
    ug_hints_add(h, UG_A_INFO_WHEEL, "Scroll", 60);
    ug_hints_add(h, UG_A_INFO_TABS, "Filter", 50);
}

static void br_destroy(UgWidget *w) {
    DemoBrowser *b = (DemoBrowser *)w;
    ug_scroller_free(&b->sb);
    ug_anim_free(&b->pulse);
}

static const UgWidgetVT BR_VT = {br_update, br_action, br_pointer, br_draw, NULL, br_hints, br_destroy};

/* ------------------------------------------------------------------ */
/* viewer                                                              */
/* ------------------------------------------------------------------ */
static int scene_responds(const UgFngPackage *pk, uint32_t msg) {
    for (int i = 0; i < pk->nobj; i++)
        for (int r = 0; r < pk->obj[i].nresp; r++)
            if (pk->obj[i].resp[r].msg == msg) return 1;
    for (int r = 0; r < pk->npkg_resp; r++)
        if (pk->pkg_resp[r].msg == msg) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* icon-scroller binding (application behaviour, SPEC 6.4 / 15.9, G)    */
/* ------------------------------------------------------------------ */
static int icase_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static const char *res_name_of(const UgScene *sc, const UgNode *n) {
    int ri = n->obj->res_index;
    return (ri >= 0 && ri < sc->pk->nres) ? sc->pk->res[ri].name : "";
}

/* "CUSTOMIZE_CAR" -> "Customize Car" */
static void prettify(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    int up = 1;
    for (; *src && n + 1 < cap; src++) {
        char c = *src;
        if (c == '_') { c = ' '; up = 1; dst[n++] = c; continue; }
        dst[n++] = up ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
        up = 0;
    }
    dst[n] = 0;
}

/* which icon family a screen uses (the data names no icons: the program assigns them) */
static const char *bind_family(const char *pkg) {
    static const struct { const char *pkg, *family; } MAP[] = {
        {"UI_Main.fng", "MAIN_ICON_"}, {"UI_OptionsMain.fng", "OPTIONS_ICON_"}, {"UI_CareerCrib.fng", "CRIB_ICON_"},
        {"UI_QRModeSelect.fng", "TRACK_ICON_"}, {"UI_ChoosePaintCategory.fng", "PAINT_ICON_"},
        {"UI_ChoosePerformanceCategory.fng", "PERFORMANCE_ICON_"}, {"UI_ChooseCustomizeCategory.fng", "VISUAL_PART_"},
        {"UI_OnlineMain.fng", "ONLINE_ICON_"}, {"UI_OL_Main.fng", "ONLINE_ICON_"},
    };
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (icase_eq(pkg, MAP[i].pkg)) return MAP[i].family;
    return NULL;
}

/* Finds the objects an icon-scroller screen leaves to the program (geometry only, no assets needed):
 * the pool of off-screen item buttons, the 118 x 59 slot, the "Option title" text, the scroller thumb and
 * track, and the two scroll-arrow badges. Returns 1 when it looks like an icon scroller. */
int orig_bind_detect(DemoViewer *v) {
    IconBind *b = &v->bind;
    memset(b, 0, sizeof *b);
    b->slot = b->title = b->thumb = b->track = b->arrow_l = b->arrow_r = -1;
    if (!v->loaded) return 0;
    const UgScene *sc = &v->sc;
    for (int i = 0; i < sc->n; i++) {
        const UgNode *n = &sc->nodes[i];
        const UgFngObject *o = n->obj;
        float w = fabsf(n->props.w[UG_P_SIZE]), h = fabsf(n->props.w[UG_P_SIZE + 1]);
        if (o->type == UG_OBJ_IMAGE && (o->flags & UG_OF_BUTTON) && (o->flags & UG_OF_NO_DIRFOCUS) && n->parent < 0 &&
            w >= 100 && w <= 130 && h >= 38 && h <= 64 && (n->wf > 240 || n->wf < -240 || n->we > 320 || n->we < -320)) {
            if (b->npool < BIND_MAX) b->pool[b->npool++] = i;
        } else if (o->type == UG_OBJ_IMAGE && n->untextured && fabsf(w - 118) < 3 && fabsf(h - 59) < 3 &&
                   fabsf(n->we) < 8 && n->wf > -190 && n->wf < -100 && b->slot < 0) {
            b->slot = i;
            b->slot_x = n->we; b->slot_y = n->wf;
        } else if (o->type == UG_OBJ_TEXT && o->text && icase_eq(o->text, "Option title")) {
            b->title = i;
        } else if (o->type == UG_OBJ_IMAGE && strstr(res_name_of(sc, n), "Scroller_Tab")) {
            b->thumb = i;
        } else if (o->type == UG_OBJ_IMAGE && fabsf(w - 564) < 3 && fabsf(h - 8) < 2) {
            b->track = i;
        } else if (o->type == UG_OBJ_IMAGE && w >= 20 && w <= 40 && h >= 20 && h <= 40 && fabsf(fabsf(n->we) - UG_R_ARROW_CX) < 14 &&
                   fabsf(n->wf - UG_R_ARROW_CY) < 8) {
            if (n->we < 0 && b->arrow_l < 0) b->arrow_l = i;
            else if (n->we > 0 && b->arrow_r < 0) b->arrow_r = i;
        }
    }
    return b->npool >= 2 && b->slot >= 0;
}

void orig_bind_set_items(DemoViewer *v, int n, const int *tex, const char (*labels)[40]) {
    IconBind *b = &v->bind;
    if (n > b->npool) n = b->npool;
    if (n > BIND_MAX) n = BIND_MAX;
    for (int k = 0; k < n; k++) {
        b->tex[k] = tex ? tex[k] : 0;
        snprintf(b->label[k], sizeof b->label[k], "%s", labels ? labels[k] : "");
    }
    b->n = n;
    b->sel = 0;
    b->pos = 0;
    b->active = n > 0;
}

/* label of one main-menu icon: the string table's UI_MAIN_<ITEM> (15.9), tried with a few spellings */
static const char *main_label(const UgAssets *as, const char *stored, char *fallback, size_t cap) {
    static const struct { const char *icon, *labels[3]; } L[] = {
        {"CAREER", {"UI_MAIN_CAREER", NULL, NULL}}, {"QUICK_RACE", {"UI_MAIN_QUICK_RACE", NULL, NULL}},
        {"SPLIT_SCREEN", {"UI_MAIN_SPLIT_SCREEN", NULL, NULL}}, {"GO_ONLINE", {"UI_MAIN_ONLINE", NULL, NULL}},
        {"LAN", {"UI_MAIN_ONLINE_LAN", NULL, NULL}}, {"CUSTOMIZE_CAR", {"UI_MAIN_CUSTOMIZE", NULL, NULL}},
        {"OPTIONS", {"UI_MAIN_OPTIONS", NULL, NULL}},
        {"PROFILE_MANAG", {"UI_MAIN_PROFILE_MANAGEMENT", "UI_MAIN_PROFILE", "UI_MAIN_PROFILE_MANAGER"}}};
    for (size_t j = 0; j < sizeof L / sizeof L[0]; j++)
        if (icase_eq(stored, L[j].icon))
            for (int k = 0; k < 3 && L[j].labels[k]; k++) {
                const char *t = ug_strtab_get(&as->strings, ug_hash_upper(L[j].labels[k]));
                if (t) return t;
            }
    prettify(fallback, cap, stored);
    if (icase_eq(stored, "PROFILE_MANAG")) snprintf(fallback, cap, "Profile Management");
    return fallback;
}

static void bind_setup(DemoViewer *v) {
    IconBind *b = &v->bind;
    memset(b, 0, sizeof *b);
    b->slot = b->title = b->thumb = b->track = b->arrow_l = b->arrow_r = -1;
    if (!v->loaded || !v->app->have_assets) return;
    if (!orig_bind_detect(v)) { b->npool = 0; return; }
    const char *fam = bind_family(v->name);
    if (!fam) { b->npool = 0; return; }
    snprintf(b->family, sizeof b->family, "%s", fam);
    int found[BIND_MAX];
    int nf = ug_assets_textures_with_prefix(&v->app->assets, fam, found, BIND_MAX);
    if (nf <= 0) { b->npool = 0; return; }
    /* The main menu lists its items in the order of SPEC 6.4 / 15.9 (R, PC build): 7 items, career, quick race,
     * go online, then LAN and customize car (I: indexes 3 and 4 were not seen), profile management, options.
     * Other screens list their icon family by name. */
    static const char *const MAIN_ORDER[] = {"MAIN_ICON_CAREER", "MAIN_ICON_QUICK_RACE", "MAIN_ICON_GO_ONLINE",
        "MAIN_ICON_LAN", "MAIN_ICON_CUSTOMIZE_CAR", "MAIN_ICON_PROFILE_MANAG", "MAIN_ICON_OPTIONS"};
    int tex[BIND_MAX], ordered = 0;
    if (!strcmp(fam, "MAIN_ICON_")) {
        for (size_t k = 0; k < sizeof MAIN_ORDER / sizeof MAIN_ORDER[0] && ordered < BIND_MAX; k++)
            for (int j = 0; j < nf; j++)
                if (icase_eq(v->app->assets.tex.t[found[j] - 1].name, MAIN_ORDER[k])) { tex[ordered++] = found[j]; break; }
    }
    if (!ordered) for (int j = 0; j < nf && ordered < b->npool; j++) tex[ordered++] = found[j];
    char labels[BIND_MAX][40];
    int n = ordered < b->npool ? ordered : b->npool;
    for (int k = 0; k < n; k++) {
        const char *stored = v->app->assets.tex.t[tex[k] - 1].name + strlen(fam);
        char pretty[40];
        const char *text = pretty;
        if (!strcmp(fam, "MAIN_ICON_")) text = main_label(&v->app->assets, stored, pretty, sizeof pretty);
        else prettify(pretty, sizeof pretty, stored);
        snprintf(labels[k], sizeof labels[k], "%s", text);
    }
    orig_bind_set_items(v, n, tex, (const char (*)[40])labels);
}

/* after ug_scene_update: write the application-controlled properties of the strip and re-layout (SPEC 6.4, R).
 * Every icon is placed by the library's strip geometry: the item nearest the centre in the slot, the others
 * through the pool buttons. */
void orig_bind_apply(DemoViewer *v, float dt) {
    IconBind *b = &v->bind;
    if (!b->active || !v->loaded) return;
    UgScene *sc = &v->sc;
    if (dt > 0) {
        float k = 1.0f - powf(0.001f, dt / 260.0f);
        b->pos += ((float)b->sel - b->pos) * k;
        if (fabsf((float)b->sel - b->pos) < 0.002f) b->pos = (float)b->sel;
    }
    if (b->pos < 0) b->pos = 0;
    if (b->pos > (float)(b->n - 1)) b->pos = (float)(b->n - 1);
    int nearest = (int)floorf(b->pos + 0.5f);
    for (int k2 = 0; k2 < b->npool; k2++) {
        UgNode *n = &sc->nodes[b->pool[k2]];
        n->tex_override = k2 < b->n ? b->tex[k2] : 0;
        if (k2 >= b->n || k2 == nearest) {                   /* the nearest item is shown in the slot */
            n->props.w[UG_P_COLOR + 3] = 0;
            ug_scene_set_world_pos(sc, b->pool[k2], -999.0f, 0);
            continue;
        }
        UgIconPlace p;
        ug_icon_place(b->pos, k2, b->slot_x, b->slot_y, &p);
        ug_scene_set_world_pos(sc, b->pool[k2], p.cx, p.cy);
        n->props.w[UG_P_SIZE] = p.w; n->props.w[UG_P_SIZE + 1] = p.h;
        n->props.w[UG_P_COLOR] = n->props.w[UG_P_COLOR + 1] = n->props.w[UG_P_COLOR + 2] = 255;
        n->props.w[UG_P_COLOR + 3] = 255.0f * p.alpha;       /* R: full opacity, no backing */
    }
    {
        UgNode *sl = &sc->nodes[b->slot];
        UgIconPlace p;
        ug_icon_place(b->pos, nearest, b->slot_x, b->slot_y, &p);
        sl->tex_override = b->tex[nearest];
        ug_scene_set_world_pos(sc, b->slot, p.cx, p.cy);
        sl->props.w[UG_P_SIZE] = p.w; sl->props.w[UG_P_SIZE + 1] = p.h;
        sl->props.w[UG_P_COLOR] = sl->props.w[UG_P_COLOR + 1] = sl->props.w[UG_P_COLOR + 2] = 255;
        sl->props.w[UG_P_COLOR + 3] = 255.0f * p.alpha;
    }
    if (b->title >= 0) sc->nodes[b->title].text_override = b->label[b->sel];
    if (b->thumb >= 0) {                                      /* thumb x = -273.7 + 547.4 * index / (N - 1) */
        UgNode *th = &sc->nodes[b->thumb];
        ug_scene_set_world_pos(sc, b->thumb, ug_icons_thumb_xf(b->n, b->pos), th->wf);
    }
    /* the end arrows are shown only when an item exists on that side (4.5) */
    if (b->arrow_l >= 0) sc->nodes[b->arrow_l].app_hidden = b->sel <= 0;
    if (b->arrow_r >= 0) sc->nodes[b->arrow_r].app_hidden = b->sel >= b->n - 1;
    ug_scene_relayout(sc);
}

static void bind_select(DemoViewer *v, UgCtx *c, int idx) {
    IconBind *b = &v->bind;
    if (idx == b->sel) return;
    b->sel = idx;
    ug_cue(c, UG_CUE_ICON_SCROLLER);
}

int orig_bind_step(DemoViewer *v, UgCtx *c, int dir) {
    IconBind *b = &v->bind;
    int ni = b->sel + dir;
    if (ni < 0 || ni >= b->n) return 1;                       /* no wrap (4.5, R): the press is ignored */
    bind_select(v, c, ni);
    /* the package's own arrow blink: its responses to the direction messages (3.6) */
    ug_scene_input_msg(&v->sc, dir < 0 ? UG_MSG_LEFT : UG_MSG_RIGHT);
    return 1;
}
static int bind_step(DemoViewer *v, UgCtx *c, int dir) { return orig_bind_step(v, c, dir); }

static int bind_hit_item(const DemoViewer *v, float x, float y) {
    const IconBind *b = &v->bind;
    for (int k = 0; k < b->n; k++) {
        UgIconPlace p;
        ug_icon_place(b->pos, k, b->slot_x, b->slot_y, &p);
        if (p.alpha < 0.5f) continue;
        if (ug_rect_hit(UG_RECT_C(p.cx, p.cy, p.w, p.h), x, y)) return k;
    }
    return -1;
}

/* Dialog packages hold a one-button layout (centre) and a two-button layout (+-150) side by side; the
 * program shows one of them. The viewer shows the two-button one (the common Yes / No case, SPEC 6.9). */
static void dialog_variant(DemoViewer *v) {
    UgScene *sc = &v->sc;
    int centre = -1, left = -1, right = -1;
    for (int i = 0; i < sc->n; i++) {
        const UgNode *n = &sc->nodes[i];
        const UgFngObject *o = n->obj;
        if (o->type != UG_OBJ_GROUP || !(o->flags & UG_OF_BUTTON) || !(o->flags & UG_OF_NO_DIRFOCUS) || n->parent >= 0) continue;
        if (fabsf(n->we) < 30 && centre < 0) centre = i;
        else if (n->we < -100 && left < 0) left = i;
        else if (n->we > 100 && right < 0) right = i;
    }
    if (centre < 0 || left < 0 || right < 0 || fabsf(sc->nodes[left].wf - sc->nodes[centre].wf) > 4) return;
    sc->nodes[centre].app_hidden = 1;
    const UgNode *g = &sc->nodes[centre];
    for (int i = 0; i < sc->n; i++) {            /* its label sits just above it */
        const UgNode *n = &sc->nodes[i];
        if (n->obj->type == UG_OBJ_TEXT && (n->obj->flags & UG_OF_NO_DIRFOCUS) && fabsf(n->we - g->we) < 20 &&
            fabsf(n->wf - (g->wf - 11.0f)) < 8)
            sc->nodes[i].app_hidden = 1;
    }
    ug_scene_relayout(sc);
    /* SPEC 6.9 (R): the right-hand, negative button has focus when a two-button dialog opens */
    ug_scene_set_focus(sc, right);
}

static void vw_unload(DemoViewer *v) {
    if (v->loaded) {
        ug_scene_free(&v->sc);
        ug_fng_free(&v->pk);
        v->loaded = 0;
    }
}

static int vw_load(DemoViewer *v, int index) {
    DemoApp *a = v->app;
    vw_unload(v);
    v->closing = 0;
    v->close_t = 0;
    v->n_msgs = 0;
    v->last_msg = 0;
    v->err[0] = 0;
    v->suppressed = 0;
    v->objects = v->missing_tex = v->has_strip = 0;
    const UgAssetPackage *p = a->have_assets ? ug_assets_package_at(&a->assets, index) : NULL;
    if (!p) { snprintf(v->err, sizeof v->err, "no such screen"); return 0; }
    v->index=index;
    snprintf(v->name,sizeof v->name,"%s",p->name);
    if(a->controller.family!=UG_CONTROLLER_PC && !strcasecmp(p->name,"UI_PC_Help_Bar.fng")){v->suppressed=1;snprintf(v->err,sizeof v->err,"PC help-bar presentation intentionally suppressed by console profile");return 0;}
    v->index = index;
    snprintf(v->name, sizeof v->name, "%s", p->name);
    if (!ug_fng_parse(p->data, p->len, &v->pk, v->err, sizeof v->err)) return 0;
    if (!ug_scene_init(&v->sc, &v->pk, &a->assets.strings)) {
        snprintf(v->err, sizeof v->err, "scene init failed");
        ug_fng_free(&v->pk);
        return 0;
    }
    ug_scene_set_assets(&v->sc, &a->assets);
    v->sc.pc_build = a->controller.family==UG_CONTROLLER_PC;
    v->sc.hide_hint_text = v->sc.pc_build && strcasecmp(v->name,"UI_PC_Help_Bar.fng");
    v->sc.console_strip = !v->sc.pc_build;
    v->loaded = 1;
    v->objects = v->pk.nobj;
    ug_scene_update(&v->sc, 0);
    v->missing_tex = ug_scene_missing_textures(&v->sc);
    v->has_strip = ug_scene_has_bottom_strip(&v->sc);
    bind_setup(v);
    orig_bind_apply(v, 0);
    /* on the PC main menu Back does nothing; the help bar offers Quit Game and Continue only (7.4.1) */
    if (icase_eq(v->name, "UI_Main.fng")) a->viewer.scr.flags |= UG_SCR_NOBACKHINT;
    else a->viewer.scr.flags &= ~UG_SCR_NOBACKHINT;
    if (strstr(v->name, "Dialog")) dialog_variant(v);
    return 1;
}

static void vw_begin_close(DemoViewer *v, UgCtx *c) {
    if (v->closing) return;
    v->closing = 1;
    v->close_t = 0;
    if (v->loaded && scene_responds(&v->pk, UG_MSG_REQ_BACK)) ug_scene_post(&v->sc, UG_MSG_REQ_BACK, 0);
    else v->close_t = UG_TUNE_EXIT_TIMEOUT_MS;        /* no controller: leave at once */
    ug_cue(c, UG_CUE_BACK);
}

static void vw_update(UgWidget *w, UgCtx *c, float dt) {
    DemoViewer *v = (DemoViewer *)w;
    if (!v->loaded) {
        if (v->closing) ug_post(c, UG_EV_BACK, w->id, 0, 0);
        return;
    }
    ug_scene_update(&v->sc, dt);
    orig_bind_apply(v, dt);
    uint32_t m;
    int exit_done = 0;
    while (ug_scene_poll_app(&v->sc, &m)) {
        v->last_msg = m;
        v->n_msgs++;
        if (m == UG_MSG_EXIT_DONE) exit_done = 1;
        if (m == UG_MSG_ENTER_DONE) ug_scene_refocus(&v->sc);      /* start the focused button's highlight now */
        if (m == UG_MSG_BACK) vw_begin_close(v, c);
    }
    v->has_strip = ug_scene_has_bottom_strip(&v->sc);
    if (v->closing) {
        v->close_t += dt;
        if (exit_done || v->close_t >= UG_TUNE_EXIT_TIMEOUT_MS) {
            v->closing = 0;
            ug_post(c, UG_EV_BACK, w->id, 0, 0);
        }
    }
}

static int vw_switch(DemoViewer *v, UgCtx *c, int delta) {
    int n = ug_assets_package_count(&v->app->assets);
    if (n <= 0) return 0;
    int ni = ((v->index + delta) % n + n) % n;
    vw_load(v, ni);
    ug_cue(c, delta < 0 ? UG_CUE_TRIGGER_L : UG_CUE_TRIGGER_R);
    return 1;
}

static int vw_action(UgWidget *w, UgCtx *c, UgAction a) {
    DemoViewer *v = (DemoViewer *)w;
    switch (a) {
    case UG_A_BACK: vw_begin_close(v, c); return 1;
    case UG_A_BTN0: vw_load(v, v->index); ug_cue(c, UG_CUE_SELECT); return 1;
    case UG_A_BTN1: if (v->loaded) ug_scene_post(&v->sc, UG_MSG_REQ_FORWARD, 0); ug_cue(c, UG_CUE_BUTTON); return 1;
    case UG_A_BTN2: if (v->loaded) ug_scene_post(&v->sc, UG_MSG_REQ_DIM, 0); ug_cue(c, UG_CUE_BUTTON); return 1;
    case UG_A_BTN3: if (v->loaded) ug_scene_post(&v->sc, UG_MSG_REQ_UNDIM, 0); ug_cue(c, UG_CUE_BUTTON); return 1;
    case UG_A_LTRIG: return vw_switch(v, c, -1);
    case UG_A_RTRIG: return vw_switch(v, c, +1);
    default: break;
    }
    if (!v->loaded || v->closing) return 0;
    if (v->bind.active) {
        if (a == UG_A_LEFT) return bind_step(v, c, -1);
        if (a == UG_A_RIGHT) return bind_step(v, c, +1);
        if (a == UG_A_ACCEPT) {
            ug_cue(c, UG_CUE_SELECT);
            ug_scene_post(&v->sc, UG_MSG_REQ_FORWARD, 0);     /* pressing an item leaves the screen (4.2) */
            return 1;
        }
    }
    int r = ug_scene_input(&v->sc, a);
    return r;
}

static int vw_pointer(UgWidget *w, UgCtx *c, const UgPtr *p) {
    DemoViewer *v = (DemoViewer *)w;
    if (!v->loaded || v->closing) return 0;
    if (v->bind.active) {
        IconBind *b = &v->bind;
        int hit = bind_hit_item(v, p->x, p->y);
        if (p->type == UG_PT_WHEEL) return bind_step(v, c, p->wheel > 0 ? -1 : 1);
        if (p->type == UG_PT_MOVE) {
            /* Item buttons are application-bound horizontal-list entries.
             * Do not fall through to generic scene hover focus for their
             * item/arrow/track region (20.10 H1). Other controls keep hover. */
            if (hit >= 0 || ug_rect_hit((UgRect){-297,-190,594,90},p->x,p->y)) return 1;
        } else if (p->type == UG_PT_DOWN && p->button == 1) {
            if (b->sel > 0 && ug_rect_hit(UG_RECT_C(-UG_R_ARROW_CX, UG_R_ARROW_CY, 34, 34), p->x, p->y)) return bind_step(v, c, -1);
            if (b->sel < b->n - 1 && ug_rect_hit(UG_RECT_C(UG_R_ARROW_CX, UG_R_ARROW_CY, 34, 34), p->x, p->y)) return bind_step(v, c, +1);
            if (hit >= 0) {
                if (hit != b->sel) bind_select(v, c, hit);
                ug_cue(c, UG_CUE_SELECT);
                ug_scene_post(&v->sc, UG_MSG_REQ_FORWARD, 0);
                return 1;
            }
        }
    }
    ug_scene_pointer(&v->sc, p);
    return p->type != UG_PT_MOVE || ug_scene_pick(&v->sc, p->x, p->y) >= 0;
}

static void vw_draw(UgWidget *w, UgCtx *c, UgCanvas *cv) {
    DemoViewer *v = (DemoViewer *)w;
    if (!v->loaded) {
        ug_kit_panel(cv, (UgRect){-230, -60, 460, 110}, 0, 0.8f);
        ug_kit_text(c, cv, v->name, 0, -35, 0.7f, UG_ACCENT, UG_J_CENTER | UG_J_VCENTER);
        ug_kit_textw(c, cv, v->err[0] ? v->err : "not loaded", 0, -8, 0.55f, UG_TEXT_LIGHT, UG_J_CENTER, 400 / 0.55f);
        return;
    }
    ug_scene_draw(&v->sc, c, cv);
}

static int vw_icon_hint(const DemoViewer *v) { return v->loaded && v->bind.active; }

/* Screens whose PC help bar offers a forward ("Continue") pill in the reference captures: the icon-scroller
 * menus and the car selects (7.4.1). Elsewhere the pills are Quit Game and Back. */
static int viewer_has_continue(const DemoViewer *v) {
    return v->bind.active || strstr(v->name, "CarSelect") || strstr(v->name, "CarLot");
}

static void vw_hints(UgWidget *w, UgHintSet *h) {
    DemoViewer *v = (DemoViewer *)w;
    if (vw_icon_hint(v)) ug_hints_add(h, UG_A_INFO_WHEEL, "Change Selection", 88);
    if (viewer_has_continue(v)) ug_hints_add(h, UG_A_ACCEPT, "Select", 85);
    ug_hints_add(h, UG_A_BTN0, "Replay", 70);
    ug_hints_add(h, UG_A_BTN1, "Exit", 66);
    ug_hints_add(h, UG_A_BTN2, "Dim", 62);
    ug_hints_add(h, UG_A_BTN3, "Undim", 61);
    ug_hints_add(h, UG_A_LTRIG, "Prev", 45);
    ug_hints_add(h, UG_A_RTRIG, "Next", 44);
    /* The viewer's own tools (replay, exit, dim, undim, previous / next screen) belong to the library's mouse
     * hint strip, not to the PC help bar (7.4.1). Exception: on the PC controls page the third pill is "Reset
     * Keys To Default", which the viewer maps to its Replay tool (the viewer has no key bindings to reset). */
    int ctl = strstr(v->name, "PC_Controller") != NULL;
    ug_hints_pc(h, UG_A_BTN0, ctl ? "Reset Keys To Default" : NULL, !ctl);
    ug_hints_pc(h, UG_A_BTN1, NULL, 1);
    ug_hints_pc(h, UG_A_BTN2, NULL, 1);
    ug_hints_pc(h, UG_A_BTN3, NULL, 1);
    ug_hints_pc(h, UG_A_LTRIG, NULL, 1);
    ug_hints_pc(h, UG_A_RTRIG, NULL, 1);
}

static void vw_destroy(UgWidget *w) { vw_unload((DemoViewer *)w); }

static const UgWidgetVT VW_VT = {vw_update, vw_action, vw_pointer, vw_draw, NULL, vw_hints, vw_destroy};

/* ------------------------------------------------------------------ */
/* page glue                                                           */
/* ------------------------------------------------------------------ */
static void bg_none(UgScreen *s, UgCtx *c, UgCanvas *cv) { (void)s; (void)c; (void)cv; }

void orig_init(DemoApp *a) {
    DemoOrigBrowser *ob = &a->orig;
    memset(ob, 0, sizeof *ob);
    ug_screen_init(&ob->scr, "Original Screens", 0);
    ob->scr.user = a;
    ob->scr.draw_bg = bg_none;
    DemoBrowser *b = &ob->br;
    b->app = a;
    ug_widget_init(&b->w, &BR_VT, "browser", DID_BROWSER, (UgRect){-304, -190, 624, 300});
    ug_scroller_init(&b->sb, (UgRect){282, -132, 8, 212}, 1, 1);
    b->sb.page = BR_ROWS - 1;
    ug_anim_init(&b->pulse, UG_ACCENT, 0, 0);
    ug_screen_add(&ob->scr, &b->w);

    DemoOrigViewer *ov = &a->viewer;
    memset(ov, 0, sizeof *ov);
    a->orig_hint_mode = UG_HINTS_PC;
    ug_screen_init(&ov->scr, "Original Screen", UG_SCR_NOFADE);
    ov->scr.user = a;
    ov->scr.draw_bg = bg_none;
    DemoViewer *v = &ov->vw;
    v->app = a;
    v->index = -1;
    ug_widget_init(&v->w, &VW_VT, "viewer", DID_VIEWER, (UgRect){-320, -240, 640, 480});
    ug_screen_add(&ov->scr, &v->w);
    br_rebuild(b);
}

void orig_free(DemoApp *a) {
    ug_screen_free(&a->orig.scr);
    ug_screen_free(&a->viewer.scr);
}

void orig_assets_changed(DemoApp *a) {
    DemoBrowser *b = &a->orig.br;
    b->tab = 0; b->sel = 0; b->first_row = 0;
    br_rebuild(b);
}

int orig_open(DemoApp *a, int index) {
    if (!a->have_assets || ug_ui_busy(&a->ui)) return 0;
    DemoViewer *v = &a->viewer.vw;
    vw_load(v, index);                     /* a failed load still opens the viewer to show the reason */
    ug_ui_push(&a->ui, &a->viewer.scr);
    return 1;
}

int orig_viewer_active(const DemoApp *a) {
    return a->ui.sp > 0 && a->ui.stack[a->ui.sp - 1] == &a->viewer.scr;
}

/* per-frame glue: the page draws its own strip? what does the grey box say? */
void orig_post_frame(DemoApp *a) {
    DemoViewer *v = &a->viewer.vw;
    int active = orig_viewer_active(a) || (a->ui.sp > 1 && a->ui.stack[a->ui.sp - 2] == &a->viewer.scr);
    int pc=a->controller.family==UG_CONTROLLER_PC;
    int overlay=!strcasecmp(v->name,"UI_PC_Help_Bar.fng");
    if(active && overlay && ((pc && v->suppressed) || (!pc && v->loaded)))vw_load(v,v->index);
    if(active && v->loaded) {
        v->sc.pc_build=pc;v->sc.console_strip=!pc;
        v->sc.hide_hint_text=pc && !overlay;
        ug_scene_update(&v->sc,0);
        v->has_strip=ug_scene_has_bottom_strip(&v->sc);
        v->missing_tex=ug_scene_missing_textures(&v->sc);
    }
    a->ui.hints.mode=pc?UG_HINTS_PC:UG_HINTS_STRIP;
    a->ui.hints.no_bg=active && v->loaded && (pc?overlay:v->has_strip);
    a->ui.hide_hints=active && v->loaded && strstr(v->name,"Dialog")!=NULL;
    a->orig_active_prev=active;

}

int demo_open_original(DemoApp *a, int index) { return orig_open(a, index); }

int demo_open_original_named(DemoApp *a, const char *file_name) {
    if (!a->have_assets || ug_ui_busy(&a->ui)) return 0;
    const UgAssetPackage *p = ug_assets_package(&a->assets, file_name);
    if (!p) return 0;
    for (int i = 0; i < ug_assets_package_count(&a->assets); i++)
        if (ug_assets_package_at(&a->assets, i) == p) {
            /* An explicitly requested screen must be a decoded original
             * package, not the browser's diagnostic for a failed load. */
            if (!vw_load(&a->viewer.vw, i)) return 0;
            ug_ui_push(&a->ui, &a->viewer.scr);
            return 1;
        }
    return 0;
}
