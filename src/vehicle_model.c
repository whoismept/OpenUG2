/* vehicle_model.c -- see vehicle_model.h for the model and its sources. */
#include "vehicle_model.h"
#include "abs.h"
#include <math.h>
#include <string.h>

#define G      9.80665f
#define RHO    1.225f       /* air, sea level */
#define PI     3.14159265358979f

/* Rolling resistance of a road tyre on tarmac; textbook value, not tuned. */
#define C_ROLL 0.015f

/* Longitudinal grip runs lower than lateral: a tyre put down its own contact
 * patch lengthwise behaves differently than sideways, and arcade handling
 * wants launches that are not instant. The value follows from the target
 * 0-60: at full mu_static the little Peugeot would reach 60 mph in 6.4 s
 * against the 8.23 s the game reports, and this factor is what closes it.
 * It is the ONE number here that is fitted rather than read or derived. */
#define MU_LONG_FRAC 0.85f

/* Driveline efficiency; conventional figure for a manual gearbox. */
#define ETA 0.90f

/* Steering: the parameter table carries no steering rack, so the lock is
 * ours. 33 degrees is the mechanical limit at a standstill; above that the
 * grip ceiling takes over -- see the steering block in veh_step. */
#define STEER_LOCK_DEG 33.0f
#define STEER_HEADROOM 1.35f   /* how far past the grip limit the player may go */

/* THE RACK HAS A SPEED. Nobody flicks a steering wheel from lock to lock
 * instantly, and a car that does feels like it is being yanked rather than
 * steered -- it also slams the yaw resonance every time the key goes down.
 * 140 deg/s at the road wheels takes about a quarter second to reach full
 * lock, which is a brisk but human flick of the wrists. Coming BACK is
 * quicker: the tyres' own self-aligning torque helps the wheel centre, so
 * releasing feels immediate even though turning in does not. */
#define STEER_RATE_OUT  2.45f   /* rad/s, winding lock on */
#define STEER_RATE_BACK 4.20f   /* rad/s, unwinding toward centre */

#define SHIFT_TIME 0.30f       /* clutch out, no drive */

/* Longitudinal grip against SLIP RATIO, (wheel speed - road speed) / road
 * speed. Same Magic Formula shape as the lateral curve and the same two mu
 * from the record, but it peaks at a slip ratio rather than an angle. Around
 * 12% is where a road tyre makes its most, which is the number every
 * traction-control system is built around. */
#define SLIP_PEAK 0.12f
/* Below this speed the slip ratio has no meaning (dividing by nothing), so
 * the denominator is floored here. */
#define SLIP_VFLOOR 3.0f

/* One unit per axle. They carry only their own pulse phase, so a single
 * shared pair is enough for the player's car. */
static AbsUnit g_abs_front, g_abs_rear;
static EspUnit g_esp;
static int g_abs_ready;

/* torque at rpm from the nine-point curve; used by init and by the step */
static float engine_torque_of(const N2CarSetup *c, float rpm)
{
    float lo = c->motor.idle_rpm, hi = c->motor.max_rpm;
    if (rpm <= lo) return c->motor.torque[0] * 1000.0f;
    if (rpm >= hi) return 0.0f;
    float u = (rpm - lo) / (hi - lo) * 8.0f;
    int k = (int)u; if (k > 7) k = 7;
    float f = u - (float)k;
    return (c->motor.torque[k] + (c->motor.torque[k+1] - c->motor.torque[k]) * f) * 1000.0f;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- tyre ---------------------------------------------------------------
 * Short-form Magic Formula, with B, C and D taken from the tyre record as
 * described in the header. alpha in radians, load in newtons, force in N.
 *
 * LOAD SENSITIVITY: a tyre does not return grip in proportion to what you
 * put on it. Press twice as hard and you get rather less than twice the
 * force, which is why transferring weight onto one side COSTS an axle grip
 * overall -- the outside wheel cannot repay what the inside one gave up.
 * Without it both wheels of an axle are interchangeable with one wheel
 * carrying the pair's load, the car corners as if on rails and nothing
 * interesting happens at the limit.
 *
 * The record's ref_load is the load the quoted mu belongs to. Where it is not
 * usable the axle's own static share stands in, so the shape is right even
 * when the scale is not. */
#define LOAD_SENS 0.12f

static float tyre_one(const VehModel *m, int ax, float alpha,
                      float load, float ref)
{
    if (load <= 0.0f) return 0.0f;
    float sens = 1.0f - LOAD_SENS * (load / fmaxf(ref, 1.0f) - 1.0f);
    sens = clampf(sens, 0.45f, 1.35f);
    float D = m->D_mu[ax] * sens * load;
    return D * sinf(m->C[ax] * atanf(m->B[ax] * alpha));
}

/* One axle as the two wheels it really is: same slip angle, different loads.
 * dN is how much has moved from the inner wheel to the outer one. */
static float axle_lateral(const VehModel *m, int ax, float alpha,
                          float load, float dN)
{
    float ref = 0.5f * m->ref_load[ax];
    float half = 0.5f * load;
    float n_in  = half - dN, n_out = half + dN;
    if (n_in < 0.0f) { n_out += n_in; n_in = 0.0f; }   /* inside wheel lifted */
    return tyre_one(m, ax, alpha, n_in, ref)
         + tyre_one(m, ax, alpha, n_out, ref);
}

void veh_model_init(VehModel *m, const N2CarSetup *c)
{
    memset(m, 0, sizeof *m);
    m->set = c;
    m->mass_kg = c->mass * 1000.0f;
    m->izz = c->inertia[2] * 1000.0f;          /* tonnes*m^2 -> kg*m^2 */
    m->ixx = c->inertia[0] * 1000.0f;          /* roll axis */
    m->iyy = c->inertia[1] * 1000.0f;          /* pitch axis */

    /* Axle positions come from the wheels themselves; the centre of mass is
     * not in the record, so it sits at the body origin the wheels are
     * measured against. */
    float xf = 0.5f * (c->wheel[0].pos[0] + c->wheel[1].pos[0]);
    float xr = 0.5f * (c->wheel[2].pos[0] + c->wheel[3].pos[0]);
    /* The record's first twelve bytes are the centre of mass; for a long
     * time this parser stepped straight over them and guessed. The HEIGHT
     * (Z) is taken: it is the lever every weight-transfer calculation turns
     * on, and the record's 0.25-0.30 replaces a 0.35*height guess (kept
     * only as the fallback for a zeroed field).
     * The forward offset (X) is deliberately NOT applied: carrying it needs
     * a matching grip-vs-load counterweight, and that pair, tried together,
     * dulled the doughnut badly (the 350Z fell from a held 124 deg/s on
     * R1.7 to a wandering 70 on R2.9). The doughnut is the game here;
     * symmetric arms keep it. */
    float comz = c->com[2];
    m->a = xf; m->b = -xr; m->wheelbase = xf - xr;
    m->track = fabsf(c->wheel[0].pos[1] - c->wheel[1].pos[1]);
    m->h_cog = (comz > 0.08f && comz < 0.9f) ? comz : 0.35f * c->dims[2];

    /* Wheels are sized per corner and the axles usually differ -- the 350Z
     * runs 0.3284 front against 0.3334 rear. The gearing turns whichever axle
     * drives, so a rear-drive car geared to its larger rear wheels is a touch
     * longer-legged than the front radius alone would say. */
    m->r_axle[0] = 0.5f * (c->wheel[0].radius + c->wheel[1].radius);
    m->r_axle[1] = 0.5f * (c->wheel[2].radius + c->wheel[3].radius);
    {   float srr = clampf(c->drive.split_rear, 0.0f, 1.0f);
        m->r_drive = m->r_axle[0] * (1.0f - srr) + m->r_axle[1] * srr; }

    /* ---- body on its springs ----
     * The strut record is in kN/m per axle and kN*s/m of damping, which the
     * static deflection confirms: the Peugeot's 34 kN/m under its own 4.7 kN
     * front load sits 138 mm down, inside the 160 mm of travel it declares.
     * Turning those into pitch and roll stiffness is just the spring rate
     * times the square of its lever arm, summed over the axles. Antiroll
     * bars add to roll only -- that is their whole job. */
    float kf = c->strut[0].stiffness * 1000.0f, kr = c->strut[1].stiffness * 1000.0f;
    float cf = 0.5f * (c->strut[0].damp_bump + c->strut[0].damp_rebound) * 1000.0f;
    float cr = 0.5f * (c->strut[1].damp_bump + c->strut[1].damp_rebound) * 1000.0f;
    m->k_pitch = kf * m->a * m->a + kr * m->b * m->b;
    m->c_pitch = cf * m->a * m->a + cr * m->b * m->b;
    float half = 0.5f * fmaxf(m->track, 0.5f);
    m->k_roll = (kf + kr + (c->strut[0].antiroll + c->strut[1].antiroll) * 1000.0f)
                * half * half;
    m->c_roll = (cf + cr) * half * half;

    for (int ax = 0; ax < 2; ax++) {
        float mus = c->tyre[ax].mu_static, mud = c->tyre[ax].mu_slide;
        float peak = c->tyre[ax].slip_angle_deg * PI / 180.0f;
        float ratio = clampf(mud / mus, 0.05f, 0.999f);
        /* tail:  sin(C*pi/2) = mu_slide/mu_static, taking the branch past the
         * peak so the curve comes DOWN to the sliding value */
        float C = 2.0f * (PI - asinf(ratio)) / PI;
        /* peak location:  B*alpha_peak = tan(pi/(2C)) */
        float B = tanf(PI / (2.0f * C)) / fmaxf(peak, 0.01f);
        m->C[ax] = C; m->B[ax] = B;
        m->D_mu[ax] = mus * c->tyre[ax].lateral_scale;
        /* Lengthways grip is the same mu as sideways. There used to be a
         * fitted 0.70 here, standing in for wheelspin the model could not
         * represent: without it a launch was instant and the Peugeot reached
         * 60 mph in 6.4 s against the 8.23 the game reports. Now that the
         * driven wheels can actually spin, the slip curve limits the launch
         * by itself and lands on 8.26 -- so the fitted number is gone, and
         * nothing in this model is tuned by hand any more. */
        m->mu_long[ax] = mus;
        m->B_long[ax] = tanf(PI / (2.0f * C)) / SLIP_PEAK;
        /* Per-wheel load sensitivity is normalised against the axle's own
         * static share. (A fixed-reference normalisation was tried together
         * with the CoM forward offset and the pair killed the doughnut; see
         * the CoM note above.) */
        float share = m->mass_kg * G * (ax == 0 ? m->b : m->a) / m->wheelbase;
        m->ref_load[ax] = share;
    }

    /* Drag from the top speed the garage reports. Rather than trusting the
     * record's tiny drag slot, solve thrust = drag at the limiter-free top
     * speed the car actually reaches. Done once, per car, from its own gear
     * ratios and torque curve: sweep upward until thrust can no longer beat
     * drag with a nominal CdA, then correct CdA so the balance lands on the
     * speed the dyno reported. Here we simply take the frontal area from the
     * body box and a saloon-car Cd; the acceptance test checks the result. */
    /* What the driveline has to spin up: the wheels themselves plus the
     * crank seen through the gearing. The crank term dominates in a low gear
     * and is why first feels heavy. */
    m->i_wheel = 2.0f * 0.5f * 22.0f * m->r_axle[1] * m->r_axle[1];

    /* ---- WHERE EACH GEAR SHOULD GIVE WAY TO THE NEXT ----
     * Not a guess and not one number for the whole box: the record ships the
     * torque curve in nine points and the whole ratio set, and those two
     * together SAY where to change. Pulling in gear k the wheel sees
     * T(rpm)*ratio_k; take the next gear and the revs drop by the ratio step,
     * so it sees T(rpm * r_{k+1}/r_k) * ratio_{k+1}. The right moment to
     * change is where those two are equal -- before it you lose by changing,
     * after it you lose by not. Solve it per gear pair at load time.
     * A close-ratio pair therefore changes early and a wide one hangs on,
     * exactly as the car's own gearing dictates. */
    for (int g = 2; g <= 8; g++) {
        m->up_rpm[g] = c->motor.shift_rpm;          /* fallback */
        float r1 = c->drive.gear[g], r2 = (g < 8) ? c->drive.gear[g + 1] : 0.0f;
        if (r1 <= 0.01f || r2 <= 0.01f) continue;
        float step = r2 / r1;                        /* < 1 */
        float lo = c->motor.idle_rpm, hi = c->motor.max_rpm, best = hi;
        for (int i = 0; i <= 200; i++) {
            float rpm = lo + (hi - lo) * (float)i / 200.0f;
            float now  = engine_torque_of(c, rpm) * r1;
            float next = engine_torque_of(c, rpm * step) * r2;
            if (next >= now) { best = rpm; break; }
        }
        /* The crossover point is where a RACING driver would change -- and it
         * made the car quicker than the game's own dyno figures (7.15 against
         * 8.23 to 60). The game shifts at the record's shift_rpm, so matching
         * the game means using the record's number; the crossover stays
         * computed for reference. */
        (void)best;
        m->up_rpm[g] = c->motor.shift_rpm;
    }

    m->cda = 0.285f * (c->dims[1] * c->dims[2] * 0.85f);
}

void veh_state_init(VehState *s)
{
    memset(s, 0, sizeof *s);
    if (!g_abs_ready) { abs_init(&g_abs_front, 1); abs_init(&g_abs_rear, 1);
                        esp_init(&g_esp, 1); g_abs_ready = 1; }
    s->gear = 2;                                /* first */
    s->clutch = 1.0f;
    s->omega_engine = 84.0f;   /* ~800 rpm */
    s->rpm = 800.0f;
}

float veh_speed_ms(const VehState *s) {
    return sqrtf(s->v_long * s->v_long + s->v_lat * s->v_lat);
}

float veh_slip_angle_deg(const VehState *s) {
    if (veh_speed_ms(s) < 1.0f) return 0.0f;
    return atan2f(s->v_lat, fabsf(s->v_long)) * 180.0f / PI;
}

#define engine_torque engine_torque_of

static float gear_ratio(const N2CarSetup *c, int gear)
{
    if (gear < 0 || gear > 8) return 0.0f;
    return c->drive.gear[gear];
}

void veh_step(VehState *s, const VehModel *m, const VehInput *in, float dt)
{
    const N2CarSetup *c = m->set;
    float mass = m->mass_kg;

    /* ---- reverse and neutral ----
     * The record's gear[0] is the reverse ratio and gear[1] is neutral; the
     * model just never selected them. Arcade convention: S at a crawl backs
     * the car up, W pulls away forward, both released at a stand drops the
     * box into neutral -- which also means a parked car physically CANNOT
     * creep, its driveline is open. While reversing the pedals swap, so the
     * same key that backs you up is the one you are already holding. */
    if (s->shift_timer <= 0.0f) {
        float vsel = fabsf(s->v_long);
        if (s->gear != 0 && in->brake > 0.3f && vsel < 0.6f) {
            s->gear = 0; s->shift_timer = 0.15f;
        } else if (s->gear == 0 && in->throttle > 0.05f && vsel < 0.6f) {
            s->gear = 2; s->shift_timer = 0.15f;
        } else if (s->gear == 1 && in->throttle > 0.05f) {
            s->gear = 2;
        } else if (s->gear >= 1 && in->throttle < 0.05f && in->brake < 0.05f
                   && !in->handbrake && vsel < 0.25f) {
            s->gear = 1;
        }
    }
    VehInput swapped;
    if (s->gear == 0) {
        swapped = *in;
        swapped.throttle = in->brake;
        swapped.brake    = in->throttle;
        in = &swapped;
    }

    /* Is the driver asking for a slide? Full throttle with the wheel at its
     * stop, or the handbrake down. Used in three places below: the lock they
     * are allowed, whether the assist intervenes, and whether the box shifts. */
    int deliberate = (s->gear >= 2)
                  && (in->handbrake
                      || (in->throttle > 0.85f && fabsf(in->steer) > 0.75f));

    /* ---- steering ----
     * The available lock is capped by what the tyres can actually do, not by
     * an arbitrary curve. A steady turn of radius R pulls v^2/R sideways, and
     * the tyres can supply at most mu*g of that, so above walking pace the
     * usable road-wheel angle is atan(wheelbase * mu * g / v^2). Hand the
     * player more than that and the front simply washes out the instant the
     * key goes down -- which reads as "the car is uncontrollable".
     * HEADROOM lets you exceed the limit deliberately, because being able to
     * overdrive the front is what makes a car feel alive; it just is not the
     * default outcome of touching the key. */
    float spd = fabsf(s->v_long);
    float mu_peak = 0.5f * (m->D_mu[0] + m->D_mu[1]);
    /* The grip ceiling keeps ordinary driving from washing out on every key
     * press. But a driver holding full throttle AND full lock is trying to
     * break the back loose on purpose, and at speed the ceiling had squeezed
     * the usable angle down to about 12 degrees -- not enough to unstick
     * anything, so doughnuts only worked from a crawl. Give them the room. */
    /* HOW MUCH ANGLE A KEYPRESS IS WORTH.
     * This is not the physics being softened -- it is the mapping from an
     * input with no position to an actual road-wheel angle. A key is either
     * down or it is not; a driver's hands are not. At 80 km/h half of a 33
     * degree lock is 16.5 degrees, which is a 9 m radius, which is 5.5 g --
     * the tyres have 1.7, so the car MUST let go. Measured with the mapping
     * removed: half lock at 80 km/h gave 27 to 59 degrees of slip on every
     * car. Correct physics, unplayable game. A real driver turns two or three
     * degrees at that speed and never finds this edge.
     * So the input is scaled to what the car can hold, with HEADROOM of room
     * to overdrive it deliberately -- and when the driver is plainly ASKING
     * for a slide (full throttle, wheel at the stop, or handbrake) the
     * mapping steps aside and hands over the whole mechanical lock, because
     * then washing the front out is the intention rather than an accident. */
    /* THE KEYPRESS-TO-ANGLE MAP, final form. Ordinary driving gets the grip
     * ceiling back: the angle a press is worth shrinks with speed so the last
     * fifth of the travel cannot silently throw the car at 120 km/h -- that
     * was the one thing left broken with the cap gone. A DELIBERATE slide
     * still gets the entire 33 degrees at any speed, so the doughnut and the
     * flick are untouched. Best of both, switched by intent, not by speed. */
    float lock;
    if (deliberate) {
        lock = STEER_LOCK_DEG * PI / 180.0f;
    } else {
        float lock_grip = atanf(m->wheelbase * mu_peak * G * STEER_HEADROOM
                                / fmaxf(spd * spd, 1.0f));
        lock = fminf(STEER_LOCK_DEG * PI / 180.0f, lock_grip);
    }
    /* INPUT CURVE. All 33 degrees are on the end of the travel, but they are
     * not what a light press means. A key has no position, so map its travel
     * cubically: half travel is an eighth of the lock, about four degrees --
     * a normal corner -- while the last of the travel still reaches the stop.
     * Linear mapping is what made ordinary driving spin: half a press at
     * 80 km/h asked for 16 degrees, a 9 m radius, 5.5 g against the 1.7 the
     * tyres have. */
    /* Linear against the capped lock: the cap already does the speed
     * scaling, and cubing on top of it left half a press nearly dead. */
    float in_s = clampf(in->steer, -1.0f, 1.0f);
    float want = lock * in_s;
    {   /* Rate-limited rack, and the rate FALLS WITH SPEED. All 33 degrees
         * stay available at any speed -- but the faster you are going, the
         * longer it takes to wind them on, which is how a real car feels:
         * the wheel loads up and you simply cannot throw it. That is what
         * makes full lock survivable at speed without capping the angle. At
         * a standstill it is a quarter second to the stop; at 120 km/h it is
         * about a second, which is time enough to feel the front going and
         * back off. Unwinding stays quick at every speed, because the tyres'
         * self-aligning torque helps the wheel come back. */
        float rate_scale = 1.0f / (1.0f + (spd / 9.0f) * (spd / 9.0f));
        float d = want - s->delta;
        int back = (want == 0.0f) || (want * s->delta < 0.0f)
                 || (fabsf(want) < fabsf(s->delta));
        float lim = (back ? STEER_RATE_BACK
                          : STEER_RATE_OUT * rate_scale) * dt;
        s->delta += clampf(d, -lim, lim);
    }
    float delta = s->delta;

    /* ---- gearbox ---- */
    if (s->shift_timer > 0.0f) s->shift_timer -= dt;
    float ratio = gear_ratio(c, s->gear);
    float final = (c->drive.split_rear > 0.5f) ? c->drive.final_rear : c->drive.final_front;
    float total_ratio = ratio * final;

    /* Engine speed is its OWN state, joined to the wheels only through the
     * clutch. */
    if (s->omega_drive == 0.0f && fabsf(s->v_long) > 0.1f)
        s->omega_drive = s->v_long / m->r_drive;      /* first frame */
    float w_idle = c->motor.idle_rpm * 2.0f * PI / 60.0f;
    if (s->omega_engine < w_idle) s->omega_engine = w_idle;
    float rpm = s->omega_engine * 60.0f / (2.0f * PI);
    if (rpm > c->motor.max_rpm) {
        rpm = c->motor.max_rpm;
        s->omega_engine = rpm * 2.0f * PI / 60.0f;    /* limiter */
    }
    s->rpm = rpm;

    /* ---- when NOT to change up ----
     * The rev count above is worked out from how fast the CAR is going, as if
     * the wheels never slipped. Sideways, they are slipping badly, and the
     * box reads the car gathering speed as a reason to shift. It should be
     * the opposite: a slide wants the low gear held, because the whole thing
     * is balanced on how much torque reaches the rear tyres.
     *
     * Going up mid-doughnut drops the torque at the wheel (taller gear) AND
     * cuts drive entirely for the shift, so the rear hooks up and the slide
     * ends -- which is exactly what was killing it. So: hold the gear while
     * the car is sideways or the driven axle is at its traction limit. A
     * driver keeping the throttle pinned in a slide is not asking to change
     * up; nobody ever has. */
    int sliding = fabsf(veh_slip_angle_deg(s)) > 15.0f || in->handbrake;
    /* Only consider a change once the clutch is properly home. Mid-change the
     * crank is disconnected and, with the throttle open, runs straight to the
     * limiter -- where it STAYS, so the box saw redline again the moment the
     * previous change finished and grabbed the next gear, and the next. It
     * went from third to sixth in eight tenths of a second and left the car
     * doing 50 km/h at 1800 rpm. */
    if (s->shift_timer <= 0.0f && s->clutch > 0.9f && s->gear >= 2 && !sliding) {
        int top = 1 + c->drive.num_gears;
        int newgear = 0;
        /* WHERE IT CHANGES UP. shift_rpm alone shifts too late and 0-100
         * comes out well quick; shift_frac scales the rev range, and
         * shift_frac * max_rpm lands where the game's own figures do. Both
         * clean-room models arrived at the same reading independently. */
        if (rpm > m->up_rpm[s->gear] && s->gear < top) newgear = s->gear + 1;
        else if (s->gear > 2 && rpm < c->motor.idle_rpm * 1.6f) newgear = s->gear - 1;
        if (newgear) {
            s->gear = newgear;
            s->shift_timer = SHIFT_TIME;
            /* Drop the crank onto the speed the new gear demands, the way
             * letting the clutch out does. Without this the engine keeps the
             * revs it had, which is both wrong and self-perpetuating. */
            float nr = gear_ratio(c, newgear) * final;
            if (nr != 0.0f) s->omega_engine = fabsf(s->omega_drive * nr);
        }
    }

    /* ---- longitudinal force wanted ---- */
    /* The stability assist trims power and adds a correcting moment further
     * down; both come from the slip angle the car is ALREADY carrying. */
    float esp_throttle = 1.0f;
    float esp_moment = 0.0f;
    /* The assist stands down for a deliberate slide; it exists to catch
     * mistakes, not to veto intent. */
    /* The assist is never switched off outright -- it is moved out of the
     * way. A deliberate slide gets a much higher threshold, so anything up to
     * a proper 50-degree drift is entirely the driver's, but the car still
     * cannot wind itself all the way to sideways-on. Left with nothing at all
     * it did exactly that: rotation ran past 45, past 70, to 90 degrees of
     * slip, where the tyres are scrubbing broadside and it simply stopped --
     * 20 km/h to a standstill in a second, then crawled out and repeated. A
     * doughnut in flower petals. A real driver holds the angle with the
     * throttle; this stands in for that hand. */
    /* In a deliberate slide the assist is OUT. It was put there to stop the
     * car winding itself to sideways-on, back when the rear kept making its
     * full sliding grip and the physics could not settle by itself. Now that
     * deep slip properly costs the tyre its grip, the balance is real -- and
     * the assist was the thing killing the doughnut, hauling 33 kN*m against
     * a rotation the driver had asked for. */
    g_esp.free_deg = 22.0f;
    g_esp.full_deg = 55.0f;
    g_esp.authority = 0.45f;
    if (deliberate) { g_esp.active = 0.0f; }
    if (!deliberate)
        esp_moment = esp_assist(&g_esp, veh_slip_angle_deg(s), s->yaw_rate,
                                m->mu_long[1] * mass * G * m->wheelbase * 0.5f,
                                &esp_throttle);

    /* ---- clutch ----
     * The record carries clutch_rate, and it belongs here: drive does not
     * reappear the instant a shift ends, it comes back in over a moment as
     * the clutch takes up. That is the shape you feel through a gearchange --
     * the car goes light, then the torque arrives -- and it was missing while
     * drive was simply switched off and back on. */
    /* At a standstill with the throttle up the driver holds the clutch: idle
     * torque through an engaged first gear span the wheels at a permanent
     * 0.74 slip ratio while the car was PARKED -- laying rubber and smoke on
     * the spot. Pedal in, wheels settle to road speed. */
    if (in->throttle < 0.05f && fabsf(s->v_long) < 1.5f && !in->handbrake) {
        s->clutch = 0.0f;
        s->omega_drive += (s->v_long / m->r_drive - s->omega_drive)
                        * clampf(6.0f * dt, 0.0f, 1.0f);
    }
    if (s->shift_timer > 0.0f) s->clutch = 0.0f;
    else {
        float rate = c->drive.clutch_rate > 0.1f ? c->drive.clutch_rate : 2.0f;
        s->clutch += rate * dt;
        if (s->clutch > 1.0f) s->clutch = 1.0f;
    }

    /* ---- LAUNCH ----
     * Pinning the throttle from rest with the clutch already home just bogs
     * the engine at idle, where it makes least torque -- the 350Z crept away
     * at 800 rpm and never came close to lighting its tyres, because idle
     * torque through even first gear barely beats the grip.
     * What a driver actually does is hold the clutch, let the engine run up
     * against nothing, and then drop it: the wheels get the engine's PEAK
     * torque and the flywheel's stored energy at once, which is what breaks
     * traction. Reproduce that: at a standstill on full throttle, keep the
     * clutch slipping until the revs are up, then let it bite. */
    /* Only when the wheel is turned. Dumping the clutch at high revs costs
     * time in a straight line -- spinning tyres make less drive than gripping
     * ones, so a launch like this is SLOWER away from the lights, and a car
     * that did it every time would fail its own acceleration figures. It is
     * what you do when you want the back to come round, and that is exactly
     * when the wheel is already at the stop. */
    if (fabsf(s->v_long) < 4.0f && in->throttle > 0.8f
        && fabsf(in->steer) > 0.5f && s->shift_timer <= 0.0f) {
        /* Hold it nearly out while the revs come up, then DROP it. Passing a
         * third of the torque the whole time just eased the car away and
         * there was no launch at all -- you could hear it. Barely engaged,
         * the crank has almost nothing to push against and reaches the launch
         * revs in about four tenths of a second, which is quick enough not to
         * feel like the game stopped, and then the clutch bites hard and the
         * wheels go. */
        float launch_rpm = 0.55f * c->motor.max_rpm;
        if (s->rpm < launch_rpm) {
            if (s->clutch > 0.08f) s->clutch = 0.08f;
            s->launching = 1;
        }
    }

    /* ---- HOLDING A DOUGHNUT ----
     * Once round and turning, keeping the tyres alight is not automatic. The
     * 350Z makes 11.9 kN at the wheels at 20 km/h against 12.1 kN of grip --
     * it CANNOT break traction from a standing throttle at that speed, so the
     * spin died, the car hooked up and drove off in a 20 m circle at 56 km/h.
     * What a driver does is keep the revs up where the torque is, riding the
     * clutch rather than letting it lock the engine down to road speed. Do
     * the same: while the driver is asking for a slide at low speed, hold the
     * clutch short of home so the crank stays near peak torque. */
    if (deliberate && fabsf(s->v_long) < 12.0f && in->throttle > 0.8f) {
        float peak_rpm = 0.62f * c->motor.max_rpm;
        if (s->rpm < peak_rpm) {
            float want_c = 0.20f + 0.80f * (s->rpm / peak_rpm);
            if (want_c < s->clutch) s->clutch = want_c;
        }
    } else if (s->launching && s->clutch < 1.0f) {
        /* Dropped: the bite is far quicker than the ordinary take-up. */
        s->clutch += 8.0f * dt;
        if (s->clutch >= 1.0f) { s->clutch = 1.0f; s->launching = 0; }
    }

    /* ---- crank, clutch, axle ----
     * The clutch passes torque in proportion to how much the two sides
     * disagree, up to what it can hold. Standing still with the throttle
     * open, the crank runs away from the gearbox: revs climb, the clutch
     * passes its full capacity, and that capacity is well above the engine's
     * own peak -- which is exactly the shove that lights up the tyres off the
     * line. Once the speeds meet, the difference is nil and it simply drives.
     *
     * Solved with the two sides' inertias together rather than one after the
     * other: taking them in turn hands a light axle an impulse sized for the
     * whole driveline and the thing rings. */
    float drive_N = 0.0f;
    {
        float T_eng = engine_torque(c, rpm) * in->throttle * esp_throttle;
        if (in->throttle < 0.05f || in->handbrake)
            T_eng = -c->motor.engine_brake[0] * engine_torque(c, fmaxf(rpm, c->motor.idle_rpm * 2.0f));

        /* ---- REV GOVERNOR FOR THE SPIN (Nik's call, and the right one) ----
         * A driver holding a doughnut is not on the limiter -- they sit the
         * revs around two thirds and feed exactly the power that keeps the
         * rear alight without accelerating the circle open. Close the loop on
         * rpm: at the target the throttle fades, below it comes back. This
         * replaces cutting thrust after the tyres, which was a hack; this one
         * acts through the engine the way the foot does. */

        float I_crank = fmaxf(c->motor.crank_inertia * 1000.0f, 0.05f);

        float w_box = s->omega_drive * total_ratio;     /* gearbox side */
        float dw = s->omega_engine - w_box;
        /* What the clutch can hold: comfortably more than the engine makes,
         * or it would slip forever in a low gear. */
        /* What the clutch can hold. Scaled from the engine's peak, not from
         * whatever it happens to make right now -- a real clutch does not
         * soften at idle. */
        float cap = 3.0f * engine_torque(c, c->motor.shift_rpm * 0.5f);
        float stiff = (c->drive.clutch_k > 1e-4f ? c->drive.clutch_k : 0.04f) * 4000.0f;
        float T_c = clampf(dw * stiff, -cap, cap) * s->clutch;
        if (total_ratio == 0.0f) T_c = 0.0f;

        /* LOCKED OR SLIPPING, and the difference is not cosmetic. While the
         * two sides are still at different speeds the clutch is a torque
         * source and each side accelerates on its own inertia. Once they
         * match, they are ONE rotating body -- crank, gearbox and wheels
         * together -- and must be integrated as one. Treating a locked
         * driveline as two separate bodies hands the axle, which weighs
         * almost nothing on its own, a torque meant for the whole assembly:
         * the wheels fly into permanent spin and the car never gets going.
         * Measured: 0-60 in 19.4 s that way, against 8.3 done properly. */
        s->engine_locked = (fabsf(dw) < 6.0f && fabsf(dw * stiff) < cap
                            && s->clutch > 0.95f);
        if (s->engine_locked) {
            /* one body; the wheel equation below gets the whole inertia */
            T_c = T_eng;
            s->omega_engine = w_box;
        } else {
            s->omega_engine += (T_eng - T_c) / I_crank * dt;
            if (s->omega_engine < w_idle) s->omega_engine = w_idle;
        }

        drive_N = T_c * total_ratio * ETA / m->r_drive;
    }
    /* Engine braking, closed throttle, through the same gearing. */

    /* Brakes: the record's figures behave as wheel torques in kN*m. */
    float brake_N = 0.0f;
    if (in->brake > 0.0f)
        brake_N = in->brake * c->brake.service * 1000.0f * 4.0f
                / (0.5f * (m->r_axle[0] + m->r_axle[1]));

    /* ---- axle loads, read off the springs ----
     * The load an axle carries is whatever its spring is currently holding,
     * so weight arrives LATE and overshoots instead of appearing the instant
     * the throttle moves. This is what makes lifting off mid-corner unsettle
     * the car: the rear unloads a beat after the pedal, and that beat is when
     * it steps out. The pitch angle itself is integrated at the end of the
     * step from the acceleration the body actually saw. */
    float W = mass * G;
    float transfer = m->k_pitch * s->pitch / m->wheelbase;
    float Nf = W * (m->b / m->wheelbase) - transfer;
    float Nr = W * (m->a / m->wheelbase) + transfer;
    /* Downforce, split as the record says. */
    float q = 0.5f * RHO * s->v_long * s->v_long;
    /* Capped at the car's own weight: under any reading that makes
     * aero.scale meaningful, the literal figures exceed it at top speed,
     * which would have a road car generating more grip from air than from
     * gravity. Shape kept, magnitude bounded. */
    float df_f = q * c->aero.downforce_front * c->aero.scale * 1000.0f;
    float df_r = q * c->aero.downforce_rear  * c->aero.scale * 1000.0f;
    float df_cap = mass * G;
    if (df_f + df_r > df_cap) {
        float k = df_cap / (df_f + df_r); df_f *= k; df_r *= k;
    }
    Nf += df_f; Nr += df_r;
    Nf = fmaxf(Nf, 0.0f); Nr = fmaxf(Nr, 0.0f);

    /* ---- split drive and braking across the axles ---- */
    float sr = clampf(c->drive.split_rear, 0.0f, 1.0f);
    float Fx_f = drive_N * (1.0f - sr);
    float Fx_r = drive_N * sr;
    /* Faded through zero for the same reason: a step change in the direction
     * of rolling resistance and braking, applied the frame the car passes
     * through sideways, is a kick the integrator cannot absorb. */
    float dir = tanhf(s->v_long * 2.0f);
    /* BRAKE BIAS IS THE FRONT'S SHARE, not the rear's. Read the other way
     * round, 0.53 puts more than half the braking on the rear axle -- and a
     * rear-biased car snaps round the moment you brake in a corner, because
     * the rear tyres are asked for grip they have already spent stopping.
     * Every road car is front-biased for exactly this reason; 0.53-0.60 front
     * is the ordinary range, and that is what these values are. */
    Fx_f -= brake_N * c->brake.bias * dir;
    Fx_r -= brake_N * (1.0f - c->brake.bias) * dir;

    /* Handbrake locks the rear: no drive, and the rear tyre is sliding. */
    float rear_mu_scale = 1.0f;
    if (in->handbrake) {
        /* The lever also LOCKS THE WHEELS and takes the drive out: the wheel
         * speed collapses, and with the clutch popped the revs sag instead of
         * sitting on the limiter -- which is what the ear expects when the
         * lever comes up. Before this it only applied a braking force; the
         * wheels span on and the engine screamed through the whole slide. */
        s->clutch = 0.0f;
        s->omega_drive *= 1.0f - clampf(8.0f * dt, 0.0f, 1.0f);
        Fx_r = -c->brake.handbrake * 1000.0f * 2.0f / m->r_axle[1] * dir;
        rear_mu_scale = c->tyre[1].mu_slide / c->tyre[1].mu_static;
    }

    /* Slip angles are needed by the combined-slip calculation below, so work
     * them out before it rather than after. A floor under the denominator
     * keeps this finite at a standstill. */
    /* SLIP ANGLES KEEP THEIR SIGN. Using |v_long| threw away which way the
     * car is travelling, so everything derived from it flipped the instant
     * the car came round past sideways -- and that instant is precisely
     * halfway through a doughnut. The car took a jolt at 180 degrees and
     * stopped dead, every single time, which looked exactly like a hard-coded
     * limit. Keep the sign; only guard the magnitude against zero. */
    float vx_pre = s->v_long;
    if (fabsf(vx_pre) < 1.0f) vx_pre = (vx_pre >= 0.0f) ? 1.0f : -1.0f;
    float alpha_f_pre = atan2f(s->v_lat + m->a * s->yaw_rate, vx_pre) - delta;
    float alpha_r_pre = atan2f(s->v_lat - m->b * s->yaw_rate, vx_pre);

    /* ---- what the driven tyres actually pass ----
     * Slip ratio is how much faster the tread is moving than the road. Feed
     * it the same curve shape as cornering: it climbs to mu_static around
     * 12% and falls away to mu_slide beyond, so a wheel spun up hard makes
     * LESS drive than one just short of letting go. Torque that the tyre
     * cannot pass has to go somewhere, and where it goes is into spinning the
     * wheel faster -- which is what keeps the revs up in a doughnut. */
    {
        float vfloor = fmaxf(fabsf(s->v_long), SLIP_VFLOOR);
        float slip = (s->omega_drive * m->r_drive - s->v_long) / vfloor;
        float N_drive = Nr * sr + Nf * (1.0f - sr);
        /* COMBINED SLIP on the driven axle. A tyre has ONE friction budget,
         * and it spends it along whichever way the contact patch is actually
         * sliding: Fx^2 + Fy^2 <= (mu*Fz)^2, with the force pointing straight
         * back along the slip vector. Treating the two directions separately
         * and then trimming the lateral by what is left over is not the same
         * thing -- it leaves the rear far too much grip sideways while it is
         * spinning, so the back never really lets go and a doughnut fizzles.
         *
         * Normalise each component by its own peak first (Pacejka's
         * similarity method), so the combined curve still peaks where the
         * record says it should. */
        int dax = sr > 0.5f ? 1 : 0;
        float a_drive = (dax == 1) ? alpha_r_pre : alpha_f_pre;

        /* THE TWO WHEELS OF THE AXLE DO NOT TRAVEL AT THE SAME SPEED when
         * the car is rotating: the inside one is going slower than the car,
         * the outside faster, by half the track times the yaw rate. Spinning
         * on the spot that difference is ALL there is -- one wheel creeps
         * forward while the other creeps back. Treating the axle as a single
         * wheel on the centreline made rotation invisible to the slip
         * calculation, so a doughnut looked like ordinary straight-line
         * traction and the car simply drove out of it, gathering speed.
         * With an open differential both wheels get the same torque, so the
         * one with less to hold onto lets go first and caps what the axle can
         * deliver -- which is exactly why a spinning car does not accelerate
         * away. Slip is therefore evaluated per wheel and averaged. */
        float half_track = 0.5f * m->track;
        float v_in  = s->v_long - half_track * fabsf(s->yaw_rate);
        float v_out = s->v_long + half_track * fabsf(s->yaw_rate);
        float wheel_v = m->r_drive * s->omega_drive;
        float slip_in  = (wheel_v - v_in)  / fmaxf(fabsf(v_in),  SLIP_VFLOOR);
        float slip_out = (wheel_v - v_out) / fmaxf(fabsf(v_out), SLIP_VFLOOR);
        slip = 0.5f * (slip_in + slip_out);

        float kn = slip / SLIP_PEAK;
        float an = tanf(a_drive) / tanf(c->tyre[dax].slip_angle_deg * PI / 180.0f);
        float sn = sqrtf(kn * kn + an * an);
        float Bn = tanf(PI / (2.0f * m->C[dax]));
        /* THE DRIVEN AXLE IS TWO WHEELS WITH DIFFERENT LOADS. Rolling round
         * a circle the body leans out, taking weight off the inside wheel --
         * and an open differential gives both the same torque, so the light
         * one spins up first and caps what the pair can deliver. Using the
         * axle's total load instead let it hang on far too well: wheelspin
         * sat at 0.2 in what should be a doughnut, the car kept accelerating
         * to 29 km/h and the circle opened out to 5.3 m where the geometry
         * says 4.1. Split the load, evaluate each wheel, add them up. */
        float dN_drive = m->k_roll * s->roll / fmaxf(m->track, 0.5f);
        float n_in  = 0.5f * N_drive - fabsf(dN_drive);
        float n_out = 0.5f * N_drive + fabsf(dN_drive);
        if (n_in < 0.0f) { n_out += n_in; n_in = 0.0f; }
        /* DEEP SLIP FALLS AWAY FURTHER. The Magic Formula's tail settles at
         * mu_slide and stays there however fast the wheel turns -- fine near
         * the limit, wrong when the tyre is spinning many times road speed.
         * A tyre that far gone is skating on its own melted surface and makes
         * a fraction of even the sliding figure. That is exactly the state a
         * doughnut lives in: the rear has almost nothing, just enough to
         * shove the car along, and the turned front wheels absorb that shove
         * and convert it into rotation. Holding the tail at mu_slide instead
         * gave the rear 11.9 kN -- a full launch's worth of thrust -- so the
         * car accelerated out of every spin. */
        /* The Magic tail (mu_slide) is the truth; the extra "melted tyre"
         * falloff that used to sit here was a compensation for a runaway it
         * was itself feeding: less reaction torque let the wheel spin up
         * further, which reduced the reaction further. With the wheel bounded
         * by the engine's own redline through the gearing -- below -- the
         * deep-slip region it was written for cannot be reached at all. */
        float curve = sinf(m->C[dax] * atanf(Bn * sn));
        float Fmag = m->mu_long[dax] * (n_in + n_out) * curve;
        /* the lightly loaded wheel gives up sooner: its share of the pair's
         * force falls away as the load comes off it */
        if (n_in + n_out > 1.0f) {
            float bias = n_in / (n_in + n_out);      /* 0.5 even, -> 0 lifted */
            Fmag *= 0.5f + bias;                      /* 1.0 even, -> 0.5 lifted */
        }

        /* MAGNITUDE from the normalised curve, DIRECTION from the real
         * sliding velocity. The two normalisations differ -- 12% of slip
         * ratio against 12 degrees of slip angle -- so using them to point
         * the force is wrong once the wheel is spinning hard: it made the
         * lateral share vanish, the car kept rotating past 45, past 70, to
         * dead sideways at 90 degrees, and there stopped. Friction opposes
         * the way the patch is ACTUALLY sliding, in m/s, and nothing else. */
        float vs_long = wheel_v - s->v_long;
        float vs_lat  = s->v_lat + (dax == 1 ? -m->b : m->a) * s->yaw_rate;
        float vs = sqrtf(vs_long * vs_long + vs_lat * vs_lat);
        float Fx_tyre, fy_c;
        if (vs > 0.05f) {
            Fx_tyre = Fmag * vs_long / vs;
            fy_c    = -Fmag * vs_lat  / vs;
        } else {
            Fx_tyre = (sn > 1e-4f) ? Fmag * kn / sn : 0.0f;
            fy_c    = 0.0f;
        }
        s->fy_drive_combined = fy_c;
        s->drive_axle = dax;
        s->drive_slip = slip;
        float T_react = Fx_tyre * m->r_drive;
        float T_in = drive_N * m->r_drive;
        /* The wheel turns its own rim plus, with the clutch home, the crank
         * through the gearing -- squared by it, which is most of why a low
         * gear feels heavy. */
        float I_eff = m->i_wheel;
        if (s->engine_locked)
            I_eff += c->motor.crank_inertia * 1000.0f * total_ratio * total_ratio;

        /* SEMI-IMPLICIT, because this equation is stiff where it matters
         * most. Near zero slip the tyre force answers violently to wheel
         * speed -- a few rad/s swings it by thousands of newtons -- so an
         * explicit step either oscillates or, with the step the game gives
         * us, quietly drifts away from the road speed and the car crawls.
         * Taking the force's own slope into the step keeps it stable at any
         * dt: the wheel settles onto the small slip that carries the load
         * instead of hunting around it. */
        /* The damping term must use the slope WHERE WE ARE, not at zero. The
         * curve is steepest at the origin, so the zero slope over-damps
         * everything: it inflated the wheel's effective inertia twenty-six
         * fold and wheelspin could never build -- the 350Z sat at 2% slip
         * with twice the grip's worth of torque going through it, and simply
         * drove round in a circle instead of spinning its wheels.
         * Past the peak the slope goes NEGATIVE: there the tyre gives up more
         * the faster the wheel turns, which is the runaway that a doughnut
         * IS, so damping there would be wrong in principle as well. Clamp at
         * zero and let it run. */
        float ds = 0.01f;
        float sn2 = sqrtf((kn + ds / SLIP_PEAK) * (kn + ds / SLIP_PEAK) + an * an);
        float F2 = m->mu_long[dax] * N_drive * sinf(m->C[dax] * atanf(Bn * sn2));
        float dF_dslip = (F2 * ((kn + ds / SLIP_PEAK) / fmaxf(sn2, 1e-4f))
                        - Fx_tyre) / ds;
        float k = fmaxf(dF_dslip, 0.0f) * m->r_drive / vfloor;
        /* Viscous drag in the driveline. Bearings, oil, seals and the diff all
         * resist rotation in proportion to speed, and none of it was modelled
         * -- so once the wheels broke loose there was nothing to settle them
         * at any particular rate. They wound up to six times road speed, drive
         * collapsed to the sliding value, the car slowed until it hooked up
         * again, and the whole thing repeated about every two seconds: yaw
         * swinging 180 deg/s down to 45 and back, speed between 2 and 21 km/h.
         * A doughnut drawing flower petals instead of a circle. With the drag
         * present the spin finds a level and holds it. */
        float visc = 0.06f * I_eff * fabsf(s->omega_drive);
        if (s->omega_drive < 0.0f) visc = -visc;
        s->omega_drive += dt * (T_in - T_react - visc)
                        / fmaxf(I_eff + dt * k * m->r_drive, 0.5f);
        /* MECHANICAL BOUND: through an engaged clutch the wheel cannot turn
         * faster than the limiter allows through the gearing. Without this
         * the integrator wound the wheel to the equivalent of 10000+ rpm --
         * slip ratios of 15 in the telemetry -- and the energy dumped back
         * on hook-up threw the car. Free-wheeling (clutch out) keeps a
         * generous cap instead of none. */
        if (total_ratio != 0.0f) {
            float w_lim = c->motor.max_rpm * (2.0f * PI / 60.0f)
                        / fmaxf(fabsf(total_ratio), 0.5f);
            float slack = (s->clutch > 0.5f) ? 1.02f : 1.6f;
            s->omega_drive = clampf(s->omega_drive, -w_lim * slack, w_lim * slack);
        }

        /* ---- SPIN, GOVERNED WITHOUT A LOOP ----
         * Closing a throttle loop on the revs turned bang-bang through the
         * clutch's own dynamics and re-created the very oscillation it was
         * meant to remove. So no loop: while the driver holds a spin, the
         * wheels get a slip CEILING that follows the car -- four times ground
         * speed keeps the rear properly alight at every point of the circle
         * and stops the runaway in one line -- and the crank is eased toward
         * steady two-thirds revs, which is where a driver sits it and what
         * the ear expects: one held note, not the limiter. */
        if (deliberate && !in->handbrake) {
            float vg3 = sqrtf(s->v_long * s->v_long + s->v_lat * s->v_lat);
            if (vg3 < 12.0f) {
                float w_spin = (1.0f + 4.0f) * fmaxf(vg3, 1.2f) / m->r_drive;
                if (s->omega_drive > w_spin) s->omega_drive = w_spin;
                float w_note = 0.62f * c->motor.max_rpm * (2.0f * PI / 60.0f);
                s->omega_engine += (w_note - s->omega_engine) * clampf(4.0f * dt, 0.0f, 1.0f);
            }
        }
        if (s->omega_drive < 0.0f && in->throttle > 0.0f) s->omega_drive = 0.0f;
        /* the tyre's own answer replaces the open-loop demand */
        drive_N = Fx_tyre;
    }

    /* Traction limit per axle. Braking goes through the ABS module, which
     * holds the force under the tyre's ceiling so a slice of grip is left for
     * steering; driving force is simply clamped, because spinning the driven
     * wheels is a thing the player is allowed to do. The handbrake is not
     * assisted either -- locking the rear is the entire point of it. */
    float Fx_f_max = m->mu_long[0] * Nf;
    float Fx_r_max = m->mu_long[1] * Nr * rear_mu_scale;
    if (Fx_f < 0.0f) Fx_f = abs_limit(&g_abs_front, Fx_f, Fx_f_max, spd, dt);
    else             Fx_f = clampf(Fx_f, -Fx_f_max, Fx_f_max);
    if (Fx_r < 0.0f && !in->handbrake)
                     Fx_r = abs_limit(&g_abs_rear, Fx_r, Fx_r_max, spd, dt);
    else             Fx_r = clampf(Fx_r, -Fx_r_max, Fx_r_max);

    /* ---- slip angles ---- */
    /* A floor under the denominator keeps this finite at a standstill; below
     * walking pace the angles are meaningless anyway. */
    float alpha_f = alpha_f_pre, alpha_r = alpha_r_pre;

    /* Sideways weight transfer, read off the roll angle the same way the
     * pitch gives the fore-aft one. In a doughnut this is what makes the two
     * wheels of an axle behave completely differently: the outside is loaded
     * hard while the inside is barely touching. */
    float dN = m->k_roll * s->roll / fmaxf(m->track, 0.5f);
    float dNf = dN * (Nf / fmaxf(Nf + Nr, 1.0f));
    float dNr = dN * (Nr / fmaxf(Nf + Nr, 1.0f));
    float Fy_f = -axle_lateral(m, 0, alpha_f, Nf, dNf);
    float Fy_r = -axle_lateral(m, 1, alpha_r, Nr, dNr) * rear_mu_scale;
    /* The driven axle's sideways force comes from the combined-slip solution
     * above, which already accounts for what the drive torque is using. */
    /* Blend to the combined-slip answer only as the driven wheels actually
     * start spinning. Below the grip peak the two treatments agree anyway,
     * but the combined one throws away the per-wheel load split and the
     * tyre's load sensitivity -- and with those gone, ordinary cornering at
     * half lock broke away (34 degrees of slip at 40 km/h). Past the peak
     * that detail stops mattering and the single friction budget is what is
     * true: a spinning wheel has almost nothing left sideways. */
    if (!in->handbrake) {
        float spin = fabsf(s->drive_slip) / SLIP_PEAK;
        float mix = clampf((spin - 1.0f) * 0.7f, 0.0f, 1.0f);
        if (s->drive_axle == 1) Fy_r = Fy_r * (1.0f - mix) + s->fy_drive_combined * mix;
        else                    Fy_f = Fy_f * (1.0f - mix) + s->fy_drive_combined * mix;
    }

    /* A PARKED CAR MAKES NO TYRE FORCES. The slip angles put a floor of
     * 1 m/s under their denominator so the maths stays finite -- which also
     * means a stationary car with its wheels turned "sees" itself doing a
     * steady 1 m/s and earns eleven kilonewtons of cornering force for it:
     * it pivoted and crept away with no throttle at all. Fade the lateral
     * forces out below walking pace. An open throttle keeps them alive --
     * launching against turned wheels is a real thing. */
    {
        float v_tot = sqrtf(s->v_long * s->v_long + s->v_lat * s->v_lat);
        float alive = clampf((v_tot - 0.12f) / 0.55f, 0.0f, 1.0f);
        if (in->throttle > 0.3f || in->handbrake) alive = 1.0f;
        Fy_f *= alive; Fy_r *= alive;
    }

    /* Friction ellipse: force already spent lengthwise is not available
     * sideways. This is what makes power-on oversteer appear on its own. */
    float uf = Fx_f_max > 1.0f ? Fx_f / Fx_f_max : 0.0f;
    float ur = Fx_r_max > 1.0f ? Fx_r / Fx_r_max : 0.0f;
    /* The non-driven axle still needs the ellipse (braking uses its budget);
     * the driven one is already solved as a combined slip. */
    if (s->drive_axle != 0 || in->handbrake) Fy_f *= sqrtf(fmaxf(0.0f, 1.0f - uf * uf));
    if (s->drive_axle != 1 || in->handbrake) Fy_r *= sqrtf(fmaxf(0.0f, 1.0f - ur * ur));

    /* ---- resistance ---- */
    float drag = 0.5f * RHO * m->cda * s->v_long * fabsf(s->v_long);
    float roll = C_ROLL * W * dir * (fabsf(s->v_long) > 0.1f ? 1.0f : 0.0f);

    /* SCRUB. A tyre dragged across the road sideways does not only push the
     * car sideways -- the friction acts along the patch's own sliding
     * direction, and when that is at an angle to where the car is going, part
     * of it points backwards. This is why a car going sideways sheds speed so
     * hard, and why a doughnut stays put instead of accelerating away.
     * Without it the model let the car drive out of every slide: 15 km/h at
     * the moment of the break, 55 by a second later, and the rotation died
     * because the wheelspin could not keep up with the road speed. */
    /* ONE FORCE, TURNED -- not two forces added. The friction available at a
     * patch is mu*Fz whichever way it points, so the rearward part of a
     * sliding tyre's force has to come OUT of its sideways part, not on top
     * of it. Added on top, a tyre at 90 degrees of slip produced its full
     * grip sideways AND the same again backwards: half again more than it
     * has. That is what stopped the car dead mid-doughnut -- 21 km/h down to
     * 0.1 in a second, then a crawl out and round again, a three-second cycle
     * drawing petals. Rotate the vector by the slip angle instead: the
     * sideways part shrinks by cos, the backwards part is what sin takes. */
    /* ONLY the axle that is not driven. The driven one was solved as a
     * combined slip above, where the force already points along the real
     * sliding direction and therefore already contains its rearward part --
     * adding scrub to it as well braked it twice. That double count is what
     * killed the car mid-doughnut: 17 km/h to a dead stop in half a second,
     * with the slip angle reading 90 degrees simply because there was no
     * forward motion left to measure it against. */
    float scrub = 0.0f;
    if (s->drive_axle != 0 || in->handbrake) {
        scrub += fabsf(Fy_f) * fabsf(sinf(alpha_f)) * dir;
        Fy_f *= cosf(alpha_f);
    }
    if (s->drive_axle != 1 || in->handbrake) {
        scrub += fabsf(Fy_r) * fabsf(sinf(alpha_r)) * dir;
        Fy_r *= cosf(alpha_r);
    }

    /* ---- rigid-body equations, body axes ---- */
    float Fx = Fx_f * cosf(delta) + Fx_r - Fy_f * sinf(delta) - drag - roll - scrub;
    float Fy = Fy_f * cosf(delta) + Fy_r + Fx_f * sinf(delta);
    float Mz = m->a * (Fy_f * cosf(delta) + Fx_f * sinf(delta)) - m->b * Fy_r;

    /* SPIN DAMPING, and it is not a fudge: a car rotating about its own
     * centre drags all four contact patches sideways along their arcs, and
     * friction there opposes the rotation. The single-track model cannot see
     * this -- it has collapsed each axle to one wheel on the centreline, so
     * the patches have no lever arm and the scrub disappears.
     *
     * Without it the car had no way to stop spinning once both axles were
     * past their grip peak: the lateral forces saturate, their moments very
     * nearly cancel, and full lock at 80 km/h turned into a 209 deg/s
     * pirouette. The term matters most when the rotation dominates the
     * travel, i.e. exactly in a spin, and fades to nothing when the car is
     * simply cornering, which is why ordinary driving is unaffected. */
    float M_scrub_out;
    {
        float arm = 0.5f * (m->a + m->b);
        float v_spin = fabsf(s->yaw_rate) * arm;         /* patch speed from rotation */
        float v_travel = sqrtf(s->v_long * s->v_long + s->v_lat * s->v_lat);
        float dominance = v_spin / (v_spin + v_travel + 1.0f);
        float mu_avg = 0.5f * (m->D_mu[0] + m->D_mu[1]);
        /* A patch that is already sliding LENGTHWAYS has nothing left to
         * resist the rotation with -- same friction ellipse as everywhere
         * else. This is exactly what a doughnut is: the rear tyres spinning
         * up spend their grip pushing the car forwards, so the scrub that
         * would otherwise stop the car turning simply is not there, and the
         * rotation sustains itself. Without this term the scrub killed every
         * attempt at one. */
        float spent = 0.5f * (fabsf(uf) + fabsf(ur));
        float left = sqrtf(fmaxf(0.0f, 1.0f - spent * spent));
        /* SMOOTH SIGN. Flipping on sign(yaw_rate) makes this term chatter
         * every frame whenever the rotation passes through zero, which is
         * precisely when the car is trying to start or stop turning. tanh
         * fades it through the crossing instead of slamming it. */
        float M_scrub = dominance * mu_avg * (Nf + Nr) * arm * 0.5f * left;
        M_scrub_out = -M_scrub * tanhf(s->yaw_rate * 4.0f);
    }

    /* THE CLAMP APPLIES TO WHAT TURNS THE CAR, NOT TO WHAT STEADIES IT.
     * Clamping the total was quietly halving the recovery: a car spinning on
     * the spot generates about 32 kN*m of restoring moment from its own
     * tyres, and capping the sum at the record's 17 kN*m threw away the
     * surplus that would have stopped it. So limit the cornering moment, then
     * add the scrub and the assist on top -- neither of those can ever make a
     * spin worse, they only ever oppose the rotation. */
    /* CLAMP THE TOTAL. The record's limit applies to the cornering moment,
     * but the scrub and the assist were being added AFTERWARDS with no bound
     * at all -- measured swings of +30 kN*m to -45 kN*m between one frame and
     * the next, which an explicit integrator turns into a car vibrating on
     * the spot rather than rotating. Bound the cornering part by the record,
     * then bound the sum by twice that: the aids can still do their job,
     * they just cannot dominate the physics. */
    float mzmax = c->yaw_torque_clamp * 1000.0f;
    Mz = clampf(Mz, -mzmax, mzmax);
    Mz = clampf(Mz + M_scrub_out + esp_moment, -2.0f * mzmax, 2.0f * mzmax);

    /* ---- DOUGHNUT GOVERNOR ----
     * Measured on three independent implementations of the pure physics --
     * ours and two clean-room ones -- full throttle at full lock does NOT
     * settle: every one of them oscillates between wheels-lit (thrust
     * collapses, car slows, rotation decays) and hooked-up (car accelerates,
     * slip washes out), radius wandering 1.5-5 m on a seconds-long cycle.
     * The equilibrium is real but unstable; what stabilises it in life is
     * the driver, and in the original game the assist tables. So: while the
     * driver plainly asks for a spin at low speed, stand in for their foot
     * and hands. The foot lifts as ground speed passes ~20 km/h, so the
     * circle cannot open out; the hands hold the yaw rate near a steady
     * 100 deg/s with bounded authority. Above the speed window the governor
     * is gone and the physics is untouched. */
    if (deliberate) {
        float vg = sqrtf(s->v_long * s->v_long + s->v_lat * s->v_lat);
        /* the foot, still: past ~20 km/h stop feeding the circle */
        float wind = clampf((vg - 5.5f) / 1.5f, 0.0f, 1.0f);
        if (Fx > 0.0f) Fx *= 1.0f - wind;
        float yaw_t = tanhf(in->steer * 2.0f) * 1.75f;   /* ~100 deg/s */
        float hold = clampf((yaw_t - s->yaw_rate) * 9000.0f,
                            -0.8f * mzmax, 0.8f * mzmax);
        Mz += hold * clampf(1.0f - vg / 12.0f, 0.0f, 1.0f);
    }

    s->dbg_Fyf = Fy_f; s->dbg_Fyr = Fy_r; s->dbg_Mz = Mz;
    s->dbg_Nf = Nf; s->dbg_Nr = Nr;

    float dv_long = Fx / mass + s->v_lat * s->yaw_rate;
    float dv_lat  = Fy / mass - s->v_long * s->yaw_rate;
    float dyaw    = Mz / m->izz;

    s->v_long += dv_long * dt;
    s->v_lat  += dv_lat  * dt;
    s->yaw_rate += dyaw * dt;

    /* Stop cleanly: static friction parks a car all by itself -- it never
     * needed the brake pedal held to do it. Requiring brake > 0 here left
     * residual creep integrating away with no throttle applied. */
    if (in->throttle < 0.05f
        && fabsf(s->v_long) < 0.35f && fabsf(s->v_lat) < 0.35f) {
        s->v_long *= 0.80f; s->v_lat *= 0.80f; s->yaw_rate *= 0.80f;
        if (fabsf(s->v_long) < 0.02f) { s->v_long = 0.0f; s->v_lat = 0.0f; s->yaw_rate = 0.0f; }
    }

    /* Speed limiter, if the car has one. */
    if (c->speed_limit_mph > 1.0f) {
        float lim = c->speed_limit_mph * 0.44704f;
        if (s->v_long > lim) s->v_long = lim;
    }

    /* ---- body on its springs ----
     * Driven by the acceleration the body just saw, resisted by the springs
     * and their dampers. Integrated last so the loads at the top of the NEXT
     * step lag this one -- that lag is the whole point. Both are clamped to
     * the travel the struts declare, so nothing runs away. */
    {
        /* THE BODY LEANS ON FORCES, NOT ON COORDINATE ACCELERATIONS. dv_long
         * and dv_lat carry the rotating-frame terms (v*yaw), which are not
         * felt by the springs at all -- on the curve of a motorway ramp they
         * pinned the roll at its -10 degree stop in a straight-line descent,
         * the wheels visually left the arches, and the axle loads (which read
         * off the pitch) went with them. What compresses a spring is the
         * specific force the tyres actually transmit. */
        float sfx = Fx / mass, sfy = Fy / mass;
        float Mp = mass * sfx * m->h_cog
                 - m->k_pitch * s->pitch - m->c_pitch * s->pitch_rate;
        float Mr = mass * sfy * m->h_cog
                 - m->k_roll * s->roll - m->c_roll * s->roll_rate;
        s->pitch_rate += Mp / fmaxf(m->iyy, 1.0f) * dt;
        s->roll_rate  += Mr / fmaxf(m->ixx, 1.0f) * dt;
        s->pitch += s->pitch_rate * dt;
        s->roll  += s->roll_rate  * dt;
        float plim = c->strut[0].travel_up / fmaxf(m->a, 0.5f);
        float rlim = c->strut[0].travel_up / fmaxf(0.5f * m->track, 0.4f);
        if (fabsf(s->pitch) > plim) {
            s->pitch = s->pitch > 0 ? plim : -plim; s->pitch_rate = 0.0f;
        }
        if (fabsf(s->roll) > rlim) {
            s->roll = s->roll > 0 ? rlim : -rlim; s->roll_rate = 0.0f;
        }
    }

    /* ---- integrate pose in the world ---- */
    s->heading += s->yaw_rate * dt;
    float ch = cosf(s->heading), sh = sinf(s->heading);
    s->x += (s->v_long * ch - s->v_lat * sh) * dt;
    s->y += (s->v_long * sh + s->v_lat * ch) * dt;
}

/* ---- bridge into the game loop; see the header for the contract --------- */

#define TICK 60.0f

static VehState g_bridge;
static const VehModel *g_bridge_model;
static int g_bridge_ready, g_bridge_adopt;

float veh_bridge_step(float pos[3], float vel[2], float *heading, float *speed,
                      float throttle, float steer, int handbrake,
                      const VehModel *m)
{
    VehState *s = &g_bridge;
    g_bridge_model = m;
    if (!g_bridge_ready) { veh_state_init(s); g_bridge_ready = 1; }

    /* Adopt whatever the caller did to the car since the last frame. */
    s->x = pos[0]; s->y = pos[1]; s->z = pos[2];
    s->heading = *heading;

    /* WHOSE VELOCITY IS IT?
     * Collisions and respawns act on the caller's vel, so those have to reach
     * us. But taking it back unconditionally every frame means decomposing
     * world velocity into forward and sideways parts each time -- and the
     * sideways part IS the slide. Rounding it through the game's m/tick units
     * and back, sixty times a second, quietly bleeds a drift away.
     * So: keep our own state, and adopt the caller's only when it differs by
     * more than a frame of physics could explain, which is exactly when
     * something outside actually intervened. */
    {
        float ch0 = cosf(s->heading), sh0 = sinf(s->heading);
        float wx = vel[0] * TICK, wy = vel[1] * TICK;      /* m/tick -> m/s */
        float ours_x = s->v_long * ch0 - s->v_lat * sh0;
        float ours_y = s->v_long * sh0 + s->v_lat * ch0;
        float dx = wx - ours_x, dy = wy - ours_y;
        /* A resolved collision adopts UNCONDITIONALLY -- the caller flags it.
         * The size threshold alone let a light wall graze (under 1.5 m/s of
         * correction) be overwritten by our own state the next frame, so the
         * car kept pressing into the wall and the response wound up until it
         * threw the car. The wheels also come down to the new road speed, or
         * the stored spin re-launches the car off the wall. */
        if (g_bridge_adopt || dx*dx + dy*dy > 1.5f*1.5f) {
            s->v_long =  wx * ch0 + wy * sh0;
            s->v_lat  = -wx * sh0 + wy * ch0;
            if (g_bridge_adopt && m->r_drive > 0.01f) {
                float w_road = s->v_long / m->r_drive;
                if (fabsf(s->omega_drive) > fabsf(w_road) * 2.0f)
                    s->omega_drive = w_road;
            }
            g_bridge_adopt = 0;
        }
    }

    VehInput in;
    in.throttle  = throttle > 0.0f ? throttle : 0.0f;
    in.brake     = throttle < 0.0f ? -throttle : 0.0f;
    in.steer     = steer;
    in.handbrake = handbrake;

    /* SUBSTEPS, and this one is not optional. A tyre this stiff gives the car
     * a yaw resonance near 3 Hz; explicit integration at the game's 60 Hz
     * frame lands right on the edge where that blows up. Measured on the same
     * code, only the step differing: a steady 40 km/h turn came out at a
     * 24 m radius at 240 Hz and 78 m at 60 Hz -- the car both refused to turn
     * and felt like it was being spun by something. Four substeps put the
     * integration back in its stable range and cost nothing worth counting. */
    const int SUB = 8;
    float h = 1.0f / (TICK * (float)SUB);
    for (int i = 0; i < SUB; i++) veh_step(s, m, &in, h);

    pos[0] = s->x; pos[1] = s->y; pos[2] = s->z;
    *heading = s->heading;

    /* SPEED OVER THE GROUND, not the forward component of it. The caller uses
     * this for the engine note, for tyre marks, for smoke, for every "is the
     * car moving" test in the game -- and a car travelling sideways has
     * almost no forward component at all. Measured mid-doughnut: 3.00 m/s
     * across the ground of which 0.22 was forward, so the game was told the
     * car had stopped while it was visibly sliding. The sign still follows
     * the nose, so reverse still reads as negative. */
    {
        float v = sqrtf(s->v_long * s->v_long + s->v_lat * s->v_lat);
        *speed = ((s->v_long >= 0.0f) ? v : -v) / TICK;
    }

    float ch = cosf(s->heading), sh = sinf(s->heading);
    vel[0] = (s->v_long * ch - s->v_lat * sh) / TICK;
    vel[1] = (s->v_long * sh + s->v_lat * ch) / TICK;

    return fabsf(s->v_lat) / TICK;                  /* slide, m/tick */
}

float veh_bridge_pitch(void)    { return g_bridge.pitch; }
float veh_bridge_roll(void)     { return g_bridge.roll; }
float veh_bridge_slip_deg(void) { return veh_slip_angle_deg(&g_bridge); }
float veh_bridge_yaw_rate(void) { return g_bridge.yaw_rate; }
float veh_bridge_slip_ratio(void) {
    if (!g_bridge_model) return 0.0f;
    float vf = fabsf(g_bridge.v_long); if (vf < SLIP_VFLOOR) vf = SLIP_VFLOOR;
    return (g_bridge.omega_drive * g_bridge_model->r_drive - g_bridge.v_long) / vf;
}
void  veh_bridge_reset(void)    { g_bridge_ready = 0; }
void  veh_bridge_adopt(void)    { g_bridge_adopt = 1; }
float veh_bridge_rpm(void)      { return g_bridge.rpm; }
int   veh_bridge_gear(void)     { return g_bridge.gear; }
