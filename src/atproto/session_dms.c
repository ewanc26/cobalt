/* Direct messages: conversation list, message fetch and send, via chat.bsky.convo.*. */

#include "atproto/session_internal.h"

#ifdef COBALT_HAS_WOLFRAM

static const int DMS_LIMIT = 50;
static const int MSGS_LIMIT = 50;

void
cobalt_session_run_dms_convos(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.convos.cursor[0] ? g_session.convos.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_chat_convo_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_chat_list_convos(g_session.wf, DMS_LIMIT, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: dm convo list fetch failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_DMS_CONVOS);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const char *my_did = g_session.did;
   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_convos_reset(&g_session.convos);
   }
   cobalt_convos_append_from_wolfram(&g_session.convos, &wf_list, my_did);
   if (in->paging && g_session.convos.count == 0) {
      g_session.convos.has_more = false;
      g_session.convos.cursor[0] = '\0';
   }
   SDL_UnlockMutex(g_session.lock);

   wf_chat_convo_list_free(&wf_list);
   COBALT_LOGI("session: dm convos +%d", g_session.convos.count);

   if (g_session.convos.count == 0) {
      cobalt_session_set_message(r, "No conversations yet.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_dms_messages(const cobalt_job_input *in, cobalt_job_result *r,
                                cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(g_session.lock);
      cursor = g_session.messages.cursor[0] ? g_session.messages.cursor : NULL;
      SDL_UnlockMutex(g_session.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_chat_message_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_chat_get_messages(g_session.wf,
                                 in->convo_id[0] ? in->convo_id : NULL,
                                 MSGS_LIMIT, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: dm messages fetch failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_DMS_MESSAGES);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const char *my_did = g_session.did;
   SDL_LockMutex(g_session.lock);
   if (!in->paging) {
      cobalt_messages_reset(&g_session.messages);
      snprintf(g_session.messages.convo_id, sizeof g_session.messages.convo_id,
               "%s", in->convo_id);
   }
   cobalt_messages_append_from_wolfram(&g_session.messages, &wf_list, my_did);
   if (in->paging && g_session.messages.count == 0) {
      g_session.messages.has_more = false;
      g_session.messages.cursor[0] = '\0';
   }
   SDL_UnlockMutex(g_session.lock);

   wf_chat_message_list_free(&wf_list);
   COBALT_LOGI("session: dm messages +%d", g_session.messages.count);

   if (g_session.messages.count == 0) {
      cobalt_session_set_message(r, "No messages yet.");
   }

   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

void
cobalt_session_run_dms_send(const cobalt_job_input *in, cobalt_job_result *r,
                            cobalt_auth_state *state)
{
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }

   if (!in->convo_id[0] || !in->message_text[0]) {
      cobalt_session_set_message(r, "Empty message.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   wf_chat_message sent;
   memset(&sent, 0, sizeof(sent));

   const wf_status status =
      wf_agent_chat_send_message(g_session.wf, in->convo_id,
                                 in->message_text, NULL, &sent);
   if (status != WF_OK) {
      COBALT_LOGW("session: dm send failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_DMS_SEND);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(g_session.lock);
   snprintf(g_session.sent_message_id, sizeof g_session.sent_message_id,
             "%s", sent.id ? sent.id : "");
   SDL_UnlockMutex(g_session.lock);

   wf_chat_message_reset(&sent);

   cobalt_session_publish();
   COBALT_LOGI("session: dm sent");

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* --- begin accessors --- */

const cobalt_convos *
cobalt_session_dms_convos(void)
{
   return &g_session.convos;
}

const cobalt_messages *
cobalt_session_dms_messages(void)
{
   return &g_session.messages;
}

const char *
cobalt_session_dms_sent_id(void)
{
   return g_session.sent_message_id;
}

bool
cobalt_session_begin_dms_convos(bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return cobalt_session_submit(COBALT_JOB_DMS_CONVOS, &in);
}

bool
cobalt_session_begin_dms_messages(const char *convo_id, bool paging)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   if (convo_id) {
      snprintf(in.convo_id, sizeof in.convo_id, "%s", convo_id);
   }
   return cobalt_session_submit(COBALT_JOB_DMS_MESSAGES, &in);
}

bool
cobalt_session_begin_dms_send(const char *convo_id, const char *text)
{
   if (!convo_id || !convo_id[0] || !text) {
      return false;
   }
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.convo_id, sizeof in.convo_id, "%s", convo_id);
   cobalt_feed_copy_text(in.message_text, sizeof in.message_text, text);
   return cobalt_session_submit(COBALT_JOB_DMS_SEND, &in);
}

#else /* COBALT_HAS_WOLFRAM */

void
cobalt_session_run_dms_convos(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state)
{
   (void) in;
   (void) state;
   cobalt_session_set_message(r, "This build has no ATProto SDK. Build "
                  "Wolfram for Wii U alongside Cobalt — see the README.");
}

void
cobalt_session_run_dms_messages(const cobalt_job_input *in, cobalt_job_result *r,
                                cobalt_auth_state *state)
{
   (void) in;
   (void) state;
   cobalt_session_set_message(r, "This build has no ATProto SDK. Build "
                  "Wolfram for Wii U alongside Cobalt — see the README.");
}

void
cobalt_session_run_dms_send(const cobalt_job_input *in, cobalt_job_result *r,
                            cobalt_auth_state *state)
{
   (void) in;
   (void) state;
   cobalt_session_set_message(r, "This build has no ATProto SDK. Build "
                  "Wolfram for Wii U alongside Cobalt — see the README.");
}

const cobalt_convos *cobalt_session_dms_convos(void)   { return &g_session.convos; }
const cobalt_messages *cobalt_session_dms_messages(void) { return &g_session.messages; }
const char *cobalt_session_dms_sent_id(void) { return g_session.sent_message_id; }

bool cobalt_session_begin_dms_convos(bool paging) { (void) paging; return false; }
bool cobalt_session_begin_dms_messages(const char *convo_id, bool paging) { (void) convo_id; (void) paging; return false; }
bool cobalt_session_begin_dms_send(const char *convo_id, const char *text) { (void) convo_id; (void) text; return false; }
#endif /* COBALT_HAS_WOLFRAM */
