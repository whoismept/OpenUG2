/* resource.c — OpenUG2 ResourceManager module implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "resource.h"

unsigned char *res_map_file(const char *path, long *len) {
    if (!path || !len) return NULL;
#ifdef _WIN32
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!count) return NULL;
    wchar_t *wide = malloc((size_t)count * sizeof *wide);
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide);
        return NULL;
    }
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    /* The parser's public length is long, which remains 32-bit on Win64. */
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        (uint64_t)size.QuadPart > LONG_MAX ||
        (uint64_t)size.QuadPart > SIZE_MAX) {
        CloseHandle(file);
        return NULL;
    }
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(file);
    if (!mapping) return NULL;
    void *p = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, (SIZE_T)size.QuadPart);
    CloseHandle(mapping);
    if (!p) return NULL;
    *len = (long)size.QuadPart;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (uintmax_t)st.st_size > LONG_MAX ||
        (uintmax_t)st.st_size > SIZE_MAX) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *len = (long)st.st_size;
#endif
    return (unsigned char *)p;
}

void res_unmap_file(unsigned char *data, long len) {
#ifdef _WIN32
    if (data && len > 0) UnmapViewOfFile(data);
#else
    if (data && len > 0) munmap(data, (size_t)len);
#endif
}

int res_list_tracks(const char *troot, char (*list)[64], int max,
                    const char *cur, int *sel) {
    int n = 0;
    DIR *td = opendir(troot); struct dirent *de;
    while (td && n < max && (de = readdir(td))) {
        char *dot = strstr(de->d_name, ".BUN");
        if (strncmp(de->d_name, "STREAM", 6) || !dot) continue;
        int L = (int)(dot - de->d_name); if (L > 63) L = 63;
        memcpy(list[n], de->d_name, L); list[n][L] = 0;
        if (!strcmp(list[n], cur)) *sel = n;
        n++;
    }
    if (td) closedir(td);
    return n;
}

int res_list_cars(const char *dataroot, char (*list)[64], int max,
                  const char *cur, int *sel) {
    static const char *parts[] = { "AUDIO","BRAKES","EXHAUST","PLATES","ROOF",
        "WHEELS","SPOILER","SPOILER_HATCH","SPOILER_SUV","MIRRORS_BODY",
        "MIRRORS_HUMMER","MIRRORS_POST","MIRRORS_SUV" };
    int n = 0;
    char croot[1024]; snprintf(croot, sizeof croot, "%s/CARS", dataroot);
    DIR *cd = opendir(croot); struct dirent *de;
    while (cd && n < max && (de = readdir(cd))) {
        if (de->d_name[0] == '.') continue;
        int isp = 0;
        for (unsigned p = 0; p < sizeof parts/sizeof parts[0]; p++)
            if (!strcmp(de->d_name, parts[p])) isp = 1;
        if (isp) continue;
        char gp[1200]; snprintf(gp, sizeof gp, "%s/%s/GEOMETRY.BIN", croot, de->d_name);
        FILE *f = fopen(gp, "rb"); if (!f) continue; fclose(f);
        int L = (int)strlen(de->d_name); if (L > 63) L = 63;
        memcpy(list[n], de->d_name, L); list[n][L] = 0;
        if (!strcmp(list[n], cur)) *sel = n;
        n++;
    }
    if (cd) closedir(cd);
    return n;
}

int res_list_circuits(const char *troot, const char *trackname,
                      char (*list)[256], int max) {
    /* ALL is a blind union of incompatible bundles, not a race world. */
    if (!trackname || !strcmp(trackname, "ALL")) return 0;
    /* the shipped 1:1 mapping: STREAM<stem>.BUN <-> ROUTES<stem>/ */
    const char *stem = strncmp(trackname, "STREAM", 6) ? trackname : trackname + 6;
    char dname[256]; snprintf(dname, sizeof dname, "ROUTES%s", stem);
    char rdir[1200]; snprintf(rdir, sizeof rdir, "%s/%s", troot, dname);

    int n = 0;
    DIR *rd = opendir(rdir); struct dirent *re;
    while (rd && n < max && (re = readdir(rd))) {
        if (strncmp(re->d_name, "Paths", 5)) continue;
        char rel[256]; snprintf(rel, sizeof rel, "%s/%s", dname, re->d_name);
        int dup=0; for (int i=0;i<n;i++) if(!strcmp(list[i],rel)) dup=1;
        if (dup) continue;
        char pp[1500]; snprintf(pp, sizeof pp, "%s/%s", troot, rel);
        long pl; unsigned char *pd = n2_read_file(pp, &pl); N2Path tp = {0};
        if (pd && n2_load_path(pd, pl, &tp) > 4) {
            /* closed loop: first waypoint ~= last (sprints are open routes) */
            float dx=tp.xy[0]-tp.xy[(tp.n-1)*2], dy=tp.xy[1]-tp.xy[(tp.n-1)*2+1];
            if (dx*dx+dy*dy < 900.0f) { memcpy(list[n], rel, strlen(rel) + 1); n++; }   /* rel fits list[n] and is terminated; strncpy(.., 255) was not for a 255-char name */
        }
        free(tp.xy); free(pd);
    }
    if (rd) closedir(rd);
    return n;
}
