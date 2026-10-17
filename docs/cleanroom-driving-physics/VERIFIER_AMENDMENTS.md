# Proposed verifier amendments — 2026-10-15

None proposed. The fresh verification round found no divergence against the approved spec or supplied derived vectors. See VERIFICATION.md for coverage and limitations. Retail runtime differential testing remains unperformed; absence of a runtime comparison is not evidence of parity.

Any future behavior divergence must be recorded here as a testable requirement/vector with provenance and pass a fresh reviewer gate before implementation work.

---

# PHY-01 performance class — 2026-10-17

Verifier round for branch `phy-01-perf-class`. Checked `SPEC.md` at
`355d67b46fd713578d17800338469cecb450169ad8e746b1199dd5564de803d4` (matches the
2026-10-17 REVIEW.md gate), `s1_perf_class.csv`, `QUESTIONS.md` Q-17 and `src/drivetrain.*`.

Result summary: no behavioral divergence was found between the implementation and any
**defined** statement of S1-12 or the S1-7.7 `κC` clause. `make drivetrain-test` = 3804 checks /
0 failed; `make drive-stage1-test` passed; strict and ASan/UBSan runs clean; an independent
verifier harness (225 checks, own spec re-derivation) reproduced all 65 CSV rows, the strict
`500→1, 401→2, 273→3, 0→3` boundaries, the forced-flag short-circuit, the per-instance
creation-time copy and the `K = (0,−0.2,−0.4,−0.7)` selection. One specification gap remains, below.

## Amendment A-17.1 (spec gap; Q-17): `κC` when the class copy is `−1`

**Gap.** S1-12 states the class is a signed byte whose `−1` means "not yet computed" and that
"if the car type is unset … the class stays `−1`, and the copy is left as `−1`", so `−1` is an
explicitly reachable value of the drivetrain block's copy. S1-7.7 then runs its first-gear term
"in any clutch mode, if `g = 2`" with no class guard and defines `κC = K[C]` only for
`C ∈ {0,1,2,3}`. The approved spec therefore does **not** define the term's coefficient when the
copy is `−1`. (Same gap for any value outside `0..3`.)

Implementation-as-found (not an instruction): `drv_kappa_c()` returns `0.0` for any class outside
`0..3`, so the term is a no-op and `C = −1` is observationally identical to `C = 0`
(verified: first gear, `Te = −4.25`, `Φ = −10` → `Φ' = −10` for both). This matches the module's
pre-amendment inert behaviour and is **not** backed by retail evidence.

**Proposed testable statement (reviewer to accept, reword or reject).** Choose one:

- (A) *Boundary convention.* "When the drivetrain block's class copy is `−1`, or otherwise
  outside `{0,1,2,3}`, the S1-7.7 first-gear coefficient is `κC = 0.0` and the term leaves `Φ`
  unchanged." Add a vector asserting `Φ' = Φ` for `C = −1` under the `Φ < Te` and `Te + Φ < 0`
  guard. Provenance: stage-boundary convention (unset value is not a performance class); no
  retail claim.
- (B) *Residual unknown.* Mark the case as a new explicit unknown (e.g. `U-17`) recording that
  `−1` is reachable and its `κC` is unspecified/untested, and leave the current stub unratified.

Recommendation: (A) if the project accepts a boundary convention, otherwise (B). Either way the
spec text should stop leaving `−1` silently indexing `K`.

## Observations (not amendments)

- Testability limit: `test-vectors/README.md` states the drive score `S` is compared to `1e-3`,
  but no test (and no spec-defined routine) recomputes `S`; only the `S → C` mapping is checked.
  The `retail_stock`/`synthetic` `S` values are composition-step outputs (U-11) and were not
  independently reproduced by the verifier.
- Doc traceability: `IMPL_DESIGN.md` line 3 still names `61f14ebb…` as "Current behavioral input"
  while its PHY-01 paragraph names `355d67b4…` as the current approved hash. Behavioural content is
  gated by the PHY-01 paragraph; the earlier line is stale.

## Residual unknowns / untested

- Q-17 / A-17.1 above: behaviour for an unset class is unspecified.
- U-11 residual: whether a mid-session upgrade recomputes a live instance's class (verified only
  that the module does not recompute its own copy); the out-of-scope construction of `a(v)`.
- U-16: **no retail runtime differential was run** (no isolated environment; the original
  `speed2.exe` was not executed). All findings are static/derived. Absence of a runtime comparison
  is not evidence of retail parity.

---

# PHY-01 round 2 — 2026-10-17 (κC = K[C], S1-12.5 / U-17)

Verifier round for branch `phy-01-perf-class` after the spec revision that added S1-12.5 and
U-17 and changed the `C = -1` handling. Checked `SPEC.md` at
`224d71e4c74c211bfa8ed6ff5233ee1fde487a1c7181c940bdd0870570555adb` (matches the approved hash for
this round), `s1_kappa_c.csv`, `s1_perf_class.csv`, and `src/drivetrain.*`.

Result summary: no divergence was found in any **reachable** case. `make drivetrain-test` =
3813 checks / 0 failed; `make drive-stage1-test` passed; strict (`-Werror -pedantic`) and
ASan/UBSan runs clean on the drivetrain sources; an independent 181-check harness re-derived
all 65 `s1_perf_class.csv` rows, all 5 `s1_kappa_c.csv` rows, the strict boundaries, the forced
flag and the S1-7.7 first-gear term with `κC = 0.25`. The `C = -1` coefficient is exact binary32
`0x3E800000` (0.25) and the first-gear term returns `Φ' = -12.5` from `Φ = -10`, `Te = 0`.
U-17 is confirmed to be treated as residual (neither the implementation nor the vectors assert
reachability). Two representation/gap amendments are proposed below.

## Amendment A-17.2 (vector/spec representation gap): K[1..3] decimal vs binary32

**Finding.** S1-7.7 and S1-12.5 describe `K` as four values "stored contiguously in `C` order",
read with a base-plus-**4**·`C` stride and calling the `-1` slot a "4-byte slot"; `s1_kappa_c.csv`
declares tolerance **"exact binary32"**. But the vector's declared values for `C = 1,2,3`
(`-0.2000`, `-0.4000`, `-0.7000`) equal the double-precision literals (`atof`), **not** the
widened binary32 table constants:

| C | declared | widened binary32 | bit pattern |
|---|---|---|---|
| 1 | `-0.2000` | `-0.20000000298023224` | `0xBE4CCCCD` |
| 2 | `-0.4000` | `-0.40000000596046448` | `0xBECCCCCD` |
| 3 | `-0.7000` | `-0.69999998807907104` | `0xBF333333` |

`drv_kappa_c()` likewise stores double literals, so the first-gear coefficient and hence the term
follow the decimal reading (`Φ' = -8.0` for `C = 1`, not `-7.9999999702…`). The difference is
~1.5e-8 relative and normally disappears after the binary32 store of `feedback`, but:
- the vector does not test the "exact binary32" convention it declares (only the `C = -1` row has
  a bit-pattern assertion);
- the test compares with exact `double ==` against `atof`, so a faithful `(double)(float)K[c]`
  implementation would **fail** the current rows; and
- the task's requested "exact binary32" check for `K` therefore cannot pass as written.

**Proposed testable statement (reviewer to accept, reword or reject).** Choose one:

- (A) *Decimal definition.* State that `K` is defined by the real values `(0, -0.2, -0.4, -0.7)`
  used at double precision in the S1-7.7 product; drop the "exact binary32" tolerance for
  `C = 0..3` and keep it only for the `-1` slot; note the 4-byte table merely stores those
  constants. Keep the current rows as-is.
- (B) *Binary32 definition.* State that the coefficient used is the binary32 table entry widened
  to the product's working precision, and add bit-pattern assertions for `C = 0..3`
  (`0x00000000`, `0xBE4CCCCD`, `0xBECCCCCD`, `0xBF333333`) mirroring the existing `-1` row, with
  a vector convention that compares the widened binary32 value rather than the decimal literal.

Recommendation: (B) if fidelity to the executable's 4-byte static table is the goal (the
provenance says the values were "read from the executable's static data"); otherwise (A), which
makes the current implementation self-consistent but abandons the binary32 claim.

## Amendment A-17.3 (spec/implementation divergence): unguarded read vs NAN guard

**Finding.** S1-12.5 states the coefficient lookup "is a plain base-plus-4·C read of the four
contiguous `K` entries with **no bounds test**, no clamp and no default" and that "other values
outside `{0,1,2,3}` likewise read adjacent constants". `drv_kappa_c()` instead returns `NAN` for
any index outside `[-1,3]`. The class scheme produces only `-1` and `0..3`, so the divergent
indices are unreachable and no vector exercises them; the spec's own S1-12.5 already says only
the `-1` sentinel is reachable.

**Proposed testable statement (reviewer to accept, reword or reject).** Choose one:

- (A) *Narrow the specification.* Restrict the defined lookup to the reachable set
  `C ∈ {-1, 0, 1, 2, 3}` and mark all other indices unspecified / implementation-defined
  (to be reported as such). No new vector is required; this matches the implementation's intent
  and U-17's residual status.
- (B) *Reproduce the literal read.* Keep the "no bounds test" statement and pin the adjacent
  slots for at least `C = 4` and `C = -2` (requires reading the executable's adjacent static
  constants), adding vector rows analogous to `KAPPA_C_UNSET`.

Recommendation: (A) given the stated unreachability; (B) only if the project wants the literal
unguarded read reproduced and has the adjacent constants from static analysis.

## Observations (not amendments)

- **U-17 confirmed residual.** `s1_kappa_c.csv`'s `KAPPA_C_UNSET` row and the `drivetrain_test.c`
  attempt-0 comment both disclaim reachability; no test drives a normally-created instance to the
  first-gear clause with `C = -1`. The coefficient itself (`0.25`) is pinned, as S1-12.5 permits.
- **Storage claims re-confirmed.** `perf_class` is a signed byte in both the tune object and the
  drivetrain block; the block default `3` is overwritten by the creation-time copy including
  `-1`; mutation of `d.tune.perf_class` after creation does not recompute the copy (U-11 residual).
- **Testability limit.** `drive_score` for the `retail_stock`/`synthetic` rows is a
  composition-step output (U-11) and was not independently recomputed; only the `S → C` mapping
  those rows feed was checked. `S1-12.4`'s stock-class census (15 forced / 31 player) matches the
  vector.
- **Strict build note.** `drive_stage1_test` under `-Werror` fails only on pre-existing unused
  static helpers in `src/nfsu2.h` and `src/race.h` (suppressed by the Makefile); the drivetrain
  strict build is clean.

## Residual unknowns / untested (round 2)

- U-16: **no retail runtime differential was run** (no isolated environment; `speed2.exe` was not
  executed). All findings are static/derived. Absence of a runtime comparison is not evidence of
  retail parity.
- U-17: reachability of `C = -1` at the S1-7.7 clause remains unresolved.
- K[1..3] binary32 convention (A-17.2) and out-of-range indices (A-17.3) are untested by vectors.

---

# PHY-01 round 3 — 2026-10-17 (κC exact binary32; A-17.2 / A-17.3 closure)

Fresh verifier round on branch `phy-01-perf-class`. `SPEC.md` sha256 =
`f00c534c8b31a77a08287ee48c072a27559dcee4bb06b4a2747f8934c6efe5ed` (matches the currently-gated
hash in `REVIEW.md` line 273 and `IMPL_DESIGN.md`). Inspected `s1_kappa_c.csv`, `s1_perf_class.csv`,
`test-vectors/README.md`, SPEC S1-7.7/S1-12/U-11/U-14/U-17, `src/drivetrain.[ch]`,
`tools/drivetrain_test.c`.

## Result: no behavioural divergence found

All checks below pass; no new amendment is required. A-17.2 and A-17.3 from round 2 are **closed**
by the spec revision plus the vector/implementation changes; see the closure notes.

### Commands (verbatim counts)

| run | command | result |
|---|---|---|
| spec hash | `sha256sum docs/cleanroom-driving-physics/SPEC.md` | `f00c534c…e5ed` — matches approved/gated hash |
| unit/vector | `make drivetrain-test` | `PASS: 3823 checks, 0 failed` |
| integration | `make drive-stage1-test` | `Stage 1 gameplay adapter tests passed` (`launch 10 s: 135.74 km/h gear 4 rpm 4497`) |
| strict | `cc -std=c99 -Wall -Wextra -Werror -pedantic -Isrc tools/drivetrain_test.c src/drivetrain.c src/braking.c` | exit 0, no diagnostics |
| sanitizers | `-fsanitize=address,undefined` build+run of `drivetrain_test` (`ASAN_OPTIONS=detect_leaks=0`) | `PASS: 3823 checks, 0 failed`, no ASan/UBSan report |
| sanitizers (integration) | `-fsanitize=address,undefined` build+run of `drive_stage1_test` | passed, no report |

Strict `drive_stage1_test` also fails under `-Werror`, but **only** on pre-existing
`-Wmisleading-indentation`/`-Wunused-function` in `src/nfsu2.h` and `src/physics.c` (unchanged by
this branch); the drivetrain sources are clean, matching round 2.

### Independent κC bit-pattern and exact-value verification

Independent verifier harness (not the project tests) called `drv_kappa_c()` and re-derived the
binary32 words in Python. Both the CSV-declared values and the runtime return value are the exact
stored words:

| `C` | returned double | bits | declared CSV value | result |
|---:|---|---|---|---|
| `-1` | `0.25` | `0x3E800000` | `0.25` | OK |
| `0` | `0.0` | `0x00000000` | `0.0` | OK |
| `1` | `-0.20000000298023224` | `0xBE4CCCCD` | `-0.20000000298023224` | OK |
| `2` | `-0.40000000596046448` | `0xBECCCCCD` | `-0.4000000059604645` | OK |
| `3` | `-0.69999998807907104` | `0xBF333333` | `-0.699999988079071` | OK |

Each declared decimal equals `(double)(float)word` exactly (not the real decimal `-0.2/-0.4/-0.7`,
which differ by 1.49e-8/1.49e-8/1.70e-8 relative). Out-of-range indices (`-2`, `4`, and `-3`, `5`)
return NaN — i.e. they are **not** presented as specified behaviour, consistent with S1-12.5's
allowance to reject/ignore and U-17's "defined set `{-1,0,1,2,3}`" statement. No vector row
requires an out-of-range read.

### Independent re-derivation of the vectors

- `s1_perf_class.csv`: all 65 rows re-mapped independently (`S>500→0`, `>401→1`, `>273→2`, else 3;
  forced flag → 3); 0 mismatches. Stock census reproduces S1-12.4 exactly: 15 forced rows, 31
  player rows, of which 20 → class 3 and 11 → class 2.
- Strict boundaries re-derived and matched: `500.0001→0`, `500.0→1`, `401.0001→1`, `401.0→2`,
  `273.0001→2`, `273.0→3`, `0.0→3`, `-1.0→3`; forced short-circuit `drv_perf_class_resolve(flag≠0, NaN)=3`.
- S1-7.7 first-gear term driven through the real `drv_couple()` with an isolated harness
  (`φ₀=-10`, `Te=0`, contacts 0, mode open), confirming the class copy is read:
  `C=-1→-12.5`, `C=0→-10`, `C=1→-7.99999997…`, `C=2→-5.99999988…`, `C=3→-2.99999979…` (all within
  the U-14 1e-5 tolerance, matching `Q8-open-first-reaction` expected `-8`). The `φ≥Te` guard was
  also confirmed to skip the term.

### A-17.2 closure (K[1..3] decimal vs binary32)

Closed in the (B) direction. SPEC S1-7.7 now tabulates the four entries as binary32 bit patterns
and states the coefficient used is the stored word; `s1_kappa_c.csv` now carries the exact widened
values; `drv_kappa_c()` stores the exact words and `test_kappa_c()` checks value **and** bit
pattern. A faithful `(double)(float)word` implementation now matches the rows.

### A-17.3 closure (unguarded read vs guard)

Closed in the (A) direction. SPEC S1-12.5 now restricts the **defined** lookup to
`C ∈ {-1,0,1,2,3}` and states any other index is outside the specification and may be rejected or
ignored; `drv_kappa_c()` returns NaN for those indices, which the spec permits. No reachable state
produces them. (Round 2's literal-read option (B) was not taken; that is an accepted reviewer
choice, not a divergence.)

## Observations (not amendments)

- Minor wording tension only: S1-7.7 still says the `K[C]` read has "no bounds test, clamp or
  default", while S1-12.5 permits an implementation to reject out-of-range indices. Because
  S1-12.5 governs and the rejected indices are outside the defined set, this is not a behavioural
  divergence; the two sections could be reconciled editorially.
- U-11 residual unchanged: the `drive_score S` values (retail/synthetic rows) are composition-step
  outputs and were **not** independently recomputed; only the `S → C` mapping they feed was.
- Storage re-confirmed: `perf_class` is a signed byte in `DrvTune` (physics-parameter model,
  default `-1`) and the drivetrain block (constructor default `3`), and `drv_init()` overwrites the
  block copy unconditionally from the tune byte, including `-1` (S1-12/S1-12.5); mutating
  `d.tune.perf_class` after creation does not recompute the copy (U-11 residual).

## Residual unknowns / untested (round 3)

- **U-14**: the first-gear term result is **not** claimed bit-exact; only the coefficient is pinned.
  Confirmed in spec, README and vectors.
- **U-17**: reachability of `C = -1` at the S1-7.7 clause remains unresolved and is **not asserted**
  by the vectors/tests; only the coefficient (`0.25`) is pinned. The "amendment boundaries" fixture
  states explicitly it makes no reachability claim.
- **U-16**: **no retail runtime differential was run** (no isolated environment; `speed2.exe` was
  not executed). All findings are static/derived from the spec and vectors. Absence of a runtime
  comparison is not evidence of retail parity.
