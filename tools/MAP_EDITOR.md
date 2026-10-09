# Map editor

Build and run from the repository root:

```sh
make map-editor
./build/map_editor --tracks ../TRACKS --map STREAML4RA
```

The native SDL/OpenGL tool uses the existing world loader and Dear ImGui. It is
independent of the game executable. Point `--tracks` at the installed game's
TRACKS directory; maps are discovered there. Full placed scenery is loaded,
including every material slice belonging to an object.
The default scenery is free roam; `--race EVENT` loads a source event's selection
and `--race 0` shows unfiltered source placements. Scenery selection is saved in
the project alongside its map.

Hold the right mouse button to look; WASD moves, Q/E changes height, Shift moves
faster and the wheel adjusts movement speed. Camera coordinates are also editable.
The viewport is unrestricted so underground placement defects can be inspected.

Reviewed collision exports are implemented as compiled C corrections in
`src/world_collision_rules.h`. The main game enables them at startup and applies
them again when loading streamed neighborhoods and race scenery. No external
collision file or extra launch option is needed. The editor continues to load
original source collision for authoring.

Model rules use the asset reader's model keys because display names may be
truncated. Removals retain visible geometry and driving ground; replacement
heights use placed geometry, with reviewed source polylines taking precedence.
The standalone authored boundary stays local. The current corrections target
STREAML4RA and its reviewed archive set; verify source fingerprints before
implementing edits for different assets or editions.

The top toolbar holds **Project / map**, **Save**, **Export all edits**,
**Undo**, **Redo** and **Apply preview**. Open **Project / map** to change
the project filename, open a saved project or load a different map.
Editing modes and selection lists are on the left; the selected source/item's
actions and properties are on the right. The central viewport is the map.
The bottom bar shows results and errors. Panels scroll independently.

Choose an **Edit mode** on the left:

- **Collision:** click an existing coloured wall or select a **Map source**.
  **Remove source collision** suppresses that source's body-wall collision in
  the preview. **Create replacement boundary** starts an editable box around
  the source; adjust its points and height in **Selection**. For an existing
  override, **Edit replacement boundary** also reactivates a removed boundary.
  **Restore original collision** discards the override. All three actions support
  undo/redo. Removal affects every collision wall of the selected source
  placement, including all material slices; it does not delete individual
  triangles, rendered geometry or driving ground. Source name, distance,
  status and optional **Source details** identify the affected placement.
  **Add custom wall** creates an independent wall; **Delete custom wall** removes
  it. **Project edits** marks source removals, boundaries and custom walls.
  Cyan marks object faces,
  orange marks road/terrain faces, pink marks generated and authored boundaries.
  The source view covers 80 metres and caps each class at 4096 candidate faces.
  Candidate triangles alone do not prove a contact: body height, burial and the
  physics solver's additional filters still matter.
- **Objects:** pick in the viewport or filter the source list. **Edit selected
  object** adds a project override; dragging a source object handle also creates
  one. Position, rotation, uniform scale, visibility and replacement are editable. **Pick replacement in map** chooses a donor;
  every material slice is copied into the preview, using the donor's centred
  world geometry. Rotation is relative to that donor's authored orientation.
- **AI paths:** show the source road graph; pick a node and **Copy connected
  street**, or create a path. Edit, insert and remove points, set speed and loop.
  Street copies stop at a junction rather than silently choosing a branch.
- **Races & icons:** select a decoded source event, copy its source course for
  editing, or create an event. Set icon/start position, event type, laps and stage.
  **Preview selected race scenery** reloads the source event's scenery selection.
  This is a map/course preview, not a running race simulation.
- **Lights:** inspect source lamp positions or create a lamp. Edit position,
  colour, power and source radius values. The preview updates lamp glow; it does
  not rebake the map's existing vertex lighting.
- **Shops:** place and name coloured shop markers: green body parts, yellow
  accessories, red paint/vinyl, blue performance, purple safe house.
- **Districts:** copy source district bounds or create a polygon; edit its label,
  corners and unlock stage. Source districts remain available as an overlay.

**Viewport handles:** select an object and use **Move (1)**, **Rotate (2)** or
**Scale (3)**. Drag a coloured X/Y/Z axis to move, a ring to rotate about that
world axis, or a square handle to change uniform scale. The white **XY** centre
moves across the horizontal plane. Geometry updates while dragging, including
all material slices of replacements. One drag creates one undo step; **Escape**
cancels it. Clicking/selecting an original object alone creates no edit.

Collision boundaries, AI routes and district polygons have movement handles for
the selected point. Lamps and shops use their position. For races, **Edit route
point** chooses a route point; turning it off moves the icon/start. Rotation and
scale apply to objects. **Grid snap** uses the specified spacing in world metres.
**Ground snap** puts an object's transformed base, or a point/marker, onto source
ground. Explicit Z-axis movement overrides ground snapping. Check stacked roads
with the XYZ fields because ground snapping uses the loaded source geometry.

Click a point handle to select it. **Shift + click** appends a point;
**Ctrl + click** moves the selected point or places an icon/lamp/object on ground.
XYZ fields allow exact heights and stacked-road corrections. Source road records
contain XY; their preview heights are projected onto loaded ground and may need
manual adjustment where decks overlap.

Numeric object/light changes use **Apply preview**; viewport drags and undo/redo
update the preview immediately. Path, wall, region and icon overlays also update
immediately. Save with **Ctrl+S**. Undo/redo (including **Ctrl+Shift+Z**),
project opening and unsaved-change prompts are available from the toolbar.

Use **Move & snapping** in the right panel to configure viewport handles.
In **Collision → Check for collision problems**, click **Check collisions**. It checks every
enabled project wall and source candidates within the selected radius of the
camera at scan start (80 m by default). **Entire source map** includes all loaded
source geometry for the current map/scenery. The checks flag:

- Walls crossing a supported source road-graph link at car height, including
  vertical ranges clipped to source triangles at the crossing.
- Aligned open barrier ends 0.15–2 m apart on the same height layer. This covers
  project walls and generated source boundaries; raw model seams are not assumed
  to be barrier ends. Connected ends, closed loops and T joins are excluded.
- Geometry fully below nearby supporting ground at several samples. Deep or
  ambiguous deck differences are excluded from the burial hint.
- Unusual wall heights or a project wall base more than 1 m above support.
  Low/high source-height hints apply to explicit walls and generated boundaries,
  avoiding ordinary road/terrain seams already ignored by physics.
- Project/generated segments shorter than 0.1 m horizontally.

Click a finding to focus the camera and select its item/source. **Show findings
in map** highlights the affected segment or representative source face. Source
findings are grouped by placement and check, with candidate counts. Findings are
review hints: an intentional race closure can cross a road, and a gap may be an
entrance. No collision geometry is removed or changed by validation.

`--check-collisions` starts the same local scan when opening the tool. For a
capture, allow enough `--frames` for the progress bar to finish.

Source/boundary ground checks run across frames with a 4 ms target; individual
operations and index/gap passes can exceed it. The scan runs only on request.
**Cancel check** stops it; edits, transforms, undo/redo and map loading invalidate
results and cancel a queued export. Validation uses indexed preview ground, so
transformed/hidden ground meshes participate without changing the original map.

Large inputs are bounded at 100,000 boundary segments, 2,000,000 road-link tests,
2,000,000 endpoint tests and 5,000 findings. **PARTIAL** is shown when a limit is
reached or the source graph is unavailable; it must not be read as an all-clear.
Completed gap findings survive a later work limit. Narrow the source radius to
review dense areas. Scan scope, thresholds, limits, XYZ, source/item IDs and
findings are included in collision and combined exports; those exports wait for
a current scan before writing. Category exports retain the full project payload.

Each mode has an **Export … report** button below its **Project edits** list.
**Export all edits**, in the top toolbar, combines every
section. For `map-edits.ug2map`, reports are written beside that project:

| Section | Filename | Readable details |
| --- | --- | --- |
| Collision | `map-edits.collision.txt` | Added/replaced/disabled walls, base XYZ, height, source objects |
| Objects | `map-edits.objects.txt` | Visibility, position, rotation, scale, whole-object replacement |
| AI paths | `map-edits.ai.txt` | Ordered XYZ points, target speed, loop, source start node and graph fingerprint |
| Races & icons | `map-edits.races.txt` | Event ID/type, laps, career stage, icon/start position, route points |
| Lights | `map-edits.lights.txt` | Position, RGB, power, inner/outer radius, original lamp reference |
| Shops | `map-edits.shops.txt` | Position, shop category/colour, name and enabled state |
| Districts | `map-edits.districts.txt` | Polygon, label, unlock stage, original district bounds |
| All edits | `map-edits.edits.txt` | Every section above |

Send a category report or the combined report for implementation; one file is
enough. Each contains a readable summary and original source context, followed
by the complete `.ug2map` project between `BEGIN_UG2MAP` and `END_UG2MAP`. The byte
count specifies the exact project payload. A category report focuses its summary
and source attribution on that section while retaining all categories in the
payload for context. No model or texture data is included.

The export is a snapshot of the current edits, including unsaved edits; it does
not mark the editable project as saved. Use **Save** separately to keep the
`.ug2map` project for reopening. Invalid edits cannot overwrite an existing
report. Missing source references are explicitly marked for review. For source
boundaries, enabled means replace original collision with the authored wall;
disabled means suppress original collision without adding a wall. Both retain
the visible model. An independent disabled wall requests no added collision.
Check map/scenery scope and source fingerprints before applying the report in
game; exporting it alone does not modify gameplay.
AI source IDs identify a copied start node in the loaded graph, not an entire
road-chain identity. Review graph fingerprints and branch mappings when applying
paths. Inactive AI, race and district records describe inactive authored edits;
they do not automatically delete the shared source road graph, event or district.

Projects use the versioned `.ug2map` format. Save writes a temporary file and
replaces the destination only after a successful write. Invalid/truncated files
leave the current document intact. Placed object slices share a bundle/placement
identity; other geometry has a content fingerprint. Missing references are
reported, including when race scenery hides an edited object.
Projects target the same installed archive set. Cross-edition/archive sharing
needs source-version checks; matching placement numbers alone cannot prove that
two different installations contain the same object.

Original archives are never written. Projects hold transforms, identifiers and
editable route/marker data, not model or texture payloads. Projects are ignored
by Git because copied source paths can contain retail coordinates.

**Current boundary:** project changes affect this authoring preview. The game
does not yet import `.ug2map` edits. Runtime collision reduction requires a
separate importer and indexed boundary representation; opening the editor does
not change the game's collision cost.

Checks:

```sh
make map-editor-test
./build/map_editor --tracks ../TRACKS --map STREAML4RA \
  --verify /tmp/editor-check.ug2map --frames 3 --shot /tmp/editor-check.png
```

The optional local-asset check exercises all categories, complete replacement
slices, collision overrides, source immutability, source identity after a map
reload, project roundtrip, viewport projection/picking at three window sizes,
coloured source-wall selection, native object/road/terrain removal/edit/restore,
native collision fixtures/read-only scans/export
interruption and all-category report exports. No retail data is a
committed test fixture.
