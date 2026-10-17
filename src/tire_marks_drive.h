/* Adapter: per-wheel tire-mark inputs (TM-2) from the coupled Stage1-5 driving
 * state. Header-only so tire_marks.c stays independent of the drive modules.
 * Reads only values the frame has already published; changes none. */
#ifndef OPENUG2_TIRE_MARKS_DRIVE_H
#define OPENUG2_TIRE_MARKS_DRIVE_H
#include "drive_stage1.h"
#include "tire_marks.h"

/* TM-2 vehicle-level gate G, the conjunction of clause 1 (wheel effects enabled
 * for this vehicle: the host's contact model is ready and prepared, the wheel
 * collider exists, and no session-phase suspension is applied), clause 2 (not the
 * special mover mode = alternate_movement, aero disabled), and clause 3 (the current
 * venue id is not in the reserved Organic-Drag block; `venue_id` is the host's current
 * global game-state code, the event's route id, or 4000 for free roam).
 *
 * Clause 1 (TM-2, amended): the pre-race start phase INCLUDING the countdown applies a
 * session-phase suspension to every car in the field, lifted when the cars are put into
 * play at race start. The host carries that phase as DRIVE_SESSION_COUNTDOWN, so both it
 * and the general no-dynamics DRIVE_SESSION_SUSPENDED make clause 1 false. */
static inline int tm_drive_gate(const Stage1Drive *s, int venue_id) {
    int clause1 = s->contact_ready && s->prepared &&
                  s->session.phase != DRIVE_SESSION_SUSPENDED &&
                  s->session.phase != DRIVE_SESSION_COUNTDOWN;
    return tm_vehicle_gate(clause1, !s->alternate_movement, venue_id);
}

/* Fills the four wheels' TmWheelIn and the vehicle origin O = P - B*c.
 * width = tyre widths from the car record (tm_car_read); mask = per-wheel
 * suppression mask (TM-U3; 0 = none). */
static inline void tm_drive_inputs(const Stage1Drive *s, const float width[4], unsigned mask,
                                   TmWheelIn out[4], float O[3]) {
    const CtPose *p = &s->contact_pose;
    const float *c = s->contacts.car.centre;
    for (int k = 0; k < 3; k++)
        O[k] = (float)(p->position[k] - (double)p->axes.forward[k] * c[0]
                       - (double)p->axes.lateral[k] * c[1] - (double)p->axes.up[k] * c[2]);
    for (int i = 0; i < 4; i++) {
        const CtWheel *w = &s->contacts.wheel[i];
        ResBasis axes; ct_wheel_axes(&p->axes, s->wheel_angle[i], &axes);
        TmWheelIn *o = &out[i];
        o->contact = w->contact;
        o->masked = (mask >> i) & 1u;
        o->sliding = !s->drivetrain.wheel[i].gripping;
        o->u = s->drivetrain.wheel[i].utilisation;
        o->N = w->load;
        /* U = V + Omega x (B r), world frame (S4-4/S5-3) */
        o->U[0] = p->velocity[0] + p->omega[1] * w->arm[2] - p->omega[2] * w->arm[1];
        o->U[1] = p->velocity[1] + p->omega[2] * w->arm[0] - p->omega[0] * w->arm[2];
        o->U[2] = p->velocity[2] + p->omega[0] * w->arm[1] - p->omega[1] * w->arm[0];
        for (int k = 0; k < 3; k++) { o->C[k] = w->centre[k]; o->l[k] = axes.lateral[k]; }
        o->surface = w->surface;
        o->width = width[i];
    }
}
#endif
