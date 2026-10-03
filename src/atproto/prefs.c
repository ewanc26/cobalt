#include "atproto/prefs.h"
#include "util/log.h"
#include "util/timefmt.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/actor_prefs_typed.h>
#endif

#include <stdio.h>
#include <string.h>

/* Bytes of a multi-byte UTF-8 sequence count as letters, so a word in a script
 * without spaces or ASCII punctuation is not split in the middle. */
static bool
is_word_byte(unsigned char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c >= 0x80;
}

static char
lower(char c)
{
   return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

static bool
ci_equal(const char *a, const char *b)
{
   for (; *a && *b; a++, b++) {
      if (lower(*a) != lower(*b)) {
         return false;
      }
   }
   return *a == *b;
}

static bool
word_is_plain(const char *w)
{
   for (; *w; w++) {
      if (!is_word_byte((unsigned char) *w)) {
         return false;
      }
   }
   return true;
}

/* Case-insensitive search; whole_word requires non-word bytes (or the string
 * edge) on both sides of the match. */
static bool
contains(const char *hay, const char *needle, bool whole_word)
{
   const size_t n = strlen(needle);
   if (n == 0) {
      return false;
   }
   for (const char *p = hay; *p; p++) {
      size_t i = 0;
      while (i < n && p[i] && lower(p[i]) == lower(needle[i])) {
         i++;
      }
      if (i != n) {
         continue;
      }
      if (whole_word) {
         if (p != hay && is_word_byte((unsigned char) p[-1])) {
            continue;
         }
         if (is_word_byte((unsigned char) p[n])) {
            continue;
         }
      }
      return true;
   }
   return false;
}

void
cobalt_prefs_clear(cobalt_prefs *prefs)
{
   if (prefs) {
      memset(prefs, 0, sizeof(*prefs));
   }
}

bool
cobalt_prefs_add_word(cobalt_prefs *prefs, const char *value, bool content,
                      bool tag)
{
   if (!prefs || !value || !value[0] || prefs->count >= COBALT_PREFS_WORDS_MAX) {
      return false;
   }
   cobalt_muted_word *w = &prefs->words[prefs->count++];
   snprintf(w->value, sizeof(w->value), "%s", value);
   w->content = content || !tag;
   w->tag = tag;
   return true;
}

bool
cobalt_prefs_text_is_muted(const cobalt_prefs *prefs, const char *text,
                           const char *const *tags, int tag_count)
{
   if (!prefs) {
      return false;
   }
   for (int i = 0; i < prefs->count; i++) {
      const cobalt_muted_word *w = &prefs->words[i];
      if (w->content && text && contains(text, w->value, word_is_plain(w->value))) {
         return true;
      }
      if (w->tag && tags) {
         /* A tag mute may be written with or without the leading #. */
         const char *v = w->value[0] == '#' ? w->value + 1 : w->value;
         for (int t = 0; t < tag_count; t++) {
            if (tags[t] && ci_equal(tags[t], v)) {
               return true;
            }
         }
      }
   }
   return false;
}

static bool
post_is_hidden(const cobalt_prefs *prefs, const cobalt_post *post, bool home)
{
   if (home && prefs->hide_reposts && post->reposted_by[0]) {
      return true;
   }
   if (prefs->count == 0) {
      return false;
   }
   const char *tags[COBALT_POST_FACETS_MAX];
   int tag_count = 0;
   for (int i = 0; i < post->facet_count && i < COBALT_POST_FACETS_MAX; i++) {
      if (post->facets[i].kind == COBALT_FACET_TAG) {
         tags[tag_count++] = post->facets[i].target;
      }
   }
   return cobalt_prefs_text_is_muted(prefs, post->text, tags, tag_count);
}

int
cobalt_prefs_filter_feed(const cobalt_prefs *prefs, cobalt_feed *feed, int from,
                         bool home)
{
   if (!prefs || !feed || from < 0 || from >= feed->count) {
      return 0;
   }
   int out = from;
   for (int i = from; i < feed->count; i++) {
      if (post_is_hidden(prefs, &feed->posts[i], home)) {
         continue;
      }
      if (out != i) {
         feed->posts[out] = feed->posts[i];
      }
      out++;
   }
   const int removed = feed->count - out;
   feed->count = out;
   return removed;
}

#ifdef COBALT_HAS_WOLFRAM

void
cobalt_prefs_from_wolfram(cobalt_prefs *prefs, const struct wf_actor_preferences *src,
                          int64_t now)
{
   if (!prefs) {
      return;
   }
   cobalt_prefs_clear(prefs);
   if (!src) {
      return;
   }

   for (size_t i = 0; i < src->feed_view_count; i++) {
      const wf_actor_pref_feed_view *fv = &src->feed_views[i];
      if (fv->feed && strcmp(fv->feed, "home") == 0 && fv->has_hide_reposts) {
         prefs->hide_reposts = fv->hide_reposts;
      }
   }

   for (size_t i = 0; i < src->muting_keyword_count; i++) {
      const wf_actor_pref_muted_word *mw = &src->muting_keywords[i];
      if (!mw->value) {
         continue;
      }
      int64_t expires = 0;
      if (mw->expires_at && cobalt_time_parse_rfc3339(mw->expires_at, &expires) &&
          now > 0 && expires <= now) {
         continue;
      }
      bool content = false;
      bool tag = false;
      for (size_t t = 0; t < mw->target_count; t++) {
         if (!mw->targets[t]) {
            continue;
         }
         content = content || strcmp(mw->targets[t], "content") == 0;
         tag = tag || strcmp(mw->targets[t], "tag") == 0;
      }
      cobalt_prefs_add_word(prefs, mw->value, content, tag);
   }
   COBALT_LOGI("prefs: %d muted word(s), hide reposts %s", prefs->count,
               prefs->hide_reposts ? "on" : "off");
}

#endif
