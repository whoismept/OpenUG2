/* map_bake.h -- high-resolution city map, baked from the loaded world.
 *
 * TRACKMAP4000 spends 512 texels on 5.6 km, so the 500 m minimap window shows
 * only 45 of them. This rebuilds the same map on the player's machine: land and
 * sea colours come from the retail image, road shapes from the road meshes the
 * engine already holds. Nothing is shipped. Results are kept in a directory of
 * our own inside the game folder; no game file is ever written.
 *
 * Header-only and GL-free, so tools/map_bake_test.c can drive it directly. */
#ifndef OPENUG2_MAP_BAKE_H
#define OPENUG2_MAP_BAKE_H

#include "nfsu2.h"
#include <zlib.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif


/* Paved surfaces the material rules class as terrain: train tracks, loading
 * bays and five unnamed records that the retail map draws as lines. The same
 * textures also cover a long unlit rail yard on the south coast, so these only
 * count where the retail map shows something (see MB_GATE_*). */
static int map_bake_is_secondary(uint32_t key) {
    static const uint32_t keys[] = { 0xeaef2b8du, 0xa5c78fa2u, 0xcebfd125u, 0x6c338031u,
                                     0x7aa659ffu, 0x9832553fu, 0xf91d2588u, 0xd1c98e50u };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) if (keys[i] == key) return 1;
    return 0;
}

#define MB_GATE_LOW   84    /* retail luminance: nothing drawn here */
#define MB_GATE_HIGH 112    /* retail luminance: clearly a drawn line */
#define MB_ROAD_GREY 205    /* one grey for every road */
#define MB_RACE_DIM  112    /* race map: roads off the route */
#define MB_RACE_LIT  242    /* race map: the route */
#define MB_RACE_LOW  120    /* retail luminance where a route begins; roads stop near 110 */
#define MB_RACE_HIGH 165
#define MB_RACE_BRIGHT 165  /* green channel of a route texel in the retail map */
#define MB_RACE_MIN_EXTENT 2000.0  /* metres: smaller race maps are already sharp */
#define MB_NAV_WIDTH 8.f    /* metres: stroke bridging a break in the road */
#define MB_NAV_LIMIT 150.f  /* metres: longer links are not one road segment */
#define MB_NAV_SAMPLE 4.f   /* metres: spacing of the on-road checks along a link */
#define MB_DRIVE_REACH 20.f  /* metres: a mesh this far from any link needs retail support */
#define MB_STROKE_CONTRAST 45 /* grey above the land: the retail map drew a line here */
#define MB_RACE_CONTRAST 40   /* the same, for the dimmer roads of a race map */
#define MB_FAR_CONTRAST 20    /* the same, beyond the loaded bundle: no mesh can compete */
#define MB_RACE_GATE_LOW  64  /* race map versions of MB_GATE_*: roads sit near 95 */
#define MB_RACE_GATE_HIGH 84
#define MB_SEAM_AREA  320   /* texels at 4096: about 600 square metres */
#define MB_SEAM_WIDTH 4.f   /* texels at 4096: about 5.5 metres */
#define MB_EDGE_LOW   90    /* road antialiasing span, on the filtered mask */
#define MB_EDGE_HIGH 165
#define MB_CASING    0.4f   /* land under a road's dark casing */
#define MB_SHADOW_REACH 5   /* texels of the 512 map: the retail shadow around roads */
#define MB_ROUTE_WIDTH 18.f /* metres: route stroke from matched links, clipped to the road */
#define MB_LINE_WIDTH  26.f /* metres: the same from a racing line, which wanders across it */
#define MB_LINE_SPACING 80.f /* metres: a sparser route is only gates and would cut corners */
#define MB_ROUTE_SHARE 0.7f /* share of a link the retail map must draw brightly */
#define MB_ZONES 4
/* Flat tones of the retail maps: sea, low ground, mid ground, hills. */
static const unsigned char mb_palette[MB_ZONES][3] = {
    { 31, 53, 66 }, { 44, 48, 45 }, { 57, 65, 54 }, { 78, 86, 74 }
};

static float mb_smooth(float lo, float hi, float x) {
    float t = (x - lo) / (hi - lo);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* Scratch for the plane filters: one plane plus one running value per column.
 * The vertical passes sweep whole rows; walking a column at a time misses the
 * cache on every texel and took 30 s for a 4096 map. */
typedef struct { unsigned char *plane; int *column; } MbWork;

/* One box-filter pass, horizontal then vertical, on an s*s plane. */
static void mb_box(unsigned char *p, const MbWork *work, int s, int r) {
    int win = 2 * r + 1;
    unsigned char *out = work->plane;
    for (int y = 0; y < s; y++) {
        const unsigned char *in = p + (long)y * s;
        int sum = in[0] * (r + 1);
        for (int i = 1; i <= r; i++) sum += in[i < s ? i : s - 1];
        unsigned char *row = out + (long)y * s;
        for (int x = 0; x < s; x++) {
            row[x] = (unsigned char)(sum / win);
            sum += in[x + r + 1 < s ? x + r + 1 : s - 1] - in[x - r > 0 ? x - r : 0];
        }
    }
    int *col = work->column;
    for (int x = 0; x < s; x++) col[x] = out[x] * (r + 1);
    for (int i = 1; i <= r; i++) {
        const unsigned char *row = out + (long)(i < s ? i : s - 1) * s;
        for (int x = 0; x < s; x++) col[x] += row[x];
    }
    for (int y = 0; y < s; y++) {
        unsigned char *dst = p + (long)y * s;
        const unsigned char *add = out + (long)(y + r + 1 < s ? y + r + 1 : s - 1) * s;
        const unsigned char *sub = out + (long)(y - r > 0 ? y - r : 0) * s;
        for (int x = 0; x < s; x++) {
            dst[x] = (unsigned char)(col[x] / win);
            col[x] += add[x] - sub[x];
        }
    }
}

/* Square dilation: any set texel within r lights the whole window. */
static void mb_dilate(unsigned char *p, const MbWork *work, int s, int r) {
    unsigned char *out = work->plane;
    for (int y = 0; y < s; y++) {
        const unsigned char *in = p + (long)y * s;
        unsigned char *row = out + (long)y * s;
        int count = 0;
        for (int i = 0; i <= r && i < s; i++) count += in[i] != 0;
        for (int x = 0; x < s; x++) {
            row[x] = count ? 255 : 0;
            if (x + r + 1 < s) count += in[x + r + 1] != 0;
            if (x - r >= 0) count -= in[x - r] != 0;
        }
    }
    int *col = work->column;
    memset(col, 0, (size_t)s * sizeof *col);
    for (int i = 0; i <= r && i < s; i++) {
        const unsigned char *row = out + (long)i * s;
        for (int x = 0; x < s; x++) col[x] += row[x] != 0;
    }
    for (int y = 0; y < s; y++) {
        unsigned char *dst = p + (long)y * s;
        for (int x = 0; x < s; x++) dst[x] = col[x] ? 255 : 0;
        if (y + r + 1 < s) { const unsigned char *a = out + (long)(y + r + 1) * s;
            for (int x = 0; x < s; x++) col[x] += a[x] != 0; }
        if (y - r >= 0) { const unsigned char *b = out + (long)(y - r) * s;
            for (int x = 0; x < s; x++) col[x] -= b[x] != 0; }
    }
}

/* Grey erosion (sign -1) or dilation (+1) with a disc, on a small w*h plane. */
static void mb_morph(const unsigned char *in, unsigned char *out, int w, int h, int r, int sign) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int best = in[(long)y * w + x];
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    int qx = x + dx, qy = y + dy;
                    if (dx * dx + dy * dy > r * r || qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                    int v = in[(long)qy * w + qx];
                    if (sign > 0 ? v > best : v < best) best = v;
                }
            out[(long)y * w + x] = (unsigned char)best;
        }
}

/* Bilinear resize, in 8.8 fixed point; src may be larger or smaller than dst. */
static void mb_upscale(const unsigned char *in, int w, int h, unsigned char *out, int s) {
    for (int y = 0; y < s; y++) {
        int fy = (int)(((2L * y + 1) * h * 128) / s) - 128;
        int y0 = fy < 0 ? 0 : fy >> 8, y1 = y0 + 1 < h ? y0 + 1 : h - 1, ty = fy < 0 ? 0 : fy & 255;
        const unsigned char *a = in + (long)y0 * w, *b = in + (long)y1 * w;
        unsigned char *row = out + (long)y * s;
        long step = ((long)w << 8) / s, fx = step / 2 - 128;
        for (int x = 0; x < s; x++, fx += step) {
            int x0 = fx < 0 ? 0 : (int)(fx >> 8), x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            int tx = fx < 0 ? 0 : (int)(fx & 255);
            int top = a[x0] * (256 - tx) + a[x1] * tx, low = b[x0] * (256 - tx) + b[x1] * tx;
            row[x] = (unsigned char)((top * (256 - ty) + low * ty + 32768) >> 16);
        }
    }
}

/* Area average down to sm*sm, for planes about to be blurred anyway. */
static void mb_shrink(const unsigned char *in, int s, unsigned char *out, int sm) {
    int f = s / sm;
    for (int y = 0; y < sm; y++)
        for (int x = 0; x < sm; x++) {
            int sum = 0;
            for (int j = 0; j < f; j++) {
                const unsigned char *row = in + (long)(y * f + j) * s + x * f;
                for (int i = 0; i < f; i++) sum += row[i];
            }
            out[(long)y * sm + x] = (unsigned char)(sum / (f * f));
        }
}

static void mb_triangle(unsigned char *mask, int s, const float *a, const float *b, const float *c) {
    float area = (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]);
    if (fabsf(area) < 1e-6f) return;
    float lox = fminf(a[0], fminf(b[0], c[0])), hix = fmaxf(a[0], fmaxf(b[0], c[0]));
    float loy = fminf(a[1], fminf(b[1], c[1])), hiy = fmaxf(a[1], fmaxf(b[1], c[1]));
    int x0 = lox < 0 ? 0 : (int)lox, x1 = hix >= s - 1 ? s - 1 : (int)hix + 1;
    int y0 = loy < 0 ? 0 : (int)loy, y1 = hiy >= s - 1 ? s - 1 : (int)hiy + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float px = x + .5f, py = y + .5f;
            float w0 = ((b[0]-px)*(c[1]-py) - (b[1]-py)*(c[0]-px)) / area;
            float w1 = ((c[0]-px)*(a[1]-py) - (c[1]-py)*(a[0]-px)) / area;
            if (w0 >= -.02f && w1 >= -.02f && 1 - w0 - w1 >= -.02f) mask[(long)y * s + x] = 255;
        }
}

/* True when a world point lies on the road layer, or within r texels of it. */
static int mb_on_road(const unsigned char *reach, int s, float x, float y, const double origin[3]) {
    int px = (int)((x - origin[0]) / origin[2] * s), py = (int)((origin[1] - y) / origin[2] * s);
    return px >= 0 && py >= 0 && px < s && py < s && reach[(long)py * s + px] != 0;
}

static void mb_stroke(unsigned char *plane, int s, const double origin[3],
                      float ax, float ay, float bx, float by, float half) {
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    float nx = -dy / len * half, ny = dx / len * half;
    float world[4][2] = { { ax + nx, ay + ny }, { bx + nx, by + ny },
                          { bx - nx, by - ny }, { ax - nx, ay - ny } };
    float p[4][2];
    for (int c = 0; c < 4; c++) {
        p[c][0] = (float)((world[c][0] - origin[0]) / origin[2] * s);
        p[c][1] = (float)((origin[1] - world[c][1]) / origin[2] * s);
    }
    mb_triangle(plane, s, p[0], p[1], p[2]);
    mb_triangle(plane, s, p[0], p[2], p[3]);
}

/* Material names miss paved surfaces such as gravel lanes (5.6% of L4RA's nodes
 * sit on them), which broke roads apart. A link is stroked only where the mesh
 * layer has no road: drawing it everywhere swallowed the real road shapes. reach
 * is the road layer grown by a few metres. nav is x,y per node; start and list
 * are the routing graph's adjacency, which includes the welds at junctions. */
static void map_bake_bridge_nav(const float *nav, int nnav, const int *start, const int *list,
                                const unsigned char *reach, const double origin[3],
                                int s, unsigned char *road) {
    float half = MB_NAV_WIDTH * .5f;
    for (int a = 0; a < nnav; a++)
        for (int k = start[a]; k < start[a + 1]; k++) {
            int b = list[k];
            if (b <= a || b >= nnav) continue;
            float ax = nav[a * 2], ay = nav[a * 2 + 1], bx = nav[b * 2], by = nav[b * 2 + 1];
            float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
            if (len < 1e-3f || len > MB_NAV_LIMIT) continue;
            int steps = (int)(len / MB_NAV_SAMPLE) + 1, broken = 0;
            for (int i = 0; i <= steps && !broken; i++) {
                float t = (float)i / steps;
                broken = !mb_on_road(reach, s, ax + dx * t, ay + dy * t, origin);
            }
            if (!broken) continue;
            mb_stroke(road, s, origin, ax, ay, bx, by, half);
        }
}

/* Everything within MB_DRIVE_REACH of a drivable link. */
static void map_bake_drive_plane(const float *nav, int nnav, const int *start, const int *list,
                                 const double origin[3], int s, unsigned char *drive) {
    for (int a = 0; a < nnav; a++)
        for (int k = start[a]; k < start[a + 1]; k++) {
            int b = list[k];
            if (b <= a || b >= nnav) continue;
            float dx = nav[b * 2] - nav[a * 2], dy = nav[b * 2 + 1] - nav[a * 2 + 1];
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1e-3f || len > MB_NAV_LIMIT) continue;
            mb_stroke(drive, s, origin, nav[a * 2], nav[a * 2 + 1], nav[b * 2], nav[b * 2 + 1],
                      MB_DRIVE_REACH);
        }
}

/* Fill the seams left between adjoining road meshes. A hole goes only if it is
 * both small and thin, so islands at junctions and medians keep their shape;
 * width comes from area over perimeter. Thresholds are in texels of this map. */
static void mb_fill_seams(unsigned char *mask, int s, long max_area, float max_width) {
    long n = (long)s * s;
    int *queue = (int *)malloc((size_t)n * sizeof(int));
    unsigned char *seen = (unsigned char *)calloc((size_t)n, 1);
    if (queue && seen)
        for (long i = 0; i < n; i++) {
            if (seen[i] || mask[i] > 127) continue;
            long head = 0, tail = 0, edge = 0;
            int border = 0;
            queue[tail++] = (int)i; seen[i] = 1;
            while (head < tail) {
                int p = queue[head++], x = p % s, y = p / s;
                int next[4] = { x > 0 ? p - 1 : -1, x + 1 < s ? p + 1 : -1,
                                y > 0 ? p - s : -1, y + 1 < s ? p + s : -1 };
                for (int k = 0; k < 4; k++) {
                    int q = next[k];
                    if (q < 0) { border = 1; continue; }
                    if (mask[q] > 127) { edge++; continue; }
                    if (!seen[q]) { seen[q] = 1; queue[tail++] = q; }
                }
            }
            if (!border && tail <= max_area && edge && 2.f * tail / edge <= max_width)
                for (long k = 0; k < tail; k++) mask[queue[k]] = 255;
        }
    free(queue); free(seen);
}

/* origin is TrackInfo's {west, north, extent}: the same frame ha_map samples. */
static long map_bake_raster(const N2Scene *scene, const double origin[3], int s,
                            unsigned char *road, unsigned char *second) {
    long triangles = 0;
    for (int i = 0; i < scene->count; i++) {
        const N2Mesh *m = &scene->meshes[i];
        unsigned char *mask = m->cat == N2_ROAD ? road
            : m->cat == N2_TERRAIN && map_bake_is_secondary(m->texkey) ? second : NULL;
        if (!mask) continue;
        for (int t = 0; t + 2 < m->nidx; t += 3) {
            float p[3][2];
            for (int k = 0; k < 3; k++) {
                const float *v = m->verts + (size_t)m->idx[t + k] * 5;
                p[k][0] = (float)((v[0] - origin[0]) / origin[2] * s);
                p[k][1] = (float)((origin[1] - v[1]) / origin[2] * s);
            }
            mb_triangle(mask, s, p[0], p[1], p[2]);
            triangles++;
        }
    }
    return triangles;
}

/* Land and sea as flat blocks. Each retail texel takes its nearest palette tone;
 * texels under a road, its dark shadow or a bright stroke say nothing about the
 * ground, so they take the nearest trustworthy zone instead. Zones are then
 * smoothed at 1024 so their borders are clean curves. zone gets s*s class ids. */
static int mb_land_zones(const unsigned char *src, int w, int h, const unsigned char *core,
                         int s, const MbWork *full, unsigned char *zone) {
    long np = (long)w * h, n = (long)s * s;
    int sm = s > 1024 ? 1024 : s, grid = sm / 1024 > 0 ? sm / 1024 : 1;
    long nm = (long)sm * sm;
    unsigned char *mask = (unsigned char *)malloc((size_t)np);
    unsigned char *label = (unsigned char *)malloc((size_t)np);
    unsigned char *plane = (unsigned char *)malloc((size_t)np);
    unsigned char *soft = (unsigned char *)malloc((size_t)nm);
    unsigned char *best = (unsigned char *)calloc((size_t)n, 1);
    int *queue = (int *)malloc((size_t)np * sizeof(int));
    MbWork small = { (unsigned char *)malloc((size_t)(np > nm ? np : nm)),
                     (int *)malloc((size_t)(w > sm ? w : sm) * sizeof(int)) };
    int ok = mask && label && plane && soft && best && queue && small.plane && small.column;
    if (ok) {
        mb_upscale(core, s, s, mask, w);
        for (long i = 0; i < np; i++) mask[i] = mask[i] > 20 ? 255 : 0;
        mb_dilate(mask, &small, w, MB_SHADOW_REACH * w / 512);
        long head = 0, tail = 0;
        for (long i = 0; i < np; i++) {
            const unsigned char *p = src + i * 4;
            int bright = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
            label[i] = 255;
            if (mask[i] || bright > 100) continue;
            long nearest = -1;
            for (int k = 0; k < MB_ZONES; k++) {
                long dr = p[0] - mb_palette[k][0], dg = p[1] - mb_palette[k][1], db = p[2] - mb_palette[k][2];
                long d = dr * dr + dg * dg + db * db;
                if (nearest < 0 || d < nearest) { nearest = d; label[i] = (unsigned char)k; }
            }
            queue[tail++] = (int)i;
        }
        if (!tail) memset(label, 1, (size_t)np);
        while (head < tail) {
            int p = queue[head++], x = p % w, y = p / w;
            int next[4] = { x > 0 ? p - 1 : -1, x + 1 < w ? p + 1 : -1,
                            y > 0 ? p - w : -1, y + 1 < h ? p + w : -1 };
            for (int k = 0; k < 4; k++)
                if (next[k] >= 0 && label[next[k]] == 255) { label[next[k]] = label[p]; queue[tail++] = next[k]; }
        }
        for (int k = 0; k < MB_ZONES; k++) {
            for (long i = 0; i < np; i++) plane[i] = label[i] == k ? 255 : 0;
            mb_upscale(plane, w, h, soft, sm);
            for (int pass = 0; pass < 2; pass++) mb_box(soft, &small, sm, grid * 2);
            mb_upscale(soft, sm, sm, full->plane, s);
            for (long i = 0; i < n; i++)
                if (full->plane[i] > best[i]) { best[i] = full->plane[i]; zone[i] = (unsigned char)k; }
        }
    }
    free(mask); free(label); free(plane); free(soft); free(best); free(queue);
    free(small.plane); free(small.column);
    return ok;
}

/* Writes s*s*3 bytes. src is the retail map as w*h RGBA. drive marks texels near
 * a drivable link, or is NULL to keep every mesh. race draws the event's dim
 * roads and bright route; links marks the route, or is NULL to read it from src.
 * Returns 0 when out of memory. */
static int map_bake_compose(const unsigned char *src, int w, int h, const unsigned char *road,
                            const unsigned char *second, const unsigned char *drive,
                            const unsigned char *links, int s, int race, unsigned char *out) {
    long n = (long)s * s, np = (long)w * h;
    /* Smooth planes are filtered at 1024 and enlarged. Radii are in those texels,
       so the look does not change between detail levels. */
    int sm = s > 1024 ? 1024 : s, grid = sm / 1024 > 0 ? sm / 1024 : 1;
    long nm = (long)sm * sm;
    MbWork tmp = { (unsigned char *)malloc((size_t)nm), (int *)malloc((size_t)sm * sizeof(int)) };
    MbWork full = { (unsigned char *)malloc((size_t)n), (int *)malloc((size_t)s * sizeof(int)) };
    unsigned char *small = (unsigned char *)malloc((size_t)np * 2);
    unsigned char *soft = (unsigned char *)malloc((size_t)nm);
    unsigned char *lum = (unsigned char *)malloc((size_t)n);
    unsigned char *retail = (unsigned char *)malloc((size_t)n);
    unsigned char *ground = (unsigned char *)malloc((size_t)n);
    unsigned char *core = (unsigned char *)malloc((size_t)n);
    unsigned char *added = (unsigned char *)calloc((size_t)n, 1);
    unsigned char *seeds = (unsigned char *)malloc((size_t)n);
    unsigned char *stroke = (unsigned char *)malloc((size_t)n);
    unsigned char *casing = (unsigned char *)malloc((size_t)n);
    unsigned char *zone = (unsigned char *)calloc((size_t)n, 1);
    int ok = tmp.plane && tmp.column && full.plane && full.column && small && soft && lum &&
             retail && ground && core && added && seeds && stroke && casing && zone;
    if (ok) {
        unsigned char *a = small, *b = small + (size_t)np;
        for (long i = 0; i < np; i++)
            a[i] = (unsigned char)((src[i*4] * 77 + src[i*4+1] * 150 + src[i*4+2] * 29) >> 8);
        mb_upscale(a, w, h, retail, s);

        /* What the ground would be without strokes: a grey opening. */
        mb_morph(a, b, w, h, 5, -1);
        mb_morph(b, a, w, h, 5, 1);
        mb_upscale(a, w, h, ground, s);

        /* How brightly the retail map draws each spot. Dilated so a thin line
           lights its full geometric width. */
        for (long i = 0; i < np; i++)
            a[i] = (unsigned char)((src[i*4] * 77 + src[i*4+1] * 150 + src[i*4+2] * 29) >> 8);
        mb_morph(a, b, w, h, 2, 1);
        mb_upscale(b, w, h, soft, sm);
        for (int pass = 0; pass < 2; pass++) mb_box(soft, &tmp, sm, grid * 2);
        mb_upscale(soft, sm, sm, lum, s);

        int gate_low = race ? MB_RACE_GATE_LOW : MB_GATE_LOW;
        int gate_high = race ? MB_RACE_GATE_HIGH : MB_GATE_HIGH;
        for (long i = 0; i < n; i++) {
            float gated = second[i] * mb_smooth((float)gate_low, (float)gate_high, lum[i]);
            int mesh = road[i];
            /* Stubs the game keeps but neither the retail map nor traffic uses. */
            if (drive && !drive[i] && lum[i] < gate_low) mesh = 0;
            core[i] = (mesh > gated ? mesh : gated) > 127 ? 255 : 0;
        }

        /* The reverse case: the retail map draws a road with no mesh under it.
           Strokes run wider than the mesh, so anything near a road is only that
           road's outline. Seeds start well clear of it and regrow inside the
           stroke until they touch the road they continue. Lowering clear thickens
           every road. */
        {
            int clear = s * 12 / 4096 > 0 ? s * 12 / 4096 : 1;   /* about 16 m */
            int contrast = race ? MB_RACE_CONTRAST : MB_STROKE_CONTRAST;
            /* A bundle holds only its own roads, but every map shows the whole
               city. Far from any link the retail strokes are the only roads. */
            if (drive) {
                memcpy(casing, drive, (size_t)n);
                mb_dilate(casing, &full, s, s * 45 / 4096 > 0 ? s * 45 / 4096 : 1);   /* about 60 m */
            }
            for (long i = 0; i < n; i++) {
                int need = drive && !casing[i] ? MB_FAR_CONTRAST : contrast;
                stroke[i] = retail[i] - ground[i] > need ? 255 : 0;
                seeds[i] = core[i];
            }
            mb_dilate(seeds, &full, s, clear);
            for (long i = 0; i < n; i++) seeds[i] = stroke[i] && !seeds[i] ? 255 : 0;
            mb_dilate(seeds, &full, s, clear + clear / 2 + 1);
            for (long i = 0; i < n; i++) if (seeds[i] && stroke[i] && !core[i]) { core[i] = 255; added[i] = 1; }
        }
        {
            float scale = s / 4096.f;
            mb_fill_seams(core, s, (long)(MB_SEAM_AREA * scale * scale) + 1, MB_SEAM_WIDTH * scale);
        }

        ok = mb_land_zones(src, w, h, core, s, &full, zone);

        /* Sharp roads on a thin dark casing, antialiased over about a texel. */
        memcpy(casing, core, (size_t)n);
        mb_dilate(casing, &full, s, s * 3 / 4096 > 0 ? s * 3 / 4096 : 1);
        for (int pass = 0; pass < 2; pass++) mb_box(core, &full, s, 1);

        for (long i = 0; ok && i < n; i++) {
            float edge = mb_smooth(MB_EDGE_LOW, MB_EDGE_HIGH, core[i]);
            float lit = 1;
            if (race) {
                if (links && !added[i]) lit = mb_smooth(MB_EDGE_LOW, MB_EDGE_HIGH, links[i]);
                else lit = mb_smooth(MB_RACE_LOW, MB_RACE_HIGH, retail[i]);
            }
            float grey = race ? MB_RACE_DIM + (MB_RACE_LIT - MB_RACE_DIM) * lit : MB_ROAD_GREY;
            const unsigned char *land = mb_palette[zone[i]];
            for (int c = 0; c < 3; c++) {
                float v = land[c] * (casing[i] ? MB_CASING : 1.f);
                v += (grey - v) * edge;
                out[i * 3 + c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + .5f);
            }
        }
    }
    free(tmp.plane); free(tmp.column); free(full.plane); free(full.column); free(small); free(soft);
    free(lum); free(retail); free(ground); free(core); free(added); free(seeds); free(stroke);
    free(casing); free(zone);
    return ok;
}

/* The part of a bake that reads the world, for one map frame and size. Built on
 * the thread that owns the scene. Afterwards it needs nothing from the scene, so
 * map_bake_from_layers can run on a worker while the world streams. */
typedef struct {
    unsigned char *road, *second, *drive;   /* drive is NULL without a nav graph */
    float *links;                           /* x0,y0,x1,y1 per link, copied from the graph */
    long nlinks;
    double origin[3];
    int size;
} MbLayers;

static void map_bake_layers_free(MbLayers *l) {
    free(l->road); free(l->second); free(l->drive); free(l->links);
    memset(l, 0, sizeof *l);
}

static int map_bake_layers_match(const MbLayers *l, const double origin[3], int s) {
    return l->road && l->size == s && fabs(l->origin[0] - origin[0]) < .01 &&
           fabs(l->origin[1] - origin[1]) < .01 && fabs(l->origin[2] - origin[2]) < .01;
}

/* Returns 0 when the scene holds no road (a bundle this frame does not cover) or
 * memory ran out; l is then empty. */
static int map_bake_layers(MbLayers *l, const N2Scene *scene, const float *nav, int nnav,
                           const int *start, const int *list, const double origin[3], int s) {
    long n = (long)s * s;
    MbWork work = { (unsigned char *)malloc((size_t)n), (int *)malloc((size_t)s * sizeof(int)) };
    unsigned char *reach = (unsigned char *)malloc((size_t)n);
    int linked = nav && start && list;
    memset(l, 0, sizeof *l);
    l->road = (unsigned char *)calloc((size_t)n, 1);
    l->second = (unsigned char *)calloc((size_t)n, 1);
    if (linked) l->drive = (unsigned char *)calloc((size_t)n, 1);
    int ok = work.plane && work.column && reach && l->road && l->second && (!linked || l->drive) &&
             map_bake_raster(scene, origin, s, l->road, l->second) > 0;
    if (ok && linked) {
        memcpy(reach, l->road, (size_t)n);
        mb_dilate(reach, &work, s, s * 4 / 4096 > 0 ? s * 4 / 4096 : 1);   /* about 5 m */
        map_bake_bridge_nav(nav, nnav, start, list, reach, origin, s, l->road);
        map_bake_drive_plane(nav, nnav, start, list, origin, s, l->drive);
        l->links = (float *)malloc((size_t)start[nnav] * 4 * sizeof(float));
        ok = l->links != NULL;
        for (int a = 0; ok && a < nnav; a++)
            for (int k = start[a]; k < start[a + 1]; k++) {
                int b = list[k];
                if (b <= a || b >= nnav) continue;
                float *e = l->links + l->nlinks++ * 4;
                e[0] = nav[a * 2]; e[1] = nav[a * 2 + 1]; e[2] = nav[b * 2]; e[3] = nav[b * 2 + 1];
            }
    }
    free(work.plane); free(work.column); free(reach);
    if (!ok) { map_bake_layers_free(l); return 0; }
    memcpy(l->origin, origin, sizeof l->origin);
    l->size = s;
    return 1;
}

/* The route from the engine's racing line, the exact answer when there is one.
 * Returns the plane, or NULL when the line is missing or too sparse to follow. */
static unsigned char *mb_route_line(const MbLayers *l, const float *line, int count, int loop) {
    if (!line || count < 2) return NULL;
    float total = 0;
    int segments = loop ? count : count - 1;
    for (int i = 0; i + 1 < count; i++)
        total += hypotf(line[i*2+2] - line[i*2], line[i*2+3] - line[i*2+1]);
    if (total / (count - 1) > MB_LINE_SPACING) return NULL;
    unsigned char *plane = (unsigned char *)calloc((size_t)l->size, (size_t)l->size);
    if (!plane) return NULL;
    for (int i = 0; i < segments; i++) {
        int j = (i + 1) % count;
        if (hypotf(line[j*2] - line[i*2], line[j*2+1] - line[i*2+1]) < 1e-3f) continue;
        mb_stroke(plane, l->size, l->origin, line[i*2], line[i*2+1], line[j*2], line[j*2+1], MB_LINE_WIDTH * .5f);
    }
    /* Square ends leave notches where segments turn: close them, then soften
       the side of the route that runs along the inside of a wide road. */
    MbWork work = { (unsigned char *)malloc((size_t)l->size * l->size), (int *)malloc((size_t)l->size * sizeof(int)) };
    if (work.plane && work.column) {
        long n = (long)l->size * l->size;
        int r = l->size * 8 / 4096 > 0 ? l->size * 8 / 4096 : 1;   /* about 11 m */
        mb_dilate(plane, &work, l->size, r);
        for (long i = 0; i < n; i++) plane[i] = (unsigned char)~plane[i];
        mb_dilate(plane, &work, l->size, r);
        for (long i = 0; i < n; i++) plane[i] = (unsigned char)~plane[i];
        for (int pass = 0; pass < 2; pass++) mb_box(plane, &work, l->size, 1);
    }
    free(work.plane); free(work.column);
    return plane;
}

/* The route as real road links: a link counts when the race map draws most of it
 * brightly. This is the fallback for a race with no racing line; a wider match
 * bleeds onto neighbouring roads at interchanges. Returns the plane, or NULL if no
 * link matched. Closing bridges the short junction links between two matched ones. */
static unsigned char *mb_route_links(const MbLayers *l, const unsigned char *src, int w, int h) {
    int s = l->size;
    long n = (long)s * s, hits = 0;
    unsigned char *plane = (unsigned char *)calloc((size_t)n, 1);
    MbWork work = { (unsigned char *)malloc((size_t)n), (int *)malloc((size_t)s * sizeof(int)) };
    if (!plane || !work.plane || !work.column || !l->nlinks) {
        free(plane); free(work.plane); free(work.column);
        return NULL;
    }
    for (long e = 0; e < l->nlinks; e++) {
        const float *p = l->links + e * 4;
        float dx = p[2] - p[0], dy = p[3] - p[1], len = sqrtf(dx * dx + dy * dy);
        if (len < .1f || len > MB_NAV_LIMIT) continue;
        int on = 0, total = 0;
        for (int i = 0; i < 5; i++) {
            float t = (i + .5f) / 5;
            int px = (int)((p[0] + dx * t - l->origin[0]) / l->origin[2] * w);
            int py = (int)((l->origin[1] - (p[1] + dy * t)) / l->origin[2] * h);
            if (px < 1 || py < 1 || px >= w - 1 || py >= h - 1) continue;
            int peak = 0;
            for (int qy = -1; qy <= 1; qy++)
                for (int qx = -1; qx <= 1; qx++) {
                    int v = src[((long)(py + qy) * w + px + qx) * 4 + 1];
                    if (v > peak) peak = v;
                }
            total++;
            on += peak > MB_RACE_BRIGHT;
        }
        if (total && on >= total * MB_ROUTE_SHARE) {
            mb_stroke(plane, s, l->origin, p[0], p[1], p[2], p[3], MB_ROUTE_WIDTH * .5f);
            hits++;
        }
    }
    int r = s * 14 / 4096 > 0 ? s * 14 / 4096 : 1;   /* about 19 m */
    mb_dilate(plane, &work, s, r);
    for (long i = 0; i < n; i++) plane[i] = (unsigned char)~plane[i];
    mb_dilate(plane, &work, s, r);
    for (long i = 0; i < n; i++) plane[i] = (unsigned char)~plane[i];
    free(work.plane); free(work.column);
    if (!hits) { free(plane); return NULL; }
    return plane;
}

/* A malloc'd size*size RGB image, or NULL when memory ran out. Thread-safe: it
 * reads only its arguments. */
static unsigned char *map_bake_from_layers(const MbLayers *l, const unsigned char *src,
                                           int w, int h, int race,
                                           const float *line, int count, int loop) {
    unsigned char *out = (unsigned char *)malloc((size_t)l->size * l->size * 3);
    unsigned char *route = race ? mb_route_line(l, line, count, loop) : NULL;
    if (race && !route) route = mb_route_links(l, src, w, h);
    if (out && !map_bake_compose(src, w, h, l->road, l->second, l->drive, route, l->size, race, out)) {
        free(out); out = NULL;
    }
    free(route);
    return out;
}

/* ---- cache ------------------------------------------------------------------
 * <game>/OpenUG2Cache/<name>_<size>_v<version>_<source hash>.map
 * The name holds everything that decides the image, so a stale file is simply
 * never opened. Deleting the directory is always safe. Bump the version whenever
 * the bake's output changes. */
#define MB_CACHE_DIR "OpenUG2Cache"
#define MB_CACHE_VERSION 3
#define MB_CACHE_MAGIC 0x314D4755u    /* "UGM1" */

static uint32_t map_cache_hash(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int map_cache_path(char *out, size_t cap, const char *root, const char *name,
                          int size, uint32_t hash) {
    int n = snprintf(out, cap, "%s/" MB_CACHE_DIR "/%s_%d_v%d_%08x.map",
                     root, name, size, MB_CACHE_VERSION, (unsigned)hash);
    return n > 0 && (size_t)n < cap;
}

static int map_cache_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* A left-neighbour delta makes the smooth image compress far better. */
static void mb_delta(unsigned char *rgb, int s, int undo) {
    for (int y = 0; y < s; y++) {
        unsigned char *row = rgb + (size_t)y * s * 3;
        if (undo) for (int i = 3; i < s * 3; i++) row[i] = (unsigned char)(row[i] + row[i - 3]);
        else for (int i = s * 3 - 1; i >= 3; i--) row[i] = (unsigned char)(row[i] - row[i - 3]);
    }
}

/* Best effort: a read-only game folder only means baking again next time. rgb is
 * left unchanged. The rename keeps a crash from leaving half a file. */
static int map_cache_write(const char *path, unsigned char *rgb, int s) {
    size_t raw = (size_t)s * s * 3;
    uLongf packed = compressBound((uLong)raw);
    unsigned char *z = (unsigned char *)malloc((size_t)packed);
    if (!z) return 0;
    uint32_t sum = (uint32_t)adler32(adler32(0, NULL, 0), rgb, (uInt)raw);
    mb_delta(rgb, s, 0);
    int ok = compress2(z, &packed, rgb, (uLong)raw, 1) == Z_OK;
    mb_delta(rgb, s, 1);
    char dir[1100], tmp[1100];
    const char *slash = strrchr(path, '/');
    if (ok && slash && (size_t)(slash - path) < sizeof dir) {
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = 0;
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0777);
#endif
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        uint32_t head[4] = { MB_CACHE_MAGIC, (uint32_t)s, (uint32_t)packed, sum };
        ok = f && fwrite(head, sizeof head, 1, f) == 1 && fwrite(z, 1, (size_t)packed, f) == (size_t)packed;
        if (f && fclose(f)) ok = 0;
        if (ok) { remove(path); ok = rename(tmp, path) == 0; }
        else remove(tmp);
    } else ok = 0;
    free(z);
    return ok;
}

/* The s*s RGB image, or NULL for anything short of an intact file of that size. */
static unsigned char *map_cache_read(const char *path, int s) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t raw = (size_t)s * s * 3;
    uint32_t head[4];
    unsigned char *z = NULL, *rgb = NULL;
    if (fread(head, sizeof head, 1, f) == 1 && head[0] == MB_CACHE_MAGIC && head[1] == (uint32_t)s &&
        head[2] <= compressBound((uLong)raw)) {
        z = (unsigned char *)malloc(head[2]);
        rgb = (unsigned char *)malloc(raw);
        uLongf out = (uLongf)raw;
        if (!z || !rgb || fread(z, 1, head[2], f) != head[2] ||
            uncompress(rgb, &out, z, head[2]) != Z_OK || out != raw) {
            free(rgb); rgb = NULL;
        } else {
            mb_delta(rgb, s, 1);
            if ((uint32_t)adler32(adler32(0, NULL, 0), rgb, (uInt)raw) != head[3]) { free(rgb); rgb = NULL; }
        }
    }
    free(z);
    fclose(f);
    return rgb;
}

#endif
