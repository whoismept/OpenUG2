/* frontend.c — menu state machine. GL-free, SDL-free. */
#include "frontend.h"
#include <string.h>
#include <math.h>

/* held-input repeat timing (seconds) */
#define HOLD_INITIAL 0.35f
#define HOLD_REPEAT  0.12f

void fe_init(Fe *fe) {
    memset(fe, 0, sizeof *fe);
    fe->screen = FE_SCREEN_TITLE;
}

void fe_set_entries(Fe *fe, const FeMenuEntry *entries, int count) {
    if (count > FE_MENU_MAX) count = FE_MENU_MAX;
    fe->entry_count = count;
    for (int i = 0; i < count; i++) fe->entries[i] = entries[i];
    fe->selected = 0;
    /* snap to first enabled entry */
    for (int i = 0; i < count; i++)
        if (fe->entries[i].enabled) { fe->selected = i; return; }
}

/* Move cursor, skipping disabled entries. Returns 1 if moved. */
static int move_cursor(Fe *fe, int dir) {
    int n = fe->entry_count;
    if (n == 0) return 0;
    int start = fe->selected;
    int pos = start;
    for (int i = 0; i < n; i++) {
        pos = (pos + dir + n) % n;
        if (fe->entries[pos].enabled) {
            fe->selected = pos;
            return pos != start;
        }
    }
    return 0;  /* no enabled entry found */
}

void fe_input(Fe *fe, FeInput input) {
    if (input == FE_INPUT_NONE) return;

    switch (fe->screen) {
    case FE_SCREEN_TITLE:
        if (input == FE_INPUT_CONFIRM) {
            fe->screen = FE_SCREEN_MAIN;
            fe->confirm_consumed = 1;  /* don't leak into main menu */
        }
        break;

    case FE_SCREEN_MAIN:
        if (fe->confirm_consumed) {
            /* eat the first confirm after a screen transition */
            fe->confirm_consumed = 0;
            if (input == FE_INPUT_CONFIRM) return;
        }
        if (input == FE_INPUT_UP) move_cursor(fe, -1);
        else if (input == FE_INPUT_DOWN) move_cursor(fe, +1);
        else if (input == FE_INPUT_CONFIRM) {
            if (fe->entry_count > 0 && fe->selected >= 0 &&
                fe->selected < fe->entry_count &&
                fe->entries[fe->selected].enabled) {
                FeAction act = fe->entries[fe->selected].action;
                if (act == FE_ACTION_QUIT) {
                    fe->screen = FE_SCREEN_QUIT_CONFIRM;
                    fe->quit_sel = 0;  /* default to NO */
                    fe->confirm_consumed = 1;
                } else {
                    fe->pending = act;
                }
            }
        } else if (input == FE_INPUT_BACK) {
            fe->screen = FE_SCREEN_QUIT_CONFIRM;
            fe->quit_sel = 0;
        }
        break;

    case FE_SCREEN_QUIT_CONFIRM:
        if (fe->confirm_consumed) {
            fe->confirm_consumed = 0;
            if (input == FE_INPUT_CONFIRM) return;
        }
        if (input == FE_INPUT_UP || input == FE_INPUT_DOWN)
            fe->quit_sel = !fe->quit_sel;
        else if (input == FE_INPUT_CONFIRM) {
            if (fe->quit_sel) fe->pending = FE_ACTION_QUIT;
            else { fe->screen = FE_SCREEN_MAIN; fe->confirm_consumed = 1; }
        } else if (input == FE_INPUT_BACK) {
            fe->screen = FE_SCREEN_MAIN;
        }
        break;
    }
}

void fe_held(Fe *fe, int dir, float dt) {
    if (dir == 0 || dir != fe->held_dir) {
        fe->held_dir = dir;
        fe->held_timer = 0;
        fe->held_repeat = HOLD_INITIAL;
        return;
    }
    fe->held_timer += dt;
    if (fe->held_timer >= fe->held_repeat) {
        fe->held_timer -= fe->held_repeat;
        fe->held_repeat = HOLD_REPEAT;
        FeInput inp = (dir > 0) ? FE_INPUT_DOWN : FE_INPUT_UP;
        fe_input(fe, inp);
    }
}

void fe_update(Fe *fe, float dt) {
    fe->pulse += dt * 3.0f;
    if (fe->pulse > 6.2831853f) fe->pulse -= 6.2831853f;
}

FeAction fe_poll_action(Fe *fe) {
    FeAction a = fe->pending;
    fe->pending = FE_ACTION_NONE;
    return a;
}

FeScreen     fe_screen(const Fe *fe) { return fe->screen; }
int          fe_selected(const Fe *fe) { return fe->selected; }
int          fe_entry_count(const Fe *fe) { return fe->entry_count; }
const FeMenuEntry *fe_entry(const Fe *fe, int i) {
    if (i < 0 || i >= fe->entry_count) return 0;
    return &fe->entries[i];
}
int          fe_quit_sel(const Fe *fe) { return fe->quit_sel; }
float        fe_pulse(const Fe *fe) {
    return 0.5f + 0.5f * sinf(fe->pulse);
}
