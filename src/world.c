/* world.c — OpenUG2 World module implementation. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>

#include "world.h"
#include "resource.h"

static void grid_build(World *w);
static void nav_build_adj(World *w);

static void world_scene_free(N2Scene *scene) {
    if (!scene) return;
    for (int i = 0; i < scene->count; i++) {
        free(scene->meshes[i].verts);
        free(scene->meshes[i].idx);
        free(scene->meshes[i].vcol);
    }
    free(scene->meshes);
    memset(scene, 0, sizeof *scene);
}

void world_neighborhood_free(WorldNeighborhood *neighborhood) {
    if (!neighborhood) return;
    world_ground_grid_free(&neighborhood->grid);
    world_scene_free(&neighborhood->scene);
    world_scene_free(&neighborhood->vista);
    for (int i = 0; i < WORLD_MAXREG; i++) {
        free(neighborhood->rgn[i].tpk.blk);
        free(neighborhood->rgn[i].data);
    }
    free(neighborhood->loc4);
    free(neighborhood->mastertpk.blk);
    if (neighborhood->master_mapped)
        res_unmap_file(neighborhood->master, neighborhood->masterlen);
    else
        free(neighborhood->master);
    free(neighborhood->commontpk.blk);
    free(neighborhood->common);
    free(neighborhood->grass.rgb);
    free(neighborhood->grass.alpha);
    free(neighborhood->grass.dxt);
    free(neighborhood->lights);
    free(neighborhood->mbb);
    memset(neighborhood, 0, sizeof *neighborhood);
}

void world_city_free(WorldCity *city) {
    if (!city) return;
    free(city->navcomp);
    free(city->nav);
    free(city->navedge);
    free(city->adjstart);
    free(city->adjlist);
    free(city->navev);
    free(city->navopen);
    memset(city, 0, sizeof *city);
}

static void world_append_lights(World *w, const N2LightSrc *src, int count) {
    for (int i = 0; i < count; i++) {
        int duplicate = 0;
        for (int j = 0; j < w->neighborhood.nlights; j++)
            if (n2_light_same(w->neighborhood.lights + j, src + i)) { duplicate = 1; break; }
        if (duplicate) continue;
        if (w->neighborhood.nlights == w->neighborhood.lightcap) {
            int next = w->neighborhood.lightcap ? w->neighborhood.lightcap * 2 : 256;
            N2LightSrc *grown = (N2LightSrc *)realloc(
                w->neighborhood.lights, (size_t)next * sizeof *grown);
            if (!grown) return;
            w->neighborhood.lights = grown; w->neighborhood.lightcap = next;
        }
        w->neighborhood.lights[w->neighborhood.nlights++] = src[i];
    }
}

/* ---- load-time duplicate stripper (Phase 48) ----
   The 8 STREAM*.BUN bundles are overlapping per-race supersets, not adjacent
   tiles, so stitching them (and even a single bundle on its own) stacks exact
   copies of the same surface at dz=0.000 — the source of the --track ALL
   z-fighting and a big chunk of wasted geometry (audit: 14551 of 28270 solid
   meshes, tools/zfight_audit.c). This drops every mesh whose
   (texkey, xyz-bbox, tri, vert) key was already registered, keeping the first
   occurrence. Runs on the whole scene BEFORE mbb/grid/batches are built, so
   the ground grid, the physics ground query, and the GPU batches all see one
   clean surface layer with no further plumbing.

   Identity is exact: two meshes are removed only when texkey, material
   ownership, bounding box, indices, vertices and prelight bytes all match.
   Counts and bounds are the cheap sort prefix; n2_mesh_content_cmp proves the
   payload rather than assuming equal dimensions mean equal geometry. An instanced
   prop reused at a DIFFERENT position has a different bbox, so it is NOT a
   duplicate and is kept — only same-place, same-asset copies are removed. */
static const N2Scene *dd_s;      /* comparator context; load is single-threaded */
static const float (*dd_bb)[6];
static int dd_cmp(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    const N2Mesh *ma = &dd_s->meshes[a], *mb = &dd_s->meshes[b];
    if (ma->texkey != mb->texkey) return ma->texkey < mb->texkey ? -1 : 1;
    for (int k = 0; k < 6; k++)
        if (dd_bb[a][k] != dd_bb[b][k]) return dd_bb[a][k] < dd_bb[b][k] ? -1 : 1;
    int content = n2_mesh_content_cmp(ma, mb);
    if (content) return content;
    return a - b;   /* stable: the earliest original index sorts first, so it
                       is the copy we keep */
}
static int dd_same(int a, int b) {
    const N2Mesh *ma = &dd_s->meshes[a], *mb = &dd_s->meshes[b];
    for (int k = 0; k < 6; k++) if (dd_bb[a][k] != dd_bb[b][k]) return 0;
    return n2_mesh_same_content(ma, mb);
}

/* Compact w->neighborhood.scene to unique meshes, fix each region's [mesh0,mesh1) range to
   the compacted positions, free the dropped geometry. Returns meshes removed. */
static int world_dedup(World *w) {
    N2Scene *s = &w->neighborhood.scene;
    int nm = s->count;
    if (nm < 2) return 0;

    float (*bb)[6] = (float (*)[6])malloc((size_t)nm * sizeof *bb);
    int   *ord     = (int *)malloc((size_t)nm * sizeof *ord);
    char  *drop    = (char *)calloc((size_t)nm, 1);
    for (int i = 0; i < nm; i++) { n2_mesh_bbox(&s->meshes[i], bb[i]); ord[i] = i; }

    dd_s = s; dd_bb = (const float (*)[6])bb;
    qsort(ord, (size_t)nm, sizeof *ord, dd_cmp);

    long droptri = 0;
    for (int i = 0; i < nm; ) {
        int j = i + 1; while (j < nm && dd_same(ord[i], ord[j])) j++;
        for (int k = i + 1; k < j; k++) { drop[ord[k]] = 1; droptri += s->meshes[ord[k]].nidx / 3; }
        i = j;
    }

    /* new index = count of survivors before this original slot */
    int *keptbefore = (int *)malloc((size_t)(nm + 1) * sizeof *keptbefore);
    keptbefore[0] = 0;
    for (int i = 0; i < nm; i++) keptbefore[i + 1] = keptbefore[i] + (drop[i] ? 0 : 1);
    for (int r = 0; r < w->neighborhood.nreg; r++) {
        w->neighborhood.rgn[r].mesh0 = keptbefore[w->neighborhood.rgn[r].mesh0];
        w->neighborhood.rgn[r].mesh1 = keptbefore[w->neighborhood.rgn[r].mesh1];
    }

    int wr = 0;
    for (int i = 0; i < nm; i++) {
        if (drop[i]) { free(s->meshes[i].verts); free(s->meshes[i].idx);
                       free(s->meshes[i].vcol); continue; }
        if (wr != i) s->meshes[wr] = s->meshes[i];
        wr++;
    }
    s->count = wr;

    /* region ranges must stay well-formed and cover exactly the survivors */
    assert(wr == keptbefore[nm]);
    for (int r = 0; r < w->neighborhood.nreg; r++)
        assert(w->neighborhood.rgn[r].mesh0 <= w->neighborhood.rgn[r].mesh1 && w->neighborhood.rgn[r].mesh1 <= wr);

    /* z-fight proof: rebuild the key over survivors and confirm none collide.
       Cheap (load-time, one pass) and it is the actual deliverable — if this
       ever fires, duplicates slipped through and --track ALL will z-fight. */
    int residual = 0;
    {
        float (*bb2)[6] = (float (*)[6])malloc((size_t)wr * sizeof *bb2);
        int   *ord2     = (int *)malloc((size_t)wr * sizeof *ord2);
        for (int i = 0; i < wr; i++) { n2_mesh_bbox(&s->meshes[i], bb2[i]); ord2[i] = i; }
        dd_s = s; dd_bb = (const float (*)[6])bb2;
        qsort(ord2, (size_t)wr, sizeof *ord2, dd_cmp);
        for (int i = 1; i < wr; i++) if (dd_same(ord2[i-1], ord2[i])) residual++;
        free(bb2); free(ord2);
    }
    assert(residual == 0);

    free(bb); free(ord); free(drop); free(keptbefore);
    printf("dedup: %d -> %d meshes (dropped %d duplicates, %ld triangles); "
           "residual coplanar duplicates: %d\n",
           nm, wr, nm - wr, droptri, residual);
    return nm - wr;
}

/* Sector/streaming-trigger audit (Phase 20): TRACKS/ holds 8 named city
 * districts (L4RA/B/C/D/F/G/H/R), each a standalone STREAM<name>.BUN
 * geometry bundle (2 MB to 115 MB) plus a small companion <name>.BUN that
 * — per its strings dump (ZCV_ and ZCS_ prefixed names: trains,
 * drawbridges, gates, garbage cans) — holds scripted/animated set-
 * dressing, not spatial data.
 * Grepped every file under TRACKS/, GLOBAL/, FRONTEND/ for trigger,
 * portal, volume, sector, zone, hub, and region-bound keywords: zero
 * hits. There is no portal graph, trigger-volume list, or district
 * connectivity table anywhere in this data — retail's PS2-era district
 * streaming was almost certainly just "load whichever district bundle
 * the camera's fixed district ID names," not a spatial trigger system.
 *
 * CORRECTION (Phase 47, measured). This comment used to claim the
 * bundles' "mesh bounds abut with no overlap/gap", so a district
 * boundary was just where one bundle ends and the next begins. That is
 * FALSE. Measuring solid-geometry bounds per bundle — excluding
 * SKY/GLOW, whose skydome spans the whole map and masks the effect:
 *   L4RA and L4RD have IDENTICAL solid bounds; so do L4RB and L4RG.
 * The bundles are OVERLAPPING SUPERSETS, not adjacent tiles: each is a
 * per-race-route working set that re-ships whatever geometry its route
 * touches. Across all 8, 51.5% of solid meshes (14551 of 28270, 1.25M
 * triangles) are exact duplicates — identical texkey, bbox, and
 * tri/vert count. Adding tri+vert to the identity key moved the total
 * by only 4 groups, so these are true duplicates, not LOD tiers.
 *
 * So "ALL" below concatenating every STREAM*.BUN is itself the cause of
 * that mode's z-fighting: it stacks coplanar copies of one surface
 * (measured dz = 0.000 between same-texkey pairs). The fix is a
 * load-time dedupe on (texkey, bbox, tri, vert) — NOT a depth bias and
 * NOT a vista/ground poly filter, because the conflicting layers are
 * the same asset twice, not low-poly vista clashing with high-poly
 * ground.
 *
 * Phase 48: that dedupe is now world_dedup(), run below right after the
 * region loop and before mbb/grid/batch build, so the ground grid, the
 * physics ground query, and the GPU batches all see one clean surface
 * layer. It keeps the first copy of each (texkey, xyz-bbox, tri, vert)
 * and drops the rest. */
static int world_neighborhood_load_facade(World *w, const char *troot,
                                          const char *trackname,
                                          const WLoadOptions *options) {
    memset(&w->neighborhood, 0, sizeof w->neighborhood);
    const int instance_world = options && options->enabled;
    if (instance_world && !strcmp(trackname, "ALL")) {
        fprintf(stderr, "instance world requires one explicit STREAM bundle; "
                        "--track ALL is not a production composition\n");
        return 0;
    }

    /* Region set. ALL is a diagnostic union, not the retail open world:
       STREAM bundles overlap as route/event supersets and can author
       incompatible layers at the same world coordinates. Supported gameplay
       loads one explicit region. */
    static char regs[WORLD_MAXREG][64]; int nreg = 0;
    if (!strcmp(trackname, "ALL")) {
        int dummy = 0;
        nreg = res_list_tracks(troot, regs, WORLD_MAXREG, "", &dummy);
    }
    if (!nreg) { snprintf(regs[0], sizeof regs[0], "%s", trackname); nreg = 1; }
    w->neighborhood.nreg = nreg;

    char loc4p[1024]; snprintf(loc4p, sizeof loc4p, "%s/LOC4DYNTEX.BIN", troot);
    w->neighborhood.loc4 = n2_read_file(loc4p, &w->neighborhood.loc4len);

    /* per-region: own TPK keys + shared LOC4 keys pick each mesh's diffuse
       slot; single-region mode also pulls a big "master" neighbour's TPK for
       mid-size regions whose buildings reference it. */
    static uint32_t tkeys[16384];
    for (int r = 0; r < nreg; r++) {
        WRegion *g = &w->neighborhood.rgn[r];
        snprintf(g->name, sizeof g->name, "%s", regs[r]);
        char trackp[1024]; snprintf(trackp, sizeof trackp, "%s/%s.BUN", troot, regs[r]);
        g->data = n2_read_file(trackp, &g->len);
        g->mesh0 = g->mesh1 = w->neighborhood.scene.count;
        if (!g->data) { fprintf(stderr, "cannot read %s\n", trackp); continue; }
        {
            const int light_cap = 8192;
            N2LightSrc *lights = (N2LightSrc *)malloc(
                (size_t)light_cap * sizeof *lights);
            if (lights) {
                int nlight = n2_load_light_sources(g->data, g->len,
                                                    lights, light_cap);
                world_append_lights(w, lights, nlight);
                free(lights);
            }
        }
        g->tpk = n2_tpk_open(g->data, g->len);
        int ntk = n2_tpk_keys(g->data, g->tpk, tkeys, 16384);
        if (w->neighborhood.loc4 && ntk < 16384)
            ntk += n2_car_tex_keys(w->neighborhood.loc4, w->neighborhood.loc4len, tkeys + ntk, 16384 - ntk);
        if (nreg == 1 && g->len > 8L*1024*1024 && g->len < 60L*1024*1024) {
            /* Location-4 shared library; use the other big region if we ARE it */
            const char *mn = strcmp(regs[r], "STREAML4RD") ? "STREAML4RD" : "STREAML4RA";
            char mp[1024]; snprintf(mp, sizeof mp, "%s/%s.BUN", troot, mn);
            w->neighborhood.master = res_map_file(mp, &w->neighborhood.masterlen);
            if (w->neighborhood.master) {
                w->neighborhood.master_mapped = 1;
                w->neighborhood.mastertpk = n2_tpk_open(w->neighborhood.master, w->neighborhood.masterlen);
                if (ntk < 16384)
                    ntk += n2_tpk_keys(w->neighborhood.master, w->neighborhood.mastertpk, tkeys + ntk, 16384 - ntk);
            }
        }
        if (!instance_world) {
            n2_vista_out = &w->neighborhood.vista;      /* backdrop impostors go here, not away */
            n2_walk_meshes(g->data, 0, g->len, &w->neighborhood.scene, tkeys, ntk);
            n2_vista_out = NULL;
        }
        g->mesh1 = w->neighborhood.scene.count;
        if (!w->neighborhood.have_grass)
            w->neighborhood.have_grass = n2_load_texture(g->data, g->len, "TRN_GRASSC", &w->neighborhood.grass);
        if (!instance_world) {
            printf("region %-12s: %3ld MB, %5d meshes, %d tex keys\n",
                   regs[r], g->len >> 20, g->mesh1 - g->mesh0, ntk);
            printf("objects: %ld seen = %ld emitted + %ld routed to vista + %ld emitted "
                   "nothing (%ld of them had no 0x134B01/B03 leaf pair)  %s\n",
                   n2_obj_seen, n2_obj_emit, n2_obj_vista, n2_obj_nomesh, n2_obj_nopair,
                   n2_obj_seen == n2_obj_emit + n2_obj_vista + n2_obj_nomesh
                       ? "(closed)" : "(LEAK)");
        }
    }

    if (instance_world) {
        const char *bundle_names[WORLD_MAXREG];
        for (int r = 0; r < nreg; r++) bundle_names[r] = regs[r];
        if (!world_instance_build_for_event(&w->neighborhood.scene, &w->neighborhood.vista, troot,
                                  bundle_names, nreg,
                                  options->focus_x, options->focus_y,
                                  options->view_radius,
                                  w->neighborhood.loc4, w->neighborhood.loc4len, &w->neighborhood.inst_stats, options->scenery_event)) {
            fprintf(stderr, "instance world: no complete home district at "
                            "(%.1f, %.1f)\n",
                    options->focus_x, options->focus_y);
            return 0;
        }
        if (options->scenery_event)
            printf("SCENERY SELECTION mode=%s event=%d hidden-placements=%ld "
                   "[load-time only; shared/unknown groups and direction flags unchanged]\n",
                   options->scenery_event == -1 ? "free" : "event",
                   options->scenery_event, w->neighborhood.inst_stats.scenery_hidden);
        for (int r = 0; r < nreg; r++) {
            w->neighborhood.rgn[r].mesh0 = 0;
            w->neighborhood.rgn[r].mesh1 = !strcmp(w->neighborhood.rgn[r].name, w->neighborhood.inst_stats.bundle)
                            ? w->neighborhood.scene.count : 0;
            printf("region %-12s: %3ld MB, %5d instance meshes\n",
                   w->neighborhood.rgn[r].name, w->neighborhood.rgn[r].len >> 20,
                   w->neighborhood.rgn[r].mesh1 - w->neighborhood.rgn[r].mesh0);
        }
    }

    printf("authored local light sources: %d\n", w->neighborhood.nlights);

    /* strip coplanar duplicates before anything downstream sees the scene.
       This handles the overlapping-superset bundles under --track ALL: the 8
       bundles re-ship the same tiles byte-identically, so the exact key
       (texkey,bbox,tri,vert) fuses them to one clean layer (residual == 0). */
    world_dedup(w);

    /* Terrain/road de-fighting (Phase 73.5). The paved ROAD strips and the
       TERRAIN base sheet are DISTINCT meshes (different textures + bboxes, so
       world_dedup rightly keeps both) that the artists laid coplanar: the road
       sits exactly on the ground plane it covers. At equal Z the depth test
       picks per-pixel between asphalt and terrain — the shimmering "in and out
       of water" stripes. Push terrain down a hair so ROAD always wins where they
       overlap; the ground query reads these same verts and prefers the nearest
       surface, so on open terrain the car still sits on the (5 cm lower) ground
       and on a road it sits on the road — no gameplay change, fight gone.
       ponytail: a flat world-space bias, not glPolygonOffset — the fight is on
       the near ground the camera looks straight at, where 5 cm is many depth
       units; distant coplanar ground fades into fog before it can shimmer. */
    for (int i = 0; i < w->neighborhood.scene.count; i++) {
        N2Mesh *m = &w->neighborhood.scene.meshes[i];
        if (m->cat != N2_TERRAIN) continue;
        for (int v = 0; v < m->nverts; v++) m->verts[v*5+2] -= 0.05f;
    }

    /* per-mesh XY bounds — the draw cull and the ground grid both key off it */
    int nm = w->neighborhood.scene.count;
    w->neighborhood.mbb = (float (*)[4])malloc((size_t)nm * 4 * sizeof(float));
    for (int i = 0; i < nm; i++) {
        N2Mesh *m = &w->neighborhood.scene.meshes[i];
        float x0=1e30f, y0=1e30f, x1=-1e30f, y1=-1e30f;
        for (int v = 0; v < m->nverts; v++) {
            float *p = m->verts + v*5;
            if (p[0]<x0)x0=p[0]; if (p[0]>x1)x1=p[0];
            if (p[1]<y0)y0=p[1]; if (p[1]>y1)y1=p[1];
        }
        w->neighborhood.mbb[i][0]=x0; w->neighborhood.mbb[i][1]=y0; w->neighborhood.mbb[i][2]=x1; w->neighborhood.mbb[i][3]=y1;
    }
    grid_build(w);
    /* Shared gameplay textures (e.g. STARTLINE) live outside TRACKS. Instance
       prototype material matching already inventories this archive during
       its build; retain the bytes here as the final texture-resolution source,
       with existing regional precedence intact. */
    char commonp[1024];
    int plen = snprintf(commonp, sizeof commonp, "%s/../GLOBAL/InGameCommon.bun", troot);
    if (plen >= 0 && (size_t)plen < sizeof commonp) {
        w->neighborhood.common = n2_read_file(commonp, &w->neighborhood.commonlen);
        if (w->neighborhood.common) w->neighborhood.commontpk = n2_tpk_open(w->neighborhood.common, w->neighborhood.commonlen);
    }
    if (options) {
        w->neighborhood.center[0] = options->focus_x;
        w->neighborhood.center[1] = options->focus_y;
        w->neighborhood.radius = options->view_radius;
    }
    return nm;
}

int world_neighborhood_load(WorldNeighborhood *neighborhood,
                            const char *troot, const char *trackname,
                            const WLoadOptions *options) {
    if (!neighborhood || !troot || !trackname) return 0;
    World facade;
    memset(&facade, 0, sizeof facade);
    int nm = world_neighborhood_load_facade(&facade, troot, trackname, options);
    *neighborhood = facade.neighborhood;
    return nm;
}

int world_city_load(WorldCity *city,
                    const WorldNeighborhood *initial_neighborhood,
                    const char *troot, const char *trackname) {
    if (!city || !initial_neighborhood || !troot || !trackname) return 0;
    World facade;
    memset(&facade, 0, sizeof facade);
    facade.neighborhood = *initial_neighborhood;
    world_load_events(&facade, troot);   /* tags nodes loaded immediately after */
    world_load_nav(&facade, troot);
    nav_build_adj(&facade);
    world_set_mode(&facade, MODE_FREEROAM, -1);
    world_build_districts(&facade);
    printf("districts: %d area codes fused onto %d nav nodes\n"
           "  (nearest TRN_ terrain mesh per node; codes are the artists' own)\n",
           facade.city.ndist, facade.city.nnav);
    for (int i = 0; i < facade.city.ndist; i++)
        printf("  %-3s nodes %5d  centroid (%8.1f,%8.1f)  medianZ %7.1f  "
               "X[%7.0f..%7.0f] Y[%7.0f..%7.0f]\n",
               facade.city.dist[i].tok, facade.city.dist[i].n,
               facade.city.dist[i].cx, facade.city.dist[i].cy,
               facade.city.dist[i].medz, facade.city.dist[i].bb[0],
               facade.city.dist[i].bb[1], facade.city.dist[i].bb[2],
               facade.city.dist[i].bb[3]);
    *city = facade.city;
    return city->nnav;
}

int world_load_ex(World *w, const char *troot, const char *trackname,
                  const WLoadOptions *options) {
    if (!w) return 0;
    memset(w, 0, sizeof *w);
    int nm = world_neighborhood_load(&w->neighborhood, troot, trackname, options);
    world_ground_grid_activate(&w->neighborhood.grid);
    world_city_load(&w->city, &w->neighborhood, troot, trackname);
    return nm;
}

int world_load(World *w, const char *troot, const char *trackname) {
    return world_load_ex(w, troot, trackname, NULL);
}

int g_world_texaudit = 0, g_world_texnoise = 0, g_world_texmiss = 0;
/* One source order for ordinary meshes, vistas and authored light textures.
   The shared fallback cannot replace a successful existing resolution. */
static int world_texture_decode(const World *w, const WRegion *g,
                                uint32_t key, N2Tex *tex, int common_only) {
    if (common_only) {
        int ok = n2_tpk_decode(w->neighborhood.common, w->neighborhood.commonlen, w->neighborhood.commontpk, key, tex);
        if (ok && g_world_texaudit)
            printf("TEXSOURCE key %08x  GLOBAL/InGameCommon.bun\n", key);
        return ok;
    }
    int ok = n2_tpk_decode(g->data, g->len, g->tpk, key, tex);
    if (!ok && w->neighborhood.loc4)
        ok = n2_load_car_tex_by_key(w->neighborhood.loc4, w->neighborhood.loc4len, key, tex);
    if (!ok && w->neighborhood.master)
        ok = n2_tpk_decode(w->neighborhood.master, w->neighborhood.masterlen, w->neighborhood.mastertpk, key, tex);
    return ok;
}

int world_bind_textures(World *w, uint32_t *keys, GLuint *texs,
                        unsigned char *modes, int cap) {
    int n = 0;
    /* Finish the original binding order across ALL regions first. Only then
       retry still-unbound requests against common; never borrow from an
       unrelated region or preempt a later region's successful old lookup. */
    for (int pass = 0; pass < (w->neighborhood.common ? 2 : 1); pass++)
    for (int r = 0; r < w->neighborhood.nreg; r++) {
        WRegion *g = &w->neighborhood.rgn[r];
        if (!g->data) continue;
        for (int i = g->mesh0; i < g->mesh1; i++) {
            uint32_t tk = w->neighborhood.scene.meshes[i].texkey; if (!tk) continue;
            int seen = 0; for (int j = 0; j < n; j++) if (keys[j] == tk) seen = 1;
            if (seen || n >= cap) continue;
            N2Tex tt = {0};   /* zero-init: n2_tpk_decode leaves dxt untouched */
            int ok = world_texture_decode(w, g, tk, &tt, pass);
            if (ok && !n2_tex_noise(&tt)) {
                keys[n] = tk; texs[n] = upload_tex(&tt);
                /* M135: order/usage/blend/wz are only decoded by n2_tpk_decode
                   itself; a key that resolved via n2_load_car_tex_by_key (the
                   shared LOC4 car-texture library) keeps them at their zero
                   default, i.e. N2_DRAW_OPAQUE -- the same behaviour every
                   world texture had before this field existed, not a new
                   misclassification. */
                if (modes) modes[n] = (unsigned char)n2_tex_mode(&tt);
                n++;
            }
            else if (g_world_texaudit && (ok || pass || !w->neighborhood.common)) {
                /* M133: separate "the archive has no such record" from "we
                   decoded it and then threw it away" -- they need different
                   fixes and the old counters merged them. */
                printf("TEXFAIL key %08x  %s  mesh %-30s cat %d\n", tk,
                       ok ? "DECODED-BUT-REJECTED-AS-NOISE" :
                       pass ? "not in common TPK" : "not in region/LOC4/master",
                       w->neighborhood.scene.meshes[i].sname, w->neighborhood.scene.meshes[i].cat);
                if (ok) g_world_texnoise++; else g_world_texmiss++;
            }
            if (ok) { free(tt.rgb); free(tt.alpha); free(tt.dxt); }
        }
        /* District light records have no mesh-owned texture slot. Request
           their shipped flare through this same regional/shared resolver
           while the STREAM bytes are still alive. */
        if (w->neighborhood.nlights > 0 && n < cap) {
            uint32_t tk = N2_TEX_SFX_FLARE_GLOWA;
            int seen = 0;
            for (int j = 0; j < n; j++) if (keys[j] == tk) { seen = 1; break; }
            if (!seen) {
                N2Tex tt = {0};
                int ok = world_texture_decode(w, g, tk, &tt, pass);
                if (ok && !n2_tex_noise(&tt)) {
                    keys[n] = tk; texs[n] = upload_tex(&tt);
                    if (modes) modes[n] = (unsigned char)n2_tex_mode(&tt);
                    n++;
                }
                if (ok) { free(tt.rgb); free(tt.alpha); free(tt.dxt); }
            }
        }
        /* M132: vista impostors carry their own authored texture keys and are
           decoded from the same TPK, in the same pass, before the region bytes
           are released. They are not region-tagged, so every region gets a
           chance at every key; a miss is silent and harmless. */
        for (int i = 0; i < w->neighborhood.vista.count; i++) {
            uint32_t tk = w->neighborhood.vista.meshes[i].texkey; if (!tk) continue;
            int seen = 0; for (int j = 0; j < n; j++) if (keys[j] == tk) seen = 1;
            if (seen || n >= cap) continue;
            N2Tex tt = {0};
            int ok = world_texture_decode(w, g, tk, &tt, pass);
            if (ok && !n2_tex_noise(&tt)) {
                keys[n] = tk; texs[n] = upload_tex(&tt);
                /* vista batches don't consult drawmode (the tier has its own
                   uVista alpha-blend path), but fill it anyway so the slot
                   never carries stale/uninitialised data. */
                if (modes) modes[n] = (unsigned char)n2_tex_mode(&tt);
                n++;
            }
            else if (g_world_texaudit && (ok || pass || !w->neighborhood.common)) {
                /* M133: separate "the archive has no such record" from "we
                   decoded it and then threw it away" -- they need different
                   fixes and the old counters merged them. M133-R: `i` here
                   indexes w->neighborhood.vista, not w->neighborhood.scene -- attribute to the vista
                   mesh that actually owns this key, not whatever scene mesh
                   happens to share the same index. */
                printf("TEXFAIL key %08x  %s  mesh %-30s cat %d\n", tk,
                       ok ? "DECODED-BUT-REJECTED-AS-NOISE" :
                       pass ? "not in common TPK" : "not in region/LOC4/master",
                       w->neighborhood.vista.meshes[i].sname, w->neighborhood.vista.meshes[i].cat);
                if (ok) g_world_texnoise++; else g_world_texmiss++;
            }
            if (ok) { free(tt.rgb); free(tt.alpha); free(tt.dxt); }
        }
    }
    /* Keep every region alive until the last shared-fallback decision. */
    for (int r = 0; r < w->neighborhood.nreg; r++) {
        free(w->neighborhood.rgn[r].data); w->neighborhood.rgn[r].data = NULL;
    }
    free(w->neighborhood.commontpk.blk); memset(&w->neighborhood.commontpk, 0, sizeof w->neighborhood.commontpk);
    free(w->neighborhood.common); w->neighborhood.common = NULL; w->neighborhood.commonlen = 0;
    return n;
}

/* ---- ground grid ----
   Uniform XY grid over the road/terrain meshes; each cell lists the meshes
   whose bbox overlaps it (CSR layout). A query tests only that cell's meshes
   with the exact n2_ground_z triangle scan, so overpasses still resolve to
   the highest surface under the point, same as the brute force. */
#define GCELL 64.0f
static WGroundGrid g_empty_grid;
static const WGroundGrid *g_active_grid = &g_empty_grid;
#define g_grid (*g_active_grid)

void world_ground_grid_activate(const WGroundGrid *grid) {
    g_active_grid = grid ? grid : &g_empty_grid;
}

void world_ground_grid_free(WGroundGrid *grid) {
    if (!grid) return;
    if (g_active_grid == grid) g_active_grid = &g_empty_grid;
    free(grid->start);
    free(grid->list);
    memset(grid, 0, sizeof *grid);
}

int world_ground_grid_build(WGroundGrid *grid, const N2Scene *scene,
                            const float (*mbb)[4]) {
    if (!grid || !scene || !mbb || scene->count <= 0 || !scene->meshes) return 0;
    WGroundGrid next = {0};
    float x0=1e30f, y0=1e30f, x1=-1e30f, y1=-1e30f;
    for (int i = 0; i < scene->count; i++) {
        if (scene->meshes[i].cat != N2_ROAD &&
            scene->meshes[i].cat != N2_TERRAIN) continue;
        if (mbb[i][0]<x0)x0=mbb[i][0]; if (mbb[i][1]<y0)y0=mbb[i][1];
        if (mbb[i][2]>x1)x1=mbb[i][2]; if (mbb[i][3]>y1)y1=mbb[i][3];
    }
    if (x0 > x1 || y0 > y1 || !isfinite(x0) || !isfinite(y0) ||
        !isfinite(x1) || !isfinite(y1)) return 0;

    int gw = (int)((x1-x0)/GCELL) + 1;
    int gh = (int)((y1-y0)/GCELL) + 1;
    if (gw <= 0 || gh <= 0 || (size_t)gw > (size_t)-1 / (size_t)gh) return 0;
    size_t cells = (size_t)gw * (size_t)gh;
    if (cells > ((size_t)-1 / sizeof(int)) - 1) return 0;

    int *start = (int *)calloc(cells + 1, sizeof *start);
    if (!start) return 0;
    for (int i = 0; i < scene->count; i++) {
        if (scene->meshes[i].cat != N2_ROAD &&
            scene->meshes[i].cat != N2_TERRAIN) continue;
        int cx0=(int)((mbb[i][0]-x0)/GCELL), cy0=(int)((mbb[i][1]-y0)/GCELL);
        int cx1=(int)((mbb[i][2]-x0)/GCELL), cy1=(int)((mbb[i][3]-y0)/GCELL);
        if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0;
        if (cx1 >= gw) cx1 = gw-1; if (cy1 >= gh) cy1 = gh-1;
        for (int cy = cy0; cy <= cy1; cy++)
            for (int cx = cx0; cx <= cx1; cx++) start[(size_t)cy*gw+cx+1]++;
    }
    for (size_t c = 0; c < cells; c++) start[c+1] += start[c];
    int total = start[cells];
    int *list = total ? (int *)malloc((size_t)total * sizeof *list) : NULL;
    int *cursor = (int *)malloc(cells * sizeof *cursor);
    if ((total && !list) || !cursor) {
        free(start); free(list); free(cursor);
        return 0;
    }
    memcpy(cursor, start, cells * sizeof *cursor);
    for (int i = 0; i < scene->count; i++) {
        if (scene->meshes[i].cat != N2_ROAD &&
            scene->meshes[i].cat != N2_TERRAIN) continue;
        int cx0=(int)((mbb[i][0]-x0)/GCELL), cy0=(int)((mbb[i][1]-y0)/GCELL);
        int cx1=(int)((mbb[i][2]-x0)/GCELL), cy1=(int)((mbb[i][3]-y0)/GCELL);
        if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0;
        if (cx1 >= gw) cx1 = gw-1; if (cy1 >= gh) cy1 = gh-1;
        for (int cy = cy0; cy <= cy1; cy++)
            for (int cx = cx0; cx <= cx1; cx++) {
                size_t c = (size_t)cy*gw+cx;
                list[cursor[c]++] = i;
            }
    }
    free(cursor);

    next.meshes = scene->meshes;
    next.x0 = x0; next.y0 = y0;
    next.gw = gw; next.gh = gh;
    next.start = start; next.list = list;
    world_ground_grid_free(grid);
    *grid = next;
    return 1;
}

static void grid_build(World *w) {
    if (!world_ground_grid_build(&w->neighborhood.grid, &w->neighborhood.scene,
                                 (const float (*)[4])w->neighborhood.mbb)) return;
    printf("ground grid: %dx%d cells, %d mesh refs\n", w->neighborhood.grid.gw, w->neighborhood.grid.gh,
           w->neighborhood.grid.start[w->neighborhood.grid.gw*w->neighborhood.grid.gh]);
    if (w->neighborhood.vista.count) {
        float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}; long tri=0;
        for (int i = 0; i < w->neighborhood.vista.count; i++) {
            const N2Mesh *m = &w->neighborhood.vista.meshes[i]; tri += m->nidx/3;
            for (int v = 0; v < m->nverts; v++)
                for (int c = 0; c < 3; c++) {
                    float q = m->verts[v*5+c];
                    if (q < mn[c]) mn[c] = q; if (q > mx[c]) mx[c] = q;
                }
        }
        if (getenv("OPENUG2_VISTA_ALPHA")) {
            /* M132-R2 evidence, straight from the raw records: for every
               texture a vista mesh binds, what alpha does the source actually
               carry? Nothing here is inferred from pixels being black. */
            printf("VALPHA name                         key        fmt   w x h    "
                   "amin amax   a=0%%   0<a<255%%   a=255%%   vcol amin amax\n");
            uint32_t seen[512]; int nseen = 0;
            for (int i = 0; i < w->neighborhood.vista.count; i++) {
                const N2Mesh *m = &w->neighborhood.vista.meshes[i];
                uint32_t tk = m->texkey;
                int dup = 0; for (int j = 0; j < nseen; j++) if (seen[j] == tk) dup = 1;
                if (dup || nseen >= 512) continue;
                seen[nseen++] = tk;
                int vamin = 255, vamax = 0;
                for (int q = 0; q < w->neighborhood.vista.count; q++)
                    if (w->neighborhood.vista.meshes[q].texkey == tk && w->neighborhood.vista.meshes[q].vcol)
                        for (int v = 0; v < w->neighborhood.vista.meshes[q].nverts; v++) {
                            int a = w->neighborhood.vista.meshes[q].vcol[v*4+3];
                            if (a < vamin) vamin = a; if (a > vamax) vamax = a;
                        }
                N2Tex tt = {0};
                int ok = 0;
                for (int r2 = 0; r2 < w->neighborhood.nreg && !ok; r2++)
                    if (w->neighborhood.rgn[r2].data)
                        ok = n2_tpk_decode(w->neighborhood.rgn[r2].data, w->neighborhood.rgn[r2].len,
                                           w->neighborhood.rgn[r2].tpk, tk, &tt);
                if (!ok && w->neighborhood.master)
                    ok = n2_tpk_decode(w->neighborhood.master, w->neighborhood.masterlen, w->neighborhood.mastertpk, tk, &tt);
                if (!ok) { printf("VALPHA %-28s %08x   UNRESOLVED\n", m->sname, tk); continue; }
                long n = (long)tt.w*tt.h, z = 0, part = 0, full = 0;
                int amin = 255, amax = 0;
                if (tt.alpha) {
                    for (long q = 0; q < n; q++) {
                        int a = tt.alpha[q];
                        if (a < amin) amin = a; if (a > amax) amax = a;
                        if (a == 0) z++; else if (a == 255) full++; else part++;
                    }
                } else { amin = amax = 255; full = n; }
                static const char *FN[9] = {"none","DXT1","?","DXT3","?","?","?","?","P8"};
                printf("VALPHA %-28s %08x  %-5s %4dx%-4d %4d %4d  %6.2f  %8.2f  %7.2f   %4d %4d\n",
                       m->sname, tk, FN[tt.afmt&8?8:tt.afmt], tt.w, tt.h, amin, amax,
                       100.0*z/n, 100.0*part/n, 100.0*full/n,
                       vamin > vamax ? -1 : vamin, vamin > vamax ? -1 : vamax);
                free(tt.rgb); free(tt.alpha); free(tt.dxt);
            }
        }
        if (getenv("OPENUG2_VISTA_CENSUS")) {
            printf("VISTA per-mesh census (name, tris, world AABB, planarity proxy):\n");
            for (int i = 0; i < w->neighborhood.vista.count; i++) {
                const N2Mesh *m = &w->neighborhood.vista.meshes[i];
                float a[3]={1e30f,1e30f,1e30f}, b[3]={-1e30f,-1e30f,-1e30f};
                for (int v = 0; v < m->nverts; v++)
                    for (int c = 0; c < 3; c++) {
                        float q=m->verts[v*5+c];
                        if (q<a[c]) a[c]=q; if (q>b[c]) b[c]=q;
                    }
                printf("VISTA %-28s tris %5d  x[%8.0f %8.0f] y[%8.0f %8.0f] z[%8.0f %8.0f]\n",
                       m->sname[0]?m->sname:"?", m->nidx/3,
                       a[0],b[0],a[1],b[1],a[2],b[2]);
            }
        }
        printf("vista: %d meshes (%ld tris) from %ld objects (%ld PAN_, %ld family) "
               "bounds x[%.0f %.0f] y[%.0f %.0f] z[%.0f %.0f]\n",
               w->neighborhood.vista.count, tri, n2_vista_objs, n2_vista_pan, n2_vista_fam,
               mn[0],mx[0],mn[1],mx[1],mn[2],mx[2]);
    } else printf("vista: none in this region\n");
}

/* ---- scripted-object entity definitions (read-only decode) ----
   Each entity's 0x39200 record ends with `... 1, 8, FNV-32 hash, char[32]
   name`, and the name is a ZCV_/ZCS_ token. Anchoring on that token (rather
   than the 0x39200 chunk, whose preamble length varies) is the robust parse:
   validate the constant `1, 8` fields at name-12/name-8, take the hash at
   name-4, then read the paired 0x39201 hull (8-corner local OBB, 48B/corner)
   that follows within a short window. Definitions only; the data has no world
   placement or mesh (verified Phase 50, see docs/FORMATS.md). */
static void sd_scan(const unsigned char *d, long len,
                    ScriptedDef *out, int cap, int *n) {
    for (long i = 40; i + 32 < len && *n < cap; i++) {
        if (!(d[i]=='Z' && d[i+1]=='C' && (d[i+2]=='V' || d[i+2]=='S') && d[i+3]=='_'))
            continue;
        if (n2_u32(d + i - 12) != 1 || n2_u32(d + i - 8) != 8) continue;  /* record marker */
        uint32_t hash = n2_u32(d + i - 4);
        int seen = 0;
        for (int k = 0; k < *n; k++) if (out[k].hash == hash) { seen = 1; break; }
        if (seen) continue;
        ScriptedDef *e = &out[(*n)++];
        e->hash = hash;
        int j = 0; while (j < 31 && d[i+j]) { e->name[j] = (char)d[i+j]; j++; }
        e->name[j] = 0;
        e->w = e->l = e->h = 0.0f;
        for (long p = i + 32; p < i + 160 && p + 8 <= len; p += 4) {  /* paired 0x39201 */
            if (n2_u32(d + p) != 0x00039201u) continue;
            long q = p + 8 + 16;                                       /* first OBB corner */
            if (q + 8*48 <= len) {
                float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
                for (int c = 0; c < 8; c++)
                    for (int a = 0; a < 3; a++) {
                        float v; memcpy(&v, d + q + c*48 + a*4, 4);
                        if (v < mn[a]) mn[a] = v;
                        if (v > mx[a]) mx[a] = v;
                    }
                e->w = mx[0]-mn[0]; e->l = mx[1]-mn[1]; e->h = mx[2]-mn[2];
            }
            break;
        }
    }
}

int world_scripted_defs(const World *w, const char *troot,
                        ScriptedDef *out, int cap) {
    int n = 0;
    for (int r = 0; r < w->neighborhood.nreg && n < cap; r++) {
        const char *rn = w->neighborhood.rgn[r].name;
        /* companion file drops the STREAM prefix: STREAML4RA -> L4RA.BUN */
        const char *stem = strncmp(rn, "STREAM", 6) ? rn : rn + 6;
        char p[1024]; snprintf(p, sizeof p, "%s/%s.BUN", troot, stem);
        long len = 0; unsigned char *d = n2_read_file(p, &len);
        if (!d) continue;
        sd_scan(d, len, out, cap, &n);
        free(d);
    }
    return n;
}

/* ---- AI / GPS navigation graph (Phase 67) --------------------------------
   The real drivable road network, as opposed to rendering bounds. Source is
   TRACKS/ROUTES<REGION>/Paths*.bin (355 files across the 8 regions); each holds
   a 0x80034147 container whose 0x34148 leaf is an array of 24-byte nodes.
   Measured record layout:
     +0  float X          +12 u16 link  +16 u16 link
     +4  float Y          +14 u16 link  (0xffff = no link)
     +8  u32 flags        +20 float cumulative distance along the segment
   A file concatenates SEVERAL segments (the distance field restarts and the
   XY jumps), so consecutive records are only joined into an edge when they sit
   within NAV_LINK_MAX. Measured spacing over 5043 sampled nodes: 20-60 m 2945,
   5-20 m 1024, 60-200 m 462, >200 m 581 -- so real road steps are well under
   120 m and the big jumps are segment breaks. */
#define NAV_LINK_MAX 120.0f

/* ---- race event catalog (Phase 71) ---------------------------------------
   See world.h for the record layout and for why this, and not a barrier prop
   list, is the authentic Freeroam/Race split.

   AUDIT TRAIL (what was looked at before settling on this): a full recursive
   chunk census of TRACKS/L4R*.BUN, GLOBAL/InGame{FreeRoam,Race,Drift,Drag}.bun,
   and every TRACKS/ROUTES<REG> Paths/Routes/TrackPosMarkers file turns up NO barrier /
   blockade instance chunk anywhere -- in particular 0x0003410B does not exist in
   any shipped file. What the data DOES ship per event is a route network
   restricted to the roads that event uses: Routes4001F.bin references only the
   6 route sectors TrackRoutesA21/A30..A34, where RoutesFreeRoam.bin references
   all 20 (A10..A44). The closure is expressed as omission, so the barrier
   positions are exactly the links where the freeroam graph leaves the event
   corridor -- computed below, never typed in. */
int world_load_events(World *w, const char *troot) {
    w->city.nev = 0; w->city.mode = MODE_FREEROAM; w->city.active_ev = -1;
    for (int r = 0; r < w->neighborhood.nreg; r++) {
        const char *rn = w->neighborhood.rgn[r].name;
        const char *stem = strncmp(rn, "STREAM", 6) ? rn : rn + 6;
        unsigned char *d = NULL; long len = 0;
        for (int fi = 0; fi < 4000 && !d; fi++) {   /* first Paths file that exists */
            char p[1024];
            snprintf(p, sizeof p, "%s/ROUTES%s/Paths%04d.bin", troot, stem, 4000 + fi);
            d = n2_read_file(p, &len);
        }
        if (!d) continue;
        N2Leaf leaf[4]; int nl = 0;
        n2_find_leaves(d, 0, len, 0x0003414cu, leaf, &nl, 4);
        for (int L = 0; L < nl; L++) {
            int n = (int)leaf[L].size / 272;
            for (int i = 0; i < n && w->city.nev < WORLD_MAXEVENT; i++) {
                const unsigned char *b = d + leaf[L].off + i * 272;
                WEvent *e = &w->city.ev[w->city.nev];
                e->id      = b[0] | (b[1] << 8);
                e->npoly   = b[2];
                e->circuit = b[3];
                e->len100m = b[5];
                if (e->id < 4000 || e->npoly < 3 || e->npoly > WORLD_EVPOLY) continue;
                snprintf(e->reg, sizeof e->reg, "%s", stem);
                e->bb[0] = e->bb[2] = 1e30f; e->bb[1] = e->bb[3] = -1e30f;
                for (int k = 0; k < e->npoly; k++) {
                    memcpy(&e->poly[k][0], b + 8 + k*8,     4);
                    memcpy(&e->poly[k][1], b + 8 + k*8 + 4, 4);
                    if (e->poly[k][0] < e->bb[0]) e->bb[0] = e->poly[k][0];
                    if (e->poly[k][0] > e->bb[1]) e->bb[1] = e->poly[k][0];
                    if (e->poly[k][1] < e->bb[2]) e->bb[2] = e->poly[k][1];
                    if (e->poly[k][1] > e->bb[3]) e->bb[3] = e->poly[k][1];
                }
                e->node0 = e->node1 = 0;
                w->city.nev++;
            }
        }
        free(d);
    }
    {   int nc = 0; for (int i = 0; i < w->city.nev; i++) nc += w->city.ev[i].circuit;
        printf("race events: %d parsed from Paths*.bin chunk 0x3414c "
               "(%d circuits, %d sprints)\n", w->city.nev, nc, w->city.nev - nc); }
    return w->city.nev;
}

/* Index of the event with this id, or -1. */
static int ev_by_id(const World *w, int id) {
    for (int i = 0; i < w->city.nev; i++) if (w->city.ev[i].id == id) return i;
    return -1;
}

int world_load_nav(World *w, const char *troot) {
    int cap = 4096, ecap = 4096;
    w->city.nav = (float *)malloc((size_t)cap * 2 * sizeof(float));
    w->city.navedge = (int *)malloc((size_t)ecap * 2 * sizeof(int));
    w->city.nnav = w->city.nnavedge = 0;
    w->city.navbb[0] = w->city.navbb[2] = 1e30f; w->city.navbb[1] = w->city.navbb[3] = -1e30f;

    for (int r = 0; r < w->neighborhood.nreg; r++) {
        const char *rn = w->neighborhood.rgn[r].name;
        const char *stem = strncmp(rn, "STREAM", 6) ? rn : rn + 6;
        for (int fi = 0; fi < 4000; fi++) {          /* Paths<id>.bin ids are sparse */
            char p[1024];
            snprintf(p, sizeof p, "%s/ROUTES%s/Paths%04d.bin", troot, stem, 4000 + fi);
            long len = 0; unsigned char *d = n2_read_file(p, &len);
            if (!d) continue;
            /* remember which race event contributed which nodes: 0x34148 is the
               event's OWN racing line, so the range below IS its corridor seed */
            int evi = ev_by_id(w, 4000 + fi);
            if (evi >= 0) w->city.ev[evi].node0 = w->city.nnav;
            N2Leaf leaf[8]; int nl = 0;
            n2_find_leaves(d, 0, len, 0x00034148u, leaf, &nl, 8);
            for (int L = 0; L < nl; L++) {
                int n = (int)leaf[L].size / 24, prev = -1;
                for (int i = 0; i < n; i++) {
                    float x, y;
                    memcpy(&x, d + leaf[L].off + i*24,     4);
                    memcpy(&y, d + leaf[L].off + i*24 + 4, 4);
                    if (!(x == x && y == y) || x < -1e5f || x > 1e5f ||
                        y < -1e5f || y > 1e5f) { prev = -1; continue; }
                    if (w->city.nnav == cap) { cap *= 2;
                        w->city.nav = (float *)realloc(w->city.nav, (size_t)cap*2*sizeof(float)); }
                    int id = w->city.nnav++;
                    w->city.nav[id*2] = x; w->city.nav[id*2+1] = y;
                    if (x < w->city.navbb[0]) w->city.navbb[0] = x;
                    if (x > w->city.navbb[1]) w->city.navbb[1] = x;
                    if (y < w->city.navbb[2]) w->city.navbb[2] = y;
                    if (y > w->city.navbb[3]) w->city.navbb[3] = y;
                    if (prev >= 0) {
                        float dx = x - w->city.nav[prev*2], dy = y - w->city.nav[prev*2+1];
                        if (dx*dx + dy*dy <= NAV_LINK_MAX*NAV_LINK_MAX) {
                            if (w->city.nnavedge == ecap) { ecap *= 2;
                                w->city.navedge = (int *)realloc(w->city.navedge, (size_t)ecap*2*sizeof(int)); }
                            w->city.navedge[w->city.nnavedge*2]   = prev;
                            w->city.navedge[w->city.nnavedge*2+1] = id;
                            w->city.nnavedge++;
                        }
                    }
                    prev = id;
                }
            }
            if (evi >= 0) w->city.ev[evi].node1 = w->city.nnav;
            free(d);
        }
    }
    w->city.navev   = (int  *)malloc((size_t)(w->city.nnav ? w->city.nnav : 1) * sizeof(int));
    w->city.navopen = (char *)malloc((size_t)(w->city.nnav ? w->city.nnav : 1));
    for (int i = 0; i < w->city.nnav; i++) { w->city.navev[i] = -1; w->city.navopen[i] = 1; }
    for (int e = 0; e < w->city.nev; e++)
        for (int i = w->city.ev[e].node0; i < w->city.ev[e].node1; i++) w->city.navev[i] = e;
    printf("nav graph: %d nodes, %d edges from TRACKS/ROUTES*/Paths*.bin (chunk 0x34148)\n",
           w->city.nnav, w->city.nnavedge);
    if (w->city.nnav) printf("  extent X[%.0f..%.0f] Y[%.0f..%.0f]\n",
                        w->city.navbb[0], w->city.navbb[1], w->city.navbb[2], w->city.navbb[3]);
    return w->city.nnav;
}

/* ---- topological districts (Phase 68) -------------------------------------
   The old bbox zones are GONE: 3D mesh bounds overlap wildly, so a building
   spanning two areas made world_zone_at() chaotic. Districts now come from the
   nav graph's own connectivity.

   FLAGS AUDIT (why this is topological and not read from the data): the 24-byte
   node's +8 u32 is NOT a district id. It is two u16s -- bits 7..15 are never
   set, and the high half is frequently 0xffff, the same "no link" sentinel the
   other slots use. Censused over all 18064 nodes: the low u16 takes 113 values
   (0..112), and EVERY one of them spans essentially the whole city (e.g. value
   0: X[-2986..2520] Y[-2274..3116], a 5507 x 5390 span vs the map's own
   ~5000 x 5300). A geographic id would be compact, so it is not one. It is
   constant in runs and changes at segment boundaries (16,16,16,...,4,4,4), i.e.
   a per-segment road id/class. So there is no baked district code to read, and
   grouping by drivable connectivity is the correct fallback. */
/* ---- routing graph + A* (Phase 70) ---------------------------------------
   The 355 route files are independent polylines, so the within-route edges
   ALONE do not form a city: measured, a BFS from node 0 reaches 9 of 15257
   nodes (0.1%). Routing needs the coincidence welds promoted to real edges --
   two races down the same street produce two node runs at the same coordinates,
   and welding them is what turns the pile of routes into a drivable network.
   Weld radius is the 5 m knee measured in Phase 68. */
#define NAV_WELD 5.0f

const char *world_district_name(const char *tok) {
    /* EXTERNAL ground truth, supplied by the project owner from the in-game
       world map. The codes come from the files; these names do not -- the
       shipped data binds them to no coordinates (Phase 66 audit). */
    if (!strcmp(tok, "SH")) return "Jackson Heights";
    if (!strcmp(tok, "UC")) return "Beacon Hill";
    if (!strcmp(tok, "CN")) return "City Core (North)";
    if (!strcmp(tok, "CS")) return "City Core (South)";
    if (!strcmp(tok, "IP")) return "Coal Harbor";
    return tok;
}

/* Promote welds to edges, then build a CSR adjacency over the whole graph. */
static void nav_build_adj(World *w) {
    int cap = w->city.nnavedge * 2 + 64, ne = w->city.nnavedge;
    int *ea = (int *)malloc((size_t)cap * 2 * sizeof(int));
    for (int e = 0; e < ne; e++) { ea[e*2] = w->city.navedge[e*2]; ea[e*2+1] = w->city.navedge[e*2+1]; }
    {   int nb = w->city.nnav * 2 + 7;
        int *head = (int *)malloc((size_t)nb * sizeof(int));
        int *next = (int *)malloc((size_t)w->city.nnav * sizeof(int));
        for (int i = 0; i < nb; i++) head[i] = -1;
        for (int i = 0; i < w->city.nnav; i++) {
            int cx = (int)floorf(w->city.nav[i*2]/NAV_WELD), cy = (int)floorf(w->city.nav[i*2+1]/NAV_WELD);
            unsigned h = ((unsigned)cx*73856093u) ^ ((unsigned)cy*19349663u);
            int b = (int)(h % (unsigned)nb); next[i] = head[b]; head[b] = i;
        }
        for (int i = 0; i < w->city.nnav; i++) {
            int cx = (int)floorf(w->city.nav[i*2]/NAV_WELD), cy = (int)floorf(w->city.nav[i*2+1]/NAV_WELD);
            for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) {
                unsigned h = ((unsigned)(cx+dx)*73856093u) ^ ((unsigned)(cy+dy)*19349663u);
                for (int j = head[(int)(h % (unsigned)nb)]; j >= 0; j = next[j]) {
                    if (j <= i) continue;
                    float ddx = w->city.nav[i*2]-w->city.nav[j*2], ddy = w->city.nav[i*2+1]-w->city.nav[j*2+1];
                    if (ddx*ddx + ddy*ddy > NAV_WELD*NAV_WELD) continue;
                    if (ne == cap) { cap *= 2; ea = (int *)realloc(ea, (size_t)cap*2*sizeof(int)); }
                    ea[ne*2] = i; ea[ne*2+1] = j; ne++;
                }
            }
        }
        free(head); free(next);
    }
    w->city.adjstart = (int *)calloc((size_t)w->city.nnav + 1, sizeof(int));
    for (int e = 0; e < ne; e++) { w->city.adjstart[ea[e*2]+1]++; w->city.adjstart[ea[e*2+1]+1]++; }
    for (int i = 0; i < w->city.nnav; i++) w->city.adjstart[i+1] += w->city.adjstart[i];
    w->city.nadj = w->city.adjstart[w->city.nnav];
    w->city.adjlist = (int *)malloc((size_t)w->city.nadj * sizeof(int));
    int *fill = (int *)malloc((size_t)w->city.nnav * sizeof(int));
    for (int i = 0; i < w->city.nnav; i++) fill[i] = w->city.adjstart[i];
    for (int e = 0; e < ne; e++) {
        int a = ea[e*2], b = ea[e*2+1];
        w->city.adjlist[fill[a]++] = b; w->city.adjlist[fill[b]++] = a;
    }
    free(fill); free(ea);
    printf("routing graph: %d nodes, %d directed links (%d edges after welding)\n",
           w->city.nnav, w->city.nadj, ne);
}

/* ---- TrackManager: freeroam vs race event (Phase 71) ----------------------
   A race is the event's own route network laid over the shared city graph. The
   corridor is the node run that Paths<id>.bin contributed, grown across the
   coincidence welds (the same street driven by two events produces two node
   runs at identical coordinates, and both are the same road). Everything the
   corridor touches but does not contain is a closed turning: one barrier each. */
#define BAR_HALF   9.0f    /* barrier wall half-width, metres */
#define BAR_REACH 40.0f    /* only test barriers this close to the car */

int world_set_mode(World *w, int mode, int evidx) {
    w->city.nbar = 0; w->city.nmasked = 0;
    if (!w->city.navopen || !w->city.adjstart) return 0;
    if (mode != MODE_RACE_EVENT || evidx < 0 || evidx >= w->city.nev) {
        w->city.mode = MODE_FREEROAM; w->city.active_ev = -1;
        for (int i = 0; i < w->city.nnav; i++) w->city.navopen[i] = 1;
        return 0;
    }
    w->city.mode = MODE_RACE_EVENT; w->city.active_ev = evidx;
    const WEvent *e = &w->city.ev[evidx];
    for (int i = 0; i < w->city.nnav; i++) w->city.navopen[i] = 0;
    for (int i = e->node0; i < e->node1; i++) w->city.navopen[i] = 1;
    /* promote welded twins; chains of coincident nodes are short, so a handful
       of sweeps reaches a fixed point (the loop exits as soon as one is idle). */
    for (int pass = 0; pass < 8; pass++) {
        int changed = 0;
        for (int i = 0; i < w->city.nnav; i++) {
            if (!w->city.navopen[i]) continue;
            for (int k = w->city.adjstart[i]; k < w->city.adjstart[i+1]; k++) {
                int nb = w->city.adjlist[k];
                if (w->city.navopen[nb]) continue;
                float dx = w->city.nav[nb*2]-w->city.nav[i*2], dy = w->city.nav[nb*2+1]-w->city.nav[i*2+1];
                if (dx*dx + dy*dy <= NAV_WELD*NAV_WELD) { w->city.navopen[nb] = 1; changed = 1; }
            }
        }
        if (!changed) break;
    }
    /* every remaining link out of the corridor is a road the race closes */
    for (int i = 0; i < w->city.nnav; i++) {
        if (!w->city.navopen[i]) continue;
        for (int k = w->city.adjstart[i]; k < w->city.adjstart[i+1]; k++) {
            int nb = w->city.adjlist[k];
            if (w->city.navopen[nb]) continue;
            w->city.nmasked++;
            float dx = w->city.nav[nb*2]-w->city.nav[i*2], dy = w->city.nav[nb*2+1]-w->city.nav[i*2+1];
            float d = sqrtf(dx*dx + dy*dy);
            if (d < 1e-3f) continue;
            float mx = w->city.nav[i*2] + dx*0.5f, my = w->city.nav[i*2+1] + dy*0.5f;
            int dup = 0;                       /* one blockade per closed mouth */
            for (int b = 0; b < w->city.nbar && !dup; b++) {
                float ex = w->city.bar[b].x-mx, ey = w->city.bar[b].y-my;
                if (ex*ex + ey*ey < BAR_HALF*BAR_HALF) dup = 1;
            }
            if (dup || w->city.nbar >= WORLD_MAXBARRIER) continue;
            WBarrier *bar = &w->city.bar[w->city.nbar++];
            bar->x = mx; bar->y = my; bar->dx = dx/d; bar->dy = dy/d;
            bar->a = i;  bar->b = nb;
        }
    }
    printf("race event %d (%s, %s, ~%d00 m): corridor nodes %d, "
           "barriers %d, directed links masked %d\n",
           e->id, e->reg, e->circuit ? "circuit" : "sprint", e->len100m,
           e->node1 - e->node0, w->city.nbar, w->city.nmasked);
    return w->city.nbar;
}

int world_barrier_push(const World *w, float *pos, float r) {
    if (w->city.mode != MODE_RACE_EVENT) return 0;
    int hit = 0;
    for (int i = 0; i < w->city.nbar; i++) {
        const WBarrier *b = &w->city.bar[i];
        float rx = pos[0]-b->x, ry = pos[1]-b->y;
        if (rx*rx + ry*ry > BAR_REACH*BAR_REACH) continue;
        float s = rx*b->dx + ry*b->dy;              /* along the closed road */
        float t = -rx*b->dy + ry*b->dx;             /* across it */
        if (s <= -r || s >= r + BAR_HALF) continue; /* behind, or already past */
        if (t < -(BAR_HALF + r) || t > BAR_HALF + r) continue;
        pos[0] -= b->dx * (s + r);                  /* back to the corridor side */
        pos[1] -= b->dy * (s + r);
        hit = 1;
    }
    return hit;
}

/* ---- race state: checkpoint gates and laps (Phase 72) ---------------------
   See world.h for where the course order comes from and for the retraction of
   the Phase 71 "0x34146 = checkpoints" note. */
#define GATE_HALF 22.0f   /* ponytail: fixed gate width. The route data carries no
                             road width; 22 m clears the widest measured snap error
                             (23 m) plus a lane either side, and only ONE gate is
                             ever armed so a wide gate cannot mis-fire on a parallel
                             street. Derive per-gate widths from the road mesh if a
                             track ever needs a tighter box. */

/* Start-grid slots for one event, from TrackPosMarkers (chunk 0x34146):
   8-byte 0x11 filler, then 48-byte records — +16 f32 X, +20 f32 Y, +24 f32 Z,
   +36 u32 track id. Returns the number written. */
static int race_load_grid(const World *w, const char *troot, int evid,
                          float (*out)[3], int cap) {
    int n = 0;
    for (int r = 0; r < w->neighborhood.nreg && n < cap; r++) {
        const char *rn = w->neighborhood.rgn[r].name;
        const char *stem = strncmp(rn, "STREAM", 6) ? rn : rn + 6;
        char p[1024];
        snprintf(p, sizeof p, "%s/ROUTES%s/TrackPosMarkersAll.bin", troot, stem);
        long len = 0; unsigned char *d = n2_read_file(p, &len);
        if (!d) continue;
        N2Leaf leaf[4]; int nl = 0;
        n2_find_leaves(d, 0, len, 0x00034146u, leaf, &nl, 4);
        for (int L = 0; L < nl; L++) {
            /* the leaf opens with an 8-byte 0x11 filler run before the records */
            long off = leaf[L].off + 8, end = leaf[L].off + leaf[L].size;
            for (; off + 48 <= end && n < cap; off += 48) {
                unsigned tid;
                memcpy(&tid, d + off + 36, 4);
                if ((int)tid != evid) continue;
                memcpy(&out[n][0], d + off + 16, 4);
                memcpy(&out[n][1], d + off + 20, 4);
                memcpy(&out[n][2], d + off + 24, 4);   /* the slot's own Z */
                n++;
            }
        }
        free(d);
    }
    return n;
}

int world_race_start(World *w, const char *troot, int evidx, int maxlaps) {
    WRace *R = &w->city.race;
    memset(R, 0, sizeof *R);
    if (evidx < 0 || evidx >= w->city.nev) return 0;
    world_set_mode(w, MODE_RACE_EVENT, evidx);
    const WEvent *e = &w->city.ev[evidx];
    R->ev = evidx; R->maxlaps = maxlaps > 0 ? maxlaps : 2;

    /* the outline closes (pts[n-1] == pts[0]), so the last vertex is a repeat */
    int np = e->npoly > 1 && e->circuit ? e->npoly - 1 : e->npoly;
    if (np > WORLD_MAXGATE) np = WORLD_MAXGATE;
    for (int k = 0; k < np; k++) {
        /* snap to the nearest node of THIS event's racing line so the gate sits
           on the road rather than on the decimated outline corner */
        int best = -1; float bd = 1e30f;
        for (int i = e->node0; i < e->node1; i++) {
            float dx = w->city.nav[i*2] - e->poly[k][0], dy = w->city.nav[i*2+1] - e->poly[k][1];
            float d2 = dx*dx + dy*dy;
            if (d2 < bd) { bd = d2; best = i; }
        }
        WGate *g = &R->gate[R->ngate];
        g->node = best;
        g->x = best >= 0 ? w->city.nav[best*2]   : e->poly[k][0];
        g->y = best >= 0 ? w->city.nav[best*2+1] : e->poly[k][1];
        /* direction of travel: central difference along the ordered outline */
        int kp = (k - 1 + np) % np, kn = (k + 1) % np;
        if (!e->circuit) { if (k == 0) kp = 0; if (k == np-1) kn = np-1; }
        float dx = e->poly[kn][0] - e->poly[kp][0];
        float dy = e->poly[kn][1] - e->poly[kp][1];
        float d = sqrtf(dx*dx + dy*dy);
        if (d < 1e-3f) { dx = 1; dy = 0; d = 1; }
        g->dx = dx/d; g->dy = dy/d; g->half = GATE_HALF;
        R->ngate++;
    }
    R->ngrid = race_load_grid(w, troot, e->id, R->grid, WORLD_MAXGRID);
    R->active = R->ngate > 0;
    R->next = 0; R->lap = 0; R->cleared = 0; R->havep = 0;
    printf("race armed: event %d (%s), %d gates from the 0x3414c outline, "
           "%d start-grid slots (0x34146), %d lap(s)\n",
           e->id, e->circuit ? "circuit" : "sprint", R->ngate, R->ngrid, R->maxlaps);
    return R->ngate;
}

void world_race_stop(World *w) { w->city.race.active = 0; }

/* Do segments AB and CD properly straddle each other? */
static int seg_cross(float ax, float ay, float bx, float by,
                     float cx, float cy, float dx, float dy) {
    float d1 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
    float d2 = (bx-ax)*(dy-ay) - (by-ay)*(dx-ax);
    float d3 = (dx-cx)*(ay-cy) - (dy-cy)*(ax-cx);
    float d4 = (dx-cx)*(by-cy) - (dy-cy)*(bx-cx);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

int world_race_update(World *w, float x, float y) {
    WRace *R = &w->city.race;
    if (!R->active || R->finished) return 0;
    if (!R->havep) { R->px = x; R->py = y; R->havep = 1; return 0; }
    float px = R->px, py = R->py;
    R->px = x; R->py = y;
    if (px == x && py == y) return 0;

    const WGate *g = &R->gate[R->next];
    /* the gate line: centre +- half across the direction of travel */
    float ax = g->x - (-g->dy)*g->half, ay = g->y - (g->dx)*g->half;
    float bx = g->x + (-g->dy)*g->half, by = g->y + (g->dx)*g->half;
    if (!seg_cross(px, py, x, y, ax, ay, bx, by)) return 0;
    /* and only in the direction of travel, so reversing back out un-scores nothing */
    if ((x-px)*g->dx + (y-py)*g->dy <= 0.0f) return 0;

    const WEvent *e = &w->city.ev[R->ev];
    int was = R->next;
    if (was == 0) {
        if (R->lap == 0) { R->lap = 1; printf("race: start line crossed, lap 1/%d\n", R->maxlaps); }
        else {
            printf("race: LAP %d complete (%d/%d checkpoints)\n",
                   R->lap, R->cleared, R->ngate - 1);
            R->lap++;
            if (R->lap > R->maxlaps) { R->finished = 1; R->lap = R->maxlaps;
                printf("race: FINISHED event %d after %d lap(s)\n", e->id, R->maxlaps); }
        }
        R->cleared = 0;
    } else {
        R->cleared++;
        printf("race: checkpoint %d/%d cleared at (%.1f, %.1f)  lap %d/%d\n",
               was, R->ngate - 1, g->x, g->y, R->lap, R->maxlaps);
    }
    if (e->circuit) R->next = (was + 1) % R->ngate;
    else if (was + 1 < R->ngate) R->next = was + 1;
    else { R->finished = 1; printf("race: FINISHED sprint %d\n", e->id); }
    return 1;
}

int world_nav_nearest(const World *w, float x, float y) {
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < w->city.nnav; i++) {
        float dx = w->city.nav[i*2]-x, dy = w->city.nav[i*2+1]-y;
        float d2 = dx*dx + dy*dy;
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best;
}

/* binary min-heap over (f, node) */
typedef struct { float f; int n; } HeapIt;
static void heap_push(HeapIt *h, int *n, HeapIt v) {
    int i = (*n)++; h[i] = v;
    while (i > 0) { int p = (i-1)/2; if (h[p].f <= h[i].f) break;
        HeapIt t = h[p]; h[p] = h[i]; h[i] = t; i = p; }
}
static HeapIt heap_pop(HeapIt *h, int *n) {
    HeapIt top = h[0]; h[0] = h[--(*n)]; int i = 0;
    for (;;) { int l = i*2+1, r = l+1, m = i;
        if (l < *n && h[l].f < h[m].f) m = l;
        if (r < *n && h[r].f < h[m].f) m = r;
        if (m == i) break;
        HeapIt t = h[m]; h[m] = h[i]; h[i] = t; i = m; }
    return top;
}

int world_route(const World *w, int start, int goal, int *out, int cap, float *outdist) {
    if (outdist) *outdist = 0.0f;
    if (!w->city.adjstart || start < 0 || goal < 0 || start >= w->city.nnav || goal >= w->city.nnav) return 0;
    /* Race mode prunes the graph to the active corridor, so a route that would
       cross a barrier is either re-routed inside it or reported unreachable. */
    if (w->city.mode == MODE_RACE_EVENT && w->city.navopen &&
        (!w->city.navopen[start] || !w->city.navopen[goal])) return 0;
    float *g = (float *)malloc((size_t)w->city.nnav * sizeof(float));
    int *came = (int *)malloc((size_t)w->city.nnav * sizeof(int));
    char *closed = (char *)calloc((size_t)w->city.nnav, 1);
    for (int i = 0; i < w->city.nnav; i++) { g[i] = 1e30f; came[i] = -1; }
    HeapIt *heap = (HeapIt *)malloc((size_t)(w->city.nadj + 16) * sizeof(HeapIt));
    int hn = 0;
    float gx = w->city.nav[goal*2], gy = w->city.nav[goal*2+1];
    g[start] = 0.0f;
    HeapIt s0; s0.n = start;
    s0.f = hypotf(w->city.nav[start*2]-gx, w->city.nav[start*2+1]-gy);
    heap_push(heap, &hn, s0);
    int found = 0;
    while (hn > 0) {
        HeapIt cur = heap_pop(heap, &hn);
        if (cur.n == goal) { found = 1; break; }
        if (closed[cur.n]) continue;
        closed[cur.n] = 1;
        for (int k = w->city.adjstart[cur.n]; k < w->city.adjstart[cur.n+1]; k++) {
            int nb = w->city.adjlist[k];
            if (closed[nb]) continue;
            if (w->city.mode == MODE_RACE_EVENT && !w->city.navopen[nb]) continue;  /* barrier */
            float dx = w->city.nav[nb*2]-w->city.nav[cur.n*2], dy = w->city.nav[nb*2+1]-w->city.nav[cur.n*2+1];
            float ng = g[cur.n] + hypotf(dx, dy);
            if (ng >= g[nb]) continue;
            g[nb] = ng; came[nb] = cur.n;
            HeapIt it; it.n = nb;
            it.f = ng + hypotf(w->city.nav[nb*2]-gx, w->city.nav[nb*2+1]-gy);
            if (hn < w->city.nadj + 15) heap_push(heap, &hn, it);
        }
    }
    int n = 0;
    if (found) {
        int chain[8192], c = 0;
        for (int v = goal; v >= 0 && c < 8192; v = came[v]) chain[c++] = v;
        for (int i = c-1; i >= 0 && n < cap; i--) out[n++] = chain[i];
        if (outdist) *outdist = g[goal];
    }
    free(g); free(came); free(closed); free(heap);
    return n;
}

/* Nearest-neighbour semantic transfer (Phase 69): give every nav node the area
   code of the TRN_ terrain mesh it sits on.

   WHY THIS AND NOT BBOXES: 3D mesh bounds overlap, so "which box is the car in"
   was ambiguous (Phase 66). A nav node is a POINT, and a point has exactly one
   NEAREST terrain chunk, so the assignment is unique and stable.

   The 5 area codes are the artists' own, straight out of the asset names
   (TRN_SH_/TRN_UC_/TRN_CN_/TRN_CS_/TRN_IP_) -- not invented, not clustered. */
#define TRN_CELL 128.0f

static int wd_slot(World *w, const char *tok) {
    for (int i = 0; i < w->city.ndist; i++) if (!strcmp(w->city.dist[i].tok, tok)) return i;
    if (w->city.ndist >= WORLD_MAXDIST) return -1;
    WDistrict *d = &w->city.dist[w->city.ndist];
    snprintf(d->tok, sizeof d->tok, "%s", tok);
    d->n = 0; d->cx = d->cy = 0; d->medz = 0;
    d->bb[0] = d->bb[2] = 1e30f; d->bb[1] = d->bb[3] = -1e30f;
    return w->city.ndist++;
}
static int cmp_f(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y);
}

int world_build_districts(World *w) {
    w->city.ndist = 0;
    if (w->city.nnav <= 0) return 0;

    int cap = 4096, nt = 0;
    float *tc = (float *)malloc((size_t)cap * 3 * sizeof(float));   /* x, y, z */
    int   *ta = (int *)malloc((size_t)cap * sizeof(int));           /* district slot */
    for (int i = 0; i < w->neighborhood.scene.count; i++) {
        const char *nm = w->neighborhood.scene.meshes[i].sname;
        if (strncmp(nm, "TRN_", 4)) continue;
        const char *a = nm + 4;
        if (!isalpha((unsigned char)a[0]) || !isalpha((unsigned char)a[1]) || a[2] != '_') continue;
        char tok[4]; tok[0]=a[0]; tok[1]=a[1]; tok[2]=0; tok[3]=0;
        int k = wd_slot(w, tok); if (k < 0) continue;
        float bb[6]; n2_mesh_bbox(&w->neighborhood.scene.meshes[i], bb);
        if (nt == cap) { cap *= 2;
            tc = (float *)realloc(tc, (size_t)cap*3*sizeof(float));
            ta = (int *)realloc(ta, (size_t)cap*sizeof(int)); }
        tc[nt*3] = (bb[0]+bb[1])*0.5f; tc[nt*3+1] = (bb[2]+bb[3])*0.5f;
        tc[nt*3+2] = (bb[4]+bb[5])*0.5f; ta[nt] = k; nt++;
    }
    if (!nt) { free(tc); free(ta); return 0; }

    float gx0=1e30f, gy0=1e30f, gx1=-1e30f, gy1=-1e30f;
    for (int i = 0; i < nt; i++) {
        if (tc[i*3] < gx0) gx0 = tc[i*3];
        if (tc[i*3] > gx1) gx1 = tc[i*3];
        if (tc[i*3+1] < gy0) gy0 = tc[i*3+1];
        if (tc[i*3+1] > gy1) gy1 = tc[i*3+1];
    }
    int gw = (int)((gx1-gx0)/TRN_CELL) + 1, gh = (int)((gy1-gy0)/TRN_CELL) + 1;
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    int *head = (int *)malloc((size_t)gw*gh*sizeof(int));
    int *nxt  = (int *)malloc((size_t)nt*sizeof(int));
    for (int i = 0; i < gw*gh; i++) head[i] = -1;
    for (int i = 0; i < nt; i++) {
        int cx = (int)((tc[i*3]-gx0)/TRN_CELL), cy = (int)((tc[i*3+1]-gy0)/TRN_CELL);
        if (cx < 0) cx = 0;
        if (cx >= gw) cx = gw-1;
        if (cy < 0) cy = 0;
        if (cy >= gh) cy = gh-1;
        nxt[i] = head[cy*gw+cx]; head[cy*gw+cx] = i;
    }

    w->city.navcomp = (int *)malloc((size_t)w->city.nnav * sizeof(int));
    float *zs = (float *)malloc((size_t)nt * sizeof(float));
    for (int i = 0; i < w->city.nnav; i++) {
        float x = w->city.nav[i*2], y = w->city.nav[i*2+1];
        int cx = (int)((x-gx0)/TRN_CELL), cy = (int)((y-gy0)/TRN_CELL);
        int best = -1; float bd = 1e30f;
        for (int ring = 0; ring <= 10 && best < 0; ring++) {
            for (int dy = -ring; dy <= ring; dy++) for (int dx = -ring; dx <= ring; dx++) {
                if (ring && abs(dx) != ring && abs(dy) != ring) continue;
                int ax = cx+dx, ay = cy+dy;
                if (ax < 0 || ay < 0 || ax >= gw || ay >= gh) continue;
                for (int j = head[ay*gw+ax]; j >= 0; j = nxt[j]) {
                    float ddx = tc[j*3]-x, ddy = tc[j*3+1]-y;
                    float dd = ddx*ddx + ddy*ddy;
                    if (dd < bd) { bd = dd; best = j; }
                }
            }
        }
        int k = best >= 0 ? ta[best] : -1;
        w->city.navcomp[i] = k;
        if (k < 0) continue;
        WDistrict *d = &w->city.dist[k];
        d->n++; d->cx += x; d->cy += y;
        if (x < d->bb[0]) d->bb[0] = x;
        if (x > d->bb[1]) d->bb[1] = x;
        if (y < d->bb[2]) d->bb[2] = y;
        if (y > d->bb[3]) d->bb[3] = y;
    }
    for (int k = 0; k < w->city.ndist; k++) {
        if (w->city.dist[k].n) { w->city.dist[k].cx /= w->city.dist[k].n; w->city.dist[k].cy /= w->city.dist[k].n; }
        int nzs = 0;
        for (int i = 0; i < nt; i++) if (ta[i] == k) zs[nzs++] = tc[i*3+2];
        if (nzs) { qsort(zs, (size_t)nzs, sizeof(float), cmp_f); w->city.dist[k].medz = zs[nzs/2]; }
    }
    free(tc); free(ta); free(head); free(nxt); free(zs);
    return w->city.ndist;
}

int world_district_at(const World *w, float x, float y, float maxdist) {
    /* world_build_districts() returns early (leaving navcomp NULL) when a region
       has nav nodes but no district-tokened terrain -- L4RB names its ground
       TRN_RDP_ and TRN_ROADDRAG_, three-letter tokens the slot parser skips. No
       districts means no district here. */
    if (!w->city.navcomp) return -1;
    int best = -1; float bd = maxdist*maxdist;
    for (int i = 0; i < w->city.nnav; i++) {
        float dx = w->city.nav[i*2]-x, dy = w->city.nav[i*2+1]-y;
        float d2 = dx*dx + dy*dy;
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best >= 0 ? w->city.navcomp[best] : -1;
}

/* The one layer-selection rule (was inlined in n2_ground_z_ref; lifted here so
   the same pick can also report the surface category). Identical arithmetic:
   ROAD/TERRAIN only, barycentric coverage with the -0.01 edge tolerance, and
   the reference key that biases UP-steps by 3x so a curb lip above the car
   never beats the road just below it. */
static int wg_pick(const N2Scene *s, const int *srcmap, float x, float y, float refz,
                   float *outz, float outn[3], WGroundHit *hit) {
    float best = 0.0f, bestkey = 1e30f; int found = 0, bestcat = WSURF_NONE;
    int highest = (refz >= N2_GROUND_HIGHEST);
    for (int m = 0; m < s->count; m++) {
        int mc = s->meshes[m].cat;
        if (mc != N2_ROAD && mc != N2_TERRAIN) continue;
        const N2Mesh *me = &s->meshes[m];
        for (int t = 0; t + 2 < me->nidx; t += 3) {
            const float *a = me->verts + me->idx[t]*5;
            const float *b = me->verts + me->idx[t+1]*5;
            const float *c = me->verts + me->idx[t+2]*5;
            float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
            if (d > -1e-9f && d < 1e-9f) continue;
            float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / d;
            float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / d;
            float w = 1.0f - u - v;
            if (u < -0.01f || v < -0.01f || w < -0.01f) continue;
            float z = u*a[2] + v*b[2] + w*c[2];
            float key;
            if (highest) key = -z;
            else { float dz = z - refz; key = dz >= 0 ? dz*3.0f : -dz; }
            if (!found || key < bestkey) {
                bestkey = key; best = z; found = 1;
                bestcat = (mc == N2_ROAD) ? WSURF_ROAD : WSURF_TERRAIN;
                if (hit) {
                    hit->mesh = srcmap ? srcmap[m] : m;
                    hit->tri = t / 3;
                    hit->cat = bestcat;
                    hit->z = z;
                }
                if (outn) {
                    float e1x=b[0]-a[0], e1y=b[1]-a[1], e1z=b[2]-a[2];
                    float e2x=c[0]-a[0], e2y=c[1]-a[1], e2z=c[2]-a[2];
                    float nx=e1y*e2z-e1z*e2y;
                    float ny=e1z*e2x-e1x*e2z;
                    float nz=e1x*e2y-e1y*e2x;
                    float nl=sqrtf(nx*nx+ny*ny+nz*nz);
                    if (nl > 1e-9f) {
                        if (nz < 0) { nx=-nx; ny=-ny; nz=-nz; }
                        outn[0]=nx/nl; outn[1]=ny/nl; outn[2]=nz/nl;
                    }
                }
                if (hit) {
                    float *hn = hit->normal;
                    if (outn) { hn[0]=outn[0]; hn[1]=outn[1]; hn[2]=outn[2]; }
                    else {
                        float e1x=b[0]-a[0], e1y=b[1]-a[1], e1z=b[2]-a[2];
                        float e2x=c[0]-a[0], e2y=c[1]-a[1], e2z=c[2]-a[2];
                        float nx=e1y*e2z-e1z*e2y;
                        float ny=e1z*e2x-e1x*e2z;
                        float nz=e1x*e2y-e1y*e2x;
                        float nl=sqrtf(nx*nx+ny*ny+nz*nz);
                        if (nl > 1e-9f) {
                            if (nz < 0) { nx=-nx; ny=-ny; nz=-nz; }
                            hn[0]=nx/nl; hn[1]=ny/nl; hn[2]=nz/nl;
                        } else { hn[0]=0.0f; hn[1]=0.0f; hn[2]=1.0f; }
                    }
                }
            }
        }
    }
    if (found) *outz = best;
    return found ? bestcat : WSURF_NONE;
}

static int wg_at(const N2Scene *s, float x, float y, float fallback,
                 float *outz, float outn[3], WGroundHit *hit) {
    /* `fallback` is the caller's current Z at every callsite, so it doubles as
       the layer reference: pick the surface nearest it, not the highest deck
       overhead (Phase 73). A sentinel fallback (|z| huge) means "no reference". */
    float refz = (fallback > -2000.0f && fallback < 4000.0f) ? fallback
                                                             : N2_GROUND_HIGHEST;
    *outz = fallback;
    if (outn) { outn[0]=0.0f; outn[1]=0.0f; outn[2]=1.0f; }
    if (hit) { hit->mesh=-1; hit->tri=-1; hit->cat=WSURF_NONE; hit->z=fallback;
               hit->normal[0]=0.0f; hit->normal[1]=0.0f; hit->normal[2]=1.0f; }
    if (s->meshes != g_grid.meshes)                        /* not the loaded world */
        return wg_pick(s, NULL, x, y, refz, outz, outn, hit);
    int cx = (int)((x - g_grid.x0) / GCELL), cy = (int)((y - g_grid.y0) / GCELL);
    if (cx < 0 || cy < 0 || cx >= g_grid.gw || cy >= g_grid.gh) return WSURF_NONE;
    /* borrow the cell's meshes into a scratch scene and reuse the exact scan */
    static N2Mesh scratch[512];
    static int srcmap[512];
    N2Scene sub = { scratch, 0, 512 };
    int c = cy*g_grid.gw + cx;
    for (int k = g_grid.start[c]; k < g_grid.start[c+1] && sub.count < 512; k++) {
        int src = g_grid.list[k];
        srcmap[sub.count] = src;
        scratch[sub.count++] = s->meshes[src];
    }
    return wg_pick(&sub, srcmap, x, y, refz, outz, outn, hit);
}

/* ROAD/TERRAIN triangle covering (x,y) and CLOSEST to wz inside [wz-down,
   wz+up]. Same coverage test and the same grid fast path as wg_pick; only the
   acceptance rule differs, so this cannot select a surface the wheel cannot
   reach. The nearest covering triangle overall is recorded separately in
   *nearest, whether or not it is reachable, so a rejection stays attributable. */
static int wws_pick(const N2Scene *s, const int *srcmap, float x, float y,
                    float wz, float up, float down, WGroundHit *hit,
                    WGroundHit *nearest, float *neard) {
    int bestcat = WSURF_NONE; float bestad = 1e30f;
    for (int m = 0; m < s->count; m++) {
        int mc = s->meshes[m].cat;
        if (mc != N2_ROAD && mc != N2_TERRAIN) continue;
        const N2Mesh *me = &s->meshes[m];
        for (int t = 0; t + 2 < me->nidx; t += 3) {
            const float *a = me->verts + me->idx[t]*5;
            const float *b = me->verts + me->idx[t+1]*5;
            const float *c = me->verts + me->idx[t+2]*5;
            float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
            if (d > -1e-9f && d < 1e-9f) continue;
            float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / d;
            float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / d;
            float w = 1.0f - u - v;
            if (u < -0.01f || v < -0.01f || w < -0.01f) continue;
            float z = u*a[2] + v*b[2] + w*c[2];
            int cat = (mc == N2_ROAD) ? WSURF_ROAD : WSURF_TERRAIN;
            float dz = z - wz, ad = dz < 0 ? -dz : dz;
            if (ad < *neard) {                       /* remember for diagnostics */
                *neard = ad;
                if (nearest) { nearest->mesh = srcmap ? srcmap[m] : m; nearest->tri = t/3;
                               nearest->cat = cat; nearest->z = z;
                               nearest->normal[0]=0; nearest->normal[1]=0; nearest->normal[2]=1; }
            }
            if (dz > up || dz < -down) continue;     /* candidate, not contact */
            /* Among reachable candidates the CLOSEST wins: continuity, so the
               surface the wheel is already riding (0 m away) always beats one
               stacked above it. The window above is what refuses a deck the
               wheel cannot reach; this tie-break is not load-bearing for that. */
            if (bestcat != WSURF_NONE && ad >= bestad) continue;
            bestcat = cat; bestad = ad;
            if (hit) {
                hit->mesh = srcmap ? srcmap[m] : m; hit->tri = t/3;
                hit->cat = cat; hit->z = z;
                float e1x=b[0]-a[0], e1y=b[1]-a[1], e1z=b[2]-a[2];
                float e2x=c[0]-a[0], e2y=c[1]-a[1], e2z=c[2]-a[2];
                float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
                float nl=sqrtf(nx*nx+ny*ny+nz*nz);
                if (nl > 1e-9f) { if (nz < 0) { nx=-nx; ny=-ny; nz=-nz; }
                                  hit->normal[0]=nx/nl; hit->normal[1]=ny/nl; hit->normal[2]=nz/nl; }
                else { hit->normal[0]=0; hit->normal[1]=0; hit->normal[2]=1; }
            }
        }
    }
    return bestcat;
}

int world_wheel_support(const N2Scene *s, float x, float y, float wheel_z,
                        float reach_up, float reach_down,
                        WGroundHit *hit, WGroundHit *cand, int *verdict) {
    WGroundHit near; float neard = 1e30f;
    near.mesh=-1; near.tri=-1; near.cat=WSURF_NONE; near.z=wheel_z;
    near.normal[0]=0; near.normal[1]=0; near.normal[2]=1;
    if (hit) { hit->mesh=-1; hit->tri=-1; hit->cat=WSURF_NONE; hit->z=wheel_z;
               hit->normal[0]=0; hit->normal[1]=0; hit->normal[2]=1; }
    int cat;
    if (s->meshes != g_grid.meshes)
        cat = wws_pick(s, NULL, x, y, wheel_z, reach_up, reach_down, hit, &near, &neard);
    else {
        int cx = (int)((x - g_grid.x0) / GCELL), cy = (int)((y - g_grid.y0) / GCELL);
        if (cx < 0 || cy < 0 || cx >= g_grid.gw || cy >= g_grid.gh) {
            if (cand) *cand = near;
            if (verdict) *verdict = WWS_NOCOVER;
            return WSURF_NONE;
        }
        static N2Mesh scratch[512];
        static int srcmap[512];
        N2Scene sub = { scratch, 0, 512 };
        int c = cy*g_grid.gw + cx;
        for (int k = g_grid.start[c]; k < g_grid.start[c+1] && sub.count < 512; k++) {
            int src = g_grid.list[k]; srcmap[sub.count] = src; scratch[sub.count++] = s->meshes[src];
        }
        cat = wws_pick(&sub, srcmap, x, y, wheel_z, reach_up, reach_down, hit, &near, &neard);
    }
    if (cand) *cand = near;                  /* attribution, reachable or not */
    if (cat != WSURF_NONE) { if (verdict) *verdict = WWS_CONTACT; return cat; }
    /* No contact: say WHY, and hand the caller the candidate it refused. */
    if (hit && near.mesh >= 0) *hit = near;
    if (verdict) *verdict = near.mesh < 0 ? WWS_NOCOVER
                          : (near.z > wheel_z ? WWS_ABOVE : WWS_BELOW);
    return WSURF_NONE;
}

static void wgs_mesh(const N2Mesh *m, int mi, const float p[3], const float q[3],
                     float *best, WGroundHit *hit) {
    if (m->cat != N2_ROAD && m->cat != N2_TERRAIN) return;
    for (int t=0; t+2<m->nidx; t+=3) {
        const float *a=m->verts+m->idx[t]*5, *b=m->verts+m->idx[t+1]*5,
                    *c=m->verts+m->idx[t+2]*5;
        /* Double intermediates keep centimetre-scale tests stable at city
         * coordinates thousands of metres from the origin. */
        double ex=b[0]-a[0], ey=b[1]-a[1], ez=b[2]-a[2];
        double fx=c[0]-a[0], fy=c[1]-a[1], fz=c[2]-a[2];
        double nx=ey*fz-ez*fy, ny=ez*fx-ex*fz, nz=ex*fy-ey*fx;
        if (fabs(nz)<1e-9) continue;
        if (nz<0) {nx=-nx;ny=-ny;nz=-nz;}
        double d0=(nx*(p[0]-a[0])+ny*(p[1]-a[1])+nz*(p[2]-a[2]))/nz;
        double d1=(nx*(q[0]-a[0])+ny*(q[1]-a[1])+nz*(q[2]-a[2]))/nz;
        if (d0<0 || d1>=0) continue;
        double f=d0/(d0-d1);
        if (f>=*best) continue;
        double x=p[0]+(q[0]-p[0])*f, y=p[1]+(q[1]-p[1])*f;
        double det=ex*fy-ey*fx;
        double u=((x-a[0])*fy-(y-a[1])*fx)/det;
        double v=(ex*(y-a[1])-ey*(x-a[0]))/det;
        if (u<0 || v<0 || u+v>1) continue;
        *best=(float)f;
        if (hit) {
            double len=sqrt(nx*nx+ny*ny+nz*nz);
            hit->mesh=mi;hit->tri=t/3;
            hit->cat=m->cat==N2_ROAD?WSURF_ROAD:WSURF_TERRAIN;
            hit->z=(float)(p[2]+(q[2]-p[2])*f);
            hit->normal[0]=(float)(nx/len);hit->normal[1]=(float)(ny/len);
            hit->normal[2]=(float)(nz/len);
        }
    }
}

float world_ground_sweep(const N2Scene *s, const float from[3],
                         const float to[3], WGroundHit *hit) {
    float best=1;
    if (hit) { memset(hit,0,sizeof *hit);hit->mesh=hit->tri=-1; }
    if (!s || !s->meshes) return best;
    if (s->meshes!=g_grid.meshes) {
        for(int i=0;i<s->count;i++)wgs_mesh(&s->meshes[i],i,from,to,&best,hit);
    } else {
        int x0=(int)floorf((fminf(from[0],to[0])-g_grid.x0)/GCELL);
        int x1=(int)floorf((fmaxf(from[0],to[0])-g_grid.x0)/GCELL);
        int y0=(int)floorf((fminf(from[1],to[1])-g_grid.y0)/GCELL);
        int y1=(int)floorf((fmaxf(from[1],to[1])-g_grid.y0)/GCELL);
        if(x0<0)x0=0;if(y0<0)y0=0;
        if(x1>=g_grid.gw)x1=g_grid.gw-1;if(y1>=g_grid.gh)y1=g_grid.gh-1;
        /* Traverse all source indices, without a capped scratch scene. */
        for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++) {
            int cell=y*g_grid.gw+x;
            for(int k=g_grid.start[cell];k<g_grid.start[cell+1];k++) {
                int i=g_grid.list[k];wgs_mesh(&s->meshes[i],i,from,to,&best,hit);
            }
        }
    }
    return best;
}

int world_ground_at(const N2Scene *s, float x, float y, float fallback, float *outz) {
    return wg_at(s, x, y, fallback, outz, NULL, NULL);
}

int world_ground_pose(const N2Scene *s, float x, float y, float fallback,
                      float *outz, float outn[3]) {
    return wg_at(s, x, y, fallback, outz, outn, NULL);
}

int world_ground_hit(const N2Scene *s, float x, float y, float fallback,
                     WGroundHit *hit) {
    float z = fallback, n[3] = {0,0,1};
    return wg_at(s, x, y, fallback, &z, n, hit);
}

int world_ground_patch_normal(const N2Scene *s, float x, float y, float heading,
                              float front, float rear, float halftrack,
                              const WGroundHit *centre, float outn[3]) {
    if (!s || !centre || centre->cat == WSURF_NONE || front <= 0.05f ||
        rear >= -0.05f || halftrack <= 0.05f) return 0;
    float fx=cosf(heading), fy=sinf(heading), lx=-fy, ly=fx;
    float px[4]={x+fx*front, x+fx*rear, x+lx*halftrack, x-lx*halftrack};
    float py[4]={y+fy*front, y+fy*rear, y+ly*halftrack, y-ly*halftrack};
    float dist[4]={front,-rear,halftrack,halftrack};
    WGroundHit h[4];
    float maxslope = centre->cat == WSURF_ROAD ? 0.55f : 0.85f;
    for (int i=0;i<4;i++) {
        int cat=world_ground_hit(s,px[i],py[i],centre->z,&h[i]);
        if (cat != centre->cat) return 0;
        if (fabsf(h[i].z-centre->z) > 0.20f+dist[i]*maxslope) return 0;
    }
    float span=front-rear;
    float zlong=(h[0].z*(-rear)+h[1].z*front)/span;
    float zlat=0.5f*(h[2].z+h[3].z);
    float centre_tol=centre->cat == WSURF_ROAD ? 0.35f : 0.75f;
    if (fabsf(zlong-centre->z)>centre_tol || fabsf(zlat-centre->z)>centre_tol ||
        fabsf(zlong-zlat)>centre_tol) return 0;
    float along=(h[0].z-h[1].z)/span;
    float across=(h[2].z-h[3].z)/(2.0f*halftrack);
    outn[0]=-along*fx-across*lx;
    outn[1]=-along*fy-across*ly;
    outn[2]=1.0f;
    float len=sqrtf(outn[0]*outn[0]+outn[1]*outn[1]+1.0f);
    outn[0]/=len; outn[1]/=len; outn[2]/=len;
    return 1;
}

float world_ground_z(const N2Scene *s, float x, float y, float fallback) {
    float z = fallback;
    world_ground_at(s, x, y, fallback, &z);
    return z;
}

void world_ground_selftest(void) {
    /* Two stacked decks with different normals: reference Z must select one
       triangle and return that SAME triangle's height and orientation. */
    float lower_v[15]={0,0,0,0,0, 2,0,0,0,0, 0,2,0,0,0};
    float upper_v[15]={0,0,10,0,0, 2,0,11,0,0, 0,2,10,0,0};
    uint16_t tri[3]={0,1,2};
    N2Mesh m[2]; memset(m,0,sizeof m);
    m[0].verts=lower_v; m[0].nverts=3; m[0].idx=tri; m[0].nidx=3; m[0].cat=N2_ROAD;
    m[1].verts=upper_v; m[1].nverts=3; m[1].idx=tri; m[1].nidx=3; m[1].cat=N2_ROAD;
    N2Scene s={m,2,2}; float z=0,n[3]; WGroundHit hit;
    assert(world_ground_pose(&s,0.25f,0.25f,0.0f,&z,n)==WSURF_ROAD);
    assert(fabsf(z)<1e-6f && fabsf(n[2]-1.0f)<1e-6f);
    assert(world_ground_pose(&s,0.25f,0.25f,10.0f,&z,n)==WSURF_ROAD);
    assert(world_ground_hit(&s,0.25f,0.25f,10.0f,&hit)==WSURF_ROAD);
    assert(hit.mesh==1 && hit.tri==0 && fabsf(hit.z-z)<1e-6f);
    assert(fabsf(hit.normal[0]-n[0])<1e-6f &&
           fabsf(hit.normal[1]-n[1])<1e-6f &&
           fabsf(hit.normal[2]-n[2])<1e-6f);
    assert(z>10.0f && z<11.0f && n[0]<-0.4f && n[2]>0.8f);

    /* A noisy centre-triangle normal must not pitch the entire chassis when
       the footprint itself lies on one coherent shallow road plane. */
    { float pv[20]={-5,-5,-0.5f,0,0, 5,-5,0.5f,0,0,
                     5, 5, 0.5f,0,0, -5,5,-0.5f,0,0};
      uint16_t pi[6]={0,1,2,0,2,3};
      N2Mesh pm; memset(&pm,0,sizeof pm);
      pm.verts=pv; pm.nverts=4; pm.idx=pi; pm.nidx=6; pm.cat=N2_ROAD;
      N2Scene ps={&pm,1,1};
      WGroundHit centre={0,0,WSURF_ROAD,0.0f,{0.6f,0.0f,0.8f}};
      float pn[3]={0,0,1};
      assert(world_ground_patch_normal(&ps,0,0,0,1.2f,-1.2f,0.7f,
                                       &centre,pn));
      assert(fabsf(pn[0]+0.0995037f)<1e-4f && fabsf(pn[1])<1e-5f &&
             fabsf(pn[2]-0.995037f)<1e-4f);
    }
}

/* Guardrail/fence collision (Phase 58). Rails are NOT separate meshes with a
   boundary flag -- they are near-vertical triangles baked into large ROAD/
   TERRAIN meshes (measured: 6 of 109 tris in one road chunk). So collide them
   per-triangle: in the car's grid cell, any ground-mesh triangle whose face is
   near-vertical (|Nz|<0.30 -- steep enough to be a wall, not the drivable ~10
   deg tarmac whose Nz~0.98) is a rail. Treat its XY projection as a segment and
   push the car circle (radius r) back out along the horizontal face normal if
   it crosses, while the car's Z sits within the rail's height span. Returns 1
   if it pushed. */
static float seg_d2(float px,float py,float ax,float ay,float bx,float by,float *ox,float *oy){
    float dx=bx-ax, dy=by-ay, L2=dx*dx+dy*dy;
    float t = L2>1e-9f ? ((px-ax)*dx+(py-ay)*dy)/L2 : 0.0f;
    if (t<0) t=0; else if (t>1) t=1;
    *ox=ax+t*dx; *oy=ay+t*dy;
    float ex=px-*ox, ey=py-*oy; return ex*ex+ey*ey;
}
/* M113 census: every triangle that PASSES world_wall_push's near-vertical and
 * height tests -- the candidate rail population the car actually met -- bucketed
 * by source category and by the triangle's own Z span. Diagnostic; it never
 * influences the push. Enabled by world_rail_census. */
/* Measured barrier-face height band. Above (M113): genuine rails 1.778-1.833 m,
 * false hillside faces 3.389 m and up. Below (M119): the drag surface's own mesh
 * seams are 0.097-0.425 m -- 96840 such candidates on one sprint run, all under
 * half a metre, none of them a barrier. A rail is a knee-high-or-taller wall, so
 * take the empty band between 0.425 and 1.778 m. */
#define WALL_RAIL_MIN_H 0.75f
#define WALL_RAIL_MAX_H 2.5f
int  world_rail_census = 0;
long world_rc_cand[2][8];      /* [0]=ROAD [1]=TERRAIN, by Z-span bucket */
long world_rc_push[2][8];      /* same, restricted to triangles that pushed */
float world_rc_min[2] = { 1e30f, 1e30f }, world_rc_max[2] = { -1e30f, -1e30f };
static int wrc_bucket(float zs) {
    if (zs < 0.5f) return 0; if (zs < 1.0f) return 1; if (zs < 1.5f) return 2;
    if (zs < 2.0f) return 3;  if (zs < 3.0f) return 4; if (zs < 5.0f) return 5;
    if (zs < 10.0f) return 6; return 7;
}

int world_wall_push(const N2Scene *s, float *pos, float r, WRailHit *hit) {
    if (s->meshes != g_grid.meshes) return 0;
    int cx = (int)((pos[0]-g_grid.x0)/GCELL), cy = (int)((pos[1]-g_grid.y0)/GCELL);
    if (cx < 0 || cy < 0 || cx >= g_grid.gw || cy >= g_grid.gh) return 0;
    int c = cy*g_grid.gw + cx, pushed = 0;
    for (int k = g_grid.start[c]; k < g_grid.start[c+1]; k++) {
        const N2Mesh *m = &s->meshes[g_grid.list[k]];
        for (int t = 0; t+2 < m->nidx; t += 3) {
            const float *a=m->verts+m->idx[t]*5, *b=m->verts+m->idx[t+1]*5, *cc=m->verts+m->idx[t+2]*5;
            float e1x=b[0]-a[0],e1y=b[1]-a[1],e1z=b[2]-a[2], e2x=cc[0]-a[0],e2y=cc[1]-a[1],e2z=cc[2]-a[2];
            float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
            float L=sqrtf(nx*nx+ny*ny+nz*nz); if (L<1e-6f) continue;
            if (fabsf(nz/L) >= 0.30f) continue;                 /* not a wall face */
            float zlo=a[2],zhi=a[2];                            /* rail height span */
            if(b[2]<zlo)zlo=b[2]; if(cc[2]<zlo)zlo=cc[2]; if(b[2]>zhi)zhi=b[2]; if(cc[2]>zhi)zhi=cc[2];
            if (pos[2] < zlo-0.5f || pos[2] > zhi+0.5f) continue;   /* car not at rail height */
            /* A rail is a LOW barrier. |nz| alone also selects hillsides: the
               L4RA route's near-vertical faces are 3.389-44.343 m tall and one
               9.4 m slope pushed the car 36 times, while every genuine road-side
               barrier met on L4RB measured 1.778-1.833 m. Nothing lies between
               those ranges across ~16000 candidate triangles, so cap the face
               height; the geometry decides, not the mesh's category. Taller
               vertical faces are buildings or terrain, which collide_walls and
               the ground query already own. */
            if (world_rail_census) {   /* population BEFORE the height cap */
                int ci = (m->cat == N2_ROAD) ? 0 : 1;
                float zs = zhi - zlo;
                world_rc_cand[ci][wrc_bucket(zs)]++;
                if (zs < world_rc_min[ci]) world_rc_min[ci] = zs;
                if (zs > world_rc_max[ci]) world_rc_max[ci] = zs;
            }
            if (zhi - zlo > WALL_RAIL_MAX_H) continue;
            /* ...and a face too SHORT to be a barrier is a seam in the surface:
               nz says vertical, but a 10 cm sliver in the drag strip pushed the
               car 1464 times on one sprint (TRN_RDP_DRAG1_01_CHOP_B1_R2 tri 40,
               nz 0.000, Z 2.12..2.22). Geometry only -- no names, no categories. */
            if (zhi - zlo < WALL_RAIL_MIN_H) continue;
            /* nearest point on the tri's longest XY edge (its footprint line) */
            float ox,oy, best=1e30f, bx=0,by=0;
            float d0=seg_d2(pos[0],pos[1],a[0],a[1],b[0],b[1],&ox,&oy); if(d0<best){best=d0;bx=ox;by=oy;}
            float d1=seg_d2(pos[0],pos[1],b[0],b[1],cc[0],cc[1],&ox,&oy); if(d1<best){best=d1;bx=ox;by=oy;}
            float d2=seg_d2(pos[0],pos[1],cc[0],cc[1],a[0],a[1],&ox,&oy); if(d2<best){best=d2;bx=ox;by=oy;}
            if (world_rail_census && best < r*r)
                world_rc_push[(m->cat == N2_ROAD) ? 0 : 1][wrc_bucket(zhi - zlo)]++;
            if (best >= r*r) continue;
            if (hit && !pushed) { hit->mesh = g_grid.list[k]; hit->tri = t/3;
                                  hit->nz = nz/L; hit->zlo = zlo; hit->zhi = zhi;
                                  hit->edged = sqrtf(best); }
            float hx=pos[0]-bx, hy=pos[1]-by;
            float hl = sqrtf(hx*hx+hy*hy); if (hl<1e-6f){ hx=nx; hy=ny; hl=sqrtf(hx*hx+hy*hy); if(hl<1e-6f)continue; }
            pos[0] = bx + hx/hl*r; pos[1] = by + hy/hl*r;        /* shove back to r */
            pushed = 1;
        }
    }
    return pushed;
}

int world_wall_clear_at(const N2Scene *s, float x, float y, float z, float r) {
    float probe[3] = {x,y,z};
    return !world_wall_push(s,probe,r,NULL);
}
