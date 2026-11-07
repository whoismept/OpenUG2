/* tex_override.h -- optional HD texture overrides loaded from user DDS files.
 *
 * Off by default. Only `--tex-overrides DIR` enables it; `--no-tex-overrides`
 * or OPENUG2_TEX_OVERRIDES=0 keep it off. Without an indexed file every entry
 * point returns at once and issues no GL call, so default rendering is
 * byte-identical to a build without this module. Usage: docs/TEXTURE_OVERRIDES.md.
 *
 * File naming: KEY8HEX[@PACK][_anything].dds, anywhere under DIR (recursion
 * depth <= 8; subfolders are only for organisation).
 *   KEY   32-bit bin hash already used as the TPK record key, 8 hex digits in
 *         either case. Names are not used: many are truncated in the data.
 *   PACK  optional, case-insensitive: binds the file to one source -- a world
 *         region stem (e.g. STREAML4RA), LOC4DYNTEX, INGAMECOMMON, a car name
 *         (covers its GLOBALB fallbacks), WHEELS or VINYLS. Master-region and
 *         body-kit textures pass no pack, so only plain KEY files apply there.
 * Minimaps may instead use their record name: TRACKMAP4144[_anything].dds.
 * Lookup: KEY@PACK for the requesting pack, then plain KEY. A file bound to a
 * different pack is never applied (logged once as "not applied"); a rejected
 * KEY@PACK falls through to plain KEY. Same key and pack twice: the first path
 * in sort order wins, the other is logged as "duplicate".
 *
 * Validation (files are untrusted; every read is bounded by the file size):
 *   - DDS magic + 124-byte header; DX10 header must be one 2D texture; cube
 *     and volume textures are rejected.
 *   - Formats: DXT1/BC1, DXT3/BC2 (+DXT2), DXT5/BC3 (+DXT4), DX10 BC1-3 incl.
 *     sRGB variants (uploaded linear, like the originals), RGBA8/BGRA8/BGRX8
 *     and 32-bit legacy masks. BC7 and everything else are rejected.
 *   - Width/height powers of two, <= min(4096, caps.max_texture_size), exactly
 *     1x, 2x, 4x or 8x the ORIGINAL on both axes.
 *   - Only whole mip levels inside the file are used (at most 16); no whole
 *     base level = reject. The file is re-read at upload and length-checked.
 *   - An original with alpha/usage/blend needs an override format with alpha;
 *     a blended original rejects DXT1.
 *
 * The caller always decodes the ORIGINAL first and passes it in. Draw mode,
 * noise gate, alpha policy and HUD measurements stay on the original record.
 * Any problem (header, truncation, scale, alpha, GL error, budget) logs a
 * "TEXOVR skip" line once and returns 0, and the caller uploads the original
 * exactly as before.
 *
 * Upload: S3TC when available, otherwise each level is CPU-decoded to RGBA8.
 * A complete chain (down to 1x1) is uploaded as is; a partial chain gets
 * regenerated mips on the RGBA path and base-only LINEAR when compressed.
 *
 * Budget: resident GPU bytes of live override textures (names deleted by
 * their owners are pruned with glIsTexture). Over budget, the upload starts at
 * the mip level that matches the original size ("budget-reduced"); if that
 * does not fit either, the original is used. Not sticky: a later request can
 * succeed after memory is freed. */
#ifndef OPENUG2_TEX_OVERRIDE_H
#define OPENUG2_TEX_OVERRIDE_H

#include <stdint.h>
#include "render.h"

/* Scan DIR once (headers only). Call after the GL context exists and after
   g_tex_s3tc is set. budget_mib <= 0 selects the default (1536 MiB).
   Returns the number of indexed override files (0 = disabled). */
int    tex_override_init(const char *dir, int budget_mib);
/* Try the override for `key`. `pack` may be NULL. Returns a new bound
   texture (GL_REPEAT, trilinear, aniso, tracked) or {0} = use the original. */
RhiTexture tex_override_upload(uint32_t key, const char *pack, const N2Tex *orig);
/* One "TEXOVR stats" line: indexed / used / skipped / budget-reduced files
   and resident MiB. Prints nothing when disabled. */
void   tex_override_stats(void);
/* Free the index. Textures stay with the callers that own them. */
void   tex_override_shutdown(void);

#endif
