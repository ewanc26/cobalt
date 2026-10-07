/* Reading: the timeline, a custom feed, a thread, notifications and post search, and the calls that start them. */

#include "atproto/session_internal.h"

#ifdef COBALT_HAS_WOLFRAM

/* Fetch the saved preferences once per sign-in. Failure is not fatal: the
 * feed is simply shown unfiltered. */
static void
ensure_prefs(void)
{
   if (g_session.prefs_loaded || !g_session.wf) {
      return;
   }
   wf_actor_preferences p;
   memset(&p, 0, sizeof(p));
   const wf_status st = wf_agent_get_actor_prefs_typed(g_session.wf, &p);
   if (st != WF_OK) {
      COBALT_LOGW("session: getPreferences failed (%d); feed unfiltered", (int) st);
      cobalt_prefs_clear(&g_session.prefs);
      return;
   }
   cobalt_prefs_from_wolfram(&g_session.prefs, &p, cobalt_time_now());
   wf_actor_preferences_free(&p);
   g_session.prefs_loaded = true;
}

void
cobalt_session_run_timeline(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see your timeline.");
      return;
   }

   /* Paging uses the cursor from the last page; a refresh deliberately does
    * not, so pulling down after an hour away gets the top of the feed rather
    * than resuming where the old cursor pointed. */
   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.feed.cursor[0] ? g_session.feed.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);

      if (!cursor) {
         /* Nothing further to fetch: report success with no new posts rather
          * than an error, since the user did nothing wrong. */
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_feed_list list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: getTimeline limit=%d cursor=%s", COBALT_SESSION_PAGE,
               cursor ? cursor : "(top)");
   wf_status status = wf_agent_get_timeline_typed(g_session.wf, COBALT_SESSION_PAGE, cursor, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: getTimeline failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_TIMELINE);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   /* Resolved once per page rather than per post: a feed rendered across a
    * second boundary showing two different "now" values would be worse than
    * one that is a moment stale. */
   ensure_prefs();
   const int64_t now = cobalt_time_now();

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_feed_reset(&g_session.feed);
   }
   const int before = g_session.feed.count;
   const int added = cobalt_feed_append_from_wolfram(&g_session.feed, &list, now);
   cobalt_prefs_filter_feed(&g_session.prefs, &g_session.feed, before, true);
   /* A page that added nothing is the end as far as this client is concerned,
    * whatever cursor came back — otherwise a screen that pages on reaching the
    * last post would ask again immediately, and keep asking. */
   if (in->paging && added == 0) {
      g_session.feed.has_more = false;
      g_session.feed.cursor[0] = '\0';
   }
   const int total = g_session.feed.count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_feed_list_free(&list);

   COBALT_LOGI("session: timeline +%d posts (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "Your timeline is empty. Follow some accounts on another "
                     "device and they will show up here.");
   }

   /* The session refreshes its tokens transparently, so a fetch may have
    * rotated them; persist whatever the agent holds now. */
   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/*
 * A custom feed (app.bsky.feed.getFeed), identified by `in->uri` — the
 * generator's AT-URI. `wf_agent_get_feed_typed` returns `wf_agent_feed_view_list`,
 * a typedef alias for the same `wf_agent_feed_list` getTimeline returns (see
 * feedgen_typed.h), so this is run_timeline with a different Wolfram call and
 * no home-timeline-specific "nothing further" framing for an empty result.
 */
void
cobalt_session_run_feed(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to view feeds.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.feed.cursor[0] ? g_session.feed.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);

      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_feed_list list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: getFeed %s limit=%d cursor=%s", in->uri, COBALT_SESSION_PAGE,
               cursor ? cursor : "(top)");
   wf_status status = wf_agent_get_feed_typed(g_session.wf, in->uri, COBALT_SESSION_PAGE, cursor, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: getFeed failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_FEED);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   ensure_prefs();
   const int64_t now = cobalt_time_now();

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_feed_reset(&g_session.feed);
   }
   const int before = g_session.feed.count;
   const int added = cobalt_feed_append_from_wolfram(&g_session.feed, &list, now);
   cobalt_prefs_filter_feed(&g_session.prefs, &g_session.feed, before, false);
   if (in->paging && added == 0) {
      g_session.feed.has_more = false;
      g_session.feed.cursor[0] = '\0';
   }
   const int total = g_session.feed.count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_feed_list_free(&list);

   COBALT_LOGI("session: feed +%d posts (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "This feed has no posts right now.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_thread(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to read threads.");
      return;
   }

   wf_agent_thread thread;
   memset(&thread, 0, sizeof(thread));

   /*
    * Depth 6 is a compromise. Deeper costs response size and parse time on a
    * console for replies that would be pinned at the maximum indent anyway
    * (COBALT_THREAD_MAX_DEPTH), and the flattened buffer would fill before
    * they were reached.
    */
   COBALT_LOGI("session: getPostThread %s", in->uri);
   wf_status status = wf_agent_get_post_thread_typed(g_session.wf, in->uri, 6, &thread);
   if (status != WF_OK) {
      COBALT_LOGW("session: getPostThread failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_THREAD);
      /* Drop whatever was loaded. The screen has already switched, so leaving
       * it would show a *different* conversation than the one asked for —
       * complete with its focus marker and actionable posts. */
      SDL_LockMutex(g_session.lock);
      cobalt_thread_reset(&g_session.conversation);
      SDL_UnlockMutex(g_session.lock);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(g_session.lock);
   cobalt_thread_from_wolfram(&g_session.conversation, &thread, now);
   const int count = g_session.conversation.count;
   const bool truncated = g_session.conversation.truncated;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_thread_free(&thread);

   COBALT_LOGI("session: thread %d posts%s", count, truncated ? " (truncated)" : "");
   if (truncated) {
      cobalt_session_set_message(r, "This conversation is longer than Cobalt can show.");
   }

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_notifications(const cobalt_job_input *in, cobalt_job_result *r,
                  cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see notifications.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.notifications.cursor[0] ? g_session.notifications.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_notification_list list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: listNotifications cursor=%s", cursor ? cursor : "(top)");
   wf_status status = wf_agent_list_notifications_typed(g_session.wf, COBALT_SESSION_PAGE,
                                                        cursor, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: listNotifications failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_NOTIFICATIONS);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_notifications_reset(&g_session.notifications);
   }
   const int added =
      cobalt_notifications_append_from_wolfram(&g_session.notifications, &list, now);
   if (in->paging && added == 0) {
      g_session.notifications.has_more = false;
      g_session.notifications.cursor[0] = '\0';
   }
   const int total = g_session.notifications.count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_notification_list_free(&list);
   COBALT_LOGI("session: notifications +%d (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "No notifications yet.");
   }

   /*
    * Mark everything up to now as seen, but only on a top-of-list fetch —
    * doing it while paging backwards through history would mark things read
    * that the user has not reached yet. A failure is logged rather than
    * surfaced: the notifications themselves arrived, and an error message
    * about a badge would be noise.
    */
   if (!in->paging && total > 0 && now > 0) {
      char seen_at[32];
      if (wf_time_format_rfc3339(now, seen_at, sizeof(seen_at)) == WF_OK &&
          wf_agent_update_seen_notifications(g_session.wf, seen_at) != WF_OK) {
         COBALT_LOGW("session: updateSeen failed — the unread badge may linger "
                     "on other clients");
      }
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* Not routed through run_actor_list — searchActors takes a query string on
 * top of limit/cursor, so its Wolfram wrapper has a different shape than the
 * mutes/blocks fetchers run_actor_list is built around. A fresh (non-paging)
 * search always resets the held list, even to empty on a bad query, since a
 * stale result from the previous query left on screen would look like a
 * match for the new one. */
void
cobalt_session_run_search_posts(const cobalt_job_input *in, cobalt_job_result *r,
                 cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to search.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.feed.cursor[0] ? g_session.feed.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_post_list list;
   memset(&list, 0, sizeof(list));
   char *next = NULL;

   COBALT_LOGI("session: searchPosts '%s' cursor=%s", in->text,
               cursor ? cursor : "(top)");
   wf_status status = wf_agent_search_posts_typed(g_session.wf, in->text, COBALT_SESSION_PAGE,
                                                  cursor, &list, &next);
   if (status != WF_OK) {
      COBALT_LOGW("session: searchPosts failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_SEARCH_POSTS);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_feed_reset(&g_session.feed);
   }
   const int added = cobalt_feed_append_posts_from_wolfram(&g_session.feed, &list, next, now);
   if (in->paging && added == 0) {
      g_session.feed.has_more = false;
      g_session.feed.cursor[0] = '\0';
   }
   const int total = g_session.feed.count;
   SDL_UnlockMutex(g_session.lock);

   free(next);
   wf_agent_post_list_free(&list);

   COBALT_LOGI("session: search +%d posts (%d held)", added, total);
   if (total == 0) {
      cobalt_session_set_message(r, "No posts matched that search.");
   }

   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

#endif /* COBALT_HAS_WOLFRAM */

bool
cobalt_session_begin_timeline(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   if (!paging) {
      g_session.feed_source = 0;
      g_session.feed_arg[0] = '\0';
   }
   return cobalt_session_submit(COBALT_JOB_TIMELINE, &in);
}

bool
cobalt_session_begin_feed(const char *feed_uri, bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", feed_uri ? feed_uri : "");
   in.paging = paging;
   if (!paging) {
      g_session.feed_source = 1;
      snprintf(g_session.feed_arg, sizeof(g_session.feed_arg), "%s", in.uri);
   }
   return cobalt_session_submit(COBALT_JOB_FEED, &in);
}

bool
cobalt_session_begin_search_posts(const char *query, bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   snprintf(in.text, sizeof(in.text), "%s", query ? query : "");
   if (!paging) {
      g_session.feed_source = 2;
      snprintf(g_session.feed_arg, sizeof(g_session.feed_arg), "%s", in.text);
   }
   return cobalt_session_submit(COBALT_JOB_SEARCH_POSTS, &in);
}

bool
cobalt_session_begin_feed_current(bool paging)
{
   switch (g_session.feed_source) {
      case 1: return cobalt_session_begin_feed(g_session.feed_arg, paging);
      case 2: return cobalt_session_begin_search_posts(g_session.feed_arg, paging);
      default: return cobalt_session_begin_timeline(paging);
   }
}

const cobalt_feed *
cobalt_session_feed(void)
{
   return &g_session.feed;
}

const cobalt_thread *
cobalt_session_thread(void)
{
   return &g_session.conversation;
}

const cobalt_notifications *
cobalt_session_notifications(void)
{
   return &g_session.notifications;
}

bool
cobalt_session_begin_notifications(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_NOTIFICATIONS, &in);
}

bool
cobalt_session_begin_thread(const char *uri)
{
   if (!uri || !uri[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", uri);
   return cobalt_session_submit(COBALT_JOB_THREAD, &in);
}
