# OpenUG2 — build
#
#   make               desktop build without the temporary frontend menu
#   make normal        explicit alias for the normal build
#   make menu          desktop build with the temporary standalone menu
#   make gles          OpenGL ES 2.0 build (embedded/mobile ARM)
#   make debug         desktop build + Dear ImGui debug overlay
#   make run DATA=DIR  build + run against your NFSU2 data directory
#   make clean
#
# Requires SDL2 (brew install sdl2 · apt install libsdl2-dev) and zlib.

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
# nfsu2.h is a static-function single-header parser shared by several modules;
# each module uses only part of it, so silence the per-TU unused copies.
CFLAGS  += -Wno-unused-function
# M_PI and scandir/alphasort are not declared by glibc under strict -std=c99.
override CFLAGS += -D_DEFAULT_SOURCE
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
DATA    ?= .

UNAME := $(shell uname)
WINDOWS := $(filter MINGW% MSYS%,$(UNAME))
EXEEXT := $(if $(WINDOWS),.exe)
GAME := nfsu2$(EXEEXT)
ifeq ($(UNAME),Darwin)
  # Keep C and C++ on the SDK belonging to the selected Xcode toolchain.
  ifndef SDKROOT
    export SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
  endif
  GL_LIBS := -framework OpenGL
else ifneq ($(WINDOWS),)
  override CFLAGS += -D_USE_MATH_DEFINES
  # Native MinGW/UCRT binaries; keep a console for diagnostics.
  SDL_LIBS := $(filter-out -mwindows,$(SDL_LIBS))
  GL_LIBS := $(shell pkg-config --libs glew) -lopengl32
else
  GL_LIBS := -lGL
endif

UGUI_CFLAGS := $(shell pkg-config --cflags libpng libcrypto)
UGUI_IMAGE_LIBS := $(shell pkg-config --libs libpng libcrypto)
override CFLAGS += $(UGUI_CFLAGS)
# engine modules: orchestrator + Renderer/Physics/AI/Audio/Resources/World
GAME_UI_SRC := $(wildcard src/ugui/*.c) src/pause_menu.c src/pause_screen.c src/pause_entries.c src/gui_view.c src/gui_carousel.c src/gui_dialog.c src/gui_layout.c src/gui_timing.c src/options_menu.c src/settings.c src/controls_bind.c src/controls_menu.c src/controls_store.c src/controls_sdl.c src/frontend/frontend.c src/frontend/frontend_draw.c
SRC  := src/main.c src/render.c src/render_params.c src/physics.c src/drivetrain.c src/braking.c src/resistance.c src/lateral.c src/drive_stage1.c src/contact.c src/body_response.c src/drive_contact.c src/tire_marks.c src/ai.c src/audio.c src/resource.c src/world.c src/world_instance.c src/world_resident.c src/world_mesh.c src/hud.c src/hud_asset.c src/hud_texture.c src/hud_baseline.c src/rhi_gl_state.c src/rhi_gl.c src/camera.c src/tex_override.c $(GAME_UI_SRC)
HDRS := src/race_ai_source.h src/world_collision_rules.h src/world_collision.h src/race_log.h src/race.h src/traffic_heatmap.h src/asset_chunks.h src/car_config.h src/car_mod.h src/nfsu2.h src/render.h src/physics.h src/ai.h src/audio.h src/resource.h src/debug.h src/world.h src/world_instance.h src/world_resident.h src/world_mesh.h src/world_capture_policy.h src/world_group_reader.h src/world_scenery.h src/ground_motion.h src/hud.h src/hud_asset.h src/hud_texture.h src/hud_baseline.h src/hud_host.h src/rhi_gl_state.h src/rhi.h src/rhi_gl.h src/render_params.h src/camera.h src/tex_override.h
HDRS += src/options_menu.h src/settings.h src/controls_bind.h src/controls_menu.h src/controls_store.h src/controls_sdl.h src/pause_menu_binding.h src/pause_menu.h src/pause_menu_layout.h src/pause_screen.h src/pause_entries.h src/gui_view.h src/gui_carousel.h src/gui_dialog.h src/gui_layout.h src/gui_timing.h src/frontend/frontend.h src/frontend/frontend_draw.h $(wildcard src/ugui/*.h)

# Pure (GL-free, SDL-free) building blocks of the authored-art screens, shared by the game and the tests.
GUI_ART_SRC := src/gui_timing.c src/pause_entries.c src/gui_carousel.c src/gui_dialog.c src/gui_layout.c src/pause_screen.c src/controls_bind.c src/controls_menu.c
GUI_ART_HDR := $(wildcard src/gui_*.h) src/pause_entries.h src/pause_screen.h src/controls_bind.h src/controls_menu.h

HDRS += src/ug2_track_collision.h src/drivetrain.h src/braking.h src/resistance.h src/lateral.h src/drive_stage1.h src/contact.h src/body_response.h src/tire_marks.h src/tire_marks_drive.h
HDRS += src/drive_trace.h src/drive_presentation.h src/drive_session.h
SRC += src/hud_race.c
HDRS += src/hud_race.h src/dir_scan.h
HDRS += src/player_motion.h

hud-race-test: tools/hud_race_test.c src/hud_race.c src/hud_race.h src/hud.h src/hud_asset.c src/hud_asset.h src/ugui/ugui_codec.c
	@mkdir -p build
	$(CC) $(CFLAGS) -DSDL_MAIN_HANDLED $(filter-out -Dmain=SDL_main,$(SDL_CFLAGS)) -Isrc tools/hud_race_test.c src/hud_race.c src/hud_asset.c src/ugui/ugui_codec.c -o build/hud_race_test -lm
	./build/hud_race_test
.PHONY: hud-race-test

.DEFAULT_GOAL := $(GAME)   # keep `make` building the binary, not the generated header

# Renderer-boundary gate (issue #14, P10): fail if any src/ file outside the GL
# backend names a GL header, a gl* entry point or a GL* type. The allowed GL
# owners are listed in tools/check_gl.sh: the backend, Dear ImGui's GL2 backend
# (debugui.cpp) and the ugui library's GL backend (ugui/ugui_gl.c) -- the two
# self-contained UI renderers that keep the GL context state themselves.
check-gl:
	@sh tools/check_gl.sh src
.PHONY: check-gl

# Auto-embed the F3 debug shaders so world_mesh.c has a CWD-independent fallback
# with no hand-maintained string copy. Relative input paths give stable array
# names src_world_debug_120_vert / _frag (+ *_len). Regenerated when either
# shader changes; a build-time artifact (gitignored, not committed).
GEN := src/generated_world_debug_shaders.h src/generated_shaders.h
src/generated_world_debug_shaders.h: src/world_debug_120.vert src/world_debug_120.frag
	@echo "/* AUTO-GENERATED from world_debug_120.vert/.frag by make -- do not edit */" > $@
	@xxd -i src/world_debug_120.vert >> $@
	@xxd -i src/world_debug_120.frag >> $@
# Renderer shader sources (src/shaders/), embedded the same way: arrays named
# src_shaders_<file>_<ext> (+ *_len), not NUL-terminated. Made static const so
# more than one file can include the header.
SHADERS := $(sort $(wildcard src/shaders/*.vert src/shaders/*.frag))
src/generated_shaders.h: $(SHADERS)
	@echo "/* AUTO-GENERATED from src/shaders/ by make -- do not edit */" > $@
	@for f in $(SHADERS); do xxd -i $$f | sed 's/^unsigned/static const unsigned/' >> $@; done

ifneq ($(WINDOWS),)
# Preserve explicit `make nfsu2` calls while tracking the actual .exe output.
nfsu2: $(GAME)
.PHONY: nfsu2
endif

$(GAME): $(SRC) $(HDRS) $(GEN)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SRC) -o $(GAME) $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lz -lm

# Explicit build variants. Both write the same `nfsu2` executable; the last
# target built is the one that is currently on disk.
normal: $(SRC) $(HDRS) $(GEN)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SRC) -o $(GAME) $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lz -lm

menu: $(SRC) $(HDRS) $(GEN)
	$(CC) $(CFLAGS) -DOPENUG2_MENU $(SDL_CFLAGS) $(SRC) \
		-o $(GAME) \
		$(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lz -lm

# Dev build with the Dear ImGui debug overlay + freecam controls (`make debug`).
# The engine stays C; ImGui + the wrapper compile as C++ and link together.
CXX      ?= c++
IMGUI_DIR := third_party/imgui
IMGUI_OBJ := build/imgui.o build/imgui_draw.o build/imgui_tables.o build/imgui_widgets.o \
             build/imgui_impl_sdl2.o build/imgui_impl_opengl2.o build/debugui.o

build/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p build
	$(CXX) -O2 $(SDL_CFLAGS) -I$(IMGUI_DIR) -c $< -o $@
build/%.o: $(IMGUI_DIR)/backends/%.cpp
	@mkdir -p build
	$(CXX) -O2 $(SDL_CFLAGS) -I$(IMGUI_DIR) -c $< -o $@
build/debugui.o: src/debugui.cpp src/rhi_gl_state.h src/race_log.h src/race.h src/debug.h src/race_ai_source.h src/car_config.h src/traffic_heatmap.h
	@mkdir -p build
	$(CXX) -O2 $(SDL_CFLAGS) -I$(IMGUI_DIR) -c src/debugui.cpp -o $@

DBG_OBJ := $(SRC:src/%.c=build/%.dbg.o)
build/%.dbg.o: src/%.c $(HDRS) $(GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DDEBUG_UI -c $< -o $@

debug: $(DBG_OBJ) $(IMGUI_OBJ)
	$(CXX) -O2 $(DBG_OBJ) $(IMGUI_OBJ) -o $(GAME) $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lz -lm

# Gameplay camera vs docs/cleanroom-camera/test-vectors (docs/CAMERA.md). DATA may point
# at an install to also check the real GLOBALB through the loader (structure only);
# FULL may name a local full per-car table (tv01 columns) to compare values too.
camera-test: tools/camera_test.c src/camera.c src/camera.h src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/camera_test.c src/camera.c -o build/camera_test -lm
	./build/camera_test docs/cleanroom-camera/test-vectors "$(DATA)" "$(FULL)"

.PHONY: camera-test

# Stage 1 drivetrain / acceleration vs docs/cleanroom-driving-physics/test-vectors
# (docs/cleanroom-driving-physics/IMPL_DESIGN.md). Pure maths tests: no SDL/GL.
drivetrain-test: tools/drivetrain_test.c src/drivetrain.c src/braking.c src/drivetrain.h src/braking.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/drivetrain_test.c src/drivetrain.c src/braking.c -o build/drivetrain_test -lm
	./build/drivetrain_test "$(VECTORS)"

.PHONY: drivetrain-test

# Tire marks vs docs/cleanroom-driving-physics/tire-marks/test-vectors
# (docs/cleanroom-driving-physics/tire-marks/IMPL_DESIGN.md). Pure maths: no SDL/GL.
TIRE_MARKS_VECTORS ?= docs/cleanroom-driving-physics/tire-marks/test-vectors
tire-marks-test: tools/tire_marks_test.c tools/test_json.h src/tire_marks.c src/tire_marks.h src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -Itools tools/tire_marks_test.c src/tire_marks.c -o build/tire_marks_test -lm
	./build/tire_marks_test "$(TIRE_MARKS_VECTORS)"

.PHONY: tire-marks-test

# GL regression of the mark pass (SPEC TM-15): hidden SDL/GL window, no game data.
tire-marks-render-test: tools/tire_marks_render_test.c tools/test_json.h src/tire_marks.c src/tire_marks.h src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/render.h src/nfsu2.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc -Itools tools/tire_marks_render_test.c src/tire_marks.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c -o build/tire_marks_render_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/tire_marks_render_test "$(TIRE_MARKS_VECTORS)"

.PHONY: tire-marks-render-test

world-instance-test: tools/world_instance_test.c src/world_instance.c src/physics.c src/physics.h src/world_instance.h src/world_capture_policy.h src/nfsu2.h src/world_group_reader.h src/world_scenery.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DWORLD_INSTANCE_TESTING -Isrc tools/world_instance_test.c src/world_instance.c src/physics.c -o build/world_instance_test -lm
	./build/world_instance_test

vinyl-census: tools/vinyl_census.c src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/vinyl_census.c -o build/vinyl_census -lm

texkey-probe: tools/texkey_probe.c src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/texkey_probe.c -o build/texkey_probe -lz -lm

car-material-test: tools/car_material_test.c src/nfsu2.h src/car_config.h src/car_mod.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/car_material_test.c -o build/car_material_test -lm
	./build/car_material_test

# HUD maths only: no GL calls are executed, but hud.c still references the GL
# symbols, so the link needs the same libraries as the engine.
hud-test: tools/hud_test.c src/hud.c src/hud_race.c src/hud_race.h src/ugui/ugui_codec.c src/hud.h src/hud_host.h src/hud_asset.h src/hud_baseline.h src/hud_texture.h src/audio.h src/render.c src/render_params.c src/render.h src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/nfsu2.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/hud_test.c src/hud.c src/hud_race.c src/ugui/ugui_codec.c src/hud_asset.c src/hud_texture.c src/hud_baseline.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c \
		-o build/hud_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/hud_test


# Asset-free native check of the baseline HUD's caller state.
hud-state-test: tools/hud_state_test.c src/render.c src/render_params.c src/hud_baseline.c src/hud_asset.c src/hud_texture.c src/rhi_gl.c src/rhi_gl_state.c src/tex_override.c $(HDRS) $(GEN)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/hud_state_test.c src/render.c src/render_params.c src/hud_baseline.c src/hud_asset.c src/hud_texture.c src/rhi_gl.c src/rhi_gl_state.c src/tex_override.c -o build/hud_state_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/hud_state_test
.PHONY: hud-state-test

# Asset-free check that the authored hud_draw hands the caller's vertex-attribute descriptions and element-buffer binding back
# (uses the hud_test_white_widget hook, compiled in only here).
hud-caller-state-test: tools/hud_caller_state_test.c src/hud.c src/hud_race.c src/ugui/ugui_codec.c src/hud_asset.c src/hud_texture.c src/hud_baseline.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c $(HDRS) $(GEN)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DHUD_TEST_HOOKS -Isrc tools/hud_caller_state_test.c src/hud.c src/hud_race.c src/ugui/ugui_codec.c src/hud_asset.c src/hud_texture.c src/hud_baseline.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c -o build/hud_caller_state_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/hud_caller_state_test
.PHONY: hud-caller-state-test

world-render-test: tools/world_render_test.c src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/world_render_test.c -o build/world_render_test -lm
	./build/world_render_test

world-resident-test: tools/world_resident_test.c src/world_resident.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DWORLD_RESIDENT_TESTING -Isrc tools/world_resident_test.c src/world_resident.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/world_resident_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/world_resident_test

physics-clock-test: tools/physics_clock_test.c src/physics.c src/physics.h src/nfsu2.h src/ai.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/physics_clock_test.c src/physics.c -o build/physics_clock_test -lm
	./build/physics_clock_test

.PHONY: physics-clock-test

district-collision-test: tools/district_collision_test.c src/physics.c src/physics.h src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/district_collision_test.c src/physics.c -o build/district_collision_test -lm
	./build/district_collision_test

ground-motion-test: tools/ground_motion_test.c src/ground_motion.h src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/ground_motion_test.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/ground_motion_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/ground_motion_test

ug2-track-collision-test: tools/ug2_track_collision_test.c src/ug2_track_collision.h src/asset_chunks.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/ug2_track_collision_test.c -o build/ug2_track_collision_test -lm
	./build/ug2_track_collision_test

.PHONY: ug2-track-collision-test

# S5-11: world polygon attribute -> numeric surface ID vectors (surface_mapping.json).
surface-mapping-test: tools/surface_mapping_test.c tools/test_json.h src/contact.c src/contact.h src/resistance.c src/braking.c src/ug2_track_collision.h src/asset_chunks.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/surface_mapping_test.c src/contact.c src/resistance.c src/braking.c -o build/surface_mapping_test -lm
	./build/surface_mapping_test

.PHONY: surface-mapping-test

# S5-11 lookup acceleration: indexed select vs a brute-force reference over
# synthetic sets (equivalence) plus a 100k-polygon timing comparison.
surface-index-test: tools/surface_index_test.c src/ug2_track_collision.h src/asset_chunks.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/surface_index_test.c -o build/surface_index_test -lm
	./build/surface_index_test

.PHONY: surface-index-test

world-collision-test: tools/world_collision_test.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/world_collision_test.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/world_collision_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/world_collision_test

traffic-heatmap-test: tools/traffic_heatmap_test.c src/traffic_heatmap.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/traffic_heatmap_test.c -o build/traffic_heatmap_test -lm
	./build/traffic_heatmap_test

ai-drive-test: tools/ai_drive_test.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/ai_drive_test.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/ai_drive_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/ai_drive_test

fence-impact-test: tools/fence_impact_test.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/fence_impact_test.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/fence_impact_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/fence_impact_test

.PHONY: fence-impact-test

race-log-test: tools/race_log_test.c src/race_log.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/race_log_test.c -o build/race_log_test
	./build/race_log_test

race-ai-test: tools/race_ai_test.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/race_ai_test.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/race_ai_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/race_ai_test

# Local asset check: optional --drive [event-id] runs normal physics/contact.
race-events-check: tools/race_events_check.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/race_events_check.c src/ai.c src/world.c src/resource.c src/world_instance.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/race_events_check $(SDL_LIBS) $(GL_LIBS) -lz -lm

world-group-test: tools/world_group_test.c src/world_group_reader.h
	@mkdir -p build
	$(CC) $(CFLAGS) tools/world_group_test.c -o build/world_group_test
	./build/world_group_test

world-group-audit: tools/world_group_audit.c src/world_group_reader.h src/world_scenery.h src/world_instance.c src/world_instance.h src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) tools/world_group_audit.c -o build/world_group_audit -lm

route-membership-audit: tools/m160_route_audit.c src/world_group_reader.h src/world_scenery.h src/world_instance.c src/world_instance.h src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) tools/m160_route_audit.c -o build/m160_route_audit -lm

wheel-render-test: tools/wheel_render_test.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/render.h src/nfsu2.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/wheel_render_test.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c -o build/wheel_render_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/wheel_render_test

wheel-reload-test: tools/wheel_reload_test.c $(SRC) $(HDRS) $(GEN)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/wheel_reload_test.c $(filter-out src/main.c,$(SRC)) -o build/wheel_reload_test $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lz -lm
	./build/wheel_reload_test

.PHONY: wheel-reload-test

body-kit-test: wheel-reload-test
modification-test: wheel-reload-test
.PHONY: modification-test
.PHONY: body-kit-test

# UG2 parameter block layout: std140 rules + every ug2.* uniform present.
.PHONY: param-layout-test
param-layout-test: tools/param_layout_test.c src/render_params.c src/render_params.h src/rhi.h src/rhi_gl.c src/rhi_gl_state.c src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/param_layout_test.c src/render_params.c src/rhi_gl.c src/rhi_gl_state.c -o build/param_layout_test $(SDL_LIBS) $(GL_LIBS)
	./build/param_layout_test

.PHONY: rhi-pipeline-test
rhi-pipeline-test: tools/rhi_pipeline_test.c src/rhi.h src/rhi_gl.c src/rhi_gl_state.c src/rhi_gl_state.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/rhi_pipeline_test.c src/rhi_gl.c src/rhi_gl_state.c -o build/rhi_pipeline_test $(SDL_LIBS) $(GL_LIBS)
	./build/rhi_pipeline_test

light-state-test: tools/light_state_test.c tools/legacy_draw.h src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/render.h src/nfsu2.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/light_state_test.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c -o build/light_state_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/light_state_test

world-texture-test: tools/world_texture_test.c src/world.c src/resource.c src/world_instance.c src/world_resident.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c $(HDRS) src/world.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/world_texture_test.c src/world.c src/resource.c src/world_instance.c src/world_resident.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c -o build/world_texture_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/world_texture_test

world-cli-test: nfsu2
	@set +e; \
	./nfsu2 --spawn >/dev/null 2>&1; spawn_missing=$$?; \
	./nfsu2 --heading >/dev/null 2>&1; heading_missing=$$?; \
	./nfsu2 --spawn malformed >/dev/null 2>&1; spawn_bad=$$?; \
	./nfsu2 --heading malformed >/dev/null 2>&1; heading_bad=$$?; \
	./nfsu2 --scenery-preview >/dev/null 2>&1; scenery_missing=$$?; \
	./nfsu2 --scenery-preview banana >/dev/null 2>&1; scenery_bad=$$?; \
	./nfsu2 --scenery-preview free --instance-audit >/dev/null 2>&1; scenery_legacy=$$?; \
	./nfsu2 --world2 --spawn start --track STREAML4RA --scenery-preview free >/dev/null 2>&1; scenery_live=$$?; \
	./nfsu2 /nonexistent/openug-test-data --world2 --spawn start --track STREAML4RA --scenery-preview free --shot /dev/null --static-spawn-audit STREAML4RA >/dev/null 2>&1; scenery_spawn=$$?; \
	./nfsu2 /nonexistent/openug-test-data --world2 --spawn start --track STREAML4RA --scenery-preview free --shot /dev/null --surface-stack STREAML4RA 0 0 >/dev/null 2>&1; scenery_stack=$$?; \
	./nfsu2 build/no-such-data-root --track STREAML4RA --world2 --spawn start \
	  --objdump /dev/null >/dev/null 2>&1; world2_load_fail=$$?; \
	./nfsu2 --resident-audit 1400 0 0 --spawn start --track STREAML4RA \
	  >/dev/null 2>&1; resident_legacy=$$?; \
	./nfsu2 --world2 --resident-audit 1400 0 0 --spawn start --track ALL \
	  >/dev/null 2>&1; resident_all=$$?; \
	./nfsu2 --world2 --resident-audit 1400 0 0 --spawn start --track STREAML4RA \
	  --event 4001 >/dev/null 2>&1; resident_race=$$?; \
	./nfsu2 build/no-such-data-root --world2 --resident-audit 1400 0 0 \
	  --spawn start --track STREAML4RA >/dev/null 2>&1; resident_valid=$$?; \
	./nfsu2 --resident-route-audit /tmp/openug-rra --spawn start \
	  --track STREAML4RA >/dev/null 2>&1; resident_route_legacy=$$?; \
	./nfsu2 --world2 --resident-route-audit /tmp/openug-rra --spawn start \
	  --track STREAML4RA --event 4001 >/dev/null 2>&1; resident_route_race=$$?; \
	./nfsu2 build/no-such-data-root --world2 \
	  --resident-route-audit /tmp/openug-rra --spawn start \
	  --track STREAML4RA >/dev/null 2>&1; resident_route_valid=$$?; \
	if test $$spawn_missing -eq 2 && test $$heading_missing -eq 2 && \
	   test $$spawn_bad -eq 2 && test $$heading_bad -eq 2 && \
	   test $$scenery_missing -eq 2 && test $$scenery_bad -eq 2 && \
	   test $$scenery_legacy -eq 1 && test $$scenery_live -eq 2 && \
	   test $$scenery_spawn -eq 2 && test $$scenery_stack -eq 2 && \
	   test $$world2_load_fail -eq 1 && test $$resident_legacy -eq 1 && \
	   test $$resident_all -eq 2 && test $$resident_race -eq 2 && \
	   test $$resident_valid -eq 1 && test $$resident_route_legacy -eq 1 && \
	   test $$resident_route_race -eq 2 && test $$resident_route_valid -eq 1; then \
	  echo "world_cli_test: PASS"; \
	else \
	  echo "world_cli_test: FAIL missing-spawn=$$spawn_missing missing-heading=$$heading_missing bad-spawn=$$spawn_bad bad-heading=$$heading_bad world2-load=$$world2_load_fail scenery-missing=$$scenery_missing scenery-bad=$$scenery_bad scenery-legacy=$$scenery_legacy scenery-live=$$scenery_live scenery-spawn=$$scenery_spawn scenery-stack=$$scenery_stack resident-legacy=$$resident_legacy resident-all=$$resident_all resident-race=$$resident_race resident-valid=$$resident_valid resident-route-legacy=$$resident_route_legacy resident-route-race=$$resident_route_race resident-route-valid=$$resident_route_valid"; \
	  exit 1; \
	fi

# Real GL/readback check; output is retained for visual inspection on failure.
render-resolution-test: nfsu2
	@set -e; capture_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/openug2-resolution.XXXXXX"); \
	echo "render_resolution_test: captures in $$capture_dir"; \
	for size in 1920x1080 3840x2160 1919x1079 960x600; do \
	  set --; test "$$size" = 1920x1080 || set -- --resolution "$$size"; \
	  ./nfsu2 "$(DATA)" --car MIATA --track STREAML4RA --heading 0 \
	    --shot-empty --shot-yaw 90 --shot-pitch -25 --frames 1 "$$@" \
	    --shot "$$capture_dir/$$size.png" >"$$capture_dir/$$size.log" 2>&1 \
	    || { cat "$$capture_dir/$$size.log"; exit 1; }; \
	  set -- $$(od -An -tu1 -j16 -N8 "$$capture_dir/$$size.png"); \
	  test $$# -eq 8; \
	  width=$$(( $$1*16777216 + $$2*65536 + $$3*256 + $$4 )); \
	  height=$$(( $$5*16777216 + $$6*65536 + $$7*256 + $$8 )); \
	  test "$$width" = "$${size%x*}" && test "$$height" = "$${size#*x}" \
	    || { echo "render_resolution_test: FAIL $$size got $${width}x$${height}"; exit 1; }; \
	done; echo 'render_resolution_test: PASS'

resolution-cli-test: nfsu2
	@set +e; \
	./nfsu2 --resolution >/dev/null 2>&1; \
	test $$? -eq 2 || { echo 'resolution_cli_test: FAIL missing size'; exit 1; }; \
	for size in '' 0x1080 1920x0 63x1080 1920x63 7681x1080 1920x4321 \
	  -1920x1080 1920 1920X1080 1920x1080junk 1920x1080x1 \
	  999999999999999999999x1080; do \
	  ./nfsu2 --resolution "$$size" >/dev/null 2>&1; \
	  test $$? -eq 2 || { echo "resolution_cli_test: FAIL invalid $$size"; exit 1; }; \
	done; \
	for size in 64x64 960x600 1920x1080 3840x2160 7680x4320 1919x1079; do \
	  ./nfsu2 build/no-such-data-root --resolution "$$size" --instance-audit >/dev/null 2>&1; \
	  test $$? -eq 1 || { echo "resolution_cli_test: FAIL valid $$size"; exit 1; }; \
	done; echo 'resolution_cli_test: PASS'

ai-drive-cli-test: nfsu2
	@set +e; \
	for args in '--ai-drive-audit' '--ai-drive-audit /tmp/ai' \
	  '--ai-drive-audit /tmp/ai --event 4201 --frames 0' \
	  '--ai-drive-audit /tmp/ai --event 4201 --shot /tmp/ai.png' \
	  '--ai-drive-audit /tmp/ai --event 4201 --spawn 0,0' \
	  '--ai-drive-audit /tmp/ai --event 4201 --race-audit /tmp/ra' \
	  '--ai-drive-audit /tmp/ai --event 4201 --resident-drive-audit /tmp/rda' \
	  '--ai-drive-audit /tmp/ai --event 4201 --track ALL' \
	  '--ai-drive-audit /tmp/ai --event 4201 --track STREAML4RB'; do \
	  ./nfsu2 build/no-such-data-root $$args >/dev/null 2>&1; \
	  test $$? -eq 2 || { echo "ai_drive_cli_test: FAIL $$args"; exit 1; }; \
	done; echo 'ai_drive_cli_test: PASS'

# OpenGL ES 2.0 (embedded/mobile). Cross-compile e.g.:
#   CC=aarch64-linux-gnu-gcc make gles
gles: $(SRC) $(HDRS) $(GEN)
	$(CC) $(CFLAGS) -DN2_GLES $(SDL_CFLAGS) $(SRC) -o $(GAME) $(SDL_LIBS) -lGLESv2 $(UGUI_IMAGE_LIBS) -lz -lm

run: $(GAME)
	./$(GAME) "$(DATA)"

clean:
	rm -f nfsu2 nfsu2.exe nfsu2-vk nfsu2-vk.exe *.png $(GEN) src/generated_shaders_vk.h
	rm -rf build

.PHONY: race-ai-test race-events-check run normal menu gles clean debug world-instance-test world-cli-test car-material-test world-render-test world-resident-test district-collision-test wheel-render-test light-state-test world-texture-test world-group-test world-group-audit ai-drive-test ai-drive-cli-test resolution-cli-test render-resolution-test

.PHONY: headlight-render-test
headlight-render-test: tools/headlight_render_test.c tools/legacy_draw.h src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/render.h src/nfsu2.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/headlight_render_test.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c -o build/headlight_render_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/headlight_render_test


# The test includes rhi_gl.c for controlled framebuffer-allocation failure.
rhi-pass-test: tools/rhi_pass_test.c src/rhi_gl.c src/rhi_gl_state.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/rhi_pass_test.c src/rhi_gl_state.c -o build/rhi_pass_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/rhi_pass_test
.PHONY: rhi-pass-test

.PHONY: traffic-heatmap-test

weather-render-test: tools/weather_render_test.c tools/legacy_draw.h src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/render.h src/nfsu2.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/weather_render_test.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c -o build/weather_render_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/weather_render_test
.PHONY: weather-render-test

# Standalone native ImGui map authoring; original archives stay read-only.
EDITOR_SRC := src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c src/physics.c src/ai.c src/resource.c src/world.c src/world_instance.c
EDITOR_OBJ := $(EDITOR_SRC:src/%.c=build/editor/%.o)
build/editor/%.o: src/%.c $(HDRS) $(GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@
build/map_editor.o: tools/map_editor.cpp tools/legacy_draw.h tools/map_editor_document.h tools/map_editor_gizmo.h tools/map_editor_collision.h $(HDRS)
	@mkdir -p build
	$(CXX) -O2 -std=c++14 -Wall -Wextra -Wno-unused-function $(SDL_CFLAGS) -Isrc -I$(IMGUI_DIR) -c $< -o $@
build/map_editor: $(EDITOR_OBJ) build/map_editor.o $(filter-out build/debugui.o,$(IMGUI_OBJ))
	$(CXX) -O2 $^ -o $@ $(SDL_LIBS) $(GL_LIBS) -lz -lm
map-editor: build/map_editor
map-editor-test: map-editor-collision-test tools/map_editor_test.cpp tools/map_editor_document.h tools/map_editor_gizmo.h
	@mkdir -p build
	$(CXX) -O2 -std=c++14 -Wall -Wextra tools/map_editor_test.cpp -o build/map_editor_test
	./build/map_editor_test
map-editor-collision-test: tools/map_editor_collision_test.cpp tools/map_editor_collision.h tools/map_editor_gizmo.h tools/map_editor_document.h
	@mkdir -p build
	$(CXX) -O2 -std=c++14 -Wall -Wextra tools/map_editor_collision_test.cpp -o build/map_editor_collision_test
	./build/map_editor_collision_test
.PHONY: map-editor map-editor-test map-editor-collision-test

# Approved synthetic interoperability vectors, independent of GL and retail data.
HUD_SPEC ?= docs/cleanroom-hud
hud-spec-test: hud-asset-probe
	python3 tools/hud_spec_test.py "$(HUD_SPEC)"

hud-asset-probe: build/hud_asset_probe

build/hud_asset_probe: src/tex_override.c tools/hud_asset_probe.c src/hud.c src/hud_race.c src/ugui/ugui_codec.c src/hud_asset.c src/hud_texture.c src/hud_baseline.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/hud.h src/hud_asset.h src/hud_texture.h src/hud_baseline.h src/render.h src/nfsu2.h src/generated_shaders.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/hud_asset_probe.c src/hud.c src/hud_race.c src/ugui/ugui_codec.c src/hud_asset.c src/hud_texture.c src/hud_baseline.c src/render.c src/render_params.c src/rhi_gl_state.c src/rhi_gl.c src/tex_override.c -o build/hud_asset_probe $(SDL_LIBS) $(GL_LIBS) -lz -lm

# RHI readback contract: coordinates, row layout, bounds and arithmetic overflow.
rhi-readback-test: tools/rhi_readback_test.c src/rhi_gl.c src/rhi_gl_state.c src/rhi.h src/rhi_gl.h src/rhi_gl_state.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/rhi_readback_test.c src/rhi_gl.c src/rhi_gl_state.c -o build/rhi_readback_test $(SDL_LIBS) $(GL_LIBS) -lm
	./build/rhi_readback_test
.PHONY: rhi-readback-test

# Settings backend vs docs/cleanroom-settings/test-vectors (synthetic vectors, no retail data).
# Profile/INI scratch files go to build/settings_test_tmp.
SETTINGS_SPEC ?= docs/cleanroom-settings
ifeq ($(OS),Windows_NT)
SETTINGS_LIBS := -ladvapi32
endif
settings-test: tools/settings_test.c src/settings.c src/settings.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/settings_test.c src/settings.c -o build/settings_test $(SETTINGS_LIBS) -lm
	./build/settings_test "$(SETTINGS_SPEC)/test-vectors" build/settings_test_tmp
.PHONY: settings-test

# Malformed input, allocation failures and synthetic file/link boundaries.
settings-boundary-test: tools/settings_boundary_test.c src/settings.c src/settings.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/settings_boundary_test.c -o build/settings_boundary_test $(SETTINGS_LIBS) -lm
	./build/settings_boundary_test
.PHONY: settings-boundary-test

# Controls screen logic (bindings, capture/steal rules, defaults, state machine, persistence) vs
# docs/cleanroom-settings/test-vectors/controls (synthetic vectors, no retail data, no window).
CONTROLS_SPEC ?= docs/cleanroom-settings
CONTROLS_SRC := src/controls_store.c src/options_menu.c $(GUI_ART_SRC)
CONTROLS_HDR := src/controls_bind.h src/controls_menu.h src/controls_store.h src/controls_sdl.h src/options_menu.h $(GUI_ART_HDR)
build/controls_test: tools/controls_test.c tools/test_json.h $(CONTROLS_SRC) $(CONTROLS_HDR) src/settings.c src/settings.h src/ugui/ugui_util.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc -Isrc/ugui tools/controls_test.c $(CONTROLS_SRC) src/settings.c src/ugui/ugui_util.c -o $@ $(SETTINGS_LIBS) $(SDL_LIBS) -lm
# SDL side (devices, samples, driving lookups, persistence) against an SDL virtual joystick; headless.
build/controls_sdl_test: tools/controls_sdl_test.c $(CONTROLS_SRC) $(CONTROLS_HDR) src/controls_sdl.c src/settings.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/controls_sdl_test.c src/controls_sdl.c $(CONTROLS_SRC) src/settings.c -o $@ $(SETTINGS_LIBS) $(SDL_LIBS) -lm
controls-test: build/controls_test build/controls_sdl_test
	./build/controls_test "$(CONTROLS_SPEC)/test-vectors/controls"
	SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 ./build/controls_sdl_test
.PHONY: controls-test


# Every frontend screen in a hidden 16:9 window: menu area drawn, side bars
# and caller state untouched.
frontend-draw-test: tools/frontend_draw_test.c src/frontend/frontend.c src/frontend/frontend_draw.c src/rhi_gl.c src/rhi_gl_state.c $(HDRS) $(GEN)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/frontend_draw_test.c src/frontend/frontend.c src/frontend/frontend_draw.c src/rhi_gl.c src/rhi_gl_state.c -o build/frontend_draw_test $(SDL_LIBS) $(GL_LIBS) -lm
	./build/frontend_draw_test
.PHONY: frontend-draw-test

# RHI main-target clear, upload results, texture limit, state snapshots, viewport and GPU timers.
rhi-frame-test: tools/rhi_frame_test.c src/rhi_gl.c src/rhi_gl_state.c src/rhi.h src/rhi_gl.h src/rhi_gl_state.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/rhi_frame_test.c src/rhi_gl.c src/rhi_gl_state.c -o build/rhi_frame_test $(SDL_LIBS) $(GL_LIBS) -lm
	./build/rhi_frame_test
.PHONY: rhi-frame-test

# The test includes rhi_gl.c to inject allocator failure into the real backend.
rhi-sampler-test: tools/rhi_sampler_test.c src/render.c src/render_params.c src/rhi_gl.c src/rhi_gl_state.c src/tex_override.c $(HDRS) $(GEN)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/rhi_sampler_test.c src/render.c src/render_params.c src/rhi_gl_state.c src/tex_override.c -o build/rhi_sampler_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/rhi_sampler_test

.PHONY: rhi-sampler-test

# Shader lifetime: synthetic GLSL only, no retail assets.
.PHONY: rhi-shader-test
rhi-shader-test: tools/rhi_shader_test.c src/rhi.h src/rhi_gl.h src/rhi_gl_state.h src/rhi_gl.c src/rhi_gl_state.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/rhi_shader_test.c src/rhi_gl.c src/rhi_gl_state.c -o build/rhi_shader_test $(SDL_LIBS) $(GL_LIBS)
	./build/rhi_shader_test

# Independent clean-room GUI: runtime resources are supplied by the user.
UGUI_SRC := $(filter-out src/ugui/ugui_gl.c,$(wildcard src/ugui/*.c))
UGUI_HDR := $(wildcard src/ugui/*.h) tools/ugui_demo_app.h
UGUI_APP := tools/ugui_demo_app.c tools/ugui_demo_orig.c
UGUI_FLAGS := -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Isrc/ugui $(shell pkg-config --cflags libpng libcrypto)
UGUI_IMAGE_LIBS := $(shell pkg-config --libs libpng libcrypto)
build/ugui_test: tools/ugui_test.c $(wildcard tools/ugui_test*.inc) $(UGUI_APP) $(UGUI_SRC) $(UGUI_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(UGUI_FLAGS) tools/ugui_test.c $(UGUI_APP) $(UGUI_SRC) -o $@ $(UGUI_IMAGE_LIBS) -lm
ugui-test: build/ugui_test
	env -u UGUI_DATA -u NFSU2_DATA python3 tools/ugui_vector_check.py build/ugui_test tools/ugui_vectors
build/ugui_demo: tools/ugui_demo.c $(UGUI_APP) $(UGUI_SRC) src/ugui/ugui_gl.c src/rhi_gl_state.c src/rhi_gl_state.h $(UGUI_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(UGUI_FLAGS) $(SDL_CFLAGS) tools/ugui_demo.c $(UGUI_APP) $(UGUI_SRC) src/ugui/ugui_gl.c src/rhi_gl_state.c -o $@ $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lm
ugui-demo: build/ugui_demo
.PHONY: ugui-test ugui-demo

.PHONY: ugui-boundary-test ugui-gl-state-test
ugui-boundary-test: tools/ugui_boundary_test.c src/ugui/ugui_anim.c src/ugui/ugui_util.c src/ugui/ugui_core.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/ugui tools/ugui_boundary_test.c src/ugui/ugui_anim.c src/ugui/ugui_util.c -o build/ugui_boundary_test -lm
	./build/ugui_boundary_test
ugui-gl-state-test: tools/ugui_gl_state_test.c $(UGUI_SRC) src/ugui/ugui_gl.c src/rhi_gl_state.c $(UGUI_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(UGUI_FLAGS) $(SDL_CFLAGS) -Isrc tools/ugui_gl_state_test.c $(UGUI_SRC) src/ugui/ugui_gl.c src/rhi_gl_state.c -o build/ugui_gl_state_test $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lm
	./build/ugui_gl_state_test

PAUSE_TEST_INC := $(wildcard tools/pause_menu_*_test.inc)
build/pause_menu_test: tools/pause_menu_test.c $(PAUSE_TEST_INC) tools/synth_pkgs.h tools/synth_fng.h src/pause_menu.c src/gui_view.c src/options_menu.c src/settings.c $(GUI_ART_SRC) $(HDRS) $(UGUI_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/pause_menu_test.c src/gui_view.c $(GUI_ART_SRC) src/options_menu.c src/settings.c src/controls_store.c src/controls_sdl.c $(UGUI_SRC) src/ugui/ugui_gl.c src/rhi_gl_state.c -o $@ $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) $(SETTINGS_LIBS) -lm
pause-menu-test: build/pause_menu_test
	env -u UGUI_DATA -u NFSU2_DATA python3 tools/pause_menu_vector_check.py build/pause_menu_test tools/pause_menu_vectors
	env -u UGUI_DATA -u NFSU2_DATA python3 tools/pause_menu_supplement_check.py build/pause_menu_test tools/pause_menu_vectors
	env -u UGUI_DATA -u NFSU2_DATA python3 tools/pause_menu_behavior_check.py build/pause_menu_test tools/pause_menu_vectors
.PHONY: pause-menu-test

build/pause_menu_gl_test: tools/pause_menu_gl_test.c $(HDRS) $(UGUI_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc $< $(UGUI_SRC) src/ugui/ugui_gl.c src/rhi_gl_state.c -o $@ $(SDL_LIBS) $(GL_LIBS) $(UGUI_IMAGE_LIBS) -lm
pause-menu-gl-test: build/pause_menu_gl_test
	SDL_VIDEODRIVER=$(if $(filter Darwin,$(UNAME)),cocoa,offscreen) ./build/pause_menu_gl_test
.PHONY: pause-menu-gl-test

# Synthetic gameplay integration checks; no installation or GL required.
drive-stage1-test: tools/drive_stage1_test.c src/lateral.c src/drive_stage1.c src/contact.c src/body_response.c src/drive_contact.c src/lateral.h src/drive_stage1.h src/contact.h src/body_response.h src/drivetrain.c src/braking.c src/resistance.c src/physics.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/drive_stage1_test.c src/lateral.c src/drive_stage1.c src/contact.c src/body_response.c src/drive_contact.c src/drivetrain.c src/braking.c src/resistance.c src/physics.c -o build/drive_stage1_test -lm
	./build/drive_stage1_test

.PHONY: drive-stage1-test
# Pure building blocks of the authored-art screens (pause entries, carousel, dialogs, row layout,
# transition timing) vs test-vectors-gui tvg07..tvg11. GL-free and SDL-free.
OPTIONS_SPEC ?= docs/cleanroom-settings
build/gui_art_test: tools/gui_art_test.c tools/test_json.h $(GUI_ART_SRC) $(GUI_ART_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/gui_art_test.c $(GUI_ART_SRC) -o $@ -lm
gui-art-test: build/gui_art_test
	./build/gui_art_test "$(OPTIONS_SPEC)/test-vectors-gui"
.PHONY: gui-art-test

# Graphics option-screen state machine vs test-vectors-gui tvg01..tvg06 (synthetic vectors, no retail
# data). GL-free and SDL-free.
build/options_menu_test: tools/options_menu_test.c tools/test_json.h src/options_menu.c src/options_menu.h src/settings.c src/settings.h $(GUI_ART_SRC) $(GUI_ART_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc tools/options_menu_test.c src/options_menu.c src/settings.c $(GUI_ART_SRC) -o $@ $(SETTINGS_LIBS) -lm
options-menu-test: build/options_menu_test build/pause_menu_test gui-art-test
	env -u UGUI_DATA -u NFSU2_DATA ./build/options_menu_test "$(OPTIONS_SPEC)/test-vectors-gui"
	env -u UGUI_DATA -u NFSU2_DATA ./build/pause_menu_test options
.PHONY: options-menu-test

# Authored presentation (gui_view.c, ug_scene_app_write) against synthetic in-memory packages. GL-free.
build/gui_view_test: tools/gui_view_test.c tools/synth_pkgs.h tools/synth_fng.h src/gui_view.c src/options_menu.c src/settings.c $(GUI_ART_SRC) $(GUI_ART_HDR) $(UGUI_SRC) $(UGUI_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(UGUI_FLAGS) $(SDL_CFLAGS) -Isrc tools/gui_view_test.c src/gui_view.c src/options_menu.c src/settings.c $(GUI_ART_SRC) $(UGUI_SRC) -o $@ $(UGUI_IMAGE_LIBS) $(SETTINGS_LIBS) -lm
gui-view-test: build/gui_view_test
	env -u UGUI_DATA -u NFSU2_DATA ./build/gui_view_test
.PHONY: gui-view-test

# Coordinate frames, group composition and the anchor read rule vs tvg12 (SPEC_GUI_ART_AMEND1). GL-free.
build/gui_frames_test: tools/gui_frames_test.c tools/synth_pkgs.h tools/synth_fng.h tools/test_json.h src/gui_view.c src/options_menu.c src/settings.c $(GUI_ART_SRC) $(GUI_ART_HDR) $(UGUI_SRC) $(UGUI_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(UGUI_FLAGS) $(SDL_CFLAGS) -Isrc tools/gui_frames_test.c src/gui_view.c src/options_menu.c src/settings.c $(GUI_ART_SRC) $(UGUI_SRC) -o $@ $(UGUI_IMAGE_LIBS) $(SETTINGS_LIBS) -lm
gui-frames-test: build/gui_frames_test
	env -u UGUI_DATA -u NFSU2_DATA ./build/gui_frames_test "$(OPTIONS_SPEC)/test-vectors-gui"
.PHONY: gui-frames-test

# Everything of the authored-art pause menu and option screens (synthetic data only).
gui-test: gui-art-test gui-view-test gui-frames-test options-menu-test pause-menu-test
.PHONY: gui-test

# Approved Stage-2 synthetic vectors and full-vector boundary checks (no assets/GL).
STAGE2_VECTORS ?= docs/cleanroom-driving-physics/stage2/test-vectors
resistance-test: tools/resistance_test.c tools/test_json.h src/resistance.c src/resistance.h src/drivetrain.c src/drivetrain.h src/braking.c src/braking.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/resistance_test.c src/resistance.c src/drivetrain.c src/braking.c -o build/resistance_test -lm
	./build/resistance_test "$(STAGE2_VECTORS)"
.PHONY: resistance-test

# Approved Stage3 synthetic components; no original data, SDL or GL.
STAGE3_VECTORS ?= docs/cleanroom-driving-physics/stage3/test-vectors
braking-test: tools/braking_test.c tools/test_json.h src/braking.c src/braking.h src/drivetrain.c src/drivetrain.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/braking_test.c src/braking.c src/drivetrain.c -o build/braking_test -lm
	./build/braking_test "$(STAGE3_VECTORS)"
.PHONY: braking-test

# Approved Stage4 synthetic vectors; no assets or graphics.
lateral-test: tools/lateral_test.c tools/test_json.h src/lateral.c src/lateral.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/lateral_test.c src/lateral.c -o build/lateral_test -lm
	./build/lateral_test docs/cleanroom-driving-physics/stage4/test-vectors
.PHONY: lateral-test
drive-stage4-test: tools/drive_stage4_test.c tools/drive_stage1_test.c src/drive_stage1.c src/contact.c src/body_response.c src/drive_contact.c src/lateral.c src/drivetrain.c src/braking.c src/resistance.c src/physics.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/drive_stage4_test.c src/drive_stage1.c src/contact.c src/body_response.c src/drive_contact.c src/lateral.c src/drivetrain.c src/braking.c src/resistance.c src/physics.c -o build/drive_stage4_test -lm
	./build/drive_stage4_test
.PHONY: drive-stage4-test

contact-test: tools/contact_test.c tools/test_json.h src/contact.c src/contact.h src/resistance.c src/braking.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/contact_test.c src/contact.c src/resistance.c src/braking.c -o build/contact_test -lm
	./build/contact_test
.PHONY: contact-test

drive-stage5-test: tools/drive_stage5_test.c tools/drive_stage4_test.c tools/drive_stage1_test.c src/body_response.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/drive_stage5_test.c src/body_response.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c -o build/drive_stage5_test -lm
	./build/drive_stage5_test
.PHONY: drive-stage5-test

drive-trace-test: tools/drive_trace_test.c src/drive_trace.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/drive_trace_test.c -o build/drive_trace_test -lm
	./build/drive_trace_test
.PHONY: drive-trace-test

BODY_VECTORS ?= docs/cleanroom-driving-physics/body-response/test-vectors
body-response-test: tools/body_response_test.c tools/test_json.h src/body_response.c src/contact.c src/resistance.c src/braking.c src/body_response.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/body_response_test.c src/body_response.c src/contact.c src/resistance.c src/braking.c -o build/body_response_test -lm
	./build/body_response_test "$(BODY_VECTORS)"
body-feedback-test: tools/body_feedback_test.c tools/drive_stage5_test.c tools/drive_stage1_test.c src/body_response.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/body_feedback_test.c src/body_response.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c -o build/body_feedback_test -lm
	./build/body_feedback_test
.PHONY: body-response-test body-feedback-test

# Supplied-3D-body acceleration contract and full host replay (approved vectors).
body-coupling-test: tools/body_coupling_test.c tools/test_json.h src/drivetrain.c src/braking.c src/body_response.c src/resistance.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/body_coupling_test.c src/drivetrain.c src/braking.c src/body_response.c src/resistance.c -o build/body_coupling_test -lm
	./build/body_coupling_test docs/cleanroom-driving-physics/body-coupling/test-vectors
body-coupling-host-test: tools/body_coupling_host_test.c tools/body_feedback_test.c tools/drive_stage5_test.c tools/drive_stage1_test.c src/body_response.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/body_coupling_host_test.c src/body_response.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c -o build/body_coupling_host_test -lm
	./build/body_coupling_host_test
.PHONY: body-coupling-test body-coupling-host-test

# Output-only dynamic pose/hub/spin interpolation at the shared render clock.
drive-presentation-test: tools/drive_presentation_test.c src/drive_presentation.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/drive_presentation_test.c src/body_response.c src/contact.c src/resistance.c src/braking.c -o build/drive_presentation_test -lm
	./build/drive_presentation_test
.PHONY: drive-presentation-test

DRIVE_TEST_SRC := src/drive_stage1.c src/drive_contact.c src/body_response.c src/contact.c src/drivetrain.c src/braking.c src/resistance.c src/lateral.c src/physics.c
tire-marks-drive-test: tools/tire_marks_drive_test.c tools/drive_stage5_test.c src/tire_marks.c src/tire_marks_drive.h $(DRIVE_TEST_SRC) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/tire_marks_drive_test.c src/tire_marks.c $(DRIVE_TEST_SRC) -o build/tire_marks_drive_test -lm
	./build/tire_marks_drive_test
.PHONY: tire-marks-drive-test

render-sampling-test: tools/render_sampling_test.c tools/test_json.h tools/drive_stage5_test.c src/drive_presentation.h $(DRIVE_TEST_SRC) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/render_sampling_test.c $(DRIVE_TEST_SRC) -o build/render_sampling_test -lm
	./build/render_sampling_test
.PHONY: render-sampling-test

ordinary-mode-test: tools/ordinary_mode_test.c tools/test_json.h tools/drive_stage5_test.c $(DRIVE_TEST_SRC) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/ordinary_mode_test.c $(DRIVE_TEST_SRC) -o build/ordinary_mode_test -lm
	./build/ordinary_mode_test
.PHONY: ordinary-mode-test

# Asset-free GLOBALB skill/catch-up configuration and adaptation inputs.
race-ai-source-test: tools/race_ai_source_test.c src/race_ai_source.h src/asset_chunks.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/race_ai_source_test.c -o build/race_ai_source_test -lm
	./build/race_ai_source_test
.PHONY: race-ai-source-test

# ----------------------------------------------------------------------------
# Vulkan backend (issue #14, P11a). Offscreen v0 only. By default it builds against the system SDL2 and
# Vulkan (sdl2-config, -lvulkan, glslangValidator on PATH). To build against a self-contained tree instead
# (headers in $(VK_SYSROOT)/usr/include, libraries in $(VK_SYSROOT)/usr/lib/x86_64-linux-gnu, tools in
# $(VK_SYSROOT)/usr/bin), set VK_SYSROOT=/path. VK_ICD is the Vulkan driver the smoke test runs on
# (default: lavapipe, if its ICD file is found; the Khronos validation layer is used when installed).
VK_SYSROOT ?=
ifneq ($(VK_SYSROOT),)
VK_LIBDIR  := $(VK_SYSROOT)/usr/lib/x86_64-linux-gnu
VK_INC     := -I$(VK_SYSROOT)/usr/include/SDL2 -I$(VK_SYSROOT)/usr/include -I$(VK_SYSROOT)/usr/include/x86_64-linux-gnu
VK_LIBS    := -L$(VK_LIBDIR) -lSDL2 -lvulkan -Wl,-rpath,$(VK_LIBDIR)
else
VK_INC     := $(SDL_CFLAGS)
VK_LIBS    := $(SDL_LIBS) $(if $(WINDOWS),-lvulkan-1,-lvulkan)
endif
VK_CFLAGS  := -std=c99 -Wall -Wextra -Wno-missing-field-initializers -D_DEFAULT_SOURCE -DN2_VULKAN $(if $(WINDOWS),-D_USE_MATH_DEFINES) $(VK_INC) -Isrc
VK_ICD     ?= $(firstword $(wildcard /usr/share/vulkan/icd.d/lvp_icd*.json /etc/vulkan/icd.d/lvp_icd*.json))
VK_RUN_ENV := $(if $(WINDOWS),,SDL_VIDEODRIVER=offscreen) $(if $(VK_ICD),VK_DRIVER_FILES=$(VK_ICD)) \
              $(if $(VK_SYSROOT),VK_ADD_LAYER_PATH=$(VK_SYSROOT)/usr/share/vulkan/explicit_layer.d LD_LIBRARY_PATH=$(VK_LIBDIR))
# Vulkan shaders: Vulkan-GLSL in src/shaders/vk/, compiled to SPIR-V and embedded
# as uint32 arrays (vk_<name>_<ext>) by glslangValidator -- same build-time embed
# idea as generated_shaders.h. Regenerated when any vk shader changes.
VK_GLSLANG ?= $(if $(VK_SYSROOT),$(VK_SYSROOT)/usr/bin/glslangValidator,glslangValidator)
VK_SHADERS := $(sort $(wildcard src/shaders/vk/*.vert src/shaders/vk/*.frag))
src/generated_shaders_vk.h: $(VK_SHADERS)
	@echo "/* AUTO-GENERATED from src/shaders/vk/ by glslangValidator -- do not edit */" > $@
	@for f in $(VK_SHADERS); do \
	  sym=vk_$$(basename $$f | tr '.' '_'); \
	  $(VK_GLSLANG) -V --target-env vulkan1.1 --vn $$sym -o $@.tmp "$$f" >/dev/null || { rm -f $@ $@.tmp; exit 1; }; \
	  cat $@.tmp >> $@; \
	done
	@rm -f $@.tmp

build/rhi_vk_smoke$(EXEEXT): tools/rhi_vk_smoke.c src/rhi_vk.c src/rhi.h src/generated_shaders_vk.h
	@mkdir -p build
	$(CC) $(VK_CFLAGS) tools/rhi_vk_smoke.c src/rhi_vk.c -o $@ $(VK_LIBS)

rhi-vk-smoke: build/rhi_vk_smoke$(EXEEXT)
	$(VK_RUN_ENV) ./build/rhi_vk_smoke$(EXEEXT)
.PHONY: rhi-vk-smoke

# Present path (P12): requires a desktop Vulkan driver; setup failures fail the test.
# Override VK_PRESENT_ENV for another display, e.g. DISPLAY=:N SDL_VIDEODRIVER=x11.
VK_PRESENT_ENV ?=
build/rhi_vk_present_test$(EXEEXT): tools/rhi_vk_present_test.c src/rhi_vk.c src/rhi.h src/generated_shaders_vk.h
	@mkdir -p build
	$(CC) $(VK_CFLAGS) tools/rhi_vk_present_test.c src/rhi_vk.c -o $@ $(VK_LIBS)

rhi-vk-present-test: build/rhi_vk_present_test$(EXEEXT)
	$(VK_PRESENT_ENV) ./build/rhi_vk_present_test$(EXEEXT)
.PHONY: rhi-vk-present-test

# Full game linked against the Vulkan backend (issue #14, P11 screenshot
# comparison). Same game sources as `nfsu2`, but the GL backend + GL-only UI
# renderers are dropped and src/rhi_vk.c + src/vk_ui_stubs.c take their place.
# Offscreen `--shot` only (no swapchain, no UI renderers). Built with the Vulkan
# flags above (VK_CFLAGS/VK_LIBS), NOT -lGL; run it with $(VK_RUN_ENV) (offscreen SDL, lavapipe).
VK_GAME_SRC  := $(filter-out src/rhi_gl.c src/rhi_gl_state.c src/ugui/ugui_gl.c,$(SRC)) \
                src/rhi_vk.c src/vk_ui_stubs.c
# VK_CFLAGS already carries -DN2_VULKAN, the SDL2 includes and -Isrc; add the game's -O2 and the
# nfsu2.h unused-static silence. The game also links libpng, libcrypto and zlib (system, or $(VK_SYSROOT)).
VK_GAME_CFLAGS := $(VK_CFLAGS) $(if $(VK_SYSROOT),,$(UGUI_CFLAGS)) -O2 -Wno-unused-function
VK_GAME_LIBS   := $(VK_LIBS) $(if $(VK_SYSROOT),-lpng16 -lcrypto,$(UGUI_IMAGE_LIBS)) -lz -lm

nfsu2-vk: $(VK_GAME_SRC) $(HDRS) $(GEN) src/generated_shaders_vk.h
	$(CC) $(VK_GAME_CFLAGS) $(VK_GAME_SRC) -o nfsu2-vk$(EXEEXT) $(VK_GAME_LIBS)
.PHONY: nfsu2-vk

# Native file mapping and Windows sorted-directory compatibility, no assets/GPU.
resource-map-test: tools/resource_map_test.c src/resource.c src/resource.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/resource_map_test.c src/resource.c -o build/resource_map_test$(EXEEXT) -lm
	./build/resource_map_test$(EXEEXT)

.PHONY: resource-map-test

resource-circuits-test: tools/resource_circuits_test.c src/resource.c src/resource.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/resource_circuits_test.c src/resource.c -o build/resource_circuits_test$(EXEEXT) -lm
	./build/resource_circuits_test$(EXEEXT)
.PHONY: resource-circuits-test

dir-scan-test: tools/dir_scan_test.c src/dir_scan.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/dir_scan_test.c -o build/dir_scan_test$(EXEEXT)
	./build/dir_scan_test$(EXEEXT)
.PHONY: dir-scan-test

# Data-root option parsing, no assets or graphics needed. Keep DebugUI enabled.
data-path-cli-test: debug
	sh tools/data_path_cli_test.sh ./$(GAME)
.PHONY: data-path-cli-test
