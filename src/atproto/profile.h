#pragma once

/*
 * An actor's profile, flattened for drawing.
 *
 * Same reasoning as feed.h and notifications.h: Wolfram hands back heap strings
 * it owns, and the render loop wants fixed buffers it can read every frame
 * without touching an allocator.
 *
 * The profile and the actor's posts are fetched together as one job, because
 * they are one screen. Two jobs would mean two spinners, and a worker that
 * takes one request at a time would serialise them anyway.
 */

#include "atproto/feed.h"
#include "cache/session_store.h"   /* COBALT_DID_MAX */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_PROFILE_BIO_MAX 512

typedef struct {
   char did[COBALT_DID_MAX];
   char handle[COBALT_POST_NAME_MAX];       /* with a leading @ */
   char display_name[COBALT_POST_NAME_MAX];
   char description[COBALT_PROFILE_BIO_MAX];

   /* Pre-formatted, because these are drawn as a single line and the numbers
    * are not otherwise needed. */
   char counts[COBALT_POST_META_MAX];       /* "120 followers · 84 following" */
   char post_count[32];                     /* "1,204 posts" */

   /*
    * The viewer's own follow record URI, empty when not following. This is
    * what an unfollow deletes, so it is the difference between the button
    * being reversible and not — exactly as with likes and reposts.
    */
   char viewer_following[COBALT_POST_URI_MAX];

   /* False until a fetch has landed, so the screen can tell "empty" from
    * "not asked yet". */
   bool loaded;

   /* True when this is the signed-in account, which has no follow button. */
   bool is_self;
} cobalt_profile;

void cobalt_profile_reset(cobalt_profile *profile);

/*
 * Format a follower/following line. Zero counts are shown here, unlike post
 * engagement counts — "0 followers" is information about an account, whereas
 * "0 likes" on a post is just noise.
 */
void cobalt_profile_format_counts(char *out, size_t out_size, int followers,
                                  int follows);

/*
 * Group a number with thin separators: 1204 becomes "1,204". Bluesky shows
 * abbreviated counts (1.2K) but those lose information at a glance and this is
 * a screen someone opened deliberately.
 */
void cobalt_profile_format_number(char *out, size_t out_size, int value,
                                  const char *singular, const char *plural);

/* Apply a follow or unfollow locally. `record_uri` is the created record, or
 * NULL when undoing. */
void cobalt_profile_apply_follow(cobalt_profile *profile, const char *record_uri);

#ifdef COBALT_HAS_WOLFRAM
struct wf_agent_profile;
void cobalt_profile_from_wolfram(cobalt_profile *out,
                                 const struct wf_agent_profile *src,
                                 const char *self_did);
#endif

#ifdef __cplusplus
}
#endif
