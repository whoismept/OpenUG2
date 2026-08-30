#include "world_instance.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nfsu2.h"

#define WINST_WORLD_LIMIT 1e8f

typedef struct {
    N2Scene scene;
    float matrix[16];
    uint32_t name_hash;
    char name[28];
    unsigned char has_matrix;
    unsigned char is_vista;
} WInstProto;

typedef struct {
    WInstProto *items;
    int count, cap;
} WInstLibrary;

/* Kept out of the public header: the standalone regression uses this to prove
 * a rejected placement releases every copy allocated for that attempted mesh. */
static long winst_placement_live_allocations;

static void *winst_place_alloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr) winst_placement_live_allocations++;
    return ptr;
}

static void winst_place_free(void *ptr) {
    if (ptr) {
        winst_placement_live_allocations--;
        free(ptr);
    }
}

long winst_test_placement_live_allocations(void) {
    return winst_placement_live_allocations;
}

static void winst_reject(WInstStats *stats) {
    if (stats) stats->rejected_meshes++;
}

static int winst_push_mesh(N2Scene *dst, N2Mesh mesh) {
    if (dst->count == dst->cap) {
        int next = dst->cap ? dst->cap * 2 : 64;
        if (next <= dst->cap || (size_t)next > SIZE_MAX / sizeof *dst->meshes)
            return 0;
        N2Mesh *grown = (N2Mesh *)realloc(dst->meshes,
                                           (size_t)next * sizeof *dst->meshes);
        if (!grown) return 0;
        dst->meshes = grown;
        dst->cap = next;
    }
    dst->meshes[dst->count++] = mesh;
    return 1;
}

int winst_place_mesh(N2Scene *dst, const N2Mesh *src,
                     const float matrix[16], const char *asset_name,
                     WInstStats *stats) {
    if (!dst || !src || !matrix || !src->verts || !src->idx ||
        src->nverts <= 0 || src->nidx <= 0) {
        winst_reject(stats);
        return 0;
    }
    size_t nverts = (size_t)src->nverts, nidx = (size_t)src->nidx;
    if (nverts > SIZE_MAX / (5 * sizeof *src->verts) ||
        nidx > SIZE_MAX / sizeof *src->idx ||
        (src->vcol && nverts > SIZE_MAX / (4 * sizeof *src->vcol))) {
        winst_reject(stats);
        return 0;
    }

    N2Mesh placed = *src;
    placed.verts = (float *)winst_place_alloc(nverts * 5 * sizeof *placed.verts);
    placed.idx = (uint16_t *)winst_place_alloc(nidx * sizeof *placed.idx);
    placed.vcol = src->vcol ? (unsigned char *)winst_place_alloc(nverts * 4) : NULL;
    if (!placed.verts || !placed.idx || (src->vcol && !placed.vcol)) {
        winst_place_free(placed.verts);
        winst_place_free(placed.idx);
        winst_place_free(placed.vcol);
        winst_reject(stats);
        return 0;
    }

    memcpy(placed.idx, src->idx, nidx * sizeof *placed.idx);
    if (placed.vcol) memcpy(placed.vcol, src->vcol, nverts * 4);
    for (int i = 0; i < src->nverts; i++) {
        const float *in = src->verts + i * 5;
        float *out = placed.verts + i * 5;
        out[0] = in[0] * matrix[0] + in[1] * matrix[4] + in[2] * matrix[8] + matrix[12];
        out[1] = in[0] * matrix[1] + in[1] * matrix[5] + in[2] * matrix[9] + matrix[13];
        out[2] = in[0] * matrix[2] + in[1] * matrix[6] + in[2] * matrix[10] + matrix[14];
        out[3] = in[3];
        out[4] = in[4];
        if (!isfinite(out[0]) || !isfinite(out[1]) || !isfinite(out[2]) ||
            fabsf(out[0]) > WINST_WORLD_LIMIT || fabsf(out[1]) > WINST_WORLD_LIMIT ||
            fabsf(out[2]) > WINST_WORLD_LIMIT) {
            winst_place_free(placed.verts);
            winst_place_free(placed.idx);
            winst_place_free(placed.vcol);
            winst_reject(stats);
            return 0;
        }
    }
    placed.inst = 1;
    snprintf(placed.aname, sizeof placed.aname, "%.27s", asset_name ? asset_name : "");
    if (!winst_push_mesh(dst, placed)) {
        winst_place_free(placed.verts);
        winst_place_free(placed.idx);
        winst_place_free(placed.vcol);
        winst_reject(stats);
        return 0;
    }
    if (stats) stats->meshes_placed++;
    return 1;
}

static unsigned char winst_fold_char(unsigned char c) {
    return c >= 'a' && c <= 'z' ? (unsigned char)(c - ('a' - 'A')) : c;
}

static uint32_t winst_name_key(const char *name, char folded[28]) {
    uint32_t h = 2166136261u;
    int i = 0;
    if (name) {
        while (i < 27 && name[i]) {
            unsigned char c = winst_fold_char((unsigned char)name[i]);
            folded[i++] = (char)c;
            h = (h ^ c) * 16777619u;
        }
    }
    folded[i] = 0;
    return h;
}

static void winst_free_scene(N2Scene *scene) {
    if (!scene) return;
    for (int i = 0; i < scene->count; i++) {
        free(scene->meshes[i].verts);
        free(scene->meshes[i].idx);
        free(scene->meshes[i].vcol);
    }
    free(scene->meshes);
    memset(scene, 0, sizeof *scene);
}

static void winst_library_free(WInstLibrary *library) {
    if (!library) return;
    for (int i = 0; i < library->count; i++) winst_free_scene(&library->items[i].scene);
    free(library->items);
    memset(library, 0, sizeof *library);
}

static int winst_library_add(WInstLibrary *library, N2Scene *scene,
                             const char *name, const float matrix[16], int has_matrix,
                             int is_vista) {
    if (library->count == library->cap) {
        int next = library->cap ? library->cap * 2 : 64;
        if (next <= library->cap || (size_t)next > SIZE_MAX / sizeof *library->items)
            return 0;
        WInstProto *grown = (WInstProto *)realloc(
            library->items, (size_t)next * sizeof *library->items);
        if (!grown) return 0;
        library->items = grown;
        library->cap = next;
    }
    WInstProto *proto = &library->items[library->count++];
    memset(proto, 0, sizeof *proto);
    proto->scene = *scene;
    proto->name_hash = winst_name_key(name, proto->name);
    memcpy(proto->matrix, matrix, sizeof proto->matrix);
    proto->has_matrix = (unsigned char)(has_matrix != 0);
    proto->is_vista = (unsigned char)(is_vista != 0);
    memset(scene, 0, sizeof *scene);
    return 1;
}

/* Hashes narrow candidate lookup only: the folded name comparison is mandatory
 * so two names with an equal 32-bit hash can never share a prototype. */
static const WInstProto *winst_library_find(const WInstLibrary *library,
                                            const char *asset_name,
                                            WInstStats *stats) {
    char folded[28];
    uint32_t key = winst_name_key(asset_name, folded);
    if (library) for (int i = 0; i < library->count; i++) {
        const WInstProto *proto = &library->items[i];
        if (proto->name_hash == key && !strcmp(proto->name, folded)) return proto;
    }
    if (stats) stats->missing_models++;
    return NULL;
}

/* An object's authored matrix belongs to the prototype, never a final mesh.
 * It is only a later fallback candidate when this is the unique ROAD/TERRAIN
 * prototype for a requested asset. */
static int winst_proto_has_own_matrix(const WInstLibrary *library,
                                      const WInstProto *proto) {
    if (!library || !proto || !proto->has_matrix || proto->scene.count <= 0)
        return 0;
    for (int mesh = 0; mesh < proto->scene.count; mesh++) {
        int cat = proto->scene.meshes[mesh].cat;
        if (cat != N2_ROAD && cat != N2_TERRAIN) return 0;
    }
    for (int i = 0; i < library->count; i++)
        if (&library->items[i] != proto &&
            !strcmp(library->items[i].name, proto->name)) return 0;
    return 1;
}

static void winst_collect_model(WInstLibrary *library, const unsigned char *data,
                                long begin, long end,
                                const uint32_t *keys, int nkeys) {
    N2Scene local;
    memset(&local, 0, sizeof local);
    int cat = n2_mesh_category(data, begin, end);
    char name[40];
    n2_mesh_name(data, begin, end, name, sizeof name);
    int scen = n2_scen_class(name);
    float matrix[16];
    int has_matrix = n2_obj_matrix(data, begin, end, matrix);
    int is_vista = 0;
    if (n2_vista_family(name)) {
        N2Geom geometry;
        is_vista = n2_obj_geom(data, begin, end, matrix, &geometry) &&
                   n2_is_vista_impostor(name, &geometry);
    }
    uint32_t texkey = n2_mesh_texkey_cat(data, begin, end, cat, keys, nkeys);
    N2Leaf vtx[64], idx[64];
    int nv = 0, ni = 0;
    n2_find_leaves(data, begin, end, 0x00134B01u, vtx, &nv, 64);
    n2_find_leaves(data, begin, end, 0x00134B03u, idx, &ni, 64);
    int pairs = nv < ni ? nv : ni;

    /* This deliberately mirrors n2_walk_meshes' verified world-material
     * partition gate. A malformed partition falls through to the
     * identical whole-object path instead of silently losing geometry. */
    int sub_ok = 0;
    N2Sub sub[64];
    uint32_t slot[64];
    int nsub = 0;
    int nslot = n2_mesh_texslots(data, begin, end, slot, 64);
    if (cat != N2_GLOW && pairs == 1) {
        nsub = n2_mesh_submeshes(data, begin, end, sub, 64);
        if (nsub > 0 && nslot > 0) {
            const unsigned char *ib0 = data + idx[0].off;
            int ibytes = (int)idx[0].size, ip = 0;
            while (ip + 2 <= ibytes && ib0[ip] == 0x11 && ib0[ip + 1] == 0x11)
                ip += 2;
            long available = (ibytes - ip) / 2;
            long chain = 0;
            sub_ok = 1;
            for (int i = 0; i < nsub && sub_ok; i++) {
                if (sub[i].mat >= (uint32_t)nslot || !slot[sub[i].mat] ||
                    (long)sub[i].start != chain || sub[i].count < 3 ||
                    chain + (long)sub[i].count > available)
                    sub_ok = 0;
                else chain += (long)sub[i].count;
            }
            if (sub_ok && chain != available - available % 3) sub_ok = 0;
        }
    }
    if (sub_ok) {
        for (int i = 0; i < nsub; i++) {
            uint32_t authored = slot[sub[i].mat];
            /* SKYDOME resolves from shared LOC4 after prototype placement,
               not from this bundle's regional key inventory. Preserve the
               verified positional slot exactly, matching n2_walk_meshes. */
            uint32_t subkey = cat == N2_SKY
                            ? authored : n2_resolve_key(authored, keys, nkeys);
            int exact = subkey != 0;
            int before = local.count;
            n2_add_pair(data, vtx[0], idx[0], cat, &local, 24, 16, cat != N2_SKY,
                        subkey ? subkey : texkey, NULL,
                        (long)sub[i].start, (long)sub[i].count);
            for (int j = before; j < local.count; j++)
                local.meshes[j].mat_exact = (unsigned char)exact;
        }
    } else {
        int exact_single_slot = nslot == 1 && slot[0] != 0;
        for (int i = 0; i < pairs; i++) {
            int before = local.count;
            n2_add_pair(data, vtx[i], idx[i], cat, &local, 24, 16, cat != N2_SKY,
                        texkey, NULL, 0, -1);
            for (int j = before; j < local.count; j++)
                local.meshes[j].mat_exact = (unsigned char)exact_single_slot;
        }
    }
    for (int i = 0; i < local.count; i++) {
        local.meshes[i].scen = (unsigned char)scen;
        snprintf(local.meshes[i].sname, sizeof local.meshes[i].sname, "%.31s", name);
    }
    if (local.count && winst_library_add(library, &local, name, matrix,
                                         has_matrix, is_vista)) return;
    winst_free_scene(&local);
}

static void winst_collect_models(WInstLibrary *library, const unsigned char *data,
                                 long begin, long end,
                                 const uint32_t *keys, int nkeys) {
    if (!library || !data || begin < 0 || end < begin) return;
    for (long offset = begin; offset + 8 <= end;) {
        uint32_t magic = n2_u32(data + offset);
        uint32_t size = n2_u32(data + offset + 4);
        long payload = offset + 8;
        if ((uint64_t)size > (uint64_t)(end - payload)) return;
        long object_end = payload + (long)size;
        if (magic == 0x80134010u)
            winst_collect_model(library, data, payload, object_end, keys, nkeys);
        else if (magic != 0 && (magic >> 28) == 8)
            winst_collect_models(library, data, payload, object_end, keys, nkeys);
        offset = object_end;
    }
}

static int chunk_end(long off, uint32_t size, long limit, long *end) {
    if (off < 0 || off > limit || (long)size < 0) return 0;
    if ((uint64_t)size > (uint64_t)(limit - off)) return 0;
    *end = off + (long)size;
    return 1;
}

static uint16_t winst_u16(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static int16_t winst_s16(const unsigned char *p) {
    return (int16_t)winst_u16(p);
}

static float winst_f32(const unsigned char *p) {
    float v;
    memcpy(&v, p, sizeof v);
    return v;
}

static long skip_filler(const unsigned char *p, long len) {
    long n = 0;
    while (n < len && p[n] == 0x11) n++;
    return n;
}

static void free_region_array(WInstRegion *regions, int count) {
    if (!regions) return;
    for (int i = 0; i < count; i++) free(regions[i].xy);
    free(regions);
}

static int append_region(WInstRegion **regions, int *count, int *cap,
                         const unsigned char *record, int nxy) {
    if (*count == *cap) {
        int next = *cap ? *cap * 2 : 16;
        if (next <= *cap || (size_t)next > SIZE_MAX / sizeof **regions) return 0;
        WInstRegion *grown = (WInstRegion *)realloc(
            *regions, (size_t)next * sizeof **regions);
        if (!grown) return 0;
        *regions = grown;
        *cap = next;
    }

    WInstRegion *r = &(*regions)[*count];
    r->id = (int)winst_u16(record + 8);
    for (int i = 0; i < 4; i++) r->bb[i] = winst_f32(record + 0x0c + i * 4);
    r->nxy = nxy;
    r->xy = (float *)malloc((size_t)nxy * 2 * sizeof *r->xy);
    if (!r->xy) return 0;
    for (int i = 0; i < nxy * 2; i++) r->xy[i] = winst_f32(record + 0x24 + i * 4);
    (*count)++;
    return 1;
}

static int parse_polygon_payload(const unsigned char *data, long len,
                                 WInstRegion **regions, int *count, int *cap) {
    long pos = skip_filler(data, len);
    while (pos < len) {
        long remain = len - pos;
        if (remain < 0x24) return 0;
        const unsigned char *record = data + pos;
        int nxy = (int)winst_u16(record + 0x0a);
        if (nxy < 1 || nxy > 64) return 0;
        long record_size = 0x24 + (long)nxy * 8;
        if (record_size > remain) return 0;
        if (!append_region(regions, count, cap, record, nxy)) return 0;
        pos += record_size;
        pos += skip_filler(data + pos, len - pos);
    }
    return 1;
}

int winst_parse_regions(const unsigned char *data, long len,
                        WInstRegion **out, int *count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!data || len < 0 || !out || !count) return 0;

    WInstRegion *regions = NULL;
    int nregions = 0, cap = 0;
    long pos = 0;
    while (pos < len) {
        if (len - pos < 8) {
            free_region_array(regions, nregions);
            return 0;
        }
        uint32_t magic = n2_u32(data + pos);
        long payload_end = 0;
        if (!chunk_end(pos + 8, n2_u32(data + pos + 4), len, &payload_end)) {
            free_region_array(regions, nregions);
            return 0;
        }
        if (magic == 0x80034150u) {
            long child = pos + 8;
            while (child < payload_end) {
                if (payload_end - child < 8) {
                    free_region_array(regions, nregions);
                    return 0;
                }
                uint32_t child_magic = n2_u32(data + child);
                long child_end = 0;
                if (!chunk_end(child + 8, n2_u32(data + child + 4),
                               payload_end, &child_end)) {
                    free_region_array(regions, nregions);
                    return 0;
                }
                if (child_magic == 0x00034152u &&
                    !parse_polygon_payload(data + child + 8,
                                           child_end - (child + 8),
                                           &regions, &nregions, &cap)) {
                    free_region_array(regions, nregions);
                    return 0;
                }
                child = child_end;
            }
        }
        pos = payload_end;
    }

    if (nregions == 0) {
        free(regions);
        return 0;
    }
    *out = regions;
    *count = nregions;
    return nregions;
}

void winst_free_regions(WInstRegion *regions, int count) {
    if (count < 0) count = 0;
    free_region_array(regions, count);
}

static int point_on_segment(float x, float y,
                            float x0, float y0, float x1, float y1) {
    float cross = (x - x0) * (y1 - y0) - (y - y0) * (x1 - x0);
    float scale = fabsf(x1 - x0) + fabsf(y1 - y0) + 1.0f;
    if (fabsf(cross) > 1e-6f * scale) return 0;
    return x >= fminf(x0, x1) - 1e-6f && x <= fmaxf(x0, x1) + 1e-6f &&
           y >= fminf(y0, y1) - 1e-6f && y <= fmaxf(y0, y1) + 1e-6f;
}

static int in_polygon(const WInstRegion *r, float x, float y) {
    if (x < r->bb[0] || x > r->bb[2] || y < r->bb[1] || y > r->bb[3]) return 0;
    int inside = 0;
    for (int i = 0, j = r->nxy - 1; i < r->nxy; j = i++) {
        float xi = r->xy[2 * i], yi = r->xy[2 * i + 1];
        float xj = r->xy[2 * j], yj = r->xy[2 * j + 1];
        if (point_on_segment(x, y, xi, yi, xj, yj)) return 1;
        if ((yi > y) != (yj > y)) {
            float crossing = (xj - xi) * (y - yi) / (yj - yi) + xi;
            if (x < crossing) inside = !inside;
        }
    }
    return inside;
}

int winst_select_regions(const WInstRegion *regions, int count,
                         float x, float y, float radius,
                         unsigned char *selected, int selected_cap,
                         int *home_index) {
    if (home_index) *home_index = -1;
    if (!regions || count < 0 || !selected || selected_cap < 0 || radius < 0.0f)
        return 0;
    for (int i = 0; i < count; i++)
        if (regions[i].id < 0 || regions[i].id >= selected_cap) return 0;

    memset(selected, 0, (size_t)selected_cap);
    float radius2 = radius * radius;
    int nselected = 0;
    double home_area = 0.0;
    for (int i = 0; i < count; i++) {
        const WInstRegion *r = &regions[i];
        if (in_polygon(r, x, y) && home_index) {
            double twice_area = 0.0;
            for (int p = 0, q = r->nxy - 1; p < r->nxy; q = p++)
                twice_area += (double)r->xy[2 * q] * r->xy[2 * p + 1] -
                              (double)r->xy[2 * p] * r->xy[2 * q + 1];
            double area = fabs(twice_area);
            if (*home_index < 0 || area < home_area) {
                *home_index = i;
                home_area = area;
            }
        }

        float dx = x < r->bb[0] ? r->bb[0] - x : x > r->bb[2] ? x - r->bb[2] : 0.0f;
        float dy = y < r->bb[1] ? r->bb[1] - y : y > r->bb[3] ? y - r->bb[3] : 0.0f;
        if (dx * dx + dy * dy <= radius2) {
            selected[r->id] = 1;
            nselected++;
        }
    }
    return nselected;
}

int winst_decode_placement(const unsigned char *record, long len,
                           WInstPlacement *out) {
    if (!record || !out || len < 64) return 0;
    memset(out, 0, sizeof *out);
    out->type_index = winst_u16(record + 0x18);
    out->flags = winst_u16(record + 0x1a);
    for (int i = 0; i < 3; i++) {
        out->bounds_min[i] = winst_f32(record + i * 4);
        out->bounds_max[i] = winst_f32(record + 0x0c + i * 4);
    }
    for (int i = 0; i < 16; i++) out->matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            out->matrix[row * 4 + col] =
                (float)winst_s16(record + 0x2c + (row * 3 + col) * 2) / 8192.0f;
    out->matrix[12] = winst_f32(record + 0x20);
    out->matrix[13] = winst_f32(record + 0x24);
    out->matrix[14] = winst_f32(record + 0x28);
    return 1;
}

typedef struct {
    int region_id;
    const unsigned char *types;
    int type_count;
    const unsigned char *placements;
    int placement_count;
} WInstSection;

typedef int (*WInstInternalVisitFn)(const WInstPlacement *placement,
                                    const char *type_name, void *userdata);

static int winst_fixed_records(const unsigned char *data, long len, int stride,
                               const unsigned char **records, int *count) {
    if (!data || len < 0 || stride <= 0 || !records || !count) return 0;
    long filler = len % stride == 0 ? 0 : skip_filler(data, len);
    long body = len - filler;
    if (body < 0 || body % stride || body / stride > INT_MAX) return 0;
    *records = data + filler;
    *count = (int)(body / stride);
    return 1;
}

static int winst_parse_section(const unsigned char *data, long begin, long end,
                               WInstSection *section) {
    if (!data || !section || begin < 0 || end < begin) return 0;
    memset(section, 0, sizeof *section);
    section->region_id = -1;
    const unsigned char *info = NULL, *types = NULL, *placements = NULL;
    long info_len = 0, type_len = 0, placement_len = 0;
    for (long pos = begin; pos < end;) {
        if (end - pos < 8) return 0;
        uint32_t magic = n2_u32(data + pos);
        long child_end = 0;
        if (!chunk_end(pos + 8, n2_u32(data + pos + 4), end, &child_end)) return 0;
        if (magic == 0x00034101u) { info = data + pos + 8; info_len = child_end - pos - 8; }
        else if (magic == 0x00034102u) { types = data + pos + 8; type_len = child_end - pos - 8; }
        else if (magic == 0x00034103u) {
            placements = data + pos + 8;
            placement_len = child_end - pos - 8;
        }
        pos = child_end;
    }
    if (!info || info_len < 0x10 || !types || !placements) return 0;
    uint32_t region_id = n2_u32(info + 0x0c);
    if (region_id > INT_MAX) return 0;
    section->region_id = (int)region_id;
    if (!winst_fixed_records(types, type_len, 68,
                             &section->types, &section->type_count) ||
        !winst_fixed_records(placements, placement_len, 64,
                             &section->placements, &section->placement_count))
        return 0;
    return section->type_count > 0;
}

static int winst_bbox_in_range(const WInstPlacement *placement,
                               float x, float y, float radius) {
    if (!placement || !isfinite(x) || !isfinite(y) || !isfinite(radius) ||
        radius < 0.0f) return 0;
    for (int i = 0; i < 3; i++)
        if (!isfinite(placement->bounds_min[i]) ||
            !isfinite(placement->bounds_max[i]) ||
            placement->bounds_min[i] > placement->bounds_max[i]) return 0;
    float dx = x < placement->bounds_min[0] ? placement->bounds_min[0] - x
             : x > placement->bounds_max[0] ? x - placement->bounds_max[0] : 0.0f;
    float dy = y < placement->bounds_min[1] ? placement->bounds_min[1] - y
             : y > placement->bounds_max[1] ? y - placement->bounds_max[1] : 0.0f;
    return dx * dx + dy * dy <= radius * radius;
}

static int winst_visit_section(const WInstSection *section,
                               float focus_x, float focus_y, float view_radius,
                               WInstInternalVisitFn visit, void *userdata,
                               WInstStats *stats) {
    for (int i = 0; i < section->placement_count; i++) {
        WInstPlacement placement;
        const unsigned char *record = section->placements + (long)i * 64;
        if (stats) stats->instances_seen++;
        if (!winst_decode_placement(record, 64, &placement)) {
            winst_reject(stats);
            continue;
        }
        if (!winst_bbox_in_range(&placement, focus_x, focus_y, view_radius)) continue;
        if (stats) stats->instances_in_range++;
        if (placement.type_index >= (unsigned)section->type_count) {
            winst_reject(stats);
            continue;
        }
        char type_name[33];
        memcpy(type_name, section->types + (long)placement.type_index * 68, 32);
        type_name[32] = 0;
        if (visit && !visit(&placement, type_name, userdata)) return 0;
    }
    return 1;
}

typedef struct {
    const unsigned char *selected;
    int selected_cap;
    int find_region;
    int found_region;
    float focus_x, focus_y, view_radius;
    WInstInternalVisitFn visit;
    void *userdata;
    WInstStats *stats;
} WInstWalk;

static int winst_walk_sections(const unsigned char *data, long begin, long end,
                               WInstWalk *walk) {
    for (long pos = begin; pos < end;) {
        if (end - pos < 8) return 0;
        uint32_t magic = n2_u32(data + pos);
        long child_end = 0;
        if (!chunk_end(pos + 8, n2_u32(data + pos + 4), end, &child_end)) return 0;
        if (magic == 0x80034100u) {
            WInstSection section;
            if (!winst_parse_section(data, pos + 8, child_end, &section)) return 0;
            if (section.region_id == walk->find_region) walk->found_region = 1;
            int selected = !walk->selected ||
                (section.region_id >= 0 && section.region_id < walk->selected_cap &&
                 walk->selected[section.region_id]);
            if (selected && walk->visit &&
                !winst_visit_section(&section, walk->focus_x, walk->focus_y,
                                     walk->view_radius, walk->visit,
                                     walk->userdata, walk->stats)) return 0;
        } else if (magic != 0 && (magic >> 28) == 8) {
            if (!winst_walk_sections(data, pos + 8, child_end, walk)) return 0;
        }
        pos = child_end;
    }
    return 1;
}

#ifdef WORLD_INSTANCE_TESTING
int winst_test_collect_placements(const unsigned char *section_data,
                                  long section_len,
                                  float focus_x, float focus_y,
                                  float view_radius,
                                  WInstVisitFn visit, void *userdata,
                                  WInstStats *stats) {
    if (!section_data || section_len < 0 || !visit) return 0;
    WInstWalk walk;
    memset(&walk, 0, sizeof walk);
    walk.find_region = -1;
    walk.focus_x = focus_x;
    walk.focus_y = focus_y;
    walk.view_radius = view_radius;
    walk.visit = visit;
    walk.userdata = userdata;
    walk.stats = stats;
    return winst_walk_sections(section_data, 0, section_len, &walk);
}
#endif

typedef struct {
    const WInstLibrary *library;
    N2Scene *scene;
    N2Scene *vista;
    WInstStats *stats;
} WInstBuildVisit;

static int winst_build_visit(const WInstPlacement *placement,
                             const char *type_name, void *userdata) {
    WInstBuildVisit *build = (WInstBuildVisit *)userdata;
    const WInstProto *proto = winst_library_find(build->library, type_name,
                                                 build->stats);
    if (!proto) return 1;
    N2Scene *dst = proto->is_vista ? build->vista : build->scene;
    for (int i = 0; i < proto->scene.count; i++) {
        const N2Mesh *mesh = &proto->scene.meshes[i];
        if (mesh->cat == N2_ROAD || mesh->cat == N2_TERRAIN) continue;
        if (!winst_place_mesh(dst, mesh, placement->matrix, type_name,
                             build->stats)) return 0;
    }
    return 1;
}

static int winst_place_ground_prototypes(const WInstLibrary *library,
                                         N2Scene *scene, N2Scene *vista,
                                         WInstStats *stats) {
    static const float identity[16] = {
        1, 0, 0, 0, 0, 1, 0, 0,
        0, 0, 1, 0, 0, 0, 0, 1,
    };
    if (!library || !scene || !vista) return 0;
    for (int i = 0; i < library->count; i++) {
        const WInstProto *proto = &library->items[i];
        int own_matrix = winst_proto_has_own_matrix(library, proto);
        const float *matrix = own_matrix ? proto->matrix : identity;
        N2Scene *dst = proto->is_vista ? vista : scene;
        for (int mesh = 0; mesh < proto->scene.count; mesh++) {
            const N2Mesh *source = &proto->scene.meshes[mesh];
            if (source->cat != N2_ROAD && source->cat != N2_TERRAIN) continue;
            if (!winst_place_mesh(dst, source, matrix, proto->name, stats)) return 0;
            if (own_matrix && stats) stats->own_matrix_meshes++;
        }
    }
    return 1;
}

static int winst_move_regions(WInstRegion **all, int *count, int *cap,
                              WInstRegion *add, int nadd) {
    if (nadd < 0 || *count > INT_MAX - nadd) return 0;
    int need = *count + nadd;
    if (need > *cap) {
        int next = *cap ? *cap : 64;
        while (next < need) {
            if (next > INT_MAX / 2) { next = need; break; }
            next *= 2;
        }
        if ((size_t)next > SIZE_MAX / sizeof **all) return 0;
        WInstRegion *grown = (WInstRegion *)realloc(
            *all, (size_t)next * sizeof **all);
        if (!grown) return 0;
        *all = grown;
        *cap = next;
    }
    memcpy(*all + *count, add, (size_t)nadd * sizeof *add);
    *count = need;
    free(add);
    return 1;
}

static int winst_has_suffix(const char *text, const char *suffix) {
    size_t n = strlen(text), s = strlen(suffix);
    return n >= s && !strcmp(text + n - s, suffix);
}

static unsigned char *winst_read_named(const char *root, const char *name,
                                       long *len) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s%s", root, name,
             winst_has_suffix(name, ".BUN") ? "" : ".BUN");
    return n2_read_file(path, len);
}

static int winst_prepare_combined(const N2Scene *dst, const N2Scene *add,
                                  N2Mesh **combined, int *count) {
    if (!dst || !add || !combined || !count || dst->count < 0 || add->count < 0 ||
        dst->count > INT_MAX - add->count) return 0;
    *count = dst->count + add->count;
    *combined = NULL;
    if (!*count) return 1;
    if ((size_t)*count > SIZE_MAX / sizeof **combined) return 0;
    *combined = (N2Mesh *)malloc((size_t)*count * sizeof **combined);
    if (!*combined) return 0;
    if (dst->count)
        memcpy(*combined, dst->meshes, (size_t)dst->count * sizeof **combined);
    if (add->count)
        memcpy(*combined + dst->count, add->meshes,
               (size_t)add->count * sizeof **combined);
    return 1;
}

static int winst_commit_scenes(N2Scene *scene, N2Scene *vista,
                               N2Scene *add_scene, N2Scene *add_vista) {
    N2Mesh *combined_scene = NULL, *combined_vista = NULL;
    int scene_count = 0, vista_count = 0;
    if (!winst_prepare_combined(scene, add_scene, &combined_scene, &scene_count) ||
        !winst_prepare_combined(vista, add_vista, &combined_vista, &vista_count)) {
        free(combined_scene);
        free(combined_vista);
        return 0;
    }
    free(scene->meshes);
    free(vista->meshes);
    free(add_scene->meshes);
    free(add_vista->meshes);
    scene->meshes = combined_scene;
    scene->count = scene->cap = scene_count;
    vista->meshes = combined_vista;
    vista->count = vista->cap = vista_count;
    memset(add_scene, 0, sizeof *add_scene);
    memset(add_vista, 0, sizeof *add_vista);
    return 1;
}

int world_instance_build(N2Scene *scene, N2Scene *vista,
                         const char *track_root,
                         const char *const *bundles, int bundle_count,
                         float focus_x, float focus_y, float view_radius,
                         const unsigned char *shared, long shared_len,
                         WInstStats *stats) {
    WInstStats local_stats;
    WInstRegion *regions = NULL;
    int region_count = 0, region_cap = 0;
    unsigned char *selected = NULL, *bundle_data = NULL;
    long bundle_len = 0;
    WInstLibrary library;
    N2Scene built_scene, built_vista;
    int ok = 0;
    memset(&local_stats, 0, sizeof local_stats);
    local_stats.home_region = -1;
    memset(&library, 0, sizeof library);
    memset(&built_scene, 0, sizeof built_scene);
    memset(&built_vista, 0, sizeof built_vista);
    if (stats) *stats = local_stats;
    if (!scene || !vista || !track_root || !bundles || bundle_count <= 0 ||
        !isfinite(focus_x) || !isfinite(focus_y) || !isfinite(view_radius) ||
        view_radius < 0.0f || shared_len < 0) goto cleanup;

    for (int i = 0; i < bundle_count; i++) {
        const char *bundle = bundles[i];
        if (!bundle || !bundle[0] || !strcmp(bundle, "ALL")) goto cleanup;
        const char *stem = !strncmp(bundle, "STREAM", 6) ? bundle + 6 : bundle;
        char companion[64];
        snprintf(companion, sizeof companion, "%s", stem);
        char *dot = strrchr(companion, '.');
        if (dot && !strcmp(dot, ".BUN")) *dot = 0;
        long companion_len = 0;
        unsigned char *companion_data = winst_read_named(track_root, companion,
                                                         &companion_len);
        if (!companion_data) goto cleanup;
        WInstRegion *part = NULL;
        int npart = 0;
        int parsed = winst_parse_regions(companion_data, companion_len,
                                         &part, &npart);
        free(companion_data);
        if (!parsed || !winst_move_regions(&regions, &region_count, &region_cap,
                                           part, npart)) {
            winst_free_regions(part, npart);
            goto cleanup;
        }
    }

    selected = (unsigned char *)calloc(65536, 1);
    if (!selected) goto cleanup;
    int home_index = -1;
    int nselected = winst_select_regions(regions, region_count, focus_x, focus_y,
                                         view_radius, selected, 65536, &home_index);
    local_stats.regions_total = region_count;
    local_stats.regions_selected = nselected;
    if (home_index < 0 || home_index >= region_count) goto cleanup;
    local_stats.home_region = regions[home_index].id;

    for (int i = 0; i < bundle_count && !bundle_data; i++) {
        long candidate_len = 0;
        unsigned char *candidate = winst_read_named(track_root, bundles[i],
                                                     &candidate_len);
        if (!candidate) continue;
        WInstWalk find;
        memset(&find, 0, sizeof find);
        find.find_region = local_stats.home_region;
        int walked = winst_walk_sections(candidate, 0, candidate_len, &find);
        if (walked && find.found_region) {
            bundle_data = candidate;
            bundle_len = candidate_len;
            snprintf(local_stats.bundle, sizeof local_stats.bundle, "%s", bundles[i]);
        } else free(candidate);
    }
    /* The home section is the atomic gate: destination scenes are untouched
     * until a complete temporary assembly has been built and committed. */
    if (!bundle_data) goto cleanup;

    uint32_t *keys = (uint32_t *)malloc(16384 * sizeof *keys);
    if (!keys) goto cleanup;
    N2Tpk tpk = n2_tpk_open(bundle_data, bundle_len);
    int nkeys = n2_tpk_keys(bundle_data, tpk, keys, 16384);
    if (shared && nkeys < 16384)
        nkeys += n2_car_tex_keys(shared, shared_len, keys + nkeys, 16384 - nkeys);
    winst_collect_models(&library, bundle_data, 0, bundle_len, keys, nkeys);
    free(keys);

    if (!winst_place_ground_prototypes(&library, &built_scene, &built_vista,
                                       &local_stats)) goto cleanup;

    WInstBuildVisit visit = { &library, &built_scene, &built_vista, &local_stats };
    WInstWalk collect;
    memset(&collect, 0, sizeof collect);
    /* Companion polygons locate home and choose this one bundle, but are not a
     * complete ownership index. Scan its sections and filter each record by
     * authored bounds so nearby geometry from unmapped sections is retained. */
    collect.find_region = local_stats.home_region;
    collect.focus_x = focus_x;
    collect.focus_y = focus_y;
    collect.view_radius = view_radius;
    collect.visit = winst_build_visit;
    collect.userdata = &visit;
    collect.stats = &local_stats;
    if (!winst_walk_sections(bundle_data, 0, bundle_len, &collect) ||
        !collect.found_region) goto cleanup;
    if (!winst_commit_scenes(scene, vista, &built_scene, &built_vista)) goto cleanup;
    ok = 1;

cleanup:
    if (!ok) {
        winst_free_scene(&built_scene);
        winst_free_scene(&built_vista);
    }
    winst_library_free(&library);
    free(bundle_data);
    free(selected);
    winst_free_regions(regions, region_count);
    if (stats) *stats = local_stats;
    return ok;
}
