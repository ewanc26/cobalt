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
   COBALT_SCREEN_SIGN_IN,
   COBALT_SCREEN_ACCOUNT,
   COBALT_SCREEN_DIAGNOSTICS,
} cobalt_screen;

/*
 * Off-TV Play, v1 shape.
 *
 * The GamePad always carries a complete, self-sufficient UI, so "GamePad only"
 * is not a separate code path — it is what the GamePad surface already does.
 * The toggle controls whether the TV mirrors a paired view or drops to an idle
 * card, which is the user-visible half of the feature and avoids tearing down
 * and rebuilding a window at runtime. Autodetection (AGENTS.md §5) can layer
 * on top of this later without restructuring the render code.
 */
typedef enum {
   COBALT_DISPLAY_DUAL = 0,   /* TV shows the paired view */
   COBALT_DISPLAY_GAMEPAD,    /* TV idles; everything happens on the GamePad */
} cobalt_display_mode;

typedef struct cobalt_app cobalt_app;

cobalt_app *cobalt_app_create(void);
void cobalt_app_destroy(cobalt_app *app);

void cobalt_app_update(cobalt_app *app, const cobalt_input *in, uint32_t now_ms);
void cobalt_app_draw(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface);

bool cobalt_app_should_quit(const cobalt_app *app);

#ifdef __cplusplus
}
#endif
