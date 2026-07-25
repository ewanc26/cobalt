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

#ifdef __cplusplus
}
#endif
