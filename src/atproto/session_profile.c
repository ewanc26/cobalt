/* A profile and what you do to it: the profile itself, its tabs, follow, mute and block, and the calls that start them. */

#include "atproto/session_internal.h"

#ifdef COBALT_HAS_WOLFRAM

/* Replace s.author_feed with the posts for one profile tab. Failure leaves the
 * feed empty rather than showing another tab's posts under the wrong label. */
static void
fetch_author_feed(const char *actor, int tab, const char *pinned_uri)
{
   wf_agent_feed_list list;
   memset(&list, 0, sizeof(list));

   const wf_status status = wf_agent_get_profile_tab_typed(
       g_session.wf, actor, tab, COBALT_SESSION_PAGE, NULL, &list);

   if (status == WF_OK) {
      const int64_t now = cobalt_time_now();
      SDL_LockMutex(g_session.lock);
      cobalt_feed_reset(&g_session.author_feed);
      cobalt_feed_append_from_wolfram(&g_session.author_feed, &list, now);
      SDL_UnlockMutex(g_session.lock);
      wf_agent_feed_list_free(&list);

      /* The Posts tab leads with the pinned post, as the official client does.
       * Best-effort: a failure here leaves the ordinary feed untouched. */
      if (pinned_uri && pinned_uri[0] && tab == WF_PROFILE_TAB_POSTS) {
        wf_agent_post_list pins;
        memset(&pins, 0, sizeof(pins));
        const char *uris[1] = {pinned_uri};
        if (wf_agent_get_posts_typed(g_session.wf, uris, 1, &pins) == WF_OK) {
          SDL_LockMutex(g_session.lock);
          cobalt_feed_pin_from_wolfram(&g_session.author_feed, &pins, now);
          SDL_UnlockMutex(g_session.lock);
          wf_agent_post_list_free(&pins);
        }
      }
   } else {
      COBALT_LOGW("session: author feed (tab %d) failed (%d) — showing the "
                  "profile without posts", tab, (int) status);
      SDL_LockMutex(g_session.lock);
      cobalt_feed_reset(&g_session.author_feed);
      SDL_UnlockMutex(g_session.lock);
   }
}

void
cobalt_session_run_profile_tab(const cobalt_job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }
   SDL_LockMutex(g_session.lock);
   char pinned[COBALT_POST_URI_MAX];
   snprintf(pinned, sizeof(pinned), "%s", g_session.pinned_uri);
   SDL_UnlockMutex(g_session.lock);
   fetch_author_feed(in->uri, in->tab, pinned);
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_profile(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   wf_agent_profile profile;
   memset(&profile, 0, sizeof(profile));

   COBALT_LOGI("session: getProfile %s", in->uri);
   wf_status status = wf_agent_get_profile(g_session.wf, in->uri, &profile);
   if (status != WF_OK) {
      COBALT_LOGW("session: getProfile failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_PROFILE);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   cobalt_profile_from_wolfram(&g_session.profile, &profile, g_session.did);
   SDL_UnlockMutex(g_session.lock);

   char pinned[COBALT_POST_URI_MAX];
   snprintf(pinned, sizeof(pinned), "%s",
            profile.pinned_post_uri ? profile.pinned_post_uri : "");
   SDL_LockMutex(g_session.lock);
   snprintf(g_session.pinned_uri, sizeof(g_session.pinned_uri), "%s", pinned);
   SDL_UnlockMutex(g_session.lock);
   wf_agent_profile_free(&profile);

   /*
    * The posts are a second request, but the same job. A failure here is not
    * fatal to the screen: the profile itself already loaded and is worth
    * showing, so the feed is left empty and the header stands on its own.
    */
   fetch_author_feed(in->uri, in->tab, pinned);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_follow(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const bool undo = in->record_uri[0] != '\0';
   wf_status status;
   char created[COBALT_POST_URI_MAX] = "";

   if (undo) {
      COBALT_LOGI("session: unfollowing %s", in->record_uri);
      status = wf_agent_unfollow(g_session.wf, in->record_uri);
   } else {
      wf_agent_post_result result;
      memset(&result, 0, sizeof(result));

      COBALT_LOGI("session: following %s", in->uri);
      status = wf_agent_follow(g_session.wf, in->uri, &result);
      if (status == WF_OK) {
         snprintf(created, sizeof(created), "%s", result.uri ? result.uri : "");
      }
      wf_agent_post_result_free(&result);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: follow failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not %s (wolfram status %d).",
                  undo ? "unfollow" : "follow", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   cobalt_profile_apply_follow(&g_session.profile, undo ? NULL : created);
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_mute(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const bool mute = in->flag;
   COBALT_LOGI("session: %s %s", mute ? "muting" : "unmuting", in->uri);
   const wf_status status = mute ? wf_agent_mute_actor(g_session.wf, in->uri)
                                 : wf_agent_unmute_actor(g_session.wf, in->uri);

   if (status != WF_OK) {
      COBALT_LOGW("session: %s failed (%d)", mute ? "mute" : "unmute",
                  (int) status);
      cobalt_session_set_message(r, "Could not %s (wolfram status %d).",
                  mute ? "mute" : "unmute", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   /* Only the loaded profile's own state, if this is the account it's
    * about — the same call also fires from the muted-accounts list, where
    * `in->uri` names whichever row was unmuted, not necessarily whoever's
    * profile (if any) happens to be loaded. */
   if (g_session.profile.loaded && strcmp(g_session.profile.did, in->uri) == 0) {
      cobalt_profile_apply_mute(&g_session.profile, mute);
   }
   if (!mute) {
      cobalt_actor_list_remove(&g_session.muted, in->uri);
   }
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_block(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const bool undo = in->record_uri[0] != '\0';
   wf_status status;
   char created[COBALT_POST_URI_MAX] = "";

   if (undo) {
      COBALT_LOGI("session: unblocking %s", in->record_uri);
      status = wf_agent_unblock(g_session.wf, in->record_uri);
   } else {
      wf_agent_post_result result;
      memset(&result, 0, sizeof(result));

      COBALT_LOGI("session: blocking %s", in->uri);
      status = wf_agent_block(g_session.wf, in->uri, &result);
      if (status == WF_OK) {
         snprintf(created, sizeof(created), "%s", result.uri ? result.uri : "");
      }
      wf_agent_post_result_free(&result);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: block failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not %s (wolfram status %d).",
                  undo ? "unblock" : "block", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   if (g_session.profile.loaded && strcmp(g_session.profile.did, in->uri) == 0) {
      cobalt_profile_apply_block(&g_session.profile, undo ? NULL : created);
   }
   if (undo) {
      cobalt_actor_list_remove(&g_session.blocked, in->uri);
   }
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

#endif /* COBALT_HAS_WOLFRAM */

const cobalt_profile *
cobalt_session_profile(void)
{
   return &g_session.profile;
}

const cobalt_feed *
cobalt_session_author_feed(void)
{
   return &g_session.author_feed;
}

bool
cobalt_session_begin_profile(const char *actor)
{
   if (!actor || !actor[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", actor);
   in.tab = WF_PROFILE_TAB_POSTS;
   SDL_LockMutex(g_session.lock);
   g_session.profile_tab = WF_PROFILE_TAB_POSTS;
   SDL_UnlockMutex(g_session.lock);
   return cobalt_session_submit(COBALT_JOB_PROFILE, &in);
}

bool
cobalt_session_begin_profile_tab(int tab)
{
  if (tab < 0 || tab >= WF_PROFILE_TAB_COUNT || !g_session.profile.loaded ||
      !g_session.profile.did[0]) {
    return false;
  }
  if (tab == WF_PROFILE_TAB_LIKES && !g_session.profile.is_self) {
    return false;
  }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", g_session.profile.did);
   in.tab = tab;
   SDL_LockMutex(g_session.lock);
   g_session.profile_tab = tab;
   SDL_UnlockMutex(g_session.lock);
   return cobalt_session_submit(COBALT_JOB_PROFILE_TAB, &in);
}

int
cobalt_session_profile_tab(void)
{
   return g_session.profile_tab;
}

bool
cobalt_session_begin_follow(void)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));

   SDL_LockMutex(g_session.lock);
   const bool have = g_session.profile.loaded && !g_session.profile.is_self && g_session.profile.did[0];
   if (have) {
      snprintf(in.uri, sizeof(in.uri), "%s", g_session.profile.did);
      snprintf(in.record_uri, sizeof(in.record_uri), "%s",
               g_session.profile.viewer_following);
   }
   SDL_UnlockMutex(g_session.lock);

   /* Nothing loaded, or it is the signed-in account — which has no follow
    * button, so this should not have been reachable. */
   if (!have) {
      return false;
   }
   return cobalt_session_submit(COBALT_JOB_FOLLOW, &in);
}

bool
cobalt_session_begin_mute(void)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));

   SDL_LockMutex(g_session.lock);
   const bool have = g_session.profile.loaded && !g_session.profile.is_self && g_session.profile.did[0];
   if (have) {
      snprintf(in.uri, sizeof(in.uri), "%s", g_session.profile.did);
      in.flag = !g_session.profile.viewer_muted;   /* toggle: mute if not muted */
   }
   SDL_UnlockMutex(g_session.lock);

   if (!have) {
      return false;
   }
   return cobalt_session_submit(COBALT_JOB_MUTE, &in);
}

bool
cobalt_session_begin_block(void)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));

   SDL_LockMutex(g_session.lock);
   const bool have = g_session.profile.loaded && !g_session.profile.is_self && g_session.profile.did[0];
   if (have) {
      snprintf(in.uri, sizeof(in.uri), "%s", g_session.profile.did);
      snprintf(in.record_uri, sizeof(in.record_uri), "%s",
               g_session.profile.viewer_blocking);
   }
   SDL_UnlockMutex(g_session.lock);

   if (!have) {
      return false;
   }
   return cobalt_session_submit(COBALT_JOB_BLOCK, &in);
}
