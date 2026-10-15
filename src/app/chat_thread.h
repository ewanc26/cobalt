#pragma once

/*
 * One direct message conversation.
 *
 * Messages are bubbles: the signed-in account's on the right, the other
 * person's on the left, the newest at the bottom. Up and Down move back through
 * the messages held. A opens the keyboard to write a reply; Y sends the reply,
 * and a reply that fails to send stays as the draft to try again.
 */

#include "atproto/chat.h"
#include "input/input.h"
#include "ui/keyboard.h"
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
   char convo_id[COBALT_CHAT_ID_MAX];
   int offset;          /* messages skipped from the newest end; 0 shows the newest */

   /* The reply being written. It survives a failed send, and is cleared only
    * once the service has taken it. */
   char reply[COBALT_CHAT_REPLY_MAX];
   cobalt_keyboard kb;
   bool replying;       /* the keyboard is open on the reply */
} cobalt_chat_thread_view;

void cobalt_chat_thread_view_init(cobalt_chat_thread_view *view);

/* Start showing a conversation from its newest messages. A draft for another
 * conversation is dropped, so a reply can only go to the one it was written for. */
void cobalt_chat_thread_view_open(cobalt_chat_thread_view *view, const char *title,
                                  const char *convo_id);

/* The reply went out: clear the draft. */
void cobalt_chat_thread_view_sent(cobalt_chat_thread_view *view);

cobalt_chat_thread_action cobalt_chat_thread_view_update(cobalt_chat_thread_view *view,
                                                         const cobalt_input *in);

void cobalt_chat_thread_view_draw(cobalt_chat_thread_view *view, cobalt_render *r,
                                  cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
