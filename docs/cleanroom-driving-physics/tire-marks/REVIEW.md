# Tire marks review

## Scan result
Starting-point grep over SPEC.md, QUESTIONS.md, test-vectors: no addresses, sub_/FUN_/LAB_/DAT_, mangled names, source paths, register or disassembly fragments. Hex hits are allowed: car-record data offsets (+0x120/+0x134 etc., data-format facts), and the three 32-bit texture name hashes with their asset names (functionally required lookup keys, flagged in TM-1.3). Full read-through done; symbols (k_s, q, kappa, etc.) are analyst's own. No original text/assets reproduced; texture pixels are not included.

## Changes
* SPEC TM-11.3: removed description of how the original's helper call is structured (code-structure detail); kept the behavioural formula and the "no practical fade" conclusion.
* SPEC TM-U6: removed reference to helper argument order; now "single static reading".
* SPEC TM-U11: removed CPU floating-point unit name; now "intermediate precision may exceed binary32".
* test-vectors/README.md: same x87 wording removed.

## Testability / vectors
Requirements are behavioural with [obs]/[inf]/[gue] tags and 12 flagged unknowns (TM-U1..U12; mirrored in QUESTIONS.md). Vectors are declared synthetic consequences of the spec (README + _provenance), none a runtime capture, no copyrighted assets. Reviewer re-checked: cell_keys (all rows recomputed from TM-10.2), render_alpha (TM-11.2), view_metric (TM-11.3, K=355.606; p and F values), and the 240SX quantisation example (w=.1025 -> 6/64). All agree.
Note for implementer: vectors only prove self-consistency with the spec text; the [inf] claims (lay thresholds, 8 kN, 128/64 capacities, alpha unity) are unverified against runtime.

## Contamination note
No contamination reached the implementer from this spec (no implementer had read it before review). PROVENANCE.md mentions the analyst's decompiled-project path as intake provenance only; not spec content.

## Approval
Date (UTC): 2026-10-15
Role: reviewer
SPEC.md sha256: 99b7079559ee599a861ac3baa0a107e2623be9097d1494c06f3605af38b242b5
test-vectors/tire_marks.json sha256: 1f18c4b4b0f299711dc68a291528533b84ab42dee213b10081a6ba14ed2e2633
Any later edit to SPEC.md invalidates approval.

## Amendment review: TM-15 render state (2026-10-16)
Scan: starting-point grep plus targeted grep for effect/technique/shader-language tokens and absolute paths over SPEC.md, QUESTIONS.md, test-vectors: nothing found. Hex hits are only the pre-approved texture name hashes and data offsets. Full read of TM-11.2, TM-14, TM-15.1-15.7, TM-U4: shader behaviour is described as formulas in neutral terms (no effect/technique/pass names, no shader code, no register-level steps, no addresses); fog constants appear only as generic (Fx, Fy, Fz). Texture data is numeric statistics only (alpha min/max/mean, mean RGB), no pixels or palettes.
Changes: none required (no contamination found); no text removed.
Recomputation (independent script): render_state.json effective_alpha (9 rows: vA = max(Ib-F,0)>>1, a = tA/255*vA/255), source_colour (5 rows, min(1, 2*t/255*128/255)), composite (5 rows, 1/2/4 layers over 0.5 grey, src*a + dst*(1-a) repeated) all agree with the file within 1e-6 and with the spec text (0.24804, 0.22297, 0.07422, 0.08203, 0.19922, 0.17621; 0.384 / 0.297 / 0.182). Vertex alpha mapping 255->127, 76->38, 85->42 agrees with TM-11.2.
Testability: TM-15 requirements are formula/state-table based with [obs]/[inf] tags; residuals listed in TM-U4. Vectors declared synthetic (render_state.json _provenance, README); no copyrighted assets.
Notes for coordinator: (1) TM-15 contradicts the existing implementation claim of alpha x2 (VERIFIER_AMENDMENTS V9, QUESTIONS TM-IQ3): spec now says alpha x1 (127 -> 0.498), colour x2 with RGB 128. Implementation needs update; no spec contamination reached the implementer via this amendment as far as the reviewer knows. (2) QUESTIONS.md TM-IQ3 text is stale relative to TM-15. (3) PROVENANCE names the Wine d3dx9 source as an analyst parser reference only; nothing copied.
Approval (amendment)
Date (UTC): 2026-10-16
Role: reviewer
SPEC.md sha256: 1b864fc16a007d8e6d6736bf8615b469ec975abc695620bf7a1bbbfd784ff454
test-vectors/render_state.json sha256: 5aa0eace8e1f55a3509211c3e7486efa81bd121252b181d761a40abc86de7e25
test-vectors/README.md sha256: 4eae6ec4bf1796d279603295afb293ac9d0e6540ce5e798758a074c992a4cb11
Any later edit to these files invalidates approval. Supersedes SPEC hash 99b70795...; hand-off hash 1b864fc1... is the one reviewed.

## Amendment review: TM-U2 gate `G` + TM-U3 suppression mask (2026-10-17)

Scope: analyst amendment to SPEC.md TM-2/TM-3/TM-U2/TM-U3 and QUESTIONS.md TM-Q2/TM-Q3/TM-IQ9.
Pre-amendment approved SPEC.md was 1b864fc1...; this edit invalidates it.

Scan: starting-point grep plus targeted grep (code tokens, `.cpp`/effect names, track/asset
names) over SPEC.md, QUESTIONS.md, test-vectors, then a full read of the amended sections and
the surrounding text. No executable addresses, `sub_`/`FUN_`/`LAB_`/`DAT_`, mangled names,
`.pdb`/`.cpp`/`.h` paths, decompiler fragments, pseudo-C, register sequences, or copied
original identifiers in SPEC.md or the vectors. Hex hits remain only the pre-approved
car-record data offsets and the three flagged texture name hashes. The new prose is
behavioural (three named gate predicates; a mask stated as per-vehicle state with no writer).

Changes required (reviewer edits):
* Category: track/event name not flagged — SPEC.md TM-2 clause 3 and TM-U2, QUESTIONS.md TM-Q2/TM-IQ9. The public venue block is identified by its retail event label; retained for traceability but now explicitly flagged as a public in-game event name (the operative contract is the numeric range [4400, 4499], not the label). Nothing sensitive removed, no text reproduced beyond the label itself, which is player-visible retail content already precedented (car model names, surface labels).
* Category: code-structure wording — SPEC.md TM-3: dropped the word "adjacent" describing the mask/enable memory layout; the enable flag is now described behaviourally as "a single per-vehicle enable flag". The fact that both fields are vehicle state is kept (it is the substance of the TM-U3 answer, not a layout leak).
No other changes; no addresses, symbols or fragments to scrub.

TM-U2 / TM-U3 statement check: both are plain predicates/facts, not code structure.
* `G` = (wheel-effects enabled for this vehicle) AND (not special mover mode) AND (venue id
  outside [4400, 4499]); each term is behavioural and independently testable (no marks while
  effects suspended / during pre-race start; none during vehicle (re)placement; none on the
  reserved venue block for any car). Tags: [inf], ids/range [obs]. Residual marked in TM-U2.
* Suppression mask = per-vehicle four-bit state, zeroed at creation, touched only by the
  state save/load path; no in-game writer, so all-clear default is correct. Testable
  consequence: the mask never suppresses a mark in ordinary play. Residual marked in TM-U3.
Arithmetic cross-check: the reserved interval is a half-open test `id < 4400` / `id < 4500`,
i.e. [4400, 4499] inclusive as stated; free-roam id 4000 lies outside. Traceable to analyst
evidence TM-E24..TM-E28 (analyst-only), which the reviewer read for traceability only.

Testability / vectors: no new or changed vectors this round — `tire_marks.json`,
`render_state.json`, `README.md` hashes are byte-identical to the previous approval
(1f18c4b4..., 5aa0eace..., 4eae6ec4...). Re-verified: `lay_predicate` covers the
per-wheel suppression bit (`masked` true → no lay) and the strict boundaries; re-derived
`render_state.json` `effective_alpha` (vA = max(Ib−F,0)>>1; a = tA/255·vA/255) and the
1/2/4-layer composites (0.384093/0.296937/0.182117) — all agree. Gap noted (not blocking):
no vector exercises gate `G` (a three-term conjunction); a truth-table vector would be a
cheap future addition. Residuals for both answers are explicitly marked in TM-U2/TM-U3.

Contamination to implementer: none from this amendment as far as the reviewer can determine.
The amendment adds no code, addresses or assets. QUESTIONS.md retains the earlier stale
TM-IQ3 wording (pre-TM-15) noted in the previous review; it does not leak new material.

Approval (amendment)
Date (UTC): 2026-10-17
Role: reviewer
SPEC.md sha256: b8fddf79783ac9ce4cdc82874806b3152c574d9f2297b05f13277ae6da79a117
QUESTIONS.md sha256: 519e81f917e48398dec0366c807730f0167242718d0094e217f86531ecf18e68
test-vectors/tire_marks.json sha256: 1f18c4b4b0f299711dc68a291528533b84ab42dee213b10081a6ba14ed2e2633 (unchanged)
test-vectors/render_state.json sha256: 5aa0eace8e1f55a3509211c3e7486efa81bd121252b181d761a40abc86de7e25 (unchanged)
test-vectors/README.md sha256: 4eae6ec4bf1796d279603295afb293ac9d0e6540ce5e798758a074c992a4cb11 (unchanged)
Any later edit to these files invalidates this approval. Implementers must use SPEC.md sha256 b8fddf79... .

## Amendment review: F1–F5 resolution — clause-1 session conjunct, per-vehicle gate `G`, `vehicle_gate` vectors (2026-10-16 UTC; round labelled 2026-10-17 local)

Scope: fresh analyst amendment to SPEC.md TM-2 (clause 1 four-conjunct `E` with the
pre-race start/countdown suspended, and the per-vehicle scope paragraph), TM-3 (mask/enable
re-stated behaviourally), TM-12 annotation, TM-U1/TM-U2/TM-U3; QUESTIONS.md TM-Q2/TM-Q3/TM-IQ9;
test-vectors/tire_marks.json (new `vehicle_gate`) and README.md. Supersedes the b8fddf79…
approval, which this edit invalidates.

Scan: starting-point grep (addresses, `sub_`/`FUN_`/`LAB_`/`DAT_`, mangled names,
`.pdb`/`.cpp`/`.h` paths, register / `@@`) over SPEC.md, QUESTIONS.md, test-vectors/; targeted
grep for code-structure tokens (`ESI`/`EBX`/`EBP`, `+0x998`, `+0x550`, `0x84`, `MeloMvr`,
`VehicleMover`, `HasWheelEffects`, `UpdateEffects`, `FUN_00`, vtable, xref,
decompil/disassembl, `speed2.exe`); then a full read of the amended sections and of all of
SPEC.md. Findings:
- Hex/data hits in the deliverables remain only the pre-approved car-record/format offsets
  (`+0x120`/`+0x134`, `0x7c`/`0x80`/`0x11`) and the three texture name hashes flagged in TM-1.3
  (functionally required lookup keys).
- No executable addresses, symbols, mangled names, original source paths, decompiler/pseudo-C
  fragments or register sequences in SPEC.md, QUESTIONS.md or the vectors. The amended prose is
  behavioural: three named gate predicates and a per-vehicle mask stated as save/load state.
- Only false positive: TM-10.2 uses `cx`/`cy` as cell-coordinate variables (matched by the
  `[abcd]x` register pattern), not registers. Left unchanged.
- Non-deliverable note: `PROVENANCE.md` (process/honesty log, not part of the approved set)
  names the coordinator's decompiled-project path with a `.cpp` recovered-routine filename; no
  decompiled content or original identifier beyond that filename. Excluded from the approved
  artefacts; recommendation: keep `PROVENANCE.md` out of implementer-visible material.
- Host-side references in the QUESTIONS.md "implementer questions" sections (`src/nfsu2.h`,
  `IMPL_DESIGN.md`, host function names) are the clean-room project's own tree, not recovered
  original artefacts; not contamination. Noted, not removed.

Changes required: **none** (no contamination found); no text removed, so no change categories
to log beyond the review above.

Testability / statement check:
- TM-2 clause 1 is a plain per-vehicle predicate: `E` = (wheel-effects flag set) ∧ (not
  session-suppressed) ∧ (wheel collider present) ∧ (mover not in the no-effects context);
  false during vehicle setup/teardown/despawn and during the pre-race start **including the
  countdown**; true during ordinary racing. Residual (paused/finished/replay; exact race
  types) marked in TM-U2.
- TM-2 per-vehicle scope: `G` is evaluated per vehicle; a non-player car with `G` true lays a
  strip exactly as the player's; residual (role↔mover-class mapping, whether all clauses apply
  in every class, ambient traffic) marked.
- Both are facts/predicates, not code structure; the new unknowns are residuals, no new open
  items.

Vectors:
- `vehicle_gate` = 21 rows: the 2×2×4 truth table over clause1×clause2×venue{4399,4400,4499,4500}
  (16) plus free roam 4000, circuit 1200, pre-race countdown, special mover mode and non-player
  (5). Independently re-derived from TM-2 (`clause3 = venue ∉ [4400,4499]`,
  `lays = clause1 ∧ clause2 ∧ clause3`): **21/21 agree**, truth table complete (no missing
  combo).
- Unchanged sections re-verified: `lay_predicate` 9/9, `render_alpha` 6/6, `view_metric` 8/8,
  `cell_keys` 5/5, and `render_state.json` (`effective_alpha`, `source_colour`, `composite`)
  — all agree within 1e-6.
- Provenance declared synthetic (README + `_provenance`); not runtime captures; no copyrighted
  assets/pixels; structure matches the file's per-section input→output schema (output key
  `lays` is section-local and documented).

Contamination to implementer: none from this amendment. The deliverables carry no original
code, addresses or assets. No evidence that contaminated spec material reached an implementer.

Approval
Date (UTC): 2026-10-16 (round local 2026-10-17)
Role: reviewer
SPEC.md sha256: 5b968486f48d970e7872848ae70d113eafa18082104307e4137e40bdd28276e0
QUESTIONS.md sha256: b3ff97026493945ce3b4b6474e989136337379f01b7f18f3c04a156c08f3c53e
test-vectors/tire_marks.json sha256: e2be25a37aa2469377766dbc913727d396b17403c5301d873fbd4ea51407194d
test-vectors/README.md sha256: 17bbd1248c4a1c417f3dd796aa8e6a26f401c569b911ff5303c859c04b0cd858
test-vectors/render_state.json sha256: 5aa0eace8e1f55a3509211c3e7486efa81bd121252b181d761a40abc86de7e25 (unchanged)
Any later edit to these files invalidates this approval. Implementers must use SPEC.md sha256
5b968486… .
F1 note: the recorded QUESTIONS.md hash is the current working-tree file (extant on disk) but
is not yet a committed git object; the coordinator should commit it so the recorded hash
resolves to an extant blob.
