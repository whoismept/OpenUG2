/* abs.c -- see abs.h. */
#include "abs.h"
#include <math.h>

#define TWO_PI 6.28318530718f

/* Below this the aid does nothing at all -- about 11 km/h, where road cars
 * drop out too. Manoeuvring, and spinning the car on purpose, stay entirely
 * the driver's business. */
#define ABS_CUTIN_MS 3.0f

void abs_init(AbsUnit *u, int enabled)
{
    u->on = enabled;
    u->reserve = 0.18f;      /* keep ~a fifth of the tyre for steering */
    u->cycle_hz = 12.0f;     /* road cars pulse around 10-15 Hz */
    u->phase = 0.0f;
    u->active = 0.0f;
}

float abs_limit(AbsUnit *u, float demand, float grip_max, float speed_ms, float dt)
{
    if (grip_max <= 1.0f) { u->active = 0.0f; return 0.0f; }

    if (!u->on || speed_ms < ABS_CUTIN_MS) {
        /* No aid: the tyre simply saturates, and once it does it is sliding,
         * which is what costs the steering. */
        u->active = 0.0f;
        return demand < -grip_max ? -grip_max
             : (demand > grip_max ? grip_max : demand);
    }

    float ceiling = grip_max * (1.0f - u->reserve);
    float mag = fabsf(demand);
    if (mag <= ceiling) {
        u->active = 0.0f;
        return demand;
    }

    /* Over the limit: this is where a real unit would start releasing. Pulse
     * around the ceiling so the deceleration ripples the way ABS does rather
     * than sitting on a flat clamp -- the player feels the aid working. */
    u->phase += TWO_PI * u->cycle_hz * dt;
    if (u->phase > TWO_PI) u->phase -= TWO_PI;
    float ripple = 1.0f - 0.08f * (0.5f + 0.5f * sinf(u->phase));
    u->active = 1.0f;
    float held = ceiling * ripple;
    return demand < 0.0f ? -held : held;
}

/* ---- stability assist; see abs.h ---------------------------------------- */

void esp_init(EspUnit *e, int enabled)
{
    e->on = enabled;
    e->free_deg = 22.0f;    /* a deliberate drift lives below this */
    e->full_deg = 55.0f;    /* by here the car is leaving, not drifting */
    e->authority = 0.45f;   /* it may use up to this share of the grip */
    e->active = 0.0f;
}

float esp_assist(EspUnit *e, float slip_deg, float yaw_rate,
                 float grip_moment, float *throttle_scale)
{
    if (throttle_scale) *throttle_scale = 1.0f;
    if (!e->on) { e->active = 0.0f; return 0.0f; }

    float mag = fabsf(slip_deg);
    if (mag <= e->free_deg) { e->active = 0.0f; return 0.0f; }

    float t = (mag - e->free_deg) / (e->full_deg - e->free_deg);
    if (t > 1.0f) t = 1.0f;
    t = t * t;                       /* ease in, so it never snaps on */
    e->active = t;

    /* Cut power as well: most of a slide this big is the driven axle being
     * asked for more than it has. */
    if (throttle_scale) *throttle_scale = 1.0f - 0.55f * t;

    /* Oppose the rotation that is taking the car around. */
    /* Faded through the zero crossing rather than switched, so it cannot
     * chatter frame to frame while the car is barely rotating. */
    float m = e->authority * t * grip_moment;
    return -m * tanhf(yaw_rate * 4.0f);
}
