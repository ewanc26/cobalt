#pragma once

/*
 * The content-label settings: the adult-content switch and each configurable
 * label's visibility (hide, warn or ignore). Reached from Account.
 *
 * Nothing shown here changes until the server confirms a write: a row reads the
 * label context, which a successful write rebuilds from the account's
 * preferences. A failed write therefore leaves the previous setting on screen.
 */

#include "app/app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clears the row selection; call when the screen is opened. */
void cobalt_app_open_label_settings(void);
void cobalt_app_update_label_settings(cobalt_app *app, const cobalt_input *in);
void cobalt_app_draw_label_settings(cobalt_app *app, cobalt_render *r,
                                    cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
