# Vehicle customization and in-place switching

Requested shop layout and proposed implementation contract, 2026-09-10.
**Not implemented yet.** This changes the intended design, not the current
runtime. Complete the wheel/material/attachment audit before enabling new
modification controls. See [vehicle rendering status](VEHICLE_RENDERING.md).

## One modification surface

Add a **Modification** tab to the existing movable ImGui inspector. Its
subtabs represent Underground 2 shops; they are diagnostic controls, not a
replacement for the later asset-backed retail-style frontend.

| Subtab | Responsibilities |
| --- | --- |
| Body Shop | Front/rear bumpers, skirts, full body kits, hoods, spoilers, mirrors, headlight/taillight assemblies, exhaust tips and physical rim selection/size |
| Performance Shop | Engine/ECU, intake/fuel/exhaust performance, transmission, turbo, nitrous installation, suspension, brakes, tyre grip/compound and weight upgrades; physical tyre upgrades are separate from visual rim selection |
| Graphics / Color Shop (red) | Body/rim/part paint where supported, finish, vinyl layers and decals; rim paint must not recolour tyre rubber or backing geometry |
| Car Specialties Shop (yellow) | Neon, window tint, custom gauges, nitrous purge visuals, hydraulics, doors/split hoods, spinners and trunk audio, as their asset/mechanic support is established |

The yellow location is **Car Specialties Shop** and the red location is
**Graphics Shop**, not Underground 1 categories. Shop identification and
accessory groups: [Car Specialties reference](https://nfs.fandom.com/wiki/Need_for_Speed:_Underground_2/Car_Specialties_Shop),
[Graphics reference](https://nfs.fandom.com/wiki/Need_for_Speed:_Underground_2/Graphics_Shop).
The [Underground 2 PC manual, printed pp. 4–5](https://oldgamesdownload.com/wp-content/uploads/manuals/need-for-speed-underground-2_win_manual_en_5rt.pdf)
also describes visual/performance upgrades and four reorderable vinyl layers.
These sources describe game behavior; they are not implementation code.

Keep the active-car selector and selection/status/preview controls above the
shop subtabs. Every available player car uses this same surface, with options
derived from its asset inventory. Missing parts are unavailable, not silently
substituted from another car. Retain each car's chosen configuration when
switching away and back; session-local storage is the first boundary. Disk
save format, money, unlocks and purchase/refund rules remain later mechanics,
not guessed behavior in a debug menu.

Move existing modification controls here rather than duplicating independent
state in Vehicle & Wheels and Lighting. Keep renderer/physics inspection
sliders clearly separate from installed upgrades. Performance controls must
not pretend prototype handling scalars are decoded retail performance parts.

## Vehicle switch must not restart the world

Car selection will replace the **active vehicle bundle**, not call `relaunch`.
"Model only" means vehicle-specific geometry, materials, textures, attachments,
measurements, handling profile and engine audio remain consistent together.
Replacing only the mesh while keeping another car's wheel radius/collision
body or audio is incorrect.

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

Current opponents share the player's render resources. In-place player changes
must not unexpectedly turn every opponent into the newly selected car: preserve
their old shared bundle lifetime or give opponents explicit asset ownership
before allowing the switch with opponents active. Race AI state stays intact.

Engine audio is also part of the transaction. Current loaders mutate callback-
visible global banks and are documented for use **before** `audio_init`; simply
calling them during gameplay would violate that contract. Prepare a replacement
bank safely and synchronize its handoff with the audio callback before freeing
the old data. Do not stop/reinitialize the whole game to avoid ownership work.

## Existing code boundaries to reuse and correct

- `src/debugui.cpp::dbgui_frame`: existing `MasterInspectorTabs`/`BeginTabItem`
  pattern. `src/debug.h::DbgState` already carries momentary car/rim requests.
- `src/main.c::relaunch`: currently uses `SDL_Quit`/`execvp` for both car and
  track changes. Replace the **two car callers** (arrow selection and ImGui),
  not track switching as an unrelated expansion.
- `src/main.c::load_rim_style`: shared startup/W/ImGui path; currently frees
  the old geometry before parsing. Make replacement transactional when reused
  for the modification surface.
- The K-key kit reload is not a complete vehicle loader: it rebuilds meshes
  but does not recompute every texture/profile/light/attachment dependent.
  Extract one complete vehicle load/validate/release path instead of copying
  that incomplete reload into four tabs.
- `n2_load_car`, `n2_car_variant_numbers`, `n2_car_prepare_wheels`,
  `n2_car_profile`, `wheel_config_for`, `phys_vehicle_from_geometry` and
  `upload_scene` already supply the relevant parsing/preparation/profile steps.
  The current `N2CarConfig` is not yet a per-part shop configuration: one KIT
  and STYLE cannot independently express all bumpers, lights and decals.
- `audio_load_ginsu_sweeps`/`audio_load_engine_bank` and their callback ownership
  must be addressed explicitly. There is no existing public hot-swap API.

## Delivery order and acceptance

1. Finish wheel orientation/material and exhaust-attachment attribution, then
   establish a complete vehicle-owned resource boundary with failure-safe load.
2. Replace car-relaunch callers with in-place switching. Exercise small/large
   cars, missing/corrupt candidates, repeated switches and return-to-prior-car.
3. Add Modification/shop subtabs using the existing ImGui pattern; migrate
   working controls and mark unsupported inventory/mechanics honestly.
4. Expand per-part configuration and mechanics one proven asset family at a
   time. All car presets use the same path; do not add per-car UI forks.

Acceptance requires unchanged world-load counters and resource identities,
unchanged session/route/pose on success or failure, no growing GPU/CPU/audio
resource count across repeated switches, no callback races, and correct body/
wheel/attachment/profile matching. Verify wheel-only views plus low-resolution
whole-car placement, both hub mirrors, and a live paused-world switch. Keep the
normal 1920x1080 default. Document timing and remaining unsupported mechanics;
do not call a mesh-only preview a successful gameplay hot swap.
