#pragma once

/*
 * Plain HTTPS GET into memory.
 *
 * This exists alongside the ATProto layer rather than inside it because
 * fetching an avatar is not protocol-shaped work: the PDS hands over an
 * ordinary URL and what comes back is a JPEG. AGENTS.md §8 says protocol logic
 * belongs in Wolfram; this is not that, and routing it through the SDK would
 * mean either exposing its transport or serialising image loads behind the one
 * request the session worker is allowed to have in flight.
 *
 * It does have to repeat two platform details Wolfram also handles, because
 * they are properties of this console rather than of the SDK:
 *
 *   - the bundled CA bundle, since there is no system trust store; and
 *   - the application's DRBG for the handshake, since devkitPro's mbedTLS
 *     seeds itself from the tick counter (see util/rng.h).
 *
 * Getting either wrong here would mean image loads are the one unprotected
 * path in an app that is careful everywhere else.
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
 * this does not assume it. Safe to call more than once.
 */
bool cobalt_http_init(const char *ca_path);
void cobalt_http_shutdown(void);

/*
 * Fetch `url` into memory, refusing anything over `max_bytes`.
 *
 * The cap is not a nicety. The URL comes from a PDS response, so a hostile or
 * compromised one could point at an endless stream; without a ceiling the
 * console would allocate until it died. The transfer is aborted as soon as the
 * limit is crossed rather than after the fact.
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
