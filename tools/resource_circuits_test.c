/* res_list_circuits: a route file name that makes "ROUTES<stem>/<name>" exactly 255 characters
 * must still come back as a terminated string (strncpy(.., 255) left it unterminated). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0700)
#endif
#include "resource.h"

static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

/* A closed 6-waypoint path chunk (0x34148, 24-byte records): first == last. */
static void write_path(const char *file) {
    unsigned char b[8 + 6 * 24]; memset(b, 0, sizeof b);
    put32(b, 0x34148u); put32(b + 4, 6 * 24);
    for (int i = 0; i < 6; i++) { float x = i < 5 ? 10.0f * i : 0.0f, y = 0.0f; memcpy(b + 8 + i * 24, &x, 4); memcpy(b + 12 + i * 24, &y, 4); }
    FILE *f = fopen(file, "wb"); assert(f); assert(fwrite(b, 1, sizeof b, f) == sizeof b); fclose(f);
}

int main(void) {
    const char *root = "build/resource_circuits_test_tmp";
    MKDIR("build"); MKDIR(root);
    char dir[512]; snprintf(dir, sizeof dir, "%s/ROUTESX", root); MKDIR(dir);
    char name[300]; memset(name, 'a', 247); memcpy(name, "Paths", 5); name[247] = 0;   /* "ROUTESX/" + 247 = 255 */
    char file[1024]; snprintf(file, sizeof file, "%s/%s", dir, name); write_path(file);
    char shortf[1024]; snprintf(shortf, sizeof shortf, "%s/Paths4001.bin", dir); write_path(shortf);

    static char list[4][256];
    memset(list, 'Z', sizeof list);
    int n = res_list_circuits(root, "STREAMX", list, 4);
    assert(n == 2);
    for (int i = 0; i < n; i++) assert(memchr(list[i], 0, sizeof list[i]) != NULL);   /* terminated inside its slot */
    int seen_short = 0, seen_long = 0;
    for (int i = 0; i < n; i++) { seen_short |= !strcmp(list[i], "ROUTESX/Paths4001.bin"); seen_long |= strlen(list[i]) == 255; }
    assert(seen_short && seen_long);
    remove(file); remove(shortf); remove(dir); remove(root);
    puts("resource_circuits_test OK");
    return 0;
}
