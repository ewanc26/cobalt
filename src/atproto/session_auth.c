/* Signing in, resuming and signing out: the runners that build and tear down the Wolfram agent, and the calls that start them. */

#include "atproto/session_internal.h"

#ifdef COBALT_HAS_WOLFRAM

void
cobalt_session_teardown_agent(void)
{
   if (g_session.wf) {
      wf_agent_free(g_session.wf);
      g_session.wf = NULL;
      g_session.prefs_loaded = false;
   }
}

/*
 * Create the agent and apply the settings every job needs.
 *
 * wf_agent rather than wf_session: the agent installs its own transparent
 * refresh-and-retry, re-points itself at the account's real PDS after login,
 * and carries the whole read/write surface — timeline, threads, likes, posting
 * — that this client is being built out towards. It only became usable here
 * once Wolfram grew wf_agent_set_ca_bundle and wf_agent_set_tls_rng, since
 * neither is reachable through an opaque agent and neither has a working
 * default on this console.
 */
static wf_agent *
new_wf_agent(const char *service)
{
   wf_agent *agent = wf_agent_new(service);
   if (!agent) {
      return NULL;
   }

   if (g_session.have_ca && wf_agent_set_ca_bundle(agent, g_session.ca_path) != WF_OK) {
      COBALT_LOGE("session: could not set the CA bundle");
      wf_agent_free(agent);
      return NULL;
   }

   /*
    * Hand libcurl's mbedTLS backend the application's DRBG for the handshake.
    * Without this it draws client randoms and ephemeral key-agreement material
    * from devkitPro's mbedtls_hardware_poll, which is the console's tick
    * counter — see util/rng.h. cobalt_session_init() has already refused to
    * come up if this could not work, so a failure here is a real surprise.
    */
   wf_status rng = wf_agent_set_tls_rng(agent, cobalt_rng_mbedtls, NULL);
#ifdef COBALT_E2E_HOST
   /* Host end-to-end only (tests/Makefile `e2e`): plain HTTP to a localhost
    * mock, and a host curl has no mbedTLS hook to install. Never defined in a
    * Wii U build. */
   rng = WF_OK;
#endif
   if (rng != WF_OK) {
      COBALT_LOGE("session: could not install the TLS RNG (%d) — refusing to "
                  "hand the handshake to a tick-seeded generator", (int) rng);
      wf_agent_free(agent);
      return NULL;
   }

   if (wf_agent_set_post_langs(agent, cobalt_session_post_lang()) != WF_OK) {
      COBALT_LOGW("session: could not set the post language");
   }

   return agent;
}

void
cobalt_session_run_login(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   cobalt_session_teardown_agent();

   const char *entry = cobalt_session_entry_service(in->service);
   SDL_LockMutex(g_session.lock);
   snprintf(g_session.service, sizeof(g_session.service), "%s", entry);
   SDL_UnlockMutex(g_session.lock);
   g_session.wf = new_wf_agent(entry);
   g_session.prefs_loaded = false;
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Could not create the client.");
      return;
   }

   /* The account's PDS is discovered from its handle, so the user never has to
    * name the host it lives on (wf_agent_login_discovered). */
   COBALT_LOGI("session: discovering the PDS for %s", in->identifier);
   char *pds = NULL;
   wf_status status = wf_agent_login_discovered(g_session.wf, in->identifier, in->password, &pds);
   if (status == WF_OK && pds) {
      COBALT_LOGI("session: signed in at %s", pds);
      SDL_LockMutex(g_session.lock);
      snprintf(g_session.service, sizeof(g_session.service), "%s", pds);
      SDL_UnlockMutex(g_session.lock);
   }
   free(pds);
   if (status != WF_OK) {
      COBALT_LOGW("session: login failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_LOGIN);
      /* The PDS's own wording ("Invalid identifier or password", "A sign in
       * code has been sent to your email") says more than our guess does. */
      const char *why = wf_agent_last_error(g_session.wf);
      if (why && *why) {
         char hint[COBALT_MESSAGE_MAX];
         snprintf(hint, sizeof hint, "%s", r->message);
         cobalt_session_set_message(r, "Server said: %.160s\n%s", why, hint);
      }
      cobalt_session_teardown_agent();
      return;
   }

   if (!cobalt_session_publish()) {
      /* Signed in, but the credentials will not outlive this run. Worth saying
       * out loud rather than silently making the user retype next boot. */
      cobalt_session_set_message(r, "Signed in, but the session could not be saved to the SD "
                     "card — you will need to sign in again next time.");
   }

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
   COBALT_LOGI("session: signed in as %s (%s)", g_session.handle, g_session.did);
}

/* Hooks for Wolfram's pairing driver. They run on this worker thread. */
static void
pair_on_code(const wf_oauth_pair_begin *begin, void *userdata)
{
   (void) userdata;
   SDL_LockMutex(g_session.lock);
   snprintf(g_session.pair_url, sizeof(g_session.pair_url), "%s", begin->pair_url);
   snprintf(g_session.pair_code, sizeof(g_session.pair_code), "%s", begin->pair_code);
   SDL_UnlockMutex(g_session.lock);
}

static int
pair_cancel(void *userdata)
{
   (void) userdata;
   /* Quitting mid-pairing stops polling instead of waiting out the window. */
   SDL_LockMutex(g_session.lock);
   const bool stop = g_session.stop;
   SDL_UnlockMutex(g_session.lock);
   return stop ? 1 : 0;
}

static void
pair_sleep(unsigned ms, void *userdata)
{
   (void) userdata;
   SDL_Delay(ms);
}

/*
 * Browser sign-in through a hosted Wolfram OAuth node. The begin/poll contract,
 * its parsing and the polling loop are Wolfram's (wolfram/oauth_pairing.h);
 * Cobalt keeps only what is its own: showing the pairing URL and code, and
 * turning the node's bearer into a session. The token is never logged.
 */
void
cobalt_session_run_oauth(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (strcmp(in->service, cobalt_session_default_service()) == 0) {
      cobalt_session_set_message(r, "Enter the OAuth node URL in Server / OAuth node.");
      return;
   }

   wf_xrpc_client *client = wf_xrpc_client_new(in->service);
   if (!client) {
      cobalt_session_set_message(r, "Could not create the OAuth-node client.");
      return;
   }
   if (g_session.have_ca) {
      wf_xrpc_client_set_ca_bundle(client, g_session.ca_path);
   }

   wf_oauth_pair_hooks hooks;
   memset(&hooks, 0, sizeof(hooks));
   hooks.on_code = pair_on_code;
   hooks.cancel = pair_cancel;
   hooks.sleep_ms = pair_sleep;

   wf_oauth_pair_poll poll;
   memset(&poll, 0, sizeof(poll));
   const wf_status st = wf_oauth_pair_run(client, in->identifier, &hooks, &poll);
   wf_xrpc_client_free(client);

   SDL_LockMutex(g_session.lock);
   g_session.pair_url[0] = '\0';
   g_session.pair_code[0] = '\0';
   SDL_UnlockMutex(g_session.lock);

   switch (st) {
      case WF_OK:
         break;
      case WF_ERR_AUTH:
         if (poll.message[0]) {
            cobalt_session_set_message(r, "%s", poll.message);
         } else {
            cobalt_session_set_message(r, "OAuth sign-in failed.");
         }
         wf_oauth_pair_poll_wipe(&poll);
         return;
      case WF_ERR_TIMEOUT:
         cobalt_session_set_message(r, "The web sign-in request expired. Start it again.");
         return;
      case WF_ERR_STATE:
         cobalt_session_set_message(r, "Sign-in was cancelled.");
         return;
      case WF_ERR_PARSE:
         cobalt_session_set_message(r, "The OAuth node sent a response Cobalt could not use.");
         return;
      default:
         cobalt_session_set_message(r, "The OAuth node could not start sign-in.");
         return;
   }

   cobalt_session_teardown_agent();
   g_session.wf = new_wf_agent(poll.service);
   if (!g_session.wf) {
      wf_oauth_pair_poll_wipe(&poll);
      cobalt_session_set_message(r, "Could not create a session for the OAuth node.");
      return;
   }
   const wf_status bearer = wf_agent_set_bearer(g_session.wf, poll.token, poll.handle, poll.did);
   if (bearer != WF_OK) {
      COBALT_LOGW("session: the node's session was refused (%d)", (int) bearer);
      wf_oauth_pair_poll_wipe(&poll);
      cobalt_session_teardown_agent();
      cobalt_session_set_message(r, "The OAuth node returned an unusable session.");
      return;
   }
   cobalt_session_publish();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
   snprintf(r->message, sizeof(r->message), "Signed in as %.160s", poll.handle);
   wf_oauth_pair_poll_wipe(&poll);
}

void
cobalt_session_run_resume(cobalt_job_result *r, cobalt_auth_state *state)
{
   cobalt_stored_session stored;
   if (!cobalt_session_store_load(&stored)) {
      /*
       * Present but undecodable is unrecoverable by construction — a missing
       * or replaced device.key, or a corrupt file. Clearing it stops
       * cobalt_session_has_saved() firing a doomed resume on every boot with a
       * message that says nothing is stored.
       */
      if (cobalt_session_store_exists()) {
         COBALT_LOGW("session: stored credentials could not be decoded, clearing");
         cobalt_session_store_clear();
         cobalt_session_set_message(r, "The saved session could not be read. Sign in again.");
      } else {
         cobalt_session_set_message(r, "No saved session was found.");
      }
      return;
   }

   cobalt_session_teardown_agent();

   char service[COBALT_SERVICE_MAX];
   snprintf(service, sizeof(service), "%s",
            stored.service[0] ? stored.service : cobalt_session_default_service());

   SDL_LockMutex(g_session.lock);
   snprintf(g_session.service, sizeof(g_session.service), "%s", service);
   SDL_UnlockMutex(g_session.lock);

   g_session.wf = new_wf_agent(service);
   g_session.prefs_loaded = false;
   if (!g_session.wf) {
      memset(&stored, 0, sizeof(stored));
      cobalt_session_set_message(r, "Could not create the client.");
      return;
   }

   /* Wolfram deep-copies this, so the stack copy can be wiped straight after. */
   wf_status status;
   if (stored.refresh_jwt[0] == '\0') {
      COBALT_LOGI("session: resuming OAuth-node session for %s at %s",
                  stored.handle, service);
      status = wf_agent_set_bearer(g_session.wf, stored.access_jwt,
                                   stored.handle, stored.did);
   } else {
      wf_session_data data;
      memset(&data, 0, sizeof(data));
      data.access_jwt = stored.access_jwt;
      data.refresh_jwt = stored.refresh_jwt;
      data.handle = stored.handle;
      data.did = stored.did;
      data.email_confirmed = -1;
      data.email_auth_factor = -1;
      data.active = -1;
      COBALT_LOGI("session: resuming %s at %s", stored.handle, service);
      status = wf_agent_resume(g_session.wf, &data);
   }
   memset(&stored, 0, sizeof(stored));

   if (status != WF_OK) {
      COBALT_LOGW("session: resume failed (%d)", (int) status);
      cobalt_session_describe_failure(r, status, COBALT_JOB_RESUME);
      cobalt_session_teardown_agent();
      /* A refresh JWT the server has rejected is never coming back, so do not
       * leave it on the card to fail again on the next boot. Transport failures
       * are different — those are worth retrying with the same credentials. */
      if (status == WF_ERR_HTTP || status == WF_ERR_AUTH) {
         cobalt_session_store_clear();
      }
      return;
   }

   /* Resuming refreshes as part of resuming, so the tokens in hand are already
    * newer than the ones just read off the card. */
   cobalt_session_publish();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
   COBALT_LOGI("session: resumed as %s", g_session.handle);
}

void
cobalt_session_run_logout(cobalt_job_result *r, cobalt_auth_state *state)
{
   if (g_session.wf) {
      /* Best effort. Wolfram clears the local session either way, and a user
       * who asked to sign out must end up signed out even if the PDS is
       * unreachable — so a failure here is logged, not surfaced. */
      wf_status status = wf_agent_logout(g_session.wf);
      if (status != WF_OK) {
         COBALT_LOGW("session: deleteSession failed (%d) — clearing locally anyway",
                     (int) status);
      }
      cobalt_session_teardown_agent();
   }

   cobalt_session_store_clear();

   SDL_LockMutex(g_session.lock);
   g_session.handle[0] = '\0';
   g_session.did[0] = '\0';
   /* The feed belongs to the account that just signed out. */
   cobalt_feed_reset(&g_session.feed);
   g_session.feed.cursor[0] = '\0';
   g_session.feed.has_more = false;
   cobalt_thread_reset(&g_session.conversation);
   cobalt_notifications_reset(&g_session.notifications);
   cobalt_profile_reset(&g_session.profile);
   cobalt_feed_reset(&g_session.author_feed);
   cobalt_actor_list_reset(&g_session.muted);
   cobalt_actor_list_reset(&g_session.blocked);
   cobalt_actor_list_reset(&g_session.followers);
   cobalt_actor_list_reset(&g_session.following);
   g_session.follow_actor[0] = '\0';
   cobalt_actor_list_reset(&g_session.search);
   SDL_UnlockMutex(g_session.lock);

   *state = COBALT_AUTH_SIGNED_OUT;
   r->ok = true;
   COBALT_LOGI("session: signed out");
}

#endif /* COBALT_HAS_WOLFRAM */

bool
cobalt_session_begin_login(const char *service, const char *identifier,
                           const char *password)
{
   if (!identifier || !identifier[0] || !password || !password[0]) {
      return false;
   }

   cobalt_job_input in;
   memset(&in, 0, sizeof(in));

   if (!cobalt_session_normalise_service(service, in.service, sizeof(in.service))) {
      COBALT_LOGW("session: service URL too long");
      return false;
   }
   snprintf(in.identifier, sizeof(in.identifier), "%s", identifier);
   snprintf(in.password, sizeof(in.password), "%s", password);

   bool ok = cobalt_session_submit(COBALT_JOB_LOGIN, &in);
   memset(&in, 0, sizeof(in));
   return ok;
}

bool
cobalt_session_begin_oauth(const char *oauth_node, const char *handle)
{
   bool available = cobalt_session_available();
#ifdef COBALT_E2E_HOST
   /* Host end-to-end only: the host has no trust store or console entropy, and
    * talks plain HTTP to a localhost mock node. Never defined in a Wii U build. */
   available = g_session.initialised;
#endif
   if (!oauth_node || !oauth_node[0] || !handle || !handle[0] ||
       cobalt_session_busy() || !available) {
      return false;
   }

   char service[COBALT_SERVICE_MAX];
   if (!cobalt_session_normalise_service(oauth_node, service, sizeof(service))) {
      return false;
   }

   SDL_LockMutex(g_session.lock);
   if (g_session.busy) {
      SDL_UnlockMutex(g_session.lock);
      return false;
   }
   memset(&g_session.input, 0, sizeof(g_session.input));
   snprintf(g_session.input.service, sizeof(g_session.input.service), "%s", service);
   snprintf(g_session.input.identifier, sizeof(g_session.input.identifier), "%s", handle);
   g_session.pending = COBALT_JOB_OAUTH;
   g_session.busy = true;
   g_session.have_result = false;
   g_session.resting_state = g_session.state;
   g_session.state = COBALT_AUTH_WORKING;
   g_session.pair_url[0] = '\0';
   g_session.pair_code[0] = '\0';
   SDL_CondSignal(g_session.wake);
   SDL_UnlockMutex(g_session.lock);
   return true;
}

bool
cobalt_session_begin_resume(void)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   return cobalt_session_submit(COBALT_JOB_RESUME, &in);
}

bool
cobalt_session_begin_logout(void)
{
   cobalt_job_input in;
   memset(&in, 0, sizeof(in));
   return cobalt_session_submit(COBALT_JOB_LOGOUT, &in);
}
