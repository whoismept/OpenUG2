# Texture overrides

OpenUG2 can replace original textures with DDS files that you supply, for
example higher-resolution versions you made yourself from your own copy of the
game. The feature is off unless you pass a directory. No override files ship
with OpenUG2.

```sh
./nfsu2 /path/to/NFSU2 --tex-overrides /path/to/overrides
```

| option | effect |
|---|---|
| `--tex-overrides DIR` | index `DIR` (recursively, headers only) once at startup |
| `--no-tex-overrides` | ignore `--tex-overrides` |
| `--tex-override-budget MiB` | GPU memory cap for override textures (default 1536) |
| `OPENUG2_TEX_OVERRIDES=0` | environment switch: disable |
| `OPENUG2_TEX_OVERRIDES_CPU=1` | test switch: force the CPU-decode upload path |

A folder named `OpenUG2Textures` inside the game data directory is used without
any flag. With neither a folder nor `--tex-overrides`, nothing is scanned or
uploaded and rendering is byte-identical to a build without the feature.

## Minimaps

Minimaps can be imported by record name, up to 4096x4096:

```
OpenUG2Textures/TRACKMAP4000.dds    free-roam city map
OpenUG2Textures/TRACKMAP4144.dds    the map of event 4144
```

A map must cover the same area as the original image, because the HUD samples it
through the original's TrackInfo frame. An imported map replaces any baked map.
It follows the Level Of Detail setting: at levels 1 and 2 the retail image stays.

## Naming

```
KEY8HEX[@PACK][_anything].dds
```

- `KEY` is the 32-bit texture hash (8 hex digits, either case) that the TPK
  record uses. Texture names are not used because many are truncated in the
  data.
- `@PACK` is optional and binds the file to one source: a world region stem
  (`STREAML4RA`), `LOC4DYNTEX`, `INGAMECOMMON`, a car folder name (`GOLF`;
  also covers that car's shared GLOBALB fallbacks), `WHEELS` or `VINYLS`.
  Textures from the shared master region and from body kits are looked up
  without a pack, so only plain `KEY` files apply to them.
- Anything after `_` is free text, e.g. `D1C98E50_road_lane.dds`.
- Subfolders are only for organisation.

Lookup order: `KEY@PACK` for the requesting source, then plain `KEY`. A file
bound to a different pack is never applied. If two files have the same key and
pack, the first in sorted path order wins and the other is reported as a
duplicate.

## Requirements

- Width and height are powers of two, at most 4096 (or the driver limit), and
  exactly 1x, 2x, 4x or 8x the original size on both axes.
- Formats: DXT1/BC1, DXT3/BC2, DXT5/BC3 (legacy or DX10 header, sRGB variants
  accepted) and uncompressed 32-bit RGBA/BGRA. BC7, cube maps, volume
  textures and arrays are rejected.
- If the original has alpha, the override needs a format with alpha; a
  blended original needs DXT3, DXT5 or RGBA, not DXT1.
- Write a full mip chain down to 1x1. A partial chain still loads, but
  compressed files then lose mipmapping.

The original texture still controls draw mode, alpha handling and filtering
decisions; the override only supplies the pixels.

## Log

Every decision is printed once with the prefix `TEXOVR`:

```
TEXOVR 3 override(s) indexed from overrides (S3TC upload, budget 1536 MiB, max 4096)
TEXOVR use D1C98E50 pack STREAML4RA 256x2048 (orig 64x512, x4) overrides/D1C98E50_road.dds
TEXOVR skip 7AA659FF (...): size 64x64 is not 1x/2x/4x/8x of original 64x512
TEXOVR not applied 9A6D73B2@NOTAPACK (...): texture requested from pack GOLF
```

A skipped or unusable file never stops the game: the original texture is used
instead. When the memory budget is reached, an override is uploaded from the
mip level that matches the original size (`budget-reduced`), or skipped if that
does not fit either. With `--shot`, a `TEXOVR stats` line follows the capture.

## Coverage

World textures (regions, LOC4DYNTEX, master, InGameCommon), terrain grass, car
textures, rims, body kits and vinyls. HUD and frontend textures are not
covered yet.
