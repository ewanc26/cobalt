#pragma once

/*
 * The Account screen: the signed-in identity, the post-language cycle, the
 * muted and blocked lists, and sign-out.
 *
 * Kept out of app_home.c because it is a screen of its own with its own touch
 * targets and row state, not part of the home menu. The same module draws both
 * surfaces from one selection; `surface` only decides whether the GamePad's
 * hit rectangles are recorded.
 */

#include "app/app.h"

#ifdef __cplusplus
extern "C" {
#endif

void cobalt_app_update_account(cobalt_app *app, const cobalt_input *in);
void cobalt_app_draw_account(cobalt_app *app, cobalt_render *r,
                             cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
