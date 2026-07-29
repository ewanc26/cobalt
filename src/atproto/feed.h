#pragma once

/*
 * The timeline, in a shape the UI can draw.
 *
 * Wolfram hands back `wf_agent_feed_list`, which keeps `record`, `embed` and
 * `reason` as owned cJSON subtrees so it stays bounded whatever a PDS sends.
 * That is the right choice for an SDK and the wrong one for a render loop: the
 * UI would be walking JSON on every frame, and AGENTS.md §9 asks for no
 * allocation on that path at all.
 *
 * So a fetch flattens the list once, into fixed-size structs with everything
 * already formatted — relative timestamp, counts, repost attribution. Drawing a
 * frame then touches nothing but plain char arrays. Fixed sizes rather than
 * heap strings for the same reason: one allocation for the whole feed, and no
 * way for a hostile display name to make the client allocate unboundedly.
 *
 * Text that does not fit is truncated on a UTF-8 boundary. Post content is
 * arbitrary user input in any script (§5), so a truncation that split a
 * multi-byte sequence would render as tofu.
 */

#include "util/timefmt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One screen shows three posts; this holds enough for a long scroll without a
 * refetch, at roughly 50 KB for the whole feed. The Wii U has room for far
 * more, but a bigger window mostly buys memory pressure during rendering.
 */
#define COBALT_FEED_MAX_POSTS 60

#define COBALT_POST_TEXT_MAX   512
#define COBALT_POST_NAME_MAX    96
#define COBALT_POST_URI_MAX    192
#define COBALT_POST_CID_MAX     96
#define COBALT_POST_META_MAX    96
#define COBALT_CURSOR_MAX      256

typedef struct {
   char author[COBALT_POST_NAME_MAX];   /* display name, or the handle */
   char handle[COBALT_POST_NAME_MAX];   /* always the handle, with a leading @ */
   char text[COBALT_POST_TEXT_MAX];
   char age[COBALT_RELATIVE_MAX];       /* "3h" */
   char meta[COBALT_POST_META_MAX];     /* "12 replies · 30 reposts · 88 likes" */

   /* Empty unless the post is in the feed because someone reposted it. */
   char reposted_by[COBALT_POST_NAME_MAX];

   /*
    * A short note standing in for content Cobalt cannot draw yet — "[image]",
    * "[video]", "[link]", "[quote]". Showing a marker is a deliberate choice
    * over dropping the embed silently: a post that is only an image would
    * otherwise render as a blank card with no explanation.
    */
   char embed_note[32];

   /* Kept for interactions and thread view, not drawn. */
   char uri[COBALT_POST_URI_MAX];
   char cid[COBALT_POST_CID_MAX];
} cobalt_post;

typedef struct {
   cobalt_post posts[COBALT_FEED_MAX_POSTS];
   int count;

   /* Paging cursor from the last response. Empty when the server indicated
    * there is nothing further. */
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
} cobalt_feed;

/* Drop every post. Does not touch the cursor. */
void cobalt_feed_reset(cobalt_feed *feed);

/*
 * Copy `text` into `out`, truncating on a UTF-8 boundary and appending an
 * ellipsis if it did not fit. Exposed for testing — the truncation rule is the
 * part worth pinning down, since it runs over arbitrary post content.
 */
void cobalt_feed_copy_text(char *out, size_t out_size, const char *text);

/*
 * Build the counts line. Zero counts are omitted rather than shown as "0
 * replies", which is how every other client reads and keeps the line short on
 * a GamePad. Produces an empty string when a post has no engagement at all.
 */
void cobalt_feed_format_counts(char *out, size_t out_size, int replies,
                               int reposts, int likes);

/*
 * Map an embed's `$type` to the short note above. Returns an empty string for
 * an unrecognised type rather than guessing, so a new embed kind shows as
 * nothing rather than as the wrong thing.
 */
const char *cobalt_feed_embed_note(const char *type);

#ifdef COBALT_HAS_WOLFRAM
/*
 * Flatten a Wolfram feed list onto the end of `feed`, stopping at
 * COBALT_FEED_MAX_POSTS. Returns how many posts were actually appended, which
 * is how the caller learns the window is full.
 *
 * Forward-declared rather than including <wolfram/feed_typed.h>, so this header
 * stays usable from a build without the SDK — and so the UI, which includes it
 * to draw, never pulls in the SDK's headers at all.
 */
struct wf_agent_feed_list;
int cobalt_feed_append_from_wolfram(cobalt_feed *feed,
                                    const struct wf_agent_feed_list *list,
                                    int64_t now);
#endif

#ifdef __cplusplus
}
#endif
