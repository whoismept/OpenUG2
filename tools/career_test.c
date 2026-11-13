/* career_test.c -- bank balance, event results and the save file (src/career.h). */
#include <stdio.h>
#include "career.h"

static int checks, failed;
#define CHECK(cond, what) do { checks++; if (!(cond)) { failed++; printf("FAIL: %s\n", what); } } while (0)

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build";
    char path[512], bad[512];
    snprintf(path, sizeof path, "%s/career_test.sav", dir);
    snprintf(bad, sizeof bad, "%s/career_test_bad.sav", dir);
    remove(path);

    Career c;
    CHECK(career_load(&c, path) == 0 && c.money == 0 && c.nevents == 0, "no file: new career");

    CHECK(career_payout(1, 1, 0) == 400 && career_payout(6, 1, 0) == 1000, "first win pays by kind");
    CHECK(career_payout(1, 2, 0) == 0, "second place pays nothing");
    CHECK(career_payout(1, 1, 1) == 100, "repeat win pays a quarter");
    CHECK(career_payout(0, 1, 0) == 0 && career_payout(7, 1, 0) == 0, "unknown kinds pay nothing");

    CHECK(career_race_result(&c, 4100, 1, 3, 0) == 0 && c.money == 0, "third place: recorded, unpaid");
    CHECK(!career_event_won(&c, 4100) && career_event(&c, 4100)->best_place == 3, "best place kept");
    CHECK(career_race_result(&c, 4100, 1, 1, 0) == 400 && c.money == 400, "win banks the payout");
    CHECK(career_race_result(&c, 4100, 1, 1, 0) == 100 && c.money == 500, "second win banks a quarter");
    CHECK(career_race_result(&c, 4100, 1, 4, 0) == 0 && career_event(&c, 4100)->best_place == 1,
          "a worse finish does not lower the best place");
    CHECK(career_race_result(&c, 4200, 3, 1, 2) == 0 && !career_event_won(&c, 4200) &&
          career_event(&c, 4200)->starts == 1 && career_event(&c, 4200)->best_place == 0,
          "a wrecked run counts as a start only");
    CHECK(career_race_result(&c, 0, 1, 1, 0) == -1 && career_race_result(&c, 5, 1, 0, 0) == -1,
          "event 0 and place 0 are refused");
    CHECK(c.nevents == 2 && career_wins(&c) == 1, "two events seen, one won");

    CHECK(!career_spend(&c, 501) && c.money == 500, "cannot overspend");
    CHECK(!career_spend(&c, -1) && c.money == 500, "negative price refused");
    CHECK(career_spend(&c, 120) && c.money == 380, "spend lowers the balance");

    CHECK(career_save(&c, path), "save");
    Career r;
    CHECK(career_load(&r, path) == 1 && r.money == 380 && r.nevents == 2, "load restores balance and events");
    CHECK(career_event(&r, 4100)->wins == 2 && career_event(&r, 4100)->starts == 4 &&
          career_event(&r, 4100)->best_place == 1 && career_event(&r, 4200)->starts == 1, "event rows round-trip");
    r.money = 9000000000LL;
    CHECK(career_save(&r, path) && career_load(&r, path) == 1 && r.money == 9000000000LL, "balance beyond 32 bits");

    static const char *const damaged[] = {
        "", "something else\nmoney 5\n", CAREER_FILE_MAGIC "\n", CAREER_FILE_MAGIC "\nmoney -5\n",
        CAREER_FILE_MAGIC "\nmoney 5\nevent 0 1 1 1\n", CAREER_FILE_MAGIC "\nmoney 5\nevent 7 1 3 2\n",
        CAREER_FILE_MAGIC "\nmoney 5\nevent 7 1 1 1\nevent 7 1 1 1\n", CAREER_FILE_MAGIC "\nmoney 5\ngarbage\n",
    };
    for (unsigned i = 0; i < sizeof damaged / sizeof *damaged; i++) {
        write_file(bad, damaged[i]);
        r.money = 77;
        CHECK(career_load(&r, bad) == -1 && r.money == 0 && r.nevents == 0, "damaged file: refused, new career");
    }
    write_file(bad, CAREER_FILE_MAGIC "\n# note\n\nmoney 5\n");
    CHECK(career_load(&r, bad) == 1 && r.money == 5, "comments and blank lines are accepted");

    Career full; career_new(&full);
    for (int i = 1; i <= CAREER_MAX_EVENTS; i++) career_race_result(&full, (uint32_t)i, 2, 1, 0);
    CHECK(full.nevents == CAREER_MAX_EVENTS && career_race_result(&full, 99999, 2, 1, 0) == -1 &&
          full.money == 300LL * CAREER_MAX_EVENTS, "a full table refuses new events and pays nothing for them");
    CHECK(career_save(&full, path) && career_load(&r, path) == 1 && r.nevents == CAREER_MAX_EVENTS, "full table round-trips");
    CHECK(!career_save(&c, "/nonexistent-dir/x/career.sav"), "unwritable path reports failure");

    remove(path); remove(bad);
    printf("career_test: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed != 0;
}
