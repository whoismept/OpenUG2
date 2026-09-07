/* frontend.h — OpenUG2 menu state machine.
 * GL-free, SDL-free: state and navigation only. The host owns the window,
 * GL context and application loop. This module owns menu state, nothing else.
 *
 * Lifecycle:
 *   fe_init()           → reset to title screen
 *   fe_input()          → feed semantic inputs each frame
 *   fe_poll_action()    → consume exactly-once host actions
 *   fe_update()         → advance animations (call once per frame)
 *   fe_state()/fe_*()   → query state for drawing
 *
 * The module never loads worlds, changes vehicles, starts races, resets
 * physics, relaunches the process or exits the application. */
#ifndef OPENUG2_FRONTEND_H
#define OPENUG2_FRONTEND_H

/* ---- screens ---- */
typedef enum {
    FE_SCREEN_TITLE,        /* "press enter to start" */
    FE_SCREEN_MAIN,         /* main menu with selectable entries */
    FE_SCREEN_QUIT_CONFIRM  /* "quit? yes/no" */
} FeScreen;

/* ---- semantic input events (one per press, not held) ---- */
typedef enum {
    FE_INPUT_NONE = 0,
    FE_INPUT_UP,
    FE_INPUT_DOWN,
    FE_INPUT_CONFIRM,
    FE_INPUT_BACK
} FeInput;

/* ---- actions emitted to the host (exactly once per confirmation) ---- */
typedef enum {
    FE_ACTION_NONE = 0,
    FE_ACTION_FREE_ROAM,
    FE_ACTION_RACE_SELECT,
    FE_ACTION_QUIT
} FeAction;

/* ---- main menu entries ---- */
#define FE_MENU_MAX 8
typedef struct {
    const char *label;
    FeAction    action;   /* what confirming this entry requests */
    int         enabled;
} FeMenuEntry;

/* ---- full state ---- */
typedef struct {
    FeScreen  screen;
    /* main menu */
    FeMenuEntry entries[FE_MENU_MAX];
    int         entry_count;
    int         selected;       /* cursor index in entries[] */
    int         quit_sel;       /* 0 = NO (cancel), 1 = YES (quit) */
    /* animation */
    float       pulse;          /* 0..1 oscillator for pulsing prompts */
    /* pending action (consumed by fe_poll_action) */
    FeAction    pending;
    /* held-input repeat state */
    int         held_dir;       /* +1 = down held, -1 = up held, 0 = released */
    float       held_timer;     /* seconds since hold started */
    float       held_repeat;    /* seconds until next repeat fires */
    int         confirm_consumed; /* prevents confirm leaking across screens */
} Fe;

/* Initialize / reset to title screen. */
void fe_init(Fe *fe);

/* Set main-menu entries. Call after fe_init, before the first frame.
 * Entries are copied. Count must be <= FE_MENU_MAX. */
void fe_set_entries(Fe *fe, const FeMenuEntry *entries, int count);

/* Feed one semantic input event. Call once per press (not per frame). */
void fe_input(Fe *fe, FeInput input);

/* Feed held-direction state for frame-rate-independent repeat.
 * dir: +1 = down, -1 = up, 0 = released. dt: seconds since last call. */
void fe_held(Fe *fe, int dir, float dt);

/* Advance animation timers. dt in seconds. */
void fe_update(Fe *fe, float dt);

/* Consume the pending action (returns FE_ACTION_NONE if nothing). */
FeAction fe_poll_action(Fe *fe);

/* Query. */
FeScreen     fe_screen(const Fe *fe);
int          fe_selected(const Fe *fe);
int          fe_entry_count(const Fe *fe);
const FeMenuEntry *fe_entry(const Fe *fe, int i);
int          fe_quit_sel(const Fe *fe);
float        fe_pulse(const Fe *fe);

#endif
