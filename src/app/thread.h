#pragma once

/*
 * The thread screen — a post and the conversation around it.
 *
 * Reuses the timeline's card idiom so moving between the two does not feel
 * like changing application: same selection-based scrolling, same layout rules
 * per surface. What it adds is an indent per row, carrying as much of the
 * reply tree as a 854-pixel-wide panel can show.
 *
 * It opens on the post the user selected rather than at the top, because a
 * long ancestor chain would otherwise bury the thing they asked to read.
 */

#include "atproto/feed.h"
#include "input/input.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_THREAD_VIEW_STAY = 0,
   COBALT_THREAD_VIEW_BACK,
   COBALT_THREAD_VIEW_REPLY,
   COBALT_THREAD_VIEW_QUOTE,
   COBALT_THREAD_VIEW_MENU,
} cobalt_thread_action;

typedef struct {
   cobalt_listnav nav;

   /* Line scroll inside the selected post's text, for posts longer than the
    * screen. total and window are measured by the last draw. */
   int text_scroll;
   int text_total;
   int text_window;
   int text_for;

   /* Set once after a fetch so the view can jump to the post that was opened,
    * exactly once, rather than fighting the user's scrolling afterwards. */
   bool centred;

   /* Set while "delete this post?" is on screen. A is yes, B is no, and
    * nothing else acts — deleting is the one action here that cannot be
    * undone, and Y sits next to buttons that are casually pressed. */
   bool confirm_delete;
   char delete_uri[COBALT_POST_URI_MAX];
} cobalt_thread_view;

void cobalt_thread_view_init(cobalt_thread_view *view);

/* Forget the cursor so the next draw re-centres on the focused post. */
void cobalt_thread_view_reset(cobalt_thread_view *view);

cobalt_thread_action cobalt_thread_view_update(cobalt_thread_view *view,
                                               const cobalt_input *in);

void cobalt_thread_view_draw(cobalt_thread_view *view, cobalt_render *r,
                             cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
