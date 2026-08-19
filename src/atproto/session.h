#pragma once

/*
 * PDS session: sign in, resume, sign out.
 *
 * This is AGENTS.md §12's step 2 — the networking spike everything downstream
 * is blocked on. It sits behind atproto/ (§8) and drives Wolfram rather than
 * speaking XRPC itself.
 *
 * Everything here is asynchronous, and that is not a stylistic choice. Wolfram's
 * transport is blocking libcurl: a sign-in against a cold PDS can take several
 * seconds including DNS, the TLS handshake and the server's own password
 * hashing. Doing that on the frame loop would stop the app pumping SDL events,
 * and since SDL owns ProcUI on this platform (§13), a stalled event pump is a
 * stalled ProcUI message queue — the console would sit on a frozen frame and
 * refuse to go back to the Wii U Menu. So calls are handed to a worker thread
 * and the UI polls for the result while continuing to draw.
 *
 * Usage from a screen:
 *
 *     if (cobalt_session_begin_login(service, id, password)) {
 *        // draw a "signing in" state; keep calling cobalt_session_poll()
 *     }
 *
 *     cobalt_job_result result;
 *     if (cobalt_session_poll(&result)) {
 *        // exactly one completion, once
 *     }
 */

#include "atproto/actors.h"
#include "atproto/feed.h"
#include "atproto/notifications.h"
#include "atproto/actor_profile.h"
#include "cache/session_store.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_IDENTIFIER_MAX 256
#define COBALT_PASSWORD_MAX   128
#define COBALT_MESSAGE_MAX    192
/* Bluesky's 300-grapheme limit, in bytes, plus a terminator. Kept here rather
 * than taken from app/compose.h so the protocol layer does not depend on a
 * screen. */
#define COBALT_COMPOSE_TEXT_MAX 3001

typedef enum {
   COBALT_AUTH_SIGNED_OUT = 0,
   COBALT_AUTH_WORKING,      /* a request is in flight */
   COBALT_AUTH_SIGNED_IN,
} cobalt_auth_state;

typedef enum {
   COBALT_JOB_NONE = 0,
   COBALT_JOB_LOGIN,
   COBALT_JOB_RESUME,
   COBALT_JOB_LOGOUT,
   COBALT_JOB_TIMELINE,
   COBALT_JOB_THREAD,
   COBALT_JOB_LIKE,
   COBALT_JOB_REPOST,
   COBALT_JOB_POST,
   COBALT_JOB_NOTIFICATIONS,
   COBALT_JOB_PROFILE,
   COBALT_JOB_FOLLOW,
   COBALT_JOB_MUTE,
   COBALT_JOB_BLOCK,
   COBALT_JOB_MUTED_LIST,
   COBALT_JOB_BLOCKED_LIST,
} cobalt_job_kind;

typedef struct {
   cobalt_job_kind kind;
   bool ok;
   /* Written for the user, not for a log: says what happened and what to do
    * about it. Empty on success. */
   char message[COBALT_MESSAGE_MAX];
} cobalt_job_result;

/*
 * Brings up the session layer: locates the bundled CA bundle and starts the
 * worker thread. Returns false if sign-in cannot work at all (no Wolfram, or
 * no trust store) — the app still runs, and cobalt_session_blocker() says why.
 */
bool cobalt_session_init(void);
void cobalt_session_shutdown(void);

/* --- capability reporting, for the diagnostics screen --- */

/* True when a sign-in could actually be attempted. */
bool cobalt_session_available(void);

/*
 * NULL when sign-in is possible, otherwise a short phrase naming the first
 * thing standing in the way ("no TLS trust store", "Wolfram not built in").
 */
const char *cobalt_session_blocker(void);

/* Path to the bundled CA bundle, or NULL if it was not found. */
const char *cobalt_session_ca_path(void);

/*
 * False if SDL could not create the worker thread, in which case requests run
 * synchronously on the caller and the app visibly stalls for their duration.
 * Worth surfacing rather than silently degrading, since it is the difference
 * between "slow" and "looks hung".
 */
bool cobalt_session_threaded(void);

/* --- state --- */

cobalt_auth_state cobalt_session_state(void);
bool cobalt_session_busy(void);

/* Empty strings rather than NULL when signed out, so callers can print them. */
const char *cobalt_session_handle(void);
const char *cobalt_session_did(void);
const char *cobalt_session_service(void);

/* True if credentials from a previous run are on the card. */
bool cobalt_session_has_saved(void);

/* --- requests --- */

/*
 * `service` may be empty, in which case the default PDS is used. It is
 * normalised (see cobalt_session_normalise_service) before use.
 * Returns false if a request is already in flight or sign-in is unavailable.
 */
bool cobalt_session_begin_login(const char *service, const char *identifier,
                                const char *password);

/* Resume the stored session, refreshing its tokens. */
bool cobalt_session_begin_resume(void);

/* Delete the session server-side and wipe the stored credentials. */
bool cobalt_session_begin_logout(void);

/*
 * Fetch the timeline. `paging` appends the next page using the cursor from the
 * last one; false replaces the feed with the top of it. Paging past the end is
 * not an error — it completes with nothing added.
 */
bool cobalt_session_begin_timeline(bool paging);

/*
 * The feed, thread, notifications and profile as last fetched. Never NULL.
 *
 * These are the worker's own buffers, not copies, so a caller MUST hold the
 * session lock across every read — including the whole of a draw, since the
 * worker rewrites them in place while a screen is still on screen (a like
 * updates a count; a refresh clears and refills the list). See
 * cobalt_session_lock().
 */
const cobalt_feed *cobalt_session_feed(void);

/*
 * Fetch the conversation around `uri`. Replaces whatever thread was loaded.
 */
bool cobalt_session_begin_thread(const char *uri);

/* The thread as last fetched; never NULL. */
const cobalt_thread *cobalt_session_thread(void);

/*
 * Toggle a like or repost on a post.
 *
 * The direction is decided from the post's current viewer state rather than
 * passed in, so a screen cannot get out of step with what the server thinks.
 * `uri` and `cid` identify the post; both are required to create a record.
 * The change is applied locally on success — see cobalt_feed_apply_like.
 */
bool cobalt_session_begin_like(const char *uri, const char *cid);
bool cobalt_session_begin_repost(const char *uri, const char *cid);

/*
 * Publish a post. `parent_uri`/`parent_cid` and `root_uri`/`root_cid` make it
 * a reply; pass NULL for all four for a top-level post. Both refs are required
 * for a reply — a reply naming the wrong root lands in the wrong conversation
 * for every other client, so a partial set is refused rather than guessed at.
 */
bool cobalt_session_begin_post(const char *text, const char *parent_uri,
                               const char *parent_cid, const char *root_uri,
                               const char *root_cid);

/*
 * Fetch notifications. `paging` appends the next page; false replaces the list.
 * A successful non-paging fetch also tells the server everything up to now has
 * been seen, which is what clears the unread badge everywhere else.
 */
bool cobalt_session_begin_notifications(bool paging);

/* Notifications as last fetched; never NULL. */
const cobalt_notifications *cobalt_session_notifications(void);

/*
 * Fetch an actor's profile and their posts together. `actor` is a handle or a
 * DID. One job rather than two: they are one screen, and a worker that takes
 * one request at a time would serialise them anyway.
 */
bool cobalt_session_begin_profile(const char *actor);

/* The profile and author feed as last fetched; never NULL. */
const cobalt_profile *cobalt_session_profile(void);
const cobalt_feed *cobalt_session_author_feed(void);

/*
 * Toggle following the loaded profile. Like the post interactions, the
 * direction comes from the profile's own viewer state rather than the caller's.
 */
bool cobalt_session_begin_follow(void);

/* Same shape, for mute and block. Neither is available on the viewer's own
 * profile, matching cobalt_session_begin_follow — the caller (app/profile.c)
 * already gates all three the same way. */
bool cobalt_session_begin_mute(void);
bool cobalt_session_begin_block(void);

/*
 * Muted and blocked accounts: fetch the list, and remove a row from it.
 *
 * Unlike begin_mute/begin_block above, these always undo — every row on
 * either list is, by definition, already muted or blocked, so there is no
 * toggle direction to read off a loaded profile. `did` is enough to unmute;
 * unblocking additionally needs the row's own block record URI, since
 * wf_agent_unblock takes a record rather than a subject.
 */
bool cobalt_session_begin_muted_list(bool paging);
bool cobalt_session_begin_blocked_list(bool paging);
bool cobalt_session_begin_unmute_actor(const char *did);
bool cobalt_session_begin_unblock_actor(const char *record_uri, const char *did);

const cobalt_actor_list *cobalt_session_muted_list(void);
const cobalt_actor_list *cobalt_session_blocked_list(void);

/*
 * Guard for the shared snapshots above.
 *
 * The worker mutates the feed, thread, notification list and profile in place
 * while the UI is drawing them — that is the whole point of applying an
 * interaction locally rather than refetching. Without this the UI can read a
 * counts line mid-rewrite, which is not a one-frame flicker: the text cache
 * keys on string contents, so a torn read gets stored under the wrong key and
 * renders wrongly until it is evicted.
 *
 * The lock is never held across network I/O, so a screen blocks on it for the
 * length of a memcpy at worst. It is reentrant (SDL mutexes are), so the
 * accessors that take it internally are safe to call while it is held.
 *
 * Bracket the whole of update-and-draw, not each accessor.
 */
void cobalt_session_lock(void);
void cobalt_session_unlock(void);

/*
 * True exactly once per completed request, handing back its outcome. Call it
 * every frame; it never blocks on the network.
 */
bool cobalt_session_poll(cobalt_job_result *out);

/* --- pure helpers (unit tested on the host, see tests/) --- */

/*
 * Turn what someone typed on a games console keyboard into a service URL:
 * trims surrounding whitespace, adds the https:// scheme when it is missing,
 * drops a trailing slash, and substitutes the default PDS for empty input.
 * Returns false (leaving `out` empty) if the result would not fit.
 */
bool cobalt_session_normalise_service(const char *input, char *out, size_t out_size);

/* The service used when the user does not name one. */
const char *cobalt_session_default_service(void);

#ifdef __cplusplus
}
#endif
