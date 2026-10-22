/* camera.c — gameplay camera. See camera.h and docs/CAMERA.md. */
#include "camera.h"
#include "nfsu2.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* ---- flagged defaults for the spec's unknowns (docs/CAMERA.md, DESIGN) ---- */
#define CAM_AIRBORNE_SCALE   1.3f   /* SPEC 6.4 / 6.8 */
/* SPEC 4 text says +-0.75 for the look-at z clamp, tv02 says +-1. lookK.z is 0, so
 * the clamp is never active and the choice has no effect (QUESTIONS.md). */
#define CAM_LOOK_Z_LIMIT     1.0f
#define CAM_E11_FWD         -10.0f  /* S1 */
#define CAM_E11_LAT          0.0f
#define CAM_E11_UP           5.0f
#define CAM_E11_PITCH        3003
#define CAM_S2_FWD          -5.0f   /* S2 */
#define CAM_S2_UP            1.5f
#define CAM_S2_PITCH         910
#define CAM_S3_FWD          -17.2f  /* S3, variant F only */
#define CAM_S3_UP            6.0f
#define CAM_S3_PITCH         5200
#define CAM_S3_SPLIT_FWD    -15.8f
#define CAM_S3_SPLIT_UP      4.4f
#define CAM_S3_SPLIT_PITCH   3540

/* ---- small vector helpers ---------------------------------------------- */
static float dot3(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void cross3(const float a[3], const float b[3], float o[3]) {
    float x = a[1]*b[2]-a[2]*b[1], y = a[2]*b[0]-a[0]*b[2], z = a[0]*b[1]-a[1]*b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
/* Returns the old length; leaves v alone when it is (near) zero. */
static float norm3(float v[3]) {
    float l = len3(v);
    if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    return l;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

const char *cam_view_name(int view) {
    static const char *n[CAM_NVIEW] = { "FAR", "CLOSE", "BUMPER", "DRIVER", "HOOD", "DRIFT" };
    return view >= 0 && view < CAM_NVIEW ? n[view] : "?";
}

/* ---- options (SPEC 2.2, 2.3) ----------------------------------------------- */
int cam_view_for_option(int opt) {
    switch (opt) {
    case CAM_OPT_BUMPER: return CAM_BUMPER;
    case CAM_OPT_HOOD:   return CAM_HOOD;
    case CAM_OPT_CLOSE:  return CAM_CLOSE;
    case CAM_OPT_FAR:    return CAM_FAR;
    case CAM_OPT_DRIFT:  return CAM_DRIFT;
    default:             return CAM_FAR;
    }
}
const char *cam_option_label(int opt) {
    static const char *n[CAM_NOPT] = { "Bumper", "Hood", "Close", "Far", "Super Far" };
    return opt >= 0 && opt < CAM_NOPT ? n[opt] : "Far";
}
int cam_option_cycle(int opt, int dir, int drift_allowed) {
    int max_opt = drift_allowed ? CAM_OPT_DRIFT : CAM_OPT_FAR;
    if (dir > 0) { opt++; if (opt > max_opt) opt = 0; }
    else if (dir < 0) { opt--; if (opt < 0) opt = max_opt; }
    return opt;
}
int cam_option_menu_step(int opt, int dir) {
    if (dir > 0) { opt++; if (opt > CAM_OPT_FAR) opt = 0; }
    else if (dir < 0) { opt--; if (opt < 0) opt = CAM_OPT_FAR; }
    return opt;
}
int cam_option_for_event(int opt, int drift_event) {
    if (drift_event) return CAM_OPT_DRIFT;
    return opt == CAM_OPT_DRIFT ? CAM_OPT_FAR : opt;
}
int cam_drift_allowed(int drift_event, int event_id) {
    return drift_event || (event_id >= 4174 && event_id <= 4179);
}

/* ---- settings file ------------------------------------------------------------ */
void cam_settings_defaults(CamSettings *s) {
    for (int i = 0; i < CAM_PLAYERS; i++) s->opt[i] = CAM_OPT_DEFAULT;
    s->mouse_look = 0;
}
int cam_settings_path(char *out, size_t cap) {
    const char *d = getenv("OPENUG2_CONFIG_DIR");
    if (d && *d) return snprintf(out, cap, "%s/camera.cfg", d) < (int)cap;
    d = getenv("XDG_CONFIG_HOME");
    if (d && *d) return snprintf(out, cap, "%s/openug2/camera.cfg", d) < (int)cap;
    d = getenv("HOME");
    if (d && *d) return snprintf(out, cap, "%s/.config/openug2/camera.cfg", d) < (int)cap;
    return 0;
}
int cam_settings_load(CamSettings *s, const char *path) {
    cam_settings_defaults(s);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int slot, v;
        if (sscanf(line, "camera_option_p%d=%d", &slot, &v) == 2 &&
            slot >= 1 && slot <= CAM_PLAYERS && v >= 0 && v < CAM_NOPT)
            s->opt[slot-1] = v;
        else if(sscanf(line,"right_mouse_look=%d",&v)==1 && (v==0 || v==1))
            s->mouse_look=v;
    }
    fclose(f);
    return 1;
}
static void mkdir_parents(const char *path) {
#ifndef _WIN32
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++)
        if (tmp[i] == '/') { tmp[i] = 0; mkdir(tmp, 0755); tmp[i] = '/'; }
#else
    (void)path;
#endif
}
int cam_settings_save(const CamSettings *s, const char *path) {
    if (!path) return 0;
    mkdir_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    for (int i = 0; i < CAM_PLAYERS; i++) fprintf(f, "camera_option_p%d=%d\n", i + 1, s->opt[i]);
    fprintf(f,"right_mouse_look=%d\n",s->mouse_look?1:0);
    return fclose(f) == 0;
}

/* ---- per-car data (SPEC 3) --------------------------------------------------- */
#define CAM_REC_COUNT0 0x720
#define CAM_REC_COUNT1 0x724
#define CAM_REC_SET0   0x730
#define CAM_REC_SET1   0x790
#define CAM_ENTRY_SIZE 16

static float rd_f32(const unsigned char *p) { float f; memcpy(&f, p, 4); return f; }

int cam_car_data_load(const unsigned char *g, long glen, const char *car, CamCarData *out) {
    if (!g || !car || !out) return 0;
    long at = n2_global_car_record(g, glen, car);
    if (at < 0 || at + N2_GLOBAL_CAR_STRIDE > glen) return 0;
    CamCarData d; memset(&d, 0, sizeof d);
    uint32_t c0 = n2_u32(g + at + CAM_REC_COUNT0), c1 = n2_u32(g + at + CAM_REC_COUNT1);
    if (c0 > CAM_SET_MAX || c1 > CAM_SET_MAX) return 0;
    d.count[0] = (int)c0; d.count[1] = (int)c1;
    for (int set = 0; set < 2; set++) {
        const unsigned char *b = g + at + (set ? CAM_REC_SET1 : CAM_REC_SET0);
        for (int i = 0; i < d.count[set]; i++) {
            const unsigned char *e = b + i * CAM_ENTRY_SIZE;
            CamEntry *o = &d.e[set][i];
            o->tag = e[0] | e[1] << 8;
            o->pitch_bau = (int16_t)(uint16_t)(e[2] | e[3] << 8);
            o->fwd = rd_f32(e + 4); o->up = rd_f32(e + 8); o->lat = rd_f32(e + 12);
            if (!isfinite(o->fwd) || !isfinite(o->up) || !isfinite(o->lat)) return 0;
        }
    }
    d.valid = 1;
    *out = d;
    return 1;
}

void cam_car_data_default(CamCarData *d) {
    /* Typical retail values (SPEC 3.3). Used for a car with no record. */
    static const CamEntry pri[6] = {
        { CAM_FAR,      0,   -5.8f,   1.7f,  0.0f },
        { CAM_CLOSE,    0,   -4.7f,   1.7f,  0.0f },
        { CAM_BUMPER,   500,  0.9f,   1.06f, 0.0f },
        { CAM_DRIVER,   0,   -0.195f, 0.805f,-0.44f },
        { CAM_HOOD,     0,    0.7f,   1.45f, 0.0f },
        { CAM_DRIFT,    3040,-5.8f,   3.45f, 0.0f },
    };
    static const CamEntry sec[6] = {
        { CAM_FAR,      0,   -8.2f,   1.85f, 0.0f },
        { CAM_CLOSE,    0,   -6.8f,   1.7f,  0.0f },
        { CAM_BUMPER,  -1342, 0.9f,   0.91f, 0.0f },
        { CAM_DRIVER,   0,   -0.26f,  0.772f,-0.315f },
        { CAM_HOOD,     0,    0.7f,   1.45f, 0.0f },
        { CAM_DRIFT,    2027,-8.2f,   3.45f, 0.0f },
    };
    memset(d, 0, sizeof *d);
    d->count[0] = d->count[1] = 6;
    memcpy(d->e[0], pri, sizeof pri);
    memcpy(d->e[1], sec, sizeof sec);
}

const CamEntry *cam_car_entry(const CamCarData *d, int set, int view) {
    if (!d || set < 0 || set > 1) return NULL;
    for (int i = 0; i < d->count[set] && i < CAM_SET_MAX; i++)
        if (d->e[set][i].tag == view) return &d->e[set][i];
    return NULL;
}

/* ---- tuning (SPEC 4) ------------------------------------------------------------ */
static const CamTuning k_tuning[CAM_NVIEW] = {
    /* FAR */ { 1.0f, 0.35f, 0.75f, 0.75f,
        {0.004f,0.004f,0}, {-1,-1,1}, {1,1,1},
        {-0.15f,0,0}, {-1.5f,-1,-1}, {1.25f,1,1},
        {-0.06f,0,0}, {-1.25f,0,-CAM_LOOK_Z_LIMIT}, {1.25f,0,CAM_LOOK_Z_LIMIT},
        {0.3f,0.3f,0.55f}, 13020 },
    /* CLOSE */ { 1.0f, 0.35f, 0.75f, 0.5f,
        {0.003f,0.003f,0}, {-1,-1,1}, {1,1,1},
        {-0.125f,0,0}, {-1.25f,-1,-1}, {1,1,1},
        {-0.06f,0,0}, {-1.25f,0,-CAM_LOOK_Z_LIMIT}, {1.25f,0,CAM_LOOK_Z_LIMIT},
        {0.3f,0.3f,0.5f}, 13020 },
    /* BUMPER */ { 0, 0, 0.75f, 0, {0,0,0},{0,0,0},{0,0,0}, {0,0,0},{0,0,0},{0,0,0},
        {0,0,0},{0,0,0},{0,0,0}, {0,0,0}, 16384 },
    /* DRIVER */ { 0, 0, 0.75f, 0, {0,0,0},{0,0,0},{0,0,0}, {0,0,0},{0,0,0},{0,0,0},
        {0,0,0},{0,0,0},{0,0,0}, {0,0,0}, 16384 },
    /* HOOD */ { 0.6f, 0.6f, 0.75f, 0.6f,
        {0,0,0}, {0,0,0}, {0,0,0},
        {-0.01f,0,0}, {-0.1f,-0.1f,-0.1f}, {0.2f,0.1f,0.1f},
        {0,0.005f,0}, {0,-0.08f,0}, {0,0.08f,0},
        {0.3f,0.3f,0.3f}, 16384 },
    /* DRIFT */ { 1.0f, 0.35f, 0.75f, 0.75f,
        {0.004f,0.004f,0}, {-1,-1,1}, {1,1,1},
        {-0.15f,0,0}, {-1.5f,-1,-1}, {1.25f,1,1},
        {-0.06f,0,0}, {-1.25f,0,-CAM_LOOK_Z_LIMIT}, {1.25f,0,CAM_LOOK_Z_LIMIT},
        {0.3f,0.3f,0.55f}, 13020 },
};
const CamTuning *cam_tuning(int view) {
    return &k_tuning[view >= 0 && view < CAM_NVIEW ? view : CAM_FAR];
}

/* ---- smoother (SPEC 5) ------------------------------------------------------------ */
void cam_smooth_set_target(CamSmooth *s, float x) {
    s->target = x;
    if (x != s->p) s->active = 1;
}
void cam_smooth_snap(CamSmooth *s) { s->p = s->target; s->v = 0; s->active = 0; }
void cam_smooth_step(CamSmooth *s, float dt) {
    if (!s->active) return;
    float A = 2.0f * (s->p - s->target) + s->v;
    float B = 3.0f * (s->target - s->p) - 2.0f * s->v;
    float u = s->T > 1e-5f ? dt / s->T : 1.0f;
    if (u >= 10.0f) { s->p = s->target; s->v = 0; s->active = 0; return; }
    float p = ((A * u + B) * u + s->v) * u + s->p;
    float v = (3.0f * A * u + 2.0f * B) * u + s->v;
    s->p = p; s->v = v;
}

/* ---- driving camera (SPEC 6) ---------------------------------------------------------- */
void cam_car_axes(float heading, const float up_in[3], CamCarFrame *f) {
    float up[3] = { up_in[0], up_in[1], up_in[2] };
    if (norm3(up) < 0.5f) { up[0] = up[1] = 0; up[2] = 1; }
    float fw[3] = { cosf(heading), sinf(heading), 0 };
    float d = dot3(fw, up);
    for (int i = 0; i < 3; i++) fw[i] -= d * up[i];
    norm3(fw);
    float l[3]; cross3(up, fw, l);
    memcpy(f->fwd, fw, sizeof fw); memcpy(f->left, l, sizeof l); memcpy(f->up, up, sizeof up);
}

void cam_init(CamState *s) {
    memset(s, 0, sizeof *s);
    s->requested = s->effective = CAM_FAR;
    s->snap = 1;
    s->variant_f = 0;               /* variant M (mirrored) is the retail default, SPEC 6.13 */
    cam_car_data_default(&s->car);
}
void cam_set_car(CamState *s, const CamCarData *d) {
    s->car = *d;
    s->snap = 1;
}
void cam_set_requested(CamState *s, int view) {
    if (view != s->requested) s->snap = 1;
    s->requested = view;
}
void cam_request_snap(CamState *s) { s->snap = 1; }
void cam_set_look_back(CamState *s, int on) { s->look_back = on ? 1 : 0; }

float cam_heading_weight(float speed) { return clampf((speed - 8.0f) / 5.0f, 0.0f, 1.0f); }

void cam_base_offsets(int view, int pitch_bau, float fwd, float lat, float up,
                      float off[3], float look[3]) {
    float t = tanf(CAM_BAU_TO_RAD(pitch_bau));
    off[0] = fwd; off[1] = lat; off[2] = up;
    if (cam_is_chase(view)) { look[0] = 0; look[1] = 0; look[2] = up + fwd * t; }
    else { look[0] = fwd + 1.0f; look[1] = 0; look[2] = up - t; }
}

void cam_targets(const CamTuning *k, int chase, int airborne, const float off[3],
                 const float look[3], const float aL[3], const float acc_world[3],
                 float eye_t[3], float look_t[3], float up_t[3]) {
    float sc = (chase && airborne) ? CAM_AIRBORNE_SCALE : 1.0f;
    for (int i = 0; i < 3; i++) {
        eye_t[i]  = clampf(k->eye_k[i]  * aL[i], k->eye_min[i],  k->eye_max[i])  + off[i] * sc;
        look_t[i] = clampf(k->look_k[i] * aL[i], k->look_min[i], k->look_max[i]) + look[i];
        float u = k->up_k[i] * acc_world[i] + (i == 2 ? 1.0f : 0.0f);
        up_t[i] = clampf(u, k->up_min[i], k->up_max[i]);
    }
}

static void heading_frame(const float H[3], const CamCarFrame *car, float X[3], float Y[3], float Z[3]) {
    memcpy(X, H, 3 * sizeof(float));
    if (norm3(X) < 1e-6f) memcpy(X, car->fwd, 3 * sizeof(float));
    Y[0] = -X[1]; Y[1] = X[0]; Y[2] = 0;                 /* normalize(Zworld x X) */
    if (norm3(Y) < 1e-6f) memcpy(Y, car->left, 3 * sizeof(float));
    cross3(X, Y, Z);
}

void cam_tick(CamState *s, const CamInput *in, float dt) {
    if (!s->started) {
        s->started = 1; s->snap = 1;
        s->t_ground = in->clock;
        s->effective = s->requested;
    }
    int airborne = in->wheels_down <= 2;
    if (in->wheels_down > 1) s->t_ground = in->clock;
    s->airborne = airborne;
    int set = in->split_screen ? 1 : 0;
    CamCarData fallback; int have_fallback = 0;

    /* 6.1 requested -> effective view */
    int eff = s->requested;
    if (eff == CAM_BUMPER || eff == CAM_HOOD) {
        int fb = 0;
        if (cam_is_chase(s->effective) && airborne) fb = 1;
        if (!fb && in->vehicle_s2) fb = 1;
        if (!fb && in->blocked) {
            const CamEntry *e = cam_car_entry(&s->car, set, eff);
            if (!e) { cam_car_data_default(&fallback); have_fallback = 1; e = cam_car_entry(&fallback, set, eff); }
            float eye[3], top[3];
            for (int i = 0; i < 3; i++)
                eye[i] = in->car.pos[i] + in->car.fwd[i] * e->fwd + in->car.left[i] * e->lat + in->car.up[i] * e->up;
            for (int i = 0; i < 3; i++) top[i] = in->car.pos[i] + 2.0f * in->car.up[i];
            fb = in->blocked(in->blocked_ctx, eye, in->car.pos, top);
        }
        if (fb) eff = CAM_FAR;
    }
    if (eff != s->effective) s->snap = 1;
    s->effective = eff;
    int chase = cam_is_chase(eff);

    /* 6.2 tuning */
    CamTuning k = *cam_tuning(eff);
    if (in->drift_event || in->event_e11) k.eye_k[0] = k.eye_k[1] = k.eye_k[2] = 0;

    /* 6.3 acceleration filter: mean of the last 4 samples */
    memcpy(s->acc_hist[s->acc_i], in->accel, sizeof in->accel);
    s->acc_i = (s->acc_i + 1) & 3;
    if (s->acc_n < 4) s->acc_n++;
    float acc[3] = {0, 0, 0};
    for (int n = 0; n < s->acc_n; n++)
        for (int i = 0; i < 3; i++) acc[i] += s->acc_hist[n][i];
    for (int i = 0; i < 3; i++) acc[i] /= (float)s->acc_n;

    /* 6.4 time constants */
    float t_eye = k.t_eye;
    if (chase && airborne) t_eye /= CAM_AIRBORNE_SCALE;
    for (int i = 0; i < 3; i++) {
        s->eye[i].T = t_eye; s->look[i].T = k.t_look;
        s->up[i].T = k.t_up; s->head[i].T = k.t_head[i];
    }
    s->fov.T = k.t_fov;

    /* 6.5 advance (order: up, fov, eye, look, heading) */
    for (int i = 0; i < 3; i++) cam_smooth_step(&s->up[i], dt);
    cam_smooth_step(&s->fov, dt);
    for (int i = 0; i < 3; i++) cam_smooth_step(&s->eye[i], dt);
    for (int i = 0; i < 3; i++) cam_smooth_step(&s->look[i], dt);
    for (int i = 0; i < 3; i++) cam_smooth_step(&s->head[i], dt);

    /* 6.6 heading target and frame */
    if (chase) {
        if (!(airborne && !s->snap)) {
            float vel[3] = { in->vel[0], in->vel[1], in->vel[2] };
            float d = dot3(vel, in->car.fwd);
            if (d < 0) for (int i = 0; i < 3; i++) vel[i] -= 2.0f * d * in->car.fwd[i];
            float w = cam_heading_weight(in->speed);
            if (norm3(vel) < 1e-6f) w = 0;     /* nothing to follow: use the car axis */
            float e = clampf(1.0f - (float)(in->clock - s->t_ground) / 2.0f, 0.0f, 1.0f);
            float h[3];
            for (int i = 0; i < 3; i++) h[i] = w * vel[i] + (1.0f - w) * in->car.fwd[i];
            h[2] *= e;
            for (int i = 0; i < 3; i++) cam_smooth_set_target(&s->head[i], h[i]);
        }
        if (s->snap) for (int i = 0; i < 3; i++) cam_smooth_snap(&s->head[i]);
    } else {
        for (int i = 0; i < 3; i++) {
            cam_smooth_set_target(&s->head[i], in->car.fwd[i]);
            cam_smooth_snap(&s->head[i]);
        }
    }
    float X[3], Y[3], Z[3];
    if (chase) {
        float H[3] = { s->head[0].p, s->head[1].p, s->head[2].p };
        heading_frame(H, &in->car, X, Y, Z);
    } else {
        memcpy(X, in->car.fwd, sizeof X); memcpy(Y, in->car.left, sizeof Y); memcpy(Z, in->car.up, sizeof Z);
    }

    /* 6.7 base eye offset and look-at */
    const CamEntry *e = cam_car_entry(&s->car, set, eff);
    if (!e) {
        if (!have_fallback) cam_car_data_default(&fallback);
        e = cam_car_entry(&fallback, set, eff);
    }
    int pitch = e->pitch_bau; float fwd = e->fwd, lat = e->lat, up = e->up;
    if (in->event_e11) {
        fwd = CAM_E11_FWD; lat = CAM_E11_LAT; up = CAM_E11_UP; pitch = CAM_E11_PITCH;
    } else if (s->look_back && s->variant_f) {
        lat = 0;
        if (set) { fwd = CAM_S3_SPLIT_FWD; up = CAM_S3_SPLIT_UP; pitch = CAM_S3_SPLIT_PITCH; }
        else     { fwd = CAM_S3_FWD;       up = CAM_S3_UP;       pitch = CAM_S3_PITCH; }
    } else if (in->vehicle_s2) {
        fwd = CAM_S2_FWD; lat = 0; up = CAM_S2_UP; pitch = CAM_S2_PITCH;
    }
    float off[3], look[3];
    cam_base_offsets(eff, pitch, fwd, lat, up, off, look);

    /* 6.8 targets */
    float aL[3] = {0, 0, 0};
    if (!in->accel_offsets_disabled) { aL[0] = dot3(acc, X); aL[1] = dot3(acc, Y); aL[2] = dot3(acc, Z); }
    float eye_t[3], look_t[3], up_t[3];
    cam_targets(&k, chase, airborne, off, look, aL, acc, eye_t, look_t, up_t);
    for (int i = 0; i < 3; i++) {
        cam_smooth_set_target(&s->eye[i], eye_t[i]);
        cam_smooth_set_target(&s->look[i], look_t[i]);
        cam_smooth_set_target(&s->up[i], up_t[i]);
    }
    cam_smooth_set_target(&s->fov, (float)k.fov_bau);

    /* 6.12 snapping */
    if (s->snap || eff == CAM_BUMPER || eff == CAM_DRIVER) {
        for (int i = 0; i < 3; i++) {
            cam_smooth_snap(&s->eye[i]); cam_smooth_snap(&s->look[i]); cam_smooth_snap(&s->up[i]);
        }
        cam_smooth_snap(&s->fov);
    }
    s->snap = 0;
}

void cam_pose(const CamState *s, const CamCarFrame *car, CamPose *o) {
    int eff = s->effective, chase = cam_is_chase(eff);
    float X[3], Y[3], Z[3];
    if (chase) {
        float H[3] = { s->head[0].p, s->head[1].p, s->head[2].p };
        heading_frame(H, car, X, Y, Z);
    } else {
        memcpy(X, car->fwd, sizeof X); memcpy(Y, car->left, sizeof Y); memcpy(Z, car->up, sizeof Z);
    }
    float k = (s->look_back && !s->variant_f) ? -1.0f : 1.0f;
    float el[3] = { k * s->eye[0].p, k * s->eye[1].p, s->eye[2].p };
    float ll[3] = { k * s->look[0].p, k * s->look[1].p, s->look[2].p };
    for (int i = 0; i < 3; i++) {
        o->eye[i]  = car->pos[i] + el[0] * X[i] + el[1] * Y[i] + el[2] * Z[i];
        o->look[i] = car->pos[i] + ll[0] * X[i] + ll[1] * Y[i] + ll[2] * Z[i];
    }
    if (chase) {
        o->up[0] = s->up[0].p; o->up[1] = s->up[1].p; o->up[2] = 1.0f;
        norm3(o->up);
    } else memcpy(o->up, car->up, sizeof o->up);
    o->fov_bau = (int)s->fov.p;
    o->view = eff;
    o->hide_car = eff == CAM_BUMPER || eff == CAM_DRIVER || (eff == CAM_HOOD && s->look_back);
    o->znear = eff == CAM_DRIVER ? 0.4f : 0.5f;
}

void cam_render_pose(const CamState *before, const CamState *after,
                     const CamCarFrame *car, float alpha, CamPose *out) {
    if (!isfinite(alpha) || alpha >= 1 || !before->started || before->snap || after->snap ||
        before->effective != after->effective || before->look_back != after->look_back ||
        before->variant_f != after->variant_f) {
        cam_pose(after, car, out);
        return;
    }
    float t = fmaxf(0, alpha);
    CamState display = *after;
    for (int i = 0; i < 3; i++) {
        display.head[i].p = before->head[i].p + t * (after->head[i].p - before->head[i].p);
        display.eye[i].p = before->eye[i].p + t * (after->eye[i].p - before->eye[i].p);
        display.look[i].p = before->look[i].p + t * (after->look[i].p - before->look[i].p);
        display.up[i].p = before->up[i].p + t * (after->up[i].p - before->up[i].p);
    }
    display.fov.p = before->fov.p + t * (after->fov.p - before->fov.p);
    cam_pose(&display, car, out);
}

void cam_mouse_look_begin(CamMouseLook *s,const CamPose *display,const CamCarFrame *car) {
    memset(s,0,sizeof *s);s->local=*display;
    float eye[3],look[3];
    for(int i=0;i<3;i++){eye[i]=display->eye[i]-car->pos[i];look[i]=display->look[i]-car->pos[i];}
    const float *axes[3]={car->fwd,car->left,car->up};
    for(int i=0;i<3;i++){
        s->local.eye[i]=dot3(eye,axes[i]);s->local.look[i]=dot3(look,axes[i]);
        s->local.up[i]=dot3(display->up,axes[i]);
    }
}

void cam_mouse_look_move(CamMouseLook *s,float dx,float dy) {
    if(!isfinite(dx)||!isfinite(dy))return;
    int chase=cam_is_chase(s->local.view);
    float v[3];for(int i=0;i<3;i++)v[i]=chase?s->local.eye[i]-s->local.look[i]:s->local.look[i]-s->local.eye[i];
    float elevation=atan2f(v[2],hypotf(v[0],v[1]));
    if(chase){
        s->yaw=fmodf(s->yaw-dx,6.28318530718f);
        s->pitch=clampf(s->pitch+dy,fminf(0,elevation)-elevation,1.5f-elevation);
    }else{
        s->yaw=clampf(s->yaw-dx,-.610865238f,.610865238f); /* 35 degrees */
        s->pitch=clampf(s->pitch-dy,0,.261799388f); /* 15 degrees up */
    }
}

void cam_mouse_look_pose(const CamMouseLook *s,const CamCarFrame *car,CamPose *out) {
    CamPose p=s->local;
    int chase=cam_is_chase(p.view);
    float *pivot=chase?p.look:p.eye,*end=chase?p.eye:p.look;
    float v[3];for(int i=0;i<3;i++)v[i]=end[i]-pivot[i];
    if(s->yaw!=0 || s->pitch!=0){
        float c=cosf(s->yaw),sn=sinf(s->yaw);
        float v0=v[0],u0=p.up[0];
        v[0]=c*v0-sn*v[1];v[1]=sn*v0+c*v[1];
        p.up[0]=c*u0-sn*p.up[1];p.up[1]=sn*u0+c*p.up[1];
        float axis[3]={v[1],-v[0],0};
        if(norm3(axis)>1e-8f){
            float cross[3];c=cosf(s->pitch);sn=sinf(s->pitch);
            cross3(axis,v,cross);float d=dot3(axis,v);
            for(int i=0;i<3;i++)v[i]=v[i]*c+cross[i]*sn+axis[i]*d*(1-c);
            cross3(axis,p.up,cross);d=dot3(axis,p.up);
            for(int i=0;i<3;i++)p.up[i]=p.up[i]*c+cross[i]*sn+axis[i]*d*(1-c);
        }
        for(int i=0;i<3;i++)end[i]=pivot[i]+v[i];
    }
    *out=p;
    for(int i=0;i<3;i++){
        out->eye[i]=car->pos[i]+car->fwd[i]*p.eye[0]+car->left[i]*p.eye[1]+car->up[i]*p.eye[2];
        out->look[i]=car->pos[i]+car->fwd[i]*p.look[0]+car->left[i]*p.look[1]+car->up[i]*p.look[2];
        out->up[i]=car->fwd[i]*p.up[0]+car->left[i]*p.up[1]+car->up[i]*p.up[2];
    }
}

/* ---- projection (SPEC 7, 7.1) --------------------------------------------------------------- */
/* SPEC 7 exactly, for a viewport whose height/width ratio is `aspect`. */
static void cam_scales_original(int fov_bau, float aspect, int main_view, float *xs, float *ys) {
    float s0 = main_view ? 1.1f : 1.0f;
    float s1 = main_view ? 1.1f * 1.15f * aspect : aspect;
    /* The half angles are whole BAU (SPEC 1: angles are 16-bit); the vectors in
     * tv09 only reproduce with truncation (docs/CAMERA.md D9). */
    int hx = (int)(s0 * (float)fov_bau * 0.5f), hy = (int)(s1 * (float)fov_bau * 0.5f);
    /* The vertical half angle grows with height/width, so a narrow viewport pushes it past 90 degrees,
     * where tan() turns negative and the picture flips (and near 90 it collapses to a sliver). SPEC 7 is
     * only defined for ordinary window shapes (no test vector reaches 46 degrees), so a limit below 90
     * changes none of them and keeps very narrow windows right side up. */
    const int max_half = (int)(CAM_BAU_PER_TURN * (80.0f / 360.0f));
    if (hx > max_half) hx = max_half;
    if (hy > max_half) hy = max_half;
    *xs = 1.0f / tanf(CAM_BAU_TO_RAD(hx));
    *ys = 1.0f / tanf(CAM_BAU_TO_RAD(hy));
}
int cam_projection_policy_by_name(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "original")) return CAM_PROJ_ORIGINAL;
    if (!strcmp(name, "wide")) return CAM_PROJ_WIDE;
    return -1;
}
void cam_projection_scales(int policy, int fov_bau, int w, int h, int main_view, float *xs, float *ys) {
    float aspect = (w > 0 && h > 0) ? (float)h / (float)w : 0.75f;
    if (policy == CAM_PROJ_WIDE && w > 0 && h > 0 && 3 * (long)w > 4 * (long)h) {
        /* SPEC 7.1: 4:3 result is the reference; only the horizontal scale shrinks by R/A
         * (no further truncation); the vertical view is unchanged (Hor+). */
        cam_scales_original(fov_bau, 0.75f, main_view, xs, ys);
        *xs = (float)((double)*xs * (4.0 * (double)h) / (3.0 * (double)w));
        return;
    }
    cam_scales_original(fov_bau, aspect, main_view, xs, ys);
}
void cam_projection(int policy, int fov_bau, int w, int h, int main_view, float zn, float zf, float m[16]) {
    float xs, ys;
    cam_projection_scales(policy, fov_bau, w, h, main_view, &xs, &ys);
    memset(m, 0, 16 * sizeof(float));
    m[0] = xs; m[5] = ys;
    m[10] = (zf + zn) / (zn - zf); m[11] = -1.0f; m[14] = 2.0f * zf * zn / (zn - zf);
}
void cam_view_matrix(const float eye[3], const float look[3], const float up_in[3], float m[16]) {
    float f[3] = { look[0]-eye[0], look[1]-eye[1], look[2]-eye[2] };
    if (norm3(f) < 1e-6f) { f[0] = 1; f[1] = f[2] = 0; }
    float up[3] = { up_in[0], up_in[1], up_in[2] };
    if (norm3(up) < 1e-6f) { up[0] = up[1] = 0; up[2] = 1; }
    float s[3]; cross3(f, up, s);
    if (norm3(s) < 1e-6f) { float z[3] = {0, 0, 1}; cross3(f, z, s); if (norm3(s) < 1e-6f) { s[0] = 0; s[1] = -1; s[2] = 0; } }
    float u[3]; cross3(s, f, u);
    float r[16] = { s[0], u[0], -f[0], 0,  s[1], u[1], -f[1], 0,  s[2], u[2], -f[2], 0,
        -dot3(s, eye), -dot3(u, eye), dot3(f, eye), 1 };
    memcpy(m, r, sizeof r);
}

/* ---- shake (SPEC 8) -------------------------------------------------------------------------- */
static const struct { const char *name; float fq, rough; } k_surf[CAM_NSURF] = {
    {"NONE",1,0}, {"ROAD",1,0}, {"ROAD_WET",1,0}, {"ROAD_DRIFT",1,0},
    {"ROAD_SMOKE_1",1,0}, {"ROAD_SMOKE_2",1,0}, {"ROAD_SMOKE_3",1,0}, {"ROAD_BRIDGE",1,0},
    {"DIRT",1,0.05f}, {"GRAVEL",1,0.02f}, {"ROUGH_ROAD",1,0.02f}, {"COBBLESTONE",3,0.03f},
    {"STAIRS",3,0.048f}, {"PUDDLE",1,0.01f}, {"DEEP_WATER",1,0.03f}, {"GRASS",1,0.05f},
    {"SIDEWALK",1.1f,0.03f}, {"WOOD",1,0.01f}, {"PLASTIC",1,0.01f}, {"GLASS",1,0.01f},
    {"SOLID_WALL",1,0.01f}, {"SEE_THROUGH_WALL",1,0.01f}, {"PLANT",1,0}, {"POST",1,0},
    {"PILLAR",1,0}, {"METAL_GRATE",1,0}, {"METAL",1,0}, {"CHAINLINK",1,0}, {"CAR",1,0},
};
void cam_surface_params(int surf, float *fq, float *rough) {
    if (surf < 0 || surf >= CAM_NSURF) surf = CAM_SURF_NONE;
    *fq = k_surf[surf].fq; *rough = k_surf[surf].rough;
}
const char *cam_surface_name(int surf) {
    return surf >= 0 && surf < CAM_NSURF ? k_surf[surf].name : "?";
}
int cam_surface_by_name(const char *name) {
    for (int i = 0; i < CAM_NSURF; i++) if (!strcmp(k_surf[i].name, name)) return i;
    return -1;
}

double cam_lattice(int32_t i) {
    uint32_t m = ((uint32_t)i << 13) ^ (uint32_t)i;
    uint32_t v = (m * (m * m * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return 1.0 - (double)v / 1073741824.0;
}
double cam_noise(double x) {
    double fl = floor(x), t = x - fl;
    int32_t i = (int32_t)fl;
    double a = cam_lattice(i), b = cam_lattice(i + 1);
    return a + (b - a) * t;
}
double cam_fbm(double x) {
    double sum = 0, amp = 1, f = 1;
    for (int k = 0; k < 6; k++) { sum += amp * cam_noise(f * x); amp *= 0.5; f *= 2.0; }
    return sum;
}

void cam_shake_params(float speed, float vmax, const int surf[4], int drag_event,
                      int nitrous, int stopped, CamShakeParams *o) {
    static const float curve[5][4] = {
        {0.0f, 0.0f, 1.0f, 1.0f}, {0.1f, 0.5f, 1.2f, 1.1f}, {0.3f, 0.8f, 1.5f, 1.2f},
        {0.6f, 1.0f, 2.0f, 1.3f}, {1.0f, 1.0f, 3.0f, 1.4f},
    };
    static const float f1[4] = {7.5f, 6.875f, 6.125f, 5.0f}, a1[4] = {0.003f, 0.002f, 0.7f, 0.5f};
    static const float f2[4] = {3.0f, 5.0f, 7.0f, 5.5f},     a2[4] = {0.007f, 0.01f, 0.6f, 0.8f};
    if (vmax <= 0) vmax = 100.0f;
    float x = clampf(4.0f * speed / vmax, 0.0f, 4.0f);
    int i = (int)floorf(x); if (i > 3) i = 3;
    float fr = x - (float)i, r[4];
    for (int j = 0; j < 4; j++) r[j] = curve[i][j] + (curve[i+1][j] - curve[i][j]) * fr;
    float A = r[0] * (drag_event ? 1.0f : 0.2f), F = r[2];
    if (nitrous && !stopped) { A += 0.2f; F *= 1.4f; }
    float fq = 0, rough = 0;
    for (int w = 0; w < 4; w++) { float q, g; cam_surface_params(surf[w], &q, &g); fq += q; rough += g; }
    fq *= 0.25f; rough *= 0.25f;
    float G = r[3] * fq, R = r[1] * 10.0f * rough;
    for (int j = 0; j < 4; j++) {
        o->freq1[j] = f1[j] * F; o->amp1[j] = a1[j] * A;
        o->freq2[j] = f2[j] * G; o->amp2[j] = a2[j] * R;
    }
}
void cam_shake_values(const CamShakeParams *p, double t, float c[4]) {
    for (int i = 0; i < 4; i++)
        c[i] = (float)(cam_fbm(p->freq1[i] * t) * p->amp1[i] + cam_fbm(p->freq2[i] * t) * p->amp2[i]);
}
float cam_shake_angle(float deg, int fov_bau) {
    int bau = (int)(deg * (CAM_BAU_PER_TURN / 360.0f));     /* truncation toward zero */
    double a = (double)bau * (6.283185307179586 / CAM_BAU_PER_TURN);
    double half = (double)fov_bau * (3.141592653589793 / CAM_BAU_PER_TURN);
    double s = sin(a) * sin(half) / sin(3.141592653589793 / 6.0);
    if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
    return (float)asin(s);
}
static void mat4_mul(const float *a, const float *b, float *o) {   /* column-major o = a*b */
    float r[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k*4+j] * b[i*4+k];
        r[i*4+j] = s;
    }
    memcpy(o, r, sizeof r);
}
void cam_shake_apply(const float c[4], int fov_bau, float view[16]) {
    /* Camera space: +x right, +y down, +z forward (SPEC 1). Rotations about x
     * (pitch, first) and y (yaw), then the translation. Converted to the GL view
     * convention (y up, -z forward) by conjugating with diag(1,-1,-1). */
    float pa = cam_shake_angle(c[2], fov_bau), ya = cam_shake_angle(c[3], fov_bau);
    float cp = cosf(pa), sp = sinf(pa), cy = cosf(ya), sy = sinf(ya);
    float rx[16] = { 1,0,0,0,  0,cp,sp,0,  0,-sp,cp,0,  0,0,0,1 };
    float ry[16] = { cy,0,-sy,0,  0,1,0,0,  sy,0,cy,0,  0,0,0,1 };
    float t[16]  = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  c[0],c[1],0,1 };
    float s[16]; mat4_mul(ry, rx, s); mat4_mul(t, s, s);
    float cv[16] = { 1,0,0,0,  0,-1,0,0,  0,0,-1,0,  0,0,0,1 };
    mat4_mul(cv, s, s); mat4_mul(s, cv, s);
    mat4_mul(s, view, view);
}

/* ---- collision helpers ------------------------------------------------------------------------ */
int cam_raise_over_sphere(float eye[3], const float c[3], float r, float margin) {
    float R = r + margin;
    float dx = eye[0] - c[0], dy = eye[1] - c[1], dz = eye[2] - c[2];
    if (dx * dx + dy * dy + dz * dz >= R * R) return 0;      /* already outside */
    eye[2] = c[2] + sqrtf(R * R - dx * dx - dy * dy);
    return 1;
}
int cam_point_near_box(const float p[3], const float box_pos[3], float heading,
                       float hl, float hw, float height, float margin) {
    float co = cosf(heading), sn = sinf(heading);
    float dx = p[0] - box_pos[0], dy = p[1] - box_pos[1];
    float lx = co * dx + sn * dy, ly = -sn * dx + co * dy;
    float ex = fmaxf(0, fabsf(lx) - hl), ey = fmaxf(0, fabsf(ly) - hw);
    float ez = p[2] < box_pos[2] ? box_pos[2] - p[2] : (p[2] > box_pos[2] + height ? p[2] - box_pos[2] - height : 0);
    return ex * ex + ey * ey + ez * ez < margin * margin;
}
