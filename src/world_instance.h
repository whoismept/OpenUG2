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

#endif
