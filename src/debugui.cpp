/* OpenUG2 debug overlay — Dear ImGui panel, compiled only into `make debug`.
 * Thin C-callable wrapper (declared in debug.h) so main.c stays plain C.
 * Backends: SDL2 + legacy OpenGL2 (matches the app's 2.1/compat GL context). */
#include <cstdio>
#include <SDL.h>
#ifdef __APPLE__
#  define GL_SILENCE_DEPRECATION 1
#  include <OpenGL/gl.h>
#else
#  include <SDL_opengl.h>
#endif
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl2.h"
#include "debug.h"

extern "C" void dbgui_init(struct SDL_Window *win, void *glctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;                      /* don't litter imgui.ini */
    io.ConfigWindowsMoveFromTitleBarOnly = false;  /* keep panels draggable */
    io.ConfigWindowsResizeFromEdges = true;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL((SDL_Window *)win, glctx);
    ImGui_ImplOpenGL2_Init();
}

extern "C" void dbgui_event(const union SDL_Event *e) {
    ImGui_ImplSDL2_ProcessEvent((const SDL_Event *)e);
}
extern "C" int dbgui_want_mouse(void)    { return ImGui::GetIO().WantCaptureMouse; }
extern "C" int dbgui_want_keyboard(void) { return ImGui::GetIO().WantCaptureKeyboard; }

extern "C" void dbgui_frame(void) {
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(620, 720), ImGuiCond_FirstUseEver);
    ImGui::Begin("NFSU2 Master Inspector", nullptr, ImGuiWindowFlags_None);
    g_dbg.fps = (int)ImGui::GetIO().Framerate;
    /* Momentary car/track switch requests: reset EVERY frame (not inside a tab,
       or an inactive tab would leave want_car at 0 = a valid index and main.c
       would relaunch the process every frame). The combos below set them on
       change only. */
    g_dbg.want_car = -1; g_dbg.want_track = -1;

    if (ImGui::BeginTabBar("MasterInspectorTabs")) {

    /* ---- Tab 1: Vehicle & Wheels ---- */
    if (ImGui::BeginTabItem("Vehicle & Wheels")) {
        if (ImGui::CollapsingHeader("Vehicle Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
            /* body paint -> u_PaintColor (uColor) when override is on; the draw
               loop reads g_dbg.paint for BODY/MISC meshes, so this repaints live. */
            ImGui::Checkbox("custom paint (override per-car colour)", (bool *)&g_dbg.paint_override);
            ImGui::ColorEdit3("body paint (u_PaintColor)", g_dbg.paint);
            if (ImGui::Button("Red"))    { g_dbg.paint_override=1; g_dbg.paint[0]=0.70f; g_dbg.paint[1]=0.05f; g_dbg.paint[2]=0.05f; } ImGui::SameLine();
            if (ImGui::Button("Blue"))   { g_dbg.paint_override=1; g_dbg.paint[0]=0.05f; g_dbg.paint[1]=0.10f; g_dbg.paint[2]=0.60f; } ImGui::SameLine();
            if (ImGui::Button("Black"))  { g_dbg.paint_override=1; g_dbg.paint[0]=0.02f; g_dbg.paint[1]=0.02f; g_dbg.paint[2]=0.03f; } ImGui::SameLine();
            if (ImGui::Button("Silver")) { g_dbg.paint_override=1; g_dbg.paint[0]=0.60f; g_dbg.paint[1]=0.62f; g_dbg.paint[2]=0.66f; }
            ImGui::TextDisabled("body kit: K cycles authored KIT00..KITnn (see console)");
        }
        if (ImGui::CollapsingHeader("Wheel Stance (per-car, metres)", ImGuiTreeNodeFlags_DefaultOpen)) {
            /* Absolute stance for the active car -- edits apply next frame, since
               the wheel transforms are rebuilt from g_dbg.wheel every frame. */
            ImGui::SliderFloat("front axle Z", &g_dbg.wheel.front_axle,  0.0f, 2.5f, "%.3f m");
            ImGui::SliderFloat("rear axle Z",  &g_dbg.wheel.rear_axle,  -2.5f, 0.0f, "%.3f m");
            ImGui::SliderFloat("front track",  &g_dbg.wheel.front_track, 0.8f, 2.2f, "%.3f m");
            ImGui::SliderFloat("rear track",   &g_dbg.wheel.rear_track,  0.8f, 2.2f, "%.3f m");
            ImGui::SliderFloat("ride height Y",&g_dbg.wheel.ride_y,     -0.5f, 0.5f, "%.3f m");
            ImGui::Text("wheelbase %.3f m", g_dbg.wheel.front_axle - g_dbg.wheel.rear_axle);
            ImGui::SliderFloat("radius/scale", &g_dbg.wheel_scale, 0.3f, 2.0f);
            ImGui::Text("radius %.3f m   %.0f RPM   steer %+.1f deg",
                        g_dbg.wheel_radius, g_dbg.wheel_rpm, g_dbg.steer_deg);
        }
        if (ImGui::CollapsingHeader("Rim: brand / style / paint", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (g_dbg.wheel_brands && g_dbg.wheel_brand_n > 0) {
                static const char *brands[32];
                int nb = g_dbg.wheel_brand_n < 32 ? g_dbg.wheel_brand_n : 32;
                for (int i = 0; i < nb; i++) brands[i] = g_dbg.wheel_brands[i];
                if (ImGui::Combo("wheel brand", &g_dbg.wheel_brand, brands, nb)) g_dbg.wheel_reload = 1;
                if (ImGui::SliderInt("wheel style", &g_dbg.wheel_style, 1, 8)) g_dbg.wheel_reload = 1;
            } else ImGui::TextDisabled("wheel library not loaded");
            ImGui::Checkbox("paint rims (off = raw OEM texture)", (bool *)&g_dbg.rim_paint);
            ImGui::ColorEdit3("rim colour", g_dbg.rim_color);
            if (ImGui::Button("Chrome/Silver")) {
                g_dbg.rim_paint=1; g_dbg.rim_color[0]=0.85f; g_dbg.rim_color[1]=0.88f; g_dbg.rim_color[2]=0.92f;
            } ImGui::SameLine();
            if (ImGui::Button("OEM Gold")) g_dbg.rim_paint = 0;
            ImGui::SameLine();
            if (ImGui::Button("Gunmetal")) {
                g_dbg.rim_paint=1; g_dbg.rim_color[0]=0.30f; g_dbg.rim_color[1]=0.32f; g_dbg.rim_color[2]=0.36f;
            }
        }
        if (ImGui::CollapsingHeader("Vehicle Handling", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("live @ %.0f km/h", g_dbg.kmh);
            ImGui::SliderFloat("acceleration", &g_dbg.tune_accel, 0.2f, 3.0f, "%.2fx");
            ImGui::SliderFloat("braking",      &g_dbg.tune_brake, 0.2f, 3.0f, "%.2fx");
            ImGui::SliderFloat("steering",     &g_dbg.tune_turn,  0.2f, 2.5f, "%.2fx");
            ImGui::SliderFloat("top speed",    &g_dbg.tune_top,  60.0f, 320.0f, "%.0f km/h");
            if (ImGui::Button("reset handling")) {
                g_dbg.tune_accel=g_dbg.tune_brake=g_dbg.tune_turn=1.0f; g_dbg.tune_top=220.0f;
            }
        }
        if (ImGui::CollapsingHeader("Car parts")) {
            ImGui::Checkbox("body",   (bool *)&g_dbg.show_body);   ImGui::SameLine();
            ImGui::Checkbox("glass",  (bool *)&g_dbg.show_glass);  ImGui::SameLine();
            ImGui::Checkbox("lights", (bool *)&g_dbg.show_lights);
            ImGui::Checkbox("tires",  (bool *)&g_dbg.show_tires);  ImGui::SameLine();
            ImGui::Checkbox("misc",   (bool *)&g_dbg.show_misc);   ImGui::SameLine();
            ImGui::Checkbox("track",  (bool *)&g_dbg.show_track);
            ImGui::TextDisabled("body paint moved to Vehicle Controls");
        }
        if (ImGui::CollapsingHeader("Mesh Inspector")) {
            static const char *catn[] = {"ROAD","TERRAIN","OTHER","SKY","GLOW","?","?","?","?","?",
                                         "BODY","GLASS","LIGHT","TIRE","MISC","BRAKELIGHT","MECH","INTERIOR"};
            ImGui::Checkbox("highlight", (bool *)&g_dbg.insp_highlight); ImGui::SameLine();
            ImGui::Checkbox("wireframe", (bool *)&g_dbg.insp_wire);
            if (ImGui::Button("Dump Selected Mesh Telemetry")) g_dbg.insp_dump = 1;
            ImGui::BeginChild("meshlist", ImVec2(0, 160), true);
            for (int i = 0; i < g_dbg.insp_count; i++) {
                int c = (g_dbg.insp_cat && i < g_dbg.insp_count) ? g_dbg.insp_cat[i] : 0;
                const char *cn = (c >= 0 && c <= 17) ? catn[c] : "?";
                char lbl[96];
                snprintf(lbl, sizeof lbl, "%3d  %-10s %5d v", i, cn,
                         g_dbg.insp_verts ? g_dbg.insp_verts[i] : 0);
                if (ImGui::Selectable(lbl, g_dbg.insp_sel == i)) g_dbg.insp_sel = i;
            }
            ImGui::EndChild();
            ImGui::Checkbox("[Debug] Flip Vertex Normals", (bool *)&g_dbg.insp_flipn);
            const char *cullm[] = { "no culling (engine default)", "cull BACK faces", "cull FRONT faces" };
            ImGui::Combo("[Debug] Face culling", &g_dbg.insp_cull, cullm, 3);
            ImGui::Checkbox("[Debug] Force Alpha Depth Write", (bool *)&g_dbg.insp_glass_depth);
            if (ImGui::Button("clear selection")) g_dbg.insp_sel = -1;
        }
        if (ImGui::CollapsingHeader("Debug")) {
            ImGui::Checkbox("anim demo (free spin + sine steer)", (bool *)&g_dbg.wheel_demo);
            ImGui::TextDisabled("drives the wheel matrices off a clock so spin/steer");
            ImGui::TextDisabled("can be verified with the car parked");
        }
        ImGui::EndTabItem();
    }

    /* ---- Tab 2: Lighting & Environment ---- */
    if (ImGui::BeginTabItem("Lighting & Environment")) {
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            /* Night mode: emissive light lenses + headlight pools/bloom + low
               ambient. Toggling applies an ambient preset; the slider below still
               fine-tunes it. The emissive/pool gating is read live in the draw. */
            if (ImGui::Checkbox("Night Mode", (bool *)&g_dbg.night_mode))
                g_dbg.ambient = g_dbg.night_mode ? 0.38f : 0.78f;
            ImGui::SameLine();
            ImGui::TextDisabled(g_dbg.night_mode ? "(lenses glow, headlights on)"
                                                 : "(daylight, lenses off)");
        }
        if (ImGui::CollapsingHeader("Camera (3rd-person chase)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("distance (back)", &g_dbg.chase_distance,  3.0f, 25.0f, "%.1f m");
            ImGui::SliderFloat("height (up)",     &g_dbg.chase_height,    1.0f, 15.0f, "%.1f m");
            ImGui::SliderFloat("stiffness (lerp)",&g_dbg.chase_stiffness, 0.02f, 1.0f, "%.2f/frame");
            if (ImGui::Button("reset chase cam")) {
                g_dbg.chase_distance=10.0f; g_dbg.chase_height=4.5f; g_dbg.chase_stiffness=0.22f;
            }
            ImGui::TextDisabled("low stiffness = looser spring; 1.0 = rigidly glued");
            ImGui::Separator();
            ImGui::Checkbox("Auto-Drive (camera test)", (bool *)&g_dbg.auto_drive);
            ImGui::TextDisabled("steady throttle + sine steer -> hands-free S-curve");
        }
        if (ImGui::CollapsingHeader("Lighting / Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("ambient",   &g_dbg.ambient,   0.0f, 1.0f);
            ImGui::SliderFloat("diffuse",   &g_dbg.diffuse,   0.0f, 1.5f);
            ImGui::SliderFloat("body spec", &g_dbg.body_spec, 0.0f, 1.0f);
            ImGui::SliderFloat("body reflection", &g_dbg.body_env, 0.0f, 2.0f, "%.2fx");
            ImGui::SliderFloat("fog density", &g_dbg.fog_density, 0.0f, 0.01f, "%.4f");
            ImGui::ColorEdit3("fog / sky colour", &g_dbg.fog_r);
        }
        if (ImGui::CollapsingHeader("Neon Underglow", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("neon on", (bool *)&g_dbg.neon_on);
            ImGui::ColorEdit3("neon colour", g_dbg.neon_col);
            ImGui::SliderFloat("intensity", &g_dbg.neon_str, 0.0f, 1.5f);
        }
        ImGui::EndTabItem();
    }

    /* ---- Tab 3: World & Entities ---- */
    if (ImGui::BeginTabItem("World & Entities")) {
        bool show_menu_hud = !g_dbg.hud_hide_menu;
        ImGui::Checkbox("show 3D HUD (menu + race)", &show_menu_hud);
        g_dbg.hud_hide_menu = !show_menu_hud;
        if (g_dbg.race_cars > 0)
            ImGui::Text("race: P%d/%d   lap %d/%d", g_dbg.race_pos, g_dbg.race_cars,
                        g_dbg.race_lap, g_dbg.race_laps);
        if (g_dbg.car_list && g_dbg.n_cars > 0) {
            static const char *items[64]; int n = g_dbg.n_cars < 64 ? g_dbg.n_cars : 64;
            for (int i = 0; i < n; i++) items[i] = g_dbg.car_list[i];
            int cur = g_dbg.sel_car;
            if (ImGui::Combo("car", &cur, items, n) && cur != g_dbg.sel_car) g_dbg.want_car = cur;
        } else ImGui::Text("car: %s (%d/%d)", g_dbg.car_name, g_dbg.sel_car+1, g_dbg.n_cars);
        if (g_dbg.track_list && g_dbg.n_tracks > 0) {
            static const char *items[64]; int n = g_dbg.n_tracks < 64 ? g_dbg.n_tracks : 64;
            for (int i = 0; i < n; i++) items[i] = g_dbg.track_list[i];
            int cur = g_dbg.sel_track;
            if (ImGui::Combo("track", &cur, items, n) && cur != g_dbg.sel_track) g_dbg.want_track = cur;
        } else ImGui::Text("track: %s (%d/%d)", g_dbg.track_name, g_dbg.sel_track+1, g_dbg.n_tracks);
        ImGui::Text("circuit: %d/%d   |   %d track meshes", g_dbg.sel_circuit+1, g_dbg.n_circuits, g_dbg.track_meshes);
        ImGui::Checkbox("Show UV Checker", (bool *)&g_dbg.show_uv_checker);
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Scenery Semantics (asset names, 0x134011)", ImGuiTreeNodeFlags_DefaultOpen)) {
            static const char *scn[] = { "-","TERRAIN","BUILDING","PROP","TREE","WALL","STRUCT","OTHER" };
            int named = 0, tot = 0;
            for (int i = 0; i < 8; i++) { tot += g_dbg.scen_count[i]; if (i) named += g_dbg.scen_count[i]; }
            ImGui::Text("%d/%d meshes named (%.1f%%)", named, tot, tot ? 100.0f*named/tot : 0.0f);
            for (int i = 1; i < 8; i++) if (g_dbg.scen_count[i]) {
                ImGui::SameLine(); ImGui::TextDisabled("%s %d", scn[i], g_dbg.scen_count[i]); }
            ImGui::Separator();
            ImGui::Text("nearby world chunks (<=60 m):");
            ImGui::BeginChild("scnear", ImVec2(0, 150), true);
            for (int i = 0; i < g_dbg.scen_near_n; i++) ImGui::Text("%s", g_dbg.scen_near[i]);
            if (!g_dbg.scen_near_n) ImGui::TextDisabled("(none in range)");
            ImGui::EndChild();
        }
        if (ImGui::CollapsingHeader("Entity Definitions (0x39200, read-only)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%d ZCV_/ZCS_ defs from L4R*.BUN  [defs only, no placement]", g_dbg.scripted_count);
            ImGui::BeginChild("entdefs", ImVec2(0, 300), true);
            for (int i = 0; i < g_dbg.scripted_count; i++) {
                const ScriptedDef *e = &g_dbg.scripted[i];
                ImGui::Text("%-24s %08x  %5.1f x%5.1f x%5.1f", e->name, e->hash, e->w, e->l, e->h);
            }
            ImGui::EndChild();
        }
        ImGui::EndTabItem();
    }

    /* ---- Tab 4: Engine Telemetry ---- */
    if (ImGui::BeginTabItem("Engine Telemetry")) {
        ImGui::Text("%.1f FPS   %.2f ms/frame", ImGui::GetIO().Framerate,
                    1000.0f / (ImGui::GetIO().Framerate > 0 ? ImGui::GetIO().Framerate : 1));
        ImGui::Text("draw calls (meshes): %d   car %d   track %d",
                    g_dbg.drawn, g_dbg.car_meshes, g_dbg.track_meshes);
        ImGui::Separator();
        ImGui::Text("district: %-10s  (%d zones parsed)",
                    g_dbg.zone_name[0] ? g_dbg.zone_name : "-", g_dbg.zone_count);
        ImGui::Separator();
        ImGui::Text("camera XYZ  %.1f  %.1f  %.1f", g_dbg.cam[0], g_dbg.cam[1], g_dbg.cam[2]);
        ImGui::Text("car XYZ     %.1f  %.1f  %.1f", g_dbg.car[0], g_dbg.car[1], g_dbg.car[2]);
        ImGui::Text("heading %.2f rad   %.0f km/h", g_dbg.heading, g_dbg.kmh);
        ImGui::Separator();
        ImGui::Checkbox("Freecam (F)", (bool *)&g_dbg.freecam);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("freecam speed", &g_dbg.speed, 0.05f, 3.0f);
        ImGui::EndTabItem();
    }

    /* ---- Tab 5: Navigation & Races ---- */
    if (ImGui::BeginTabItem("Navigation & Races")) {
    /* The real drivable road network parsed
       from the per-region ROUTES path files (chunk 0x34148), drawn top-down
       in world XY. Independent of the 3D geometry viewer. ---- */
    ImGui::Text("%d nodes, %d edges, %d districts", g_dbg.nnav, g_dbg.nnavedge, g_dbg.ndist);
    ImGui::SameLine();
    ImGui::TextDisabled("district %s", g_dbg.zone_name[0] ? g_dbg.zone_name : "-");
    if (g_dbg.nnav > 1) {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float side = avail.x < avail.y ? avail.x : avail.y;
        if (side < 80.0f) side = 80.0f;
        if (side > 300.0f) side = 300.0f;   /* leave room for the track manager below */
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x+side, p0.y+side), IM_COL32(12,14,20,255));
        float x0=g_dbg.navbb[0], x1=g_dbg.navbb[1], y0=g_dbg.navbb[2], y1=g_dbg.navbb[3];
        float w = x1-x0, h = y1-y0, span = w > h ? w : h;
        if (span < 1.0f) span = 1.0f;
        /* world -> screen; world +Y is north, screen +Y is down, so flip Y */
        #define MAPX(X) (p0.x + ((X)-x0)/span*side)
        #define MAPY(Y) (p0.y + side - ((Y)-y0)/span*side)
        /* Colour each edge by its fused district. Drawn as POLYLINES, not
           independent lines: ImGui uses 16-bit indices, and 15895 separate
           AddLine calls blew past the 65536-vertex limit (assert in
           AddDrawListToDrawDataEx). Chaining consecutive edges roughly halves
           the vertex count and keeps the whole graph under the cap. */
        static const ImU32 DC[8] = {
            IM_COL32( 90,190,255,200), IM_COL32(255,150, 60,200),
            IM_COL32(120,255,120,200), IM_COL32(255, 90,200,200),
            IM_COL32(255,230, 80,200), IM_COL32(160,140,255,200),
            IM_COL32( 60,255,230,200), IM_COL32(255,120,120,200) };
        static ImVec2 chain[4096];
        int nchain = 0, curd = -2;
        for (int e = 0; e <= g_dbg.nnavedge; e++) {
            int a = -1, b = -1, dcur = -1;
            if (e < g_dbg.nnavedge) {
                a = g_dbg.navedge[e*2]; b = g_dbg.navedge[e*2+1];
                dcur = g_dbg.navcomp ? g_dbg.navcomp[a] : 0;
            }
            int cont = (e < g_dbg.nnavedge) && nchain > 0 && dcur == curd &&
                       a == g_dbg.navedge[(e-1)*2+1] && nchain < 4095;
            if (!cont) {
                if (nchain > 1)
                    dl->AddPolyline(chain, nchain,
                                    curd >= 0 ? DC[curd & 7] : IM_COL32(130,130,130,160),
                                    0, 1.0f);
                nchain = 0;
                if (e >= g_dbg.nnavedge) break;
                curd = dcur;
                chain[nchain++] = ImVec2(MAPX(g_dbg.nav[a*2]), MAPY(g_dbg.nav[a*2+1]));
            }
            chain[nchain++] = ImVec2(MAPX(g_dbg.nav[b*2]), MAPY(g_dbg.nav[b*2+1]));
        }
        /* active race event: outline polygon + one tick per closed road */
        if (g_dbg.mode == MODE_RACE_EVENT && g_dbg.active_ev >= 0 && g_dbg.ev) {
            const WEvent &e = g_dbg.ev[g_dbg.active_ev];
            ImVec2 op[WORLD_EVPOLY];
            for (int i = 0; i < e.npoly; i++)
                op[i] = ImVec2(MAPX(e.poly[i][0]), MAPY(e.poly[i][1]));
            dl->AddPolyline(op, e.npoly, IM_COL32(255,255,255,230), 0, 2.5f);
            for (int i = 0; i < g_dbg.bar_count; i++) {
                const WBarrier &b = g_dbg.bar[i];
                /* the blockade sits across the closed road: perpendicular to it */
                float px = MAPX(b.x), py = MAPY(b.y);
                float tx = -b.dy * 6.0f, ty = b.dx * 6.0f;   /* screen +Y is down */
                dl->AddLine(ImVec2(px-tx, py+ty), ImVec2(px+tx, py-ty),
                            IM_COL32(255,50,50,255), 3.0f);
            }
        }
        /* checkpoint gates: armed one bright yellow, cleared grey, pending dim */
        if (g_dbg.race && g_dbg.race->active) {
            const WRace *R = g_dbg.race;
            for (int i = 0; i < R->ngate; i++) {
                const WGate &g = R->gate[i];
                /* the gate spans across the direction of travel; world +Y is up */
                float cx = MAPX(g.x), cy = MAPY(g.y);
                float ax = MAPX(g.x - (-g.dy)*g.half), ay = MAPY(g.y - g.dx*g.half);
                float bx = MAPX(g.x + (-g.dy)*g.half), by = MAPY(g.y + g.dx*g.half);
                int cleared = R->next == 0 ? i > 0 : i < R->next;
                ImU32 col = i == R->next   ? IM_COL32(255,225, 60,255)   /* armed  */
                          : i == 0         ? IM_COL32(255,255,255,200)   /* s/f    */
                          : cleared        ? IM_COL32(130,130,130,190)   /* done   */
                                           : IM_COL32( 90, 90,110,150);  /* pending*/
                dl->AddLine(ImVec2(ax,ay), ImVec2(bx,by), col, i == R->next ? 3.0f : 1.5f);
                if (i == R->next) dl->AddCircle(ImVec2(cx,cy), 6.0f, col, 0, 2.0f);
            }
            for (int i = 0; i < R->ngrid; i++)
                dl->AddCircleFilled(ImVec2(MAPX(R->grid[i][0]), MAPY(R->grid[i][1])),
                                    2.0f, IM_COL32(120,200,255,220));
        }
        /* GPS route overlay */
        if (g_dbg.gps_path && g_dbg.gps_n > 1) {
            static ImVec2 rp[8192];
            int rn = g_dbg.gps_n < 8192 ? g_dbg.gps_n : 8191;
            for (int i = 0; i < rn; i++) {
                int nd = g_dbg.gps_path[i];
                rp[i] = ImVec2(MAPX(g_dbg.nav[nd*2]), MAPY(g_dbg.nav[nd*2+1]));
            }
            dl->AddPolyline(rp, rn, IM_COL32(80,255,170,255), 0, 3.0f);
            dl->AddCircleFilled(rp[rn-1], 5.0f, IM_COL32(80,255,170,255));
        }
        /* right-click inside the map sets the GPS destination */
        {   ImVec2 mp = ImGui::GetIO().MousePos;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                mp.x >= p0.x && mp.x <= p0.x+side && mp.y >= p0.y && mp.y <= p0.y+side) {
                g_dbg.gps_want_x = x0 + (mp.x - p0.x) / side * span;
                g_dbg.gps_want_y = y0 + (p0.y + side - mp.y) / side * span;
                g_dbg.gps_request = 1;
            }
        }
        /* player */
        float px = MAPX(g_dbg.car[0]), py = MAPY(g_dbg.car[1]);
        dl->AddCircleFilled(ImVec2(px, py), 4.0f, IM_COL32(255,80,60,255));
        float hx = px + cosf(g_dbg.heading)*11.0f;
        float hy = py - sinf(g_dbg.heading)*11.0f;   /* Y flipped */
        dl->AddLine(ImVec2(px,py), ImVec2(hx,hy), IM_COL32(255,220,90,255), 2.0f);
        ImGui::Dummy(ImVec2(side, side));
        for (int i = 0; i < g_dbg.ndist && i < 8; i++) {
            if (i) ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(DC[i & 7]), "%s", g_dbg.dist_tok[i]);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s = %s", g_dbg.dist_tok[i], g_dbg.dist_name[i]);
        }
        ImGui::TextDisabled("X[%.0f..%.0f] Y[%.0f..%.0f]  car (%.0f, %.0f)",
                            x0, x1, y0, y1, g_dbg.car[0], g_dbg.car[1]);
        if (g_dbg.gps_n > 1)
            ImGui::Text("GPS: %d nodes, %.0f m, %d ms", g_dbg.gps_n, g_dbg.gps_dist, g_dbg.gps_ms);
        else ImGui::TextDisabled("right-click the map to set a GPS destination");
        #undef MAPX
        #undef MAPY
    } else ImGui::TextDisabled("no navigation data loaded");

    /* ---- Race & Track Manager: the engine's Freeroam / Race-event split.
       Events come from the shipped 0x3414c catalog; picking one masks the A*
       graph to that event's corridor and makes its road closures solid. ---- */
    if (ImGui::CollapsingHeader("Race & Track Manager", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = g_dbg.mode;
        if (ImGui::RadioButton("Freeroam Mode", mode == MODE_FREEROAM)) {
            g_dbg.want_mode = MODE_FREEROAM; g_dbg.want_event = -1; g_dbg.mode_request = 1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(mode == MODE_RACE_EVENT
            ? "race event active" : "whole city drivable, no barriers");

        ImGui::Text("%d race events parsed (chunk 0x3414c)", g_dbg.ev_count);
        if (mode == MODE_RACE_EVENT && g_dbg.active_ev >= 0) {
            const WEvent &e = g_dbg.ev[g_dbg.active_ev];
            ImGui::TextColored(ImVec4(1.0f,0.85f,0.3f,1.0f),
                "event %d  %s  %s  ~%d00 m  |  %d barriers, %d links masked",
                e.id, e.reg, e.circuit ? "CIRCUIT" : "SPRINT", e.len100m,
                g_dbg.bar_count, g_dbg.masked_links);
        }
        /* --- live race HUD (Phase 72) --- */
        if (g_dbg.race && g_dbg.race->active) {
            const WRace *R = g_dbg.race;
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f,0.88f,0.25f,1.0f),
                "Lap: %d/%d      Checkpoints Cleared: %d/%d",
                R->lap > 0 ? R->lap : 1, R->maxlaps, R->cleared, R->ngate - 1);
            ImGui::Text("next gate: %s%d of %d   |   %d start-grid slots",
                        R->next == 0 ? "START/FINISH #" : "checkpoint #",
                        R->next, R->ngate - 1, R->ngrid);
            if (R->finished) ImGui::TextColored(ImVec4(0.4f,1.0f,0.5f,1.0f), "RACE FINISHED");
            if (ImGui::Button("Stop race")) g_dbg.race_stop_request = 1;
            ImGui::SameLine();
            if (ImGui::Button("Restart")) g_dbg.race_start_request = 1;
        } else if (g_dbg.mode == MODE_RACE_EVENT) {
            ImGui::Separator();
            if (ImGui::Button("Start race")) g_dbg.race_start_request = 1;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::SliderInt("laps", &g_dbg.race_maxlaps_want, 1, 8);
        }

        if (ImGui::BeginListBox("##events", ImVec2(-1, 180))) {
            for (int i = 0; i < g_dbg.ev_count; i++) {
                const WEvent &e = g_dbg.ev[i];
                char lbl[96];
                snprintf(lbl, sizeof lbl, "%d  %-4s  %-7s  ~%d00 m  (%d nodes)",
                         e.id, e.reg, e.circuit ? "circuit" : "sprint",
                         e.len100m, e.node1 - e.node0);
                if (ImGui::Selectable(lbl, mode == MODE_RACE_EVENT && i == g_dbg.active_ev)) {
                    g_dbg.want_mode = MODE_RACE_EVENT; g_dbg.want_event = i;
                    g_dbg.mode_request = 1;
                }
            }
            ImGui::EndListBox();
        }
    }
    ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    }
    ImGui::End();
}

extern "C" void dbgui_render(void) {
    ImGui::Render();
    /* the app binds its shader program once and never rebinds; the GL2 backend
       is fixed-function, so unbind the program around it and restore after. */
    GLint prev = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prev);
    glUseProgram(0);
    /* the GL2 backend draws with client-side arrays: any VBO the app left bound
       would make glVertexPointer read garbage, and attrib 0 aliases gl_Vertex. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(0);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    glUseProgram(prev);
}

extern "C" void dbgui_shutdown(void) {
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}
