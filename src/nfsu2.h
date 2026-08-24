/* nfsu2.h — single-header loader for NFSU2 track assets.
 * Ports the reverse-engineered .BUN chunk format + DXT1 texture decode to C.
 * Public domain, stdlib only. See README for the format notes.
 */
#ifndef NFSU2_H
#define NFSU2_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* One drawable submesh: interleaved [px,py,pz,u,v] verts + u16 triangle list. */
enum { N2_ROAD = 0, N2_TERRAIN = 1, N2_OTHER = 2, N2_SKY = 3, N2_GLOW = 4,
       /* car mesh classes, from material name */
       N2_CAR_BODY = 10, N2_CAR_GLASS = 11, N2_CAR_LIGHT = 12,
       N2_CAR_TIRE = 13, N2_CAR_MISC = 14, N2_CAR_BRAKELIGHT = 15, N2_CAR_MECH = 16 };
typedef struct {
    float   *verts;   /* 5 floats per vertex: pos.xyz, uv */
    unsigned char *vcol; /* 4 bytes/vertex: RGBA prelight from the source stream
                            (world meshes, 24B stride, colour @ off 12). NULL for
                            car meshes and any stream without it. */
    int      nverts;
    uint16_t *idx;
    int      nidx;
    int      cat;     /* N2_ROAD / N2_TERRAIN / N2_OTHER, from material name */
    uint32_t texkey;  /* car meshes: bound TPK diffuse key (0x134012), 0 if none */
    uint32_t matkey;  /* car meshes: the submesh's material key, 0 = none. This
                         is what ties a panel to its material record, and so to
                         the shading the data asks for rather than a guess. */
    int      trim;    /* car BODY meshes only: 1 = plastic bumper/skirt (verified
                          real per-part name tokens, e.g. GOLF_KIT00_FRONT_BUMPER_A),
                          duller/broader specular than the metallic paint panels */
    uint32_t namekey; /* car meshes: hash of the part name with any trailing
                          _A.._D LOD suffix stripped, so every tier of one part
                          shares a key. 0 = unnamed. Drives n2_car_dedupe_lod. */
    int      vkind;   /* car meshes: 0 = no variant token, 1 = KITnn, 2 = STYLEnn */
    int      vnum;    /* the nn of that token */
    uint32_t famkey;  /* namekey with the variant token ALSO removed, so
                          KIT00_FRONT_BUMPER and KIT05_FRONT_BUMPER — the stock
                          part and its aftermarket replacement — collide. Drives
                          the kit override in n2_car_apply_config. */
    unsigned char scen; /* track meshes: N2_SC_* semantic class from the asset
                           name in this object's own 0x134011 chunk */
    char sname[32];     /* track meshes: that asset name (e.g. XO_StreetLightC_1a_00) */
    unsigned char vrepair; /* 1 = this mesh kept its good geometry after corrupt
                              source vertices were excluded (M133) */
    /* --- instance placement (world2) --- */
    char     aname[28];  /* asset name of the model this came from */
    unsigned char inst;  /* 1 = a copy placed from an instance record. Not fed
                            to collision: walls are derived from geometry, and
                            thousands of trees and parked cars would seal the
                            player in. */
    unsigned char hasm;  /* the model HAS a matrix of its own. If not, its
                            vertices are already in world space and it must not
                            be placed from instances. */
    float objm[16];      /* that matrix. The vertices are already transformed by
                            it, so placing the same model elsewhere needs a
                            RELATIVE transform: instance matrix * inverse(objm). */
} N2Mesh;

/* Active customization profile.
 *
 * NOTE on how the data actually lays out, which is not the obvious model:
 * KIT00 is the WHOLE car (28-29 part families on MIATA/GOLF: body, doors,
 * roof, hood, lights, wheels, engine...). KIT01..KIT29 carry only 3-5
 * families each — front bumper, rear bumper, skirt, sometimes trunk audio.
 * So a body kit does not REPLACE the car, it OVERRIDES those few parts on
 * top of KIT00. Dropping KIT00 when a kit is selected would delete the
 * entire vehicle except its bumpers.
 * Hoods work the same way but under a STYLEnn token (there is no STYLE00 —
 * the stock hood is KIT00_HOOD), so hood_style 0 means "keep KIT00's".
 *
 * spoiler/wheel are accepted but inert: neither lives in a car's own
 * GEOMETRY.BIN. They are separate part libraries (CARS/SPOILER, CARS/WHEELS,
 * each its own GEOMETRY.BIN), so wiring them needs a second asset load, not
 * a filter over this file. Left as fields so the call sites don't churn. */
typedef struct {
    int body_kit;       /* 0 = stock KIT00 bumpers/skirts, N = KITnn overrides */
    int hood_style;     /* 0 = stock KIT00 hood,           N = STYLEnn overrides */
    int spoiler_style;  /* inert, see note */
    int wheel_style_id; /* inert, see note */
} N2CarConfig;

typedef struct {
    N2Mesh *meshes;
    int     count, cap;
} N2Scene;

/* Decoded RGB texture (3 bytes/pixel, top-left origin).
 *
 * ALPHA. `alpha` is a separate w*h plane, non-NULL only when the source
 * actually carries transparency AND that transparency says something -- a plane
 * that is 255 everywhere is freed, so an opaque asset uploads as GL_RGB and
 * renders exactly as it did before alpha was decoded at all. Sources, all of
 * them world as well as car:
 *   P8    palette entries are RGBA; channel 3 is the alpha.
 *   DXT1  one-bit transparency, and ONLY in the c0 <= c1 block mode, where
 *         palette index 3 means transparent. The c0 > c1 mode is fully opaque.
 *   DXT3  the block's 4-bit explicit alpha plane.
 *   DXT5  not produced by these assets; no decoder path exists.
 * `afmt` records which of those produced the plane (0 none, 1 DXT1, 3 DXT3,
 * 8 P8). Consumers must treat alpha == NULL as fully opaque. */
typedef struct { int w, h; unsigned char *rgb; unsigned char *alpha;
    /* Optional raw S3TC block copy (base mip only) for a direct GPU upload
       via glCompressedTexImage2D; rgb/alpha stay populated as the portable
       fallback. Set only by n2_load_car_tex_by_key for straight DXT1/DXT3
       slots. dxtfmt: 0 = none (use rgb), 1 = DXT1, 3 = DXT3. */
    unsigned char *dxt; int dxtlen, dxtfmt;
    int afmt;   /* source alpha format: 0 none, 1 DXT1 1-bit, 3 DXT3, 8 P8 */
    /* HOW THE TEXTURE IS MEANT TO BE DRAWN, straight from its record. These
       four bytes are what decide whether alpha means anything -- see the note
       where the alpha plane is kept or freed.
         order   draw order: 0 opaque layer, 5..7 transparent
         usage   0 none, 1 cutout (alpha test), 2 blending
         blend   0 none, 1 ordinary src/1-src, 2 additive (neon, glow)
         wz      1 write depth (opaque), 0 do not
       Measured over one district's 1595 unique textures, the combinations are
       exactly:
         (0,0,0,1) x1236  opaque
         (0,1,0,1) x 158  cutout: barriers, frames, foliage
         (5,2,2,0) x  87  additive: neon and lit windows
         (5,2,1,0) x  46  blended: shop windows, clock faces, glass
         (0,2,0,1) x  53  has alpha but draws opaque (water) */
    unsigned char order, usage, blend, wz; } N2Tex;

/* Draw mode derived from the fields above. */
enum { N2_DRAW_OPAQUE = 0, N2_DRAW_CUTOUT = 1, N2_DRAW_BLEND = 2, N2_DRAW_ADD = 3 };
static int n2_tex_mode(const N2Tex *t) {
    if (t->usage == 1) return N2_DRAW_CUTOUT;
    if (t->usage == 2 && t->blend == 1) return N2_DRAW_BLEND;
    if (t->blend == 2) return N2_DRAW_ADD;
    return N2_DRAW_OPAQUE;
}

/* ---- file I/O ---- */
static unsigned char *n2_read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc(n);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

static uint32_t n2_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Skip the run of 0x11 filler bytes that prefixes a vertex/leaf payload. */
static int n2_skip_filler(const unsigned char *p, int n) {
    int i = 0; while (i < n && p[i] == 0x11) i++; return i;
}

/* ---- mesh extraction ---- */
/* A mesh with a NaN or an absurd coordinate anywhere in it. Isolated bad
 * vertices are a source defect and are repaired per vertex elsewhere; this is
 * the last check before a mesh is accepted at all. */
static uint32_t n2_str_hash(const char *s) {
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; s++) h = h * 33u + (unsigned char)*s;
    return h;
}

/* Material parameters, 160 bytes per record. Each scale-and-colour pair gives
   one bound, and the shader interpolates between the two by dot(V, N). */
typedef struct {
    char     name[28];
    uint32_t hash;
    float    dif_min[3], dif_max[3];   /* colour * scale */
    float    spec_min[3], spec_max[3];
    float    env_min[3],  env_max[3];  /* already scaled by 0.5 */
    float    spec_pow;                 /* specular exponent for the shader path */
    float    env_pow;                  /* (+0x7c) */
} N2LightMat;

static int n2_load_lightmats(const unsigned char *d, long len, N2LightMat *out, int cap) {
    int n = 0;
    for (long i = 0; i + 168 <= len && n < cap; i++) {
        if (n2_u32(d + i) != 0x00135200u || n2_u32(d + i + 4) != 160) continue;
        const unsigned char *r = d + i + 8;
        N2LightMat *m = &out[n];
        memset(m, 0, sizeof *m);
        memcpy(m->name, r + 0x14, 27);
        m->hash = n2_u32(r + 0x0c);
        #define N2F(o) (*(const float *)(r + (o)))
        for (int k = 0; k < 3; k++) {
            m->dif_min[k]  = N2F(0x34 + k*4) * N2F(0x30);
            m->dif_max[k]  = N2F(0x44 + k*4) * N2F(0x40);
            m->spec_min[k] = N2F(0x60 + k*4) * N2F(0x5c);
            m->spec_max[k] = N2F(0x70 + k*4) * N2F(0x6c);
            m->env_min[k]  = N2F(0x84 + k*4) * N2F(0x80) * 0.5f;
            m->env_max[k]  = N2F(0x94 + k*4) * N2F(0x90) * 0.5f;
        }
        m->spec_pow = 1.1f * N2F(0x58);
        m->env_pow  = N2F(0x7c);
        #undef N2F
        n++; i += 160;
    }
    return n;
}

typedef struct { float x, y, z, r_in, r_out; unsigned char cr, cg, cb; } N2LightSrc;

static int n2_load_lights(const unsigned char *d, long len, N2LightSrc *out, int cap) {
    int n = 0;
    for (long i = 0; i + 8 <= len && n < cap; i++) {
        if (n2_u32(d + i) != 0x00135003u) continue;
        long sz = (long)n2_u32(d + i + 4);
        if (sz < 96 || i + 8 + sz > len) continue;
        long p = i + 8, e = p + sz;
        while (p < e && d[p] == 0x11) p++;
        for (; p + 96 <= e && n < cap; p += 96) {
            if (d[p + 0x07] != 1) continue;              /* disabled */
            float ro = *(const float *)(d + p + 0x1c);
            float ri = *(const float *)(d + p + 0x30);
            if (!(ro > 0.0f) || ro > 5000.0f) continue;  /* skip map-wide sources such as the moon */
            uint32_t c = n2_u32(d + p + 0x0c);
            out[n].x = *(const float *)(d + p + 0x10);
            out[n].y = *(const float *)(d + p + 0x14);
            out[n].z = *(const float *)(d + p + 0x18);
            out[n].r_in = ri; out[n].r_out = ro;
            out[n].cr = (unsigned char)(c & 0xff);
            out[n].cg = (unsigned char)((c >> 8) & 0xff);
            out[n].cb = (unsigned char)((c >> 16) & 0xff);
            n++;
        }
        i += 8 + sz - 1;
    }
    return n;
}

static int n2_mesh_is_broken(const N2Mesh *m) {
    for (int v = 0; v < m->nverts; v++) {
        const float *q = m->verts + v*5;
        for (int k = 0; k < 3; k++)
            if (!(q[k] == q[k]) || q[k] > 1e8f || q[k] < -1e8f) return 1;
    }
    return 0;
}

static void n2_push_mesh(N2Scene *s, N2Mesh m) {
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->meshes = (N2Mesh *)realloc(s->meshes, s->cap * sizeof(N2Mesh));
    }
    s->meshes[s->count++] = m;
}

/* Collect leaf chunk (start,size) pairs of a given magic within [beg,end). */
typedef struct { long off; uint32_t size; } N2Leaf;
static void n2_find_leaves(const unsigned char *d, long beg, long end,
                           uint32_t want, N2Leaf *out, int *n, int cap) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o), size = n2_u32(d + o + 4);
        long ds = o + 8;
        if (magic == want && *n < cap) { out[*n].off = ds; out[*n].size = size; (*n)++; }
        else if (magic != 0 && (magic >> 28) == 8) n2_find_leaves(d, ds, ds + size, want, out, n, cap);
        o = ds + size;
    }
}

/* substring search within an unterminated byte run */
static int n2_contains(const unsigned char *hay, long n, const char *needle) {
    long m = (long)strlen(needle);
    for (long i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0) return 1;
    return 0;
}

/* case-insensitive substring search: retail's world-object material names
 * are NOT uniformly cased like the ROAD/TERRAIN/SKY prefixes below — e.g.
 * "XO_BulbAOrange_1a_00", "XB_CasinoF_Neon_1a_LL_00" — so glow/neon/flare
 * detection can't reuse the uppercase-run scan those checks rely on. */
static int n2_icontains(const unsigned char *hay, long n, const char *needle) {
    long m = (long)strlen(needle);
    for (long i = 0; i + m <= n; i++) {
        long k = 0;
        while (k < m) {
            unsigned char a = hay[i+k], b = (unsigned char)needle[k];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
            k++;
        }
        if (k == m) return 1;
    }
    return 0;
}

/* Classify a 0x80134010 mesh by the material name in its first 0x134011 leaf.
 * Names look like TRN_ROADA_CHOP_*, TRN_TERRAINA_*, XO_TRAFFICCONE_* ... */
/* The semantic asset-name classifier, declared early so the category test can
 * defer to it; defined with the rest of the scenery classification below. */
enum { N2_SC_NONE = 0, N2_SC_TERRAIN, N2_SC_BUILDING, N2_SC_PROP,
       N2_SC_TREE, N2_SC_WALL, N2_SC_STRUCT, N2_SC_OTHER };
static int n2_scen_class(const char *nm);

static int n2_mesh_category(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        /* neon signs, bulbs, lens flares: real light *emitters*, distinct
         * from opaque fixtures (StreetLightCbWALL, LightPoleB, TrafficLightC
         * are the pole/housing mesh — matching bare LIGHT/LAMP would also
         * catch those and wrongly make solid poles additive-transparent).
         * Checked case-insensitively: unlike SKY/ROAD/TERRAIN below, retail
         * casing for these is inconsistent per-region (mixed vs. upper). */
        if (n2_icontains(p, s, "NEON") || n2_icontains(p, s, "GLOW") ||
            n2_icontains(p, s, "FLARE") || n2_icontains(p, s, "BULB"))
            return N2_GLOW;
        for (long i = 0; i + 5 < s; i++) {
            /* start of an uppercase A-Z name run */
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    const unsigned char *n = p + i; long L = j - i;
                    /* Matching "SKY" as a substring also catches the stadium,
                       whose texture is named for the sky it reflects but which
                       is a building: it then joins the sky pass, is drawn with
                       the sky's matrix and far plane, and blacks out half the
                       screen. The dome is a 101-vertex mesh thousands of
                       metres up; the stadium sits at ground level. The dome's
                       name is exactly SKYDOME. */
                    if ((L == 7 && n2_contains(n, L, "SKYDOME"))
                        || (L > 4 && n[0]=='S' && n[1]=='K' && n[2]=='Y' && n[3]=='_'))
                        return N2_SKY;
                    if (n2_contains(n, L, "ROAD")) return N2_ROAD;
                    if (n2_contains(n, L, "TERRAIN")) return N2_TERRAIN;
                    /* RDP_ is the shipped road-paint/pavement family: RDP_LANEA,
                       RDP_ROADSTRIP, RDP_AIRPORT_* are the textures real roads
                       already wear, and TRN_RDP_RUNWAY_/TRN_RDP_DRAG_ are the
                       paved runway and drag surfaces. They carry no "ROAD" token
                       in the name, so the literal test above left them TERRAIN
                       and M114 gave a paved runway dirt handling (L4RB peak 161
                       -> 81 km/h). Paved is a road surface; the generic TRN_
                       ground below is not. */
                    if (L >= 7 && !memcmp(n, "TRN_RDP", 7)) return N2_ROAD;
                    /* The airport apron the runway feeds into: paved concrete,
                       driven continuously with the runway (measured: L4RB left
                       TRN_RDP_RUNWAY at f302 straight onto
                       TRN_CONCRETE_01_CHOP_A3_4 and dropped into the dirt
                       profile mid-apron). Concrete slabs only -- TRN_FOUNDATION_,
                       TRN_GRASS_, TRN_TRAINTRACKS_ and every unknown ground stay
                       terrain. */
                    if (L >= 12 && !memcmp(n, "TRN_CONCRETE", 12)) return N2_ROAD;
                    /* Ground whose asset name does not spell "TERRAIN". L4RB's
                       ground is TRN_RDP_RUNWAY_/TRN_GRASS_/TRN_CONCRETE_/
                       TRN_FOUNDATION_/TRN_TRAINTRACKS_, so the literal test left
                       it N2_OTHER -- invisible to world_ground_z, which then
                       returns the caller's own Z. Measured: the car drove at
                       z -12.087 while TRN_RDP_RUNWAY_KT_CHOP_A2_3 lay at z 0.009
                       directly beneath it, i.e. 12.1 m UNDER the airport, which
                       is why the terminal read as a detached prop overhead and
                       the world read as an empty plane. Defer to the SAME
                       semantic classifier the loader already tags meshes with
                       instead of adding another name rule; ROAD and SKY keep
                       their precedence above. */
                    { char sn[40]; int cl = (int)(L < 39 ? L : 39);
                      memcpy(sn, n, (size_t)cl); sn[cl] = 0;
                      if (n2_scen_class(sn) == N2_SC_TERRAIN) return N2_TERRAIN; }
                    return N2_OTHER;
                }
                i = j;
            }
        }
    }
    return N2_OTHER;
}

/* Extract one (vertex,index) leaf pair. stride = 24 (scenery, uv@16) or 36
 * (car, normal@12 uv@24). cull_skybox drops huge shells (tracks only). */
/* Widest authored coordinate in the shipped bundles is ~13 km; the corrupt
   values retail leaves behind measure ~1e38. 60000 separates them by 33 orders
   of magnitude and matches the existing absurd-span guard below. */
#define N2_VERT_SANE 60000.0f
static void n2_add_pair(const unsigned char *d, N2Leaf vtx, N2Leaf idx,
                        int cat, N2Scene *scene,
                        int stride, int uvoff, int cull_skybox, uint32_t texkey,
                        const float *mtx, long istart, long icount) {
    const unsigned char *vb = d + vtx.off;
    int vlen = (int)vtx.size;
    int pad = n2_skip_filler(vb, vlen);
    int body = vlen - pad;
    if (body <= 0 || body % stride) return;
    int n = body / stride;
    const unsigned char *rec = vb + pad;

    /* Per-vertex sanity (M133). Retail leaves a few vertices uninitialised --
       NaN, or a ~1e38 coordinate -- inside otherwise perfectly good objects.
       The old guard measured the bbox over ALL vertices and threw the WHOLE
       object away when any one of them was corrupt, which silently deleted 150
       shipped L4RA objects and 14 on L4RB: every XS_WARN* road sign, every
       XS_MEDIANPOLE* median pole, the XS_SPEED* speed signs and the
       XO_AIRPORT_*SIGN set. They are real geometry at real authored transforms
       and only a handful of their vertices are broken.
       Mark the broken vertices instead. Triangles that reference one are
       dropped below; the rest of the object is emitted normally, so the sign
       survives and the garbage spike never reaches the GPU. */
    unsigned char vbad_small[512];
    unsigned char *vbad = n <= (int)sizeof vbad_small
                        ? vbad_small : (unsigned char *)malloc((size_t)n);
    if (!vbad) return;
    memset(vbad, 0, (size_t)n);
    int nbad = 0;
    if (cull_skybox) {
        float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
        for (int i = 0; i < n; i++) {
            int corrupt = 0;
            for (int c = 0; c < 3; c++) {
                float v; memcpy(&v, rec + i*stride + c*4, 4);
                /* NaN fails every comparison; VERT_SANE is far outside anything
                   the shipped world authors (its widest asset spans ~13 km) and
                   far below the ~1e38 uninitialised values actually observed. */
                if (v != v || v > N2_VERT_SANE || v < -N2_VERT_SANE) corrupt = 1;
            }
            if (corrupt) { vbad[i] = 1; nbad++; continue; }
            for (int c = 0; c < 3; c++) {
                float v; memcpy(&v, rec + i*stride + c*4, 4);
                if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v;
            }
        }
#ifdef N2_NO_VERTEX_REPAIR
        /* A/B control build: the pre-M133 rule -- one corrupt vertex discards
           the whole object. Used only to produce same-pose before captures. */
        if (nbad) { if (vbad != vbad_small) free(vbad); return; }
#endif
        if (nbad == n) { if (vbad != vbad_small) free(vbad); return; }
        float span = 0;
        for (int c = 0; c < 3; c++) if (mx[c]-mn[c] > span) span = mx[c]-mn[c];
        /* the skybox is dropped by material name now; keep only an absurd-span
           safety, measured over the SANE vertices, so big city ground and
           buildings (thousands of units across) still draw. */
        if (span >= 60000.0f) { if (vbad != vbad_small) free(vbad); return; }
    }

    N2Mesh m; memset(&m, 0, sizeof(m));
    m.cat = cat; m.texkey = texkey; m.nverts = n;
    m.verts = (float *)malloc((size_t)n * 5 * sizeof(float));
    /* World stream (24B stride) packs an RGBA8 prelight colour between position
       and UV (pos@0, colour@12, uv@16). Car stream (36B) has no such slot. */
    int coloff = (stride == 24) ? 12 : -1;
    if (coloff >= 0) m.vcol = (unsigned char *)malloc((size_t)n * 4);
    /* M133-R: every allocation this function owns is checked before use, and
       every early return below frees exactly what was owned at that point —
       a partially built mesh is never pushed to the scene. */
    if (!m.verts || (coloff >= 0 && !m.vcol)) {
        free(m.verts); free(m.vcol);
        if (vbad != vbad_small) free(vbad);
        return;
    }
    for (int i = 0; i < n; i++) {
        float px, py, pz;
        memcpy(&px, rec+i*stride,   4);
        memcpy(&py, rec+i*stride+4, 4);
        memcpy(&pz, rec+i*stride+8, 4);
        if (mtx) {   /* place local-space geometry into the world (track props) */
            m.verts[i*5+0] = px*mtx[0]+py*mtx[4]+pz*mtx[8] +mtx[12];
            m.verts[i*5+1] = px*mtx[1]+py*mtx[5]+pz*mtx[9] +mtx[13];
            m.verts[i*5+2] = px*mtx[2]+py*mtx[6]+pz*mtx[10]+mtx[14];
        } else { m.verts[i*5+0]=px; m.verts[i*5+1]=py; m.verts[i*5+2]=pz; }
        memcpy(m.verts + i*5 + 3, rec + i*stride + uvoff, 8);
        if (m.vcol) memcpy(m.vcol + i*4, rec + i*stride + coloff, 4);
    }
    /* the index leaf carries the same 0x11 filler prefix as the vertex leaf
       (as whole 0x1111 u16 words). Skipping it is essential: when the filler is
       not a multiple of 6 bytes (e.g. 4 or 8), leaving it in shifts the triangle
       grouping and shears every mesh — car wheels into urchins, buildings into
       loose planes. */
    const unsigned char *ib0 = d + idx.off; int ibytes = (int)idx.size, ip = 0;
    while (ip + 2 <= ibytes && ib0[ip] == 0x11 && ib0[ip+1] == 0x11) ip += 2;
    const unsigned char *ib = ib0 + ip;
    int nidx = (ibytes - ip) / 2;
    /* icount >= 0 restricts this draw to one 0x134B02 submesh's slice of the
       index buffer (its own material). Ranges are in u16s past the filler.
       m.verts/m.vcol are already allocated and filled by this point, so a
       rejection here must free them explicitly rather than just returning. */
    if (icount >= 0) {
        if (istart < 0 || istart >= nidx) {
            free(m.verts); free(m.vcol);
            if (vbad != vbad_small) free(vbad);
            return;
        }
        if (istart + icount > nidx) icount = nidx - istart;
        if (icount < 3) {
            free(m.verts); free(m.vcol);
            if (vbad != vbad_small) free(vbad);
            return;
        }
        ib += istart * 2;
        nidx = (int)icount;
    }
    m.idx = (uint16_t *)malloc((size_t)nidx * sizeof(uint16_t));
    if (!m.idx) {
        free(m.verts); free(m.vcol);
        if (vbad != vbad_small) free(vbad);
        return;
    }
    m.nidx = 0;
    for (int i = 0; i + 2 < nidx; i += 3) {
        uint16_t a = (uint16_t)(ib[i*2]     | ib[i*2+1]     << 8);
        uint16_t b = (uint16_t)(ib[(i+1)*2] | ib[(i+1)*2+1] << 8);
        uint16_t c = (uint16_t)(ib[(i+2)*2] | ib[(i+2)*2+1] << 8);
        if (a < n && b < n && c < n && !vbad[a] && !vbad[b] && !vbad[c]) {
            m.idx[m.nidx++] = a; m.idx[m.nidx++] = b; m.idx[m.nidx++] = c;
        }
    }
    if (m.nidx == 0) {
        free(m.verts); free(m.idx); free(m.vcol);
        if (vbad != vbad_small) free(vbad);
        return;
    }
    /* M133-R: repair using the ORIGINAL per-source-vertex mask, still alive —
       not rediscovered from m.verts[i*5] (transformed X only), which is blind
       to a corrupt Y or Z and would let it through into the mesh bounds, the
       ground grid and the GPU batch. A corrupt vertex is left in the array
       (indices reference it) but no triangle uses it; park it on the first
       vertex the mask itself calls good, so the bbox/grid/batch never see the
       spike. nbad == n was already rejected above, so a good vertex exists. */
    m.vrepair = nbad ? 1 : 0;
    if (nbad) {
        static const char *pr2 = (const char *)1;
        if (pr2 == (const char *)1) pr2 = getenv("OPENUG2_REPAIR_PROBE");
        if (pr2) fprintf(stderr, "REPAIRED verts %d bad %d tris %d at (%.1f %.1f %.1f)\n",
                         n, nbad, m.nidx/3, mtx?mtx[12]:0.0f, mtx?mtx[13]:0.0f,
                         mtx?mtx[14]:0.0f);
        int good = -1;
        for (int i = 0; i < n; i++) if (!vbad[i]) { good = i; break; }
        if (good >= 0)
            for (int i = 0; i < n; i++)
                if (vbad[i]) memcpy(m.verts + i*5, m.verts + good*5, 3*sizeof(float));
    }
    if (vbad != vbad_small) free(vbad);
    n2_push_mesh(scene, m);
}

/* Pick a track mesh's diffuse key from its 0x134012 slot list. Road/terrain use
 * the LAST slot (their own surface texture; missing -> asphalt/grass fallback,
 * never an earlier grass base slot). Props/buildings prefer the last slot that
 * actually RESOLVES (is in `keys`) — a building often lists a detail slot last
 * that lives in an unshipped pack, with the real facade in an earlier slot. */
static uint32_t n2_mesh_texkey_cat(const unsigned char *d, long beg, long end,
                               int cat, const uint32_t *keys, int nkeys) {
    N2Leaf t12[4]; int n12 = 0;
    n2_find_leaves(d, beg, end, 0x00134012u, t12, &n12, 4);
    uint32_t last = 0, lastres = 0;
    for (int a = 0; a < n12; a++) {
        const unsigned char *p = d + t12[a].off; long ls = t12[a].size;
        for (long b = 0; b + 4 <= ls; b += 4) {
            uint32_t v = n2_u32(p + b); if (!v) continue;
            last = v;
            for (int c = 0; c < nkeys; c++) if (keys[c] == v) { lastres = v; break; }
        }
    }
    if (cat == N2_ROAD || cat == N2_TERRAIN) return last;
    return lastres ? lastres : last;
}

/* A 0x80134010 object's 4x4 world transform, from its 0x134011 header (after
 * that header's own 0x11 filler): row-major, translation in row 3 (+0x40..+0x7f).
 * Track props are modelled in local space and placed by this matrix — without it
 * every building piles at the origin. Fills identity + returns 0 on miss. */
static int n2_obj_matrix(const unsigned char *d, long beg, long end, float *m) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    N2Leaf h[2]; int nh = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, h, &nh, 2);
    if (!nh) return 0;
    const unsigned char *p = d + h[0].off; int s = (int)h[0].size;
    int pad = n2_skip_filler(p, s);
    if (pad + 0x40 + 64 > s) return 0;
    for (int i = 0; i < 16; i++) memcpy(&m[i], p + pad + 0x40 + i*4, 4);
    if (m[15] < 0.5f || m[15] > 1.5f) {          /* not a sane affine matrix */
        for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return 0;
    }
    return 1;
}

/* Walk to every 0x80134010 mesh, classify it, extract its vtx/idx pairs. `keys`
 * is the set of texture keys resolvable for this region (local TPK + shared
 * pack), used to pick each mesh's diffuse slot. */
/* Scenery semantics (Phase 65). EVERY track mesh object carries its own asset
 * name in its 0x134011 material chunk -- verified: 10735 of 10735 objects in
 * STREAML4RA are named. So each mesh gets an EXACT name, read straight from its
 * own chunk. (A spatial join against the 0x80034100 solid bboxes was tried and
 * rejected: those bboxes are huge overlapping chunk regions, so 9507 of 10475
 * meshes were claimed by >1 solid -- avg 5.2 -- which would mislabel collisions.)
 * Prefix census for L4RA: TRN 4248, XB 2405, XO 1458, XS 759, XW 739, XT 454,
 * ZPM 166, PAN 51, UC 43, XV 32. */
static int n2_scen_class(const char *nm) {
    if (!nm || !nm[0]) return N2_SC_NONE;
    if (!strncmp(nm, "TRN", 3) || !strncmp(nm, "PAN", 3)) return N2_SC_TERRAIN;
    if (!strncmp(nm, "XB",  2)) return N2_SC_BUILDING;   /* buildings/barriers */
    if (!strncmp(nm, "XO",  2)) return N2_SC_PROP;       /* poles, cans, barrels */
    if (!strncmp(nm, "XT",  2)) return N2_SC_TREE;
    if (!strncmp(nm, "XW",  2)) return N2_SC_WALL;       /* walls / fences */
    if (!strncmp(nm, "XS",  2) || !strncmp(nm, "XV", 2)) return N2_SC_STRUCT;
    return N2_SC_OTHER;
}
static const char *n2_scen_name(int sc) {
    static const char *n[] = { "-", "TERRAIN", "BUILDING", "PROP",
                               "TREE", "WALL", "STRUCT", "OTHER" };
    return (sc >= 0 && sc <= N2_SC_OTHER) ? n[sc] : "?";
}

/* Asset name of a track mesh object, from its own 0x134011 chunk. */
static void n2_mesh_name(const unsigned char *d, long beg, long end,
                         char *out, int cap) {
    out[0] = 0;
    N2Leaf mt[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mt, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mt[k].off; long s = mt[k].size;
        for (long i = 0; i + 4 < s; i++) {
            int ch = p[i];
            if ((ch>='A'&&ch<='Z') || (ch>='a'&&ch<='z')) {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') ||
                                 (p[j]>='a'&&p[j]<='z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) { int L = (int)(j-i); if (L > cap-1) L = cap-1;
                    memcpy(out, p+i, L); out[L] = 0; return; }
                i = j;
            }
        }
    }
}

/* ---- --vista-census (Milestone 76): measured backdrop-impostor evidence ----
 * Same traversal boundaries as n2_walk_meshes. For every object it measures the
 * world-space geometry the renderer would actually draw, so impostor detection
 * can rest on measurement instead of spelling. Diagnostic; the classifier below
 * is what the parser consults. */
typedef struct {
    float xyspan, zspan;      /* world AABB extents */
    long  tris;
    float dom[3];             /* area-weighted mean normal, normalized */
    float planarity;          /* area-weighted mean |dot(n_i, dom)|: 1 = one flat sheet */
    float area;
} N2Geom;

/* Measure one 0x80134010 object exactly as n2_walk_meshes would build it:
 * every 0x134B01/0x134B03 leaf pair, stride 24, placed by the object matrix. */
static int n2_obj_geom(const unsigned char *d, long beg, long end,
                       const float *m, N2Geom *g) {
    memset(g, 0, sizeof *g);
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    double nsum[3] = {0,0,0}, asum = 0;
    N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
    n2_find_leaves(d, beg, end, 0x00134B01u, vtx, &nv, 64);
    n2_find_leaves(d, beg, end, 0x00134B03u, idx, &ni, 64);
    int pairs = nv < ni ? nv : ni;
    if (!pairs) return 0;

    /* two passes over each pair: positions first (bbox), then triangles */
    for (int k = 0; k < pairs; k++) {
        const unsigned char *vb = d + vtx[k].off;
        int pad = n2_skip_filler(vb, (int)vtx[k].size), body = (int)vtx[k].size - pad;
        if (body <= 0 || body % 24) continue;
        int n = body / 24; const unsigned char *rec = vb + pad;
        float *wp = (float *)malloc((size_t)n * 3 * sizeof(float));
        if (!wp) continue;
        for (int i = 0; i < n; i++) {
            float px, py, pz;
            memcpy(&px, rec+i*24,   4);
            memcpy(&py, rec+i*24+4, 4);
            memcpy(&pz, rec+i*24+8, 4);
            wp[i*3+0] = px*m[0]+py*m[4]+pz*m[8] +m[12];
            wp[i*3+1] = px*m[1]+py*m[5]+pz*m[9] +m[13];
            wp[i*3+2] = px*m[2]+py*m[6]+pz*m[10]+m[14];
            for (int c = 0; c < 3; c++) {
                if (wp[i*3+c] < mn[c]) mn[c] = wp[i*3+c];
                if (wp[i*3+c] > mx[c]) mx[c] = wp[i*3+c];
            }
        }
        const unsigned char *ib0 = d + idx[k].off;
        int ibytes = (int)idx[k].size, ip = 0;
        while (ip + 2 <= ibytes && ib0[ip] == 0x11 && ib0[ip+1] == 0x11) ip += 2;
        const unsigned char *ib = ib0 + ip;
        int nidx = (ibytes - ip) / 2;
        for (int t = 0; t + 2 < nidx; t += 3) {
            unsigned a = (unsigned)(ib[t*2] | ib[t*2+1] << 8);
            unsigned b = (unsigned)(ib[(t+1)*2] | ib[(t+1)*2+1] << 8);
            unsigned c = (unsigned)(ib[(t+2)*2] | ib[(t+2)*2+1] << 8);
            if ((int)a >= n || (int)b >= n || (int)c >= n) continue;
            float e1[3], e2[3], nr[3];
            for (int j = 0; j < 3; j++) { e1[j] = wp[b*3+j]-wp[a*3+j];
                                          e2[j] = wp[c*3+j]-wp[a*3+j]; }
            nr[0] = e1[1]*e2[2]-e1[2]*e2[1];
            nr[1] = e1[2]*e2[0]-e1[0]*e2[2];
            nr[2] = e1[0]*e2[1]-e1[1]*e2[0];
            float len = sqrtf(nr[0]*nr[0]+nr[1]*nr[1]+nr[2]*nr[2]);
            if (len < 1e-9f) continue;                 /* degenerate sliver */
            float ar = len * 0.5f;
            for (int j = 0; j < 3; j++) nsum[j] += (double)(nr[j]/len) * ar;
            asum += ar; g->tris++;
        }
        free(wp);
    }
    if (mn[0] > mx[0] || asum <= 0) return 0;
    float sx = mx[0]-mn[0], sy = mx[1]-mn[1];
    g->xyspan = sx > sy ? sx : sy;
    g->zspan  = mx[2]-mn[2];
    g->area   = (float)asum;
    double dl = sqrt(nsum[0]*nsum[0]+nsum[1]*nsum[1]+nsum[2]*nsum[2]);
    if (dl < 1e-12) return 1;                          /* normals cancel: a shell */
    for (int j = 0; j < 3; j++) g->dom[j] = (float)(nsum[j]/dl);
    /* |mean normal| / total area is already the area-weighted coherence: a single
       flat sheet keeps its full length, a closed shell cancels to ~0. */
    g->planarity = (float)(dl / asum);
    return 1;
}

/* Confirmed vista-impostor families, measured across all eight shipped bundles
 * (see --vista-census). These are the backdrop asset families, NOT a substring
 * net: each is anchored so it cannot swallow an ordinary asset that merely
 * contains the letters. PANARAMA is retail's own misspelling of "panorama". */
static int n2_vista_family(const char *nm) {
    /* NOT PAN_: those keep their own unconditional cull in n2_walk_meshes. The
       census proves 27 of the 38 PAN_ assets are small modelled backdrop blocks
       (spans 69..2265 m, planarity down to 0.005 -- closed shells, not sheets),
       so routing them through a size test would put them all back in the world. */
    if (!strncmp(nm, "TRN_PANARAMA", 12)) return 1;           /* L4RB/L4RG naming */
    { size_t L = strlen(nm);                                  /* ..._WORLD_LOD tail */
      if (L >= 10 && !strcmp(nm + L - 10, "_WORLD_LOD")) return 1; }
    return 0;
}

/* A backdrop impostor is a confirmed-family asset that ALSO measures as a giant
 * billboard: city-scale footprint, a real vertical wall of geometry, and near-
 * planar (one dominant normal). Both halves are required -- family alone would
 * trust spelling, geometry alone would eat legitimate large terrain sheets. */
#define N2_VISTA_XY     3000.0f   /* footprint larger than any real ground sheet */
#define N2_VISTA_Z       300.0f   /* a backdrop is a tall wall, terrain is not */
/* Measured: the three leaking backdrops sit at 0.707/0.759/0.904. The nearest
   non-family object at this scale is SKYDOME at 0.637 (a closed shell, whose
   normals cancel), so 0.65 separates sheet from shell with the sample's own gap. */
#define N2_VISTA_PLANAR    0.65f
static int n2_is_vista_impostor(const char *nm, const N2Geom *g) {
    return n2_vista_family(nm) && g->xyspan >= N2_VISTA_XY &&
           g->zspan >= N2_VISTA_Z && g->planarity >= N2_VISTA_PLANAR;
}

/* ---- M102 fallback census -------------------------------------------------
 * Records WHY each ROAD/TERRAIN object took the per-submesh path or the old
 * single-last-slot fallback. Pure bookkeeping: it reads the same values the
 * validation already computed and never influences emission. Off unless
 * n2_m102 is set before the load. */
enum { N2_FB_OK = 0, N2_FB_NOREC, N2_FB_MULTILEAF, N2_FB_MALFORMED,
       N2_FB_BADSLOT, N2_FB_NCAT };
typedef struct {
    char     name[32];
    int      cat, nslot, nsub, nleaf, ntri;
    uint32_t key, slot0;
    float    bb[6];               /* x0 x1 y0 y1 z0 z1, world space */
    long     b02off; uint32_t b02size;   /* M104: the raw leaves, for stride probing */
    long     b03off; uint32_t b03size;
    long     avail;               /* usable u16 indices in B03, post-filler */
    uint32_t slots[16];
} N2FbRec;
static int  n2_m102 = 0;
static long n2_m102_obj[N2_FB_NCAT], n2_m102_tri[N2_FB_NCAT], n2_m102_idx[N2_FB_NCAT];
#define N2_FB_MAXS 16
static N2FbRec n2_m102_s[N2_FB_NCAT][N2_FB_MAXS];
static int  n2_m102_ns[N2_FB_NCAT];
static long n2_m102_risk[N2_FB_NCAT], n2_m102_risktri[N2_FB_NCAT];
static long n2_m102_bad[3];   /* cat5 split: 0 mat_id range, 1 empty slot, 2 unresolved key */
static long n2_m102_rng_res, n2_m102_rng_unres;   /* emitted ranges, by key source */
static void n2_m102_note2(N2FbRec *r, const unsigned char *d, long beg, long end,
                          const uint32_t *slot, int nslot, N2Leaf i0) {
    N2Leaf sm[8]; int nsm = 0;
    n2_find_leaves(d, beg, end, 0x00134B02u, sm, &nsm, 8);
    r->b02off = nsm ? sm[0].off : -1; r->b02size = nsm ? sm[0].size : 0;
    r->b03off = i0.off; r->b03size = i0.size;
    const unsigned char *ib0 = d + i0.off; int ib = (int)i0.size, ip = 0;
    while (ip + 2 <= ib && ib0[ip] == 0x11 && ib0[ip+1] == 0x11) ip += 2;
    r->avail = (ib - ip) / 2;
    for (int k = 0; k < 16; k++) r->slots[k] = k < nslot ? slot[k] : 0;
}
static void n2_m102_note(int why, const char *anm, int cat, int nslot, int nsub,
                         int nleaf, long nidx, uint32_t key, uint32_t slot0,
                         const unsigned char *d, N2Leaf v0, const float *mtx) {
    n2_m102_obj[why]++; n2_m102_idx[why] += nidx; n2_m102_tri[why] += nidx / 3;
    if (nslot > 1 && key != slot0) { n2_m102_risk[why]++; n2_m102_risktri[why] += nidx/3; }
    if (n2_m102_ns[why] >= N2_FB_MAXS) return;
    N2FbRec *r = &n2_m102_s[why][n2_m102_ns[why]++];
    snprintf(r->name, sizeof r->name, "%.31s", anm);
    r->cat = cat; r->nslot = nslot; r->nsub = nsub; r->nleaf = nleaf;
    r->ntri = (int)(nidx / 3); r->key = key; r->slot0 = slot0;
    r->bb[0]=r->bb[2]=r->bb[4]= 1e30f;
    r->bb[1]=r->bb[3]=r->bb[5]=-1e30f;
    const unsigned char *vb = d + v0.off; int vlen = (int)v0.size;
    int pad = n2_skip_filler(vb, vlen);
    const unsigned char *rec = vb + pad; int n = (vlen - pad) / 24;
    for (int i = 0; i < n; i++) {
        float px, py, pz;
        memcpy(&px, rec + i*24 + 0, 4); memcpy(&py, rec + i*24 + 4, 4);
        memcpy(&pz, rec + i*24 + 8, 4);
        float wx = px*mtx[0]+py*mtx[4]+pz*mtx[8] +mtx[12];
        float wy = px*mtx[1]+py*mtx[5]+pz*mtx[9] +mtx[13];
        float wz = px*mtx[2]+py*mtx[6]+pz*mtx[10]+mtx[14];
        if (wx<r->bb[0])r->bb[0]=wx; if (wx>r->bb[1])r->bb[1]=wx;
        if (wy<r->bb[2])r->bb[2]=wy; if (wy>r->bb[3])r->bb[3]=wy;
        if (wz<r->bb[4])r->bb[4]=wz; if (wz>r->bb[5])r->bb[5]=wz;
    }
}

/* REPLACEABLE SLOTS. Two materials are not fixed by the model: the paint
   (CARSKIN) and the glass (WINDSHIELD) are substituted at runtime from the
   player's choices. That is also why the second word of every material record
   is zero in the file -- it is a slot for a material pointer filled in later. */
#define N2_MAT_CARSKIN    0xd6d6080au
#define N2_MAT_WINDSHIELD 0x471a1dcau

/* Selected body paint: material hash, 0 = the silver default. */
static uint32_t n2_carskin_mat = 0;

/* Declared here because n2_walk_meshes' per-submesh road path (M101) needs
 * them; the definitions stay with the car material code further down. */
typedef struct { uint32_t start, count, mat, matid; } N2Sub;
static int n2_mesh_submeshes(const unsigned char *d, long beg, long end,
                             N2Sub *out, int cap);
static int n2_mesh_texslots(const unsigned char *d, long beg, long end,
                            uint32_t *out, int cap);
static uint32_t n2_resolve_key(uint32_t v, const uint32_t *keys, int nkeys);

/* M132: when set, vista/LOD impostors are EMITTED into this scene instead of
 * being dropped on the floor. They still never reach the ordinary world scene,
 * so ground, collision, navigation and spawn selection are untouched -- those
 * all query the world scene. NULL restores the old discard exactly. */
static N2Scene *n2_vista_out = NULL;
static long n2_vista_objs = 0, n2_vista_pan = 0, n2_vista_fam = 0;
/* M133 object-level accounting: every 0x80134010 the walk sees ends in exactly
   one bucket, so "source object" -> "scene mesh" can be reconciled. */
static long n2_obj_seen = 0, n2_obj_vista = 0, n2_obj_emit = 0, n2_obj_nomesh = 0,
            n2_obj_nopair = 0;

static void n2_walk_meshes(const unsigned char *d, long beg, long end, N2Scene *scene0,
                           const uint32_t *keys, int nkeys) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o), size = n2_u32(d + o + 4);
        long ds = o + 8;
        if (magic == 0x80134010u) {
            N2Scene *scene = scene0;   /* redirected below for vista impostors */
            n2_obj_seen++;
            int obj_before = scene0->count;
            int cat = n2_mesh_category(d, ds, ds + size);
            char anm[40]; n2_mesh_name(d, ds, ds + size, anm, sizeof anm);
            int sc = n2_scen_class(anm);
            /* PAN_ objects are panorama vista impostors: huge flat backdrop
               billboards (spans to ~4300, placed at the map edge / below ground,
               Z down to -955) that retail draws only as a far horizon ring. Drawn
               as solid world geometry they loom through the city as giant angled
               planes -- the "scattered planes / explosion". The buildings and
               terrain they back are placed correctly (verified: transformed
               centroids form the coherent city that matches the nav graph), so
               cull only the impostors. ponytail: cull, not a backdrop-ring pass --
               re-add far-plane billboards if the empty horizon ever matters. */
            if (!strncmp(anm, "PAN", 3)) {
                if (!n2_vista_out) { n2_obj_vista++; o = ds + size; continue; }  /* dropped */
                scene = n2_vista_out; n2_vista_objs++; n2_vista_pan++;
            }
            uint32_t tk = n2_mesh_texkey_cat(d, ds, ds + size, cat, keys, nkeys);
            float objm[16]; n2_obj_matrix(d, ds, ds + size, objm);   /* world placement */
            { /* M133 diagnostic: how many 0x134011 headers does this object
                 carry, and what translation does each one hold? Name matching
                 is diagnostic only and never reaches production behaviour. */
              static const char *probe = (const char *)1;
              if (probe == (const char *)1) probe = getenv("OPENUG2_OBJ_PROBE");
              if (probe && anm[0] && strstr(anm, probe)) {
                  N2Leaf hh[8]; int nhh = 0;
                  n2_find_leaves(d, ds, ds + size, 0x00134011u, hh, &nhh, 8);
                  fprintf(stderr, "OBJPROBE %-30s headers=%d  used T=(%.3f %.3f %.3f)\n",
                          anm, nhh, objm[12], objm[13], objm[14]);
                  N2Leaf sl[4]; int nsl = 0;
                  n2_find_leaves(d, ds, ds + size, 0x00134012u, sl, &nsl, 4);
                  for (int q = 0; q < nsl; q++) {
                      const unsigned char *pp = d + sl[q].off; long ls = sl[q].size;
                      fprintf(stderr, "OBJPROBE   slot-leaf %d size %ld words:", q, ls);
                      for (long b = 0; b + 4 <= ls && b < 64; b += 4)
                          fprintf(stderr, " %08x", n2_u32(pp + b));
                      fprintf(stderr, "\n");
                  }
                  for (int q = 0; q < nhh; q++) {
                      const unsigned char *pp = d + hh[q].off; int ss = (int)hh[q].size;
                      int pd = n2_skip_filler(pp, ss);
                      if (pd + 0x40 + 64 > ss) { fprintf(stderr, "OBJPROBE   leaf %d size %d: too small\n", q, ss); continue; }
                      float t[4];
                      for (int c = 0; c < 4; c++) memcpy(&t[c], pp + pd + 0x40 + (12+c)*4, 4);
                      fprintf(stderr, "OBJPROBE   leaf %d size %5d T=(%.3f %.3f %.3f) w=%.3f\n",
                              q, ss, t[0], t[1], t[2], t[3]);
                  }
              } }
            /* Backdrop impostors the PAN_ prefix above does not catch: retail
               names the same job TRN_PANARAMA* / *_WORLD_LOD in some bundles, so
               they were reaching the world as ordinary opaque TERRAIN and walling
               the camera in (Milestone 75). Family test first -- it is free, and
               the geometry measure below allocates. */
            if (scene == scene0 && n2_vista_family(anm)) {
                N2Geom vg;
                if (n2_obj_geom(d, ds, ds + size, objm, &vg) &&
                    n2_is_vista_impostor(anm, &vg)) {
                    if (!n2_vista_out) { n2_obj_vista++; o = ds + size; continue; }  /* dropped */
                    scene = n2_vista_out; n2_vista_objs++; n2_vista_fam++;
                }
            }
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + size, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + size, 0x00134B03u, idx, &ni, 64);
            int pairs = nv < ni ? nv : ni;
            /* the skybox shell is kept now (Phase 21: rendered camera-locked,
               depth-write off) so its absurd span must skip the size-safety
               cull that still guards every other category. */
            int cull = (cat != N2_SKY);
            /* Per-submesh materials for road/terrain (Milestone 101). A road
             * object lists SEVERAL 0x134012 slots and its single 0x134B02 leaf
             * partitions the index buffer into ranges, each naming its slot by
             * mat_id -- the same linkage the car path already uses. Binding one
             * key per object (the last slot) put a 64x64 intersection tile over
             * a whole carriageway whose geometry is 91% lane/shoulder strips
             * (M100, TRN_SH_ROADA_CHOP_A13_R7: mat_id 2 owns 9 of 105 indices).
             * Emit one mesh per range instead. Anything that does not verify --
             * no records, several index leaves, a mat_id out of range, an empty
             * or unresolved slot, or ranges that do not chain from 0 -- falls
             * straight through to the unchanged single-mesh path below. */
            int sub_ok = 0;
            N2Sub sub[64]; int nsub = 0;
            uint32_t slot[64]; int nslot = 0;
            int fb_why = -1; long fb_idx = 0, fb_chain = 0;   /* M102 census only */
            if ((cat == N2_ROAD || cat == N2_TERRAIN) && pairs == 1) {
                nsub  = n2_mesh_submeshes(d, ds, ds + size, sub, 64);
                nslot = n2_mesh_texslots(d, ds, ds + size, slot, 64);
                if (nsub > 0 && nslot > 0) {
                    const unsigned char *ib0 = d + idx[0].off;
                    int ibytes = (int)idx[0].size, ip = 0;
                    while (ip + 2 <= ibytes && ib0[ip] == 0x11 && ib0[ip+1] == 0x11) ip += 2;
                    long avail = (ibytes - ip) / 2;
                    sub_ok = 1;
                    long chain = 0;
                    for (int a = 0; a < nsub && sub_ok; a++) {
                        if (sub[a].mat >= (uint32_t)nslot)
                            { sub_ok = 0; fb_why = N2_FB_BADSLOT; if (n2_m102) n2_m102_bad[0]++; }
                        else if (!slot[sub[a].mat])
                            { sub_ok = 0; fb_why = N2_FB_BADSLOT; if (n2_m102) n2_m102_bad[1]++; }
                        else if ((long)sub[a].start != chain)
                            { sub_ok = 0; fb_why = N2_FB_MALFORMED; }   /* contiguous */
                        else if (sub[a].count < 3) { sub_ok = 0; fb_why = N2_FB_MALFORMED; }
                        else if (chain + (long)sub[a].count > avail)
                            { sub_ok = 0; fb_why = N2_FB_MALFORMED; }
                        else chain += (long)sub[a].count;
                    }
                    /* The records partition every WHOLE triangle; the leaf may
                       carry one spare u16 of alignment padding (measured: avail%3
                       is 0 or 1, never 2). Require the partition to end exactly
                       at the last whole triangle so a short chain can never
                       silently drop geometry. */
                    if (sub_ok && chain != avail - avail % 3)
                        { sub_ok = 0; fb_why = N2_FB_MALFORMED; }
                    fb_chain = chain;
                }
            }
            if (n2_m102 && (cat == N2_ROAD || cat == N2_TERRAIN)) {
                /* usable indices across every pair, post-filler */
                for (int k = 0; k < pairs; k++) {
                    const unsigned char *ib0 = d + idx[k].off;
                    int ibytes = (int)idx[k].size, ip = 0;
                    while (ip + 2 <= ibytes && ib0[ip] == 0x11 && ib0[ip+1] == 0x11) ip += 2;
                    fb_idx += (ibytes - ip) / 2;
                }
                int why = N2_FB_OK;
                if (sub_ok) why = N2_FB_OK;
                else if (fb_why >= 0) why = fb_why;
                else if (pairs != 1) why = N2_FB_MULTILEAF;
                else {
                    N2Leaf sm[8]; int nsm = 0;
                    n2_find_leaves(d, ds, ds + size, 0x00134B02u, sm, &nsm, 8);
                    why = (nsm == 0) ? N2_FB_NOREC
                        : (nsm  > 1) ? N2_FB_MULTILEAF : N2_FB_MALFORMED;
                }
                if (pairs > 0) {
                    int ns0 = n2_m102_ns[why];
                    n2_m102_note(why, anm, cat, nslot, nsub, pairs,
                                 sub_ok ? fb_chain : fb_idx, tk,
                                 nslot ? slot[0] : 0, d, vtx[0], objm);
                    if (n2_m102_ns[why] > ns0)
                        n2_m102_note2(&n2_m102_s[why][ns0], d, ds, ds + size,
                                      slot, nslot, idx[0]);
                }
            }
            if (sub_ok) {
                for (int a = 0; a < nsub; a++) {
                    int before = scene->count;
                    /* Resolvability is a PER-SUBMESH question (M103). A record's
                       own slot key wins whenever this region's TPK set can supply
                       it; a slot that lives in a pack we do not ship falls back to
                       the object's `last` key for THAT RANGE ONLY, which is
                       exactly what the whole object used to get. Geometry and the
                       index partition are identical either way -- only the key
                       differs -- so nothing is dropped or duplicated. */
                    uint32_t sk = n2_resolve_key(slot[sub[a].mat], keys, nkeys);
                    if (!sk) { sk = tk; if (n2_m102) n2_m102_rng_unres++; }
                    else if (n2_m102) n2_m102_rng_res++;
                    n2_add_pair(d, vtx[0], idx[0], cat, scene, 24, 16, cull,
                                sk, objm,
                                (long)sub[a].start, (long)sub[a].count);
                    for (int m2 = before; m2 < scene->count; m2++) {
                        scene->meshes[m2].scen = (unsigned char)sc;
                        snprintf(scene->meshes[m2].sname, sizeof scene->meshes[m2].sname,
                                 "%.31s", anm);
                    }
                }
            } else
            for (int k = 0; k < pairs; k++) {
                int before = scene->count;
                n2_add_pair(d, vtx[k], idx[k], cat, scene, 24, 16, cull, tk, objm, 0, -1);
                for (int m2 = before; m2 < scene->count; m2++) {   /* tag the new mesh */
                    scene->meshes[m2].scen = (unsigned char)sc;
                    snprintf(scene->meshes[m2].sname, sizeof scene->meshes[m2].sname,
                             "%.31s", anm);
                }
            }
            if (scene != scene0) n2_obj_vista++;          /* routed to the vista scene */
            else if (scene0->count > obj_before) n2_obj_emit++;
            else {
                n2_obj_nomesh++;
                if (!pairs) n2_obj_nopair++;
                static const char *pr = (const char *)1;
                if (pr == (const char *)1) pr = getenv("OPENUG2_EMPTY_PROBE");
                if (pr)
                    fprintf(stderr, "EMPTYOBJ %-30s cat %d scen %s leafpairs %d "
                            "T=(%.1f %.1f %.1f)\n", anm[0]?anm:"?", cat,
                            n2_scen_name(sc), pairs, objm[12], objm[13], objm[14]);
            }
        } else if (magic != 0 && (magic >> 28) == 8) {
            n2_walk_meshes(d, ds, ds + size, scene0, keys, nkeys);
        }
        o = ds + size;
    }
}

/* ---- --transform-audit (Milestone 74): GL-free placement forensics ----
 * Mirrors n2_walk_meshes' traversal boundaries exactly (same magics, same
 * recursion rule, same leaf finder, same 24B/uv@16 vertex convention) but
 * builds no scene: it prints each object's placement evidence so the current
 * transform convention can be checked against the alternatives. Diagnostic
 * only -- nothing here is on the render path. */
typedef struct {
    int quota[3];          /* remaining prints: 0 terrain/road, 1 XB, 2 prop */
    int nobj, nmtx;        /* objects seen / with a matrix that passed w-check */
    int n11_hist[5];       /* how many 0x134011 leaves each object owns (4+ = [4]) */
    int nested;            /* objects containing a nested 0x80134010 */
    int bycls[8], mtxbycls[8];
    int rot3x3, trans_only, ident;   /* 3x3 is rotated/scaled / pure translation / full identity */
    float maxdet;
} N2Audit;

static void n2_aabb_local(const unsigned char *d, N2Leaf v, float *mn, float *mx,
                          int *nvert, long *vtxoff) {
    mn[0]=mn[1]=mn[2]=1e30f; mx[0]=mx[1]=mx[2]=-1e30f; *nvert = 0;
    const unsigned char *vb = d + v.off;
    int pad = n2_skip_filler(vb, (int)v.size), body = (int)v.size - pad;
    *vtxoff = v.off + pad;
    if (body <= 0 || body % 24) return;
    int n = body / 24; const unsigned char *rec = vb + pad;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) {
            float f; memcpy(&f, rec + i*24 + c*4, 4);
            if (f < mn[c]) mn[c] = f; if (f > mx[c]) mx[c] = f;
        }
    *nvert = n;
}

/* AABB of the local box's 8 corners under a transform. conv 0 = current
 * (row-vector: v*M, translation m[12..14]); conv 1 = transposed (M*v,
 * translation m[3,7,11]). */
static void n2_aabb_xform(const float *lmn, const float *lmx, const float *m,
                          int conv, float *mn, float *mx) {
    mn[0]=mn[1]=mn[2]=1e30f; mx[0]=mx[1]=mx[2]=-1e30f;
    for (int k = 0; k < 8; k++) {
        float p[3] = { (k&1)?lmx[0]:lmn[0], (k&2)?lmx[1]:lmn[1], (k&4)?lmx[2]:lmn[2] }, w[3];
        if (conv == 0) {
            w[0] = p[0]*m[0]+p[1]*m[4]+p[2]*m[8] +m[12];
            w[1] = p[0]*m[1]+p[1]*m[5]+p[2]*m[9] +m[13];
            w[2] = p[0]*m[2]+p[1]*m[6]+p[2]*m[10]+m[14];
        } else {
            w[0] = p[0]*m[0]+p[1]*m[1]+p[2]*m[2] +m[3];
            w[1] = p[0]*m[4]+p[1]*m[5]+p[2]*m[6] +m[7];
            w[2] = p[0]*m[8]+p[1]*m[9]+p[2]*m[10]+m[11];
        }
        for (int c = 0; c < 3; c++) { if (w[c]<mn[c]) mn[c]=w[c]; if (w[c]>mx[c]) mx[c]=w[c]; }
    }
}

static void n2_audit_meshes(const unsigned char *d, long beg, long end, N2Audit *a) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o), size = n2_u32(d + o + 4);
        long ds = o + 8;
        if (magic == 0x80134010u) {
            char anm[40]; n2_mesh_name(d, ds, ds + size, anm, sizeof anm);
            int sc = n2_scen_class(anm);
            float m[16]; int has = n2_obj_matrix(d, ds, ds + size, m);
            N2Leaf h[2]; int n11 = 0;
            n2_find_leaves(d, ds, ds + size, 0x00134011u, h, &n11, 2);
            /* an object is "nested" if a child 0x80134010 lives inside it */
            int nest = 0;
            for (long q = ds; q + 8 <= ds + size; ) {
                uint32_t mg = n2_u32(d+q), sz = n2_u32(d+q+4);
                if (mg == 0x80134010u) { nest = 1; break; }
                if (mg != 0 && (mg >> 28) == 8) { q += 8; continue; }   /* descend */
                q += 8 + sz;
            }
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + size, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + size, 0x00134B03u, idx, &ni, 64);

            /* is the 3x3 anything other than identity? If placement were only
               ever a translation, the rotation half would be read from the
               wrong offset (or not stored here at all). */
            { float dev = 0;
              for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
                  float e = m[r*4+c] - (r == c ? 1.0f : 0.0f);
                  if (e < 0) e = -e; if (e > dev) dev = e; }
              float det = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[1]*(m[4]*m[10]-m[6]*m[8])
                        + m[2]*(m[4]*m[9]-m[5]*m[8]);
              if (det > a->maxdet) a->maxdet = det;
              if (dev > 1e-3f) a->rot3x3++;
              else if (m[12] || m[13] || m[14]) a->trans_only++;
              else a->ident++; }

            a->nobj++; a->nmtx += has; a->nested += nest;
            a->n11_hist[n11 > 4 ? 4 : n11]++;
            if (sc >= 0 && sc < 8) { a->bycls[sc]++; a->mtxbycls[sc] += has; }

            int b = (sc == N2_SC_TERRAIN) ? 0 : (sc == N2_SC_BUILDING) ? 1 :
                    (sc == N2_SC_PROP || sc == N2_SC_STRUCT || sc == N2_SC_WALL) ? 2 : -1;
            if (b >= 0 && a->quota[b] > 0 && nv > 0) {
                a->quota[b]--;
                float lmn[3], lmx[3], cmn[3], cmx[3], tmn[3], tmx[3];
                int nvert; long voff;
                n2_aabb_local(d, vtx[0], lmn, lmx, &nvert, &voff);
                n2_aabb_xform(lmn, lmx, m, 0, cmn, cmx);
                n2_aabb_xform(lmn, lmx, m, 1, tmn, tmx);
                printf("\n%-28s %-8s objoff=0x%08lx vtxoff=0x%08lx nvert=%d "
                       "leaves(11/4B01/4B03)=%d/%d/%d nested=%d\n",
                       anm[0] ? anm : "(unnamed)", n2_scen_name(sc), o, voff, nvert,
                       n11, nv, ni, nest);
                printf("  local   AABB   x[%9.2f %9.2f] y[%9.2f %9.2f] z[%9.2f %9.2f]\n",
                       lmn[0], lmx[0], lmn[1], lmx[1], lmn[2], lmx[2]);
                printf("  matrix present=%d   m[12..14]=(%10.2f %10.2f %10.2f)  "
                       "m[3,7,11]=(%10.2f %10.2f %10.2f)\n",
                       has, m[12], m[13], m[14], m[3], m[7], m[11]);
                printf("  world(current) x[%9.2f %9.2f] y[%9.2f %9.2f] z[%9.2f %9.2f]\n",
                       cmn[0], cmx[0], cmn[1], cmx[1], cmn[2], cmx[2]);
                printf("  world(transp)  x[%9.2f %9.2f] y[%9.2f %9.2f] z[%9.2f %9.2f]\n",
                       tmn[0], tmx[0], tmn[1], tmx[1], tmn[2], tmx[2]);
            }
        } else if (magic != 0 && (magic >> 28) == 8) {
            n2_audit_meshes(d, ds, ds + size, a);
        }
        o = ds + size;
    }
}

static void n2_transform_audit(const unsigned char *d, long len, const char *rn) {
    N2Audit a; memset(&a, 0, sizeof a);
    a.quota[0] = a.quota[1] = a.quota[2] = 3;
    printf("transform-audit: %s (%ld MB)\n", rn, len >> 20);
    n2_audit_meshes(d, 0, len, &a);
    printf("\n-- summary --------------------------------------------------\n");
    printf("objects=%d  matrix-passed-w-check=%d (%.1f%%)  nested 0x80134010=%d\n",
           a.nobj, a.nmtx, a.nobj ? 100.0*a.nmtx/a.nobj : 0.0, a.nested);
    printf("0x134011 leaves per object: 0=%d 1=%d 2=%d 3=%d 4+=%d\n",
           a.n11_hist[0], a.n11_hist[1], a.n11_hist[2], a.n11_hist[3], a.n11_hist[4]);
    printf("3x3: rotated/scaled=%d  translation-only=%d  full identity=%d  max det=%.4f\n",
           a.rot3x3, a.trans_only, a.ident, a.maxdet);
    for (int c = 1; c <= N2_SC_OTHER; c++)
        if (a.bycls[c]) printf("  %-9s objects=%5d  with matrix=%5d (%.1f%%)\n",
                               n2_scen_name(c), a.bycls[c], a.mtxbycls[c],
                               100.0*a.mtxbycls[c]/a.bycls[c]);
}

static void n2_census_walk(const unsigned char *d, long beg, long end,
                           const char *rn, float thresh, int *nobj, int *nbig) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t magic = n2_u32(d + o), size = n2_u32(d + o + 4);
        long ds = o + 8;
        if (magic == 0x80134010u) {
            char anm[40]; n2_mesh_name(d, ds, ds + size, anm, sizeof anm);
            (*nobj)++;
            float m[16]; n2_obj_matrix(d, ds, ds + size, m);
            N2Geom g;
            if (n2_obj_geom(d, ds, ds + size, m, &g) && g.xyspan >= thresh) {
                (*nbig)++;
                int cat = n2_mesh_category(d, ds, ds + size);
                /* order must match the enum at the top: ROAD=0 TERRAIN=1 OTHER=2 SKY=3 GLOW=4 */
                static const char *cn[] = { "ROAD","TERRAIN","OTHER","SKY","GLOW" };
                printf("%-11s %-28s %-7s %-8s %8.0f %8.0f %7ld  "
                       "dom(%5.2f %5.2f %5.2f) planar %.3f  %s\n",
                       rn, anm[0] ? anm : "(unnamed)",
                       (cat >= 0 && cat <= 4) ? cn[cat] : "?",
                       n2_scen_name(n2_scen_class(anm)),
                       g.xyspan, g.zspan, g.tris,
                       g.dom[0], g.dom[1], g.dom[2], g.planarity,
                       !strncmp(anm, "PAN", 3) ? "PAN_ (culled by prefix)" :
                       n2_is_vista_impostor(anm, &g) ? "IMPOSTOR (culled)" :
                         (n2_vista_family(anm) ? "family, geometry-failed" : "retained"));
            }
        } else if (magic != 0 && (magic >> 28) == 8) {
            n2_census_walk(d, ds, ds + size, rn, thresh, nobj, nbig);
        }
        o = ds + size;
    }
}

static void n2_vista_census(const unsigned char *d, long len, const char *rn,
                            float thresh, int hdr) {
    if (hdr)
        printf("%-11s %-28s %-7s %-8s %8s %8s %7s  %-24s %-12s %s\n",
               "region","asset name","cat","class","XYspan","Zspan","tris",
               "dominant normal","planarity","verdict");
    int nobj = 0, nbig = 0;
    n2_census_walk(d, 0, len, rn, thresh, &nobj, &nbig);
    fprintf(stderr, "  %s: %d objects, %d over %.0f m\n", rn, nobj, nbig, thresh);
}

/* Parse a STREAM .BUN into categorized world-space meshes. Returns mesh count. */
static int n2_load_scene(const unsigned char *d, long len, N2Scene *scene,
                         const uint32_t *keys, int nkeys) {
    memset(scene, 0, sizeof(*scene));
    n2_walk_meshes(d, 0, len, scene, keys, nkeys);
    return scene->count;
}

/* Classify a car 0x80134010 mesh by its material name (part name). */
static int n2_car_category(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        for (long i = 0; i + 5 < s; i++) {
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    const unsigned char *n = p + i; long L = j - i;
                    if (n2_contains(n,L,"WINDOW") || n2_contains(n,L,"GLASS")) return N2_CAR_GLASS;
                    /* no diffuse texture exists anywhere in the extracted data
                       for ANY light part (verified: not this car's own TPK, not
                       the shared CARS/TEXTURES.BIN, not any file under GLOBAL —
                       exhaustive search) — housings are chrome+lens by material
                       colour, same treatment as glass. BRAKE first: "BRAKELIGHT"
                       contains "LIGHT" too. */
                    if (n2_contains(n,L,"BRAKE") && n2_contains(n,L,"LIGHT"))  return N2_CAR_BRAKELIGHT;
                    if (n2_contains(n,L,"LIGHT") || n2_contains(n,L,"LAMP"))   return N2_CAR_LIGHT;
                    if (n2_contains(n,L,"TIRE") || n2_contains(n,L,"WHEEL"))   return N2_CAR_TIRE;
                    /* mechanical compartment detail (engine bay, exhaust pipe):
                       unpainted metal/plastic, not glossy body shell — checked
                       before the generic KIT/BODY catch-all below, since these
                       names also contain "KIT##" and would otherwise match it.
                       Parts that already carry their own texture (e.g. GOLF's
                       engine bay atlas) still use it unchanged; this only
                       changes the flat-colour FALLBACK for cars where the part
                       has no texture of its own (e.g. Miata's engine bay). */
                    if (n2_contains(n,L,"ENGINE") || n2_contains(n,L,"EXHAUST")) return N2_CAR_MECH;
                    if (n2_contains(n,L,"BASE") || n2_contains(n,L,"BODY") ||
                        n2_contains(n,L,"KIT")  || n2_contains(n,L,"STYLE"))   return N2_CAR_BODY;
                    return N2_CAR_MISC;
                }
                i = j;
            }
        }
    }
    return N2_CAR_MISC;
}

/* Locate the KITnn / STYLEnn token inside an extracted name run.
 * Returns 1 = KITnn, 2 = STYLEnn, 0 = neither, and reports the token span so
 * the family key can cut it out. KITW (widebody) fails the digit test and is
 * handled as an always-skip below, not here. */
static int n2_name_variant(const unsigned char *n, long L, int *num,
                           long *tok_at, long *tok_len) {
    for (long q = 0; q + 4 < L; q++)
        if (n[q]=='K'&&n[q+1]=='I'&&n[q+2]=='T'&&
            n[q+3]>='0'&&n[q+3]<='9'&&n[q+4]>='0'&&n[q+4]<='9') {
            *num = (n[q+3]-'0')*10 + (n[q+4]-'0');
            *tok_at = q; *tok_len = 5; return 1;
        }
    for (long q = 0; q + 6 < L; q++)
        if (memcmp(n+q,"STYLE",5)==0 && n[q+5]>='0'&&n[q+5]<='9' && n[q+6]>='0'&&n[q+6]<='9') {
            *num = (n[q+5]-'0')*10 + (n[q+6]-'0');
            *tok_at = q; *tok_len = 7; return 2;
        }
    return 0;
}

/* Classify a car mesh against the active profile.
 * Returns 1 = skip outright. Otherwise reports the variant token and the
 * family key (name minus that token minus any _A.._D LOD suffix) so
 * n2_car_apply_config can let an aftermarket part shadow the stock one.
 *
 * A car GEOMETRY.BIN overlays every customization option at once, so the
 * walk keeps only what the profile can use: the shared/un-tokened parts,
 * KIT00 (the base car), the selected KITnn, and the selected STYLEnn. */
static int n2_car_is_variant(const unsigned char *d, long beg, long end,
                             const N2CarConfig *cfg,
                             int *out_kind, int *out_num, uint32_t *out_fam) {
    if (out_kind) *out_kind = 0;
    if (out_num)  *out_num  = 0;
    if (out_fam)  *out_fam  = 0;
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        for (long i = 0; i + 5 < s; i++) {
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    const unsigned char *n = p + i; long L = j - i;
                    if (n2_contains(n,L,"KITW") ||
                        n2_contains(n,L,"WIDE")  ||   /* widebody variants (WIDE1..4) */
                        n2_contains(n,L,"DECAL"))     /* decal mount shells (used only
                                                         when a sticker is applied) */
                        return 1;
                    int num = 0; long ta = 0, tl = 0;
                    int kind = n2_name_variant(n, L, &num, &ta, &tl);
                    if (kind == 1 && num != 0 && num != cfg->body_kit)   return 1;
                    if (kind == 2 && num != cfg->hood_style)             return 1;
                    if (out_kind) *out_kind = kind;
                    if (out_num)  *out_num  = num;
                    if (out_fam) {                    /* hash the name minus the token */
                        long e = L;
                        if (e >= 2 && n[e-2]=='_' && n[e-1]>='A' && n[e-1]<='D') e -= 2;
                        uint32_t h = 2166136261u;
                        for (long q = 0; q < e; q++) {
                            if (kind && q >= ta && q < ta + tl) continue;
                            h ^= n[q]; h *= 16777619u;
                        }
                        *out_fam = h ? h : 1u;
                    }
                    return 0;
                }
                i = j;
            }
        }
    }
    return 0;
}

/* Let selected aftermarket parts shadow their stock counterparts.
 * KIT00 supplies the whole car; a selected KITnn or STYLEnn supplies a few
 * replacement families. Wherever both provide the same family, drop KIT00's.
 * Runs before the LOD collapse, so the winner still picks its best tier. */
static void n2_car_apply_config(N2Scene *s, const N2CarConfig *cfg) {
    if (!cfg->body_kit && !cfg->hood_style) return;   /* pure stock: nothing shadows */
    int n = s->count;
    char *drop = (char *)calloc((size_t)(n ? n : 1), 1);
    if (!drop) return;                                /* OOM: keep everything */
    /* Mark first, compact after. Removing inline while scanning would move the
       anchor mesh out from under the loop (swap-with-last can relocate i
       itself), which silently skips shadowing and leaves both the stock and
       the aftermarket part drawn on top of each other. */
    for (int i = 0; i < n; i++) {
        int sel = (s->meshes[i].vkind == 1 && s->meshes[i].vnum == cfg->body_kit
                                           && cfg->body_kit != 0)
               || (s->meshes[i].vkind == 2);
        if (!sel || !s->meshes[i].famkey) continue;
        for (int j = 0; j < n; j++)
            if (j != i && s->meshes[j].vkind == 1 && s->meshes[j].vnum == 0 &&
                s->meshes[j].famkey == s->meshes[i].famkey)
                drop[j] = 1;
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (drop[i]) { free(s->meshes[i].verts); free(s->meshes[i].idx); continue; }
        if (w != i) s->meshes[w] = s->meshes[i];
        w++;
    }
    s->count = w;
    free(drop);
}

/* LOD family key: FNV-1a of the part name with any trailing _A.._D suffix
 * stripped, so every tier of one part hashes alike.
 *
 * Car geometry stores each part several times at descending detail. MOST
 * families spell the tier as a suffix (BASE_A/_B/_C, BODY_A.._D, WHEEL_A..),
 * but NOT all: the name field is a fixed-width buffer, and parts with long
 * names have the suffix truncated off entirely — MIATA_KIT00_HEADLIGHT_LEFT_
 * and both side mirrors are byte-identical across all three of their tiers
 * (verified by raw hex dump). So a suffix test alone cannot see those, which
 * is why they kept rendering stacked. Hashing the stripped name groups both
 * spellings; n2_car_dedupe_lod then resolves each group spatially. */
static uint32_t n2_car_name_key(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        for (long i = 0; i + 5 < s; i++) {
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                long L = j - i;
                if (L >= 5) {
                    if (p[i+L-2] == '_' && p[i+L-1] >= 'A' && p[i+L-1] <= 'D') L -= 2;
                    uint32_t h = 2166136261u;
                    for (long q = 0; q < L; q++) { h ^= p[i+q]; h *= 16777619u; }
                    return h ? h : 1u;          /* 0 is reserved for "unnamed" */
                }
                i = j;
            }
        }
    }
    return 0;
}

static void n2_mesh_bbox(const N2Mesh *m, float *bb) {
    bb[0]=bb[2]=bb[4] = 1e30f; bb[1]=bb[3]=bb[5] = -1e30f;
    for (int v = 0; v < m->nverts; v++) {
        const float *p = m->verts + v*5;
        for (int c = 0; c < 3; c++) {
            if (p[c] < bb[2*c])   bb[2*c]   = p[c];
            if (p[c] > bb[2*c+1]) bb[2*c+1] = p[c];
        }
    }
}

/* Intersection volume as a fraction of the SMALLER box. 0 when disjoint, or
 * when either box is degenerate (a flat quad has zero volume) — that case
 * errs toward keeping both meshes, which is the safe direction. */
static float n2_bbox_overlap(const float *a, const float *b) {
    float ov = 1.0f, va = 1.0f, vb = 1.0f;
    for (int c = 0; c < 3; c++) {
        float lo = a[2*c]   > b[2*c]   ? a[2*c]   : b[2*c];
        float hi = a[2*c+1] < b[2*c+1] ? a[2*c+1] : b[2*c+1];
        if (hi <= lo) return 0.0f;
        ov *= hi - lo;
        va *= a[2*c+1] - a[2*c];
        vb *= b[2*c+1] - b[2*c];
    }
    float mn = va < vb ? va : vb;
    return mn > 1e-9f ? ov / mn : 0.0f;
}

/* Collapse each LOD family to its highest-detail tier.
 *
 * Two meshes are the same part only if they share a name key AND their
 * bounding boxes genuinely overlap. Both halves matter, and a fleet sweep
 * proved why neither alone is safe:
 *   - name alone is wrong: truncation makes L/R pairs collide (LANCEREVO8 and
 *     IMPREZAWRX both have SIX meshes reading "..._KIT00_HEADLIGHT_"), and on
 *     on several cars some same-named DOOR_PANEL / DOOR_SILL /
 *     BRAKELIGHT nodes are distinct parts sitting apart. Deduping those by
 *     name would delete real geometry. The overlap gate keeps them: the
 *     anchor only absorbs meshes sharing its space, so the right-hand
 *     headlight simply starts its own group.
 *   - file order is wrong: keeping the first occurrence loses detail, because
 *     the tiers are NOT always stored best-first. Seven parts across the fleet
 *     (roofs, brake lights and skirts on a number of cars) store
 *     a later tier with MORE vertices than the one named _A.
 * Hence: group spatially, then keep the tier with the most detail.
 *
 * The unit of the decision is the PART, not the mesh: a part is split into one
 * mesh per material, and two tiers of the same part need not split the same
 * way. Comparing meshes individually drops a slice the winning tier has no
 * counterpart for, and the bodywork ends up with holes in it. So slices are
 * grouped by part name, tiers are compared by their total vertex count, and
 * the losing tiers go out whole. */
#define N2_LOD_OVERLAP 0.4f
static void n2_car_dedupe_lod(N2Scene *s) {
    int n = s->count;
    if (n < 2) return;
    float (*bb)[6] = (float (*)[6])malloc((size_t)n * sizeof *bb);
    char *drop = (char *)calloc((size_t)n, 1);
    char *done = (char *)calloc((size_t)n, 1);
    if (!bb || !drop || !done) { free(bb); free(drop); free(done); return; }
    for (int i = 0; i < n; i++) n2_mesh_bbox(&s->meshes[i], bb[i]);

    for (int i = 0; i < n; i++) {
        if (done[i] || drop[i] || !s->meshes[i].namekey) continue;

        /* This family: same key, sharing the anchor's space. */
        int member[256], nmem = 0;
        member[nmem++] = i; done[i] = 1;
        for (int j = i + 1; j < n && nmem < 256; j++) {
            if (done[j] || drop[j]) continue;
            if (s->meshes[j].namekey != s->meshes[i].namekey) continue;
            if (n2_bbox_overlap(bb[i], bb[j]) < N2_LOD_OVERLAP) continue;
            member[nmem++] = j; done[j] = 1;
        }
        if (nmem < 2) continue;

        /* Score each tier by all of its slices together. */
        long best_score = -1; const char *best_name = s->meshes[i].aname;
        for (int a = 0; a < nmem; a++) {
            const char *name = s->meshes[member[a]].aname;
            long score = 0;
            for (int b = 0; b < nmem; b++)
                if (!strcmp(s->meshes[member[b]].aname, name))
                    score += s->meshes[member[b]].nverts;
            if (score > best_score) { best_score = score; best_name = name; }
        }
        for (int a = 0; a < nmem; a++)
            if (strcmp(s->meshes[member[a]].aname, best_name)) drop[member[a]] = 1;
    }

    int w = 0;
    for (int i = 0; i < n; i++) {
        if (drop[i]) { free(s->meshes[i].verts); free(s->meshes[i].idx); continue; }
        if (w != i) s->meshes[w] = s->meshes[i];
        w++;
    }
    s->count = w;
    free(bb); free(drop); free(done);
}


/* Plastic trim within N2_CAR_BODY: bumpers and rocker skirts are moulded
 * polyurethane, not the metal body shell, and carry their own name tokens
 * (verified real, fleet-wide: MIATA/GOLF/HUMMER/350Z/SKYLINE all have
 * <CAR>_KIT00_FRONT_BUMPER_*, REAR_BUMPER_*, SKIRT_* — the stock KIT00 set
 * that survives n2_car_is_variant). Used to dial back specular/gloss so
 * they read as a duller material than the painted panels around them. */
static int n2_car_is_trim(const unsigned char *d, long beg, long end) {
    N2Leaf mat[4]; int nm = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        if (n2_contains(p, s, "BUMPER") || n2_contains(p, s, "SKIRT")) return 1;
    }
    return 0;
}

/* Soft-top vs painted roof: the shipped data draws no distinction (M111).
 * Every car has a *_KIT00_ROOF_* part -- hardtops included -- and on all five
 * cars measured (HUMMER, MIATA, MUSTANGGT, 350Z, ECLIPSE) the roof's 0x134012
 * slot list is a subset of its own *_KIT00_BODY_* slots, i.e. the same body
 * texture keys. There is no material, texture or naming discriminator to key a
 * canvas rule on, so ROOF is treated as ordinary body panel and rendered from
 * its own material like every other body mesh. The previous name-substring rule
 * forced (0.035, 0.032, 0.030) on all of them, which is where the black Hummer
 * roof came from. */

/* Find this mesh's bound diffuse: scan its 0x134012 texture-slot list (8-byte
 * entries: key + 0) for a key present in the car's TPK (keys[]). 0 if none. */
static uint32_t n2_mesh_texkey(const unsigned char *d, long beg, long end,
                               const uint32_t *keys, int nkeys) {
    if (!nkeys) return 0;
    N2Leaf t12[4]; int n12 = 0;
    n2_find_leaves(d, beg, end, 0x00134012u, t12, &n12, 4);
    for (int a = 0; a < n12; a++) {
        const unsigned char *p = d + t12[a].off; long ls = t12[a].size;
        for (long b = 0; b + 4 <= ls; b += 4) {
            uint32_t v = n2_u32(p + b);
            for (int c = 0; c < nkeys; c++) if (v == keys[c]) return v;
        }
    }
    return 0;
}

/* Every key in a mesh's 0x134012 slot list, BY SLOT INDEX (8-byte entries:
 * key + 0), including keys this car's TPK can't resolve — the index is what
 * a 0x134B02 record's mat_id refers to, so gaps must keep their position. */
static int n2_mesh_texslots(const unsigned char *d, long beg, long end,
                            uint32_t *out, int cap) {
    N2Leaf t12[4]; int n12 = 0, n = 0;
    n2_find_leaves(d, beg, end, 0x00134012u, t12, &n12, 4);
    for (int a = 0; a < n12; a++) {
        const unsigned char *p = d + t12[a].off; long ls = t12[a].size;
        for (long b = 0; b + 8 <= ls && n < cap; b += 8) out[n++] = n2_u32(p + b);
    }
    return n;
}

static uint32_t n2_resolve_key(uint32_t v, const uint32_t *keys, int nkeys) {
    if (!v) return 0;
    for (int i = 0; i < nkeys; i++) if (keys[i] == v) return v;
    return 0;   /* lives in a pack we don't ship — caller falls back to paint */
}

/* One 0x134B02 record = one material slice of the index buffer. Layout,
 * decoded from MIATA_KIT00_FRONT_BUMPER_A and cross-checked on BODY_A
 * (60 bytes, after the usual 0x11 filler):
 *   +0  bbox min xyz (3 floats)      +12 index count
 *   +16 bbox max xyz (3 floats)      +28 mat_id  = index into the 0x134012
 *                                                  slot list above
 *   +32 render-state flag            +52 index start
 * The starts/counts chain exactly across the buffer (0->39->1065->1083 =
 * 1143 = the whole index list), which is what confirms the field roles.
 * mat_id is NOT the same field as the flag at +32: BODY_A's last record has
 * mat_id 1 but flag 2, and BODY_A only HAS two slots. */
static int n2_mesh_submeshes(const unsigned char *d, long beg, long end,
                             N2Sub *out, int cap) {
    N2Leaf sm[4]; int nsm = 0;
    n2_find_leaves(d, beg, end, 0x00134B02u, sm, &nsm, 4);
    if (nsm != 1) return 0;                     /* only the simple case */
    const unsigned char *p = d + sm[0].off; long ls = sm[0].size;
    /* This leaf's filler is whole 0x1111 u16 words, exactly like the index leaf.
     * n2_skip_filler eats single 0x11 bytes, so a record whose first bbox float
     * merely ENDS in 0x11 lost one data byte and the leaf then failed body%60
     * (measured: 12 L4RA objects, every one 60k-1 after the byte skip; skipping
     * in pairs gives 60k for all of them -- M104). Local to this leaf; the
     * global n2_skip_filler is unchanged because the vertex and index leaves
     * depend on its current behaviour. */
    int pad = 0;
    while (pad + 2 <= (int)ls && p[pad] == 0x11 && p[pad+1] == 0x11) pad += 2;
    long body = ls - pad;
    if (body <= 0 || body % 60 || body / 60 > cap) return 0;
    int n = (int)(body / 60);
    for (int i = 0; i < n; i++) {
        const unsigned char *q = p + pad + i*60;
        out[i].count = n2_u32(q + 12);
        out[i].mat   = n2_u32(q + 28);
        out[i].matid = n2_u32(q + 32);   /* index into the object's material list */
        out[i].start = n2_u32(q + 52);
    }
    return n;
}

/* Parse a car GEOMETRY.BIN (36-byte verts w/ normals), tagging each mesh with
 * a class from its material name and its per-mesh diffuse texture key.
 *
 * INVESTIGATED (car submesh materials, Golf, all findings verified against
 * real bytes): 0x134B02 DOES exist per car mesh object and DOES hold
 * multiple 60-byte records (same 0x11-filler-prefix convention as every
 * other leaf in this format; skip filler, then size/60 is exact) with
 * varying mat_id/flag fields — e.g. GOLF_KIT00_FRONT_BUMPER_A has 5 records,
 * mat_id 0-3, flag 0-4. Splitting the index buffer at these record
 * boundaries and checking each record's vertex bbox confirms they ARE real,
 * spatially-distinct sub-groups (a small asymmetric bracket vs. the big
 * symmetric shell, etc.) — this part of the directive was right.
 *
 * BUT: the chain that would make this useful for TEXTURE routing —
 * mat_id -> 0x134003 hash list -> a DIFFERENT 0x134011/0x134012 per submesh
 * — does not exist for car objects. Checked every Golf mesh: zero objects
 * have more than one 0x134011 material block or more than one 0x134012
 * texture-slot list. There is exactly one texture key per whole object,
 * full stop; mat_id/flag never select among alternatives because no
 * alternatives are stored. flag's value set is a small, consistent {0..4}
 * across unrelated meshes and mirrors (L/R copies of the same part keep the
 * same flag, different mat_id) — looks like a small built-in render-state
 * enum (cull mode / blend mode / vertex-color-source, guessing), not a
 * material-lookup key. So "bind a different texture per submesh" is not
 * implementable from this data — there is nothing per-submesh to bind.
 * Splitting meshes at these boundaries anyway (same texture on every
 * resulting piece) would add draw calls for a pixel-identical result, so
 * it isn't done. If a future car is found with >1 material/texslot block
 * per object, THAT would be the real signal this is worth revisiting.
 * removed vinyl/badge fallback, not a real submesh material). */
static int n2_mat_class(uint32_t h, int fallback) {
    if (!h) return fallback;
    /* Leave wheels alone. A rim carries several metal materials on one part,
       and the moment the class changes mid-part the mesh is split and its
       spokes stop being drawn. */
    if (fallback == N2_CAR_TIRE) return fallback;
    if (h == n2_str_hash("WINDSHIELD"))       return N2_CAR_GLASS;
    /* Only the LENS emits. A headlight assembly is mostly metal -- chrome,
       aluminium and mouldings -- with a single glass submesh. Filling the
       whole part with one emissive colour turns the lamp into a bright blob
       with no shape. */
    /* The lens is GLASS, not a flat fill: its material carries a strong
       grazing-angle reflection (0.180 rising to 0.703 on the brake lens) and
       its name says GLASS. Drawn in the transparent pass with the body
       glazing. */
    /* The lens also emits: white at the front, red at the rear. It cannot be
       folded into the general glass pass or the lamps go dark. */
    if (h == n2_str_hash("BRAKELIGHTGLASS") ||
        h == n2_str_hash("BRAKELIGHT"))       return N2_CAR_BRAKELIGHT;
    if (h == n2_str_hash("HEADLIGHTGLASS"))   return N2_CAR_LIGHT;
    if (h == n2_str_hash("CHROME") ||
        h == n2_str_hash("HEADLIGHTREFLECTOR") ||
        h == n2_str_hash("ALUMINUM") ||
        h == n2_str_hash("MOLDINGS") ||
        h == n2_str_hash("CLEARPLASTIC") ||
        h == n2_str_hash("DULLPLASTIC"))      return N2_CAR_MISC;
    if (h == N2_MAT_CARSKIN)                  return N2_CAR_BODY;
    return fallback;
}

static int n2_mesh_matslots(const unsigned char *d, long beg, long end,
                            uint32_t *out, int cap) {
    N2Leaf t13[4]; int n13 = 0, n = 0;
    n2_find_leaves(d, beg, end, 0x00134013u, t13, &n13, 4);
    for (int a = 0; a < n13; a++) {
        const unsigned char *p = d + t13[a].off; long ls = t13[a].size;
        for (long b = 0; b + 8 <= ls && n < cap; b += 8) out[n++] = n2_u32(p + b);
    }
    return n;
}

static int n2_car_part_name(const unsigned char *d, long beg, long end,
                            char *out, int cap) {
    N2Leaf mat[4]; int nm = 0;
    if (cap > 0) out[0] = 0;
    n2_find_leaves(d, beg, end, 0x00134011u, mat, &nm, 4);
    for (int k = 0; k < nm; k++) {
        const unsigned char *p = d + mat[k].off; long s = mat[k].size;
        for (long i = 0; i + 5 < s; i++) {
            if (p[i] >= 'A' && p[i] <= 'Z') {
                long j = i;
                while (j < s && (p[j]=='_' || (p[j]>='A'&&p[j]<='Z') || (p[j]>='0'&&p[j]<='9'))) j++;
                if (j - i >= 5) {
                    int n = (int)(j - i); if (n > cap - 1) n = cap - 1;
                    for (int q = 0; q < n; q++) out[q] = (char)p[i+q];
                    out[n] = 0;
                    return 1;
                }
                i = j;
            }
        }
    }
    return 0;
}

typedef struct { uint32_t mat; unsigned char r, g, b; } N2Paint;

static int n2_load_paints(const unsigned char *d, long len, N2Paint *out, int cap) {
    long pairs = -1, np = 0, tbl = -1, ts = 0;
    for (long i = 0; i + 8 <= len; i++) {
        uint32_t m = n2_u32(d + i); long sz = (long)n2_u32(d + i + 4);
        if (sz <= 0 || i + 8 + sz > len) continue;
        if (m == 0x00034605u && pairs < 0) { pairs = i + 8; np = sz / 8; }
        else if (m == 0x0003460Cu && tbl < 0) { tbl = i + 8; ts = sz; }
        if (pairs >= 0 && tbl >= 0) break;
    }
    if (pairs < 0 || tbl < 0) return 0;
    int n = 0;
    for (long p = tbl; p + 2 <= tbl + ts && n < cap; ) {
        short ln = (short)(d[p] | (d[p+1] << 8));
        if (ln < 1 || ln > 40 || p + 2 + ln*2 > tbl + ts) { p += 2; continue; }
        uint32_t mat = 0; int have = 0; unsigned char rgb[3] = {0,0,0};
        int ok = 1;
        for (int k = 0; k < ln; k++) {
            short ix = (short)(d[p+2+k*2] | (d[p+3+k*2] << 8));
            if (ix < 0 || ix >= np) { ok = 0; break; }
            uint32_t key = n2_u32(d + pairs + (long)ix*8);
            uint32_t val = n2_u32(d + pairs + (long)ix*8 + 4);
            if (key == 0x6ba02c05u) mat = val;
            else if (key == 0x0000d99au) { rgb[0] = (unsigned char)val; have |= 1; }
            else if (key == 0x02ddc8f0u) { rgb[1] = (unsigned char)val; have |= 2; }
            else if (key == 0x00136707u) { rgb[2] = (unsigned char)val; have |= 4; }
        }
        if (ok && mat && have == 7) {
            out[n].mat = mat; out[n].r = rgb[0]; out[n].g = rgb[1]; out[n].b = rgb[2];
            n++;
        }
        p += ok ? 2 + ln*2 : 2;
    }
    return n;
}

static const N2LightMat *n2_find_lightmat(const N2LightMat *m, int n, uint32_t hash) {
    /* STOCK PAINT PER CAR. Chunk 0x00034601 holds 106 records of 64 bytes:
       +0x00 paint material hash, +0x14 car number, +0x18 variant number,
       +0x1c colour name. Car numbers 1..30 are the playable cars, one colour
       each; 31..45 are traffic, with eight variants apiece. */
    if (hash == N2_MAT_CARSKIN)
        hash = n2_carskin_mat ? n2_carskin_mat : n2_str_hash("METPAINTSILVER");
    for (int i = 0; i < n; i++) if (m[i].hash == hash) return &m[i];
    return NULL;
}

static void n2_walk_car(const unsigned char *d, long beg, long end, N2Scene *scene,
                        const uint32_t *keys, int nkeys, const N2CarConfig *cfg) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t m = n2_u32(d + o), s = n2_u32(d + o + 4);
        long ds = o + 8;
        if (m == 0x80134010u) {
            int vkind = 0, vnum = 0; uint32_t vfam = 0;
            if (n2_car_is_variant(d, ds, ds + s, cfg, &vkind, &vnum, &vfam)) { o = ds + s; continue; }
            int cat = n2_car_category(d, ds, ds + s);
            char pname[28]; n2_car_part_name(d, ds, ds + s, pname, sizeof pname);
            int trim = cat == N2_CAR_BODY && n2_car_is_trim(d, ds, ds + s);
                        uint32_t nk2 = n2_car_name_key(d, ds, ds + s);   /* LOD family, resolved after the walk */
            uint32_t tk = n2_mesh_texkey(d, ds, ds + s, keys, nkeys);
            N2Leaf vtx[64], idx[64]; int nv = 0, ni = 0;
            n2_find_leaves(d, ds, ds + s, 0x00134B01u, vtx, &nv, 64);
            n2_find_leaves(d, ds, ds + s, 0x00134B03u, idx, &ni, 64);
            int pairs = nv < ni ? nv : ni;

            /* Per-submesh material routing. One object can mix materials: a
               bumper is mostly body paint with a small badge patch, and its
               0x134012 list carries a key per material. Binding the ONE key
               we can resolve to the whole object smears the badge atlas over
               the entire panel (one front bumper: the badge owns 18 of
               1143 indices, 1.6%, but was painting 100% of it dark). So when
               the records actually resolve to different textures, emit one
               mesh per record instead. */
            uint32_t slots[16]; int nslot = n2_mesh_texslots(d, ds, ds + s, slots, 16);
            N2Sub sub[32]; int nsub = pairs == 1 ? n2_mesh_submeshes(d, ds, ds + s, sub, 32) : 0;
            /* Submesh material. The same id also indexes the material list,
               whose entries are name hashes (WINDSHIELD, CHROME, MOULDINGS and
               so on). Without this link the glass is indistinguishable from
               the bodywork. */
            uint32_t mslots[32]; int nmslot = n2_mesh_matslots(d, ds, ds + s, mslots, 32);
            uint32_t subtex[32], submat[32]; int differ = 0, big = 0;
            for (int k = 0; k < nsub; k++) {
                subtex[k] = sub[k].mat < (uint32_t)nslot
                          ? n2_resolve_key(slots[sub[k].mat], keys, nkeys) : 0;
                submat[k] = sub[k].matid < (uint32_t)nmslot ? mslots[sub[k].matid] : 0;
                /* Split only on a MEANINGFUL difference: another texture, or
                   another class by material. Splitting on any material change
                   at all breaks a wheel rim into pieces -- it carries several
                   metal materials on one part -- and its spokes stop being
                   drawn. */
                if (subtex[k] != subtex[0]
                    || n2_mat_class(submat[k], cat) != n2_mat_class(submat[0], cat)) differ = 1;
                if (sub[k].count > sub[big].count) big = k;
            }
            if (nsub > 1 && differ) {
                for (int k = 0; k < nsub; k++) {
                    int before = scene->count;
                    /* class comes from the submesh MATERIAL, not the part name */
                    int kcat = n2_mat_class(submat[k], cat);
                    n2_add_pair(d, vtx[0], idx[0], kcat, scene, 36, 28, 0,
                                subtex[k], NULL, sub[k].start, sub[k].count);
                    if (scene->count > before) {
                        scene->meshes[before].trim = trim && kcat == N2_CAR_BODY;
                        scene->meshes[before].matkey = submat[k];
                                                /* The dominant slice keeps the plain family key so it
                           still dedupes against LOD tiers that never split
                           (a lower tier can lack the badge slot entirely, so
                           it stays whole); the small extra slices get their
                           own keys and only ever match the same slice of
                           another tier. */
                        /* Every slice of a part carries the SAME family key.
                           Giving each slice its own key made the tier choice
                           run slice against slice, and tiers do not split the
                           same way: a slice with no counterpart in the winning
                           tier was simply dropped, which punched holes in the
                           bodywork. */
                        scene->meshes[before].namekey = nk2;
                        scene->meshes[before].vkind = vkind;
                        scene->meshes[before].vnum = vnum;
                        scene->meshes[before].famkey = vfam;
                        snprintf(scene->meshes[before].aname,
                                 sizeof scene->meshes[before].aname, "%s", pname);
                    }
                }
            } else {
                for (int k = 0; k < pairs; k++) {   /* car parts have identity transforms */
                    int before = scene->count;
                    n2_add_pair(d, vtx[k], idx[k], cat, scene, 36, 28, 0, tk, NULL, 0, -1);
                    if (scene->count > before) {
                        scene->meshes[before].trim = trim;
                        /* an unsplit mesh takes the material of its largest submesh */
                        scene->meshes[before].matkey = nsub ? submat[big] :
                            (nmslot ? mslots[0] : 0);
                                                scene->meshes[before].namekey = nk2;
                        scene->meshes[before].vkind = vkind;
                        scene->meshes[before].vnum = vnum;
                        scene->meshes[before].famkey = vfam;
                        snprintf(scene->meshes[before].aname,
                                 sizeof scene->meshes[before].aname, "%s", pname);
                    }
                }
            }
        } else if (m != 0 && (m >> 28) == 8) {
            n2_walk_car(d, ds, ds + s, scene, keys, nkeys, cfg);
        }
        o = ds + s;
    }
}

static int n2_load_car(const unsigned char *d, long len, N2Scene *scene,
                       const uint32_t *keys, int nkeys, const N2CarConfig *cfg) {
    static const N2CarConfig stock = { 0, 0, 0, 0 };
    if (!cfg) cfg = &stock;
    memset(scene, 0, sizeof(*scene));
    n2_walk_car(d, 0, len, scene, keys, nkeys, cfg);
    n2_car_apply_config(scene, cfg);   /* aftermarket parts shadow stock ones */
    n2_car_dedupe_lod(scene);          /* collapse each LOD family to its best tier */
    return scene->count;
}

static void n2_free_scene(N2Scene *s) {
    for (int i = 0; i < s->count; i++) { free(s->meshes[i].verts); free(s->meshes[i].idx); }
    free(s->meshes); memset(s, 0, sizeof(*s));
}

/* ---- AI racing-line path (ROUTES.../Paths...bin, chunk 0x34148) ---- */
/* 24-byte records: x(f32) y(f32) then unknown; a smooth 2D centerline. */
typedef struct { float *xy; int n; } N2Path;
static int n2_load_path(const unsigned char *d, long len, N2Path *p) {
    p->xy = NULL; p->n = 0;
    N2Leaf leaf[8]; int nl = 0;
    n2_find_leaves(d, 0, len, 0x00034148u, leaf, &nl, 8);
    if (!nl) return 0;
    int n = (int)leaf[0].size / 24;
    p->xy = (float *)malloc((size_t)n * 2 * sizeof(float));
    int c = 0;
    for (int i = 0; i < n; i++) {
        float x, y;
        memcpy(&x, d + leaf[0].off + i*24,     4);
        memcpy(&y, d + leaf[0].off + i*24 + 4, 4);
        if (x==x && y==y && x>-1e6f && x<1e6f && y>-1e6f && y<1e6f) {
            p->xy[c*2] = x; p->xy[c*2+1] = y; c++;
        }
    }
    p->n = c;
    return c;
}

/* Index of the racing-line waypoint nearest (x,y) — a car's progress metric. */
static int n2_nearest_wp(const N2Path *p, float x, float y) {
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < p->n; i++) {
        float dx=p->xy[i*2]-x, dy=p->xy[i*2+1]-y, d=dx*dx+dy*dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Ground height at (x,y): the road/terrain triangle surface under the point
 * (barycentric z), or `fallback` if none covers it. Brute force over track
 * triangles — fine for ~10k tris/frame.
 *
 * Layer selection (Phase 73): when `refz` is a real reference height (a car's
 * current Z), pick the covering surface NEAREST it, not the globally highest.
 * "Highest wins" teleports a car driving under an overpass — or past a mesh
 * whose roof/hillside also covers the XY — straight up onto that upper deck
 * (measured: a 19.4 m single-frame pop on the L4RA lap). Nearest-to-reference
 * keeps the car on the surface it is actually on: a ramp climb still tracks
 * because Z rises continuously, but a deck 15 m overhead is no longer chosen.
 * Pass refz = N2_GROUND_HIGHEST to get the old highest-wins behaviour. */
#define N2_GROUND_HIGHEST 1e30f
static float n2_ground_z_ref(N2Scene *s, float x, float y, float fallback, float refz) {
    float best = 0.0f, bestkey = 1e30f; int found = 0;
    int highest = (refz >= N2_GROUND_HIGHEST);
    for (int m = 0; m < s->count; m++) {
        /* road + terrain ONLY: SKY (the skydome spans every XY up to Z~8000)
           and GLOW must be excluded or the query returns a point on the dome
           and launches the car kilometres into the air. */
        if (s->meshes[m].cat != N2_ROAD && s->meshes[m].cat != N2_TERRAIN) continue;
        N2Mesh *me = &s->meshes[m];
        for (int t = 0; t + 2 < me->nidx; t += 3) {
            float *a = me->verts + me->idx[t]*5;
            float *b = me->verts + me->idx[t+1]*5;
            float *c = me->verts + me->idx[t+2]*5;
            float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
            if (d > -1e-9f && d < 1e-9f) continue;
            float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / d;
            float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / d;
            float w = 1.0f - u - v;
            if (u < -0.01f || v < -0.01f || w < -0.01f) continue;
            float z = u*a[2] + v*b[2] + w*c[2];
            /* key: highest-wins uses -z (smaller = higher); reference mode uses
               distance to refz, but bias UP-steps so a surface far above the car
               never beats a road just below it (a curb lip vs the overpass). */
            float key;
            if (highest) key = -z;
            else { float dz = z - refz; key = dz >= 0 ? dz*3.0f : -dz; }
            if (!found || key < bestkey) { bestkey = key; best = z; found = 1; }
        }
    }
    return found ? best : fallback;
}
static float n2_ground_z(N2Scene *s, float x, float y, float fallback) {
    return n2_ground_z_ref(s, x, y, fallback, N2_GROUND_HIGHEST);
}

/* ---- per-car dimension profile (Phase 63) ----------------------------------
 * Everything the loaded car's own geometry states about its running gear, so no
 * absolute constant is ever applied to a vehicle.
 *
 * WHAT THE DATA ACTUALLY HAS (audited across all 31 shipped cars):
 *   - N2_CAR_TIRE mesh  -> EXACT wheel radius, width, and hub Z.  Hub Z measures
 *     0.000 on every car (max |dev| 0.019 on AMBULANCE): the wheel is modelled
 *     at the body's model origin, so ride height == wheel radius.
 *   - Body AABB         -> EXACT length / width / height.
 *
 * WHAT IT DOES NOT HAVE (verified, do not go looking again):
 *   - No WHEEL_FRONT_LEFT / _REAR_RIGHT dummy nodes. Cars ship ONE wheel part
 *     (<CAR>_KIT00_FRONT_WHEEL_A/B/C = LOD tiers), no per-corner variants.
 *   - FRONT_BRAKE and REAR_BRAKE exist but are ALSO modelled at the origin
 *     (measured centres (+0.010,-0.005,0.000) and (-0.007,-0.006,0.000) on TT),
 *     so they carry no axle position either.
 *   => wheelbase and track width are NOT stored anywhere in GEOMETRY.BIN. They
 *      are derived below from this car's own measured body extents. That is a
 *      derivation, not a reading -- flagged so nobody mistakes it for ground
 *      truth. The arch-cutout geometry was tried as a source and rejected: the
 *      detector also fires on cabin floor/interior openings (TT gave 12 "arch"
 *      runs, not 2), so it is not reliable enough to place wheels with.
 *
 * 2 of 31 cars (IMPREZAWRX, LANCEREVO8) ship no tyre mesh at all; they fall back
 * to the fleet-median radius/body-length ratio applied to THEIR OWN body length,
 * so even the fallback scales with the individual car. */
typedef struct {
    char  name[32];
    int   has_tire;      /* 0 = radius came from the body-length ratio, not a tyre */
    float wheel_r;       /* EXACT from the tyre mesh (or car-derived if !has_tire) */
    float wheel_rx;      /* tyre half-extent along model X (diagnostic/rolling radius) */
    float wheel_rz;      /* tyre half-extent along model Z (vertical contact radius) */
    float wheel_w;       /* EXACT tyre width */
    float hub_z;         /* EXACT hub centre Z in model space (0.000 fleet-wide) */
    float body[6];       /* EXACT body AABB: x0,x1,y0,y1,z0,z1 */
    float ride;          /* = wheel_r: hub sits at model origin, tyre flush */
    float wheelbase;     /* DERIVED from body length */
    float track_f;       /* DERIVED from body width */
    float track_r;       /* DERIVED from body width */
    float clearance;     /* EXACT once ride is known: ride + body z0 */
} N2CarProfile;

/* Fallback ratios, used ONLY by the 2 of 31 cars that ship no tyre mesh.
 * Data-first audit for those two (IMPREZAWRX, LANCEREVO8), all verified:
 *   - no FRONT_WHEEL part of any kind in their GEOMETRY.BIN;
 *   - their "base" dirs CARS/IMPREZA and CARS/LANCER hold ONLY VINYLS.BIN --
 *     no geometry, so there is nothing to inherit a radius from;
 *   - their PARTS_ANIMATIONS.bin holds only ZAM_ door/hood/trunk animations.
 * So no wheel radius exists for them anywhere in the game files, and a
 * derivation is unavoidable. The best available signal is each car's OWN brake
 * disc (it physically sits inside the rim): median wheelR/brakeR = 2.087 over
 * the 27 cars shipping both (spread 1.83..2.85, the high end being SUVs).
 * Result is then clamped so the car's own body cannot clip the road. */
#define N2_R_PER_BRAKE 2.087f
#define N2_R_PER_LEN   0.0726f   /* last resort: no tyre AND no brake disc */

/* Radius of a car's brake disc, measured in its own X-Z plane from the raw
 * GEOMETRY.BIN (the walker drops brake parts into N2_CAR_MECH, so the name is
 * only available here). 0 if the car ships none. Feeds the tyre-less fallback. */
static float n2_car_brake_radius(const unsigned char *d, long beg, long end) {
    long o = beg;
    while (o + 8 <= end) {
        uint32_t m = n2_u32(d+o), s = n2_u32(d+o+4); long ds = o+8;
        if (m == 0x80134010u) {
            N2Leaf mt[4]; int nm = 0; char nm2[128]; nm2[0] = 0;
            n2_find_leaves(d, ds, ds+s, 0x00134011u, mt, &nm, 4);
            for (int k = 0; k < nm && !nm2[0]; k++) {
                const unsigned char *q = d + mt[k].off; long sz = mt[k].size;
                for (long i = 0; i + 5 < sz; i++) {
                    if (q[i] >= 'A' && q[i] <= 'Z') { long j = i;
                        while (j < sz && (q[j]=='_' || (q[j]>='A'&&q[j]<='Z') || (q[j]>='0'&&q[j]<='9'))) j++;
                        if (j-i >= 5) { int L = (int)(j-i); if (L > 127) L = 127;
                            memcpy(nm2, q+i, L); nm2[L] = 0; break; }
                        i = j; }
                }
            }
            if (nm2[0] && strstr(nm2, "BRAKE") && !strstr(nm2, "BRAKELIGHT")) {
                N2Leaf v[64]; int nv = 0;
                n2_find_leaves(d, ds, ds+s, 0x00134B01u, v, &nv, 64);
                if (nv > 0) {
                    const unsigned char *vb = d + v[0].off; int vl = (int)v[0].size;
                    int pad = n2_skip_filler(vb, vl), body = vl - pad;
                    if (body > 0 && body % 36 == 0) {
                        int n = body/36; const unsigned char *rec = vb + pad;
                        float x0=1e30f,x1=-1e30f,z0=1e30f,z1=-1e30f;
                        for (int i = 0; i < n; i++) { float x,z;
                            memcpy(&x, rec+i*36, 4); memcpy(&z, rec+i*36+8, 4);
                            if(x<x0)x0=x; if(x>x1)x1=x; if(z<z0)z0=z; if(z>z1)z1=z; }
                        return 0.25f*((x1-x0)+(z1-z0));
                    }
                }
            }
        } else if (m && (m>>28) == 8) {
            float r = n2_car_brake_radius(d, ds, ds+s);
            if (r > 0.0f) return r;
        }
        o = ds + s;
    }
    return 0.0f;
}

static void n2_car_profile(const N2Scene *s, const char *name,
                           float frontf, float rearf, float trackf,
                           float brake_r, N2CarProfile *p) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name ? name : "?");
    float b0[3] = { 1e30f, 1e30f, 1e30f }, b1[3] = { -1e30f, -1e30f, -1e30f };
    float t0[3] = { 1e30f, 1e30f, 1e30f }, t1[3] = { -1e30f, -1e30f, -1e30f };
    int ntv = 0;
    for (int i = 0; i < s->count; i++) {
        int tire = s->meshes[i].cat == N2_CAR_TIRE;
        for (int q = 0; q < s->meshes[i].nverts; q++) {
            const float *v = s->meshes[i].verts + q*5;
            for (int a = 0; a < 3; a++) {
                if (tire) { if (v[a] < t0[a]) t0[a] = v[a]; if (v[a] > t1[a]) t1[a] = v[a]; }
                else      { if (v[a] < b0[a]) b0[a] = v[a]; if (v[a] > b1[a]) b1[a] = v[a]; }
            }
            if (tire) ntv++;
        }
    }
    if (b0[0] > b1[0]) { b0[0]=b0[1]=b0[2]=0; b1[0]=b1[1]=b1[2]=0; }
    p->body[0]=b0[0]; p->body[1]=b1[0];
    p->body[2]=b0[1]; p->body[3]=b1[1];
    p->body[4]=b0[2]; p->body[5]=b1[2];
    if (ntv) {                                  /* exact, from this car's tyre */
        p->has_tire = 1;
        p->wheel_rx = 0.5f * (t1[0]-t0[0]);
        p->wheel_rz = 0.5f * (t1[2]-t0[2]);
        p->wheel_r  = 0.5f * (p->wheel_rx + p->wheel_rz);
        p->wheel_w  = t1[1]-t0[1];
        p->hub_z    = 0.5f * (t0[2]+t1[2]);
    } else {                                    /* car-derived, never absolute */
        p->has_tire = 0;
        /* prefer this car's own brake disc; else its own body length */
        p->wheel_r  = brake_r > 0.01f ? brake_r * N2_R_PER_BRAKE
                                      : (b1[0]-b0[0]) * N2_R_PER_LEN;
        /* hard no-clip guarantee from the car's OWN body: ride == wheel_r, and
           clearance = ride + body z0, so r must exceed -z0 or the shell scrapes
           the road. 2 cm margin keeps the sills clear of camber. */
        float rmin = -b0[2] + 0.02f;
        if (p->wheel_r < rmin) p->wheel_r = rmin;
        p->wheel_rx = p->wheel_r;
        p->wheel_rz = p->wheel_r;
        p->wheel_w  = p->wheel_r * 0.62f;       /* fleet-median width/radius */
        p->hub_z    = 0.0f;
    }
    p->ride      = p->wheel_r;                  /* hub at origin => flush tyre */
    p->wheelbase = (b1[0]*frontf) - (b0[0]*rearf);
    p->track_f   = 2.0f * b1[1] * trackf;
    p->track_r   = p->track_f;
    p->clearance = p->ride + b0[2];
}

/* Exact wheel geometry from a per-car table in GLOBALB.BUN (the decompressed
 * GlobalB.lzc). Structural audit correction: these path anchors are NOT inside
 * a 0x00135200 AttribSys record, so do not use that reader as proof of this
 * fixed-offset layout. The unique "CARS\<NAME>\GEOMETRY.BIN" path locates a
 * repeating 2192-byte record; the wheel block sits 0x40 before the path.
 * Front/rear axle X and half-track Y are in the same frame and scale as the
 * model and reproduce real spec dimensions across the sampled fleet. This
 * supersedes the body-box fraction fallback. Returns 1 on a plausible hit. */
typedef struct { float front_axle, rear_axle, front_track, rear_track; } N2WheelAttr;
static int n2_global_wheel_attr(const unsigned char *g, long glen,
                                const char *carname, N2WheelAttr *w) {
    if (!g || !carname || !w) return 0;
    char sig[128];
    int n = snprintf(sig, sizeof sig, "CARS\\%s\\GEOMETRY.BIN", carname);
    if (n <= 0 || n >= (int)sizeof sig) return 0;
    long at = -1;
    for (long i = 0; i + n <= glen; i++)
        if (g[i] == (unsigned char)sig[0] && memcmp(g + i, sig, (size_t)n) == 0) { at = i; break; }
    if (at < 0) return 0;
    long base = at - 0x40;                       /* wheel block precedes the path */
    if (base < 0 || base + 392 + 4 > glen) return 0;
    float fx, rx, fy, ry;                        /* front/rear axle X, front/rear half-track Y */
    memcpy(&fx, g + base + 288, 4); memcpy(&rx, g + base + 384, 4);
    memcpy(&fy, g + base + 292, 4); memcpy(&ry, g + base + 388, 4);
    fy = fy < 0 ? -fy : fy; ry = ry < 0 ? -ry : ry;
    /* sanity-gate to real car proportions so a mismatched record can't misplace */
    if (!(fx > 0.3f && fx < 2.5f && rx < -0.3f && rx > -2.5f &&
          fy > 0.4f && fy < 1.3f && ry > 0.4f && ry < 1.3f)) return 0;
    w->front_axle = fx; w->rear_axle = rx;
    w->front_track = 2.0f * fy; w->rear_track = 2.0f * ry;
    return 1;
}

/* ---- DXT1 / BC1 decode ---- */
static void n2_rgb565(uint16_t c, unsigned char *o) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    o[0] = (r << 3) | (r >> 2); o[1] = (g << 2) | (g >> 4); o[2] = (b << 3) | (b >> 2);
}
/* alf: optional w*h output. DXT1 carries one-bit transparency ONLY in the
   c0 <= c1 mode, where palette index 3 means "transparent black"; in the
   c0 > c1 mode every index is opaque. Anything else would be inventing a
   chroma key out of black pixels. */
static void n2_dxt1(const unsigned char *src, int w, int h, unsigned char *out,
                    unsigned char *alf) {
    int bi = 0;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            uint16_t c0 = src[bi] | src[bi+1] << 8, c1 = src[bi+2] | src[bi+3] << 8;
            uint32_t bits = n2_u32(src + bi + 4); bi += 8;
            unsigned char pal[4][3];
            n2_rgb565(c0, pal[0]); n2_rgb565(c1, pal[1]);
            for (int j = 0; j < 3; j++) {
                if (c0 > c1) { pal[2][j] = (2*pal[0][j]+pal[1][j])/3; pal[3][j] = (pal[0][j]+2*pal[1][j])/3; }
                else         { pal[2][j] = (pal[0][j]+pal[1][j])/2; pal[3][j] = 0; }
            }
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    int x = bx+px, y = by+py; if (x >= w || y >= h) continue;
                    int idx = (bits >> (2*(py*4+px))) & 3;
                    memcpy(out + (y*w+x)*3, pal[idx], 3);
                    if (alf) alf[y*w+x] = (c0 <= c1 && idx == 3) ? 0 : 255;
                }
        }
}

/* alf: optional w*h output for the block's 4-bit explicit alpha (may be NULL) */
static void n2_dxt3(const unsigned char *src, int w, int h, unsigned char *out,
                    unsigned char *alf) {
    int bi = 0;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            /* 8-byte 4-bit alpha, then 8-byte DXT1 colour */
            uint16_t c0 = src[bi+8] | src[bi+9] << 8, c1 = src[bi+10] | src[bi+11] << 8;
            uint32_t bits = n2_u32(src + bi + 12);
            unsigned char pal[4][3];
            n2_rgb565(c0, pal[0]); n2_rgb565(c1, pal[1]);
            for (int j = 0; j < 3; j++) {
                pal[2][j] = (2*pal[0][j]+pal[1][j])/3;
                pal[3][j] = (pal[0][j]+2*pal[1][j])/3;
            }
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    int x = bx+px, y = by+py; if (x >= w || y >= h) continue;
                    int idx = (bits >> (2*(py*4+px))) & 3;
                    memcpy(out + (y*w+x)*3, pal[idx], 3);
                    if (alf) {
                        int a4 = (src[bi + (py*4+px)/2] >> (((py*4+px)&1)*4)) & 15;
                        alf[y*w+x] = (unsigned char)(a4 * 17);
                    }
                }
            bi += 16;
        }
}

/* DXT5/BC3: 8-byte interpolated alpha (2 endpoints + 3-bit indices), then the
 * 8-byte DXT1 colour block. alf optional. */
static void n2_dxt5(const unsigned char *src, int w, int h, unsigned char *out,
                    unsigned char *alf) {
    int bi = 0;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            int a0 = src[bi], a1 = src[bi+1], al[8];
            al[0] = a0; al[1] = a1;
            if (a0 > a1) for (int k = 1; k <= 6; k++) al[k+1] = ((7-k)*a0 + k*a1)/7;
            else { for (int k = 1; k <= 4; k++) al[k+1] = ((5-k)*a0 + k*a1)/5; al[6]=0; al[7]=255; }
            uint64_t abits = 0; for (int k = 0; k < 6; k++) abits |= (uint64_t)src[bi+2+k] << (8*k);
            uint16_t c0 = src[bi+8] | src[bi+9] << 8, c1 = src[bi+10] | src[bi+11] << 8;
            uint32_t bits = n2_u32(src + bi + 12);
            unsigned char pal[4][3];
            n2_rgb565(c0, pal[0]); n2_rgb565(c1, pal[1]);
            for (int j = 0; j < 3; j++) {
                pal[2][j] = (2*pal[0][j]+pal[1][j])/3; pal[3][j] = (pal[0][j]+2*pal[1][j])/3;
            }
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    int x = bx+px, y = by+py; if (x >= w || y >= h) continue;
                    int pi = py*4+px;
                    memcpy(out + (y*w+x)*3, pal[(bits >> (2*pi)) & 3], 3);
                    if (alf) alf[y*w+x] = (unsigned char)al[(abits >> (3*pi)) & 7];
                }
            bi += 16;
        }
}

/* JDLZ decompress (EA NFS). Writes up to out_cap bytes; returns bytes written. */
static int n2_jdlz(const unsigned char *s, int slen, unsigned char *out, int out_cap) {
    if (slen < 16 || memcmp(s, "JDLZ", 4)) return 0;
    int usize = (int)n2_u32(s + 8);
    if (usize > out_cap) usize = out_cap;
    int op = 0, ip = 16, flags1 = 1, flags2 = 1;
    while (ip < slen && op < usize) {
        if (flags1 == 1) flags1 = s[ip++] | 0x100;
        if (flags2 == 1) flags2 = s[ip++] | 0x100;
        if (flags1 & 1) {
            int len, dist;
            if (flags2 & 1) { len = (s[ip+1] | ((s[ip] & 0xF0) << 4)) + 3; dist = (s[ip] & 0x0F) + 1; }
            else            { dist = (s[ip+1] | ((s[ip] & 0xE0) << 3)) + 17; len = (s[ip] & 0x1F) + 3; }
            ip += 2;
            for (int k = 0; k < len && op < usize; k++) { out[op] = out[op - dist]; op++; }
            flags2 >>= 1;
        } else out[op++] = s[ip++];
        flags1 >>= 1;
    }
    return op;
}

/* ---- EA "HUFF" (EAC canonical-Huffman, methods 30FBh..35FBh) ----
 * Used by CARS/ * /VINYLS.BIN blobs ("HUFF" wrapper + EAC stream). Format per
 * Martin Korth's documentation (problemkaputt.de/psx-spx.htm, "CDROM File
 * Compression EA Methods (Huffman)"), reimplemented from the spec:
 * big-endian bitstream; header = u16 method + optional 3B + u24 size + u8
 * escape; canonical code widths until the Kraft sum fills the code space;
 * symbols delta-assigned over not-yet-used values; ESC = {varlen 0: EOS-bit
 * or raw literal; varlen n: repeat previous byte n times}. */

typedef struct { const unsigned char *b; long len, pos; } N2Bits;
static uint32_t n2_bits(N2Bits *s, int n) {
    uint32_t v = 0;
    while (n-- > 0) {
        if (s->pos >= s->len * 8) return v << (n + 1);   /* starved: zeros */
        v = v << 1 | ((s->b[s->pos >> 3] >> (7 - (s->pos & 7))) & 1);
        s->pos++;
    }
    return v;
}
static uint32_t n2_huff_varlen(N2Bits *s) {
    int num = 2;
    while (n2_bits(s, 1) == 0 && num < 24) num++;
    return n2_bits(s, num) + (1u << num) - 4;
}

/* Decompress one EAC Huffman stream into out. Returns bytes written (0=fail). */
static long n2_huff(const unsigned char *src, long slen, unsigned char *out, long cap) {
    N2Bits s = { src, slen, 0 };
    uint32_t method = n2_bits(&s, 16);
    if ((method & 0xF0FF) != 0x30FB) return 0;
    if (method & 0x100) n2_bits(&s, 24);
    long dsize = (long)n2_bits(&s, 24);
    if (dsize <= 0 || dsize > cap) return 0;
    int esc = (int)n2_bits(&s, 8);
    int numcodes[17] = {0}, width = 0, total = 0;
    uint32_t code = 0;
    while (width < 16 && (code << (16 - width)) < 0x10000u) {
        uint32_t n = n2_huff_varlen(&s);
        width++;
        if (n > 256) return 0;
        numcodes[width] = (int)n; total += (int)n;
        code = code * 2 + n;
    }
    if (total < 1 || total > 256) return 0;
    unsigned char vals[256], defined[256];
    memset(defined, 0, sizeof defined);
    int dat = 0xFF;
    for (int i = 0; i < total; i++) {
        long n = (long)n2_huff_varlen(&s) + 1;
        if (n > 256) return 0;
        while (n > 0) {
            dat = (dat + 1) & 0xFF;
            if (!defined[dat]) n--;
        }
        defined[dat] = 1; vals[i] = (unsigned char)dat;
    }
    /* canonical decode tables */
    uint32_t first_code[18]; int first_idx[18];
    uint32_t c = 0; int idx = 0;
    for (int w = 1; w <= width; w++) {
        first_code[w] = c; first_idx[w] = idx;
        c = (c + (uint32_t)numcodes[w]) << 1; idx += numcodes[w];
    }
    long op = 0;
    while (op < dsize && s.pos < s.len * 8) {
        uint32_t v = 0; int w = 0, sym = -1;
        while (w < width) {
            v = v << 1 | n2_bits(&s, 1); w++;
            if (v - first_code[w] < (uint32_t)numcodes[w]) {
                sym = vals[first_idx[w] + (int)(v - first_code[w])]; break;
            }
        }
        if (sym < 0) return 0;                    /* corrupt tree/stream */
        if (sym != esc) { out[op++] = (unsigned char)sym; continue; }
        uint32_t n = n2_huff_varlen(&s);
        if (n == 0) {
            if (n2_bits(&s, 1) == 1) break;       /* end of stream */
            out[op++] = (unsigned char)n2_bits(&s, 8);
        } else {
            unsigned char prev = op ? out[op-1] : 0;
            while (n-- > 0 && op < dsize) out[op++] = prev;
        }
    }
    if (op != dsize) return 0;
    if ((method & 0xFEFF) == 0x32FB) {            /* optional delta filters */
        unsigned char x = 0;
        for (long i = 0; i < dsize; i++) { x = (unsigned char)(x + out[i]); out[i] = x; }
    } else if ((method & 0xFEFF) == 0x34FB) {
        unsigned char x = 0, y = 0;
        for (long i = 0; i < dsize; i++) { x = (unsigned char)(x + out[i]);
                                           y = (unsigned char)(y + x); out[i] = y; }
    }
    return op;
}

/* Locate the car TPK offset-slot table (0x33310003, 24-byte records) inside the
 * 0xb3310000 header block. Returns the payload pointer + size, or NULL. */
static const unsigned char *n2_tpk_slots(const unsigned char *d, long len, uint32_t *outsz) {
    long hoff = -1;
    for (long i = 0; i + 4 < len; i++)
        if (d[i]==0 && d[i+1]==0 && d[i+2]==0x31 && d[i+3]==0xb3) { hoff = i + 8; break; }
    if (hoff < 0) return NULL;
    uint32_t hsz = n2_u32(d + hoff - 4); long hend = hoff + hsz, o = hoff;
    while (o + 8 <= hend) {
        uint32_t m = n2_u32(d + o), s = n2_u32(d + o + 4);
        if (m == 0x33310003u) { *outsz = s; return d + o + 8; }
        o += 8 + s;
    }
    return NULL;
}

/* All texture keys present in the car TPK (offset-slot table). Returns count. */
static int n2_car_tex_keys(const unsigned char *d, long len, uint32_t *keys, int maxk) {
    uint32_t sz; const unsigned char *p = n2_tpk_slots(d, len, &sz);
    if (!p) return 0;
    int n = 0;
    for (uint32_t i = 0; i + 0x18 <= sz && n < maxk; i += 0x18) keys[n++] = n2_u32(p + i);
    return n;
}

/* Byte size of a full mip chain for a w×h texture (bpb = 8 DXT1 / 16 DXT3). */
static int n2_mipbytes2(int w, int h, int bpb) {
    int t = 0;
    for (;;) {
        int bw = w < 4 ? 1 : w/4, bh = h < 4 ? 1 : h/4;
        t += bw*bh*bpb;
        if (w == 1 && h == 1) break;
        if (w > 1) w /= 2; if (h > 1) h /= 2;
    }
    return t;
}
static int n2_mipbytes(int s, int bpb) { return n2_mipbytes2(s, s, bpb); }

/* Decode ONE car texture by its TPK key: find the slot, JDLZ-decompress, then
 * recover square dims + DXT1/DXT3 by matching the mip-chain size to DecodedSize
 * (car textures are square; format isn't stored, so it's inferred). Returns 1. */
static int n2_load_car_tex_by_key(const unsigned char *d, long len, uint32_t key, N2Tex *t) {
    memset(t, 0, sizeof *t);   /* all outputs defined on success AND failure */
    uint32_t sz; const unsigned char *p = n2_tpk_slots(d, len, &sz);
    if (!p) return 0;
    int absoff = 0, enc = 0, dec = 0; uint32_t hfe = 0;
    for (uint32_t i = 0; i + 0x18 <= sz; i += 0x18)
        if (n2_u32(p + i) == key) {
            absoff = (int)n2_u32(p + i + 4); enc = (int)n2_u32(p + i + 8);
            dec = (int)n2_u32(p + i + 12);
            hfe = n2_u32(p + i + 16);   /* +0x10: header_from_end (was mis-read as RefCount) */
            break;
        }
    if (dec <= 0 || absoff < 0 || (long)absoff + enc > len) return 0;
    unsigned char *raw = (unsigned char *)malloc(dec);
    if (enc >= 20 && memcmp(d + absoff, "HUFF", 4) == 0) {
        /* "HUFF"-wrapped blob (16-byte wrapper + EAC Huffman stream) — used
           by every VINYLS.BIN slot and by some TEXTURES.BIN slots. If the
           payload carries a vinyl record it's decoded here; otherwise it's
           raw DXT and falls through to the square solver below. */
        if (n2_huff(d + absoff + 16, enc - 16, raw, dec) != dec)
            { free(raw); return 0; }
        /* HUFF payload = pixel data (base level only) + a 144-byte trailing
           record: name[24], key u32 @+0x18 (validates the slot), w,h u16s
           @+0x38, format u32 @+0x84 — ASCII "DXT1"/"DXT3", or a code
           (0x29 etc.) for P8-style palette indices (vinyls). */
        if (dec >= 144) {
            const unsigned char *rec = raw + dec - 144;
            int w = rec[0x38] | rec[0x39] << 8, h = rec[0x3a] | rec[0x3b] << 8;
            uint32_t fmt = n2_u32(rec + 0x84);
            if (w >= 8 && h >= 8 && w <= 2048 && h <= 2048 &&
                n2_u32(rec + 0x18) == key && rec[0] >= 'A' && rec[0] <= 'Z') {
                long n = (long)w * h;
                if (fmt == 0x31545844 && n/2 + 144 <= dec) {        /* "DXT1" */
                    t->w = w; t->h = h; t->alpha = NULL;
                    t->rgb = (unsigned char *)malloc(n * 3);
                    n2_dxt1(raw, w, h, t->rgb, NULL);
                    t->dxtlen = (int)(n/2); t->dxtfmt = 1;          /* base-level blocks */
                    t->dxt = (unsigned char *)malloc(t->dxtlen);
                    memcpy(t->dxt, raw, t->dxtlen);
                    free(raw); return 1;
                }
                if (fmt == 0x33545844 && n + 144 <= dec) {          /* "DXT3" */
                    t->w = w; t->h = h;
                    t->rgb = (unsigned char *)malloc(n * 3);
                    t->alpha = (unsigned char *)malloc(n);
                    n2_dxt3(raw, w, h, t->rgb, t->alpha);
                    t->dxtlen = (int)n; t->dxtfmt = 3;
                    t->dxt = (unsigned char *)malloc(t->dxtlen);
                    memcpy(t->dxt, raw, t->dxtlen);
                    free(raw); return 1;
                }
                if (n + 144 <= dec) {
                    /* palette indices (vinyls). The palette ships ALL-ZERO —
                       the game recolors vinyls from the player's colours —
                       so synthesize a dark cut-vinyl look: most-frequent
                       index = transparent background. */
                    long hist[256] = {0};
                    for (long i = 0; i < n; i++) hist[raw[i]]++;
                    int bg = 0;
                    for (int i = 1; i < 256; i++) if (hist[i] > hist[bg]) bg = i;
                    t->w = w; t->h = h;
                    t->rgb = (unsigned char *)malloc(n * 3);
                    t->alpha = (unsigned char *)malloc(n);
                    for (long i = 0; i < n; i++) {
                        int ix = raw[i];
                        unsigned char v = (unsigned char)(20 + (ix & 15) * 8);
                        t->rgb[i*3] = v; t->rgb[i*3+1] = v; t->rgb[i*3+2] = v;
                        t->alpha[i] = ix == bg ? 0 : 255;
                    }
                    free(raw); return 1;
                }
            }
        }
    } else
        n2_jdlz(d + absoff, enc, raw, dec);
    /* Exact format from the embedded texture header (independent RE), replacing
       the old square-dimension size guess. The info header sits header_from_end
       (slot +0x10) bytes before the payload end; P = dec - header_from_end +
       0x88 lands on it. key @P+0 validates the slot, w/h are u16 @P+0x20/0x22,
       and the byte @P+0x26 is the exact ImageCompressionType. */
    long P = (long)dec - (long)hfe + 0x88;
    if (P < 0 || P + 0x28 > dec) { free(raw); return 0; }
    if (n2_u32(raw + P) != key) { free(raw); return 0; }          /* header sanity */
    int tw = raw[P+0x20] | raw[P+0x21] << 8;
    int th = raw[P+0x22] | raw[P+0x23] << 8;
    unsigned fmt = raw[P+0x26];
    if (tw < 1 || th < 1 || tw > 4096 || th > 4096) { free(raw); return 0; }
    long n = (long)tw * th;
    t->w = tw; t->h = th;
    if (fmt == 0x20) {                    /* uncompressed BGRA: no compressed upload */
        if (n*4 > P) { free(raw); return 0; }
        t->rgb = (unsigned char *)malloc(n*3);
        t->alpha = (unsigned char *)malloc(n);
        for (long q = 0; q < n; q++) {    /* B,G,R,A -> RGB + alpha plane */
            t->rgb[q*3]   = raw[q*4+2]; t->rgb[q*3+1] = raw[q*4+1];
            t->rgb[q*3+2] = raw[q*4];   t->alpha[q]   = raw[q*4+3];
        }
        free(raw); return 1;              /* dxtfmt stays 0 -> RGBA upload path */
    }
    if (fmt == 0x22) {                    /* DXT1 */
        t->alpha = NULL; t->rgb = (unsigned char *)malloc(n*3);
        n2_dxt1(raw, tw, th, t->rgb, NULL); t->dxtfmt = 1;
    } else if (fmt == 0x24) {             /* DXT3 */
        t->rgb = (unsigned char *)malloc(n*3); t->alpha = (unsigned char *)malloc(n);
        n2_dxt3(raw, tw, th, t->rgb, t->alpha); t->dxtfmt = 3;
    } else if (fmt == 0x26) {             /* DXT5 */
        t->rgb = (unsigned char *)malloc(n*3); t->alpha = (unsigned char *)malloc(n);
        n2_dxt5(raw, tw, th, t->rgb, t->alpha); t->dxtfmt = 5;
    } else {                              /* palettised (0x08/0x80/0x81) / unknown */
        fprintf(stderr, "car tex %08x: unhandled format 0x%02x (%dx%d) skipped\n", key, fmt, tw, th);
        free(raw); return 0;
    }
    /* Keep the whole compressed mip chain (base..1x1) for the GPU fast path;
       the header sits right after it, so cap at P. */
    int bpb = (t->dxtfmt == 1) ? 8 : 16;
    t->dxtlen = n2_mipbytes2(tw, th, bpb);
    if (t->dxtlen > (int)P) t->dxtlen = (int)P;
    t->dxt = (unsigned char *)malloc(t->dxtlen);
    memcpy(t->dxt, raw, t->dxtlen);
    free(raw);
    return 1;
}

/* Find a named texture in the file's local TPK and decode it. Returns 1 on hit. */
static int n2_load_texture(const unsigned char *d, long len, const char *name, N2Tex *t) {
    memset(t, 0, sizeof *t);   /* all outputs defined on success AND failure */
    /* TPK header block marker: magic 0xb3310000 (LE bytes 00 00 31 b3) */
    long hdr = -1;
    for (long i = 0; i + 4 < len; i++)
        if (d[i]==0x00 && d[i+1]==0x00 && d[i+2]==0x31 && d[i+3]==0xb3) { hdr = i; break; }
    if (hdr < 0) return 0;
    /* pixel block 0xb3320000 (LE 00 00 32 b3) after the header */
    long pix = -1;
    for (long i = hdr; i + 4 < len; i++)
        if (d[i]==0x00 && d[i+1]==0x00 && d[i+2]==0x32 && d[i+3]==0xb3) { pix = i + 8; break; }
    if (pix < 0) return 0;
    long hbeg = hdr + 8, hsize = n2_u32(d + hdr + 4);
    long nlen = (long)strlen(name);
    for (long i = hbeg; i + 0x3c < hbeg + hsize; i++) {
        if (memcmp(d + i, name, nlen) == 0 && d[i + nlen] == 0) {
            uint32_t off = n2_u32(d + i + 0x24);
            uint16_t w = d[i+0x38] | d[i+0x39] << 8, hh = d[i+0x3a] | d[i+0x3b] << 8;
            if (w == 0 || hh == 0 || w > 2048 || hh > 2048) return 0;
            t->w = w; t->h = hh;
            t->rgb = (unsigned char *)malloc((long)w * hh * 3);
            t->alpha = NULL;
            n2_dxt1(d + pix + off, w, hh, t->rgb, NULL);
            return 1;
        }
    }
    return 0;
}

/* Rough "is this decoded image just noise?" test: mean abs colour difference of
 * horizontally-spaced sampled pixels. Real textures are locally coherent; a
 * wrong format/swizzle decodes to high-frequency rainbow. Used to reject a
 * texture we can't decode correctly (e.g. a swizzled surface) so it falls back
 * instead of binding garbage. */
static int n2_tex_noise(const N2Tex *t) {
    long sum = 0, cnt = 0;
    for (int y = 0; y < t->h; y += 4)
        for (int x = 0; x + 4 < t->w; x += 4) {
            const unsigned char *a = t->rgb + ((long)y*t->w + x)*3, *b = a + 12;
            sum += abs(a[0]-b[0]) + abs(a[1]-b[1]) + abs(a[2]-b[2]); cnt++;
        }
    return cnt && (sum / (cnt*3)) > 55;
}

/* A STREAM TPK opened once for repeated by-hash lookups. A region can have MANY
 * 0xb3310000 header blocks (each paired with a following 0x33320002 pixel block);
 * a single O(len) pass records them all, so a huge shared "master" region can be
 * mmap'd and queried without re-scanning it per texture. */
typedef struct { long hbeg, hsize, dbase; } N2TpkBlk;
typedef struct { N2TpkBlk *blk; int nblk; } N2Tpk;
static N2Tpk n2_tpk_open(const unsigned char *d, long len) {
    N2Tpk t; t.blk = NULL; t.nblk = 0; int cap = 0;
    long ph = -1; uint32_t phs = 0;
    for (long i = 0; i + 8 < len; i++) {
        if (d[i]==0 && d[i+1]==0 && d[i+2]==0x31 && d[i+3]==0xb3) {    /* header 0xb3310000 */
            ph = i + 8; phs = n2_u32(d + i + 4);
        } else if (ph >= 0 && d[i]==0x02 && d[i+1]==0x00 && d[i+2]==0x32 && d[i+3]==0x33) {
            long pbase = i + 8, room = len - pbase;       /* pixels 0x33320002 */
            if (room > 0x40000000) room = 0x40000000;
            if (t.nblk == cap) { cap = cap ? cap*2 : 64;
                t.blk = (N2TpkBlk *)realloc(t.blk, (size_t)cap*sizeof(N2TpkBlk)); }
            t.blk[t.nblk].hbeg = ph; t.blk[t.nblk].hsize = phs;
            t.blk[t.nblk].dbase = pbase + n2_skip_filler(d + pbase, (int)room);
            t.nblk++; ph = -1;
        }
    }
    return t;
}
/* Collect every record hash (@+0x18) across all blocks. Returns the count. */
static int n2_tpk_keys(const unsigned char *d, N2Tpk t, uint32_t *keys, int maxk) {
    int n = 0;
    for (int b = 0; b < t.nblk && n < maxk; b++) {
        long hbeg = t.blk[b].hbeg, hend = hbeg + t.blk[b].hsize;
        for (long i = hbeg; i + 0x40 < hend && n < maxk; i++) {
            if (!(d[i] >= 'A' && d[i] <= 'Z')) continue;
            if (d[i+0x17] != 0) continue;                 /* 24-byte name null-terminated */
            keys[n++] = n2_u32(d + i + 0x18);
            i += 0x7b;                                    /* next 0x7c record */
        }
    }
    return n;
}

/* Decode one texture by its record hash, searching all blocks. Fields (Nikki's
 * layout minus a 0x0C name-pad prefix): +0x18 BinKey +0x24 Offset
 * +0x28 PaletteOffset +0x2c Size +0x30 PaletteSize +0x38 W(u16) +0x3a H(u16).
 * P8 when PaletteSize >= 1024 (256-entry RGBA); else DXT1/DXT3 by Size. */
static int n2_tpk_decode(const unsigned char *d, long len, N2Tpk t, uint32_t hash, N2Tex *tex) {
    /* every output field defined on BOTH paths, so a failed decode can never
       leave a caller reading a stale alpha/dxt pointer from a reused struct */
    memset(tex, 0, sizeof *tex);
    for (int b = 0; b < t.nblk; b++) {
        long hbeg = t.blk[b].hbeg, hend = hbeg + t.blk[b].hsize, dbase = t.blk[b].dbase;
        for (long i = hbeg; i + 0x40 < hend; i++) {
            if (!(d[i] >= 'A' && d[i] <= 'Z')) continue;
            if (n2_u32(d + i + 0x18) != hash) continue;
            uint32_t off = n2_u32(d + i + 0x24), paloff = n2_u32(d + i + 0x28);
            uint32_t sz = n2_u32(d + i + 0x2c), palsz = n2_u32(d + i + 0x30);
            int w = d[i+0x38] | d[i+0x39]<<8, hh = d[i+0x3a] | d[i+0x3b]<<8;
            if (w<=0 || hh<=0 || w>4096 || hh>4096) continue;
            tex->w = w; tex->h = hh; tex->rgb = (unsigned char *)malloc((long)w*hh*3);
            tex->order = d[i+0x45]; tex->usage = d[i+0x49];
            tex->blend = d[i+0x4a]; tex->wz = d[i+0x4b];
            tex->dxt = NULL; tex->dxtlen = 0; tex->dxtfmt = 0;
            /* M132-R2: alpha is DECODED, not discarded. The palettes are RGBA
               and the DXT blocks carry real alpha; throwing it away was why a
               panorama sheet rendered as an opaque black-edged slab. It is only
               RETAINED when it says something -- an all-255 plane is freed, so
               every opaque road and building keeps the exact RGB upload and the
               exact rendering it had before. */
            unsigned char *alf = (unsigned char *)malloc((long)w*hh);
            tex->alpha = NULL;
            tex->afmt = 0;
            /* THE FORMAT IS IN THE RECORD. Guessing it -- palette if a palette
               is present, otherwise DXT3 or DXT1 by size -- is wrong for the
               uncompressed ones: a 32-bit BGRA sheet read as DXT1 comes out as
               noise with its red and blue swapped, which is exactly the pink
               signage. It also decodes the shared headlight texture to solid
               black, putting the lamps out. */
            unsigned fmt = d[i+0x3e];
            if (fmt == 0x20) {                       /* uncompressed BGRA */
                if (dbase + off + (long)w*hh*4 > len) { free(tex->rgb); free(alf); continue; }
                const unsigned char *px = d + dbase + off;
                for (long p = 0; p < (long)w*hh; p++) {
                    tex->rgb[p*3+0] = px[p*4+2];     /* B,G,R,A -> R,G,B */
                    tex->rgb[p*3+1] = px[p*4+1];
                    tex->rgb[p*3+2] = px[p*4+0];
                    if (alf) alf[p]  = px[p*4+3];
                }
                tex->afmt = 8;
            } else if (fmt == 0x08 || (palsz >= 1024 && dbase+paloff+1024 <= len
                                       && dbase+off+(long)w*hh <= len)) {
                if (palsz < 1024 || dbase+paloff+1024 > len
                    || dbase+off+(long)w*hh > len) { free(tex->rgb); free(alf); continue; }
                const unsigned char *pal = d + dbase + paloff, *ix = d + dbase + off;
                for (long p = 0; p < (long)w*hh; p++) {   /* P8: index -> RGBA palette */
                    const unsigned char *c = pal + (long)ix[p]*4;
                    tex->rgb[p*3]=c[0]; tex->rgb[p*3+1]=c[1]; tex->rgb[p*3+2]=c[2];
                    if (alf) alf[p] = c[3];
                }
                tex->afmt = 8;                      /* P8 */
            } else if (dbase + off + (long)w*hh/2 <= len) {
                if      (fmt == 0x24) { n2_dxt3(d + dbase + off, w, hh, tex->rgb, alf); tex->afmt = 3; }
                else if (fmt == 0x26) { n2_dxt5(d + dbase + off, w, hh, tex->rgb, alf); tex->afmt = 3; }
                else if (fmt == 0x22) { n2_dxt1(d + dbase + off, w, hh, tex->rgb, alf); tex->afmt = 1; }
                else {   /* record says nothing usable: fall back to the old guess */
                    int dxt3 = (long)sz > (long)w*hh*9/10;
                    if (dxt3) { n2_dxt3(d + dbase + off, w, hh, tex->rgb, alf); tex->afmt = 3; }
                    else      { n2_dxt1(d + dbase + off, w, hh, tex->rgb, alf); tex->afmt = 1; }
                }
            } else { free(tex->rgb); free(alf); continue; }
            if (alf) {
#ifdef N2_NO_WORLD_ALPHA
                free(alf);        /* A/B control build: pre-M132-R2 behaviour */
#else
                /* WHETHER ALPHA MEANS ANYTHING IS IN THE RECORD, not in the
                   image. In an opaque texture the fourth byte is not
                   transparency but leftover data, and cutting on it punches
                   holes in the road; scanning the plane instead is guesswork
                   that gets it wrong both ways -- an opaque asset whose spare
                   byte happens to vary keeps a plane it should not have, and a
                   flare sheet that is genuinely uniform loses the one it
                   needs. usage and blend say outright which it is. */
                if (n2_tex_mode(tex) == N2_DRAW_OPAQUE) {
                    free(alf);
                } else {
                    tex->alpha = alf;
                    /* Normalise to 0..255: some palettes never reach full
                       alpha -- measured peaks of 79, 190, 191 and 217 -- and
                       left as they are, a sheet that should be solid draws
                       washed out. */
                    long tot = (long)w * hh, amax = 0;
                    for (long p = 0; p < tot; p++) if (alf[p] > amax) amax = alf[p];
                    if (amax > 0 && amax < 255)
                        for (long p = 0; p < tot; p++) {
                            long v = (long)alf[p] * 255 / amax;
                            alf[p] = (unsigned char)(v > 255 ? 255 : v);
                        }
                }
#endif
            }
            return 1;
        }
    }
    return 0;
}

#endif /* NFSU2_H */
