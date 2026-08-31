#ifndef WORLD_CAPTURE_POLICY_H
#define WORLD_CAPTURE_POLICY_H

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
