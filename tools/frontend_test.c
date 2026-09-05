/* frontend_test.c — behavioral tests for frontend state machine.
 * GL-free: tests only frontend.c logic, not the drawing adapter.
 * Build: cc -std=c99 -Wall -I. frontend_test.c frontend.c -lm */
#include "frontend.h"
#include <assert.h>
#include <stdio.h>

#define T(name) static void name(void)
#define RUN(t) do { printf("  %-52s", #t); t(); printf("OK\n"); tests++; } while(0)

static int tests;

/* Standard menu: Free Roam (on), Race (off), Quit (on). */
static const FeMenuEntry STD_MENU[] = {
    {"FREE ROAM",    FE_ACTION_FREE_ROAM,  1},
    {"RACE",         FE_ACTION_RACE_SELECT, 0},
    {"QUIT",         FE_ACTION_QUIT,        1},
};

static void setup(Fe *fe) {
    fe_init(fe);
    fe_set_entries(fe, STD_MENU, 3);
}

/* ---- init / title ---- */

T(test_init_title) {
    Fe fe;
    fe_init(&fe);
    assert(fe_screen(&fe) == FE_SCREEN_TITLE);
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
}

T(test_title_ignores_nav) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_UP);
    fe_input(&fe, FE_INPUT_DOWN);
    fe_input(&fe, FE_INPUT_BACK);
    assert(fe_screen(&fe) == FE_SCREEN_TITLE);
}

T(test_title_to_main) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);
    assert(fe_screen(&fe) == FE_SCREEN_MAIN);
}

/* ---- confirm leak prevention ---- */

T(test_confirm_no_leak_title_to_main) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, guard armed */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* eaten by guard */
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
    /* next confirm works */
    fe_input(&fe, FE_INPUT_CONFIRM);
    assert(fe_poll_action(&fe) == FE_ACTION_FREE_ROAM);
}

T(test_confirm_guard_cleared_by_nav) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, guard armed */
    fe_input(&fe, FE_INPUT_DOWN);      /* clears guard, cursor 0→2 (QUIT) */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* activates QUIT entry */
    assert(fe_screen(&fe) == FE_SCREEN_QUIT_CONFIRM);
}

T(test_confirm_no_leak_main_to_quit) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, guard armed */
    fe_input(&fe, FE_INPUT_DOWN);      /* clears guard, cursor → 2 (QUIT) */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* main → quit_confirm, guard armed */
    assert(fe_screen(&fe) == FE_SCREEN_QUIT_CONFIRM);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* must be eaten, not bounce to main */
    assert(fe_screen(&fe) == FE_SCREEN_QUIT_CONFIRM);
}

T(test_confirm_no_leak_quit_to_main) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, guard armed */
    fe_input(&fe, FE_INPUT_BACK);      /* clears guard, → quit_confirm */
    /* BACK path sets no guard on quit_confirm, so confirm works immediately */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* quit_sel=0 (NO) → main, guard armed */
    assert(fe_screen(&fe) == FE_SCREEN_MAIN);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* must be eaten by guard */
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
}

T(test_quit_guard_then_normal) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_DOWN);      /* → QUIT, clears guard */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* → quit_confirm, guard armed */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* eaten */
    assert(fe_screen(&fe) == FE_SCREEN_QUIT_CONFIRM);
    /* guard consumed, toggle to YES, confirm */
    fe_input(&fe, FE_INPUT_DOWN);      /* toggle → YES */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* YES → emit quit */
    assert(fe_poll_action(&fe) == FE_ACTION_QUIT);
}

/* ---- navigation ---- */

T(test_cursor_skips_disabled) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, cursor=0 */
    fe_input(&fe, FE_INPUT_DOWN);      /* skip RACE(1) → QUIT(2) */
    assert(fe_selected(&fe) == 2);
}

T(test_cursor_wraps_down) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, cursor=0 */
    fe_input(&fe, FE_INPUT_DOWN);      /* → 2 */
    fe_input(&fe, FE_INPUT_DOWN);      /* wrap → 0 */
    assert(fe_selected(&fe) == 0);
}

T(test_cursor_wraps_up) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, cursor=0 */
    fe_input(&fe, FE_INPUT_UP);        /* wrap → 2 */
    assert(fe_selected(&fe) == 2);
}

T(test_set_entries_snaps_first_enabled) {
    Fe fe;
    FeMenuEntry e[] = {
        {"A", FE_ACTION_FREE_ROAM,   0},
        {"B", FE_ACTION_RACE_SELECT, 1},
        {"C", FE_ACTION_QUIT,        1},
    };
    fe_init(&fe);
    fe_set_entries(&fe, e, 3);
    assert(fe_selected(&fe) == 1);
}

/* ---- disabled entries ---- */

T(test_disabled_cannot_activate) {
    Fe fe;
    FeMenuEntry e[] = {
        {"X", FE_ACTION_FREE_ROAM, 0},
        {"Y", FE_ACTION_QUIT,      0},
    };
    fe_init(&fe);
    fe_set_entries(&fe, e, 2);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_DOWN);      /* clears guard; no enabled entry to move to */
    fe_input(&fe, FE_INPUT_CONFIRM);
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
}

/* ---- action polling ---- */

T(test_poll_exactly_once) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_DOWN);      /* → 2, clears guard */
    fe_input(&fe, FE_INPUT_DOWN);      /* wrap → 0 (FREE ROAM) */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* activate FREE ROAM */
    assert(fe_poll_action(&fe) == FE_ACTION_FREE_ROAM);
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
}

/* ---- quit confirm ---- */

T(test_back_opens_quit) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_BACK);      /* → quit_confirm */
    assert(fe_screen(&fe) == FE_SCREEN_QUIT_CONFIRM);
    assert(fe_quit_sel(&fe) == 0);
}

T(test_quit_no_cancels) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_BACK);      /* → quit_confirm, quit_sel=0 */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* NO → main */
    assert(fe_screen(&fe) == FE_SCREEN_MAIN);
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
}

T(test_quit_yes_emits) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_BACK);      /* → quit_confirm, quit_sel=0 */
    fe_input(&fe, FE_INPUT_DOWN);      /* toggle → YES */
    assert(fe_quit_sel(&fe) == 1);
    fe_input(&fe, FE_INPUT_CONFIRM);
    assert(fe_poll_action(&fe) == FE_ACTION_QUIT);
}

T(test_quit_back_cancels) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_BACK);      /* → quit_confirm */
    fe_input(&fe, FE_INPUT_DOWN);      /* YES */
    fe_input(&fe, FE_INPUT_BACK);      /* cancel → main */
    assert(fe_screen(&fe) == FE_SCREEN_MAIN);
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
}

/* ---- reinit ---- */

T(test_reinit_clears_pending) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main */
    fe_input(&fe, FE_INPUT_DOWN);      /* clears guard, → 2 */
    fe_input(&fe, FE_INPUT_DOWN);      /* → 0 (FREE ROAM) */
    fe_input(&fe, FE_INPUT_CONFIRM);   /* pending FREE_ROAM */
    fe_init(&fe);
    assert(fe_poll_action(&fe) == FE_ACTION_NONE);
    assert(fe_screen(&fe) == FE_SCREEN_TITLE);
}

/* ---- held repeat ---- */

T(test_held_fires_after_delay) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, cursor=0 */
    fe_input(&fe, FE_INPUT_DOWN);      /* → 2 */
    assert(fe_selected(&fe) == 2);
    fe_held(&fe, +1, 0.01f);           /* start hold */
    assert(fe_selected(&fe) == 2);
    fe_held(&fe, +1, 0.36f);           /* past 0.35s → repeat fires */
    assert(fe_selected(&fe) == 0);     /* wrapped to 0 */
}

T(test_held_reset_on_dir_change) {
    Fe fe; setup(&fe);
    fe_input(&fe, FE_INPUT_CONFIRM);   /* title → main, cursor=0 */
    fe_input(&fe, FE_INPUT_DOWN);      /* → 2 */
    fe_held(&fe, +1, 0.01f);           /* establish direction */
    fe_held(&fe, +1, 0.30f);           /* 0.30 < 0.35, no fire */
    fe_held(&fe, -1, 0.01f);           /* direction change resets timer */
    fe_held(&fe, -1, 0.30f);           /* 0.30 since reset < 0.35 */
    assert(fe_selected(&fe) == 2);     /* no repeat fired */
}

/* ---- pulse ---- */

T(test_pulse_bounded) {
    Fe fe;
    fe_init(&fe);
    for (int i = 0; i < 1000; i++) fe_update(&fe, 0.016f);
    float p = fe_pulse(&fe);
    assert(p >= 0.0f && p <= 1.0f);
}

/* ---- query safety ---- */

T(test_entry_oob_returns_null) {
    Fe fe; setup(&fe);
    assert(fe_entry(&fe, -1) == 0);
    assert(fe_entry(&fe, 99) == 0);
    assert(fe_entry(&fe, 0) != 0);
}

/* ---- main ---- */
int main(void) {
    printf("frontend_test:\n");
    RUN(test_init_title);
    RUN(test_title_ignores_nav);
    RUN(test_title_to_main);
    RUN(test_confirm_no_leak_title_to_main);
    RUN(test_confirm_guard_cleared_by_nav);
    RUN(test_confirm_no_leak_main_to_quit);
    RUN(test_confirm_no_leak_quit_to_main);
    RUN(test_quit_guard_then_normal);
    RUN(test_cursor_skips_disabled);
    RUN(test_cursor_wraps_down);
    RUN(test_cursor_wraps_up);
    RUN(test_set_entries_snaps_first_enabled);
    RUN(test_disabled_cannot_activate);
    RUN(test_poll_exactly_once);
    RUN(test_back_opens_quit);
    RUN(test_quit_no_cancels);
    RUN(test_quit_yes_emits);
    RUN(test_quit_back_cancels);
    RUN(test_reinit_clears_pending);
    RUN(test_held_fires_after_delay);
    RUN(test_held_reset_on_dir_change);
    RUN(test_pulse_bounded);
    RUN(test_entry_oob_returns_null);
    printf("ALL PASSED (%d tests)\n", tests);
    return 0;
}
