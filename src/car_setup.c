/* car_setup.c -- parser for the shipped per-car parameter table. See car_setup.h. */
#include "car_setup.h"
#include <string.h>

/* Record layout inside the car table. A record is CAR_RECORD bytes:
 *   +0x000  car name, NUL padded
 *   +0x040  path of the car's geometry file
 *   +0x110  start of the parameters; the offsets below are from there. */
#define CAR_RECORD       0x890
#define CAR_TABLE_MAGIC  0x00034600u
#define P_COM            0x000   /* centre of mass X (fwd), Y, Z (height) */
#define P_WHEEL          0x010   /* 4 x 0x30: pos[3], radius, width */
#define P_TYRE           0x0D0   /* 2 x 0x20: one per axle          */
#define P_MASS           0x110   /* mass, then length/width/height  */
#define P_INERTIA_X      0x120   /* diagonal, stride 0x14           */
#define P_STRUT          0x170   /* 2 x 0x20: one per axle          */
#define P_DRIVE          0x1B0
#define P_ENGINE         0x1F0
#define P_AERO           0x260
#define P_BRAKE          0x270
#define P_YAW_CLAMP      0x288
#define P_SPEED_LIMIT    0x290

static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float rd_f(const unsigned char *p) {
    /* the file stores little-endian IEEE floats */
    uint32_t v = rd_u32(p); float f; memcpy(&f, &v, 4); return f;
}
static int rd_i(const unsigned char *p) { return (int)rd_u32(p); }

static int name_eq(const unsigned char *rec, const char *name) {
    for (int i = 0; i < 24; i++) {
        unsigned char a = rec[i], b = (unsigned char)name[i];
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
        if (a != b) return 0;
        if (!a) return 1;
    }
    return 1;
}

/* Locate the car table and step over the filler its payload starts with. */
static long find_table(const unsigned char *d, long len, long *tlen_out) {
    for (long i = 0; i + 8 <= len; i++) {
        if (rd_u32(d + i) != CAR_TABLE_MAGIC) continue;
        long sz = (long)rd_u32(d + i + 4);
        if (sz <= CAR_RECORD || i + 8 + sz > len) continue;
        long tbl = i + 8;
        while (sz > 0 && d[tbl] == 0x11) { tbl++; sz--; }
        *tlen_out = sz;
        return tbl;
    }
    return -1;
}

int n2_car_setup_load(N2CarSetup *I, const unsigned char *d, long len,
                      const char *car_name)
{
    if (!I || !d || !car_name) return 0;

    long tlen = 0, tbl = find_table(d, len, &tlen);
    if (tbl < 0) return 0;

    for (long r = 0; r + CAR_RECORD <= tlen; r += CAR_RECORD) {
        const unsigned char *rec = d + tbl + r;
        if (!name_eq(rec, car_name)) continue;

        const unsigned char *P = rec + 0x110;
        N2CarSetup s;
        memset(&s, 0, sizeof s);

        for (int k = 0; k < 3; k++) s.com[k] = rd_f(P + P_COM + k * 4);

        for (int i = 0; i < 4; i++) {
            const unsigned char *w = P + P_WHEEL + i * 0x30;
            for (int k = 0; k < 3; k++) s.wheel[i].pos[k] = rd_f(w + k * 4);
            s.wheel[i].radius = rd_f(w + 0x10);
            s.wheel[i].width  = rd_f(w + 0x14);
        }
        for (int a = 0; a < 2; a++) {
            const unsigned char *t = P + P_TYRE + a * 0x20;
            s.tyre[a].slip_angle_deg = rd_f(t + 0x00);
            s.tyre[a].ref_load       = rd_f(t + 0x04);
            s.tyre[a].lateral_scale  = rd_f(t + 0x08);
            s.tyre[a].mu_static      = rd_f(t + 0x0C);
            s.tyre[a].mu_slide       = rd_f(t + 0x10);

            const unsigned char *u = P + P_STRUT + a * 0x20;
            s.strut[a].progression   = rd_f(u + 0x00);
            s.strut[a].stiffness     = rd_f(u + 0x04);
            s.strut[a].damp_bump     = rd_f(u + 0x08);
            s.strut[a].damp_rebound  = rd_f(u + 0x0C);
            s.strut[a].antiroll      = rd_f(u + 0x10);
            s.strut[a].travel_up     = rd_f(u + 0x14);
            s.strut[a].travel_down   = rd_f(u + 0x18);
        }
        s.mass    = rd_f(P + P_MASS);
        s.dims[0] = rd_f(P + P_MASS + 0x04);
        s.dims[1] = rd_f(P + P_MASS + 0x08);
        s.dims[2] = rd_f(P + P_MASS + 0x0C);
        for (int k = 0; k < 3; k++) s.inertia[k] = rd_f(P + P_INERTIA_X + k * 0x14);

        s.drive.shift_frac  = rd_f(P + P_DRIVE + 0x00);
        s.drive.final_front = rd_f(P + P_DRIVE + 0x08);
        s.drive.final_rear  = rd_f(P + P_DRIVE + 0x0C);
        s.drive.split_rear  = rd_f(P + P_DRIVE + 0x10);
        s.drive.clutch_k    = rd_f(P + P_DRIVE + 0x14);
        s.drive.num_gears   = rd_i(P + P_DRIVE + 0x18);
        s.drive.clutch_rate = rd_f(P + P_DRIVE + 0x1C);
        /* eight ratios: reverse, neutral, then the forward gears */
        for (int k = 0; k < 8; k++) s.drive.gear[k] = rd_f(P + P_DRIVE + 0x20 + k * 4);
        s.drive.gear[8] = 0.0f;

        s.motor.idle_rpm      = rd_f(P + P_ENGINE + 0x00);
        s.motor.shift_rpm     = rd_f(P + P_ENGINE + 0x04);
        s.motor.max_rpm       = rd_f(P + P_ENGINE + 0x08);
        s.motor.crank_inertia = rd_f(P + P_ENGINE + 0x0C);
        for (int k = 0; k < 9; k++) s.motor.torque[k] = rd_f(P + P_ENGINE + 0x10 + k * 4);
        for (int k = 0; k < 3; k++) s.motor.engine_brake[k] = rd_f(P + P_ENGINE + 0x58 + k * 4);

        s.aero.unused_drag     = rd_f(P + P_AERO + 0x00);
        s.aero.downforce_front = rd_f(P + P_AERO + 0x04);
        s.aero.downforce_rear  = rd_f(P + P_AERO + 0x08);
        s.aero.scale           = rd_f(P + P_AERO + 0x0C);

        s.brake.unused0   = rd_f(P + P_BRAKE + 0x00);
        s.brake.service   = rd_f(P + P_BRAKE + 0x04);
        s.brake.handbrake = rd_f(P + P_BRAKE + 0x08);
        s.brake.bias      = rd_f(P + P_BRAKE + 0x0C);

        s.yaw_torque_clamp = rd_f(P + P_YAW_CLAMP);
        s.speed_limit_mph  = rd_f(P + P_SPEED_LIMIT);

        /* Sanity gate: a record that fails this is not a car, and driving with
         * a zero mass or no gears would divide by zero further down. */
        if (!(s.mass > 0.2f && s.mass < 20.0f)) return 0;
        if (s.drive.num_gears < 1 || s.drive.num_gears > 7) return 0;
        if (!(s.motor.max_rpm > s.motor.idle_rpm && s.motor.idle_rpm > 100.0f)) return 0;
        for (int i = 0; i < 4; i++)
            if (!(s.wheel[i].radius > 0.05f && s.wheel[i].radius < 1.5f)) return 0;
        for (int a = 0; a < 2; a++)
            if (!(s.strut[a].stiffness > 0.0f) || !(s.tyre[a].mu_static > 0.0f)) return 0;

        *I = s;
        return 1;
    }
    return 0;
}

int n2_car_setup_list(const unsigned char *d, long len,
                      void (*cb)(const char *name, void *ud), void *ud)
{
    if (!d || !cb) return 0;
    long tlen = 0, tbl = find_table(d, len, &tlen);
    if (tbl < 0) return 0;

    int n = 0;
    for (long r = 0; r + CAR_RECORD <= tlen; r += CAR_RECORD) {
        char nm[25];
        memcpy(nm, d + tbl + r, 24); nm[24] = 0;
        if (!nm[0]) continue;
        cb(nm, ud);
        n++;
    }
    return n;
}
