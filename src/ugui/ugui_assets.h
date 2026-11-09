/* ugui_assets.h -- loads the original frontend from the user's installation
 * at run time (SPEC 14, 15, 16, 17). Nothing from the install is copied into
 * the repository. GL-free. */
#ifndef UGUI_ASSETS_H
#define UGUI_ASSETS_H

#include "ugui_bundle.h"
#include "ugui_canvas.h"
#include "ugui_face.h"
#include "ugui_fng.h"
#include "ugui_tex.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t key; char *name; uint8_t *data; size_t len; int source; } UgAssetPackage;

typedef struct UgAssets {
    char dir[512];
    char language[32];
    UgBuf files[8]; int nfiles;
    char file_names[8][64];
    UgTexLib tex;
    UgAssetPackage *pk; int npk, pk_cap;
    UgFace *face; int nface, face_cap;
    UgStrTab strings;
    int have_strings;        /* nonempty table from the requested LANGUAGES file */
    int main_face;            /* index of the main UI font in face[] or -1 */
    int n_global_tex, n_front_tex;
    char log[1024];           /* human-readable summary / warnings */
} UgAssets;

/* Opens the game data directory `dir` (the install root). `language` may be
 * NULL (English). Returns 1 when at least the screens were found, 0 when the
 * directory holds nothing usable (message in `msg`). Missing optional files
 * only add a warning to a->log. */
int  ug_assets_open(UgAssets *a, const char *dir, const char *language, char *msg, size_t msglen);
/* Also load the common in-game screens (pause menu, results and HUD). */
int  ug_assets_open_ingame(UgAssets *a, const char *dir, const char *language, char *msg, size_t msglen);
void ug_assets_close(UgAssets *a);

/* Chooses the data directory: explicit argument, then $UGUI_DATA / $NFSU2_DATA,
 * then the current directory. Returns the first that looks like an install
 * (has FRONTEND or GLOBAL), or NULL. `out` receives the path. */
const char *ug_assets_find_dir(const char *explicit_dir, char *out, size_t outlen);

const UgAssetPackage *ug_assets_package(const UgAssets *a, const char *file_name);  /* "UI_Main.fng" */
const UgAssetPackage *ug_assets_package_by_key(const UgAssets *a, uint32_t key);
int  ug_assets_package_count(const UgAssets *a);
const UgAssetPackage *ug_assets_package_at(const UgAssets *a, int i);               /* sorted by name */
/* Font by resource name ("ConduitMdITC_TT21i.ffn"); NULL when the requested original face is unavailable. */
const UgFace *ug_assets_face(const UgAssets *a, const char *res_name);
const UgFace *ug_assets_main_face(const UgAssets *a);
/* Texture library index for an image resource name ("X.tga"), or -1. */
int  ug_assets_texture_for(const UgAssets *a, const char *res_name);
int  ug_assets_texture_by_name(const UgAssets *a, const char *base_name);
/* Looks the kit's art roles up in the loaded texture packs by resource name
 * (the names are keys into the user's install, no art is embedded). Returns how
 * many of the roles resolved; unresolved ones stay 0 (omitted). */
int  ug_skin_resolve(UgSkin *skin, const UgAssets *a);
/* Textures whose stored name starts with `prefix` (case-insensitive), as texture
 * library indices + 1, sorted by name; returns the count written (max `cap`). */
int  ug_assets_textures_with_prefix(const UgAssets *a, const char *prefix, int *out, int cap);

#ifdef __cplusplus
}
#endif
#endif
