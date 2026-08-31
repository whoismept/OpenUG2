#ifndef WORLD_INSTANCE_H
#define WORLD_INSTANCE_H

#include <stdint.h>

#include "nfsu2.h"

typedef struct {
    int id;
    float bb[4];
    float *xy;
    int nxy;
} WInstRegion;

typedef struct {
    uint16_t type_index;
    uint16_t flags;
    /* Populated by the section walker from 0x34102, not from 0x34103. */
    uint32_t model_keys[3];
    float bounds_min[3], bounds_max[3];
    float matrix[16];
} WInstPlacement;

typedef struct {
    long instances_seen;
    long instances_in_range;
    long meshes_placed;
    long missing_models;
    long own_matrix_meshes;
    long rejected_meshes;
    long keyed_models;
    long lod_fallbacks;
    long unkeyed_models;
    int regions_total;
    int regions_selected;
    int home_region;
    char bundle[64];
} WInstStats;

int  winst_parse_regions(const unsigned char *data, long len,
                         WInstRegion **out, int *count);
void winst_free_regions(WInstRegion *regions, int count);
int  winst_select_regions(const WInstRegion *regions, int count,
                          float x, float y, float radius,
                          unsigned char *selected, int selected_cap,
                          int *home_index);
int  winst_decode_placement(const unsigned char *record, long len,
                            WInstPlacement *out);
int  winst_place_mesh(N2Scene *dst, const N2Mesh *src,
                      const float matrix[16], const char *asset_name,
                      WInstStats *stats);
int world_instance_build(N2Scene *scene, N2Scene *vista,
                         const char *track_root,
                         const char *const *bundles, int bundle_count,
                         float focus_x, float focus_y, float view_radius,
                         const unsigned char *shared, long shared_len,
                         WInstStats *stats);

#ifdef WORLD_INSTANCE_TESTING
typedef int (*WInstVisitFn)(const WInstPlacement *placement,
                            const char *type_name, void *userdata);
int winst_test_collect_placements(const unsigned char *section_data,
                                  long section_len,
                                  float focus_x, float focus_y,
                                  float view_radius,
                                  WInstVisitFn visit, void *userdata,
                                  WInstStats *stats);
#endif

#endif
