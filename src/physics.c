/* physics.c — OpenUG2 Physics module implementation. */
#include <math.h>
#include <assert.h>
#include <string.h>

#include "physics.h"

/* handbrake: rear grip lets go, the lateral velocity survives → drift */
#define HANDBRAKE_GRIP   0.985f
#define REVERSE_SPD_FRAC 0.2f    /* reverse cap ~45 km/h */
#define REVERSE_ACCEL    0.7f    /* reverse thrust vs forward */
#define BRAKE_ACCEL      1.5f    /* braking vs forward thrust: ~10 m/s^2 */
#define COAST_DRAG       0.9994f /* off-throttle engine braking on top of drag */
/* turn authority ramps in by ~35% of top speed then holds — responsive from
 * low speed without getting twitchy/spinny flat out. */
#define TURN_RAMP_FRAC   0.35f
/* ...and eases off toward top speed so 200+ km/h stays stable (NFSU2 cars
 * corner tight at city speed, wide at full tilt) */
#define TURN_HISPD_DROP  0.55f

PhysTune g_phys_tune = { 1.0f, 1.0f, 1.0f, 220.0f };   /* stock defaults */

/* Asphalt: the tuned NFSU2 feel, unchanged. */
const PhysSurface PHYS_SURF_ROAD    = { 1.00f, 1.00f, PHYS_FRICTION, PHYS_GRIP, 1.00f };
/* Dirt/grass/hillside: it can still be driven onto and across, but it will not
 * carry the car to road speed and it holds the sideways component far longer,
 * so a hill has to be climbed slowly and slid across instead of railed up. */
const PhysSurface PHYS_SURF_TERRAIN = { 0.55f, 0.50f, 0.99800f,     0.94f,     0.75f };

static float pv_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float phys_steer_response(float current, float target) {
    target=pv_clamp(target,-1.0f,1.0f);
    float k=fabsf(target)<0.001f ? 0.18f : 0.14f;
    return current+(target-current)*k;
}

/* ---- sprung ride / four-wheel contact (M130) ------------------------------ */

/* Wheel offsets re-centred on their own centroid, so four equal contact forces
   produce exactly zero pitch and roll torque and a resting car cannot drift. */
static void pr_axes(const PhysRideSupport *s, float ax[4], float ay[4],
                    float *ipitch, float *iroll) {
    float mx = 0, my = 0;
    for (int k = 0; k < 4; k++) { mx += s->ax[k]; my += s->ay[k]; }
    mx *= 0.25f; my *= 0.25f;
    float sp = 0, sr = 0;
    for (int k = 0; k < 4; k++) {
        ax[k] = s->ax[k] - mx; ay[k] = s->ay[k] - my;
        sp += ax[k]*ax[k]; sr += ay[k]*ay[k];
    }
    *ipitch = sp > 1e-4f ? sp : 1e-4f;
    *iroll  = sr > 1e-4f ? sr : 1e-4f;
}

float phys_ride_wheel_z(const PhysRideState *r, const PhysRideSupport *s, int k) {
    float ax[4], ay[4], ip, ir; pr_axes(s, ax, ay, &ip, &ir);
    return r->z + ax[k]*r->pitch + ay[k]*r->roll;
}

void phys_ride_init(PhysRideState *r, const PhysRideSupport *s) {
    memset(r, 0, sizeof *r);
    float ax[4], ay[4], ip, ir; pr_axes(s, ax, ay, &ip, &ir);
    float sum = 0, lo = 1e30f, hi = -1e30f; int n = 0;
    for (int k = 0; k < 4; k++) if (s->valid[k]) {
        sum += s->z[k]; n++;
        if (s->z[k] < lo) lo = s->z[k];
        if (s->z[k] > hi) hi = s->z[k];
    }
    r->z = n ? sum / n : s->z[0];
    (void)lo; (void)hi;
    /* Fit the resting plane. Three coherent contacts are the minimum that
       defines one; below that do not invent a pose out of two points. */
    if (n >= 3) {
        float sp = 0, sr = 0, wp = 0, wr = 0;
        for (int k = 0; k < 4; k++) if (s->valid[k]) {
            float d = s->z[k] - r->z;
            sp += ax[k]*d; wp += ax[k]*ax[k];
            sr += ay[k]*d; wr += ay[k]*ay[k];
        }
        if (wp > 1e-4f) r->pitch = sp / wp;
        if (wr > 1e-4f) r->roll  = sr / wr;
        /* Coherence is judged on the RESIDUAL, not on the raw spread: a car on
           an 8% grade legitimately sees 0.25 m across a 3.1 m wheelbase, and
           that is one plane. A wheel standing on a different layer is not, and
           shows up as a residual no plane can absorb. */
        float res = 0;
        for (int k = 0; k < 4; k++) if (s->valid[k]) {
            float e = s->z[k] - (r->z + ax[k]*r->pitch + ay[k]*r->roll);
            if (e < 0) e = -e;
            if (e > res) res = e;
        }
        float ap = r->pitch < 0 ? -r->pitch : r->pitch;
        float ar = r->roll  < 0 ? -r->roll  : r->roll;
        if (res > PHYS_RIDE_PLANE_SPREAD ||
            ap > PHYS_RIDE_MAXTILT || ar > PHYS_RIDE_MAXTILT) {
            r->pitch = 0; r->roll = 0;       /* unrelated layers: stay level */
        }
    }
    for (int k = 0; k < 4; k++) {
        if (!s->valid[k]) { r->compression[k] = -PHYS_RIDE_DROOP; continue; }
        r->contact_mask |= 1u << k;
        float c = s->z[k] - (r->z + ax[k]*r->pitch + ay[k]*r->roll);
        if (c >  PHYS_RIDE_BUMP)  c =  PHYS_RIDE_BUMP;
        if (c < -PHYS_RIDE_DROOP) c = -PHYS_RIDE_DROOP;
        r->compression[k] = c;
    }
    /* An unsupported spawn is NOT silently grounded: it starts airborne and
       falls, exactly as a mid-air placement should. */
    r->air_frames = n ? 0 : 1;
}

float phys_ride_reach_down(const PhysRideState *r, float dt) {
    float fall = r->vz < 0 ? -r->vz * dt : 0.0f;
    return PHYS_RIDE_DROOP + fall;
}

void phys_ride_step(PhysRideState *r, const PhysRideSupport *s, float dt) {
    const float w    = 6.2831853f * PHYS_RIDE_FREQ;   /* rad/s */
    const float K    = w * w;                          /* 1/s^2 per metre       */
    const float C    = 2.0f * PHYS_RIDE_ZETA * w;      /* 1/s                   */
    float ax[4], ay[4], ip, ir; pr_axes(s, ax, ay, &ip, &ir);

    unsigned was = r->contact_mask;
    r->impact = 0.0f;
    r->lift   = 0.0f;

    /* per-wheel compression and force. The +G preload makes zero compression
       the resting equilibrium, so a fully supported car sits exactly on its
       contacts instead of sagging by an invented amount. */
    float force[4] = {0,0,0,0};
    unsigned mask = 0;
    for (int k = 0; k < 4; k++) {
        float wz = r->z + ax[k]*r->pitch + ay[k]*r->roll;
        float wv = r->vz + ax[k]*r->pitch_rate + ay[k]*r->roll_rate;
        if (!s->valid[k]) {                       /* hangs at full droop */
            r->compression[k] = -PHYS_RIDE_DROOP;
            continue;
        }
        float c = s->z[k] - wz;
        if (c < -PHYS_RIDE_DROOP) {      /* tracked, but hanging clear of it */
            r->compression[k] = -PHYS_RIDE_DROOP;
            continue;                    /* no contact, no force: it falls */
        }
        mask |= 1u << k;
        if (c > PHYS_RIDE_BUMP) c = PHYS_RIDE_BUMP;
        r->compression[k] = c;
        force[k] = K*c - C*wv + PHYS_RIDE_G;
    }
    r->contact_mask = mask;

    if (!mask) {                                   /* free flight */
        r->vz -= PHYS_RIDE_G * dt;
        r->z  += r->vz * dt;
        /* bleed angular rates so an airborne car cannot tumble without bound */
        r->pitch_rate *= 0.98f; r->roll_rate *= 0.98f;
        r->pitch += r->pitch_rate * dt; r->roll += r->roll_rate * dt;
        r->air_frames++;
    } else {
        /* Landing frame: record the arrival speed for the caller, then let the
           spring and damper absorb it. There is deliberately NO restitution
           term. One used to force vz to 0.25*impact here, AFTER the damper had
           already resolved the frame, so landing energy was handled twice and a
           single 4.4 m/s arrival on a continuous -6% road launched the car into
           a 6-hop, 174-of-180-frame ballistic cycle (M130-R2 case 1). */
        if (!was) r->impact = r->vz < 0 ? -r->vz : r->vz;
        float fz = 0, tp = 0, tr = 0;
        for (int k = 0; k < 4; k++) { fz += force[k]; tp += ax[k]*force[k]; tr += ay[k]*force[k]; }
        r->vz         += (fz * 0.25f - PHYS_RIDE_G) * dt;
        r->pitch_rate += (tp / ip) * dt;
        r->roll_rate  += (tr / ir) * dt;
        r->z     += r->vz * dt;
        r->pitch += r->pitch_rate * dt;
        r->roll  += r->roll_rate  * dt;
        r->air_frames = 0;
    }

    if (r->pitch >  PHYS_RIDE_MAXTILT) { r->pitch =  PHYS_RIDE_MAXTILT; if (r->pitch_rate > 0) r->pitch_rate = 0; }
    if (r->pitch < -PHYS_RIDE_MAXTILT) { r->pitch = -PHYS_RIDE_MAXTILT; if (r->pitch_rate < 0) r->pitch_rate = 0; }
    if (r->roll  >  PHYS_RIDE_MAXTILT) { r->roll  =  PHYS_RIDE_MAXTILT; if (r->roll_rate  > 0) r->roll_rate  = 0; }
    if (r->roll  < -PHYS_RIDE_MAXTILT) { r->roll  = -PHYS_RIDE_MAXTILT; if (r->roll_rate  < 0) r->roll_rate  = 0; }

    /* bump stop: no wheel may end the frame compressed past its travel, so the
       body can never settle through a surface it is standing on. */
    float pen = 0;
    for (int k = 0; k < 4; k++) {
        if (!s->valid[k]) continue;
        float wz = r->z + ax[k]*r->pitch + ay[k]*r->roll;
        float over = (s->z[k] - wz) - PHYS_RIDE_BUMP;
        if (over > pen) pen = over;
    }
    if (pen > 0) {
        /* Bounded by the travel that produced it: one frame cannot push a valid
           contact further than full bump past its stop, so this can correct a
           penetration but can never lift the body across a surface. Only
           reachable contacts reach this loop, so an out-of-reach deck overhead
           contributes nothing. */
        if (pen > PHYS_RIDE_PEN_MAX) pen = PHYS_RIDE_PEN_MAX;
        r->z += pen; r->lift = pen; if (r->vz < 0) r->vz = 0;
    }

    /* refresh the reported compressions after the clamp/bump-stop */
    for (int k = 0; k < 4; k++) {
        if (!s->valid[k]) { r->compression[k] = -PHYS_RIDE_DROOP; continue; }
        float wz = r->z + ax[k]*r->pitch + ay[k]*r->roll;
        float c = s->z[k] - wz;
        if (c >  PHYS_RIDE_BUMP)  c =  PHYS_RIDE_BUMP;
        if (c < -PHYS_RIDE_DROOP) c = -PHYS_RIDE_DROOP;
        r->compression[k] = c;
    }
}

PhysVehicle phys_vehicle_from_geometry(float body_len, float body_wid, float body_hgt,
                                       float wheelbase, float track, float tyre_w) {
    PhysVehicle v = { 1.0f, 1.0f, 1.0f, 1.0f };
    float vol = body_len * body_wid * body_hgt;
    if (vol > 1e-3f) {
        /* mass/inertia proxy: sqrt so the 17x volume spread across the fleet
           (8.3 to 141.7 m^3) cannot swamp the model before the clamp does */
        float m = sqrtf(PHYS_FLEET_VOLUME / vol);
        v.accel = pv_clamp(m, 0.75f, 1.25f);
        v.brake = pv_clamp(m, 0.75f, 1.25f);
    }
    if (wheelbase > 0.5f)
        v.steer = pv_clamp(PHYS_FLEET_WHEELBASE / wheelbase, 0.80f, 1.20f);
    if (tyre_w > 0.02f && track > 0.5f) {
        /* wider tyre and wider track = less sideways scrub. lat is RETENTION,
           so a grippier car gets a smaller multiplier. */
        float g = sqrtf(PHYS_FLEET_TYREW / tyre_w)
                * powf(PHYS_FLEET_TRACK / track, 0.25f);
        v.lat = pv_clamp(g, 0.90f, 1.10f);
    }
    return v;
}

float phys_car_step(float pos[3], float vel[2], float *heading, float *speed,
                    float throttle, float steer, int handbrake,
                    const PhysSurface *sf, const PhysVehicle *vh) {
    static const PhysVehicle NEUTRAL = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!sf) sf = &PHYS_SURF_ROAD;
    if (!vh) vh = &NEUTRAL;
    float top = g_phys_tune.top_kmh / 3.6f / PHYS_TICKRATE * sf->topfrac;
    float hf[2] = { cosf(*heading), sinf(*heading) };
    float fwd = vel[0]*hf[0] + vel[1]*hf[1];   /* signed forward speed */
    if (throttle > 0) {
        /* throttle tapers as speed builds: punchy off the line, eases near top */
        float sp = *speed < 0 ? -*speed : *speed;
        float a = PHYS_ACCEL * g_phys_tune.accel * (1.15f - 0.55f*sp/PHYS_MAXSPD)
                  * throttle * sf->accel * vh->accel;
        vel[0] += hf[0]*a; vel[1] += hf[1]*a;
    } else if (throttle < 0) {
        /* moving forward = brakes (strong); at rest / rolling back = reverse */
        float a = PHYS_ACCEL * (fwd > 0.01f ? BRAKE_ACCEL*g_phys_tune.brake*vh->brake
                                            : REVERSE_ACCEL);
        vel[0] += hf[0]*a*throttle; vel[1] += hf[1]*a*throttle;
    } else {
        vel[0] *= COAST_DRAG; vel[1] *= COAST_DRAG;
    }
    vel[0] *= sf->drag; vel[1] *= sf->drag;
    float spd = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]);
    float dir = (vel[0]*hf[0]+vel[1]*hf[1]) < 0 ? -1.f : 1.f;  /* fwd vs reverse */
    float sfac = spd/(PHYS_MAXSPD*TURN_RAMP_FRAC); if (sfac > 1) sfac = 1;
    float hifrac = spd/PHYS_MAXSPD; if (hifrac > 1) hifrac = 1;
    *heading += steer * PHYS_TURN * g_phys_tune.turn * sfac
                * (1.0f - TURN_HISPD_DROP*hifrac) * dir * sf->steer * vh->steer;
    /* decompose velocity in the new heading frame, clamp forward, scrub side */
    float nf[2] = { cosf(*heading), sinf(*heading) }, nr[2] = { nf[1], -nf[0] };
    float vf = vel[0]*nf[0]+vel[1]*nf[1], vl = vel[0]*nr[0]+vel[1]*nr[1];
    if (vf >  top) vf =  top;
    if (vf < -top*REVERSE_SPD_FRAC) vf = -top*REVERSE_SPD_FRAC;
    /* surface and vehicle both scale retention; keep the product a contraction
       so a slide can never be amplified. */
    { float lat = sf->lat * vh->lat;
      if (lat > 0.99f) lat = 0.99f; if (lat < 0.50f) lat = 0.50f;
      vl *= handbrake ? HANDBRAKE_GRIP : lat; }
    vel[0] = nf[0]*vf + nr[0]*vl; vel[1] = nf[1]*vf + nr[1]*vl;
    *speed = vf;                      /* forward speed, for HUD/collision */
    pos[0] += vel[0]; pos[1] += vel[1];
    return vl < 0 ? -vl : vl;         /* drift magnitude */
}

void phys_selftest(void) {
    /* the NFSU2 tuning targets, asserted: 0-100 in 3-6 s, top ~220 km/h,
       100-0 braking well under 5 s */
    float pos[3]={0,0,0}, vel[2]={0,0}, h=0, spd=0;
    int t100 = -1;
    for (int t = 1; t <= 60*60; t++) {
        phys_car_step(pos, vel, &h, &spd, 1.0f, 0, 0, NULL, NULL);
        if (t100 < 0 && PHYS_KMH(spd) >= 100.0f) t100 = t;
    }
    assert(t100 > 2*60 && t100 < 6*60);
    assert(PHYS_KMH(spd) > 200.0f && PHYS_KMH(spd) < 232.0f);
    /* brake from ~100 km/h */
    vel[0] = cosf(h)*100.0f/3.6f/PHYS_TICKRATE; vel[1] = sinf(h)*100.0f/3.6f/PHYS_TICKRATE;
    int tstop = -1;
    for (int t = 1; t <= 8*60 && tstop < 0; t++) {
        phys_car_step(pos, vel, &h, &spd, -1.0f, 0, 0, NULL, NULL);
        if (spd <= 0.0f) tstop = t;
    }
    assert(tstop > 0 && tstop < 5*60);

    /* Full keyboard lock at 50 km/h must remain an arcade turn, not rotate the
       car through a U-turn and beyond in two seconds. */
    {
        float p[3]={0,0,0}, v[2]={50.0f/3.6f/PHYS_TICKRATE,0}, hd=0, s=0;
        for (int t=0;t<120;t++) phys_car_step(p,v,&hd,&s,1.0f,1.0f,0,NULL,NULL);
        assert(hd > 1.40f && hd < 2.15f);       /* about 80..123 degrees */
    }
    {
        float st=0.0f;
        st=phys_steer_response(st,1.0f);
        assert(st > 0.05f && st < 0.20f);       /* no instant full lock */
        for (int t=1;t<10;t++) st=phys_steer_response(st,1.0f);
        assert(st > 0.70f && st < 0.90f);       /* responsive inside 1/6 s */
        for (int t=0;t<16;t++) st=phys_steer_response(st,0.0f);
        assert(fabsf(st) < 0.05f);              /* recentres promptly */
    }

    /* --- surface profiles (M114) --------------------------------------------
     * Flat ground, sustained full throttle, identical inputs: terrain must
     * settle materially below road speed and must slide measurably more. */
    {
        float rp[3]={0,0,0}, rv[2]={0,0}, rh=0, rs=0;
        float tp[3]={0,0,0}, tv[2]={0,0}, th=0, ts=0;
        for (int t = 0; t < 60*60; t++) {
            phys_car_step(rp, rv, &rh, &rs, 1.0f, 0, 0, &PHYS_SURF_ROAD, NULL);
            phys_car_step(tp, tv, &th, &ts, 1.0f, 0, 0, &PHYS_SURF_TERRAIN, NULL);
        }
        assert(PHYS_KMH(ts) < 0.65f * PHYS_KMH(rs));   /* materially slower */
        assert(PHYS_KMH(ts) > 5.0f);                   /* still drivable */
        /* same steering input from the same speed: terrain keeps more of the
         * sideways component, i.e. it slides more (phys_car_step returns the
         * post-grip lateral magnitude). */
        float ap[3]={0,0,0}, av[2], ah=0, as2=0, bp[3]={0,0,0}, bv[2], bh=0, bs=0;
        av[0]=bv[0]=30.0f/3.6f/PHYS_TICKRATE; av[1]=bv[1]=0;
        float dr = 0, dt2 = 0;
        for (int t = 0; t < 30; t++) {
            dr  = phys_car_step(ap, av, &ah, &as2, 1.0f, 1.0f, 0, &PHYS_SURF_ROAD, NULL);
            dt2 = phys_car_step(bp, bv, &bh, &bs,  1.0f, 1.0f, 0, &PHYS_SURF_TERRAIN, NULL);
        }
        assert(dt2 > dr);                              /* more lateral slide */
    }

    /* --- vehicle profiles (M121) ---------------------------------------------
     * Geometry-derived factors must stay inside their clamps for anything the
     * fleet can hand us, including the extremes (8.3 m^3 hatchback, 141.7 m^3
     * bus) and degenerate/missing measurements. */
    {
        const float ex[][6] = {
            {  3.9f, 1.9f, 1.2f, 2.43f, 1.43f, 0.188f },   /* smallest measured */
            {  4.8f, 2.5f, 1.8f, 3.11f, 1.76f, 0.284f },   /* HUMMER */
            { 12.0f, 2.6f, 4.5f, 7.88f, 2.63f, 0.489f },   /* largest measured */
            {  0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.000f },   /* nothing measured */
        };
        for (int i = 0; i < 4; i++) {
            PhysVehicle v = phys_vehicle_from_geometry(ex[i][0], ex[i][1], ex[i][2],
                                                       ex[i][3], ex[i][4], ex[i][5]);
            assert(v.accel >= 0.75f && v.accel <= 1.25f);
            assert(v.brake >= 0.75f && v.brake <= 1.25f);
            assert(v.steer >= 0.80f && v.steer <= 1.20f);
            assert(v.lat   >= 0.90f && v.lat   <= 1.10f);
        }
        /* a neutral profile must reproduce the NULL path exactly */
        PhysVehicle nv = { 1.0f, 1.0f, 1.0f, 1.0f };
        float ap[3]={0,0,0}, av[2]={0,0}, ah=0, as3=0;
        float bp[3]={0,0,0}, bv[2]={0,0}, bh=0, bs2=0;
        for (int t = 0; t < 600; t++) {
            phys_car_step(ap, av, &ah, &as3, 1.0f, 0.5f, 0, NULL, NULL);
            phys_car_step(bp, bv, &bh, &bs2, 1.0f, 0.5f, 0, NULL, &nv);
        }
        assert(ap[0] == bp[0] && ap[1] == bp[1] && ah == bh && as3 == bs2);
        /* braking still stops a heavy car: the slowest brake factor is 0.75 */
        PhysVehicle hv = { 0.75f, 0.75f, 1.0f, 1.0f };
        float cp2[3]={0,0,0}, cv[2], ch=0, cs3=0; int stop = -1;
        cv[0]=100.0f/3.6f/PHYS_TICKRATE; cv[1]=0;
        for (int t = 1; t <= 10*60 && stop < 0; t++) {
            phys_car_step(cp2, cv, &ch, &cs3, -1.0f, 0, 0, NULL, &hv);
            if (cs3 <= 0.0f) stop = t;
        }
        assert(stop > 0 && stop < 7*60);
    }

}

/* Does this mesh present an actual wall to the car here, and WHICH feature?
 * Near-vertical face, height span overlapping the car, XY projection within r.
 * Two passes over the same faces: the first finds the closest edge feature and
 * the union vertical span of every contacting face, the second is not needed --
 * the span is accumulated as we go. The normal comes from the closest point to
 * the car centre, never from triangle winding, so a wall pushes the car away
 * from itself rather than along whatever axis its bounding box prefers. */
int cw_mesh_feature(const N2Scene *s, int mi, float px, float py,
                    float r, float cz0, float cz1, PhysWallContact *out) {
    if (mi < 0 || mi >= s->count) return 0;
    const N2Mesh *m = &s->meshes[mi];
    float r2 = r*r;
    float bestd2 = 1e30f, bcx = 0, bcy = 0; int btri = -1;
    float ulo = 1e30f, uhi = -1e30f;                 /* union span of contacts */
    float fnx = 0, fny = 0;                          /* winding normal, fallback */
    for (int t = 0; t + 2 < m->nidx; t += 3) {
        const float *A = m->verts + m->idx[t]*5;
        const float *B = m->verts + m->idx[t+1]*5;
        const float *C = m->verts + m->idx[t+2]*5;
        float e1[3], e2[3], n[3];
        for (int a = 0; a < 3; a++) { e1[a] = B[a]-A[a]; e2[a] = C[a]-A[a]; }
        n[0]=e1[1]*e2[2]-e1[2]*e2[1]; n[1]=e1[2]*e2[0]-e1[0]*e2[2];
        n[2]=e1[0]*e2[1]-e1[1]*e2[0];
        float L = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (L < 1e-9f) continue;
        if (fabsf(n[2]/L) >= 0.30f) continue;                 /* not a wall face */
        float zlo = A[2], zhi = A[2];
        if (B[2]<zlo) zlo=B[2]; if (C[2]<zlo) zlo=C[2];
        if (B[2]>zhi) zhi=B[2]; if (C[2]>zhi) zhi=C[2];
        if (zhi < cz0 || zlo > cz1) continue;                 /* not at car height */
        const float *P[3] = { A, B, C };
        int touched = 0;
        for (int e = 0; e < 3; e++) {
            const float *p0 = P[e], *p1 = P[(e+1)%3];
            float dx = p1[0]-p0[0], dy = p1[1]-p0[1], l2 = dx*dx+dy*dy;
            float u = l2 > 1e-9f ? ((px-p0[0])*dx + (py-p0[1])*dy) / l2 : 0.0f;
            if (u < 0) u = 0; if (u > 1) u = 1;
            float sx = p0[0]+dx*u, sy = p0[1]+dy*u;
            float qx = px - sx, qy = py - sy, d2 = qx*qx + qy*qy;
            if (d2 > r2) continue;
            touched = 1;
            /* closest feature wins; ties go to the lower triangle index, so the
               choice is the same on every run regardless of float noise */
            if (d2 < bestd2 || (d2 == bestd2 && btri >= 0 && t/3 < btri)) {
                bestd2 = d2; bcx = sx; bcy = sy; btri = t/3;
                float nl = sqrtf(n[0]*n[0]+n[1]*n[1]);
                if (nl > 1e-9f) { fnx = n[0]/nl; fny = n[1]/nl; }
            }
        }
        if (touched) { if (zlo < ulo) ulo = zlo; if (zhi > uhi) uhi = zhi; }
    }
    if (btri < 0) return 0;
    float span = uhi - ulo;
    if (span < WALL_MIN_FACE_SPAN) return 0;     /* a seam, not a barrier */
    if (out) {
        out->mesh = mi; out->tri = btri; out->cx = bcx; out->cy = bcy;
        out->dist = sqrtf(bestd2); out->pen = r - out->dist; out->span = span;
        float ox = px - bcx, oy = py - bcy;
        if (out->dist > 1e-6f) { out->nx = ox / out->dist; out->ny = oy / out->dist; }
        else { out->nx = fnx; out->ny = fny; }   /* centre exactly on the face:
                                                    winding normal is all there is */
    }
    return 1;
}

int cw_probe_contact(const N2Scene *s, int mi, float px, float py,
                     float r, float cz0, float cz1) {
    return cw_mesh_feature(s, mi, px, py, r, cz0, cz1, NULL);
}

/* Resolution order is the obstacle order phys_collect_walls produced (mesh
   index order, stable across runs). Each contact is resolved against the
   position the previous one left behind, so overlapping walls compose instead
   of fighting; within one mesh the closest feature wins. */
int collide_walls(float *pos, float *vel, const float obst[][4],
                  const float obz[][2], int nobst, float r, float cz0, float cz1,
                  const N2Scene *scene, const int *src,
                  PhysWallContact *log, int maxlog) {
    int hits = 0;
    for (int o = 0; o < nobst; o++) {
        float x0=obst[o][0]-r, y0=obst[o][1]-r, x1=obst[o][2]+r, y1=obst[o][3]+r;
        if (pos[0]<=x0 || pos[0]>=x1 || pos[1]<=y0 || pos[1]>=y1) continue;
        /* vertical volumes must actually overlap for this to be a collision */
        if (obz && (obz[o][1] < cz0 || obz[o][0] > cz1)) continue;
        if (scene && src) {
            /* the rect was broad phase only: resolve against the FACE */
            PhysWallContact c;
            if (!cw_mesh_feature(scene, src[o], pos[0], pos[1], r, cz0, cz1, &c))
                continue;
            float vn = vel[0]*c.nx + vel[1]*c.ny;
            if (c.pen <= 0.0f && vn >= 0.0f) continue;   /* touching, not colliding:
                                                            a car resting against a
                                                            face is not a response */
            if (c.pen > 0.0f) { pos[0] += c.nx * c.pen; pos[1] += c.ny * c.pen; }
            if (vn < 0) { vel[0] -= vn*c.nx; vel[1] -= vn*c.ny; }  /* keep tangent */
            if (log && hits < maxlog) log[hits] = c;
            hits++;
            continue;
        }
        /* legacy AABB-only path, for callers with no scene to resolve against */
        float pl=pos[0]-x0, pr=x1-pos[0], pd=pos[1]-y0, pu=y1-pos[1], m=pl; int ax=0;
        if (pr<m){m=pr;ax=1;} if (pd<m){m=pd;ax=2;} if (pu<m){m=pu;ax=3;}
        if      (ax==0){ pos[0]=x0; if(vel[0]>0)vel[0]=0; }
        else if (ax==1){ pos[0]=x1; if(vel[0]<0)vel[0]=0; }
        else if (ax==2){ pos[1]=y0; if(vel[1]>0)vel[1]=0; }
        else           { pos[1]=y1; if(vel[1]<0)vel[1]=0; }
        hits++;
    }
    return hits;
}
void collide_walls_selftest(void) {
    float obst[1][4] = {{0,0,10,10}};
    float p[3]={5,5,0}, v[2]={1,1};
    assert(collide_walls(p, v, obst, NULL, 1, 1.0f, 0, 0, NULL, NULL, NULL, 0) == 1); /* deep inside -> resolved */
    assert(p[0]<=0 || p[0]>=10 || p[1]<=0 || p[1]>=10);    /* ...and now outside the box */
    float p2[3]={0.5f,5,0}, v2[2]={2,0};                   /* near left face, moving +x */
    collide_walls(p2, v2, obst, NULL, 1, 1.0f, 0, 0, NULL, NULL, NULL, 0);
    assert(p2[0] <= -1.0f + 1e-4f && v2[0] == 0.0f);       /* pushed left, +x vel killed */
    float p3[3]={100,100,0}, v3[2]={1,0};
    assert(collide_walls(p3, v3, obst, NULL, 1, 1.0f, 0, 0, NULL, NULL, NULL, 0) == 0); /* far outside */

    /* --- Z overlap gate (M95) --- */
    float obz[1][2] = {{204.4f, 210.5f}};                  /* the M94 slab */
    float q[3]={5,5,199.08f}, qv[2]={1,1};                 /* car 5.3 m BELOW it */
    assert(collide_walls(q, qv, obst, obz, 1, 1.0f, 199.08f, 200.55f, NULL, NULL, NULL, 0) == 0);
    assert(q[0]==5.0f && q[1]==5.0f && qv[0]==1.0f && qv[1]==1.0f);  /* untouched */
    float u[3]={5,5,215.0f}, uv[2]={1,1};                  /* car ABOVE it */
    assert(collide_walls(u, uv, obst, obz, 1, 1.0f, 215.0f, 216.5f, NULL, NULL, NULL, 0) == 0);
    /* XY + Z overlap still resolves exactly as the XY-only path does */
    float w1[3]={5,5,206.0f}, wv1[2]={1,1};
    float w2[3]={5,5,206.0f}, wv2[2]={1,1};
    int h1 = collide_walls(w1, wv1, obst, obz, 1, 1.0f, 206.0f, 207.5f, NULL, NULL, NULL, 0);
    int h2 = collide_walls(w2, wv2, obst, NULL, 1, 1.0f, 0, 0, NULL, NULL, NULL, 0);
    assert(h1 == 1 && h1 == h2);
    assert(w1[0]==w2[0] && w1[1]==w2[1] && wv1[0]==wv2[0] && wv1[1]==wv2[1]);
    /* touching spans count as overlapping (no gap) */
    float t1[3]={5,5,204.4f}, tv1[2]={1,1};
    assert(collide_walls(t1, tv1, obst, obz, 1, 1.0f, 202.9f, 204.4f, NULL, NULL, NULL, 0) == 1);
}

#define WALL_MIN_HEIGHT 2.5f    /* heuristic mesh below this = flat, not a wall */
#define WALL_MAX_SPAN   300.0f  /* skip oversized shells (sky domes etc.) */

/* Which scenery stops a car (Phase 65). Each mesh now carries its asset-name
 * class, so the decision is semantic instead of a pure height guess:
 *   BUILDING / WALL / STRUCT -> always a solid, immovable boundary
 *   TREE / TERRAIN           -> never a wall here (ground is the query's job)
 *   PROP  -> MEASURED, not assumed: the XO_ prefix mixes 1x1x1.5 m boxes
 *            (XO_IP_WBOX) with 24x29x36 m office blocks (XO_INDUSTRIALOFFICESA)
 *            and 7x8x26 m tower containers, so the prefix alone cannot decide.
 *            Height bands over L4RA's 939 props: <2 m 254, 2-4 274, 4-8 353,
 *            8-16 359, 16-32 166, >32 60. Street furniture is distinguished by a
 *            SMALL FOOTPRINT (poles/cans/boxes are thin), not by being short --
 *            a streetlight is 8 m tall but ~1 m wide. So a prop is solid when
 *            its shorter horizontal span reaches PROP_SOLID_SPAN; thinner props
 *            stay out of the AABB set for a future knock-down/rebound pass.
 * Unnamed meshes keep the old N2_OTHER height heuristic. */
#define PROP_SOLID_SPAN 3.0f
static int scen_is_wall(int sc) {
    return sc == N2_SC_BUILDING || sc == N2_SC_WALL || sc == N2_SC_STRUCT;
}
int phys_collect_walls(const N2Scene *s, float (*obst)[4], int *src,
                       float (*obz)[2], int max) {
    int nobst = 0;
    for (int i = 0; i < s->count && nobst < max; i++) {
        int sc = s->meshes[i].scen;
        int prop_check = 0;
        if (sc != N2_SC_NONE) {                 /* named: decide semantically */
            if (sc == N2_SC_TERRAIN) continue;              /* ground, never a wall */
            /* props, trees and unclassified: let measured size decide, so a tree
               cluster or a big container still blocks but a trunk/pole does not */
            if (!scen_is_wall(sc)) prop_check = 1;
        } else if (s->meshes[i].cat != N2_OTHER) continue;   /* unnamed fallback */
        if (s->meshes[i].nverts < 3) continue;
        float ox0=1e30f,oy0=1e30f,oz0=1e30f, ox1=-1e30f,oy1=-1e30f,oz1=-1e30f;
        for (int v=0;v<s->meshes[i].nverts;v++){ float *p=s->meshes[i].verts+v*5;
            if(p[0]<ox0)ox0=p[0]; if(p[0]>ox1)ox1=p[0];
            if(p[1]<oy0)oy0=p[1]; if(p[1]>oy1)oy1=p[1];
            if(p[2]<oz0)oz0=p[2]; if(p[2]>oz1)oz1=p[2]; }
        /* BUILDING/WALL/STRUCT are authored solid semantics, so their height
         * is not a classifier. In particular L4RA's XW_SANDSTONEBASE pieces
         * are only 0.548 m tall but have real 0.539 m vertical faces. Let the
         * geometric narrow phase distinguish those from a sub-0.30 m seam.
         * Keep the 2.5 m heuristic for props/unclassified meshes only. */
        if (!scen_is_wall(sc) && oz1-oz0 < WALL_MIN_HEIGHT) continue;
        if (ox1-ox0 > WALL_MAX_SPAN || oy1-oy0 > WALL_MAX_SPAN) continue;
        if (prop_check) {   /* thin street furniture: leave it drivable-through */
            float sx = ox1-ox0, sy = oy1-oy0, smin = sx < sy ? sx : sy;
            if (smin < PROP_SOLID_SPAN) continue;
        }
        obst[nobst][0]=ox0; obst[nobst][1]=oy0; obst[nobst][2]=ox1; obst[nobst][3]=oy1;
        if (obz) { obz[nobst][0]=oz0; obz[nobst][1]=oz1; }   /* same pass, already measured */
        if (src) src[nobst] = i;
        nobst++;
    }
    return nobst;
}

#define CAR_RADIUS 2.6f   /* car-to-car collision circle */

float phys_car_contacts(float carpos[3], float vel[2], float speed,
                        AiCar *ais, int nai) {
    const float MIN = CAR_RADIUS*2.0f;
    float thud = 0.0f;
    /* player vs AI: player pushed at full weight; AIs share the rest so they
       don't get shoved off their line too hard. */
    for (int k = 0; k < nai; k++) {
        float dx = ais[k].pos[0]-carpos[0], dy = ais[k].pos[1]-carpos[1];
        float d2 = dx*dx+dy*dy;
        if (d2 > 1e-4f && d2 < MIN*MIN) {
            float d = sqrtf(d2), push = (MIN - d);
            float ux = dx/d, uy = dy/d;
            carpos[0]    -= ux*push*0.5f; carpos[1]    -= uy*push*0.5f;
            ais[k].pos[0]+= ux*push*0.5f; ais[k].pos[1]+= uy*push*0.5f;
            vel[0]*=0.85f; vel[1]*=0.85f;   /* bump scrubs a little speed */
            float s = (speed<0?-speed:speed)/PHYS_MAXSPD;
            if (0.3f + s*0.5f > thud) thud = 0.3f + s*0.5f;
        }
    }
    for (int a = 0; a < nai; a++) for (int b = a+1; b < nai; b++) {
        float dx = ais[b].pos[0]-ais[a].pos[0], dy = ais[b].pos[1]-ais[a].pos[1];
        float d2 = dx*dx+dy*dy;
        if (d2 > 1e-4f && d2 < MIN*MIN) {
            float d = sqrtf(d2), push = (MIN - d)*0.5f, ux = dx/d, uy = dy/d;
            ais[a].pos[0]-=ux*push; ais[a].pos[1]-=uy*push;
            ais[b].pos[0]+=ux*push; ais[b].pos[1]+=uy*push;
        }
    }
    return thud;
}
