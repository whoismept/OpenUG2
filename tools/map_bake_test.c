/* map_bake_test.c -- GL-free checks of the city map bake. */
#include "map_bake.h"
#include <assert.h>

static int fails;
static void chk(const char *name, int ok) {
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    fails += !ok;
}

enum { S = 1024, W = 64 };

/* Retail fixture: dark land, with one bright line in the top half only. */
static void fixture(unsigned char *rgba) {
    for (int i = 0; i < W * W; i++) {
        int bright = i % W == 16 && i / W < W / 2;
        memset(rgba + i * 4, bright ? 220 : 40, 3);
        rgba[i * 4 + 3] = 0;
    }
}

static int pixel(const unsigned char *out, int x, int y) { return out[((long)y * S + x) * 3]; }

int main(void) {
    printf("map bake\n");
    chk("top level is 4096", map_bake_size_for_level(5) == 4096);
    chk("no saved setting keeps full detail", map_bake_size_for_level(0) == 4096);
    chk("level 4 is 2048", map_bake_size_for_level(4) == 2048);
    chk("level 3 is 1024", map_bake_size_for_level(3) == 1024);
    chk("levels 1 and 2 keep the retail map",
        map_bake_size_for_level(2) == 0 && map_bake_size_for_level(1) == 0);

    /* World -> texel uses the frame ha_map samples: +X east, +Y north. */
    float verts[] = { 10,90,0,0,0,  30,90,0,0,0,  30,70,0,0,0,  90,10,0,0,0 };
    uint16_t idx[] = { 0, 1, 2 };
    N2Mesh mesh; memset(&mesh, 0, sizeof mesh);
    mesh.verts = verts; mesh.nverts = 4; mesh.idx = idx; mesh.nidx = 3; mesh.cat = N2_ROAD;
    N2Scene scene = { &mesh, 1, 1 };
    double origin[3] = { 0, 100, 100 };
    unsigned char *road = (unsigned char *)calloc(S, S), *second = (unsigned char *)calloc(S, S);
    long tris = map_bake_raster(&scene, origin, S, road, second);
    chk("road triangle is counted", tris == 1);
    chk("north-west world point lands top-left", road[(long)150 * S + 250] == 255);
    chk("outside the triangle stays clear", road[(long)900 * S + 900] == 0);
    chk("roads do not touch the secondary layer", second[(long)150 * S + 250] == 0);
    memset(road, 0, (size_t)S * S);
    mesh.cat = N2_TERRAIN; mesh.texkey = 0x12345678u;
    chk("ordinary terrain is not drawn", map_bake_raster(&scene, origin, S, road, second) == 0);
    mesh.texkey = 0xeaef2b8du;
    chk("rail terrain reaches the secondary layer",
        map_bake_raster(&scene, origin, S, road, second) == 1 && second[(long)150 * S + 250] == 255);

    /* A vertical secondary strip through both halves, and a road beside it. */
    unsigned char rgba[W * W * 4], *out = (unsigned char *)malloc((size_t)S * S * 3);
    fixture(rgba);
    memset(road, 0, (size_t)S * S); memset(second, 0, (size_t)S * S);
    for (int y = 0; y < S; y++)
        for (int x = 256; x < 272; x++) second[(long)y * S + x] = 255;
    for (int y = 0; y < S; y++)
        for (int x = 700; x < 730; x++) road[(long)y * S + x] = 255;
    chk("compose succeeds", map_bake_compose(rgba, W, W, road, second, NULL, S, 0, out));
    chk("secondary surface shows where retail draws it", pixel(out, 264, 200) > 90);
    chk("secondary surface vanishes where retail does not", pixel(out, 264, 800) < 60);
    chk("road shows without any retail stroke", pixel(out, 715, 800) > 80);
    /* The edge must ramp over several texels: count those between land and road. */
    int land = pixel(out, 560, 800), top = pixel(out, 715, 800), ramp = 0;
    for (int x = 560; x < 715; x++) {
        int v = pixel(out, x, 800);
        ramp += v > land + (top - land) / 10 && v < top - (top - land) / 10;
    }
    printf("  land %d, road %d, %d texels between them\n", land, top, ramp);
    chk("road edge is a gradient, not a hard step", ramp >= 4);

    /* The row-sweep filters must match a direct evaluation. */
    {
        enum { P = 16 };
        unsigned char in[P * P], plane[P * P], want[P * P], scratch[P * P]; int column[P];
        MbWork work = { scratch, column };
        for (int i = 0; i < P * P; i++) in[i] = (unsigned char)(i * 37 % 251);
        for (int y = 0; y < P; y++)
            for (int x = 0; x < P; x++) {
                int any = 0;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int qx = x + dx, qy = y + dy;
                        if (qx >= 0 && qy >= 0 && qx < P && qy < P) any |= in[qy * P + qx] > 200;
                    }
                want[y * P + x] = any ? 255 : 0;
            }
        for (int i = 0; i < P * P; i++) plane[i] = in[i] > 200 ? 255 : 0;
        mb_dilate(plane, &work, P, 2);
        chk("row-sweep dilation matches direct evaluation", !memcmp(plane, want, sizeof want));
        memset(plane, 90, sizeof plane);
        mb_box(plane, &work, P, 3);
        int flat = 1;
        for (int i = 0; i < P * P; i++) flat &= plane[i] == 90;
        chk("box filter keeps a flat plane flat", flat);
    }

    /* Layers: everything the worker needs, for one frame and size. */
    {
        MbLayers layers;
        mesh.cat = N2_ROAD; mesh.texkey = 0;
        chk("layers build from a scene", map_bake_layers(&layers, &scene, NULL, 0, NULL, NULL, origin, S));
        chk("layers carry the road", layers.road[(long)150 * S + 250] == 255);
        chk("no nav graph means no drive plane", layers.drive == NULL);
        chk("layers match their own frame", map_bake_layers_match(&layers, origin, S));
        double other[3] = { 0, 100, 200 };
        chk("layers reject another frame", !map_bake_layers_match(&layers, other, S));
        chk("layers reject another size", !map_bake_layers_match(&layers, origin, S / 2));
        fixture(rgba);
        unsigned char *image = map_bake_from_layers(&layers, rgba, W, W, 0);
        chk("a map bakes from layers alone", image != NULL);
        chk("layers survive a bake for the next race", layers.road[(long)150 * S + 250] == 255);
        free(image);
        map_bake_layers_free(&layers);
        chk("freed layers match nothing", !map_bake_layers_match(&layers, origin, S));
        mesh.cat = N2_OTHER;
        chk("a frame with no road gives no layers",
            !map_bake_layers(&layers, &scene, NULL, 0, NULL, NULL, origin, S) && !layers.road);
    }

    /* Links bridge breaks only: an existing road keeps its exact shape. */
    {
        float nav[] = { 20, 50,  60, 50,  60, 900,  500, 500,  540, 500,  600, 500,  640, 500 };
        /* 0-1 break, 1-2 too long, 3-4 on road, 5-6 break through a covered middle */
        int start[] = { 0, 1, 3, 4, 5, 6, 7, 8 }, list[] = { 1,  0, 2,  1,  4,  3,  6,  5 };
        double frame[3] = { 0, 1000, 1000 };
        unsigned char *reach = (unsigned char *)calloc(S, S);
        for (int y = 480; y < 520; y++)
            for (int x = 490; x < 560; x++) reach[(long)y * S + x] = 255;
        memset(road, 0, (size_t)S * S);
        for (int y = 480; y < 520; y++)
            for (int x = 610; x < 640; x++) reach[(long)y * S + x] = 255;
        map_bake_bridge_nav(nav, 7, start, list, reach, frame, S, road);
        chk("a link across a break becomes a stroke", road[(long)973 * S + 41] == 255);
        chk("a link too long to be one road is skipped", road[(long)500 * S + 61] == 0);
        chk("a link on an existing road draws nothing", road[(long)511 * S + 532] == 0);
        chk("a break between covered ends is still found", road[(long)511 * S + 620] == 255);

        /* A seam fills; a wider island of similar area stays. */
        memset(road, 255, (size_t)S * S);
        for (int x = 100; x < 160; x++) road[(long)100 * S + x] = 0;
        for (int y = 200; y < 214; y++)
            for (int x = 200; x < 214; x++) road[(long)y * S + x] = 0;
        mb_fill_seams(road, S, 400, 4.f);
        chk("a thin seam inside a road is filled", road[(long)100 * S + 130] == 255);
        chk("a compact island keeps its shape", road[(long)207 * S + 207] == 0);
        free(reach);
    }

    /* One grey along a road, whatever the retail map does there. */
    {
        memset(road, 0, (size_t)S * S); memset(second, 0, (size_t)S * S);
        for (int y = 0; y < S; y++)
            for (int x = 300; x < 340; x++) road[(long)y * S + x] = 255;
        for (int x = 0; x < S; x++)
            for (int y = 300; y < 340; y++) road[(long)y * S + x] = 255;
        unsigned char bright[W * W * 4];
        fixture(bright);
        chk("second compose succeeds", map_bake_compose(bright, W, W, road, second, NULL, S, 0, out));
        chk("road grey ignores retail brightness", abs(pixel(out, 800, 320) - pixel(out, 250, 320)) <= 1 &&
            abs(pixel(out, 320, 200) - pixel(out, 320, 800)) <= 1);
    }

    /* The retail map settles both disagreements with the meshes. */
    {
        unsigned char *drive = (unsigned char *)calloc(S, S);
        memset(road, 0, (size_t)S * S); memset(second, 0, (size_t)S * S);
        unsigned char lines[W * W * 4];
        fixture(lines);                               /* retail line at x~264, top half */
        for (int y = 0; y < S; y++)
            for (int x = 600; x < 620; x++) { road[(long)y * S + x] = 255; drive[(long)y * S + x] = 255; }
        for (int x = 800; x < 820; x++)
            for (int y = 700; y < 760; y++) road[(long)y * S + x] = 255;   /* unlinked stub */
        chk("third compose succeeds", map_bake_compose(lines, W, W, road, second, drive, S, 0, out));
        chk("a retail road with no mesh is drawn", pixel(out, 264, 200) > 150);
        chk("that road does not extend past the retail line", pixel(out, 264, 800) < 60);
        chk("a stub with no link or retail line is removed", pixel(out, 810, 730) < 60);
        chk("a linked road stays", pixel(out, 610, 800) > 150);
        free(drive);
    }

    /* A race map keeps its bright route and dim roads, as two clean tones. */
    {
        memset(road, 0, (size_t)S * S); memset(second, 0, (size_t)S * S);
        unsigned char event[W * W * 4];
        for (int i = 0; i < W * W; i++) {
            int x = i % W, y = i / W;
            int on_road = (x >= 14 && x <= 18) || (y >= 14 && y <= 18);
            int route = x >= 14 && x <= 18;
            memset(event + i * 4, route ? 245 : on_road ? 95 : 40, 3);
            event[i * 4 + 3] = 0;
        }
        for (int y = 0; y < S; y++) for (int x = 240; x < 280; x++) road[(long)y * S + x] = 255;
        for (int x = 0; x < S; x++) for (int y = 240; y < 280; y++) road[(long)y * S + x] = 255;
        chk("race compose succeeds", map_bake_compose(event, W, W, road, second, NULL, S, 1, out));
        chk("the route stays bright", pixel(out, 260, 800) > 215);
        chk("other roads stay dim", abs(pixel(out, 800, 260) - MB_RACE_DIM) <= 8);
        chk("land keeps the race map's colour", pixel(out, 700, 700) < 70);

        /* Rail on a dim race road survives the gate that a bright city map needs. */
        memset(road, 0, (size_t)S * S); memset(second, 0, (size_t)S * S);
        for (int x = 0; x < S; x++) for (int y = 240; y < 280; y++) second[(long)y * S + x] = 255;
        chk("rail compose succeeds", map_bake_compose(event, W, W, road, second, NULL, S, 1, out));
        chk("a secondary surface on a dim road is kept", pixel(out, 800, 260) > 90);
    }

    free(road); free(second); free(out);
    printf(fails ? "map_bake_test: FAIL\n" : "map_bake_test: PASS\n");
    return fails != 0;
}
