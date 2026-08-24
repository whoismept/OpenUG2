/* vehicle_model.h -- arcade car dynamics driven by the shipped parameters.
 *
 * THE MODEL. A single-track ("bicycle") body carries forward speed, lateral
 * speed and yaw rate; around it sit the states a slide actually needs: the
 * driven axle's own spin, the crank's own speed, the clutch between them,
 * and the body's pitch and roll on its springs. A kinematic car -- position,
 * heading, one scalar speed -- cannot point one way while travelling
 * another, which is the definition of a slide; these states are exactly
 * what make one representable.
 *
 * Tyre forces use Pacejka's Magic Formula in its short form,
 *
 *     F = D * sin(C * atan(B * s))
 *
 * (Pacejka & Bakker, "The Magic Formula Tyre Model", Delft 1991), applied
 * twice: against slip angle for cornering, against slip ratio for drive and
 * braking, one friction budget shared between them on the driven axle. The
 * shipped tyre record fills the formula in exactly, nothing invented:
 *
 *   D = mu_static * load                       the peak
 *   C  from  sin(C*pi/2) = mu_slide/mu_static  the tail it falls away to
 *   B  from  B*peak = tan(pi/(2C))             puts the peak where the record
 *                                              says (slip_angle_deg / 12%)
 *
 * Two independent clean-room implementations derived the same construction
 * from the same three numbers, and the C values it yields (1.22-1.36) are
 * where measured passenger-car data sits. Drift is not a mode anywhere in
 * this file: past the peak the rear makes less force, and the car rotates.
 *
 * WHERE EVERY NUMBER COMES FROM -- the full provenance, in order of rank:
 *
 *  1. The car table in GLOBALB.BUN (car_setup.h): mass, dims, inertia,
 *     wheel positions/radii/widths, tyre mu pair + breakaway angle, spring
 *     rates + dampers + antiroll + travel, gear ratios + finals + drive
 *     split + clutch, the nine-point torque curve + rev points, downforce,
 *     brakes, yaw clamp, limiter. These are PUBLIC numbers: the garage dyno
 *     prints them to the player (106.2 ft-lbs @3150 for the starter Peugeot
 *     matches the record to the decimal).
 *  2. Textbook constants, named at their definitions: air density 1.225,
 *     rolling resistance 0.015, driveline efficiency 0.90, longitudinal
 *     slip peak 12%, gravity. Standard figures, not tuned.
 *  3. Derived at load time and checked by tools/veh_test against the game's
 *     own dyno screen (0-60 8.23 s / 0-100 23.66 s / top 141 mph for the
 *     Peugeot): drag area from body box and saloon Cd; Magic Formula B/C/D
 *     as above; pitch/roll stiffness from spring rate times lever arm.
 *  4. Judgement calls, each argued where it lives: steering lock 33 deg;
 *     rack rates; clutch capacity 3x peak torque. (The CoM height used to
 *     be one; it now comes from the record's own first twelve bytes, with
 *     0.35*height only as the fallback for a zeroed field.)
 *  5. Gameplay, deliberately NOT physics -- the driver's hands and feet,
 *     plus aids (abs.h): the doughnut governor (spin ceiling + steady
 *     revs), launch clutch, stability assist, ABS. All gated on intent
 *     (full throttle + full lock, or the handbrake) and inert otherwise.
 *
 * This file is the product of thirty-odd Opus iterations and a dozen Fable
 * ones: behavioural targets off the game's own dyno screen, parameters out
 * of its data files, physics out of the literature, and every dead end
 * documented where it died.
 */
#ifndef VEHICLE_MODEL_H
#define VEHICLE_MODEL_H

#include "car_setup.h"

typedef struct {
    float x, y, z;          /* world position, metres */
    float heading;          /* radians, anticlockwise from +X */
    float v_long, v_lat;    /* body axes, m/s: nose-ward and left-ward */
    float yaw_rate;         /* rad/s, anticlockwise */
    float rpm;
    int   gear;             /* 0 reverse, 1 neutral, 2 = first */
    float shift_timer;      /* seconds left with the clutch fully out */
    float clutch;           /* 0 disengaged .. 1 fully home */
    /* THE DRIVEN WHEELS SPIN AT THEIR OWN RATE. Without this the engine
       speed is derived from how fast the CAR is going, which says a spinning
       wheel is impossible: the revs sag exactly when they should be screaming
       and the drive force cannot hold steady through a slide. With it, wheel
       spin is a real state -- torque in, grip out, and whatever is left over
       accelerates the wheel. */
    float omega_drive;      /* rad/s at the driven axle */
    /* THE ENGINE SPINS ON ITS OWN. Tie the revs rigidly to the wheels and a
       standing start is stuck at idle, making idle torque -- so the car pulls
       away gently and can never light up its tyres. A real launch is the
       other way round: the engine runs up against nothing much, and when the
       clutch takes up it delivers far more than it makes in steady state,
       because the flywheel is dumping stored energy too. That is why a car
       spins its wheels off the line at all. */
    float omega_engine;     /* rad/s at the crank */
    int   launching;        /* clutch held out, revs building */
    int   engine_locked;    /* clutch home: crank and wheels are one body */
    float fy_drive_combined;/* driven axle's lateral force from combined slip */
    int   drive_axle;       /* which axle that was */
    float drive_slip;       /* its slip ratio, for the blend */
    float dbg_Fyf, dbg_Fyr, dbg_Mz, dbg_Nf, dbg_Nr;   /* last step's forces */
    /* Body on its springs: pitch positive nose-up, roll positive left-up.
       These are not decoration -- the load an axle carries is read off the
       spring deflection, so the car's weight arrives at the tyres late and
       overshoots, the way it does on a real car. Lifting off mid-corner
       unloads the rear a moment AFTER the throttle closes, which is exactly
       when the back steps out. */
    float pitch, pitch_rate;
    float roll,  roll_rate;
    /* Where the front wheels are actually pointed. The player's input is a
       request; the rack takes time to get there. */
    float delta;
} VehState;

typedef struct {
    float throttle;         /* 0..1 */
    float brake;            /* 0..1 */
    float steer;            /* -1..1, positive left */
    int   handbrake;
} VehInput;

/* Derived once per car so the step does not redo it every frame. */
typedef struct {
    float mass_kg, izz, ixx, iyy;
    float a, b, wheelbase;  /* CoM to front axle, to rear axle */
    float track;
    float h_cog;
    float r_axle[2];        /* rolling radius per axle -- the record gives the
                               rear wheels their own size on most cars */
    float r_drive;          /* what the gearing actually turns */
    float B[2], C[2], D_mu[2];   /* Magic Formula, per axle: 0 front 1 rear */
    float B_long[2];             /* same curve against slip ratio */
    float i_wheel;               /* driven axle rotational inertia, kg*m^2 */
    float mu_long[2];
    float ref_load[2];      /* load the quoted mu belongs to, per axle */
    float k_pitch, c_pitch; /* body on its springs, N*m per rad and per rad/s */
    float k_roll,  c_roll;
    float up_rpm[9];        /* change-up point per gear, from the torque curve */
    float cda;              /* drag area, m^2 */
    const N2CarSetup *set;
} VehModel;

void  veh_model_init(VehModel *m, const N2CarSetup *c);
void  veh_state_init(VehState *s);
void  veh_step(VehState *s, const VehModel *m, const VehInput *in, float dt);

/* Diagnostics a test or HUD wants; all derived, none stored. */
float veh_speed_ms(const VehState *s);
float veh_slip_angle_deg(const VehState *s);

/* ---- bridge into the existing game loop ---------------------------------
 * Same shape as the kinematic step it stands in for, including its units:
 * the game keeps velocity and speed in METRES PER TICK at 60 Hz, not in m/s,
 * so this converts on the way in and out. Position and heading are read back
 * from the caller every frame, so a teleport, a respawn or a collision
 * response applied outside this model is picked up rather than fought.
 *
 * throttle is signed the way the game already sends it: positive drives,
 * negative brakes. Returns the sideways slide in metres per tick, which is
 * what the caller feeds to tyre smoke and skid marks. */
float veh_bridge_step(float pos[3], float vel[2], float *heading, float *speed,
                      float throttle, float steer, int handbrake,
                      const VehModel *m);

/* State of the bridged car, for the HUD and for the body attitude. */
float veh_bridge_pitch(void);   /* radians, nose up positive */
float veh_bridge_roll(void);    /* radians, left side up positive */
float veh_bridge_slip_deg(void);
float veh_bridge_yaw_rate(void);   /* rad/s */
float veh_bridge_slip_ratio(void); /* driven-wheel slip ratio: >0 spinning up */
void  veh_bridge_reset(void);      /* forget the car, for tests */
/* The game resolved a collision this frame: the caller's velocity is the
 * truth now, whatever the size of the correction. */
void  veh_bridge_adopt(void);
float veh_bridge_rpm(void);
int   veh_bridge_gear(void);

#endif
