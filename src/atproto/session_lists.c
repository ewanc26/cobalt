/* People lists and curated lists: followers, following, likes, muted and blocked accounts, actor search, lists and saved feeds, and the calls that start them. */

#include "atproto/session_internal.h"

#ifdef COBALT_HAS_WOLFRAM

/* Shared by run_muted_list/run_blocked_list — the two calls are identical
 * apart from which Wolfram wrapper to call and which list to fill. */
static void
run_actor_list(const cobalt_job_input *in, cobalt_job_result *r,
               cobalt_auth_state *state, cobalt_actor_list *list,
               wf_status (*fetch)(wf_agent *, const char *, int, const char *,
                                  wf_agent_actor_list *),
               const char *empty_message)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = list->cursor[0] ? list->cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_actor_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      fetch(g_session.wf, in->uri[0] ? in->uri : NULL, COBALT_SESSION_PAGE, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: actor list fetch failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not load the list (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(list);
   }
   const int added = cobalt_actor_list_append_from_wolfram(list, &wf_list);
   if (in->paging && added == 0) {
      list->has_more = false;
      list->cursor[0] = '\0';
   }
   const int total = list->count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_actor_list_free(&wf_list);
   COBALT_LOGI("session: actor list +%d (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "%s", empty_message);
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

static wf_status
fetch_mutes(wf_agent *agent, const char *actor, int limit, const char *cursor,
            wf_agent_actor_list *out)
{
   (void) actor;
   return wf_agent_get_mutes_typed(agent, limit, cursor, out);
}

static wf_status
fetch_blocks(wf_agent *agent, const char *actor, int limit, const char *cursor,
             wf_agent_actor_list *out)
{
   (void) actor;
   return wf_agent_get_blocks_typed(agent, limit, cursor, out);
}

void
cobalt_session_run_muted_list(const cobalt_job_input *in, cobalt_job_result *r,
               cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &g_session.muted, fetch_mutes,
                  "No muted accounts.");
}

void
cobalt_session_run_blocked_list(const cobalt_job_input *in, cobalt_job_result *r,
                 cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &g_session.blocked, fetch_blocks,
                  "No blocked accounts.");
}

void
cobalt_session_run_followers(const cobalt_job_input *in, cobalt_job_result *r,
              cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &g_session.followers, wf_agent_get_followers_typed,
                  "No followers yet.");
}

void
cobalt_session_run_following(const cobalt_job_input *in, cobalt_job_result *r,
              cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &g_session.following, wf_agent_get_follows_typed,
                  "Not following anyone.");
}

/* getRepostedBy is the same shape as the followers fetches — a plain actor
 * list keyed by a URI instead of a DID — so it rides run_actor_list. */
void
cobalt_session_run_reposted_by(const cobalt_job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &g_session.likes, wf_agent_get_reposted_by_typed,
                  "No reposts yet.");
}

/* getLikes is not: Wolfram hands back wf_agent_actor_like_list, whose rows
 * carry the actor one level down, so it gets its own run rather than a
 * fetch_fn that would have to lie about the output type. */
void
cobalt_session_run_likes(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.likes.cursor[0] ? g_session.likes.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_actor_like_list wf_likes;
   memset(&wf_likes, 0, sizeof(wf_likes));

   const wf_status status =
      wf_agent_get_likes_typed(g_session.wf, in->uri, COBALT_SESSION_PAGE, cursor, &wf_likes);
   if (status != WF_OK) {
      COBALT_LOGW("session: getLikes failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not load the likes (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(&g_session.likes);
   }
   int added = 0;
   for (size_t i = 0; i < wf_likes.like_count; i++) {
      if (g_session.likes.count >= COBALT_ACTORS_MAX) {
         COBALT_LOGI("session: likes window full at %d, dropping the rest",
                      g_session.likes.count);
         break;
      }
      const wf_agent_profile_view *src = &wf_likes.likes[i].actor;
      cobalt_actor *out = &g_session.likes.actors[g_session.likes.count];
      memset(out, 0, sizeof(*out));

      snprintf(out->did, sizeof(out->did), "%s",
               src->did ? src->did : "");
      snprintf(out->handle, sizeof(out->handle), "@%s",
               src->handle ? src->handle : "");
      const char *display = src->display_name;
      if (!display || display[0] == '\0') {
         display = src->handle ? src->handle : "";
      }
      cobalt_feed_copy_text(out->display_name, sizeof(out->display_name),
                            display);
      snprintf(out->avatar, sizeof(out->avatar), "%s",
               src->avatar ? src->avatar : "");
      g_session.likes.count++;
      added++;
   }
   if (g_session.likes.count >= COBALT_ACTORS_MAX) {
      g_session.likes.cursor[0] = '\0';
      g_session.likes.has_more = false;
   } else if (wf_likes.cursor && wf_likes.cursor[0]) {
      snprintf(g_session.likes.cursor, sizeof(g_session.likes.cursor), "%s", wf_likes.cursor);
      g_session.likes.has_more = true;
   } else {
      g_session.likes.cursor[0] = '\0';
      g_session.likes.has_more = false;
   }
   const int total = g_session.likes.count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_actor_like_list_free(&wf_likes);
   COBALT_LOGI("session: likes +%d (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "No likes yet.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_search_actors(const cobalt_job_input *in, cobalt_job_result *r,
                  cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   if (!in->paging && in->text[0] == '\0') {
      SDL_LockMutex(g_session.lock);
      cobalt_actor_list_reset(&g_session.search);
      g_session.search.has_more = false;
      g_session.search.cursor[0] = '\0';
      SDL_UnlockMutex(g_session.lock);
      *state = COBALT_AUTH_SIGNED_IN;
      r->ok = true;
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.search.cursor[0] ? g_session.search.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_actor_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_search_actors_typed(g_session.wf, in->text, COBALT_SESSION_PAGE, cursor,
                                   &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: actor search failed (%d)", (int) status);
      cobalt_session_set_message(r, "Search failed (wolfram status %d).", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(&g_session.search);
   }
   const int added = cobalt_actor_list_append_from_wolfram(&g_session.search, &wf_list);
   if (in->paging && added == 0) {
      g_session.search.has_more = false;
      g_session.search.cursor[0] = '\0';
   }
   const int total = g_session.search.count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_actor_list_free(&wf_list);
   COBALT_LOGI("session: search '%s' +%d (%d held)", in->text, added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "No accounts found for \"%s\".", in->text);
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* The signed-in account's own lists (app.bsky.graph.getLists). */
void
cobalt_session_run_lists(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.lists.cursor[0] ? g_session.lists.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_list_view_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_get_lists_typed(g_session.wf, g_session.did, COBALT_SESSION_PAGE, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: lists fetch failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not load your lists (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_list_summary_list_reset(&g_session.lists);
   }
   const int added =
      cobalt_list_summary_list_append_from_wolfram(&g_session.lists, &wf_list);
   if (in->paging && added == 0) {
      g_session.lists.has_more = false;
      g_session.lists.cursor[0] = '\0';
   }
   const int total = g_session.lists.count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_list_view_list_free(&wf_list);
   COBALT_LOGI("session: lists +%d (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "You haven't made any lists yet.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/*
 * The account's saved custom feeds, with display names, from Wolfram. A name it
 * cannot resolve falls back to the URI's record key, so a feed is never
 * silently dropped from the picker.
 */
void
cobalt_session_run_saved_feeds(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   (void) in;
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   wf_saved_feed feeds[COBALT_SAVED_FEEDS_MAX];
   size_t n = 0;
   const wf_status status = wf_agent_get_saved_feeds(g_session.wf, feeds, COBALT_SAVED_FEEDS_MAX, &n);
   if (status != WF_OK) {
      COBALT_LOGW("session: saved feeds failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not load your saved feeds (wolfram status %d).", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   memset(&g_session.saved_feeds, 0, sizeof(g_session.saved_feeds));
   for (size_t i = 0; i < n; i++) {
      const int k = g_session.saved_feeds.count++;
      snprintf(g_session.saved_feeds.feeds[k].label, sizeof(g_session.saved_feeds.feeds[k].label), "%s", feeds[i].name);
      snprintf(g_session.saved_feeds.feeds[k].uri, sizeof(g_session.saved_feeds.feeds[k].uri), "%s", feeds[i].uri);
   }
   const int total = g_session.saved_feeds.count;
   SDL_UnlockMutex(g_session.lock);

   COBALT_LOGI("session: %d saved feeds", total);
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* One list's members (app.bsky.graph.getList). `in->uri` is the list's AT
 * URI, set by cobalt_session_begin_list_members. */
void
cobalt_session_run_list_members(const cobalt_job_input *in, cobalt_job_result *r,
                 cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.list_members.cursor[0] ? g_session.list_members.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_list_item_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_get_list_typed(g_session.wf, in->uri, COBALT_SESSION_PAGE, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: list members fetch failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not load that list (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(&g_session.list_members);
      memset(g_session.list_member_uris, 0, sizeof(g_session.list_member_uris));
   }
   const int first = g_session.list_members.count;
   const int added = cobalt_actor_list_append_from_wolfram_list_items(
      &g_session.list_members, &wf_list);
   for (int i = 0; i < added; i++) {
      snprintf(g_session.list_member_uris[first + i],
               sizeof(g_session.list_member_uris[first + i]), "%s",
               wf_list.items[i].uri ? wf_list.items[i].uri : "");
   }
   if (in->paging && added == 0) {
      g_session.list_members.has_more = false;
      g_session.list_members.cursor[0] = '\0';
   }
   const int total = g_session.list_members.count;
   SDL_UnlockMutex(g_session.lock);

   wf_agent_list_item_list_free(&wf_list);
   COBALT_LOGI("session: list members +%d (%d held)", added, total);

   if (total == 0) {
      cobalt_session_set_message(r, "This list has no members.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* The record key at the end of at://did/app.bsky.graph.list/rkey. */
static bool
list_rkey(const char *uri, char *out, size_t cap)
{
   const char *slash;

   if (!uri || !out || cap == 0) {
      return false;
   }
   slash = strrchr(uri, '/');
   if (!slash || !slash[1]) {
      return false;
   }
   snprintf(out, cap, "%s", slash + 1);
   return true;
}

void
cobalt_session_run_list_create(const cobalt_job_input *in, cobalt_job_result *r,
                                cobalt_auth_state *state)
{
   wf_agent_post_result created;
   wf_status status;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }
   if (!in->list_name[0]) {
      cobalt_session_set_message(r, "A list needs a name.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   memset(&created, 0, sizeof(created));
   status = wf_agent_graph_create_list(g_session.wf, in->list_purpose,
                                       in->list_name,
                                       in->list_description[0]
                                          ? in->list_description : NULL,
                                       &created);
   if (status != WF_OK) {
      COBALT_LOGW("session: create list failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not create the list (wolfram status %d).",
                                 (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   wf_agent_post_result_free(&created);
   cobalt_session_set_message(r, "List created.");
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_list_update(const cobalt_job_input *in, cobalt_job_result *r,
                               cobalt_auth_state *state)
{
   wf_repo_record record;
   wf_agent_post_result updated;
   wf_status status;
   char rkey[COBALT_POST_URI_MAX];
   char *json;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }
   if (!in->list_name[0] || !list_rkey(in->uri, rkey, sizeof(rkey))) {
      cobalt_session_set_message(r, "That list has no name or no record key.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   memset(&record, 0, sizeof(record));
   status = wf_agent_get_record_typed(g_session.wf, g_session.did,
                                      "app.bsky.graph.list", rkey, NULL,
                                      &record);
   if (status != WF_OK || !record.value || !cJSON_IsObject(record.value)) {
      COBALT_LOGW("session: list record fetch failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not read that list (wolfram status %d).",
                                 (int) status);
      wf_repo_record_free(&record);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   cJSON_DeleteItemFromObjectCaseSensitive(record.value, "name");
   cJSON_DeleteItemFromObjectCaseSensitive(record.value, "description");
   cJSON_DeleteItemFromObjectCaseSensitive(record.value, "purpose");
   cJSON_AddStringToObject(record.value, "name", in->list_name);
   if (in->list_description[0]) {
      cJSON_AddStringToObject(record.value, "description",
                              in->list_description);
   }
   cJSON_AddStringToObject(record.value, "purpose", in->list_purpose);
   json = cJSON_PrintUnformatted(record.value);
   if (!json) {
      cobalt_session_set_message(r, "Could not build the updated list record.");
      wf_repo_record_free(&record);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   memset(&updated, 0, sizeof(updated));
   status = wf_agent_graph_update_list(g_session.wf, rkey, json, &updated);
   cJSON_free(json);
   wf_repo_record_free(&record);
   if (status != WF_OK) {
      COBALT_LOGW("session: update list failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not save the list (wolfram status %d).",
                                 (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   wf_agent_post_result_free(&updated);

   SDL_LockMutex(g_session.lock);
   for (int i = 0; i < g_session.lists.count; i++) {
      if (strcmp(g_session.lists.lists[i].uri, in->uri) == 0) {
         snprintf(g_session.lists.lists[i].name,
                  sizeof(g_session.lists.lists[i].name), "%s", in->list_name);
         snprintf(g_session.lists.lists[i].description,
                  sizeof(g_session.lists.lists[i].description), "%s",
                  in->list_description);
         snprintf(g_session.lists.lists[i].purpose,
                  sizeof(g_session.lists.lists[i].purpose), "%s",
                  in->list_purpose);
         break;
      }
   }
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_set_message(r, "List updated.");
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_list_add_member(const cobalt_job_input *in,
                                    cobalt_job_result *r,
                                    cobalt_auth_state *state)
{
   wf_agent_post_result item;
   wf_status status;
   char subject[COBALT_DID_MAX];
   char *did = NULL;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }
   if (!in->uri[0] || !in->member_did[0]) {
      cobalt_session_set_message(r, "Choose a list and an account first.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   snprintf(subject, sizeof(subject), "%s", in->member_did);
   if (subject[0] == '@') {
      memmove(subject, subject + 1, strlen(subject));
   }
   if (strncmp(subject, "did:", 4) == 0) {
      did = subject;
   } else {
      char *resolved = NULL;
      status = wf_agent_resolve_handle(g_session.wf, subject, &resolved);
      if (status != WF_OK || !resolved || !resolved[0]) {
         cobalt_session_set_message(r, "Could not find @%s.", subject);
         free(resolved);
         *state = COBALT_AUTH_SIGNED_IN;
         return;
      }
      snprintf(subject, sizeof(subject), "%s", resolved);
      free(resolved);
      did = subject;
   }

   memset(&item, 0, sizeof(item));
   status = wf_agent_graph_create_list_item(g_session.wf, in->uri, did, &item);
   if (status != WF_OK) {
      COBALT_LOGW("session: add list member failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not add that account (wolfram status %d).",
                                 (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   wf_agent_post_result_free(&item);
   cobalt_session_set_message(r, "Added to the list.");
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_list_remove_member(const cobalt_job_input *in,
                                       cobalt_job_result *r,
                                       cobalt_auth_state *state)
{
   wf_status status;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }
   if (!in->list_item_uri[0]) {
      cobalt_session_set_message(r, "That member has no record to remove.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   status = wf_agent_graph_delete_list_item(g_session.wf, in->list_item_uri);
   if (status != WF_OK) {
      COBALT_LOGW("session: remove list member failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not remove that account (wolfram status %d).",
                                 (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   cobalt_session_set_message(r, "Removed from the list.");
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* A starter pack is the pack record plus its underlying list's members. The
 * fresh call fetches both; a paging call only asks the list for its next page.
 * The pack and member list are separate snapshots so a failed member page can
 * leave the header on screen with a recoverable message. */
void
cobalt_session_run_starter_pack(const cobalt_job_input *in, cobalt_job_result *r,
                                cobalt_auth_state *state)
{
   char list_uri[COBALT_POST_URI_MAX];
   const char *cursor = NULL;

   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }
   if (!in->paging && (!in->uri[0] ||
       !cobalt_starter_pack_uri_from_link(in->uri, list_uri, sizeof(list_uri)))) {
      cobalt_session_set_message(r, "That is not a starter-pack link.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   if (!in->paging) {
      wf_graph_starter_pack_view view;
      wf_status status;

      memset(&view, 0, sizeof(view));
      status = wf_agent_get_starter_pack_typed(g_session.wf, in->uri, &view);
      if (status != WF_OK) {
         COBALT_LOGW("session: starter pack fetch failed (%d)", (int) status);
         SDL_LockMutex(g_session.lock);
         cobalt_starter_pack_reset(&g_session.starter_pack);
         cobalt_actor_list_reset(&g_session.starter_pack_members);
         SDL_UnlockMutex(g_session.lock);
         cobalt_session_set_message(r, "Could not load that starter pack (wolfram status %d).",
                                    (int) status);
         *state = COBALT_AUTH_SIGNED_IN;
         return;
      }

      SDL_LockMutex(g_session.lock);
      cobalt_starter_pack_reset(&g_session.starter_pack);
      cobalt_actor_list_reset(&g_session.starter_pack_members);
      cobalt_starter_pack_fill_from_wolfram(&g_session.starter_pack, &view);
      snprintf(list_uri, sizeof(list_uri), "%s",
               g_session.starter_pack.list_uri);
      SDL_UnlockMutex(g_session.lock);
      wf_graph_starter_pack_view_free(&view);
   } else {
      SDL_LockMutex(g_session.lock);
      snprintf(list_uri, sizeof(list_uri), "%s", g_session.starter_pack.list_uri);
      cursor = g_session.starter_pack_members.cursor[0]
                  ? g_session.starter_pack_members.cursor
                  : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!list_uri[0] || !cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   if (list_uri[0]) {
      wf_agent_list_item_list members;
      wf_status status;

      memset(&members, 0, sizeof(members));
      status = wf_agent_get_list_typed(g_session.wf, list_uri, COBALT_SESSION_PAGE,
                                       cursor, &members);
      if (status != WF_OK) {
         COBALT_LOGW("session: starter-pack members failed (%d)", (int) status);
         cobalt_session_set_message(r, "Could not load this pack's members (wolfram status %d).",
                                    (int) status);
      } else {
         SDL_LockMutex(g_session.lock);
         if (!in->paging) {
            cobalt_actor_list_reset(&g_session.starter_pack_members);
         }
         const int added = cobalt_actor_list_append_from_wolfram_list_items(
            &g_session.starter_pack_members, &members);
         if (in->paging && added == 0) {
            g_session.starter_pack_members.has_more = false;
            g_session.starter_pack_members.cursor[0] = '\0';
         }
         const int total = g_session.starter_pack_members.count;
         SDL_UnlockMutex(g_session.lock);
         wf_agent_list_item_list_free(&members);
         if (total == 0 && !in->paging) {
            cobalt_session_set_message(r, "This starter pack has no available members.");
         }
      }
   }

   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

#endif /* COBALT_HAS_WOLFRAM */

const cobalt_actor_list *
cobalt_session_muted_list(void)
{
   return &g_session.muted;
}

const cobalt_actor_list *
cobalt_session_blocked_list(void)
{
   return &g_session.blocked;
}

bool
cobalt_session_begin_muted_list(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_MUTED_LIST, &in);
}

static bool
begin_follow_list(cobalt_job_kind kind, cobalt_actor_list *list,
                  const char *actor, bool paging)
{
   if (!actor || actor[0] == '\0') {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   snprintf(in.uri, sizeof(in.uri), "%s", actor);

   if (!paging) {
      SDL_LockMutex(g_session.lock);
      cobalt_actor_list_reset(list);
      snprintf(g_session.follow_actor, sizeof(g_session.follow_actor), "%s", actor);
      SDL_UnlockMutex(g_session.lock);
   }
   return cobalt_session_submit(kind, &in);
}

/* Likes/reposted-by: same shape as begin_follow_list but keyed by a post
 * URI, and the two kinds share one list so there is no list argument. */
static bool
begin_likes_list(cobalt_job_kind kind, const char *uri, bool paging)
{
   if (!uri || uri[0] == '\0') {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   snprintf(in.uri, sizeof(in.uri), "%s", uri);

   if (!paging) {
      SDL_LockMutex(g_session.lock);
      cobalt_actor_list_reset(&g_session.likes);
      snprintf(g_session.likes_uri, sizeof(g_session.likes_uri), "%s", uri);
      SDL_UnlockMutex(g_session.lock);
   }
   return cobalt_session_submit(kind, &in);
}

bool
cobalt_session_begin_followers(const char *actor, bool paging)
{
   return begin_follow_list(COBALT_JOB_FOLLOWERS, &g_session.followers, actor, paging);
}

bool
cobalt_session_begin_following(const char *actor, bool paging)
{
   return begin_follow_list(COBALT_JOB_FOLLOWING, &g_session.following, actor, paging);
}

const cobalt_actor_list *
cobalt_session_followers_list(void)
{
   return &g_session.followers;
}

const cobalt_actor_list *
cobalt_session_following_list(void)
{
   return &g_session.following;
}

bool
cobalt_session_begin_likes(const char *uri, bool paging)
{
   return begin_likes_list(COBALT_JOB_LIKES, uri, paging);
}

bool
cobalt_session_begin_reposted_by(const char *uri, bool paging)
{
   return begin_likes_list(COBALT_JOB_REPOSTED_BY, uri, paging);
}

const cobalt_actor_list *
cobalt_session_likes_list(void)
{
   return &g_session.likes;
}

const char *
cobalt_session_follow_list_actor(void)
{
   return g_session.follow_actor;
}

bool
cobalt_session_begin_blocked_list(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_BLOCKED_LIST, &in);
}

const cobalt_actor_list *
cobalt_session_search_results(void)
{
   return &g_session.search;
}

bool
cobalt_session_begin_search_actors(const char *query, bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   if (query) {
      snprintf(in.text, sizeof(in.text), "%s", query);
   }
   return cobalt_session_submit(COBALT_JOB_SEARCH_ACTORS, &in);
}

const cobalt_list_summary_list *
cobalt_session_lists(void)
{
   return &g_session.lists;
}

const cobalt_saved_feeds *
cobalt_session_saved_feeds(void)
{
   return &g_session.saved_feeds;
}

bool
cobalt_session_begin_saved_feeds(void)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   return cobalt_session_submit(COBALT_JOB_SAVED_FEEDS, &in);
}

bool
cobalt_session_begin_lists(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_LISTS, &in);
}

const cobalt_actor_list *
cobalt_session_list_members(void)
{
   return &g_session.list_members;
}

bool
cobalt_session_begin_list_members(const char *list_uri, bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", list_uri ? list_uri : "");
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_LIST_MEMBERS, &in);
}

const char *
cobalt_session_list_member_uri(int index)
{
   if (index < 0 || index >= g_session.list_members.count) {
      return "";
   }
   return g_session.list_member_uris[index];
}

bool
cobalt_session_begin_list_create(const char *name, const char *description,
                                  const char *purpose)
{
   cobalt_job_input in;

   if (!name || !name[0]) {
      return false;
   }
   memset(&in, 0, sizeof(in));
   snprintf(in.list_name, sizeof(in.list_name), "%s", name);
   snprintf(in.list_description, sizeof(in.list_description), "%s",
            description ? description : "");
   snprintf(in.list_purpose, sizeof(in.list_purpose), "%s",
            purpose && purpose[0] ? purpose : COBALT_LIST_PURPOSE_CURATE);
   return cobalt_session_submit(COBALT_JOB_LIST_CREATE, &in);
}

bool
cobalt_session_begin_list_update(const char *list_uri, const char *name,
                                  const char *description, const char *purpose)
{
   cobalt_job_input in;

   if (!list_uri || !list_uri[0] || !name || !name[0]) {
      return false;
   }
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", list_uri);
   snprintf(in.list_name, sizeof(in.list_name), "%s", name);
   snprintf(in.list_description, sizeof(in.list_description), "%s",
            description ? description : "");
   snprintf(in.list_purpose, sizeof(in.list_purpose), "%s",
            purpose && purpose[0] ? purpose : COBALT_LIST_PURPOSE_CURATE);
   return cobalt_session_submit(COBALT_JOB_LIST_UPDATE, &in);
}

bool
cobalt_session_begin_list_add_member(const char *list_uri, const char *subject)
{
   cobalt_job_input in;

   if (!list_uri || !list_uri[0] || !subject || !subject[0]) {
      return false;
   }
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", list_uri);
   snprintf(in.member_did, sizeof(in.member_did), "%s", subject);
   return cobalt_session_submit(COBALT_JOB_LIST_ADD_MEMBER, &in);
}

bool
cobalt_session_begin_list_remove_member(const char *list_item_uri)
{
   cobalt_job_input in;

   if (!list_item_uri || !list_item_uri[0]) {
      return false;
   }
   memset(&in, 0, sizeof(in));
   snprintf(in.list_item_uri, sizeof(in.list_item_uri), "%s", list_item_uri);
   return cobalt_session_submit(COBALT_JOB_LIST_REMOVE_MEMBER, &in);
}

const cobalt_starter_pack *
cobalt_session_starter_pack(void)
{
   return &g_session.starter_pack;
}

const cobalt_actor_list *
cobalt_session_starter_pack_members(void)
{
   return &g_session.starter_pack_members;
}

bool
cobalt_session_begin_starter_pack(const char *pack_uri, bool paging)
{
   cobalt_job_input in;

   if (!pack_uri || !pack_uri[0]) {
      return false;
   }
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", pack_uri);
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_STARTER_PACK, &in);
}

bool
cobalt_session_begin_unmute_actor(const char *did)
{
   if (!did || !did[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", did);
   in.flag = false;   /* always undo — every row on this list is muted */
   return cobalt_session_submit(COBALT_JOB_MUTE, &in);
}

bool
cobalt_session_begin_unblock_actor(const char *record_uri, const char *did)
{
   if (!record_uri || !record_uri[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.record_uri, sizeof(in.record_uri), "%s", record_uri);
   /* Not used to decide undo (record_uri already does that) — carried so
    * run_block can drop the right row from s.blocked locally. */
   snprintf(in.uri, sizeof(in.uri), "%s", did ? did : "");
   return cobalt_session_submit(COBALT_JOB_BLOCK, &in);
}
