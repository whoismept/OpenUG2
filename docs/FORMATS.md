# NFS: Underground 2 — file format reference

Clean-room interoperability notes for the retail PC data files. This document
records consumed layouts and bounded research observations, the evidence behind
each claim, and the reader where implemented. All integers are
little-endian unless a section explicitly says otherwise.

> These are independent, clean-room notes on the *layout* of a game's data
> files, produced for interoperability (reading data you already own), in the
> spirit of OpenMW/OpenRW. They contain no Electronic Arts code and no game
> assets. "Need for Speed" and "Underground" are trademarks of Electronic Arts;
> this project is unaffiliated with and unendorsed by EA. See the README's
> Legal Notice & Disclaimer.

## How to read this document

Every non-trivial claim belongs to one of four evidence levels:

- **PROVEN** — byte layout and meaning agree across multiple records or files,
  and a production parser/test exercises the relationship end to end.
- **OBSERVED** — bounded original-file checks establish the stored layout or
  relationship, but production consumption or retail runtime use is unverified.
- **PLAUSIBLE** — the values are consistent and useful, but no structural link
  or independent cross-check proves the field's meaning yet.
- **UNKNOWN** — bytes or a relationship exist, but assigning semantics would be
  guesswork. Unknown fields must stay unknown in code and documentation.

Offsets are relative to the payload of the chunk or record being described,
not to its 8-byte chunk header. Examples below are synthetic: do not paste
retail bytes, extracted asset tables, executable output, or disassembly into the
repository. When a new fact is proven, update both this file and the parser
guard that makes the assumption safe.

### Implementation map

| data | production reader | owner after parsing |
|---|---|---|
| generic chunks, track/car meshes, TPK, codecs | `src/nfsu2.h` | `N2Scene`, `N2Tex`, `N2Path` |
| region loading, events, nav, ground/contact | `src/world.c` | `World` |
| authored district lights (`0x135003`) | `n2_load_light_sources` in `src/nfsu2.h` | `World.lights` |
| file mapping and content discovery | `src/resource.c` | caller |
| car axle/track anchors in `GLOBALB.BUN` | `n2_global_wheel_attr` in `src/nfsu2.h` | `N2WheelAttr` |
| generic `0x00135200` attribute records | `src/attrib.h` | diagnostic `N2Attrib` view |
| packed track polygons and activation-group rows | `src/ug2_track_collision.h` | `UG2TrackCollision`; wall selection in `src/world_collision.h` |
| FEng packages, UI scripts, textures and compression | `src/ugui/ugui_{bundle,fng,anim,tex,codec}.c` | decoded UI package/resource owners |
| vehicle presets and career opponent descriptors | local research readers only | no production roster importer |
| ABK, Ginsu and XAS audio | `src/audio.c` | `EngineAudio` and decoded PCM |

The parser is deliberately defensive. A format observation is not permission
to index unchecked data: validate the chunk boundary, filler, record stride,
index range, material slot, allocation, and output count before emitting a
runtime object.

### Retail data by subsystem

Paths below are relative to the user's original game-data root. Stored data,
executing code and OpenUG2 policy are separate evidence categories.

| Subsystem | Original files | Format section / boundary |
|---|---|---|
| Racer identities and career associations | `GLOBAL/GLOBALB.BUN`, equivalent decoded `GLOBAL/GlobalB.lzc` | [Vehicle presets](#named-ai-vehicle-presets-0x00030220), [career records](#career-events-and-opponent-descriptors); exact roaming-rival selection unresolved |
| Free-roam navigation and connections | `TRACKS/ROUTES<region>/RoutesFreeRoam.bin`, `PathsFreeRoam.bin`, `TrackPosMarkersFreeRoam.bin` | [Route/path split](#freeroam-vs-race-event--the-split-is-the-games-own); route records are not vehicle counts |
| Event navigation and grids | Same directory, `Paths<id>.bin`, `Routes<id>F.bin`, `Routes<id>B.bin`, `TrackPosMarkersAll.bin` | [Racing lines](#racing-lines--circuits-tracksroutespathsbin); geometry does not identify the selected opponents |
| Static track collision | `TRACKS/STREAM<region>.BUN`; group table in companion `TRACKS/<region>.BUN` | [Packed polygons](#packed-track-collision-0x80034130); material/contact semantics partly unknown |
| Local prop shapes and physical-property candidates | Companion `TRACKS/<region>.BUN` | [Local shapes](#scripted--dynamic-objects-zcv_--zcs_--companion-l4rbun); world placement unresolved |
| In-game interface, HUD and shared art | `GLOBAL/InGameCommon.bun`, `InGameFreeRoam.bun`, `InGameRace.bun`, `HUD_CustomTextures_ALL.bin` | [HUD packages](#in-game-hud-and-interface-bundles); package presence does not prove mode activation |

The supplied `speed2.exe` has encounter/controller/action and traffic-level
labels, but no recovered loader/controller call chain binds them to a complete
behavior schema. Density, intersection timing, off-screen simulation and rival
selection must not be inferred from those labels or bundle filenames.

Local evidence for the October 13–15, 2026 audit is retained under ignored
`scratchpad/`: `original-racer-ai-files-2026-10-15/`,
`speed2-freeroam-traffic-2026-10-15/`, `superpowers/bun-collision-oct13/`,
`ingame-freeroam-bun-2026-10-15/` and `ingame-common-bun-2026-10-15/`.
Those reports retain original-file hashes, offsets, check scope and reproduction
helpers locally; they are optional audit material, not dependencies of these
docs or files to add to Git. Public notes contain layouts and aggregate results
only, with no original bytes, full asset tables or executable extracts.

## Chunk container (`.BIN` / `.BUN`)

Every file is a flat stream of chunks:

```
u32 magic
u32 size          // payload size, in bytes, right after this header
u8  payload[size]
```

- `magic == 0` → padding/alignment, skip.
- top nibble of `magic` == `0x8` (e.g. `0x80134000`, `0x8003b601`) → the payload
  is itself a sequence of nested chunks; recurse into `[payload, payload+size)`.
- otherwise → an opaque leaf chunk identified by its 32-bit id.

**PROVEN parser rule.** Container recursion is a property of the magic's high
nibble, not of a filename or a hard-coded parent list. A safe walker is:

```c
walk(data, begin, end):
    off = begin
    while off + 8 <= end:
        id   = read_u32le(data + off)
        size = read_u32le(data + off + 4)
        body = off + 8
        if body + size < body || body + size > end: stop_as_malformed()
        if id == wanted_leaf: consume(body, size)
        else if id != 0 && (id >> 28) == 8: walk(data, body, body + size)
        off = body + size
```

The production helper is `n2_find_leaves`. It follows this traversal but the
current implementation still trusts a measured retail chunk size against its
parent end; that is a known hardening gap, not a contract to copy. New readers
must keep the explicit overflow/bounds check. Never search beyond the current
container merely because a magic value happens to occur in texture pixels or
compressed data.

## Packed track collision (`0x80034130`)

The eight inspected `TRACKS/STREAM<region>.BUN` files contain a polygon store
separate from drawable `0x134xxx` meshes: 599 groups, 417,572 packed polygons,
620,291 spatial references and 37,671 grid cells. The bounded production reader
is `ug2_track_collision_read` in `src/ug2_track_collision.h`; synthetic checks
are in `tools/ug2_track_collision_test.c`.

Each `0x80034130` contains these leaves. Offsets are leaf/record-relative.

| Leaf | Layout |
|---|---|
| `0x34131` | 48-byte header: `u32` section at +0; `u16` grid width/height at +4/+6; `f32` XY origin at +8/+12; minimum XYZ at +16/+20/+24; maximum XYZ at +32/+36/+40 |
| `0x34134` | 32-byte polygon: group index byte +0; opaque byte +1 and `u16` +2; signed Z base +4; `u16 0xffff` +6; four `i16` X at +8, Y at +16, Z deltas at +24 |
| `0x34136` | 4-byte reference: `u16` polygon index followed by opaque `u16` metadata |
| `0x34137` | 4-byte cell: `u16` reference start and `u16` reference count |

**PROVEN decoding:** `x = packed_x / 8`, `y = packed_y / 8`,
`z = signed_z_base + packed_z_delta / 256`. A triangle-shaped record repeats
its third corner as the fourth. Cells use X-fast row-major order and a 32-unit
XY width. All inspected references are bounded and their polygon XY bounds
overlap the referenced cell. Empty groups use sentinel bounds; do not require
ordered geometric min/max for an empty polygon leaf.

Companion leaf `0x3412f` supplies 32-byte activation-group rows: name hash at
+0, a bounded NUL-terminated name at +9, and opaque intervening fields. The
first row is the special unnamed-hash/NONE entry. The reader validates later
name hashes with seed `0xffffffff`, `h = 33*h + byte`, modulo 2^32, and checks
each polygon's group index against the table. This is a stored association;
the retail activation flags and contact rules are not fully decoded.

OpenUG2 defaults to the BUN wall source (`WCOLLISION_BUN`); explicit
`--collision-source legacy` retains the drawable/authored wall path for
comparison. `wce_bun` filters named event groups and emits wall-only meshes;
existing ROAD/TERRAIN support and local prop handling remain separate. This
selection is an OpenUG2 policy, not proof of retail activation equivalence.
The original grid structure does not establish a measured runtime speedup.
Surface/grip IDs, one-sidedness and the retail loader-to-contact binding remain
**UNKNOWN**; do not turn every metadata value into a named physics material.

## Track and car geometry (family `0x134xxx`)

The large `TRACKS/STREAM*.BUN` files hold renderable world geometry. The small
per-region `TRACKS/L4R*.BUN` companions hold gameplay/script definitions and
are not substitutes for the STREAM mesh. Car geometry uses the same object
family in `CARS/<NAME>/GEOMETRY.BIN`, with a different vertex stride.

```text
0x80134000  object collection
  0x80134010  drawable object
    0x00134011  object/material header: asset name and world matrix
    0x00134012  positional texture-slot list
    0x80134100  geometry container
      0x00134900  header/counts (not required by the current extractor)
      0x00134B01  vertex buffer
      0x00134B02  material/index-range table
      0x00134B03  u16 triangle index buffer
```

One `0x80134010` is an authored object, not necessarily one runtime draw. If
its `0x134B02` records select different texture slots, OpenUG2 emits one
`N2Mesh` per validated index range.

### `0x134011`: name and placement

**PROVEN.** After the leaf's own `0x11` filler, `+0x40` holds a 4x4 `f32`
affine transform. Basis vectors occupy floats 0–2, 4–6 and 8–10; translation is
floats 12–14; a sane affine record has `m[15]` near 1.

```c
world.x = x*m[0] + y*m[4] + z*m[8]  + m[12];
world.y = x*m[1] + y*m[5] + z*m[9]  + m[13];
world.z = x*m[2] + y*m[6] + z*m[10] + m[14];
```

The 16-float array is byte-compatible with the column-major matrix convention
used by the renderer; do not transpose it. Terrain/road vertices are commonly
already in world space and therefore carry identity. Placed buildings and props
carry local vertices plus a non-identity transform. Production reader:
`n2_obj_matrix`.

The same leaf contains an ASCII asset/material name. The parser scans a bounded
name run rather than assuming a universal offset. This name drives two distinct
classifications:

- draw/contact category: road, terrain, sky, glow or other;
- scenery semantics in `N2Mesh.scen`: terrain, building, prop, tree, wall,
  structure or other.

Known paved exceptions are deliberate: `TRN_RDP*` and `TRN_CONCRETE*` are
classified as road before the generic `TRN*` terrain rule. Names help select
behaviour; they are not placement records.

### `0x134B01`: vertex buffers

A filler prefix precedes the records. World/car vertex leaves use a byte run of
`0x11`; index and submesh leaves require paired `0x1111` words so the first
real data byte cannot be consumed accidentally.

| asset | stride | offsets |
|---|---:|---|
| STREAM scenery | 24 B | position `f32[3] @+0`; prelight **D3DCOLOR (BGRA8) `@+12`**; UV `f32[2] @+16` |
| car part | 36 B | position `f32[3] @+0`; normal `f32[3] @+12`; colour `u32 @+24`; UV `f32[2] @+28` |

OpenUG2 stores parsed vertices as five floats `{x,y,z,u,v}`. World prelight is
retained separately as four bytes per vertex, **byte-swapped to RGBA on read**.
Source indices remain `u16`.

**Prelight byte order (PROVEN).** The `@+12` slot is a D3DCOLOR: little-endian
ARGB, so the bytes sit in the stream as `B,G,R,A`. Copying the run straight
through as RGBA swaps red and blue. Three independent measurements on
`STREAML4RA`:

1. `XO_TRAFFICLIGHTC_1A_00` lens vertices read `(78,78,163)`, `(83,83,163)`,
   `(89,89,165)` — R==G with a high third byte. Re-read as BGRA they are
   `(163,78,78)`, a saturated dark red: the stop lens.
2. The rendered triple was purple / teal / green. Under an R↔B swap, red
   `(255,0,0)` → blue-violet, amber `(255,190,0)` → teal `(0,190,255)`, and
   green `(0,255,0)` is its own mirror and stays correct. All three observed
   colours match that permutation exactly, including green being the one that
   looked right.
3. Across all 7,363,547 prelight vertices in the bundle, 58.7% are neutral
   grey (`R==G==B`, invariant under the swap — which is why the fault stayed
   hidden), and of the rest the blue-dominant vertices outnumber red-dominant
   ones 1,968,986 to 956,758. A city baked under sodium and tungsten street
   lighting does not skew 2.06:1 cool.

Fixing the order warms the whole baked night scene (pavement, shop interiors,
stone facades) and restores red/amber/green traffic lenses.

**Corrupt-vertex recovery (PROVEN parser robustness, not a new file field).**
Some otherwise valid world objects contain isolated NaN or approximately
`1e38` positions. `n2_add_pair` marks any vertex outside ±60,000 m or NaN,
drops only triangles referencing it, and parks unused corrupt vertices on a
known-good point so bounds and GPU batches cannot inherit the spike. If every
vertex or every triangle is invalid, the mesh is rejected. The bad-vertex mask
must survive until triangle filtering and repair are complete; reconstructing
it from transformed X alone is incorrect.

### `0x134B02`: submesh/material ranges

**PROVEN layout.** After paired `0x1111` filler, the leaf is an array of
60-byte records:

| offset | type | meaning |
|---:|---|---|
| `+0x00` | `f32[3]` | local range bounding-box minimum |
| `+0x0c` | `u32` | index count |
| `+0x10` | `f32[3]` | local range bounding-box maximum |
| `+0x1c` | `u32` | `mat`: texture-slot id, positional index into `0x134012` |
| `+0x20` | `u32` | **PROVEN (M135)**: `matid`, positional index into the object's own `0x134013` material-hash list (see below). Distinct from `mat` at `+0x1c`: on `GOLF_KIT00_BODY_A`, one record has `mat=1, matid=1`; another has `mat=0, matid=4` -- independent selectors into two independent lists. |
| `+0x24` | 16 B | unknown/reserved |
| `+0x34` | `u32` | index start |

The start/count ranges are measured in `u16` indices after index-leaf filler.
For the simple production path they must start at zero, form a contiguous chain,
contain at least one triangle each, stay inside `0x134B03`, and end at the last
whole triangle. One spare alignment index is allowed; a two-index tail is not.
`n2_mesh_submeshes` currently accepts exactly one submesh leaf whose body is a
multiple of 60. Car-only callers additionally run
`n2_car_submesh_partition_ok`, which re-validates the SAME chain against the
object's own decoded index-buffer length before a material/texture split is
ever trusted; any violation keeps the whole-object path.

### `0x134013`: positional material-hash list

**PROVEN (M135).** Same 8-byte-entry shape as `0x134012` (first `u32` kept,
second unused), parsed by `n2_mesh_matslots`. Values are `n2_str_hash(name)`
of a material class name (`h = 0xFFFFFFFF; h = h*33 + byte`, applied per
byte); unlike `0x134012` keys, POSITION within the list is per-object (not
globally fixed), but the HASH VALUE is globally meaningful -- the same
material class hashes to the same `u32` everywhere.

Verified against live `GOLF` data, independently re-derived (not copied from
any external source):

```text
n2_str_hash("WINDSHIELD") == 0x471a1dca
n2_str_hash("CARSKIN")    == 0xd6d6080a
```

`GOLF_BASE_A`'s own `0x134013` list is
`[0fedee40, 02a05578, 010cb64a, 3ed70c43, 471a1dca]` -- index 4 is
`WINDSHIELD`, selected by three of its submeshes (one wide front-facing
pane plus a mirrored left/right pair), none of which resolve a texture key,
exactly the shape real glass geometry produces. `GOLF_KIT00_BODY_A`'s list
is `[0fedee40, a7366ae6, d6d6080a, 010cb64a, 02a05578]` -- index 2 is
`CARSKIN`, selected by its dominant 534-index submesh. Checked across every
one of `GOLF`'s 608 objects and 2171 `0x134B02` records: every `matid` is
in-bounds for its own object's `0x134013` list, 2171/2171 (100%).

`n2_mat_class(hash, fallback)` maps a hash to a car category. Only
`WINDSHIELD -> N2_CAR_GLASS` and `CARSKIN -> N2_CAR_BODY` are classified;
every other hash -- known-but-unmapped (chrome, aluminium, moldings,
plastics, tire/rim materials, lens classes all measure the same way but are
not yet wired) or absent/out-of-range -- returns `fallback`, the
object-level category from `n2_car_category`, unchanged. A car object
splits into separate `N2Mesh` slices when EITHER the resolved texture
differs across `0x134B02` records OR the classified material differs; a
same-texture body-and-glass object could never split under the old
texture-only rule.

### `0x134012`: positional texture slots

The leaf contains 8-byte entries:

```text
+0 u32 texture key
+4 u32 zero/unknown
```

`submesh.mat_id` is a positional index into this list. No intermediate
`0x134003` lookup is required by the proven path. A synthetic example:

```text
slots:  [0]=0x11111111  [1]=0x22222222  [2]=0x33333333
ranges: {start=0,count=48,mat=1}, {start=48,count=9,mat=2}
result: first 16 triangles use slot 1; final 3 triangles use slot 2
```

For ordinary world objects, structurally valid records are emitted per range.
This includes buildings, props and other set dressing, not only road/terrain:
otherwise a multi-material building inherits its last texture slot (for
example `OBJ_RAILING`) across every opaque wall. A key available in the
region/shared TPK wins; an unavailable key falls back only for that range. A
malformed object falls back to the legacy one-key whole-object path rather
than partially dropping geometry.

### Slots that name a material, not a texture

A slot usually names a diffuse map. A few name a **material** that never
carries one, and for those the "fall back to the object's sibling key" rule
above is always wrong: there is no art to approximate, so the substitute is a
different picture rather than a near-enough one.

`HEADLIGHTGLOW` (`0x3394fe62`, hashed by the usual `h = h*33 + c` seeded
`0xFFFFFFFF`) is the light-pool card on street fixtures -- the soft cone a lamp
throws. Measured on `STREAML4RA`: 210 submeshes across 132 object kinds carry
it, and **no TPK record for it exists in any shipped pack** (all five
`STREAM*.BUN`, `LOC4DYNTEX.BIN`, `GLOBALB.BUN`). On `XO_LightWallA` the
fallback handed its 16-triangle cone the fixture's own lens texture
(`OBJ_SHOEBOX`, a bright disc over a dark housing) stretched across a 2.8 m
card. Smaller cards read as pale discs standing in the road; the large ones
read as opaque black slabs across the sky, hiding whole city blocks.

`n2_slot_is_lightpool()` drops those ranges in both walkers (`n2_walk_meshes`
and `winst_collect_model`). World mesh count on `STREAML4RA` falls 57540 ->
56922; vista count and the dedup residual are unchanged.

This is a named constant, **not a detector**. The other 270 unresolved
submeshes (91 distinct keys, 79 of them crackable to `ARC_*` / `SGN_*` /
`LOD_*` / `RDP_*` names) do name real textures that merely live in a pack this
bundle does not ship, and for those the sibling fallback is a reasonable
wall-for-wall guess -- dropping them would punch holes in buildings. Add a
constant here only for a material proven to have no art anywhere.

`N2Mesh.mat_exact` records whether a range owns its texture structurally. A
verified range and a true single-slot object set it; an unresolved range or a
multi-slot whole-object fallback does not. Render draw modes are consumed only
when this bit is set, so malformed data can keep its geometry and diffuse
fallback without spreading cutout/blend/additive state across unrelated faces.

Cars use the same linkage when multiple records resolve to different
textures, OR (M135) when they classify to different material categories via
`matid -> 0x134013` (see above) even on the SAME texture -- the case a
same-texture body-and-glass object needs. If neither differs, the car
remains one draw to avoid a pixel-identical draw-call increase.
Implementation: `n2_mesh_texslots`, `n2_mesh_matslots`, `n2_mesh_submeshes`,
`n2_walk_meshes`, and `n2_walk_car`.

**Complete-tier LOD selection (M135).** Splitting by material means two LOD
tiers of one part need not contain the same slices. `n2_walk_car` tags every
slice emitted from one `0x80134010` occurrence with the same `tierid`
(`N2Mesh.tierid`, a plain per-object counter, distinct from `namekey` which
groups a part ACROSS tiers). `n2_car_dedupe_lod` competes whole tiers, not
individual meshes: tiers sharing a `namekey` whose union bounding boxes
overlap score by SUMMED index count (`nidx`, not `nverts` -- `n2_add_pair`
duplicates the full vertex array per split slice, so scoring by vertex count
would reward a heavily-split tier for slice count alone) across all their
slices, and the losing tier's slices are dropped as one unit. This is what
lets an unmatched glass slice in the winning tier survive instead of being
compared, and possibly dropped, independently of its own tier's body panel.

### Scenery group membership (`0x34107` / `0x34108`, M139)

**PROVEN structure; activation UNDECODED.** Each surveyed companion `L4R*.BUN`
contains an override table and a variable-length group list. This is not the
polygon directory: event graphics and ordinary scenery share spatial sections.

`0x34107` is an exact array of 8-byte records, without a filler prefix:

| offset | type | measured meaning |
|---:|---|---|
| `+0` | `u16` | STREAM `0x34101` section id |
| `+2` | `u16` | zero-based placement row in that section's `0x34103` |
| `+4` | `u16` | copy of that placement's `+0x1a` flags |
| `+6` | `u16` | number of references from the group list |

`0x34108` consists of records of length `align4(52 + 2 * count)`:

| offset | type | measured meaning |
|---:|---|---|
| `+0/+4` | `u32[2]` | unconsumed fields (both 11 in sampled named groups) |
| `+8` | `char[32]` | bounded, terminated group name |
| `+40` | `u32` | group-name hash (`h=0xffffffff; h=h*33+c`) |
| `+44` | `u32` | unconsumed field; not assigned activation semantics |
| `+48` | `u32` | reference count |
| `+52` | `u16[count]` | indices into `0x34107`, followed by alignment bytes |

Across RA/RB/RC/RD/RF/RG/RH/RR, all 21,342 override rows resolve to an existing
section/placement with identical flags. All group hashes, index bounds and
stored reference counts agree. RA's 166 start/finish graphics each resolve
through this chain to their own `BARRIERS_<event>` group. For example,
`BARRIERS_4144` includes overrides 4968/4969, which point to section 1718,
placement rows 98/99. No spatial transform is changed by this interpretation.
Other real groups include `PLAYER_BARRIERS_*`, `FREE_ROAM`, stage-qualified
free-roam groups and `SMOKEABLE`. Do not treat every group as race-only.

The six RB start/finish graphics are **not** in this override table and carry
zero placement flags. Thus group membership alone cannot hide every venue's
race graphic. Neither direction-specific flag bits, runtime state changes,
nor career-stage activation is proven here. The placement's `+0x1c` is **not**
a global override index (6,430/6,431 RA reverse-link attempts fail); join by
the override's section and placement fields instead.

The checked reader is `src/world_group_reader.h`, shared by the instance
builder and `tools/world_group_audit.c` (moved out of `tools/` in M140).
`make world-group-test world-group-audit` builds asset-free corruption tests
and an audit that verifies the complete companion table before visiting members.
It rejects malformed chunk boundaries, duplicate tables, excessive nesting,
unterminated names, hash/refcount mismatches and out-of-range references. The
normal instance-driven free-roam loader applies the conservative `free`
selection: placements exclusively owned by numeric event groups are absent
from both rendering and collision. Explicit capture-only previews may select
one numeric event. Live `--event` races request that event's own authored group;
a missing own group retains shared/unknown memberships while other named events remain inactive. Retail direction/career
activation timing remains undecoded.

**M140/M147 conservative selection policy, not retail activation semantics.**
`--world2 --scenery-preview free|EVENT` accepts only an instance audit or a
fixed `--shot` capture. `src/world_scenery.h` recognizes complete numeric
`BARRIERS_<id>` / `PLAYER_BARRIERS_<id>` group names, unions membership by
`(section id, placement row)`, and suppresses a placement only when all its
memberships are numeric event groups and none matches the selection. `free`
selects no event. Non-event, unknown, ungrouped and shared-with-non-event
placements stay visible. Positive isolated-race selections also omit exclusive
`BARRIERS_CAREER<stage>` and `BARRIERS_DEMO` closures. This is a gameplay policy,
not recovered activation semantics; free roam also omits exclusive career/demo closures, and active
or shared-with-ordinary memberships still win. L4RA demo-only panels at section
619, rows 26/27 blocked the supported Circuit 4001 road; the shared selection fix
lets its three test opponents finish without removing ordinary walls.
Model names, whole sections and undecoded flag bits
are not visibility rules. In particular, forward/backward graphics in a selected
event remain together, and RB's six ungrouped graphics remain present.

The builder checks every override target in the chosen home bundle, including
targets outside the viewing radius: section/row/type bounds, matching copied
flags and unique targets. Missing/inconsistent data fails assembly before
replacing the caller's scenes; an absent own group preserves the requested numeric selection and common scenery. The compact
selection owns its data after the companion file is released. Filtering occurs
before instance emission, so render meshes and collision extraction consume
the same scene; ground prototypes and all placement transforms are unchanged.
`world_instance_build` remains an unchanged-policy wrapper; the explicit
`world_instance_build_for_event` argument is `0` (off), `-1` (free) or a
positive event id. Selection applies when loading the scene, including in-process
race transitions; it is not a decoded career activation system.

The runtime's current first instance chunk is a 1000 m placement neighborhood.
This radius is policy, not a file-format field. At the L4RA airport reference it
selects 2,149 placements and emits 13,797 meshes after dedup; the former 700 m
chunk emitted 12,017. A 7000 m whole-bundle measurement emitted 96,841 meshes,
confirming that full residency is not a substitute for moving-neighborhood
streaming. ROAD/TERRAIN prototypes remain the separately emitted support base.

### Object identity, duplicates and vista objects

The STREAM bundles are overlapping route/event working sets, not adjacent
open-world tiles. `--track ALL` blindly unions incompatible supersets and is
not evidence of correct world composition. `world_dedup` removes exact
same-place duplicates using texture key, six-axis bounds, triangle count and
vertex count before bounds, ground grids and GPU batches are built.

`PAN_*`, measured `TRN_PANARAMA*`, and `*_WORLD_LOD` sheets are authored
vista/impostor geometry. They are routed to `World.vista`, never to the
ordinary collision/ground scene. Production `--tier ordinary` does not batch
or draw them; the common texture-binding pass may still resolve a vista key
before the tier gate. `--tier full` is experimental because some shipped
sheets are fully opaque and still form hard horizon bands.

Two shipped cases do not follow the spelling, and both are decided by
measurement or material instead (`--vista-census 0` prints the verdict per
object):

* **`PAN_OCEAN` / `PAN_OCEAN_B98` are the water surface, not backdrop.** They
  measure 10021 m and 3278 m of footprint against 13 m and 0 m of Z, planarity
  1.000, dominant normal exactly `+Z`. `n2_is_ground_sheet` keeps a PAN\_ object
  that measures as a large horizontal sheet in the ordinary scene, so the bay
  and the canals have water and ground again instead of reading through to the
  fog colour. Every other PAN\_ asset is a vertical wall or a closed shell; the
  nearest horizontal one is a 234 m `PAN_INDUSTRIALNORTHBRIDGE_C` fragment, so
  the 1000 m footprint floor separates them with the sample's own gap.
* **Three impostor objects carry no usable name.** Retail left one as
  `OBJECT01` and two whose `0x134011` name leaf is not ASCII at all, so no
  spelling rule can reach them and they entered the world as solid geometry --
  `TRN_COASTROADLOD_A_DM` laid across the conservatory park at z 0..35 with an
  `ARC_PANARAMABUILDINGS` billboard standing in it. `n2_impostor_atlas` matches
  the object's `0x134012` slots against the six backdrop atlases instead:

  | key | atlas |
  |---|---|
  | `0801e3a1` | `TRN_COASTROADLOD_A_DM` |
  | `531cd2db` | `TRN_TREELINEA_DM` |
  | `5db83e34` | `TRN_FREEWAYLOD_A_DM` |
  | `b83ca1b6` | `TRN_TREES_FENCES_LOD_A_` |
  | `ea9eb4a8` | `ARC_PANARAMABUILDINGSB_` |
  | `ea9f4109` | `ARC_PANARAMABUILDINGSC_` |

  `tools/impostor_atlas_census.py TRACKS` is the evidence: of 28985 objects in
  the eight bundles, 89 bind one of these atlases, all 89 are impostors, and
  only those three leftovers are not already `PAN_*` / `TRN_PANARAMA*`.

### Rejecting a mis-decoded texture (`n2_tex_noise`)

A decoded texture is discarded only when **all three** of these agree, measured
over STREAML4RA's 1119 decoded textures (`--tex-audit` prints a `TEXFAIL` line
per rejection):

| term | what it measures | mis-decoded | genuine art |
|---|---|---|---|
| contrast | mean abs difference of pixels 4 apart | > 55 | (17 of 21 also exceed it) |
| hue coherence | fraction of horizontal edges whose per-channel deltas share a sign | 0.37 – 0.48 | 0.54 – 1.00 |
| palette | distinct 5-bit colours per pixel | 0.097 – 0.122 | 0.008 – 0.081 |

Contrast alone used to decide, and it was wrong 17 times out of 21. The
survivors of that rule were dumped and inspected: only four are actually
mis-decoded (`SFX_LIGHT_BEAMA`, a second beam sheet and two track-barrier
surfaces — rainbow confetti). The other seventeen decode perfectly and are
exactly the assets that are legitimately high-contrast at a 4-pixel stride:
neon shop signs (VICTOR, ROYALE, PARKING, BURGER KING, W HARPER), a Chinese
shop sign, an `ARC_M` letter panel, an LV badge, a cherry-blossom cutout, a red
neon tube and a terracotta roof tile — the roof tile scored the **highest**
contrast of all 21 (109) purely because its ridges alternate on the sample
stride.

Those seventeen lost their texture, and a world batch with a named-but-
unresolved key is dropped by `main.c`'s `b->unresolved` gate or falls
back to flat grey, so the signs rendered as blank slabs. With the three-term
rule, STREAML4RA binds 1115 distinct textures instead of 1098 and reports 4
`TEXFAIL`s instead of 21. The rule is strictly narrower than the old one, so no
texture that used to bind can stop binding; the regression fixture lives in
`tools/world_texture_test.c` (`test_noise_rejects_only_mis_decodes`).

### Instance-driven world districts (`0x341xx`)

**PROVEN.** A companion regional bundle supplies polygonal districts through
`0x80034150` → `0x00034152`. Its payload starts with a run of `0x11` filler;
the following records are variable-length:

| offset | type | meaning |
|---:|---|---|
| `+0x08` | `u16` | district id |
| `+0x0a` | `u16` | polygon vertex count, 1–64 |
| `+0x0c` | `f32[4]` | XY bounding rectangle `{min_x,min_y,max_x,max_y}` |
| `+0x1c` | 8 B | not consumed by the current reader |
| `+0x24` | `f32[vertex_count][2]` | polygon XY vertices |

The complete record length is `0x24 + vertex_count * 8`; a run of `0x11`
bytes may separate adjacent records. The loader validates both the enclosing
chunk boundaries and this computed length before copying a polygon.

The STREAM bundle contains one or more `0x80034100` sections. Each usable
section has all three children below; their payloads may start with `0x11`
filler, which is skipped only when that makes the remaining length an exact
record stride.

| child | layout used by OpenUG2 |
|---|---|
| `0x00034101` | at least 16 bytes; `u32` district id at `+0x0c` |
| `0x00034102` | 68-byte type records; display name in the first 32 bytes; three authored model keys at `+0x20/+0x24/+0x28` |
| `0x00034103` | 64-byte placement records |

**Model identity (M137 implementation).** The type record's three `u32` keys
reference the `u32` at `0x134011 +0x10` after that leaf's filler. They describe
the primary model and explicit alternative detail models. The instance reader
tries them in stored order, skipping zero or unavailable keys. This is an
availability fallback, **not** a reconstruction of retail's distance-based LOD
or district dependency selection. It never invents an A/B/Z name substitution.
If at least one key is present but none resolves, the placement remains missing;
it must not silently select an unrelated same-name model. The old name lookup
is retained only when all three keys are zero.
Multiple resident copies with the same stored key still select the first copy;
section-specific copy selection remains unimplemented. A matching key solves
the measured name ambiguity, not the whole region/dependency problem.

The stored object name starts at `0x134011 +0xa4` after filler in the surveyed
U2 world records. `winst_model_identity` validates a bounded, terminated name
there, falling back to the old scan for short/unknown layouts. The keyed lookup
does not recompute identity from this name: names can be truncated while the
stored key still identifies the complete name. Scanning earlier numeric header
bytes for text also misnamed actual tree models, breaking their lookup and
semantic classification. Correcting the instance-reader name restores the
source scenery class without a name-based visibility exception. The legacy
non-instance name reader is unchanged.

Measured evidence: the L4RB type names `XT_TreewallA_1a_00` and `_2a_00`
reference primary model keys `809d61db` and `80af7a5c`. Their primary models
exist but the old scan called both `mmRBl`. The types also explicitly reference
`80ab1754` / `80bd2fd5`, the corresponding Z-detail models, in slot 2. Two
start/finish graphic variants can share the same shortened display name yet
have distinct stored keys; selecting by key resolves the measured 2.326/4.610 m
bounds discrepancies without changing any placement transform.

Research credit: [noclip.website](https://github.com/magcius/noclip.website),
Jasper St. Pierre and contributors, particularly its Most Wanted
[model/instance reader](https://github.com/magcius/noclip.website/blob/6b16cfda00ef5af3ee2a66d8b928bb0bf700e5b6/src/NeedForSpeedMostWanted/region.ts)
and [region management](https://github.com/magcius/noclip.website/blob/6b16cfda00ef5af3ee2a66d8b928bb0bf700e5b6/src/NeedForSpeedMostWanted/map.ts),
at revision `6b16cfda00ef5af3ee2a66d8b928bb0bf700e5b6`. These were architecture
and research references; no source code was copied or ported. The project is
[MIT-licensed](https://github.com/magcius/noclip.website/blob/6b16cfda00ef5af3ee2a66d8b928bb0bf700e5b6/LICENSE)
and its license notice explains its reverse-engineering provenance. U2's
68-byte type records and model-index offset differ from MW's 72-byte records
and must be verified separately. Any future code reuse must preserve the
applicable copyright/license notices and be reviewed for project policy.

A placement record has `f32[3]` AABB minimum at `+0x00`, AABB maximum at
`+0x0c`, `u16 type_index` at `+0x18`, `u16 flags` at `+0x1a`, translation
`f32[3]` at `+0x20`, and a 3×3 signed-`i16` row-major rotation/scale block at
`+0x2c`. Each rotation/scale cell is divided by 8192.0; negative values stay
negative. The runtime keeps that binary row-major 3×3 ordering in the matching
`WInstPlacement.matrix` slots; it does not transpose the block. Translation is
kept at matrix elements 12–14, with identity for the last row/column. This
record convention is required for the decoded placement to match authored
instance bounds when the engine's direct placement multiplication is applied.
Malformed or truncated chunk/section data makes the corresponding parser/walk
fail according to its API. Records are skipped and never placed when
`type_index >= type_count`, any placement AABB coordinate is non-finite, an
AABB minimum exceeds its maximum, or the bbox lies outside the requested focus
radius. Once an eligible selected placement reaches `winst_place_mesh`, an
allocation/copy failure, a non-finite transformed coordinate, an absolute-limit
failure, or a scene-append failure aborts the entire staged build atomically;
destination scenes remain untouched rather than receiving a partial placement
set.

Districts may overlap. For an explicit focus point, OpenUG2 chooses the
smallest containing polygon as the home district and marks every district
whose XY bounding rectangle is within the configured radius. It then selects
one STREAM bundle that actually contains a section for that home id; it does
not compose every overlapping bundle. If no such section exists, assembly
fails without replacing the destination scenes.

Models in the selected bundle are collected as local prototypes: their object
matrix is retained as prototype metadata, not baked into the local vertices.
An in-range non-ground placement applies its own decoded matrix directly to
the prototype selected by its type's authored model keys. ROAD/TERRAIN prototypes are instead emitted once:
a unique prototype with an authored object matrix uses that matrix; otherwise
it uses identity and remains in its local/world coordinates. Unreferenced
non-ground variants are not emitted. Existing vista classification still routes
those prototypes to `World.vista`, and existing deduplication, terrain bias,
ground-grid, collision, batching, and texture ownership rules run afterward.

## Textures

Two TPK container variants are used. World textures may be P8, DXT1 or DXT3;
car textures may be raw BGRA, DXT1, DXT3 or DXT5 after decompression. Decoders
populate `N2Tex.rgb` and retain a separate alpha plane only when at least one
pixel is not fully opaque.

**Track TPK (uncompressed table).** Header block `0xb3310000` holds fixed
`0x7c`-byte records. The record is the NFSU2 texture header (per the Nikki
library, minus a `0x0C` padding prefix that this in-place variant omits):

```
+0x18 BinKey (u32)          // record hash — what meshes reference
+0x24 Offset (u32)          // texture data offset
+0x28 PaletteOffset (u32)   // palette offset (P8 only)
+0x2c Size (u32)
+0x30 PaletteSize (u32)     // 0 = not palettized; 1024 = 256-entry RGBA
+0x38 Width  (u16)
+0x3a Height (u16)
+0x3e format (u8)           // PROVEN (M135-R): 0x08 P8, 0x22 DXT1, 0x24 DXT3
+0x45 order  (u8)           // draw-order hint (unused by OpenUG2)
+0x49 usage  (u8)           // PROVEN (M135): 0=opaque/normal, 1=cutout (alpha test)
+0x4a blend  (u8)           // PROVEN (M135): 1=source-alpha blend, 2=additive
+0x4b wz     (u8)           // depth-write hint (unused by OpenUG2)
```

`order`/`usage`/`blend`/`wz` are only read when a **complete** `0x4c`-byte
record is present (`i + 0x4c <= hend`); a truncated header leaves all four at
their zero-initialized default, which maps to opaque — matching every other
"missing data degrades to the safe/legacy behaviour" rule in this codebase.

Pixels are in block `0xb3320000`, under its **`0x33320002`** sub-chunk payload
(a `0x33320001` info sub-chunk comes first), and **`Offset`/`PaletteOffset` are
relative to the data start after the `0x11` alignment filler** that prefixes it
(same quirk as vertex buffers). Three pixel formats occur, distinguished by the
record, not the (always-0) compression byte:

- **P8** (`format == 0x08`, palette present) — a 256-entry RGBA palette at
  `PaletteOffset` then 8-bit indices at `Offset`. This is what the
  road-surface textures use (`RDP_AIRPORT_ROADPATCH_A` is 512×512 P8), which
  is why they looked like high-entropy noise until decoded through the
  palette — **not** a swizzle.
- **DXT1 / DXT3** (`format == 0x22` / `0x24`, no palette) — dispatched
  directly from the `+0x3e` tag (**PROVEN, M135-R**: independently verified
  against 2309 real records across three files —
  `tools/tex_format_census.c` in scratch —  with **zero contradictions**:
  P8=0x08 36/36, DXT1=0x22 2033/2033, DXT3=0x24 240/240; superseded the old
  `Size > w*h*9/10` heuristic, which is no longer consulted for format
  selection at all). `PaletteSize` is still cross-checked as a corruption
  guard (P8 requires a real palette; DXT1/DXT3 require none); a tag that
  matches neither a proven format nor its expected `PaletteSize` is
  **rejected**, not guessed at (`tools/car_material_test.c` T11–T13).
  `DXT5` (`0x26`) and `BGRA8` (`0x20`) are recognised by name (an external
  reference claims those values) but never observed on real world data —
  those belong to the car `TEXTURES.BIN` offset-slot path
  (`n2_load_car_tex_by_key`), which has its own, separately-proven
  compression byte.

  Once the format is known, the byte-length **bound** still uses a
  per-format **ceil-to-4 block count**: `bx=(w+3)/4, by=(h+3)/4`; DXT1 needs
  `bx*by*8` bytes, DXT3 needs `bx*by*16` bytes. **PROVEN fix (M135):** the
  previous bound used a flat `w*h/2` estimate, which under-validated DXT3 by
  2× and let `n2_dxt3` read up to 46 bytes past the end of the data block on
  a non-multiple-of-4 texture — a latent OOB read, now closed (see
  `tools/car_material_test.c` fixtures T7/T8/T8b).

Alpha semantics are format-specific and **PROVEN** by A/B renders:

- P8 palette entries are RGBA and palette byte 3 is alpha.
- DXT1 is transparent only in the `c0 <= c1` mode and only for selector 3.
- DXT3 carries an explicit 4-bit alpha value per pixel.
- An all-255 decoded plane is discarded, preserving the opaque RGB upload path.
- Authored alpha bytes are never normalized, thresholded or color-keyed by the
  decoder; the raw palette/DXT alpha value is passed through byte-for-byte.
  Any dimming/discard decision belongs to the renderer (see "Draw modes"
  below), not the texture loader.

### Draw modes (M135)

`n2_tex_mode(const N2Tex *t)` derives one of four modes from the header bytes
above:

| `usage` | `blend` | mode | meaning |
|---|---|---|---|
| `1` | — | `N2_DRAW_CUTOUT` | alpha-test discard below 0.5; no blending |
| `2` | `1` | `N2_DRAW_BLEND` | depth write off, `GL_SRC_ALPHA`/`GL_ONE_MINUS_SRC_ALPHA` |
| any | `2` | `N2_DRAW_ADD` | depth write off, `GL_SRC_ALPHA`/`GL_ONE` |
| else | else | `N2_DRAW_OPAQUE` | depth test+write on, no blending, no discard |

Measured `(order, usage, blend, wz)` signatures from real TPK records:
opaque `(0,0,0,1)`, cutout `(0,1,0,1)` (`OBJ_RAILING`, `STREAML4RA`), blend
`(5,2,1,0)`, additive `(5,2,2,0)`. One oddball signature, `(0,2,0,1)`, occurs
on both a water surface and a parking-wall texture that both render opaque in
the retail game despite carrying non-opaque alpha — `n2_tex_mode` maps it to
`N2_DRAW_OPAQUE` (falls through both `usage==1` and `blend` checks), matching
observed retail behaviour; it is not currently distinguished from ordinary
opaque textures because no further evidence separates the two real uses.

The mode is per-texture-record, threaded from `world_bind_textures` through a
parallel `mtexmode`/`modes` array (mirroring the existing `mtex` GL-texture-
handle array) into `N2Batch.drawmode`, and dispatched at draw time: opaque and
cutout batches share the ordinary pass (cutout toggles the shader's
`uAlphaTest` uniform per batch); blend and additive batches are collected into
separate arrays (grown to the full batch count, never silently capped) and
drawn in a second pass after all opaque/cutout geometry, with depth test on
but depth write off (so they don't occlude each other or later translucents,
but are still correctly hidden behind real opaque geometry). Blend batches
are sorted back-to-front (`n2_sort_back_to_front`, GL-free, unit-tested) by
squared bbox-centre-to-camera distance with a deterministic smaller-index
tie-break; additive batches draw after, in their original stable
texture-sorted order.

**Authored alpha in blend/additive passes (PROVEN fix, M135-R):** the shader
multiplies the sampled texture's own alpha into the output alpha
(`uTextureAlpha`>0.5: `t.a*uAlpha`) for exactly this deferred pass — every
other pass (opaque, cutout, cars, HUD/debug) keeps the flat `uAlpha`-only
output unchanged. Before this fix, a `BLEND` texture always drew fully
opaque and an `ADD` texture always drew at full strength, regardless of its
own per-texel alpha, since the shared lit fragment path only ever emitted
the flat `uAlpha` uniform. Texture alpha is never gated on `uVColor` —
per-vertex prelight strength and authored texture transparency are
independent controls.

**LOC4 fallback (M135-R/M136 census):** `world_bind_textures` falls back to the
shared `LOC4DYNTEX.BIN` car-texture library (`n2_load_car_tex_by_key`) for a
key its own region TPK can't resolve; that reader never decodes
`order`/`usage`/`blend`/`wz`, so a LOC4-resolved key is always
`N2_DRAW_OPAQUE`. Ordinary world-material traffic in the two reference scenes
still resolves from the regional TPK. The authored `SKYDOME` is the measured
exception: its two `0x134012` slots live only in LOC4 and are rendered by the
dedicated sky pass, which consumes the cap's decoded DXT3 alpha directly and
does not use the ordinary-world draw-mode value.

### Authored sky (`SKYDOME`)

The real object has two texture slots and two exact material ranges:

```text
slot 0  5fb8bcd1  SKY_SUNRISE_A_CAP
slot 1  2414a01e  SKY_SUNRISE_A
range 0 start 0   count 288  mat 1   // 96 dome triangles
range 1 start 288 count 144  mat 0   // 48 cap triangles
```

Both keys are absent from the regional TPK and present in LOC4. Consequently
the mesh parser must preserve a structurally valid sky range's authored slot
even when the region-local key inventory cannot resolve it; texture resolution
happens later in `world_bind_textures`. The matching LOC4 pairs are sunrise
`2414a01e/5fb8bcd1`, sunset `27f186b7/3e6947ea`, and night
`8a9a05cf/b0eb9302` (dome/cap). Only the two proven sunrise source keys are
profile-remapped. `SKYDOME` exactly and the `SKY_` prefix classify as sky;
names merely containing those letters, such as `XB_SKYDOMEB_*` and
`XB_FACTORYSKYLIGHT*`, remain ordinary geometry.

### Authored district light sources (`0x135003`)

**PROVEN.** STREAM bundles contain nested `0x00135003` leaves whose payload,
after the leading `0x11` filler run, is a sequence of 96-byte records. The
production parser uses the bounded recursive chunk walk; it does not search
texture or geometry bytes for the leaf id.

| record offset | type | current meaning / validation |
|---|---|---|
| `+0x07` | `u8` | enabled when exactly `1` |
| `+0x0c` | `u32` | packed little-endian RGBA |
| `+0x10` | `f32[3]` | world-space position |
| `+0x1c` | `f32` | outer radius; require `0 < value <= 5000` |
| `+0x30` | `f32` | inner radius; require `0 <= value <= outer` |

The parser rejects a malformed leaf as a unit when its post-filler size is not
divisible by 96, ignores disabled/non-finite/out-of-range records, and
deduplicates exact accepted records. A map-wide 9999 m record is excluded by
the measured 5 km safety ceiling rather than by an asset-name exception. Real
single-region censuses produce 2,228 accepted unique sources in L4RA and 193
in L4RB.

These records do not own a texture slot. Rendering explicitly resolves the
shipped `SFX_FLARE_GLOWA` key `0x17e5ebd2` through the normal regional -> LOC4
-> master texture resolver, then draws camera-facing additive quads at night.
The current renderer sizes the visual quad from the inner radius and retains
the outer radius as validated source metadata; neither value participates in
collision, ground, navigation or physics.

Track meshes bind `0x134012` slot keys to TPK `BinKey` values. Each region's
own `STREAM*.BUN` TPK carries its
grass/road/prop textures (names vary per region: `TRN_GRASSC` vs
`ORG_GRASS_001`, `RDP_PARKING…` vs `RDP_AIRPORT_ROADPATCH_A`), and the shared
`TRACKS/LOC4DYNTEX.BIN` (a compressed offset-slot TPK, same format as car
TEXTURES.BIN) holds the sky + facade textures.

A STREAM file can contain many header/data pairs. `n2_tpk_open` records every
`0xb3310000` header and the following `0x33320002` data base once; lookups
then search those blocks without rescanning the file. World texture resolution
is region-local first, then shared packs. The region file buffers remain owned
by `World` until `world_bind_textures` finishes decoding and GPU upload.

**Common gameplay textures (M138).** The same uncompressed-table decoder also
reads `GLOBAL/InGameCommon.bun`. `STARTLINE`, key `96d35495`, is absent from the
regional/shared TRACKS sources surveyed but present there as a 64x64 DXT3
record: `(order, usage, blend, wz) = (5,2,1,0)`, decoded alpha 153..204.
The keyed, placed `421X_STARTFINGRAPHIC` mesh references it directly. Without
this source it drew a flat grey fallback rectangle; resolving the record gives
the authored checkered road marking at the unchanged transform and UVs.
This library is the last texture fallback, in a second pass after all original
own-region/LOC4/master binding attempts; it never contributes geometry or changes
material slot selection. Missing common files remain optional. Buffer and TPK
index lifetime ends after binding. This proves the texture dependency, not the
event activation rule; do not infer free-roam visibility from a texture match.

**Car TPK (`CARS/*/TEXTURES.BIN`, compressed).** Same outer container
(`0xb3300000` → `0xb3310000` header + `0xb3320000` pixels). The header block
has three sub-chunks: `0x33310001` (info), `0x33310002` (hash table),
`0x33310003` (**offset-slot table**, 24-byte records):

```text
+0x00 u32 Key
+0x04 u32 AbsoluteOffset  // file-absolute compressed block
+0x08 u32 EncodedSize
+0x0c u32 DecodedSize
+0x10 u32 HeaderFromEnd
+0x14 u32 Unknown
```

The field at slot `+0x10` was previously called `RefCount`; that is wrong.
It is `HeaderFromEnd`. After decode:

```text
P = DecodedSize - HeaderFromEnd + 0x88
P+0x00  u32 key             // must equal the slot key
P+0x20  u16 width
P+0x22  u16 height
P+0x26  u8  compression     // 0x20 BGRA, 0x22 DXT1, 0x24 DXT3, 0x26 DXT5
```

Most blocks at `AbsoluteOffset` are **JDLZ** compressed (header `"JDLZ"`,
decoded size at `+8`, encoded size at `+12`). Some are a 16-byte `"HUFF"`
wrapper around an EAC Huffman stream. The embedded header, not a guessed square
size, selects dimensions and compression. The full compressed mip chain is
retained for the GPU fast path when possible; RGB(A) is always decoded as the
portable fallback. Production reader: `n2_load_car_tex_by_key`.

## Car material → texture binding (`GEOMETRY.BIN`)

Each car mesh (`0x80134010`) carries, alongside its `0x134011` **material**
record (part name + a bounding box + a transform), a `0x134012` **texture-slot
list**: 8-byte entries `u32 key, u32 0`. The slot index is significant:
`0x134B02.mat_id` selects it positionally. If records select different keys,
the object is split into per-range draws; this prevents a tiny badge or light
atlas from being painted over an entire body panel. If the linkage is absent or
all records resolve to one key, the whole-object path is retained.

Not every listed key is guaranteed to be present in that car's TPK. The
production path intersects slots with `n2_car_tex_keys`, validates every range,
and keeps the dominant material slice's part-family key so LOD deduplication
still chooses one authored tier. A zero/unresolved small slice is allowed to
remain untextured; inventing a texture would be less accurate than the data.

The hashes are the standard **NFS "bin" hash** of the asset name:

```
h = 0xFFFFFFFF;  for each byte c of the name:  h = h*0x21 + c   (mod 2^32)
```

The same hash function is used throughout the asset system, but do not infer
that every adjacent hash is a diffuse key. The **proven** texture relationship
is `0x134B02.mat_id -> 0x134012[mat_id].key -> TPK slot key`. Part names and
their hashes instead drive part identity, LOD grouping and customization-family
selection in `n2_walk_car`, `n2_car_dedupe_lod` and
`n2_car_apply_config`.

### Car configuration and external part libraries

`KIT00` is the stock whole car. Higher `KITnn` groups contain only the part
families overridden by that body kit; dropping `KIT00` would remove most of the
vehicle. Hood alternatives use `STYLEnn` and override the stock `KIT00_HOOD`.
Spoilers and wheels are separate `CARS/SPOILER` and `CARS/WHEELS` libraries,
not variants embedded in the selected car's `GEOMETRY.BIN`.

The parser derives three identities from the part name:

- `namekey`: trailing LOD suffix removed;
- `vkind/vnum`: `KITnn` or `STYLEnn` variant;
- `famkey`: variant token also removed, so replacement and stock family match.

This is why an apparently duplicated car object must not be discarded before
configuration and LOD selection have run.

## Racing lines & circuits (`TRACKS/ROUTES*/Paths*.bin`)

Container `0x80034147` → children `0x34148/49/4a/4c/4d`. Chunk **`0x34148`** is
the racing line/navigation record: **24-byte records**:

| offset | type | status |
|---:|---|---|
| `+0x00` | `f32 x` | **PROVEN**, STREAM world X |
| `+0x04` | `f32 y` | **PROVEN**, STREAM world Y |
| `+0x08` | `u16` segment index, `u16` flags | segment index **PROVEN** against `0x34149`; flags **UNKNOWN** |
| `+0x0c`, `+0x0e`, `+0x10` | `u16` links, `0xffff` absent | bounded node-index links; used by the topology fallback, direction/priority **UNKNOWN** |
| `+0x14` | `f32` | cumulative segment distance, **PROVEN** by monotonic runs |

`n2_load_path` consumes only XY for a circuit polyline. `world_load_nav`
connects consecutive sane records when their spacing is under
`NAV_LINK_MAX`, then welds coincident route runs within 5 m and builds a CSR
graph. Do not silently treat the `+8` low value as a district id: its measured
values span the whole city and change with route segments.

Chunk `0x34149` contains 220-byte segment records. Across the 105 event
files, all 2,923 records have a node start/end range (`u16` at `+28/+30`,
end exclusive) whose size equals the `u16` count at `+32`. Every node in
that range identifies the same segment index at its `0x34148 +8` field.
The first 16 bytes hold the segment name. The last-resort event reader consumes
these ranges and node links through the shared checked recursive chunk walker.
It validates range/count/ownership, finite coordinates, complete node coverage
and link bounds before building a graph. Consecutive edges stay within each
segment; explicit links join segments. Existing city routing connects ordered
outline targets. Edges longer than the existing 120 m route limit are omitted.

This fallback runs only after the existing Paths/Routes course readers fail.
It treats explicit links as undirected and chooses shortest graph legs; retail
branch priorities and direction flags remain undecoded. The runtime subsequently
applies its normal grid, gate, road-support and body-collision checks. A decoded
route alone does not establish that the event can finish.

Routes come in two kinds: **open sprints** (first waypoint far from the last)
and **closed circuits** (first ≈ last). Runtime circuit discovery is restricted
to the loaded region's own `ROUTES<stem>/` directory; the old whole-city
bounding-box filter admitted foreign courses because outlier geometry inflated
the box.

### Freeroam vs. race event — the split is the game's own

Each `ROUTES<REG>/` holds one `Paths<id>.bin` per race event **plus** a single
`PathsFreeRoam.bin`. Comparing the leaves settles which data belongs to which:

| leaf | `Paths4001` | `Paths4006` | `PathsFreeRoam` |
|---|---|---|---|
| `0x34148` racing line | 0x1ff8 | 0x2ac0 | *absent* |
| `0x34149` | 0x2260 | 0x49e8 | *absent* |
| `0x3414a` | 0x10ffc | 0x10ffc | 0x10ffc — **byte-identical** |
| `0x3414c` event catalog | 0x3fc0 | 0x3fc0 | *absent* |
| `0x3414d` | 0xfb70 | 0xfb70 | 0xfb70 — **byte-identical** |

`0x3414a`/`0x3414d` are the shared per-region city data (same MD5 in every file
of a region); the other three are per-event. The free-roam path file has no
race-event overlay. `Routes<id>F/B.bin` confirms the distinction from the other
side: the current bounded inventory finds nine distinct sector names in
`Routes4001F.bin`. Earlier notes reporting six were incomplete. Sector names
identify route geometry, not a car roster.

The actual ordered free-roam road points are in `RoutesFreeRoam.bin` leaf
`0x34121`, not in the shared `PathsFreeRoam.bin` leaves. After the 8-byte leaf
header, each route record stores matching `u16` point counts at `+44` and
`+46`. Its points begin at `+128`, at 56-byte intervals, with world `f32 X/Y`
at point offsets `+4/+8`. The next record begins after `140 + 56*N` bytes;
the final record omits the trailing 8-byte header. Six shipped files contain
14,695 points. Runtime links consecutive points within each record and joins
nearby route ends; the original junction-selection rules remain unknown.
Each point also carries two `u16` values at `+24/+26`; divided by 256, they
range from 1 to about 53 m and vary smoothly along routes, consistent with
lateral road extents. OpenUG2 conservatively uses the smaller value to offset
traffic toward the right lane, then checks the shifted point against ROAD.
No speed, headway, spawn timing or camera-visibility rule has been decoded from
these records. Current traffic pacing and off-screen spawn selection are
OpenUG2 behavior built on the authored road geometry, not recovered retail rules.

### Authored route connections (`0x34122` / `0x34123`)

**OBSERVED:** each `0x34121` route body starts with a `u16` type and a unique
per-file `u16` route ID; the name starts at +8. Leaf `0x34122` begins with four
zero bytes followed by 32-byte connection records:

| Record offsets | Stored fields |
|---|---|
| +0, +20 | zero `u32` words in the inspected files |
| +4/+6 | primary A/B point indices (`u16`) |
| +8/+10 | primary A/B route IDs (`u16`) |
| +12, +16 | opaque packed words; do not assign turn/right-of-way semantics |
| +24/+26 | secondary A/B route IDs, or paired `0xffff` |
| +28/+30 | secondary A/B point indices, or paired `0xffff` |

Across six populated free-roam files, all 3,951 primary and 1,489 non-sentinel
secondary pairs resolve to bounded points in the same file. Primary tuples
have reversed counterparts, sometimes with different metadata; serialization
symmetry is not proof of legal travel in both directions. Some linked points
are over five coordinate units apart, so proximity welding cannot reproduce
the authored table exactly. Check road support, height/layer and body clearance
before using a pair as a drivable connection.

In the inspected L4RA file, `0x34123` contains 38 records of 276 bytes. Each
has two bounded route/point references and sixteen 16-byte slots after a
20-byte prefix. Its operational purpose is **UNKNOWN**: neither the record
count nor slot count establishes traffic emitters, signal phases or car limits.

Event `Routes<id>F.bin` uses the same record reader. If a car lies beyond the
conservative local search and that search finds no clear line, race avoidance
searches the source-derived corridor accepted at the grid, including the car's
current lateral position. Every candidate still needs road footprint support
and solid body clearance; the width alone does not establish a drivable lane.

When an endpoint ground probe loses ROAD on a hill, course selection retries
with approximately one-metre steps from the predecessor, carrying the last
road height forward. Successful endpoint probes keep their existing behavior.
The retry still requires ROAD throughout, endpoint footprint support and the
existing body sweep; it does not grant clearance through barriers.

### `0x3414c` — race event catalog (**SOLVED**)

A flat table of that region's events, **272-byte records**:

```
+0  u16  event id (4001..)          — also the Paths<id>.bin number
+2  u8   outline point count N
+3  u8   circuit flag  1 = closed lap circuit, 0 = point-to-point sprint
+4  u8   flag (0/1)
+5  u8   course length hint, units of 100 m (observed 20/30/40/50/60)
+6  u16  pad
+8  33 * (f32 x, f32 y)             — track outline polygon, world XY
```

Only the first `N` points are live (the array is a fixed 33 slots and the tail
holds unused values). Closure is not universal: 42 of the 105 inspected live
outlines have different first/last XY points. Repeated catalog copies agree
in their live prefix, but unused tails vary in 99 entries; compare live fields,
not entire padded records. Census over the shipped regions: **105 events
— L4RA 60, L4RB 9, L4RC 12, L4RD 3, L4RF 8, L4RG 13.** `ROUTESL4RH` and
`ROUTESL4RR` have no numeric event Paths files and empty `RoutesFreeRoam`
route bodies in this dump. This does not prove absence of runtime AI there.

### `0x34146` — start grids (`TrackPosMarkers*.bin`)

8-byte `0x11` filler, then **48-byte records**:

```
+0  u32 11    +4  u32 11    +8  u32 name hash   +12 u32 slot index
+16 f32 X     +20 f32 Y     +24 f32 Z           +28 f32 0
+32 u16 heading (turn / 65536), +34 u16 unknown
+36 u32 track id (4000 = freeroam) +40 char[4] tag  +44 u32 0
```

The record count is **variable**, not universally 18. Event 4001 has 18; L4RB
sprint 4201 has 20 arranged as two ten-slot clusters for opposite directions.
These are **starting grids, not checkpoints**. Numbered forward slots use the
authored heading at `+32`; legacy unnamed grids derive heading from the course.

`+8` is the marker-name hash (`h=0xffffffff; h=h*33+c`) and `+12` the slot index
within that name. Measured names: `ForwardStartingPosition` (`0x8e55c895`, 8
slots), `ForwardStartLine` (`0x6a56b90a`, 1), `BackwardStartingPosition`
(`0x8f90f7bf`, 8) and `BackwardStartLine` (`0xeec70374`, 1). In all 35 events
whose two grids are at different ends, the Forward grid is the one at gate 0.
On divided freeway circuits (L4RA 4001–4006, 4083) the two grids sit on opposite
carriageways; the Backward one starts the field across the median. The loader
therefore keeps only Forward slots when an event ships them, sorted by slot
index. The fallback accepts unnamed positions; StartLine markers are always
excluded. Four-car fields prefer four abreast, then two rows of two; six-car
URL fields use three rows of two. The player occupies the last selected slot.
The entire formation must pass ground, wheel support, body clearance,
non-overlap and course-entry checks before placement.

### Checkpoint gates & laps — derived, not stored (Phase 72)

There is no separately decoded checkpoint list. The **course order** is the
event's own `0x3414c` outline polygon. `world_race_start` snaps each live
outline point to the nearest node from that event's `0x34148` range and builds
a gate perpendicular to the local course direction. For a circuit, the repeated
closing point is removed and gate 0 is start/finish. For a sprint, every live
outline point is retained and the last gate finishes the event without a lap
wrap. Only the armed gate can score and the previous→current car segment must
cross it, preventing proximity-only and out-of-order clears.

### Race barriers — no barrier chunk exists (**measured, Phase 71**)

A recursive chunk census of `TRACKS/L4R*.BUN`,
`GLOBAL/InGame{FreeRoam,Race,Drift,Drag}.bun` and every `ROUTES<REG>` file finds
**no blockade/barrier instance chunk** — in particular `0x0003410B` is not
present in any shipped file. Road closure is expressed as **omission**: the
event's route network simply does not contain the side streets. The engine
therefore derives barrier placement (`world_set_mode` in `src/world.c`) as the
links where the freeroam graph leaves the active event's corridor. Measured for
event 4001: 341 corridor nodes, **24 barriers, 103 directed links masked**.

## Global vehicle attributes (`GLOBAL/GLOBALB.BUN`)

`GLOBALB.BUN` is the decompressed form of `GlobalB.lzc`. It contains several
unrelated systems; do not describe the entire file as one flat AttribSys stream.

### Per-car geometry anchor

The shared bounded chunk walker locates one `0x00034600` table, including inside
nested containers. Its payload starts with eight `0x11` bytes followed by
2,192-byte records (46 in the local dataset). Each record has a bounded name
at `+0x00` and matching geometry path at `+0x40`.
`n2_global_car_record` rejects malformed framing, partial records, invalid
names/paths, multiple tables and duplicate requested records. Matching strings
in unrelated payloads are ignored; this is not a whole-file string search.

| offset from `anchor - 0x40` | type | meaning |
|---:|---|---|
| `+288` | `f32` | front axle X |
| `+292` | `f32` | front half-track Y |
| `+384` | `f32` | rear axle X |
| `+388` | `f32` | rear half-track Y |

The coordinates share the car model frame: front X is positive, rear X is
negative, and full track width is `2*abs(Y)`. Values reproduce real wheelbase
and track dimensions across the sampled fleet. `n2_global_wheel_attr` accepts
the record only after conservative plausibility bounds; otherwise the caller
falls back to measurements derived from that car's geometry.

### Source dimensions and inertia

`n2_global_car_shape` exposes the following without changing live handling or
wheel placement. Values must be finite, dimensions/inertia positive, and wheel
indices must match source order. The returned record offset provides provenance.

| record offset | type | meaning |
|---:|---|---|
| `+0x220` | `f32` | mass, interpreted as tonnes |
| `+0x224..+0x22c` | `3*f32` | physical body L/W/H, metres |
| `+0x230`, `+0x244`, `+0x258` | `f32` each | Ixx/Iyy/Izz, tonne·m² |
| `+0x120 + 0x30*k` | `2*f32` | wheel X/Y, metres, k=0..3 |
| wheel `+0x10`, `+0x14` | `f32` each | tyre radius and width, metres |
| wheel `+0x18` | `u32` | source wheel index |

Across all 46 local records, inertia matches the solid-box relation
`Ixx = mass*(W²+H²)/12` and cyclic permutations. These physical dimensions are
not render bounding boxes: fourteen traffic records share generic mass/body
values. The live axle reader validates mirrored wheel pairs and allows source
body dimensions to extend its passenger-car bounds; BUS therefore uses its
authored axle/track positions instead of geometry fractions.

`--vehicle-compare A B` and `--fleet-census` print the source record offset,
mass, dimensions, inertia and all four tyre sizes alongside existing results.
`make car-material-test` covers synthetic/malformed tables; optionally run
`./build/car_material_test ../GLOBAL/GLOBALB.BUN` to check every local source
record's inertia and wheel mirroring without committing game assets.

### Stock and upgraded powertrain fields

The following offsets are relative to the 2,192-byte car record and are
validated before runtime use:

| offset | type | meaning |
|---:|---|---|
| `+0x220` | `f32` | mass in tonnes |
| `+0x2c0`, `+0x460`, `+0x4a0`, `+0x4e0` | 64-byte blocks | stock, L1, L2 and L3 transmissions |
| gearbox `+0x08` | `f32` | final-drive ratio |
| gearbox `+0x10` | `f32` | rear-drive fraction (stock block) |
| gearbox `+0x18` | `u32` | forward gear count |
| gearbox `+0x20..+0x3c` | `8*f32` | reverse, neutral and six forward ratios |
| `+0x300..+0x308` | `3*f32` | idle and two RPM thresholds (redline/limiter labels provisional) |
| `+0x310..+0x330` | `9*f32` | primary engine samples, interpreted as kN·m |
| `+0x334..+0x354` | `9*f32` | secondary engine samples added to the primary curve during stock composition |
| `+0x380` | `f32` | currently used as steering response; meaning unverified |
| `+0x530`, `+0x570`, `+0x5b0`, `+0x5f0` | `9*f32` each | master gain curve and three upgrade curves |

Stock engine input is the pointwise sum of the primary and secondary curves,
including when no performance upgrade is selected. `N2PhysicsAttr.torque` holds
that composed curve, not the primary array alone. An independent local check of
46 car records found seven with nonzero secondary curves. Synthetic parser tests
cover all nine additions, the zero-secondary fallback and malformed values.

The source of this composition fact is the
[nfsu2-re record recovery research](https://github.com/troikoss/nfsu2-re/blob/main/docs/pc-physics-parameters.md).
Its provenance is original executable analysis; this parser change is independently
written and contains no recovered code or retail sample data. This establishes
stock curve composition, not the full engine or tyre/body dynamics. OpenUG2's
power-level additions remain provisional and do not implement the original
upgrade-selection graph.

The current mapping uses idle to limiter in eight equal intervals. Constructor
research in [the engine recovery notes](https://github.com/troikoss/nfsu2-re/blob/main/docs/pc-vehicle-physics.md)
supports these endpoints for the primary curve; the torque unit interpretation
and complete upgraded behavior remain unverified. The master gain curve
equals the third upgrade curve; the first two are 0.34 and 0.68 times it in the
local data. Source evidence supports four gearbox blocks directly. The gain
curves form a consistent upgrade ladder, but their exact division among the
separately sold engine, ECU and induction products is not yet proven, so OpenUG2 exposes them as power
levels rather than inventing category-specific percentages. Brake, tyre and
suspension upgrade fields remain undecoded.

### Stock drivetrain and axle-force fields — local aggregate verification

**Interpretation evidence `[inf]`; no retail runtime observed.** The offsets
below come from the Stage-1 static-evidence spec; their semantics are not
retail-proven. Local aggregate checks across the 46 local records (no
per-car values committed anywhere) support the framing:

| record offset | type | reading | Local aggregate (46 records) |
|---:|---|---|---|
| `+0x1E0` / `+0x200` | `f32` | axle slip-angle threshold (front / rear) | 12.0 in all 46 (inactive per the analyst note) |
| `+0x1EC` / `+0x20C` | `f32` | axle peak-grip multiplier | front 1.425–2.5; rear 1.425–3.0 |
| `+0x1F0` / `+0x210` | `f32` | axle sliding-grip multiplier | front 1.2–1.7; rear 1.2–2.8 |
| `+0x2C0` | `f32` | launch fraction (stock block) | 0.8 in all 46 |
| `+0x2C4` | `f32` | unnamed, stock block | 500.0 in all 46 |
| `+0x2C8` / `+0x2CC` | `f32` | final-drive ratio (stock block) | 3.27–4.8; equal per record in 46/46 |
| `+0x2D0` | `f32` | rear drive share | values {0.0 (×10), 0.4 (×1), 0.5 (×23), 0.575 (×1), 1.0 (×11)} |
| `+0x2D4` | `f32` | clutch coupling coefficient | 0.04–0.05 |
| `+0x2D8` | `u32` | forward gear count (stock block) | 4 (×3), 5 (×29), 6 (×14) |
| `+0x2DC` | `f32` | clutch rate | 2.0 / 4.0 in every record |
| `+0x2E0…+0x2FC` | `8*f32` | reverse, neutral, six forward ratios | forward ratios strictly descend in 46/46 |
| `+0x300` / `+0x304` / `+0x308` | `f32` | idle / redline / limiter RPM | 800–1000 / 5000–8500 / 6000–9000 |
| `+0x30C` | `f32` | engine rotational inertia | 19 distinct values, ~2e-4–7e-4 tonne·m² |
| `+0x358…+0x360` | `3*f32` | engine-braking samples | 0.065–0.41; present in all 46 |
| `+0x398` / `+0x39C` | `f32` | slip-dependent vertical-torque restriction scalars | 17.0 and 0.0 in all 46 |
| `+0x270` | `f32` | yaw-inertia multiplier kI (Stage 5 BR-1) | 2.0–2.28, 11 distinct |
| `+0x370` | `f32` | unknown (tiny) | 3e-4–5e-4, 11 distinct |
| `+0x374` / `+0x378` | `f32` | aero coefficients cF / cR (Stage 2) | 0.13–0.185 / 0.14–0.18 |
| `+0x37C` | `f32` | aero-to-preload factor kA (Stage 2) | 0–0.03, 5 distinct |
| `+0x384` / `+0x388` / `+0x38C` | `f32` | brake strengths Kb / Kh and rear share beta (Stage 3) | 0.4–1.0 / 0.3–0.67 / 0.47–0.6 |
| `+0x390` / `+0x394` | `f32` | rear slip-bonus b0 / b1 (Stage 5) | 0.0 and 2.5 in all 46 |

Cross-checks: inertia matches the solid-box relation within 1% in 46/46;
the forward-ratio sequence descends strictly in 46/46; the stock and L1–L3
gearbox blocks share framing. The local drive-share values suggest three
coarse classes (0.0 / 0.5 / 1.0) plus two intermediate records; the class
mapping is **not** proven.

**Provenance boundary.** Every offset above lies below `+0x3E0` and is
therefore identical to retail in the local image. The L1–L3 blocks (`+0x460`
onwards) are inside the local installation's modified region
(`GlobalB.lzc`/`GLOBALB.BUN` bytes `+0x3E0…+0x5AE` differ from retail), so
local L1–L3 observations are **install-specific** and must not be cited as
retail evidence. Common constants (0.8, 500.0, 12.0, 17.0, 0.0) may be
engine-wide defaults rather than per-car tuning; their producers are
unresolved.

### `steer_ratio` (`+0x380`) — correlation survey (2026-11-04)

**Aggregates only; meaning unverified.** Correlations across the 46 local
records: with kI 0.47, with Izz 0.41, with mass 0.37, with wheelbase 0.26,
with rear share 0.06 (none). Traffic records average 1.017 (fourteen at
exactly 1.0; the heavy traffic record is 1.25); racers average 1.106
(range 1.01–1.25). Values are consistent with a discrete,
handling-family-flavoured multiplier but equally fit other bucketings;
no consumer is wired and no behavioural correlation exists. Keep the
"steering response" label provisional. Per-car values are not committed;
reproduction needs the local data root.

### Unmapped record regions — structural survey (2026-11-04)

**Structure `[obs]`; semantics unknown unless tagged.** Local
`GLOBAL/GLOBALB.BUN` only. The local image is documented to diverge from
retail in record bytes `+0x3E0…+0x5AE`; offsets outside that window match
retail in the local copy. Aggregates only; nothing here is a per-car dump.
"Traffic" below means the pooled records for TAXI, TAXI02, AMBULANCE,
FIRETRUCK, PANELVAN, PARCELVAN, PICKUP, 4DR_SEDAN, 4DR_SEDAN02, SUV,
MINIVAN, HATCHBACK, HATCHBACK02, BUS and COUPE.

| record offset | observation (aggregate) | reading |
|---:|---|---|
| `+0x340…+0x354` | 6 floats, ≤8 distinct each, range 0–0.1 | unknown; possible damage/wear factors |
| `+0x370` | 3e-4…5e-4, 13 distinct | unknown |
| `+0x3A0` | integer-valued, {0, 125…159}, 14 distinct; 0×15 and 155×11 dominate | candidate handling-class/preset ID; not correlated with rear share; unresolved `[inf]` |
| `+0x3C0` | 0.015 (all 46) | constant, unknown |
| `+0x3C4` | {0.199, 0.6, 0.801} | enum-like; unknown |
| `+0x3C8` | 0.125 (all 46) | constant, unknown |
| `+0x3CC` | {0.05, 0.06, 0.2} | enum-like; unknown |
| `+0x3D0…+0x3DC` | 0.05 (all 46) | constant, unknown |
| `+0x6A4…+0x6AC` | duplicated **exactly** at `+0x6B4…+0x6BC` in 46/46; one trailing zero each | unknown paired block |
| `+0x650`, `+0x670` | `(12.0, 1.0, 1.0)` constant pair + two ≈1.5–2.13 floats (differ between copies); traffic shares one default (2.0/1.5) | unknown paired block; 12.0 matches the axle slip-angle sentinel |
| `+0x6C0…+0x6CC` | four floats on the same scale as stock brakes (`+0x384`) and steer (`+0x380`); per-car values, **not** a constant multiple of stock; `+0x6CC == +0x380` in 24/46 | candidate upgraded brakes; no mapping proven `[inf]` |
| **`+0x6D0…+0x6EC`, `+0x6F0…+0x70C`** | two 8-float blocks, 0x20 stride, **identical field layout and scale to the stock axle suspension blocks** (j, k, d+, d−, a, U, L, ref); stiffer progression/stiffness/damping in every record; all traffic records carry one shared default (1.0/100.0/5.0/5.0/0.0/0.15/−0.15/0.15) | **strongest structural candidate for the upgraded suspension package (single tier, front+rear)** `[inf]`; no retail-name mapping, no second tier found |
| `+0x734…+0x738` | {−5.8, 1.7} in 44/46; heavy vehicles {−6.7, 2.3} | unknown constant pair |
| `+0x744…+0x748` | {−4.7, 1.7} in 44/46 (3 variants total) | unknown constant pair |
| `+0x754…+0x758`, `+0x7B4…+0x7B8`, `+0x7D4…+0x7D8` | three float pairs on similar scale; traffic shares one default (0.716/1.09/0.94 and 0.25/1.5); racers differ; `+0x754 != +0x7B4` per record | duplicated-style calibration blocks; unresolved |
| `+0x7F4` | 7 (racers) / 85 (traffic) — binary split by segment | unknown integer flag; candidate traffic/racer class |
| `+0x7F8` | 135–275 | unknown |
| `+0x7FC`, `+0x800` | −43 / +43 (all 46) | constant pair, unknown |
| `+0x808` | 7000 (racers) / 8000 (traffic) | unknown large-integer flag |
| `+0x810…+0x834` | paired sequences 48…53 / 200…360 | unknown; possibly paired limits |
| `+0x860…+0x870` | 2…120, traffic distinct from racers (e.g. 5.0/2.0/30.0 patterns) | unknown |
| `+0x7B0`, `+0x84C…+0x850` | bytes do not decode as finite float32 in all records | likely non-float data (integer/hash); float interpretation invalid here |

**What this survey does not prove**

- No name mapping from any block to a retail upgrade product
  ("sport suspension", brake kit, …) and no upgrade-tier ladder; only one
  suspension-shaped upgrade tier is visible.
- No separate tyre-upgrade grip block has been identified.
- The suspension candidate blocks are above `+0x5AE` and therefore outside
  the documented divergent window (retail-identical locally); the
  semantics remain unproven either way.
- Brake/tyre/suspension upgrade graph, purchase flow and tuning variants
  remain undecoded `[inf]`.
- Everything in the survey is a **structural** observation. Promotion to a
  named field requires cross-car analysis, behavioural correlation and a
  reviewed amendment before any engine use.

### `0x00135200` AttribSys records

The file also contains standard chunk-framed records with magic
`0x00135200`. `src/attrib.h` can enumerate them, find a bounded printable
instance name, read the class hash preceding that name and expose a conservative
trailing-float view for diagnostics. The per-car path anchors above are **not
inside those records**. Never use the generic AttribSys walker to justify fixed
offsets around a car path, or vice versa.

### Named AI vehicle presets (`0x00030220`)

A read-only local data check finds one 26,368-byte leaf, consistent with 32
824-byte records. Each record contains a NUL-terminated car model at `+0x08`
and a preset name at `+0x28`; all 32 model names match installed car folders.
Ten preset names contain AI labels, including demo vehicles and named AI
presets. The same table also contains player, character and promotional cars.
This establishes vehicle-preset identity, not a driving-behavior schema.
Remaining fields, tuning levels and runtime selection rules are **UNKNOWN**;
OpenUG2 does not currently consume this table.

The race `Paths` and `Routes` files above provide authored geometry and event
metadata. No verified aggression, overtaking, catch-up, braking or spawn-policy
field has been decoded from the inspected GLOBAL/route data. Current race pace,
passing and steering decisions are OpenUG2 policies in `src/ai.c`.

### Career events and opponent descriptors

**OBSERVED layout, PLAUSIBLE opponent semantics.** Two `0x80034a10`
containers in the inspected GLOBALB have a `0x34a1d` string pool and a
`0x34a11` event-record leaf. Their leaves contain 224 and 8 records of
**136 bytes**, respectively. This is separate from the 272-byte route catalog.

| Event-record offset | Observation |
|---|---|
| +0/+2/+4/+6 | Four `u16` offsets to bounded strings in that container's pool |
| +8 | `u32` hash of the +0-resolved event name; seed `0xffffffff`, multiply 33 and add each byte |
| +24/+28/+32/+36 | Nonzero `u16` values match track IDs in GLOBALB leaf `0x34201` |
| +64 | Five candidate opponent descriptors, each 12 bytes |
| +124 | Byte in 0..5; that many prefix descriptors have nonempty bounded names |

Each descriptor has a pool-name offset (`u16` +0), an undecoded `u16` +2,
a preset-like hash (`u32` +4), and four opaque bytes at +8. Seven counted
descriptor hashes match named character car presets, with matching character
names. The other 690 counted hashes equal the rolling hash of `RANDOM`.
That equality supports a generic opponent-selection hypothesis; it does not
recover the random pool, weights, difficulty fields or actual spawn loop.
Unused descriptor slots can retain stale values: do not count all five slots.

The track-definition leaf `0x34201` has 131 records of 296 bytes in this
dump, with a name at +0 and track ID at +140. These links establish static
event/name/track/preset associations, not executed race rosters. Validate each
record stride, string start/termination, count and reference before importing.
OpenUG2 currently has no production importer for these career descriptors.

The exact free-roaming rival selection table is **UNKNOWN**. Outrun-related
message identifiers in the pools do not bind an event record to roaming-car
selection. AI-named presets include demo cars and are not a proven free-roam
roster. Executable encounter labels remain leads until their consumers are traced.

## In-game HUD and interface bundles

The supplied shared file is `GLOBAL/InGameCommon.bun` (with an equivalent
JDLZ-compressed `InGameCommon.lzc`), not `InGameCommon.bin`.
`InGameMemoryFile.bin` is a separate resource and must not be substituted.
Full-buffer decoding also verifies `GlobalB.lzc` equals `GLOBALB.BUN`; these
comparisons establish stored equivalence, not which copy retail opens at runtime.

**OBSERVED inventory**, using the existing GL-free `src/ugui` readers over
original files:

| Bundle | Decoded FEng packages / objects | Embedded textures | Additional content / boundary |
|---|---:|---:|---|
| `InGameFreeRoam.bun` | 1 / 258 | 43 | Stored package is `HUD_SingleRace.fng`; trailing `0x8003b201` has 38 unresolved `0x3b210` records |
| `InGameRace.bun` | 13 / 1,332 | 43 | Race HUD plus pre/post-race screens; includes `HUD_SingleRace.fng` and `HUD_Short_Track.fng` |
| `InGameCommon.bun` | 27 / 2,179 | 113 across two packs | Shared dialogs, pause, split-screen/Outrun HUD, 14 model records, cinematic references and unresolved binary blocks |

Bundle FEng chunk `0x30203` carries a raw package. `0x30210` carries a
four-byte screen-hash prefix before the package; the package body can itself
be compressed. Use `ug_package_from_chunk` / `ug_fng_parse`, not a whole-file
printable-string search. The parsed object graph includes resource references,
image/text/group types, transforms, UI scripts, animation tracks and responses.
These scripts animate/handle the interface; they are not recovered vehicle AI.

Embedded texture catalogs use 124-byte records in the inspected packs, with
separate keys and pixel storage. A screen's image/font reference does not imply
that its bytes are embedded in that bundle. In the free-roam screen, only
21 of 47 image references resolve to that pack's keys; dependencies remain
external or unresolved. A successful pixel decode does not certify usable art.
See [HUD_SPRITES.md](HUD_SPRITES.md) for runtime sprite selection/fallbacks and
[FRONTEND_ASSETS.md](FRONTEND_ASSETS.md) for the broader resource graph.

`HUD_CustomTextures_ALL.bin` supplies the custom dial artwork used by the
current HUD. The numbered `HUD_CustomTextures_00..10.bin` files have no TPK
records in this dump; do not select a texture source solely by its name.
Stored placeholders and multi-mode HUD objects do not prove retail activation,
live telemetry, density controls or collision behavior. Unclassified trailing
blocks prevent a whole-file claim that such settings are absent.

## Engine sound banks (`SOUND/ENGINE/CAR_*_ENG_MB_SPU.abk`)

EA "Ginsu" engine audio: per-car banks of RPM-band loops. Header `ABKC`;
bank offset `u32 @+0x18`, bank size `u32 @+0x24` → a `BNKl` bank holding
**PT ("platform") headers**: `'P','T',u16 platform`, then a TLV stream of
`(tag u8, len u8, big-endian value)` — `0x84` sample rate (default 22050),
`0x85` sample count, `0x86/0x87` loop start/end, `0x88` data offset
(bank-relative). Engine banks carry **8 streams**: `[0]` idle, `[1..3]`
accel low/mid/high, `[4..6]` decel low/mid/high, `[7]` high/whine
(28 kHz). Verified across all 41 banks.

Stream data is **EA-XA mono ADPCM**, a mix of two frame kinds:

- **15-byte ADPCM frame** = 28 samples. Header byte: coef index in the high
  nibble, shift in the low. Per 4-bit nibble (high first):
  `s = ((nib << (20 - shift)) + prev*c1 + prev2*c2 + 0x80) >> 8`, clamped,
  with `c1 = {0,240,460,392}`, `c2 = {0,0,-208,-220}`.
- **`0xEE` frame (61 bytes)** = predictor reseed + 28 uncompressed samples:
  two `s16be` @+1/+3 are the encoder's exact `prev/prev2` seed for the
  following ADPCM run, then 28 `s16be` output samples @+5.

Both the frame grammar and the decode formula were pinned empirically: the
byte/sample accounting fits every stream exactly, decode is exact at every
`0xEE` resync, and the decoded loops come out near-perfectly periodic
(autocorrelation 0.94-1.00 — they're seamless engine loops).

## Native Ginsu sweeps (`SOUND/ENGINE/GIN_*.gin`) — SOLVED

The authentic per-car engine audio: one continuous recording of the engine
revving `rpm_min → rpm_max`, **named by car** (`GIN_Hummer`,
`GIN_Nissan_240SX`, …; `_DCL`/`Decel` variants for coast). There is no
car→bank table anywhere in the data — car identity lives in these
filenames (the `CAR_NN` .abk banks are the console-style fallback).

```
+0x00  "Gnsu20\0\0"
+0x08  f32 rpm_min, f32 rpm_max
+0x10  u32 n1 (=50), u32 n2 (=128)
+0x18  u32 total_samples, u32 sample_rate (24000-36000)
+0x20  u32 rpm_pos[n1+1]   // sample position at rpm_min + i*(range/n1);
                           // DESCENDING in _DCL files (they rev down)
       u32 grain[n2+1]     // ascending cycle-aligned loop points
       audio               // EA-XAS v0
```

**EA-XAS v0** (layout confirmed against vgmstream's `ea_xas_decoder.c`,
used as a format reference only): 0x13-byte frames of 32 samples. The
frame's leading `u32` (LE) packs coef index (bits 0-3), **hist2** (bits
4-15, read as `s16 & 0xFFF0`), shift (bits 16-19), **hist1** (bits 20-31,
same trick); both history samples are *output*, so every frame decodes
independently — which is what makes granular grain-jumping seamless. Then
15 bytes = 30 nibbles, high first:
`s = ((nib << 12) >> shift) + hist1*c1 + hist2*c2`, clamped, with the
CD-XA coefficient pairs `c1 = {0, 0.9375, 1.796875, 1.53125}`,
`c2 = {0, 0, -0.8125, -0.859375}`.

Validated: the local fundamental measured at every `rpm_pos` marker equals
**rpm/60 Hz** exactly (up to autocorrelation octave picks) — the curve is
the RPM→position map, and engine fundamental = one cycle per rev. Playback
= loop the grain window containing `pos(rpm)`, pitch-ratio
`rpm / rpm_at(grain)`, crossfade accel vs `_DCL` sweep by throttle load.

## Sponsor vinyls (`CARS/*/VINYLS.BIN`) and EA "HUFF" compression — SOLVED

`VINYLS.BIN` is one big compressed TPK (same offset-slot container as
`TEXTURES.BIN`), but every slot's blob is **EA "HUFF"**: a 16-byte wrapper
(`"HUFF"`, u32 version 0x1001, u32 decSize LE, u32 encSize LE) around an
**EAC canonical-Huffman stream** (signature `30FBh`, part of the same EA
Canada codec family as RefPack `10FBh`). Some `TEXTURES.BIN` slots use the
same wrapper around raw DXT payloads.

**EAC Huffman** (per Martin Korth's spec at problemkaputt.de/psx-spx.htm,
"CDROM File Compression EA Methods (Huffman)"; implemented from the spec
as `n2_huff` in nfsu2.h): big-endian bitstream; u16 method 30FBh..35FBh
(+3 skip bytes if bit8), u24 decompressed size, u8 escape code; canonical
code widths read until the Kraft sum fills the 16-bit code space; symbol
values delta-assigned over not-yet-used bytes; decode loop = literal
symbols, with ESC + varint n meaning {n=0: EOS bit or raw 8-bit literal;
n>0: repeat previous byte n times} — the run mechanism that gives vinyls
their ~60:1 ratios. Methods 32FBh/34FBh add prefix-sum unfiltering.
Varint = count zero bits z, then read (z+2) bits + (1<<(z+2)) - 4.

**Vinyl payload** = P8-style 8-bit indices (w*h base + mip levels) + a
256-entry RGBA palette + a 144-byte trailing record: `name[24]` (e.g.
`GOLF_AEM_SCORPION`), key u32 @+0x18, palette offset/size @+0x2c/+0x30,
pixel size @+0x34, width/height u16 @+0x38. **The palette ships all-zero**
— vinyls are runtime-recolored from the player's chosen colours (the ~16
used indices are art shade levels; the most frequent index is the
transparent background). The engine synthesizes a dark cut-vinyl look and
composites the art under the badge atlas across the whole body (body
panels share one UV layout; most carry no texture key of their own).

## Scripted / dynamic objects (`ZCV_` / `ZCS_`) — companion `L4R*.BUN`

Each district has a small companion bundle (`TRACKS/L4RA.BUN` … `L4RR.BUN`)
alongside its `STREAM*.BUN`. It contains named local shape definitions for
vehicle/scripted set-dressing (`ZCV_`) and props (`ZCS_`), plus separate parameter
tables. These names and shapes do not establish world placement or animation.
The bounded eight-companion census found 646 shape triples and 89 unique names.

### Entity definition table

Container `0x80034020` holds a bundle-dependent number of named definitions.
Each entity is a **triple** of consecutive chunks in this order:

```
0x39200  named local-shape header
0x39201  variable array of 48-byte vertex records
0x39202  8-byte filler prefix, then u16 adjacency indices
```

**`0x39200` header** (0x5c body). Offsets are into the chunk body (past the
8-byte `magic`+`size`), re-measured Phase 51 — an earlier draft of this table
was shifted 16 bytes because it missed the leading preamble:

| off  | type      | value / meaning                                            |
|------|-----------|------------------------------------------------------------|
| 0x00 | u32       | `0x11111111` — filler word                                 |
| 0x04 | u32       | 3                                                          |
| 0x08 | u32       | 3                                                          |
| 0x0c | u32       | per-record instance id (varies)                            |
| 0x10 | u32       | 0                                                          |
| 0x14 | u32       | 1                                                          |
| 0x18 | u32       | observed value 8; semantic purpose unknown                  |
| 0x1c | u32       | **name hash**, seed `0xffffffff`, `h = 33*h + byte` modulo 2^32; not FNV |
| 0x20 | char[32]  | **name**, NUL-padded (`ZCV_TrainEngineA`, `ZCS_Barrel_A`, …)|
| 0x40 | u32       | `0x24` — record type / sub-size, constant                  |
| 0x44 | u8[…]     | opaque trailing bytes; no transform interpretation established |

The engine parses these string-anchored (find the `ZCV_`/`ZCS_` token, verify
the constant `1, 8` at name-12 / name-8, take the hash at name-4) — robust
against the preamble, since the chunk body always ends `… 1, 8, hash, name`.
See `world_scripted_defs()` / the inspector's "Entity Definitions" tab.

**`0x39201` local vertices**, 48 bytes per record (**OBSERVED**):

| off  | type        | meaning                                                  |
|------|-------------|----------------------------------------------------------|
| 0x00 | u32, u32    | observed values `3, 3`; semantic purpose unknown          |
| 0x08 | u16, u16    | neighbor count and start index into `0x39202` |
| 0x0c | u32         | vertex identifier |
| 0x10 | f32[3]      | local XYZ |

Vertex counts vary; the arrays are not universally eight box corners.
Bound the complete record array and each adjacency range before use.

**`0x39202` connectivity**, variable length (**OBSERVED**):

| off  | type        | meaning                                                  |
|------|-------------|----------------------------------------------------------|
| 0x00 | u32, u32    | `0x11111111 0x11111111` — the standard 8-byte `0x11` filler prefix (as everywhere in this format) |
| 0x08 | u16[]       | vertex-neighbor indices, addressed by each vertex's count/start |

All 646 inspected shapes have bounded, symmetric, non-self adjacency; the
vertex ranges partition the index array. These are not explicit triangle
triplets. The triple establishes a named local shape, not a decoded world
transform or animation channel. The existing `world_scripted_defs` diagnostic
uses a legacy bounds approximation; it is not a general adjacency importer.

### Physical-property candidates and unresolved placement

Companion `0x34026` contains 256-byte named records associated with local shape
keys. Scalar +32 and diagonal values +48/+68/+88 fit box mass/inertia
predictions within 1% for 67 of 79 inspected L4RA records. This makes physical
authoring **PLAUSIBLE**; units, remaining fields and retail initialization are
unresolved. The earlier scale/state-only interpretation was unsupported.

No world-placement/animation linkage for these definitions has been recovered
from the inspected data. Missing literal hash occurrences cannot establish
that no placement exists elsewhere or under another encoding. `0x37090` has
no verified per-entity mapping; its operational purpose stays **UNKNOWN**.
Likewise, `0x8003b601` groups validate as bounded typed parameter fields,
not the vertex/index meshes assumed by an older helper. Do not derive a
world instance, render mesh or AI spawn rule from any of these shapes alone.

## The retail executable

`speed2.exe` is packed with **SafeDisc** DRM (section names `stxt774`/`stxt371`,
`.text` entropy ≈ 8.0). Static disassembly reads encrypted bytes, so the formats
above were recovered from the *data* files, not the exe.

## Credits / references

These format notes stand on the shoulders of prior community reverse-engineering.
Used only as references to understand the formats — all code here is an
independent implementation.

- **yugecin — [`nfsu2-re`](https://github.com/yugecin/nfsu2-re)**
  ([docs](https://yugecin.github.io/nfsu2-re/docs.html),
  [functions](https://yugecin.github.io/nfsu2-re/funcs.html),
  [structs](https://yugecin.github.io/nfsu2-re/structs.html),
  [enums](https://yugecin.github.io/nfsu2-re/enums.html),
  [vars](https://yugecin.github.io/nfsu2-re/vars.html)) — an excellent, detailed
  reverse-engineering project of NFSU2. The chunk-container structure (the
  8-byte `magic`+`size` header and the `0x8`-nibble nesting rule) was confirmed
  against yugecin's documentation. Huge thanks — great work.
- **[Nikki](https://github.com/SpeedReflect/Nikki)** — NFS modding library; the
  reference for the TPK layout and the NFSU2 texture-header struct. Its field
  layout was what revealed the `PaletteOffset`/`PaletteSize` fields and cracked
  the P8 road-surface textures (which had otherwise looked like noise).
- **[OpenNFSTools](https://github.com/MWisBest/OpenNFSTools)** — reference for
  the JDLZ decompression algorithm.
- **[vgmstream](https://github.com/vgmstream/vgmstream)** — reference for the
  Gnsu20 table layout and the EA-XAS v0 frame format (`meta/gin.c`,
  `coding/ea_xas_decoder.c`).

## Open: SFX_LIGHT_BEAMA pixel base (`GLOBAL/InGameCommon.bun`)

Key `2e95ce7c`, name `SFX_LIGHT_BEAMA`, 32x256, format tag `0x24` (DXT3),
`Offset = 166528`, `Size = 10752` (base level 8192 plus two mip levels),
draw descriptor `(order,usage,blend,wz) = (7,2,2,0)` -> additive.

Decoding at the block base `n2_tpk_open` derives (pixel block `0x33320002` at
45960 plus a 123-byte `0x11` filler run -> `dbase` 46083) produces saturated
per-block colour static. 46083 is not 16-byte aligned; the nearby 16-aligned
bases decode to flat yellow, not a beam gradient. Every other measured
consumer of this archive resolves correctly, so the filler rule is not
obviously wrong in general.

UNPROVEN. Until the real base is established, `n2_tex_noise` correctly rejects
the decode and the renderer skips the resulting untextured batches -- see
`docs/DEVELOPER_GUIDE.md`, "Unresolvable textures and the black light-shaft
wedges". The 747 authored `XO_*SPOTLIGHT*` / `*BEAMYA*` cones in L4RA are
invisible until this is solved.
