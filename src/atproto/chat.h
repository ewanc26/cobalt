#pragma once

/*
 * Direct message conversations, flattened for drawing: the read-only inbox.
 *
 * Same shape as the notifications list (see notifications.h): Wolfram keeps
 * each convo as an owned subtree with the members as profile views, and a
 * render loop needs fixed-size rows with the names already resolved. A convo
 * does not say who the viewer is, so the counterpart is picked by comparing
 * member DIDs against the signed-in account, which is the one thing a row
 * cannot show without.
 *
 * Only the inbox lives here. Reading a conversation's messages, replying and
 * the request inbox are later steps (cobalt#107).
 */

#include "atproto/feed.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_CHAT_CONVOS_MAX 50
#define COBALT_CHAT_ID_MAX     64
#define COBALT_CHAT_PREVIEW_MAX 128

typedef struct {
   char id[COBALT_CHAT_ID_MAX];          /* convo id, for opening it later */
   char name[COBALT_POST_NAME_MAX];      /* counterpart, or "Group" */
   char handle[COBALT_POST_NAME_MAX];    /* with a leading @, empty for a group */
   char avatar[COBALT_POST_AVATAR_MAX];  /* URL, empty when unset */
   char preview[COBALT_CHAT_PREVIEW_MAX]; /* last message text, empty if none */
   int unread;                           /* unread messages in this convo */
   bool group;                           /* more than one other member */
} cobalt_chat_convo;

typedef struct {
   cobalt_chat_convo items[COBALT_CHAT_CONVOS_MAX];
   int count;
   int unread;                           /* unread messages across the inbox */
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
} cobalt_chat_inbox;

void cobalt_chat_inbox_reset(cobalt_chat_inbox *inbox);

/* As cobalt_feed_can_page, for the same reason. */
bool cobalt_chat_inbox_can_page(const cobalt_chat_inbox *inbox);

/*
 * Index of the first member whose DID is not the viewer's, or -1 when every
 * member is the viewer (or there are none). A convo with the viewer alone is
 * not one anyone can reply to, so the caller shows nothing for it.
 */
int cobalt_chat_pick_counterpart(const char *const *member_dids, size_t count,
                                 const char *viewer_did);

/*
 * Number of other members. Anything above one is a group, which this inbox
 * labels "Group" rather than inventing a name for it.
 */
int cobalt_chat_other_member_count(const char *const *member_dids, size_t count,
                                   const char *viewer_did);

#define COBALT_CHAT_MESSAGES_MAX 50
/* A reply's text in bytes, with room for the terminator. The chat service allows
 * 1000 graphemes, which is never more bytes than this for ordinary text. */
#define COBALT_CHAT_REPLY_MAX 1024

typedef struct {
   char text[COBALT_POST_TEXT_MAX];
   char age[COBALT_RELATIVE_MAX];
   bool mine;                             /* sent by the signed-in account */
} cobalt_chat_message;

/*
 * One conversation's messages, oldest first, as the thread draws them. Only
 * the newest COBALT_CHAT_MESSAGES_MAX are held: a longer history would need
 * paging, which the read-only view does not have yet.
 */
typedef struct {
   char convo_id[COBALT_CHAT_ID_MAX];
   cobalt_chat_message items[COBALT_CHAT_MESSAGES_MAX];
   int count;
} cobalt_chat_thread;

void cobalt_chat_thread_reset(cobalt_chat_thread *thread);

/* A reply the chat service will take: some text that is not only spaces. */
bool cobalt_chat_reply_valid(const char *text);

/*
 * Starting a conversation with an account the inbox does not hold yet. The
 * target is a handle or a DID as typed. A handle has to be resolved to a DID
 * first, because getConvoForMembers takes DIDs only. Sized like the DID and
 * handle buffers, and well inside the job's URI field.
 */
#define COBALT_CHAT_TARGET_MAX 128

typedef enum {
   COBALT_CHAT_TARGET_INVALID = 0,
   COBALT_CHAT_TARGET_DID,
   COBALT_CHAT_TARGET_HANDLE,
} cobalt_chat_target_kind;

/*
 * Copy `input` into `out` with surrounding spaces, tabs and line breaks, and
 * one leading '@', removed. False when nothing is left, or the result would
 * not fit in `cap` bytes with its terminator.
 */
bool cobalt_chat_target_normalise(const char *input, char *out, size_t cap);

/*
 * Whether a normalised target is a DID (did:<method>:<id>) or a handle in the
 * atproto syntax: dot-separated labels, at least two, with a TLD that starts
 * with a letter. Anything else, including a handle with a space or an
 * underscore, is COBALT_CHAT_TARGET_INVALID.
 */
cobalt_chat_target_kind cobalt_chat_target_kind_of(const char *target);

/* True when both DIDs are set and name the same account. */
bool cobalt_chat_is_self(const char *viewer_did, const char *target_did);

/*
 * Copy a convo id out of a server response. The id has to be non-empty, use
 * only printable ASCII without spaces, and fit both `cap` and COBALT_CHAT_ID_MAX.
 */
bool cobalt_chat_convo_id_copy(char *out, size_t cap, const char *id);

#ifdef COBALT_HAS_WOLFRAM
struct wf_chat_convo_list;
struct wf_chat_message_list;

/* Replace the thread with a getMessages page. The server sends newest first, so
 * the page is reversed into place. Returns the number of messages held. */
int cobalt_chat_thread_fill_from_wolfram(cobalt_chat_thread *out,
                                         const struct wf_chat_message_list *list,
                                         const char *viewer_did, int64_t now);


/*
 * Append every convo in a parsed listConvos page that has a counterpart.
 * Returns the number added. A full inbox clears the cursor, as the
 * notifications list does, so a screen that auto-pages stops at the cap.
 */
int cobalt_chat_inbox_append_from_wolfram(cobalt_chat_inbox *out,
                                          const struct wf_chat_convo_list *list,
                                          const char *viewer_did);
#endif

#ifdef __cplusplus
}
#endif
