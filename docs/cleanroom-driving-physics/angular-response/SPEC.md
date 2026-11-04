# Ordinary angular damping and slip torque restriction — behavioural specification

Subject: the two angular-momentum corrections applied to the world angular
momentum derivative in the ordinary (non-race-exception) chassis boundary:

1. the **stabilization/angular-curve damping pair** (EFA-2), and
2. the **slip-dependent vertical torque restriction** (EFA-3).

Status: **proposal, not approved.** Derived exclusively from the bounded
static-evidence summaries EFA-2 and EFA-3 (see PROVENANCE.md) plus the local
`GLOBAL/GLOBALB.BUN` aggregate checks recorded in `docs/FORMATS.md`. No
retail runtime was observed. Nothing here is an implementation
authorization; it requires an analyst amendment and a fresh reviewer gate.

Confidence tags: **[inf]** inferred from static executable/data evidence;
**[gss]** guessed (none used normatively). Untagged statements inherit the
tag of their section heading.

Relationship: this document and the Stage 4/5 specs share the angular
momentum equation. Stage 2's translational stabilization (`-8*z*P`) is
referenced only for gate placement; it is not re-specified here.

## AR-0 Domain, state and composition

1. Inputs: world angular momentum `L` (kN·m·s per the Stage conventions),
   body orientation `B` and body-axis angular velocity magnitude
   `w = |Omega|` where `Omega = B * inverse(I) * transpose(B) * L`, the
   accumulated world torque `T` for the current derivative evaluation, the
   Stage 2 stabilization state `z` (pre-Euler, as in Stage 2), the two gates
   (`angular_curve_enabled`, `stabilize`), the selected restriction scalar
   `K` (AR-4), and the exported signed forward speed.
2. Output: the corrected angular momentum derivative `Ldot`.
3. Validity domain: ordinary (non-race-exception) boundary. Race-state
   code 4 inputs are named as gate overrides only (AR-3); the race-state
   producer is external and unspecified here. `K` must be nonnegative
   (AR-4); negative `K` is outside the fixture domain.
4. Ordering: the curve and stabilize terms modify `Ldot` first; the
   raw-torque override then applies to the vertical (world Z) component of
   the already damped result (AR-5).

## AR-1 Stabilization curve D(w) [inf]

`D(w)` is the endpoint-clamped, piecewise-linear interpolation of the
following (rad/s, 1/s) pairs, with the decimal parameters interpreted as
binary32:

| w (rad/s) | D(w) (1/s) |
|---:|---:|
| 0 | 0.05 |
| 2 | 0.05 |
| 4 | 0.25 |
| 6 | 0.7 |
| 8 | 2 |
| 10 | 5 |

Rules:

1. For `w < 0` the first knot applies (curve is a function of magnitude);
   for `w > 10` the last knot applies (5). Endpoint clamping is exact.
2. Between consecutive knots, linear interpolation between the binary32
   endpoint values. Below/above the first/last knot, the endpoint value is
   used without extrapolation.
3. The knots lie inside `[0, 10]`; `w` is a magnitude and is never negative
   in this boundary.

Representative values (binary32 arithmetic, for orientation only):

- `D(0) = D(2) = 0.05`; `D(3) = 0.15`; `D(4) = 0.25`; `D(6) = 0.7`;
  `D(8) = 2`; `D(10) = 5`; `D(w ≥ 10) = 5`.

## AR-2 Gates [inf]

Two independent gates govern the two damping terms:

| gate | governs |
| --- | --- |
| `angular_curve_enabled` | the `D(w) * L` term |
| `stabilize` | the `2 * z * L` term (and, separately, the `-8 * z * P` translational term, which is out of scope here but shares this gate) |

Ordinary state codes:

- **Code 0 and code 2**: both gates initially enabled.
- **Code 1**: both gates disabled.

Exceptional boundary (gate narrowing only, never widening): under the
supplied exceptional **race-state code 4**, or **throttle strictly greater
than binary32 0.3**, or **all four exported contacts plus
`abs(body_forward.z)` strictly greater than binary32 0.15**, `stabilize` is
disabled **without** disabling `angular_curve_enabled`. The three triggers
are independent alternatives; any one suffices. The race-state code
producer, the throttle producer and the all-contacts/`|body_forward.z|`
producers are external to this document (U-1…U-3).

The `stabilize` gate's `z` is the pre-Euler Stage 2 stabilization state,
exactly as Stage 2 consumes it; this document does not re-derive `z`.

## AR-3 Angular momentum derivative [inf]

Given accumulated world torque `T`, the angular momentum derivative,
**before** the conditional vertical override of AR-4, is:

```
Ldot = T
     - (angular_curve_enabled ? D(w) * L : 0)
     - (stabilize            ? 2 * z * L : 0)
```

Both subtractions are world-vector operations; no body-axis conversion
occurs anywhere in this section. `D(w) * L` scales each component of `L` by
the scalar `D(w)`.

Synthetic checks (T = 0, L = (0,0,2), pre-Euler z as stated):

- `w = 2, z = 0`, curve on, stabilize on: `D(w)=0.05`, `Ldot.z = -0.1`.
- `w = 8, z = 0`, same gates: `D(w)=2`, `Ldot.z = -4`.
- `state code 1` (both gates off): `Ldot.z = T.z`.
- Code 4 / throttle > b32(.3) / contacts+|fz| > b32(.15): stabilize off,
  curve on: `Ldot.z = T.z - D(w) * L.z`.

## AR-4 Slip-dependent vertical torque restriction [inf]

### Frame refresh (once per frame, "frame refresh" phase)

For **local movement**, select `K` from two effective source-car scalars in
the 2,192-byte car record (same record as `S5-1`/`S1-1`):

- if the established numeric selector `p` equals zero: `K = row[+0x398]`;
- otherwise: `K = row[+0x39C]`.

These offsets are **not** contact count, inertia, or another selector
(U-4). Local aggregate (46 records, `docs/FORMATS.md`): `+0x398 = 17.0` and
`+0x39C = 0.0` in every record surveyed.

**p ≠ 0 only:** if the selected `K` is exactly zero, compute `sigma`, the
established wrapped body-minus-velocity heading in radians, defined by the
S4-4 producer: body heading minus world velocity heading, wrapped to
(-180°, 180°] with the S4-4 angular quantizer, and **set to zero when the
exported signed forward speed is below 1 m/s**; above that threshold the
wrapped value stands (reverse motion enters the zero-slip case regardless
of speed magnitude).

If `sigma * L.z < 0` and `|sigma| > b32(0.1)`, **set `K = 20` kN·m**.
Exact zero product does **not** trigger; exactly `b32(0.1)` does **not**
trigger (strictly greater is required).

**Remote movement** has its own tiny override and is **outside this
contract** (U-5).

The selected `K` is held for the frame's derivative evaluations.

### Derivative evaluation (each substep)

If `K != 0` and the **raw accumulated** torque satisfies `raw T.z > K`,
replace `Ldot.z` by `K`; if `raw T.z < -K`, replace `Ldot.z` by `-K`;
otherwise retain the already damped `Ldot.z` from AR-3.

This is **not** `clamp(T.z - damping, -K, K)`:

- the comparison uses the **raw** accumulated vertical torque, before the
  damping terms;
- the replacement happens **after** damping and replaces `Ldot.z` wholesale;
- no body-axis conversion occurs at this override.

Valid nonnegative `K` is the bounded fixture domain. Effective per-car `K`
values and the upstream selector/race selection must be retained or
explicitly supplied to any test (U-4); this document does not choose them.

Synthetic checks:

- `K = 20`, raw `T.z = 25`, damping contribution `-3`: raw exceeds K →
  `Ldot.z = 20` (not 17).
- `K = 20`, raw `T.z = 19`, damping contribution `-50`: raw in range →
  `Ldot.z = -31`.
- `K = 20`, raw `T.z = -25`: `Ldot.z = -20`.
- `p ≠ 0`, selected `K = 0`, `sigma = -0.2`, `L.z > 0`: `sigma*L.z < 0` and
  `|sigma| > b32(0.1)` → `K` becomes 20 for the frame.
- `p ≠ 0`, selected `K = 0`, `sigma = +0.2`, `L.z > 0`: product not negative
  → `K` remains 0 → override inactive.
- `p ≠ 0`, selected `K = 0`, `|sigma| == b32(0.1)`: not strictly greater →
  `K` remains 0.
- `p ≠ 0`, selected `K = 0`, signed forward speed `< 1 m/s`: `sigma = 0` →
  `K` remains 0.
- `p == 0`: `K = row[+0x398]` (17.0 locally), no sigma test.

## AR-5 Composition order [inf]

1. AR-3 is evaluated first: `Ldot = T - curveTerm - stabilizeTerm`.
2. AR-4 then inspects the **raw** `T.z` and, when the trigger holds,
   **replaces** `Ldot.z`; `Ldot.x` and `Ldot.y` are untouched by the
   override.
3. The translational `-8 * z * P` damping is a separate pathway under the
   same `stabilize` gate and is not modified by this document.
4. Order within a frame: frame refresh (K selection, sigma evaluation) once;
   derivative rule each substep.

## AR-6 Worked cases (synthetic; arithmetic in real precision unless noted)

1. Curve knots: `D(0)=0.05`, `D(2)=0.05`, `D(3)=0.15`, `D(4)=0.25`,
   `D(6)=0.7`, `D(8)=2`, `D(10)=5`.
2. Clamp: `D(w>10)=5`; below `w=0` never occurs (magnitude).
3. `T=0, L=(0,0,2), w=2, z=0`, curve on, stabilize on → `Ldot.z=-0.1`.
4. Same with `w=8` → `Ldot.z=-4`.
5. Interpolation: `w=3`, `L=(0,0,2)` → `D(3)=0.15` → `Ldot.z=-0.3`.
6. Stabilize contribution: `w=2, z=0.5, L=(0,0,2), T=0` →
   curve `-0.1`, stabilize `-2*0.5*2 = -2` → `Ldot.z = -2.1`.
7. State 1 → `Ldot = T`.
8. Code 4 (stabilize off, curve on): `w=2, z=0.5, L=(0,0,2), T=0` →
   `Ldot.z = -0.1` (stabilize term absent).
9. Override beyond K: raw `T.z=25`, `K=20`, damping `-3` → `Ldot.z=20`.
10. Override inside K: raw `T.z=19`, `K=20`, damping `-50` → `Ldot.z=-31`.
11. Negative side: raw `T.z=-25`, `K=20` → `Ldot.z=-20`.
12. K selection `p==0` → local fleet `K=17`; `|raw T.z| > 17` clamps, else
    damped value stands.
13. K selection `p≠0`, raw row `+0x39C = 0`, `sigma=-0.2`, `L.z>0`, forward
    speed ≥ 1 m/s → `K=20`.
14. Same with `sigma=+0.2` → `K=0` → no override.
15. Same with `|sigma| == b32(0.1)` → `K=0` → no override.
16. Same with forward speed < 1 m/s → `sigma=0` → `K=0` → no override.

## AR-7 Explicitly outside this boundary [inf]

- **U-1** race-state code 4 producer, mode reachability and full state map;
- **U-2** throttle source/timing feeding the `> b32(0.3)` alternative;
- **U-3** all-contacts / `|body_forward.z| > b32(0.15)` producer;
- **U-4** identity of selector `p`; effective per-car `+0x398`/`+0x39C`
  provenance beyond the local aggregates; never equate `p` to contact
  count or torque-limit selectors;
- **U-5** remote-movement override and its tiny constants;
- **U-6** the exact sigma producer beyond the S4-4 reference (wrapping
  edge cases, reverse-motion equivalence, reset semantics);
- **U-7** adjusted-vs-basic inertia transition and the `L.z / kI`
  asymmetry — specified in the body-response contract (W1-4), not here;
- **U-8** collision/rest behaviour, airborne angular behaviour, and the
  exact orientation repair (outside all current contracts).

## Cross-references

- Evidence summary: `docs/cleanroom-driving-physics/feel-audit/ORIGINAL_GAPS.md`,
  items 2 (EFA-2) and 3 (EFA-3). See PROVENANCE.md.
- Stage 2 (stabilization state `z`, `-8*z*P`):
  `docs/cleanroom-driving-physics/stage2/SPEC.md`.
- Stage 4 (sigma producer, S4-4): `docs/cleanroom-driving-physics/stage4/SPEC.md`.
- Local aggregates: `docs/FORMATS.md` "Stock drivetrain and axle-force
  fields — local aggregate verification" (+0x398/+0x39C rows).
- Implementation status: none. Gameplay supplies an unscaled inertia and
  zero damping today; nothing here is live.
