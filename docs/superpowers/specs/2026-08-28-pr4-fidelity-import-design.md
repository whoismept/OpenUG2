# PR #4 Fidelity Import Design

## Goal

Recover the retail-data fidelity improvements demonstrated by PR #4 without
merging its custom vehicle dynamics, doughnut behaviour, forced paint,
post-processing, live cube-map, or wholesale shader replacement.

The import is staged so each subsystem can be tested, reviewed, and reverted
independently. The existing `main` behaviour remains available until a staged
replacement has passed deterministic and visual A/B checks.

## Non-goals

- No doughnut governor, drift-oriented tuning, ABS/ESP, launch controller, or
  PR vehicle integration.
- No forced orange 350Z paint or other hard-coded customization.
- No PR chase camera, post-processing, or live environment cube.
- No wholesale replacement of the current fragment shader.
- No `--track ALL` composition as a production open-world mode.
- No attempt to solve the existing vertical-contact failure in these
  rendering milestones.

## M134: Instance-driven world assembly

### Data flow

1. Read region polygons from companion bundles (`0x80034150` / `0x00034152`).
2. Select the home region containing the requested spawn and nearby regions
   within the configured view radius.
3. Read a model library in local coordinates from `0x80134000` /
   `0x80134010`; do not bake the model object matrix into its vertices.
4. Read type and instance records from section chunks:
   - `0x00034102`: 68-byte type records.
   - `0x00034103`: 64-byte placements, including type index, position, and
     signed 16-bit rotation/scale values where 8192 represents 1.0.
5. Place each referenced model directly with the instance matrix.
6. Feed the completed scene through the existing deduplication, bounds,
   ground-grid, collision, batching, and rendering paths.

### Boundaries

The implementation belongs in a dedicated world-instance module with a small
public API declared by `world.h`. Parser helpers that do not require OpenGL
must remain testable from a standalone C harness.

`world_load` selects the instance builder only when explicitly requested. The
legacy prototype walk remains the default during M134, allowing deterministic
A/B comparison. A named spawn is resolved before region selection and is
reapplied only after the final ground grid exists.

The builder must load one authored district working set for the home region,
not merge every overlapping STREAM bundle. Race-only barriers and markers must
not be hidden by an unexplained name list: exclusions require measured asset
semantics and are tested independently.

### Acceptance

- Release and debug builds complete with zero warnings.
- A standalone parser test covers region selection, signed rotation/scale,
  direct instance placement, missing model references, and malformed chunk
  bounds.
- On the retail L4RA airport pose, the builder reports deterministic region,
  instance, placed-mesh, missing-model, and triangle counts across two runs.
- The airport road/apron, markings, aircraft, buildings, and roadside objects
  are coherent in four fixed camera headings.
- The spawn is supported by ROAD/TERRAIN and no newly placed mesh contains a
  NaN or absurd coordinate.
- The legacy loader produces byte-identical reference diagnostics when the
  instance builder is disabled.

## M135: Texture, material, glass, and part fidelity

Decode texture format and draw-mode metadata from the record rather than from
pixel-plane guesses. Connect submesh material ids from `0x134B02` to the
object material list in `0x134013`, while preserving the existing positional
texture-slot mapping. Select a complete winning detail tier per part rather
than independently discarding unmatched slices.

The renderer adds only the uniforms and passes needed for record-directed
opaque, cutout, blended, and additive drawing. The current shader is modified
surgically. Golf rear glass and a representative opaque body panel form the
regression pair: the glass must be present and translucent while the body stays
opaque. Face count and complete-part mesh coverage must not decrease.

## M136: Authored sky and world lights

Restrict the sky category to `SKYDOME` and `SKY_` families. Bind the selected
sunrise, sunset, or night texture from `LOC4DYNTEX.BIN` to the authored dome
and cap, drawing the dome in world coordinates with fog disabled only for that
pass.

Decode district light-source records and shared lamp textures, then render
their authored colours in a dedicated additive pass. Every pass must restore
the world matrix and shader state before returning. The feature does not add
post-processing or cube-map reflections.

## M137: Car data foundation

Import the GlobalB car-parameter parser and `car_dyno` as data and diagnostic
infrastructure only. It must not change player physics. Standalone build
instructions include all dependencies and the retail-data checks must pass
before any field is connected to gameplay.

## Existing wheel steering

The current main branch already rotates the front-wheel render transforms from
the steering value. No PR vehicle dynamics are required to retain this visual
behaviour. Each milestone therefore includes a fixed steering capture to
ensure the existing feature is not regressed, but M134-M137 add no alternative
steering model.

## Integration and Git policy

Work occurs on `feature/instance-world-render-fidelity`. Each milestone is a
separate scoped commit after its own verification. Nothing is pushed directly
to `main`; integration happens only after review of the milestone evidence.
Original retail assets and derived binary/disassembly output remain local and
must never enter Git.
