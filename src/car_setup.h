/* car_setup.h -- per-car parameters read out of the shipped global data.
 *
 * The game ships a table of vehicle records, one per car, holding mass,
 * wheel placement, tyre grip, spring rates, gear ratios, the engine torque
 * curve, aerodynamics, brakes and the speed limiter.
 *
 * These are not hidden numbers. The garage's Dyno screen prints them for the
 * player: for the starter Peugeot 106 it reports a peak of 106.2 ft-lbs at
 * 3150 rpm and 119.8 bhp at 6250 rpm, and the record in the table gives
 * 144.0 N*m -- 106.2 ft-lbs -- peaking at the same point, with 89.3 kW,
 * that is 119.8 bhp. Reading the table programmatically shows exactly what
 * the game already shows in the garage.
 *
 * Units are the ones the file stores: mass in tonnes, force in kN, torque in
 * kN*m, metres, seconds, degrees.
 */
#ifndef CAR_SETUP_H
#define CAR_SETUP_H

#include <stdint.h>

typedef struct {                 /* one per wheel */
    float pos[3];                /*   position in body axes */
    float radius;
    float width;
} N2WheelSpec;

typedef struct {                 /* one per axle */
    float slip_angle_deg;        /*   slip angle at which grip breaks away */
    float ref_load;              /*   fixed reference load, kN per wheel: the
                                      load the quoted grip belongs to. Grip is
                                      sub-linear in load -- an axle pressed
                                      past this reference saturates, a lighter
                                      one keeps relatively more -- and this is
                                      the pivot of that curve (1.0 across the
                                      shipped fleet) */
    float lateral_scale;         /*   multiplier on the lateral force */
    float mu_static;             /*   gripping */
    float mu_slide;              /*   sliding; the gap between the two is
                                      what makes a slide a slide */
} N2TyreSpec;

typedef struct {                 /* one per axle */
    float progression;
    float stiffness;
    float damp_bump;
    float damp_rebound;
    float antiroll;
    float travel_up;             /*   bump stop */
    float travel_down;           /*   full rebound */
} N2StrutSpec;

typedef struct {
    float unused0;
    float service;
    float handbrake;
    float bias;                  /*   share going to the rear axle */
} N2BrakeSpec;

typedef struct {
    float idle_rpm;
    float shift_rpm;
    float max_rpm;
    float crank_inertia;
    float torque[9];             /*   curve, evenly spaced idle..max */
    float engine_brake[3];
} N2MotorSpec;

typedef struct {
    float shift_frac;
    float final_front;
    float final_rear;
    float split_rear;            /*   0 = FWD, 1 = RWD, 0.5 = AWD */
    float clutch_k;
    int   num_gears;
    float clutch_rate;
    float gear[9];               /*   [0] reverse, [1] neutral, [2..] first up */
} N2DriveSpec;

typedef struct {
    float unused_drag;
    float downforce_front;
    float downforce_rear;
    float scale;
} N2AeroSpec;

typedef struct {
    float com[3];                /* centre of mass in body axes; Z is its
                                    height above the contact patch -- the
                                    game's own load-transfer lever arm */
    float mass;                  /* tonnes */
    float dims[3];               /* L, W, H */
    float inertia[3];            /* diagonal of the inertia tensor */
    N2WheelSpec wheel[4];        /* 0 front-left, 1 front-right, 2/3 rear */
    N2TyreSpec  tyre[2];         /* 0 front, 1 rear */
    N2StrutSpec strut[2];
    N2BrakeSpec brake;
    N2MotorSpec motor;
    N2DriveSpec drive;
    N2AeroSpec  aero;
    float yaw_torque_clamp;      /* kN*m */
    float speed_limit_mph;       /* 0 = no limiter */
} N2CarSetup;

/* Read one car's record out of the global data buffer, by name (case
 * insensitive, e.g. "350Z", "PEUGOT106"). Returns 1 when the car was found
 * and the record passed the sanity gate, 0 otherwise; on 0 the output is
 * untouched. */
int n2_car_setup_load(N2CarSetup *out, const unsigned char *data, long len,
                      const char *car_name);

/* Walk the table and hand every car's name to cb. Returns the number of
 * records visited, or 0 when the table is missing. */
int n2_car_setup_list(const unsigned char *data, long len,
                      void (*cb)(const char *name, void *ud), void *ud);

#endif
