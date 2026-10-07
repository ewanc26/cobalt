#ifndef COBALT_ATPROTO_PREFS_H
#define COBALT_ATPROTO_PREFS_H

/*
 * The slice of the account's saved preferences Cobalt honours when it draws a
 * feed: muted words and "hide reposts" on the home timeline. The word list and
 * the matching rules are Wolfram's (wf_muted_list); this keeps the list beside
 * Cobalt's one own rule and applies both to a feed.
 */

#include "atproto/feed.h"

#include <stdbool.h>
#include <stdint.h>

#include <wolfram/muted_words.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   wf_muted_list muted;
   bool hide_reposts;       /* home timeline only */
} cobalt_prefs;

void cobalt_prefs_clear(cobalt_prefs *prefs);

/* Replace `prefs` with what the server returned. Expired mutes are skipped. */
void cobalt_prefs_from_wolfram(cobalt_prefs *prefs, const wf_actor_preferences *src,
                               int64_t now);

/*
 * Whether `text` (and `tags`, which may be NULL) contains a muted word, by
 * Wolfram's rules: case-insensitive, a single plain word matches whole words
 * only, a phrase or one with punctuation matches as a substring.
 */
bool cobalt_prefs_text_is_muted(const cobalt_prefs *prefs, const char *text,
                                const char *const *tags, int tag_count);

/*
 * Drop posts from `feed` starting at index `from` that the preferences hide.
 * `home` is true for the home timeline, which is the only feed hide_reposts
 * applies to. Returns how many were removed.
 */
int cobalt_prefs_filter_feed(const cobalt_prefs *prefs, cobalt_feed *feed,
                             int from, bool home);

#ifdef __cplusplus
}
#endif

#endif
