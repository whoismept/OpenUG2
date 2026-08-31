# M134 Instance World Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in, instance-driven L4RA world assembly path that places local-coordinate models directly from shipped instance matrices and proves the airport scene without importing PR #4 physics or effects.

**Architecture:** A GL-free `world_instance` module owns chunk parsing, region selection, local prototype collection, and direct instance placement. `world.c` invokes it through an explicit load request before existing deduplication, bounds, ground-grid, navigation, collision, batching, and rendering stages. `main.c` supplies a measured diagnostic spawn and keeps the legacy loader as the default A/B control.

**Tech Stack:** C99, existing single-header NFSU2 parser, SDL2/OpenGL production build, synthetic binary fixtures, retail-data command-line audits.

**Spec:** `docs/superpowers/specs/2026-08-28-pr4-fidelity-import-design.md`

## Global Constraints

- Do not import PR #4 vehicle dynamics, doughnut behaviour, ABS/ESP, launch control, camera, post-processing, live cube-map, or forced paint.
- Do not replace the fragment shader or change texture/alpha behaviour in M134.
- The legacy world loader remains the default and must produce unchanged diagnostics when instance mode is disabled.
- Load one authored district working set for the home region; never compose production free roam with `--track ALL`.
- Parser and matrix code must be GL-free and covered by a failing synthetic test before production implementation.
- Do not hide geometry with new asset-name blacklists. Existing measured vista classification may route backdrop impostors to `World.vista`.
- Original retail assets, executable bytes, and derived disassembly output remain local and never enter Git.
- Release and debug builds must complete with zero warnings before M134 is considered complete.

---

### Task 1: Pure region and instance-record parser

**Files:**
- Create: `src/world_instance.h`
- Create: `src/world_instance.c`
- Create: `tools/world_instance_test.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: little-endian chunk bytes and `n2_u32` from `src/nfsu2.h`.
- Produces:

```c
typedef struct {
    int id;
    float bb[4];
    float *xy;
    int nxy;
} WInstRegion;

typedef struct {
    uint16_t type_index;
    uint16_t flags;
    float bounds_min[3], bounds_max[3];
    float matrix[16];
} WInstPlacement;

int  winst_parse_regions(const unsigned char *data, long len,
                         WInstRegion **out, int *count);
void winst_free_regions(WInstRegion *regions, int count);
int  winst_select_regions(const WInstRegion *regions, int count,
                          float x, float y, float radius,
                          unsigned char *selected, int selected_cap,
                          int *home_index);
int  winst_decode_placement(const unsigned char *record, long len,
                            WInstPlacement *out);
```

- `winst_decode_placement` reads type at `+0x18`, flags at `+0x1a`, position at `+0x20`, and nine signed 16-bit row-major rotation/scale values at `+0x2c`; 8192 equals 1.0.

- [ ] **Step 1: Write the failing synthetic parser tests**

Create `tools/world_instance_test.c` with helpers that write little-endian u16/u32/f32 fields into fixed byte arrays. Construct one `0x80034150` container holding one `0x00034152` polygon record: id 17, bbox `[-10,-5,10,5]`, and four vertices forming that rectangle. Assert:

```c
WInstRegion *r = NULL; int nr = 0;
assert(winst_parse_regions(buf, used, &r, &nr) == 1);
assert(nr == 1 && r[0].id == 17 && r[0].nxy == 4);
unsigned char selected[32] = {0}; int home = -1;
assert(winst_select_regions(r, nr, 0, 0, 2, selected, 32, &home) == 1);
assert(home == 0 && selected[17] == 1);
assert(winst_select_regions(r, nr, 30, 0, 5, selected, 32, &home) == 0);
```

Construct a 64-byte placement with type 3, flags `0x8040`, position `(100,-25,7.5)`, and signed rotation values `{0,-8192,0, 8192,0,0, 0,0,8192}`. Assert exact type/flags/translation and matrix values within `1e-6`. Truncate both fixtures by one byte and assert the parse/decode returns 0 without allocation leaks.

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
make world-instance-test
```

Expected: compilation fails because `world_instance.h` and the four `winst_*` functions do not exist.

- [ ] **Step 3: Implement the minimal GL-free parser**

Implement bounded direct-child walking, variable `0x11` filler skipping, polygon parsing, point-in-polygon home selection, bbox-to-radius selection, and signed matrix decoding. Reject negative/overflowing chunk sizes, region ids outside the supplied selected array, vertex counts outside `1..64`, and truncated records.

Add to `Makefile`:

```make
world-instance-test: tools/world_instance_test.c src/world_instance.c src/world_instance.h src/nfsu2.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tools/world_instance_test.c src/world_instance.c -o build/world_instance_test -lm
	./build/world_instance_test
```

The target creates `build/` before compilation and prints `world_instance_test: PASS` only after every assertion.

- [ ] **Step 4: Run the test and verify GREEN**

Run `make world-instance-test` twice. Expected: `world_instance_test: PASS` both times, exit 0.

- [ ] **Step 5: Commit**

```bash
git add Makefile src/world_instance.h src/world_instance.c tools/world_instance_test.c
git commit -m "test: define instance world record parsing"
```

---

### Task 2: Local prototype library and direct placement

**Files:**
- Modify: `src/world_instance.h`
- Modify: `src/world_instance.c`
- Modify: `src/nfsu2.h`
- Modify: `tools/world_instance_test.c`

**Interfaces:**
- Consumes: Task 1 region/placement functions; existing `N2Scene`, `N2Mesh`, `n2_obj_matrix`, `n2_mesh_category`, `n2_mesh_name`, `n2_mesh_submeshes`, `n2_mesh_texslots`, and `n2_add_pair`.
- Produces:

```c
typedef struct {
    long instances_seen;
    long instances_in_range;
    long meshes_placed;
    long missing_models;
    long own_matrix_meshes;
    long rejected_meshes;
    int regions_total;
    int regions_selected;
    int home_region;
    char bundle[64];
} WInstStats;

int winst_place_mesh(N2Scene *dst, const N2Mesh *src,
                     const float matrix[16], const char *asset_name,
                     WInstStats *stats);
```

`N2Mesh` gains only `char aname[28]` and `unsigned char inst`; do not add the PR fields `hasm` or `objm` to final scene meshes because prototype matrices remain internal to `world_instance.c`.

- [ ] **Step 1: Extend the test with a direct-placement regression**

Build a three-vertex local mesh at `(0,0,0)`, `(2,0,0)`, `(0,1,0)` with one triangle, UVs, vertex colours, category, texture key, and source name. Apply a matrix representing a 90-degree XY rotation and translation `(100,-25,7.5)`. Assert:

```c
assert(winst_place_mesh(&dst, &src, p.matrix, "XO_TEST", &st) == 1);
assert(dst.count == 1 && dst.meshes[0].inst == 1);
assert(strcmp(dst.meshes[0].aname, "XO_TEST") == 0);
assert(close3(dst.meshes[0].verts + 0, 100.0f, -25.0f, 7.5f));
assert(close3(dst.meshes[0].verts + 5, 100.0f, -23.0f, 7.5f));
assert(close3(dst.meshes[0].verts + 10, 99.0f, -25.0f, 7.5f));
assert(dst.meshes[0].verts != src.verts && dst.meshes[0].idx != src.idx);
assert(dst.meshes[0].vcol != src.vcol);
```

Add a NaN coordinate case and assert it is rejected, frees the copied buffers, leaves `dst.count` unchanged, and increments `rejected_meshes`.

- [ ] **Step 2: Run the test and verify RED**

Run `make world-instance-test`. Expected: compile failure because `winst_place_mesh`, `WInstStats`, `N2Mesh.inst`, and `N2Mesh.aname` do not exist.

- [ ] **Step 3: Implement direct placement and local prototype collection**

Implement `winst_place_mesh` with deep copies of vertices, indices, and optional vertex colours. Apply the instance matrix directly in OpenGL column-major form. Preserve UV, category, texture key, scene class, source name, and vertex-repair marker. Validate finite world coordinates and an absolute coordinate limit of `1e8` before pushing.

Inside `world_instance.c`, add private `WInstProto`/`WInstLibrary` types. Walk each `0x80134010` model object and collect its geometry with `mtx = NULL`, keeping the object matrix only on `WInstProto` for unique ROAD/TERRAIN fallback placement. Preserve existing per-submesh material partition validation and slot selection; malformed partitions use the existing whole-object fallback.

Use case-folded, 27-character asset-name hashing plus exact folded comparison for type-to-prototype lookup. An unresolved name increments `missing_models`; it must not place a different hash-collision model.

- [ ] **Step 4: Run parser and build checks**

Run:

```bash
make world-instance-test
make clean
make -j4
```

Expected: test PASS; release build exit 0 with zero warnings.

- [ ] **Step 5: Commit**

```bash
git add src/world_instance.h src/world_instance.c src/nfsu2.h tools/world_instance_test.c
git commit -m "world: place local models from instance matrices"
```

---

### Task 3: District builder and production world integration

**Files:**
- Modify: `src/world_instance.h`
- Modify: `src/world_instance.c`
- Modify: `src/world.h`
- Modify: `src/world.c`
- Modify: `src/main.c`
- Modify: `Makefile`
- Test: `tools/world_instance_test.c`

**Interfaces:**
- Consumes: Tasks 1-2 parser, prototype library, and placement API.
- Produces:

```c
typedef struct {
    int enabled;
    float focus_x, focus_y;
    float view_radius;
} WLoadOptions;

int world_load_ex(World *w, const char *troot, const char *trackname,
                  const WLoadOptions *options);
int world_load(World *w, const char *troot, const char *trackname);

int world_instance_build(N2Scene *scene, N2Scene *vista,
                         const char *track_root,
                         const char *const *bundles, int bundle_count,
                         float focus_x, float focus_y, float view_radius,
                         const unsigned char *shared, long shared_len,
                         WInstStats *stats);

#ifdef WORLD_INSTANCE_TESTING
typedef int (*WInstVisitFn)(const WInstPlacement *placement,
                            const char *type_name, void *userdata);
int winst_test_collect_placements(const unsigned char *section_data,
                                  long section_len,
                                  float focus_x, float focus_y,
                                  float view_radius,
                                  WInstVisitFn visit, void *userdata,
                                  WInstStats *stats);
#endif
```

`world_load` is a wrapper that calls `world_load_ex(..., NULL)` and therefore preserves the legacy API and behaviour.

- [ ] **Step 1: Add failing section-selection tests**

Extend the synthetic fixture with two sections. Each contains `0x00034101`, `0x00034102`, and `0x00034103`; only one placement bbox intersects the focus radius. Assert that the private section decoder exposed through a test-only `winst_test_collect_placements` callback visits exactly the in-range instance, reports the second as out of range, and rejects a type index equal to `type_count`.

Compile the test with `-DWORLD_INSTANCE_TESTING`, so the test hook is absent from production builds.

- [ ] **Step 2: Run the test and verify RED**

Run `make world-instance-test`. Expected: compile failure because section collection and the production builder are missing.

- [ ] **Step 3: Implement one-district assembly**

Implement `world_instance_build` with these rules:

1. Parse companion-region polygons for the requested bundle set.
2. Find the smallest polygon containing the focus and mark polygons within `view_radius`.
3. Select one bundle carrying a section for the home region. If none does, fail without modifying the destination scenes.
4. Build that bundle's local prototype library and texture-key set.
5. Scan instance AABBs against the focus radius; place referenced non-ground models directly from their instance matrix.
6. Place each ROAD/TERRAIN prototype once using its own object matrix. A prototype without an object matrix remains in local/world coordinates with identity.
7. Do not place an unreferenced non-ground LOD variant. Route models already identified by the existing measured vista predicate to `vista`, not `scene`.
8. Do not introduce `TRACKBARRIER`, `ZPM_`, `BANDEROLLE`, or other new name-based suppression.
9. Return deterministic stats and free all temporary region, prototype, index, and bundle buffers on success and failure.

- [ ] **Step 4: Integrate behind explicit options**

Add `src/world_instance.c` to `SRC` and `src/world_instance.h` to `HDRS`.

In `world_load_ex`, retain regional file mappings and texture packs, but skip the legacy `n2_walk_meshes` call when `options && options->enabled`. Invoke `world_instance_build` before `world_dedup`; all existing downstream terrain bias, bounds, grid, event, nav, district, texture, and GPU paths remain unchanged.

In `main.c`, parse:

```text
--world2
--spawn start
--spawn X,Y
--heading DEG
--instance-audit
```

For M134, `start` is the measured L4RA airport reference pose `(1695.2, -883.6)` used for deterministic validation. It is a diagnostic lookup, not a new free-roam policy. Apply the requested XY before `world_load_ex` so region selection is correct; resolve Z from the completed world using the nearest supporting layer reference, not `world_ground_z(..., 0)`. Do not change ordinary launches without `--world2`.

`--instance-audit` prints `WInstStats`, scene class counts, finite-coordinate failures, support category/Z at the requested spawn, then exits before SDL/GL initialization.

- [ ] **Step 5: Verify synthetic and legacy paths**

Run:

```bash
make world-instance-test
make clean
make -j4
make debug -j4
git diff --check
```

Expected: fixture PASS; both builds exit 0 with zero warnings; diff check clean.

Run the existing legacy command twice without `--world2` and compare its diagnostic output after removing timing-only lines. Expected: byte-identical output and unchanged mesh/face counts.

- [ ] **Step 6: Commit**

```bash
git add Makefile src/world_instance.h src/world_instance.c src/world.h src/world.c src/main.c tools/world_instance_test.c
git commit -m "world: add opt-in instance-driven district assembly"
```

---

### Task 4: Retail airport evidence and M134 documentation

**Files:**
- Modify: `docs/DEVELOPER_GUIDE.md`
- Modify: `docs/FORMATS.md`
- Modify: `docs/PROJECT_STATE.md` locally only if present and ignored
- No production-code additions unless a verification failure is first reproduced by a new failing test.

**Interfaces:**
- Consumes: `--instance-audit`, `--world2`, `--spawn`, `--heading`, and the M134 stats API.
- Produces: documented format layout, deterministic audit evidence, and fixed-camera screenshots kept outside Git.

- [ ] **Step 1: Run deterministic retail audits**

With the local retail data root, run twice:

```bash
./nfsu2 /Users/mert/Downloads/compressed --track STREAML4RA --world2 --spawn start --heading -101 --instance-audit
```

Normalize only absolute paths and elapsed time. Diff the two outputs. Expected: identical selected bundle/home region, instance totals, placed mesh totals, missing-model count, triangle count, and supported spawn Z/category.

- [ ] **Step 2: Capture four fixed headings**

Run headless screenshots at `-101`, `-11`, `79`, and `169` degrees with post-processing absent. Inspect each for coherent runway/apron, road paint, aircraft, buildings, walls, trees/props, grounded spawn, NaN/OOB counters, and obvious duplicate/floating geometry. Keep screenshots and logs under `scratchpad/m134/`; do not stage them.

- [ ] **Step 3: Run regression verification**

Run:

```bash
make clean
make -j4
make debug -j4
make world-instance-test
git diff --check
git status --short
```

Run one existing L4RA legacy static capture and confirm its diagnostic mesh/face counts match the pre-M134 baseline. The known vertical-contact defect is reported separately and is not relabelled as fixed by M134.

- [ ] **Step 4: Document the measured implementation**

Update `docs/FORMATS.md` with the exact region/type/instance chunk layouts, filler handling, signed rotation scale, local-model rule, ground fallback rule, and overlapping-district selection. Update `docs/DEVELOPER_GUIDE.md` with the opt-in data flow and commands. Do not copy milestone logs or retail binary bytes into documentation.

- [ ] **Step 5: Commit documentation**

```bash
git add docs/FORMATS.md docs/DEVELOPER_GUIDE.md
git commit -m "docs: describe instance-driven world assembly"
```

- [ ] **Step 6: Final M134 branch check**

Compare `git diff main...HEAD` against the Global Constraints. Expected changed production scope: instance parser/builder, explicit loader option and audit CLI only. Expected absent paths/symbols: `vehicle_model`, `abs`, `post`, `envcube`, `REGPAINTORANGE`, doughnut code, and shader replacement.
