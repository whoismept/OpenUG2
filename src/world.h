/* world.h — OpenUG2 World module: multi-region city loading (STREAM .BUN
 * stitching into one scene), per-region texture binding, per-mesh bounds
 * for draw culling, and a grid-accelerated ground query. Owns the region
 * file buffers between load and texture upload. */
#ifndef OPENUG2_WORLD_H
#define OPENUG2_WORLD_H

#include "nfsu2.h"
#include "render.h"
#include "debug.h"   /* ScriptedDef, for world_scripted_defs */

#define WORLD_MAXREG 16
#define WORLD_MAXDIST 24
#define WORLD_MAXEVENT 128
#define WORLD_MAXBARRIER 2048
#define WORLD_EVPOLY 33

/* WEvent / WBarrier / MODE_* live in debug.h so the ImGui panel can read them
 * without pulling in GL — same reason ScriptedDef does. */

/* A district = one connected component of the drivable nav graph (Phase 68).
 *
 * Replaces the old bbox zones, which were unusable: 3D mesh bounds overlap, so
 * one building could span several "districts". Districts are now derived from
 * road connectivity, which is what a player actually experiences.
 *
 * DATA AUDIT first (Data-First): the nav node's +8 u32 is NOT a region code.
 * It is two u16s (bits 7..15 never set; the high half is usually 0xffff, the
 * same no-link sentinel as the other slots). Over all 18064 nodes the low u16
 * takes 113 values and EVERY value spans nearly the whole city (value 0:
 * X[-2986..2520] Y[-2274..3116]), where a geographic id would be compact. It
 * is constant in runs and flips at segment boundaries, i.e. a per-segment road
 * id/class. And the five district NAMES the game shows are UI-only strings
 * (the LANGUAGES files), bound to no coordinates anywhere. Hence topology, not lookup. */
typedef struct {
    char  tok[4];   /* 2-letter area code straight out of the TRN_ asset names */
    int   n;        /* nav nodes fused into this district */
    float bb[4];    /* x0, x1, y0, y1 of those nodes */
    float cx, cy;   /* node centroid */
    float medz;     /* median terrain elevation (separates the hill districts) */
} WDistrict;

typedef struct {
    char name[64];
    unsigned char *data; long len;   /* file buffer, freed after texture bind */
    N2Tpk tpk;
    int mesh0, mesh1;                /* this region's mesh range in the scene */
} WRegion;

typedef struct {
    N2Scene scene;
    WRegion rgn[WORLD_MAXREG]; int nreg;
    unsigned char *loc4; long loc4len;                       /* shared tex library */
    unsigned char *master; long masterlen; N2Tpk mastertpk;  /* single-region mode */
    N2Tex grass; int have_grass;                             /* terrain fallback */
    float (*mbb)[4];   /* per-mesh XY bbox (x0,y0,x1,y1) for culling + ground grid */
    WDistrict dist[WORLD_MAXDIST]; int ndist;  /* connected road components */
    int *navcomp;                             /* district index per nav node, -1 = none */
    /* AI/GPS navigation graph: the real drivable road network (see world_load_nav) */
    float *nav;        /* nnav * 2 floats: world X,Y of each node */
    int    nnav;
    int   *navedge;    /* nnavedge * 2 node indices */
    int    nnavedge;
    float  navbb[4];   /* x0,x1,y0,y1 over all nodes, for map framing */
    int   *adjstart;   /* CSR adjacency over the welded graph: nnav+1 offsets */
    int   *adjlist;    /* neighbour node indices */
    int    nadj;
    /* --- race events vs freeroam (Phase 71) --- */
    WEvent ev[WORLD_MAXEVENT]; int nev;
    int   *navev;      /* event index that contributed each nav node, -1 = none */
    char  *navopen;    /* 1 = node is inside the active corridor (all 1 in freeroam) */
    WBarrier bar[WORLD_MAXBARRIER]; int nbar;
    int    mode;       /* MODE_FREEROAM / MODE_RACE_EVENT */
    int    active_ev;  /* index into ev[], -1 in freeroam */
    int    nmasked;    /* directed CSR links disabled by the active barriers */
    WRace  race;       /* checkpoint / lap tracking (Phase 72) */
    /* M132: authored backdrop impostors (PAN_*, TRN_PANARAMA*, *_WORLD_LOD),
     * kept in their own scene. They are rendered as a background pass and are
     * deliberately invisible to ground selection, wheel support, collision,
     * navigation and spawn -- every one of those queries w->scene. */
    N2Scene vista;
} World;

/* Load the navigation graph for the loaded regions from TRACKS/ROUTES<REGION>/
 * Paths*.bin. Each file's 0x34148 leaf is an array of 24-byte records:
 *   +0 float X, +4 float Y, +8 flags, +12/+14/+16 u16 neighbour indices
 *   (0xffff = none), +20 float cumulative distance along the segment.
 * Files concatenate several segments, so consecutive records are only joined
 * when they are close enough to be one road (see NAV_LINK_MAX in world.c).
 * Returns the node count. */
int world_load_nav(World *w, const char *troot);

/* Flood-fill the nav graph into connected drivable components, largest first.
 * Returns the district count. */
int world_build_districts(World *w);

/* District of the nav node nearest (x,y) within maxdist, else -1. */
int world_district_at(const World *w, float x, float y, float maxdist);

/* Canonical district display names. The 2-letter codes are the artists' own
 * (read from the TRN_ asset names); the human names below were supplied by the
 * project owner from the in-game world map as EXTERNAL ground truth -- they are
 * NOT derived from the shipped files, which bind those strings to no
 * coordinates (see the LANGUAGES audit in Phase 66). */
const char *world_district_name(const char *tok);

/* Index of the nav node nearest (x,y), or -1 if the graph is empty. */
int world_nav_nearest(const World *w, float x, float y);

/* A* over the welded nav graph. Writes the node-index path (start..goal) into
 * out[] and the route length in metres into *outdist. Returns the node count,
 * 0 if unreachable. */
int world_route(const World *w, int start, int goal, int *out, int cap, float *outdist);

/* Parse the shipped race-event catalog for the loaded regions (Phase 71).
 *
 * DATA-FIRST: the split between freeroam and races is the game's own, not ours.
 * Every TRACKS/ROUTES<REG>/Paths<id>.bin holds three race-only leaves (0x34148
 * racing line, 0x34149, 0x3414c catalog) on top of the two leaves that are
 * BYTE-IDENTICAL in every file of a region (0x3414a, 0x3414d — the shared
 * freeroam network). PathsFreeRoam.bin carries only the shared pair. So a race
 * event IS a per-event route network laid over the common city.
 *
 * 0x3414c is that region's event catalog: 272-byte records,
 *   +0 u16 event id   +2 u8 outline point count   +3 u8 circuit flag
 *   +4 u8 flag        +5 u8 length hint (x100 m)  +6 u16 pad
 *   +8 33 * (f32 x, f32 y) track-outline polygon, closed (pts[n-1] == pts[0]).
 * Returns the event count. Call before world_load_nav so it can tag nodes. */
int world_load_events(World *w, const char *troot);

/* Switch the engine between freeroam and a race event (evidx into w->ev, or -1).
 *
 * In MODE_RACE_EVENT the drivable graph is masked to the event's own corridor
 * and every link that leaves it becomes a barrier — that is the road closure
 * the neon blockades represent, derived from the event's shipped route network
 * rather than from hand-placed coordinates. Returns the barrier count. */
int world_set_mode(World *w, int mode, int evidx);

/* Arm a race on event evidx: switch to MODE_RACE_EVENT, build the checkpoint
 * gates, load the start grid, and reset the lap counters (Phase 72).
 *
 * DATA-FIRST, and a CORRECTION to the Phase 71 note: chunk 0x34146
 * (TrackPosMarkers) is NOT a checkpoint list. Measured for event 4001, its 18
 * records all sit inside a 15 x 30 m patch around (-400, 255) — two 4-wide,
 * 2-deep STARTING GRIDS (one per race direction, matching Routes<id>F/B.bin)
 * plus one lone marker each. They are spawn slots, not course anchors.
 *
 * The course order comes from the event's own 0x3414c outline polygon, whose
 * vertices are ordered around the lap and land 2-23 m from a racing-line node
 * (measured over all 17 of event 4001's). Each vertex is snapped to its nearest
 * node so the gate sits on the road, and squared to the direction of travel
 * taken from its polygon neighbours. Cross-check: outline vertex 0 is 8 m from
 * the 0x34146 start grid, so both decodes agree on where the start line is.
 *
 * Returns the gate count (gate 0 = start/finish, 1.. = checkpoints). */
int world_race_start(World *w, const char *troot, int evidx, int maxlaps);

/* Feed the car's XY once per frame. Only the single armed gate can be cleared,
 * and only by CROSSING it (previous position -> current position segment test),
 * so corner-cutting and standing on a gate both fail to score. Returns 1 on the
 * frame a gate is cleared. */
int world_race_update(World *w, float x, float y);

void world_race_stop(World *w);

/* Push the car circle (centre pos[3], radius r) back inside the corridor if it
 * has crossed an active race barrier. No-op in freeroam. Returns 1 if it pushed. */
int world_barrier_push(const World *w, float *pos, float r);

/* Load trackname ("ALL" = every STREAM*.BUN under troot, else one region)
 * into w->scene. Builds per-mesh bounds and the ground grid. Returns the
 * mesh count (0 = nothing readable). */
int world_load(World *w, const char *troot, const char *trackname);

/* Decode + upload every distinct mesh texture (own TPK -> LOC4 -> master),
 * writing the key->GL map, then free the region buffers. Needs a GL context.
 * Returns the number of textures bound. */
/* Bind every distinct world texture. `mode` receives each one's draw mode
   from its record (0 opaque, 1 cutout, 2 blended, 3 additive); pass NULL if
   the caller does not care. */
int world_bind_textures(World *w, uint32_t *keys, GLuint *texs,
                        unsigned char *mode, int cap);

/* Ground height at (x,y): same contract as n2_ground_z but only tests the
 * road/terrain meshes whose bbox covers the point (grid lookup). */
/* Which kind of surface the ground query landed on. */
enum { WSURF_NONE = 0, WSURF_ROAD, WSURF_TERRAIN };

/* THE ground-contact query: the nearest-to-reference supporting layer under
 * (x,y), plus what it is. `fallback` doubles as the layer reference exactly as
 * before. Writes the surface Z (or `fallback` when nothing supports the XY) and
 * returns WSURF_*. world_ground_z is a thin wrapper on this, so there is one
 * layer-selection rule, not two. */
int world_ground_at(const N2Scene *s, float x, float y, float fallback, float *outz);

/* Same layer-selection rule, with the selected triangle's unit normal. Height
 * and chassis orientation therefore cannot come from different stacked decks. */
int world_ground_pose(const N2Scene *s, float x, float y, float fallback,
                      float *outz, float outn[3]);

/* Full result from the same ground selector. `mesh` is always an index in the
 * caller's scene (also when the world grid fast path uses a scratch scene).
 * Intended for contact diagnostics; it does not change selection behaviour. */
typedef struct {
    int mesh, tri, cat;
    float z, normal[3];
} WGroundHit;
int world_ground_hit(const N2Scene *s, float x, float y, float fallback,
                     WGroundHit *hit);

/* Derive a chassis plane from the footprint around an already-selected centre
 * contact. Returns 0 when the footprint cannot be supported by one coherent
 * layer; callers then keep their previous stable orientation. */
int world_ground_patch_normal(const N2Scene *s, float x, float y, float heading,
                              float front, float rear, float halftrack,
                              const WGroundHit *centre, float outn[3]);

/* Reachable wheel support (M130, corrected in M130-R).
 *
 * CANDIDATE vs CONTACT. A triangle covering the wheel XY is a CANDIDATE. Only a
 * candidate inside the wheel's own contact window -- [wheel_z - reach_down,
 * wheel_z + reach_up], both measured in centimetres, not metres -- is a
 * suspension CONTACT. The distinction is the whole point: on L4RB the selected
 * layer jumped from ROAD z=-9.114 to TERRAIN z=+4.224 in 16 frames, and a
 * surface metres overhead is a candidate the wheel can see, never support it
 * can stand on. There is no recovery reach: a car under a deck stays under it.
 *
 * SELECTION RULE among reachable candidates: the one CLOSEST to wheel_z wins
 * (smallest |dz|). Continuity, not height: the surface the wheel is already
 * riding is 0 m away and therefore always beats anything stacked above it, and
 * when the layer the wheel is on ends, the window -- not the tie-break -- is
 * what refuses the remaining layer overhead. (Measured: with the window at
 * 0.25/0.18 the L4RB deck 7.0 m up is rejected outright.)
 *
 * Returns WSURF_* when there is a contact and fills `hit` with it; returns
 * WSURF_NONE otherwise. `cand` (optional) always receives the nearest covering
 * candidate, contact or not, so a rejection can still be attributed to a mesh
 * and triangle. *verdict is one of: */
#define WWS_CONTACT  0   /* reachable contact; `hit` is valid                  */
#define WWS_ABOVE    1   /* nearest candidate is above the contact window      */
#define WWS_BELOW    2   /* nearest candidate is below it: the wheel is in air */
#define WWS_NOCOVER  3   /* no triangle covers the XY at all                   */
int world_wheel_support(const N2Scene *s, float x, float y, float wheel_z,
                        float reach_up, float reach_down,
                        WGroundHit *hit, WGroundHit *cand, int *verdict);

float world_ground_z(const N2Scene *s, float x, float y, float fallback);
void world_ground_selftest(void);

/* Push the car circle (centre pos[3], radius r) out of any near-vertical
   guardrail/fence face baked into the road/terrain. Returns 1 if it pushed. */
/* Report-only record of the first triangle that satisfied the push test, filled
 * during the SAME pass before the shove (NULL to ignore). Purely diagnostic:
 * the push itself is unchanged. */
typedef struct { int mesh, tri; float nz, zlo, zhi, edged; } WRailHit;
int world_wall_push(const N2Scene *s, float *pos, float r, WRailHit *hit);
/* M133 texture-binding census: set before world_bind_textures to report every
 * key that produced no GPU texture, split by cause. Diagnostic only. */
extern int g_world_texaudit, g_world_texnoise, g_world_texmiss;
/* M113 rail-candidate census (diagnostic; see world.c). */
extern int  world_rail_census;
extern long world_rc_cand[2][8], world_rc_push[2][8];
extern float world_rc_min[2], world_rc_max[2];

/* Decode the scripted-object entity DEFINITIONS (name + FNV-32 hash + local
 * OBB extents) from each loaded district's companion L4R*.BUN, deduped by
 * hash. Read-only inspector data — the companion carries no world placement
 * or mesh (see docs/FORMATS.md). Returns the number written (<= cap). */
int world_scripted_defs(const World *w, const char *troot,
                        ScriptedDef *out, int cap);

/* Build the scene from instance records: models stay in local coordinates and
 * each placement supplies its own transform, so a model that appears many
 * times is drawn once per placement instead of once overall. The older loader
 * bakes each model's matrix into its vertices, which then has to be undone for
 * every further copy; here there is nothing to undo. */
int world2_build(N2Scene *out, const char *troot,
                 const char *const *bundles, int nbundles,
                 float sx, float sy, float viewdist,
                 const unsigned char *loc4, long loc4len);
extern int  world2_on;
extern char world2_bundle[64];   /* district chosen by world2_build */

extern float world_inst_x, world_inst_y, world_inst_r;
int world_instantiate(World *w, const char *troot, float sx, float sy,
                      float viewdist);

#endif
