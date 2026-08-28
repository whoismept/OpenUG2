#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    const float want[9] = {0, 1, 0, 0, -1, 0, 0, 0, 0};
    for (int i = 0; i < 9; i++) assert(near(p.matrix[i], want[i]));
    assert(near(p.matrix[10], 1.0f));
    assert(near(p.matrix[12], 100.0f) && near(p.matrix[13], -25.0f));
    assert(near(p.matrix[14], 7.5f) && near(p.matrix[15], 1.0f));

    memset(&p, 0, sizeof p);
    assert(winst_decode_placement(record, (long)sizeof record - 1, &p) == 0);
}

int main(void) {
    test_regions();
    test_placement();
    puts("world_instance_test: PASS");
    return 0;
}
