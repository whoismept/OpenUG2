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

/* A float's low bytes can equal the filler marker. Padding must leave
   complete records, including the legacy car stream's odd filler lengths. */
static void vertex_padding(void) {
    const uint32_t bits[]={0x3f800011u,0x3f801111u,0x3f111111u};
    for(int stride=24;stride<=36;stride+=12)for(int pad=0;pad<=12;pad++)
    for(unsigned k=0;k<sizeof bits/sizeof bits[0];k++) {
        unsigned char data[256]={0};memset(data,0x11,(size_t)pad);
        put_u32(data+pad,bits[k]);put_f32(data+pad+stride,2);
        put_f32(data+pad+2*stride+4,2);
        uint16_t indices[]={0,1,2};memcpy(data+160,indices,sizeof indices);
        N2Leaf v={0,(uint32_t)(pad+3*stride)},i={160,sizeof indices};
        N2Scene scene={0};
        n2_add_pair(data,v,i,N2_ROAD,&scene,stride,stride==24?16:28,
                    stride==24?12:24,1,0,NULL,0,-1,N2_DRAW_OPAQUE);
        assert(scene.count==1 && scene.meshes[0].nverts==3 && scene.meshes[0].nidx==3);
        assert(!memcmp(scene.meshes[0].verts,&bits[k],4));
        free(scene.meshes[0].verts);free(scene.meshes[0].idx);free(scene.meshes[0].vcol);free(scene.meshes);
    }
    /* Keep all-filler and incomplete vertex leaves rejected. */
    unsigned char data[80];memset(data,0x11,sizeof data);
    uint16_t indices[]={0,1,2};memcpy(data+72,indices,sizeof indices);
    N2Scene scene={0};N2Leaf v={0,72},i={72,sizeof indices};
    n2_add_pair(data,v,i,N2_ROAD,&scene,24,16,12,1,0,NULL,0,-1,N2_DRAW_OPAQUE);
    assert(!scene.count);memset(data,0,72);v.size=71;
    n2_add_pair(data,v,i,N2_ROAD,&scene,24,16,12,1,0,NULL,0,-1,N2_DRAW_OPAQUE);
    assert(!scene.count);
    memset(data,0,72);memset(data,0x11,24);v.size=60;
    n2_add_pair(data,v,i,N2_ROAD,&scene,24,16,12,1,0,NULL,0,-1,N2_DRAW_OPAQUE);
    assert(!scene.count);free(scene.meshes);
}

int main(void) {
    vertex_padding();
    const char *ground_names[]={"TRN_SI_DRIFTMARKER01_CHOP_A", "TRN_SI_ROAD01_CHOP_A3_R4", "TRN_TEST_TERRAINA",
        "TRN_PARKINGLOT_02_CHOP_C5_R", "TRN_GRASS_C5_R", "TRN_FOUNDATION_C5_R",
        "XO_ROADSIGNB_1A_00", "XT_ROADSIDE_TREE", "XO_TERRAIN_DECORATION"};
    const int categories[]={N2_OTHER,N2_ROAD,N2_TERRAIN,N2_ROAD,N2_TERRAIN,N2_TERRAIN,
        N2_OTHER,N2_OTHER,N2_OTHER};
    for(int k=0;k<9;k++) {
        TestBuf material={{0},0};
        chunk(&material,0x134011,(const unsigned char *)ground_names[k],(long)strlen(ground_names[k])+1);
        assert(n2_mesh_category(material.b,0,material.n)==categories[k]);
    }
    /* Binary bounds/matrix bytes may look like a name or a light emitter.
       Classification and identity use the bounded authored header field. */
    const char *authored[]={"TRN_TEST_ROADA", "TRN_TEST_TERRAINA", "SKYDOME", "XB_FACTORYSKYLIGHT", "XO_NEON_SIGN",
        "XO_ROADSIGNB_1A_00", "XT_ROADSIDE_TREE", "XO_TERRAIN_DECORATION", "XO_RoadsignB_1a_00"};
    const int expected[]={N2_ROAD,N2_TERRAIN,N2_SKY,N2_OTHER,N2_GLOW,N2_OTHER,N2_OTHER,N2_OTHER,N2_OTHER};
    for(int padding=0;padding<=3;padding+=3)for(int k=0;k<9;k++) {
        unsigned char info[256]={0};memset(info,0x11,(size_t)padding);
        memcpy(info+padding+0x30,"FAKE_NEON_ROAD",14);
        strcpy((char *)info+padding+0xa4,authored[k]);
        TestBuf material={{0},0};chunk(&material,0x134011,info,sizeof info);
        TestBuf nested={{0},0};chunk(&nested,0x80134010,material.b,material.n);
        char name[40];n2_mesh_name(nested.b,0,nested.n,name,sizeof name);
        assert(!strcmp(name,authored[k]));
        assert(n2_mesh_category(nested.b,0,nested.n)==expected[k]);
        char short_name[4];n2_mesh_name(nested.b,0,nested.n,short_name,sizeof short_name);
        assert(!strncmp(short_name,authored[k],3) && short_name[3]==0);
    }
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
