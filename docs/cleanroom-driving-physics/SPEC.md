# Driving physics — clean-room behavioural specification

Subject: retail PC `speed2.exe` (sha256 `f9dd86c0…24168a7`, see PROVENANCE.md) and its retail data.
This document covers **Stage 1: longitudinal acceleration under throttle**. Later stages append sections.

Confidence tags: **[obs]** observed in runtime or directly in data files;
**[inf]** inferred from static executable analysis or reasoning; **[gss]** guessed (none used normatively; any
guess is labelled and must not be implemented as fact). Statements without a tag inherit the tag of their
section heading.

Evidence basis for Stage 1: static analysis of the executable plus direct reading of the retail data files.
No runtime capture of the retail game was used (a headless run was attempted and then stopped; no data from
it is used). All per-tick vectors are therefore **derived** from the statements below, not captured.

---

## S1-0 Conventions

1. Units [inf]: mass in **tonnes**, force in **kN**, torque in **kN·m**, length in **m**, time in **s**,
   angular speed in **rad/s**, rotational inertia in **t·m²**. These are mutually consistent
   (kN / t = m/s²; kN·m / t·m² = rad/s²) and every formula below is consistent with them; the
   choice of tonne/kN is inferred from typical values (mass ≈ 1.0–1.7, peak torque ≈ 0.13–0.5).
2. RPM conversion [inf]: `rpm = ω · Kr` with `Kr = 9.549304` (binary32 `0x4118C9F3`), and
   `ω = rpm · Kw` with `Kw = 0.10471967` (binary32 `0x3DD67744`). Use these exact binary32 constants.
3. Storage [inf]: every persistent state/parameter is binary32. Intermediate arithmetic in the original runs
   at higher than binary32 precision; bit-exact reproduction of intermediate rounding is **not** required by this stage (see
   Unknowns U-14); tolerances are given with the vectors.
4. Gear index [inf]: `g = 0` reverse, `g = 1` neutral (N), `g = 2 … n+1` forward gears 1 … n (n = gear count).
5. Wheel index [inf]: wheels 0,1 are the front axle (positive x), wheels 2,3 the rear axle (negative x).
   Wheels 0 and 3 lie on the +y side, 1 and 2 on the −y side. Which side is "left" is not needed in Stage 1.
6. `clamp(x, lo, hi)`, `min`, `max` have their usual meanings. "Linear interpolation" between `a` and `b`
   with fraction `f` is `(1−f)·a + f·b`.

---

## S1-1 Parameter sources (car record)

### S1-1.1 Location [inf]
Per-car tuning lives in the 2,192-byte (0x890) car records of the table with chunk id `0x00034600` in the decompressed
`GLOBAL/GlobalB.lzc` (the retail decompressed image is the 8,008,064-byte `GLOBAL/GLOBALB.BUN`; see
docs/FORMATS.md "Global vehicle attributes" for framing). The physics uses the sub-record that starts at
**record offset +0x110**; this spec gives offsets relative to the record start ("row+") so they match
FORMATS.md.

Note [inf]: the local install's `GlobalB.lzc` is a modified, uncompressed file (adds a 47th row, and differs
from retail in record bytes +0x3E0…+0x5AE of every row). All Stage 1 fields below lie outside that range
and are identical to retail.

### S1-1.2 Fields used in Stage 1 [inf]

| row offset | type | symbol | meaning |
|---:|---|---|---|
| +0x220 | f32 | `m` | vehicle mass (t) |
| +0x120 + 0x30·i, field +0x10 | f32 | `R_i` | tyre rolling radius of wheel i (m) |
| +0x120 + 0x30·i, field +0x18 | u32 | — | wheel's own index 0…3 (used to tell rear wheels: index > 1) |
| +0x1E0 + 0x20·a, field +0x0 | f32 | `θmax_a` | slip-angle threshold of axle a (a=0 front, 1 rear) (rad) — Stage 4 relevant; retail value 12 makes it inactive |
| +0x1E0 + 0x20·a, field +0xC | f32 | `μpk_a` | axle peak-grip multiplier |
| +0x1E0 + 0x20·a, field +0x10 | f32 | `μsl_a` | axle sliding-grip multiplier |
| +0x2C0 | f32 | `λ` | launch fraction (see S1-4.2) |
| +0x2C4 | f32 | — | not used in Stage 1 |
| +0x2C8 | f32 | `Ff` | front final-drive ratio |
| +0x2CC | f32 | `Fr` | rear final-drive ratio |
| +0x2D0 | f32 | `s` | rear drive share 0…1 (0 = FWD, 1 = RWD) |
| +0x2D4 | f32 | `k` | clutch coupling coefficient (kN·m per rad/s) |
| +0x2D8 | u32 | `n` | forward gear count |
| +0x2DC | f32 | `cr` | clutch rate (1/s scale) |
| +0x2E0 + 4·j | f32 ×8 | `r[j]` | gear ratios, j = 0 (R), 1 (N), 2 … 7 (forward 1…6) |
| +0x300 | f32 | `ε0` | idle rpm |
| +0x304 | f32 | `ε1` | shift rpm (automatic up-shift basis) |
| +0x308 | f32 | `ε2` | maximum rpm (rev limit) |
| +0x30C | f32 | `Ie` | engine rotational inertia (t·m²) |
| +0x310 + 4·j | f32 ×9 | `Tp[j]` | primary torque samples (kN·m) |
| +0x334 + 4·j | f32 ×9 | `Ts[j]` | secondary torque samples (kN·m); nonzero on forced-induction cars |
| +0x358 + 4·j | f32 ×3 | `Tb[j]` | engine-braking (closed-throttle drag) samples (kN·m) |

### S1-1.3 Stock composition [inf]
For a car without performance upgrades, the effective engine torque table is
`Tq[j] = Tp[j] + Ts[j]` (j = 0…8, binary32 sums). Every other Stage-1 field is used unchanged.
[inf] Upgrade/tuning/mode variants alter these fields; they are out of scope (see S1-9).

### S1-1.4 Drive class row [inf]
The rear share selects a drive class: `class = A` if `s < 0.2`, `C` if `s > 0.8`, otherwise `B`.
Stage 1 uses one value from it, the launch scale `σL`: A → 1.0, B → 1.2, C → 1.1.

### S1-1.5 Constants fixed by the executable [inf]
Wheel rotational inertia `Iw = 0.01` t·m² for every wheel of every car (re-imposed every frame).
Gravity constant used elsewhere: 9.8 m/s².

---

## S1-2 Time stepping [inf unless tagged]

1. Each rendered frame supplies a duration `Δt`. [inf] `Δt` is the measured frame time quantised to whole
   multiples of 1/1800 s (round to nearest, with carried timing error), with a frame-limiter clamp whose exact bounds are unresolved
   (U-5). World time scale is 1.0.
2. The frame is split into `N = trunc(Δt / 0.0044) + 1` equal substeps of length `h = Δt / N`
   (0.0044 = binary32 `0x3B902DE0`). An exact integer quotient still gets the extra substep.
   At 60 fps: N = 4, h = 1/240 s.
3. Once per frame, before the substeps: driver inputs are shaped (S1-5) and the gear selector runs (S1-4.5).
4. Each substep, in this order:
   a. Every dynamic block computes its time derivatives from the **current** state (all derivatives are
      collected before any state changes).
   b. Explicit Euler: `x ← x + h·ẋ` for every state scalar.
   c. Post-step of every block, in block order: body; then for i = 0…3 wheel i followed by its suspension;
      then engine (S1-3.5); then clutch/gearbox (S1-4.4).
   d. Ground-contact refresh (later stage).
   e. Drivetrain/tyre coupling (S1-7), which produces the wheel drive torques, tyre forces and engine
      feedback torque used by the next substep's derivatives; tyre forces are then applied to the body.
5. At vehicle creation, the post-step sequence (4c–4e) also runs with zero substep duration.
   In neutral the resulting engine feedback is `Φ = 0`: the engine post-step clears it and neutral
   torque distribution adds no reaction. The initial Engaging clutch post-step sets `mc = 0.7`.
   These are the states after creation processing; a test may explicitly start from another state.
6. State scalars relevant here: engine speed `ωe`; clutch engagement `c`; for each wheel angle `φi` and
   spin `ωi`; the body's linear/angular state (S1-8).

---

## S1-3 Engine [inf]

### S1-3.1 Curve lookup
`L(samples[0..q−1], a, b, x)`: `p = (x − a) · (q−1)/(b − a)`; `i = trunc(p)` toward zero.
If `p < 0` or `i < 0` → `samples[0]`. If `i ≥ q−1` → `samples[q−1]`. Otherwise
`f = p − i` and the result is `f·samples[i+1] + (1−f)·samples[i]`.

### S1-3.2 Inputs from other blocks
- `t` — shaped throttle 0…1 (S1-5), set once per frame.
- `mc` — clutch-state throttle multiplier (S1-4.4), set by the clutch post-step; initial value 1.0.
- `Λ` — limiter flag, initially off.
- `Φ` — feedback torque accumulated during coupling (S1-7); reset to 0 in every engine post-step.
- `ν` — nitrous stage index (0 = none; Stage 1 assumes 0, see S1-9).

### S1-3.3 Engine shaft torque `Te(ωe)`
Let `rpm = ωe·Kr` and `u = 0` if `Λ` else `u = mc · t`.
1. `Ton = L(Tq, ε0, ε2, rpm) · G · B · u`, where `G` is an engine torque scale (initial 1.0; see U-7) and
   `B = 1.05` if the vehicle-instance "bonus" flag is set, else 1.0 (U-6).
2. (Nitrous, ν > 0, deferred.) Adds `u · L(Nq, ε0, ε1, rpm) · Nscale · κν` where `Nq` is a fixed 9-sample
   table `(0.3, 0.4, 0.45, 0.5, 0.55, 0.6, 0.7, 1.0, 0.7)`, `Nscale` is a per-vehicle-instance nitrous scale
   (source not traced) and `κν = (1.0, 1.4, 1.7)` for ν = 1, 2, 3.
3. (Over-rev adjustment, deferred; inactive in Stage 1 because its state starts at −1 and is only re-armed in
   the drag/other race-mode family.) If its state `a > 0.6`: `Ton ← Ton · (1 − 0.75·(min(a,1) − 0.6)/0.4)`.
4. `Toff = −(1 − u) · L(Tb, ε0, ε2, rpm)`, doubled when `Λ` is on.
5. `Te = Ton + Toff`. If `Te < 0` and `Λ` is off: `Te ← 0.65·Te` when `g = N`, else `Te ← 0.85·Te`.

### S1-3.4 Derivative
`ω̇e = (Te + Φ) / Ie`. (A divisor multiplier exists for clutch modes 0 and 2; its value is the fixed constant
1.0, so it has no effect.)

### S1-3.5 Post-step
1. `ωe ← clamp(ωe, ε0·Kw, ε2·Kw)` (the engine never drops below idle; no stalling).
2. `Φ ← 0`.
3. Limiter hysteresis on `rpm = ωe·Kr`: if `Λ` is on and `rpm < ε2 − 0.0125·ε2` → off.
   Otherwise, if `Λ` is off and `rpm > ε2 − 25` → on.
4. Two further engine sub-states are advanced here: (a) a boost-gauge state driven by the secondary curve's
   maximum, which [inf] has no reader in the vehicle simulation (presentation only); (b) the over-rev
   adjustment of S1-3.3.3 (inactive). Neither changes Stage 1 dynamics.

---

## S1-4 Clutch and gearbox [inf]

### S1-4.1 State
Current gear `g`, target gear `g*`, clutch mode `M ∈ {0 Disengaging, 1 Open, 2 Engaging, 3 Locked}`,
engagement `c ∈ [0,1]` (an integrated state), pending-shift flag `P`, wheel-side speed `ωw` (S1-7.1).
Initial: `g = g* = N`, `M = 2`, `c = 0`, `P = 0`.
Rate scale `e = 1.0` (other values only in deferred race modes). External-clutch status is off (U-13).

### S1-4.2 Shift tables (computed at vehicle creation)
- Launch rpm: `U[1] = (ε0 + (ε1 − ε0)·λ) · σL`. [inf] it is not read by any Stage 1 rule.
- For forward gears `j = 2 … n`: `U[j] = ε1 − 100` and `D[j+1] = U[j] · (r[j+1]/r[j])` (0 if `|r[j]| ≤ 1e−5`).
  `D[j+1]` is the rpm at which gear j+1 is entered after an up-shift at `U[j]`.
  Evaluate the ratio before multiplying. Static evidence does not require rounding the ratio to binary32
  before multiplication; the stored result is binary32. Existing exact-binary32 derived vectors use
  an explicitly rounded ratio and are a reference convention, not proof of retail intermediate rounding (U-14).
- Top gear: `U[n+1] = ε2 + 500` (never reached).
- [inf] An alternative "optimal up-shift" search (first rpm, from `(ε0+ε1)/2` in 50-rpm steps, at which the
  next gear's wheel torque exceeds the current gear's) exists in the retail game but is inactive in the examined default static path; runtime activation
  has not been established. Do **not** select it as the default.

### S1-4.3 Shift-point jitter
Three offsets `ρ1, ρ2, ρ3` are drawn uniformly from [−100, 0) rpm at creation and each time the clutch
leaves Open towards Engaging (S1-4.4). The draws use a game-wide pseudo-random stream; a context flag can
suppress them (offsets then 0). See U-3. For deterministic tests set all three to 0.
Up-shift threshold for gear g: `Uth(g) = min(U[g] + (ρ3 if g = 3 else ρ1), ε1)`.
Down-shift second threshold uses the fixed lower bound −100 instead of a draw:
`Uth'(g) = min(U[g] − 100, ε1)`.

### S1-4.4 Clutch post-step (every substep)
First compute `ωw` (S1-7.1). Let `CRAWL ≡ (t < 0.3) ∧ (ωw ≤ ε0·Kw + 1e−4)` (throttle off and wheels at or
below idle-equivalent speed).
- **M = 0 Disengaging**: `mc = 0`. If `c ≤ 0`: `c = 0`, `M = 1`.
- **M = 1 Open**: `mc = 0`; `g ← g*`. If `P` or not `CRAWL`: `M = 2` and redraw the jitter (S1-4.3).
- **M = 2 Engaging**: `mc = 0.7`. If `c ≥ 1`: `c = 1` and, if `|ωe − ωw| ≤ 50` (rad/s) or `g = N`, `M = 3`.
  Then, if `CRAWL` and `g ≠ N`: `M = 0`.
- **M = 3 Locked**: `mc = 1`. If `g ≠ N` and `CRAWL`: `M = 0`. If `g = g*`: `P = 0`; else `P = 1`, `M = 0`.
- After the mode logic a "missed-shift" timer (only armed by the deferred manual/drag rules, normally 0) can
  force `mc = 0` for 0.45 s, and a "shift bonus" (drag only) can raise `mc` above 1 for 0.5 s; both deferred.

Engagement derivative (used in step S1-2.4a): `ċ = cr · e · K[M]` with `K = (−3, 0, 1.5, 0)`; e.g. with
`cr = 2`, full disengage takes 1/6 s and full engagement 1/3 s. (Shift-bonus scaling of `ċ` is drag-only.)

Consequences [inf]: an up-shift is: cut throttle and open the clutch (Disengaging), change gear (Open),
re-engage at 70 % throttle (Engaging), lock when the engine has been pulled to within 50 rad/s of the
wheel-side speed.

### S1-4.5 Gear selector (once per frame)
Inputs: gear command `κ` from the driver (U-1), engine rpm `ρe` (see U-10 for an override source),
shaped throttle `t`, wheel contact flags and slip speeds `si` (S1-7.3).

**Automatic, `κ = −1`:**
1. Mark the driver-selected gear as "automatic".
2. If `g ≤ N`: `g* = 2` (first gear).
3. Else if `M ≠ 3`: no change.
4. Else:
   - Up-shift: if `ρe ≥ Uth(g)` and `g < n+1`: if every wheel is in ground contact **and** has signed slip
     speed `si < 4` m/s, then `g* = g + 1`; otherwise nothing (no down-shift test either).
   - Down-shift (when `ρe < Uth(g)` or `g = n+1`): with
     `d1 = D[g] + ρ2` and `d2 = (r[g]/r[g−1])·Uth'(g−1) − 200` for `g > 2`,
     and for first gear `d1 = ε0 + 100` if `t < 0.2` else `0`, `d2 = 0`:
     if `ρe ≤ d1` and `ρe ≤ d2` then `g* = g − 1`.

**Direct command `κ ≥ 0`:** if `κ ≠ g` and `κ ≠ g*`, then `g* = κ`.

**Manual-assist command `κ = −2`** [inf; command-to-menu mapping remains U-2]:
Maintain elapsed assist time `a` and a remembered gear `g_last` for the latest requested manual change.
An accepted forward driver selection in `2 … n+1` becomes `gs`; when the previously selected value
was neutral, reset `a = 0`. Initial `a` and `g_last` are supplied selector state; their retail
initialization is not established here (U-2). Each invocation of this command adds the frame duration to `a`, including
frames when `M ≠ 3`. Only `M = 3` evaluates the following decisions:

- If `gs ≠ g*`, remember `g_last = g` and reset `a = 0`. If `gs < g*`, define the down-shift ceiling as
  `(r[g]/r[g−1])·1.2·ε2` for `g > 2`, otherwise `ε0+100` when `t < 0.2`, else 0.
  If decision rpm exceeds the ceiling, cancel by setting `gs = g* = g`; otherwise set `g* = g−1`.
  If `gs > g*`, set `g* = g+1`. These branches return the assist command immediately.
- If `gs = g*`, report automatic when `a > 10` seconds, or when `g > g_last` and decision rpm
  `> ε2−300`. “Above the last” means above the remembered pre-change gear, not above top gear.
  Reporting alone does not reset the timer.
- Otherwise, if decision rpm `< ε0+100` and `g > 2`, set `g* = g−1` and report automatic.
  If neither condition applies, retain assist.

The valid driver-selected range prevents a normal request below first or above top gear. The selector
itself has no additional top-gear guard in the `gs > g*` branch. Behavior for inconsistent externally
injected state is outside the valid input contract. Reporting automatic is a selector result passed to
the driver; which user-menu mode consumes that result remains unresolved (U-2).

For automatic selection at `g = 2`, the second down-shift threshold is 0. Both thresholds must pass;
positive idle rpm therefore cannot request neutral. The Stage-1 rules do not produce stationary
first/neutral hunting at ordinary idle, regardless of the first threshold `ε0+100`.

---

## S1-5 Driver input shaping (once per frame) [inf]
Raw pedals: throttle `t0`, brake `b0`, handbrake `hb0` (0…1).
1. Pedal exclusivity (applies except in some race-mode families, U-12): if `t0 > b0` and `t0 > hb0`, any of
   `b0`, `hb0` below 0.5 is set to 0; otherwise a throttle below 0.5 is set to 0.
2. Dead-zone: if `t0 ≤ 0.15` → `t = 0`, else `t = t0`.
3. Anti-stall floor: if `0 < t < 0.5`, `g > N` and engine rpm `< 3000` → `t = 0.5`.
The result `t` is the engine throttle of S1-3.

---

## S1-6 (reserved)

---

## S1-7 Drivetrain and tyre coupling (every substep, after post-steps) [inf]

### S1-7.1 Axle selection and wheel-side speed
Front axle is driven iff `s ≤ 0.999`; rear axle iff `s ≥ 0.001` (both for AWD).
Wheel-side engine speed `ωw = r[g] · W`, where with `Wf = Ff·(ω0+ω1)/2`, `Wr = Fr·(ω2+ω3)/2`:
`W = Wr` (rear only), `Wf` (front only), `(Wf + Wr)/2` (both).
The same pattern with wheel angular accelerations `βi = (τi − Ri·Fi)/Iw` gives `αw` (S1-7.5), and with wheel
slip speeds `si` (no final drive, no ratio) gives the mean driven slip `s̄` (S1-7.7).
Number of wheels in ground contact: `nc` (0…4) [inf].

### S1-7.2 Distribution `Dist(T)` of an engine-side torque T
If `g = N`: nothing happens (no wheel torque, no reaction). Otherwise:
1. `Φ ← Φ − max(0.25, nc/4) · T` (reaction on the engine).
2. `Tg = T · r[g]`; front axle torque `Af = (1 − s)·Ff·Tg`; rear `Ar = s·Fr·Tg`.
3. Split per axle between wheels (a, b) = (0, 1) front and (3, 2) rear: base bias `p = 0.35` if both wheels of
   the axle touch the ground, else `p = 0.5`; if `|ωa + ωb| ≤ 1e−5` then `f = 0.5`, else `f = ωb/(ωa+ωb)`;
   if `f < p` then `f = 1 − 0.175`; else if `f > 1 − p` then `f = 0.175` (0.175 = 0.35/2 regardless of p).
   Wheel b receives `f·A`, wheel a receives `(1−f)·A`. (Two forced-bias overrides exist for a drag-race traction
   aid; deferred.)
4. Torques add to each wheel's drive torque `τi`, except for a wheel flagged locked (brakes, Stage 3).

### S1-7.3 Initial drive (first step of coupling)
Use the stored `ωw` sampled at the start of the clutch post-step, before its mode logic can change
`g`. Do not refresh it at the beginning of coupling. Later finishing operations explicitly resample
wheel-side speed as described in S1-7.7.
- `M ∈ {0, 2}`: `Δ = clamp(ωe − ωw, −300, 300)`; `δ = (ωw − ωe)·Kr` (rpm). Coupling `k' = k`, except
  `k' = 0.5·k` when the previously stored `δ_prev ≠ 0`, `δ·δ_prev < 0` and both `|δ|, |δ_prev| > 2000`.
  Store `δ_prev = δ`. Apply `Dist(k' · c · Δ)`.
- `M = 3`: apply `Dist(Te)`.
- `M = 1`: nothing.

### S1-7.4 Tyre preparation (each wheel; Stage-1 part)
Inputs per wheel: normal load `Ni` (from suspension/contact, later stage — see S1-9), contact-frame velocity
`(vx, vy)` along the wheel's heading and across it, surface grip factors `μS,pk`, `μS,sl` (1.0 on ordinary road,
S1-9), per-frame grip multiplier `γi` (S1-7.8), brake torques (0 without brake/handbrake input).
1. Slip speed `si = Ri·ωi − vx` (stored; also used by S1-4.5 and S1-7.7).
2. If `|(si, vy)| > 0` and `(vx ≠ 0 or vy ≠ 0)`: `μ = μS,sl · γi · μsl_a`; `Fs = Ni·μ/|(si, vy)|`.
   - **Sliding** if `|si| > 0.5` or `|slip angle| > θmax_a` or locked: `Fi = si·Fs`, lateral `Fyi = −vy·Fs`
     (Stage 4); if body speed·2.237 < 1 and `μ > 0.1`, divide both by `μ`.
   - Otherwise **gripping**: `Fi = wL·τi/Ri`; lateral force from the lateral curve (Stage 4).
   `wL` (and a lateral counterpart `wT` multiplying `Fyi` in both branches) are per-wheel multipliers
   initialised to 1.0; no other Stage-1 writer was found. In the sliding branch `Fi = wL·si·Fs`.
3. If `|(si, vy)| = 0` or both contact-frame velocity components are 0: gripping branch.
   This predicate does not use the wheel spin or its direction state. A spinning wheel with
   `vx = vy = 0` takes the gripping branch; a dragged locked wheel with nonzero contact velocity
   can take the sliding branch. Stage 1 accepts the already calculated gripping lateral force as
   an input, multiplied by `wT`; use 0 for a straight symmetric test. Its curve remains Stage 4.

### S1-7.5 Iterative pass (5 times)
In each pass, for each wheel that is gripping and not locked, update its longitudinal force
`Fi ← Fi + (τi − Ri·Fi) − Ai · Iw / Ri`, where `Ai` is the body's planar acceleration at that wheel's contact
point projected on the wheel heading (S1-8.2), recomputed at the start of the pass from all current forces.
(This expression is reproduced literally; it is not dimensionally homogeneous — see U-9.)
Within **each** of the five passes, after all eligible wheel forces have been updated,
only if `M = 3`, apply the lock correction
`Dist( (αe − αw) / Dlock )` with `αe = (Te + Φ)/Ie`,
`Dlock = 1/Ie + [front driven] (1−s)/Iw · (r[g]·Ff)²/2 + [rear driven] s/Iw · (r[g]·Fr)²/2`.
`Te` is recomputed at this point; `Φ` includes all reaction torques applied so far.
`αw` uses current wheel accelerations computed from the just-updated forces and the current accumulated drive torques. Body acceleration is
computed once at the start of a pass and shared by that pass's wheel updates.

### S1-7.6 Friction limit (each wheel, after the passes)
`Fmax = μS,pk · μpk_a · γi · Ni` (for the player's rear wheels × `kv`, S1-7.8).
If `τi > 0` and the wheel is not locked, use `1.5·Fi` instead of `Fi` for this test only.
If `|(F, Fyi)| > Fmax` and `> 0.001`, scale both components by `Fmax/|(F, Fyi)|` (F is the 1.5-scaled value
where applicable; the 1.5 factor is undone only when no scaling happened). Afterwards, when `|vx| > 1`,
`Fi ← Fi − 0.15·sin(slip angle)·Fyi` (Stage 4 coupling). The resulting force vector, with `Ni` as its
normal component, is applied to the body at the contact point.
This final stored `Fi` is also the force used in the next wheel derivative's `τi−Ri·Fi`; there is
no separate unscaled longitudinal force for wheel spin.

### S1-7.7 Finishing
If `g ≠ N`, `M = 3` and `nc > 0`: resample wheel-side speed from current wheel speeds
and gear for the finishing calculations, and let `d = ωw − ωe`. This calculation does not
overwrite the cached initial-coupling speed unless the slip-sync branch explicitly sets it.
- If `t ≤ 0.2` or `s̄ ≤ 0.1`: `Φ ← Φ + clamp(k·d, −|Te|, |Te|)`.
- Else (throttle on and driven wheels slipping): set `ωw = ωe` and move the driven wheels so their
  wheel-side speed equals the engine's: for each driven axle, `δω = (ωe − ωw,old)/(r[g]·F_axle)`; if body
  speed·2.237 < 40 or neither wheel of that axle touches the ground, first set both wheels to their mean;
  then add `δω` to both. For AWD, sample the combined `ωw,old` **once before either axle
  changes** and use that same value for both axle corrections. Airborne tests use the axle's two
  contact flags. Finishing resamples this old value from current wheel speeds and current gear.
  In exact arithmetic this correction makes the combined driven wheel-side speed equal `ωe`.
Independently of the preceding `M = 3` and `nc > 0` condition, in any clutch mode, if `g = 2` (first gear): with `Te` recomputed, if `Φ < Te` and `Te + Φ < 0`, then
`Φ ← Φ + Ψ(Te + Φ) · κC · (Te + Φ)` where `Ψ` is the piecewise-linear table
`(−10, 1.0) (−7.5, 0.96) (−3.5, 0.925) (−0.3, 0.875) (−0.05, 0.0)` (clamped at both ends) and `κC = K[C]`
with `K` a fixed four-entry table indexed by the car's performance class `C ∈ {0,1,2,3}` (defined in S1-12).

The four entries are stored **contiguously in `C` order as binary32**, and the coefficient used is the
stored binary32 value (not the real decimal literal). Their exact encodings are:

| `C` | bits (little-endian) | bit pattern | coefficient (exact stored value) |
|---:|---|---|---|
| 0 | `00 00 00 00` | `0x00000000` | `0.0` |
| 1 | `CD CC 4C BE` | `0xBE4CCCCD` | `−0.20000000298023223876953125` |
| 2 | `CD CC CC BE` | `0xBECCCCCD` | `−0.4000000059604644775390625` |
| 3 | `33 33 33 BF` | `0xBF333333` | `−0.699999988079071044921875` |

The shorthand `(0, −0.2, −0.4, −0.7)` used elsewhere denotes these stored binary32 values; for `C = 1…3`
they are the nearest-binary32 values, not the real decimals (they differ by ≈1.5e−8 relative).

Precision of the term [inf]: `Ψ`, `κC`, `Te + Φ` and the stored `Φ` are all binary32; the product
`(Ψ·κC)·(Te + Φ) + Φ` is evaluated at a working precision wider than binary32 (the original's default working
significand is 53 bits, i.e. **double**), and the resulting `Φ` is stored back as binary32. The vector in
`test-vectors/s1_kappa_c.csv` therefore pins the coefficient exactly (a stored constant) but does not claim
a bit-exact term result; bit-exact intermediate rounding is not required by this stage (U-14).

`K[C]` is read as a plain base-plus-4·C access with no bounds test, clamp or default. The class scheme
produces only the sentinel `C = −1` and `C ∈ {0,1,2,3}`; those are the only defined inputs of `K`. The value
for `C = −1` is stated in **S1-12.5**; no other index is defined by this specification.

### S1-7.8 Grip multipliers
`γi = q · (1 − 0.025·X)`; for rear wheels additionally × a handbrake/yaw term that equals
`1 + |forward-axis vertical component|` when driving straight (Stage 4/5); × nitrous traction
`(1.0, 1.4, 1.7)` for ν = 1…3 (deferred). `q` is a per-vehicle grip factor (initial 1.0; ×0.85 for
non-player cars in normal races) and `X` a location-dependent factor that is 0 when a global feature switch is
off (U-4). Player speed factor `kv = 0.85 + 0.15·clamp((v − 13.4108)·0.11185, 0, 1)` (v = body speed, m/s),
applied to rear wheels (index > 1) of the human-driven car when the wheel is not locked and `ωi ≥ 0`;
it multiplies both the sliding coefficient (S1-7.4) and `Fmax` (S1-7.6).

---

## S1-8 Wheels and body (Stage-1 part) [inf]

### S1-8.1 Wheel spin
`φ̇i = ωi`. With the wheel's smoothed net torque `τ̄i ← 0.5·(τi − Ri·Fi + τ̄i,prev)` (stored each derivative
evaluation):
`ω̇i = (τ̄i − 0.002·ρS·ωi − ζi·0.1·ωi) / Iw`, where `ρS` is the surface rolling-drag factor (1.0 on road)
and `ζi ∈ {0, 1}` a per-wheel drag flag (0 in normal driving; Stage 2/5). A locked wheel has `ω̇i = 0`.
If the wheel is gripping and `h > 0.001`: `ω̇i ← ω̇i − si/(Ri·h)` (snaps the wheel to rolling speed in one
substep).
Post-step: reset `τi`, `Fi`, `Fyi`, `si` to 0; apply the angle adjustment below; a wheel that changes rotation direction
under braking is stopped (`ωi = 0`) (Stage 3); direction state: forward if `ωi > 1e−5`, stopped if
`ωi ≥ −1e−5`, else backward.

Angle adjustment [inf, Q-10]: let `A = 628.3179931640625` (binary32),
`B = 6283.18017578125` (binary32). If `φ > A`, subtract `A` once, then set to 0 if the
result exceeds `B`. Else if `φ < −A`, add `A` once, then set to 0 if the result is below
`−B`. Equality does not trigger either operation. This is not modulo `2π`; the angle is a
presentation state and this adjustment does not enter Stage-1 spin dynamics.

### S1-8.2 Body (planar, as used by S1-7.5)
`ax = (Ex + Σi (Fi·hx,i + Fyi·lx,i)) / m`, `ay = (Ey + Σi (Fi·hy,i + Fyi·ly,i)) / m`,
`α = Iz⁻¹ · Σi (moment of wheel i's planar force about the body origin)`,
`Ai = (ax − α·yi)·hx,i + (α·xi + ay)·hy,i`,
with positive yaw moment `x·cy−y·cx` for a planar force `(cx,cy)` at `(x,y)`.
Stage 1 takes `Iz⁻¹`, contact arms and unit vectors as supplied physical inputs; their derivation
from the body record and suspension is deferred, not a license to assign guessed retail values.
Here `h`, `l` are the wheel heading/lateral unit vectors in the body frame, `(xi, yi)` the contact arm, and
`(Ex, Ey)` the other forces on the body (aerodynamics, gravity, collisions) expressed in the body frame.
Straight-line, symmetric case: `Ai = ax`, `α = 0`, and `m·v̇ = Σ Fi + Ex`.
The body's mass is `m` from the record.

---

## S1-9 Deferred to later stages, and where Stage 1 touches them
- **Normal loads `Ni`** (suspension, weight transfer, contact) — Stage 5. Stage 1 formulas take `Ni` as input.
- **Aerodynamic drag/downforce** (`Ex`, row +0x370…), wheel rolling drag (`0.002·ρS·ω`), the per-car speed
  limit (row +0x3A0) — Stage 2.
- **Brakes**, ABS-like brake torque scaling, wheel lock state — Stage 3 (zero when pedals are 0).
- **Lateral forces**, slip angle, `θmax`, lateral grip curve — Stage 4.
- **Surface table** (29 entries; peak, sliding, rolling-drag factors; 1.0 on ordinary road rows) — Stage 5.
- **Race-mode variants** (drag, drift and other families): drive-class parameter variants, perfect-shift bonus,
  missed-shift penalty, alternate clutch rates, over-rev adjustment, launch traction aid, rev-limit cap `ε2`
  for up-shifts — not in Stage 1. Stage 1 = circuit/sprint/free-roam family.
- **Nitrous** (ν > 0) — not in Stage 1.
- **Upgrades/tuning** — not in Stage 1.

---

## S1-10 Unknowns / ambiguities
- **U-1** Which gear command the player's "Automatic"/"Manual" option produces. [inf] Automatic = −1.
- **U-2** Full semantics of the −2 command's "report automatic" return path.
- **U-3** Random jitter: the generator state is shared game-wide and the suppression flag's owner is unknown;
  whether player shifts are jittered in practice is unconfirmed.
- **U-4** Per-car grip factor `q` sources and the location factor `X` (its feature switch and lookup).
- **U-5** Exact end-to-end frame timing remains unresolved (Q-16). Static evidence shows
  carried timing error, clamping before quantization, a nearest-1/1800-second count and a
  separate conversion to the 4000-Hz clock for the carried error. A stateless
  `round(raw_dt·1800)/1800` with optional disabled bounds is only a test adapter, not a
  complete retail timing model. Do not infer runtime frame cadence from this description.
- **U-6** Source of the 1.05 torque "bonus" flag (per-vehicle-instance byte).
- **U-7** Writers of the engine torque scale `G` other than its initial 1.0.
- **U-8** `nc` and the body-speed scalars are inferred from use (counts/averages, mph conversion 2.237).
- **U-9** The grip-wheel force update mixes force and torque terms; reproduce literally.
- **U-10** The shift-decision rpm may come from an alternative source under some unidentified global condition;
  default is engine rpm.
- **U-11** Performance class `C` (0…3) is derived by the parameter-composition step; its definition,
  thresholds, forced-class rule, owner and lifetime are specified in **S1-12**, where the drive score
  `S` is defined given the sampled acceleration curve `a(v)`. Residual: the construction of `a(v)` (its
  speed sampling and gear advance) is not specified here, so `S` is reproducible only when that curve —
  or `S` itself — is supplied (out of Stage-1 scope); and whether a mid-session upgrade recomputes a
  live instance's class (versus only on reconstruction/load).
- **U-12** The race-mode flags that disable pedal exclusivity.
- **U-13** An external analog-clutch path (status 3) exists in the player input code; normally off.
- **U-14** Exact binary32/extended-precision rounding of intermediates is not specified; vectors carry tolerances.
  Q-2 resolves arithmetic grouping only; exact-binary32 shift-table vectors use a documented
  reference convention and do not establish exact retail results.
  The S1-7.7 `κC` coefficients are stored binary32 and are pinned exactly (S1-7.7); the first-gear
  term's product is evaluated with binary32 operands at a wider (default double) working precision and
  stored back as binary32, so a faithful implementation matches the *coefficient* exactly but the term
  result only to tolerance.
- **U-15** Initial engine speed at vehicle creation (vectors start at idle; S1-3.5 clamps to idle anyway).
- **U-16** No retail runtime trace validates any per-tick behaviour yet.
- **U-17** Whether a *simulated* drivetrain ever reaches the S1-7.7 first-gear clause with its class copy
  equal to the unset sentinel `−1` is not established. The drivetrain copy can hold `−1` (it is overwritten
  with the physics-parameter object's unset class byte when the car type is unset at vehicle creation) and
  neither the clause nor its caller gates on the class or car type, but the normal spawn path resolves the
  class before the copy is taken. The value the clause would then use is fixed (S1-12.5); only the
  reachability of the state is open, and no runtime trace exists (U-16). The defined class set is
  `{−1, 0, 1, 2, 3}`; any other index is produced by no reachable state and is outside the specification
  (S1-12.5).


## S1-11 Question resolution and remaining boundaries (2026-10-15)

All executable behavior clarified in this revision is **inferred from static evidence**. No runtime
claim is added. Synthetic regression cases are in `test-vectors/s1_question_cases.json`.

Q-1: mutually exclusive axle-bias conditions (S1-7.2). Q-2: ratio-first grouping (S1-4.2),
with intermediate rounding unresolved (U-14). Q-3: contact-frame translation predicate (S1-7.4).
Q-4: correction in each pass with updated forces (S1-7.5). Q-5: positive-idle no-neutral result
(S1-4.5). Q-6: remembered gear, valid selection range and assist timer semantics (S1-4.5).
Q-7: one combined AWD speed snapshot (S1-7.7). Q-8: independent first-gear condition (S1-7.7);
class remains input U-11. Q-9: final stored force shared by wheel and body (S1-7.6).
Q-10: angle adjustment (S1-8.1). Q-12: cached initial coupling speed (S1-7.3).
Q-13: creation post-step clears feedback before neutral coupling (S1-2.5).
Q-14: supplied lateral force is a Stage-1 boundary, curve deferred (S1-7.4).
Q-15: moment sign and supplied body parameters (S1-8.2), parameter extraction deferred.

**Q-11 remains partial:** the rear multiplier is mode-gated and blended with a handbrake-dependent
state; it must not be applied unconditionally to every rear wheel in all modes. The existing
`1+|forward-axis vertical component|` statement is a restricted straight-running case, not a general
formula. Until Stage 4/5 resolves the mode eligibility, blend, yaw and speed dependence, accept the
complete rear multiplier as an environmental input. Use 1 only for an explicitly level, straight,
no-handbrake synthetic case. No general retail default is established.

**Q-16 remains partial:** U-5 records why independent rounding of each frame is insufficient.
Frame-duration vectors exercise subdivision of supplied `Δt`; they do not test the upstream clock.

The earlier approval applies only to its recorded hash. This revision requires a fresh reviewer gate.

## S1-12 Performance class `C` (2026-10-17)

This resolves the class source named in U-11 and completes Q-8's `κC` reference in S1-7.7. The class
values below were derived by deterministic execution of the parameter-composition step against retail
car rows, and the mapping and stored tables were read from the executable's data (static analysis; the
game was not launched — see U-16).

`C` is an integer 0…3 (0 = highest performance) held per **vehicle instance** in the physics-parameter
object as a signed byte, with `−1` meaning "not yet computed"; it is *not* a field of the car-type
record. It is derived by the parameter-composition step from the *current effective record* — the
car-type record with currently installed upgrades and active tuning applied — so two instances of the
same model with different builds may have different `C`. The drivetrain block keeps its own copy, taken
at vehicle creation; that block's constructor default is 3, but the copy overwrites it. If the car type
is unset the composition step does nothing, the class stays `−1`, and the copy is left as `−1`. Whether
a mid-session upgrade recomputes a live instance's class (as opposed to only on
reconstruction/load) is not established.

### S1-12.1 Forced class [inf]
If the effective record's signed 32-bit field at row + 0x844 is nonzero, `C = 3` without forming a
score. In the retail table every non-player traffic/service row has this field set (value 2) and every
player-available row has 0.

### S1-12.2 Drive score `S` [inf]
Using the effective record, form a drive-acceleration-versus-speed curve `a(v)` in m/s² — the
composition step's 64-sample scoring curve over `[0, v_top]`, `v_top` being its top sampled speed. With
`cd` the aerodynamic coefficient at row + 0x370, `m` the mass at row + 0x220, and the fixed binary32
step `Δ = 1.3410818576812744`:

```
v ← 0 ;  S ← 0
while v ≤ v_top:
    a ← curve(v)                 # endpoint-clamped linear interpolation of the 64 samples
    r ← v · cd · v / m
    if a < r: stop
    S ← S + (a − r) · Δ
    v ← v + Δ
```

`S` is stored as binary32 in the physics-parameter object. The loop condition is inclusive
(`v ≤ v_top`) and the break is strict (`a < r`); equality `a = r` continues with a zero increment. The
composition step's construction of `a(v)` (its speed sampling and gear advance) is out of Stage-1 scope
(see U-11); the class rule below depends only on `S`.

The `K` table of S1-7.7 is four **binary32** coefficients stored contiguously in `C` order; the shorthand
`(0.0, −0.2, −0.4, −0.7)` denotes those stored binary32 values, and their exact bit patterns and real values
are tabulated in S1-7.7 (not the real decimal literals).

### S1-12.3 Classification [inf]
The thresholds are applied in descending order with **strict** `>`:

| condition | `C` |
|---|---|
| `S > 500` | 0 |
| `401 < S ≤ 500` | 1 |
| `273 < S ≤ 401` | 2 |
| `S ≤ 273` (including `S ≤ 0`) | 3 |

Equality falls to the lower class: `S = 500 → 1`, `S = 401 → 2`, `S = 273 → 3`, `S = 0 → 3`. These are
the only thresholds; the `0` entry only ever yields class 3. No further clamping or rounding is applied.

### S1-12.4 Stock classes [inf]
With stock (unupgraded, untuned) records: the 15 traffic/service rows are forced to 3; among the 31
player models, 20 are 3 (`S` 144–266) and 11 are 2 (`S` 274–334, the A3/RX8/350Z/G35/3000GT/GTO/
MUSTANGGT/SKYLINE/LANCEREVO8/RX7/IMPREZAWRX group). No stock record reaches `S > 401`, so classes 0–1
occur only with upgraded/tuned records (a scaled-torque sweep crosses all four classes; see
`test-vectors/s1_perf_class.csv`).

### S1-12.5 The `−1` class copy and the S1-7.7 coefficient [inf]
The class copy held by the drivetrain block is the physics-parameter object's signed class byte sign-extended
to an integer; it is written unconditionally at vehicle creation from that byte, overwriting the drivetrain
block constructor's default of `3`. When the car type is unset at creation the composition step does not run,
so the object's class byte is still its `−1` sentinel and the copy is `−1`.

The S1-7.7 coefficient lookup is a plain base-plus-4·C read of the four contiguous `K` entries with no bounds
test, no clamp and no default. For `C = −1` the selected 4-byte slot is the one immediately preceding the
`K` table; its binary32 value is `0.25` (`0x3E800000`) and it is a constant also used elsewhere in the vehicle
physics. The coefficient actually applied is therefore `κC = 0.25` for `C = −1` — not `K[0] = 0` and not a
no-op. The class scheme produces no index other than the `−1` sentinel and `0…3`; those five are the only
**defined** inputs of `K`, and their coefficients are pinned above and in S1-7.7. Any other index is outside
this specification (it is never produced, and its behaviour need not be reproduced); an implementation may
reject or ignore it.

[inf] For traceability only, the unguarded read would select the static neighbours of the table: the word
immediately after `K[3]` is binary32 `0.0` (`0x00000000`), so `C = 4` would read `0.0`; the word before the
`−1` slot is binary32 `1.0` (`0x3F800000`), so `C = −2` would read `1.0`. These are constant-pool
neighbours, not behaviour any reachable state depends on, and they are not requirements.

Whether a drivetrain whose copy is `−1` is ever simulated as far as the first-gear clause is not established
(U-17); the clause and its caller have no class or car-type gate, but the normal spawn path resolves the class
before the copy is taken. The direction of this statement is behavioral only: an implementation that returns
`0` for `C = −1` diverges from the original if the state is reached.

Provenance: this section's findings are inferred from static evidence and deterministic execution of
the parameter-composition step; the game was not launched, so no retail runtime trace was captured
(U-16) and the tags are [inf].
