#pragma once

/*
 * Notifications, flattened for drawing.
 *
 * Same reasoning as the feed (see feed.h): Wolfram keeps each notification's
 * record as an owned cJSON subtree, which is right for an SDK and wrong for a
 * render loop. A fetch flattens once into fixed-size structs with the wording
 * already resolved.
 *
 * What makes this list different from the feed is that its rows are not all
 * about a post. A follow has no subject to open, and a like refers to
 * something the *viewer* wrote rather than anything in the notification. So a
 * row carries the URI it should open, resolved at parse time, rather than the
 * screen having to know which reason means which field.
 */

#include "atproto/feed.h"
#include "util/clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_NOTIFICATIONS_MAX 50
#define COBALT_NOTIFICATION_SUMMARY_MAX 64

typedef struct {
   char actor[COBALT_POST_NAME_MAX];    /* display name, or the handle */
   char handle[COBALT_POST_NAME_MAX];   /* with a leading @ */
   char avatar[COBALT_POST_AVATAR_MAX]; /* URL, empty when unset */
   char summary[COBALT_NOTIFICATION_SUMMARY_MAX];  /* "liked your post" */
   /* The lexicon reason, kept so adjacent rows that describe the same event
    * can be merged without re-parsing the wording. */
   char reason[32];

   /* The reply/mention text, when the notification is one. Empty otherwise —
    * a like carries no words of its own. */
   char text[COBALT_POST_TEXT_MAX];
   char age[COBALT_RELATIVE_MAX];

   /* What opening this row should show, or empty when there is nothing to
    * open (a follow). */
   char subject_uri[COBALT_POST_URI_MAX];

   /* The account behind the notification, so a follow can open their profile. */
   char actor_did[96];

   bool unread;

   /* 1, or how many notifications this row stands for once grouped. */
   int group_count;
   /* Epoch of the notification's timestamp; 0 when the server sent none. */
   int64_t posted_at;
} cobalt_notification;

typedef struct {
   cobalt_notification items[COBALT_NOTIFICATIONS_MAX];
   int count;
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
   int unread;
} cobalt_notifications;

void cobalt_notifications_reset(cobalt_notifications *list);

/* As cobalt_feed_can_page, for the same reason. */
bool cobalt_notifications_can_page(const cobalt_notifications *list);

/*
 * Turn a lexicon reason into something readable. Returns the reason verbatim
 * for anything unrecognised rather than inventing wording — a new notification
 * kind should read oddly, not wrongly.
 */
const char *cobalt_notification_summary(const char *reason);

/*
 * True when a reason's subject is the notification's own record (a reply,
 * mention or quote — the thing to open is the post that caused it) rather than
 * `reasonSubject` (a like or repost — the thing to open is what the viewer
 * wrote). Exposed because getting this backwards opens the wrong post, which
 * is the sort of thing worth pinning down in a test.
 */
bool cobalt_notification_subject_is_self(const char *reason);

/*
 * Merge adjacent like/repost notifications about the same post posted within
 * 48 hours of each other, as the official Bluesky client does: five people
 * liking one post is one row, not five. Only adjacent rows merge, so the list
 * stays in the order the server sent.
 *
 * `unread` keeps counting the notifications themselves, not the merged rows,
 * so the "N new" badge does not shrink when rows are combined.
 */
void cobalt_notifications_group(cobalt_notifications *list);

#ifdef COBALT_HAS_WOLFRAM
struct wf_agent_notification_list;
int cobalt_notifications_append_from_wolfram(
   cobalt_notifications *out, const struct wf_agent_notification_list *list,
   int64_t now);
#endif

#ifdef __cplusplus
}
#endif
