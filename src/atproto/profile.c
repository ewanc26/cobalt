#include "atproto/profile.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/agent.h>
#endif

#include <stdio.h>
#include <string.h>

/* Same middle dot the counts line uses — Latin-1, so present in any font. */
#define SEPARATOR " \xC2\xB7 "

void
cobalt_profile_reset(cobalt_profile *profile)
{
   if (profile) {
      memset(profile, 0, sizeof(*profile));
   }
}

void
cobalt_profile_format_number(char *out, size_t out_size, int value,
                             const char *singular, const char *plural)
{
   if (!out || out_size == 0) {
      return;
   }
   out[0] = '\0';

   if (value < 0) {
      value = 0;
   }

   /* Build the digits backwards with separators, then reverse. Doing it this
    * way avoids needing to know the digit count up front. */
   char digits[24];
   int len = 0;
   int n = value;

   do {
      if (len > 0 && len % 4 == 3 && len < (int) sizeof(digits) - 1) {
         digits[len++] = ',';
      }
      if (len >= (int) sizeof(digits) - 1) {
         break;
      }
      digits[len++] = (char) ('0' + (n % 10));
      n /= 10;
   } while (n > 0);

   char grouped[24];
   for (int i = 0; i < len; i++) {
      grouped[i] = digits[len - 1 - i];
   }
   grouped[len] = '\0';

   snprintf(out, out_size, "%s %s", grouped,
            value == 1 ? singular : plural);
}

void
cobalt_profile_format_counts(char *out, size_t out_size, int followers,
                             int follows)
{
   if (!out || out_size == 0) {
      return;
   }

   char a[48];
   char b[48];
   cobalt_profile_format_number(a, sizeof(a), followers, "follower", "followers");
   cobalt_profile_format_number(b, sizeof(b), follows, "following", "following");

   snprintf(out, out_size, "%s%s%s", a, SEPARATOR, b);
}

void
cobalt_profile_apply_follow(cobalt_profile *profile, const char *record_uri)
{
   if (!profile) {
      return;
   }
   snprintf(profile->viewer_following, sizeof(profile->viewer_following), "%s",
            (record_uri && record_uri[0]) ? record_uri : "");
}

void
cobalt_profile_apply_block(cobalt_profile *profile, const char *record_uri)
{
   if (!profile) {
      return;
   }
   snprintf(profile->viewer_blocking, sizeof(profile->viewer_blocking), "%s",
            (record_uri && record_uri[0]) ? record_uri : "");
}

void
cobalt_profile_apply_mute(cobalt_profile *profile, bool muted)
{
   if (!profile) {
      return;
   }
   profile->viewer_muted = muted;
}

#ifdef COBALT_HAS_WOLFRAM

void
cobalt_profile_from_wolfram(cobalt_profile *out,
                            const struct wf_agent_profile *src,
                            const char *self_did)
{
   if (!out) {
      return;
   }
   cobalt_profile_reset(out);
   if (!src) {
      return;
   }

   const wf_agent_profile *typed = (const wf_agent_profile *) src;

   snprintf(out->did, sizeof(out->did), "%s", typed->did ? typed->did : "");
   snprintf(out->handle, sizeof(out->handle), "@%s",
            typed->handle ? typed->handle : "");

   /* A display name is optional; the handle always exists. */
   const char *display = typed->display_name;
   if (!display || display[0] == '\0') {
      display = typed->handle ? typed->handle : "";
   }
   cobalt_feed_copy_text(out->display_name, sizeof(out->display_name), display);

   cobalt_feed_copy_text(out->description, sizeof(out->description),
                         typed->description ? typed->description : "");

   /* Wolfram's field is named avatar_cid, but for a *parsed* profile view it
    * holds the server's avatar URL rather than a blob CID — the SDK documents
    * this at the assignment in agent.c. */
   snprintf(out->avatar, sizeof(out->avatar), "%s",
            typed->avatar_cid ? typed->avatar_cid : "");

   cobalt_profile_format_counts(out->counts, sizeof(out->counts),
                                typed->followers_count, typed->follows_count);
   cobalt_profile_format_number(out->post_count, sizeof(out->post_count),
                                typed->posts_count, "post", "posts");

   snprintf(out->viewer_following, sizeof(out->viewer_following), "%s",
            typed->following ? typed->following : "");
   snprintf(out->viewer_blocking, sizeof(out->viewer_blocking), "%s",
            typed->blocking ? typed->blocking : "");
   out->viewer_muted = typed->muted;

   /* Comparing DIDs rather than handles: a handle can be changed, and the
    * follow button appearing on your own profile would be a visible bug. */
   out->is_self = self_did && self_did[0] && typed->did &&
                  strcmp(self_did, typed->did) == 0;

   out->loaded = true;
}

#endif /* COBALT_HAS_WOLFRAM */
