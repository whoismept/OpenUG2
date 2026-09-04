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
  │    ├─ decode authored 0x135003 light sources into World.lights
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

### Default instance-driven district flow

Normal free roam uses the instance-world path. With no track argument the
engine opens `STREAML4RA`; selecting any single `--track STREAM...` keeps the
same path. `--world2` remains a backward-compatible alias for old scripts and
`--track ALL` remains a separate legacy research union, never a playable map.

Without a developer `--spawn` override, `main` intersects companion region
polygons with section ids actually present in the selected STREAM bundle,
chooses a deterministic authored focus, builds the resident, then selects the
nearest ROAD triangle whose full car footprint has support, collision clearance
and headroom. This removes the old hard-coded L4RA `--spawn start` requirement.

For this path, `main` resolves the authored focus before `world_load_ex`.
`world_instance_build` reads companion district polygons, selects the smallest
containing home district and nearby district ids, then chooses one bundle that
has the home section. It builds a local prototype library from that bundle,
places in-range non-ground instances with their instance matrices, and emits
the one-time ROAD/TERRAIN fallback prototypes before the normal world pipeline:

The fixed-neighborhood diagnostic uses a 1000 m placement radius. At the
airport reference it emits 13,797 post-dedup meshes (2,149 eligible
placements), versus 12,017 at the former 700 m radius. Loading the whole bundle
at 7000 m emits 96,841 post-dedup meshes, confirming that the complete STREAM
superset cannot be one resident scene. Production `--world2` free-roam now uses
the M144 moving-resident policy documented below.

Instance model identity is the authored key, not the display name. The section
walker carries the three keys from each type record into `WInstPlacement`;
`winst_library_resolve` tries the primary then the explicit available alternatives.
Keyed records never fall back to a similarly named object. The bounded object
name is used for diagnostics and scenery classification. Name-only compatibility
is restricted to records with no keys. Multiple resident copies of the SAME
key still use the first copy; district-specific copy selection and live streaming
are separate, unimplemented work.

```text
single-STREAM authored focus
  -> companion polygon selection
  -> one home-section STREAM bundle
  -> local prototype library + instance placements
  -> world_dedup -> terrain bias -> bounds/ground grid -> nav -> textures/batches
```

After the completed ground grid exists, the provisional pose is resolved from
an authored ROAD/TERRAIN triangle. Once the car dimensions and collision scene
are available, the final free-roam pose is refined by the same patch, wall and
headroom rules used by safe-spawn diagnostics.

Use the GL-free audit for deterministic assembly evidence:

```sh
./nfsu2 DATA_ROOT --track STREAML4RA --instance-audit
```

It reports selected bundle/home district, region and instance totals, placed
meshes, missing-model and rejected-placement counts, triangle and scene-class
totals, keyed/explicit-LOD/name-only resolution counts, finite-coordinate failures,
and the ROAD/TERRAIN support result; it
exits before SDL/OpenGL initialization. Run it twice and compare the outputs,
normalizing only machine-specific absolute paths or elapsed time if present.
For a fixed render check, add `--spawn X,Y --heading DEG`, replace the audit
switch with `--shot OUTPUT.png`, and change only the heading (for example
`-101`, `-11`, `79`, or `169`). In this explicit-pose `--shot` mode, `--heading`
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
single-region bundles also consult a measured master-neighbour TPK. The last
fallback is `GLOBAL/InGameCommon.bun`, resolved relative to the TRACKS directory.
All original per-region binding attempts finish before this last fallback;
a common copy cannot preempt a later region's successful lookup, nor cause
ordinary meshes to borrow from unrelated regions in diagnostic multi-region mode.
The common library supplies textures only; its keys are not added to prototype
or submesh material selection. All STREAM buffers survive binding; the common
bytes and TPK index are freed/reset at its end. A missing
record and a decoded-but-noise-rejected texture are different failures; keep
those counters separate. A measured example is common key `2e95ce7d`,
`SFX_LIGHT_BEAMB` (32x256, tagged DXT3): the positional hanging-light material
references it, but the current decode is high-frequency colour noise and is
rejected. Do not force that key into instance material selection until its data
layout/offset is decoded correctly; the existing regional fallback is safer.

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

`world_dedup` runs before bounds, the ground grid and GPU upload. Bounds and
counts are only a cheap grouping prefix; identity also compares material
ownership plus the complete index, vertex/UV and prelight payload byte-for-byte.
This matters after material splitting because different ranges share the same
source vertex pool and can have equal counts without being duplicate faces. Do
not replace the exact comparison with an epsilon merge: nearby distinct
authored layers can be meaningful.

Terrain receives a small -0.05 m world-space bias after dedup so coplanar road
strips win the depth test and ground selector. This is current engine policy,
not a decoded file-format field.

## 7. Rendering

### Batch construction

`upload_world_batches` excludes sky/glow, sorts ordinary meshes by spatial
cell, resolved GL texture and effective draw mode, splits runs before the
`u16` vertex ceiling, then
sorts the final batch list by texture. The post-sort batch index is not the
emission index. Diagnostics must replay the recorded emission run; re-deriving
membership after the final sort can attribute the wrong object.

Each uploaded `BatchedVertex` contains position, UV, generated normal and
source prelight colour. Normals are generated per source mesh so unrelated
objects are not smoothed together.

Sky and glow use `upload_cat_batches` and separate depth/blend behaviour.
Vista batches exist only for the experimental full tier.

### Authored sky

`SKYDOME` is not a camera-locked flat colour. It is a world-space shell with
an exact two-range material partition: an opaque dome and a DXT3 alpha cap.
Their source sunrise keys live in `LOC4DYNTEX.BIN`; `--sky` remaps only those
keys to the matching shipped `night` (default), `sunrise`, or `sunset` pair
before the normal texture-binding pass. Invalid profiles exit with status 2.

The sky pass uses the real camera view, a 30 km far plane and depth writes off.
It enables the narrow `uEmissiveTex` shader gate, draws the dome first and the
alpha-blended cap second, then restores blending, depth mask, fog density,
shader gates and the ordinary MVP. Exact `SKYDOME`/`SKY_` classification is an
invariant: broad substring matching moves buildings and factory skylights into
this special pass and is forbidden.

### Authored district lights

`world_load_ex` extracts enabled `0x135003` light records while each
`WRegion.data` buffer is still owned by the world. It appends exact-unique
records to dynamic `World.lights` storage before texture upload releases the
STREAM bytes. `main` frees that array at shutdown; scene meshes and texture
objects do not own it.

Light records have no mesh texture slot, so `world_bind_textures` explicitly
requests the shipped `SFX_FLARE_GLOWA` key through the same regional/LOC4/master
resolver as world materials. Do not add a second texture decoder or keep
region bytes alive after binding merely for lights.

At night, sources inside the ordinary fog-derived view distance draw as
camera-facing quads in `render_district_lights`. This late additive pass enables
depth testing, turns depth writes off, consumes texture alpha through
`uEmissiveTex`, and restores the incoming uniforms, texture, blend factors and
blend/depth enable/write state. Like other mesh draws, it sets vertex attributes
and buffers; the caller binds the main program on texture unit zero. Additive
emission fades toward zero, not fog RGB, so distant black sprite edges do not
add colored rectangles. `make light-state-test` verifies this on a real SDL/GL
context with synthetic textures, including an occluded light and deliberately
different incoming state; it requires no retail assets. Daylight
draws zero authored light quads. Fixed M136 reference poses draw 197 of 2,228
L4RA sources and 64 of 193 L4RB sources; these numbers prove distance culling,
not a universal visibility count for every camera.

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

Texture mode is not sufficient evidence that an entire object owns that mode.
`n2_world_draw_mode` permits authored cutout/blend/additive only for an exact
`0x134B02 -> 0x134012` range or a true single-slot object. A malformed
multi-slot fallback is forced opaque, and batch construction keeps that opaque
fallback separate from exact ranges even when both resolve to the same GL
texture and effective draw mode.

The main world draw loop in `main.c` dispatches on `drawmode`:

- `OPAQUE`/`CUTOUT` share the ordinary pass (depth test+write on, no
  blending); `CUTOUT` additionally sets the shader's `uAlphaTest` uniform to
  1.0 for the duration of its run of batches, enabling a fragment-shader
  `discard` below 0.5 alpha. Toggled per-batch like the existing `lasttex`
  bind cache, reset to 0.0 after the loop.
- `BLEND`/`ADD` batches are collected into two separate arrays (`defblend`,
  `defadd`, grown to `nbatch` -- never silently capped) instead of being
  drawn inline, then drawn in one pass right after: depth write off (so
  they don't occlude what's behind or each other), depth test still on (so
  real opaque geometry still occludes them correctly). `defblend` is sorted
  back-to-front by `n2_sort_back_to_front` (GL-free, unit-tested in
  `tools/car_material_test.c`) on squared bbox-centre-to-camera distance,
  with a deterministic smaller-index tie-break; `defadd` draws after, in its
  original stable texture-sorted order (additive-over-additive order only
  affects rounding in an associative sum, not correctness, so it needs no
  per-frame sort). `glBlendFunc` is set per-batch (`GL_SRC_ALPHA,
  GL_ONE_MINUS_SRC_ALPHA` for `BLEND`, `GL_SRC_ALPHA, GL_ONE` for `ADD`).
  This mirrors the existing car-glass and vista translucent-pass shape
  elsewhere in the file.
- Authored alpha (M135-R): the shader's `uTextureAlpha` uniform, enabled for
  the whole deferred blend/add bracket, multiplies the sampled texture's own
  alpha into the fragment's output alpha (`t.a*uAlpha` instead of the flat
  `uAlpha` every other pass uses) -- otherwise a cutout-shaped blend/additive
  sheet (e.g. a lit-window texture with transparent gaps between windows)
  would draw fully opaque/full-strength through those gaps, since `uAlpha`
  alone carries no per-texel information. Disabled (0.0) everywhere else,
  including `CUTOUT`, cars, and every HUD/debug `uUnlit` pass.

Texture alpha is never gated on `uVColor`: per-vertex prelight strength and
authored texture transparency are independent and must not be conflated.

### Texture format dispatch (M135-R)

`n2_tpk_decode` selects P8/DXT1/DXT3 from the record's own `+0x3e` format
tag (`N2_TEXFMT_P8=0x08`, `_DXT1=0x22`, `_DXT3=0x24`), not the old
`Size`-vs-`w*h` heuristic -- independently verified against 2309 real
records across three files with zero contradictions (see `docs/FORMATS.md`).
`PaletteSize` is still cross-checked as a corruption guard (P8 requires a
real palette; DXT1/DXT3 require none); a tag that doesn't match any proven
format, or contradicts `PaletteSize`, is rejected outright rather than
falling back to a guess. `N2_TEXFMT_DXT5`/`_BGRA8` are named for
completeness (an external reference claims those byte values) but never
seen on real world data -- those belong to the car `TEXTURES.BIN`
offset-slot path (`n2_load_car_tex_by_key`), which has its own,
separately-proven compression byte and is not affected by this change.

### LOC4 fallback and draw modes (M135-R census)

`world_bind_textures` first tries the region's own
TPK, then `w->loc4` (`LOC4DYNTEX.BIN`, decoded through
`n2_load_car_tex_by_key`, the CAR texture-library reader) as a shared
fallback, then the master TPK. M138 retries still-unbound requests against
`GLOBAL/InGameCommon.bun` only after every original region attempt. Only
`n2_tpk_decode` reads the `order`/
`usage`/`blend`/`wz` draw-mode bytes; a key resolved through the LOC4
fallback keeps them at their zero default (`N2_DRAW_OPAQUE`), same as every
world texture had before the field existed. The original M135-R census resolved
ordinary world-material traffic directly from its regional TPK; this is not a
guarantee for objects restored later. M138's restored `421X_STARTFINGRAPHIC`
needs common `STARTLINE` (`96d35495`), a 64x64 DXT3 BLEND record whose decoded
alpha ranges from 153 to 204. M136
proved one intentional exception: the `SKYDOME` dome/cap pairs live in LOC4.
They bypass ordinary draw-mode dispatch and use the dedicated authored-sky
path, so the cap's decoded alpha is retained without inventing LOC4 header
metadata. Extend LOC4 draw-mode decoding only if a future ordinary world
texture is proven to require it.

`make world-texture-test` runs synthetic files through the real world loader,
decoder and hidden OpenGL upload. It checks common fallback for meshes, vistas
and lights, RGBA/mode preservation, duplicate requests, region/master precedence,
later-region precedence, unavailable keys and common-buffer cleanup. No retail
assets are required. `--tex-audit` reports `TEXSOURCE` for common-library hits.
Texture resolution is not event visibility: a restored start/finish graphic's
presence in free roam still needs an independently proven activation rule.

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

Before gathering support, `ground_motion_limit` (in `src/ground_motion.h`)
checks the XY move proposed by `phys_car_step`. `world_ground_sweep` finds
above-to-below crossings of actual ROAD/TERRAIN triangles by each wheel's
upper contact envelope. The move stops before a crossing; only velocity into
the face is removed. Z remains owned by the ride integrator, and overhead
layers are not recovery targets. The query uses the active ground grid without
a capped list of candidate meshes. Accepted yaw is reconstructed and rechecked
because wheel endpoints do not interpolate linearly through a turn.

Run `make ground-motion-test` for synthetic slope, overhead, winding, dense-grid
and turning regressions. This is sampled wheel-envelope protection, not full
rigid-body continuous collision detection. It does not cover subsequent wall
pushes, vertical/angular suspension integration, or restore missing support.
In particular, support loss can still leave the car at the ride tilt limit;
passing these tests is not evidence of retail handling or a playable lap.

### Collision

Building rectangles are broad phase only. `cw_mesh_feature` confirms a nearby
source-mesh face clipped to the car's vertical envelope, rejects combined wall
spans below `WALL_MIN_FACE_SPAN`, and returns the closest feature normal and
penetration. `collide_walls` pushes along that normal and removes only the
into-wall velocity component.
Clipping must happen **before** projecting the face into XY: overlapping Z
bounds alone allow a distant upper edge to cause a false ground-level contact.

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

### Scenery group audit (M139)

Build `make world-group-test world-group-audit`, then run:

```sh
./build/world_group_audit /path/to/NFSU2/TRACKS L4RA
./build/world_group_audit /path/to/NFSU2/TRACKS L4RA BARRIERS_4144
```

The first command validates all companion group tables and their STREAM
section/placement targets. The second also prints members of that exact
authored group. No GL context or game launch is required. Exit 0 means the
structural checks passed, 1 means missing/corrupt/inconsistent data, and 2
means invalid arguments or a requested group with no members.

The reader validates the enclosing chunks before visiting payloads, then checks
group hashes, reference bounds/counts, placement row bounds and the copied
flags. It never treats placement `+0x1c` as a global override index. Input
buffers remain alive until the borrowed table view is no longer used.

This tool does **not** enable or disable groups in the engine. Event membership
is proven; activation timing, race direction and career-stage state are not.
Some venue graphics (six in RB) have no override membership. Do not convert
this audit into a blanket name-prefix filter or section-level suppression.

### Scenery selection (M140/M147, conservative; not live activation)

The checked reader is shared from `src/world_group_reader.h`. The selection
policy in `src/world_scenery.h` hides only exclusively numeric-event
memberships; ordinary/shared/unknown placements remain. Both rendering and
collision are built from that same filtered scene. Normal instance-driven free
roam selects `free` by default; live `--event` races remain unfiltered until
retail direction/career activation timing is decoded.

For a bounded city comparison, run these as three independent loads:

```sh
./nfsu2 /path/to/NFSU2 --world2 --track STREAML4RA \
  --spawn 1837.844,-791.08 --heading 90 --car GOLF \
  --scenery-preview free --shot free.png --frames 1
./nfsu2 /path/to/NFSU2 --world2 --track STREAML4RA \
  --spawn 1837.844,-791.08 --heading 90 --car GOLF \
  --scenery-preview 4144 --shot event4144.png --frames 1
# Repeat the first command with a different output name, then compare hashes.
```

`--instance-audit` can replace the capture flags for a GL-free assembly check.
The preview cannot be used interactively, with `--event`, race/drive audits,
or alternate-spawn captures/audits. Event selection here does not arm a race.
An unrecognized numeric event or inconsistent group data fails loading; it
does not silently fall back to an unfiltered scene.

At this RA pose with the current 1000 m chunk, free suppresses 258 placements;
event 4144 suppresses 226, restoring 32 placements / 92 emitted meshes / 604
triangles. The fixed-camera start/finish strip returns without moving the road
or neighboring scenery.
The standard RB slot-16 neighborhood has no affected exclusive memberships;
its unchanged preview is a negative control, not evidence of missing filtering.

Run `make world-instance-test world-group-test world-cli-test` for synthetic
off/event/off coverage. The instance fixture uses identical model names for
five placements with different memberships; it checks emitted meshes and real
wall-contact responses, shared memberships, unknown events and corrupt targets.
Runtime direction/career selection is still unimplemented. Do not present this
preview as retail free-roam fidelity or automatic Enter/finish activation.

### Moving world neighborhood (M144, default free-roam)

The instance-driven world keeps one replaceable `WorldResident`. Persistent
navigation, districts, events and race state stay in `WorldCity`; geometry,
ground grid, regional texture sources, lights and bounds live in
`WorldNeighborhood`. `WorldResidentResources` owns the matching GL textures,
ordinary/sky/glow/vista batches, per-mesh material maps and solid-collision
arrays. Do not cache a scene mesh or batch index outside that package.

Resident replacement is transactional. Normal free-roam prepares CPU geometry
on one worker; `--resident-sync` selects the synchronous diagnostic control:

1. `world_resident_target` snaps the player to a deterministic 400 m cell.
2. `world_resident_prepare` / `world_neighborhood_load` builds a detached CPU
   candidate without activating its grid or making GL calls.
3. CPU validation checks finite geometry/index/bounds, zero rejected instances
   and ROAD/TERRAIN support under the current player layer.
4. `world_resident_resources_build` resolves textures and uploads every pass,
   then builds collision from that same candidate scene.
5. At a frame boundary `world_resident_activate` swaps the complete owner and
   activates its ground grid; only then is the former resident destroyed.

Build failure leaves the old resident active and the failed snapped cell is not
retried until the player targets another cell. Player position, velocity,
heading, sprung ride/contact state, camera and input are deliberately outside
the resident and must remain byte-identical across activation. This path is
free-roam-only, uses one STREAM bundle and never uses `--track ALL`.

The worker owns copied request strings and a zero-initialised candidate.
Completion is atomically published; the frame thread joins it before validating
the **current** player pose and calling `world_resident_finish`. Stale results
are discarded, and shutdown joins any outstanding job before freeing data.
Texture decode/upload, batches and cleanup still block the frame thread.
`--resident-drive-audit PREFIX --resident-realtime` exercises this worker with
physics paced at approximately 60 Hz; unpaced audits otherwise use the sync
control. A successful resident swap does not prove continuous ground contact.

`WorldNeighborhood.master` records whether the master resource came from
`mmap`. Destruction must use `res_unmap_file` for mapped data and `free` only
for heap data; treating both as heap allocations caused a real L4RB shutdown
crash and is covered by `world-resident-test`.

The measured production policy is `{resident=1400, draw=933, safety=67,
cell=400}` metres. Its invariant is `resident >= draw + cell + safety`; 1200 m
is therefore invalid even if one test camera appears complete. Measurements at
the supported L4RA start / L4RB sprint-grid pose:

| radius | L4RA meshes / batches / textures / GPU ms | L4RB meshes / batches / textures / GPU ms |
|---:|---:|---:|
| 1000 | 15880 / 3214 / 424 / 511 | 3179 / 642 / 128 / 45 |
| 1200 | 19076 / 3780 / 570 / 701 | 3730 / 879 / 240 / 93 |
| **1400** | **23768 / 4528 / 694 / 1077** | **4142 / 1059 / 277 / 101** |
| 1600 | 31557 / 5397 / 777 / 1546 | 6916 / 1449 / 371 / 135 |

Use `--resident-audit RADIUS X Y` with one explicit `--track` to measure the
real loader/uploader. `--spawn` is optional. The audit is rejected for legacy,
`ALL` and `--event` modes. Times are synchronous development measurements, not
frame-budget promises. They predate the background CPU preparation above.

Use `--resident-route-audit PREFIX` with the same mode guards to exercise the
production transaction across two supported navigation points. The audit takes
`PREFIX_before.png`, `PREFIX_swap1.png`, `PREFIX_after1.png`,
`PREFIX_swap2.png` and `PREFIX_after2.png`, and fails if support, mesh-to-batch
or obstacle-source ownership is invalid. It freezes vehicle physics only; route
points come from the persistent navigation graph and must resolve to ROAD or
TERRAIN in the active resident.

The final deterministic routes were:

| region | resident centers | final mesh / batch / texture / obstacle counts |
|---|---|---:|
| L4RA | `(1600,-400)` -> `(1200,-400)` | 37728 / 6159 / 873 / 17745 |
| L4RB | `(800,400)` -> `(400,400)` | 4103 / 1020 / 253 / 1878 |

Two independent runs per region produced identical hashes for every
corresponding capture and repeated the same centers and counts. L4RB remained
visually coherent from road to suspension bridge. A persistent black
panel/strip family near the L4RA `(1200,-400)` resident appears in both swap and
post-swap captures; it is therefore a separate map/material defect, not a
one-frame residency tear.

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
