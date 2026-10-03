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
   long status;
} cobalt_http_response;

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
