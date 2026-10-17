/* drivetrain.c — OpenUG2 drivetrain / longitudinal acceleration (Stage 1).
 * Implements docs/cleanroom-driving-physics/SPEC.md Stage 1; see drivetrain.h
 * and docs/cleanroom-driving-physics/IMPL_DESIGN.md. Section tags "S1-x.y"
 * refer to that spec. Persistent state is binary32, intermediates are double. */
#include "drivetrain.h"
#include "tire_marks.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MPS_TO_MPH 2.237        /* S1-7.4 / U-8 */

static double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ===================================================================== */
/* Car record: derived tables (S1-1.3, S1-1.4, S1-4.2)                     */
/* ===================================================================== */
int drv_drive_class(float s) {
    if (s < 0.2f) return DRV_CLASS_A;
    if (s > 0.8f) return DRV_CLASS_C;
    return DRV_CLASS_B;
}
float drv_launch_scale(int cls) {
    return cls == DRV_CLASS_A ? 1.0f : (cls == DRV_CLASS_C ? 1.1f : 1.2f);
}

/* S1-12.3: descending thresholds, strict >. Equality falls to the lower class. */
int drv_perf_class_from_score(float score) {
    const double S = (double)score;
    if (S > 500.0) return 0;
    if (S > 401.0) return 1;
    if (S > 273.0) return 2;
    return 3;
}

/* S1-12.1: the forced flag short-circuits the classification; the score is unused. */
int drv_perf_class_resolve(int forced, float score) {
    if (forced != 0) return 3;
    return drv_perf_class_from_score(score);
}

/* S1-7.7 / S1-12.5: the fixed four-entry K table, contiguous in C order, read
 * as a plain base-plus-slot access with no bounds test, clamp or default. The
 * coefficient is the stored binary32 word itself, so the table is held as the
 * exact bit patterns tabulated in S1-7.7 rather than decimal literals (whose
 * nearest doubles differ from the stored values by about 1.5e-8). The slot
 * immediately preceding K[0] holds the stored 0.25 constant (binary32
 * 0x3E800000) that the unset sentinel C = -1 selects (S1-12.5). The class
 * scheme produces only -1 and 0..3; any other index is outside both the scheme
 * and the stated read (U-17), so it is reported as unspecified rather than
 * guessed. */
double drv_kappa_c(int perf_class) {
    static const uint32_t slots[5] = {
        0x3E800000u,  /* C = -1: preceding slot, stored 0.25 (S1-12.5) */
        0x00000000u,  /* K[0] =  0.0                                  */
        0xBE4CCCCDu,  /* K[1] = -0.20000000298023223876953125         */
        0xBECCCCCDu,  /* K[2] = -0.4000000059604644775390625          */
        0xBF333333u   /* K[3] = -0.699999988079071044921875          */
    };
    if (perf_class < -1 || perf_class > 3) return NAN;
    float stored = 0.0f;
    memcpy(&stored, &slots[perf_class + 1], sizeof stored);  /* exact word read */
    return (double)stored;
}
float drv_curve_knot_rpm(float a, float b, int q, int i) {
    if (q < 2) return a;
    return (float)((double)a + (double)i * ((double)b - (double)a) / (double)(q - 1));
}

int drv_car_finalize(DrvCar *car) {
    if (car->gear_count < 1 || car->gear_count > DRV_MAX_FORWARD) return -1;
    if (!(car->engine_inertia > 0.0f) || !(car->mass > 0.0f)) return -1;
    if (!(car->max_rpm > car->idle_rpm)) return -1;
    for (int i = 0; i < 4; i++) if (!(car->wheel_radius[i] > 0.0f)) return -1;

    for (int j = 0; j < DRV_TORQUE_SAMPLES; j++) {
        car->torque[j] = car->torque_primary[j] + car->torque_secondary[j];   /* binary32 sum */
        if (!isfinite(car->torque[j])) return -1;
    }
    car->drive_class = drv_drive_class(car->rear_share);
    car->launch_scale = drv_launch_scale(car->drive_class);

    const int n = car->gear_count;
    const double e0 = car->idle_rpm, e1 = car->shift_rpm, e2 = car->max_rpm;
    memset(car->up_rpm, 0, sizeof car->up_rpm);
    memset(car->down_rpm, 0, sizeof car->down_rpm);
    car->up_rpm[1] = (float)((e0 + (e1 - e0) * (double)car->launch_fraction) * (double)car->launch_scale);
    for (int j = 2; j <= n; j++) {
        car->up_rpm[j] = (float)(e1 - 100.0);
        const double rj = car->ratio[j];
        car->down_rpm[j + 1] = fabs(rj) <= 1e-5 ? 0.0f
            : car->up_rpm[j] * (car->ratio[j + 1] / car->ratio[j]);   /* Reference rounding convention only; see S1-4.2 / U-14. */
    }
    car->up_rpm[n + 1] = (float)(e2 + 500.0);
    for (int j = 0; j < DRV_MAX_FORWARD + 2; j++)
        if (!isfinite(car->up_rpm[j]) || !isfinite(car->down_rpm[j])) return -1;
    return 0;
}

void drv_tune_default(DrvTune *t) {
    memset(t, 0, sizeof *t);
    t->torque_scale = 1.0f;
    t->perf_class = (signed char)DRV_PERF_CLASS_UNSET;  /* composition step has not run (S1-12) */
    t->grip_q = 1.0f;
    t->rate_scale = 1.0f;
}

void drv_env_default(DrvEnv *e) {
    memset(e, 0, sizeof *e);
    e->rear_grip_multiplier = 1.0f; /* Explicit level/straight/no-handbrake test adapter. */
    for (int i = 0; i < 4; i++) {
        e->wheel[i].contact = 1;
        e->wheel[i].mu_peak_surface = 1.0f;
        e->wheel[i].peak_scale = 1.0f;
        e->wheel[i].mu_slide_surface = 1.0f;
        e->wheel[i].roll_drag = 1.0f;
    }
}

/* ===================================================================== */
/* Stateless helpers                                                       */
/* ===================================================================== */
float drv_quantize_frame_dt_test_adapter(const DrvTune *tune, double seconds) {
    double q = seconds;
    if (tune) {
        if (tune->frame_dt_min > 0.0f && q < tune->frame_dt_min) q = tune->frame_dt_min;
        if (tune->frame_dt_max > 0.0f && q > tune->frame_dt_max) q = tune->frame_dt_max;
    }
    return (float)(floor(q / DRV_FRAME_QUANTUM + 0.5) * DRV_FRAME_QUANTUM);
}

int drv_substep_count(float frame_dt, float *h) {
    double dt = (double)frame_dt;
    int n = dt > 0.0 ? (int)(dt / (double)DRV_SUBSTEP_MAX) + 1 : 1;   /* trunc, then +1 */
    if (h) *h = (float)(dt / (double)n);
    return n;
}

/* S1-3.1 */
double drv_curve(const float *s, int q, double a, double b, double x) {
    if (!s || q < 2 || !isfinite(a) || !isfinite(b) || !(b > a) || isnan(x)) return NAN;
    double p = (x - a) * (double)(q - 1) / (b - a);
    if (p <= 0.0) return s[0];
    if (p >= q - 1) return s[q - 1];
    if (!isfinite(p)) return NAN;
    int i = (int)p;                              /* bounded before conversion */
    double f = p - (double)i;
    return f * (double)s[i + 1] + (1.0 - f) * (double)s[i];
}

/* S1-3.3 (nitrous and over-rev adjustment are deferred, S1-9) */
double drv_engine_torque_at(const DrvCar *car, const DrvTune *tune, int gear, int limiter, double rpm, double u) {
    const double G = tune->torque_scale, B = tune->torque_bonus ? 1.05 : 1.0;
    if (limiter) u = 0.0;
    double ton = drv_curve(car->torque, DRV_TORQUE_SAMPLES, car->idle_rpm, car->max_rpm, rpm) * G * B * u;
    double toff = -(1.0 - u) * drv_curve(car->engine_brake, DRV_BRAKE_SAMPLES, car->idle_rpm, car->max_rpm, rpm);
    if (limiter) toff *= 2.0;
    double te = ton + toff;
    if (te < 0.0 && !limiter) te *= (gear == DRV_GEAR_N) ? 0.65 : 0.85;
    return te;
}

/* S1-7.3 */
double drv_clutch_torque(const DrvCar *car, double omega_e, double omega_w, double engage, float *delta_prev) {
    double delta_w = clampd(omega_e - omega_w, -300.0, 300.0);
    double delta_rpm = (double)(float)((omega_w - omega_e) * (double)DRV_KR);
    double k = car->clutch_k;
    double dp = (double)*delta_prev;
    if (dp != 0.0 && delta_rpm * dp < 0.0 && fabs(delta_rpm) > 2000.0 && fabs(dp) > 2000.0) k *= 0.5;
    *delta_prev = (float)delta_rpm;
    return k * engage * delta_w;
}

/* S1-7.2 step 3 */
void drv_axle_split(double wa, double wb, int both_contact, double *share_a, double *share_b) {
    double p = both_contact ? 0.35 : 0.5;
    double f = fabs(wa + wb) <= 1e-5 ? 0.5 : wb / (wa + wb);
    if (f < p) f = 1.0 - 0.175;
    else if (f > 1.0 - p) f = 0.175;      /* else-if: the vectors need it (see IMPL_DESIGN) */
    if (share_b) *share_b = f;
    if (share_a) *share_a = 1.0 - f;
}

/* ===================================================================== */
/* Body helpers (S1-8.2)                                                   */
/* ===================================================================== */
void drv_body_accel_straight(void *ud, const float F[4], const float Fy[4], float A[4]) {
    const DrvBodyStraight *b = (const DrvBodyStraight *)ud;
    (void)Fy;                                   /* symmetric case: lateral forces cancel */
    double sum = b->ex;
    for (int i = 0; i < 4; i++) sum += F[i];
    float a = (float)(sum / (double)b->mass);
    for (int i = 0; i < 4; i++) A[i] = a;
}

static void planar_acceleration(const DrvBodyPlanar *b,const float F[4],const float Fy[4],double out[3]) {
    double fx = b->ex, fy = b->ey, mom = 0;
    for (int i = 0; i < 4; i++) {
        double cx = (double)F[i] * b->head[i][0] + (double)Fy[i] * b->lat[i][0];
        double cy = (double)F[i] * b->head[i][1] + (double)Fy[i] * b->lat[i][1];
        fx += cx; fy += cy;
        mom += (double)b->pos[i][0] * cy - (double)b->pos[i][1] * cx;
    }
    out[0]=fx/(double)b->mass;out[1]=fy/(double)b->mass;out[2]=mom*(double)b->inv_iz;
}
void drv_body_planar_acceleration(const DrvBodyPlanar *b,const float F[4],const float Fy[4],float out[3]) {
    double values[3];planar_acceleration(b,F,Fy,values);
    for(int k=0;k<3;k++)out[k]=(float)values[k];
}
void drv_body_accel_planar(void *ud, const float F[4], const float Fy[4], float A[4]) {
    const DrvBodyPlanar *b = (const DrvBodyPlanar *)ud;
    double values[3];planar_acceleration(b,F,Fy,values);
    double ax=values[0],ay=values[1],al=values[2];
    for (int i = 0; i < 4; i++)
        A[i] = (float)((ax - al * (double)b->pos[i][1]) * (double)b->head[i][0]
                     + (al * (double)b->pos[i][0] + ay) * (double)b->lat[i][0]);
}

/* ===================================================================== */
/* Instance                                                                */
/* ===================================================================== */
static void redraw_jitter(Drivetrain *d) {     /* S1-4.3 */
    for (int k = 0; k < 3; k++) {
        if (d->tune.rand01) {
            double u = (double)d->tune.rand01(d->tune.rand_ud);
            d->jitter[k] = (float)(-100.0 + 100.0 * u);
        } else {
            d->jitter[k] = 0.0f;                /* suppressed */
        }
    }
}

static void engine_post(Drivetrain *d);
static void clutch_post(Drivetrain *d);

int drv_init(Drivetrain *d, const DrvCar *car, const DrvTune *tune) {
    DrvCar finalized = *car;
    if (drv_car_finalize(&finalized) != 0) return -1;
    memset(d, 0, sizeof *d);
    d->car = finalized;
    if (tune) d->tune = *tune; else drv_tune_default(&d->tune);
    if (!(d->tune.rate_scale > 0.0f)) d->tune.rate_scale = 1.0f;
    /* S1-12: the drivetrain block's constructor default is 3; the creation-time
     * copy of the physics-parameter object's class overwrites it (including -1
     * when the parameter-composition step did not run). */
    d->perf_class = (signed char)DRV_PERF_CLASS_DEFAULT;
    d->perf_class = d->tune.perf_class;

    for (int i = 0; i < 4; i++) {
        d->wheel[i].w_long = 1.0f;
        d->wheel[i].w_lat = 1.0f;
        d->wheel[i].dir = DRV_DIR_STOPPED;
    }
    drv_env_default(&d->env);

    float rpm0 = d->tune.initial_rpm > 0.0f ? d->tune.initial_rpm : d->car.idle_rpm;       /* U-15 */
    d->omega_e = (float)clampd((double)rpm0 * DRV_KW, (double)d->car.idle_rpm * DRV_KW, (double)d->car.max_rpm * DRV_KW);
    d->throttle_mult = 1.0f;                       /* S1-3.2 */
    d->gear = d->target_gear = DRV_GEAR_N;         /* S1-4.1 */
    d->mode = DRV_CLUTCH_ENGAGING;
    d->engage = 0.0f;
    d->sel_gear = -1;
    d->manual_last_gear = -1;  /* Unspecified until drv_assist_supply(). */
    redraw_jitter(d);
    /* S1-2.5: zero-time post-processing, without derivative evaluation.
     * Initial wheel angles/spins are zero, so their post-step has no effect. */
    engine_post(d);
    clutch_post(d);
    drv_couple(d, &d->env, NULL, NULL);
    return 0;
}

/* ===================================================================== */
/* Wheel-side combination (S1-7.1)                                         */
/* ===================================================================== */
static int front_driven(const Drivetrain *d) { return d->car.rear_share <= 0.999f; }
static int rear_driven(const Drivetrain *d)  { return d->car.rear_share >= 0.001f; }

/* Combine per-wheel values the way S1-7.1 combines spins: mean per driven axle,
 * optionally times the axle's final drive, mean over driven axles. */
static double side_combine(const Drivetrain *d, const double v[4], int final_drive) {
    const int fd = front_driven(d), rd = rear_driven(d);
    double wf = (v[0] + v[1]) * 0.5, wr = (v[2] + v[3]) * 0.5;
    if (final_drive) { wf *= d->car.final_front; wr *= d->car.final_rear; }
    if (fd && rd) return (wf + wr) * 0.5;
    if (rd) return wr;
    return wf;
}

double drv_wheel_side_speed(const Drivetrain *d) {
    double w[4];
    for (int i = 0; i < 4; i++) w[i] = d->wheel[i].omega;
    return (double)d->car.ratio[d->gear] * side_combine(d, w, 1);
}

double drv_engine_torque(const Drivetrain *d) {
    double rpm = (double)(float)(d->omega_e * DRV_KR);
    return drv_engine_torque_at(&d->car, &d->tune, d->gear, d->limiter, rpm, (double)(d->throttle_mult * d->throttle));
}

/* ===================================================================== */
/* Per-frame: input shaping (S1-5) and gear selector (S1-4.5)              */
/* ===================================================================== */
static float uth(const Drivetrain *d, int g) {        /* up-shift threshold */
    double v = (double)d->car.up_rpm[g] + (double)(g == 3 ? d->jitter[2] : d->jitter[0]);
    return (float)fmin(v, (double)d->car.shift_rpm);
}
static float uth2(const Drivetrain *d, int g) {       /* Uth' */
    return (float)fmin((double)d->car.up_rpm[g] - 100.0, (double)d->car.shift_rpm);
}

static void select_gear(Drivetrain *d, int cmd, double rpm_e, float frame_dt) {
    const DrvCar *c = &d->car;
    const int n = c->gear_count;
    const int g = d->gear;
    d->report_auto = 0;

    if (cmd >= 0) {                                    /* direct command */
        if (cmd <= n + 1 && cmd != g && cmd != d->target_gear) d->target_gear = cmd;
        return;
    }
    if (cmd == DRV_CMD_AUTO) {
        d->sel_gear = -1;                              /* mark driver-selected gear "automatic" */
        if (g <= DRV_GEAR_N) { d->target_gear = 2; return; }
        if (d->mode != DRV_CLUTCH_LOCKED) return;
        if (rpm_e >= (double)uth(d, g) && g < n + 1) {
            int ok = 1;
            for (int i = 0; i < 4; i++)
                if (!d->env.wheel[i].contact || !(d->wheel[i].slip_speed < 4.0f)) ok = 0;
            if (ok) d->target_gear = g + 1;
            return;                                    /* otherwise nothing, no down-shift test */
        }
        double d1, d2;
        if (g > 2) {
            d1 = (double)c->down_rpm[g] + (double)d->jitter[1];
            d2 = ((double)c->ratio[g] / (double)c->ratio[g - 1]) * (double)uth2(d, g - 1) - 200.0;
        } else {
            d1 = d->throttle < 0.2f ? (double)c->idle_rpm + 100.0 : 0.0;
            d2 = 0.0;
        }
        if (rpm_e <= d1 && rpm_e <= d2) d->target_gear = g - 1;
        return;
    }
    if (cmd == DRV_CMD_MANUAL_ASSIST) {
        /* U-2: never invent the missing retail initial assist state. */
        if (!d->assist_state_supplied) return;
        d->manual_hold_time = (float)((double)d->manual_hold_time + frame_dt);
        if (d->mode != DRV_CLUTCH_LOCKED) return;
        int gs = d->sel_gear;
        if (gs < 2 || gs > n + 1) return;
        if (gs != d->target_gear) {
            d->manual_last_gear = g;
            d->manual_hold_time = 0;
            if (gs < d->target_gear) {
                const double ceiling = g > 2
                    ? ((double)c->ratio[g] / c->ratio[g - 1]) * 1.2 * c->max_rpm
                    : (d->throttle < 0.2f ? c->idle_rpm + 100.0 : 0.0);
                if (rpm_e > ceiling) d->sel_gear = d->target_gear = g;
                else d->target_gear = g - 1;
            } else d->target_gear = g + 1;
            return;
        }
        if (d->manual_hold_time > 10.0f ||
            (g > d->manual_last_gear && rpm_e > c->max_rpm - 300.0)) {
            d->report_auto = 1;
        } else if (rpm_e < c->idle_rpm + 100.0 && g > 2) {
            d->target_gear = g - 1;
            d->report_auto = 1;
        }
    }
}

/* S1-4.5 / U-2: caller owns the initial assist state and valid selections. */
int drv_assist_supply(Drivetrain *d, int selected, float elapsed, int remembered) {
    const int top = d->car.gear_count + 1;
    if (selected < DRV_GEAR_N || selected > top || remembered < 0 || remembered > top ||
        !(elapsed >= 0) || !isfinite(elapsed)) return -1;
    d->sel_gear = selected;
    d->manual_hold_time = elapsed;
    d->manual_last_gear = remembered;
    d->assist_state_supplied = 1;
    return 0;
}
int drv_assist_select(Drivetrain *d, int selected) {
    if (!d->assist_state_supplied || selected < 2 || selected > d->car.gear_count + 1) return -1;
    if (d->sel_gear == DRV_GEAR_N) d->manual_hold_time = 0;
    d->sel_gear = selected;
    return 0;
}

void drv_frame_begin(Drivetrain *d, const DrvInput *in, float frame_dt, double rpm_override) {
    const double rpm = (double)(float)(d->omega_e * DRV_KR);
    double t0 = clampd(in->throttle, 0.0, 1.0), b0 = clampd(in->brake, 0.0, 1.0), hb0 = clampd(in->handbrake, 0.0, 1.0);

    /* One shared S1-5.1/S3-2 operation, before engine-only shaping. */
    BrkPedals pedals={(float)t0,(float)b0,(float)hb0};
    brk_pedals(&pedals,d->brakes.enabled?d->brakes.exclusivity:1);
    t0=pedals.throttle;b0=pedals.service;hb0=pedals.handbrake;
    if(d->brakes.enabled)
        brk_distribute(&d->brakes.car,&pedals,d->brakes.signed_distribution?(double)(float)((double)d->env.signed_forward*2.236999988555908f):(double)d->env.body_speed*BRK_MPH,&d->brakes.frame);
    /* S1-5.2 dead zone */
    double t = t0 <= 0.15 ? 0.0 : t0;
    /* S1-5.3 anti-stall floor */
    if (t > 0.0 && t < 0.5 && d->gear > DRV_GEAR_N && rpm < 3000.0) t = 0.5;
    d->throttle = (float)t; d->brake = (float)b0; d->handbrake = (float)hb0;

    select_gear(d, in->gear_cmd, rpm_override >= 0.0 ? rpm_override : rpm, frame_dt);
}

/* ===================================================================== */
/* Substep: derivatives, Euler, post-steps (S1-2.4 a-c)                    */
/* ===================================================================== */
static const double CLUTCH_RATE_K[4] = { -3.0, 0.0, 1.5, 0.0 };

static void clutch_post(Drivetrain *d) {                /* S1-4.4 */
    const DrvCar *c = &d->car;
    d->omega_w = (float)drv_wheel_side_speed(d);
    const int crawl = d->throttle < 0.3f && d->omega_w <= c->idle_rpm * DRV_KW + 1e-4f;
    switch (d->mode) {
    case DRV_CLUTCH_DISENGAGING:
        d->throttle_mult = 0.0f;
        if (d->engage <= 0.0f) { d->engage = 0.0f; d->mode = DRV_CLUTCH_OPEN; }
        break;
    case DRV_CLUTCH_OPEN:
        d->throttle_mult = 0.0f;
        d->gear = d->target_gear;
        if (d->pending_shift || !crawl) { d->mode = DRV_CLUTCH_ENGAGING; redraw_jitter(d); }
        break;
    case DRV_CLUTCH_ENGAGING:
        d->throttle_mult = 0.7f;
        if (d->engage >= 1.0f) {
            d->engage = 1.0f;
            if (fabs((double)d->omega_e - (double)d->omega_w) <= 50.0 || d->gear == DRV_GEAR_N) d->mode = DRV_CLUTCH_LOCKED;
        }
        if (crawl && d->gear != DRV_GEAR_N) d->mode = DRV_CLUTCH_DISENGAGING;
        break;
    case DRV_CLUTCH_LOCKED:
    default:
        d->throttle_mult = 1.0f;
        if (d->gear != DRV_GEAR_N && crawl) d->mode = DRV_CLUTCH_DISENGAGING;
        if (d->gear == d->target_gear) d->pending_shift = 0;
        else { d->pending_shift = 1; d->mode = DRV_CLUTCH_DISENGAGING; }
        break;
    }
    /* missed-shift timer and drag shift bonus: deferred (S1-4.4, S1-9) */
}

static void engine_post(Drivetrain *d) {                /* S1-3.5 */
    const DrvCar *c = &d->car;
    d->omega_e = (float)clampd(d->omega_e, c->idle_rpm * DRV_KW, c->max_rpm * DRV_KW);
    d->feedback = 0.0f;
    const double rpm = (double)(float)(d->omega_e * DRV_KR);
    const double e2 = c->max_rpm;
    if (d->limiter) { if (rpm < e2 - 0.0125 * e2) d->limiter = 0; }
    else if (rpm > e2 - 25.0) d->limiter = 1;
}

double drv_wheel_resistance(float omega,float surface,int damage) {
    return -(double).002f*surface*omega-(damage?(double).1f*omega:0);
}

int drv_brakes_supply(Drivetrain *d,const BrkCar *car,unsigned locks,int relief_disabled,int exclusivity) {
    if(!d || !car || locks>15 || !isfinite(car->service) || car->service<0 ||
       !isfinite(car->handbrake) || car->handbrake<0 || !isfinite(car->rear_share) ||
       car->rear_share<0 || car->rear_share>1)return -1;
    BrkCar supplied=*car;
    memset(&d->brakes,0,sizeof d->brakes);
    d->brakes.enabled=1;d->brakes.car=supplied;
    d->brakes.exclusivity=!!exclusivity;d->brakes.relief_disabled=!!relief_disabled;
    for(int i=0;i<4;i++)d->brakes.locked[i]=d->env.wheel[i].locked=(locks>>i)&1;
    return 0;
}

double drv_wheel_derivative(Drivetrain *d,int i,float h) {
    DrvWheel *w=&d->wheel[i];const DrvWheelEnv *e=&d->env.wheel[i];
    const double radius=d->car.wheel_radius[i];
    double net=.5*((double)w->tau-radius*w->force+w->tau_smooth);
    w->tau_smooth=(float)net;
    double acceleration=e->locked?0:
        (net+drv_wheel_resistance(w->omega,e->roll_drag,e->drag_flag))/(double)DRV_WHEEL_INERTIA;
    /* S3-5: gripping correction follows locked torque/drag zeroing. */
    if(w->gripping && (double)h>.001)acceleration-=(double)w->slip_speed/(radius*h);
    return acceleration;
}

void drv_integrate(Drivetrain *d, float h) {
    const double dh = (double)h;
    /* (a) derivatives from the current state */
    const double te = drv_engine_torque(d);
    d->engine_torque = (float)te;
    const double we_dot = (te + (double)d->feedback) / (double)d->car.engine_inertia;
    const double c_dot = (double)d->car.clutch_rate * (double)d->tune.rate_scale * CLUTCH_RATE_K[d->mode & 3];
    double w_dot[4];
    for (int i = 0; i < 4; i++) w_dot[i]=drv_wheel_derivative(d,i,h);
    /* (b) explicit Euler on every state scalar */
    d->omega_e = (float)((double)d->omega_e + dh * we_dot);
    d->engage = (float)((double)d->engage + dh * c_dot);
    for (int i = 0; i < 4; i++) {
        DrvWheel *w = &d->wheel[i];
        w->phi = (float)((double)w->phi + dh * (double)w->omega);
        w->omega = (float)((double)w->omega + dh * w_dot[i]);
    }
    drv_post(d);
}

void drv_post(Drivetrain *d) {
    /* (c) post-steps in block order: wheels (+ suspension, later stage), engine, clutch */
    for (int i = 0; i < 4; i++) {
        DrvWheel *w = &d->wheel[i];
        w->tau = 0.0f; w->force = 0.0f; w->force_lat = 0.0f; w->slip_speed = 0.0f;
        const double turn_limit = 628.3179931640625, reset_limit = 6283.18017578125;
        if (w->phi > turn_limit) {
            w->phi = (float)((double)w->phi - turn_limit);
            if (w->phi > reset_limit) w->phi = 0;
        } else if (w->phi < -turn_limit) {
            w->phi = (float)((double)w->phi + turn_limit);
            if (w->phi < -reset_limit) w->phi = 0;
        }
        brk_direction_post(&w->omega,&w->dir);
    }
    engine_post(d);
    clutch_post(d);
}

/* ===================================================================== */
/* Coupling (S1-7)                                                         */
/* ===================================================================== */
static void dist(Drivetrain *d, double T) {             /* S1-7.2 */
    if (d->gear == DRV_GEAR_N) return;
    double nc = d->contact_count / 4.0;
    d->feedback = (float)((double)d->feedback - (nc > 0.25 ? nc : 0.25) * T);
    const double Tg = T * (double)d->car.ratio[d->gear];
    const double s = d->car.rear_share;
    const double axle_torque[2] = { (1.0 - s) * (double)d->car.final_front * Tg, s * (double)d->car.final_rear * Tg };
    static const int pair[2][2] = { { 0, 1 }, { 3, 2 } };     /* (a, b) per axle */
    for (int ax = 0; ax < 2; ax++) {
        const int a = pair[ax][0], b = pair[ax][1];
        double sa, sb;
        drv_axle_split(d->wheel[a].omega, d->wheel[b].omega, d->env.wheel[a].contact && d->env.wheel[b].contact, &sa, &sb);
        if (!d->env.wheel[a].locked) d->wheel[a].tau = (float)((double)d->wheel[a].tau + sa * axle_torque[ax]);
        if (!d->env.wheel[b].locked) d->wheel[b].tau = (float)((double)d->wheel[b].tau + sb * axle_torque[ax]);
    }
}

static double psi(double x) {                           /* S1-7.7 table */
    static const double xs[5] = { -10.0, -7.5, -3.5, -0.3, -0.05 };
    static const double ys[5] = { 1.0, 0.96, 0.925, 0.875, 0.0 };
    if (x <= xs[0]) return ys[0];
    if (x >= xs[4]) return ys[4];
    for (int i = 0; i < 4; i++)
        if (x <= xs[i + 1]) return ys[i] + (ys[i + 1] - ys[i]) * (x - xs[i]) / (xs[i + 1] - xs[i]);
    return ys[4];
}

/* S1-7.5: isolated current-state lock correction, before distribution. */
double drv_lock_correction(const Drivetrain *d) {
    const DrvCar *c = &d->car;
    const double Iw = DRV_WHEEL_INERTIA;
    const double Te = drv_engine_torque(d);
    const double ae = (Te + (double)d->feedback) / (double)c->engine_inertia;
    double beta[4];
    for (int i = 0; i < 4; i++)
        beta[i] = ((double)d->wheel[i].tau - (double)c->wheel_radius[i] * (double)d->wheel[i].force) / Iw;
    const double aw = (double)c->ratio[d->gear] * side_combine(d, beta, 1);
    const double rg = c->ratio[d->gear];
    double dlock = 1.0 / (double)c->engine_inertia;
    if (front_driven(d)) dlock += (1.0 - c->rear_share) / Iw * (rg * c->final_front) * (rg * c->final_front) * 0.5;
    if (rear_driven(d))  dlock += c->rear_share / Iw * (rg * c->final_rear) * (rg * c->final_rear) * 0.5;
    return (ae - aw) / dlock;
}

static double grip_multiplier(const Drivetrain *d,int i) {
    if(d->env.supplied_frame_grip)return d->env.frame_grip[i>1];
    double gamma=(double)d->tune.grip_q*(1-.025*(double)d->tune.grip_loc_x);
    return i>1?gamma*d->env.rear_grip_multiplier:gamma;
}
static double rear_speed_multiplier(const Drivetrain *d,int i) {
    if(d->tune.is_player && i>1 && !d->env.wheel[i].locked && d->wheel[i].omega>=0)
        return .85+.15*clampd(((double)d->env.body_speed-13.4108)*.11185,0,1);
    return 1;
}
void drv_prepare_tyre(Drivetrain *d,int i) {
    DrvWheel *w=&d->wheel[i];DrvWheelEnv *e=&d->env.wheel[i];
    const int axle=i>1;
    const double radius=d->car.wheel_radius[i],load=e->load;
    /* Pre-decision grip uses incoming lock/spin (S3-4). */
    const double mu=(double)e->mu_slide_surface*grip_multiplier(d,i)*d->car.mu_slide[axle]*rear_speed_multiplier(d,i);
    const double slip=radius*w->omega-e->vx,mag=hypot(slip,e->vy);
    if(d->brakes.enabled) {
        BrkInput in={.radius=(float)radius,.omega=w->omega,.vx=e->vx,.vy=e->vy,
            .load=e->load,.mu=mu,.service=d->brakes.frame.service[i],
            .handbrake=d->brakes.handbrake[i],.tau=w->tau,.locked=e->locked,
            .relief_disabled=d->brakes.relief_disabled};
        BrkResult out=brk_prepare(&in);
        w->tau=out.tau;w->omega=out.omega;e->locked=d->brakes.locked[i]=out.locked;
    }
    w->slip_speed=(float)slip;w->gripping=1;
    if(mag>0 && (e->vx!=0 || e->vy!=0)) {
        if(fabs(slip)>.5 || fabs((double)e->slip_angle)>d->car.theta_max[axle] || e->locked) {
            double force=w->w_long*slip*load*mu/mag;
            double lateral=-e->vy*load*mu/mag*w->w_lat;
            if((double)(d->env.planar_tyres?d->env.signed_forward:d->env.body_speed)*MPS_TO_MPH<1 && mu>.1){force/=mu;lateral/=mu;}
            w->force=(float)force;w->force_lat=(float)lateral;w->gripping=0;
        }
    }
    if(w->gripping) {
        w->force=(float)((double)w->w_long*w->tau/radius);
        w->force_lat=(float)((double)w->w_lat*e->lateral_grip_force);
    }
}

void drv_couple(Drivetrain *d, const DrvEnv *env, DrvBodyAccelFn body, void *body_ud) {
    const DrvCar *c = &d->car;
    const double Iw = DRV_WHEEL_INERTIA;
    d->env = *env;
    env=&d->env;
    if(d->brakes.enabled)for(int i=0;i<4;i++)d->env.wheel[i].locked=d->brakes.locked[i];
    d->contact_count = 0;
    for (int i = 0; i < 4; i++) if (env->wheel[i].contact) d->contact_count++;
    for (int i = 0; i < 4; i++) {            /* idempotent: normally already zero after the post-step */
        d->wheel[i].tau = 0.0f; d->wheel[i].force = 0.0f; d->wheel[i].force_lat = 0.0f;
    }

    const double wside = d->omega_w; /* S1-7.3: clutch post-step snapshot. */
    const double te0 = drv_engine_torque(d);

    /* S1-7.3 initial drive */
    if (d->mode == DRV_CLUTCH_DISENGAGING || d->mode == DRV_CLUTCH_ENGAGING)
        dist(d, drv_clutch_torque(c, d->omega_e, wside, d->engage, &d->delta_prev));
    else if (d->mode == DRV_CLUTCH_LOCKED)
        dist(d, te0);

    /* S1-7.4 plus S3-3/4: brakes, sampled slip, lock and tyre branch. */
    for(int i=0;i<4;i++)drv_prepare_tyre(d,i);

    /* S1-7.5 iterative pass */
    for (int pass = 0; pass < 5; pass++) {
        float A[4] = { 0, 0, 0, 0 }, F[4], Fy[4];
        for (int i = 0; i < 4; i++) { F[i] = d->wheel[i].force; Fy[i] = d->wheel[i].force_lat; }
        if (body) body(body_ud, F, Fy, A);
        for (int i = 0; i < 4; i++) {
            DrvWheel *w = &d->wheel[i];
            if (w->gripping && !env->wheel[i].locked) {
                const double R = c->wheel_radius[i];
                w->force = (float)((double)w->force + ((double)w->tau - R * (double)w->force) - (double)A[i] * Iw / R);
            }
        }
        if (d->mode == DRV_CLUTCH_LOCKED && d->gear != DRV_GEAR_N) {
            dist(d, drv_lock_correction(d));
        }
    }

    /* S1-7.6 friction limit */
    for (int i = 0; i < 4; i++) {
        DrvWheel *w = &d->wheel[i];
        const DrvWheelEnv *e = &env->wheel[i];
        const int ax = i > 1;
        const double Fmax = (double)e->mu_peak_surface * (double)c->mu_peak[ax] * grip_multiplier(d,i) * (double)e->load * rear_speed_multiplier(d,i) * (env->planar_tyres?e->peak_scale:1);
        const int boosted = w->tau > 0.0f && !e->locked;
        double F = (double)w->force, Fy = (double)w->force_lat;
        double Ft = boosted ? 1.5 * F : F;
        double mag = hypot(Ft, Fy);
        if (mag > Fmax && mag > 0.001) {
            double k = Fmax / mag;
            F = Ft * k; Fy *= k;            /* the 1.5 factor is kept when scaling happened */
        }
        if (fabs((double)e->vx) > 1.0) F -= (double).15f * sin((double)e->slip_angle) * Fy;
        w->force = (float)F; w->force_lat = (float)Fy;
        d->force_long[i] = w->force; d->force_lat[i] = w->force_lat; d->load[i] = e->load;
        /* Tire-marks TM-3 utilisation: M (as tested, incl. the 1.5 factor, before
         * scaling) against Fmax; sliding branch or locked = 1; no contact = 0. */
        w->utilisation = tm_utilisation(mag, Fmax, !w->gripping || e->locked, e->contact);
    }
    d->utilisation_mean = .25f * (d->wheel[0].utilisation + d->wheel[1].utilisation
                                  + d->wheel[2].utilisation + d->wheel[3].utilisation);

    /* S1-7.7 finishing */
    if (d->gear != DRV_GEAR_N && d->mode == DRV_CLUTCH_LOCKED && d->contact_count > 0) {
        double sl[4], ws[4];
        for (int i = 0; i < 4; i++) { sl[i] = d->wheel[i].slip_speed; ws[i] = d->wheel[i].omega; }
        const double sbar = side_combine(d, sl, 0);
        const double wold = (double)c->ratio[d->gear] * side_combine(d, ws, 1);
        const double dd = wold - (double)d->omega_e;
        const double Te = drv_engine_torque(d);
        if (d->throttle <= 0.2f || sbar <= 0.1) {
            d->feedback = (float)((double)d->feedback + clampd((double)c->clutch_k * dd, -fabs(Te), fabs(Te)));
        } else {
            d->omega_w = d->omega_e;
            const int driven[2] = { front_driven(d), rear_driven(d) };
            for (int ax = 0; ax < 2; ax++) {
                if (!driven[ax]) continue;
                const int a = ax ? 3 : 0, b = ax ? 2 : 1;
                const double fin = ax ? c->final_rear : c->final_front;
                const double dw = ((double)d->omega_e - wold) / ((double)c->ratio[d->gear] * fin);
                if ((double)env->body_speed * MPS_TO_MPH < 40.0 || (!env->wheel[a].contact && !env->wheel[b].contact)) {
                    float m = (float)(((double)d->wheel[a].omega + (double)d->wheel[b].omega) * 0.5);
                    d->wheel[a].omega = m; d->wheel[b].omega = m;
                }
                d->wheel[a].omega = (float)((double)d->wheel[a].omega + dw);
                d->wheel[b].omega = (float)((double)d->wheel[b].omega + dw);
            }
        }
    }
    if (d->gear == 2) {                   /* first gear, any clutch mode (S1-7.7) */
        const double Te = drv_engine_torque(d);
        const double phi = d->feedback;
        if (phi < Te && Te + phi < 0.0) {
            /* kappa_C = K[C] from the class copy taken at creation (S1-12);
             * C = -1 selects the preceding 0.25 constant (S1-12.5). */
            const double kc = drv_kappa_c(d->perf_class);
            d->feedback = (float)(phi + psi(Te + phi) * kc * (Te + phi));
        }
    }
}
