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
 * The inbox and the request inbox's list live here, with accepting and
 * declining a request. A group's pending join requests are here too: the same
 * flattening, for the same reason, but only a group's owner can list them, so
 * a screen reaches them from a conversation rather than the inbox. Groups
 * themselves — creating one, editing it — are still later steps (cobalt#107).
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
 * The request inbox's badge text: "No requests" for 0 (and below), "1 request"
 * for 1, "N requests" otherwise. False when `out` is NULL, `cap` is 0, or the
 * text does not fit in `cap` bytes with its terminator.
 */
bool cobalt_chat_requests_count_label(int count, char *out, size_t cap);

/* The row for `convo_id`, borrowed from `inbox`, or NULL when there is none. */
const cobalt_chat_convo *cobalt_chat_inbox_find(const cobalt_chat_inbox *inbox,
                                                const char *convo_id);

/*
 * Drop the row for `convo_id`, keeping the order of the rest. Its unread count
 * leaves the inbox total. False when no row has that id, or `inbox` is NULL.
 */
bool cobalt_chat_inbox_remove(cobalt_chat_inbox *inbox, const char *convo_id);

/*
 * Put `item` at the front of `inbox`, dropping the last row when it is full.
 * False when `inbox` or `item` is NULL, or a row with the same id is already
 * held, so one conversation never appears twice.
 */
bool cobalt_chat_inbox_add_front(cobalt_chat_inbox *inbox, const cobalt_chat_convo *item);

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

/* --- a group's pending join requests --- */

/*
 * Who asked to join a group, flattened for drawing. Only the group's owner can
 * list these, so a screen opens them from the group's own conversation. The DID
 * is the handle the approve and reject calls take, so a row keeps it whether or
 * not the name could be read.
 */
#define COBALT_CHAT_JOIN_REQUESTS_MAX 50

typedef struct {
   char did[COBALT_CHAT_TARGET_MAX];   /* who asked, for the decision */
   char name[COBALT_POST_NAME_MAX];    /* display name, or the handle */
   char handle[COBALT_POST_NAME_MAX];  /* with a leading '@' */
   char age[COBALT_RELATIVE_MAX];      /* "3h" since they asked */
} cobalt_chat_join_request;

typedef struct {
   char convo_id[COBALT_CHAT_ID_MAX];  /* the group these belong to */
   cobalt_chat_join_request items[COBALT_CHAT_JOIN_REQUESTS_MAX];
   int count;
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
} cobalt_chat_join_requests;

void cobalt_chat_join_requests_reset(cobalt_chat_join_requests *list);

/* As cobalt_feed_can_page, for the same reason: a full window with a cursor
 * left must not be paged forever. */
bool cobalt_chat_join_requests_can_page(const cobalt_chat_join_requests *list);

/* The row for `did`, borrowed, or NULL when there is none. */
const cobalt_chat_join_request *cobalt_chat_join_requests_find(
   const cobalt_chat_join_requests *list, const char *did);

/*
 * Drop the request from `did`, keeping the order of the rest. False when no row
 * has that DID, or `list` is NULL. Called once a decision has been accepted, so
 * the row leaves without a refetch.
 */
bool cobalt_chat_join_requests_remove(cobalt_chat_join_requests *list, const char *did);

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

struct wf_chat_join_requests;

/*
 * Append every request in a parsed listJoinRequests page. Returns the number
 * added. A request whose DID is already held is skipped, so paging never
 * doubles a row; a full list clears the cursor, as the other lists do.
 */
int cobalt_chat_join_requests_append_from_wolfram(cobalt_chat_join_requests *out,
                                                  const struct wf_chat_join_requests *list,
                                                  int64_t now);
#endif

#ifdef __cplusplus
}
#endif
