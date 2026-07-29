#pragma once

/*
 * The notifications screen.
 *
 * Rows here are not posts, so they do not use the post card: a like carries no
 * text of its own, and a follow has nothing to open. Drawing them as cut-down
 * posts would leave a column of near-empty tiles. They get their own compact
 * row instead — actor, what they did, and the text when there is any.
 *
 * Opening a row goes to the relevant thread, which is why the parse step
 * resolves the subject URI: the screen should not have to know that a like
 * points at reasonSubject while a reply points at itself.
 */

#include "atproto/notifications.h"
#include "input/input.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_NOTIFY_STAY = 0,
   COBALT_NOTIFY_BACK,
   COBALT_NOTIFY_OPEN_THREAD,
} cobalt_notify_action;

typedef struct {
   int selected;
   int scroll;
   int last_visible;

   SDL_Rect hit[COBALT_NOTIFICATIONS_MAX];
   int hit_index[COBALT_NOTIFICATIONS_MAX];
   int hit_count;
   bool hit_valid;
} cobalt_notify_view;

void cobalt_notify_view_init(cobalt_notify_view *view);
void cobalt_notify_view_rewind(cobalt_notify_view *view);

cobalt_notify_action cobalt_notify_view_update(cobalt_notify_view *view,
                                               const cobalt_input *in);

void cobalt_notify_view_draw(cobalt_notify_view *view, cobalt_render *r,
                             cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
