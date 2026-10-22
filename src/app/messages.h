#pragma once

/*
 * The direct message inbox screen.
 *
 * A row shows who the conversation is with, the last message and how many are
 * unread. Confirm opens a conversation. X starts a new one: the keyboard takes
 * a handle or DID, and once the service has found or created the conversation
 * the screen opens it. Rows are the same compact shape as notifications rather
 * than post cards: a conversation is a person and a line of text, not a post
 * to read in full.
 *
 * Y shows the message requests: conversations someone else started. There A
 * accepts the highlighted one, which then opens, and X declines it, which only
 * removes it from the list. Each is one explicit press and one job; nothing is
 * sent on entry, and nothing is retried.
 */

#include "atproto/chat.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_MESSAGES_STAY = 0,
   COBALT_MESSAGES_BACK,
   COBALT_MESSAGES_OPEN,
   /* A handle or DID was accepted from the keyboard. The caller begins the
    * start job with the target, then reports whether it began. */
   COBALT_MESSAGES_START,
   /* A request was accepted or declined. The target is in view->decision; the
    * caller begins the job and reports whether it began. */
   COBALT_MESSAGES_ACCEPT,
   COBALT_MESSAGES_DECLINE,
} cobalt_messages_action;

typedef enum {
   COBALT_MESSAGES_MODE_INBOX = 0,
   COBALT_MESSAGES_MODE_REQUESTS,
} cobalt_messages_mode;

/*
 * The one accept or decline in flight, or the one last pressed. Set when a
 * press is made, and `pending` only once the job has begun, so a refused press
 * cannot leave a decision marked as in flight.
 */
typedef struct {
   char convo_id[COBALT_CHAT_ID_MAX];
   char name[COBALT_POST_NAME_MAX];   /* the counterpart, for opening it */
   bool accept;                       /* true to accept, false to decline */
   bool pending;                      /* the job is in flight */
} cobalt_messages_decision;

typedef enum {
   COBALT_MESSAGES_DECISION_NONE = 0,   /* no job was in flight */
   COBALT_MESSAGES_DECISION_ACCEPTED,
   COBALT_MESSAGES_DECISION_DECLINED,
   COBALT_MESSAGES_DECISION_FAILED,
} cobalt_messages_decision_outcome;

/*
 * The new-conversation state, apart from the screen so its transitions can be
 * tested on the host. The keyboard edits `target` in place, so a refused or
 * failed start keeps what was typed for the person to correct.
 */
typedef struct {
   char target[COBALT_CHAT_TARGET_MAX];
   cobalt_keyboard kb;
   bool editing;   /* the keyboard is open on target */
   bool pending;   /* the start job is in flight */
} cobalt_messages_start;

typedef enum {
   COBALT_MESSAGES_START_NONE = 0,   /* no start was in flight: nothing to do */
   COBALT_MESSAGES_START_OPEN,       /* a conversation id came back: open it */
   COBALT_MESSAGES_START_FAILED,     /* the job failed, or answered without an id */
} cobalt_messages_start_outcome;

typedef struct {
   cobalt_listnav nav;
   cobalt_listnav requests_nav;
   cobalt_messages_mode mode;
   cobalt_messages_start start;
   cobalt_messages_decision decision;
} cobalt_messages_view;

void cobalt_messages_view_init(cobalt_messages_view *view);
void cobalt_messages_view_rewind(cobalt_messages_view *view);

cobalt_messages_action cobalt_messages_view_update(cobalt_messages_view *view,
                                                   const cobalt_input *in);

void cobalt_messages_view_draw(cobalt_messages_view *view, cobalt_render *r,
                               cobalt_surface_id surface);

/* Open the keyboard on the new-conversation target, keeping what was typed. */
void cobalt_messages_start_open(cobalt_messages_start *s);

/* The start job was begun (`began`) or refused without a job. A refusal opens
 * the keyboard again on the same text, so it can be corrected. */
void cobalt_messages_start_began(cobalt_messages_start *s, bool began);

/*
 * The start job finished. Only a finish that follows a began start counts;
 * otherwise NONE. A conversation id clears the target and opens it; anything
 * else is a failure, and keeps the target for another try.
 */
cobalt_messages_start_outcome cobalt_messages_start_finished(cobalt_messages_start *s,
                                                             bool ok,
                                                             const char *convo_id);

/* The target as a display name: no leading '@' and no surrounding spaces. */
bool cobalt_messages_start_name(const cobalt_messages_start *s, char *out, size_t cap);

/* The accept or decline job began (`began`). A refusal changes nothing, so an
 * accept already in flight keeps its state. */
void cobalt_messages_decision_began(cobalt_messages_view *view, bool began);

/*
 * The accept or decline job finished. Only a finish that follows a began
 * decision counts; otherwise NONE. A failure leaves the request where it is.
 */
cobalt_messages_decision_outcome cobalt_messages_decision_finished(cobalt_messages_view *view,
                                                                   bool ok);

#ifdef __cplusplus
}
#endif
