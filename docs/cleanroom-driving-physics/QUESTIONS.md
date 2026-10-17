# Implementer questions: driving physics Stage 1

Raised by the implementer while building `src/drivetrain.c` against SPEC.md
(sha256 620bfb86...e73311). Each item says what was done in the meantime. Nothing
here was resolved by looking at anything outside the spec and its test vectors.

## Historical questions: vector/text ambiguity at the earlier approved hash

* **Q-1 (S1-7.2 step 3, bias rule).** Read literally, the two statements "if `f < p` then `f = 1 - 0.175`;
  if `f > 1 - p` then `f = 0.175`" run one after the other, so the first result (0.825) would immediately
  trip the second test when `p = 0.35` (0.825 > 0.65) and become 0.175. The vector row `20,10,1,0` expects
  `share_b = 0.825`, so the second test must be an else-branch. Implemented as `else if`. Spec should say so.
* **Q-2 (S1-4.2, `D[j+1]` arithmetic).** `U[j]*r[j+1]/r[j]` is ambiguous about grouping. In binary32 the
  vectors (`landing_table_D`, exact binary32) match `U[j] * (r[j+1] / r[j])` and not `(U[j]*r[j+1]) / r[j]`
  (they differ in the last bit for 240SX, SKYLINE, SYNTH_RWD). Implemented as the former, in float.
  Spec should state the grouping.

## Historical questions: interim behavior at the earlier approved hash

* **Q-3 (S1-7.4, "and the wheel moves").** What defines "the wheel moves"? Implemented as the wheel's direction state
  (S1-8.1) not being "stopped" (`|omega| > 1e-5`). Alternatives: body speed nonzero, or `vx != 0`. Affects the
  first substeps of a standing start and wheels being dragged while locked.
* **Q-4 (S1-7.5, order of the lock correction).** "In each pass, ... Then, only if M = 3, apply the lock
  correction." Implemented as: inside every one of the 5 passes, after the force update. If it was meant once after
  all passes, the result differs. Also not stated: whether `alpha_w` uses `tau_i - R_i F_i` with the forces updated by
  this pass (implemented: yes).
* **Q-5 (S1-4.5, first-gear down-shift target).** For `g = 2` (first gear) with `t < 0.2` and `rpm <= eps0 + 100`
  the rule gives `g* = g - 1` = neutral. With the automatic rule "if `g <= N`, `g* = 2`" the next frame, this makes a car
  at a standstill with the throttle off hunt between first and neutral (clutch Disengaging/Open/Engaging cycles).
  Implemented literally. Is that retail behaviour or does something else (e.g. the creep logic, a throttle
  threshold) prevent it?
* **Q-6 (S1-4.5, manual-assist).** (a) "when `rpm > eps2 - 300` in a gear above the last one": no gear exists above
  the last; the sub-rule is **not implemented**. (b) When `gs < g*` and `g <= 2` there is no lower forward gear (spec
  would compute `r[g]/r[g-1]` with `r[1] = 0`): implemented as "no change". (c) When `gs > g*` the spec says
  `g* = g + 1`: implemented with an extra guard `g < n + 1`. (d) When the 10 s "without change" timer counts and
  what resets it: implemented as "frame time accumulated while `gs == g*`; reset when `gs` changes or after reporting".
  (e) Spec U-2 already lists the return path as unknown; `report_auto` is exposed as an output flag only.
* **Q-7 (S1-7.1 / S1-7.7 slip-lock, which `omega_w,old`).** For AWD the "wheel-side speed" `omega_w` is the average of
  both axles, so `delta_omega = (omega_e - omega_w,old) / (r[g] F_axle)` is applied to both axles using that combined
  value. Implemented as written (combined value). Also, "neither wheel of that axle touches the ground" is read from the
  `contact` flags. There is no vector for this branch.
* **Q-8 (S1-7.7 first-gear rule vs. conditional).** The text "In any clutch mode, if g = 2" is implemented as independent
  of the `g != N, M = 3, nc > 0` condition above it. Because the performance class `C` is unknown (U-11), `kappa_C` defaults
  to 0 (`DrvTune.perf_class = 0`), which makes the whole rule a no-op by default.
* **Q-9 (S1-7.6, which force is stored and fed to the wheel).** After the friction limit the stored `F_i` is the
  returned force (including the 1.5 factor when scaling happened). The wheel torque term `tau_i - R_i F_i` in
  S1-8.1 and the body use that same final value. Not stated explicitly.
* **Q-10 (S1-8.1, `phi` wrap range).** "wrap `phi_i` into range": implemented as `[0, 2*pi)`. No effect on dynamics.
* **Q-11 (S1-7.8, rear yaw term).** The rear handbrake/yaw term "= `1 + |forward-axis vertical component|` when driving
  straight" is taken as an input (`DrvEnv.fwd_axis_vertical`, default 0 so the term is 1). The conditions under which it
  applies other than "straight" (handbrake? speed?) are not stated; applied to rear wheels always.
* **Q-12 (S1-7.3 / S1-7.1, when `omega_w` is sampled).** The clutch post-step computes `omega_w`; the coupling later in the
  same substep re-computes it from the wheel speeds (gear may have changed in an Open to Engaging step). Spec does not say
  whether the coupling reads the post-step value. Only matters for one substep after a gear swap, when `c = 0`.
* **Q-13 (S1-2.4 / S1-3.5, `Phi` and coupling at creation).** The caller must run `drv_couple()` once after
  `drv_init()` (S1-2.5); the first derivative sees `Phi = 0`. Fine for the free-rev vectors; confirm intended.
* **Q-14 (S1-7.4, lateral gripping force).** The lateral curve is Stage 4. The gripping-branch lateral force is an input
  (`DrvWheelEnv.lateral_grip_force`, default 0) multiplied by `wT`.
* **Q-15 (S1-8.2, planar body).** `Iz` and wheel arms are not in Stage 1. `drv_body_accel_planar()` takes `1/Iz`,
  arms and unit vectors from the caller; the moment sign convention (`x*cy - y*cx`) is the standard one implied by the
  `A_i` expression, not stated.
* **Q-16 (S1-2.1 frame quantiser).** `drv_quantize_frame_dt()` rounds to nearest 1/1800 s and applies optional
  clamp bounds that are 0 (off) by default (U-5).

## Historical parameter boundaries at the earlier approved hash

`G` (U-7), bonus flag (U-6), `q` and `X` (U-4), performance class `C` (U-11), jitter stream (U-3, default suppressed =
offsets 0), initial engine speed (U-15, default idle), shift-decision rpm override (U-10, argument of `drv_frame_begin`),
frame-time bounds (U-5): all are explicit fields of `DrvTune` / arguments and documented in `src/drivetrain.h`.


## Reviewed disposition — 2026-10-15

The preceding entries are retained as the question record; their interim choices are not approvals.
Answers amend SPEC.md; the current reviewer hash gate is recorded in REVIEW.md. All new executable findings are static
inferences; none is runtime-validated.

| Questions | Status | Specification answer |
|---|---|---|
| Q-1 | Resolved | S1-7.2: mutually exclusive conditions |
| Q-2 | Partial | S1-4.2: ratio first; binary32 intermediate rounding remains U-14 |
| Q-3 | Resolved | S1-7.4: either contact-frame velocity component nonzero |
| Q-4 | Resolved | S1-7.5: correction every pass, using updated forces |
| Q-5 | Resolved | S1-4.5: second threshold 0 prevents positive-idle down-shift |
| Q-6 | Resolved for valid Stage-1 inputs | S1-4.5: remembered gear and timer clarified; U-2 menu mapping and initial assist state remain |
| Q-7 | Resolved | S1-7.7: one combined snapshot for both axles |
| Q-8 | Resolved | S1-7.7: independent condition; class source remains U-11 |
| Q-9 | Resolved | S1-7.6: final force shared by body and wheel derivative |
| Q-10 | Resolved | S1-8.1: large-range one-step angle adjustment |
| Q-11 | Partial | S1-11: full rear multiplier is input until mode/blend context is resolved |
| Q-12 | Resolved | S1-7.3: cached clutch-post-step wheel-side speed |
| Q-13 | Resolved | S1-2.5: creation post-processing; feedback 0 and mc=0.7 in neutral |
| Q-14 | Resolved as Stage-1 boundary | S1-7.4: already-signed lateral force supplied; curve deferred |
| Q-15 | Resolved as Stage-1 boundary | S1-8.2: positive moment x*cy-y*cx; parameters supplied |
| Q-16 | Partial | U-5/S1-11: residual clock and clamp unresolved end to end |

New derived regression examples: `test-vectors/s1_question_cases.json`; provenance and tolerances
in `test-vectors/README.md`. Runtime differential verification remains outstanding (U-16).

## Addendum — 2026-10-17

Q-8's `κC` source is now specified. The performance class `C` is defined in SPEC **S1-12**: a drive
score `S` integrated over the composed acceleration-versus-speed curve, classified by strict `>`
against `{273, 401, 500}` (0→3 scale), with a per-row forced-3 flag and a per-instance signed-byte
storage (`−1` = unset). `κC = K[C]` with the fixed table `K = (0, −0.2, −0.4, −0.7)`. U-11 is narrowed
accordingly and no longer blocks Q-8.

Residual (kept in U-11): whether a mid-session upgrade recomputes a live instance's class (versus only
on reconstruction/load); the out-of-scope construction of the drive-acceleration curve `a(v)` that the
score integrates; and no per-record upgraded class values beyond the torque-scale sweep that crosses
all four thresholds. Vectors: `test-vectors/s1_perf_class.csv`. This addendum is a static/derived finding
(data reading plus deterministic emulation of the parameter-composition step), not a runtime validation
(U-16). No new implementer question is raised.

## Implementer addendum — 2026-10-17 (performance class)

Raised while implementing S1-12 / the S1-7.7 `κC` clause against the approved hash
`355d67b46fd713578d17800338469cecb450169ad8e746b1199dd5564de803d4`.

* **Q-17 (S1-12 / S1-7.7, `κC` when `C = −1`).** S1-12 defines `C` as a signed byte whose
  `−1` value means "not yet computed", and S1-7.7 defines `κC = K[C]` only for
  `C ∈ {0,1,2,3}`. The specification does not state the first-gear term's coefficient when
  the drivetrain block's creation-time copy is `−1` (parameter-composition step not run, or
  car type unset). Implemented in the meantime as `drv_kappa_c()` returning `0.0` for any
  class outside `0..3`, which leaves the term a no-op — the same behaviour the module had
  while U-11 was open. Question: is `−1` unreachable in the drivetrain block in retail (the
  copy always holds a computed class by the time the drivetrain couples), or does the term
  have defined behaviour with an uncomputed class?

## Analyst answer — 2026-10-17 (Q-17 / A-17.1)

Static analysis of the retail executable (never launched) resolves the coefficient but not the
reachability. The first-gear clause loads the drivetrain block's class copy and multiplies by a plain
base-plus-4·C read of the four contiguous `K` entries with **no bounds test, clamp or default**. For
`C = −1` the 32-bit address wraps to the 4-byte slot immediately preceding the `K` table, whose binary32
value is **`0.25`** (`0x3E800000`) — a constant also used elsewhere in the vehicle physics, not padding.
So the term is **not** a no-op at `−1`; it would compute `Φ ← Φ + Ψ(Te+Φ)·0.25·(Te+Φ)`.

The drivetrain block's own byte is initialised to the constructor default `3` and then overwritten
unconditionally at vehicle creation with the sign-extended class byte of the physics-parameter object; that
object starts at the `−1` sentinel and is only written by the composition step, which does nothing when the
car type is unset. So the copy can be `−1`, and it is not refreshed afterwards.

What is unresolved: whether a drivetrain in that state is ever *simulated* as far as the first-gear clause.
The clause and its caller gate only on gear and the drag condition, not on class or car type, but the normal
spawn path resolves the class immediately before the copy is taken, so the unset state at creation is
expected to be transient/abnormal. Recorded as residual **U-17** in SPEC.

Disposition of A-17.1: **reject option (A)** ratifying `κC = 0.0` as a retail claim (observed retail value
is `0.25`); **adopt (B)** in part — SPEC S1-12.5 now states the `−1` coefficient as `0.25` and U-17 records
the open reachability. The interim `0.0` stub should be changed to `0.25` for `C = −1`.

## Analyst addendum — 2026-10-17 (Q-17 representation; A-17.2 / A-17.3)

Follow-up to the Q-17 answer and the round-2 verifier amendments. The `K` coefficients are stored as four
**binary32** words; the exact encodings are `0x00000000`, `0xBE4CCCCD`, `0xBECCCCCD`, `0xBF333333` for
`C = 0..3`, with stored values `0`, `−0.20000000298023224`, `−0.4000000059604645`, `−0.699999988079071`.
The shorthand `(0, −0.2, −0.4, −0.7)` denotes these stored values, not the real decimal literals (they
differ by ≈1.5e−8 relative). The S1-7.7 product reads the stored binary32 entries at a wider (default
double) working precision and stores `Φ` back as binary32, so the coefficient is exact but the term result
carries the U-14 tolerance. `test-vectors/s1_kappa_c.csv` now carries the stored values and bit patterns.

For `K`, only the sentinel `C = −1` (`0.25`, `0x3E800000`) and `C = 0..3` are defined; the class scheme
produces no other index and the specification does not require reproducing an out-of-range read (A-17.3).
This refines the Q-17 disposition: beside changing the `−1` stub to `0.25`, an implementation that stores
the `C = 1..3` coefficients as decimal `double` literals diverges from the specification's stored binary32
values and should be corrected. Whether a simulated drivetrain reaches the clause with `−1` remains U-17.
