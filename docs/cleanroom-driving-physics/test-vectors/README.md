# Stage 1 test vectors

All vectors are **derived**: the analyst computed them by applying SPEC.md §S1 statements to car-record
fields read from the retail `GLOBAL/GLOBALB.BUN` (identical to the installed `GlobalB.lzc` for every Stage-1
field), or to an invented synthetic record. **None were captured from the running game.** They test that an
implementation reads the record and applies the stated formulas the same way. They do not prove retail
fidelity of the formulas (SPEC U-16).

Cars: `PEUGOT` (FWD, 5 gears), `240SX` (RWD, 5 gears), `SKYLINE` (AWD s=0.5, 6 gears, nonzero secondary
torque curve, clutch rate 4.0), `SYNTH_RWD` (invented values, s=0.9, 4 gears).

Reviewer note: the retail-car files contain numbers derived from the retail data (torque samples, ratios).
If the project policy forbids committing retail-derived numbers, keep only the `SYNTH_RWD` rows and regenerate
the retail rows locally from the user's data.

| file | content | statements exercised | tolerance |
|---|---|---|---|
| `s1_params.json` | per-car record fields (S1-1.2), composed torque `Tq`, drive class, wheel radii, axle grip, rpm knots of the 9- and 3-sample curves, shift tables `U` and `D` with jitter = 0 | S1-1, S1-4.2 | exact binary32 except `D`: 2e-6 relative (U-14) |
| `s1_engine_torque_<car>.csv` | `Te` at 250-rpm steps for u = 1, 0.5, 0 (forward gear), u = 0 in neutral, and limiter on (no nitrous, G = B = 1) | S1-3.1, S1-3.3 | 1e-6 kN·m |
| `s1_substeps.csv` | substep count and length for given frame durations (binary32 frame dt) | S1-2.2 | exact |
| `s1_clutch_torque.csv` | clutch slip torque `k'·c·Δ` and the stored rpm delta, including the 300 rad/s clamp and the halving rule | S1-7.3 | 1e-6 |
| `s1_axle_split.csv` | per-axle wheel shares from wheel speeds, contact and the bias rule (`override_mode` is always 0; the deferred drag-aid overrides are not specified and have no rows) | S1-7.2 step 3 | 1e-6 |
| `s1_freerev_neutral_<car>.csv` | full-throttle free rev in **neutral** with the gear command held at neutral, 60 fps frames (4 substeps of 1/240 s), starting at idle with `c = 0`, `M = 2`, `mc = 1`, `Φ = 0`: engine speed, limiter, clutch mode/engagement and `mc` after each substep's post-steps | S1-2, S1-3, S1-4.4 (in neutral no torque reaches the wheels and there is no reaction, S1-7.2) | ±0.05 rad/s; limiter toggles may shift by one substep where rpm is within 0.1 rpm of a threshold |
| `s1_perf_class.csv` | per-car retail stock performance class (drive score `S` and class index `C`, plus the forced-class flag), strict-threshold boundary cases, and a torque-scale sweep of one car crossing all four classes | S1-12, S1-7.7 | class exact integer; `S` to 1e-3 (binary32) |
| `s1_kappa_c.csv` | the S1-7.7 first-gear coefficient `κC = K[C]` for `C = 0..3` and for the unset sentinel `C = −1` | S1-7.7, S1-12.5 | coefficient exact (stored binary32); S1-7.7 term result per U-14 |

A full launch in gear (speed, gear and force against time) is **not** provided. It depends on normal loads
from the suspension (Stage 5), aerodynamic and rolling forces (Stage 2), the body integrator, and the
game-wide random stream (U-3). A vector built on assumed values for those would look authoritative but would
be partly guessed.

Provenance: all numbers were computed from the SPEC statements and the stated parameter rows (retail rows read from the owner's legally installed data; `SYNTH_RWD` invented). No retail asset files, captures or traces are included.

## Question regression cases (2026-10-15)

`s1_question_cases.json` supplies invented, isolated inputs and mathematically derived expected
outputs for the amended question answers. Each case identifies its behavior and relevant inputs;
unspecified later-stage effects are absent. Numeric tolerance is 1e-5. Cases are specification
regressions, **not retail runtime observations**. Their purpose is to distinguish branch predicates,
state snapshots, timer boundaries, AWD correction and stored-force behavior. They are separate
from the existing CSV schemas.

For Q-2 the landing table's existing exact-binary32 entries assume the ratio is rounded to binary32
before multiplication. Static analysis confirms ratio-first grouping but does not confirm this
intermediate rounding. Allow a small floating-point tolerance (2e-6 relative) for those landing
values until U-14 is resolved; exact match only tests the original vector-generation convention.

The free-rev cases deliberately begin with `mc=1`. The revised creation rule performs a zero-time
post-step and yields `mc=0.7` in initial neutral/Engaging state. These existing cases continue to test
their explicitly prescribed starting state; they must not be treated as retail vehicle-creation traces.
Frame subdivision CSVs start with supplied durations and do not test the upstream residual clock
quantization or frame clamp (Q-16).

Each question case exercises the named isolated operation: its input state is supplied at that
operation, after any earlier frame processing (in particular, assist `elapsed` is the value used
for the decision). `assist_report` cases assume Locked mode. `slip_sync` supplies the active AWD
slipping branch with both axles driven; it does not test eligibility. The creation case assumes
otherwise valid neutral creation state. Omitted inputs must not be interpreted as retail defaults.

## Performance-class vectors (2026-10-17)

`s1_perf_class.csv` covers S1-12. The columns are `case, source, force_flag, drive_score, class, note`.

- `source = retail_stock`: 46 retail car rows. The class index and drive score were obtained by
  **deterministically executing the original composition routine** (in a CPU emulator, with a synthetic
  empty-upgrade environment) on each row's physics source read from `GLOBAL/GLOBALB.BUN`; the game was
  never launched. For rows whose flag at row + 0x844 is nonzero, `drive_score` is blank and the class is
  the forced 3. This is a reproduction of the original routine's behaviour, not a runtime capture.
- `source = rule`: `S`-only boundary cases derived from the strict `>` classification (S1-12.3); they
  test the mapping independently of any car.
- `source = synthetic`: a uniform torque-scale sweep of the 350Z record, again executed through the
  original routine, crossing the 273/401/500 thresholds (all four classes reached).

The class index is an exact integer; the drive score is compared to 1e-3 (binary32 storage). The
threshold table `{0, 273, 401, 500}`, the step `Δ = 1.3410818576812744` and the coefficient table
`{0, −0.2, −0.4, −0.7}` were read from the executable's static data (the coefficients are stored
binary32; their exact stored values/bit patterns are in `s1_kappa_c.csv`). Provenance: values derived by
deterministic emulation of the composition step; no game runtime capture and no original asset is
included.

Testability note: the `drive_score` values of the `retail_stock` and `synthetic` rows are outputs of the
composition step's drive-acceleration curve, whose construction is out of Stage-1 scope (SPEC U-11), so
those rows validate the class index given `S` rather than an independent recomputation of `S`. The
`source = rule` rows test the `S → class` mapping on its own.

## κC coefficient vectors (2026-10-17; Q-17 / A-17.1, revised for A-17.2 / A-17.3)

`s1_kappa_c.csv` covers SPEC S1-7.7 / S1-12.5. The columns are `case, c_class, kappa_c, status, note`.
The four `K` entries are stored as **binary32**; their exact stored values and bit patterns (read from the
executable's static data) are:

| `C` | bit pattern | stored value (exact) |
|---:|---|---|
| 0 | `0x00000000` | `0.0` |
| 1 | `0xBE4CCCCD` | `−0.20000000298023224` |
| 2 | `0xBECCCCCD` | `−0.4000000059604645` |
| 3 | `0xBF333333` | `−0.699999988079071` |

The `kappa_c` column carries these stored values, not the decimal literals `−0.2 / −0.4 / −0.7` (from
which they differ by ≈1.5e−8 relative). The coefficient is exact; the file does **not** assert a bit-exact
S1-7.7 *term* result — that product is evaluated at a wider working precision and stored back as binary32
(U-14).

The `c_class = −1` row is the unset-sentinel case: the coefficient lookup performs a plain base-plus-4·C
read of the four contiguous `K` entries with no bounds test, so `−1` selects the 4-byte slot immediately
preceding the table, whose binary32 value is `0.25` (`0x3E800000`). This row fixes the coefficient only;
whether a simulated drivetrain reaches the first-gear clause with the copy at `−1` is unresolved (SPEC
U-17). Indices other than `−1` and `0..3` are not produced by the class scheme and are outside the
specification (A-17.3); no vector row requires an out-of-range read.

Provenance: values read from the executable's static data (static read of the constant pool); the game was
never launched, so this is not a runtime capture (U-16).
