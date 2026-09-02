# M144 Moving World Neighborhood Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stream one deterministic instance-driven neighborhood at a time and replace rendering, ground queries, lights, and solid collision together while preserving the moving player.

**Architecture:** Split persistent city/navigation state from replaceable neighborhood state, give the ground grid and GPU objects explicit owners, and build a complete candidate `WorldResident` while the current resident remains active. A pure reload policy chooses stable centers; activation happens once at a frame boundary and failure leaves the former resident untouched.

**Tech Stack:** C99, SDL2, OpenGL 2.1/OpenGL ES 2.0, existing NFSU2 chunk parsers, assert-based GL-free C tests, hidden-context GL integration tests.

**Spec:** `docs/superpowers/specs/2026-09-01-m144-moving-world-neighborhood-design.md`

## Global Constraints

- Moving residency is enabled only for instance-driven `--world2` free-roam.
- Never use `--track ALL`; never merge overlapping STREAM event supersets.
- Placement matrices, material routing, lighting, vehicle physics, and camera tuning are unchanged.
- Candidate construction cannot mutate active scene/grid/GPU/collision pointers.
- One activation replaces scene, bounds, grid, textures/batches, lights, and obstacles together.
- Player XY/Z, velocity, heading, vertical-contact state, and camera state are preserved exactly.
- No asynchronous loading in M144; synchronous stalls are accepted and measured.
- Every production edit follows RED -> GREEN. Keep existing dirty M140-M143 files intact and stage/commit nothing unless the user separately authorizes Git changes.

---

### Task 1: Pure reload policy and measured safety invariant

**Files:**
- Create: `src/world_resident.h`
- Create: `src/world_resident.c`
- Create: `tools/world_resident_test.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: player XY and the active resident center.
- Produces:
  ```c
  typedef struct {
      float resident_radius;
      float draw_radius;
      float safety_margin;
      float cell_size;
  } WResidentPolicy;

  int world_resident_policy_valid(const WResidentPolicy *policy);
  int world_resident_target(const WResidentPolicy *policy,
                            float player_x, float player_y,
                            float active_x, float active_y,
                            float out_center[2]);
  ```

- [x] **Step 1: Write the policy tests before adding production definitions**

  Add the following cases to `tools/world_resident_test.c`:

  ```c
  #include <assert.h>
  #include <math.h>
  #include <stdio.h>
  #include "world_resident.h"

  static int nearf(float a, float b) { return fabsf(a-b) < 0.001f; }

  static void test_policy(void) {
      WResidentPolicy p = {1400.0f, 933.0f, 67.0f, 400.0f};
      float c[2] = {-1.0f, -1.0f};
      assert(world_resident_policy_valid(&p));
      assert(!world_resident_target(&p, 399.9f, 0.0f, 0.0f, 0.0f, c));
      assert(world_resident_target(&p, 400.1f, 0.0f, 0.0f, 0.0f, c));
      assert(nearf(c[0], 400.0f) && nearf(c[1], 0.0f));
      assert(!world_resident_target(&p, 400.1f, 0.0f, c[0], c[1], c));
      p.resident_radius = 1200.0f;
      assert(!world_resident_policy_valid(&p));
      p.resident_radius = 1400.0f; p.cell_size = 0.0f;
      assert(!world_resident_policy_valid(&p));
      p.cell_size = 400.0f;
      assert(!world_resident_target(&p, NAN, 0.0f, 0.0f, 0.0f, c));
  }
  ```

- [x] **Step 2: Run RED**

  Run: `make world-resident-test`

  Expected: compilation fails because `world_resident.h` and its functions do not exist.

- [x] **Step 3: Implement only validation, trigger distance, and deterministic snapping**

  In `src/world_resident.c`, use:

  ```c
  int world_resident_policy_valid(const WResidentPolicy *p) {
      if (!p || !isfinite(p->resident_radius) || !isfinite(p->draw_radius) ||
          !isfinite(p->safety_margin) || !isfinite(p->cell_size)) return 0;
      return p->draw_radius > 0.0f && p->safety_margin >= 0.0f &&
             p->cell_size > 0.0f &&
             p->resident_radius >= p->draw_radius + p->cell_size + p->safety_margin;
  }
  ```

  `world_resident_target` returns zero for invalid/non-finite input, uses
  `cell_size` as the recenter distance, and snaps each output axis with
  `floorf(value / cell_size + 0.5f) * cell_size`. If the snapped target equals
  the active center, return zero to prevent repeated rebuilds.

- [x] **Step 4: Add and run the test target**

  Add `src/world_resident.c` to `SRC` and `src/world_resident.h` to `HDRS`. Add:

  ```make
  world-resident-test: tools/world_resident_test.c src/world_resident.c src/world_resident.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/world_resident_test.c src/world_resident.c -o build/world_resident_test -lm
	./build/world_resident_test
  ```

  Run: `make world-resident-test`

  Expected: `world_resident_test: PASS`.

- [x] **Step 5: Record a scoped diff checkpoint**

  Verify `git diff --check` and record Task 1 as complete in the plan. Do not stage or commit the pre-existing dirty worktree.

---

### Task 2: Owned ground grid with explicit activation

**Files:**
- Modify: `src/world.h`
- Modify: `src/world.c`
- Modify: `tools/world_resident_test.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: an `N2Scene` and its parallel mesh bounds.
- Produces:
  ```c
  typedef struct {
      const N2Mesh *meshes;
      float x0, y0;
      int gw, gh;
      int *start, *list;
  } WGroundGrid;

  int  world_ground_grid_build(WGroundGrid *grid, const N2Scene *scene,
                               const float (*mbb)[4]);
  void world_ground_grid_activate(const WGroundGrid *grid);
  void world_ground_grid_free(WGroundGrid *grid);
  ```

- [x] **Step 1: Write RED ownership and activation tests**

  Extend `tools/world_resident_test.c` with two one-triangle ROAD scenes at
  different heights. Build both grids, activate A, query A, build B without
  activation and prove A is unchanged, then activate B and prove only B uses
  the fast grid:

  ```c
  assert(world_ground_grid_build(&ga, &sa, bba));
  world_ground_grid_activate(&ga);
  assert(world_ground_at(&sa, 1, 1, -9, &z) == WSURF_ROAD && nearf(z, 2));
  assert(world_ground_grid_build(&gb, &sb, bbb));
  assert(world_ground_at(&sa, 1, 1, -9, &z) == WSURF_ROAD && nearf(z, 2));
  world_ground_grid_activate(&gb);
  assert(world_ground_at(&sb, 1, 1, -9, &z) == WSURF_ROAD && nearf(z, 7));
  world_ground_grid_free(&ga);
  world_ground_grid_free(&gb);
  assert(!ga.start && !ga.list && !gb.start && !gb.list);
  ```

- [x] **Step 2: Run RED**

  Run: `make world-resident-test`

  Expected: missing `WGroundGrid` and lifecycle symbols.

- [x] **Step 3: Move the current CSR allocation into the owned builder**

  Replace `g_grid`'s embedded arrays with:

  ```c
  static const WGroundGrid *g_active_grid;
  ```

  `world_ground_grid_build` must zero the output first, allocate its own
  `start/list`, and on every allocation/geometry failure call
  `world_ground_grid_free` before returning zero. It must not set
  `g_active_grid`.

  `world_ground_grid_activate(NULL)` clears the active fast path. Ground and
  rail queries use `g_active_grid` only when `scene->meshes` matches
  `g_active_grid->meshes`; otherwise retain their existing brute-force fallback.

- [x] **Step 4: Replace initial one-shot activation without behavior change**

  Add `WGroundGrid grid;` to the current `World` geometry owner. In
  `world_load_ex`, replace `grid_build(w)` with:

  ```c
  if (!world_ground_grid_build(&w->grid, &w->scene,
                               (const float (*)[4])w->mbb)) return 0;
  world_ground_grid_activate(&w->grid);
  ```

- [x] **Step 5: Run GREEN and existing ground consumers**

  Extend `world-resident-test` to link the real ground implementation and its
  required translation units:

  ```make
  world-resident-test: tools/world_resident_test.c src/world_resident.c src/world.c src/resource.c src/world_instance.c src/render.c src/physics.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DWORLD_RESIDENT_TESTING -Isrc tools/world_resident_test.c src/world_resident.c src/world.c src/resource.c src/world_instance.c src/render.c src/physics.c -o build/world_resident_test $(SDL_LIBS) $(GL_LIBS) -lz -lm
	./build/world_resident_test
  ```

  Run:

  ```text
  make world-resident-test
  make world-instance-test
  make world-cli-test
  ```

  Expected: all PASS; fixed-center instance audit counts remain unchanged.

- [x] **Step 6: Record a scoped diff checkpoint**

  Run `git diff --check`; do not stage or commit.

---

### Task 3: Separate persistent city state from replaceable neighborhood CPU state

**Files:**
- Modify: `src/world.h`
- Modify: `src/world.c`
- Modify: `src/main.c`
- Modify: `tools/world_texture_test.c`
- Modify: `tools/world_instance_test.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct {
      N2Scene scene, vista;
      WRegion rgn[WORLD_MAXREG];
      int nreg;
      unsigned char *loc4, *master, *common;
      long loc4len, masterlen, commonlen;
      N2Tpk mastertpk, commontpk;
      N2Tex grass;
      int have_grass;
      N2LightSrc *lights;
      int nlights, lightcap;
      float (*mbb)[4];
      WGroundGrid grid;
      WInstStats inst_stats;
      float center[2], radius;
  } WorldNeighborhood;

  typedef struct {
      WDistrict dist[WORLD_MAXDIST];
      int ndist;
      int *navcomp;
      float *nav;
      int nnav;
      int *navedge;
      int nnavedge;
      float navbb[4];
      int *adjstart, *adjlist;
      int nadj;
      WEvent ev[WORLD_MAXEVENT];
      int nev;
      int *navev;
      char *navopen;
      WBarrier bar[WORLD_MAXBARRIER];
      int nbar;
      int mode, active_ev, nmasked;
      WRace race;
  } WorldCity;

  typedef struct { WorldCity city; WorldNeighborhood neighborhood; } World;

  int  world_city_load(WorldCity *city,
                       const WorldNeighborhood *initial_neighborhood,
                       const char *track_root, const char *trackname);
  int  world_neighborhood_load(WorldNeighborhood *neighborhood,
                               const char *track_root, const char *trackname,
                               const WLoadOptions *options);
  void world_city_free(WorldCity *city);
  void world_neighborhood_free(WorldNeighborhood *neighborhood);
  ```

- [x] **Step 1: Add RED destructor tests using test-owned allocations**

  Allocate every pointer-bearing member of `WorldNeighborhood`, including one
  mesh, vertex/index/color arrays, TPK block arrays, lights, bounds, and grid
  arrays. Call `world_neighborhood_free` twice and assert all pointer/count
  fields are zero after each call. Repeat for every pointer-bearing `WorldCity`
  navigation field with `world_city_free`.

- [x] **Step 2: Run RED**

  Run: `make world-resident-test`

  Expected: missing lifetime types/functions.

- [x] **Step 3: Implement idempotent CPU destruction first**

  `world_neighborhood_free` must:

  - clear active grid first only if it points to this neighborhood's grid;
  - call `world_ground_grid_free`;
  - call `n2_free_scene` for ordinary and vista scenes;
  - free region bytes and every region TPK index;
  - free LOC4/master/common bytes and TPK indices;
  - free decoded grass buffers, lights, and bounds;
  - zero the structure.

  `world_city_free` frees nav, adjacency, component, event-mask, and race-owned
  arrays exactly once, then zeros the structure.

- [x] **Step 4: Split loading without changing assembly semantics**

  Move region selection, instance placement, deduplication, terrain bias,
  bounds, grid, texture-source opening, lights, vista, and stats into
  `world_neighborhood_load`. Move event catalog, nav, adjacency, mode, and
  district setup into `world_city_load`. The initial neighborhood is supplied
  read-only so the existing district terrain attribution can use the same
  scene/grid it uses today; later neighborhood swaps do not rebuild city data.

  The compatibility `world_load_ex(World *w, ...)` loads the initial
  neighborhood, activates its grid, then loads city state once against that
  neighborhood. It remains the entry point until Task 5 and must produce the
  same scene at the same center.

- [x] **Step 5: Update consumers mechanically and rerun fixed-center evidence**

  Use `world.neighborhood.scene`, `.vista`, `.mbb`, `.lights`, and
  `.inst_stats`; use `world.city` for route/event/district operations. Do not
  change any selection predicate or draw/collision equation.

  Run:

  ```text
  make world-resident-test
  make world-instance-test
  make world-texture-test
  make world-cli-test
  make clean && make -j4
  make debug -j4
  ```

  Compare the existing fixed-center `--instance-audit` output before/after;
  mesh, triangle, placement, missing-model, rejection, and support lines must be
  byte-identical.

- [x] **Step 6: Record a scoped diff checkpoint**

  Run `git diff --check`; do not stage or commit.

---

### Task 4: Own and destroy one neighborhood's GPU/collision package

**Files:**
- Modify: `src/render.h`
- Modify: `src/render.c`
- Modify: `src/world_resident.h`
- Modify: `src/world_resident.c`
- Modify: `src/main.c`
- Modify: `tools/world_texture_test.c`
- Modify: `tools/world_resident_test.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct {
      uint32_t *texture_keys;
      GLuint *textures;
      unsigned char *texture_modes;
      int texture_count;
      GLuint *mesh_textures;
      unsigned char *mesh_modes;
      int *mesh_batch;
      int mesh_count;
      GLuint terrain_texture;
      N2Batch *ordinary, *sky, *glow, *vista;
      int ordinary_count, sky_count, glow_count, vista_count;
      WorldMeshBatch *debug_batches;
      float (*obstacles)[4];
      float (*obstacle_z)[2];
      int *obstacle_src;
      int obstacle_count;
  } WorldResidentResources;

  void render_batch_array_free(N2Batch **batches, int *count);
  void world_resident_resources_free(WorldResidentResources *resources);
  int  world_resident_resources_build(WorldResidentResources *out,
                                      WorldNeighborhood *neighborhood);
  ```

- [x] **Step 1: Write RED ownership tests**

  In the hidden GL context used by `world_texture_test`, create two textures and
  two `N2Batch` entries with live VBO/IBO handles, place them in a
  `WorldResidentResources`, call the destructor, and assert:

  ```c
  assert(glIsTexture(tex[0]) && glIsBuffer(batch[0].vbo));
  world_resident_resources_free(&r);
  assert(!r.textures && !r.ordinary && !r.debug_batches && !r.obstacles);
  assert(r.texture_count == 0 && r.ordinary_count == 0 && r.obstacle_count == 0);
  world_resident_resources_free(&r); /* idempotent */
  ```

  Keep debug wrappers pointed at the same batch handles and verify the destructor
  does not attempt a second deletion through the wrapper array.

- [x] **Step 2: Run RED**

  Run: `make world-texture-test`

  Expected: missing resource types/destructors.

- [x] **Step 3: Implement exact GL ownership**

  `render_batch_array_free` deletes each nonzero VBO/IBO once, frees the array,
  zeros pointer/count. `world_resident_resources_free` deletes the unique
  texture ids, calls the batch destructor for each owning pass, frees the
  non-owning debug wrapper array without deleting handles, frees the parallel
  per-mesh texture/mode/batch maps and collision arrays, then zeros the
  structure.

- [x] **Step 4: Extract the existing upload path into the builder**

  Move, without changing ordering or shader policy:

  - `world_bind_textures` output map and per-mesh texture/mode resolution;
  - ordinary, sky, glow, and vista batch upload;
  - debug wrappers over ordinary handles;
  - `phys_collect_walls` obstacle/source/Z arrays.

  The builder allocates dynamically from measured scene counts; it must not use
  the current fixed 2048 texture or obstacle arrays. Clear pre-existing GL
  errors before upload; a zero required handle or a new `glGetError()` result
  other than `GL_NO_ERROR` is a failed candidate. On any allocation/upload
  failure, call `world_resident_resources_free(out)` and return zero.

- [x] **Step 5: Run GREEN and rendering regressions**

  Run:

  ```text
  make world-texture-test
  make world-render-test
  make world-resident-test
  make clean && make -j4
  make debug -j4
  ```

  Re-capture Golf glass and the M140 fixed airport pose; hashes must match their
  pre-Task-4 captures because resource ownership alone changes no draw policy.

- [x] **Step 6: Record a scoped diff checkpoint**

  Run `git diff --check`; do not stage or commit.

---

### Task 5: Transactional resident construction and activation

**Files:**
- Modify: `src/world_resident.h`
- Modify: `src/world_resident.c`
- Modify: `src/world.c`
- Modify: `src/main.c`
- Modify: `tools/world_resident_test.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct {
      WorldNeighborhood world;
      WorldResidentResources resources;
      float center[2];
      float radius;
      unsigned long generation;
  } WorldResident;

  typedef struct {
      const char *track_root;
      const char *trackname;
      int scenery_event;
      WResidentPolicy policy;
  } WResidentBuildArgs;

  int  world_resident_build(WorldResident *candidate,
                            const WResidentBuildArgs *args,
                            float center_x, float center_y,
                            float player_x, float player_y, float player_z);
  void world_resident_activate(WorldResident **active,
                               WorldResident **candidate);
  void world_resident_free(WorldResident *resident);
  ```

- [x] **Step 1: Write RED transaction tests with a test build hook**

  Add a test-only builder hook under `WORLD_RESIDENT_TESTING` that supplies
  synthetic neighborhoods/resources and can fail at CPU, validation, GPU, or
  collision stage. Assert:

  ```c
  WorldResident *active = fixture_resident(1, 0.0f, 0.0f, 2.0f);
  const N2Mesh *old_meshes = active->world.scene.meshes;
  world_ground_grid_activate(&active->world.grid);
  assert(!fixture_candidate_build(&candidate, FAIL_GPU));
  assert(active->world.scene.meshes == old_meshes);
  assert(world_ground_at(&active->world.scene, 1, 1, -9, &z) == WSURF_ROAD);
  assert(fixture_candidate_build(&candidate, BUILD_OK));
  world_resident_activate(&active, &candidate);
  assert(!candidate && active->generation == 2);
  assert(active->world.scene.meshes != old_meshes);
  ```

  Add a candidate with no support under player XY and assert rejection before
  GPU construction. Add a successful candidate and assert player data passed
  by the caller remains byte-identical across activation.

- [x] **Step 2: Run RED**

  Run: `make world-resident-test`

  Expected: transaction API missing.

- [x] **Step 3: Implement CPU validation before GL work**

  Validation requires:

  - nonempty ordinary scene and nonempty ROAD/TERRAIN grid;
  - every indexed mesh has finite vertices, valid indices, and finite bounds;
  - `world_ground_hit` at player XY succeeds using player Z as reference;
  - `inst_stats.rejected_meshes == 0` for ordinary supported builds;
  - candidate center/radius equal the requested deterministic values.

  Do not change player Z to the returned support height. The hit proves the
  resident contains the current layer; `PhysVerticalState` continues resolving
  vertical motion after activation.

- [x] **Step 4: Implement pointer swap and delayed old destruction**

  `world_resident_activate` swaps the pointers, activates only the new grid,
  increments generation, returns the previous pointer through `candidate`, and
  does not free it internally. The frame loop destroys that previous resident
  only after no draw/collision call can reference it.

- [x] **Step 5: Run GREEN**

  Run:

  ```text
  make world-resident-test
  make world-instance-test
  make world-texture-test
  make world-render-test
  ```

  Expected: transaction failures preserve the active query results; successful
  activation changes all resident-owned pointers together.

- [x] **Step 6: Record a scoped diff checkpoint**

  Run `git diff --check`; do not stage or commit.

---

### Task 6: Wire synchronous neighborhood movement into `--world2` free-roam

**Files:**
- Modify: `src/main.c`
- Modify: `src/world_resident.c`
- Modify: `tools/world_resident_test.c`
- Modify: `docs/DEVELOPER_GUIDE.md`
- Modify: `docs/PROJECT_STATE.md` (local, gitignored)

**Interfaces:**
- Consumes: Task 1 policy and Task 5 build/activation lifecycle.
- Produces: one active `WorldResident *` used by every world draw, ground query,
  light pass, and wall collision call.

- [x] **Step 1: Add RED CLI/mode guards**

  Extend `world-cli-test` so moving residency requested without `--world2`, with
  `--track ALL`, or in race mode exits 2; a valid `--world2 --track STREAML4RA`
  invocation reaches normal data-root validation instead of argument rejection.

- [x] **Step 2: Run RED**

  Run: `make world-cli-test`

  Expected: new mode-guard cases fail.

- [x] **Step 3: Replace startup aliases with the active resident owner**

  Remove `N2Scene scene = world.scene` and fixed texture/obstacle arrays. Every
  frame reads:

  ```c
  N2Scene *scene = &active_resident->world.scene;
  WorldResidentResources *wr = &active_resident->resources;
  ```

  Pass `scene`/`wr` consistently to ground, collision, lights, ordinary/vista,
  and debug drawing. No cached mesh index may survive activation unless it is
  rebuilt as part of `WorldResidentResources`.

- [x] **Step 4: Add the frame-boundary reload transaction**

  At the beginning of the update frame, only when `--world2` and city mode is
  free-roam:

  ```c
  float target[2];
  if (!candidate && world_resident_target(&policy, carpos[0], carpos[1],
                                           active->center[0], active->center[1],
                                           target)) {
      candidate = calloc(1, sizeof *candidate);
      if (!candidate || !world_resident_build(candidate, &build_args,
                                               target[0], target[1],
                                               carpos[0], carpos[1], carpos[2])) {
          world_resident_free(candidate);
          candidate = NULL;
          failed_cell[0] = target[0]; failed_cell[1] = target[1];
      }
  }
  if (candidate) {
      WorldResident *old = active;
      world_resident_activate(&active, &candidate);
      world_resident_free(old);
  }
  ```

  Store the failed snapped cell and do not retry it until the target changes.
  Copy none of `carpos`, `vel`, `heading`, vertical state, camera smoothing, or
  input state into resident structs.

- [x] **Step 5: Run GREEN and fixed-state regressions**

  Run:

  ```text
  make world-cli-test
  make world-resident-test
  make world-instance-test
  make world-group-test
  make world-texture-test
  make world-render-test
  make car-material-test
  make clean && make -j4
  make debug -j4
  git diff --check
  ```

  At a stationary supported start, take two captures and compare hashes with
  the pre-M144 baseline. No reload should occur and rendering must be unchanged.

- [x] **Step 6: Measure and select the production policy constants**

  Add `--resident-audit RADIUS X Y` using the same resident builder, never a
  parallel parser. At 1000, 1200, 1400, and 1600 m for three supported L4RA/L4RB
  poses, record one table containing build/upload ms and mesh/triangle/batch/
  texture/light/obstacle counts. Set the default to the smallest radius where:

  ```text
  resident_radius >= measured ordinary draw radius + cell_size + safety_margin
  ```

  Keep the rejected full-city result out of production and document the chosen
  constants and measured peak resident counts in `DEVELOPER_GUIDE.md`.

- [x] **Step 7: Drive deterministic boundary routes**

  Run two identical routes crossing at least two resident cells in L4RA and
  L4RB. For each swap verify:

  - target center, scene/batch/texture/obstacle counts repeat exactly;
  - player position, velocity, heading, and vertical state do not jump because
    of activation;
  - support exists and NaN/OOB/out-of-range source counts remain zero;
  - before/on/after screenshots contain no empty band, duplicate district, or
    collision/visual mismatch.

- [x] **Step 8: Update the local project index and leave Git untouched**

  Record result/evidence/files/next-smallest-fix in `docs/PROJECT_STATE.md`.
  Report the complete dirty-file list and verification evidence. Do not stage,
  commit, push, merge, or open a PR without a separate user instruction.
