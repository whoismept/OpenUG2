# Graphify findings and measurement roadmap

This note records the useful architecture signals from the ImGui-excluded
Graphify projection. It is a navigation aid, not a replacement for source
inspection or runtime evidence.

## Provenance and limits

- Source projection: local `graphify-out/graph.json`.
- Filtered view: local `graphify-out-no-imgui/` (ImGui source paths, ImGui
  symbols and incident edges removed; non-ImGui callers remain).
- Current view: 856 nodes, 2,154 edges and 62 communities.
- Edge confidence: 83% extracted and 17% inferred (362 inferred edges).
- This is a filtered projection of an existing graph, not a fresh AST
  extraction. Verify every inferred relationship in the owning source before
  changing behaviour.

## Findings that affect the roadmap

### 1. Open-world loading has a clear measurement seam

The graph places these functions on the same cross-community path:

```text
world_neighborhood_load_facade
  -> world_instance_build_for_event
  -> WorldResident prepare/finish
  -> ground-grid activation, texture binding and batch upload
```

This matches the observed area-transition hitch and gives the next profiling
boundary. Measure the phases separately instead of treating one resident swap
as one undifferentiated stall:

1. target-cell selection;
2. `world_resident_prepare` / `world_neighborhood_load` CPU work;
3. CPU validation and ground-grid construction;
4. `world_resident_finish_step` / resource and texture upload;
5. activation and retired-resident cleanup.

Keep `peak-step`, aggregate `total-work`, and worker CPU time as separate
measurements. A lower aggregate total does not prove a lower frame hitch.

### 2. Ground, wall and rail contact are separate investigation paths

The graph separates the `physics.c` cluster from the `N2Scene` support helpers,
ground-grid construction and the `m94_rail` path. The useful collision seam to
trace is therefore:

```text
world_ground_hit / world_ground_grid_*
  -> ss_nearest_safe_road / support selection
  -> physics wheel/ride response
world_wall_clear_at / wall contacts
m94_rail / rail contacts
```

This does not prove a missing collision. It does prevent collapsing a fall or
pin into one generic “physics bug” before checking resident ownership, source
layer, support coverage and rail/barrier attribution.

### 3. Current AI driving is a compact, mostly isolated subsystem

`ai_drive_step` is strongly connected to `ai_projection` and `ai_segment`, but
the graph does not show a strong bridge to `world_route` or resident loading.
Treat the current AI-drive audit as an input/kinematic exercise, not automatic
proof that open-world streaming, ground support or race completion works.

Before using AI as a map explorer, the audit needs to record resident swaps,
route refresh after activation, wheel support and collision-source attribution.

### 4. High-degree parser helpers are not refactor targets by themselves

`n2_u32()` and `n2_find_leaves()` are the top “God Nodes”. They are shared
low-level parser helpers, so their degree is expected and partly structural.
Do not optimize or split them solely because Graphify ranks them highly.

No import cycles were found. The isolated nodes are mostly semantic image and
document nodes; they are not an actionable code defect.

## Ordered next work

1. Add phase-level timings to the resident route/drive audit and capture the
   same supported L4RA/L4RB poses before changing loader policy.
2. Add one collision attribution trace that correlates ground, wall, rail and
   resident-generation data at the first unsupported wheel or pin frame.
3. Extend the AI-drive audit only enough to prove resident-aware route refresh
   and support/collision coverage; do not use scripted completion as a proxy
   for retail race behaviour.
4. Re-run Graphify after these changes and use source-level checks to resolve
   the highest-value inferred edges. Keep generic parser helpers out of the
   priority list unless profiling identifies a real cost.

The local HTML/JSON view is useful for navigation, but production decisions
still require deterministic tests, runtime traces and source attribution.
