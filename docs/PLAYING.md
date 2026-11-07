# Playing OpenUG2

Use a legally acquired Underground 2 installation. Pass the directory
containing `CARS`, `TRACKS`, and `GLOBAL` to `--path`. Quote paths with spaces.
The [desktop test-build guide](TEST_BUILDS.md) covers downloaded previews.
They use the executable name `openug2` or `openug2.exe`.

## Build from source

The desktop build needs a C and C++ compiler, GNU Make, SDL2, OpenGL, zlib,
libpng, OpenSSL/libcrypto, `pkg-config`, and `xxd`. On Windows, use the
[MSYS2 UCRT64 instructions](WINDOWS.md).

On Debian or Ubuntu:

```sh
sudo apt install build-essential pkg-config libsdl2-dev zlib1g-dev libpng-dev libssl-dev libgl-dev xxd
```

On macOS, install the Xcode Command Line Tools, then run:

```sh
brew install sdl2 zlib libpng openssl@3 pkgconf
```

Check that `xxd` is on your PATH. On Arch Linux, the SDL2 headers and
`sdl2-config` are provided by [sdl2-compat](https://archlinux.org/packages/extra/x86_64/sdl2-compat/).
If you do not already have `xxd`, use [tinyxxd](https://archlinux.org/packages/extra/x86_64/tinyxxd/).

From a checkout of `development`, run:

```sh
make debug
./nfsu2 --path "/path/to/Underground 2" --hud
```

`make debug` includes the developer menu. Plain `make` builds the desktop
game with the same executable name. `make gles` targets OpenGL ES 2.0 and
needs target SDL2, zlib and GLES2 development libraries when cross-compiling.
See [Contributing](../CONTRIBUTING.md) for updating an existing checkout.

## Pick a car or start a race

With no track option, OpenUG2 starts in free roam on `STREAML4RA`. The asset HUD
appears automatically in races. Use `--hud` to show it in free roam.

```sh
./nfsu2 --path "/path/to/Underground 2" --car MIATA --hud
./nfsu2 --path "/path/to/Underground 2" --track STREAML4RA --event 4083
./nfsu2 --path "/path/to/Underground 2" --track STREAML4RB --event 4201
```

Event 4083 is a circuit race. Event 4201 is a drag race. Specify the race
region with `--track`. For a closed-loop path directly, use
`--track STREAML4RA --circuit ROUTESL4RA/Paths4175.bin`.

- `--car NAME` selects a folder under `CARS`.
- `--hud-mph` displays speed in MPH. `--hud-fit 4:3` centres the HUD in a 4:3 frame.
  See [widescreen HUD](HUD_WIDESCREEN.md).
- `--camera 0` through `--camera 4` choose a starting view. See [camera controls](CAMERA.md).
- `--drivetrain` enables the experimental [stock drivetrain model](DRIVETRAIN.md).
- `--help` shows the command syntax. Select a single region for driving.
  `--track ALL` is a diagnostic mode.

## More controls

The [README](../README.md#controls) lists the driving basics. In drag races,
`E` shifts up and `Q` shifts down. Hold the right mouse button to look around.
`L` toggles high beam and `J` flashes the headlights.

In the pause menu, use the arrow keys and Enter. On a controller, Start
pauses or resumes, the D-pad navigates, A selects, and B goes back. Where
available, open **Options → Controls** to change bindings.

## Developer menu (ImGui)

Press `1` in a debug build to open the **NFSU2 Master Inspector**. Add
`--devui` to open it at launch.

- **Modification** previews parts, wheels, paint, vinyls, neon, and performance
  settings. Stop before swapping parts. The [customization guide](VEHICLE_CUSTOMIZATION.md)
  describes the shop tabs and preview limits.
- **Vehicle Diagnostics** and **Lighting & Environment** show vehicle setup,
  camera, weather, and lighting controls.
- **World & Entities** and **Navigation & Races** show traffic, routes, and
  events. **Engine Telemetry** shows frame timing and scene counts.
- **Placement Marks** uses `F9` to mark a problem and `F10` to mark its expected
  position. Marks can be copied or exported to `placement_marks.txt`.
- **Race Log** records race telemetry. Press `.` to mark a problem during a run.

`F6` cycles rims and `K` cycles body kits. `F` enables the free camera.
In free flight use WASD, `Q` / `E` for height, and Shift for speed.

## Take a screenshot

```sh
./nfsu2 --path "/path/to/Underground 2" --car SKYLINE --hud --resolution 1280x720 --shot preview.png
```

The default resolution is 1920×1080. `--shot` writes a PNG from a hidden
window and exits. The log reports the actual drawable size. Add `--devui`
with a debug build to capture the inspector.

See the [map editor guide](../tools/MAP_EDITOR.md) for the separate map editing
tool and [texture overrides](TEXTURE_OVERRIDES.md) for loading your own DDS
replacements. For implementation and diagnostics, use the
[developer guide](DEVELOPER_GUIDE.md).
