# OpenUG2 roadmap

OpenUG2 can load original game data, let you drive in free roam, and run
selected races with AI. Handling, course coverage, and career progression
are still in development. Follow the [community roadmap](https://github.com/whoismept/OpenUG2/issues/5)
for open work and discussion.

## Project priorities

1. **Vehicle foundation.** Validate tyre and wheel rendering, part attachments,
   suspension, and handling. [Vehicle dynamics issue](https://github.com/whoismept/OpenUG2/issues/9)
2. **Customization and ownership.** Extend the live preview to persistent
   per-car choices, purchases, and more upgrade packages. [Customization design](VEHICLE_CUSTOMIZATION.md)
3. **Lighting and weather.** Validate headlight placement, street lights,
   race closure lighting, and wet-road rendering. [Rendering notes](DEVELOPER_GUIDE.md#7-rendering)
4. **World collision.** Attribute false barriers and overlapping geometry to
   their source and test contact across regions. [Collision issue](https://github.com/whoismept/OpenUG2/issues/8)
5. **World coverage.** Find the missing mountain-road geometry and verify
   that the route is continuous. [Visibility investigation](DEVELOPER_GUIDE.md#next-map-visibility-milestone-northern-mountain-roads)
6. **Route guidance.** Verify the selected-route arrow and minimap against
   the authored roads and HUD. [Navigation notes](DEVELOPER_GUIDE.md#route-selection-direction-arrow)
7. **Racing.** Validate opponents, route completion, and race rules across
   the shipped events. [Racing issue](https://github.com/whoismept/OpenUG2/issues/10)
8. **Driving HUD.** Finish validating gauges, minimap, standings, and the
   transition between free roam and races. [HUD notes](DEVELOPER_GUIDE.md#player-hud---hud)
9. **Front end and career.** Integrate the original menus and implement the
   progression, economy, and save flow. [Game flow](GAME_FLOW.md) · [Front-end issue](https://github.com/whoismept/OpenUG2/issues/13)

## Future hardware and platforms

- **Steering wheels and force feedback.** Validate and calibrate wheels,
  pedals, and shifters on real devices. Then add optional, adjustable
  steering torque and verify safe force clearing on pause or disconnect.
- **VR driving.** Prototype OpenXR with a seated camera, stereo rendering,
  and head tracking. Validate timing and recentering on one headset first.
  See the [wheel and VR implementation notes](DEVELOPER_GUIDE.md#planned-wheel-and-vr-support).
- **Builds and rendering.** Expand native hardware coverage and continue
  work on the renderer. Desktop preview builds exist and build automation
  is paused. [Build and release plan](BUILD_RELEASE_PLAN.md) · [Renderer issue](https://github.com/whoismept/OpenUG2/issues/14)

To help, choose an [open issue](https://github.com/whoismept/OpenUG2/issues)
and follow the [contribution guide](../CONTRIBUTING.md).
