/* What is actually inside CARS/<CAR>/VINYLS.BIN.
 *
 * Each slot is a "HUFF"-wrapped blob whose payload ends in a 144-byte record
 * carrying the vinyl's own 24-byte NAME, its key, size and pixel format. The
 * names do not appear in `strings` because the record is inside the compressed
 * stream, which is why the archive looked like 1786 anonymous keys.
 *
 *   ./build/vinyl_census /path/to/data SKYLINE [--all]
 *
 * Read-only; decodes through the same n2_load_car_tex_by_key the game uses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nfsu2.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: vinyl_census DATAROOT CAR [--all]\n"); return 2; }
    int all = argc > 3 && !strcmp(argv[3], "--all");
    char path[1024];
    snprintf(path, sizeof path, "%s/CARS/%s/VINYLS.BIN", argv[1], argv[2]);
    long len = 0; unsigned char *d = n2_read_file(path, &len);
    if (!d) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    static uint32_t keys[4096];
    int n = n2_car_tex_keys(d, len, keys, 4096);
    printf("%s: %ld KB, %d slots\n\n", path, len >> 10, n);

    /* Group by the leading family token of the name, which is what a catalogue
       would key off; print every entry with --all. */
    long decoded = 0, named = 0, withalpha = 0;
    char fam[256][24]; long famn[256]; int nfam = 0;
    for (int i = 0; i < n; i++) {
        N2Tex t;
        if (!n2_load_car_tex_by_key(d, len, keys[i], &t)) continue;
        decoded++;
        if (t.alpha) withalpha++;
        char name[25] = "";
        n2_car_tex_name_by_key(d, len, keys[i], name, sizeof name);
        if (name[0]) named++;
        if (all)
            printf("  %08x %4dx%-4d %s %-24s\n", keys[i], t.w, t.h,
                   t.alpha ? "alpha" : "  -  ", name);
        else if (name[0]) {
            char f[24]; int k = 0;
            for (; name[k] && k < 23 && name[k] != '_' ; k++) f[k] = name[k];
            f[k] = 0;
            int hit = -1;
            for (int q = 0; q < nfam; q++) if (!strcmp(fam[q], f)) { hit = q; break; }
            if (hit < 0 && nfam < 256) { hit = nfam++; snprintf(fam[hit], 24, "%s", f); famn[hit] = 0; }
            if (hit >= 0) famn[hit]++;
        }
        free(t.rgb); free(t.alpha); free(t.dxt);
    }
    printf("\ndecoded %ld/%d, %ld carry a name, %ld carry alpha\n",
           decoded, n, named, withalpha);
    if (!all) {
        printf("\nname families:\n");
        for (int q = 0; q < nfam; q++) printf("  %-22s %4ld\n", fam[q], famn[q]);
    }
    free(d);
    return 0;
}
