#pragma once

/*
 * The Settings screen: theme, post-text scale, high contrast and a reset row.
 *
 * Kept out of app_home.c because it is a screen of its own with its own touch
 * targets and persistence, not part of the home menu. The screen draws either
 * surface from identical state, the same way every other Cobalt screen does;
 * `surface` only decides whether the GamePad's hit rectangles are recorded.
 */

#include "app/app.h"

#ifdef __cplusplus
extern "C" {
#endif

void cobalt_settings_view_update(cobalt_app *app, const cobalt_input *in);
void cobalt_settings_view_draw(cobalt_app *app, cobalt_render *r,
                               cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
