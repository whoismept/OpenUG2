# M136 Authored Sky and World Lights Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the shipped night/sunrise/sunset sky on the real two-range SKYDOME and render shipped district light-source records as texture-backed additive halos, without importing PR #4 camera, post-processing, physics, or cube-map behavior.

**Architecture:** Keep file interpretation GL-free in `nfsu2.h`, collect light records while region bytes are owned by `World`, and make the existing renderer consume only validated results. Sky and lamp textures remain shipped assets; the renderer adds one narrowly gated textured-emissive shader path and restores every changed GL state after each dedicated pass.

**Tech Stack:** C99, SDL2, OpenGL 2.1 / OpenGL ES 2.0, existing TPK/JDLZ decoders, standalone C regression harnesses.

**Spec:** `docs/superpowers/specs/2026-08-28-pr4-fidelity-import-design.md`

## Global Constraints

- Clean-room: do not commit retail assets, executable bytes, or disassembly output.
- Production defaults to the authored night sky; `--sky sunrise|sunset|night` is deterministic and invalid values exit 2.
- No post-processing, environment cube, camera replacement, vehicle dynamics, forced paint, or `--track ALL` production behavior.
- No name-based hiding: classification may recognize only exact `SKYDOME` or the `SKY_` prefix.
- All parsers are bounded, use `memcpy` for floats, and retain conservative fallbacks.
- Work only on `feature/instance-world-render-fidelity`; do not push or merge during the milestone.

---

### Task 1: Authored two-range sky

**Files:**
- Modify: `src/nfsu2.h`
- Modify: `src/render.h`
- Modify: `src/render.c`
- Modify: `src/main.c`
- Modify: `tools/car_material_test.c`
- Modify: `Makefile`
- Modify: `docs/FORMATS.md`
- Modify: `docs/DEVELOPER_GUIDE.md`

**Interfaces:**
- Consumes: existing `0x134012` positional slots, `0x134B02` submesh ranges, LOC4 offset-slot decoder, `N2Batch.texkey`.
- Produces: `n2_sky_profile_parse`, `n2_sky_remap_key`, exact SKYDOME classification, two sky material slices, `RProg.uEmissiveTex`.

- [x] **Step 1: Write failing parser/profile tests**

Add assertions driven through the real `n2_mesh_category`/`n2_walk_meshes` paths:

```c
chk("exact SKYDOME is sky", category_for_name("SKYDOME") == N2_SKY);
chk("SKY_ family is sky", category_for_name("SKY_NIGHT_TEST") == N2_SKY);
chk("building containing SKYDOME is ordinary",
    category_for_name("XB_SKYDOMEB_1A_LL_00") == N2_OTHER);
chk("factory skylight is ordinary",
    category_for_name("XB_FACTORYSKYLIGHTA_1A_00") == N2_OTHER);
chk("SKYDOME emits dome and cap ranges", sky_scene.count == 2);
chk("SKYDOME preserves all 432 indices", total_nidx(&sky_scene) == 432);
chk("night remap selects shipped dome key",
    n2_sky_remap_key(N2_SKY_NIGHT, 0x2414a01eu) == 0x8a9a05cfu);
chk("night remap selects shipped cap key",
    n2_sky_remap_key(N2_SKY_NIGHT, 0x5fb8bcd1u) == 0xb0eb9302u);
```

- [x] **Step 2: Verify RED**

Run `make car-material-test`. Expected: false-positive classification and one-mesh SKYDOME assertions fail; profile symbols are initially undefined.

- [x] **Step 3: Implement minimal GL-free sky semantics**

In `nfsu2.h`, accept only the exact uppercase token `SKYDOME` or tokens beginning `SKY_`. Permit structurally valid SKY objects through the existing submesh partition gate while GLOW keeps its dedicated whole-object handling. Add:

```c
enum { N2_SKY_SUNRISE = 0, N2_SKY_SUNSET, N2_SKY_NIGHT };
static int n2_sky_profile_parse(const char *name);
static uint32_t n2_sky_remap_key(int profile, uint32_t source_key);
```

Only the structurally proven source keys `0x2414a01e` (dome) and `0x5fb8bcd1` (cap) are remapped. Unknown keys are returned unchanged.

- [x] **Step 4: Bind selected LOC4 textures through the normal world map**

Parse `--sky`; default to `N2_SKY_NIGHT`. After scene assembly and before `world_bind_textures`, remap only `N2_SKY` mesh keys. The normal own-TPK → LOC4 → master resolver must upload them; do not create a second texture decoder.

- [x] **Step 5: Add the dedicated textured-emissive shader gate and world-space sky pass**

Add `uEmissiveTex`. It samples texture RGB/alpha, multiplies RGB by `uColor`, discards effectively clear texels, and otherwise preserves the normal fog equation. For sky only, temporarily set fog density to zero, use the real camera view matrix with a 30 km far plane, draw the dome key before the cap key, alpha-blend the cap, then restore fog density, blending, depth mask, `uEmissiveTex`, and the ordinary MVP.

- [x] **Step 6: Verify GREEN and capture**

Run `make car-material-test`, release/debug builds, instance/CLI tests, and `git diff --check`. Capture L4RA and L4RB at fixed production poses for night plus `--sky sunrise` and `--sky sunset`. Confirm buildings named `XB_SKYDOME*`/`XB_FACTORYSKYLIGHT*` remain in the ordinary pass and the sky has texture rather than flat fog color.

- [x] **Step 7: Commit Task 1**

Commit only Task 1 files with message `render: draw authored two-range sky`.

---

### Task 2: Authored district light sources

**Files:**
- Modify: `src/nfsu2.h`
- Modify: `src/world.h`
- Modify: `src/world.c`
- Modify: `src/main.c`
- Modify: `src/render.c` / `src/render.h` (isolated light pass and state restoration)
- Create: `tools/world_render_test.c`
- Create: `tools/light_state_test.c` (asset-free real GL regression)
- Modify: `Makefile`
- Modify: `docs/FORMATS.md`
- Modify: `docs/DEVELOPER_GUIDE.md`

**Interfaces:**
- Consumes: bounded chunk walk, region buffers before `world_bind_textures`, `SFX_FLARE_GLOWA` key `0x17e5ebd2`, `RProg.uEmissiveTex`, shared quad.
- Produces: `N2LightSrc`, `n2_load_light_sources`, `World.lights`/`World.nlights`, additive camera-facing light pass.

- [x] **Step 1: Write failing 0x135003 tests**

Create synthetic nested chunks containing exact 96-byte records and assert:

```c
assert(n2_load_light_sources(valid, valid_len, out, 8) == 1);
assert(close3(out[0].pos, 10.0f, -20.0f, 3.0f));
assert(out[0].rgba == 0xff969696u);
assert(near(out[0].r_out, 30.0f) && near(out[0].r_in, 10.0f));
assert(n2_load_light_sources(disabled, disabled_len, out, 8) == 0);
assert(n2_load_light_sources(bad_stride, bad_stride_len, out, 8) == 0);
assert(n2_load_light_sources(mapwide_9999m, mapwide_len, out, 8) == 0);
assert(n2_load_light_sources(truncated, truncated_len, out, 8) == 0);
```

Add an exact duplicate fixture and require one output.

- [x] **Step 2: Verify RED**

Run `make world-render-test`. Expected: compile failure because `N2LightSrc` and `n2_load_light_sources` do not exist.

- [x] **Step 3: Implement the bounded parser**

Walk only valid nested chunks. For `0x00135003`, skip the `0x11` prefix and reject the entire leaf unless the remaining body is a multiple of 96. Accept records only when byte `+0x07 == 1`; position `f32[3] @+0x10`; `r_out @+0x1c`; `r_in @+0x30`; packed RGBA `u32 @+0x0c`. Require finite coordinates inside ±60 km, `0 < r_out <= 5000`, and `0 <= r_in <= r_out`. Deduplicate exact accepted records. Read floats with `memcpy`.

- [x] **Step 4: Collect sources while World owns region bytes**

Append/deduplicate every loaded region's sources before texture upload frees the buffers. Store them in dynamic `World.lights`; free them on shutdown. Expected retail census: L4RA 2,228 exact-unique local sources after rejecting the map-wide 9999 m source and malformed leaf; L4RB 193.

- [x] **Step 5: Render shipped additive halos**

Resolve `SFX_FLARE_GLOWA` (`0x17e5ebd2`) from the normal regional texture map. At frame end, camera-distance cull lights using the ordinary view distance; construct a camera-facing quad from the shared quad mesh; size from `r_in` with a 1 m minimum; set authored RGB from packed color; use `uEmissiveTex`, depth test on, depth write off, `SRC_ALPHA, ONE`; restore MVP, alpha, blend, depth mask, texture and shader state.

- [x] **Step 6: Verify and capture**

Run `make world-render-test`, all existing parser/world tests, release/debug builds, `git diff --check`, and fixed night captures at the L4RA airport/city and L4RB airport. Confirm source counts, distance culling, no daytime hard-coded light, no solid billboard rectangles, and unchanged supported spawn/geometry counts.

Review follow-up: `make light-state-test` reproduced the leaked shader color
before the fix. It now verifies exact pass-state restoration, depth occlusion
even when depth testing was disabled at entry, distance culling, and black
sprite edges adding zero under fog. The additive-fog assertion also failed
before its isolated fix. These checks use synthetic GL input, not game assets.

- [x] **Step 7: Commit Task 2**

Commit only Task 2 files with message `render: draw authored district light sources`.
