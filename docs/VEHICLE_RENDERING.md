# Vehicle rendering status and audit notes

Updated 2026-09-10. This consolidates the recent vehicle rendering work on
`development`; the fixes remain local and uncommitted. The broader wheel audit
now covers every selected wheel model found in the local archives. Its scope
is observation only: runtime behaviour and assets are unchanged by the audit.

## Proven rendering fixes

- **Complete wheel geometry.** Stock wheels retain every material slice of
  the selected source tier and attach by `car_mount`, including slices whose
  material category is INTERIOR. Brake assemblies follow the respective hubs
  and front steering. Aftermarket selection uses `n2_rim_select_tier` to retain
  every slice of one size/LOD tier, rather than drawing only `wheelgm[0]` or
  stacking size variants. Startup, wheel selection and inspector reload share
  the loader. The original first-size choice and per-car radius fitting remain.
- **Authored transparency.** The investigated player/library square backing
  faces use atlas transparency to mask their corners; this is not a universal
  claim for unresolved traffic materials. The decoder now preserves CPU DXT1 one-bit
  alpha and embedded texture draw metadata; compressed GPU blocks are retained
  unchanged. `render_wheel_mesh` uses alpha cutout below 0.5 with depth writes,
  or texture-alpha blending without depth writes, according to source metadata.
  It restores incoming texture, alpha, blend and depth state. The geometric
  edge-length filter is removed, so complete source faces are drawn.
- **Library orientation.** Alpha alone left solid discs over the spokes because
  aftermarket loading skipped the stock source-to-hub transform. Both paths
  now use `n2_prepare_wheel_mesh`: `x=-x`, `y=width_midpoint-y`, once per fresh
  load, with one common pivot across slices and before library radius fitting.
  Indices and UVs are preserved. The measured NFSU STYLE02 backing moves from
  source Y=+0.183822 m to hub-local Y=-0.091438 m, behind the spokes on both
  mirrored sides.
- **Draw order.** Opaque opponents render before player glass and wheels.
  Blended wheel ranges across the four hubs use back-to-front centre-depth
  ordering. Existing body/vinyl and opaque world-fallback policies are retained.

The selected NFSU STYLE02 tier now draws all 435 triangles per hub, compared
with 120 before complete-tier selection and 433 with the old filter. ADVAN
STYLE02 now draws all 461, compared with 457 with the filter. MIATA and HUMMER
wheel radii remain 0.292 m and 0.429 m respectively; these are rendering fixes,
not suspension, collision or contact changes.

## What the evidence covers

| Evidence | Established scope | Limit |
| --- | --- | --- |
| Texture/selection census | 29 stock wheels; 80 selected library styles across 15 archives | Source data, not visual approval of every car/style |
| Library orientation census | All 80 selected styles retain indices, UVs, radial size and common slice pivots | Transform consistency does not by itself prove rendered inside/outside appearance |
| Corrected production captures | MIATA/HUMMER stock, NFSU STYLE02 cutout and ADVAN STYLE02 blend; opposite-side MIATA controls | Eight 1920x1080 views of two cars, not a full visual fleet audit |
| Earlier stock attachment work | Coverage/attachment checks across 29 player cars; left/right/underside captures for five | Predates the final alpha/orientation fixes; not final wheel-material approval |
| Broader source census | 81 geometry archives; 5,191 wheel object occurrences including source size/LOD variants | Data inspection, not 5,191 rendered approvals |
| Broader wheel-only review | 159 selected models: 29 player stock, 15 other vehicle stock, 80 exposed and 35 unexposed library styles; four views each | One selected tier per model/style, not all size/LOD variants or car-by-style combinations |

The earlier metadata census found 29/29 player stock wheels using cutout. Its
80 exposed library styles are
73 cutout and seven blended: ADVAN 01/02, AVUS 02, KONIG 07/08, RACINGHART 06
and ROTA 02. Blended styles must not be treated as universal binary cutout.
These counts concern the selected source tiers, not every size/LOD variant or
every car-by-library-style combination.

The eight corrected production views show visible spokes and removed opaque square
corners with complete geometry. Earlier alpha-only images named `*_after.png`
still show covering discs and are rejected candidates. Successful images are
named `*_fixed.png`; frozen references use `*_before.png`.

Prior verification includes 180 material assertions, extended GL alpha/depth/
state/sorting fixtures, CPU/GL/CLI suites, ASan/UBSan and debug/menu/normal
builds. The installed normal executable was current at the end of that work.
The missing-texture GL fixture emitted an Apple texture-zero sampler message;
its pixel, GL-error and sanitizer checks passed.

## Broad wheel audit findings (2026-09-10)

The inventory includes 24 named wheel-library archives, including SPINNER,
plus a 64-byte empty `WHEELS/GEOMETRY.BIN`. The 35 styles beyond the current
15-brand UI come from DAVIN, DONZ, FOXX, GIANELLE, GIOVANNA, KAIZER, OASIS,
SPINNER and WELDWHEEL. Their inspection does not enable them in gameplay.

- **Facing:** all 29 selected player-stock and 115 selected library models
  show their outer face on the outside in both mirrored diagnostic views.
  No new blanket flip is justified. Source object matrices with negative
  determinants (including BBS 02 and KONIG 04) are not sufficient evidence
  of reversal. Mounted position, steering, body overlap and road contact
  remain separate whole-car checks; this does not dismiss the user's report.
- **Traffic/service square planes:** all 15 other vehicle stock selections
  resolve texture key zero through the current car-local texture path.
  Fourteen render an opaque square backing; 4DR_SEDAN has a polygonal wheel
  without that quad. For example, TAXI slice 1, source triangles 30/31,
  material `010cb64a`, loses the backing mask. This proves an unresolved
  material path in this diagnostic, not absent retail textures or the cause
  of a particular player-car screenshot. Recover the correct texture source;
  do not delete faces. Current opponent wheels are still procedural.
- **Small alpha/UV edge candidates:** selected 5ZIGEN 01 (slice 2, triangles
  475/476), AVUS 02 (slice 2, triangle 362) and LEXANI 13 (slice 2, triangles
  512/513), all material `010cb64a`, have opaque alpha samples beyond the
  estimated circular radius on broad axial faces. These are source-sampling
  flags, not proof of a full visible square. The check uses nearest CPU alpha
  samples, not GPU filtering/mips. Raw-tint-off controls retain the AVUS
  angular appearance; no geometry or alpha threshold was changed.
- **Rubber/backing shading:** library rim paint and `uSpec=0.85` apply across
  all slices. The shader's `0.25 + 1.5*luminance` paint floor brightens dark
  texels; its axial sheen also remains on the backing when paint is disabled.
  Four paint-off controls are still gray at the back. Disabling paint alone
  is therefore not a complete fix. Attribute metal versus rubber/backing
  material ranges before exposing independent rim colour controls.

The wheel-only atlas contains 32 sheets at 768x1080: four 192x192 views per
model (outside, inside, oblique outside, mirrored outside), five rows per
sheet. It uses the production parser, source-to-hub preparation, shader,
alpha draw helper and blended-range ordering. Neutral diagnostic lighting,
gray library tint and radius-normalized framing are deliberate; it is not a
retail-lighting comparison or a whole-gameplay render. Every sheet was reviewed.
Four additional paint-off controls cover NFSU 02 and the three edge candidates.

Evidence and a per-sheet index: `scratchpad/vehicle_wheel_audit/REPORT.md` and
`scratchpad/vehicle_wheel_audit/INDEX.md`. Source tables record exact archive,
object, material, index range, UVs and alpha samples. The full census passes
ASan/UBSan; all selected atlas views pass GL-error assertions. An Apple
texture-zero sampler warning remains. Production source/binary hashes are
unchanged by this audit; prior build/test results above are not new runs.

## Capture operations

The application default remains **1920x1080**. `--resolution WIDTHxHEIGHT`
overrides one run; a low-resolution audit does not change that default.
Screenshots and visual audits use a hidden, fixed-size GL window and reject
unsupported actual sizes instead of silently shrinking the output. A working
display is still required. Interactive windows remain resizable.

For a normal production stock capture from the workspace root:

```sh
./nfsu2 .. --car MIATA --track STREAML4RA --heading 0 \
  --shot-yaw 90 --shot-pitch -35 --shot-empty --daylight --frames 3 \
  --shot scratchpad/miata_stock.png
```

Add `--resolution 960x600` for a small diagnostic capture. Default `NFSU` /
`STYLE01` selects the authored stock wheel; other brand/style selections use
the separate `CARS/WHEELS` library. Wheel-only views must record their selected
car/style, view and dimensions. They isolate wheel surfaces but cannot approve
body attachment, road contact or whole-scene transparency.

## Unresolved appearance and exhaust attachment

The next UI/resource-ownership stage is specified in
[Vehicle customization and in-place switching](VEHICLE_CUSTOMIZATION.md):
one Modification surface with shop subtabs and car-only reloads that preserve
the loaded world/session. This design is not yet implemented.

Rim tint/specular still affects tyre and backing slices, and ADVAN retains
visible angular surfaces. Range-centre sorting is neither triangle-level nor
global scene transparency sorting. High-speed wheel blur and opponent tyres
remain procedural. Separate diameter/width selection, independent brake-disc
rotation, tyre-road visual fidelity and the complete modification UI are open.

The object near MIATA's body origin is attributed to `KIT00_EXHAUST_A`.
Its source object transform is identity; the KIT00 rear bumper owns a
`RIGHT_EXHAUST` position marker at approximately (-1.991, -0.516, +0.092) m.
The marker basis does not directly match the stock pipe's axes, and KIT01 has
a different attachment position. A 350Z comparison also has a nonidentity
exhaust pivot and left/right markers. Correct assembly orientation is still
unproven. This is an attachment issue; no guessed transform or mesh removal
has been applied.

Local reproduction logs and captures remain under
`scratchpad/vehicle_wheel_draw/`, `scratchpad/vehicle_materials/`,
`scratchpad/vehicle_exhaust/` and `scratchpad/render_resolution/`. Their reports
describe successive states; the later wheel-draw report supersedes earlier
statements that alpha routing, the filter removal or orientation are pending.
Do not copy retail assets or disassembly into documentation or Git.
