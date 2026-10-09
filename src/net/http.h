#pragma once

/*
 * Plain HTTPS GET into memory, for avatars and other PDS-supplied image URLs.
 *
 * This is a thin layer over Wolfram's generic GET (wf_http_get_limited) and
 * contains no transport code: https-only enforcement, the redirect cap, the
 * timeout, the user agent, the response size ceiling, the CA bundle and the
 * handshake RNG are all Wolfram client settings, applied once in
 * cobalt_http_init(). What lives here is only what is Cobalt's own: the values
 * for those settings, and a short-lived body cache so the TV and GamePad image
 * caches share one download (see http.c).
 *
 * It uses a Wolfram client of its own with no bearer token, so the signed-in
 * account's credentials can never be sent to an image host.
 *
 * Needs a Wolfram build (COBALT_HAS_WOLFRAM); without one every fetch fails,
 * and image loads degrade to placeholders.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   unsigned char *data;   /* heap, NUL-terminated for convenience */
   size_t size;
   long status;           /* HTTP status, 0 when the request never completed */
   int error;             /* Wolfram wf_status on a failed fetch, else 0 */
   char error_text[96];   /* short reason when the fetch failed, else "" */
} cobalt_http_response;

/* How the fetch client is wired up; see cobalt_http_reason() for the prose.
 * The diagnostics screen prints the reason, and the updater appends it to a
 * failed-"Could not reach GitHub" message instead of leaving every failure
 * looking like the network.
 */
typedef enum {
   COBALT_HTTP_STATE_UNINITIALISED = 0, /* init not attempted or refused */
   COBALT_HTTP_STATE_READY,             /* client + trust store + TLS RNG */
   COBALT_HTTP_STATE_NO_WOLFRAM,        /* built without Wolfram: nothing fetches */
   COBALT_HTTP_STATE_NO_CA,             /* running, but no trust store: verification fails */
   COBALT_HTTP_STATE_NO_TLS_RNG         /* init refused: the handshake would be tick-seeded */
} cobalt_http_state;

/* The fetch client's current state, for the diagnostics screen. */
cobalt_http_state cobalt_http_get_state(void);

/* A short reason for the current state ("not initialised", "no TLS trust
 * store - run `make cacert`", ...). Never NULL; used by the diagnostics
 * screen and as the fallback when a fetch has no error text of its own. */
const char *cobalt_http_reason(void);

/*
 * `ca_path` may be NULL, in which case requests will fail verification on this
 * platform — the caller is expected to have refused to come up already, but
 * this does not assume it. Returns false if the TLS RNG hook cannot be
 * installed. Safe to call more than once.
 */
bool cobalt_http_init(const char *ca_path);
void cobalt_http_shutdown(void);

/*
 * Fetch `url` into memory, refusing anything over `max_bytes`.
 *
 * The cap is not a nicety. The URL comes from a PDS response, so a hostile or
 * compromised one could point at an endless stream; without a ceiling the
 * console would allocate until it died. Wolfram aborts the transfer as soon as
 * the limit is crossed rather than after the fact.
 *
 * Blocking. Call from a worker thread, never from the frame loop.
 * On true, free with cobalt_http_response_free.
 */
bool cobalt_http_get(const char *url, size_t max_bytes,
                     cobalt_http_response *out);

void cobalt_http_response_free(cobalt_http_response *response);

#ifdef __cplusplus
}
#endif
