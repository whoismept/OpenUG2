/* abs.h -- anti-lock braking, as a module the dynamics calls into.
 *
 * A locked wheel is the worst of both worlds: it gives up most of its
 * braking AND all of its steering, because a tyre sliding straight ahead has
 * nothing left for sideways. That is why a car with the brake buried ploughs
 * on into the corner no matter where the wheel is pointed.
 *
 * Real ABS senses the wheel decelerating faster than the car and releases
 * until it spins up again, cycling several times a second. We do not carry
 * per-wheel spin states, so this models the outcome rather than the
 * mechanism: hold the demanded braking force just under what the tyre can
 * take, keep a slice of grip in reserve for steering, and report the
 * modulation so the HUD and the sound can pulse with it.
 *
 * Kept separate from the tyre model deliberately: it is a driver aid, it can
 * be switched off per car or per difficulty, and a car without it should
 * behave like a car without it.
 */
#ifndef ABS_H
#define ABS_H

typedef struct {
    int   on;            /* 0 disables the aid entirely */
    float reserve;       /* fraction of grip kept back for steering, 0..0.5 */
    float cycle_hz;      /* how fast it pulses, for feel and for the HUD */
    /* live state */
    float phase;
    float active;        /* 0..1, how hard it is working right now */
} AbsUnit;

void  abs_init(AbsUnit *u, int enabled);

/* Limit one axle's braking force.
 *   demand   the force the pedal is asking for, N (negative = braking)
 *   grip_max what the tyre can pass at this load, N
 * Returns the force to apply. Updates the unit's activity for the HUD. */
/* speed_ms lets the unit stand down where it has no business acting: below a
 * crawl there is nothing to lose by locking a wheel, and a car being spun on
 * the spot deliberately should not have an aid fighting it. Real units cut
 * out around walking pace for the same reason. */
float abs_limit(AbsUnit *u, float demand, float grip_max, float speed_ms, float dt);

/* ---- stability assist ---------------------------------------------------
 * The other half of the aid: when the car is sliding further than the driver
 * can plausibly have meant, help it back. A real system does this by braking
 * individual wheels; with one wheel per axle we apply the equivalent yaw
 * moment directly and trim the throttle, which is what those brake pulses
 * amount to.
 *
 * It stays out of the way below `free_deg` of slip, so ordinary cornering
 * and a deliberate drift are untouched -- the aid only appears once the slide
 * is past what you would hold on purpose, and it fades in rather than
 * snapping, so it never feels like the car was taken away.
 *
 *   slip_deg   current body slip angle, degrees
 *   yaw_rate   rad/s, signed
 *   grip_moment  the moment a fully gripping axle pair could make, N*m
 * Returns the correcting yaw moment in N*m; *throttle_scale gets the factor
 * to apply to drive force (1 = untouched). */
typedef struct {
    int   on;
    float free_deg;      /* slip angle below which it does nothing */
    float full_deg;      /* slip angle at which it works at full strength */
    float authority;     /* fraction of available grip it may use */
    float active;        /* 0..1, for the HUD */
} EspUnit;

void  esp_init(EspUnit *e, int enabled);
float esp_assist(EspUnit *e, float slip_deg, float yaw_rate,
                 float grip_moment, float *throttle_scale);

#endif
