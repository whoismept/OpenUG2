/* frontend_draw.h — GL drawing adapter for the frontend state machine.
 * Requires an active GL context. Uses GLES2-compatible shaders.
 * Does NOT create a window or own the application loop.
 *
 * Lifecycle:
 *   fed_init()  → compile shader, upload quad VBO (call once, GL context live)
 *   fed_draw()  → render the current Fe state (call each frame)
 *   fed_free()  → release GL resources
 *
 * All GL state changes are delimited: viewport, blend, depth test and program
 * are saved and restored, so the caller's GL state is not corrupted. */
#ifndef OPENUG2_FRONTEND_DRAW_H
#define OPENUG2_FRONTEND_DRAW_H

#include "frontend.h"

/* Opaque drawing context. */
typedef struct FeDraw FeDraw;

/* Allocate and initialize GL resources. Returns NULL on failure. */
FeDraw *fed_init(void);

/* Draw the current Fe state into the current framebuffer.
 * vp_w/vp_h: viewport pixel size (for aspect-correct layout). */
void fed_draw(FeDraw *d, const Fe *fe, int vp_w, int vp_h);

/* Release GL resources and free memory. */
void fed_free(FeDraw *d);

#endif
