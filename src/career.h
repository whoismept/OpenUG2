/* career.h -- OpenUG2's own career state: bank balance and event results.
 *
 * Header-only, GL-free and data-free: tools/career_test.c drives it directly.
 * This is not the retail career. The retail save's career region is opaque and
 * stays untouched (settings.c preserves it); this state lives in its own text
 * file next to camera.cfg (PROVENANCE[CAR-04]). Payouts and the starting balance
 * are guesses, listed in docs/PROVENANCE.md as CAR-01..03. */
#ifndef OPENUG2_CAREER_H
#define OPENUG2_CAREER_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAREER_MAX_EVENTS 512
#define CAREER_FILE_MAGIC "openug2-career 1"

typedef struct { uint32_t id; int best_place, wins, starts; } CareerEvent;

typedef struct {
    int64_t money;
    int nevents;
    CareerEvent events[CAREER_MAX_EVENTS];
} Career;

static void career_new(Career *c) {
    memset(c, 0, sizeof *c);
    c->money = 0;   /* PROVENANCE[CAR-01]: starting balance is a guess */
}

static const CareerEvent *career_event(const Career *c, uint32_t id) {
    for (int i = 0; c && i < c->nevents; i++)
        if (c->events[i].id == id) return &c->events[i];
    return NULL;
}

static int career_event_won(const Career *c, uint32_t id) {
    const CareerEvent *e = career_event(c, id);
    return e && e->wins > 0;
}

static int career_wins(const Career *c) {
    int n = 0;
    for (int i = 0; c && i < c->nevents; i++) n += c->events[i].wins > 0;
    return n;
}

/* PROVENANCE[CAR-02]: only a win pays, by race kind; the amounts are guesses.
 * PROVENANCE[CAR-03]: winning an event again pays a quarter. */
static int career_payout(int kind, int place, int already_won) {
    /* indexed by race.h's N2_RACE_*: circuit, sprint, drag, drift, Street X, URL */
    static const int first[] = { 0, 400, 300, 250, 350, 400, 1000 };
    if (place != 1 || kind < 1 || kind > 6) return 0;
    return already_won ? first[kind] / 4 : first[kind];
}

/* Record a finished event and bank its payout. place is 1-based; failed is a
 * blown engine or wreck, which counts as a start and nothing else. Returns the
 * money added, or -1 when the event table is full or the input is invalid. */
static int career_race_result(Career *c, uint32_t id, int kind, int place, int failed) {
    if (!c || id == 0 || place < 1) return -1;
    CareerEvent *e = (CareerEvent *)career_event(c, id);
    if (!e) {
        if (c->nevents >= CAREER_MAX_EVENTS) return -1;
        e = &c->events[c->nevents++];
        memset(e, 0, sizeof *e);
        e->id = id;
    }
    e->starts++;
    if (failed) return 0;
    int pay = career_payout(kind, place, e->wins > 0);
    if (!e->best_place || place < e->best_place) e->best_place = place;
    if (place == 1) e->wins++;
    c->money += pay;
    return pay;
}

/* 1 and the balance drops if it covers the price; otherwise 0 and no change. */
static int career_spend(Career *c, int64_t price) {
    if (!c || price < 0 || price > c->money) return 0;
    c->money -= price;
    return 1;
}

static int career_path(char *out, size_t cap) {
    const char *d = getenv("OPENUG2_CONFIG_DIR");
    if (d && *d) return snprintf(out, cap, "%s/career.sav", d) < (int)cap;
    d = getenv("XDG_CONFIG_HOME");
    if (d && *d) return snprintf(out, cap, "%s/openug2/career.sav", d) < (int)cap;
    d = getenv("HOME");
    if (d && *d) return snprintf(out, cap, "%s/.config/openug2/career.sav", d) < (int)cap;
    return 0;
}

/* Written to a sibling file first, so a crash mid-write keeps the old save. */
static int career_save(const Career *c, const char *path) {
    char tmp[1024];
    if (!c || !path || snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "%s\nmoney %lld\n", CAREER_FILE_MAGIC, (long long)c->money);
    for (int i = 0; i < c->nevents; i++)
        fprintf(f, "event %u %d %d %d\n", (unsigned)c->events[i].id, c->events[i].best_place,
                c->events[i].wins, c->events[i].starts);
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok) {
#ifdef _WIN32
        remove(path);   /* rename does not replace on Windows */
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

/* 1 on a valid file, 0 when there is none, -1 when one exists but cannot be
 * read. All but 1 leave a new career. On -1 the caller should not save over
 * the file: it may hold progress a later version or a hand repair can recover. */
static int career_load(Career *c, const char *path) {
    career_new(c);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    char line[128];
    int ok = fgets(line, sizeof line, f) && !strncmp(line, CAREER_FILE_MAGIC, strlen(CAREER_FILE_MAGIC));
    int have_money = 0;
    while (ok && fgets(line, sizeof line, f)) {
        long long money; unsigned id; int best, wins, starts;
        if (sscanf(line, "money %lld", &money) == 1) {
            if (money < 0) ok = 0;
            c->money = money; have_money = 1;
        } else if (sscanf(line, "event %u %d %d %d", &id, &best, &wins, &starts) == 4) {
            if (!id || best < 0 || wins < 0 || starts < wins || career_event(c, id) ||
                c->nevents >= CAREER_MAX_EVENTS) ok = 0;
            else c->events[c->nevents++] = (CareerEvent){ id, best, wins, starts };
        } else if (line[0] != '\n' && line[0] != '#') ok = 0;
    }
    fclose(f);
    if (ok && have_money) return 1;
    career_new(c);
    return -1;
}

#endif
