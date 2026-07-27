#pragma once

/*
 * AT Protocol layer.
 *
 * AGENTS.md §8: this wraps Wolfram (Ewan's C ATProto SDK, built as a sibling
 * checkout) rather than growing a second ATProto implementation inside Cobalt.
 * Everything protocol-shaped — session handling, lexicon types, XRPC — belongs
 * behind this interface so the UI never talks to Wolfram directly and Wolfram
 * can be extended in place rather than forked.
 *
 * Wolfram is optional at build time. Without it (COBALT_HAS_WOLFRAM undefined)
 * these calls still link and report an unavailable status, so the app builds
 * and boots from a bare checkout.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_ATPROTO_ABSENT = 0,   /* built without Wolfram */
   COBALT_ATPROTO_READY,        /* SDK up and usable */
   COBALT_ATPROTO_NO_ENTROPY,   /* up, but signing/keygen fails closed */
   COBALT_ATPROTO_ERROR,        /* SDK present but failed to initialise */
} cobalt_atproto_status;

bool cobalt_atproto_init(void);
void cobalt_atproto_shutdown(void);

cobalt_atproto_status cobalt_atproto_get_status(void);

/* Short human-readable summary for the diagnostics screen. */
const char *cobalt_atproto_status_string(void);

/* Wolfram's version, or "not built in". */
const char *cobalt_atproto_sdk_version(void);

/*
 * Session (app-password sign-in, AGENTS.md §7).
 *
 * NOT VERIFIED ON HARDWARE OR AGAINST A REAL WOLFRAM CHECKOUT — see the
 * "UNVERIFIED WOLFRAM SURFACE" notice in atproto.c. The shape of this API
 * (blocking calls, plain result codes, no async/callback machinery) matches
 * what a single-threaded frame loop can use safely: cobalt_app_update() defers
 * the actual call by one frame so a "Signing in..." screen gets drawn first,
 * per AGENTS.md §9's no-hang requirement as far as the UI thread goes — the
 * call itself still blocks on the network for its duration.
 */
typedef enum {
   COBALT_LOGIN_OK = 0,
   COBALT_LOGIN_BAD_CREDENTIALS,
   COBALT_LOGIN_NETWORK_ERROR,
   COBALT_LOGIN_UNAVAILABLE,   /* no Wolfram/XRPC support built in */
} cobalt_login_result;

/* Blocks until the PDS responds or the request fails. Persists the resulting
 * session to SD on success (see the plaintext-storage caveat in atproto.c). */
cobalt_login_result cobalt_atproto_login(const char *identifier, const char *app_password);

/* Drops the in-memory session and deletes the persisted copy from SD. */
void cobalt_atproto_logout(void);

bool cobalt_atproto_has_session(void);

/* NULL when there is no active session. */
const char *cobalt_atproto_session_handle(void);

/*
 * Timeline (app.bsky.feed.getTimeline).
 *
 * Also unverified against Wolfram's real API — see atproto.c.
 */
typedef struct {
   char author_handle[128];
   char author_display_name[256];
   char text[1024];
   char created_at[40]; /* ISO 8601, as returned by the PDS */
} cobalt_feed_post;

#define COBALT_TIMELINE_MAX_POSTS 25

typedef enum {
   COBALT_TIMELINE_OK = 0,
   COBALT_TIMELINE_NOT_SIGNED_IN,
   COBALT_TIMELINE_NETWORK_ERROR,
   COBALT_TIMELINE_UNAVAILABLE,
} cobalt_timeline_result;

/* Blocks until the PDS responds or the request fails. `out_posts` must have
 * room for at least `max_posts` entries (max COBALT_TIMELINE_MAX_POSTS). */
cobalt_timeline_result cobalt_atproto_fetch_timeline(cobalt_feed_post *out_posts,
                                                      int max_posts, int *out_count);

#ifdef __cplusplus
}
#endif
