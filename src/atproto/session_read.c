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

static void cobalt_session_run_convo_list(const cobalt_job_input *in, cobalt_job_result *r,
                                          cobalt_auth_state *state, bool requests);

void
cobalt_session_run_chat_inbox(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state)
{
   cobalt_session_run_convo_list(in, r, state, false);
}

void
cobalt_session_run_chat_requests(const cobalt_job_input *in, cobalt_job_result *r,
                                 cobalt_auth_state *state)
{
   cobalt_session_run_convo_list(in, r, state, true);
}

/* The inbox and the request inbox are the same listing with a different
 * endpoint and a different list to fill, so both come through here. */
static void
cobalt_session_run_convo_list(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state, bool requests)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }

   cobalt_chat_inbox *box = requests ? &g_session.requests : &g_session.chat;
   const cobalt_job_kind kind = requests ? COBALT_JOB_CHAT_REQUESTS : COBALT_JOB_CHAT_INBOX;
   const char *what = requests ? "chat requests" : "chat inbox";

   char cursor_buf[COBALT_CURSOR_MAX];
   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      if (box->cursor[0]) {
         snprintf(cursor_buf, sizeof(cursor_buf), "%s", box->cursor);
         cursor = cursor_buf;
      }
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_chat_convo_list list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: %s cursor=%s", what, cursor ? cursor : "(top)");
   wf_status status = requests
      ? wf_agent_chat_list_convo_requests_typed(g_session.wf, COBALT_SESSION_PAGE, cursor, &list)
      : wf_agent_chat_list_convos(g_session.wf, COBALT_SESSION_PAGE, cursor, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: %s fetch failed (%d)", what, (int) status);
      cobalt_session_describe_failure(r, status, kind);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_chat_inbox_reset(box);
   }
   const int added = cobalt_chat_inbox_append_from_wolfram(box, &list, g_session.did);
   if (in->paging && added == 0) {
      box->has_more = false;
      box->cursor[0] = '\0';
   }
   const int total = box->count;
   SDL_UnlockMutex(g_session.lock);

   wf_chat_convo_list_free(&list);
   COBALT_LOGI("session: %s +%d (%d held)", what, added, total);

   if (total == 0) {
      cobalt_session_set_message(r, requests ? "No message requests." : "No messages yet.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_messages(const cobalt_job_input *in, cobalt_job_result *r,
                                 cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }
   if (!in->uri[0]) {
      cobalt_session_set_message(r, "That conversation could not be opened.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   wf_chat_message_list list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: getMessages");
   wf_status status = wf_agent_chat_get_messages(g_session.wf, in->uri,
                                                 COBALT_CHAT_MESSAGES_MAX, NULL, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: getMessages failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_MESSAGES);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(g_session.lock);
   snprintf(g_session.chat_thread.convo_id, sizeof(g_session.chat_thread.convo_id), "%s",
            in->uri);
   cobalt_chat_thread_fill_from_wolfram(&g_session.chat_thread, &list, g_session.did, now);
   const int total = g_session.chat_thread.count;
   SDL_UnlockMutex(g_session.lock);

   wf_chat_message_list_free(&list);

   if (total == 0) {
      cobalt_session_set_message(r, "No messages in this conversation yet.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_send(const cobalt_job_input *in, cobalt_job_result *r,
                             cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to send messages.");
      return;
   }
   if (!in->uri[0] || !cobalt_chat_reply_valid(in->text)) {
      cobalt_session_set_message(r, "There is nothing to send.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   wf_chat_message sent;
   memset(&sent, 0, sizeof(sent));

   COBALT_LOGI("session: sendMessage");
   wf_status status = wf_agent_chat_send_message(g_session.wf, in->uri, in->text, NULL, &sent);
   wf_chat_message_reset(&sent);
   if (status != WF_OK) {
      COBALT_LOGW("session: sendMessage failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_SEND);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_start(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state)
{
   SDL_LockMutex(g_session.lock);
   g_session.chat_started[0] = '\0';
   SDL_UnlockMutex(g_session.lock);

   if (!g_session.wf || !g_session.did[0]) {
      cobalt_session_set_message(r, "Sign in to send messages.");
      return;
   }

   char target[COBALT_CHAT_TARGET_MAX];
   const cobalt_chat_target_kind kind =
      cobalt_chat_target_normalise(in->uri, target, sizeof(target))
         ? cobalt_chat_target_kind_of(target)
         : COBALT_CHAT_TARGET_INVALID;
   if (kind == COBALT_CHAT_TARGET_INVALID) {
      cobalt_session_set_message(r, "Enter a handle or DID to message.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   char did[COBALT_CHAT_TARGET_MAX];
   if (kind == COBALT_CHAT_TARGET_HANDLE) {
      char *resolved = NULL;

      COBALT_LOGI("session: resolveHandle");
      wf_status status = wf_agent_resolve_handle(g_session.wf, target, &resolved);
      const bool fits = resolved && strlen(resolved) < sizeof(did);
      if (status == WF_OK && fits) {
         snprintf(did, sizeof(did), "%s", resolved);
      }
      free(resolved);

      if (status != WF_OK || !fits) {
         /* The service answers an unknown handle with an HTTP error, so that
          * reads as "no such account" rather than as a network fault. */
         if (status == WF_OK || status == WF_ERR_HTTP) {
            cobalt_session_set_message(r, "No account has that handle.");
         } else {
            COBALT_LOGW("session: resolveHandle failed (%d)", (int) status);
            cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_START);
         }
         *state = COBALT_AUTH_SIGNED_IN;
         return;
      }
   } else {
      snprintf(did, sizeof(did), "%s", target);
   }

   if (cobalt_chat_is_self(g_session.did, did)) {
      cobalt_session_set_message(r, "That is your own account.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   /* getConvoForMembers takes every member, the viewer included. */
   const char *members[2] = { g_session.did, did };
   wf_chat_convo convo;
   memset(&convo, 0, sizeof(convo));

   COBALT_LOGI("session: getConvoForMembers");
   wf_status status = wf_agent_chat_get_convo_for_members_typed(g_session.wf, members, 2, &convo);
   char id[COBALT_CHAT_ID_MAX];
   const bool copied = status == WF_OK && cobalt_chat_convo_id_copy(id, sizeof(id), convo.id);
   wf_chat_convo_free(&convo);

   if (!copied) {
      if (status == WF_OK) {
         cobalt_session_set_message(r, "The server did not return a conversation.");
      } else {
         COBALT_LOGW("session: getConvoForMembers failed (%d)", (int) status);
         cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_START);
      }
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   snprintf(g_session.chat_started, sizeof(g_session.chat_started), "%s", id);
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* --- a group's name, members and join link --- */

/* Split, validate, resolve and de-duplicate a member list through Wolfram. On
 * failure sets the message in `r` and frees `out`; on success the caller owns
 * `out` and must free it after the request. */
static bool
resolve_group_members(const char *list, wf_chat_member_list *out,
                      cobalt_job_result *r)
{
   const wf_status status = wf_agent_chat_resolve_member_list(
      g_session.wf, list, g_session.did, out);

   if (status == WF_OK) {
      return true;
   }
   if (out->invalid) {
      if (status == WF_ERR_HTTP) {
         cobalt_session_set_message(r, "No account has the handle @%s.", out->invalid);
      } else if (status == WF_ERR_INVALID_ARG) {
         cobalt_session_set_message(r, "That is not a handle or DID: %s", out->invalid);
      } else {
         COBALT_LOGW("session: resolve member list failed (%d)", (int) status);
         cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_GROUP_CREATE);
      }
   } else {
      cobalt_session_set_message(r, "Enter someone else to put in the group.");
   }
   wf_chat_member_list_free(out);
   return false;
}

/* Read a group's detail and full member list. The typed parsers do the JSON
 * work; this only copies their results into the screen's fixed buffers. */
static bool
fetch_group_detail(const char *convo_id, cobalt_chat_group *out,
                   cobalt_job_result *r, cobalt_job_kind kind)
{
   wf_chat_convo convo;
   wf_chat_convo_members members;
   wf_status status;

   memset(&convo, 0, sizeof(convo));
   memset(&members, 0, sizeof(members));

   COBALT_LOGI("session: getConvo");
   status = wf_agent_chat_get_convo(g_session.wf, convo_id, &convo);
   if (status != WF_OK) {
      COBALT_LOGW("session: getConvo failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, kind);
      return false;
   }
   cobalt_chat_group_from_convo(out, &convo, convo_id);
   wf_chat_convo_free(&convo);

   COBALT_LOGI("session: getConvoMembers");
   status = wf_agent_chat_get_convo_members_typed(g_session.wf, convo_id, &members);
   if (status != WF_OK) {
      COBALT_LOGW("session: getConvoMembers failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, kind);
      return false;
   }
   cobalt_chat_group_from_members(out, &members);
   wf_chat_convo_members_free(&members);
   return true;
}

static void
publish_group(const cobalt_chat_group *group)
{
   SDL_LockMutex(g_session.lock);
   g_session.group = *group;
   SDL_UnlockMutex(g_session.lock);
}

void
cobalt_session_run_chat_group_info(const cobalt_job_input *in, cobalt_job_result *r,
                                   cobalt_auth_state *state)
{
   cobalt_chat_group group;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }
   cobalt_chat_group_reset(&group);
   if (!cobalt_chat_convo_id_copy(group.convo_id, sizeof(group.convo_id), in->uri)) {
      cobalt_session_set_message(r, "That group is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   if (!fetch_group_detail(in->uri, &group, r, COBALT_JOB_CHAT_GROUP_INFO)) {
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   publish_group(&group);
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_group_create(const cobalt_job_input *in, cobalt_job_result *r,
                                     cobalt_auth_state *state)
{
   wf_chat_member_list members;
   wf_chat_convo convo;
   cobalt_chat_group group;
   char id[COBALT_CHAT_ID_MAX];

   SDL_LockMutex(g_session.lock);
   g_session.chat_group_started[0] = '\0';
   SDL_UnlockMutex(g_session.lock);

   if (!g_session.wf || !g_session.did[0]) {
      cobalt_session_set_message(r, "Sign in to send messages.");
      return;
   }
   if (!in->name[0]) {
      cobalt_session_set_message(r, "Give the group a name.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   if (!resolve_group_members(in->text, &members, r)) {
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   memset(&convo, 0, sizeof(convo));
   COBALT_LOGI("session: createGroup");
   wf_status status = wf_agent_chat_create_group_typed(
      g_session.wf, (const char *const *) members.dids, members.count, in->name,
      &convo);
   wf_chat_member_list_free(&members);
   if (status != WF_OK) {
      COBALT_LOGW("session: createGroup failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_GROUP_CREATE);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   if (!cobalt_chat_convo_id_copy(id, sizeof(id), convo.id)) {
      cobalt_session_set_message(r, "The server did not return the new group.");
      wf_chat_convo_free(&convo);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   cobalt_chat_group_from_convo(&group, &convo, id);
   cobalt_chat_group_set_name(&group, in->name);
   wf_chat_convo_free(&convo);

   SDL_LockMutex(g_session.lock);
   snprintf(g_session.chat_group_started, sizeof(g_session.chat_group_started),
            "%s", id);
   SDL_UnlockMutex(g_session.lock);

   /* The create response carries only the important members; the detail fetch
    * fills the rest. If it fails, the group still exists and is usable. */
   fetch_group_detail(id, &group, r, COBALT_JOB_CHAT_GROUP_INFO);
   publish_group(&group);
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_group_rename(const cobalt_job_input *in, cobalt_job_result *r,
                                     cobalt_auth_state *state)
{
   wf_chat_convo convo;
   cobalt_chat_group group;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }
   if (!in->uri[0] || !in->text[0]) {
      cobalt_session_set_message(r, "Give the group a name.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   memset(&convo, 0, sizeof(convo));
   COBALT_LOGI("session: editGroup");
   wf_status status = wf_agent_chat_edit_group_typed(g_session.wf, in->uri, in->text, &convo);
   wf_chat_convo_free(&convo);
   if (status != WF_OK) {
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_GROUP_RENAME);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   group = g_session.group;
   SDL_UnlockMutex(g_session.lock);
   if (!cobalt_chat_convo_id_copy(group.convo_id, sizeof(group.convo_id), in->uri)) {
      cobalt_session_set_message(r, "That group is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   cobalt_chat_group_set_name(&group, in->text);
   fetch_group_detail(in->uri, &group, r, COBALT_JOB_CHAT_GROUP_INFO);
   publish_group(&group);
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_group_add(const cobalt_job_input *in, cobalt_job_result *r,
                                  cobalt_auth_state *state)
{
   wf_chat_member_list members;
   wf_chat_convo convo;
   cobalt_chat_group group;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }
   if (!in->uri[0]) {
      cobalt_session_set_message(r, "That group is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   if (!resolve_group_members(in->text, &members, r)) {
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   memset(&convo, 0, sizeof(convo));
   COBALT_LOGI("session: addGroupMembers");
   wf_status status = wf_agent_chat_add_members_typed(
      g_session.wf, in->uri, (const char *const *) members.dids, members.count,
      &convo);
   wf_chat_member_list_free(&members);
   if (status != WF_OK) {
      wf_chat_convo_free(&convo);
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_GROUP_ADD);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   group = g_session.group;
   SDL_UnlockMutex(g_session.lock);
   if (!cobalt_chat_convo_id_copy(group.convo_id, sizeof(group.convo_id), in->uri)) {
      wf_chat_convo_free(&convo);
      cobalt_session_set_message(r, "That group is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   cobalt_chat_group_from_convo(&group, &convo, in->uri);
   wf_chat_convo_free(&convo);
   fetch_group_detail(in->uri, &group, r, COBALT_JOB_CHAT_GROUP_INFO);
   publish_group(&group);
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_group_remove(const cobalt_job_input *in, cobalt_job_result *r,
                                     cobalt_auth_state *state)
{
   const char *dids[1];
   wf_chat_convo convo;
   cobalt_chat_group group;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }
   if (!in->uri[0] || !in->member_did[0]) {
      cobalt_session_set_message(r, "Choose a member to remove.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   dids[0] = in->member_did;
   memset(&convo, 0, sizeof(convo));
   COBALT_LOGI("session: removeGroupMembers");
   wf_status status = wf_agent_chat_remove_members_typed(g_session.wf, in->uri, dids, 1, &convo);
   if (status != WF_OK) {
      wf_chat_convo_free(&convo);
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_GROUP_REMOVE);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   group = g_session.group;
   SDL_UnlockMutex(g_session.lock);
   if (!cobalt_chat_convo_id_copy(group.convo_id, sizeof(group.convo_id), in->uri)) {
      wf_chat_convo_free(&convo);
      cobalt_session_set_message(r, "That group is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   cobalt_chat_group_from_convo(&group, &convo, in->uri);
   wf_chat_convo_free(&convo);
   fetch_group_detail(in->uri, &group, r, COBALT_JOB_CHAT_GROUP_INFO);
   publish_group(&group);
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_join_link(const cobalt_job_input *in, cobalt_job_result *r,
                                  cobalt_auth_state *state)
{
   wf_chat_join_link link;
   cobalt_chat_group group;
   wf_status status;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }
   if (!in->uri[0]) {
      cobalt_session_set_message(r, "That group is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   memset(&link, 0, sizeof(link));
   COBALT_LOGI("session: %s", in->link_create ? "createJoinLink"
                                               : in->link_disable ? "disableJoinLink"
                                                                  : "enableJoinLink");
   status = in->link_create
               ? wf_agent_chat_create_join_link_typed(
                    g_session.wf, in->uri, false, WF_CHAT_JOIN_RULE_ANYONE, &link)
               : in->link_disable
                    ? wf_agent_chat_disable_join_link_typed(g_session.wf, in->uri, &link)
                    : wf_agent_chat_enable_join_link_typed(g_session.wf, in->uri, &link);
   if (status != WF_OK) {
      wf_chat_join_link_free(&link);
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_JOIN_LINK);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   group = g_session.group;
   SDL_UnlockMutex(g_session.lock);
   if (!cobalt_chat_convo_id_copy(group.convo_id, sizeof(group.convo_id), in->uri)) {
      wf_chat_join_link_free(&link);
      cobalt_session_set_message(r, "That group is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   if (link.code) {
      snprintf(group.join_link_code, sizeof(group.join_link_code), "%s", link.code);
      group.join_link_enabled = wf_chat_join_link_is_enabled(link.enabled_status);
      wf_chat_join_link_url(group.join_link_code, group.join_link_url,
                            sizeof(group.join_link_url));
   }
   wf_chat_join_link_free(&link);

   fetch_group_detail(in->uri, &group, r, COBALT_JOB_CHAT_GROUP_INFO);
   publish_group(&group);
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* Accept or decline one request. Both come through here: the call differs, and
 * the list change is the same. The convo id is in `in->uri`. */
static void
cobalt_session_run_convo_decision(const cobalt_job_input *in, cobalt_job_result *r,
                                  cobalt_auth_state *state, bool accept)
{
   const cobalt_job_kind kind = accept ? COBALT_JOB_CHAT_ACCEPT : COBALT_JOB_CHAT_DECLINE;
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }

   char id[COBALT_CHAT_ID_MAX];
   if (!cobalt_chat_convo_id_copy(id, sizeof(id), in->uri)) {
      cobalt_session_set_message(r, "That message request is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   /* Accepting answers with a rev or nothing, and declining with the convo ref.
    * Neither body is needed, so the status-only calls are used: the typed
    * wrapper for accept parses a convoView, which the server does not send. */
   COBALT_LOGI("session: %s", accept ? "acceptConvo" : "leaveConvo");
   wf_status status = accept ? wf_agent_chat_accept_convo(g_session.wf, id)
                             : wf_agent_chat_leave_convo(g_session.wf, id);
   if (status != WF_OK) {
      COBALT_LOGW("session: %s failed (%d)", accept ? "acceptConvo" : "leaveConvo", (int) status);
      cobalt_session_describe_failure(r, status, kind);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   /* Accepted, the conversation joins the inbox, copied from the request list
    * so no fetch is needed. Only when the inbox is already loaded: an empty one
    * is fetched when Messages is opened, and one row would hide the rest. */
   SDL_LockMutex(g_session.lock);
   const cobalt_chat_convo *row = cobalt_chat_inbox_find(&g_session.requests, id);
   if (row) {
      cobalt_chat_convo moved = *row;
      cobalt_chat_inbox_remove(&g_session.requests, id);
      if (accept && g_session.chat.count > 0) {
         cobalt_chat_inbox_add_front(&g_session.chat, &moved);
      }
   }
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_accept(const cobalt_job_input *in, cobalt_job_result *r,
                               cobalt_auth_state *state)
{
   cobalt_session_run_convo_decision(in, r, state, true);
}

void
cobalt_session_run_chat_decline(const cobalt_job_input *in, cobalt_job_result *r,
                                cobalt_auth_state *state)
{
   cobalt_session_run_convo_decision(in, r, state, false);
}

void
cobalt_session_run_chat_join_requests(const cobalt_job_input *in, cobalt_job_result *r,
                                      cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }
   if (!in->uri[0]) {
      cobalt_session_set_message(r, "That group could not be opened.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   char cursor_buf[COBALT_CURSOR_MAX];
   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      if (g_session.join_requests.cursor[0]) {
         snprintf(cursor_buf, sizeof(cursor_buf), "%s", g_session.join_requests.cursor);
         cursor = cursor_buf;
      }
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_chat_join_requests list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: listJoinRequests cursor=%s", cursor ? cursor : "(top)");
   wf_status status = wf_agent_chat_list_join_requests_typed(g_session.wf, in->uri,
                                                             COBALT_SESSION_PAGE, cursor,
                                                             &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: listJoinRequests failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_CHAT_JOIN_REQUESTS);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_chat_join_requests_reset(&g_session.join_requests);
      /* The id was validated before this job was submitted, so the copy cannot
       * refuse, and it does not truncate the way a bare %s would. */
      (void) cobalt_chat_convo_id_copy(g_session.join_requests.convo_id,
                                       sizeof(g_session.join_requests.convo_id), in->uri);
   }
   const int added = cobalt_chat_join_requests_append_from_wolfram(
      &g_session.join_requests, &list, now);
   if (in->paging && added == 0) {
      g_session.join_requests.has_more = false;
      g_session.join_requests.cursor[0] = '\0';
   }
   const int total = g_session.join_requests.count;
   SDL_UnlockMutex(g_session.lock);

   wf_chat_join_requests_free(&list);
   COBALT_LOGI("session: listJoinRequests +%d (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "No join requests.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* Approve or reject one join request. Both come through here: the call differs,
 * and the list change is the same. The convo id is in `in->uri` and the
 * requester's DID in `in->member_did`. */
static void
cobalt_session_run_join_decision(const cobalt_job_input *in, cobalt_job_result *r,
                                 cobalt_auth_state *state, bool approve)
{
   const cobalt_job_kind kind = approve ? COBALT_JOB_CHAT_JOIN_APPROVE
                                        : COBALT_JOB_CHAT_JOIN_REJECT;
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in to see messages.");
      return;
   }

   char id[COBALT_CHAT_ID_MAX];
   if (!cobalt_chat_convo_id_copy(id, sizeof(id), in->uri) ||
       cobalt_chat_target_kind_of(in->member_did) != COBALT_CHAT_TARGET_DID) {
      cobalt_session_set_message(r, "That join request is not valid.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   COBALT_LOGI("session: %s", approve ? "approveJoinRequest" : "rejectJoinRequest");
   wf_status status;
   if (approve) {
      /* Approving answers with the updated convo view, which is not needed. */
      wf_chat_convo convo;
      memset(&convo, 0, sizeof(convo));
      status = wf_agent_chat_approve_join_request_typed(g_session.wf, id, in->member_did,
                                                        &convo);
      wf_chat_convo_free(&convo);
   } else {
      wf_chat_ok ok;
      memset(&ok, 0, sizeof(ok));
      status = wf_agent_chat_reject_join_request_typed(g_session.wf, id, in->member_did,
                                                       &ok);
      wf_chat_ok_free(&ok);
   }
   if (status != WF_OK) {
      COBALT_LOGW("session: join decision failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, kind);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   /* Settled, so the row leaves the loaded list without a refetch. */
   SDL_LockMutex(g_session.lock);
   cobalt_chat_join_requests_remove(&g_session.join_requests, in->member_did);
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_chat_join_approve(const cobalt_job_input *in, cobalt_job_result *r,
                                     cobalt_auth_state *state)
{
   cobalt_session_run_join_decision(in, r, state, true);
}

void
cobalt_session_run_chat_join_reject(const cobalt_job_input *in, cobalt_job_result *r,
                                    cobalt_auth_state *state)
{
   cobalt_session_run_join_decision(in, r, state, false);
}

#endif /* COBALT_HAS_WOLFRAM */

bool
cobalt_session_begin_chat_accept(const char *convo_id)
{
   char id[COBALT_CHAT_ID_MAX];
   if (!cobalt_chat_convo_id_copy(id, sizeof(id), convo_id)) {
      return false;
   }
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", id);
   return cobalt_session_submit(COBALT_JOB_CHAT_ACCEPT, &in);
}

bool
cobalt_session_begin_chat_decline(const char *convo_id)
{
   char id[COBALT_CHAT_ID_MAX];
   if (!cobalt_chat_convo_id_copy(id, sizeof(id), convo_id)) {
      return false;
   }
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", id);
   return cobalt_session_submit(COBALT_JOB_CHAT_DECLINE, &in);
}

bool
cobalt_session_begin_chat_join_requests(const char *convo_id, bool paging)
{
   char id[COBALT_CHAT_ID_MAX];
   if (!cobalt_chat_convo_id_copy(id, sizeof(id), convo_id)) {
      return false;
   }
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", id);
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_CHAT_JOIN_REQUESTS, &in);
}

const cobalt_chat_join_requests *
cobalt_session_chat_join_requests(void)
{
   return &g_session.join_requests;
}

/* Both decisions name the group and the requester, so they share the guard. */
static bool
begin_join_decision(const char *convo_id, const char *did, bool approve)
{
   char id[COBALT_CHAT_ID_MAX];
   if (!cobalt_chat_convo_id_copy(id, sizeof(id), convo_id) ||
       cobalt_chat_target_kind_of(did) != COBALT_CHAT_TARGET_DID) {
      return false;
   }
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", id);
   snprintf(in.member_did, sizeof(in.member_did), "%s", did);
   return cobalt_session_submit(approve ? COBALT_JOB_CHAT_JOIN_APPROVE
                                        : COBALT_JOB_CHAT_JOIN_REJECT, &in);
}

bool
cobalt_session_begin_chat_join_approve(const char *convo_id, const char *did)
{
   return begin_join_decision(convo_id, did, true);
}

bool
cobalt_session_begin_chat_join_reject(const char *convo_id, const char *did)
{
   return begin_join_decision(convo_id, did, false);
}

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
cobalt_session_begin_chat_inbox(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_CHAT_INBOX, &in);
}

const cobalt_chat_inbox *
cobalt_session_chat_inbox(void)
{
   return &g_session.chat;
}

bool
cobalt_session_begin_chat_requests(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_CHAT_REQUESTS, &in);
}

const cobalt_chat_inbox *
cobalt_session_chat_requests(void)
{
   return &g_session.requests;
}

bool
cobalt_session_begin_chat_send(const char *convo_id, const char *text)
{
   if (!convo_id || !convo_id[0] || !cobalt_chat_reply_valid(text)) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   snprintf(in.text, sizeof(in.text), "%s", text);
   return cobalt_session_submit(COBALT_JOB_CHAT_SEND, &in);
}

bool
cobalt_session_begin_chat_messages(const char *convo_id)
{
   if (!convo_id || !convo_id[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   return cobalt_session_submit(COBALT_JOB_CHAT_MESSAGES, &in);
}

const cobalt_chat_thread *
cobalt_session_chat_thread(void)
{
   return &g_session.chat_thread;
}

bool
cobalt_session_begin_chat_start(const char *handle_or_did)
{
   char target[COBALT_CHAT_TARGET_MAX];
   if (!cobalt_chat_target_normalise(handle_or_did, target, sizeof(target)) ||
       cobalt_chat_target_kind_of(target) == COBALT_CHAT_TARGET_INVALID) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", target);
   return cobalt_session_submit(COBALT_JOB_CHAT_START, &in);
}

const char *
cobalt_session_chat_started_convo(void)
{
   return g_session.chat_started;
}

bool
cobalt_session_begin_chat_group_info(const char *convo_id)
{
   if (!convo_id || !convo_id[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   return cobalt_session_submit(COBALT_JOB_CHAT_GROUP_INFO, &in);
}

const cobalt_chat_group *
cobalt_session_chat_group(void)
{
   return &g_session.group;
}

bool
cobalt_session_begin_chat_group_create(const char *name, const char *members)
{
   if (!name || !name[0] || !members || !members[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.name, sizeof(in.name), "%s", name);
   snprintf(in.text, sizeof(in.text), "%s", members);
   return cobalt_session_submit(COBALT_JOB_CHAT_GROUP_CREATE, &in);
}

const char *
cobalt_session_chat_group_started(void)
{
   return g_session.chat_group_started;
}

bool
cobalt_session_begin_chat_group_rename(const char *convo_id, const char *name)
{
   if (!convo_id || !convo_id[0] || !name || !name[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   snprintf(in.text, sizeof(in.text), "%s", name);
   return cobalt_session_submit(COBALT_JOB_CHAT_GROUP_RENAME, &in);
}

bool
cobalt_session_begin_chat_group_add(const char *convo_id, const char *members)
{
   if (!convo_id || !convo_id[0] || !members || !members[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   snprintf(in.text, sizeof(in.text), "%s", members);
   return cobalt_session_submit(COBALT_JOB_CHAT_GROUP_ADD, &in);
}

bool
cobalt_session_begin_chat_group_remove(const char *convo_id, const char *did)
{
   if (!convo_id || !convo_id[0] || !did || !did[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   snprintf(in.member_did, sizeof(in.member_did), "%s", did);
   return cobalt_session_submit(COBALT_JOB_CHAT_GROUP_REMOVE, &in);
}

bool
cobalt_session_begin_chat_join_link_create(const char *convo_id)
{
   if (!convo_id || !convo_id[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   in.link_create = true;
   return cobalt_session_submit(COBALT_JOB_CHAT_JOIN_LINK, &in);
}

bool
cobalt_session_begin_chat_join_link_toggle(const char *convo_id, bool disable)
{
   if (!convo_id || !convo_id[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", convo_id);
   in.link_disable = disable;
   return cobalt_session_submit(COBALT_JOB_CHAT_JOIN_LINK, &in);
}

/* How many thread requests were accepted, so a test can see a conversation was
 * fetched again without depending on when the network answers. */
static int g_thread_requests;

bool
cobalt_session_begin_thread(const char *uri)
{
   if (!uri || !uri[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", uri);
   if (!cobalt_session_submit(COBALT_JOB_THREAD, &in)) {
      return false;
   }
   g_thread_requests++;
   return true;
}

int
cobalt_session_thread_requests(void)
{
   return g_thread_requests;
}
