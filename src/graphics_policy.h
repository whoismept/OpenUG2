/* graphics_policy.h -- what the renderer draws, from the graphics settings.
 *
 * Header-only and GL-free: tools/settings_test.c drives it directly. */
#ifndef OPENUG2_GRAPHICS_POLICY_H
#define OPENUG2_GRAPHICS_POLICY_H

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

/* Car Reflection, Car Shadow and Road Reflection Detail. apply is 0 when there is
 * no saved setting, so the built-in defaults stay. Reflection Update Rate 0 skips
 * the scene capture (the preset for Level Of Detail 1 and 2), 1..3 refresh a face
 * every third frame, every other frame or every frame; Car Reflection Detail picks
 * 64 or 128 px faces. Car Shadow 0 draws the square footprint and 1..2 the
 * projected body and wheel shadows. Road Reflection Detail 0..1 keeps the
 * surface highlights only; 2 adds the wet-road scene reflections and 3 traces
 * them with twice the steps. */
typedef struct {
    int apply;
    int car_reflection_rate, car_reflection_detail;
    int car_shadow;
    int road_reflections, road_reflection_quality;
} DetailPolicy;

static DetailPolicy detail_policy(const StGraphics *g) {
    DetailPolicy p = { 0, 2, 0, 1, 0, 1 };
    int level = g ? g->v[ST_GFX_PERF_LEVEL] : 0;
    if (level <= 0) return p;
    int rate = g->v[ST_GFX_CAR_ENV_MAP], road = g->v[ST_GFX_ROAD_REFLECTION];
    p.apply = 1;
    p.car_reflection_rate = rate < 0 ? 0 : rate > 3 ? 3 : rate;
    p.car_reflection_detail = g->v[ST_GFX_CAR_ENV_MAP_UPDATE] != 0;
    p.car_shadow = g->v[ST_GFX_CAR_SHADOW] > 0;
    p.road_reflections = road >= 2;
    p.road_reflection_quality = road >= 3 ? 2 : 1;
    return p;
}

#endif
