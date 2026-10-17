# Driving physics Stage 1 review

## Scan result
Starting-point grep run over SPEC.md and test-vectors. No executable addresses, sub_/FUN_/LAB_/DAT_ names, mangled names, source paths, disassembly, pseudo-C, register sequences or original identifiers found. Remaining hex hits are allowed: binary32 bit patterns of numeric constants (Kr, Kw, 0.0044), the chunk id 0x00034600 and data-record offsets (+0x110, +0x220 ... +0x358 are offsets inside the game-data car record, cross-checked as data-format facts, not executable layout). Full read-through done; symbols (rho_S, zeta, etc.) are the analyst's own names.

## Changes
* SPEC S1-0.3 / U-14: removed reference to the CPU floating-point unit by name; now "higher than binary32 precision".
* SPEC S1-1.1: clarified 0x00034600 is a chunk id, not a file offset.
* SPEC S1-4.2: removed description of a disabled design-time switch in the original (original control-flow layout); kept the behavioural statement (do not implement as default).
* SPEC U-10: removed description of an original global sentinel value; now "unidentified global condition".
* test-vectors/s1_params.json: removed row_file_offset fields (locations in the owner's file; same call as docs/cleanroom-camera).
* test-vectors/s1_axle_split.csv: removed 2 rows for override modes 1/2 (deferred drag aid, not specified in SPEC, so untestable).
* test-vectors/README.md: removed pointer to analyst-only generator script and evidence hash file; added provenance paragraph; updated axle-split note.

## Testability / arithmetic check (recomputed by reviewer)
substeps (10, 60, 227.27, 240 Hz), all axle-split rows, all clutch rows incl. the 0.5 halving case and 300 clamp, SYNTH_RWD torque rows (u=1, 0.5, 0 gear, neutral x0.65, limiter x2) and the first two freerev substeps (omega 106.1086, 106.87; engagement 0.015625 steps) all match the SPEC formulas.
Limits: all vectors are derived from the spec, not captured (U-16), so they check implementation consistency, not retail fidelity. The [obs] tags mean observed in static analysis/data, not at runtime. Several mechanisms (S1-7.5 literal update, S1-7.7 slip-lock branch) have no vectors. Remaining unknowns U-1..U-16 are explicitly marked and acceptable.
SYNTH_RWD vectors (params, torque, clutch, freerev, axle, substeps) are self-sufficient for an implementer.

## Retail-derived vectors: recommendation (no deletion made)
PEUGOT/240SX/SKYLINE rows in s1_params.json and s1_engine_torque_* are small per-car derived facts (about 40 numbers/car), comparable to the camera spec's effective tuning tables: acceptable in my view. The s1_freerev_neutral_{PEUGOT,240SX,SKYLINE}.csv files (about 26 KB each) are computed outputs, not data dumps, but are the bulkiest retail-linked items; if the user wants a conservative stance, delete those three and keep SYNTH_RWD. Final call is the user's under CONTRIBUTING.md.

## Deviations / contamination notice
* Analyst ran the game under Wine once, not in a VM and with network not disabled (logged in PROVENANCE.md). No data from it was used. Not a contamination of the spec.
* Public notes (troikoss/nfsu2-re) were used as address leads only; no addresses reached the spec.
* No contamination has reached an implementer through this spec as reviewed.

## Approval
Date (UTC): 2026-10-15
Role: reviewer
SPEC.md sha256: 620bfb86586a98a62714be10c20fb4ca3f300bcbf2b6623b45700065e8e73311
(Any later edit to SPEC.md invalidates this approval.)


## Fresh question-amendment gate — 2026-10-15T13:35:25.760343+00:00

Role: reviewer (fresh cleanroom-reviewer context, driving_questions_review).
Scope: SPEC.md, QUESTIONS.md, test-vectors, REVIEW.md, PROVENANCE.md and HANDOFF.md.
Implementation, implementation design, and original executable were not accessed or run.
Analyst evidence for Q-1–Q-16 was consulted for traceability only.

### Scan and change categories
- Full specification and question read-through plus recursive contamination scan: no original
  executable addresses, recovered symbols, source paths, decompilation, disassembly, original comments,
  long original strings, assets or wholesale executable-table dumps found in the reviewed inputs.
  Bit patterns and car-data offsets/chunk ID are numeric/data-format facts; planar acceleration labels
  are mathematical notation. REVIEW.md's scanner vocabulary is historical review terminology.
- SPEC S1-3.3: replaced storage-oriented nitrous-scale notation with an independent descriptive symbol
  as a precaution. This is naming hygiene, not evidence of a recovered original identifier or code exposure.
- SPEC S1-4.5: clarified accepted-selection wording and explicitly left initial assist timer/remembered
  gear as supplied state with retail initialization unresolved under U-2.
- QUESTIONS.md: marked interim implementation choices as historical and published reviewed disposition.
- test-vectors/README.md: made landing-table tolerance consistent with U-14 and documented isolated-case
  entry conditions, assist timing and supplied branch state; omitted inputs are not retail defaults.
- HANDOFF.md: added current gate status and marked old implementation/testing and open-item notes historical.

### Testability and provenance
All 18 added synthetic cases were independently checked against the specification arithmetic or
predicates. CSVs were read in full for schema and finite numeric data (2,580 rows total); parameter JSON
was inspected. These checks are not implementation tests, runtime observations or differential verification.
The Q-4 example tests current-force acceleration and one correction; it does not test five-pass sequencing.
Q-14 has no dedicated lateral-curve vector because that curve is outside Stage 1. Q-2 uses the stated
ratio-first convention with intermediate rounding unresolved; exact comparison is not a retail oracle.
All vector families have provenance in test-vectors/README.md: retail data-field observations plus derived
formulas, or invented synthetic inputs plus derived formulas. No runtime-captured vector is claimed.
Small selected car-data parameter facts and computed outputs remain; no original asset is included.

Q-1, Q-3–Q-10, Q-12–Q-13 resolved within the Stage-1 valid input contract; Q-14–Q-15 resolved as supplied
later-stage boundaries. Q-2, Q-11 and Q-16 remain partial. U-1–U-16 remain explicit, including U-2 assist
initialization/menu consumption, U-14 precision and U-16 absence of runtime proof. Approval is a clean
behavioral-spec gate, not a claim of complete retail fidelity. No confirmed contamination exposure was
identified; no implementation quarantine is requested on the evidence reviewed.

### Approval (supersedes earlier hash)
Date (UTC): 2026-10-15T13:35:25.760343+00:00
Role: reviewer
Outcome: APPROVED for the stated Stage-1 boundaries and explicit unknowns.
SPEC.md sha256: 61f14ebb5c850fa8aa414ee20baae001fb64d4fff692e9a7d6698dbabce399bb
Any later edit to SPEC.md invalidates this approval.


## Fresh performance-class gate — 2026-10-17

Role: reviewer (fresh cleanroom-reviewer context, driving_perfclass_review).
Scope: docs/cleanroom-driving-physics/SPEC.md (new S1-12, revised S1-7.7, narrowed U-11),
QUESTIONS.md 2026-10-17 addendum, test-vectors/README.md and test-vectors/s1_perf_class.csv, REVIEW.md.
Implementation under `src/` was not accessed. Analyst evidence in
`cleanroom/analysis/stage1-perfclass/` was consulted for traceability only and not copied.

### Scan and change categories
- Recursive contamination scan plus full read-through: no original executable addresses, disassembly,
  decompiler pseudo-code, register sequences, recovered symbol/struct/field names, source paths, mangled
  names, original strings, assets or wholesale table dumps found in the reviewed inputs. Remaining hex
  hits are allowed numeric/data-format facts (binary32 bit patterns of constants; car-record size/chunk
  id), and the new `row + 0x844/+0x370/+0x220` figures are offsets inside the game-data car record, the
  same class already accepted in S1-1.
- Evidence-tag inconsistency (category: confidence tagging). S1-12 was the only place in the spec using
  `[obs]`; its values come from deterministic emulation of the composition step and from reading the
  executable's static data, with the game never launched (U-16). Retagged to match the header convention
  (all prior sections are `[inf]`) and reworded "observed by deterministic execution" to "derived";
  S1-12.4 heading "Observed stock classes" to "Stock classes".
- Wall pointer (category: analyst-only path in an implementer-visible file). test-vectors/README.md
  re-introduced a sentence pointing readers to `cleanroom/analysis/stage1-perfclass/`. Removed (the
  prior gate had removed a comparable pointer); replaced with neutral provenance text.
- Testability/unknown marking (category: under-specified requirement). S1-12.2's score loop integrates
  a drive-acceleration curve `a(v)` whose construction is not specified anywhere in the spec; the text
  previously implied it "belongs to the composition contract", a contract the spec does not contain.
  Marked the `a(v)` construction as out of Stage-1 scope in S1-12.2 and added it to the U-11 residual so
  the `S`-reproducibility boundary is explicit and not silently dropped. Added a matching testability
  note to test-vectors/README.md (the `retail_stock`/`synthetic` rows validate the class index given `S`,
  not an independent recomputation of `S`; the `rule` rows test the mapping alone).
- Provenance-note wording (category: confidence tagging). The trailing S1-12 note and the QUESTIONS
  addendum previously called the finding "observational"; reworded to static/derived with runtime
  explicitly excluded.

### Testability and provenance
- Verified against analyst evidence: the step `Δ`, threshold set `{0,273,401,500}`, coefficient table
  `{0,−0.2,−0.4,−0.7}` and the torque-scale sweep values match SPEC S1-12 and s1_perf_class.csv.
- Checked all s1_perf_class.csv rows: the `source = rule` boundary rows (500.0001→0, 500→1, 401.0001→1,
  401→2, 273.0001→2, 273→3, 0→3, −1→3) and every `retail_stock`/`synthetic` class index are consistent
  with the strict `>` mapping in S1-12.3. Class counts (31 player + 15 forced = 46 retail rows) match
  S1-12.4.
- Vectors are derived behavioral outputs backed by a formula, not a copy of an original data table;
  provenance is stated in test-vectors/README.md and no original asset is included.
- Residual unknowns are explicit: U-11 now records both the mid-session-recompute question and the
  out-of-scope `a(v)` construction. U-16 still records the absence of any retail runtime validation.

### Deviations / contamination notice
- No contamination reached an implementer through this spec as approved here. However, the
  `cleanroom/analysis/stage1-perfclass/` pointer was present in the implementer-visible
  test-vectors/README.md between the analyst's commit and this gate. If any implementer session read
  that README, it should be treated as a possible analyst-path exposure and logged in PROVENANCE.md; no
  analyst content itself was copied into spec/, so no code quarantine is requested on the evidence
  reviewed. Coordinator to confirm the implementer never followed the pointer.

### Approval (supersedes earlier hashes)
Date (UTC): 2026-10-17
Role: reviewer
Outcome: APPROVED for the stated Stage-1 boundaries, the S1-12 class mapping and the explicit unknowns.
SPEC.md sha256: 355d67b46fd713578d17800338469cecb450169ad8e746b1199dd5564de803d4
Any later edit to SPEC.md invalidates this approval.


## Fresh Q-17 class-copy gate — 2026-10-17

Role: reviewer (fresh cleanroom-reviewer context, driving_kappac_review).
Scope: SPEC.md (new S1-12.5, revised S1-7.7 sentence, new U-17), QUESTIONS.md 2026-10-17 Q-17 answer,
test-vectors/README.md and new test-vectors/s1_kappa_c.csv, REVIEW.md.
Implementation under `src/`, `tools/` and the Makefile were not accessed or run. Analyst evidence in
`cleanroom/analysis/stage1-perfclass/` (`notes.md` §7, `evidence/perfclass_cminus1.*`) was consulted for
traceability only and not copied.

### Scan and change categories
- Recursive contamination scan plus full read-through of all new material: no executable addresses,
  `sub_`/`FUN_`/`LAB_`/`DAT_`, disassembly, decompiler pseudo-code, register sequences, recovered
  symbol/struct/field names, source paths, mangled names, original strings, copied assets or wholesale
  table dumps found. Remaining hex hits are the allowed numeric/data-format facts already accepted at
  earlier gates: binary32 bit patterns of constants (`Kr`, `Kw`, `0.0044`, and the new `0x3E800000` for
  the `−1` coefficient) and game-data car-record offsets/chunk id. The `K` table and its adjacent `0.25`
  constant are stored-data facts, explicitly in scope; the statements about the `−1` coefficient use no
  code address.
- Confidence tagging: S1-12.5 and the trailing S1-12 provenance note tag their findings `[inf]` and
  exclude runtime observation (U-16); consistent with the header convention and the prior gate.
- Methodology pointer (category: reviewer hygiene, in an implementer-visible file). test-vectors/README.md
  named the analyst's reverse-engineering tool command in the `s1_kappa_c.csv` provenance. Replaced that
  clause with the neutral "static read of the constant pool". No behavioral content changed.
- No other rewrites were needed; the new statements were otherwise behavioral and correctly marked.

### Testability and provenance
- S1-7.7 now states the reference `κC = K[C]`, that the four entries are contiguous in `C` order, and that
  the read is unguarded (no bounds test, clamp or default) with out-of-range `C` reading adjacent stored
  constants. S1-12.5 pins the sentinel result as `κC = 0.25` (binary32 `0x3E800000`) for `C = −1`. Both
  are checkable against `s1_kappa_c.csv`, whose rows give `kappa_c` for `C = 0..3` and `C = −1` at exact
  binary32 tolerance.
- `s1_kappa_c.csv` `−1` row pins only the coefficient: its `status`/`note` state that the read is
  unguarded and that reachability of the state at the term is U-17. It does not assert that a simulated
  drivetrain reaches the clause.
- Reachability is carried as residual **U-17**, not asserted: U-17 records that the copy can hold `−1`,
  that the clause and its caller have no class/car-type gate, but that the normal spawn path resolves the
  class before the copy is taken and that no runtime trace exists (U-16). S1-12.5 similarly states the
  `0.25` result conditionally ("an implementation that returns `0` for `C = −1` diverges from the original
  if the state is reached").
- Verified against analyst evidence: the coefficient table `{0, −0.2, −0.4, −0.7}`, the adjacent
  `0.25` binary32 value, the unconditional sign-extended copy, the `−1` sentinel lifecycle, and the open
  reachability (F1–F8, `notes.md` §7) all match SPEC S1-12.5/U-17, QUESTIONS.md A-17.1 and the CSV note.
- Provenance of the new README section is consistent with the other vector families: values are derived
  from static data/emulation, the game was not launched, and no original asset or runtime capture is
  included.

### Deviations / contamination notice
- No contamination reached an implementer through this revision; no code quarantine is requested on the
  evidence reviewed. The prior gate's possible `cleanroom/analysis/stage1-perfclass/` README-pointer
  exposure remains a coordinator item for PROVENANCE.md and is unchanged by this gate.
- The A-17.1 disposition correctly rejects ratifying the interim `κC = 0.0` stub as a retail claim and
  adopts `0.25`; the implementer must update `drv_kappa_c()` for `C = −1` (outside this gate's scope).

### Approval (supersedes earlier hashes)
Date (UTC): 2026-10-17
Role: reviewer
Outcome: APPROVED for the stated Stage-1 boundaries, the S1-12.5 `−1` coefficient, the general out-of-range
read statement, and the explicit unknowns (U-17 reachability; U-16 no runtime trace).
SPEC.md sha256: 224d71e4c74c211bfa8ed6ff5233ee1fde487a1c7181c940bdd0870570555adb
Any later edit to SPEC.md invalidates this approval.


## Fresh κC-representation gate (A-17.2 / A-17.3) — 2026-10-17

Role: reviewer (fresh clean-room-reviewer context, driving_kappac_review round 3).
Scope: SPEC.md (revised S1-7.7 `K` table/precision/defined-index set; revised S1-12.2 / S1-12.5 / U-14 /
U-17), QUESTIONS.md Q-17 representation addendum, test-vectors/README.md κC section and
test-vectors/s1_kappa_c.csv, REVIEW.md. Implementation under `src/`, `tools/` and the Makefile were
not accessed or run. Analyst evidence in `cleanroom/analysis/stage1-perfclass/` (`notes.md` §8,
`evidence/perfclass_kappa_repr.*`) was consulted for traceability only and not copied.

### Scan and change categories
- Recursive contamination scan plus full read-through of all new material: no executable addresses,
  `sub_`/`FUN_`/`LAB_`/`DAT_`, disassembly, decompiler pseudo-code, register sequences, recovered
  symbol/struct/field names, source paths, mangled names, original strings, copied assets or wholesale
  table dumps reached the reviewed files. The only hex strings are the accepted data-format facts:
  binary32 bit patterns of stored constants (`Kr`, `Kw`, `0.0044`, the `K`/`0.25` pool words) and
  game-data car-record offsets/chunk id. The stored `K` values and their neighbours are stored *data*
  constants, which the task explicitly allows; no code address appears. The analyst-only pointers/tool
  command removed at earlier gates did not reappear (`cleanroom/analysis/`, `ghidra`, `notes.md`,
  `evidence/`, `scripts/` absent from all four reviewed files).
- Hardware naming (category: prior-gate hygiene regression). S1-7.7's new precision sentence re-named the
  original's floating-point unit ("default FPU significand"), a form the first gate had deliberately
  removed. Reworded to "default working significand is 53 bits, i.e. double"; the behaviourally relevant
  precision fact (wider than binary32 = double) is unchanged and still justifies the U-14 tolerance.
- Confidence tagging (category: internal tag inconsistency). S1-12.5's traceability paragraph was the
  spec's only `[obs]` claim and contradicted the section's own closing note ("the tags are [inf]") and
  the prior performance-class gate's convention. Retagged to `[inf]`. The paragraph is explicitly
  non-normative ("not requirements"), so no behaviour changed. The header definition of `[obs]` is
  untouched.

### Testability and provenance
- S1-7.7 now pins the four `K` entries as exact binary32 words with their bit patterns and real values,
  states the contiguous `C`-order layout, the `(Ψ·κC)·(Te+Φ)+Φ` grouping and the wider (double) working
  precision, and restricts the defined index set to `{−1,0,1,2,3}`. S1-12.5 pins `κC = 0.25`
  (`0x3E800000`) for the `−1` sentinel and marks every other index out of the specification (never
  produced; may be rejected/ignored). These are checkable against `test-vectors/s1_kappa_c.csv`.
- Independently round-tripped `s1_kappa_c.csv`: `0.0→0x00000000`, `−0.20000000298023224→0xBE4CCCCD`,
  `−0.4000000059604645→0xBECCCCCD`, `−0.699999988079071→0xBF333333`, `0.25→0x3E800000`. Each CSV decimal
  parses to a double that equals the exact real value of the stated binary32 word
  (`−13421773/67108864`, `−13421773/33554432`, `−11744051/16777216`, `1/4`), matching SPEC's exact
  expansions. The README's stored-value table is consistent with the CSV.
- The README no longer overclaims bit-exact term results: the tolerance row and the κC section both state
  "coefficient exact (stored binary32); S1-7.7 term result per U-14", matching U-14's widened-operand /
  binary32-store statement. The `−1` row fixes the coefficient only and leaves the reachability of the
  state as residual U-17.
- Verified against analyst evidence: the `K` byte layout, the four exact values, the adjacent `0.25`
  word, the `(Ψ·K[C])·(Te+Φ)+Φ_old` operand widths, and the 53-bit working significand in
  `perfclass_kappa_repr.*` all agree with SPEC S1-7.7 / S1-12.5 / U-14 / U-17 and QUESTIONS.md A-17.2.
- Residual unknowns remain explicit and marked: U-14 (intermediate rounding; vectors carry tolerances),
  U-16 (no retail runtime validation), U-17 (whether a simulated drivetrain reaches the first-gear
  clause with the `−1` copy). Minor non-blocking observation (no change made): the CSV `status` for the
  `−1` row reads "out-of-range sentinel" while the spec classifies `−1` as a *defined* input; the adjacent
  `note` disambiguates and the tested `kappa_c` value (`0.25`) is normative, so this is terminology only.

### Deviations / contamination notice
- No contamination reached an implementer through this revision; no code quarantine is requested on the
  evidence reviewed. No original runtime trace exists and none is claimed. The prior gate's possible
  `cleanroom/analysis/stage1-perfclass/` README-pointer exposure remains a coordinator item for
  PROVENANCE.md and is unchanged by this gate.
- The `0`-valued interim `drv_kappa_c()` stub for out-of-range classes remains the implementer's choice;
  the specification now requires `0.25` for `C = −1` (outside this gate's scope).

### Approval (supersedes earlier hashes)
Date (UTC): 2026-10-17
Role: reviewer
Outcome: APPROVED for the stated Stage-1 boundaries, the exact `K` binary32 table and defined index set,
the `κC = 0.25` `−1` coefficient, and the explicit unknowns (U-14 tolerance; U-16 no runtime trace;
U-17 reachability).
SPEC.md sha256: f00c534c8b31a77a08287ee48c072a27559dcee4bb06b4a2747f8934c6efe5ed
Any later edit to SPEC.md invalidates this approval.
