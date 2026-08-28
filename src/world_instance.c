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
} WInstProto;

typedef struct {
    WInstProto *items;
    int count, cap;
} WInstLibrary;

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
    placed.verts = (float *)malloc(nverts * 5 * sizeof *placed.verts);
    placed.idx = (uint16_t *)malloc(nidx * sizeof *placed.idx);
    placed.vcol = src->vcol ? (unsigned char *)malloc(nverts * 4) : NULL;
    if (!placed.verts || !placed.idx || (src->vcol && !placed.vcol)) {
        free(placed.verts);
        free(placed.idx);
        free(placed.vcol);
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
            free(placed.verts);
            free(placed.idx);
            free(placed.vcol);
            winst_reject(stats);
            return 0;
        }
    }
    placed.inst = 1;
    snprintf(placed.aname, sizeof placed.aname, "%.27s", asset_name ? asset_name : "");
    if (!winst_push_mesh(dst, placed)) {
        free(placed.verts);
        free(placed.idx);
        free(placed.vcol);
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
                             const char *name, const float matrix[16], int has_matrix) {
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
    if (!library || !proto || !proto->has_matrix || proto->scene.count != 1)
        return 0;
    int cat = proto->scene.meshes[0].cat;
    if (cat != N2_ROAD && cat != N2_TERRAIN) return 0;
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
    uint32_t texkey = n2_mesh_texkey_cat(data, begin, end, cat, keys, nkeys);
    N2Leaf vtx[64], idx[64];
    int nv = 0, ni = 0;
    n2_find_leaves(data, begin, end, 0x00134B01u, vtx, &nv, 64);
    n2_find_leaves(data, begin, end, 0x00134B03u, idx, &ni, 64);
    int pairs = nv < ni ? nv : ni;

    /* This deliberately mirrors n2_walk_meshes' verified ROAD/TERRAIN
     * material partition gate. A malformed partition falls through to the
     * identical whole-object path instead of silently losing geometry. */
    int sub_ok = 0;
    N2Sub sub[64];
    uint32_t slot[64];
    int nsub = 0, nslot = 0;
    if ((cat == N2_ROAD || cat == N2_TERRAIN) && pairs == 1) {
        nsub = n2_mesh_submeshes(data, begin, end, sub, 64);
        nslot = n2_mesh_texslots(data, begin, end, slot, 64);
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
            uint32_t subkey = n2_resolve_key(slot[sub[i].mat], keys, nkeys);
            n2_add_pair(data, vtx[0], idx[0], cat, &local, 24, 16, cat != N2_SKY,
                        subkey ? subkey : texkey, NULL,
                        (long)sub[i].start, (long)sub[i].count);
        }
    } else {
        for (int i = 0; i < pairs; i++)
            n2_add_pair(data, vtx[i], idx[i], cat, &local, 24, 16, cat != N2_SKY,
                        texkey, NULL, 0, -1);
    }
    for (int i = 0; i < local.count; i++) {
        local.meshes[i].scen = (unsigned char)scen;
        snprintf(local.meshes[i].sname, sizeof local.meshes[i].sname, "%.31s", name);
    }
    if (local.count && winst_library_add(library, &local, name, matrix, has_matrix)) return;
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
    for (int i = 0; i < count; i++) {
        const WInstRegion *r = &regions[i];
        if (in_polygon(r, x, y) && home_index && *home_index < 0) *home_index = i;

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
        float v = winst_f32(record + 0x20 + i * 4);
        out->bounds_min[i] = v;
        out->bounds_max[i] = v;
    }
    for (int i = 0; i < 16; i++) out->matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            out->matrix[col * 4 + row] =
                (float)winst_s16(record + 0x2c + (row * 3 + col) * 2) / 8192.0f;
    out->matrix[12] = out->bounds_min[0];
    out->matrix[13] = out->bounds_min[1];
    out->matrix[14] = out->bounds_min[2];
    return 1;
}
