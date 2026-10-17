# Stage5 independent implementation

Approved input: SPEC.md SHA-256
`131e2c9e26091c647fb96bdb9ac0891e43c309487abe1e984b4408b711eec16e`,
reviewed components.json (77 cases), surfaces.json (29 six-channel rows) and
README. Stage1–4 companion hashes were checked against their approvals before
implementation; no specification/vector was edited. Implementer inputs were
approved shared specs/reviews/questions/vectors, this skill, and the explicitly
permitted independent src/tools/Makefile and design/documentation baseline.
No original executable, raw asset, analyst evidence, original source,
PROVENANCE/HANDOFF or verification report was inspected. Only the independent
native application's ordinary loaders used the user-authorized installation.

## Design and requirement mapping

- S5-0/1: `contact.h/.c` defines an independent finite-input component API.
  `drive_contact.c:stage5_car_read` adds only approved row fields through the
  baseline GLOBALB framing, validates them, and leaves upgrades/variants outside
  the contract. `ct_inertia` supplies both stock diagonals; `ct_change_centre`
  handles explicit changed-centre geometry. The host chooses the existing
  unscaled box yaw-inertia mode. Drivetrain wheel inertia remains `.01`.
- S5-2: `ct_frame` refreshes held mount arms/preloads; `ct_preload` uses signed
  exported speed, previous aero enable, stock centre-enabled shares and explicit
  ride offsets. The playable host supplies zero lift offsets. It does not add
  Stage2 reported load to preload or spring load.
- S5-3/4: `ct_post` queries in 0/1/2/3 order and samples each pair's currently
  stored travel. `ct_spring` clamps geometric travel, retains rates at small/zero
  h, distinguishes exact lower endpoint from clamped noncontact, and applies
  progressive spring/damping/paired coupling without presentation-reference
  subtraction. World height/normal/ID are external. Bump is sampled before
  override/steep-normal processing. Noncontact produces zero scalar/aligned load.
  `ct_bottom/ct_impulse` expose requested impulse and independent supplied vector
  caps. The playable host reports requests with application suppressed, then runs
  one zero-duration ordered sweep after any upper-limit test, without an Euler
  update. Unknown caps/rest impulses are not guessed.
- S5-5: `ct_apply` supports all four force selectors, retained noncontact point,
  full world wheel heading/lateral/up transformation, contact application point
  and full cross-product moment. Positive-load alignment is independent of a
  component contact flag; composed noncontacts have scalar zero. All contacts
  are aligned before coupling. Provisional five-pass coupling retains body-local
  planar forces and r0.x/r0.y arms; it never accumulates final normal forces.
- S5-6: separate blend, eta, grip and speed conversion helpers expose strict
  component boundaries. `DrvEnv.supplied_frame_grip` bypasses the lower-stage
  shorthand gamma when the complete Stage5 front/rear grip is supplied. Stage1
  player rear-speed factor and response scale still apply once. The composed
  brake distribution selects corrected binary32 signed-forward mph; direct S3
  fixture entry remains unchanged. Signed reverse B is neither absolutized nor
  clamped. Nonzero H uses existing Stage3 relief/lock/spin/direction/smoothing.
- S5-7: the small normative numeric mapping is encoded directly in independent
  `CtSurface` records. Peak/sliding/spin/body/bump remain independent; bump uses
  floor cells and nonnegative modular sample indices. `sample` supplies fresh
  wheel channels; frame refresh uses exported surfaces/contact flags for body
  drag. No unreviewed semantic material labels are mapped.
- S5-8: `stage5_drive_step` is the selected composition entry, separated from the
  existing lower-stage adapter. Creation performs zero-time preparation before
  the first changed pedal frame. Frame preload precedes mode refresh; shared
  exclusivity/brake distribution occurs in `drv_frame_begin` once. Handbrake/
  grip refresh once, then derivative/Euler/post/geometry/aero/coupling/final
  accumulation proceeds. No current frame input removes preceding prepared
  wheel forces before their first derivative. Contact/surface/load/speed snapshots
  export only after all substeps. Reset preserves config/providers and restores
  creation travel/rate/blend, without attempting restored-state reconstruction.
- S5-11 (refines S5-7, resolves the attribute-to-ID part of U5-2):
  `ct_surface_id` is the identity over 0…28 and returns invalid above 28
  without indexing a row; `CT_SURFACE_ID_*`/`CT_SURFACE_RECORD_*` constants and
  `ct_surface_record` encode the 29×152-byte channel layout. `UG2TrackPolygon.attribute`
  exposes the loader byte at polygon-record offset 2 unchanged, and
  `ug2_tc_surface_attribute` selects the covering polygon with the
  reachable-wheel ground rule. `stage5_world_query` derives the contact ID from
  it, keeping the fixed fallbacks (no pack/no polygon/override→1, steep→0).
  No semantic material names are introduced.

## Explicit host architecture and limits

The original full body/momentum/angular/world/material producer is unknown. This
round does not invent one. The independent OpenUG2 ride supplies heave, pitch,
roll and their rates; these are prescribed/extrapolated trajectories, with
cosmetic body_roll excluded. The host uses XY body-centre coordinates and the
fixed vertical bridge `P.z=carpos.z+centre.z-mean(mount.z-radius)`. During substeps
P.z/pitch/roll extrapolate the input rates. P.xy and yaw integrate the actual
transformed forces/moments. Its body accumulators retain full vertical forces
and pitch/roll moments for inspection/external 3D consumers, but the prescribed
ride owns playable vertical motion. Integrated vertical momentum is discarded
at the explicit constraint; it does not feed another ride force/gravity pass.
Dynamic brake/throttle pitch and full 3D load transfer therefore remain open.
No `M*a*height/wheelbase` shortcut or cosmetic-lean contact producer was added.
Prescribed pose tests are clearly distinguished from dynamic pedal causality.

`stage5_world_query` uses the existing independent reachable-wheel query, unit
triangle normals, upward .25 m host reach and downward max(stock droop, existing
host swept fall reach). Missing support supplies a valid up normal and ID1.
Every selected triangle uses the explicit ordinary numeric ID1 fixture; arbitrary
IDs remain injectable. This is not a recovered material mapping. The host selects
bottoming suppression because retail cap values are unknown. Query/reference,
impulse, vertical bridge and body choices are documented fixture policies, not
retail facts. I5-1 was added to QUESTIONS.md to target future reviewed producers.
Other supplied choices: q=1, X/event/v0/s0=0, reduction=0, scale=1, zero damage,
ordinary state, initial locks=0, relief enabled, zero extra angular damping,
no nitrous/upgrades/launch. Existing steering histories remain host box filters.

`--drivetrain` loads Stage5 for initial car and transactional car switch. Existing
reset/race-mode/pause paths keep the new configuration and clear transients.
Legacy default and drag/drift remain unchanged. Audio/HUD/wheel presentation keep
reading the composed engine/wheel state. Main no longer calls the arcade chassis
hold/velocity scaling on the Stage5 path. Collision-produced XY velocities enter
the following simulation sample without resetting wheel spin or yaw.

## Tests-first and coverage

`tools/contact_test.c` encoded all 77 reviewed cases before contact behavior was
written; the initial test build failed at undefined ct_* symbols as expected
(`/tmp/stage5-red.log`). No implementation architecture came from original code.
All component vectors and each of 29 x 6 surface parameters pass, 503 checks.
Additional `tools/drive_stage5_test.c` covers stock inertia/centre shift,
centre-disabled preload share, override-before-steep handling, independent impulse
caps/suppression, four force selectors, synthetic stock field mapping/rejection,
moving handbrake braking/release/blend, service brakes, mirrored steering,
prescribed opposite pitch/roll consequences, ordered liftoff/recontact,
first-derivative retained force with new HB/missing terrain, preload aero latency,
wheel/body surface snapshot timing, bottom extra sweep, reverse, reset/switch,
collision velocity, pause and exact 30/60/90/144/240 Hz state equality.
Accounting compares each full force/moment to once-only gravity, freshly computed
aero and the four final wheel contributions. Frame and blend/post counts are
checked against actual composed operations, not only component arithmetic.

Prior approved fixtures are retained byte-for-byte. Stage1–4 independent tests
continue to exercise their supplied boundaries; the S5 opt-in flag changes only
the composed signed brake speed and complete grip entry. No prior vector was
silently rewritten to accommodate supersession.

## Validation commands and results

Validation is recorded below after the source freeze. Native bounded drives use
normal controls/world queries and ordinary independent loaders, never original
execution or direct asset inspection. No runtime/differential parity is claimed.

Executed commands (workspace root; compiler `cc`, system C99 toolchain):

```sh
make contact-test drive-stage5-test lateral-test drive-stage4-test braking-test resistance-test drivetrain-test drive-stage1-test physics-clock-test district-collision-test > /tmp/stage5-all-tests.log 2>&1
cc -O2 -std=c99 -Wall -Wextra -Werror -Wno-unused-function -Wno-misleading-indentation -D_DEFAULT_SOURCE -Isrc tools/contact_test.c src/contact.c src/resistance.c src/braking.c -lm -o /tmp/stage5-strict-component
/tmp/stage5-strict-component
cc -O2 -std=c99 -Wall -Wextra -Werror -Wno-unused-function -Wno-misleading-indentation -D_DEFAULT_SOURCE -Isrc tools/drive_stage5_test.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c -lm -o /tmp/stage5-strict-adapter
/tmp/stage5-strict-adapter
cc -O1 -g -std=c99 -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tools/contact_test.c src/contact.c src/resistance.c src/braking.c -lm -o /tmp/stage5-asan-component
ASAN_OPTIONS=detect_leaks=1 /tmp/stage5-asan-component > /tmp/stage5-asan-component.log 2>&1
cc -O1 -g -std=c99 -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tools/drive_stage5_test.c src/drive_contact.c src/contact.c src/drive_stage1.c src/drivetrain.c src/braking.c src/lateral.c src/resistance.c src/physics.c -lm -o /tmp/stage5-asan-adapter
ASAN_OPTIONS=detect_leaks=1 /tmp/stage5-asan-adapter > /tmp/stage5-asan-adapter.log 2>&1
ASAN_OPTIONS=detect_leaks=0 /tmp/stage5-asan-component > /tmp/stage5-asan-component-noleaks.log 2>&1
ASAN_OPTIONS=detect_leaks=0 /tmp/stage5-asan-adapter > /tmp/stage5-asan-adapter-noleaks.log 2>&1
make normal > /tmp/stage5-normal.log 2>&1
make gles > /tmp/stage5-gles.log 2>&1
make debug > /tmp/stage5-debug.log 2>&1
```

Results: component Stage5 503 checks, Stage4 208, Stage3 479, Stage2 348,
Stage1 3,606, all zero failures. Stage1/4/5 adapters, physics clock and district
collision suites pass. New adapter test after the source freeze also passes its
opposite prescribed pitch/roll and real lifecycle-count assertions. Strict new
component/adapter executables pass. Existing baseline misleading-indentation and
unused static-header warnings are disabled in the strict command; no new warning
category was hidden. ASan+UBSan executes both suites successfully with leak
detection disabled, with no memory/undefined-behavior diagnostic. Both initial
`detect_leaks=1` runs failed at LeakSanitizer's environment/ptrace limitation;
therefore no leak-check success is claimed. Normal and GLES builds pass with
existing baseline warnings. Debug/native completion is recorded below.

### Final lifecycle correction and repeat

A self-audit before final delivery found that the upper-limit branch repeated
only contact geometry. S5-8 requires the established full post callback sweep.
The implementation now repeats body publication/force clearing/gravity and
`drv_post` (wheel, engine and clutch post callbacks) before the h=0 contact
sweep. It still performs no derivative or Euler integration. A new regression
sets an explicit supplied OPEN clutch state with a pending target and observes
that its target-gear transition occurs during the zero sweep. The test failed
before this correction at `zero_sweep_gear==2`
(`/tmp/stage5-zero-sweep-red.log`), then passed. No requirement/vector changed.

The full eleven-target command above was repeated successfully against this final
source, logged as `/tmp/stage5-all-tests-final.log`. Source/tests were re-frozen
at that point. Strict adapter and ASan+UBSan adapter were recompiled with the
same commands/flags above; final logs are `/tmp/stage5-strict-adapter.log` and
`/tmp/stage5-asan-adapter-final.log`. GLES/debug were rebuilt from final source,
with final logs `/tmp/stage5-gles-final.log` and `/tmp/stage5-debug-final.log`.
The earlier normal build was before this narrow post-sweep correction; final
GLES/debug compile the corrected complete application sources.

The pre-correction native logs `/tmp/stage5-native.log` (420 ticks, normal build)
and `/tmp/stage5-final.log` (900 ticks, debug build) are preliminary evidence,
not final-source proof. The latter naturally recorded 106 wall-response frames,
61.67 km/h peak, 80.43 m travelled, and zero NaN/Z/XY-out-of-bounds flags. Both
used real controls and world queries; no body state was forced. A final-source
native run and capture inspection are recorded below.

### Final-source native acceptance

```sh
DISPLAY=:0 SDL_AUDIODRIVER=dummy ./nfsu2 '/home/dq/Games/Need for Speed Underground 2' --car 240SX --drivetrain --track STREAML4RA --resolution 640x360 --drive-audit /tmp/stage5-accepted --frames 900 > /tmp/stage5-accepted.log 2>&1
```

This completed with exit 0 after the final debug rebuild. The debug executable
is left on disk. Native summary: 61.67 km/h peak, 20.16 km/h final, 80.43 m
travelled, 106 naturally occurring wall-response frames, no rail response;
NaN/Z/XY-out-of-bounds flags all zero. All 78 Stage5 telemetry rows are finite.
At tick169 signed speed is 3.21296 m/s. The moving HB pulse at170 requests rear
H=5 (front 0), blend .666667; by184 speed is 1.49768 m/s and blend .5. Release
at185 gives H=0 and blend .516667; at195 blend .683333 and speed 1.89130 m/s,
showing recovery. The preceding prepared force persists at release as specified.
Loads respond to actual road/ride geometry: at169 aligned N is
(4.66100,3.33783,3.64865,4.57950) kN; at184 it is
(4.04868,4.06427,3.58194,3.55717). Steering/service-brake phases likewise retain
finite variable travel and loads. These are prescribed-host geometry outcomes,
not evidence of recovered dynamically integrated brake-pitch.

Spawn and end captures `/tmp/stage5-accepted_spawn.png` and
`/tmp/stage5-accepted_end.png` were viewed: the car, wheels and world render
coherently; the ending car is beside the wall naturally encountered by the
script. No body velocity/load/contact was forced to manufacture acceptance.
Logs/captures show independent OpenUG2 output only. No original executable was
launched, nor were original raw data files inspected directly.

Final source fingerprints: `drive_contact.c`
`4a4a3639846affff7521f5f31b62352338a68b705f362ddff80a179630788dd9`;
`drive_stage5_test.c`
`40a2c171b2f3e915e988a431f9c707a2ff714711adfd5ea52f4935799da4e51d`.
Approved Stage1–5 SPEC hashes and all Stage5 vector/README hashes remain unchanged.
The only appended shared question is I5-1; PROVENANCE/HANDOFF/report artifacts
belong to the coordinator/verifier and were not edited or read by this role.
No stage/commit/push was performed.

## Intermittent 240SX braking investigation and manual trace — 2026-10-15

The reported intermittent wheelspin/standstill or bog after service braking
was not reproduced as an isolated implementation defect. No tyre, clutch,
brake-lock, suspension, collision or force-lifecycle equation was changed.
S3-4 expressly retains locks when its motion predicate is false; pedal release
alone cannot justify an unconditional unlock. S5-0 excludes complete rest/stuck
handling, and the existing unknowns remain open. No shared requirement or vector
was changed and no new normative answer was inferred from these experiments.

Independent diagnostic design: `src/drive_trace.h` writes only supplied const
state to a caller-owned stream. The opt-in `--drivetrain-trace FILE` selects the
same drivetrain as `--drivetrain` and records each actual simulation tick after
world and vehicle collision responses. It never produces driver inputs or
advances physics. `active` identifies legacy race modes. The CLI opens/closes
the stream and flushes every 60 ticks. The trace does not read original assets;
the native application's existing loaders remain the only runtime data path.

The recorded distinctions follow S1-2/S3-7/S5-8: incoming prepared force,
final next-prepared force, pre-host body velocity, final host velocity, and
wheel/contact snapshots remain separate. S3-4/5 lock/torque/spin, S5-3/4 travel
and aligned loads, S5-5 final wheel forces, and external collision observations
allow classification without changing their contracts. `ride_*`/pitch/roll are
the host's updated pose after its ride step; contact forces belong to the
preceding drivetrain preparation. Ordinary UI and default simulation behavior
are unchanged. Usage and units are in `docs/DRIVETRAIN.md`.

Tests-first: `tools/drive_trace_test.c` initially failed to compile at the absent
`drive_trace.h` (`/tmp/drive_trace_red.log`). Its completed test verifies matching
CSV schemas, representative values distinguishing host/body velocity and old/new
forces, zero-load contacting wheels, lock/spin/torque/blocker columns, and bytewise
immutability of both input structures. `make drive-trace-test` passes. Strict
C99 compilation passes with the baseline static parser's unused-function and
misleading-indentation warnings disabled. ASan+UBSan passes with leak detection
disabled. The trace test also compiles with `aarch64-linux-gnu-gcc`; no ARM64
runtime/emulator was available, so this is a build check only.

The existing component/adapter/clock/collision targets all pass in
`/tmp/stationary-tests.log`: contact, drive-stage5, lateral, drive-stage4,
braking, resistance, drivetrain, drive-stage1, physics-clock, district-collision,
plus the new trace test. Approved SPEC hashes remain unchanged.

Reproduction limits: a 900-case synthetic sweep of drive share, ordinary/strong
service brakes and 1–150 braking ticks recovered. Additional supported synthetic
ride offsets, recontact and 0–12% plane grades recovered; the grade experiment
used approved Stage1 240SX parameters with invented suspension geometry, not a
claim of complete stock-car tuning. Native 240SX default-heading service brake,
coast and relaunch runs recovered. A native uphill-heading variant displayed
800 RPM and roughly 0.8–1.3 m/s during part of full-throttle recovery, but also
encountered repeated wall responses (100 by tick600); it does not isolate the
reported persistent stall and is not evidence for a tyre or clutch fix.

Final native trace smoke uses normal loaders and `--drive-audit` only as its
control producer (`/tmp/stationary-trace-smoke.log/.csv`): 420 unique monotonic
ticks, 133 columns per row, all finite, service/HB/release recorded. W still
requests service brake for two ticks while reversing at the release transition,
then service coefficients clear; this is the existing keyboard policy. A
separate no-audit `--shot --perf-still` smoke checks that enabling capture does
not inject acceleration. No successful physical keyboard injection is claimed:
the available xdotool could not access a display. Manual-user capture remains
necessary to classify the intermittent occurrence. No commit or push performed.

## Host direction handoff accelerator hold — 2026-10-15

This is independent host keyboard policy at U3-1/U5-4, where original device and
reverse mapping are outside the approved contract. The five SPEC.md hashes were
checked against their current REVIEW.md approvals before implementation and
remain unchanged. No new original-behavior requirement or vector is inferred.

Root cause in the Stage5 adapter: the near-rest direction latch changed before
the actual gearbox completed its clutch transition. The adapter then supplied
the new W/S accelerator demand through the still-opposite actual gear. An
Engaging clutch could remain powered while its target was already the other
direction. The independently generated pre-fix `/tmp/stationary-repeat.csv`
contains 25 W/actual-reverse/nonzero-throttle rows; tick310 has pedal1,
displayed gear-1, displayed target1, mode2, engagement .3875 and throttle1.
Trace/HUD gear indices are R=-1, N=0 and first=1; these differ from the core's
R=0, N=1 and first=2. An initial temporary diagnostic query used the core index
for the CSV and was corrected; the baseline capture was not modified.

`src/drive_contact.c` now withholds the raw accelerator when either the actual
gear or the still-pending target opposes the latched requested direction.
Checking the target also covers an interrupted handoff whose gear changes
inside the tick. The conservative hold may last until the following 60 Hz tick.
Service braking uses the existing signed-motion snapshot beyond the +/-0.3 m/s
handoff window, including a latch left by an interrupted transition/collision.
The selector still receives automatic-forward or direct-reverse command.

S1-5 pedal exclusivity, dead zone and anti-stall shaping still run exactly once
per frame in `drv_frame_begin`; shaped throttle stays held throughout its
substeps. S2 frame resistance inputs, S4 steering and S5 grip/brake consumers
therefore see coherent frame inputs. S1-4 clutch/selector equations, S1-3 engine,
S1-7 tyre coupling, S3 lock handling and S5 contacts are unchanged. Following
S1-2/S3-7/S5-8, previously prepared forces and stored engine/wheel energy remain;
the policy prevents fresh opposite-direction accelerator demand, without
promising instantaneous removal of all opposite-directed force or motion.
Lower Stage1–4 supplied-input fixtures and default/alternate legacy models are
unchanged.

Tests were written first. `/tmp/direction-handoff-red.log` records the existing
code failing the opposite-actual-gear throttle assertion during a substep query.
`tools/drive_stage5_test.c` now checks both directions in all four clutch modes,
actual/target disagreement within a tick, rapid alternating W/S followed by a
sustained launch, ordinary neutral launch, normal partial throttle and the
forward anti-stall floor, motion-opposing brakes with a mismatched latch, finite
recovery and preservation of the incoming prepared-force derivative. Its final
123,220 substep observations pass. These are invented host regression states,
not additions to approved original-behavior vectors.

Validation: contact, drive-stage5, lateral, drive-stage4, braking, resistance,
drivetrain, drive-stage1, physics-clock, district-collision and drive-trace
targets pass (`/tmp/direction-handoff-tests.log`, plus final Stage5 output in
`/tmp/direction-handoff-final-test.log`). Strict C99 `-Wall -Wextra -Werror`
passes with the baseline parser's unused-function and misleading-indentation
warnings suppressed. Final Stage5 ASan+UBSan passes with leak detection disabled.
ARM64 cross-compilation passes; no ARM64 runtime/emulator was available, so no
ARM64 runtime result is claimed. GLES and debug builds pass, and the final
debug executable is left at `nfsu2`.

Final native check used normal stock loaders, existing input-only `--drive-audit`
and observational `--drivetrain-trace`:

```sh
DISPLAY=:0 SDL_AUDIODRIVER=dummy ./nfsu2 '/home/dq/Games/Need for Speed Underground 2' --car 240SX --drivetrain --track STREAML4RA --resolution 640x360 --drive-audit /tmp/direction-handoff-native --drivetrain-trace /tmp/direction-handoff-native.csv --frames 900
```

Exit0; 900 unique ticks, 133 columns each, all finite. At ticks346–347 W is
held with displayed actual R=-1, target first=1 and throttle0. First is selected
at348; throttle resumes at349; signed speed becomes positive at351 and reaches
4.8886 m/s at420 with no wall response through this recovery. No row supplies
nonzero throttle in an actual gear opposite the latched requested direction.
Later wall responses occur; the viewed ending capture shows the car beside a
wall. This does not reproduce or resolve every persistent stall, or establish
original-runtime parity. Physical keyboard injection was unavailable; the
existing script supplies inputs only. No original runtime, raw asset analysis,
analyst material or verification report was accessed. No shared questions or
vectors were edited, and no commit/push was performed.

Frozen implementation SHA-256: `drive_contact.c`
`7fb35d1f09fdcab2ed00e70d021072695397a841dc08c10f0f5355a7dd3c316d`;
`drive_stage5_test.c`
`c64f58bd2938b969bb3930799980c18dc26f0fa981c24ddf2338f780d88bf993`.

## S5-11 world surface attribute to surface ID — 2026-10-17

Approved input verification for this increment: SPEC.md SHA-256
`2723c32c63f54a04ce8ffe218386ff3b0c898aff2e9f534f685d182b7af4211c` matches
REVIEW.md (the earlier header hash records the prior revision). Companion
hashes checked before implementation: `test-vectors/surface_mapping.json`
`595b537920d2943f0f098c1f0fc534e906c37b8ff2853e139f2846605ece002b`,
`test-vectors/README.md`
`cfff89f16c0b48257592ffdcdace7a87845b420e6a11975ef7f8350437b609a4`,
`QUESTIONS.md`
`d4dc33e26ee54ccba02774cf0131b97988fd48b1e37b031bffcda042102f57f8`,
`test-vectors/surfaces.json`
`eaff75a3b963042083034016561809f5d719ed6f655d10ff1660bfc2be2b446c`.
No specification/vector/review was edited; no original executable, raw asset,
analyst evidence, original source, PROVENANCE/HANDOFF or verification report
was inspected.

Requirement trace and decisions:

- S5-11.1/.3 (identity): `ct_surface_id` returns the attribute widened
  unchanged for 0…28; there is no lookup, offset, clamp or name table.
  Vector rows in `surface_mapping.json` are checked directly.
- S5-11.2 (domain): `CT_SURFACE_ID_MIN/MAX/COUNT` are 0/28/29;
  `ct_surface_id` returns `CT_SURFACE_ID_INVALID` (-1) above 28 so no caller
  can index an undefined row. The host keeps its fixed default ID for that
  residual case (U5-2) instead of guessing a row.
- S5-11.4 (no transform at load/query): `UG2TrackPolygon` gained an explicit
  `attribute` field loaded from byte 2 of the 32-byte record
  (`UG2_TC_POLYGON_SIZE`/`UG2_TC_ATTRIBUTE_OFFSET`); the loader stores the byte
  verbatim and the selector returns it verbatim. The metadata byte view is
  unchanged. `tools/ug2_track_collision_test.c` asserts the fixture byte 0x34
  survives loading.
- S5-11 selection rule: `ug2_tc_surface_attribute` (header-only static beside
  the loader) picks the polygon/triangle whose face covers the query point and
  is nearest to the wheel base inside the same reach window used by the world
  ground query, rejecting near-vertical faces and preferring the rising-ramp
  candidate. This mirrors the reachable-wheel support rule already used by the
  independent host so the ID comes from the polygon that supplies support.
  No engine search internals are claimed; validity of the height search stays
  U5-2 residual.
- S5-11.5 (fallbacks): `CT_SURFACE_ID_DEFAULT` (1) is used for no collision
  pack, no selected polygon, and the forced override
  (`ct_ground_distance`); `CT_SURFACE_ID_STEEP` (0) is used on the failed
  steep-normal test (`ct_distance`). These are fixed constants and do not
  consult geometry.
- S5-11.7 (record layout): `CT_SURFACE_RECORD_COUNT/SIZE` are 29/152 and
  `ct_surface_record` decodes the six little-endian binary32 channels at
  0x18/0x1C/0x24/0x28/0x2C/0x30 into `CtSurface`; the test decodes synthetic
  records and compares against `surfaces.json` per row.
- Host wiring: `stage5_world_query` now receives a `Stage5WorldSource`
  (scene + collision pointer). The collision pointer addresses
  `world.neighborhood.track_collision`, whose address is stable across
  resident/track switches, so the ID follows the currently loaded pack. When
  support is found the query maps the selected polygon attribute through
  `ct_surface_id`; otherwise the result stays the fixed default 1.
- Tests-first: `tools/surface_mapping_test.c` was added and run red before the
  implementation, then green. It covers the identity table, domain bounds,
  fallback behavior through `ct_ground_distance`/`ct_distance`, the
  boundary/representative vectors (including out-of-domain null), selector
  cases (covering/nearest/reach window/attribute 0/attribute 29/near-vertical
  face/empty/null set), a composed contact-carries-ID check, and the 29×152
  channel layout decode.
- Validation (all after the change): `make surface-mapping-test` 275 checks,
  0 failures; `make ug2-track-collision-test` PASS (with new attribute
  assertions); `make world-collision-test`, `make ground-motion-test`,
  `make district-collision-test` PASS; `make car-material-test` 389 passed,
  0 failed; `make contact-test` 503 checks, 0 failures plus the direction
  handoff and adapter stages; `make drive-stage5-test` PASS; `make debug`
  exit 0.

Frozen implementation SHA-256: `contact.c`
`ba87a8e1fb3c89886b4b6c7b7bd5349759434f2fc080ca81908ce017cb6220ef`;
`contact.h`
`df69f4bac0c533e7672d00151e55352113885edd0397184acc0ebee2acaa6512`;
`ug2_track_collision.h`
`f3b7395f39e71cf857752888d0fe9fd9be47a3be68aa3bf350d45059c60cb7ca`;
`main.c`
`a50e2b0a1b45ea0a8542f3ea8bd1edd1b7ea4165836288d6882ab94902edecf3`;
`surface_mapping_test.c`
`24bc05d5b92b9a63e94c3829c1247a080144984aefe8a66cb96341345c6a669f`;
`ug2_track_collision_test.c`
`74a00f0a614da0d727b1a3512c4a867e4767f61b8db611a8228c38a3080e488f`;
`Makefile`
`cc8ed92905c883c0274af84e389c63c3d73328f534e76bb1d752a2d4cf5d1f1f`.

## S5-11 lookup acceleration (performance-only) — 2026-10-17

Approved input verification for this increment: SPEC.md SHA-256
`2723c32c63f54a04ce8ffe218386ff3b0c898aff2e9f534f685d182b7af4211c` still
matches REVIEW.md; `test-vectors/surface_mapping.json` is unchanged. No
specification, vector, review or verification file was edited. No original
executable, raw asset, analyst evidence, original source, PROVENANCE/HANDOFF or
verification report was inspected. The S5-11 selector's returned byte is
unchanged; this increment only replaces its per-query linear scan.

Requirement trace and decisions:

- S5-11.1/.4 (identity, no transform): `UG2TrackPolygon.attribute` is still the
  byte read at record offset 2 and is still returned verbatim. The index stores
  only polygon indices; no attribute copy, remap or clamp is introduced.
- S5-11 selection rule (same rule, smaller candidate set): every polygon is
  inserted into a uniform XY CSR grid
  (`UG2TrackIndex` in `src/ug2_track_collision.h`) once in
  `ug2_track_collision_index_build`, called by `ug2_track_collision_read` before
  publication. A query visits only the cell containing `(x,y)`
  (`ug2_tc_index_coord`, identical monotone mapping at build and query). Cell
  size is `UG2_TC_INDEX_CELL` (32.0f), chosen well below the world grid's 64 m
  so collision cells stay small; memory is `cells+1` ints plus one int per
  polygon-cell reference, like `WGroundGrid`.
- Exactness: the selector accepts a covering triangle only when all barycentric
  coordinates are >= -0.01. Any such point lies within `2*0.01*diam` of the
  triangle (centroid dilation), so a polygon is inserted into every cell its
  raw XY bounds dilated by `UG2_TC_INDEX_PAD_REL*diam + UG2_TC_INDEX_PAD_ABS`
  (0.03*diam + 0.05 m) overlap. The extra relative margin over 0.02 covers the
  binary32 rounding of u,v,w (bounded by ~eps*|coordinate|, sub-millimetre for
  the loaded +-4096 m range). Build and query share the same clamped coordinate
  mapping, so a point inside a polygon's dilated range resolves to a cell that
  contains the polygon even outside the grid extents. Per-cell lists are in
  ascending polygon index order and both dispatch paths keep the first polygon
  on equal keys (`key >= best_key` skips), so ties are resolved identically.
- Fallbacks: an unindexed owner (hand-built test data) or a non-finite query
  point keeps the original full scan, byte-identical. An index allocation
  failure fails `ug2_track_collision_read` (which retains the previous owner),
  matching the loader's existing allocation-failure policy instead of silently
  regressing to per-query scans; the failed build leaves no index behind.
- Lifecycle: `ug2_track_collision_free` frees `polygons`, `index.start` and
  `index.list`; `ug2_track_collision_read` frees the owner it replaces
  (including its index) as before. The shallow `World` facade copy in
  `world_resident.c` only round-trips the neighborhood, as it already did for
  `polygons`/`mbb`, so no ownership change.
- Host wiring is unchanged: `stage5_world_query` still calls
  `ug2_tc_surface_attribute` with the same arguments and the same
  `ct_surface_id` domain step; `src/main.c`, `src/world.c` and
  `src/contact.{c,h}` are untouched.

Tests-first and validation:

- Added `tools/surface_index_test.c` (`make surface-index-test`): an independent
  brute-force reference of the same rule is compared against the indexed lookup
  and the unindexed fallback over empty/null sets, single covers, reach-window
  edges, points outside the bounds and within the barycentric tolerance, stacked
  and overlapping ties, near-vertical/steep faces, cell-boundary and
  negative-coordinate points, sliver/degenerate lattice geometry at the shipped
  +-4096 m range, arbitrary-quad queries synthesized inside the +-0.02
  barycentric band, and six LCG fuzz sets. Result: 34,934 checks, 0 failures
  (7,924 reference hits, 26,987 misses). Timing on 100,000 synthetic polygons,
  200 queries: indexed 0.0002 s vs brute force 0.299 s (~1,700x).
- Test sensitivity was confirmed by temporarily zeroing both pads in a scratch
  copy: the suite then fails 18 equivalence checks (sliver/tolerance-band), so
  the dilation is load-bearing and covered.
- `tools/ug2_track_collision_test.c` now also asserts that a successful read
  publishes a grid (`index.start/list`, `gw/gh` > 0) and that empty/empty-bound
  reads publish none; PASS.
- Re-ran on the final tree: `make surface-mapping-test` 275 checks, 0 failures;
  `make ug2-track-collision-test` PASS; `make world-collision-test` PASS;
  `make ground-motion-test` PASS; `make district-collision-test` PASS;
  `make contact-test` 503 checks, 0 failures; `make car-material-test` 389
  passed, 0 failed; `make drive-stage5-test` PASS; `make debug` exit 0.
- Sanitizer check: `tools/surface_index_test.c` and
  `tools/ug2_track_collision_test.c` rebuilt with ASan+UBSan
  (`-fno-sanitize-recover=all`) run clean; the loader test shows no leaks.

Frozen implementation SHA-256: `ug2_track_collision.h`
`68e34cab793e516a07ca203bef7608fff64dcf7800800009a0372eb558b98bb6`;
`surface_index_test.c`
`de08e7ba3a2700c4ea6917c1939f65e824d4f5a6353a5543d4cf0ca712590963`;
`ug2_track_collision_test.c`
`7272c7e182a2cf384b47906b5a39eec1b7ba6f65d3291d89175951a08e377da8`;
`Makefile`
`795a28448b99a973281b9fcb15f3cd7749ef2b24d5b14c32dd60f9c771765c8a`.

Post-report verification (same tree, no source changes): the facade review is
closed — `world_bind_textures_step` frees only `rgn[].data`/`commontpk.blk`/
`common`, so the shallow `World` copy in `world_resident.c` cannot affect the
collision index; `make normal` (release objects) exits 0; `make
world-resident-test` PASS (its nfsu2-less fixture messages are the expected
missing-data path); `make debug` still exits 0.
