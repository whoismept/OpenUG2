/* M160 route-membership audit.
 *
 * GL-free, read-only. Answers one question with measured data: which authored
 * placements that a LIVE RACE currently loads are exclusive to a DIFFERENT
 * event's barrier group, and how close do those sit to the raced event's own
 * authored racing line.
 *
 * Membership uses the production reader and the production selection helper
 * (wg_selection_open / wg_selection_visible), so "foreign" here means exactly
 * what free-roam scenery selection already hides: proven exclusive membership
 * in numeric event groups that do not include the raced event.
 *
 * The route is the event's OWN 0x34148 racing line from
 * TRACKS/ROUTES<REG>/Paths<event>.bin -- the same leaf world_load_events uses
 * as the event corridor seed. Nothing is placed, filtered, hidden or rendered.
 */
#include "../src/world_instance.c"
#include "../src/world_group_reader.h"
#include "../src/world_scenery.h"

#define NAV_LINK_MAX 120.0f     /* production segment-break threshold */
#define ROUTE_STEP     1.0f     /* polyline subdivision, metres */

typedef struct { float *xy; int n, cap; } Route;

static void route_push(Route *r, float x, float y) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 1024;
        r->xy = realloc(r->xy, (size_t)r->cap * 2 * sizeof *r->xy);
    }
    r->xy[r->n * 2] = x; r->xy[r->n * 2 + 1] = y; r->n++;
}

/* Read the event's own racing line, joining consecutive records only when they
 * sit within NAV_LINK_MAX (a Paths file concatenates several segments), then
 * subdividing each accepted edge so a point-to-polyline test is uniform. */
static int route_load(Route *r, const char *troot, const char *reg, int event) {
    char path[1024];
    snprintf(path, sizeof path, "%s/ROUTES%s/Paths%04d.bin", troot, reg, event);
    long len = 0;
    unsigned char *d = n2_read_file(path, &len);
    if (!d) return 0;
    N2Leaf leaf[8]; int nl = 0;
    n2_find_leaves(d, 0, len, 0x00034148u, leaf, &nl, 8);
    int raw = 0;
    for (int L = 0; L < nl; L++) {
        int n = (int)leaf[L].size / 24;
        float px = 0, py = 0; int have = 0;
        for (int i = 0; i < n; i++) {
            float x, y;
            memcpy(&x, d + leaf[L].off + i * 24,     4);
            memcpy(&y, d + leaf[L].off + i * 24 + 4, 4);
            if (!(x == x && y == y) || x < -1e5f || x > 1e5f ||
                y < -1e5f || y > 1e5f) { have = 0; continue; }
            raw++;
            if (have) {
                float dx = x - px, dy = y - py;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist <= NAV_LINK_MAX) {
                    int steps = (int)(dist / ROUTE_STEP);
                    for (int s = 1; s < steps; s++)
                        route_push(r, px + dx * s / steps, py + dy * s / steps);
                }
            }
            route_push(r, x, y);
            px = x; py = y; have = 1;
        }
    }
    free(d);
    printf("ROUTE event=%d region=%s raw_nodes=%d polyline_points=%d\n",
           event, reg, raw, r->n);
    return r->n > 0;
}

/* XY distance from one route point to an authored placement's world AABB. */
static float aabb_d(const WInstPlacement *p, float x, float y) {
    float dx = x < p->bounds_min[0] ? p->bounds_min[0] - x
             : x > p->bounds_max[0] ? x - p->bounds_max[0] : 0.0f;
    float dy = y < p->bounds_min[1] ? p->bounds_min[1] - y
             : y > p->bounds_max[1] ? y - p->bounds_max[1] : 0.0f;
    return sqrtf(dx * dx + dy * dy);
}

typedef struct {
    WInstSection *sections;
    unsigned char *present;
} Index;

static int index_sections(const unsigned char *d, long begin, long end,
                          unsigned depth, Index *a) {
    if (depth > 64) return 0;
    for (long p = begin; p < end;) {
        long next;
        if (end - p < 8 || !chunk_end(p + 8, n2_u32(d + p + 4), end, &next)) return 0;
        uint32_t id = n2_u32(d + p);
        if (id == 0x80034100u) {
            WInstSection s;
            if (!winst_parse_section(d, p + 8, next, &s) || s.region_id < 0 ||
                s.region_id >= 65536 || a->present[s.region_id]) return 0;
            a->sections[s.region_id] = s; a->present[s.region_id] = 1;
        } else if (id && (id >> 28) == 8 &&
                   !index_sections(d, p + 8, next, depth + 1, a)) return 0;
        p = next;
    }
    return 1;
}

/* Every group name referencing one override, appended into buf. */
static void group_names_for(const WGTable *t, unsigned override_index,
                            char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = 0;
    for (size_t p = 0; p < t->group_bytes;) {
        const unsigned char *g = t->groups + p;
        uint32_t refs = wg_u32(g + 48);
        for (uint32_t j = 0; j < refs; j++)
            if (wg_u16(g + 52 + 2 * (size_t)j) == override_index) {
                int w = snprintf(buf + used, cap - used, "%s%s",
                                 used ? "," : "", (const char *)(g + 8));
                if (w > 0 && (size_t)w < cap - used) used += (size_t)w;
                break;
            }
        p += (size_t)((52ULL + 2ULL * refs + 3) & ~3ULL);
    }
}

/* Every event id this region's 0x3414c catalogs advertise. Paths files
 * concatenate the whole regional catalog, so scanning all of them and
 * de-duplicating is a superset of what world_load_events reads. */
static int ev_cmp(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int catalog_events(const char *troot, const char *reg, int *out, int cap) {
    int n = 0;
    for (int id = 4000; id < 8000; id++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/ROUTES%s/Paths%04d.bin", troot, reg, id);
        long len = 0;
        unsigned char *d = n2_read_file(path, &len);
        if (!d) continue;
        N2Leaf leaf[8]; int nl = 0;
        n2_find_leaves(d, 0, len, 0x0003414cu, leaf, &nl, 8);
        for (int L = 0; L < nl; L++)
            for (int i = 0; i < (int)(leaf[L].size / 272) && n < cap; i++) {
                const unsigned char *b = d + leaf[L].off + i * 272;
                int eid = b[0] | (b[1] << 8), npoly = b[2];
                if (eid < 4000 || npoly < 3 || npoly > 33) continue;
                int dup = 0;
                for (int k = 0; k < n; k++) if (out[k] == eid) { dup = 1; break; }
                if (!dup) out[n++] = eid;
            }
        free(d);
    }
    qsort(out, (size_t)n, sizeof *out, ev_cmp);
    return n;
}

typedef struct {
    unsigned override_index, section, row;
    char name[33];
    float x, y, z, dist;
    int foreign;
} Row;

static int row_cmp(const void *a, const void *b) {
    const Row *p = a, *q = b;
    return (p->dist > q->dist) - (p->dist < q->dist);
}

/* Catalog-wide validation of the per-event race selection. For every region
 * and every advertised event this exercises exactly the load-path steps the
 * selection change affects -- table open, group presence, selection open, and
 * the full-bundle override cross-check -- and reports the defect magnitude the
 * selection removes. It builds no scene: nothing downstream of the selection
 * was changed, so a scene build would only add cost. */
/* Superset of structural naming: anything that could be drivable surface or
 * world terrain rather than trackside dressing. Deliberately broad; a hit is
 * something to inspect, not automatically a defect. */
static int structural_name(const char *n) {
    return strstr(n, "TRN_") || strstr(n, "Terrain") || strstr(n, "TERRAIN") ||
           strstr(n, "Road")  || strstr(n, "ROAD")   || strstr(n, "PAN_")    ||
           strstr(n, "SKYDOME");
}
static int sweep(const char *troot, float corridor) {
    static const char *regions[] = { "L4RA","L4RB","L4RC","L4RD","L4RF","L4RG","L4RH","L4RR" };
    long tot_ev = 0, tot_degrade = 0, tot_fail = 0, tot_noroute = 0, tot_onroute = 0;
    long tot_risky = 0;
    int worst_on_route = 0, worst_ev = 0;
    char worst_reg[8] = "";
    printf("%-5s %-6s %6s %8s %8s %8s %7s %9s  %s\n",
           "reg", "event", "own", "foreign", "nonev", "route", "onroute", "selection", "note");
    for (size_t r = 0; r < sizeof regions / sizeof *regions; r++) {
        const char *reg = regions[r];
        char stream[16];
        snprintf(stream, sizeof stream, "STREAM%s", reg);
        long len = 0, clen = 0;
        unsigned char *d = winst_read_named(troot, stream, &len);
        unsigned char *c = winst_read_named(troot, reg, &clen);
        int evs[512];
        int nev = catalog_events(troot, reg, evs, 512);
        if (!d || !c) {
            printf("%-5s %-6s %6s %8s %8s %8s %7s %9s  no bundle (%d catalog events)\n",
                   reg, "-", "-", "-", "-", "-", "-", "-", nev);
            free(d); free(c);
            continue;
        }
        Index idx = {0};
        idx.sections = calloc(65536, sizeof *idx.sections);
        idx.present  = calloc(65536, 1);
        WGTable t;
        int table_ok = idx.sections && idx.present &&
                       wg_open_file(c, (size_t)clen, &t) &&
                       index_sections(d, 0, len, 0, &idx);
        if (!table_ok) {
            printf("%-5s %-6s %6s %8s %8s %8s %7s %9s  TABLE OPEN FAILED\n",
                   reg, "-", "-", "-", "-", "-", "-", "FAIL");
            tot_fail++;
        }
        for (int e = 0; table_ok && e < nev; e++) {
            int event = evs[e];
            tot_ev++;
            int present = wg_event_group_present(&t, event);
            WGSelection sel = {0};
            const char *verdict = "ok";
            const char *note = "";
            long own = 0, foreign = 0, nonev = 0, risky = 0;
            int on_route = 0, npts = 0;
            if (!present) {
                tot_degrade++;
                verdict = "degrade";
                note = "no authored group; unfiltered scene";
            } else if (!wg_selection_open(&t, event, &sel)) {
                tot_fail++;
                verdict = "FAIL";
                note = "selection rejected an authored group";
            } else if (!winst_check_scenery(d, 0, len, 0, &sel)) {
                tot_fail++;
                verdict = "FAIL";
                note = "override cross-check failed";
            } else {
                for (size_t i = 0; i < sel.count; i++) {
                    unsigned m = sel.items[i].membership;
                    if (!(m & WG_EVENT)) nonev++;
                    else if (m & WG_ACTIVE) own++;
                    else if (!(m & WG_OTHER)) foreign++;
                }
                /* Selection may only ever remove race dressing. Report every
                   hidden placement whose authored type name reads as road or
                   terrain, so the claim stays checkable against shipped data.
                   These are NOT lost ground: winst_build_visit skips every
                   ROAD/TERRAIN mesh and winst_place_ground_prototypes places
                   the surface from the model library without consulting any
                   selection, so only dressing attached to a road-named
                   prototype is removed. Pinned by
                   test_selection_never_hides_ground. */
                for (size_t i = 0; i < sel.count; i++) {
                    const WGSelected *it = &sel.items[i];
                    if (wg_selection_visible(&sel, it->section, it->row)) continue;
                    if (!idx.present[it->section]) continue;
                    const WInstSection *sec = &idx.sections[it->section];
                    WInstPlacement pl;
                    if (it->row >= (unsigned)sec->placement_count ||
                        !winst_decode_placement(sec->placements + 64L * it->row, 64, &pl) ||
                        pl.type_index >= (unsigned)sec->type_count) continue;
                    char nm[33];
                    memcpy(nm, sec->types + 68L * pl.type_index, 32);
                    nm[32] = 0;
                    if (!structural_name(nm)) continue;
                    risky++;
                    tot_risky++;
                    printf("  ROADNAMED %s/%d hides section=%u row=%u %s XY=(%.3f,%.3f)\n",
                           reg, event, it->section, it->row, nm,
                           (double)pl.matrix[12], (double)pl.matrix[13]);
                }
                Route route = {0};
                FILE *devnull = NULL; (void)devnull;
                /* route_load prints one line per event; keep the sweep table
                   readable by loading quietly through the same parser. */
                char path[1024];
                snprintf(path, sizeof path, "%s/ROUTES%s/Paths%04d.bin", troot, reg, event);
                long rlen = 0;
                unsigned char *rd = n2_read_file(path, &rlen);
                if (rd) {
                    N2Leaf leaf[8]; int nl = 0;
                    n2_find_leaves(rd, 0, rlen, 0x00034148u, leaf, &nl, 8);
                    for (int L = 0; L < nl; L++) {
                        int n = (int)leaf[L].size / 24;
                        float px = 0, py = 0; int have = 0;
                        for (int i = 0; i < n; i++) {
                            float x, y;
                            memcpy(&x, rd + leaf[L].off + i * 24,     4);
                            memcpy(&y, rd + leaf[L].off + i * 24 + 4, 4);
                            if (!(x == x && y == y) || x < -1e5f || x > 1e5f ||
                                y < -1e5f || y > 1e5f) { have = 0; continue; }
                            if (have) {
                                float dx = x - px, dy = y - py;
                                float dist = sqrtf(dx * dx + dy * dy);
                                if (dist <= NAV_LINK_MAX) {
                                    int steps = (int)(dist / ROUTE_STEP);
                                    for (int st = 1; st < steps; st++)
                                        route_push(&route, px + dx * st / steps,
                                                           py + dy * st / steps);
                                }
                            }
                            route_push(&route, x, y);
                            px = x; py = y; have = 1;
                        }
                    }
                    free(rd);
                }
                npts = route.n;
                if (!npts) { tot_noroute++; note = "no own racing line"; }
                for (size_t i = 0; i < sel.count && npts; i++) {
                    const WGSelected *it = &sel.items[i];
                    if (wg_selection_visible(&sel, it->section, it->row)) continue;
                    const WInstSection *sec = &idx.sections[it->section];
                    WInstPlacement p;
                    if (!idx.present[it->section] ||
                        it->row >= (unsigned)sec->placement_count ||
                        !winst_decode_placement(sec->placements + 64L * it->row, 64, &p))
                        continue;
                    for (int k = 0; k < route.n; k++)
                        if (aabb_d(&p, route.xy[k * 2], route.xy[k * 2 + 1]) <= corridor) {
                            on_route++; break;
                        }
                }
                free(route.xy);
                tot_onroute += on_route;
                if (on_route > worst_on_route) {
                    worst_on_route = on_route; worst_ev = event;
                    snprintf(worst_reg, sizeof worst_reg, "%s", reg);
                }
            }
            free(sel.items);
            printf("%-5s %-6d %6ld %8ld %8ld %8d %7d %9s  %s%s\n",
                   reg, event, own, foreign, nonev, npts, on_route, verdict,
                   risky ? "road-named placements hidden (dressing only) " : "", note);
        }
        free(idx.sections); free(idx.present); free(d); free(c);
    }
    printf("\nSWEEP events=%ld degraded=%ld failed=%ld without_own_route=%ld\n",
           tot_ev, tot_degrade, tot_fail, tot_noroute);
    printf("SWEEP road-named placements some event selection hides: %ld "
           "(dressing only; ground comes from the unfiltered prototype path)\n", tot_risky);
    printf("SWEEP foreign placements standing within %.1f m of the raced route: "
           "%ld total, worst %s/%d with %d\n",
           (double)corridor, tot_onroute, worst_reg[0] ? worst_reg : "-",
           worst_ev, worst_on_route);
    return tot_fail ? 1 : 0;
}

/* What kinds of asset can event selection ever hide? Selecting one event's
 * group only suppresses placements proven exclusive to OTHER numeric event
 * groups, so this enumerates every distinct authored type name reachable that
 * way. If the set is race dressing only, per-event selection cannot remove
 * structural geometry no matter which event is raced. */
typedef struct { char name[33]; long count; } AssetTally;
static int asset_cmp(const void *a, const void *b) {
    const AssetTally *p = a, *q = b;
    if (p->count != q->count) return p->count < q->count ? 1 : -1;
    return strcmp(p->name, q->name);
}
static int assets(const char *troot) {
    static const char *regions[] = { "L4RA","L4RB","L4RC","L4RD","L4RF","L4RG","L4RH","L4RR" };
    AssetTally *tally = calloc(4096, sizeof *tally);
    size_t ntally = 0;
    long total = 0;
    if (!tally) return 1;
    for (size_t r = 0; r < sizeof regions / sizeof *regions; r++) {
        const char *reg = regions[r];
        char stream[16];
        snprintf(stream, sizeof stream, "STREAM%s", reg);
        long len = 0, clen = 0;
        unsigned char *d = winst_read_named(troot, stream, &len);
        unsigned char *c = winst_read_named(troot, reg, &clen);
        Index idx = {0};
        idx.sections = calloc(65536, sizeof *idx.sections);
        idx.present  = calloc(65536, 1);
        WGTable t;
        if (d && c && idx.sections && idx.present &&
            wg_open_file(c, (size_t)clen, &t) && index_sections(d, 0, len, 0, &idx)) {
            for (size_t i = 0; i < t.override_count; i++) {
                const unsigned char *ov = t.overrides + 8 * i;
                unsigned sid = wg_u16(ov), row = wg_u16(ov + 2);
                if (!idx.present[sid]) continue;
                const WInstSection *sec = &idx.sections[sid];
                WInstPlacement p;
                if (row >= (unsigned)sec->placement_count ||
                    !winst_decode_placement(sec->placements + 64L * row, 64, &p) ||
                    p.type_index >= (unsigned)sec->type_count) continue;
                char name[33];
                memcpy(name, sec->types + 68L * p.type_index, 32);
                name[32] = 0;
                total++;
                size_t k = 0;
                for (; k < ntally; k++) if (!strcmp(tally[k].name, name)) break;
                if (k == ntally && ntally < 4096) {
                    snprintf(tally[ntally].name, sizeof tally[ntally].name, "%s", name);
                    ntally++;
                }
                if (k < 4096) tally[k].count++;
            }
        } else fprintf(stderr, "assets: skipped %s\n", reg);
        free(idx.sections); free(idx.present); free(d); free(c);
    }
    qsort(tally, ntally, sizeof *tally, asset_cmp);
    printf("ASSETS group-referenced placements=%ld distinct_types=%zu\n", total, ntally);
    for (size_t i = 0; i < ntally; i++)
        printf("  %8ld  %s\n", tally[i].count, tally[i].name);
    free(tally);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[2], "--assets")) return assets(argv[1]);
    if (argc >= 3 && !strcmp(argv[2], "--sweep")) {
        float corridor = argc >= 4 ? (float)atof(argv[3]) : 40.0f;
        if (!(corridor > 0.0f) || corridor > 5000.0f) return 2;
        return sweep(argv[1], corridor);
    }
    if (argc < 4) {
        fprintf(stderr, "usage: m160_route_audit TRACK_ROOT L4RG EVENT [CORRIDOR_M]\n"
                        "       m160_route_audit TRACK_ROOT L4RG EVENT --near X Y R\n"
                        "       m160_route_audit TRACK_ROOT --sweep [CORRIDOR_M]\n");
        return 2;
    }
    const char *troot = argv[1], *reg = argv[2];
    int event = atoi(argv[3]);
    float corridor = 40.0f, nx = 0, ny = 0, nr = 0;
    int near_mode = 0;
    if (argc == 8 && !strcmp(argv[4], "--near")) {
        near_mode = 1;
        nx = (float)atof(argv[5]); ny = (float)atof(argv[6]); nr = (float)atof(argv[7]);
        if (!isfinite(nx) || !isfinite(ny) || !(nr > 0.0f) || nr > 5000.0f) return 2;
    } else if (argc == 5) corridor = (float)atof(argv[4]);
    else if (argc != 4) return 2;
    if (strlen(reg) != 4 || strncmp(reg, "L4R", 3) ||
        !strchr("ABCDFGHR", reg[3]) || event < 4000 || event > 65535 ||
        !(corridor > 0.0f) || corridor > 5000.0f) return 2;

    int result = 1;
    long len = 0, clen = 0;
    char stream[16];
    snprintf(stream, sizeof stream, "STREAM%s", reg);
    unsigned char *d = winst_read_named(troot, stream, &len);
    unsigned char *c = winst_read_named(troot, reg, &clen);
    Index idx = {0};
    Route route = {0};
    WGTable t;
    WGSelection sel = {0};
    Row *rows = NULL;
    idx.sections = calloc(65536, sizeof *idx.sections);
    idx.present  = calloc(65536, 1);
    if (!d || !c || !idx.sections || !idx.present) goto done;
    if (!wg_open_file(c, (size_t)clen, &t) || !index_sections(d, 0, len, 0, &idx)) goto done;
    if (!route_load(&route, troot, reg, event)) goto done;
    /* Production selection semantics for this exact event id. */
    if (!wg_selection_open(&t, event, &sel)) goto done;

    if (near_mode) {
        /* Every authored placement, grouped or not, whose world AABB reaches
           the query circle. Ownership comes from the same production
           selection, so "ungrouped" here means no group references it. */
        printf("NEAR (%.3f,%.3f) r=%.2f m  event=%d\n",
               (double)nx, (double)ny, (double)nr, event);
        long shown = 0;
        for (int s = 0; s < 65536; s++) {
            if (!idx.present[s]) continue;
            const WInstSection *sec = &idx.sections[s];
            for (int row = 0; row < sec->placement_count; row++) {
                WInstPlacement p;
                if (!winst_decode_placement(sec->placements + 64L * row, 64, &p) ||
                    p.type_index >= (unsigned)sec->type_count) continue;
                float d = aabb_d(&p, nx, ny);
                if (d > nr) continue;
                char name[33];
                memcpy(name, sec->types + 68L * p.type_index, 32);
                name[32] = 0;
                const WGSelected *it = wg_selection_find(&sel, (unsigned)s, (unsigned)row);
                const char *own = !it ? "UNGROUPED"
                                : (it->membership & WG_ACTIVE) ? "OWN"
                                : !(it->membership & WG_EVENT) ? "NONEVENT"
                                : (it->membership & WG_OTHER) ? "SHARED" : "FOREIGN";
                char groups[256];
                groups[0] = 0;
                if (it) {
                    unsigned oi = 0;
                    for (size_t k = 0; k < t.override_count; k++) {
                        const unsigned char *r = t.overrides + 8 * k;
                        if (wg_u16(r) == s && wg_u16(r + 2) == row) { oi = (unsigned)k; break; }
                    }
                    group_names_for(&t, oi, groups, sizeof groups);
                }
                printf("  d=%7.2f m  %-9s section=%-5d row=%-5d %-30s "
                       "XY=(%9.3f,%10.3f) Zc=%8.3f AABB[%9.3f %9.3f][%10.3f %10.3f] %s\n",
                       (double)d, own, s, row, name,
                       (double)p.matrix[12], (double)p.matrix[13], (double)p.matrix[14],
                       (double)p.bounds_min[0], (double)p.bounds_max[0],
                       (double)p.bounds_min[1], (double)p.bounds_max[1], groups);
                shown++;
            }
        }
        printf("NEAR total=%ld\n", shown);
        result = 0;
        goto done;
    }

    rows = calloc(sel.count, sizeof *rows);
    if (sel.count && !rows) goto done;

    long n_active = 0, n_foreign = 0, n_shared = 0, n_free = 0;
    size_t nrows = 0;
    for (size_t i = 0; i < sel.count; i++) {
        const WGSelected *s = &sel.items[i];
        if (!idx.present[s->section]) goto done;
        const WInstSection *sec = &idx.sections[s->section];
        if (s->row >= (unsigned)sec->placement_count) goto done;
        WInstPlacement p;
        if (!winst_decode_placement(sec->placements + 64L * s->row, 64, &p) ||
            p.type_index >= (unsigned)sec->type_count) goto done;
        int visible = wg_selection_visible(&sel, s->section, s->row);
        if (!(s->membership & WG_EVENT))            n_free++;
        else if (s->membership & WG_ACTIVE)         n_active++;
        else if (s->membership & WG_OTHER)          n_shared++;
        else                                        n_foreign++;

        float best = 1e30f;
        for (int k = 0; k < route.n; k++) {
            float dd = aabb_d(&p, route.xy[k * 2], route.xy[k * 2 + 1]);
            if (dd < best) best = dd;
        }
        if (best > corridor) continue;
        Row *r = &rows[nrows++];
        r->override_index = 0;             /* filled below by table order */
        r->section = s->section; r->row = s->row;
        memcpy(r->name, sec->types + 68L * p.type_index, 32);
        r->name[32] = 0;
        r->x = p.matrix[12]; r->y = p.matrix[13]; r->z = p.matrix[14];
        r->dist = best;
        r->foreign = !visible;
    }

    printf("MEMBERSHIP event=%d overrides=%zu active=%ld foreign_exclusive=%ld "
           "shared_with_nonevent=%ld nonevent_only=%ld\n",
           event, sel.count, n_active, n_foreign, n_shared, n_free);
    printf("CORRIDOR %.1f m: %zu group placements within reach of the racing line\n",
           (double)corridor, nrows);

    qsort(rows, nrows, sizeof *rows, row_cmp);
    /* Recover override index for the rows we print, so a reader can re-query
       them with world_group_audit. */
    for (size_t i = 0; i < nrows; i++)
        for (size_t k = 0; k < t.override_count; k++) {
            const unsigned char *r = t.overrides + 8 * k;
            if (wg_u16(r) == rows[i].section && wg_u16(r + 2) == rows[i].row) {
                rows[i].override_index = (unsigned)k; break;
            }
        }

    long fnear = 0;
    printf("\nFOREIGN-EXCLUSIVE placements on this event's route "
           "(hidden by free-roam selection, loaded by the live race):\n");
    for (size_t i = 0; i < nrows; i++) {
        if (!rows[i].foreign) continue;
        fnear++;
        char groups[256];
        group_names_for(&t, rows[i].override_index, groups, sizeof groups);
        printf("  d=%7.2f m  section=%-5u row=%-5u override=%-5u %-24s "
               "XYZ=(%9.3f,%10.3f,%8.3f) groups=%s\n",
               (double)rows[i].dist, rows[i].section, rows[i].row,
               rows[i].override_index, rows[i].name,
               (double)rows[i].x, (double)rows[i].y, (double)rows[i].z, groups);
    }
    long anear = 0;
    for (size_t i = 0; i < nrows; i++) if (!rows[i].foreign) anear++;
    printf("\nSUMMARY route_corridor=%.1f m foreign_on_route=%ld own_or_shared_on_route=%ld\n",
           (double)corridor, fnear, anear);
    result = 0;
done:
    if (result) fprintf(stderr, "m160 audit failed: missing, malformed or inconsistent input.\n");
    free(rows); free(route.xy); free(sel.items);
    free(idx.sections); free(idx.present); free(d); free(c);
    return result;
}
