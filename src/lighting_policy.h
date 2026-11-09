/* lighting_policy.h -- which lights the renderer draws, from the graphics settings.
 *
 * Header-only and GL-free: tools/settings_test.c drives it directly. */
#ifndef OPENUG2_LIGHTING_POLICY_H
#define OPENUG2_LIGHTING_POLICY_H

#include "settings.h"

typedef struct {
    int original;    /* original game's projected headlight pool instead of per-pixel beams */
    int headlight;   /* Car Headlight: beams or pool light the road; off leaves lit lenses */
    int glow;        /* Light Glow: lamp halos and district flares */
} LightingPolicy;

/* Level Of Detail 1 and 2 take the original manner. Level 1's preset also turns
 * Car Headlight and Light Glow off, as the original does. A level of 0 means no
 * saved setting: every value then reads 0, so the full lighting is kept rather
 * than switching the headlights off for everyone who never opened the menu. */
static LightingPolicy lighting_policy(const StGraphics *g) {
    LightingPolicy p = { 0, 1, 1 };
    int level = g ? g->v[ST_GFX_PERF_LEVEL] : 0;
    if (level <= 0) return p;
    p.original = level <= 2;
    p.headlight = g->v[ST_GFX_CAR_HEADLIGHT] != 0;
    p.glow = g->v[ST_GFX_LIGHT_GLOW] != 0;
    return p;
}

#endif
