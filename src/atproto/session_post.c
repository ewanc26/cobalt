/* Writing: posts, quotes, threads, likes, reposts and deleting, and the calls that start them. */

#include "atproto/session_internal.h"

#ifdef COBALT_HAS_WOLFRAM

/*
 * Like and repost share everything but two function calls, so they share an
 * implementation. `undo` is decided by the caller from the post's viewer
 * state; passing the record URI in rather than looking it up again keeps the
 * worker from touching the feed before it has to.
 */
void
cobalt_session_run_interaction(const cobalt_job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state, bool is_like)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const bool undo = in->record_uri[0] != '\0';
   const char *what = is_like ? "like" : "repost";
   wf_status status;
   char created[COBALT_POST_URI_MAX] = "";

   if (undo) {
      COBALT_LOGI("session: removing %s %s", what, in->record_uri);
      status = is_like ? wf_agent_unlike(g_session.wf, in->record_uri)
                       : wf_agent_delete_repost(g_session.wf, in->record_uri);
   } else {
      wf_agent_post_result result;
      memset(&result, 0, sizeof(result));

      COBALT_LOGI("session: %s %s", what, in->uri);
      status = is_like ? wf_agent_like(g_session.wf, in->uri, in->cid, &result)
                       : wf_agent_repost(g_session.wf, in->uri, in->cid, &result);

      if (status == WF_OK) {
         snprintf(created, sizeof(created), "%s", result.uri ? result.uri : "");
      }
      wf_agent_post_result_free(&result);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: %s failed (%d)", what, (int) status);
      /* Named so the message says which action failed — "the request failed"
       * on a screen with two buttons is not much help. */
      cobalt_session_set_message(r, "Could not %s that post (wolfram status %d).",
                  undo ? (is_like ? "remove the like from" : "un-repost")
                       : (is_like ? "like" : "repost"),
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   /*
    * Reflect it locally rather than refetching the whole feed for one
    * changed number. The next refresh reconciles with the server.
    */
   /*
    * Reflect it locally rather than refetching the whole feed for one changed
    * number. Both the feed and the loaded thread are updated, since the same
    * post is frequently on screen in both at once. The next refresh
    * reconciles with the server.
    */
   const char *record = undo ? NULL : created;

   SDL_LockMutex(g_session.lock);
   if (is_like) {
      cobalt_feed_apply_like(&g_session.feed, in->uri, record);
      cobalt_thread_apply_like(&g_session.conversation, in->uri, record);
   } else {
      cobalt_feed_apply_repost(&g_session.feed, in->uri, record);
      cobalt_thread_apply_repost(&g_session.conversation, in->uri, record);
   }
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_delete_post(const cobalt_job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   COBALT_LOGI("session: deleting %s", in->uri);
   wf_status status = wf_agent_delete_post(g_session.wf, in->uri);
   if (status != WF_OK) {
      COBALT_LOGW("session: delete failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not delete that post (wolfram status %d). It is "
                     "still there.", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   cobalt_feed_remove_post(&g_session.feed, in->uri);
   cobalt_feed_remove_post(&g_session.author_feed, in->uri);
   for (int i = 0; i < g_session.conversation.count; i++) {
      if (strcmp(g_session.conversation.posts[i].uri, in->uri) == 0) {
         /* The tree's indents and focus no longer line up once a row is gone;
          * the screen returns to where it came from rather than drawing it. */
         cobalt_thread_reset(&g_session.conversation);
         break;
      }
   }
   SDL_UnlockMutex(g_session.lock);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/*
 * Read an image from the SD card, upload it and return an
 * app.bsky.embed.images object holding it (caller frees), or NULL.
 */
static cJSON *
upload_attachment(const char *path, const char *alt)
{
   cJSON *embed = NULL;
   const wf_status st = wf_agent_upload_image_file(g_session.wf, path, alt, &embed);
   if (st != WF_OK) {
      COBALT_LOGW("session: attachment %s failed (%d)", path, (int) st);
      return NULL;
   }
   return embed;
}

/* Reply-gate on a new top-level post (the rules are Wolfram's). A failure here
 * does not roll the post back — it exists, just ungated, which is the safer
 * failure than silently dropping it. `gate` is a cobalt_reply_gate: 0 none,
 * 1 followed/mentioned, 2 nobody. */
static void
apply_reply_gate(int gate, const char *post_uri)
{
   if (!post_uri || !post_uri[0]) {
      return;
   }
   const wf_status st = wf_agent_set_reply_gate(g_session.wf, post_uri, (wf_reply_gate) gate);
   if (st != WF_OK) {
      COBALT_LOGW("session: reply gate failed (%d) for %s", (int) st,
                  post_uri ? post_uri : "(no uri)");
   }
}

static void
run_post_thread(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const char *texts[COBALT_THREAD_POSTS_MAX];
   for (int i = 0; i < in->thread_count; i++) {
      texts[i] = g_session.thread_texts[i];
   }

   wf_agent_post_result first;
   memset(&first, 0, sizeof(first));
   size_t posted = 0;
   const wf_status status = wf_agent_post_thread(
      g_session.wf, texts, (size_t) in->thread_count, &posted, &first, NULL);

   COBALT_LOGI("session: thread of %d: %d posted (status %d)", in->thread_count,
               (int) posted, (int) status);
   apply_reply_gate(in->reply_gate, first.uri);
   wf_agent_post_result_free(&first);
   *state = COBALT_AUTH_SIGNED_IN;

   if (status == WF_OK) {
      cobalt_session_publish();
      r->ok = true;
      return;
   }
   if (posted == 0) {
      cobalt_session_set_message(r, "Could not publish that (wolfram status %d). Nothing was "
                     "posted.", (int) status);
      return;
   }
   /* Part of it is public. Say so, and do not leave the draft up to be sent
    * again: that would post the first ones twice. */
   cobalt_session_publish();
   cobalt_session_set_message(r, "Only %d of %d posts went out (wolfram status %d). The rest "
                  "were not sent.", (int) posted, in->thread_count, (int) status);
   r->ok = true;
   r->partial = true;
}

void
cobalt_session_run_post(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (in->thread_count > 1) {
      run_post_thread(in, r, state);
      return;
   }
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   wf_agent_post_result result;
   memset(&result, 0, sizeof(result));

   cJSON *images = NULL;
   if (in->attach_path[0]) {
      images = upload_attachment(in->attach_path, in->attach_alt);
      if (!images) {
         cobalt_session_set_message(r, "Could not upload that image. Nothing was posted.");
         *state = COBALT_AUTH_SIGNED_IN;
         return;
      }
   }

   wf_status status;
   if (in->quote) {
      COBALT_LOGI("session: quoting %s", in->uri);
      status = images ? wf_agent_quote_with_media(g_session.wf, in->text, in->uri,
                                                  in->cid, images, &result)
                      : wf_agent_quote(g_session.wf, in->text, in->uri, in->cid, &result);
   } else if (in->uri[0]) {
      /*
       * A reply. wf_agent_reply_refs rather than wf_agent_reply, because the
       * latter uses the parent as its own root — correct only when replying to
       * a top-level post, and silently wrong for a reply to a reply.
       */
      COBALT_LOGI("session: replying to %s (root %s)", in->uri, in->root_uri);
      char *embed_json = images ? cJSON_PrintUnformatted(images) : NULL;
      status = (images && !embed_json)
                  ? WF_ERR_ALLOC
                  : wf_agent_reply_refs_with_embed(
                       g_session.wf, in->text, in->root_uri, in->root_cid, in->uri,
                       in->cid, embed_json, &result);
      free(embed_json);
   } else if (images) {
      char *embed_json = cJSON_PrintUnformatted(images);
      cJSON_Delete(images);
      images = NULL;
      status = embed_json
                  ? wf_agent_post_with_embed(g_session.wf, in->text, embed_json, &result)
                  : WF_ERR_ALLOC;
      free(embed_json);
   } else {
      COBALT_LOGI("session: posting %d bytes", (int) strlen(in->text));
      status = wf_agent_post(g_session.wf, in->text, &result);
   }
   /* quote_with_media takes the embed by pointer but not ownership. */
   if (images) {
      cJSON_Delete(images);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: post failed (%d)", (int) status);
      cobalt_session_set_message(r, "Could not publish that (wolfram status %d). Nothing was "
                     "posted.", (int) status);
      wf_agent_post_result_free(&result);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   COBALT_LOGI("session: posted %s", result.uri ? result.uri : "(no uri)");

   apply_reply_gate(in->reply_gate, result.uri);

   wf_agent_post_result_free(&result);

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

#endif /* COBALT_HAS_WOLFRAM */

/*
 * Decide the direction here rather than making the caller do it: the viewer
 * state lives next to the post, so a screen that passed its own idea of
 * "liked" could disagree with what was last fetched and send the wrong verb.
 */
static bool
begin_interaction(cobalt_job_kind kind, const char *uri, const char *cid,
                  bool is_like)
{
   if (!uri || !uri[0] || !cid || !cid[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", uri);
   snprintf(in.cid, sizeof(in.cid), "%s", cid);

   /*
    * Find the post in whichever view holds it and read its viewer state. An
    * empty record URI means "not yet done", which the worker reads as
    * create-not-delete.
    *
    * `found` is tracked separately rather than testing record_uri for empty:
    * an unliked post in the feed also has an empty record, and falling through
    * to the thread on that would pick up a stale copy from before a refresh
    * and turn a like into an unlike of a record the server may have dropped.
    */
   bool found = false;
   SDL_LockMutex(g_session.lock);
   for (int i = 0; i < g_session.feed.count && !found; i++) {
      if (strcmp(g_session.feed.posts[i].uri, uri) == 0) {
         const char *record = is_like ? g_session.feed.posts[i].viewer_like
                                      : g_session.feed.posts[i].viewer_repost;
         snprintf(in.record_uri, sizeof(in.record_uri), "%s", record);
         found = true;
      }
   }
   for (int i = 0; i < g_session.conversation.count && !found; i++) {
      if (strcmp(g_session.conversation.posts[i].uri, uri) == 0) {
         const char *record = is_like ? g_session.conversation.posts[i].viewer_like
                                      : g_session.conversation.posts[i].viewer_repost;
         snprintf(in.record_uri, sizeof(in.record_uri), "%s", record);
         found = true;
      }
   }
   for (int i = 0; i < g_session.author_feed.count && !found; i++) {
      if (strcmp(g_session.author_feed.posts[i].uri, uri) == 0) {
         const char *record = is_like ? g_session.author_feed.posts[i].viewer_like
                                      : g_session.author_feed.posts[i].viewer_repost;
         snprintf(in.record_uri, sizeof(in.record_uri), "%s", record);
         found = true;
      }
   }
   SDL_UnlockMutex(g_session.lock);

   return cobalt_session_submit(kind, &in);
}

bool
cobalt_session_begin_delete_post(const char *uri)
{
   if (!uri || !cobalt_post_uri_is_by(uri, g_session.did)) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", uri);
   return cobalt_session_submit(COBALT_JOB_DELETE_POST, &in);
}

bool
cobalt_session_begin_post_thread(const char *const *texts, int count, int reply_gate)
{
   if (!texts || count < 2 || count > COBALT_THREAD_POSTS_MAX) {
      return false;
   }
   for (int i = 0; i < count; i++) {
      if (!texts[i] || !texts[i][0]) {
         return false;
      }
   }
   if (cobalt_session_busy()) {
      return false;
   }

   for (int i = 0; i < count; i++) {
      snprintf(g_session.thread_texts[i], sizeof(g_session.thread_texts[i]), "%s", texts[i]);
   }
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.thread_count = count;
   in.reply_gate = reply_gate;
   return cobalt_session_submit(COBALT_JOB_POST, &in);
}

bool
cobalt_session_begin_post(const char *text, const char *parent_uri,
                          const char *parent_cid, const char *root_uri,
                          const char *root_cid, int reply_gate,
                          const char *attach_path, const char *attach_alt)
{
   if (!text || !text[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.text, sizeof(in.text), "%s", text);
   if (attach_path) {
      snprintf(in.attach_path, sizeof(in.attach_path), "%s", attach_path);
   }
   if (attach_alt) {
      snprintf(in.attach_alt, sizeof(in.attach_alt), "%s", attach_alt);
   }

   const bool is_reply = parent_uri && parent_uri[0];
   in.reply_gate = is_reply ? 0 : reply_gate;
   if (is_reply) {
      /* All four refs or none. A reply missing its root is worse than a
       * refused request: it publishes into the wrong conversation. */
      if (!parent_cid || !parent_cid[0] || !root_uri || !root_uri[0] ||
          !root_cid || !root_cid[0]) {
         COBALT_LOGE("session: refusing a reply with incomplete refs");
         return false;
      }
      snprintf(in.uri, sizeof(in.uri), "%s", parent_uri);
      snprintf(in.cid, sizeof(in.cid), "%s", parent_cid);
      snprintf(in.root_uri, sizeof(in.root_uri), "%s", root_uri);
      snprintf(in.root_cid, sizeof(in.root_cid), "%s", root_cid);
   }

   return cobalt_session_submit(COBALT_JOB_POST, &in);
}

bool
cobalt_session_begin_quote(const char *text, const char *quote_uri,
                           const char *quote_cid, int reply_gate,
                           const char *attach_path, const char *attach_alt)
{
   if (!text || !text[0] || !quote_uri || !quote_uri[0] || !quote_cid ||
       !quote_cid[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.text, sizeof(in.text), "%s", text);
   snprintf(in.uri, sizeof(in.uri), "%s", quote_uri);
   snprintf(in.cid, sizeof(in.cid), "%s", quote_cid);
   in.quote = true;
   in.reply_gate = reply_gate;
   if (attach_path) {
      snprintf(in.attach_path, sizeof(in.attach_path), "%s", attach_path);
   }
   if (attach_alt) {
      snprintf(in.attach_alt, sizeof(in.attach_alt), "%s", attach_alt);
   }

   return cobalt_session_submit(COBALT_JOB_POST, &in);
}

bool
cobalt_session_begin_like(const char *uri, const char *cid)
{
   return begin_interaction(COBALT_JOB_LIKE, uri, cid, true);
}

bool
cobalt_session_begin_repost(const char *uri, const char *cid)
{
   return begin_interaction(COBALT_JOB_REPOST, uri, cid, false);
}
