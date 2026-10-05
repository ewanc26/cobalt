#include "atproto/prefs.h"
#include "util/log.h"
#include "util/timefmt.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/actor_prefs_typed.h>
#include <wolfram/moderation.h>
#endif

#include <stdio.h>
#include <string.h>

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
#ifdef COBALT_HAS_WOLFRAM
   /*
    * The matching rules are Wolfram's (`wf_mod_match_mute_words`, a port of the
    * official client's matcher), not Cobalt's own: whole-word for a single word,
    * substring for a phrase or one with punctuation, punctuation trimmed from
    * the ends of words, and CJK-style languages by substring. Cobalt used to
    * carry a copy of these rules, as did Indigo; they belong in the SDK.
    */
   if (!prefs || prefs->count == 0) {
      return false;
   }
   wf_mod_muted_word words[COBALT_PREFS_WORDS_MAX];
   char values[COBALT_PREFS_WORDS_MAX][COBALT_PREFS_WORD_MAX];
   const char *tag_list[COBALT_POST_FACETS_MAX];
   size_t tag_n = 0;

   memset(words, 0, sizeof(words));
   for (int i = 0; i < prefs->count; i++) {
      const cobalt_muted_word *w = &prefs->words[i];
      /* A tag mute may be written with or without the leading #. */
      const char *v = (w->tag && !w->content && w->value[0] == '#') ? w->value + 1 : w->value;
      snprintf(values[i], sizeof(values[i]), "%s", v);
      words[i].value = values[i];
      words[i].targets_content = w->content;
      words[i].targets_tag = w->tag;
   }
   for (int t = 0; tags && t < tag_count && tag_n < COBALT_POST_FACETS_MAX; t++) {
      if (tags[t]) {
         tag_list[tag_n++] = tags[t];
      }
   }
   wf_mod_mute_word_match *matches = NULL;
   size_t match_count = 0;
   if (wf_mod_match_mute_words(&matches, &match_count, words, (size_t) prefs->count,
                               text ? text : "", tag_list, tag_n, NULL, 0) != WF_OK) {
      return false;
   }
   wf_mod_mute_word_matches_free(matches, match_count);
   return match_count > 0;
#else
   /* No SDK, no network, nothing to filter. */
   (void) prefs; (void) text; (void) tags; (void) tag_count;
   return false;
#endif
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
