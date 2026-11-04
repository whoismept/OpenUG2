# OpenUG2 physics — progress log

Plain-language record of what has been done, what was found, and what is
blocked. Updated as work happens. No proprietary game data is included;
measurements are aggregate-only and need a local, legally owned data root
to reproduce.

## TL;DR

- Development environment is fully working on Windows (MSYS2 UCRT64): the
  engine builds with `make debug`, boots with the local game data, renders,
  and all physics-related deterministic tests pass (11/11 suites).
- The physics spec that already existed and was approved covers more than
  expected — the big body/angular amendment (BR) **already exists and is
  implemented** in the engine. The real missing piece is that the
  **playable car does not use it** (host integration), which is blocked on
  the maintainer.
- One genuinely missing contract has been drafted: the **timed steering
  histories** (input 0.55 s/66 slots; rate 0.15 s/18 slots).
- Format work `GLOBALB.BUN` done: all Stage-1 offsets documented with
  local aggregate verification; a candidate **upgraded suspension
  package** was structurally identified (not proven, not wired in).
- Branch `docs/timed-history-spec` on the MathTeles fork holds all work;
  **nothing has been merged, nothing in production changed**.

## Environment setup (2026-11-04)

- Installed MSYS2 (UCRT64) + toolchain (`gcc`, `make`, `SDL2`, `GLEW`,
  `libpng`, `openssl`, `zlib`, `pkgconf`, `vim`).
- Fixed a self-inflicted build break: the first build used the wrong
  (Windows) `make`, which corrupted `src/generated_shaders.h`; deleting
  the generated files and rebuilding with the MSYS2 `make` fixed it.
- `make debug` builds fine (pre-existing format-truncation warnings in
  `src/main.c`, unchanged).
- Smoke test: `./nfsu2.exe --path <data> --resolution 1280x720 --frames 1
  --shot out.png` renders correctly (city, HUMMER, HUD works).
- 11/11 deterministic physics suites pass (`physics-clock-test`,
  `drivetrain-test`, `braking-test`, `lateral-test`, `contact-test`,
  `drive-stage*/body-*-test`).
- `car-material-test` **crashes with a segmentation fault** on this
  machine. Reproduced 3/3 runs, deterministic: it passes all earlier
  milestones and dies inside the synthetic fixture "B unmatched glass
  slice across two detail tiers" in `tools/car_material_test.c` (after the
  `printf` at line ~2006, before any game-data check). No code was
  changed on this branch, so the crash exists in the audited
  `development` tree; it may be toolchain-specific (MSYS2 UCRT64 GCC
  16.2.0).
  Not filed as an issue yet — awaiting the maintainer's/user's call.

## What was done (commits on `docs/timed-history-spec`)

1. `d802b32` — **Draft spec: timed steering histories** (U4-2 / Q4-I1).
   Two ring buffers, capacity trunc(window×120+0.5) = 66/18, expiry
   strictly older than the window, exact-age retention, overwrite removes
   the oldest from the running sum, empty slots don't dilute the average.
   12 synthetic vector cases, independently verified by simulation.
   Not approved; needs analyst amendment + reviewer gate.
2. `b72a8c4` — **Gap map** (`GAP_MAP.md`): the whole #9 audit organised
   into workstreams W1–W6 with a suggested order. Corrected later (see
   6).
3. `1d26292` — **FORMATS.md: Stage-1 fields documented** with aggregate
   validation across all 46 local car records (ratio monotonicity 46/46,
   front==rear final drive 46/46, inertia vs solid-box <1%, 7 records
   with nonzero secondary curves). Retail vs install boundary made
   explicit: the local install differs from retail in record bytes
   `+0x3E0…+0x5AE`; everything below is retail-identical.
4. `47a4556` — **Structural survey of unmapped record regions**: candidate
   upgraded suspension package at `+0x6D0`/`+0x6F0` (same 8-float layout
   as stock axle blocks, stiffer, traffic shares one default); candidate
   upgraded brakes (`+0x6C0..+0x6CC`, unproven); a 46/46 exactly
   duplicated 3-float block; binary traffic/racer splits in
   `+0x7F4`/`+0x808`; constants and enum-like fields catalogued. Nothing
   is used by the engine; semantics unproven.
5. `fc7057e` — **Fixed-input baseline** (`fixed-input-baseline.md`):
   `--vehicle-compare` (M121) outputs for HUMMER/MIATA/240SX recorded as
   a reproducible baseline (OpenUG2 model, not retail). Baseline values:
   HUMMER 4.98 s 0–100 / 212.75 km/h; MIATA 4.43 s / 220; 240SX 4.27 s /
   220.
6. `ec39902` — **Correction**: found the body-response amendment
   (`body-response/SPEC.md`) already **approved 2026-10-15** and
   implemented (`src/body_response.c`, 63/63 vectors). Retracted the
   duplicated draft; gap map corrected. The actual remaining gap is
   **host integration**, not specification.
7. `4913fe0` — Gap map status updated (W3-1/W2-4 done).

## Discoveries worth knowing

- The local `GLOBAL/GLOBALB.BUN` is 8,008,064 bytes with a valid
  `0x00034600` table and 46 records — matching the documented retail
  decompressed image, except the `+0x3E0…+0x5AE` region, which the local
  install modifies (L1–L3 blocks). Those are therefore install-specific.
- Every car carries constants (launch 0.8, 500.0, slip-angle 12.0, torque
  limit 17.0/0.0) that look engine-wide, not per-car tuning.
- The game ships two suspension tiers in the record; traffic vehicles all
  carry the shared non-upgrade default.
- `+0x270` kI (yaw-inertia multiplier, 2.0–2.28) exists per car but is
  **not consumed** by the playable yaw equation today.

## Blocked / needs the maintainer

- **Host integration** — the playable vehicle still uses the prescribed
  planar/M130 ride path; the approved dynamic body-response path is not
  wired in. This is the audit's Priority 1 and the biggest remaining
  piece. Needs a maintainer decision + design review.
- **Timed-history spec gate** — drafted, needs analyst amendment +
  reviewer sign-off before implementation.
- **Any retail runtime measurement** — the project has no authorised
  original-executable capture; without one, no parity claim is possible.

## Not claimed anywhere

- No parity with retail handling. All vectors are synthetic.
- No engine behaviour was changed. Production is untouched.
- No EA assets or dumps were committed; measurements are aggregate-only.

## How to reproduce locally

```
make debug                                     # build
./nfsu2.exe --path <data-root> --shot out.png  # smoke test
for t in physics-clock-test drivetrain-test braking-test; do make $t; done
./nfsu2.exe --path <data-root> --vehicle-compare HUMMER MIATA
```
