#ifndef WORLD_CAPTURE_POLICY_H
#define WORLD_CAPTURE_POLICY_H

#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    WORLD_SPAWN_INVALID = -1,
    WORLD_SPAWN_AUTHORED = 0,
    WORLD_SPAWN_EXPLICIT = 1
};

/* Parse the public --spawn contract without assigning a bundle-specific
 * coordinate. "start" deliberately leaves xy untouched: the selected STREAM
 * bundle derives its own authored focus after argument parsing. */
static inline int world_spawn_parse(const char *value, float xy[2]) {
    if (!value || !xy) return WORLD_SPAWN_INVALID;
    if (!strcmp(value, "start")) return WORLD_SPAWN_AUTHORED;
    float x = 0.0f, y = 0.0f;
    char tail = 0;
    if (sscanf(value, "%f,%f%c", &x, &y, &tail) != 2 ||
        !isfinite(x) || !isfinite(y)) return WORLD_SPAWN_INVALID;
    xy[0] = x;
    xy[1] = y;
    return WORLD_SPAWN_EXPLICIT;
}

/* GL-free policy seam for explicit instance-world evidence captures. The
 * explicit world2 focus is never a legacy-showcase candidate; only a shot with
 * an explicit heading becomes a frozen, fixed-camera capture. */
typedef struct {
    int preserve_explicit_pose;
    int fixed_camera;
    int freeze_motion;
} WorldCapturePolicy;

static inline WorldCapturePolicy world_capture_policy(int world2, int has_shot,
                                                       int static_shot,
                                                       int heading_set) {
    int fixed_capture = world2 && has_shot && !static_shot && heading_set;
    WorldCapturePolicy policy = {world2 != 0, fixed_capture, fixed_capture};
    return policy;
}

#endif
