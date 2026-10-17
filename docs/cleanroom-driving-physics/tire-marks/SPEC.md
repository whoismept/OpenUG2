# Tire marks (skid marks) — clean-room specification

Status: analyst draft for review. Implementation requires the exact reviewer-approved
SHA-256 of this file. Companions: the approved Stage1–5, body-response, body-coupling
and handbrake-release contracts in `docs/cleanroom-driving-physics/`. This contract
consumes their per-wheel quantities; it changes none of their laws.

Confidence tags: **[obs]** observed directly in shipped data bytes; **[inf]** inferred from
static examination of the intake executable (no execution); **[gue]** guessed / weakly
supported. Unless tagged otherwise a statement is [inf]. Vectors are synthetic arithmetic
consequences of this text (see `test-vectors/README.md`). The original was not executed.

## TM-0 Conventions and boundary

- World frame is right-handed with **z up**; ground height is a function of world (x, y).
  Lengths m, speeds m/s, loads kN (as in Stage5), angles rad. `b32(x)` is the nearest
  binary32. Thresholds below are binary32 values; strict/non-strict comparisons are exact
  and must not become tolerance tests.
- Wheel indices 0/1 front, 2/3 rear (Stage4/5).
- Terms reused from companions: world wheel centre `C`; world wheel heading `h` and world
  wheel lateral axis `l` (the wheel frame of Stage4/5, including steer); sampled wheel
  velocity `U = V + Omega × (B·r)` (S4-4/S5-3, world frame); aligned load `N` (S5-5);
  published contact flag (S5-5); the Stage1/3/4 tyre-preparation branch ("sliding" vs
  "gripping") and the S1-7.6/S4-6 friction cap; vehicle geometric origin
  `O = P − B·c` (Stage5 S5 centre convention); per-wheel surface id `s` (Stage5 ground
  query result, 0…28).
- A **trail** is a per-wheel reference to at most one "open" strip. A **strip** is a short
  polyline of 1…8 stored mark points. All strips live in a shared, fixed-capacity store
  that is independent of vehicles.

## TM-1 Data

### TM-1.1 Tyre width (car record) [obs]

In the 2,192-byte car record (FORMATS.md "Global vehicle attributes"), wheel `i` (0…3)
has a binary32 **tyre width** `W_i` (m) at row offset `+0x134 + 0x30·i` (field `+0x14` of
the per-wheel block that starts at `+0x120 + 0x30·i`; radius is field `+0x10`, index `+0x18`).
Examples from retail `GLOBAL/GLOBALB.BUN`: 240SX all four 0.205; 350Z front 0.225,
rear 0.235; G35 front 0.225, rear 0.245; Hummer front 0.285, rear 0.315. The value used is
the effective (composed) record's field; whether upgrades alter it is unknown (TM-U9).

### TM-1.2 Surface parameters

Surface ids are those of the Stage5 surface table (29 entries, 0…28). Labels below are
descriptive only. For each id the mark system uses a **width factor** `k_s` and an
optional **mark texture** (functionally required asset name, flagged; looked up by its
32-bit name hash in the common in-game texture pack `GLOBAL/InGameCommon.bun`). [obs] for
values; [inf] for the role of each column.

| id | label | k_s | mark texture |
|---:|---|---:|---|
| 0 | none | 1.0 | — |
| 1–7 | road, wet road, drift road, three coloured-smoke road variants, bridge | 1.0 | `SKID_ROAD` |
| 8, 9 | dirt, gravel | 1.4 | `SKID_DIRT` |
| 10, 11, 12 | rough road, cobblestone, stairs | 1.0 | `SKID_ROAD` |
| 13 | puddle | 1.0 | `SKID_ROAD` (intensity forced to 0, TM-5) |
| 14 | deep water | 1.0 | — |
| 15 | grass | 1.4 | `SKID_GRASS` |
| 16, 17, 18, 19 | sidewalk, wood, plastic, glass | 1.0 | `SKID_ROAD` |
| 20–28 | walls, plant, post, pillar, metal grate, metal, chain-link, car | 1.0 | — |

`k_s` is 1.4 exactly as binary32 `b32(1.4)` for ids 8, 9, 15 and 1.0 elsewhere.
A surface whose texture is "—", or whose texture cannot be found, still creates and stores
strips (TM-6…TM-10) but never draws them (TM-11). [inf]

### TM-1.3 Mark textures [obs]

Texture records in `GLOBAL/InGameCommon.bun` (TPK header fields per FORMATS.md):

| name (asset id, flagged) | name hash | size | pixel format | usage / blend bytes |
|---|---|---|---|---|
| `SKID_ROAD` | 0x9076F58F | 64×32 | P8 (palette) | 2 / 1 |
| `SKID_DIRT` | 0x906F30FC | 64×32 | DXT3 | 2 / 1 |
| `SKID_GRASS` | 0x9E904149 | 64×32 | DXT3 | 2 / 1 |

Usage 2 / blend 1 is FORMATS.md's source-alpha blend class (depth write off). Texture
pixels are not reproduced here; load them from the user's install.

## TM-2 Cadence and vehicle-level gate

Once per outer vehicle update (the call that runs all of that frame's integration
substeps), after the frame's final integration and state publication, the mark sampler runs
for wheels 0, 1, 2, 3 in that order — **only if** the vehicle-level gate `G` is true.
`G` is the conjunction of three conditions (TM-U2, resolved 2026-10-17):

1. **Wheel effects are enabled for this vehicle.** A per-vehicle Boolean `E`, the
   conjunction of four conditions: the vehicle's wheel-effects flag is set; the vehicle is
   not session-suppressed; its wheel collider exists; and its mover is not in the no-effects
   context. `E` is false while the vehicle is being set up, torn down or despawned (flag
   cleared) and during the pre-race start phase **including the countdown**, where a
   session-phase suspension is applied to every car in the field and lifted when the cars are
   put into play at race start. It is true during ordinary racing. **Residual (unknown):** the
   paused, finished and replay phases, and the exact set of race types that run the pre-race
   start phase. [inf]
2. **The vehicle is not in the special mover mode**, the mode in which aero is disabled and
   the published per-wheel utilisation is forced to 0 (TM-12). It is entered during vehicle
   (re)placement/initialisation (placing the car at its start transform) and leaves when
   normal vehicle dynamics take over. [inf; exact real-play trigger gue]
3. **The global game-state code — the current venue id (the number used to select the
   venue's route/track data) — is not in the reserved range [4400, 4499] inclusive.** That
   range is the id block of the **Organic Drag** events (public in-game event name, flagged
   for traceability; block header 4400; shipped venues 4401, 4402, 4404). Free roam uses 4000,
   and ordinary circuit/sprint/street/drift/speedway
   events use ids outside the block, so this condition is true for them and **false for the
   Organic Drag events**. [obs for ids/range; inf for the comparison]

When `G` is false nothing is sampled: no strip is ended and no point is added; each wheel's
open strip stays open. In particular **no skid marks are ever laid on the Organic Drag
venues (ids 4400…4499), for any car, including the player's.** [inf]

`G` is evaluated **per vehicle**: the sampler runs for the player vehicle and for every
in-play non-player vehicle (AI opponents and traffic), each with its own clause-1 and
clause-2 state and all sharing the single global clause-3 venue id. The wheel-effects
routine is reached from the update path of more than one car-mover class, and the pre-race
start phase suspends wheel effects for every car in the field, so a non-player car with `G`
true lays a strip exactly as the player's does. A host that samples only the player vehicle
does not reproduce this. **Residual (unknown):** which car roles use which mover class,
whether every mover class that runs the routine applies all three clauses, and whether
ambient free-roam traffic outside a race field runs it at all. [inf]

Per-wheel inputs read by the sampler are the values left by the most recent force
preparation/cap of that frame (the same values the frame publishes): sliding branch flag,
utilisation `u`, load `N`, contact flag, surface `s`, `U`, `C`, `l`. No per-substep sampling
and no frame-duration dependence exist. [inf]

## TM-3 Lay predicate (per wheel)

Define the **utilisation** `u` produced at the friction cap (S1-7.6 / S4-6), per wheel:

1. Let `M` be the magnitude of the tested force vector exactly as tested by the cap
   (including the temporary 1.5 longitudinal factor when that rule applies), *before* any
   scaling; let `Fmax` be the cap limit.
2. If `Fmax > .001` then `u = min(M/Fmax, 1)`, else `u = 1`.
3. If the wheel took the sliding branch in that preparation, or is locked, `u = 1`.
4. If the contact flag is false, `u = 0`.

`u` is the per-wheel value the frame publishes (TM-12). The wheel **lays a mark** this
frame iff all hold:

- contact flag true;
- the wheel's bit in the per-vehicle **wheel-suppression mask** is clear (TM-U3);
- `sqrt(U.x² + U.y²) > 1` (horizontal sampled speed, strict);
- sliding branch selected **or** `u·N > b32(8)` (strict; product compared, `N` in kN).

Consequences: with Stage1's sliding rule (|s|>.5, |alpha|>theta_max, or locked) any
sliding wheel moving faster than 1 m/s lays a mark regardless of load. A gripping wheel
lays one only when `u·N > 8` kN, i.e. near saturation under a heavy load. Equality (`u·N=8`,
speed exactly 1) does not lay. If the predicate is false the wheel's trail is **ended** (TM-9).
NaN comparisons are false. [inf]

**Wheel-suppression mask (TM-U3, resolved 2026-10-17).** The mask is a per-vehicle field of
four bits (bit `i` = wheel `i`) with a single per-vehicle **enable** flag. Both
belong to the vehicle's serialized state: they are set to zero when the vehicle object is
created, and their only other access is the per-vehicle state save/load path (used to persist
and to exchange car state). **No in-game event writer exists** — no damage, race-type, drag,
scripted-state or mode-change code ever sets either field. Therefore, unless a loaded or
exchanged state carries non-zero values, the mask is always all-clear and this clause never
suppresses a mark in ordinary play. The same mask together with its enable flag also gates a
per-wheel tyre effect; shipped effect tables contain effects named for a blown tyre /
driving on a flat tyre consistent with that intended use, but no producer was found (naming:
[gue]). [inf]

## TM-4 Mark sample geometry

When a wheel lays a mark compute, in this order:

1. Travel-normal unit vector `a = normalize(−U.y, U.x, 0)` (horizontal, 90° counter-clockwise
   from horizontal travel, i.e. to the **left** of travel).
2. **Alignment** `q = |l · a|`. On a level wheel frame this equals `|h·U_h| / |U_h|`
   = `|cos(alpha)|`, alpha the angle between heading and horizontal travel (1 rolling
   straight, 0 sliding exactly sideways). A tilted `l` contributes only its horizontal part.
3. `w_s = b32(W_i · k_s)`; `q_w = q` if `q ≥ b32(.5)` else `b32(.5)`;
   **half-width** `w = b32(w_s · q_w · .5)`. (Full mark width is `2w`, between
   `.5·W_i·k_s` and `W_i·k_s`.)
4. `A0 = w · a`. Probe points `Lp = C + A0`, `Rp = C − A0` (same z as `C`).
5. `zL = b32(ground(Lp) + d_i)`, `zR = ground(Rp) + d_i`, where `ground` is the
   established world ground-height query at (x, y) of the probe (the same query family as
   Stage5 contact; probe z is the wheel-centre height) and `d_i` is a per-wheel ground
   offset, 0 unless a producer is supplied (none found, TM-U8).
6. Sample point `Pm = (C.x, C.y, b32((zL + zR)/2))`.
7. Sample half-width vector `Am = w · normalize(A0.x, A0.y, (zL − zR)/2)`; its length is
   `w` and it follows the cross-slope between the probes.

Notes: the mark is centred on the **wheel centre's** horizontal position, not a contact
patch point; its height is the mean probed ground height. Behaviour when a probe finds no
ground is unknown (TM-U7). [inf]

## TM-5 Intensity

- `I = q` if the sliding branch was selected, else `I = b32(q · b32(1/3))`.
- If `s = 13` (puddle) then `I = 0`; otherwise if `I < b32(.3)` then `I = b32(.3)`.

So a straight lock-up or wheelspin gives 1; a sliding wheel drifting at alpha gives
`max(|cos alpha|, .3)`; a gripping-but-saturated wheel gives `max(q/3, .3)` (≤ 1/3).
Intensity is not a function of slip magnitude, speed or load beyond the TM-3 gate. [inf]

## TM-6 Stored point encoding

Each stored point holds: position `P` (three binary32, exact), quantised half-width vector
and an intensity byte:

- `Aq_j = trunc(64·Am_j)` stored as a signed 8-bit value per component j (truncation toward
  zero); decoded `Am_j' = Aq_j / 64`. Resolution 1/64 m; stock half-widths are < .23 m
  so no overflow occurs (out-of-range behaviour TM-U10).
- `Ib = trunc(255·I)` stored as an unsigned byte; decoded `I' = Ib · b32(1/255)`.

Truncation is observable: a 240SX road mark has `w = b32(.1025)`, stored as 6/64 =
.09375 m across travel along a world axis. Small cross-slope z components (< 1/64 m) are
lost. [inf]

A strip records: 1…8 points; its surface id; a view flag `f` (TM-11.4); its creating
vehicle; the last committed segment direction `D` (unit) and the accumulated last-segment
length `S`; an axis-aligned bounding box and its centre; its grid cell (TM-10.2). [inf]

## TM-7 Adding a sample to a wheel's trail

Given the wheel's vehicle origin `O`, sample `(Pm, Am, s, I)` and the vehicle's current view
flag `f`:

1. If `|Pm − O| > 4` (3-D Euclidean) do nothing: the trail is neither ended nor extended.
2. If the trail has no open strip: create a strip (TM-10.1) with surface `s`, flag `f`,
   creator = this vehicle, and append the sample (TM-8). It becomes the trail's open strip.
3. Else if the open strip's surface ≠ `s` or its flag ≠ `f`: **continue** (step 5).
4. Else append the sample to the open strip (TM-8):
   - result *ok*: done;
   - result *ended*: the strip is closed and the trail has no open strip; the sample is
     discarded (the next laying frame starts a fresh, unconnected strip);
   - result *full*: continue (step 5).
5. **Continue**: take the open strip's last stored point decoded (`P`, `Aq/64`, `Ib/255`),
   close the old strip, create a new strip with surface `s`, flag `f`, first point = that
   decoded point, then append the current sample. The new strip becomes open.

Hence strips chain without gaps across surface changes and capacity overflow; the first
segment of a continued strip blends from the old point's intensity to the new one (e.g. road
→ puddle fades from 255 to 0 over one segment). [inf]

## TM-8 Append algorithm (one strip, n stored points)

- `n = 0`: store the sample as point 0; `S = 0`. Result ok.
- `n ≥ 1`: let `p` be the last stored point's position, `δ = Pm − p`, `d = |δ|`,
  `e = normalize(δ)` (a zero vector normalises to (1, 0, 0)).
  - `n = 1`: store as point 1; `D = e`; `S = d`. Result ok.
  - `n ≥ 2`: `κ = 1 − e·D`; `L = d + S`.
    1. If `κ > b32(.2)`: close the strip (detach from the trail); result *ended*. Nothing is
       stored.
    2. Else if `L ≤ 3` and (`L < b32(.25)` or `κ < b32(.002)`): **replace** the last stored
       point by the sample (position, half-width, intensity); `S = L`; `D` unchanged.
       Result ok.
    3. Else if `n = 8`: result *full* (nothing stored here).
    4. Else store the sample as point n; `D = e`; `S = d`. Result ok.

Effects: points are committed at most about every 3 m of straight travel, more often on
curves (direction change of the newest chord against the last committed chord > ~3.6°,
once at least .25 m has accumulated); a direction change with `κ > .2` (≈ 36.9°) between
consecutive samples breaks the mark. After every append (store or replace) expand the
strip box by the axis-aligned cube of half-size `d` around `Pm` (`d = 0` for point 0;
the box initialises to point 0), set the box centre to the box midpoint, and union the box
into the strip's cell box. [inf]

## TM-9 Ending a trail

"End" sets the wheel's trail to "no open strip" and marks the strip closed. The strip and
its points remain in the world. Ending happens when the TM-3 predicate is false, when TM-8
returns *ended*, at TM-7 continuation, and when the wheel object is destroyed. No other end
trigger (gear change, reset, camera cut) was established (TM-U5). [inf]

## TM-10 Storage, lifetime and recycling

### TM-10.1 Strip store

- Capacity: 128 strips, or 64 when the session has exactly two local players [inf; player
  count identification gue]. Capacity is fixed (no growth). Store and textures are created
  at world/track session start and destroyed at session end; nothing else clears all marks.
- Recency order: a new strip is most-recent; each time a strip passes all per-view tests
  of TM-11.3/TM-11.4 in any view it becomes most-recent (even if it has no texture or
  fewer than two points).
- Creating a strip when the store is full first removes the **least-recent** strip
  (closing any trail that has it open) and its cell membership.
- There is **no time-based fade or expiry**: a mark persists until evicted (by the store or
  by its cell, TM-10.2) or until session end. [inf]

### TM-10.2 Spatial cells

- Each strip belongs to the cell of its first point: `cx = floor(trunc(65536·x)/2^22)`,
  `cy` likewise (64 m × 64 m squares; arithmetic floor for negatives).
- Cell store capacity 40. A cell is created on demand when a strip starts in an unseen cell;
  if the cell store is full the **least-recently-touched cell is destroyed with all its
  strips**. A cell is touched when a strip is created in it, and when it passes the cell
  tests of TM-11.4 in a view if **more than** 300 world frames (60 Hz world frame counter)
  have elapsed since its last touch.
- A cell's box starts as the creating point and is the union of its strips' boxes. [inf]

## TM-11 Rendering (observable)

### TM-11.1 Geometry per segment

For each strip with a mark texture and `n ≥ 2`, draw one quad per consecutive point pair
(k, k+1), k = 0…n−2, with corners

`v0 = P_k + A_k`, `v1 = P_{k+1} + A_{k+1}`, `v2 = P_{k+1} − A_{k+1}`, `v3 = P_k − A_k`

using decoded half-width vectors, each raised by +`b32(.05)` m in z. Texture coordinates
v0 (0,0), v1 (1,0), v2 (1,1), v3 (0,1): U runs along the segment, the full texture once per
segment whatever its length; V runs from the left edge (0) to the right edge (1). [inf]

### TM-11.2 Colour and alpha

All four vertices have RGB = (128, 128, 128). Vertex alpha for a point with intensity byte
`Ib` under a per-strip fade byte `F` is `max(Ib − F, 0) >> 1` (integer, 0…127); v0/v3 use
point k, v1/v2 point k+1. Blending is the texture's source-alpha class (TM-1.3). How the
original combines these values is now specified in TM-15 (colour ×2, alpha ×1). Full-intensity marks
(Ib 255) give alpha 127; minimum laid intensity (Ib 76) gives 38; gripping-saturated
(Ib 85) gives 42. [inf]

### TM-11.3 Distance fade (as built)

Per view, `K = 256 / tan(phi/2)`, phi the view camera's field-of-view angle (`K = 10` if
the tangent is 0). With strip-centre-to-camera distance `dc`,
`p = trunc(K·dc)` if `dc/2 < 1`, else `p = trunc(2K)`. If `p ≤ 4` the strip is not drawn;
if `p > 10` then `F = 0`; otherwise `F = trunc(256 − (p − 4)·256/6) mod 256`.

The formula above is the observed-by-analysis behaviour (it uses the distance where a
size-based fade would be expected to use a radius). For any ordinary FOV, `F = 0` for
`dc ≥ 2` and the strip is hidden or faded only within a few centimetres of the camera, i.e.
**no practical distance fade** [inf; medium confidence, TM-U6]. A reimplementation that
wants retail parity should not add a fade.

### TM-11.4 Culling and view visibility

A cell is considered in a view when not behind the camera
(`(c − cam)·fwd ≥ −r`, with `c, r` its box centre and half-diagonal), its metric
`trunc(K·r/(|c−cam| − r))` (or `trunc(K)` when `|c−cam| − r ≤ r`) exceeds 10, and its box
passes the view frustum. Within it each strip box must also pass the frustum. This yields a
hard visibility distance growing with the cell's mark extent (≈ `r·(1 + K/10)`). A strip
created by a vehicle is drawn in view index v only if bit v of that vehicle's current view
mask is set; `f` = (bits 1 and 2 both set). Single-view hosts may treat every strip as
visible and `f` constant [gue for bit meaning].

## TM-12 Shared trigger values (smoke / screech)

The utilisation `u` of TM-3 is published per wheel every frame (forced to 0 in the special
mover mode of TM-2) together with its four-wheel mean. The executable reads these published
values in several other places; which effects (smoke, audio, camera) use them and with which
thresholds was **not** established (TM-U1). No smoke/screech law is specified here. The
mark system itself consumes only the unpublished internal values listed in TM-2. Whether the
published `u` is in fact forced to 0 in the special mover mode is a TM-U1 (published-value
consumer) question: it cannot affect marks, because TM-2 clause 2 excludes that mode before
the sampler runs and the mark system reads only the internal values. [inf]

## TM-13 Edge cases

- Zero-length step at `n ≥ 2`: `e = (1,0,0)`; κ depends on `D` and can end the strip.
- Speed exactly 1 m/s horizontal: no mark (strict). Vertical speed is ignored.
- Wheel suppressed or airborne: trail ends every frame; resumption starts a new strip.
- Gate `G` false for some frames then true: the open strip resumes and may draw a long
  straight segment over the gap, subject to TM-8 (L ≤ 3 replacement / κ break).
- Origin distance > 4 m (e.g. a very long wheelbase vehicle): that wheel never lays marks.
- Puddle marks occupy store capacity but are invisible (alpha 0) except for the blended
  first segment of a continued strip.

## TM-14 Vectors

`test-vectors/tire_marks.json` covers: lay predicate boundaries; sample geometry and
intensity (straight, 30°, 70°, sideways; road/dirt/grass/puddle; cross slope);
quantisation; strip sequences (straight replacement, arc commits and *full* continuation,
corner break, surface split, origin-distance reject, long steps); the per-vehicle gate `G`
truth table and venue-range boundaries (`vehicle_gate`); vertex alpha; the fade
metric; cell keys. `test-vectors/render_state.json` (TM-15) covers effective per-pixel
opacity, source colour and composited results over a reference grey.

## TM-15 Render-state amendment (2026-10-16, resolves most of TM-U4)

This section adds to TM-11 and replaces its open question on colour/alpha scaling. Tags:
**[obs]** = read directly from shipped data (texture pack bytes, and the shader/state blobs
stored as data inside the executable); **[inf]** = static examination of executable
instructions (not executed). Statements use Direct3D 9 / OpenGL-neutral terms.

### TM-15.1 Pipeline used

Marks are drawn with the **same shader and pass used for ordinary static world surfaces**
(the road itself), first pass only, with the world transform = identity (vertices are
world-space). [inf] The shader that pass uses is: [obs]

- Vertex stage: position × view-projection; vertex colour passed through **unchanged**
  (no lighting, no material, no global tint constant); texture coordinate passed through
  plus a per-texture scroll offset that is zero for the mark textures (their scroll fields
  are zero, TM-15.5) [offset zero: inf]; a per-vertex fog factor (TM-15.4).
- Pixel stage, with `T` = filtered texel (RGBA in 0…1) and `C` = interpolated vertex colour
  (RGBA in 0…1, each byte / 255):
  `out.rgb = clamp(2 · T.rgb · C.rgb, 0, 1)`, `out.a = T.a · C.a`.
  The colour is doubled; the **alpha is not**.
- The alternative fixed-function path of the same effect (used if programmable shading is
  unavailable) has the identical law: colour = texture × vertex colour × 2, alpha =
  texture × vertex alpha, lighting off, fog off. [obs]

### TM-15.2 Meaning of the vertex bytes

- Vertex RGB byte 128 ⇒ `C.rgb = 128/255`, so the colour multiplier is
  `2·128/255 = 256/255 ≈ 1.0039`: **128 is (almost exactly) unity for colour**; the mark
  colour is the texel colour (×1.0039, clamped). [obs]
- Vertex alpha byte `vA = max(Ib − F, 0) >> 1` (TM-11.2, 0…127) ⇒ `C.a = vA/255`.
  **Alpha is not doubled: 127 maps to 127/255 = 0.49804, not to 1.** (Answer to TM-Q4: a
  full-intensity mark shows the texture at ~50 % of the texel's own alpha.) [obs]
- No other per-mark, per-vehicle, time-of-day or global factor multiplies the colour or
  alpha in this path: the RGB bytes are the constant 128 and the alpha comes only from
  intensity and the TM-11.3 fade byte. Whole-frame post-processing (if enabled) applies to
  all scene pixels alike and is not part of the mark contract. [inf]

### TM-15.3 Fixed-function state while drawing a mark

| state | value | tag |
|---|---|---|
| blending | on; `result = src·a + dst·(1 − a)` (source factor = source alpha, destination factor = one minus source alpha, add) | [inf] from texture blend byte 1 [obs] |
| alpha test | off (only textures with usage byte 1 get a test, reference 95/255; the mark textures have usage 2) | [inf] |
| depth test | on, pass if less-or-equal | [obs] |
| depth write | **off** (every blended texture disables depth writes) | [inf] |
| depth bias / slope-scaled bias / polygon offset | none (never set anywhere) | [inf] |
| z-fight fix | only the +`b32(.05)` m vertex lift of TM-11.1 | [inf] |
| face culling | none (both windings drawn) | [obs] |
| colour write mask | RGBA | [obs] |
| fog | the world surfaces' fog (TM-15.4) | [obs] shader, [inf] same parameters as the road |
| texture addressing | wrap in U and V | [obs] |
| mip filter | linear between levels; min/mag filter = the same user-quality setting as world surfaces | [obs] |

Each segment is submitted as one triangle fan `v0, v1, v2, v3` (two triangles
`v0 v1 v2` and `v0 v2 v3`, shared diagonal v0–v2), vertex colour interpolated per
triangle (Gouraud). [inf]

### TM-15.4 Fog

Per vertex, `f = min(max((Fx − z_view)·Fy, Fz), 1)` with `(Fx, Fy, Fz)` the frame's world
fog parameters and `z_view` the view-space depth; the fogged colour is
`f·out.rgb + (1 − f)·fogColour` (alpha unaffected), enabled whenever world fog is enabled.
I.e. marks are fogged exactly like the road under them. [obs] formula, [inf] parameters
shared with the road.

### TM-15.5 Mark texture data [obs]

All three are 64 × 32 with **3 mip levels** (64×32, 32×16, 16×8; data size 2,688 bytes
each). The 0x7c-byte record's usage byte is 2, blend byte 1, scroll type 0, scroll speeds 0.

| texture | format | alpha min / max / mean (0–255) | alpha values | fraction alpha 0 | mean RGB of texels with alpha > 0 |
|---|---|---|---|---|---|
| `SKID_ROAD` | P8, 256-entry palette, entry byte 3 = alpha (stored 0…127); used colours are grey (R = G = B) | 0 / 127 / 114.16 | only 0 and 127 | 0.10 | (8.31, 8.31, 8.31), max 17 |
| `SKID_DIRT` | DXT3 (4-bit alpha, value·17) | 0 / 102 / 90.22 | 5 distinct | 0.07 | (58.87, 53.91, 38.57) |
| `SKID_GRASS` | DXT3 | identical image to `SKID_DIRT` (level-0 bytes equal) | | | |

- The palette alpha is uploaded **as stored** (127 → 127/255); it is not doubled on load. [inf]
- Profiles: alpha is uniform along U (column-band means within ±3 % of the overall mean) and
  softened at the V edges (row means of the outer 1/8 bands: road ≈ 80 vs 127 in the
  middle; dirt ≈ 55 vs 102). Mip means: road 114.2 / 114.3 / 114.4, dirt 90.2 / 89.7 / 90.1.
- Pixel bases: offsets are relative to the 0x80-aligned start of the texture-pack pixel
  payload (the 0x11 filler bytes are padding up to that alignment; the first palette entry
  may itself begin with the value 0x11).

### TM-15.6 Effective opacity and colour (normative formulas)

With texel alpha `tA` (0–255, after mip filtering), texel colour `tC`:

- `a = (tA/255) · (vA/255)`, `vA = max(Ib − F, 0) >> 1`.
- `src = min(1, 2 · (tC/255) · (128/255))` per channel; `result = src·a + dst·(1 − a)`
  (then fog per TM-15.4 on `src`).

Numeric examples (F = 0):

| case | vA | tA | a |
|---|---:|---:|---:|
| road, Ib 255, opaque texel | 127 | 127 | 0.24804 |
| road, Ib 255, texture-mean texel | 127 | 114.16 | 0.22297 |
| road, Ib 76 (minimum laid) | 38 | 127 | 0.07422 |
| road, Ib 85 (gripping-saturated) | 42 | 127 | 0.08203 |
| dirt/grass, Ib 255, max texel | 127 | 102 | 0.19922 |
| dirt/grass, Ib 255, mean texel | 127 | 90.22 | 0.17621 |

So one full-intensity road layer is ≈ 25 % opaque and near-black (src ≈ 0.033): over a
0.5 grey road it gives ≈ 0.384 (−23 %). Because marks do not write depth, **overlapping
strips (e.g. the rear wheel running in the front wheel's track, or repeated passes) all
blend**: two layers ≈ 0.297, four ≈ 0.182 over 0.5 grey. Since all road-mark texels have
nearly the same near-black colour, the result is effectively independent of the order in
which overlapping strips are drawn. [inf; arithmetic exact]

### TM-15.7 Draw order

- All marks of a view are drawn together in one block per view per frame, after the
  world-surface render lists queued before it have been drawn (so the road is already in
  the colour and depth buffers) and before later render-list flushes. [inf]
- Within the block, order is cell order then strip order as in TM-10/TM-11 (no sorting by
  depth). [inf]
- Order relative to cars, tyre smoke and other decals was **not** established
  (TM-U4 residual). With depth writes off and depth test on, opaque cars occlude marks
  correctly in either order; only blended car parts and smoke could differ.
- No polygon offset exists; hosts that see z-fighting with their own road depth should
  keep the 0.05 m lift and a less-or-equal depth test rather than add bias for parity.

## Unknowns / ambiguities

- **TM-U1** Smoke, screech, camera or controller consumers of the published `u`/sliding
  values and their thresholds; whether they share TM-3's gate. Includes whether the published
  `u` is actually zeroed in the special mover mode (TM-12; not a mark-system concern).
- **TM-U2** *Resolved 2026-10-17 (see TM-2).* Producers of the vehicle gate `G`: a
  per-vehicle wheel-effects-enabled flag (set/cleared on join/leave play and during a session
  phase such as the pre-race start), the special mover mode (entered during vehicle
  (re)placement/initialisation), and the current venue id compared with the reserved
  Organic-Drag block [4400, 4499] (clause false on those events). Residual: whether ordinary
  single-player driving ever holds the special mover mode (believed not), the exact frames
  on which the effects-enabled flag flips, and the clause-1 state in the paused, finished and
  replay phases (TM-2 clause 1).
- **TM-U3** *Resolved 2026-10-17 (see TM-3).* The per-wheel suppression mask (four bits) and
  its adjacent enable flag are per-vehicle serialized state, zeroed at creation and touched
  only by the state save/load path; **no in-game producer exists**, so the mask is all-clear
  in ordinary play. Residual: the external source of non-zero values (a saved/exchanged car
  state) and the "blown tyre" naming, which remains [gue].
- **TM-U4** *Mostly resolved by TM-15* (colour ×2 so RGB 128 ≈ unity; alpha ×1 so 127 →
  0.498; blend/depth/cull/fog/filter state; no depth bias). Residual: draw order of the mark
  block relative to cars, tyre smoke and other blended decals; the min/mag filter values of the
  user quality setting; exact per-frame fog parameter values; fixed-point precision of the
  original pixel pipeline (vectors allow ±1/255).
- **TM-U5** Whether vehicle reset/teleport, replays or race restarts end trails or clear the
  store (only wheel destruction and session teardown were found).
- **TM-U6** TM-11.3 rests on a single static reading; confidence medium.
  The view FOV units and per-camera FOV values feeding `K` are not restated here.
- **TM-U7** Ground-query result when a probe finds no ground, and multi-level ground
  selection by probe height.
- **TM-U8** Any producer of the per-wheel ground offset `d_i` (only initialisation to 0 found).
- **TM-U9** Whether upgrades/tuning change `W_i`; surface table assumed static (no writer to
  the width factor or texture columns found).
- **TM-U10** Behaviour for half-width components ≥ 2 m or intensity > 1 (byte wrap); not
  reachable with stock data.
- **TM-U11** Exact intermediate precision of the original (sums, sqrt, normalisation; may exceed binary32) — vectors use
  tolerances.
- **TM-U12** The frustum test details and the strip-box expansion quirk (cube of half-size
  `d`) only matter for culling parity.
