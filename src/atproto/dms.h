#pragma once

#include "atproto/feed.h"
#include "util/clock.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_CONVO_MAX 50
#define COBALT_DMS_MSG_MAX 80
#define COBALT_DMS_NAME_MAX COBALT_POST_NAME_MAX

typedef struct {
   char id[COBALT_POST_URI_MAX];
   char handle[COBALT_DMS_NAME_MAX];
   char name[COBALT_DMS_NAME_MAX];
   char avatar[COBALT_POST_AVATAR_MAX];
   char last_text[COBALT_POST_TEXT_MAX];
   int unread;
   bool has_unread;
   char age[COBALT_RELATIVE_MAX];
} cobalt_convo;

typedef struct {
   char id[COBALT_POST_URI_MAX];
   char sender[COBALT_DMS_NAME_MAX];
   char sender_avatar[COBALT_POST_AVATAR_MAX];
   char text[COBALT_POST_TEXT_MAX];
   char age[COBALT_RELATIVE_MAX];
   bool is_mine;
} cobalt_message;

typedef struct {
   cobalt_convo items[COBALT_CONVO_MAX];
   int count;
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
} cobalt_convos;

typedef struct {
   cobalt_message items[COBALT_DMS_MSG_MAX];
   int count;
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
   char convo_id[COBALT_POST_URI_MAX];
} cobalt_messages;

void cobalt_convos_reset(cobalt_convos *list);
void cobalt_messages_reset(cobalt_messages *list);

/* Whether the convo list can be paged further — true only when the server
 * returned a cursor and the window is not full. */
bool cobalt_convos_can_page(const cobalt_convos *list);

#ifdef COBALT_HAS_WOLFRAM

#include <wolfram/chat_typed.h>
#include <wolfram/agent.h>

bool cobalt_convos_append_from_wolfram(cobalt_convos *out, const wf_chat_convo_list *list,
                                       const char *my_did);
bool cobalt_messages_append_from_wolfram(cobalt_messages *out, const wf_chat_message_list *list,
                                       const char *my_did);

#endif

#ifdef __cplusplus
}
#endif
