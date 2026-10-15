#pragma once

/*
 * One direct message conversation, read-only.
 *
 * Messages are bubbles: the signed-in account's on the right, the other
 * person's on the left, the newest at the bottom where the eye lands. The view
 * opens on the newest messages; Up and Down move back and forward through the
 * ones held. Replying is not here yet.
 */

#include "input/input.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_CHAT_THREAD_STAY = 0,
   COBALT_CHAT_THREAD_BACK,
} cobalt_chat_thread_action;

typedef struct {
   char title[96];      /* the other person, as the inbox row named them */
   char convo_id[64];   /* kept to reload with Y */
   int offset;          /* messages skipped from the newest end; 0 shows the newest */
} cobalt_chat_thread_view;

void cobalt_chat_thread_view_init(cobalt_chat_thread_view *view);

/* Start showing a conversation from its newest messages. */
void cobalt_chat_thread_view_open(cobalt_chat_thread_view *view, const char *title,
                                  const char *convo_id);

cobalt_chat_thread_action cobalt_chat_thread_view_update(cobalt_chat_thread_view *view,
                                                         const cobalt_input *in);

void cobalt_chat_thread_view_draw(cobalt_chat_thread_view *view, cobalt_render *r,
                                  cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
