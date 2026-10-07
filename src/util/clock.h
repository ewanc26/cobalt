#pragma once

/*
 * The console's clock, and the size of a post's relative age ("3h"). Parsing,
 * formatting and the relative text are Wolfram's (wolfram/time.h); the only
 * part that is Cobalt's is reading the clock.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enough for "just now" plus a terminator, and for a fallback date. */
#define COBALT_RELATIVE_MAX 16

/* Seconds since the Unix epoch, or 0 when the clock is unavailable. Callers treat 0 as
 * "no idea", which makes every post read as "now" rather than as 1970. */
int64_t cobalt_time_now(void);

#ifdef __cplusplus
}
#endif
