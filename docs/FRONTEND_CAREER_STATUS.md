# Frontend and career implementation evidence

Source audit: 9 November 2026, development baseline
`02e306a9092a208ba361aa128f40b64850dd7f37`.
This index separates the reviewed presentation rules, the independent
implementation and the remaining main-menu/career questions. It does not define
new retail behavior or provide replacement layouts.

## What the documents establish

| Subject | Evidence | Implementation consequence |
| --- | --- | --- |
| Authored screen presentation | [SPEC_GUI_ART sections 1 and 8](cleanroom-settings/SPEC_GUI_ART.md), approved in [REVIEW](cleanroom-settings/REVIEW.md) | Parse packages, bind their objects, resolve original textures/fonts and evaluate their scripts. Missing required art makes the affected presentation unavailable. |
| Coordinate composition and anchors | [SPEC_GUI_ART_AMEND1](cleanroom-settings/SPEC_GUI_ART_AMEND1.md), [tvg12](cleanroom-settings/test-vectors-gui/tvg12_frames.json) | Use the existing virtual-screen transform and sample authored anchors before scripts move them. The pause/options measurements do not establish main-menu geometry. |
| Display/options navigation | [SPEC_GUI sections 1 and 5](cleanroom-settings/SPEC_GUI.md), [CONTROLS_SPEC section 2](cleanroom-settings/CONTROLS_SPEC.md) | The documented Display/Customize routes can use existing state machines. The main-to-options link is explicitly marked inferred in CONTROLS_SPEC. |
| Main/career package identities | [FRONTEND_ASSETS](FRONTEND_ASSETS.md#screen-graph-evidence) | `UI_Main.fng`, `UI_StartCareer.fng`, `UI_CareerSelect.fng`, crib, lot and map names are observed package references. Their names alone do not establish navigation, unlock conditions or profile operations. |
| Localized labels | [FRONTEND_ASSETS](FRONTEND_ASSETS.md#localization-and-rendering-implications), [SPEC_GUI identifiers](cleanroom-settings/SPEC_GUI.md) | Resolve label hashes through the requested language table and resolve the package's font resource. A readable caption is not proof that its language lookup succeeded. |
| Profile/career boundary | [Settings SPEC sections 3 and 5](cleanroom-settings/SPEC.md), [GAME_FLOW](GAME_FLOW.md#open-verification-items) | Settings and profile-container operations exist. Career/garage payload regions and exact progression/save semantics remain opaque or unverified; do not assign them invented fields or counters. |

## Existing code to reuse

- `src/ugui/ugui_assets.c`: installation file selection and resource ownership.
- `src/ugui/ugui_fng.c`, `ugui_scene.c`: parsed objects, resources, responses,
  authored scripts and the application-message queue.
- `src/ugui/ugui_strtab.c`, `ugui_ffont.c`, `ugui_tex.c`: label, font and texture
  decoding. Their synthetic fixtures live under `tools/ugui_vectors`.
- `tools/ugui_demo_orig.c`: original-package inspection and application-bound
  icon-scroller preview. This viewer is not a career controller.
- `src/gui_view.c`, `gui_carousel.c`, `gui_dialog.c`, `gui_timing.c`: the reviewed
  pause/options binding, application property-write and transition machinery.
- `src/settings.c`: existing settings/profile container handling; opaque career
  bytes are preserved rather than interpreted.

The bitmap-font frontend in `src/frontend` is provisional. It is not a source
for retail menu appearance, starting-car choice, career state or save behavior.
The earlier asset inventory predates much of the current `ugui` implementation;
its suggestion to add another JDLZ/FNG index reader is already covered by these
modules.

## Reproduced language-source defect

Before this change, the asset walker accepted the first decodable string table
from any loaded bundle. Frontend/global bundles load before the requested
`LANGUAGES/<language>.bin[.bacc]`. An unrelated table could therefore replace the
requested language; even a valid zero-entry table set `have_strings` and
prevented the requested table from loading.

The synthetic installation test reproduces this with the approved font and
string-table fixtures, without retail data. It covers a conflicting shared
label, an empty shared table, explicit French selection, missing/malformed
selected files, a shared in-game table and an empty selected-file chunk before
a usable table.

The loader now accepts localization tables only from the requested language
file. Empty chunks do not prevent a later nonempty table in that file from
loading. Missing or unusable selected data leaves `have_strings` clear and logs
an explicit warning. Screen/texture/font discovery remains usable; successful
package discovery does not imply successful localization. This fixes a
reproducible source-selection defect; it does not establish the cause of any
particular retail-data failure.

Validation: `make ugui-test`. The test is
`test_assets_selected_language_source` in `tools/ugui_test_assets.inc`.

## Engine-side career state

`src/career.h` keeps a bank balance and per-event results (best place, wins,
starts) in `career.sav` beside `camera.cfg`. Finishing an authored event records
the result and banks a payout for first place; the free-roam HUD Bank shows the
balance. `--no-career` and every capture run leave it out. It is OpenUG2's own
state: payouts and the starting balance are guesses (`PROVENANCE.md` CAR-01..04),
nothing is read from or written to the retail profile's career region, and it
has no stages, unlocks, cars or shops yet. `make career-test` needs no game data.

## Contract still needed for New Game / Continue

Before connecting gameplay or profile writes, establish from reviewed evidence:

1. Which application-controlled items belong to each main/career screen,
   including their exact icon and language keys, visibility and availability.
2. Which authored message means each action, its target and transition timing,
   and the package opened when a career/profile already exists or is absent.
3. The profile-selection, replacement and cancellation rules and the connection
   to a verified start/load operation. Preserve unknown career payload fields.
4. The platform/showroom scene and camera/environment bindings behind the UI.
   A black backdrop in an inspection view does not establish a retail backdrop.

`GAME_FLOW` remains a gameplay research reference. Its reported event inventories
are not decoded unlock counters, and its opening-car/district information is not
an implementation contract for these screen transitions.

Runtime inspection requires an explicit, reviewed original-UI entry in the
sandbox collector. The current collector's gameplay entries are not UI entries;
keep their executable, scene names and result meanings intact. Synthetic parser
and state tests can proceed locally without game data while that runtime
contract remains unavailable.
