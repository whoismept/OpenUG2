/* world2.c -- instance-driven scene assembly.
 *
 * HOW THIS DIFFERS FROM world.c
 * -----------------------------
 * The older loader BAKES each model's world matrix straight into its
 * vertices, so a model arrives already positioned, once per asset name.
 * Placing further copies then has to undo that bake
 * (inverse(model matrix) * instance matrix), and wherever the bake is not
 * what we assume, the result is garbage -- objects floating in mid-air.
 *
 * Here nothing is baked:
 *
 *   1. the model library is loaded in LOCAL coordinates;
 *   2. the scene is built FROM INSTANCES: the instance matrix is applied to
 *      the local geometry DIRECTLY, with nothing to undo;
 *   3. only the sections of the regions in range are read, not the whole
 *      bundle.
 *
 * CONTAINER LAYOUT
 *
 *   Model library, in the district geometry bundle:
 *     0x80134000  model list
 *       0x80134010  one model
 *         0x00134011  header: name in the last 28 bytes, 4x4 matrix at +0x40
 *         0x00134012  texture table
 *         0x80134100  geometry
 *           0x00134900  mesh header
 *           0x00134b01  vertices, stride 24: xyz @0, colour @12, uv @16
 *           0x00134b02  submeshes, 60-byte records, index count at +0x0C
 *           0x00134b03  indices, uint16
 *
 *   Placement, in the same bundle:
 *     0x80034100  section
 *       0x00034101  header; +0x0c = region number
 *       0x00034102  types, 68 bytes: name @0, hashes @+0x20
 *       0x00034103  instances, 64 bytes:
 *                     +0x18 u16 type index
 *                     +0x1a u16 flags
 *                     +0x20 float[3] position
 *                     +0x2c s16[9] rotation*scale, row major, 8192 = 1.0
 *       0x00034105  culling tree
 *
 *   Regions, in the district companion file:
 *     0x80034150 -> 0x00034152  boundaries as 2D polygons
 *
 * NOTE: every chunk's payload starts with variable-length 0x11 filler that
 * must be skipped. Lengths of 52, 68 and 100 bytes all occur.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "nfsu2.h"
#include "world.h"

/* Centre and radius for instance placement. Set from main before world_load,
   which knows nothing about the spawn point itself. Radius 0 disables it. */
float world_inst_x = 0, world_inst_y = 0, world_inst_r = 0;

/* --world2: assemble the scene with the instance-driven builder instead of
   walking the prototypes. The older path stays for comparison. */
int world2_on = 0;

char world2_bundle[64] = "";   /* selected district; the light sources need it */

/* ---------------- chunk tree walking ---------------- */

static long w2_filler(const unsigned char *p, long n) {
    long i = 0;
    while (i < n && p[i] == 0x11) i++;
    return i;
}

/* Direct child of a container. */
static int w2_child(const unsigned char *d, long beg, long end,
                    uint32_t want, long *off, long *size) {
    long q = beg;
    while (q + 8 <= end) {
        uint32_t m = n2_u32(d + q);
        long s = (long)n2_u32(d + q + 4);
        if (s < 0 || q + 8 + s > end) return 0;
        if (m == want) { *off = q + 8; *size = s; return 1; }
        q += 8 + s;
    }
    return 0;
}

/* A model name carries its level of detail as a suffix (`..._1A_00`,
   `..._1B_00`, `..._1Z_00`). Returns 1 when the suffix is present, meaning the
   model is an LOD variant and must not be placed without an instance
   referencing it. */
static int w2_is_lod(const char *nm) {
    long n = (long)strlen(nm);
    for (long i = 0; i + 3 <= n; i++)
        if (nm[i] == '_' && nm[i+1] == '1'
            && ((nm[i+2] >= 'A' && nm[i+2] <= 'Z') || (nm[i+2] >= 'a' && nm[i+2] <= 'z')))
            return 1;
    return 0;
}

/* ---------------- model library ---------------- */

typedef struct {
    char  name[32];
    int   m0, m1;          /* mesh range in lib, in LOCAL coordinates */
    float objm[16];        /* the model's own matrix: fallback placement */
    int   hasm;
    int   cat;             /* N2_ROAD / N2_TERRAIN / other */
} W2Proto;

typedef struct {
    N2Scene  lib;          /* model meshes in local coordinates */
    W2Proto *proto;
    int      nproto, cap;
} W2Lib;

static void w2_lib_add(W2Lib *L, const W2Proto *p) {
    if (L->nproto == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 1024;
        L->proto = (W2Proto *)realloc(L->proto, (size_t)L->cap * sizeof *L->proto);
    }
    L->proto[L->nproto++] = *p;
}

/* Walk every model and collect its geometry in LOCAL coordinates. The model
   matrix is deliberately NOT applied -- that is the whole point here. */
static void w2_collect(const unsigned char *d, long beg, long end,
                       W2Lib *L, const uint32_t *keys, int nkeys, int depth) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o);
        long size = (long)n2_u32(d + o + 4);
        if (size < 0 || o + 8 + size > end) return;
        long ds = o + 8;

        if (magic == 0x80134010u) {
            W2Proto p;
            memset(&p, 0, sizeof p);
            n2_mesh_name(d, ds, ds + size, p.name, sizeof p.name);
            p.hasm = n2_obj_matrix(d, ds, ds + size, p.objm);
            /* Panoramic backdrops: huge flat billboards that stand in for the
               distant horizon. They span -955 to +1441 in height and sit at the
               edge of the map, so drawn as ordinary geometry they lean over the
               city and break the ground query -- the spawn point ends up at
               303 instead of 23.7. */
            if (!strncmp(p.name, "PAN", 3)) { o = ds + size; continue; }

            int cat = n2_mesh_category(d, ds, ds + size);
            p.cat = cat;
            if (n2_vista_family(p.name)) {
                N2Geom vg;
                if (n2_obj_geom(d, ds, ds + size, p.objm, &vg) &&
                    n2_is_vista_impostor(p.name, &vg)) { o = ds + size; continue; }
            }
            uint32_t tk = n2_mesh_texkey_cat(d, ds, ds + size, cat, keys, nkeys);

            N2Leaf vtx[64], idx[64];
            int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + size, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + size, 0x00134B03u, idx, &ni, 64);
            int pairs = nv < ni ? nv : ni;

            p.m0 = L->lib.count;

            /* PER-SUBMESH MATERIALS. A model lists SEVERAL texture slots and
               its submesh table cuts the index buffer into ranges, each naming
               one slot. Binding a single texture to the whole model loses all
               the signage -- a car park keeps its concrete and drops its
               'Parking', 'ENTER HERE' and 'CLEARANCE' lettering.
               Anything that fails validation (no records, several index
               sheets, a slot number out of range, an empty slot, ranges that
               do not tile the buffer) falls back to one mesh, one texture. */
            int sub_ok = 0;
            N2Sub sub[64]; int nsub = 0;
            uint32_t slot[64]; int nslot = 0;
            if (pairs == 1) {
                nsub  = n2_mesh_submeshes(d, ds, ds + size, sub, 64);
                nslot = n2_mesh_texslots(d, ds, ds + size, slot, 64);
                if (nsub > 0 && nslot > 0) {
                    const unsigned char *ib0 = d + idx[0].off;
                    int ibytes = (int)idx[0].size, ip = 0;
                    while (ip + 2 <= ibytes && ib0[ip] == 0x11 && ib0[ip+1] == 0x11) ip += 2;
                    long avail = (ibytes - ip) / 2, chain = 0;
                    sub_ok = 1;
                    for (int a = 0; a < nsub && sub_ok; a++) {
                        if (sub[a].mat >= (uint32_t)nslot)      sub_ok = 0;
                        else if (!slot[sub[a].mat])             sub_ok = 0;
                        else if ((long)sub[a].start != chain)   sub_ok = 0;
                        else if (sub[a].count < 3)              sub_ok = 0;
                        else if (chain + (long)sub[a].count > avail) sub_ok = 0;
                        else chain += (long)sub[a].count;
                    }
                    if (sub_ok && chain != avail - avail % 3) sub_ok = 0;
                }
            }

            if (sub_ok) {
                for (int a = 0; a < nsub; a++) {
                    int before = L->lib.count;
                    /* the slot's key when this district ships it; otherwise
                       the model-wide key, but only for this range */
                    uint32_t sk = n2_resolve_key(slot[sub[a].mat], keys, nkeys);
                    if (!sk) sk = tk;
                    n2_add_pair(d, vtx[0], idx[0], cat, &L->lib, 24, 16, 0, sk, NULL,
                                (long)sub[a].start, (long)sub[a].count);
                    for (int m2 = before; m2 < L->lib.count; m2++) {
                        L->lib.meshes[m2].scen = (unsigned char)n2_scen_class(p.name);
                        snprintf(L->lib.meshes[m2].sname, sizeof L->lib.meshes[m2].sname,
                                 "%.31s", p.name);
                    }
                }
            } else
            for (int k = 0; k < pairs; k++) {
                int before = L->lib.count;
                /* mtx = NULL keeps the vertices LOCAL */
                n2_add_pair(d, vtx[k], idx[k], cat, &L->lib, 24, 16, 0, tk, NULL, 0, -1);
                for (int m2 = before; m2 < L->lib.count; m2++) {
                    L->lib.meshes[m2].scen = (unsigned char)n2_scen_class(p.name);
                    snprintf(L->lib.meshes[m2].sname, sizeof L->lib.meshes[m2].sname,
                             "%.31s", p.name);
                }
            }
            p.m1 = L->lib.count;
            if (p.m1 > p.m0) w2_lib_add(L, &p);
        } else if (magic != 0 && (magic >> 28) == 8 && depth < 6) {
            w2_collect(d, ds, ds + size, L, keys, nkeys, depth + 1);
        }
        o = ds + size;
    }
}

/* ---------------- regions ---------------- */

typedef struct { int id; float bb[4]; float *v; int nv; } W2Reg;

static int w2_regions(const unsigned char *d, long len, W2Reg **out) {
    long q = 0, co = 0, cs = 0;
    while (q + 8 <= len) {
        uint32_t m = n2_u32(d + q);
        long s = (long)n2_u32(d + q + 4);
        if (s < 0 || q + 8 + s > len) break;
        if (m == 0x80034150u) {
            if (!w2_child(d, q + 8, q + 8 + s, 0x00034152u, &co, &cs)) return 0;
            break;
        }
        q += 8 + s;
    }
    if (!cs) return 0;

    int cap = 512, n = 0;
    W2Reg *r = (W2Reg *)malloc((size_t)cap * sizeof *r);
    long o = co + w2_filler(d + co, cs), end = co + cs;
    while (o + 0x24 <= end) {
        int nv = d[o + 0x0a];
        if (nv <= 0 || nv > 64 || o + 0x24 + (long)nv * 8 > end) break;
        if (n == cap) { cap *= 2; r = (W2Reg *)realloc(r, (size_t)cap * sizeof *r); }
        r[n].id = (int16_t)(d[o + 8] | (d[o + 9] << 8));
        for (int i = 0; i < 4; i++) memcpy(&r[n].bb[i], d + o + 0x0c + i * 4, 4);
        r[n].nv = nv;
        r[n].v = (float *)malloc((size_t)nv * 2 * sizeof(float));
        memcpy(r[n].v, d + o + 0x24, (size_t)nv * 2 * sizeof(float));
        n++;
        o += 0x24 + (long)nv * 8;
    }
    *out = r;
    return n;
}

static int w2_in_poly(const W2Reg *r, float x, float y) {
    if (x < r->bb[0] || x > r->bb[2] || y < r->bb[1] || y > r->bb[3]) return 0;
    int c = 0;
    for (int i = 0, j = r->nv - 1; i < r->nv; j = i++) {
        float xi = r->v[i*2], yi = r->v[i*2+1];
        float xj = r->v[j*2], yj = r->v[j*2+1];
        if ((yi > y) != (yj > y) &&
            x < (xj - xi) * (y - yi) / (yj - yi + 1e-9f) + xi) c = !c;
    }
    return c;
}

/* ---------------- model lookup by name ---------------- */

typedef struct { uint32_t h; int pi; } W2Ix;

static uint32_t w2_hash(const char *s) {
    /* A model's own name is upper-cased and clipped to 27 characters, while
       the type table carries it as authored. Fold both to the same form. */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 27 && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        h ^= c; h *= 16777619u;
    }
    return h;
}

static int w2_name_eq(const char *a, const char *b) {
    for (int i = 0; i < 27; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'a' && x <= 'z') x = (unsigned char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (unsigned char)(y - 'a' + 'A');
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

static int w2_ixcmp(const void *a, const void *b) {
    uint32_t x = ((const W2Ix *)a)->h, y = ((const W2Ix *)b)->h;
    return x < y ? -1 : x > y ? 1 : 0;
}

static const float IDENT[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

/* Models that are never drawn. Track barriers are invisible walls that keep a
   car on course: not one of their texture slots resolves, so drawing them
   yields flat blue panels standing where a building should be. Race markers
   and banners belong to a race start and finish and have no place in free
   roam. */
static int w2_invisible(const char *n) {
    return n2_icontains((const unsigned char *)n, (long)strlen(n), "TRACKBARRIER")
        || !strncmp(n, "ZPM_", 4) || !strncmp(n, "zpm_", 4)
        || n2_icontains((const unsigned char *)n, (long)strlen(n), "BANDEROLLE");
}

/* Place every mesh of a model into the scene using matrix M (OpenGL layout). */
static int w2_place(N2Scene *dst, const W2Lib *L, int pi, const float *M) {
    int added = 0;
    if (w2_invisible(L->proto[pi].name)) return 0;
    for (int m = L->proto[pi].m0; m < L->proto[pi].m1; m++) {
        const N2Mesh *src = &L->lib.meshes[m];
        if (src->nverts <= 0 || src->nidx <= 0) continue;
        N2Mesh c = *src;
        c.inst = 1;
        snprintf(c.aname, sizeof c.aname, "%s", L->proto[pi].name);
        c.verts = (float *)malloc((size_t)src->nverts * 5 * sizeof(float));
        c.idx   = (uint16_t *)malloc((size_t)src->nidx * sizeof(uint16_t));
        memcpy(c.idx, src->idx, (size_t)src->nidx * sizeof(uint16_t));
        if (src->vcol) {
            c.vcol = (unsigned char *)malloc((size_t)src->nverts * 4);
            memcpy(c.vcol, src->vcol, (size_t)src->nverts * 4);
        }
        for (int v = 0; v < src->nverts; v++) {
            float px = src->verts[v*5], py = src->verts[v*5+1], pz = src->verts[v*5+2];
            c.verts[v*5+0] = px*M[0] + py*M[4] + pz*M[8]  + M[12];
            c.verts[v*5+1] = px*M[1] + py*M[5] + pz*M[9]  + M[13];
            c.verts[v*5+2] = px*M[2] + py*M[6] + pz*M[10] + M[14];
            c.verts[v*5+3] = src->verts[v*5+3];
            c.verts[v*5+4] = src->verts[v*5+4];
        }
        if (n2_mesh_is_broken(&c)) {   /* nan vertices: drop it */
            free(c.verts); free(c.idx); free(c.vcol); continue;
        }
        n2_push_mesh(dst, c);
        added++;
    }
    return added;
}

/* ---------------- scene assembly ---------------- */

int world2_build(N2Scene *out, const char *troot,
                 const char *const *bundles, int nbundles,
                 float sx, float sy, float viewdist,
                 const unsigned char *loc4, long loc4len)
{
    static uint32_t keys[16384];
    int nkeys = 0;
    memset(out, 0, sizeof *out);

    /* --- regions from the companion of every bundle --- */
    W2Reg *regs = NULL; int nregs = 0, rcap = 0;
    for (int b = 0; b < nbundles; b++) {
        const char *rn = bundles[b];
        const char *stem = strncmp(rn, "STREAM", 6) ? rn : rn + 6;
        char cp[1024]; snprintf(cp, sizeof cp, "%s/%s.BUN", troot, stem);
        long clen; unsigned char *cd = n2_read_file(cp, &clen);
        if (!cd) continue;
        W2Reg *part = NULL; int np = w2_regions(cd, clen, &part);
        for (int i = 0; i < np; i++) {
            int dup = 0;
            for (int j = 0; j < nregs && !dup; j++) if (regs[j].id == part[i].id) dup = 1;
            if (dup) { free(part[i].v); continue; }
            if (nregs == rcap) {
                rcap = rcap ? rcap * 2 : 512;
                regs = (W2Reg *)realloc(regs, (size_t)rcap * sizeof *regs);
            }
            regs[nregs++] = part[i];
        }
        free(part); free(cd);
    }
    if (!nregs) { fprintf(stderr, "world2: no regions parsed\n"); return 0; }

    /* which regions we need: our own plus everything within view range */
    unsigned char *want = (unsigned char *)calloc(8192, 1);
    int home = -1, nsel = 0;
    for (int i = 0; i < nregs; i++) {
        int inside = w2_in_poly(&regs[i], sx, sy);
        if (inside && (home < 0 ||
            (regs[i].bb[2]-regs[i].bb[0]) < (regs[home].bb[2]-regs[home].bb[0]))) home = i;
        float dx = sx < regs[i].bb[0] ? regs[i].bb[0]-sx : (sx > regs[i].bb[2] ? sx-regs[i].bb[2] : 0);
        float dy = sy < regs[i].bb[1] ? regs[i].bb[1]-sy : (sy > regs[i].bb[3] ? sy-regs[i].bb[3] : 0);
        if (inside || dx*dx + dy*dy <= viewdist*viewdist) {
            int id = regs[i].id;
            if (id >= 0 && id < 8192 && !want[id]) { want[id] = 1; nsel++; }
        }
    }
    printf("world2: %d regions, %d selected, own region %d\n",
           nregs, nsel, home >= 0 ? regs[home].id : -1);

    /* BUNDLE CHOICE. Districts are overlapping working sets built for
       different routes, not adjacent map tiles -- some pairs cover exactly the
       same bounds. Loading all of them brings the same building in three or
       four times over. Take ONLY the bundle that carries a section for our own
       region. */
    int only = -1;
    if (home >= 0) {
        int hid = regs[home].id;
        for (int b = 0; b < nbundles && only < 0; b++) {
            char bp[1024]; snprintf(bp, sizeof bp, "%s/%s.BUN", troot, bundles[b]);
            long blen; unsigned char *bd = n2_read_file(bp, &blen);
            if (!bd) continue;
            long q = 0;
            while (q + 8 <= blen) {
                uint32_t m = n2_u32(bd + q);
                long sz = (long)n2_u32(bd + q + 4);
                if (sz < 0 || q + 8 + sz > blen) break;
                if (m == 0x80034100u) {
                    long ho, hs;
                    if (w2_child(bd, q+8, q+8+sz, 0x00034101u, &ho, &hs)) {
                        long hp = ho + w2_filler(bd + ho, hs);
                        if ((int)n2_u32(bd + hp + 0x0c) == hid) { only = b; break; }
                    }
                }
                q += 8 + sz;
            }
            free(bd);
        }
    }
    if (only >= 0) {
        printf("world2: district %s (holds our region)\n", bundles[only]);
        snprintf(world2_bundle, sizeof world2_bundle, "%s", bundles[only]);
    }

    long total_inst = 0, placed = 0, nomodel = 0, fallback = 0;
    long skipped_lod = 0;

    for (int b = 0; b < nbundles; b++) {
        if (only >= 0 && b != only) continue;
        char bp[1024]; snprintf(bp, sizeof bp, "%s/%s.BUN", troot, bundles[b]);
        long blen; unsigned char *bd = n2_read_file(bp, &blen);
        if (!bd) continue;

        /* this bundle's texture keys plus the shared library */
        N2Tpk pack = n2_tpk_open(bd, blen);
        nkeys = n2_tpk_keys(bd, pack, keys, 16384);
        if (loc4 && nkeys < 16384)
            nkeys += n2_car_tex_keys(loc4, loc4len, keys + nkeys, 16384 - nkeys);

        /* --- this bundle's model library, in LOCAL coordinates --- */
        W2Lib L; memset(&L, 0, sizeof L);
        w2_collect(bd, 0, blen, &L, keys, nkeys, 0);

        W2Ix *ix = (W2Ix *)malloc((size_t)(L.nproto ? L.nproto : 1) * sizeof *ix);
        for (int i = 0; i < L.nproto; i++) { ix[i].h = w2_hash(L.proto[i].name); ix[i].pi = i; }
        qsort(ix, (size_t)L.nproto, sizeof *ix, w2_ixcmp);
        unsigned char *used = (unsigned char *)calloc((size_t)(L.nproto ? L.nproto : 1), 1);
        /* Which models any instance record in this district refers to at all.
           The fallback path -- placing a model by its own matrix -- is only
           valid for models nothing refers to: terrain, roads and other unique
           geometry. Otherwise a model whose instances all live in OTHER
           regions turns up in our frame as a stray object standing at the
           position of its first instance. */
        unsigned char *anyref = (unsigned char *)calloc((size_t)(L.nproto ? L.nproto : 1), 1);
        {
            long q2 = 0;
            while (q2 + 8 <= blen) {
                uint32_t m2 = n2_u32(bd + q2);
                long s2 = (long)n2_u32(bd + q2 + 4);
                if (s2 < 0 || q2 + 8 + s2 > blen) break;
                if (m2 == 0x80034100u) {
                    long to2, ts2;
                    if (w2_child(bd, q2+8, q2+8+s2, 0x00034102u, &to2, &ts2)) {
                        long tp2 = to2 + w2_filler(bd + to2, ts2);
                        int nt2 = (int)((to2 + ts2 - tp2) / 68);
                        for (int t = 0; t < nt2; t++) {
                            char nm2[32];
                            snprintf(nm2, sizeof nm2, "%.31s", (const char *)(bd + tp2 + (long)t*68));
                            uint32_t h2 = w2_hash(nm2);
                            int lo2 = 0, hi2 = L.nproto - 1, at2 = -1;
                            while (lo2 <= hi2) { int md = (lo2+hi2)/2;
                                if (ix[md].h < h2) lo2 = md+1;
                                else { if (ix[md].h == h2) at2 = md; hi2 = md-1; } }
                            for (int k = at2; k >= 0 && k < L.nproto && ix[k].h == h2; k++)
                                if (w2_name_eq(L.proto[ix[k].pi].name, nm2)) anyref[ix[k].pi] = 1;
                        }
                    }
                }
                q2 += 8 + s2;
            }
        }

        /* --- sections: placement --- */
        long q = 0;
        while (q + 8 <= blen) {
            uint32_t m = n2_u32(bd + q);
            long s = (long)n2_u32(bd + q + 4);
            if (s < 0 || q + 8 + s > blen) break;
            if (m == 0x80034100u) {
                long ho, hs, to, ts, io_, is_;
                if (w2_child(bd, q+8, q+8+s, 0x00034101u, &ho, &hs) &&
                    w2_child(bd, q+8, q+8+s, 0x00034102u, &to, &ts) &&
                    w2_child(bd, q+8, q+8+s, 0x00034103u, &io_, &is_)) {
                    long hp = ho + w2_filler(bd + ho, hs);
                    int rid = (int)n2_u32(bd + hp + 0x0c);
                    (void)rid;
                    /* Select by GEOMETRY, not by region number. A large object
                       placed by a NEIGHBOURING region can still stand directly
                       over our heads -- a multi-storey car park whose decks
                       carry parked cars is placed from the region next door, so
                       filtering by number drops it and leaves those cars in
                       mid-air. Each instance record carries its own world
                       bounds; decide on those. */
                    {
                        long tp = to + w2_filler(bd + to, ts);
                        int ntypes = (int)((to + ts - tp) / 68);
                        long ip = io_ + w2_filler(bd + io_, is_);
                        for (long o = ip; o + 64 <= io_ + is_; o += 64) {
                            total_inst++;
                            /* instance bounds against the view radius */
                            float amin[3], amax[3];
                            memcpy(amin, bd + o, 12); memcpy(amax, bd + o + 0x0c, 12);
                            float dx = sx < amin[0] ? amin[0]-sx : (sx > amax[0] ? sx-amax[0] : 0);
                            float dy = sy < amin[1] ? amin[1]-sy : (sy > amax[1] ? sy-amax[1] : 0);
                            if (dx*dx + dy*dy > viewdist*viewdist) continue;
                            int ti = bd[o+0x18] | (bd[o+0x19] << 8);
                            if (ti < 0 || ti >= ntypes) continue;
                            char nm[32];
                            snprintf(nm, sizeof nm, "%.31s", (const char *)(bd + tp + (long)ti*68));

                            /* instance matrix applied DIRECTLY, nothing to undo */
                            float M[16];
                            for (int k = 0; k < 16; k++) M[k] = (k % 5 == 0) ? 1.0f : 0.0f;
                            for (int k = 0; k < 9; k++) {
                                int16_t v; memcpy(&v, bd + o + 0x2c + k*2, 2);
                                M[(k/3)*4 + (k%3)] = (float)v / 8192.0f;
                            }
                            memcpy(&M[12], bd + o + 0x20, 12);

                            uint32_t h = w2_hash(nm);
                            int lo = 0, hi = L.nproto - 1, at = -1;
                            while (lo <= hi) {
                                int mid = (lo + hi) / 2;
                                if (ix[mid].h < h) lo = mid + 1;
                                else { if (ix[mid].h == h) at = mid; hi = mid - 1; }
                            }
                            if (at < 0) { nomodel++; continue; }
                            int any = 0;
                            for (int k = at; k < L.nproto && ix[k].h == h; k++) {
                                int pi = ix[k].pi;
                                if (!w2_name_eq(L.proto[pi].name, nm)) continue;
                                /* A model with NO matrix of its own already has
                                   world-space vertices (terrain, roads). Place
                                   it once as it is and do not apply the instance
                                   matrix, which would send it flying. */
                                /* GROUND IS NEVER DUPLICATED. Roads and terrain
                                   are unique world geometry, placed once by
                                   their own matrix. Applying an instance matrix
                                   scatters them: ground heights spread across
                                   -985..1441 and the spawn point lands at 303
                                   instead of 23.7. */
                                if (L.proto[pi].cat == N2_ROAD || L.proto[pi].cat == N2_TERRAIN
                                    || !L.proto[pi].hasm) {
                                    if (!used[pi]) {
                                        placed += w2_place(out, &L, pi,
                                            L.proto[pi].hasm ? L.proto[pi].objm : IDENT);
                                        used[pi] = 1;
                                    }
                                    any = 1; continue;
                                }
                                placed += w2_place(out, &L, pi, M);
                                used[pi] = 1; any = 1;
                            }
                            if (!any) nomodel++;
                        }
                    }
                }
            }
            q += 8 + s;
        }

        /* --- fallback: models no instance refers to are placed by their own
               matrix. This is what keeps terrain and roads in place, since they
               have no instance records at all. --- */
        for (int i = 0; i < L.nproto; i++) {
            if (used[i]) continue;
            if (anyref[i] && L.proto[i].cat != N2_ROAD && L.proto[i].cat != N2_TERRAIN)
                continue;   /* has instances, but in other regions: not our frame */
            /* NEVER PLACE LOD VARIANTS. A building sits in the library in
               several versions -- `_1A` full, then `_1B`, `_1C`, `_1Z` down to a
               coarse silhouette -- and an instance refers to exactly one of
               them. Placing every unreferenced variant grows grey drums and
               floating shop fronts around the city: the variants are laid out
               in the library in a row 26 to 31 units apart, which is a library
               layout, not city positions.
               Ground and roads still pass: they have no instance records. */
            if (L.proto[i].cat != N2_ROAD && L.proto[i].cat != N2_TERRAIN
                && w2_is_lod(L.proto[i].name)) { skipped_lod++; continue; }
            fallback += w2_place(out, &L, i, L.proto[i].hasm ? L.proto[i].objm : IDENT);
        }

        /* the library is done: its geometry has been copied into the scene */
        for (int i = 0; i < L.lib.count; i++) {
            free(L.lib.meshes[i].verts);
            free(L.lib.meshes[i].idx);
            free(L.lib.meshes[i].vcol);
        }
        free(L.lib.meshes); free(L.proto); free(ix); free(used); free(anyref); free(bd);
    }

    {   /* diagnostics: geometry per class and where it sits */
        long nroad=0, nterr=0, noth=0; float zmin=1e30f, zmax=-1e30f;
        for (int i = 0; i < out->count; i++) {
            const N2Mesh *m = &out->meshes[i];
            if (m->cat == N2_ROAD) nroad++; else if (m->cat == N2_TERRAIN) nterr++; else noth++;
            for (int v = 0; v < m->nverts; v++) {
                float z = m->verts[v*5+2];
                if (m->cat == N2_ROAD || m->cat == N2_TERRAIN) {
                    if (z < zmin) zmin = z; if (z > zmax) zmax = z;
                }
            }
        }
        printf("world2: road %ld, terrain %ld, other %ld; ground z %.1f..%.1f\n",
               nroad, nterr, noth, zmin, zmax);
    }
    {   /* how much geometry is still untextured, and which models */
        long with = 0, without = 0;
        struct { char nm[32]; long n; } top[12]; int nt = 0;
        for (int i = 0; i < out->count; i++) {
            const N2Mesh *m = &out->meshes[i];
            if (m->texkey) { with++; continue; }
            without++;
            int f = -1;
            for (int k = 0; k < nt; k++) if (!strncmp(top[k].nm, m->sname, 31)) { f = k; break; }
            if (f >= 0) top[f].n++;
            else if (nt < 12) { snprintf(top[nt].nm, 32, "%.31s", m->sname); top[nt].n = 1; nt++; }
        }
        printf("world2: textured %ld, untextured %ld (%.1f%%)\n",
               with, without, out->count ? 100.0*without/out->count : 0.0);
        for (int a = 0; a < nt; a++) for (int b = a+1; b < nt; b++)
            if (top[b].n > top[a].n) { char t[32]; long n2;
                memcpy(t, top[a].nm, 32); n2 = top[a].n;
                memcpy(top[a].nm, top[b].nm, 32); top[a].n = top[b].n;
                memcpy(top[b].nm, t, 32); top[b].n = n2; }
        for (int k = 0; k < nt && k < 8; k++)
            printf("    untextured: %-30s %ld meshes\n", top[k].nm, top[k].n);
    }
    printf("world2: %ld instances, %ld meshes placed, %ld with no model, "
           "%ld by own matrix, %d in the scene\n",
           total_inst, placed, nomodel, fallback, out->count);
    printf("world2: skipped unreferenced LOD variants: %ld\n", skipped_lod);

    for (int i = 0; i < nregs; i++) free(regs[i].v);
    free(regs); free(want);
    return out->count;
}
