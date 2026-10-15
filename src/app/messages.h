#pragma once

/*
 * The direct message inbox screen.
 *
 * Read-only for now: a row shows who the conversation is with, the last
 * message and how many are unread. Opening a conversation needs the message
 * view, which is the next step (cobalt#107), so confirm does nothing yet and
 * the hints say so rather than promising it.
 *
 * Rows are the same compact shape as notifications rather than post cards:
 * a conversation is a person and a line of text, not a post to read in full.
 */

#include "input/input.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_MESSAGES_STAY = 0,
   COBALT_MESSAGES_BACK,
   COBALT_MESSAGES_OPEN,
} cobalt_messages_action;

typedef struct {
   cobalt_listnav nav;
} cobalt_messages_view;

void cobalt_messages_view_init(cobalt_messages_view *view);
void cobalt_messages_view_rewind(cobalt_messages_view *view);

cobalt_messages_action cobalt_messages_view_update(cobalt_messages_view *view,
                                                   const cobalt_input *in);

void cobalt_messages_view_draw(cobalt_messages_view *view, cobalt_render *r,
                               cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
