# OpenUG2 developer and agent guide

This is the working technical map for humans and coding agents joining OpenUG2.
It explains how the current engine fits together, where format facts live, and
which invariants must survive a change. Read [FORMATS.md](FORMATS.md) alongside
this guide: this file describes the engine; FORMATS describes the retail data.

OpenUG2 is an early clean-room reimplementation. It is not yet a complete
Underground 2 replacement, and a successful screenshot is not proof that a
parser, map or race path is correct.

## 1. Clean-room boundary

Allowed:

- independently describe byte layouts needed for interoperability;
- inspect legally owned retail data locally;
- write original parsers, tests, synthetic fixtures and aggregate measurements;
- cite public reverse-engineering documentation as research.

Never commit or paste:

- EA assets, archives, textures, meshes, audio or table dumps;
- the retail executable;
- disassembly/decompiler output or copied game code;
- screenshots that expose material the project is not permitted to redistribute.

Repository documentation uses synthetic structures and aggregate evidence only.
A local diagnostic may name an asset to attribute a bug, but do not turn a
retail-data transcript into a committed fixture.

## 2. Current product boundary

The supported reference is one source region at a time:

- `STREAML4RA`: closed-circuit and city/hill reference;
- `STREAML4RB`: airport/drag and sprint reference.

`--track ALL` is a research mode. STREAM bundles are overlapping route/event
supersets; unioning them creates incompatible authored layers in the same world
coordinates. Do not use ALL to prove placement, contact or gameplay.

The production render tier is `ordinary`. `--tier full` is experimental
vista research and can show opaque panorama sheets as hard-edged bands.

## 3. Recommended read order

For a new human or LLM developer:

1. `README.md` — supported paths, gaps, controls and contribution rules.
2. `docs/PROJECT_STATE.md`, when present locally — short live milestone index;
   it is intentionally gitignored and may not exist in a fresh clone.
3. This guide — subsystem boundaries and invariants.
4. `docs/FORMATS.md` — only the format family relevant to the task.
5. The public header for the subsystem.
6. The smallest set of implementation functions named by the header/docs.

Do not begin by reading all 6,000+ lines of `main.c`. Find the call site, then
follow ownership into `world.c`, `physics.c`, `render.c`, `audio.c` or
`nfsu2.h`.

## 4. Source ownership

| file | owns | must not become |
|---|---|---|
| `src/main.c` | SDL/GL setup, orchestration, input, state transitions, camera, diagnostics | a second parser or physics engine |
| `src/nfsu2.h` | clean-room asset parsing, codecs, scene/path types, car profile extraction | renderer or world-state owner |
| `src/world.c/.h` | region lifetime, scene assembly, dedup, textures, nav/events, ground/contact queries | UI or car integrator |
| `src/render.c/.h` | GPU upload, batching, shader programs, matrices, PNG/font helpers | asset-format authority |
| `src/physics.c/.h` | pure horizontal arcade dynamics, sprung ride, mesh wall response, car contact | world query or GL code |
| `src/ai.c/.h` | circuit load and current kinematic opponents | player physics |
| `src/resource.c/.h` | read-only mapping and directory discovery | format parser |
| `src/audio.c/.h` | audio decode/playback and synthesis | vehicle powertrain authority |
| `src/debug.c/.h` | optional ImGui developer interface | production HUD/front end |
| `src/attrib.h` | conservative generic AttribSys diagnostics | proof of per-car fixed-offset fields |

Headers carry the contracts. Long historical comments are evidence notes, but
the executable control flow and current tests are authoritative when a stale
comment conflicts with them.

## 5. Startup and asset-to-frame flow

```text
data root
  ├─ resource discovery
  │    res_list_tracks / res_list_cars / res_list_circuits
  ├─ world_load
  │    ├─ map STREAM region bytes
  │    ├─ open regional/shared TPK indices
  │    ├─ n2_walk_meshes
  │    │    chunk object -> transform -> material ranges -> N2Mesh
  │    ├─ route vista objects into World.vista
  │    ├─ world_dedup
  │    ├─ terrain road-bias, XY bounds and ground grid
  │    └─ events -> nav -> welded CSR graph -> districts
  ├─ car load
  │    GEOMETRY.BIN + TEXTURES.BIN + GLOBALB wheel anchor
  ├─ GL context
  │    world_bind_textures -> upload_world_batches / category batches
  └─ fixed game loop
       input -> horizontal physics -> collisions -> wheel support -> ride
       -> race state -> camera -> render passes -> optional diagnostics
```

### Opt-in instance-driven district flow

The legacy STREAM object walk remains the default. `--world2` is a diagnostic
instance-world path and must be paired with one explicit `--track STREAM...`
and `--spawn start` or `--spawn X,Y`; it refuses `--track ALL`. The named
`start` pose is only a repeatable L4RA reference focus, not a new freeroam
spawn policy.

For this path, `main` resolves the requested XY before `world_load_ex`.
`world_instance_build` reads companion district polygons, selects the smallest
containing home district and nearby district ids, then chooses one bundle that
has the home section. It builds a local prototype library from that bundle,
places in-range non-ground instances with their instance matrices, and emits
the one-time ROAD/TERRAIN fallback prototypes before the normal world pipeline:

```text
--world2 focus
  -> companion polygon selection
  -> one home-section STREAM bundle
  -> local prototype library + instance placements
  -> world_dedup -> terrain bias -> bounds/ground grid -> nav -> textures/batches
```

After the completed ground grid exists, the diagnostic spawn Z is resolved
against the nearest authored ROAD/TERRAIN layer. This establishes load-time
support evidence only; it does not establish vertical contact, collision,
physics, alpha/glass, lighting, or vista behavior.

Use the GL-free audit for deterministic assembly evidence:

```sh
./nfsu2 DATA_ROOT --track STREAML4RA --world2 --spawn start --heading -101 --instance-audit
```

It reports selected bundle/home district, region and instance totals, placed
meshes, missing-model and rejected-placement counts, triangle and scene-class
totals, finite-coordinate failures, and the ROAD/TERRAIN support result; it
exits before SDL/OpenGL initialization. Run it twice and compare the outputs,
normalizing only machine-specific absolute paths or elapsed time if present.
For a fixed render check, retain the same arguments, replace the audit switch
with `--shot OUTPUT.png`, and change only `--heading` (for example `-101`,
`-11`, `79`, or `169`). In this explicit `--world2 --shot` mode, `--heading`
is the fixed camera direction (and seeds the parked car yaw); the requested
supported spawn is preserved, with no legacy showcase selection, race-grid
placement, or capture autopilot. `--instance-audit` remains GL-free and has no
camera output. Keep generated logs and captures outside Git.

### Region bytes and texture lifetime

`WRegion.data` owns each loaded STREAM buffer. Mesh parsing stores copied CPU
geometry, but `N2Tpk` stores offsets into the original bytes. Therefore the
region buffers must stay alive through `world_bind_textures`. That function
decodes/uploads each distinct key and only then frees `WRegion.data`.

`LOC4DYNTEX.BIN` is the shared lookup after the region-local TPK. Some
single-region bundles also consult a measured master-neighbour TPK. A missing
record and a decoded-but-noise-rejected texture are different failures; keep
those counters separate.

### Scene ownership

`N2Scene` owns a growable array of `N2Mesh`. Every mesh owns:

- `verts`: five floats per vertex, `{x,y,z,u,v}`;
- optional world prelight `vcol`;
- `u16` triangle indices;
- category, texture key and source semantic/name metadata.

`n2_free_scene` currently frees vertices and indices but not `vcol`; it is
used primarily by car scenes where `vcol == NULL`. A caller freeing world
meshes must also release `vcol`. Extending the generic helper is desirable,
but do not assume it already owns that path.

`World.scene` is ordinary gameplay geometry. `World.vista` is a separate
background-only scene and must never enter ground, collision, spawn or nav.

## 6. World parser invariants

### Object traversal

`n2_walk_meshes` recursively visits `0x80134010` objects. For each object:

1. read bounded asset name and semantic category;
2. read the `0x134011` world transform;
3. redirect measured panorama/LOD impostors to `World.vista`;
4. collect paired `0x134B01` vertex and `0x134B03` index leaves;
5. validate the `0x134B02 -> 0x134012` material partition;
6. emit one transformed `N2Mesh` per valid material range, or use the
   conservative whole-object fallback;
7. attach the original source name/class to every emitted range.

The object-accounting invariant is:

```text
objects seen = ordinary objects emitted + vista objects + objects with no mesh
```

Do not “fix” placement in the renderer. Track positions are baked into CPU
vertices in `n2_add_pair`; world batches draw with an identity model transform.
If OBJ output is coherent but the screen is not, compare the exact emitted
`BatchedVertex` run against its source mesh before changing matrices.

### Material ranges

Never bind the last texture slot to a whole road merely because it looks
reasonable on one object. Validate start/count/mat-id records and emit each
range with its positional slot. Preserve the fallback for malformed records;
partial success must not silently omit triangles.

### Corrupt vertices

Isolated NaN/huge vertices are source defects inside valid objects. The repair
mask is per source vertex and remains live until:

- all referenced triangles are filtered;
- unused bad vertices are parked on a good point;
- mesh bounds are safe.

The repair is not permission to clamp arbitrary geometry. If all vertices or
triangles are bad, reject the mesh and report it.

### Duplicate and terrain policy

`world_dedup` runs before bounds, the ground grid and GPU upload. Identity is
exact texture key + XYZ bounds + index count + vertex count. Do not replace it
with an epsilon merge: nearby distinct authored layers can be meaningful.

Terrain receives a small -0.05 m world-space bias after dedup so coplanar road
strips win the depth test and ground selector. This is current engine policy,
not a decoded file-format field.

## 7. Rendering

### Batch construction

`upload_world_batches` excludes sky/glow, sorts ordinary meshes by spatial
cell and resolved GL texture, splits runs before the `u16` vertex ceiling, then
sorts the final batch list by texture. The post-sort batch index is not the
emission index. Diagnostics must replay the recorded emission run; re-deriving
membership after the final sort can attribute the wrong object.

Each uploaded `BatchedVertex` contains position, UV, generated normal and
source prelight colour. Normals are generated per source mesh so unrelated
objects are not smoothed together.

Sky and glow use `upload_cat_batches` and separate depth/blend behaviour.
Vista batches exist only for the experimental full tier.

### Visibility tiers

- `baseline`: historical fixed 700 m gate, diagnostic only;
- `ordinary`: production default, view distance derived from fog, no vista;
- `full`: ordinary scene plus experimental authored panorama/LOD pass.

Ordinary does not build or draw vista batches. The current common
`world_bind_textures` pass can still decode/upload a vista key into the shared
key map before the later tier gate; treating that avoidable load-time work as
absent would be inaccurate. A change to full must be verified separately and
must not alter ordinary same-pose output unless that is the explicit task.

### Alpha

`N2Tex.alpha == NULL` means fully opaque. P8, DXT1 and DXT3 world decoders
retain meaningful alpha; an all-255 plane is freed. Do not invent black
chromakey transparency. DXT1 transparency exists only in its three-colour mode.

### Draw modes (M135)

Authored records carry a draw-mode byte pair (`usage`/`blend`, see
`docs/FORMATS.md`); `n2_tex_mode` maps them to
`N2_DRAW_{OPAQUE,CUTOUT,BLEND,ADD}`. The mode is per-texture, resolved once in
`world_bind_textures` (optional trailing `modes` output array) and threaded
per-mesh into `upload_world_batches`/`upload_cat_batches` (optional trailing
`mtexmode` array), landing on `N2Batch.drawmode`. Callers that don't care
(sky/glow/vista/diagnostic-marker batches) pass `NULL` and get
`N2_DRAW_OPAQUE`, unchanged from before this field existed.

The main world draw loop in `main.c` dispatches on `drawmode`:

- `OPAQUE`/`CUTOUT` share the ordinary pass (depth test+write on, no
  blending); `CUTOUT` additionally sets the shader's `uAlphaTest` uniform to
  1.0 for the duration of its run of batches, enabling a fragment-shader
  `discard` below 0.5 alpha. Toggled per-batch like the existing `lasttex`
  bind cache, reset to 0.0 after the loop.
- `BLEND`/`ADD` batches are collected into a `deferred[]` array instead of
  being drawn inline, then drawn in one pass right after: depth write off
  (so they don't occlude what's behind or each other), depth test still on
  (so real opaque geometry still occludes them correctly), `glBlendFunc`
  set per-batch (`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` for `BLEND`,
  `GL_SRC_ALPHA, GL_ONE` for `ADD`). This mirrors the existing car-glass and
  vista translucent-pass shape elsewhere in the file. Not sorted
  back-to-front within itself; every batch is still individually depth-
  tested against the already-drawn opaque scene, and no proven defect
  requires finer ordering yet.

Texture alpha is never gated on `uVColor`: per-vertex prelight strength and
authored texture transparency are independent and must not be conflated.

## 8. Car loading and presentation

The car path is:

```text
GEOMETRY.BIN
  -> n2_walk_car
  -> material-range split when a resolved texture key differs, OR a proven
     material class differs (0x134B02 matid -> 0x134013 hash, e.g.
     WINDSHIELD -> N2_CAR_GLASS, CARSKIN -> N2_CAR_BODY)
  -> n2_car_apply_config (KIT/STYLE override)
  -> n2_car_dedupe_lod (complete-tier selection, see below)
  -> n2_car_profile (body/tyre measurements)

TEXTURES.BIN
  -> offset-slot lookup
  -> JDLZ or HUFF decode
  -> embedded header validates key/dimensions/format
  -> DXT/BGRA decode and GPU upload

GLOBALB.BUN
  -> unique CARS\\<NAME>\\GEOMETRY.BIN anchor
  -> exact axle X and track Y fields
  -> conservative geometry fallback if absent/implausible
```

`KIT00` remains the base car. Higher kit/style records override only matching
families. Roof, body, badge and light appearance must come from the same
material rules; never hard-code roof colour by part name.

### Material routing (`0x134B02.matid -> 0x134013`)

A `0x134B02` submesh record carries two positional indices, not one:
`mat` (`+0x1c`) into the object's `0x134012` texture-slot list (unchanged,
existing), and `matid` (`+0x20`) into a SEPARATE `0x134013` material-hash
list (`n2_mesh_matslots`). These answer different questions: `mat` says
which texture a range binds; `matid` says what CLASS of material it is,
independent of texture. A car object can (and does) put body paint and
window glass on the *same* texture slot while giving them different
`matid` values — `n2_walk_car`'s old texture-only split could never see
that difference and drew the whole thing as one opaque panel.

`n2_mat_class` maps a material hash to a category. Only two mappings are
proven and used: `WINDSHIELD -> N2_CAR_GLASS`, `CARSKIN -> N2_CAR_BODY`
(both hash constants independently re-derived and verified against live
`GOLF` data — see the `n2_mesh_submeshes` doc comment for the exact
records). Chrome, aluminium, moldings, plastics, tire/rim materials and
lens classes are measurable the same way but are deliberately NOT
classified yet; an unmapped or out-of-range `matid` inherits the
object-level category from `n2_car_category`, unchanged.

Before trusting a matid-driven (or texture-driven) split at all,
`n2_car_submesh_partition_ok` requires the `0x134B02` ranges to start at
index 0, chain contiguously with no gap or overlap, and end exactly at the
decoded index buffer's own end. Any violation keeps the old whole-object
path — a malformed partition can shrink coverage but must never grow or
duplicate it.

### Complete-tier LOD selection

Splitting a car object by material means two LOD tiers of the same part no
longer necessarily contain the same slices — a lower tier can lack a glass
slice entirely. `n2_car_dedupe_lod` therefore competes whole TIERS, not
individual meshes: every slice `n2_walk_car` emits from one `0x80134010`
occurrence carries the same `tierid` (a plain incrementing counter, reset
implicitly per car load, distinct from `namekey` which groups a part
ACROSS tiers). Tiers sharing a `namekey` and whose UNION bounding boxes
overlap compete as one family; the tier with the larger SUMMED `nidx`
(triangle index count) across all its slices wins, and every slice of
every other tier in that group is dropped. Do not score by summed
`nverts` — `n2_add_pair` duplicates the whole source vertex array per
split slice, so a heavily-split tier would win merely for having more
slices, independent of how much triangle detail it contributes. Ties keep
the earlier-encountered tier, matching the pre-M135 tie-break exactly.

The stock wheel visual is currently partly procedural. Wheel position and ride
height are separate concerns: axle/track data place contact points, while the
selected tyre radius/body profile determines presentation. Spoilers and wheel
libraries require separate asset loads.

## 9. Vehicle dynamics and contact

There are two coupled but distinct systems.

### Horizontal arcade model

`phys_car_step` runs at 60 Hz in metres per tick. It owns XY velocity,
heading, acceleration/braking, drag, speed-sensitive steering and lateral scrub.
`PhysSurface` multiplies road/terrain behaviour; `PhysVehicle` applies bounded
geometry-derived car differences. No decoded torque curve, gears, drivetrain or
mass currently feeds the model.

Audio's virtual gearbox is not a physics gearbox.

### Four-wheel sprung ride

The old player path assigned centre ground Z directly each frame. Current flow:

1. derive four wheel XY positions from heading, axles and half-tracks;
2. compute each wheel's current contact Z from `PhysRideState`;
3. call `world_wheel_support` with bounded reach up/down;
4. fill `PhysRideSupport`;
5. initialise once with `phys_ride_init`, then step at `1/60 s`;
6. render the body from sprung heave, pitch and roll.

A triangle covering XY is only a candidate. It is contact only inside the
wheel's reach window. The nearest candidate by absolute Z distance wins; a deck
metres above the car is never support. With no contacts, gravity integrates and
`air_frames` grows. Do not add a multi-metre “recovery” reach to hide a bad
spawn or layer transition.

The horizontal model uses metres/tick; the ride integrator uses metres/second.
Mixing those units creates violent launch or damping errors.

### Collision

Building rectangles are broad phase only. `cw_mesh_feature` confirms a nearby
source-mesh face overlapping the car's vertical envelope, rejects combined wall
spans below `WALL_MIN_FACE_SPAN`, and returns the closest feature normal and
penetration. `collide_walls` pushes along that normal and removes only the
into-wall velocity component.

`world_wall_push` handles near-vertical road/terrain guardrail faces using a
measured height band. `world_barrier_push` is race-corridor closure. These are
three separate predicates; a threshold proven for one is not automatically
valid for another.

## 10. Racing, navigation and AI

Content discovery maps `STREAM<stem>.BUN` to `ROUTES<stem>/`. Circuit lists
contain only closed `Paths*.bin` files from that region. When a region has no
circuits, its shipped sprint events can be selected instead.

`world_load_events` reads `0x3414c` before nav so each event's `0x34148`
node range can be tagged. `nav_build_adj` combines consecutive route edges and
5 m coincidence welds into CSR adjacency. `world_set_mode` masks roads outside
the active event corridor and derives closure barriers at outgoing links.

`world_race_start` builds ordered gates from the event outline and loads
shipped grid slots. Start placement selects a supported grid from the correct
direction cluster; it must not project the car onto gate 0 when that XY has no
ground.

Current opponent AI in `ai_step` is kinematic:

- targets sequential racing-line waypoints;
- eases heading and speed for bends;
- snaps Z with `world_ground_at`;
- does not run player physics, surface handling or mesh collision.

That asymmetry is a known limitation, not evidence that player contact should
be simplified to match AI. Sprint opponents are not implemented.

## 11. Diagnostics and evidence

Prefer the narrowest diagnostic that observes the production path:

| question | useful diagnostic |
|---|---|
| object transform/ownership | `--transform-audit REGION` |
| source mesh vs uploaded batch | `--batch-audit REGION NAME` or `#N` |
| material range fallback | `--fallback-census`, `--mesh-material` |
| placement/render completeness | `--local-scene-audit`, same-pose screenshots |
| surface/layer selection | `--surface-stack`, `--cover-probe` |
| race start and progression | `--startline-audit`, `--grid-audit`, `--race-trace` |
| collision feature | `--wall-probe`, `--face-census`, rail census tools |
| panorama classification | `--vista-census`, `--lod-census` |

Audit code must not change production behaviour. If an audit needs an override,
gate it entirely inside the audit flag and prove the ordinary path is unchanged.
Do not select a conclusion from screenshots alone when a source mesh, triangle,
layer or batch can be attributed.

### Minimum verification

For every code change:

```sh
make
make debug
git diff --check
```

Also run the task-relevant deterministic self-test or headless reference:

- parser/material: object/face counts, partition equality, named target audit;
- renderer: same-pose before/after PNG plus ordinary-tier regression;
- physics/contact: `phys_selftest`, `collide_walls_selftest`, synthetic
  harness and L4RA/L4RB trace;
- race: supported start, ordered gate sequence and finish condition;
- portable code: `make gles` where the GLES2 development headers are available.

Boot currently calls the core physics, wall and ground self-tests. Zero warnings
is part of acceptance, not polish.

## 12. Evidence-driven change workflow

1. Reproduce on one named supported region/path.
2. Attribute the first wrong boundary: bytes → parser → scene → batch → shader,
   or input → horizontal physics → collision → support → ride.
3. State the hypothesis and the observation that would disprove it.
4. Add a read-only diagnostic or pure synthetic test.
5. Change the smallest owning function.
6. Verify the target and at least one unaffected reference path.
7. Update FORMATS for new byte facts and this guide for changed architecture.
8. Report result, evidence, files and the next smallest remaining limitation.

Do not bundle parser, renderer, physics and camera changes because they happen to
affect one screenshot. A fix belongs at the first boundary where data diverges.

## 13. Git and collaboration

- Use a descriptive feature branch; never push directly to `main` during
  development.
- Keep commits scoped and explain measured limitations honestly.
- Open a draft PR for review when collaborating through GitHub.
- Do not include “Codex” in branch, commit or PR names.
- Preserve unrelated local changes and untracked `scratchpad/` evidence.
- Never stage the local-only `CLAUDE.md`, `docs/PROJECT_STATE.md` or
  `docs/OPUS_TASK_TEMPLATE.md`; they are intentionally gitignored.

For an agent task, specify:

```text
Goal: one observable outcome
Read: exact guide/format section and owning functions
Evidence: baseline reproduction and named target
Scope: allowed files and explicit non-goals
Verify: deterministic tests and unaffected reference
Output: result / evidence / files / remaining limitation
```

Avoid role-play, praise, milestone-history dumps and broad “fix the map” prompts.
A capable coding model is most effective when the planner has already localized
the owning boundary and supplied a falsifiable acceptance test.

## 14. Common failure patterns

- **Everything at the origin:** object transform was skipped or transposed.
- **One texture across a road/car panel:** submesh range was ignored.
- **Objects apparently scattered:** vista sheet, incompatible ALL composition,
  unsupported camera/spawn, or wrong material can imitate placement failure;
  attribute the source mesh before changing transforms.
- **Car teleports onto a deck:** support selected by global height instead of
  bounded wheel reach and continuity.
- **Car pins near a building:** broad AABB was used as contact, Z overlap was
  ignored, or response used the box axis rather than the touched face.
- **Thin seam behaves as a wall:** contacting-face union span was not checked.
- **Missing roadside content:** one corrupt vertex deleted a whole object,
  unresolved texture was mistaken for missing geometry, or view policy culled it.
- **Foreign circuit/start:** routes were selected outside the loaded region or
  a gate was substituted for the shipped supported grid.
- **Full-world screenshot looks worse than ordinary:** experimental vista
  sheets were treated as ordinary opaque geometry.

When one of these reappears, reuse its existing invariant and test. Do not add a
second workaround in the camera, shader or spawn code.
