# M144 Moving World Neighborhood Design

Status: proposed for review. This document defines the implementation boundary;
it does not authorize loading every STREAM bundle or changing vehicle physics.

## Goal

Make the instance-driven open world continuous beyond its initial load area.
As the player drives, OpenUG2 builds the next authored neighborhood from the
same placement records used by M134-M142, prepares its rendering and collision
resources, and replaces the active neighborhood at one frame boundary.

The player must never observe a partially rebuilt world. Geometry, textures,
lights, ground queries, and solid collision must always describe the same
resident neighborhood.

## Current constraints

- `--world2` correctly rejects `--track ALL`. STREAM bundles are overlapping
  event/route supersets, not additive open-world tiles.
- The initial instance-world load is centered once with a 1000 m radius.
- The ordinary renderer can request context almost to the edge of that radius.
  A moving resident set therefore needs a measured guard band; a fixed 1000 m
  radius plus an arbitrary recenter threshold is not safe.
- `world.c` owns one module-global ground grid. Building another `World` would
  overwrite the active query index before the renderer and collision system
  swap to the new scene, and the previous grid arrays are not released.
- Texture upload, world batches, debug batch wrappers, light state, and wall
  obstacles are created as one-shot state in `main.c`; there is no complete
  reusable destroy path.
- Navigation, events, districts, and race state are persistent city data. They
  must not be reparsed or reset merely because nearby render geometry changes.

## Architecture

### 1. Persistent city and replaceable neighborhood

Split the existing `World` ownership into two explicit lifetimes:

```text
WorldCity (one track/session)
  navigation, adjacency, districts, events, barriers, race metadata
  selected STREAM bundle and immutable load configuration

WorldNeighborhood (replaceable)
  center/radius/scenery filter
  ordinary scene + vista scene + mesh bounds + ground grid
  region/LOC4/master/common texture source bytes until upload
  authored lights and instance statistics

WorldResident (replaceable, ready-to-draw package)
  WorldNeighborhood CPU data
  resolved GL textures and draw modes
  ordinary/vista/debug batches
  wall obstacle arrays and source/Z metadata
```

`WorldCity` remains stable while driving. `WorldNeighborhood` is built from the
existing companion polygons, types, placements, and model libraries. It never
concatenates every STREAM bundle and does not invent object positions.

The first refactor must preserve the existing one-shot load byte-for-byte at a
fixed center before movement is enabled.

### 2. Ground-grid ownership

Replace the anonymous singleton allocation with an owned `WGroundGrid`:

```c
typedef struct {
    const N2Mesh *meshes;
    float x0, y0;
    int gw, gh;
    int *start, *list;
} WGroundGrid;
```

Each neighborhood builds and frees its own grid. Existing ground/collision APIs
may retain their `N2Scene *` signatures during M144, but they resolve through an
explicit active-grid pointer set only by `world_resident_activate()`. Building
a candidate neighborhood must never change the active pointer.

This bridge is intentionally narrow. A later cleanup may pass `WGroundGrid *`
directly, but M144 does not require a cross-project API rewrite.

### 3. GPU and collision ownership

Move the current world texture maps, authored draw modes, terrain texture,
ordinary/vista batches, and debug wrappers behind a reusable resident builder
and destroyer. Destruction must delete owned GL textures, VBOs, and IBOs exactly
once; debug wrappers share handles and therefore must not delete them again.

Build wall obstacle arrays from the candidate scene before activation. The
active scene, mesh bounds, ground grid, batches, lights, and obstacle arrays are
swapped as one `WorldResident *`, rather than copied into unrelated globals.

### 4. Transactional build and swap

M144 uses synchronous loading first. A stall is acceptable for this correctness
milestone; a torn world is not.

```text
player crosses reload boundary
  -> choose deterministic target center
  -> build candidate CPU neighborhood (active resident unchanged)
  -> validate scene, grid, support, finite data, and placement accounting
  -> upload candidate textures and batches
  -> build candidate collision arrays
  -> at the next frame boundary, atomically select candidate
  -> activate candidate ground grid
  -> destroy the former resident after the swap
```

Any parse, allocation, support, texture, or upload failure destroys only the
candidate and leaves the old resident active. It must not fall back to legacy
world assembly, `ALL`, or a partially empty scene. Repeated failure is rate
limited until the player reaches a different target cell.

### 5. Reload policy and measured radius

The policy is a pure function of player XY, resident center, draw distance,
resident radius, safety margin, and target-cell size. It must satisfy:

```text
resident radius >= ordinary draw radius + recenter threshold + safety margin
```

Before selecting production constants, run the same three supported city poses
at 1000, 1200, 1400, and 1600 m. Record meshes, triangles, batches, distinct
textures, collision obstacles, build time, upload time, and frame time. Choose
the smallest radius that preserves the invariant above without approaching the
previously rejected full-city (~97k mesh) residency.

The target center is snapped to a deterministic XY cell so small steering
changes do not rebuild the same area repeatedly. Directional look-ahead may be
measured later, but is not required for the first correct implementation.

### 6. State preserved across a swap

- Player position, velocity, heading, speed, vertical-contact state, and chosen
  vehicle profile.
- Camera smoothing/orbit state, input state, developer-overlay state, and
  authored sky selection.
- The active STREAM bundle and free-roam scenery selection.
- Persistent navigation, districts, and event catalog in `WorldCity`.

The swap validates ROAD/TERRAIN support at the player's current XY using the
current player Z as the layer reference. It must not respawn, ground-snap, or
rotate the car. If the candidate cannot support the current position, it is
rejected and the former resident remains active.

## M144 scope

M144 enables moving neighborhoods only for instance-driven `--world2`
free-roam. Regular circuit/sprint execution, legacy world assembly, and AI keep
their current ownership. The code must fail loudly if moving residency is
requested in an unsupported mode rather than silently resetting race state.

Likely implementation boundary:

- `src/world.h/.c`: persistent/replaceable ownership, grid lifecycle, load and
  dispose APIs.
- `src/world_instance.h/.c`: unchanged placement semantics; expose only the
  inputs needed for repeatable neighborhood construction.
- `src/render.h/.c`: world-GPU resource construction and destruction.
- new `src/world_resident.h/.c`: reload policy, candidate construction,
  validation, activation, and telemetry.
- `src/main.c`: trigger and frame-boundary swap orchestration only.
- focused GL-free lifecycle/policy tests plus existing render tests.

## Verification

### Deterministic tests

- Reload policy does not trigger inside the safe area, triggers before the draw
  sphere reaches the resident edge, snaps consistently, and does not thrash on
  the boundary.
- Loading the same center twice produces identical scene counts and a stable
  content hash.
- A -> B -> A reproduces A's counts/hash and frees each discarded grid, scene,
  obstacle set, and GPU owner exactly once.
- A failed candidate leaves every active pointer and query result unchanged.
- A successful swap changes scene, bounds, grid, textures/batches, lights, and
  obstacles together; no subsystem retains an index into the former scene.
- Collision attribution after the swap references only meshes in the active
  scene.

### Retail-data integration checks

- Drive across at least two neighborhood boundaries in both L4RA and L4RB.
- Capture before, on, and after each boundary at fixed headings. There must be
  no empty horizon, duplicate district, unsupported car, or one-frame mismatch
  between visible geometry and collision.
- At every swap: player XY/velocity/heading are continuous; support exists;
  finite-coordinate failures, NaNs, and out-of-range mesh references are zero.
- Repeat the same route twice and compare reload centers, counts, and captures
  for determinism.
- Release/debug builds, parser tests, world instance/group/CLI tests, material
  tests, and render tests remain clean.

Runtime telemetry is one concise line per build/swap: center, radius, build ms,
upload ms, mesh/batch/texture/light/obstacle counts, and accepted/rejected
status. No per-frame log spam.

## Non-goals

- Asynchronous or threaded streaming; synchronous correctness comes first.
- Loading or blending all STREAM bundles.
- Changing object transforms, placement rules, materials, lighting, physics,
  suspension, or camera tuning.
- Migrating race/AI state between neighborhoods.
- Implementing career/direction semantics for scenery records.
- Animating scripted `ZCV_`/`ZCS_` definitions whose placement linkage remains
  unproven.
- Hiding holes with fog, teleporting the player, or retaining stale collision.

## Follow-up after M144

Once the synchronous transaction is proven, a separate milestone may move CPU
parsing to a worker and queue GL upload on the render thread. That optimization
must preserve the same `WorldResident` ownership and activation contract, so it
does not change the correctness model established here.
