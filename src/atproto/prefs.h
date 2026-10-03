#ifndef COBALT_ATPROTO_PREFS_H
#define COBALT_ATPROTO_PREFS_H

/*
 * The slice of the account's saved preferences Cobalt honours when it draws a
 * feed: muted words and "hide reposts" on the home timeline. Kept apart from
 * the network code so the matching rules can be tested on the host.
 */

#include "atproto/feed.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_PREFS_WORDS_MAX 48
#define COBALT_PREFS_WORD_MAX 64

typedef struct {
   char value[COBALT_PREFS_WORD_MAX];
   bool content;            /* applies to post text */
   bool tag;                /* applies to hashtags */
} cobalt_muted_word;

typedef struct {
   cobalt_muted_word words[COBALT_PREFS_WORDS_MAX];
   int count;
   bool hide_reposts;       /* home timeline only */
} cobalt_prefs;

void cobalt_prefs_clear(cobalt_prefs *prefs);

/*
 * Add a word. Empty values and a full list are ignored. A word with neither
 * target is treated as content-only, which is what the server means by the
 * default.
 */
bool cobalt_prefs_add_word(cobalt_prefs *prefs, const char *value, bool content,
                           bool tag);

/*
 * Whether `text` (and `tags`, which may be NULL) contains a muted word. Matching
 * is case-insensitive. A single alphanumeric word matches whole words only, so
 * muting "cat" does not hide "category"; a phrase, or a word with punctuation in
 * it, matches as a substring.
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

#ifdef COBALT_HAS_WOLFRAM
struct wf_actor_preferences;
/* Replace `prefs` with what the server returned. Expired mutes are skipped. */
void cobalt_prefs_from_wolfram(cobalt_prefs *prefs,
                               const struct wf_actor_preferences *src,
                               int64_t now);
#endif

#ifdef __cplusplus
}
#endif

#endif
