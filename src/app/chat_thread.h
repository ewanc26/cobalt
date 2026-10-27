#pragma once

/*
 * One direct message conversation.
 *
 * Messages are bubbles: the signed-in account's on the right, the other
 * person's on the left, the newest at the bottom. Up and Down move back through
 * the messages held. A opens the keyboard to write a reply; Y sends the reply,
 * and a reply that fails to send stays as the draft to try again.
 *
 * A group (more than one other member) has a second sub-mode on the same
 * screen: X opens the group's pending join requests, A approves the highlighted
 * one and X rejects it, and B returns to the conversation. Only a group's owner
 * may list them, so a conversation that is not a group never offers it.
 */

#include "atproto/chat.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_CHAT_THREAD_STAY = 0,
   COBALT_CHAT_THREAD_BACK,
   /* A join request was approved or rejected. The target is in
    * view->join_decision; the caller begins the job and reports whether it
    * began. */
   COBALT_CHAT_THREAD_JOIN_APPROVE,
   COBALT_CHAT_THREAD_JOIN_REJECT,
} cobalt_chat_thread_action;

typedef enum {
   COBALT_CHAT_THREAD_MODE_MESSAGES = 0,
   COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS,
} cobalt_chat_thread_mode;

/*
 * The one approve or reject in flight, or the one last pressed. Set when a
 * press is made, and `pending` only once the job has begun, so a refused press
 * cannot leave a decision marked as in flight.
 */
typedef struct {
   char did[COBALT_CHAT_TARGET_MAX];
   char name[COBALT_POST_NAME_MAX];   /* the requester, for the result notice */
   bool approve;                      /* true to approve, false to reject */
   bool pending;                      /* the job is in flight */
} cobalt_chat_thread_decision;

typedef enum {
   COBALT_CHAT_THREAD_DECISION_NONE = 0,   /* no job was in flight */
   COBALT_CHAT_THREAD_DECISION_APPROVED,
   COBALT_CHAT_THREAD_DECISION_REJECTED,
   COBALT_CHAT_THREAD_DECISION_FAILED,
} cobalt_chat_thread_decision_outcome;

typedef struct {
   char title[96];      /* the other person, as the inbox row named them */
   char convo_id[COBALT_CHAT_ID_MAX];
   int offset;          /* messages skipped from the newest end; 0 shows the newest */
   bool group;          /* more than one other member: join requests can be opened */

   /* The reply being written. It survives a failed send, and is cleared only
    * once the service has taken it. */
   char reply[COBALT_CHAT_REPLY_MAX];
   cobalt_keyboard kb;
   bool replying;       /* the keyboard is open on the reply */

   /* The group's join requests, on their own sub-mode of this screen. */
   cobalt_chat_thread_mode mode;
   cobalt_listnav join_nav;
   cobalt_chat_thread_decision join_decision;
} cobalt_chat_thread_view;

void cobalt_chat_thread_view_init(cobalt_chat_thread_view *view);

/* Start showing a conversation from its newest messages, in message mode. A
 * draft for another conversation is dropped, so a reply can only go to the one
 * it was written for. `group` says whether the conversation has more than one
 * other member, and so whether its join requests can be opened. */
void cobalt_chat_thread_view_open(cobalt_chat_thread_view *view, const char *title,
                                  const char *convo_id, bool group);

/* The reply went out: clear the draft. */
void cobalt_chat_thread_view_sent(cobalt_chat_thread_view *view);

/* The approve or reject job began (`began`). A refusal changes nothing, so a
 * decision already in flight keeps its state. */
void cobalt_chat_thread_decision_began(cobalt_chat_thread_view *view, bool began);

/*
 * The approve or reject job finished. Only a finish that follows a began
 * decision counts; otherwise NONE. A failure leaves the request where it is.
 */
cobalt_chat_thread_decision_outcome cobalt_chat_thread_decision_finished(
   cobalt_chat_thread_view *view, bool ok);

cobalt_chat_thread_action cobalt_chat_thread_view_update(cobalt_chat_thread_view *view,
                                                         const cobalt_input *in);

void cobalt_chat_thread_view_draw(cobalt_chat_thread_view *view, cobalt_render *r,
                                  cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
