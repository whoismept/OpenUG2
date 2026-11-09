# Stock drivetrain

The native OpenUG2 executable uses the approved bounded Stage1–5 engine, clutch,
automatic gearbox, tyre, service brake, steering, resistance, suspension-contact,
handbrake and dynamic body composition by default. `--no-drivetrain` selects the
legacy model. Audits and captures keep the legacy model unless `--drivetrain` is
given. Without `--drivetrain`, a car with no valid tuning falls back to the legacy
model instead of exiting. Drag/drift race families use legacy
physics because their alternate producers are outside these specifications.

W accelerates forward; S brakes forward motion and selects reverse near rest.
W brakes reverse motion before returning to automatic forward gears. Space now
requests the specified rear handbrake torque and frame grip blend. It does not
hold body velocity, erase yaw or assign wheel spin. Initial locks are explicitly
supplied as zero; release reevaluates them through the Stage3 motion predicate.
Engine sound, HUD RPM/gear, wheel spin/steering, pause, reset, collision velocity
and car switching continue to use the composed simulation state. Stock tuning
is required at loading and switching; malformed fields reject the candidate.
Upgrades, nitrous and alternate race handling are outside this opt-in.

During a forward/reverse handoff, the host withholds accelerator input while
either the actual gear or its pending target opposes the requested direction.
The selector and clutch continue their normal transition; service braking still
opposes actual motion. This independent keyboard policy can delay acceleration
until the next 60 Hz tick after a handoff. Existing wheel/engine energy and the
preceding prepared force remain in the simulation, so it does not erase motion
instantly or establish a fix for every intermittent post-braking stall.

## Contact and body boundary

Stage5 computes frame preload from stock mass, centre, wheel mounts and the
preceding aero-enable/signed-speed snapshot. Ordered wheel queries produce
travel/rate, progressive spring/damping/paired loads, contact flags, alignment,
contact points, and distinct tyre/spin/body surface channels. The aligned load
feeds every tyre/brake-lock/cap calculation. Final world forces and all three
moments are available in `Stage1Drive.contact_force/contact_moment` and the body
accumulators. Gravity, actual aero and each wheel force enter those accumulators
once. Preload's separate specified aero feed-forward is not the reported aero
load added again.

The playable body now integrates centre position, all linear/angular momentum
components and a full body rotation basis from the approved body-response
amendment. Braking and lateral wheel forces act below the centre and create
pitch and roll. Vertical force imbalance creates heave. The resulting pose and
held velocities determine subsequent wheel query points, contact arms, travel,
travel rate and loads. No road-following pose, visual lean or algebraic axle
transfer overwrites ordinary dynamic state. A separate prescribed-pose adapter
remains for the earlier component fixtures.

Physics/world vectors, angular pseudovectors, application arms and orientation
use the same fixed right-handed x-forward/y-left/z-up coordinates (identity
conversion). The physical geometric origin is `X-B*centre`. Existing host
`carpos` is a ground-reference proxy: geometric origin plus body up times
`mean(mount.z-radius)`. This keeps the independent zero-travel start on its
supplied ground reference. The renderer consumes the full repaired B and
geometric origin directly; its body drop is cosmetic only. Wheel transforms use
effective mounts and computed travel. Camera/HUD ride fields are output views.

The explicitly chosen independent host inputs are inertia selector **c=1**,
driver role **p=1**, initial adjusted inertia, level heading and zero
vertical/angular velocity at spawn. Session lifecycle is a separate explicit
input: supplied snapshots pass unchanged; the ordinary host independently uses
**4** during active advancing play and **3** during its explicit start countdown.
The mode refresh follows valid body initialization and preceding force/torque
preparation. Pause advances no dynamics; resumption refreshes the active status.
Other routes keep established scheduling without a new numeric mapping. These
are host policies within approved OM-1..4, not a recovered frontend state machine.
A status change does not reset stabilization history or prepared accumulators. The selected inertia,
asymmetric world-z transition, angular damping curves, stabilization gates,
world-z torque restriction and late opposing torque follow BR-1..5. The numeric
limits come from the effective BodyCar, loaded from the approved stock offsets
+0x398/+0x39c by default. Bottoming impulses are enabled with per-application
1000 linear / 10 angular caps; the suppression input remains explicit. Force
caps are 1200 linear / 100 moment, and point-force moments use requested force.
The required upper-limit zero-duration post sweep remains ordered.

Existing wall/rail/vehicle services remain independent collision policy. Their
explicit position and horizontal velocity corrections enter through
`stage5_body_host_correction`, without resetting wheels or angular state. The
ordinary ground-motion/ride solver does not overwrite dynamic pose. Reset is an
independent initialization policy; complete retail reset/rest/collision and
special-state behaviour remains outside the contract.

The external world adapter uses independent `world_wheel_support` selection,
unit triangle normals, an upward 0.25 m window and a downward window at least
stock droop/current host swept-fall reach. Missing queries return valid default
normal and numeric surface as explicit host policy. Every selected triangle is
assigned **numeric ID 1 as a fixture**; no ROAD/TERRAIN/name-to-retail-ID mapping
is claimed. Injected queries can provide all 29 specified IDs. Peak, sliding,
spin drag, body drag and bump channels remain separate even when fixture 1 makes
them unit/zero. Immediate wheel preparation uses fresh IDs; frame body drag uses
the previously exported contact/ID snapshot.

## Scheduling and supplied choices

The application retains its independent fixed 60 Hz accumulator with fractional
residual, bounded catch-up, and paused time discarded. Each tick follows the
specified substeps. Its first derivative consumes the preceding prepared forces;
new pedals, missing terrain or surfaces affect subsequent preparations. Frame
preload precedes mode refresh; control exclusivity occurs once, then corrected
signed brake distribution and handbrake/grip refresh once. No extra Euler step
occurs for the bottoming zero sweep. Export happens after all substeps.

Other host inputs are ordinary state 0, response scale 1, supplied reduction 0,
base grip 1, drafting/event/calibration inputs zero, no damage/bonus/launch,
initial locks zero and relief enabled. The original timed steering-history
producer remains unknown; the independent host retains 33-sample input and
nine-sample rate box histories at 60 Hz. Reset preserves tuning/query/environment
configuration and clears travel/rate, blend, histories, yaw, locks and preparation
using their explicit creation/reset fixtures. Lower Stage1–4 standalone adapters
remain available to preserve their approved supplied-input fixture boundaries;
the application opt-in configures Stage5.

## Repeatable checks

```sh
make contact-test drive-stage5-test lateral-test drive-stage4-test braking-test \
  resistance-test drivetrain-test drive-stage1-test physics-clock-test district-collision-test
make debug
DISPLAY=:0 SDL_AUDIODRIVER=dummy ./nfsu2 '/path/to/your/NFSU2 installation' \
  --car 240SX --drivetrain --track STREAML4RA --resolution 640x360 \
  --drive-audit /tmp/stage5 --frames 420
```

The bounded native audit drives the normal controls and world queries. With
`--drivetrain`, ticks 170–184 request moving handbrake and a small steer input,
then release; steering pulses occur at 210–239 and 255–284; service braking at
300–344 is followed by acceleration. No physical state is forced. `STAGE5` rows
show handbrake torque, blend/grip, contact mask, travel, aligned loads, prescribed
dynamic pose, body forces/moments, signed speed and wall/rail counts. Later scripted
segments can meet obstacles; S near rest may select reverse. `--frames` controls
the bound (default audit 2,220). The audit writes spawn/end captures under the
selected prefix. Dummy audio may be omitted for normal interactive sound.

For an intermittent problem during manual driving, record a CSV capture:

```sh
./nfsu2 '/path/to/your/NFSU2 installation' --car 240SX \
  --drivetrain-trace /tmp/240sx-driving.csv
```

The trace option enables the stock drivetrain and keeps W/S/Space and steering
under your control. Reproduce the problem, wait a few seconds, then quit normally.
It writes one row per simulation tick, flushes every second of simulated time,
and replaces the named file. Pause produces no rows. Each row includes pedal,
gear/clutch/RPM, wheel spin/lock/torque, suspension travel/load, tyre forces and
ground/wall/rail/vehicle collision observations. `host_vx/host_vy` are the final
world velocity after collisions; `body_vx/body_vy` are the drivetrain's velocity
before those host responses. `incoming_f*` is the previous preparation consumed
at tick entry, while `prepared_f*` is the next prepared force at tick exit.
Forces are kN, torques kN·m, velocities m/s, angular speeds rad/s and travel m.
`active=0` marks a legacy race mode where drivetrain fields are inactive snapshots.
The capture reads only independent simulation state; it does not dump car assets.
`make drive-trace-test` checks the CSV schema and that recording leaves state intact.

All vectors are synthetic calculations from static-inferred approved contracts.
Component agreement, host tests and native captures do not establish original
runtime/differential, full world/material or numerical retail handling parity. Detailed
architecture and validation are in
[Stage5 implementation design](cleanroom-driving-physics/stage5/IMPL_DESIGN.md).


## Body-response checks

`make body-response-test body-feedback-test` runs all 63 approved synthetic
body vectors and independent BR-7 wheel-to-body-to-suspension acceptance.
`build/body_feedback.csv` records every derivative/preparation boundary with h,
X, B, P, L, held V/Omega, z, selected inertia, F/T, previous/current contacts,
wheel travel/rate and aligned load. Tests expose force-to-momentum and
momentum-to-pose latency, braking pitch, lateral roll, vertical imbalance,
changed subsequent loads/forces, continued evolution after release, explicit
collision correction and bottoming's zero sweep. Inputs and expected signs are
synthetic and make no retail trajectory or angle claim. Design:
[body implementation decisions](../src/DESIGN.md).

Run the ordinary stock 350Z through the same loader as other cars:

```sh
./nfsu2 /path/to/installation --car 350Z --drivetrain
```

The removed temporary 350Z configuration experiment is preserved only as
[archived research](cleanroom-driving-physics/test350z/README.md).
