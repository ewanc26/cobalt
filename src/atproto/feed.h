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

/*
 * Long enough for the CDN URLs Bluesky serves avatars from, which run to about
 * 130 bytes, with room for a self-hosted PDS that is less terse. Sized to match
 * COBALT_IMAGECACHE_URL_MAX: truncating here would produce a URL that fetches
 * the wrong image or nothing at all, and either is worse than the placeholder.
 */
#define COBALT_POST_AVATAR_MAX 256

typedef struct {
   char author[COBALT_POST_NAME_MAX];   /* display name, or the handle */
   char handle[COBALT_POST_NAME_MAX];   /* always the handle, with a leading @ */

   /*
    * Where the author's avatar lives, empty when they have not set one. Only a
    * URL: fetching and decoding happen in ui/imagecache.c, off the frame loop,
    * and the card draws a lettered placeholder until one arrives.
    */
   char avatar[COBALT_POST_AVATAR_MAX];

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

   /*
    * Raw counts as well as the formatted line, because an interaction updates
    * them locally before the server has been asked again — reformatting needs
    * the numbers back.
    */
   int reply_count;
   int repost_count;
   int like_count;

   /*
    * The viewer's own like/repost record URIs, empty when they have not done
    * either. These are what an "unlike" deletes, so they are the difference
    * between an interaction being reversible and not.
    */
   char viewer_like[COBALT_POST_URI_MAX];
   char viewer_repost[COBALT_POST_URI_MAX];

   /* Kept for interactions and thread view, not drawn. */
   char uri[COBALT_POST_URI_MAX];
   char cid[COBALT_POST_CID_MAX];

   /*
    * The conversation this post belongs to. A reply record must name both its
    * parent and the thread root; getting the root wrong produces a reply that
    * every other client renders in the wrong place. When the post is itself a
    * root these mirror uri/cid, so a caller never has to special-case it.
    */
   char root_uri[COBALT_POST_URI_MAX];
   char root_cid[COBALT_POST_CID_MAX];
} cobalt_post;

typedef struct {
   cobalt_post posts[COBALT_FEED_MAX_POSTS];
   int count;

   /* Paging cursor from the last response. Empty when the server indicated
    * there is nothing further. */
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
} cobalt_feed;

/*
 * A post and its surrounding conversation.
 *
 * Flattened from Wolfram's recursive node tree into a list with an indent
 * level per row, because that is what a scrolling list can draw and what the
 * D-pad can move through. The tree shape is preserved only as far as the
 * indent conveys it — beyond a few levels the GamePad runs out of width, so
 * deeper replies are pinned at the maximum indent rather than disappearing.
 */
#define COBALT_THREAD_MAX_POSTS 40
#define COBALT_THREAD_MAX_DEPTH 4

typedef struct {
   cobalt_post posts[COBALT_THREAD_MAX_POSTS];
   unsigned char depth[COBALT_THREAD_MAX_POSTS];
   int count;

   /* Index of the post the thread was opened on, so the view can start there
    * rather than at the top of a long ancestor chain. */
   int focus;

   /* Set when the conversation did not fit. Shown to the user: a thread that
    * silently stops looks like the end of the conversation. */
   bool truncated;
} cobalt_thread;

/* Drop every post. Does not touch the cursor. */
void cobalt_feed_reset(cobalt_feed *feed);

void cobalt_thread_reset(cobalt_thread *thread);

/*
 * Bring a list cursor back into range after the list may have shrunk.
 *
 * Every list screen needs this and none of them can derive it from the scroll
 * maths, which only ever raises `scroll` to meet `selected` and so cannot
 * recover from a cursor past the end — the symptom is a screen that draws
 * nothing and takes one press of UP per row to escape. Shared rather than
 * repeated four times, and pure so it can be tested.
 *
 * `selected` lands on the last row, or -1 for an empty list, which callers
 * already guard before indexing.
 */
void cobalt_list_clamp(int *selected, int *scroll, int count);

/*
 * Whether a screen should ask for another page.
 *
 * Not simply `has_more`: the window is fixed, so once it is full every further
 * page appends nothing while the server still returns a cursor. A screen that
 * auto-pages on reaching the last row would then request forever, holding the
 * worker busy so no interaction ever ran.
 */
bool cobalt_feed_can_page(const cobalt_feed *feed);

/*
 * Apply a like or repost locally, without refetching.
 *
 * Interactions are reflected immediately and reconciled by the next refresh,
 * which is what every other client does and what makes a button on a console
 * feel connected to anything. `record_uri` is the created record's URI, or
 * NULL when undoing. Returns false if the post is no longer in the feed —
 * a refresh can replace it while the request is in flight.
 */
bool cobalt_feed_apply_like(cobalt_feed *feed, const char *post_uri,
                            const char *record_uri);
bool cobalt_feed_apply_repost(cobalt_feed *feed, const char *post_uri,
                              const char *record_uri);

/* The same, for a loaded thread. A post is frequently on screen in both the
 * feed and a thread at once, and both copies have to move together. */
bool cobalt_thread_apply_like(cobalt_thread *thread, const char *post_uri,
                              const char *record_uri);
bool cobalt_thread_apply_repost(cobalt_thread *thread, const char *post_uri,
                                const char *record_uri);

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

/* Flatten a getPostThread tree. Ancestors first, then the requested post
 * (recorded in `focus`), then replies depth-first. */
struct wf_agent_thread;
void cobalt_thread_from_wolfram(cobalt_thread *out,
                                const struct wf_agent_thread *src, int64_t now);
#endif

#ifdef __cplusplus
}
#endif
