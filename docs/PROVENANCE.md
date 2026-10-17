# Provenance registry: proven vs inferred

Separates what the clean-room specs and vectors establish from what the implementation had to
decide. Only items that are **not** proven are listed. Everything the specs tag as data/static and
that a vector pins is treated as proven and is not tracked here.

How it is enforced:

- Each row has an ID (`CAM-01`, `PHY-07`, ...). In code, a comment containing `PROVENANCE[ID]` marks
  the site (inside an existing comment it is written as plain text).
- `tools/provenance_check.py` (`make provenance-check`) fails if a tag names an unknown ID, a listed
  file lacks its tag, a listed file is missing, an ID repeats, or a status is not in the list below.
  It also prints counts per status.
- To add an item: add a row here, add the tag in code, run the check. A `—` in the Code column means
  the behaviour has no single site (or is an absence, such as an omitted feature).

Statuses:

| Status | Meaning |
|---|---|
| inferred | one reasoning step beyond static evidence |
| guessed | no evidence; a value or rule picked to proceed |
| choice | implementation decision where the spec is silent or the engine differs |
| provisional | specified but explicitly marked provisional by the spec/reviewer |
| fixture | a test or placeholder value used as the runtime default |
| unknown | spec says the behaviour is unidentified; code supplies a neutral stand-in or omits it |

Spec refs: camera `docs/cleanroom-camera/SPEC.md` s13; HUD `docs/cleanroom-hud/SPEC.md`; settings
`docs/cleanroom-settings/SPEC*.md` and `QUESTIONS.md`; driving `docs/cleanroom-driving-physics/`
(`SPEC.md` U-n, `stageN/SPEC.md` UN-n, `tire-marks/SPEC.md` TM-Un).

## Camera

| ID | Item | Status | Code | Ref |
|---|---|---|---|---|
| CAM-01 | Wheel surface id: ROAD->ROAD, TERRAIN->GRASS, airborne->NONE | choice | `src/main.c` | SPEC 13.15 |
| CAM-02 | Up-probe starts 0.3 m up, radius 0.05 m | choice | `src/main.c` | SPEC 13.16 |
| CAM-03 | Other-car collision box dimensions | choice | `src/main.c` | SPEC 13.17 |
| CAM-04 | Option stored in `camera.cfg` (engine has no save profile) | choice | `src/camera.c` | QUESTIONS 7 |
| CAM-05 | Shake `vmax` default 100 m/s | guessed | `src/main.c`, `src/camera.c` | SPEC 13.9 |
| CAM-06 | Chase clip radius 0.55 m | choice | `src/main.c` | docs/CAMERA.md |
| CAM-07 | Shake axes/signs and nitrous predicate | inferred | `src/camera.c` | SPEC 13.8, 13.10 |
| CAM-08 | Module-wide assumptions: car origin = model origin, acceleration in m/s^2, dt = 1/60, split-screen secondary camera, free roam keeps stored option, state S2 / look-back F / event E11 | inferred | — | SPEC 13.1-5, 11, 14, 18 |

## HUD

| ID | Item | Status | Code | Ref |
|---|---|---|---|---|
| HUD-01 | Widescreen fit `min(W/640,H/480)` and group shift | provisional | `src/hud_asset.c` | SPEC fitment |
| HUD-02 | Tach needle: linear angle `235*rpm/scale` | provisional | `src/hud_asset.c` | C1-4 |
| HUD-03 | Tach scale family selection (7500/8500/9500/10500) | provisional | `src/hud_asset.c` | C1-4, C2-2 |
| HUD-04 | Inferred redline (opt-in, off by default) | inferred | `src/hud.h` | C1-5 |
| HUD-05 | Inferred NOS level arc (opt-in, off by default) | inferred | `src/hud.h` | C3-4 |
| HUD-06 | N2O glow alpha wraps modulo 256 | inferred | `src/hud_asset.c` | E3 |
| HUD-07 | New glow event ignored while one runs | guessed | `src/hud_asset.c` | E3 |
| HUD-08 | Shift icon on with automatic transmission | guessed | `src/hud.h` | H4 |
| HUD-09 | Gear-1 standstill threshold 0.5 m/s | guessed | `src/hud_asset.h` | F1 |
| HUD-10 | Glyph overlap order, justification precedence, missing-glyph policy | provisional | `src/hud.c` | SPEC format |
| HUD-11 | Gear text colour rule | provisional | `src/hud_asset.h` | C1-3/C3-3/E1 |
| HUD-12 | Glow speed 0.82 ms per unit (fitted to captures) | fixture | `src/hud_asset.h` | capture set 5 |
| HUD-13 | D-pad centre figure omitted (lookup key unknown) | unknown | — | C1-9 |

## Settings backend

| ID | Item | Status | Code | Ref |
|---|---|---|---|---|
| SET-01 | Unknown profile defaults (EA TRAX order/mode, autosave, prompt, names, bad-connection) | fixture | `src/settings.c` | QUESTIONS impl 1 |
| SET-02 | Unidentified float fields round-tripped (`unk28/32/36`) | unknown | `src/settings.h` | SPEC 5.2-5.4 |
| SET-03 | `controller_config` range 0..8 | inferred | `src/settings.h` | SPEC 5 |
| SET-04 | Preset level outside 0..5 clamped | choice | `src/settings.c` | impl 4 |
| SET-05 | Safe-mode `/s` matched case-insensitively | choice | `src/settings.c` | impl 5, SPEC 11.7 |
| SET-06 | Container trailing slack accepted; block B size must be 54930 | choice | — | impl 6-7 |
| SET-07 | EA TRAX masks indexed by track id; auto-detect stores performance level; autosave non-zero = set | choice | — | impl 2, 3, 8 |

## Options GUI

| ID | Item | Status | Code | Ref |
|---|---|---|---|---|
| GUI-01 | Pointer behaviour on rows, arrows, value text; hover and wheel do nothing | guessed | `src/options_menu.c` | GUI impl 4, ART impl 9 |
| GUI-02 | Arrow hit box 24x24 | guessed | `src/options_menu.h` | ART impl 9 |
| GUI-03 | FSAA row hidden when the cap is unlimited | choice | `src/options_menu.c` | GUI impl 6 |
| GUI-04 | `session > 2` means connected; extra online flag is a plain input | inferred | `src/pause_entries.c`, `src/pause_screen.c` | ART 2.4, impl 6 |
| GUI-05 | Audio/Gameplay/Player hub entries inert; registry write failure silent; Start/Back ignored during exit; hidden slots parked at y=6969; highlight timing | choice | — | ART impl 3, 4, 10, 11; GUI impl 11 |
| GUI-06 | Dialog/exit-script ordering (AMEND1 B2); group scale/rotation order from FEngLib | inferred | — | AMEND1 B2, A3 |

Open decision: SPEC_GUI_ART's pause behaviour (static) replaced earlier rules from the user's own
observation (ART impl 1). Older vectors are skipped by name in the checkers.

## Driving physics (Stages 1-5, body, session)

All stage specs state that behaviour is inferred from static analysis and that no retail runtime trace
validates it (U-16, U3-5, U4-6, U5-1). Bit-exact x87 precision is not reproduced (U-14, U3-4, U4-3,
U5-5); vectors carry tolerances. The path is opt-in (`--drivetrain`); drag and drift races fall back
to the legacy model.

| ID | Item | Status | Code | Ref |
|---|---|---|---|---|
| PHY-01 | Performance class `C` source unknown; default 0 makes the rule inert | unknown | `src/drivetrain.h` | U-11 |
| PHY-02 | Torque scale `G` = 1 and bonus flag off | fixture | `src/drivetrain.h` | U-6, U-7 |
| PHY-03 | Grip factor `q` = 1, location factor `X` = 0 | fixture | `src/drivetrain.h` | U-4, U4-4 |
| PHY-04 | Initial engine speed = idle | fixture | `src/drivetrain.h` | U-15 |
| PHY-05 | Shift jitter suppressed by default | unknown | `src/drivetrain.h` | U-3 |
| PHY-06 | Frame-time clamp is a test adapter, not retail timing | unknown | `src/drivetrain.h` | U-5, Q-16 |
| PHY-07 | Manual-assist initial state supplied by caller | unknown | `src/drivetrain.h` | U-2 |
| PHY-08 | m/s to mph factor 2.237 inferred from use | inferred | `src/drivetrain.c` | U-8 |
| PHY-09 | Shift-table intermediate rounding convention | provisional | `src/drivetrain.c` | U-14, Q-2 |
| PHY-10 | Initial lock mask and one-sided slip-relief switch are host inputs, not retail defaults | fixture | `src/drive_stage1.h` | U3-2, U3-3 |
| PHY-11 | 60 Hz box histories stand in for the unknown timed steering history | choice | `src/drive_stage1.h` | U4-2, U4-5 |
| PHY-12 | Inertia selector c=1, driver role p=1, session status 4 playing / 3 countdown | choice | `src/drive_contact.c`, `src/drive_session.h` | OM-1..4, U4-5, U5-6 |
| PHY-13 | Every world triangle gets numeric surface id 1 (no material-to-id mapping) | fixture | `src/main.c` | U5-2 |
| PHY-14 | Legacy physics kept for drag/drift; drivetrain stack only with `--drivetrain` | choice | `src/main.c` | docs/DRIVETRAIN.md |
| PHY-15 | Other host inputs: response scale 1, base grip 1, drafting/event zero, no damage/bonus/launch, bottoming impulse caps, force caps | fixture | — | U5-3, U5-6 |

## Tire marks

| ID | Item | Status | Code | Ref |
|---|---|---|---|---|
| TM-08 | Gate `G` residuals: exact frames the per-vehicle effects flag flips; session states (paused/finished/replay); role↔mover-class mapping; host samples the player only | unknown | `src/tire_marks_drive.h` | TM-U2 residual |
| TM-09 | Suppression mask residual: any external source of non-zero values; "blown tyre" naming | guessed | `src/tire_marks.h` | TM-U3 residual |
| TM-03 | Missing ground probe: reuse the other, none = no mark | choice | `src/tire_marks.c` | TM-U7 |
| TM-04 | Out-of-range half-width/intensity saturates instead of wrapping | choice | `src/tire_marks.c` | TM-U10 |
| TM-05 | Mark draw order relative to cars, smoke and decals | unknown | `src/tire_marks.c` | TM-U4 |
| TM-06 | Frustum cull parity (strip-box quirk) | unknown | `src/tire_marks.h` | TM-U12 |
| TM-07 | Trail clearing on reset, replay, restart; width factor static under upgrades | unknown | — | TM-U5, TM-U9 |

2026-10-17: the former `TM-01` (gate `G` assumed true) and `TM-02` (mask default clear) were resolved by
static analysis and pinned by spec `vehicle_gate` vectors (tire-marks SPEC `TM-2`/`TM-3`, approved
sha256 `5b968486...`). The gate is a per-vehicle 3-term conjunction with a reserved venue block
`[4400,4499]`; the suppression mask has no gameplay writer. Their residual unknowns moved to `TM-08`/`TM-09`.
Round role log: `docs/cleanroom-driving-physics/tire-marks/PROVENANCE.md`. No retail runtime differential was run.

## Priority and difficulty

Ranked least to most important, by how much an item changes gameplay or fidelity. Both the ranking and
the difficulty ratings are the maintainers' judgement, not something the specs state.

| Code | Difficulty | What it needs |
|---|---|---|
| D | Decision | Pick and document; no new evidence |
| S | Static | More analysis of the original executable |
| C | Capture | A retail screenshot or runtime capture |
| R | Runtime trace | Retail runtime traces and differential testing (hardest) |

| Tier | IDs | Why it ranks here | Needs |
|---|---|---|---|
| 1. Negligible | SET-02, SET-03, SET-04, SET-05, SET-06, PHY-08, PHY-09, TM-04, TM-06, GUI-02, GUI-05, CAM-04, CAM-06, HUD-12 | Odd inputs only, round-tripped untouched, or an invisible constant | D; PHY-09 at most S |
| 2. Cosmetic / UI | TM-03, TM-05, TM-07, HUD-06 to HUD-11, HUD-13, GUI-01, GUI-03, GUI-04, GUI-06 | Small visual or pointer differences; no effect on driving | S for GUI-04, GUI-06, HUD-13; C for HUD-06 to 11, TM-05, GUI-01 |
| 3. Fidelity | CAM-08, CAM-07, CAM-05, CAM-01, CAM-02, CAM-03, HUD-01 to HUD-05, SET-01, SET-07, TM-08, TM-09 | Visible differences in camera feel, gauge accuracy, defaults and marks | C for most; S for CAM-01 to 03, TM-08, TM-09; HUD-03 also needs each car's max RPM; SET-01/07 need a fresh-profile capture |
| 4. Core driving | see order below | Changes how the car drives | R for most |

Tier 4, least to most important:

1. PHY-04, PHY-02, PHY-05, PHY-07: initial RPM, torque scale and bonus, shift jitter, manual-assist start state (S/R)
2. PHY-10, PHY-15: initial locks, relief switch, other fixed host inputs (R)
3. PHY-12: inertia selector and session status (S)
4. PHY-11: steering-history stand-in (R)
5. PHY-01: performance class, rule currently inert (S, then R)
6. PHY-03: grip factors `q` and `X`, affects every car (S/R)
7. PHY-14: drag and drift races still on legacy physics; needs new specs (S plus R)
8. PHY-13: all surfaces use id 1, so no surface-specific grip (S for the mapping, then R)
9. PHY-06: frame timing, which every stage depends on and which is unresolved end to end (R)

All driving items share one gap: no retail runtime trace validates any per-tick behaviour (U-16). The
best value for effort is PHY-13 (bounded static work) and PHY-01.

## Not covered

Not audited: world, AI, rendering, audio, resource loading, `main.c` outside the sites above, and the
HUD/camera code paths not named here. Items marked `—` have no tagged site yet.
