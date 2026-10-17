/* drivetrain.h — OpenUG2 drivetrain / longitudinal acceleration (Stage 1).
 *
 * Standalone pure-maths module: no GL, no SDL, no file IO, no dependency on
 * physics.c or nfsu2.h. Implements docs/cleanroom-driving-physics/SPEC.md
 * Stage 1 (engine, clutch, gearbox, driver-input shaping, drivetrain/tyre
 * coupling, wheel spin). Design notes: docs/cleanroom-driving-physics/IMPL_DESIGN.md.
 *
 * Units (SPEC S1-0.1): tonnes, kN, kN*m, m, s, rad/s, t*m^2. Gear index
 * (S1-0.4): 0 reverse, 1 neutral, 2..n+1 forward gears 1..n. Wheel index
 * (S1-0.5): 0,1 front; 2,3 rear.
 *
 * Calling pattern for one rendered frame (S1-2):
 *     drv_frame_begin(&d, &in, frame_dt, -1);          // S1-5, S1-4.5
 *     n = drv_substep_count(frame_dt, &h);
 *     for each substep {
 *         drv_integrate(&d, h);                        // 4a,4b + wheel/engine/clutch post-steps
 *         ... caller integrates its body with d.force_long/force_lat, refreshes ground contact ...
 *         drv_couple(&d, &env, body_fn, body_ud);      // 4e: fills torques/forces for next substep
 *     }
 * drv_init() performs neutral creation post-processing (S1-2.5). Call
 * drv_couple() after initialization with the caller-supplied environment. The body
 * integrator and the normal loads belong to later stages; they enter here only
 * through DrvEnv and DrvBodyAccelFn.
 *
 * Every persistent state/parameter is binary32 (S1-0.3); intermediates are
 * double. Spec-unknown values are explicit fields of DrvTune. */
#ifndef OPENUG2_DRIVETRAIN_H
#define OPENUG2_DRIVETRAIN_H
#include "braking.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants (SPEC S1-0.2, S1-2.2, S1-1.5) ---------------------------- */
#define DRV_KR            9.549304f     /* rpm = omega * Kr   (0x4118C9F3) */
#define DRV_KW            0.10471967f   /* omega = rpm * Kw   (0x3DD67744) */
#define DRV_SUBSTEP_MAX   0.0044f       /* 0x3B902DE0 */
#define DRV_FRAME_QUANTUM (1.0 / 1800.0)
#define DRV_WHEEL_INERTIA 0.01f         /* Iw, t*m^2 */
#define DRV_MAX_GEAR_SLOTS 8            /* ratio[] slots: R, N, 1..6 */
#define DRV_MAX_FORWARD   6
#define DRV_TABLE_SLOTS   9             /* indexed by gear index 0..8 */
#define DRV_TORQUE_SAMPLES 9
#define DRV_BRAKE_SAMPLES  3

enum { DRV_GEAR_R = 0, DRV_GEAR_N = 1 };
enum { DRV_CLUTCH_DISENGAGING = 0, DRV_CLUTCH_OPEN = 1, DRV_CLUTCH_ENGAGING = 2, DRV_CLUTCH_LOCKED = 3 };
enum { DRV_CLASS_A = 0, DRV_CLASS_B = 1, DRV_CLASS_C = 2 };
/* Performance class C (S1-12): 0 = highest performance ... 3 = lowest. The
 * physics-parameter object stores it as a signed byte; -1 = not yet computed.
 * The drivetrain block's own created copy defaults to 3 before the
 * creation-time copy overwrites it. */
enum { DRV_PERF_CLASS_UNSET = -1, DRV_PERF_CLASS_DEFAULT = 3 };
/* gear command (kappa, S1-4.5) */
enum { DRV_CMD_MANUAL_ASSIST = -2, DRV_CMD_AUTO = -1 /* >= 0: direct gear index */ };
enum { DRV_DIR_BACKWARD = -1, DRV_DIR_STOPPED = 0, DRV_DIR_FORWARD = 1 };

/* ---- per-car record (SPEC S1-1.2) ---------------------------------------- */
typedef struct DrvCar {
    /* fields read from the record; fill, then call drv_car_finalize() */
    float mass;                       /* m   (t)  row+0x220 */
    float wheel_radius[4];            /* R_i (m) */
    float theta_max[2];               /* axle slip-angle threshold (rad), a = 0 front, 1 rear */
    float mu_peak[2];                 /* axle peak-grip multiplier */
    float mu_slide[2];                /* axle sliding-grip multiplier */
    float launch_fraction;            /* lambda  row+0x2C0 */
    float final_front, final_rear;    /* Ff, Fr */
    float rear_share;                 /* s, 0 = FWD, 1 = RWD */
    float clutch_k;                   /* k (kN*m per rad/s) */
    int   gear_count;                 /* n, 1..6 */
    float clutch_rate;                /* cr */
    float ratio[DRV_MAX_GEAR_SLOTS];  /* r[j], j = 0 R, 1 N, 2..7 forward 1..6 */
    float idle_rpm, shift_rpm, max_rpm; /* eps0, eps1, eps2 */
    float engine_inertia;             /* Ie */
    float torque_primary[DRV_TORQUE_SAMPLES];   /* Tp */
    float torque_secondary[DRV_TORQUE_SAMPLES]; /* Ts (zero when absent) */
    float engine_brake[DRV_BRAKE_SAMPLES];      /* Tb */

    /* derived by drv_car_finalize() (S1-1.3, S1-1.4, S1-4.2) */
    float torque[DRV_TORQUE_SAMPLES]; /* Tq = Tp + Ts (binary32 sum) */
    int   drive_class;                /* DRV_CLASS_A/B/C */
    float launch_scale;               /* sigma_L */
    float up_rpm[DRV_TABLE_SLOTS];    /* U[g]; U[1] launch rpm, U[2..n], U[n+1]; others 0 */
    float down_rpm[DRV_TABLE_SLOTS];  /* D[g]; D[3..n+1]; others 0 */
} DrvCar;

/* Returns 0 on success, -1 if the record is unusable (gear_count outside 1..6,
 * non-positive inertia/mass/radius, eps2 <= eps0). */
int  drv_car_finalize(DrvCar *car);
int  drv_drive_class(float rear_share);                 /* S1-1.4 */
float drv_launch_scale(int drive_class);                /* S1-1.4 */
/* S1-12.3: performance class 0..3 from the stored binary32 drive score S,
 * thresholds applied in descending order with strict >. */
int    drv_perf_class_from_score(float score);
/* S1-12.1 + S1-12.3: a nonzero forced flag yields class 3 without forming a
 * score; otherwise the score mapping above. */
int    drv_perf_class_resolve(int forced, float score);
/* S1-7.7 / S1-12.5: the unguarded K[C] read. The four entries are the stored
 * binary32 words 0x00000000, 0xBE4CCCCD, 0xBECCCCCD, 0xBF333333 (shorthand
 * 0.0, -0.2, -0.4, -0.7), contiguous in C order, and the coefficient returned
 * is each exact stored value (not a double built from the decimal literal).
 * The unset sentinel C = -1 selects the 4-byte slot immediately preceding
 * K[0], whose stored binary32 value is 0.25 (0x3E800000); it is not K[0] and
 * not a no-op. The class scheme produces only -1 and 0..3 (U-17), so any other
 * index is outside the stated read and returns NAN. */
double drv_kappa_c(int perf_class);
/* rpm of sample i of a q-sample curve spread over [a, b] (S1-3.1) */
float drv_curve_knot_rpm(float a, float b, int q, int i);

/* ---- explicit tunables for spec unknowns ---------------------------------
 * Defaults: drv_tune_default(). Each field cites the spec item it stands for. */
typedef struct DrvTune {
    float torque_scale;     /* G  (S1-3.3, U-7): initial 1.0 */
    int   torque_bonus;     /* B = 1.05 when set (S1-3.3, U-6): default 0 */
    /* C (S1-12): per-vehicle-instance signed byte of the physics-parameter
     * object, 0..3 or DRV_PERF_CLASS_UNSET (-1). Derived by the parameter-
     * composition step (out of Stage-1 scope, U-11); drv_init copies it into
     * the drivetrain block. drv_tune_default() leaves it unset. */
    signed char perf_class;
    float initial_rpm;      /* engine rpm at creation (U-15): <= 0 means idle */
    float grip_q;           /* q (S1-7.8, U-4): 1.0 */
    float grip_loc_x;       /* X (S1-7.8, U-4): 0 (feature switch off) */
    int   is_player;        /* human-driven car: enables k_v on rear wheels (S1-7.8) */
    float rate_scale;       /* e (S1-4.1): 1.0 outside deferred race modes */
    /* Shift-point jitter (S1-4.3, U-3). rand01 returns a uniform value in [0,1)
     * or NULL: jitter suppressed (all offsets 0). Offset = -100 + 100*u. */
    float (*rand01)(void *ud);
    void  *rand_ud;
    /* Test-adapter clamp only (U-5/Q-16). 0 disables a bound. */
    float frame_dt_min, frame_dt_max;
} DrvTune;
void drv_tune_default(DrvTune *t);

/* ---- driver input and environment ---------------------------------------- */
typedef struct DrvInput {
    float throttle, brake, handbrake;   /* raw pedals t0, b0, hb0 in 0..1 */
    int   gear_cmd;                     /* kappa: -1 auto, -2 manual-assist, >= 0 direct gear index (U-1) */
} DrvInput;

typedef struct DrvWheelEnv {
    int   contact;          /* wheel touches the ground */
    float load;             /* N_i (kN), later stage */
    float vx, vy;           /* contact-frame velocity along / across the wheel heading (m/s) */
    float slip_angle;       /* rad (Stage 4 input) */
    float mu_peak_surface;  /* muS,pk  (1.0 on ordinary road) */
    float mu_slide_surface; /* muS,sl  (1.0) */
    float roll_drag;        /* rho_S, surface rolling-drag factor (1.0) */
    int   locked;           /* Stage1 supplied input; Stage3 current-lock mirror */
    int   drag_flag;        /* zeta_i (0/1) */
    float peak_scale;      /* S4-6 effective scale; initialized to 1 by drv_env_default */
    float lateral_grip_force; /* gripping-branch lateral force before wT (Stage 4); default 0 */
} DrvWheelEnv;

typedef struct DrvEnv {
    DrvWheelEnv wheel[4];
    float body_speed;           /* magnitude, m/s */
    float signed_forward;      /* S4-5 low-speed normalization snapshot */
    float frame_grip[2];        /* S5 supplied complete front/rear frame grip */
    int supplied_frame_grip;
    int planar_tyres;          /* enables supplied Stage4 signed speed/peak_scale */
    float rear_grip_multiplier; /* Full rear multiplier supplied by Stage 4/5 (S1-11/Q-11); no retail default. */
} DrvEnv;
/* Synthetic level/straight/no-handbrake environment: rear multiplier 1,
 * four contacts, surface factors 1, other fields 0. Supply actual loads and
 * full rear multiplier before use outside that explicitly restricted case. */
void drv_env_default(DrvEnv *e);

/* Provisional acceleration input A[i] (BC-2). F/Fy are the current per-wheel
 * forces at a shared pass-start boundary. */
typedef void (*DrvBodyAccelFn)(void *ud, const float F[4], const float Fy[4], float A[4]);

/* Straight-line symmetric body (S1-8.2): A_i = (Ex + sum F_i) / m. */
typedef struct DrvBodyStraight { float mass; float ex; } DrvBodyStraight;
void drv_body_accel_straight(void *ud, const float F[4], const float Fy[4], float A[4]);

/* Reduced local planar body (BC-1/2). pos = frame-held local mount arm x/y;
 * head/lat = local steered directions. ex/ey snapshot transpose(B)*Fprepared
 * before all five passes. Composed BR callers supply the selected inverse local
 * yaw inertia; isolated fixtures may explicitly supply their scalar. */
typedef struct DrvBodyPlanar {
    float mass, inv_iz;
    float pos[4][2], head[4][2], lat[4][2];
    float ex, ey, external_moment; /* legacy fixture input, excluded by BC-2 */
} DrvBodyPlanar;
/* Diagnostic pass-start (ax,ay,alpha); no accumulation or duration factor. */
void drv_body_planar_acceleration(const DrvBodyPlanar *body,const float F[4],const float Fy[4],float out[3]);
void drv_body_accel_planar(void *ud, const float F[4], const float Fy[4], float A[4]);

/* ---- state ---------------------------------------------------------------- */
typedef struct DrvWheel {
    float phi, omega;           /* angle, spin */
    float tau;                  /* drive torque accumulated this coupling */
    float force, force_lat;     /* F_i, Fy_i */
    float slip_speed;           /* s_i */
    float tau_smooth;           /* smoothed net torque */
    float w_long, w_lat;        /* wL, wT multipliers (1.0) */
    int   gripping;             /* result of S1-7.4 branch */
    int   dir;                  /* DRV_DIR_* */
    float utilisation;          /* tire-marks TM-3 u at the S1-7.6 cap (0..1); output only */
} DrvWheel;

typedef struct Drivetrain {
    DrvCar  car;                /* copy; finalized */
    DrvTune tune;
    signed char perf_class;     /* C copy taken at creation (S1-12); may be DRV_PERF_CLASS_UNSET */
    DrvWheel wheel[4];

    /* engine (S1-3) */
    float omega_e;              /* engine speed */
    float throttle_mult;        /* mc */
    int   limiter;              /* Lambda */
    float feedback;             /* Phi */
    float throttle;             /* shaped t */
    float brake, handbrake;     /* shaped (S1-5), for later stages */

    /* clutch / gearbox (S1-4) */
    int   gear, target_gear;
    int   mode;                 /* DRV_CLUTCH_* */
    float engage;               /* c */
    int   pending_shift;        /* P */
    float omega_w;              /* wheel-side speed, refreshed by clutch post-step and by coupling */
    float delta_prev;           /* stored rpm delta (S1-7.3) */
    float jitter[3];            /* rho1..rho3 */
    int   sel_gear;             /* driver-selected gear gs; -1 = automatic marker */
    int   report_auto;          /* manual-assist selector reports "automatic" to the driver (output, per frame) */
    float manual_hold_time;     /* elapsed assist time, advanced on every -2 invocation */
    int   manual_last_gear;     /* remembered pre-change g (S1-4.5), supplied initial value */
    int   assist_state_supplied; /* U-2: -2 decisions disabled until drv_assist_supply() */

    /* Stage3 is explicit opt-in. Initial locks and relief/exclusivity switches
     * must be supplied; disabled preserves the Stage1 brake-free boundary. */
    struct {
        int enabled,exclusivity,relief_disabled,locked[4];
        int signed_distribution; /* S5 composition only; direct S3 fixtures unchanged */
        BrkCar car;
        BrkFrame frame;
        float handbrake[4]; /* injected H, not an inferred eta producer (S3-8) */
    } brakes;

    /* last coupling results (outputs for the body integrator) */
    DrvEnv env;
    float  force_long[4], force_lat[4], load[4];
    float  utilisation_mean;    /* four-wheel mean of DrvWheel.utilisation (TM-12) */
    int    contact_count;       /* nc */
    float  engine_torque;       /* Te used by the last derivative */
} Drivetrain;

/* Create a drivetrain at rest, in neutral, clutch Engaging with c = 0 (S1-4.1).
 * The car is copied and finalized. Creation zero-time post-processing yields
 * mc=0.7 and Phi=0. Manual-assist initial state remains caller-supplied (U-2).
 * The physics-parameter performance class is copied into d->perf_class at
 * creation (S1-12); it is never recomputed by this module (U-11 residual).
 * Returns 0 or -1 (bad car). */
int  drv_init(Drivetrain *d, const DrvCar *car, const DrvTune *tune);

/* ---- stateless helpers (exposed for the test vectors) ----------------------- */
/* TEST ADAPTER ONLY (U-5/Q-16): clamp supplied duration, then independently
 * round to 1/1800 s. No residual clock; does not model retail frame timing. */
float drv_quantize_frame_dt_test_adapter(const DrvTune *tune, double seconds);
/* S1-2.2: N = trunc(dt/0.0044)+1, h = dt/N. Returns N (h may be NULL). */
int   drv_substep_count(float frame_dt, float *h);
/* S1-3.1 curve lookup. */
double drv_curve(const float *samples, int q, double a, double b, double x);
/* S1-3.3 Te for explicit rpm and pedal-side u = mc*t (u is ignored (0) when the limiter is on). */
double drv_engine_torque_at(const DrvCar *car, const DrvTune *tune, int gear, int limiter, double rpm, double u);
/* S1-7.3 clutch slip torque k'*c*Delta; updates *delta_prev. */
double drv_clutch_torque(const DrvCar *car, double omega_e, double omega_w, double engage, float *delta_prev);
/* S1-7.2 step 3: shares of the axle torque for wheels a and b. */
void drv_axle_split(double omega_a, double omega_b, int both_contact, double *share_a, double *share_b);

/* S1-7.5: correction from current forces/torques; does not distribute it. */
double drv_lock_correction(const Drivetrain *d);

/* S2-4 / S1-8.1: unsmoothed wheel resistance, including airborne wheels. */
double drv_wheel_resistance(float omega,float surface,int damage);

/* S3-0/U3-2/U3-3: enable with explicit initial lock mask and switches.
 * H remains zero unless a later-stage caller injects nonnegative per-wheel H. */
int drv_brakes_supply(Drivetrain *d,const BrkCar *car,unsigned initial_lock_mask,
                     int relief_disabled,int exclusivity);
/* Isolated preparation/derivative boundaries, also used by the coupled solver. */
void drv_prepare_tyre(Drivetrain *d,int wheel);
double drv_wheel_derivative(Drivetrain *d,int wheel,float h);

/* ---- per-frame and per-substep operations ---------------------------------- */
/* S1-4.5 / U-2: supply initial selected gear, elapsed seconds and remembered
 * pre-change gear. Selected may be neutral or 2..top; remembered is 0..top.
 * Then accept forward selections through drv_assist_select(). Both return
 * -1 for inputs outside this API contract, leaving state unchanged. */
int drv_assist_supply(Drivetrain *d, int selected, float elapsed, int remembered);
int drv_assist_select(Drivetrain *d, int selected);
/* S1-5 input shaping + S1-4.5 gear selector. rpm_override >= 0 replaces the engine rpm
 * used by the selector (U-10). frame_dt is used for the manual-assist timers. */
void drv_frame_begin(Drivetrain *d, const DrvInput *in, float frame_dt, double rpm_override);
/* S1-2.4 a, b, c: derivatives, explicit Euler, wheel/engine/clutch post-steps. */
void drv_integrate(Drivetrain *d, float h);
/* S3-7: optional correction sweep, with no derivative or Euler integration. */
void drv_post(Drivetrain *d);
/* S1-2.4 e / S1-7: coupling. Fills force_long, force_lat, load, torques, Phi. */
void drv_couple(Drivetrain *d, const DrvEnv *env, DrvBodyAccelFn body, void *body_ud);

/* Current engine shaft torque Te (S1-3.3) from the live state. */
double drv_engine_torque(const Drivetrain *d);
/* Wheel-side speed (S1-7.1) for the current gear, from the live wheel spins. */
double drv_wheel_side_speed(const Drivetrain *d);

#ifdef __cplusplus
}
#endif
#endif
