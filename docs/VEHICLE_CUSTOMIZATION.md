# Vehicle customization and in-place switching

Status updated 2026-09-15. The development checkout now implements independent
visual-part replacement, failure-safe rim/kit replacement, coloured shop
subtabs and in-process vehicle/map transitions in ImGui. Persistent ownership,
purchases, career unlocks and in-world shop access are still unimplemented.
Visual acceptance is pending user testing; no gameplay capture was made for this
change.

The requested story, progression tables and shop access contract are collected
in [GAME_FLOW.md](GAME_FLOW.md). That reference separates sourced retail behavior
from OpenUG2 requirements and unresolved data mappings.

## One modification surface

The **Modification** tab has been added to the existing movable ImGui inspector. Its
subtabs represent Underground 2 shops; they are diagnostic controls, not a
replacement for the later asset-backed retail-style frontend.

| Subtab | Responsibilities |
| --- | --- |
| Body Shop (green) | Front/rear bumpers, skirts, full body kits, hoods, spoilers, mirrors, headlight/taillight assemblies, exhaust tips and physical rim selection/size |
| Performance Shop (blue) | Engine/ECU, intake/fuel/exhaust performance, transmission, turbo, nitrous installation, suspension, brakes, tyre grip/compound and weight upgrades; physical tyre upgrades are separate from visual rim selection |
| Graphics / Color Shop (red) | Body/rim/part paint where supported, finish, vinyl layers and decals; rim paint must not recolour tyre rubber or backing geometry |
| Car Specialties Shop (yellow) | Neon, window tint, custom gauges, nitrous purge visuals, hydraulics, doors/split hoods, spinners and trunk audio, as their asset/mechanic support is established |

The purple Safe House currently shows installed part choices only. Owned-item
swap/remove/refit is specified in GAME_FLOW.md and awaits inventory/save support.

The yellow location is **Car Specialties Shop** and the red location is
**Graphics Shop**, not Underground 1 categories. Shop identification and
accessory groups: [Car Specialties reference](https://nfs.fandom.com/wiki/Need_for_Speed:_Underground_2/Car_Specialties_Shop),
[Graphics reference](https://nfs.fandom.com/wiki/Need_for_Speed:_Underground_2/Graphics_Shop).
The [Underground 2 PC manual, printed pp. 4–5](https://oldgamesdownload.com/wp-content/uploads/manuals/need-for-speed-underground-2_win_manual_en_5rt.pdf)
also describes visual/performance upgrades and four reorderable vinyl layers.
These sources describe game behavior; they are not implementation code.

The active-car selector is above the shop subtabs. It now stages and commits a
new car while the SDL window, world and player pose remain alive. Every available
player car uses this same surface, with options derived from its asset inventory.
Missing parts are unavailable, not silently substituted from another car. The
current debug switch resets that car's visual part selection to stock; retaining
per-car configurations when switching away and back is a later inventory
boundary. Disk
save format, money, unlocks and purchase/refund rules remain later mechanics,
not guessed behavior in a debug menu.

Paint/rim paint and clear-coat controls now live in Graphics; neon and trunk
audio live in Specialties. Wheel placement, handling scalars and engine-cover
mesh inspection live in Vehicle Diagnostics. Performance controls must
not pretend prototype handling scalars are decoded retail performance parts.

## Vehicle switch resource contract

Car selection replaces the **active vehicle bundle** without calling `relaunch`.
"Model only" means vehicle-specific geometry, materials, textures, attachments,
measurements and handling profile remain consistent together. Engine audio still
uses the previous active bank during the transition and is the next ownership
boundary to close.

| Retain unchanged | Rebuild/rebind for the selected car |
| --- | --- |
| SDL window, GL context, world CPU/GPU meshes and texture bindings, resident manager, collision/navigation data | Car geometry and GPU buffers, texture maps/modes/vinyl composite, stock wheels/brakes and fitted aftermarket parts |
| Track, route/destination, race/session progress, opponents' state, camera mode and debug layout | Car bounds, wheel anchors/radius/profile, body collision dimensions, bloom/exhaust/other attachment data and engine audio |
| Player world position/heading and the currently supported deck | Vehicle-bound spring/support caches, wheel spin, steering smoothing, gearbox/RPM and controls reconciled with the new vehicle |

First support switching while stopped or explicitly paused. Prepare a candidate
without destroying the current car. Validate its required assets and placement
at the existing pose; if loading fails or the new body cannot fit safely, keep
the old vehicle and show the reason. Do not restart the race, teleport to a
different deck or reload the district as a recovery shortcut.

Commit the candidate at a frame boundary after it is complete; release the old
vehicle only when no render/audio consumer can use it. Asset decoding may later
move off-thread if measured stalls justify it; GPU work stays with the owning
GL context. The initial design does not require a new streaming framework.

Current opponents share some player texture lookups. In-place player changes
must not unexpectedly turn every opponent into the newly selected car: preserve
their old texture ownership or give opponents explicit asset ownership
before allowing the switch with opponents active. Race AI state stays intact.

Engine audio is also part of the transaction. Current loaders mutate callback-
visible global banks and are documented for use **before** `audio_init`; simply
calling them during gameplay would violate that contract. Prepare a replacement
bank safely and synchronize its handoff with the audio callback before freeing
the old data. Do not stop/reinitialize the whole game to avoid ownership work.

## Existing code boundaries to reuse and correct

- `src/debugui.cpp::dbgui_frame`: existing `MasterInspectorTabs`/`BeginTabItem`
  pattern. `src/debug.h::DbgState` already carries momentary car/rim requests.
- Track selection stages a complete `WorldResident` and swaps it in the running
  SDL/GL session; failed loads keep the current map and session alive. Car
  selection uses the same staged in-process transaction for geometry, profile,
  wheel fit and textures, preserving the current world and pose.
- `src/main.c::load_rim_style`: shared startup/F6/ImGui path now stages
  geometry, diffuse texture and GPU uploads; failures retain the old wheels.
- `prepare_body_kit` and the K/ImGui request path now validate and stage complete
  visual candidates, including independent local parts and socket attachments,
  texture additions, bounds and light anchors. They preserve the world, factory
  handling, wheel anchors and engine audio. This is not whole-vehicle switching.
- `n2_load_car`, `n2_car_variant_numbers`, `n2_car_prepare_wheels`,
  `n2_car_profile`, `wheel_config_for`, `phys_vehicle_from_geometry` and
  `upload_scene` already supply the relevant parsing/preparation/profile steps.
  `src/car_config.h` now carries independent visual-part selections;
  `src/car_mod.h` discovers compatible drawable options and assembles libraries.
  Decals, material finishes and performance packages are not part of it yet.
- `audio_load_ginsu_sweeps`/`audio_load_engine_bank` and their callback ownership
  must be addressed explicitly. There is no existing public hot-swap API.

## Remaining delivery order and acceptance

The debug preview now covers in-process geometry/profile/wheel/texture switching.
Next implement purchase/ownership/save and shop-entry gates according to
GAME_FLOW.md, then give opponents isolated textures and add synchronized engine
audio replacement while preserving the existing prepare/validate/commit failure
behavior.

Validation completed: 276 material assertions, rollback at 11 failure boundaries,
all 44 catalog cars through kit cycling, and 9,312 independent choices across the
29 cars with options, including mixed assemblies and stock restoration. These
are CPU/ASan/UBSan checks with simulated GPU ownership, not visual acceptance.
Normal/debug builds pass. Keep actual gameplay and asset attachment appearance
pending until manually tested; never mark an undecoded career mechanic complete
because a debug selector exists.
