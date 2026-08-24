/* car_dyno -- print every car's parameters, and check the parser against the
 * game's own Dyno screen.
 *
 *   cc -std=c99 -o car_dyno tools/car_dyno.c src/car_setup.c -Isrc
 *   ./car_dyno <path to GLOBALB.BUN>
 *
 * The garage prints peak torque and peak power for the selected car, so those
 * two numbers are an independent check on this parser: if what we read out of
 * the table disagrees with what the game shows the player, the parser is
 * wrong. For the starter Peugeot 106 the screen reads 106.2 ft-lbs @ 3150 rpm
 * and 119.8 bhp @ 6250 rpm; that is the assertion at the bottom.
 */
#include "car_setup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NM_PER_FTLB  1.35582f
#define KW_PER_BHP   0.7457f
#define PI           3.14159265358979f

/* The torque curve is nine samples evenly spread over idle..max. Walk it
 * finely to find the peaks the way a dyno sweep would. */
static void peaks(const N2MotorSpec *m, float *t_nm, float *t_rpm,
                  float *p_kw, float *p_rpm)
{
    float lo = m->idle_rpm, hi = m->max_rpm;
    *t_nm = *t_rpm = *p_kw = *p_rpm = 0.0f;
    for (int s = 0; s <= 8000; s++) {
        float rpm = lo + (hi - lo) * (float)s / 8000.0f;
        float u = (rpm - lo) / (hi - lo) * 8.0f;
        int k = (int)u; if (k > 7) k = 7;
        /* torque is stored in kN*m */
        float a = m->torque[k] * 1000.0f, b = m->torque[k + 1] * 1000.0f;
        float t = a + (b - a) * (u - (float)k);
        float p = t * rpm * 2.0f * PI / 60.0f / 1000.0f;   /* kW */
        if (t > *t_nm) { *t_nm = t; *t_rpm = rpm; }
        if (p > *p_kw) { *p_kw = p; *p_rpm = rpm; }
    }
}

static const unsigned char *g_data; static long g_len;
static int g_fail;

static void show(const char *name, void *ud)
{
    (void)ud;
    N2CarSetup s;
    if (!n2_car_setup_load(&s, g_data, g_len, name)) return;
    float tnm, trpm, pkw, prpm;
    peaks(&s.motor, &tnm, &trpm, &pkw, &prpm);
    printf("%-13s %5.0f kg  %s  %d-speed  %6.1f ft-lbs @%5.0f  %6.1f bhp @%5.0f\n",
           name, s.mass * 1000.0f,
           s.drive.split_rear > 0.9f ? "RWD" :
           s.drive.split_rear < 0.1f ? "FWD" : "AWD",
           s.drive.num_gears,
           tnm / NM_PER_FTLB, trpm, (pkw / KW_PER_BHP), prpm);
}

/* One car whose Dyno screen we have, checked against what we parsed. */
static void check(const char *car, float want_ftlb, float want_rpm,
                  float want_bhp, float want_prpm)
{
    N2CarSetup s;
    if (!n2_car_setup_load(&s, g_data, g_len, car)) {
        printf("FAIL %s: not in the table\n", car); g_fail = 1; return;
    }
    float tnm, trpm, pkw, prpm;
    peaks(&s.motor, &tnm, &trpm, &pkw, &prpm);
    float ftlb = tnm / NM_PER_FTLB, bhp = pkw / KW_PER_BHP;
    /* The rev figures are the game's, rounded to a multiple of 25; ours come
     * off a fine sweep of a nine-point curve, so allow a percent there. */
    int ok = fabsf(ftlb - want_ftlb) < 0.5f && fabsf(bhp - want_bhp) < 0.5f
          && fabsf(trpm - want_rpm) < want_rpm * 0.02f
          && fabsf(prpm - want_prpm) < want_prpm * 0.02f;
    printf("%s %s: parsed %.1f ft-lbs @%.0f, %.1f bhp @%.0f "
           "(garage: %.1f @%.0f, %.1f @%.0f)\n",
           ok ? "ok  " : "FAIL", car, ftlb, trpm, bhp, prpm,
           want_ftlb, want_rpm, want_bhp, want_prpm);
    if (!ok) g_fail = 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: car_dyno <GLOBALB.BUN>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)g_len);
    if (!d || fread(d, 1, (size_t)g_len, f) != (size_t)g_len) {
        fprintf(stderr, "short read\n"); return 2;
    }
    fclose(f); g_data = d;

    int n = n2_car_setup_list(d, g_len, show, NULL);
    if (!n) { fprintf(stderr, "car table not found\n"); return 2; }
    printf("\n%d records\n\n", n);

    check("PEUGOT106", 106.2f, 3150.0f, 119.8f, 6250.0f);

    free(d);
    return g_fail ? 1 : 0;
}
