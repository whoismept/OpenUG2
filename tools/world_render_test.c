/* M136 GL-free regressions for authored world-render records. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nfsu2.h"

typedef struct { unsigned char b[1024]; long n; } TestBuf;

static void put_u32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_f32(unsigned char *p, float v) { memcpy(p, &v, 4); }

static void chunk(TestBuf *out, uint32_t magic,
                  const unsigned char *payload, long size) {
    assert(out->n + 8 + size <= (long)sizeof out->b);
    put_u32(out->b + out->n, magic);
    put_u32(out->b + out->n + 4, (uint32_t)size);
    memcpy(out->b + out->n + 8, payload, (size_t)size);
    out->n += 8 + size;
}

static void light_record(unsigned char rec[96], int enabled, uint32_t rgba,
                         float x, float y, float z, float r_out, float r_in) {
    memset(rec, 0, 96);
    rec[7] = (unsigned char)enabled;
    put_u32(rec + 0x0c, rgba);
    put_f32(rec + 0x10, x); put_f32(rec + 0x14, y); put_f32(rec + 0x18, z);
    put_f32(rec + 0x1c, r_out); put_f32(rec + 0x30, r_in);
}

static TestBuf light_leaf(const unsigned char *records, long bytes) {
    unsigned char payload[512];
    assert(bytes + 3 <= (long)sizeof payload);
    memset(payload, 0x11, 3);
    memcpy(payload + 3, records, (size_t)bytes);
    TestBuf leaf = {{0}, 0};
    chunk(&leaf, 0x00135003u, payload, bytes + 3);
    TestBuf root = {{0}, 0};
    chunk(&root, 0x80135000u, leaf.b, leaf.n);
    return root;
}

static int near(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void) {
    unsigned char rec[96], pair[192];
    N2LightSrc out[8];

    light_record(rec, 1, 0xff969696u, 10.0f, -20.0f, 3.0f, 30.0f, 10.0f);
    TestBuf valid = light_leaf(rec, sizeof rec);
    assert(n2_load_light_sources(valid.b, valid.n, out, 8) == 1);
    assert(near(out[0].pos[0], 10.0f) && near(out[0].pos[1], -20.0f) &&
           near(out[0].pos[2], 3.0f));
    assert(out[0].rgba == 0xff969696u);
    assert(near(out[0].r_out, 30.0f) && near(out[0].r_in, 10.0f));

    light_record(rec, 0, 0xff969696u, 10, -20, 3, 30, 10);
    TestBuf disabled = light_leaf(rec, sizeof rec);
    assert(n2_load_light_sources(disabled.b, disabled.n, out, 8) == 0);

    light_record(rec, 1, 0xff969696u, 10, -20, 3, 9999.0f, 10);
    TestBuf mapwide = light_leaf(rec, sizeof rec);
    assert(n2_load_light_sources(mapwide.b, mapwide.n, out, 8) == 0);

    memset(pair, 0, sizeof pair);
    light_record(pair, 1, 0xff969696u, 10, -20, 3, 30, 10);
    memcpy(pair + 96, pair, 96);
    TestBuf duplicate = light_leaf(pair, sizeof pair);
    assert(n2_load_light_sources(duplicate.b, duplicate.n, out, 8) == 1);

    unsigned char malformed[97]; memset(malformed, 0, sizeof malformed);
    TestBuf bad_stride = light_leaf(malformed, sizeof malformed);
    assert(n2_load_light_sources(bad_stride.b, bad_stride.n, out, 8) == 0);

    TestBuf truncated = valid;
    assert(n2_load_light_sources(truncated.b, truncated.n - 1, out, 8) == 0);

    light_record(rec, 1, 0xff969696u, NAN, -20, 3, 30, 10);
    TestBuf nonfinite = light_leaf(rec, sizeof rec);
    assert(n2_load_light_sources(nonfinite.b, nonfinite.n, out, 8) == 0);

    puts("world_render_test: PASS");
    return 0;
}
