/* post.h -- the frame's colour treatment.
 *
 * Drawing produces a raw scene; this is what makes it look like evening. The
 * frame is rendered into a texture, a blurred copy of its brightest parts is
 * added back in a warm tint, and the whole is desaturated slightly towards
 * that tint.
 *
 * Kept out of render.c on purpose: that file owns GPU upload, batching and the
 * scene shaders, and a screen-space effect is a separate concern that happens
 * after all of them.
 */
#ifndef SFX_POST_H
#define SFX_POST_H

#include "../render.h"

/* Size (or resize) the offscreen target. Returns 0 if it could not be made,
   in which case the caller should simply draw straight to the screen. */
int  pp_init(int w, int h);

/* Redirect this frame into the target. Call before clearing. */
void pp_begin(void);

/* Resolve the target to the screen. `amount` scales the whole effect, so 0
   gives back the untouched frame. Call after everything is drawn and before
   any glReadPixels, or a screenshot captures the untoned intermediate. */
void pp_end_and_draw(GpuMesh *quad, float amount);

#endif
