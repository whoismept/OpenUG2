# Tire marks - implementation design log

Implements `SPEC.md` (sha256 `5b968486f48d970e7872848ae70d113eafa18082104307e4137e40bdd28276e0`,
the 2026-10-17 per-vehicle-gate amendment: TM-2 clause 1 gains the session-state conjunct
(pre-race start **including the countdown**) and the gate is restated as a per-vehicle
conjunction; earlier revisions `b8fddf79...` TM-U2/TM-U3, `1b864fc1...` TM-15/TM-11.2 and
`99b70795...` geometry/storage)
and encodes `test-vectors/tire_marks.json`
(sha256 `e2be25a37aa2469377766dbc913727d396b17403c5301d873fbd4ea51407194d`, adding the
`vehicle_gate` truth table) and `test-vectors/render_state.json`
(sha256 `5aa0eace8e1f55a3509211c3e7486efa81bd121252b181d761a40abc86de7e25`). Clean-room: only the
spec, the vectors and the merged independent Stage1-5 modules were used. Open points the spec leaves
unanswered are in `QUESTIONS.md` under "Implementer questions" (TM-IQ1..13).

## Files

| file | role |
|---|---|
| `src/tire_marks.h/.c` | pure module: surface table, lay predicate, sample geometry, strip store/trails, view build |
| `src/tire_marks_drive.h` | header-only adapter from `Stage1Drive` to `TmWheelIn` and gate G |
| `src/drivetrain.c/.h` | publishes per-wheel utilisation `u` at the friction cap (`DrvWheel.utilisation`, `Drivetrain.utilisation_mean`) |
| `src/main.c` | session init, sampler call, texture load, draw; arcade ribbons kept for `!stage1_active` only |
| `tools/tire_marks_test.c` | every vector section + non-vector behaviour checks (`make tire-marks-test`) |
| `src/render.c/.h`, `src/shaders/ug2.frag` | `render_mark_begin/end` and the `uVista > 1.5` mark branch (TM-15) |
| `tools/tire_marks_render_test.c` | GL regression of the mark pass (`make tire-marks-render-test`) |
| `tools/tire_marks_drive_test.c` | adapter integration on the synthetic Stage5 fixture (`make tire-marks-drive-test`) |

## Decisions, traced to the spec

- **TM-0/TM-11.1 arithmetic.** Thresholds are `float`. Lay-predicate speed is `sqrtf(Ux*Ux+Uy*Uy)` in
  binary32 (the vector "slow 1.0 exactly" uses 0.6/0.8, which only evaluates to exactly 1 in binary32
  arithmetic; the Makefile's `-std=c99` disables FMA contraction). Comparisons are strict, never tolerance.
- **TM-1.2** `tm_surface_width_factor`/`tm_surface_texture` hold the 29-id table; ids >28 behave as id 0.
  Textures are looked up by the TM-1.3 name hashes in `GLOBAL/InGameCommon.bun` with the existing
  `n2_tpk_decode`; a missing texture leaves its strips stored but undrawn (TM-1.2).
- **TM-1.1** `tm_car_read` reads `W_i` at row `+0x134+0x30*i` through `n2_global_car_record`.
- **TM-3** `tm_lays` is the four-condition predicate. `u` is computed in `drv_couple`'s S1-7.6 loop from
  the same `mag` and `Fmax` the cap tests (so the 1.5 factor is included, pre-scaling), via the inline
  `tm_utilisation` (header-only so the drivetrain needs no link dependency). `u=1` when the wheel took the
  sliding branch (`!gripping`) or is locked; `u=0` with no contact. The four-wheel mean is published as well
  (TM-12); no smoke/screech consumer is added (TM-U1).
- **TM-4/5/6** `tm_sample` follows the numbered steps; `b32` rounding is applied at each stated point.
  Quantisation is `trunc(64*Am)` (int8) and `trunc(255*I)` (uint8). Saturation instead of wrap for
  out-of-range values (TM-U10, TM-IQ8). `d_i = 0` (`TmGround.offset`, TM-U8).
- **TM-7/8/9** `tm_trail_add` (origin gate 4 m, create, continue, ended, full) and `strip_append`
  (n=0/1, kappa break, L<=3 replace, n=8 full, box growth by a cube of half-size `d`, union into the cell
  box). The event log (`new, commit, replace, end, full, new-continued, new-seeded, far`) is what the vectors
  assert: creation emits `new`/`new-seeded`/`new-continued`, and every append (including the seed point)
  emits `commit` or `replace`. A continuation copies the old last point's stored bytes (TM-IQ1).
- **TM-10.1** Fixed 128-strip store (64 selectable via `tm_init`); recency stamp counter; eviction removes
  the least-recent strip and nulls the trail pointer that holds it. No time-based expiry anywhere.
- **TM-10.2** 40 cells keyed by `floor(trunc(65536*x)/2^22)` (int64 arithmetic floor); creation touches the
  cell; the least-recently-touched cell is destroyed with its strips when a 41st is needed.
- **TM-11.1/2** `tm_view_build` emits four vertices per segment (v0..v3, UVs per spec, `+b32(.05)` z lift,
  RGB 128), alpha `max(Ib-F,0)>>1` emitted unscaled (0...127; the former `alpha_scale` host choice is gone),
  grouped by texture so the host binds each texture once.
- **TM-11.3/4** Cell and strip tests: not-behind, metric `>10`, optional frustum callback, strip metric `p>4`.
  `tm_view_metric` is the exact `p`/`F` formula (no extra fade). Strips passing become most-recent, cells
  touched after >300 frames. The `F` fade is applied as the spec states (practically always 0).
- **TM-2** One sampler call per player tick after the final integration/host position correction:
  wheels 0..3 in order, only when `tm_drive_gate`. Inputs are the values the frame left behind (contact
  flag, `gripping`, `utilisation`, load, `contact.wheel[i].centre/surface`, `U = V + Omega x arm`, steered
  wheel lateral axis, `O = P - B c`).
- **TM-2 gate `G` (per-vehicle amendment, 2026-10-17).** `G` is a per-vehicle conjunction of the
  three spec clauses, sharing the one global venue id. It is factored into a pure predicate in the
  module and a host mapping in the adapter:
  - pure module: `tm_vehicle_gate(clause1, clause2, venue_id) = clause1 && clause2 &&
    tm_venue_lays_marks(venue_id)`; clause 3 is
    `tm_venue_lays_marks(venue_id) = !(4400 <= venue_id <= 4499)` (`TM_VENUE_RESERVED_LO/HI`).
    This is what the `vehicle_gate` vectors test (`lays = clause1 AND clause2 AND clause3`).
  - host adapter: `tm_drive_gate(s, venue_id) = tm_vehicle_gate(clause1, !s->alternate_movement,
    venue_id)` with `clause1 = s->contact_ready && s->prepared &&
    s->session.phase != DRIVE_SESSION_SUSPENDED && s->session.phase != DRIVE_SESSION_COUNTDOWN`.
  Clause 1 (wheel effects enabled for this vehicle) is the host mapping: `contact_ready && prepared`
  (collider present / mover prepared) and "no session-phase suspension". The amendment makes the
  pre-race start phase **including the countdown** an explicit suspended phase, lifted at race start;
  the host carries it as `DRIVE_SESSION_COUNTDOWN` (set by `drive_session_local(active, countdown)`
  while `race_state == 0`), which clause 1 now excludes alongside the general
  `DRIVE_SESSION_SUSPENDED`. Clause 2 (special mover mode, aero disabled / utilisation forced to 0)
  is `alternate_movement` (the Stage1-5 alternate-movement channel). Clause 3 is unchanged.
  The host passes the current global game-state code = the active event's route id
  (`world.city.ev[world.city.race.ev].id`, the number that selects `Paths<id>.bin`), or 4000 for
  free roam. Free roam and ordinary circuit/sprint/street/drift/speedway ids are outside the block,
  so the gate reduces to clauses 1-2; on an Organic-Drag venue (ids 4401/4402/4404 shipped) the gate
  is false for every car, so nothing is sampled and each open strip stays open (TM-2). The
  countdown conjunct is the only behavioural change from the amendment.
- **TM-2 per-vehicle scope limitation.** The spec's sampler runs per vehicle: for the player and for
  every in-play non-player vehicle (AI opponents and traffic), each with its own clause-1/clause-2
  state and the shared clause-3 venue id; a non-player car with `G` true lays a strip exactly as the
  player's. This host still samples **only the player vehicle** (`g_tm_player` at the single call
  site in `main.c`); that is a **scope limitation of this host, not a spec choice**, and no claim of
  reproducing AI/traffic marks is made (also recorded in QUESTIONS.md TM-IQ9).
- **TM-3 mask (2026-10-17 amendment).** The amendment resolves TM-U3: the four-bit suppression mask
  and its enable flag are per-vehicle serialized state with **no in-game producer**, so the
  all-clear value the host already supplies (`tm_drive_inputs(..., mask = 0, ...)` in `main.c`) is
  correct for ordinary play. No behaviour change: the `masked` bit path in `tm_lays` is retained for
  a loaded/exchanged state that carries non-zero bits, which no host path currently produces.
- **TM-13** Verified by tests: origin range, ended trail on airborne/masked wheel, store persistence.

## Host integration (main.c)

- Session start (one store for the process): `tm_init(&g_tm,128)`, texture load, GPU buffers (streamed
  vertex/colour buffers, static normals and quad indices; at most 3584 vertices).
- Sampling runs only when the clean-room drivetrain is active (`stage1_active && contact_ready`). When it is
  not (legacy physics, drag/drift races) the trail is ended and the old arcade ribbon recorder runs instead; the
  arcade ribbon draw stays reachable only through that path (`skidpen` is not armed while the clean-room path runs).
  Tyre smoke puffs are unchanged (TM-12: no smoke law specified).
- Drawing: see "TM-15 render path" below (replaces the earlier `uVista` / `alpha_scale = 2` arrangement).
- Draw order: oldest strip first (TM-U4 leaves it open). Camera K from `256 * P[5]`.
- Tyre widths come from `tm_car_read` at start and on in-process car switch (defaults .22 m if a record is
  missing).

## TM-15 render path

`render_mark_begin/end` (`src/render.c`, used by the mark block in `main.c`) bind the car `glass` pipeline and set the
shader to the mark law; the caller binds the streams and texture and draws. Mapping to TM-15.3:

| spec state | host realisation |
|---|---|
| source-alpha blend, `src*a + dst*(1-a)` | glass pipeline: `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` (rgb and alpha), add |
| depth test `<=`, depth write off | glass pipeline: `RHI_COMPARE_LESS_EQUAL`, `depth_write = 0` |
| no culling | glass pipeline: `RHI_CULL_NONE` |
| no depth bias | the RHI pipeline description has no bias/polygon-offset field; none is set. Only the +0.05 m lift (TM-11.1) |
| alpha test off | the mark branch returns before every other path; no `discard` (the backdrop path's `a < 0.02` discard is not used) |
| colour `clamp(2*T*C)`, alpha `T.a*C.a` | `uVista > 1.5` branch of `ug2.frag`; vertex bytes RGB 128 / alpha 0...127 straight from `tm_view_build` (UNORM8x4 attribute); no lighting, tint, `uAlpha` or `uColor` use |
| fog on colour only | `mix(uFogColor, rgb, fog)` with the host's exp^2 world fog (TM-IQ10); alpha untouched |
| wrap in U and V, linear mips | default sampler of `upload_tex` (repeat, trilinear); the earlier `render_texture_clamp` on the mark textures was removed. Full mip chain, not 3 levels (TM-IQ11) |
| world transform identity | `render_model(NULL)`; vertices are world space |
| triangle fan v0 v1 v2 v3 | two triangles `0 1 2`, `0 2 3` (static index buffer) |

`tm_render_state()`, `tm_mark_source`, `tm_mark_opacity`, `tm_mark_blend` are the pure reference model of the same law,
used by the tests. Vertex bytes of a segment equal the TM-11.2 values, so the vector rows apply to what the module emits.
The draw point is unchanged (after the world render lists and the car shadow, before cars; TM-15.7 residual open: order
against cars, glass, smoke is not established; depth test with no depth write keeps opaque cars correct either way).

The mark textures themselves: `tm_load_textures` restores the 0x80-aligned payload base (`tm_payload_base`, TM-15.5
"pixel bases"; TM-IQ12) before decoding, so P8 palette alpha is used as stored (127 -> 127/255) and the DXT3 alpha plane
as stored (value*17).

## Tests

`make tire-marks-test` (754 checks, including every `render_state.json` section: constants, effective_alpha 9 rows,
source_colour 5 rows, composite 5 rows, plus a built-strip check and `tm_payload_base`; `make tire-marks-render-test`
(52 checks, hidden GL window, no data needed except the optional texture statistics): the composite vector rows
through the production mark pass (+-3/255), colour x256/255 and clamp, alpha not doubled (white mark 127/255),
texel alpha used as is, no discard of faint pixels, fog on colour only, no culling, depth `<=` pass/reject, depth write
off, sampler wrap/mips, state restore; when `GLOBAL/InGameCommon.bun` is found (`TM_DATA_DIR`, default
`/home/dq/Games/Need for Speed Underground 2`) the three decoded textures are checked against TM-15.5 (alpha max/mean).
Earlier list: lay_predicate 9, `vehicle_gate` 21 rows (each row checks both
`tm_venue_lays_marks(venue) == clause3` and `tm_vehicle_gate(clause1, clause2, venue) == lays`),
sample_geometry 7, strip_sequences 6 (events, strips, points,
attached flags), render_alpha 6, view_metric 8 plus K, cell_keys 5, then non-vector behaviour (surface table,
hashes, utilisation, origin range, trail ending, 128/64 capacity eviction, 40-cell eviction, vertex geometry/alpha/
culling, and the TM-2 clause-3 venue block `[4400, 4499]`). `make tire-marks-drive-test` drives the Stage5 fixture: gripping cruise lays nothing, a handbrake slide
lays strips from the locked rear wheels, `u` stays in 0..1, gate drops in alternate-mover mode,
on the reserved venue block `[4400, 4499]` (true at 4000/4001/4399/4500) and in the pre-race
start/countdown phase (`DRIVE_SESSION_COUNTDOWN` and `DRIVE_SESSION_SUSPENDED` both false; true
again when `DRIVE_SESSION_ACTIVE`).
The vectors prove spec self-consistency only; the `[inf]` claims (8 kN, 128/64 capacities, alpha unity) are
unverified against retail (REVIEW.md note).


## Not verified here

The whole in-game draw (strip streaming in `main.c`, view/camera K, draw order against cars) has not been run in a game
session. The mark pass itself (pipeline + shader + sampler) is exercised by `tire-marks-render-test`; retail fixed-point
pixel pipeline parity is limited to the +-1/255 per layer the vectors allow; retail fog parameters are not available (TM-IQ10).
