# Provenance — driving physics clean-room project

- Branch: `driving-physics` (from `development` 520c3f5).
- Purpose: interoperability/preservation. Recreate the retail PC game's car driving
  physics (longitudinal first, then lateral) in OpenUG2 from observed behaviour, without guessing.
  Current `src/physics.c` is an arcade approximation and is NOT a source of truth.
- Authorization: user owns a legal NFSU2 install and requested the analysis (2026-10-15).
- Subject: retail `speed2.exe`, 4 800 512 bytes, sha256
  `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7` (same binary as docs/cleanroom-settings).
- Layout: analyst-only scratch in `/cleanroom/` (git-ignored: binary-derived notes, traces, scripts);
  reviewed spec, vectors, review log and provenance in this folder; implementation in `src/`.
- Roles (fresh context each): analyst → reviewer (hash gate) → implementer → verifier.
- Staging: S1 acceleration → S2 resistance/top speed → S3 braking → S4 steering/lateral grip → S5 handbrake/weight transfer/surface.
  Each stage = separate spec section + vectors + approved hash + implementation + verification.

## Role log
(append: date, role, agent, stage, approved hash)
- 2026-10-15 ANALYST (cleanroom-analyst, fresh) Stage 1 acceleration: static analysis + data files. Also launched
  speed2.exe once under Wine 10 (copy of user's prefix in cleanroom/analysis/runtime/prefix, headless gamescope, audio off,
  memory read-only harness) to try to capture rpm/gear/speed; reached only the profile-load screen, no data captured or used.
  NOT run in a VM/snapshot and network was NOT disabled (deviation from workflow safety guidance). No writes to the game
  folder or original prefix observed. Public notes from troikoss/nfsu2-re were used as address leads only.
  SPEC.md sha256 at hand-back: a6254518ec49d3d5a8fa474ea349b30a3f0d6f8a69606fb4050f9e1efcdc7308 (unreviewed).
- 2026-10-15 REVIEWER (cleanroom-reviewer, fresh) Stage 1: APPROVED, SPEC.md sha256 620bfb86586a98a62714be10c20fb4ca3f300bcbf2b6623b45700065e8e73311 (see REVIEW.md).

- 2026-10-15 ANALYST (fresh cleanroom-analyst, driving_questions_analyst): examined Q-1–Q-16,
  original executable hash verified equal to recorded intake. Used existing analyst Ghidra evidence
  and direct static byte disassembly; no retail process launched, no runtime captures claimed.
  Amended SPEC.md and question statuses; added explicitly derived synthetic regression examples.
  Q-2 intermediate rounding, Q-11 general rear-grip context, Q-16 end-to-end timing remain partial.
  No implementation files or IMPL_DESIGN.md accessed. Evidence/commands under
  cleanroom/analysis/evidence/questions-2026-10-15/ and cleanroom/analysis/scripts/questions_*.py.
  Prior spec approval is stale; fresh reviewer gate and later runtime verification are required.
  Analyst hand-back SPEC.md SHA-256: `202c817768242d63fa0533eae4f6c7873ea7e2efb4dfb8e36322869ce6c6ceb2` (unreviewed).

- 2026-10-15T13:35:25.760343+00:00 REVIEWER (fresh cleanroom-reviewer, driving_questions_review): reviewed Q-1–Q-16
  amendments and all implementer-visible spec/vector/shared documents except IMPL_DESIGN.md.
  Consulted analyst-only findings for traceability; no implementation or executable run/read.
  Naming hygiene, explicit assist initialization unknown, vector tolerances/entry conditions and historical
  handoff status clarified. All 18 synthetic question cases checked against spec arithmetic/predicates;
  no runtime or differential verification claimed. No confirmed contamination incident identified.
  APPROVED SPEC.md SHA-256: `61f14ebb5c850fa8aa414ee20baae001fb64d4fff692e9a7d6698dbabce399bb`. Q-2, Q-11, Q-16 partial; U-1–U-16 retained.

- 2026-10-15 IMPLEMENTER (fresh cleanroom-implementer, driving_amendment_implementation), coordinator-relayed report:
  worked from approved hash `61f14ebb5c850fa8aa414ee20baae001fb64d4fff692e9a7d6698dbabce399bb`;
  reported changing only src/drivetrain.c, src/drivetrain.h, tools/drivetrain_test.c and IMPL_DESIGN.md,
  with no forbidden access. Reported 3,606 checks, strict C99, ASan/UBSan passing and LeakSanitizer
  unavailable due to the ptrace environment. Access history and changed-file scope are implementer-reported,
  not independently established by this verifier.
- 2026-10-15 VERIFIER (fresh cleanroom-verifier, driving_amendment_verification): independently checked
  approved spec hash, reference intake hash, all 3,606 harness checks, strict C99 compilation and
  ASan/UBSan execution (leak detection disabled), plus 40,011 independent deterministic numerical property
  cases. No implementation/spec edits, no original executable launch, no disassembly inspected.
  Harmless namespace isolation probe failed; runtime differential testing unperformed. Vectors remain
  spec-derived, not runtime captures. No behavioral amendments proposed and no new contamination observed
  in this context. Reports: VERIFICATION.md, VERIFIER_AMENDMENTS.md; logs in cleanroom/verification/.

- 2026-10-15 IMPLEMENTER (fresh cleanroom-implementer, `drivetrain_game_integration`),
  coordinator-relayed integration role: approved Stage-1 hash
  `61f14ebb5c850fa8aa414ee20baae001fb64d4fff692e9a7d6698dbabce399bb` and explicitly
  permitted independent application baseline. Access history is role-reported;
  no original/analyst access reported. See IMPL_DESIGN.md for implementation scope.
- 2026-10-15 VERIFIER (fresh cleanroom-verifier, `drivetrain_integration_verifier`):
  hash independently matched; four focused test targets passed (3,606 approved
  checks), plus 20,066 independent synthetic integration checks in strict project
  warning policy and ASan/UBSan (leak detection off). Native 120-tick Stage-1/HUD
  smoke exited 0 and endpoint image inspected; art decode limitation recorded.
  No implementation/spec edits, no original executable/Wine/disassembly or analyst
  evidence accessed. Retail differential unperformed; no behavioral divergence or
  spec amendment proposed. See INTEGRATION_VERIFICATION.md and ignored
  cleanroom/verification/integration/ for reproduction. No new contamination observed.

- 2026-10-15 COORDINATOR: prepared Stage-1 publication commit f9cc1ce. Normalized
  CSV fixture line endings to LF without changing values; all 3,606 drivetrain checks passed.
  Analyst scratch remains ignored. A fresh cleanroom-implementer, stage1_upstream_merge,
   resolved independent project integration conflicts with upstream development 961f783
   in Makefile and src/main.c only; approved Stage-1 specification remains unchanged.

- 2026-10-17 ANALYST (fresh cleanroom-analyst, PHY-01 `perf_class`): resolved unknown U-11
  (performance class C) by static analysis of the reference binary only (no retail runtime).
  Added S1-12 to SPEC.md, narrowed U-11, added a QUESTIONS.md addendum and test vectors
  `test-vectors/s1_perf_class.csv` (+ README row). Evidence kept analyst-only in
  `cleanroom/analysis/stage1-perfclass/` (`notes.md`, `evidence/`, `scripts/`). Reference hash
  confirmed unchanged (`f9dd86c0…468a7`); no implementation or decompilation content placed in the spec.

- 2026-10-17 REVIEWER (fresh cleanroom-reviewer, PHY-01 spec gate): scanned the new S1-12 /
  S1-7.7 / U-11 material and vectors; retagged confidence markers to the project convention
  ([inf], "derived"), removed an analyst-only path re-introduced into the implementer-visible
  `test-vectors/README.md`, marked the `a(v)` sample construction as out of Stage-1 scope, and
  appended the gate to REVIEW.md. APPROVED SPEC.md SHA-256:
  `355d67b46fd713578d17800338469cecb450169ad8e746b1199dd5564de803d4`.
  Contamination notice: the transient analyst-path pointer existed only in the reviewed README;
  no implementer session had run yet, so no implementer exposure occurred. Recorded for audit.

- 2026-10-17 IMPLEMENTER (fresh cleanroom-implementer, PHY-01): worked from approved hash
  `355d67b4…03d4`; changed only `src/drivetrain.c`, `src/drivetrain.h`, `tools/drivetrain_test.c`,
  `docs/cleanroom-driving-physics/IMPL_DESIGN.md`, and added Q-17 to QUESTIONS.md. Reported
  `make drivetrain-test` 3,804 checks / 0 failed, strict C99 clean, ASan/UBSan clean, `make normal`
  and `make drive-stage1-test` exit 0. No original/analyst access reported.

- 2026-10-17 VERIFIER (fresh cleanroom-verifier, PHY-01): confirmed approved hash `355d67b4…03d4`,
  reproduced 3,804 checks (strict C99 + ASan/UBSan) and 225 independent vector/property checks with
  no divergence against any defined S1-12 / S1-7.7 statement. Proposed amendment A-17.1 for Q-17
  (coefficient undefined at class −1). No retail differential (U-16); `S` values not independently
  recomputed (composition step, U-11). No implementation/spec edits. See VERIFIER_AMENDMENTS.md.

- 2026-10-17 ANALYST (fresh cleanroom-analyst, PHY-01 Q-17): resolved the class −1 coefficient by
  static analysis; added S1-12.5 and residual U-17 to SPEC.md, recorded the answer in QUESTIONS.md,
  and added `test-vectors/s1_kappa_c.csv`. Reference hash unchanged (`f9dd86c0…468a7`).
  CONTAMINATION NOTE (incidental, low impact): a repo-wide grep over the docs tree returned several
  `docs/cleanroom-driving-physics/IMPL_DESIGN.md` lines (an implementer design note) to the analyst
  context. The analyst reports it did not open that file and did not read `src/`/`tools/`/`Makefile`;
  the surfaced lines restated facts already disclosed in QUESTIONS.md Q-17 and VERIFIER_AMENDMENTS.md
  A-17.1, so no new implementation information was exposed. No implementer exposure occurred; no code
   quarantine required. Recommendation recorded: exclude implementer design notes from repo-wide greps.

- 2026-10-17 REVIEWER (fresh cleanroom-reviewer, PHY-01 Q-17 gate): approved the S1-12.5 / S1-7.7 /
  U-17 amendment; trimmed an analyst RE-tool command name from the vectors README; APPROVED SPEC.md
  SHA-256 `224d71e4c74c211bfa8ed6ff5233ee1fde487a1c7181c940bdd0870570555adb`. no implementer exposure.

- 2026-10-17 IMPLEMENTER (fresh cleanroom-implementer, PHY-01 Q-17 fix): worked from approved hash
  `224d71e4…55adb`; changed `src/drivetrain.c` (`drv_kappa_c` slot table: `C=−1 → 0.25`), `src/drivetrain.h`,
  `tools/drivetrain_test.c` (loads `s1_kappa_c.csv`), and `IMPL_DESIGN.md` (stale-hash fix). Reported
  `make drivetrain-test` 3,813 checks / 0 failed, strict C99 + ASan/UBSan clean, `make normal` and
  `make drive-stage1-test` exit 0. No original/analyst access reported.

- 2026-10-17 VERIFIER (fresh cleanroom-verifier, PHY-01 round 2): confirmed hash `224d71e4…55adb`,
  reproduced 3,813 checks (strict + ASan/UBSan) and 181 independent checks; all five `s1_kappa_c`
  rows and the first-gear term reproduced; U-17 confirmed residual. Proposed A-17.2 (`K` decimal
  double literals vs the vector's "exact binary32" claim) and A-17.3 (out-of-range beyond `−1`
  asserted but implemented as `NAN`). No retail differential (U-16). See VERIFIER_AMENDMENTS.md.

- 2026-10-17 ANALYST (fresh cleanroom-analyst, PHY-01 A-17.2/A-17.3): pinned `K` and the `C=−1`
  coefficient to their exact stored binary32 values and narrowed S1-12.5 to the defined index set;
  updated `s1_kappa_c.csv` (exact stored values + bit patterns), the vectors README, QUESTIONS.md and
  U-14/U-17. Reference hash unchanged (`f9dd86c0…24168a7`).
  CONTAMINATION NOTE (incidental, low impact): one grep run to locate the vector consumer incidentally
  surfaced a few lines of `tools/drivetrain_test.c` (`test_kappa_c` region) and `src/drivetrain.{c,h}`
  line references to the analyst context. The analyst reports every value/precision finding came from
  the binary and evidence files, not that output, and edited nothing under `src/`/`tools/`/`Makefile`.
  No implementer exposure; no code quarantine required. Repeats the earlier recommendation to scope
  repo-wide greps away from implementer artifacts.
