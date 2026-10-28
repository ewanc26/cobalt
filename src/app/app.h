#pragma once

/*
 * Application state and screen management.
 *
 * The app owns state only; it never owns windows or renderers. Draw is called
 * once per surface per frame with the same state, and each screen decides how
 * to lay itself out for that surface. This is the structural half of AGENTS.md
 * §5's two-mode requirement: the GamePad view is not a scaled TV view, it is a
 * separate layout driven from identical state.
 */

#include "input/input.h"
#include "ui/popup.h"
#include "ui/render.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_SCREEN_HOME = 0,
   COBALT_SCREEN_TIMELINE,
   COBALT_SCREEN_THREAD,
   COBALT_SCREEN_COMPOSE,
   COBALT_SCREEN_NOTIFICATIONS,
   COBALT_SCREEN_PROFILE,
   COBALT_SCREEN_SIGN_IN,
   COBALT_SCREEN_ACCOUNT,
   COBALT_SCREEN_MUTED_LIST,
   COBALT_SCREEN_BLOCKED_LIST,
   COBALT_SCREEN_FOLLOWS_LIST,
   COBALT_SCREEN_LIKES_LIST,
   COBALT_SCREEN_SEARCH,
   COBALT_SCREEN_FEEDS,
   COBALT_SCREEN_LISTS,
   COBALT_SCREEN_DIAGNOSTICS,
   COBALT_SCREEN_ENTROPY,
   COBALT_SCREEN_MESSAGES,
   COBALT_SCREEN_CHAT_THREAD,
   COBALT_SCREEN_QUEUE,
   COBALT_SCREEN_SETTINGS,
} cobalt_screen;

/*
 * Off-TV Play, v1 shape.
 *
 * The GamePad always carries a complete, self-sufficient UI, so "GamePad only"
 * is not a separate code path — it is what the GamePad surface already does
 * when nothing pairs with it. The toggle decides which of the two a surface is
 * drawing: in DUAL the TV reads and the GamePad is the remote
 * (cobalt_surface_role in ui/render.h), and in GAMEPAD the pad is the whole app
 * while the TV drops to an idle card. That avoids tearing down and rebuilding a
 * window at runtime, and it means autodetection (AGENTS.md §5) can layer on top
 * later without restructuring the render code: it would only pick the mode.
 */
typedef enum {
   COBALT_DISPLAY_DUAL = 0,   /* TV reads, GamePad acts */
   COBALT_DISPLAY_GAMEPAD,    /* TV idles; everything happens on the GamePad */
} cobalt_display_mode;

typedef struct cobalt_app cobalt_app;

cobalt_app *cobalt_app_create(void);
void cobalt_app_destroy(cobalt_app *app);

/* Index of the highlighted home entry; lets tests navigate without guessing. */
int cobalt_app_home_selection(const cobalt_app *app);
cobalt_screen cobalt_app_screen(const cobalt_app *app);
/* TV+GamePad, or GamePad only. */
cobalt_display_mode cobalt_app_display(const cobalt_app *app);
/* List cursor and first drawn row, so tests can check they survive a detour. */
void cobalt_app_view_position(const cobalt_app *app, cobalt_screen screen, int *selected,
                              int *scroll);
void cobalt_app_timeline_position(const cobalt_app *app, int *selected, int *scroll);
/* How far back a conversation's window is, in messages; its drag and Up/Down
 * both move this one number. */
int cobalt_app_chat_offset(const cobalt_app *app);

/* The open menu, for tests that tap its rows. NULL when no menu is up. */
const cobalt_popup *cobalt_app_popup(const cobalt_app *app);

void cobalt_app_update(cobalt_app *app, const cobalt_input *in, uint32_t now_ms);
void cobalt_app_draw(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface);

bool cobalt_app_should_quit(const cobalt_app *app);

#ifdef __cplusplus
}
#endif
