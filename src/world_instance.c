#include "world_instance.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nfsu2.h"

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
