/* texkey_probe -- GL-free forensics for one world texture key.
 *
 * Answers the two questions that come up when a mesh wears the wrong picture:
 *   1. does this bundle hold a record for the key at all, and is it the ONLY
 *      one?  n2_tpk_decode stops at its first hit, so a second row here means
 *      a winner is being picked rather than found.
 *   2. what does the record actually say -- size, format, authored draw
 *      metadata (order/usage/blend/wz) and how much of its alpha is real.
 *
 * A key with no record is either a texture living in a pack this bundle does
 * not ship, or a MATERIAL name that has no art anywhere (see FORMATS.md,
 * "Slots that name a material, not a texture").
 *
 * usage: texkey_probe DATAROOT TRACK KEYHEX [KEYHEX...]
 *        N2_PROBE_DIR=<dir> also writes each decoded texture as a .ppm,
 *        with fully transparent texels flagged magenta.
 */
#include "../src/nfsu2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void record_name(const unsigned char *d, long i, char *out, int cap) {
    int n = 0;
    while (n < cap - 1 && n < 24 && d[i + n] >= 32 && d[i + n] < 127) { out[n] = (char)d[i + n]; n++; }
    out[n] = 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s DATAROOT TRACK KEYHEX...\n", argv[0]); return 2; }
    char path[1024];
    snprintf(path, sizeof path, "%s/TRACKS/%s.BUN", argv[1], argv[2]);
    long len = 0;
    unsigned char *d = n2_read_file(path, &len);
    if (!d) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    N2Tpk t = n2_tpk_open(d, len);
    printf("%s: %ld bytes, %d TPK block(s)\n", path, len, t.nblk);
    static const char *modenm[4] = { "OPAQUE", "CUTOUT", "BLEND", "ADD" };
    const char *dir = getenv("N2_PROBE_DIR");

    for (int a = 3; a < argc; a++) {
        uint32_t key = (uint32_t)strtoul(argv[a], NULL, 16);
        printf("\nkey %08x\n", key);
        int hits = 0;
        for (int b = 0; b < t.nblk; b++) {
            long hbeg = t.blk[b].hbeg, hend = hbeg + t.blk[b].hsize;
            for (long i = hbeg; i + 0x4c <= hend; i++) {
                if (!(d[i] >= 'A' && d[i] <= 'Z')) continue;
                if (n2_u32(d + i + 0x18) != key) continue;
                int w = d[i+0x38] | d[i+0x39]<<8, h = d[i+0x3a] | d[i+0x3b]<<8;
                char nm[32]; record_name(d, i, nm, sizeof nm);
                printf("  %s block %3d  %-26s %5dx%-5d fmt=%02x order=%u usage=%u blend=%u wz=%u%s\n",
                       hits ? "  " : "->", b, nm, w, h, d[i+0x3e],
                       d[i+0x45], d[i+0x49], d[i+0x4a], d[i+0x4b],
                       (w <= 0 || h <= 0 || w > 4096 || h > 4096) ? "   [rejected: implausible size]" : "");
                hits++;
            }
        }
        if (!hits) { printf("  (no record in this bundle -- unshipped texture, or a material name)\n"); continue; }

        N2Tex tex;
        if (!n2_tpk_decode(d, len, t, key, &tex)) { printf("  decode failed\n"); continue; }
        long px = (long)tex.w * tex.h, zero = 0, full = 0, mid = 0;
        if (tex.alpha)
            for (long i = 0; i < px; i++)
                tex.alpha[i] == 0 ? zero++ : tex.alpha[i] == 255 ? full++ : mid++;
        printf("  decoded %dx%d afmt=%d -> %s\n", tex.w, tex.h, tex.afmt, modenm[n2_tex_mode(&tex)]);
        if (tex.alpha) printf("  alpha: %ld transparent, %ld partial, %ld opaque (%.1f%% non-opaque)\n",
                              zero, mid, full, 100.0 * (double)(zero + mid) / (double)px);
        else           printf("  alpha: none retained (fully opaque plane)\n");
        if (dir) {
            char out[1024]; snprintf(out, sizeof out, "%s/tex_%08x.ppm", dir, key);
            FILE *f = fopen(out, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", tex.w, tex.h);
                for (long i = 0; i < px; i++) {
                    unsigned char c[3] = { tex.rgb[i*3], tex.rgb[i*3+1], tex.rgb[i*3+2] };
                    if (tex.alpha && tex.alpha[i] == 0) { c[0] = 255; c[1] = 0; c[2] = 255; }
                    fwrite(c, 1, 3, f);
                }
                fclose(f); printf("  wrote %s\n", out);
            }
        }
        free(tex.rgb); free(tex.alpha); free(tex.dxt);
    }
    free(t.blk); free(d);
    return 0;
}
