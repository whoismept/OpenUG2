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
extern "C" {
    extern int   g_tex_aniso_max;
    extern float g_tex_aniso;
    void render_texture_detail(float aniso);
}

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
extern "C" int dbgui_want_text(void) { return ImGui::GetIO().WantTextInput; }

static ImU32 heat_colour(float value) {
    value=fmaxf(0,fminf(1,value));
    float r,g,b;ImGui::ColorConvertHSVtoRGB((1-value)*.66f,.9f,1,r,g,b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r,g,b,.95f));
}
static void heatmap_controls() {
    ImGui::SeparatorText("Traffic / collision heatmap");
    ImGui::Checkbox("Record diagnostics",(bool *)&g_dbg.heat_record);
    ImGui::SameLine();if(ImGui::Button("Clear heatmap"))g_dbg.heat_clear=1;
    ImGui::Combo("Layer",&g_dbg.heat_layer,
        "Off\0Current occupancy\0Stationary vehicles\0World corrections\0Vehicle corrections\0");
    if(!g_dbg.heat_layer)return;
    ImGui::Checkbox("Follow player",(bool *)&g_dbg.heat_local);
    ImGui::SameLine();ImGui::Checkbox("Near player height (+/-4 m)",(bool *)&g_dbg.heat_height);
    if(g_dbg.heat_local)ImGui::SliderFloat("Map width",&g_dbg.heat_span,100,1500,"%.0f m");
    ImGui::Checkbox("Include racers in traffic layers",(bool *)&g_dbg.heat_racers);
    ImGui::TextDisabled("%s | %.1f s collected | 4 Hz traffic snapshots",
        g_dbg.heat_record?"Recording":"Paused",g_dbg.heatmap.seconds);
    if(g_dbg.heat_layer==1)ImGui::TextWrapped("Blue: 1 vehicle; red: 4+ per directed segment. Hover for speed and count.");
    else if(g_dbg.heat_layer==2)ImGui::TextWrapped("Red: all vehicles stationary for 3+ seconds. Queues can be legitimate; this is not proof of stuck AI.");
    else ImGui::TextWrapped("Blue to red: correction distance, relative to the hottest bin. 10 s decay; expires after 60 s. Points mark vehicle positions in 8 m bins, not exact surface contacts.");
    if(g_dbg.heatmap.evictions)ImGui::TextDisabled("Oldest correction bins replaced: %u (capacity %d)",g_dbg.heatmap.evictions,HEAT_CONTACTS);
}
static void draw_heatmap(ImDrawList *dl,ImVec2 p0,float side,float x0,float y0,float span) {
    const TrafficHeatmap &h=g_dbg.heatmap;
    const auto map=[&](float x,float y){return ImVec2(p0.x+(x-x0)*side/span,p0.y+side-(y-y0)*side/span);};
    ImVec2 mouse=ImGui::GetIO().MousePos;
    bool inside=mouse.x>=p0.x && mouse.x<=p0.x+side && mouse.y>=p0.y && mouse.y<=p0.y+side;
    int hover=-1;float nearest=64;
    if(g_dbg.heat_layer<=2) {
        for(int i=0;i<h.nroads;i++) {
            const HeatRoad &r=h.roads[i];
            if((r.racer && !g_dbg.heat_racers) || (g_dbg.heat_height && fabsf(r.z-g_dbg.car[2])>4))continue;
            ImVec2 a=map(r.a[0],r.a[1]),b=map(r.b[0],r.b[1]);
            float dx=b.x-a.x,dy=b.y-a.y,length=sqrtf(dx*dx+dy*dy);
            if(length>1) { // offset opposite directions to opposite sides of the road
                float ox=-dy/length*3,oy=dx/length*3;a.x+=ox;b.x+=ox;a.y+=oy;b.y+=oy;
            }
            float value=g_dbg.heat_layer==1?(r.count-1)/3.0f:(float)r.stationary/r.count;
            ImU32 color=heat_colour(value);
            if(length>1) {
                dl->AddLine(a,b,color,4);
                ImVec2 mid((a.x+b.x)*.5f,(a.y+b.y)*.5f);
                float ux=dx/length,uy=dy/length;
                dl->AddTriangleFilled(ImVec2(mid.x+ux*5,mid.y+uy*5),
                    ImVec2(mid.x-ux*4-uy*4,mid.y-uy*4+ux*4),
                    ImVec2(mid.x-ux*4+uy*4,mid.y-uy*4-ux*4),color);
            } else dl->AddCircleFilled(a,5,color);
            float t=length>1?fmaxf(0,fminf(1,((mouse.x-a.x)*dx+(mouse.y-a.y)*dy)/(length*length))):0;
            float ex=mouse.x-a.x-t*dx,ey=mouse.y-a.y-t*dy,d2=ex*ex+ey*ey;
            if(d2<nearest){nearest=d2;hover=i;}
        }
        if(inside && hover>=0) {
            const HeatRoad &r=h.roads[hover];
            ImGui::SetTooltip("%s | edge %d -> %d | Z %.1f m\n%d vehicles, %d stationary >=3 s\nMean speed %.1f km/h",
                r.racer?"Racers":"Traffic",r.from,r.to,r.z,r.count,r.stationary,r.speed*3.6f);
        }
    } else {
        int kind=g_dbg.heat_layer==3?HEAT_WORLD:HEAT_VEHICLE;float peak=.0001f;
        for(int i=0;i<HEAT_CONTACTS;i++) {
            const HeatContact &c=h.contacts[i];
            if(c.used && c.kind==kind && (!g_dbg.heat_height || fabsf(c.pos[2]-g_dbg.car[2])<=4))peak=fmaxf(peak,c.heat);
        }
        for(int i=0;i<HEAT_CONTACTS;i++) {
            const HeatContact &c=h.contacts[i];
            if(!c.used || c.kind!=kind || (g_dbg.heat_height && fabsf(c.pos[2]-g_dbg.car[2])>4))continue;
            ImVec2 p=map(c.pos[0],c.pos[1]);float value=sqrtf(c.heat/peak);
            dl->AddCircleFilled(p,4+5*value,heat_colour(value));
            float dx=mouse.x-p.x,dy=mouse.y-p.y,d2=dx*dx+dy*dy;
            if(d2<nearest){nearest=d2;hover=i;}
        }
        if(inside && hover>=0) {
            const HeatContact &c=h.contacts[hover];
            ImGui::SetTooltip("Vehicle position %.1f, %.1f, %.1f\n%u solver corrections (not crashes)\nDecayed distance %.3f m; peak single correction %.3f m\nLast seen %.1f s ago",
                c.pos[0],c.pos[1],c.pos[2],c.corrections,c.heat,c.peak,h.seconds-c.last);
        }
    }
}

static bool shop_tab(const char *label, ImVec4 colour) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    bool open=ImGui::BeginTabItem(label);
    ImGui::PopStyleColor();
    return open;
}

static void part_selector(int p) {
    if (!g_dbg.mod_parts) return;
    const N2PartMenu *menu=g_dbg.mod_parts+p;
    const char *label="Stock / none";
    for(int i=0;i<menu->count;i++)
        if(menu->options[i].value==g_dbg.mod_current.parts[p])label=menu->options[i].label;
    ImGui::BeginDisabled(menu->count<2 || g_dbg.kmh>1.0f || g_dbg.kmh<-1.0f);
    if(ImGui::BeginCombo(n2_part_labels[p],menu->count<2?"Not available for this car":label)) {
        for(int i=0;i<menu->count;i++) {
            bool selected=menu->options[i].value==g_dbg.mod_current.parts[p];
            if(ImGui::Selectable(menu->options[i].label,selected)) {
                g_dbg.mod_request_slot=p;g_dbg.mod_request_value=menu->options[i].value;
            }
            if(selected)ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}


/* ---------------------------------------------------------------------------
 * Placement marks. The user drives to a spot that looks wrong,
 * presses M, then drives to where it should be and presses Shift+M. Each mark
 * records the probe point AND what the production ground selector reports
 * under it, so a report names the actual covering chunk instead of an XY guess.
 * Marks live here, not in DbgState: nothing on the engine side reads them. */
enum { MARK_MAX = 256, MARK_NOTE = 64 };
struct PlacementMark {
    int  target;                 /* 0 = defect here, 1 = should be here */
    float pos[3], ground_z;
    int  cat;
    char asset[32], district[24], note[MARK_NOTE];
};
static PlacementMark g_marks[MARK_MAX];
static int g_nmarks = 0;
static char g_mark_note[MARK_NOTE] = "";

static const char *mark_cat_name(int cat) {
    return cat == 1 ? "ROAD" : cat == 2 ? "TERRAIN" : "none";
}

static void mark_line(const PlacementMark *m, int index, char *out, size_t cap) {
    snprintf(out, cap,
             "MARK %02d %-9s x=%9.2f y=%9.2f z=%8.2f  groundZ=%8.2f cat=%-7s "
             "asset=%-28s district=%-3s note=%s",
             index + 1, m->target ? "SHOULD-BE" : "DEFECT",
             m->pos[0], m->pos[1], m->pos[2], m->ground_z,
             mark_cat_name(m->cat), m->asset[0] ? m->asset : "-",
             m->district[0] ? m->district : "-", m->note);
}

static void mark_capture(int target) {
    if (g_nmarks >= MARK_MAX) return;
    PlacementMark *m = &g_marks[g_nmarks];
    m->target = target;
    m->pos[0] = g_dbg.probe[0]; m->pos[1] = g_dbg.probe[1]; m->pos[2] = g_dbg.probe[2];
    m->ground_z = g_dbg.probe_ground_z;
    m->cat = g_dbg.probe_ground_cat;
    snprintf(m->asset, sizeof m->asset, "%s", g_dbg.probe_asset);
    snprintf(m->district, sizeof m->district, "%s", g_dbg.zone_name);
    snprintf(m->note, sizeof m->note, "%s", g_mark_note);
    char line[320];
    mark_line(m, g_nmarks, line, sizeof line);
    printf("%s\n", line);          /* also in the log, so nothing is lost on quit */
    fflush(stdout);
    g_nmarks++;
}

/* One text block for both the clipboard and the file, so what the user pastes
   is byte-for-byte what lands on disk. */
static void mark_text(char *out, size_t cap) {
    size_t used = (size_t)snprintf(out, cap,
        "# OpenUG2 placement marks -- track %s, car %s\n", g_dbg.track_name,
        g_dbg.car_name);
    for (int i = 0; i < g_nmarks && used < cap; i++) {
        char line[320];
        mark_line(&g_marks[i], i, line, sizeof line);
        used += (size_t)snprintf(out + used, cap - used, "%s\n", line);
    }
}

extern "C" void dbgui_frame(void) {
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(620, 720), ImGuiCond_FirstUseEver);
    ImGui::Begin("NFSU2 Master Inspector", nullptr, ImGuiWindowFlags_None);
    g_dbg.fps = (int)ImGui::GetIO().Framerate;
    /* Momentary car/track switch requests: reset every frame. The main loop
       consumes them on the frame thread while keeping the SDL/GL session alive. */
    g_dbg.want_car = -1; g_dbg.want_track = -1;

    if (ImGui::BeginTabBar("MasterInspectorTabs")) {
    if(ImGui::BeginTabItem("Race Log")) {
        RaceLog *log=g_dbg.race_log;
        bool automatic=g_dbg.race_log_auto!=0;
        if(ImGui::Checkbox("Record each race automatically",&automatic))g_dbg.race_log_auto=automatic;
        ImGui::TextWrapped("Press . once where a problem occurs. It saves the exact location, ground asset, race progress and nearby vehicle states. Recording continues with the panel closed.");
        ImGui::InputText("Problem note",g_dbg.race_log_note,sizeof g_dbg.race_log_note);
        if(log) {
            ImGui::Text("%s | %ld samples | %ld marks",log->file?"Recording":"Stopped",log->samples,log->marks);
            if(ImGui::Button("Start recording"))g_dbg.race_log_start=1;
            ImGui::SameLine();
            if(ImGui::Button("Mark problem (.)"))g_dbg.race_log_mark=1;
            ImGui::SameLine();
            if(ImGui::Button("Finish and save"))g_dbg.race_log_stop=1;
            if(ImGui::Button("Flush to file"))g_dbg.race_log_flush=1;
            ImGui::SameLine();
            if(ImGui::Button("Copy file path"))ImGui::SetClipboardText(log->path);
            ImGui::TextWrapped("%s",log->path[0]?log->path:"A file will be created beside the executable when recording starts.");
            ImGui::TextWrapped("%s",log->status);
            ImGui::BeginChild("RaceProblemMarks",ImVec2(0,220),true);
            long first=log->marks>64?log->marks-64:0;
            for(long j=first;j<log->marks;j++) {
                const RaceLogMark *m=&log->recent[j%64];
                ImGui::Text("#%ld %.2fs event%d gate%d (%.2f, %.2f, %.2f) %.1f km/h",j+1,m->tick/60.0,m->event,m->gate,m->pos[0],m->pos[1],m->pos[2],m->speed);
                ImGui::TextWrapped("%s | %s",m->asset,m->note);
            }
            ImGui::EndChild();
            ImGui::TextDisabled("The file retains every mark; this panel shows the latest 64.");
        }
        ImGui::EndTabItem();
    }

    /* ---- Tab 1: Vehicle & Wheels ---- */
    if (ImGui::BeginTabItem("Modification")) {
        if (g_dbg.car_list && g_dbg.n_cars > 0) {
            if (ImGui::BeginCombo("Car", g_dbg.car_name)) {
                for (int i=0;i<g_dbg.n_cars;i++) {
                    bool selected=i==g_dbg.sel_car;
                    if (ImGui::Selectable(g_dbg.car_list[i],selected) && !selected) g_dbg.want_car=i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Car changes apply when the vehicle bundle is ready; the session stays open.");
        }
        ImGui::TextWrapped("Shop preview: changes are free for testing. Career purchases and shop entry restrictions are not active yet.");
        if (ImGui::BeginTabBar("ShopTabs")) {
        if (shop_tab("Body", ImVec4(0.35f, 0.9f, 0.45f, 1))) {
            ImGui::TextUnformatted("Green / Body Shop");
            ImGui::TextDisabled("Stop the car to replace parts.");
            for (int p=0; p<N2_PART_COUNT; p++)
                if (p!=N2_PART_AUDIO && p!=N2_PART_ENGINE) part_selector(p);
        if (ImGui::CollapsingHeader("Body kit preset")) {
            ImGui::Text("%s: %d available kits (including stock)",g_dbg.car_name,g_dbg.body_kit_count);
            bool moving=g_dbg.kmh>1.0f || g_dbg.kmh<-1.0f;
            ImGui::BeginDisabled(moving || g_dbg.body_kit_count<2);
            char label[32];
            snprintf(label,sizeof label,"KIT%02d%s",g_dbg.body_kit_current,
                     g_dbg.body_kit_current==0?" (stock)":"");
            if (ImGui::BeginCombo("Installed kit",label)) {
                for(int i=0;i<g_dbg.body_kit_count;i++) {
                    int kit=g_dbg.body_kit_ids[i];
                    snprintf(label,sizeof label,"KIT%02d%s",kit,kit==0?" (stock)":"");
                    if(ImGui::Selectable(label,kit==g_dbg.body_kit_current))g_dbg.body_kit_request=kit;
                    if(kit==g_dbg.body_kit_current)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if(g_dbg.body_kit_count==1)ImGui::TextDisabled("This car has no optional body kits in its archive.");
            ImGui::TextDisabled("Stop the car to change kits. K cycles available kits.");
            if(g_dbg.body_kit_status[0])ImGui::TextWrapped("%s",g_dbg.body_kit_status);
        }
        if (ImGui::CollapsingHeader("Rims", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (g_dbg.wheel_brands && g_dbg.wheel_brand_n > 0) {
                static const char *brands[32];
                int nb = g_dbg.wheel_brand_n < 32 ? g_dbg.wheel_brand_n : 32;
                for (int i = 0; i < nb; i++) brands[i] = g_dbg.wheel_brands[i];
                if (ImGui::Combo("wheel brand", &g_dbg.wheel_brand, brands, nb)) g_dbg.wheel_reload = 1;
                if (ImGui::SliderInt("wheel style", &g_dbg.wheel_style, 1, 8)) g_dbg.wheel_reload = 1;
                if (g_dbg.wheel_load_failed)
                    ImGui::TextWrapped("Could not load that wheel. Current wheels kept.");
            } else ImGui::TextDisabled("wheel library not loaded");
        }
            ImGui::TextWrapped("Not yet supported: mirrors, carbon fibre conversion, wide body kits, rim sizing.");
            ImGui::EndTabItem();
        }
        if (shop_tab("Specialties", ImVec4(1, 0.85f, 0.3f, 1))) {
            ImGui::TextUnformatted("Yellow / Car Specialties Shop");
            part_selector(N2_PART_AUDIO);
        if (ImGui::CollapsingHeader("Neon Underglow", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("neon on", (bool *)&g_dbg.neon_on);
            ImGui::ColorEdit3("neon colour", g_dbg.neon_col);
            ImGui::SliderFloat("intensity", &g_dbg.neon_str, 0.0f, 1.5f);
        }
            ImGui::TextWrapped("Driving effects: hold L for high beams, J to flash, N for nitro. Beam preview and aiming are in Lighting & Environment.");
            ImGui::TextWrapped("Not yet supported: custom gauges, doors, split hoods, hydraulics, engine/trunk neon, nitrous purge, spinners, window tint.");
            ImGui::EndTabItem();
        }
        if (shop_tab("Graphics", ImVec4(1, 0.4f, 0.4f, 1))) {
            ImGui::TextUnformatted("Red / Graphics Shop");
        if (ImGui::CollapsingHeader("Vinyl", ImGuiTreeNodeFlags_DefaultOpen)) {
            if(!g_dbg.vinyl_catalog_ready) {
                g_dbg.vinyl_catalog_request=1;
                ImGui::TextDisabled("Loading this car's vinyl catalogue...");
            } else {
                static ImGuiTextFilter filter;
                const char *current=g_dbg.vinyl_current>0 && g_dbg.vinyl_current<=g_dbg.vinyl_count
                    ? g_dbg.vinyl_names[g_dbg.vinyl_current-1] : "None";
                ImGui::Text("Selected: %s",current);
                if(ImGui::Button("Remove vinyl"))g_dbg.vinyl_request=0;
                ImGui::SameLine();ImGui::TextDisabled("%d designs",g_dbg.vinyl_count);
                filter.Draw("Search vinyl");
                if(ImGui::BeginListBox("##vinyl_list",ImVec2(-1,180))) {
                    if(ImGui::Selectable("None",g_dbg.vinyl_current==0))g_dbg.vinyl_request=0;
                    for(int i=0;i<g_dbg.vinyl_count;i++) {
                        if(!filter.PassFilter(g_dbg.vinyl_names[i]))continue;
                        ImGui::PushID(i);
                        if(ImGui::Selectable(g_dbg.vinyl_names[i],g_dbg.vinyl_current==i+1))
                            g_dbg.vinyl_request=i+1;
                        ImGui::PopID();
                    }
                    ImGui::EndListBox();
                }
                ImGui::TextDisabled("One vinyl at a time. Changing cars clears the selection.");
                if(g_dbg.vinyl_status[0])ImGui::TextWrapped("%s",g_dbg.vinyl_status);
            }
        }
        if (ImGui::CollapsingHeader("Body paint", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char *quality[] = { "Low", "Medium", "High" };
            ImGui::Combo("Vehicle detail", &g_dbg.vehicle_quality, quality, 3);
            ImGui::TextDisabled("Low: square shadow. Medium/High: mesh shadows and local city reflections.");
            ImGui::TextDisabled("Controls paint, clear coat and glass reflections for every car.");
            /* body paint -> u_PaintColor (uColor) when override is on; the draw
               loop reads g_dbg.paint for BODY/MISC meshes, so this repaints live. */
            ImGui::Checkbox("custom paint (override per-car colour)", (bool *)&g_dbg.paint_override);
            ImGui::ColorEdit3("Body colour", g_dbg.paint);
            if (ImGui::Button("Red"))    { g_dbg.paint_override=1; g_dbg.paint[0]=0.70f; g_dbg.paint[1]=0.05f; g_dbg.paint[2]=0.05f; } ImGui::SameLine();
            if (ImGui::Button("Blue"))   { g_dbg.paint_override=1; g_dbg.paint[0]=0.05f; g_dbg.paint[1]=0.10f; g_dbg.paint[2]=0.60f; } ImGui::SameLine();
            if (ImGui::Button("Black"))  { g_dbg.paint_override=1; g_dbg.paint[0]=0.02f; g_dbg.paint[1]=0.02f; g_dbg.paint[2]=0.03f; } ImGui::SameLine();
            if (ImGui::Button("Silver")) { g_dbg.paint_override=1; g_dbg.paint[0]=0.60f; g_dbg.paint[1]=0.62f; g_dbg.paint[2]=0.66f; }
            ImGui::SliderFloat("Clear coat", &g_dbg.body_clearcoat, 0.0f, 1.0f);
            bool shine=g_dbg.paint_shine!=0;
            if(ImGui::Checkbox("Streetlight paint shine",&shine))g_dbg.paint_shine=shine;
            ImGui::TextDisabled("Shine follows nearby lights; Low vehicle detail disables it.");
            ImGui::SliderFloat("Paint highlight", &g_dbg.body_spec, 0.05f, 1.0f);
            ImGui::SliderFloat("Paint reflection", &g_dbg.body_env, 0.0f, 2.0f, "%.2fx");
        }
        if (ImGui::CollapsingHeader("Rim paint", ImGuiTreeNodeFlags_DefaultOpen)) {
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
            ImGui::TextWrapped("Not yet supported: multiple vinyl layers, decals, individual part paint, metallic and pearlescent finishes. Matte is a planned OpenUG2 option.");
            ImGui::EndTabItem();
        }
        if (shop_tab("Performance", ImVec4(0.4f, 0.65f, 1, 1))) {
            ImGui::TextUnformatted("Blue / Performance Shop");
            if(g_dbg.perf_source_available) {
                const char *levels[]={"Stock","Street / L1","Pro / L2","Extreme / L3"};
                ImGui::Combo("Power curve",&g_dbg.perf_power_level,levels,4);
                ImGui::Text("Peak power: %.1f kW",g_dbg.perf_peak_kw[g_dbg.perf_power_level]);
                ImGui::Combo("Transmission",&g_dbg.perf_transmission_level,levels,4);
                int t=g_dbg.perf_transmission_level;
                ImGui::Text("%d gears, final drive %.3f",g_dbg.perf_gears[t],
                            g_dbg.perf_final_drive[t]);
                ImGui::TextDisabled("Values come from this car's GLOBALB record and affect driving live.");
            } else ImGui::TextDisabled("This vehicle has no validated GLOBALB performance record.");
            ImGui::Separator();
            ImGui::TextWrapped("The file proves four power curves and four transmissions. Exact ECU, engine and turbo product-to-curve mapping is still being decoded, so they are not given invented individual multipliers.");
            ImGui::TextWrapped("Nitrous, suspension, brakes, tyres and weight reduction remain pending.");
            ImGui::EndTabItem();
        }
        if (shop_tab("Safe House", ImVec4(0.8f, 0.55f, 1, 1))) {
            ImGui::TextUnformatted("Purple / Safe House");
            ImGui::TextWrapped("Owned inventory and saving are not implemented yet. This will be the place to swap, remove and refit purchased compatible parts without buying them again.");
            ImGui::TextDisabled("Installed parts (read-only preview)");
            for (int p=0; g_dbg.mod_parts && p<N2_PART_COUNT; p++) {
                const N2PartMenu *menu=g_dbg.mod_parts+p;
                const char *label="Stock / none";
                for(int i=0;i<menu->count;i++)
                    if(menu->options[i].value==g_dbg.mod_current.parts[p])label=menu->options[i].label;
                ImGui::Text("%s: %s",n2_part_labels[p],label);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        if(g_dbg.body_kit_status[0])ImGui::TextWrapped("%s",g_dbg.body_kit_status);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Vehicle Diagnostics")) {
        if (ImGui::CollapsingHeader("Wheel Stance (per-car, metres)", ImGuiTreeNodeFlags_DefaultOpen)) {
            /* Absolute stance for the active car -- edits apply next frame, since
               the wheel transforms are rebuilt from g_dbg.wheel every frame. */
            ImGui::SliderFloat("front axle Z", &g_dbg.wheel.front_axle,  0.0f, 2.5f, "%.3f m");
            ImGui::SliderFloat("rear axle Z",  &g_dbg.wheel.rear_axle,  -2.5f, 0.0f, "%.3f m");
            ImGui::SliderFloat("front track",  &g_dbg.wheel.front_track, 0.8f, 2.2f, "%.3f m");
            ImGui::SliderFloat("rear track",   &g_dbg.wheel.rear_track,  0.8f, 2.2f, "%.3f m");
            ImGui::SliderFloat("wheel hub height",&g_dbg.wheel.ride_y,  -0.5f, 0.5f, "%.3f m");
            ImGui::SliderFloat("body lowering", &g_dbg.body_drop, 0.0f, 0.20f, "%.3f m");
            ImGui::TextDisabled("Body lowering is limited by ground clearance; wheel contact stays fixed.");
            ImGui::Text("wheelbase %.3f m", g_dbg.wheel.front_axle - g_dbg.wheel.rear_axle);
            ImGui::SliderFloat("radius/scale", &g_dbg.wheel_scale, 0.3f, 2.0f);
            ImGui::Text("radius %.3f m   %.0f RPM   steer %+.1f deg",
                        g_dbg.wheel_radius, g_dbg.wheel_rpm, g_dbg.steer_deg);
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
        if (ImGui::CollapsingHeader("Engine cover mesh")) {
            ImGui::TextWrapped("Visual mesh preview only; this does not install an engine upgrade.");
            part_selector(N2_PART_ENGINE);
        }
        if (ImGui::CollapsingHeader("Car parts")) {
            ImGui::Checkbox("body",   (bool *)&g_dbg.show_body);   ImGui::SameLine();
            ImGui::Checkbox("glass",  (bool *)&g_dbg.show_glass);  ImGui::SameLine();
            ImGui::Checkbox("lights", (bool *)&g_dbg.show_lights);
            ImGui::Checkbox("tires",  (bool *)&g_dbg.show_tires);  ImGui::SameLine();
            ImGui::Checkbox("misc",   (bool *)&g_dbg.show_misc);   ImGui::SameLine();
            ImGui::Checkbox("track",  (bool *)&g_dbg.show_track);
            ImGui::TextDisabled("Paint controls: Modification > Graphics");
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
            /* Night mode: emissive light lenses + headlight beams/bloom + low
               ambient. Toggling applies an ambient preset; the slider below still
               fine-tunes it. The emissive/beam gating is read live in the draw. */
            if (ImGui::Checkbox("Night Mode", (bool *)&g_dbg.night_mode))
                g_dbg.ambient = g_dbg.night_mode ? 0.38f : 0.78f;
            ImGui::SameLine();
            ImGui::TextDisabled(g_dbg.night_mode ? "(lenses glow, headlights on)"
                                                 : "(daylight, lenses off)");
        }
        if (ImGui::CollapsingHeader("Rain & wet roads", ImGuiTreeNodeFlags_DefaultOpen)) {
            if(ImGui::Button("Dry")){g_dbg.rain_intensity=0;g_dbg.road_wetness=0;}
            ImGui::SameLine();
            if(ImGui::Button("After rain")){g_dbg.rain_intensity=0;g_dbg.road_wetness=0.85f;}
            ImGui::SameLine();
            if(ImGui::Button("Rain")){g_dbg.rain_intensity=0.7f;g_dbg.road_wetness=1;}
            ImGui::TextDisabled("Screen droplets; no world-space rain particles.");
            ImGui::SliderFloat("Rain intensity", &g_dbg.rain_intensity, 0, 1);
            ImGui::SliderFloat("Road wetness", &g_dbg.road_wetness, 0, 1);
            ImGui::Combo("Weather detail", &g_dbg.weather_quality, "Low\0Medium\0High\0");
            bool reflections=g_dbg.road_reflections!=0;
            if(ImGui::Checkbox("Road scene reflections (expensive)",&reflections))g_dbg.road_reflections=reflections;
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Reflects objects visible on screen. Low detail keeps surface highlights only.");
            if(g_dbg.road_reflection_draws<0)ImGui::TextDisabled("Scene reflections unavailable; using surface highlights.");
            else ImGui::TextDisabled("Visible-scene reflections: %d road draws (Medium/High)",g_dbg.road_reflection_draws);
            ImGui::TextWrapped("Visual preview: wetness stays at the selected value; driving grip is unchanged.");
        }
        if (ImGui::CollapsingHeader("Headlights", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Combo("Beam preview", &g_dbg.headlight_mode, "Low beam\0High beam\0Off\0");
            ImGui::TextWrapped("Hold L for high beam or J to flash. Enable Night Mode for steady lights. Change the installed lamp in Modification / Body.");
            ImGui::SliderFloat("Low beam downward angle", &g_dbg.low_beam_pitch, 1.0f, 10.0f, "%.1f deg");
            ImGui::SliderFloat("High beam downward angle", &g_dbg.high_beam_pitch, 0.0f, 5.0f, "%.1f deg");
            ImGui::SliderFloat("Low beam reach", &g_dbg.low_beam_range, 10.0f, 60.0f, "%.0f m");
            ImGui::SliderFloat("High beam reach", &g_dbg.high_beam_range, 40.0f, 140.0f, "%.0f m");
            ImGui::SliderFloat("Lamp intensity", &g_dbg.headlight_gain, 0.0f, 3.0f);
            ImGui::SliderFloat("Clear lens opacity", &g_dbg.headlight_lens_alpha, 0.0f, 0.6f);
            bool shadows = g_dbg.headlight_shadows != 0;
            if (ImGui::Checkbox("Headlight shadows", &shadows)) g_dbg.headlight_shadows = shadows;
            if (g_dbg.headlight_shadow_draws < 0)
                ImGui::TextDisabled("Shadow maps unavailable on this renderer.");
            else
                ImGui::TextDisabled("World shadow caster draws: %d", g_dbg.headlight_shadow_draws);
        }
        if (ImGui::CollapsingHeader("Camera (3rd-person chase)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("distance (back)", &g_dbg.chase_distance,  3.0f, 25.0f, "%.1f m");
            ImGui::SliderFloat("height (up)",     &g_dbg.chase_height,    1.0f, 15.0f, "%.1f m");
            ImGui::SliderFloat("stiffness (lerp)",&g_dbg.chase_stiffness, 0.02f, 1.0f, "%.2f/frame");
            if (ImGui::Button("reset chase cam")) {
                g_dbg.chase_distance=4.0f; g_dbg.chase_height=2.0f; g_dbg.chase_stiffness=0.22f;
            }
            ImGui::TextDisabled("low stiffness = looser spring; 1.0 = rigidly glued");
            ImGui::Separator();
            ImGui::Checkbox("Auto-Drive (camera test)", (bool *)&g_dbg.auto_drive);
            ImGui::TextDisabled("steady throttle + sine steer -> hands-free S-curve");
        }
        if (ImGui::CollapsingHeader("District lights", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("flare size", &g_dbg.light_halo, 0.2f, 3.0f, "%.2fx");
            ImGui::SliderFloat("flare brightness", &g_dbg.light_gain, 0.0f, 3.0f, "%.2fx");
            ImGui::TextDisabled("authored street/district lamps; Night Mode must be on");
        }
        if (ImGui::CollapsingHeader("Lighting / Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("ambient",   &g_dbg.ambient,   0.0f, 1.0f);
            ImGui::SliderFloat("diffuse",   &g_dbg.diffuse,   0.0f, 1.5f);
            ImGui::SliderFloat("fog density", &g_dbg.fog_density, 0.0f, 0.01f, "%.4f");
            ImGui::ColorEdit3("fog / sky colour", &g_dbg.fog_r);
        }
        if (ImGui::CollapsingHeader("Texture detail", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (g_tex_aniso_max <= 1) {
                ImGui::TextWrapped("Anisotropic filtering unavailable on this GL "
                                   "context; textures stay at plain trilinear.");
            } else {
                /* Live: render_texture_detail re-filters every resident texture,
                   so the picture changes while the slider moves rather than on
                   the next load. */
                int steps = 1; while ((1 << steps) <= g_tex_aniso_max) steps++;
                int cur = 0; while ((1 << (cur + 1)) <= (int)g_dbg.tex_detail &&
                                    (1 << (cur + 1)) <= g_tex_aniso_max) cur++;
                char label[32];
                snprintf(label, sizeof label, "%dx", 1 << cur);
                if (ImGui::SliderInt("anisotropy", &cur, 0, steps - 1, label))
                    g_dbg.tex_detail = (float)(1 << cur);
                render_texture_detail(g_dbg.tex_detail);
                ImGui::SameLine();
                ImGui::TextDisabled("(max %dx)", g_tex_aniso_max);
                ImGui::TextWrapped("Sharpens any surface seen edge-on -- road, "
                                   "kerbs, the car's own flanks. 1x is plain "
                                   "trilinear; a race frame changes 6.2%% of its "
                                   "pixels between 1x and 16x.");
            }
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
        ImGui::Text("car: %s (change in Modification)", g_dbg.car_name);
        if (g_dbg.track_list && g_dbg.n_tracks > 0) {
            static const char *items[64]; int n = g_dbg.n_tracks < 64 ? g_dbg.n_tracks : 64;
            for (int i = 0; i < n; i++) items[i] = g_dbg.track_list[i];
            int cur = g_dbg.sel_track;
            if (ImGui::Combo("track", &cur, items, n) && cur != g_dbg.sel_track) g_dbg.want_track = cur;
        } else ImGui::Text("track: %s (%d/%d)", g_dbg.track_name, g_dbg.sel_track+1, g_dbg.n_tracks);
        ImGui::Text("circuit: %d/%d   |   %d track meshes", g_dbg.sel_circuit+1, g_dbg.n_circuits, g_dbg.track_meshes);
        ImGui::SeparatorText("Traffic");
        ImGui::BeginDisabled(!g_dbg.traffic_available);
        ImGui::SliderInt("Traffic density", &g_dbg.traffic_target, 0, g_dbg.traffic_max, "%d cars");
        if(ImGui::Button("Default traffic"))g_dbg.traffic_target=4;
        ImGui::Checkbox("Draw off-screen traffic (diagnostic)",(bool *)&g_dbg.traffic_cull_off);
        ImGui::EndDisabled();
        ImGui::Text("Active traffic: %d / %d   Roaming racers: %d   On screen: %d",
                    g_dbg.traffic_active,g_dbg.traffic_target,g_dbg.traffic_racers,
                    g_dbg.traffic_visible);
        ImGui::TextWrapped("Applies live. Cars spawn on clear roads out of view; excess cars leave when out of view. Roaming racers are unchanged.");
        if(!g_dbg.traffic_available)ImGui::TextDisabled("Available in open world and road races; closed venues exclude traffic.");
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

    /* ---- Tab: Placement Marks ---- */
    if (ImGui::BeginTabItem("Placement Marks")) {
        static bool hotkeys = true;
        static char status[256] = "";
        ImGui::TextWrapped("Drive onto a spot, then mark it. M = the "
                           "defect, Shift+M = where it should be. Fill the note first "
                           "so both ends of a pair share a name. (F9/F10 still work "
                           "where the function row is reachable.)");
        ImGui::Separator();
        ImGui::TextUnformatted("probe   car");
        ImGui::Text("XYZ     %9.2f  %9.2f  %8.2f",
                    g_dbg.probe[0], g_dbg.probe[1], g_dbg.probe[2]);
        ImGui::Text("ground  z=%8.2f  %s", g_dbg.probe_ground_z,
                    mark_cat_name(g_dbg.probe_ground_cat));
        ImGui::Text("under   %s", g_dbg.probe_asset[0] ? g_dbg.probe_asset : "(no covering surface)");
        ImGui::Text("district %s", g_dbg.zone_name[0] ? g_dbg.zone_name : "-");
        ImGui::Separator();
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("note", g_mark_note, sizeof g_mark_note);
        ImGui::Checkbox("M / Shift+M hotkeys", &hotkeys);
        /* letter hotkeys must stand down while the note field has focus, or
           typing a note fires a mark per keystroke. F9/F10 keep working as
           aliases -- on a Mac the function row is behind fn/Mission Control. */
        if (hotkeys && !ImGui::GetIO().WantTextInput) {
            int shift = ImGui::GetIO().KeyShift;
            if (ImGui::IsKeyPressed(ImGuiKey_M, false)) mark_capture(shift ? 1 : 0);
        }
        if (hotkeys && ImGui::IsKeyPressed(ImGuiKey_F9, false))  mark_capture(0);
        if (hotkeys && ImGui::IsKeyPressed(ImGuiKey_F10, false)) mark_capture(1);
        if (ImGui::Button("Mark DEFECT (M)"))    mark_capture(0);
        ImGui::SameLine();
        if (ImGui::Button("Mark SHOULD-BE (Shift+M)")) mark_capture(1);
        ImGui::SameLine();
        ImGui::BeginDisabled(g_nmarks == 0);
        if (ImGui::Button("Undo last")) g_nmarks--;
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) { g_nmarks = 0; status[0] = 0; }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(g_nmarks == 0);
        if (ImGui::Button("Copy all to clipboard")) {
            static char blob[MARK_MAX * 320 + 128];
            mark_text(blob, sizeof blob);
            ImGui::SetClipboardText(blob);
            snprintf(status, sizeof status, "%d mark(s) copied -- paste them straight into the report", g_nmarks);
        }
        ImGui::SameLine();
        if (ImGui::Button("Write placement_marks.txt")) {
            static char blob[MARK_MAX * 320 + 128];
            mark_text(blob, sizeof blob);
            FILE *f = fopen("placement_marks.txt", "w");
            if (f) { fputs(blob, f); fclose(f);
                     snprintf(status, sizeof status, "wrote placement_marks.txt (%d marks) next to the working directory", g_nmarks); }
            else   snprintf(status, sizeof status, "could not write placement_marks.txt");
        }
        ImGui::EndDisabled();
        if (status[0]) ImGui::TextDisabled("%s", status);
        ImGui::Separator();
        ImGui::Text("%d mark(s)", g_nmarks);
        ImGui::BeginChild("marklist", ImVec2(0, 260), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (int i = 0; i < g_nmarks; i++) {
            PlacementMark *m = &g_marks[i];
            ImGui::PushStyleColor(ImGuiCol_Text, m->target
                ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)    /* should-be */
                : ImVec4(0.95f, 0.55f, 0.35f, 1.0f));  /* defect */
            ImGui::Text("%02d %-9s", i + 1, m->target ? "SHOULD-BE" : "DEFECT");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%8.1f %8.1f %7.1f  gz%7.1f %-7s %-28s %s",
                        m->pos[0], m->pos[1], m->pos[2], m->ground_z,
                        mark_cat_name(m->cat), m->asset[0] ? m->asset : "-",
                        m->note);
        }
        if (!g_nmarks) ImGui::TextDisabled("(nothing marked yet)");
        ImGui::EndChild();
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
        ImGui::Checkbox("Free camera (F)", (bool *)&g_dbg.freecam);
        ImGui::TextWrapped("Right mouse: orbit the car. F: free camera, WASD move, Q/E down/up, Shift faster.");
        ImGui::EndTabItem();
    }

    /* ---- Tab 5: Navigation & Races ---- */
    if (ImGui::BeginTabItem("Navigation & Races")) {
    ImGui::SeparatorText("Collision walls (3D)");
    ImGui::Checkbox("Show collision walls",(bool *)&g_dbg.wall_show);
    if(g_dbg.wall_show) {
        ImGui::SliderFloat("Wall radius",&g_dbg.wall_range,25,200,"%.0f m");
        ImGui::Checkbox("Show through scenery",(bool *)&g_dbg.wall_through);
        ImGui::Checkbox("Walls near player height (+/-4 m)",(bool *)&g_dbg.wall_height);
        ImGui::TextWrapped("Cyan: object walls (%d faces). Orange: road/terrain walls (%d). Pink: generated barrier/planter boundaries (%d).",
            g_dbg.wall_faces[0],g_dbg.wall_faces[1],g_dbg.wall_faces[2]);
        ImGui::TextWrapped("Candidate faces at their current heights. Body height, buried faces and contact span determine actual collisions. This view does not change physics.");
        if(g_dbg.wall_truncated)ImGui::TextColored(ImVec4(1,.6f,.1f,1),"Face limit reached; reduce the radius.");
    }
    heatmap_controls();
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
        if (side > (g_dbg.heat_layer?450.0f:300.0f)) side = g_dbg.heat_layer?450.0f:300.0f;   /* leave room for the track manager below */
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x+side, p0.y+side), IM_COL32(12,14,20,255));
        float x0=g_dbg.navbb[0], x1=g_dbg.navbb[1], y0=g_dbg.navbb[2], y1=g_dbg.navbb[3];
        float w = x1-x0, h = y1-y0, span = w > h ? w : h;
        if (span < 1.0f) span = 1.0f;
        if(g_dbg.heat_layer && g_dbg.heat_local) {
            span=fmaxf(100,g_dbg.heat_span);x0=g_dbg.car[0]-span*.5f;y0=g_dbg.car[1]-span*.5f;
        }
        dl->PushClipRect(p0,ImVec2(p0.x+side,p0.y+side),true);
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
                                    g_dbg.heat_layer?IM_COL32(65,70,80,160):
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
        if(g_dbg.heat_layer)draw_heatmap(dl,p0,side,x0,y0,span);
        /* player */
        float px = MAPX(g_dbg.car[0]), py = MAPY(g_dbg.car[1]);
        dl->AddCircleFilled(ImVec2(px, py), 4.0f, IM_COL32(255,80,60,255));
        float hx = px + cosf(g_dbg.heading)*11.0f;
        float hy = py - sinf(g_dbg.heading)*11.0f;   /* Y flipped */
        dl->AddLine(ImVec2(px,py), ImVec2(hx,hy), IM_COL32(255,220,90,255), 2.0f);
        dl->PopClipRect();
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
        ImGui::SliderFloat("Opponent pace",&g_dbg.race_pace,40,180,"%.0f km/h");
        ImGui::TextDisabled("Corners, traffic and fitted vehicle limits reduce this pace.");
        int mode = g_dbg.mode;
        if (ImGui::RadioButton("Freeroam Mode", mode == MODE_FREEROAM)) {
            g_dbg.want_mode = MODE_FREEROAM; g_dbg.want_event = -1; g_dbg.mode_request = 1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(mode == MODE_RACE_EVENT
            ? "race event active" : "whole city drivable, no barriers");

        ImGui::Text("%d race layouts in this region", g_dbg.ev_count);
        if(g_dbg.race_error[0])ImGui::TextWrapped("%s",g_dbg.race_error);
        if (mode == MODE_RACE_EVENT && g_dbg.active_ev >= 0) {
            const WEvent &e = g_dbg.ev[g_dbg.active_ev];
            ImGui::TextColored(ImVec4(1.0f,0.85f,0.3f,1.0f),
                "event %d  %s  %s  ~%d00 m  |  %d barriers, %d links masked",
                e.id, e.reg, n2_race_name(e.info.kind), e.len100m,
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
            if(R->kind==N2_RACE_DRIFT)ImGui::Text("Drift score: %.0f  chain: +%.0f",R->drift.bank,R->drift.chain);
            if(R->kind==N2_RACE_DRAG)ImGui::TextDisabled("W/S throttle/brake, A/D or arrows change one lane, E/Q shift up/down");
            if (R->finished) ImGui::TextColored(ImVec4(0.4f,1.0f,0.5f,1.0f),
                R->failed==1?"ENGINE BLOWN":R->failed==2?"WRECKED":"RACE FINISHED");
            if (ImGui::Button("Stop race")) g_dbg.race_stop_request = 1;
            ImGui::SameLine();
            if (ImGui::Button("Restart")) g_dbg.race_start_request = 1;
        } else if (g_dbg.mode == MODE_RACE_EVENT) {
            ImGui::Separator();
            if (ImGui::Button("Start race")) g_dbg.race_start_request = 1;
            ImGui::SameLine();
            if (g_dbg.active_ev>=0 && g_dbg.ev[g_dbg.active_ev].circuit) {
                ImGui::SetNextItemWidth(110);
                ImGui::SliderInt("laps", &g_dbg.race_maxlaps_want, 1, 8);
            }
        }

        static int race_filter=0;
        ImGui::Combo("Race type",&race_filter,"All\0Circuit\0Sprint\0Drag\0Drift\0Street X\0URL\0");
        if (ImGui::BeginListBox("##events", ImVec2(-1, 180))) {
            for (int i = 0; i < g_dbg.ev_count; i++) {
                const WEvent &e = g_dbg.ev[i];
                if(race_filter && e.info.kind!=race_filter)continue;
                char lbl[96];
                snprintf(lbl, sizeof lbl, "%d  %-4s  %-7s  ~%d00 m  (%d nodes)",
                         e.id, e.reg, n2_race_name(e.info.kind),
                         e.len100m, e.node1 - e.node0);
                if (ImGui::Selectable(lbl, mode == MODE_RACE_EVENT && i == g_dbg.active_ev)) {
                    g_dbg.want_mode = MODE_RACE_EVENT; g_dbg.want_event = i;
                    g_dbg.mode_request = 1;
                }
                if (ImGui::IsItemHovered() && e.info.name[0]) ImGui::SetTooltip("%s", e.info.name);
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
