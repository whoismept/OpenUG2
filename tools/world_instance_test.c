#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nfsu2.h"
#include "world_instance.h"
#include "world_capture_policy.h"

/* Test the private prototype library without widening the production API. The
 * normal translation unit is still linked by the Makefile; rename only its
 * public functions in this isolated copy and expose its private helpers here. */
#define winst_parse_regions winst_parse_regions_test_copy
#define winst_free_regions winst_free_regions_test_copy
#define winst_select_regions winst_select_regions_test_copy
#define winst_decode_placement winst_decode_placement_test_copy
#define winst_place_mesh winst_place_mesh_test_copy
#define world_instance_build world_instance_build_test_copy
#define winst_test_collect_placements winst_test_collect_placements_test_copy
#define winst_test_placement_live_allocations winst_test_placement_live_allocations_test_copy
#define static
#include "../src/world_instance.c"
#undef static
#undef winst_test_placement_live_allocations
#undef winst_test_collect_placements
#undef world_instance_build
#undef winst_place_mesh
#undef winst_decode_placement
#undef winst_select_regions
#undef winst_free_regions
#undef winst_parse_regions

/* Test-only observation hook; it is deliberately not part of world_instance.h. */
long winst_test_placement_live_allocations(void);

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
    put_f32(record + 0x00, 100.0f);
    put_f32(record + 0x04, -25.0f);
    put_f32(record + 0x08, 7.5f);
    put_f32(record + 0x0c, 100.0f);
    put_f32(record + 0x10, -25.0f);
    put_f32(record + 0x14, 7.5f);
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

    float identity[16] = {
        1, 0, 0, 0, 0, 1, 0, 0,
        0, 0, 1, 0, 0, 0, 0, 1,
    };
    src.verts[0] = 100000000.0f;
    assert(winst_place_mesh(&dst, &src, identity, "XO_TEST", &st) == 1);
    assert(dst.count == 2);
    long live_before_rejections = winst_test_placement_live_allocations();
    const float invalid[] = { NAN, INFINITY, -INFINITY, 100000008.0f };
    for (int i = 0; i < (int)(sizeof invalid / sizeof invalid[0]); i++) {
        src.verts[0] = invalid[i];
        assert(winst_place_mesh(&dst, &src, identity, "XO_TEST", &st) == 0);
        assert(dst.count == 2 && st.rejected_meshes == i + 1);
        assert(winst_test_placement_live_allocations() == live_before_rejections);
    }
    src.verts[0] = 0;
    free_scene(&dst);
}

static long add_leaf(unsigned char *buf, long pos, unsigned long magic,
                     const unsigned char *body, long body_len) {
    put_u32(buf + pos, magic);
    put_u32(buf + pos + 4, (unsigned long)body_len);
    memcpy(buf + pos + 8, body, (size_t)body_len);
    return pos + 8 + body_len;
}

static void make_instance_record(unsigned char record[64], int type,
                                 float x0, float y0, float x1, float y1) {
    memset(record, 0, 64);
    put_f32(record + 0x00, x0);
    put_f32(record + 0x04, y0);
    put_f32(record + 0x08, -1.0f);
    put_f32(record + 0x0c, x1);
    put_f32(record + 0x10, y1);
    put_f32(record + 0x14, 1.0f);
    put_u16(record + 0x18, (unsigned int)type);
    put_f32(record + 0x20, 0.5f * (x0 + x1));
    put_f32(record + 0x24, 0.5f * (y0 + y1));
    for (int i = 0; i < 3; i++) put_s16(record + 0x2c + 2 * (i * 3 + i), 8192);
}

static long add_section(unsigned char *buf, long pos, int region_id,
                        const char *type_name,
                        const unsigned char *placements, int placement_count) {
    unsigned char info[60], type[68], instances[12 + 2 * 64];
    memset(info, 0, sizeof info);
    memset(type, 0, sizeof type);
    memset(instances, 0x11, 12);
    put_u32(info + 0x0c, (unsigned long)region_id);
    snprintf((char *)type, 32, "%s", type_name);
    memcpy(instances + 12, placements, (size_t)placement_count * 64);

    long section = pos;
    pos += 8;
    pos = add_leaf(buf, pos, 0x00034101ul, info, sizeof info);
    pos = add_leaf(buf, pos, 0x00034102ul, type, sizeof type);
    pos = add_leaf(buf, pos, 0x00034103ul, instances,
                   12 + (long)placement_count * 64);
    put_u32(buf + section, 0x80034100ul);
    put_u32(buf + section + 4, (unsigned long)(pos - section - 8));
    return pos;
}

typedef struct {
    int count;
    char name[32];
    WInstPlacement placement;
} VisitCapture;

static int capture_placement(const WInstPlacement *placement,
                             const char *type_name, void *userdata) {
    VisitCapture *capture = (VisitCapture *)userdata;
    capture->count++;
    capture->placement = *placement;
    snprintf(capture->name, sizeof capture->name, "%s", type_name);
    return 1;
}

static void test_section_collection(void) {
    unsigned char data[1024], first[2 * 64], second[64];
    memset(data, 0, sizeof data);
    make_instance_record(first + 0, 0, -2.0f, -2.0f, 2.0f, 2.0f);
    make_instance_record(first + 64, 0, 50.0f, 50.0f, 52.0f, 52.0f);
    make_instance_record(second, 1, -1.0f, -1.0f, 1.0f, 1.0f);
    long used = 0;
    used = add_section(data, used, 17, "XO_NEAR", first, 2);
    used = add_section(data, used, 23, "XO_INVALID", second, 1);

    VisitCapture capture;
    WInstStats stats;
    memset(&capture, 0, sizeof capture);
    memset(&stats, 0, sizeof stats);
    assert(winst_test_collect_placements(data, used, 0.0f, 0.0f, 5.0f,
                                         capture_placement, &capture, &stats) == 1);
    assert(capture.count == 1);
    assert(strcmp(capture.name, "XO_NEAR") == 0);
    assert(capture.placement.type_index == 0);
    assert(stats.instances_seen == 3);
    assert(stats.instances_in_range == 2);
    assert(stats.rejected_meshes == 1);

    unsigned char nested[sizeof data + 8];
    put_u32(nested, 0x80000001ul);
    put_u32(nested + 4, (unsigned long)used);
    memcpy(nested + 8, data, (size_t)used);
    memset(&capture, 0, sizeof capture);
    memset(&stats, 0, sizeof stats);
    assert(winst_test_collect_placements(nested, used + 8,
                                         0.0f, 0.0f, 5.0f,
                                         capture_placement, &capture, &stats) == 1);
    assert(capture.count == 1 && stats.instances_seen == 3);
    assert(winst_test_collect_placements(data, used - 1,
                                         0.0f, 0.0f, 5.0f,
                                         capture_placement, &capture, &stats) == 0);
    assert(winst_test_collect_placements(nested, used + 7,
                                         0.0f, 0.0f, 5.0f,
                                         capture_placement, &capture, &stats) == 0);
}

static void test_instance_failure_aborts(void) {
    float verts[] = {
        NAN, 0, 0, 0, 0,
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
    };
    uint16_t idx[] = {0, 1, 2};
    N2Mesh mesh;
    WInstProto proto;
    WInstLibrary library;
    N2Scene scene, vista;
    WInstStats stats;
    WInstBuildVisit build;
    WInstPlacement placement;
    memset(&mesh, 0, sizeof mesh);
    memset(&proto, 0, sizeof proto);
    memset(&library, 0, sizeof library);
    memset(&scene, 0, sizeof scene);
    memset(&vista, 0, sizeof vista);
    memset(&stats, 0, sizeof stats);
    memset(&placement, 0, sizeof placement);
    mesh.verts = verts;
    mesh.nverts = 3;
    mesh.idx = idx;
    mesh.nidx = 3;
    mesh.cat = N2_OTHER;
    proto.scene.meshes = &mesh;
    proto.scene.count = 1;
    proto.name_hash = winst_name_key("XO_FAIL", proto.name);
    library.items = &proto;
    library.count = 1;
    for (int i = 0; i < 16; i++) placement.matrix[i] = i % 5 == 0 ? 1.0f : 0.0f;
    build.library = &library;
    build.scene = &scene;
    build.vista = &vista;
    build.stats = &stats;

    assert(winst_build_visit(&placement, "XO_FAIL", &build) == 0);
    assert(scene.count == 0 && vista.count == 0);
    assert(stats.rejected_meshes == 1);
}

static void test_ground_stage_eligibility_and_failure(void) {
    float verts[] = {
        0, 0, 0, 0, 0,
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
    };
    uint16_t idx[] = {0, 1, 2};
    N2Mesh mesh[2];
    WInstProto proto[2];
    WInstLibrary library;
    N2Scene scene, vista;
    WInstStats stats;
    memset(mesh, 0, sizeof mesh);
    memset(proto, 0, sizeof proto);
    memset(&library, 0, sizeof library);
    mesh[0].verts = verts;
    mesh[0].nverts = 3;
    mesh[0].idx = idx;
    mesh[0].nidx = 3;
    mesh[0].cat = N2_ROAD;
    proto[0].scene.meshes = mesh;
    proto[0].scene.count = 1;
    proto[0].has_matrix = 1;
    proto[0].matrix[0] = proto[0].matrix[5] = proto[0].matrix[10] =
        proto[0].matrix[15] = 1.0f;
    proto[0].matrix[12] = 7.0f;
    winst_name_key("TRN_GROUND", proto[0].name);
    library.items = proto;
    library.count = 1;

    memset(&scene, 0, sizeof scene);
    memset(&vista, 0, sizeof vista);
    memset(&stats, 0, sizeof stats);
    assert(winst_place_ground_prototypes(&library, &scene, &vista, &stats) == 1);
    assert(scene.count == 1 && near(scene.meshes[0].verts[0], 7.0f));
    assert(stats.own_matrix_meshes == 1);
    free_scene(&scene);

    proto[1] = proto[0];
    library.count = 2;
    memset(&scene, 0, sizeof scene);
    memset(&vista, 0, sizeof vista);
    memset(&stats, 0, sizeof stats);
    assert(winst_place_ground_prototypes(&library, &scene, &vista, &stats) == 1);
    assert(scene.count == 2);
    assert(near(scene.meshes[0].verts[0], 0.0f));
    assert(near(scene.meshes[1].verts[0], 0.0f));
    assert(stats.own_matrix_meshes == 0);
    free_scene(&scene);

    library.count = 1;
    proto[0].scene.count = 2;
    mesh[1] = mesh[0];
    mesh[1].cat = N2_OTHER;
    memset(&scene, 0, sizeof scene);
    memset(&vista, 0, sizeof vista);
    memset(&stats, 0, sizeof stats);
    assert(winst_place_ground_prototypes(&library, &scene, &vista, &stats) == 1);
    assert(scene.count == 1 && near(scene.meshes[0].verts[0], 0.0f));
    assert(stats.own_matrix_meshes == 0);
    free_scene(&scene);

    proto[0].scene.count = 1;
    verts[0] = NAN;
    memset(&scene, 0, sizeof scene);
    memset(&vista, 0, sizeof vista);
    memset(&stats, 0, sizeof stats);
    assert(winst_place_ground_prototypes(&library, &scene, &vista, &stats) == 0);
    assert(scene.count == 0 && vista.count == 0);
    assert(stats.rejected_meshes == 1);
    verts[0] = 0.0f;
}

static int write_fixture_file(const char *path,
                              const unsigned char *data, long len) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    size_t written = fwrite(data, 1, (size_t)len, file);
    int closed = fclose(file);
    return written == (size_t)len && closed == 0;
}

static void test_builder_home_atomicity_and_bundle_isolation(void) {
    const char *root = "build/world_instance_fixture";
    assert(mkdir(root, 0777) == 0 || errno == EEXIST);
    unsigned char companion[256], stream_a[512], stream_b[512], stream_c[2048];
    unsigned char placement[64];
    long companion_len = make_regions(companion, sizeof companion);
    make_instance_record(placement, 0, -1.0f, -1.0f, 1.0f, 1.0f);
    long stream_a_len = add_section(stream_a, 0, 23, "XO_WRONG", placement, 1);
    long stream_b_len = add_section(stream_b, 0, 17, "XO_HOME", placement, 1);
    unsigned char model[512], header[128], verts[72], idx[6];
    memset(model, 0, sizeof model);
    memset(header, 0, sizeof header);
    memset(verts, 0, sizeof verts);
    memset(idx, 0, sizeof idx);
    memcpy(header, "XO_FAIL", 8);
    for (int i = 0; i < 16; i++) put_f32(header + 0x40 + i * 4,
                                          i % 5 == 0 ? 1.0f : 0.0f);
    put_f32(verts + 24, 1.0f);
    put_f32(verts + 48 + 4, 1.0f);
    put_u16(idx + 0, 0);
    put_u16(idx + 2, 1);
    put_u16(idx + 4, 2);
    long model_len = 0;
    model_len = add_leaf(model, model_len, 0x00134011ul, header, sizeof header);
    model_len = add_leaf(model, model_len, 0x00134b01ul, verts, sizeof verts);
    model_len = add_leaf(model, model_len, 0x00134b03ul, idx, sizeof idx);
    put_u32(stream_c, 0x80134010ul);
    put_u32(stream_c + 4, (unsigned long)model_len);
    memcpy(stream_c + 8, model, (size_t)model_len);
    make_instance_record(placement, 0, -1.0f, -1.0f, 1.0f, 1.0f);
    put_f32(placement + 0x20, NAN);
    long stream_c_len = add_section(stream_c, 8 + model_len, 17,
                                    "XO_FAIL", placement, 1);
    assert(write_fixture_file("build/world_instance_fixture/L4RA.BUN",
                              companion, companion_len));
    assert(write_fixture_file("build/world_instance_fixture/L4RB.BUN",
                              companion, companion_len));
    assert(write_fixture_file("build/world_instance_fixture/L4RC.BUN",
                              companion, companion_len));
    assert(write_fixture_file("build/world_instance_fixture/STREAML4RA.BUN",
                              stream_a, stream_a_len));
    assert(write_fixture_file("build/world_instance_fixture/STREAML4RB.BUN",
                              stream_b, stream_b_len));
    assert(write_fixture_file("build/world_instance_fixture/STREAML4RC.BUN",
                              stream_c, stream_c_len));

    N2Mesh scene_sentinel, vista_sentinel;
    memset(&scene_sentinel, 0, sizeof scene_sentinel);
    memset(&vista_sentinel, 0, sizeof vista_sentinel);
    scene_sentinel.cat = 77;
    vista_sentinel.cat = 88;
    N2Scene scene = { &scene_sentinel, 1, 1 };
    N2Scene vista = { &vista_sentinel, 1, 1 };
    WInstStats stats;
    const char *only_wrong[] = { "STREAML4RA" };
    memset(&stats, 0, sizeof stats);
    assert(world_instance_build(&scene, &vista, root, only_wrong, 1,
                                0.0f, 0.0f, 5.0f,
                                NULL, 0, &stats) == 0);
    assert(scene.meshes == &scene_sentinel && scene.count == 1 && scene.cap == 1);
    assert(vista.meshes == &vista_sentinel && vista.count == 1 && vista.cap == 1);
    assert(scene.meshes[0].cat == 77 && vista.meshes[0].cat == 88);
    assert(stats.home_region == 17 && stats.bundle[0] == 0);

    const char *forced_failure[] = { "STREAML4RC" };
    memset(&stats, 0, sizeof stats);
    assert(world_instance_build(&scene, &vista, root, forced_failure, 1,
                                0.0f, 0.0f, 5.0f,
                                NULL, 0, &stats) == 0);
    assert(scene.meshes == &scene_sentinel && scene.count == 1 && scene.cap == 1);
    assert(vista.meshes == &vista_sentinel && vista.count == 1 && vista.cap == 1);
    assert(scene.meshes[0].cat == 77 && vista.meshes[0].cat == 88);
    assert(stats.rejected_meshes == 1);

    memset(&scene, 0, sizeof scene);
    memset(&vista, 0, sizeof vista);
    memset(&stats, 0, sizeof stats);
    const char *requested[] = { "STREAML4RA", "STREAML4RB" };
    assert(world_instance_build(&scene, &vista, root, requested, 2,
                                0.0f, 0.0f, 5.0f,
                                NULL, 0, &stats) == 1);
    assert(strcmp(stats.bundle, "STREAML4RB") == 0);
    assert(stats.home_region == 17);
    assert(scene.count == 0 && vista.count == 0);

    remove("build/world_instance_fixture/L4RA.BUN");
    remove("build/world_instance_fixture/L4RB.BUN");
    remove("build/world_instance_fixture/L4RC.BUN");
    remove("build/world_instance_fixture/STREAML4RA.BUN");
    remove("build/world_instance_fixture/STREAML4RB.BUN");
    remove("build/world_instance_fixture/STREAML4RC.BUN");
    rmdir(root);
}

static void test_private_prototype_paths(void) {
    WInstLibrary library;
    WInstProto proto[2];
    N2Mesh ranges[2];
    WInstStats st;
    char folded[28];
    memset(&library, 0, sizeof library);
    memset(proto, 0, sizeof proto);
    memset(ranges, 0, sizeof ranges);
    memset(&st, 0, sizeof st);
    library.items = proto;
    library.count = 1;
    proto[0].name_hash = winst_name_key("XO_TARGET", folded);
    snprintf(proto[0].name, sizeof proto[0].name, "XO_OTHER");
    assert(winst_library_find(&library, "xo_target", &st) == NULL);
    assert(st.missing_models == 1);

    proto[0].name_hash = winst_name_key("xo_target", proto[0].name);
    proto[0].has_matrix = 1;
    proto[0].scene.meshes = ranges;
    proto[0].scene.count = 2;
    ranges[0].cat = N2_ROAD;
    ranges[1].cat = N2_ROAD;
    assert(winst_library_find(&library, "XO_TARGET", &st) == &proto[0]);
    assert(st.missing_models == 1);
    assert(winst_proto_has_own_matrix(&library, &proto[0]) == 1);

    proto[1] = proto[0];
    library.count = 2;
    assert(winst_proto_has_own_matrix(&library, &proto[0]) == 0);

    unsigned char model[512], header[128], slot[8], sub[60], verts[72], idx[6];
    memset(model, 0, sizeof model);
    memset(header, 0, sizeof header);
    memset(slot, 0, sizeof slot);
    memset(sub, 0, sizeof sub);
    memset(verts, 0, sizeof verts);
    memset(idx, 0, sizeof idx);
    memcpy(header, "TRN_ROAD_TEST", 13);
    for (int i = 0; i < 16; i++) put_f32(header + 0x40 + i * 4,
                                          i % 5 == 0 ? 1.0f : 0.0f);
    put_u32(slot, 0x1234);
    put_u32(sub + 12, 2); /* malformed: a material range cannot be a triangle */
    put_u32(sub + 28, 0);
    put_u32(sub + 52, 0);
    put_f32(verts + 24, 1.0f);
    put_f32(verts + 48 + 4, 1.0f);
    put_u16(idx + 0, 0);
    put_u16(idx + 2, 1);
    put_u16(idx + 4, 2);
    long used = 0;
    used = add_leaf(model, used, 0x00134011ul, header, sizeof header);
    used = add_leaf(model, used, 0x00134012ul, slot, sizeof slot);
    used = add_leaf(model, used, 0x00134b02ul, sub, sizeof sub);
    used = add_leaf(model, used, 0x00134b01ul, verts, sizeof verts);
    used = add_leaf(model, used, 0x00134b03ul, idx, sizeof idx);
    memset(&library, 0, sizeof library);
    winst_collect_model(&library, model, 0, used, NULL, 0);
    assert(library.count == 1 && library.items[0].scene.count == 1);
    assert(library.items[0].scene.meshes[0].nidx == 3);
    assert(library.items[0].scene.meshes[0].texkey == 0x1234u);
    winst_library_free(&library);
}

static void test_world2_capture_policy(void) {
    WorldCapturePolicy capture = world_capture_policy(1, 1, 0, 1);
    assert(capture.preserve_explicit_pose);
    assert(capture.fixed_camera);
    assert(capture.freeze_motion);

    WorldCapturePolicy interactive = world_capture_policy(1, 0, 0, 1);
    assert(interactive.preserve_explicit_pose);
    assert(!interactive.fixed_camera);
    assert(!interactive.freeze_motion);

    WorldCapturePolicy no_heading = world_capture_policy(1, 1, 0, 0);
    assert(no_heading.preserve_explicit_pose);
    assert(!no_heading.fixed_camera);
    assert(!no_heading.freeze_motion);

    WorldCapturePolicy legacy = world_capture_policy(0, 1, 0, 1);
    assert(!legacy.preserve_explicit_pose);
    assert(!legacy.fixed_camera);
    assert(!legacy.freeze_motion);

    WorldCapturePolicy legacy_static = world_capture_policy(0, 1, 1, 1);
    assert(!legacy_static.preserve_explicit_pose);
    assert(!legacy_static.fixed_camera);
    assert(!legacy_static.freeze_motion);
}

int main(void) {
    test_regions();
    test_placement();
    test_direct_placement();
    test_private_prototype_paths();
    test_section_collection();
    test_instance_failure_aborts();
    test_ground_stage_eligibility_and_failure();
    test_builder_home_atomicity_and_bundle_isolation();
    test_world2_capture_policy();
    puts("world_instance_test: PASS");
    return 0;
}
