# OpenUG2 physics — gap map (draft index)

Status: **navigation draft, not an approved specification and not a commitment**.
Derived from the feel-audit documents and the Stage 1–5 specs; no retail
runtime was used. Nothing here authorizes implementation. Approval, design
and sequencing decisions belong to the maintainer and the spec gate.

Last updated: 2026-11-04. Audited commit context: OpenUG2 `development` @
`424da39` (timed-history draft in branch `docs/timed-history-spec`).

Legend for "Needs":

- **static** — derivable from the static-evidence summaries already in the
  repository (no new evidence required to draft the contract);
- **local-data** — verifiable offline against a legally owned local data
  installation (e.g. `GLOBAL/GLOBALB.BUN`); never commit derived dumps;
- **runtime** — requires an original-runtime observation that does not exist
  and is not currently authorized; blocked on a maintainer decision;
- **gate** — needs an analyst amendment + fresh reviewer gate before code.

## 1. Workstreams

### W1 — Behavioural contracts still missing (spec work; static; gate before code)

| ID | Gap | Evidence anchor | Notes |
| --- | --- | --- | --- |
| W1-1 | **Timed steering histories** (input 0.55 s/66; rate 0.15 s/18; overwrite/expiry/append rules) | EFA-4; Stage4 U4-2 / Q4-I1 | **Drafted** in `timed-history/` (unreviewed). |
| W1-2 | **Ordinary angular damping** — curve D(w) endpoint-clamped, stabilize gate, independent gates | EFA-2; Stage4 U4-5 / Q4-I2 | Pairs `(0,.05),(2,.05),(4,.25),(6,.7),(8,2),(10,5)` rad/s→1/s. Includes `-8*z*P` translational damping unchanged. Race-state code 4 exception inputs remain external/unresolved. |
| W1-3 | **Slip-dependent vertical torque restriction** — K from row `+0x398` (selector p==0) or `+0x39C`; zero-K + p≠0 + sigma test can set K=20; raw-T.z clamp after damping, sign-strict | EFA-3 | Selector `p` identity unresolved; must not be equated to contact count / torque-limit selectors. |
| W1-4 | **Body-response amendment** — P/L/B/M coupling, V=P/M, Ω=B·I⁻¹·Bᵀ·L, basis-vector derivatives, adjusted-vs-basic inertia transition (L.z/kI on adjusted→basic only), adjusted yaw inertia multiplier, post-step Ω cap 250 rad/s, per-substep orthonormalization, bottoming impulse caps (1000 kN·s linear / 10 kN·m·s angular), full suspension feedback loop | EFA-1; Stage5 U5-6; Stage4 U4-5 | Largest contract; combine EFA-1..3. Inertia selector `c` identity unresolved — never guess from contact count. |
| W1-5 | **Shift-adjustment grip composition** — A>0 multiplies two wheel-grip coefficients by `1+min(1,A/R)` (captured operations only) | EFA-5 | Conditional follow-up; full four-wheel law and A/R producers unresolved. Spec only with explicit A/R inputs. |
| W1-6 | **Timed-history call cadence** — who calls the producer, at what rate, relative to S4-2 consumption | EFA-4 cadence unknown; Stage4 U4-1/U4-2 | Separate small contract; blocks any frame-rate-independence claim for the new filter. |
| W1-7 | **response_scale API mapping** — S4-6 names response_scale; composed cap has no separately named input; ResInput.scale is copied to torque/aero only | Audit finding 7 (C) | Contract/API clarification; no math fix asserted. |

### W2 — GLOBALB.BUN format coverage (local-data; no retail runtime)

Already proven and documented (`docs/FORMATS.md` "Global vehicle attributes"):
table framing, mass/body/inertia (+0x220…), wheel shapes, axle/track anchors,
gearbox blocks (stock/L1–L3), RPM thresholds, primary/secondary torque
samples, gain curves, `rear_drive`, `steer_ratio` (meaning unverified).

| ID | Gap | Notes |
| --- | --- | --- |
| W2-1 | Document Stage-1 fields not yet in FORMATS.md: `+0x1E0` axle blocks (slip-angle threshold A, peak/sliding grip multipliers), `+0x2C0` launch fraction, `+0x2C4`, `+0x2C8/+0x2CC` front/rear final drives, `+0x2D0` rear share, `+0x2D4` clutch coupling k, `+0x2D8` gear count, `+0x2DC` clutch rate, `+0x2E0` ratio table, `+0x30C` engine inertia, `+0x358` engine-braking samples | All [inf] from the analyst spec; local-data validation possible (46 records). Keep tags and cross-car checks. |
| W2-2 | `+0x398` / `+0x39C` slip-restriction scalars (W1-3) — presence, range, cross-car distribution; measured negatives allowed | Never commit table dumps; document aggregates only. |
| W2-3 | Brake, tyre and suspension **upgrade** packages — locate and structurally prove, or demonstrate measured negatives | Explicit "help needed" slice from #9; largest unmapped region of the 0x890 record. |
| W2-4 | Aero / drag-related fields if any exist in the record | Not yet investigated; may be a measured negative. |
| W2-5 | `steer_ratio` (+0x380) meaning — unverified label; candidates: steering response/rate scaling vs geometry ratio | Needs behavioural correlation or analyst amendment; do not rename without evidence. |
| W2-6 | `GlobalB.lzc` modified-record region (+0x3E0…+0x5AE differs from retail) — document the divergence boundary so offsets can be marked retail-identical vs install-specific | Provenance hygiene for all W2 offsets. |

### W3 — Harness and measurement (local engine; no retail)

| ID | Gap | Notes |
| --- | --- | --- |
| W3-1 | **Fixed-input comparison harness** for HUMMER, MIATA, 240SX — scripted inputs, fixed tick, logged outputs (speed, gear, slip, loads, forces) | Listed in #9 help-needed. Works today against OpenUG2; compare across builds, not against retail. |
| W3-2 | Probe: prescribed-ride 30 m/s braking — Fz/My preparation vs consumed pose | Audit finding 1 probe; reproducible with existing fixtures. |
| W3-3 | Yaw-inertia selection probe — kI=1 vs kI=10 identity check currently passes (gameplay ignores kI) | Audit finding 2 probe; becomes a real regression test once W1-4 lands. |
| W3-4 | `--fleet-census` / `--vehicle-compare` extensions for W2 fields | Keep diagnostics read-only; production behaviour unchanged. |

### W4 — Host boundaries that cap fidelity (implementation-adjacent; gate first)

| ID | Boundary | Evidence anchor |
| --- | --- | --- |
| W4-1 | Playable vertical/pitch/roll come from the M130 ride solver; Stage5 Fz/Mx/My are computed but not consumed → no tyre-force-driven dive/squat/roll feedback | Integration finding 1; SPEC S5-8 / U5-6 |
| W4-2 | Gameplay supplies unscaled yaw inertia and zero extra angular damping; stock kI never read into yaw equation | Integration finding 2 (`src/main.c`, `src/drive_contact.c`) |
| W4-3 | Contact impulses and collision angular response bypass the composed body state; bottoming application suppressed; no angular-impulse channel from world walls / car contacts | Integration finding 3 |
| W4-4 | All live world contacts report surface ID 1; six surface channels exist but are unreachable from the world adapter; bump amplitude ≡ 0; other IDs injectable only | Integration finding 4 |
| W4-5 | Keyboard-only adapter: fixed 33/9 windows; W/S combined pedal; Q/E manual shift only in drag mode; no device mapping | Integration finding 5; superseded for histories by W1-1 + W1-6 once approved |
| W4-6 | Response-scale G identity unresolved (W1-7) | Integration finding 7 |
| W4-7 | High-speed behaviour unestablished; 142.84 km/h max in the 240SX diagnostic; 97.3% gripping samples; no trajectory oracle | Integration findings 6/C; SPEC U4-6/U5-1 |

### W5 — Needs an original-runtime decision (blocked; do not start)

| ID | Item | Why blocked |
| --- | --- | --- |
| W5-1 | Matched opening-350Z vs OpenUG2-350Z capture (first controllable frame + transitions; effective config; controls; timing) | Requires running the retail executable instrumented; project has not authorized runtime capture; static-only so far. |
| W5-2 | Reference configuration proof (stock vs special setup; instance bonus; handling family) | Same blocker; W5-1 prerequisite. |
| W5-3 | Any parity sign-off (steering step/release/reversal; accel/brake; shifts; crest/landing; surface transitions) | Needs W5-1/W5-2; tolerances defined from repeatability first. |
| W5-4 | Full four-wheel EFA-5 law and A/R producers | Incomplete capture in evidence register; needs the same decision. |
| W5-5 | Race-state code 4 / throttle>.3 / all-contacts gates for ordinary angular damping | Producer reachability unknown; needs runtime or analyst resolution. |

### W6 — Coupled subsystems (outside #9 scope, listed for completeness)

| ID | Item | Where |
| --- | --- | --- |
| W6-1 | World material → surface ID mapping (U5-2) | #8 collision coverage / formats |
| W6-2 | Collision impulse angular channel (affects W4-3) | #8 |
| W6-3 | AI/opponent dynamics parity on race routes | #10 |

## 2. Suggested order (spec-first, smallest risk first)

1. **W1-1 review/gate** — finish the timed-history draft (already on branch). Cheap, unblocks the pattern for the rest.
2. **W2-1 / W2-2 / W2-6** — document known Stage-1 offsets in FORMATS.md with local-data cross-car checks. Pure docs + diagnostics; directly serves #9's "locate and prove fields" slice.
3. **W1-2 + W1-3** — angular damping and slip torque restriction as one amendment draft (they share the angular-momentum equation). Small, high-value, fully static.
4. **W3-1 / W3-3** — fixed-input harness and the kI probe as committed deterministic tests (no behaviour change; shadow mode).
5. **W1-6** — producer cadence contract (small; required before implementing W1-1).
6. **W1-4** — body-response amendment (large; do only after 1–4 land and review capacity exists).
7. **W2-3** — brake/tyre/suspension upgrade packages (open-ended format research; timeboxed; measured negatives are a valid outcome).
8. **W5-\*** — only if and when the maintainer authorizes an original-runtime observation method.

Explicitly **not** suggested: jumping to implementation of any W1 item before its gate; touching production tuning; using `--track ALL` as evidence.

## 3. What this map is not

- Not an approved plan; the maintainer may reorder or drop items.
- Not a substitute for the analyst evidence register; where this map and the register disagree, the register wins and this file gets corrected.
- Not a claim of retail parity anywhere.
