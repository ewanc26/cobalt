#include "atproto/prefs.h"
#include "util/log.h"

#include <string.h>

void
cobalt_prefs_clear(cobalt_prefs *prefs)
{
   if (prefs) {
      memset(prefs, 0, sizeof(*prefs));
   }
}

bool
cobalt_prefs_text_is_muted(const cobalt_prefs *prefs, const char *text,
                           const char *const *tags, int tag_count)
{
   if (!prefs) {
      return false;
   }
   return wf_muted_list_match(&prefs->muted, text, tags,
                              tags && tag_count > 0 ? (size_t) tag_count : 0, false);
}

static bool
post_is_hidden(const cobalt_prefs *prefs, const cobalt_post *post, bool home)
{
   if (home && prefs->hide_reposts && post->reposted_by[0]) {
      return true;
   }
   if (prefs->muted.count == 0) {
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

void
cobalt_prefs_from_wolfram(cobalt_prefs *prefs, const wf_actor_preferences *src,
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
   wf_muted_list_from_prefs(&prefs->muted, src, now);
   COBALT_LOGI("prefs: %u muted word(s), hide reposts %s", (unsigned) prefs->muted.count,
               prefs->hide_reposts ? "on" : "off");
}
