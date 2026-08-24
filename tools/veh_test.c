/* veh_test -- acceptance gate for vehicle_model against the garage dyno.
 *
 *   cc -std=c99 -Isrc -o veh_test tools/veh_test.c src/vehicle_model.c \
 *      src/car_setup.c -lm
 *   ./veh_test <GLOBALB.BUN>
 *
 * Targets are what the game's own Dyno Results screen reports for the
 * starter Peugeot 106: 0-60 mph in 8.23 s, 0-100 mph in 23.66 s, top speed
 * 141.0 MPH. Exits non-zero if any is off by more than 10%.
 */
#include "vehicle_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DT   (1.0f / 240.0f)     /* fixed step; 240 Hz keeps the tyre stiff */
#define MPH  0.44704f

static unsigned char *slurp(const char *p, long *len) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)*len);
    if (d && fread(d, 1, (size_t)*len, f) != (size_t)*len) { free(d); d = NULL; }
    fclose(f); return d;
}

/* Full-throttle run in a straight line. Reports the times to reach each
 * speed and the speed it settles at. */
static void accel_run(const VehModel *m, float *t60, float *t100, float *top)
{
    VehState s; veh_state_init(&s);
    VehInput in = { 1.0f, 0.0f, 0.0f, 0 };
    *t60 = *t100 = -1.0f; *top = 0.0f;
    float t = 0.0f, last = 0.0f;
    for (long i = 0; i < 240L * 200; i++) {      /* 200 s is plenty */
        veh_step(&s, m, &in, DT);
        t += DT;
        float mph = s.v_long / MPH;
        if (*t60 < 0.0f && mph >= 60.0f) *t60 = t;
        if (*t100 < 0.0f && mph >= 100.0f) *t100 = t;
        if (mph > *top) *top = mph;
        /* settled? */
        if (t > 30.0f && fabsf(mph - last) < 0.0005f) break;
        last = mph;
    }
}

/* Handbrake turn from 50 km/h: does the rear break away, how far does the
 * body slip angle go, and does the car straighten up again afterwards. */
static void drift_run(const VehModel *m, float *peak_slip, float *end_slip,
                      float *peak_yaw)
{
    VehState s; veh_state_init(&s);
    s.v_long = 50.0f / 3.6f; s.gear = 3;
    *peak_slip = *end_slip = *peak_yaw = 0.0f;
    for (long i = 0; i < 240L * 8; i++) {
        float t = (float)i * DT;
        VehInput in = { 0.5f, 0.0f, 0.8f, 0 };
        if (t > 0.5f && t < 1.3f) { in.handbrake = 1; in.throttle = 0.0f; }
        if (t > 1.3f) { in.steer = -0.35f; in.throttle = 0.6f; }  /* opposite lock */
        if (t > 4.0f) { in.steer = 0.0f; in.throttle = 0.3f; }
        veh_step(&s, m, &in, DT);
        float sl = fabsf(veh_slip_angle_deg(&s));
        float yw = fabsf(s.yaw_rate) * 180.0f / 3.14159265f;
        if (t > 0.5f && sl > *peak_slip) *peak_slip = sl;
        if (yw > *peak_yaw) *peak_yaw = yw;
        if (!isfinite(s.v_long) || !isfinite(s.yaw_rate)) {
            printf("  NUMERICS BLEW UP at t=%.2f\n", t); *peak_slip = -1.0f; return;
        }
    }
    *end_slip = fabsf(veh_slip_angle_deg(&s));
}


/* Ordinary driving: get up to speed, feed in steering, and see whether the
 * car turns or simply departs. A stable car settles at a steady yaw rate
 * with a small slip angle; an undriveable one spins the moment the input
 * arrives. */
static void handling_run(const VehModel *m, float kmh, float steer,
                         float *radius, float *slip, float *yaw, int *spun)
{
    VehState s; veh_state_init(&s);
    s.v_long = kmh / 3.6f; s.gear = 3;
    VehInput in = { 0.35f, 0.0f, 0.0f, 0 };
    *spun = 0;
    /* let it settle at speed first */
    for (int i = 0; i < 240; i++) veh_step(&s, m, &in, DT);
    in.steer = steer;
    float ysum = 0.0f, ssum = 0.0f, rsum = 0.0f; int n = 0;
    for (long i = 0; i < 240L * 4; i++) {
        veh_step(&s, m, &in, DT);
        float sl = fabsf(veh_slip_angle_deg(&s));
        if (sl > 60.0f) *spun = 1;
        if (i > 240) {                      /* average over the settled part */
            float yr = fabsf(s.yaw_rate);
            ysum += yr * 180.0f / 3.14159265f;
            ssum += sl;
            rsum += yr > 0.01f ? fabsf(s.v_long) / yr : 999.0f;
            n++;
        }
    }
    *yaw = ysum / n; *slip = ssum / n; *radius = rsum / n;
}

/* Straight-line stability: no steering at all, does it hold its heading. */
static float straight_drift(const VehModel *m)
{
    VehState s; veh_state_init(&s);
    VehInput in = { 1.0f, 0.0f, 0.0f, 0 };
    for (long i = 0; i < 240L * 15; i++) veh_step(&s, m, &in, DT);
    return s.heading * 180.0f / 3.14159265f;
}


/* THE PATH THE GAME ACTUALLY TAKES. Everything above calls veh_step directly
 * at a fine step; the game calls veh_bridge_step once per 60 Hz frame, in the
 * game's units, and the bridge substeps internally. Driveability has to be
 * judged here or it is not being judged at all. */
static void bridge_run(const VehModel *m, float kmh, float steer,
                       float *radius, float *slip, float *yaw)
{
    float pos[3] = {0,0,0}, vel[2], heading = 0.0f, speed;
    speed = kmh / 3.6f / 60.0f;                 /* m/tick */
    vel[0] = speed; vel[1] = 0.0f;
    veh_bridge_reset();
    for (int i = 0; i < 60; i++)
        veh_bridge_step(pos, vel, &heading, &speed, 0.35f, 0.0f, 0, m);
    float ysum = 0, ssum = 0, rsum = 0; int n = 0;
    for (int i = 0; i < 60 * 4; i++) {
        veh_bridge_step(pos, vel, &heading, &speed, 0.35f, steer, 0, m);
        if (i > 60) {
            float v = fabsf(speed) * 60.0f;      /* back to m/s */
            float yr = fabsf(veh_bridge_yaw_rate());
            ysum += yr * 180.0f / 3.14159265f;
            ssum += fabsf(veh_bridge_slip_deg());
            rsum += yr > 0.01f ? v / yr : 999.0f;
            n++;
        }
    }
    *yaw = ysum / n; *slip = ssum / n; *radius = rsum / n;
}


/* Doughnut: from walking pace, full lock and full throttle, held. A car that
 * can do one settles into a steady rotation on a small radius and stays
 * there; a car that cannot either straightens out or spins to a stop. */
static void donut_run(const VehModel *m, float *yaw, float *radius,
                      float *slip, float *held)
{
    VehState s; veh_state_init(&s);
    s.v_long = 12.0f / 3.6f; s.gear = 2;
    VehInput in = { 1.0f, 0.0f, 1.0f, 0 };
    float ysum = 0, rsum = 0, ssum = 0; int n = 0, spinning = 0;
    for (long i = 0; i < 240L * 10; i++) {
        veh_step(&s, m, &in, DT);
        float yr = fabsf(s.yaw_rate);
        if (i > 240L * 3) {                    /* after it has settled */
            ysum += yr * 180.0f / 3.14159265f;
            rsum += yr > 0.05f ? fabsf(s.v_long) / yr : 99.0f;
            ssum += fabsf(veh_slip_angle_deg(&s));
            if (yr > 0.5f) spinning++;         /* still going round */
            n++;
        }
    }
    *yaw = ysum / n; *radius = rsum / n; *slip = ssum / n;
    *held = 100.0f * (float)spinning / (float)n;
}


/* The doughnut as the GAME runs it: through the bridge, in the game's units,
 * one call per 60 Hz frame. */
static void bridge_donut(const VehModel *m, float *yaw, float *radius,
                         float *slip, float *held, float *spread)
{
    float pos[3] = {0,0,0}, vel[2], heading = 0.0f, speed;
    speed = 0.0f;                     /* FROM A STANDSTILL, as a player would */
    vel[0] = 0.0f; vel[1] = 0.0f;
    veh_bridge_reset();
    float ysum = 0, rsum = 0, ssum = 0; int n = 0, spinning = 0;
    for (int i = 0; i < 60 * 10; i++) {
        veh_bridge_step(pos, vel, &heading, &speed, 1.0f, 1.0f, 0, m);
        float yr = fabsf(veh_bridge_yaw_rate());
        if (i > 60 * 3) {
            float v = fabsf(speed) * 60.0f;
            ysum += yr * 180.0f / 3.14159265f;
            rsum += yr > 0.05f ? v / yr : 99.0f;
            ssum += fabsf(veh_bridge_slip_deg());
            if (yr > 0.5f) spinning++;
            n++;
        }
    }
    *yaw = ysum/n; *radius = rsum/n; *slip = ssum/n;
    *held = 100.0f * (float)spinning / (float)n;
    /* How ROUND is it? A doughnut worth the name holds one radius; a car
     * wandering between radii draws petals, not a circle. */
    {   float m2 = 0.0f; int k = 0;
        float pos2[3] = {0,0,0}, vel2[2], head2 = 0.0f, sp2 = 0.0f;
        vel2[0] = 0; vel2[1] = 0;
        veh_bridge_reset();
        for (int i = 0; i < 60*10; i++) {
            veh_bridge_step(pos2, vel2, &head2, &sp2, 1.0f, 1.0f, 0, m);
            if (i > 60*3) {
                float v = fabsf(sp2)*60.0f, yr = fabsf(veh_bridge_yaw_rate());
                float R = yr > 0.05f ? v/yr : 99.0f;
                m2 += (R - *radius)*(R - *radius); k++;
            }
        }
        *spread = k ? sqrtf(m2/k) : 0.0f;
    }
}

static int off(float got, float want) { return fabsf(got - want) > want * 0.10f; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: veh_test <GLOBALB.BUN>\n"); return 2; }
    long len = 0; unsigned char *d = slurp(argv[1], &len);
    if (!d) { perror(argv[1]); return 2; }

    const char *cars[] = { "PEUGOT106", "350Z", "IMPREZAWRX" };
    int fail = 0;
    for (int k = 0; k < 3; k++) {
        N2CarSetup c;
        if (!n2_car_setup_load(&c, d, len, cars[k])) {
            printf("%s: not found\n", cars[k]); fail = 1; continue;
        }
        VehModel m; veh_model_init(&m, &c);
        float t60, t100, top, ps, es, py;
        accel_run(&m, &t60, &t100, &top);
        drift_run(&m, &ps, &es, &py);

        printf("\n%s  (%.0f kg, %s, CdA %.2f m^2)\n", cars[k], m.mass_kg,
               c.drive.split_rear > 0.9f ? "RWD" :
               c.drive.split_rear < 0.1f ? "FWD" : "AWD", m.cda);
        printf("  Magic Formula: front B %.2f C %.3f D %.3f | rear B %.2f C %.3f D %.3f\n",
               m.B[0], m.C[0], m.D_mu[0], m.B[1], m.C[1], m.D_mu[1]);
        printf("  0-60 %6.2f s   0-100 %7.2f s   top %6.1f mph\n",
               t60, t100, top);
        printf("  handbrake turn: peak slip %.1f deg, peak yaw %.0f deg/s, "
               "settles at %.1f deg %s\n", ps, py, es,
               ps > 15.0f ? (es < 5.0f ? "-- slides and recovers" : "-- STILL SIDEWAYS")
                          : "-- NO SLIDE");

        {   float r, sl, yw; int spun;
            printf("  drives:");
            float sp[3] = { 40.0f, 80.0f, 120.0f };
            for (int q = 0; q < 3; q++) {
                handling_run(&m, sp[q], 0.5f, &r, &sl, &yw, &spun);
                printf("  %.0fkm/h R%.0fm slip%.0f%s", sp[q], r, sl,
                       spun ? " SPUN" : "");
                if (spun) fail = 1;
            }
            printf("\n  through the bridge at 60 Hz (what the game runs):");
            for (int q = 0; q < 3; q++) {
                bridge_run(&m, sp[q], 0.5f, &r, &sl, &yw);
                printf("  %.0fkm/h R%.0fm slip%.0f", sp[q], r, sl);
            }
            printf("\n  full lock at 80: ");
            handling_run(&m, 80.0f, 1.0f, &r, &sl, &yw, &spun);
            printf("R%.0fm slip %.0f deg yaw %.0f deg/s%s | straight drift %.1f deg\n",
                   r, sl, yw, spun ? "  SPUN" : "", straight_drift(&m));
        }

        {   float dy, dr, ds, dh;
            donut_run(&m, &dy, &dr, &ds, &dh);
            float by, br, bs, bh;
            float bsp;
            bridge_donut(&m, &by, &br, &bs, &bh, &bsp);
            printf("  doughnut THROUGH BRIDGE: %.0f deg/s, R%.1f m (+-%.1f), slip %.0f, held %.0f%%%s\n",
                   by, br, bs, bh, bsp, bsp > br*0.35f ? "  NOT ROUND" : "  round");
            printf("  doughnut: %.0f deg/s on a %.1f m radius, slip %.0f deg, "
                   "sustained %.0f%% of the time %s\n", dy, dr, ds, dh,
                   (dh > 60.0f && dr < 15.0f) ? "-- holds it" : "-- rear drive only");
        }

        if (k == 0) {   /* only the Peugeot has published targets */
            printf("  vs garage: 8.23 / 23.66 / 141.0\n");
            if (off(t60, 8.23f))   { printf("  FAIL 0-60\n");  fail = 1; }
            if (off(t100, 23.66f)) { printf("  FAIL 0-100\n"); fail = 1; }
            if (off(top, 141.0f))  { printf("  FAIL top\n");   fail = 1; }
            if (ps < 15.0f)        { printf("  FAIL no drift\n"); fail = 1; }
        }
    }
    free(d);
    printf("\n%s\n", fail ? "FAIL" : "all targets within 10%");
    return fail;
}
