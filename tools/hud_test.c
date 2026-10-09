/* hud_test — the HUD's pure maths, with no GL context.
 *
 * These are the parts that are wrong SILENTLY: an arrow that unwinds the long
 * way round a seam still animates, a map transform that is not its own inverse
 * still draws dots, and a layout that scales with width still fills the screen
 * at 16:9. Each case below fails under the specific mistake it guards.
 */
#include "../src/hud.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846f
static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* ---- 1. arrow angle: the wrap is the whole point ---- */
static void test_angle_delta_takes_the_short_way(void) {
    assert(near(hud_angle_delta(0.0f, 0.5f), 0.5f, 1e-5f));
    assert(near(hud_angle_delta(0.5f, 0.0f), -0.5f, 1e-5f));

    /* Crossing the seam: heading just under +pi, target just over -pi. The
       true turn is a hair to the LEFT (+), not almost a full turn right. */
    float from = PI - 0.05f, to = -PI + 0.05f;
    float d = hud_angle_delta(from, to);
    assert(d > 0.0f && d < 0.2f);
    /* the naive to-from would report about -6.18 rad here */
    assert(fabsf(to - from) > 6.0f);

    /* and the mirror case turns the other way */
    assert(hud_angle_delta(-PI + 0.05f, PI - 0.05f) < 0.0f);

    /* every result is inside (-pi, +pi], including many wraps out */
    for (int i = -40; i <= 40; i++) {
        float a = (float)i * 0.7f;
        float w = hud_angle_delta(0.0f, a);
        assert(w > -PI - 1e-4f && w <= PI + 1e-4f);
        /* and it is the same angle modulo a full turn */
        float k = (a - w) / (2.0f * PI);
        assert(near(k, floorf(k + 0.5f), 1e-3f));
    }
    /* exactly opposite resolves to +pi, never -pi, so the arrow is stable */
    assert(near(hud_angle_delta(0.0f, PI), PI, 1e-4f));
    printf("  arrow angle wraps to the short way round                PASS\n");
}

/* ---- 2. map transform: projection and picking must be inverses ---- */
static void test_map_project_and_pick_round_trip(void) {
    HudState s; memset(&s, 0, sizeof s);
    s.car_x = -863.0f; s.car_y = -290.0f; s.car_heading = 0.7f;
    const int W = 1920, H = 1080;

    /* a point 100 m north-east of the car comes back unchanged */
    const float tx = s.car_x + 70.0f, ty = s.car_y + 70.0f;
    float mx, my;
    hud_map_project(tx, ty, s.car_x, s.car_y, 260.0f,
                    PI * 0.5f - s.car_heading, &mx, &my);
    assert(mx * mx + my * my < 1.0f);          /* inside the window */

    /* Drive it through the SAME layout the draw uses by picking the NDC the
       projection lands on. hud_map_pick owns the panel geometry, so this also
       pins the draw and the pick to one definition. */
    float back_x = 0, back_y = 0;
    /* recover the panel centre by picking the exact centre of the panel: the
       car's own position must map to the panel centre, whatever the layout */
    float cx_ndc = 0, cy_ndc = 0;
    {   /* search the NDC that returns the car position (panel centre) */
        int found = 0;
        for (int iy = -200; iy <= 200 && !found; iy++)
            for (int ix = -200; ix <= 200; ix++) {
                float nx = (float)ix / 200.0f, ny = (float)iy / 200.0f;
                float wx, wy;
                if (!hud_map_pick(nx, ny, W, H, &s, &wx, &wy)) continue;
                if (near(wx, s.car_x, 1.5f) && near(wy, s.car_y, 1.5f)) {
                    cx_ndc = nx; cy_ndc = ny; found = 1; break;
                }
            }
        assert(found);
    }
    /* now project a world point, place it in NDC the way hud_draw does, and
       pick it back */
    float asp = (float)W / (float)H, MAP_R = 0.30f;
    float px = cx_ndc + mx * MAP_R / asp, py = cy_ndc + my * MAP_R;
    assert(hud_map_pick(px, py, W, H, &s, &back_x, &back_y));
    assert(near(back_x, tx, 2.0f));
    assert(near(back_y, ty, 2.0f));

    /* outside the round panel the pick must refuse rather than invent a spot */
    float junk_x = 12345.0f, junk_y = 54321.0f;
    assert(!hud_map_pick(0.0f, 0.0f, W, H, &s, &junk_x, &junk_y));
    assert(junk_x == 12345.0f && junk_y == 54321.0f);   /* outputs untouched */

    /* heading-up: the world point straight AHEAD of the car must land on the
       +Y (up) axis of the panel, at any heading. This is what fails if the
       quarter-turn in the rotation is dropped. */
    for (int i = 0; i < 12; i++) {
        float hdg = (float)i * 0.5f;
        float ax = s.car_x + cosf(hdg) * 80.0f, ay = s.car_y + sinf(hdg) * 80.0f;
        float ox, oy;
        hud_map_project(ax, ay, s.car_x, s.car_y, 260.0f, PI * 0.5f - hdg, &ox, &oy);
        assert(near(ox, 0.0f, 1e-4f));
        assert(oy > 0.0f);
    }
    printf("  map projection and picking are exact inverses          PASS\n");
}

/* ---- 3. layout: a unit must be the same size at 16:9 and at 4:3 ---- */
static void test_layout_keeps_proportions_across_aspects(void) {
    const float wide = 1920.0f / 1080.0f, classic = 1024.0f / 768.0f;

    /* A margin quoted in units must come out the same DISTANCE from the edge
       in pixels-of-height at both aspects... */
    float wx, wy, cxx, cyy;
    hud_place(wide,    -1.0f, -1.0f, 0.38f, 0.38f, &wx, &wy);
    hud_place(classic, -1.0f, -1.0f, 0.38f, 0.38f, &cxx, &cyy);
    assert(near(wy, cyy, 1e-6f));                 /* vertical is identical */
    /* ...and horizontally it must shrink in NDC as the screen widens, which is
       exactly what keeps it a fixed number of PIXELS from the edge. */
    assert((wx + 1.0f) < (cxx + 1.0f));
    assert(near((wx + 1.0f) * wide, (cxx + 1.0f) * classic, 1e-5f));

    /* anchors stay on their own edges */
    float rx, ry;
    hud_place(wide, 1.0f, 1.0f, -0.34f, -0.16f, &rx, &ry);
    assert(rx < 1.0f && rx > 0.0f && ry < 1.0f && ry > 0.0f);

    /* top centre is EXACTLY centred at every aspect: the navigation arrow is
       specified to sit there, and x must not drift with the aspect */
    for (int i = 0; i < 4; i++) {
        const float asps[4] = { 16.0f/9.0f, 4.0f/3.0f, 21.0f/9.0f, 1.0f };
        float ax, ay;
        hud_place(asps[i], 0.0f, 1.0f, 0.0f, -0.20f, &ax, &ay);
        assert(ax == 0.0f);
        assert(near(ay, 0.80f, 1e-6f));
    }
    printf("  layout units hold their proportions across aspects     PASS\n");
}

/* ---- 4. a square stays square: the map window must not shear ---- */
static void test_map_window_is_isotropic(void) {
    HudState s; memset(&s, 0, sizeof s);
    s.car_heading = 0.0f;
    float ex, ey, nx, ny;
    hud_map_project(100.0f, 0.0f, 0.0f, 0.0f, 200.0f, 0.0f, &ex, &ey);
    hud_map_project(0.0f, 100.0f, 0.0f, 0.0f, 200.0f, 0.0f, &nx, &ny);
    /* equal world distances give equal panel distances in both axes */
    assert(near(sqrtf(ex*ex + ey*ey), sqrtf(nx*nx + ny*ny), 1e-6f));
    assert(near(sqrtf(ex*ex + ey*ey), 0.5f, 1e-6f));
    /* a degenerate radius must not divide by zero */
    float dx, dy;
    hud_map_project(1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &dx, &dy);
    assert(dx == dx && dy == dy);            /* not NaN */
    printf("  map window is isotropic and radius-safe                PASS\n");
}

/* ---- 5. sprint must never borrow a circuit's lap counter ---- */
static void test_sprint_reports_progress_not_fabricated_laps(void) {
    HudState s; memset(&s, 0, sizeof s);
    char buf[64];

    /* free roam: no status line at all */
    hud_status_text(&s, buf, sizeof buf);
    assert(buf[0] == 0);

    /* circuit: real laps */
    s.racing = 1; s.circuit = 1; s.lap = 2; s.laps = 3;
    hud_status_text(&s, buf, sizeof buf);
    assert(!strcmp(buf, "LAP 2/3"));

    /* sprint: progress, and NOTHING that reads as a lap count */
    s.circuit = 0; s.sprint_progress = 0.435f;
    /* lap/laps deliberately left populated: a sprint must ignore them */
    hud_status_text(&s, buf, sizeof buf);
    assert(!strstr(buf, "LAP"));
    assert(!strstr(buf, "/"));
    assert(!strcmp(buf, "44 PCT"));

    /* progress is clamped, so a checkpoint overshoot cannot print 160 PCT */
    s.sprint_progress = 1.8f;  hud_status_text(&s, buf, sizeof buf);
    assert(!strcmp(buf, "100 PCT"));
    s.sprint_progress = -0.3f; hud_status_text(&s, buf, sizeof buf);
    assert(!strcmp(buf, "0 PCT"));

    /* a circuit whose lap counter runs past the total still reads sanely */
    s.circuit = 1; s.lap = 9; s.laps = 2;
    hud_status_text(&s, buf, sizeof buf);
    assert(!strcmp(buf, "LAP 2/2"));
    /* and lap 0 (not yet over the line) shows as the first lap, not "0" */
    s.lap = 0; hud_status_text(&s, buf, sizeof buf);
    assert(!strcmp(buf, "LAP 1/2"));
    s.race_kind=N2_RACE_DRIFT;s.drift_score=1234;s.drift_chain=56;
    hud_status_text(&s,buf,sizeof buf);assert(!strcmp(buf,"SCORE 1234  +56"));
    s.race_kind=N2_RACE_DRAG;s.engine_heat=.5f;s.shift_ready=1;
    hud_status_text(&s,buf,sizeof buf);assert(!strcmp(buf,"HEAT 50 PCT SHIFT"));
    s.engine_failed=1;hud_status_text(&s,buf,sizeof buf);assert(!strcmp(buf,"ENGINE BLOWN"));
    printf("  sprints show progress, never fabricated laps           PASS\n");
}

/* ---- 5b. fitted parts are REMOVED, not greyed out ---- */
static void test_unfitted_parts_are_removed(void) {
    HudState s; memset(&s, 0, sizeof s);
    assert(!hud_shows_nos(&s));              /* nothing fitted -> no N2O bar */
    s.have_nos = 1;
    assert(hud_shows_nos(&s));
    /* an ACTIVE bottle on an UNFITTED car must still show nothing: activation
       is not fitment, and drawing the bar here would be the bug */
    s.have_nos = 0; s.nitro_active = 1; s.nitro_tank = 1.0f;
    assert(!hud_shows_nos(&s));
    /* fitment is independent of whether a tank model exists */
    s.have_nos = 1; s.have_nitro_tank = 0;
    assert(hud_shows_nos(&s));
    assert(!hud_shows_nos(NULL));
    printf("  unfitted parts are removed, not drawn inert          PASS\n");
}

/* ---- 6. money: hidden in a race, and only shown when it exists ---- */
static void test_money_visibility(void) {
    HudState s; memset(&s, 0, sizeof s);
    assert(!hud_shows_money(&s));            /* free roam, no economy -> no */
    s.have_money = 1;
    assert(hud_shows_money(&s));             /* free roam, economy    -> yes */
    s.racing = 1;
    assert(!hud_shows_money(&s));            /* racing hides it even so      */
    s.have_money = 0;
    assert(!hud_shows_money(&s));
    assert(!hud_shows_money(NULL));
    printf("  money hides in races and when no economy exists        PASS\n");
}

/* ---- 7. the art gate: flat UI art passes, decoded noise does not ---- */
static void test_art_gate_separates_ui_from_noise(void) {
    enum { W = 64, H = 64 };
    static unsigned char flat[W*H*3], noise[W*H*3];
    /* UI-like: a few flat blocks with hard edges */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned char v = (x < 20) ? 0 : (x < 42 ? 200 : 90);
            unsigned char *p = flat + ((long)y*W + x)*3;
            p[0] = v; p[1] = v; p[2] = (unsigned char)(v / 2);
        }
    /* mis-decode-like: no horizontal coherence at all */
    unsigned int r = 12345u;
    for (long i = 0; i < (long)W*H*3; i++) { r = r*1103515245u + 12345u; noise[i] = (unsigned char)(r >> 16); }

    float f_flat = hud_art_run_fraction(flat, W, H);
    float f_noise = hud_art_run_fraction(noise, W, H);
    assert(f_flat > HUD_ART_RUN_MIN);
    assert(f_noise < HUD_ART_RUN_MIN);
    /* the two must not merely straddle the threshold, they must be far apart,
       or the gate is a coin flip on the next sprite */
    assert(f_flat - f_noise > 0.5f);
    /* measured reference points from the shipped packs (see hud.h) */
    assert(HUD_ART_RUN_MIN < 0.719f);   /* below the worst real sprite */
    assert(HUD_ART_RUN_MIN > 0.192f);   /* above ARROW_MARKERA's noise */
    /* degenerate inputs must not divide by zero or reject everything */
    assert(hud_art_run_fraction(NULL, W, H) >= HUD_ART_RUN_MIN);
    assert(hud_art_run_fraction(flat, 1, 1) >= HUD_ART_RUN_MIN);
    printf("  art gate separates flat UI art from decoded noise      PASS\n");
}

int main(void) {
    printf("hud_test:\n");
    test_angle_delta_takes_the_short_way();
    test_map_project_and_pick_round_trip();
    test_layout_keeps_proportions_across_aspects();
    test_map_window_is_isotropic();
    test_sprint_reports_progress_not_fabricated_laps();
    test_unfitted_parts_are_removed();
    test_money_visibility();
    test_art_gate_separates_ui_from_noise();
    printf("hud_test: PASS\n");
    return 0;
}
