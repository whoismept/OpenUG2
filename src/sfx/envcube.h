/* envcube.h -- the live environment reflection.
 *
 * There is no environment texture in the shipped data: the slot exists and
 * nothing fills it, which is also why the game's graphics options expose a
 * reflection DETAIL and a reflection RATE -- both only mean something for
 * something rendered live. So it is rendered here: six faces at low
 * resolution, refreshed one face at a time so the cost is spread.
 *
 * What samples it is the car's reflection term, and only where the material
 * asks for reflection at all; the world never does.
 */
#ifndef SFX_ENVCUBE_H
#define SFX_ENVCUBE_H

#include "../render.h"

/* Create the cube at `size` per face. Returns 0 if it could not be made, in
   which case the shader falls back to its procedural night sphere. */
int    env_cube_init(int size);

/* The GL cube texture, for binding. */
GLuint env_cube_tex(void);

/* Forward and up vectors for face `f` (0..5), for building its view matrix. */
void   env_cube_face_dirs(int f, float *fwd, float *up);

/* Redirect drawing into face `f`; env_cube_end restores the screen and its
   viewport. */
void   env_cube_begin(int face);
void   env_cube_end(int w, int h);

#endif
