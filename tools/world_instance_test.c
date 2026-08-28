#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nfsu2.h"
#include "world_instance.h"

static void put_u16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void put_s16(unsigned char *p, int v) {
    put_u16(p, (unsigned int)(unsigned short)v);
}

static void put_u32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v & 0xfful);
    p[1] = (unsigned char)((v >> 8) & 0xfful);
    p[2] = (unsigned char)((v >> 16) & 0xfful);
    p[3] = (unsigned char)((v >> 24) & 0xfful);
}

static void put_f32(unsigned char *p, float v) {
    unsigned int bits = 0;
    memcpy(&bits, &v, sizeof bits);
    put_u32(p, bits);
}

static int near(float a, float b) {
    return fabsf(a - b) <= 1e-6f;
}

static int close3(const float *v, float x, float y, float z) {
    return near(v[0], x) && near(v[1], y) && near(v[2], z);
}

static void free_scene(N2Scene *scene) {
    for (int i = 0; i < scene->count; i++) {
        free(scene->meshes[i].verts);
        free(scene->meshes[i].idx);
        free(scene->meshes[i].vcol);
    }
    free(scene->meshes);
    memset(scene, 0, sizeof *scene);
}

static long make_regions(unsigned char *buf, size_t cap) {
    const long filler = 3;
    const long record = 0x24 + 4 * 8;
    const long child_size = filler + record;
    const long used = 8 + 8 + child_size;
    assert(cap >= (size_t)used);
    memset(buf, 0, cap);

    put_u32(buf + 0, 0x80034150ul);
    put_u32(buf + 4, (unsigned long)(8 + child_size));
    put_u32(buf + 8, 0x00034152ul);
    put_u32(buf + 12, (unsigned long)child_size);
    memset(buf + 16, 0x11, (size_t)filler);

    unsigned char *r = buf + 16 + filler;
    put_u16(r + 8, 17);
    put_u16(r + 10, 4);
    put_f32(r + 0x0c, -10.0f);
    put_f32(r + 0x10, -5.0f);
    put_f32(r + 0x14, 10.0f);
    put_f32(r + 0x18, 5.0f);
    put_f32(r + 0x24 + 0, -10.0f);
    put_f32(r + 0x24 + 4, -5.0f);
    put_f32(r + 0x24 + 8, 10.0f);
    put_f32(r + 0x24 + 12, -5.0f);
    put_f32(r + 0x24 + 16, 10.0f);
    put_f32(r + 0x24 + 20, 5.0f);
    put_f32(r + 0x24 + 24, -10.0f);
    put_f32(r + 0x24 + 28, 5.0f);
    return used;
}

static void test_regions(void) {
    unsigned char buf[256];
    long used = make_regions(buf, sizeof buf);
    WInstRegion *r = NULL;
    int nr = 0;
    assert(winst_parse_regions(buf, used, &r, &nr) == 1);
    assert(nr == 1 && r[0].id == 17 && r[0].nxy == 4);
    assert(near(r[0].bb[0], -10.0f) && near(r[0].bb[1], -5.0f));
    assert(near(r[0].bb[2], 10.0f) && near(r[0].bb[3], 5.0f));

    unsigned char selected[32] = {0};
    int home = -1;
    assert(winst_select_regions(r, nr, 0, 0, 2, selected, 32, &home) == 1);
    assert(home == 0 && selected[17] == 1);
    memset(selected, 0, sizeof selected);
    home = -1;
    assert(winst_select_regions(r, nr, 30, 0, 5, selected, 32, &home) == 0);
    assert(home == -1);
    winst_free_regions(r, nr);

    r = NULL;
    nr = 0;
    assert(winst_parse_regions(buf, used - 1, &r, &nr) == 0);
    assert(r == NULL && nr == 0);
}

static void test_placement(void) {
    unsigned char record[64];
    memset(record, 0, sizeof record);
    put_u16(record + 0x18, 3);
    put_u16(record + 0x1a, 0x8040);
    put_f32(record + 0x20, 100.0f);
    put_f32(record + 0x24, -25.0f);
    put_f32(record + 0x28, 7.5f);
    const int rot[9] = {0, -8192, 0, 8192, 0, 0, 0, 0, 8192};
    for (int i = 0; i < 9; i++) put_s16(record + 0x2c + 2 * i, rot[i]);

    WInstPlacement p;
    assert(winst_decode_placement(record, (long)sizeof record, &p) == 1);
    assert(p.type_index == 3 && p.flags == 0x8040);
    assert(near(p.bounds_min[0], 100.0f) && near(p.bounds_min[1], -25.0f));
    assert(near(p.bounds_min[2], 7.5f));
    assert(near(p.bounds_max[0], 100.0f) && near(p.bounds_max[1], -25.0f));
    assert(near(p.bounds_max[2], 7.5f));
    const float want[16] = {
        0, 1, 0, 0, -1, 0, 0, 0,
        0, 0, 1, 0, 100, -25, 7.5f, 1
    };
    for (int i = 0; i < 16; i++) assert(near(p.matrix[i], want[i]));

    memset(&p, 0, sizeof p);
    assert(winst_decode_placement(record, (long)sizeof record - 1, &p) == 0);
}

static void test_direct_placement(void) {
    float verts[] = {
        0, 0, 0, 0.25f, 0.50f,
        2, 0, 0, 0.75f, 0.50f,
        0, 1, 0, 0.25f, 1.00f,
    };
    uint16_t idx[] = { 0, 1, 2 };
    unsigned char vcol[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    };
    N2Mesh src;
    memset(&src, 0, sizeof src);
    src.verts = verts;
    src.vcol = vcol;
    src.nverts = 3;
    src.idx = idx;
    src.nidx = 3;
    src.cat = N2_OTHER;
    src.texkey = 0x12345678u;
    src.scen = N2_SC_PROP;
    src.vrepair = 1;
    snprintf(src.sname, sizeof src.sname, "XO_SOURCE");

    WInstPlacement p;
    memset(&p, 0, sizeof p);
    p.matrix[0] = 0;  p.matrix[1] = 1;  p.matrix[2] = 0;
    p.matrix[4] = -1; p.matrix[5] = 0;  p.matrix[6] = 0;
    p.matrix[8] = 0;  p.matrix[9] = 0;  p.matrix[10] = 1;
    p.matrix[12] = 100; p.matrix[13] = -25; p.matrix[14] = 7.5f;
    p.matrix[15] = 1;

    N2Scene dst;
    WInstStats st;
    memset(&dst, 0, sizeof dst);
    memset(&st, 0, sizeof st);
    assert(winst_place_mesh(&dst, &src, p.matrix, "XO_TEST", &st) == 1);
    assert(dst.count == 1 && dst.meshes[0].inst == 1);
    assert(strcmp(dst.meshes[0].aname, "XO_TEST") == 0);
    assert(close3(dst.meshes[0].verts + 0, 100.0f, -25.0f, 7.5f));
    assert(close3(dst.meshes[0].verts + 5, 100.0f, -23.0f, 7.5f));
    assert(close3(dst.meshes[0].verts + 10, 99.0f, -25.0f, 7.5f));
    assert(dst.meshes[0].verts != src.verts && dst.meshes[0].idx != src.idx);
    assert(dst.meshes[0].vcol != src.vcol);
    assert(dst.meshes[0].cat == src.cat && dst.meshes[0].texkey == src.texkey);
    assert(dst.meshes[0].scen == src.scen && dst.meshes[0].vrepair == src.vrepair);
    assert(strcmp(dst.meshes[0].sname, src.sname) == 0);

    src.verts[0] = NAN;
    assert(winst_place_mesh(&dst, &src, p.matrix, "XO_TEST", &st) == 0);
    assert(dst.count == 1 && st.rejected_meshes == 1);
    src.verts[0] = 0;
    free_scene(&dst);
}

int main(void) {
    test_regions();
    test_placement();
    test_direct_placement();
    puts("world_instance_test: PASS");
    return 0;
}
