#pragma once

/*
 * Timestamp parsing and relative formatting.
 *
 * ATProto timestamps are RFC 3339 strings and every post carries one, so this
 * runs once per visible post per fetch. It is deliberately free of libc's
 * timezone machinery: `timegm` is not portable to devkitPPC, and `mktime`
 * would drag in local-time handling that depends on a console clock whose
 * timezone Cobalt has no reason to trust. The conversion is done arithmetically
 * instead, which also makes the whole module testable on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enough for "just now" plus a terminator, and for a fallback date. */
#define COBALT_RELATIVE_MAX 16

/*
 * Parse an RFC 3339 UTC timestamp — "2026-07-29T10:15:30Z", with optional
 * fractional seconds ("...30.123Z"), which the PDS does emit.
 *
 * Only the UTC form is accepted. ATProto requires it, and silently
 * misinterpreting a numeric offset as UTC would put posts hours out of order,
 * which is worse than showing nothing. Returns false on anything else.
 */
bool cobalt_time_parse_rfc3339(const char *text, int64_t *out_epoch);

/*
 * Compact "how long ago", in the shape a feed wants: "now", "42s", "9m",
 * "3h", "6d", "12w", "2y". A timestamp in the future clamps to "now" rather
 * than producing a negative age — a console with a wrong clock should look
 * slightly odd, not render every post as "-4h".
 */
void cobalt_time_relative(int64_t epoch, int64_t now, char *out, size_t out_size);

/* Seconds since the Unix epoch, or 0 if the platform clock is unusable. */
int64_t cobalt_time_now(void);

#ifdef __cplusplus
}
#endif
