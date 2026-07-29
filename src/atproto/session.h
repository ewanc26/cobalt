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

#include "cache/session_store.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_IDENTIFIER_MAX 256
#define COBALT_PASSWORD_MAX   128
#define COBALT_MESSAGE_MAX    192

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
