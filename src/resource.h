/* resource.h — OpenUG2 ResourceManager module: file mapping and asset
 * discovery (which tracks / cars / circuits exist on disk). Chunk PARSING
 * stays in nfsu2.h — the ground-truth parser this module feeds. */
#ifndef OPENUG2_RESOURCE_H
#define OPENUG2_RESOURCE_H

#include "nfsu2.h"

/* mmap a file read-only. Lazy paging: pages fault in only when touched, so a
 * 100MB shared "master" region costs just the bytes actually read (TPK header
 * + the few textures we decode), not a full load. NULL on failure. */
unsigned char *res_map_file(const char *path, long *len);
void res_unmap_file(unsigned char *data, long len);

/* Enumerate selectable tracks: STREAM*.BUN files under troot. Writes up to
 * max names (extension stripped) and sets *sel to the entry matching cur.
 * Returns the count. */
int res_list_tracks(const char *troot, char (*list)[64], int max,
                    const char *cur, int *sel);

/* Enumerate selectable cars: folders under DATAROOT/CARS with a GEOMETRY.BIN,
 * minus the shared part folders (WHEELS, SPOILER, ...). */
int res_list_cars(const char *dataroot, char (*list)[64], int max,
                  const char *cur, int *sel);

/* Enumerate the selected region's own selectable circuits: closed-loop
 * Paths*.bin (first waypoint ~= last) from the ONE route directory that
 * belongs to trackname -- STREAML4RA -> ROUTESL4RA, a 1:1 shipped mapping
 * (M87: every race catalog lives in exactly one region directory, and no
 * event references a second bundle). trackname "ALL" returns 0: a blind union
 * of bundles is not a valid race world (M86), so it offers no circuits.
 * Paths are written relative to troot ("ROUTESX/PathsN.bin"). */
int res_list_circuits(const char *troot, const char *trackname,
                      char (*list)[256], int max);

#endif
