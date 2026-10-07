#include "util/threadprio.h"
#include "atproto/session.h"
#include "atproto/actors.h"
#include "atproto/feed.h"
#include "atproto/prefs.h"
#include "atproto/notifications.h"
#include "atproto/actor_profile.h"
#include "cache/session_store.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/rng.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/actor_prefs_typed.h>
#include <wolfram/actor_typed.h>
#include <wolfram/feed_gen_typed.h>
#include <wolfram/agent.h>
#include <wolfram/attach.h>
#include <wolfram/embed.h>
#include <wolfram/failure.h>
#include <wolfram/feed_typed.h>
#include <wolfram/graph_typed.h>
#include <wolfram/list_typed.h>
#include <wolfram/moderation_typed.h>
#include <wolfram/oauth_pairing.h>
#include <wolfram/session.h>
#include <wolfram/thread_typed.h>
#include <wolfram/threadgate_postgate.h>
#include <wolfram/time.h>
#include <wolfram/xrpc.h>
#endif

#include <SDL.h>
#include <cJSON.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Bundled by `make cacert` into romfs/. Without it curl cannot verify any
 * certificate on this platform — see tools/fetch_cacert.sh. */
#define CA_BUNDLE_FILE "cacert.pem"

#define DEFAULT_SERVICE "https://bsky.social"

/* What a job carries in. Copied out of the request under the lock so the
 * worker never reads a buffer the main thread might be rewriting. */
typedef struct {
   char service[COBALT_SERVICE_MAX];
   char identifier[COBALT_IDENTIFIER_MAX];
   char password[COBALT_PASSWORD_MAX];
   /* Timeline only: append the next page rather than replacing the feed. */
   bool paging;

   /* Interactions and thread fetches: which post, and for an undo, which
    * record of the viewer's own to delete. */
   char uri[COBALT_POST_URI_MAX];
   char cid[COBALT_POST_CID_MAX];
   char record_uri[COBALT_POST_URI_MAX];

   /* Mute only: the target state (true = mute, false = unmute). Unlike
    * follow/block, mute has no record URI for record_uri's emptiness to
    * signal an undo with, so the direction is carried explicitly. */
   bool flag;

   /* A thread of new posts: how many, with the texts in s.thread_texts (too big
    * to copy with every job; only one job runs at a time and a new one is
    * refused while one is in flight). Zero for everything else. */
   int thread_count;

   /* Composing. `text` is the post body; the refs are empty for a new post. */
   char text[COBALT_COMPOSE_TEXT_MAX];
   char root_uri[COBALT_POST_URI_MAX];
   char root_cid[COBALT_POST_CID_MAX];

   /* Reply-control for a new top-level post; ignored on a reply. Matches
    * `cobalt_reply_gate`: 0 everyone, 1 followed/mentioned, 2 nobody. */
   int reply_gate;

   /* Profile tab switches: a cobalt_profile_tab. */
   int tab;

   /* Composing a quote post: `uri`/`cid` name the quoted post, not a parent. */
   bool quote;

   /* Optional image to attach (SD path); empty for none. */
   char attach_path[COBALT_ATTACH_PATH_MAX];
   char attach_alt[COBALT_ATTACH_ALT_MAX];
} job_input;

static struct {
   bool initialised;

   /* The texts of a thread being posted; see job_input.thread_count. */
   char thread_texts[COBALT_THREAD_POSTS_MAX][COBALT_COMPOSE_TEXT_MAX];

   /* Set once at init and read-only afterwards, so no locking. */
   char ca_path[COBALT_PATH_MAX];
   bool have_ca;

   /* The loaded profile's pinned post, so a tab switch can re-add it. */
   char pinned_uri[COBALT_POST_URI_MAX];

   /* Index into POST_LANGS; 0 is "none". Applied to each new agent. */
   int post_lang;
   const char *blocker;

   SDL_mutex *lock;
   SDL_cond *wake;
   SDL_Thread *thread;
   bool stop;

   cobalt_job_kind pending;
   job_input input;

   bool busy;
   bool have_result;
   cobalt_job_result result;

   /* Published by the worker, read by the UI. Guarded by `lock`. */
   cobalt_auth_state state;
   /* What `state` was before the in-flight request set it to WORKING. A job
    * that fails leaves it alone, so a failed refresh does not sign the user
    * out and a failed sign-in does not claim they are signed in. */
   cobalt_auth_state resting_state;
   char handle[COBALT_HANDLE_MAX];
   char did[COBALT_DID_MAX];
   char service[COBALT_SERVICE_MAX];
   char pair_url[COBALT_OAUTH_URL_MAX];
   char pair_code[COBALT_OAUTH_CODE_MAX];

   /* Published by the worker alongside the auth state, and read by the
    * timeline screen. Guarded by `lock` on write, read while idle. */
   cobalt_feed feed;
   /* Named to stay clear of `thread`, which is the worker. */
   cobalt_thread conversation;
   cobalt_notifications notifications;
   cobalt_profile profile;
   cobalt_feed author_feed;
   int profile_tab;
   cobalt_actor_list muted;
   cobalt_actor_list blocked;
   cobalt_actor_list followers;
   cobalt_actor_list following;
   char follow_actor[COBALT_POST_URI_MAX];
   /* getLikes/getRepostedBy share this one list: only one is on screen at
    * a time, and a fetch always resets it, so there is no cross-talk. */
   cobalt_actor_list likes;
   char likes_uri[COBALT_POST_URI_MAX];
   /* What the shared feed window holds, so refresh and paging re-fetch the
    * same source. Written and read on the UI thread only. */
   int feed_source;            /* 0 home, 1 custom feed, 2 post search */
   char feed_arg[COBALT_POST_URI_MAX];
   cobalt_actor_list search;
   cobalt_list_summary_list lists;
   cobalt_saved_feeds saved_feeds;
   cobalt_actor_list list_members;

#ifdef COBALT_HAS_WOLFRAM
   /* Owned by the worker while a job runs; only touched off-thread when idle. */
   wf_agent *wf;
#endif

   /* Worker-only: the account's muted words and hide-reposts, fetched once per
    * sign-in. */
   cobalt_prefs prefs;
   bool prefs_loaded;
} s;

/* --- pure helpers --- */

const char *
cobalt_session_default_service(void)
{
   return DEFAULT_SERVICE;
}

static bool
is_space(char c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool
cobalt_session_normalise_service(const char *input, char *out, size_t out_size)
{
   if (!out || out_size == 0) {
      return false;
   }
   out[0] = '\0';

   const char *begin = input ? input : "";
   while (*begin && is_space(*begin)) {
      begin++;
   }

   const char *end = begin + strlen(begin);
   /* Trailing slashes matter: Wolfram builds "<base>/xrpc/<nsid>", so a stray
    * one would produce a double slash that some PDS routers reject. */
   while (end > begin && (is_space(end[-1]) || end[-1] == '/')) {
      end--;
   }

   size_t len = (size_t) (end - begin);
   if (len == 0) {
      if (strlen(DEFAULT_SERVICE) >= out_size) {
         return false;
      }
      snprintf(out, out_size, "%s", DEFAULT_SERVICE);
      return true;
   }

   /* A scheme only counts if it comes before the first path separator, so
    * "example.com/pds://x" is treated as a bare host, not as a URL. */
   bool has_scheme = false;
   for (size_t i = 0; i + 2 < len; i++) {
      if (begin[i] == '/') {
         break;
      }
      if (begin[i] == ':' && begin[i + 1] == '/' && begin[i + 2] == '/') {
         has_scheme = true;
         break;
      }
   }

   const char *prefix = has_scheme ? "" : "https://";
   if (strlen(prefix) + len >= out_size) {
      return false;
   }

   snprintf(out, out_size, "%s%.*s", prefix, (int) len, begin);
   return true;
}

/* --- state accessors --- */

cobalt_auth_state
cobalt_session_state(void)
{
   if (!s.lock) {
      return COBALT_AUTH_SIGNED_OUT;
   }
   SDL_LockMutex(s.lock);
   cobalt_auth_state state = s.state;
   SDL_UnlockMutex(s.lock);
   return state;
}

bool
cobalt_session_busy(void)
{
   if (!s.lock) {
      return false;
   }
   SDL_LockMutex(s.lock);
   bool busy = s.busy;
   SDL_UnlockMutex(s.lock);
   return busy;
}

/*
 * These hand back pointers into state the worker also writes, which is only
 * safe because the worker publishes them as whole strings under the lock (see
 * publish_session) and never edits them in place. A caller can therefore read a
 * frame-old value, but never a half-written one.
 */
const char *
cobalt_session_handle(void)
{
   return s.handle;
}

const char *
cobalt_session_did(void)
{
   return s.did;
}

const char *
cobalt_session_pair_url(void)
{
   return s.pair_url;
}

const char *
cobalt_session_pair_code(void)
{
   return s.pair_code;
}

const char *
cobalt_session_service(void)
{
   return s.service[0] ? s.service : DEFAULT_SERVICE;
}

bool
cobalt_session_available(void)
{
   return s.initialised && s.blocker == NULL;
}

const char *
cobalt_session_blocker(void)
{
   return s.blocker;
}

const char *
cobalt_session_ca_path(void)
{
   return s.have_ca ? s.ca_path : NULL;
}

bool
cobalt_session_threaded(void)
{
   return s.thread != NULL;
}

bool
cobalt_session_has_saved(void)
{
   return cobalt_session_store_exists();
}

/* --- the jobs themselves --- */

static void
set_message(cobalt_job_result *r, const char *fmt, ...)
   __attribute__((format(printf, 2, 3)));

static void
set_message(cobalt_job_result *r, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(r->message, sizeof(r->message), fmt, ap);
   va_end(ap);
}

/* Tags offered on the account screen; "" means leave `langs` off. */
static const char *const POST_LANGS[] = { "", "en", "cy", "ga", "gd", "fr",
                                          "de", "es" };
#define POST_LANG_COUNT ((int) (sizeof(POST_LANGS) / sizeof(POST_LANGS[0])))
#define POST_LANG_FILE "postlang.txt"

static void
load_post_lang(void)
{
   char path[COBALT_PATH_MAX];
   s.post_lang = 0;
   if (!cobalt_data_path(path, sizeof(path), POST_LANG_FILE)) {
      return;
   }
   FILE *f = fopen(path, "rb");
   if (!f) {
      return;
   }
   char tag[16] = { 0 };
   size_t n = fread(tag, 1, sizeof(tag) - 1, f);
   fclose(f);
   tag[n] = '\0';
   for (size_t i = 0; i < n; i++) {
      if (tag[i] == '\n' || tag[i] == '\r') {
         tag[i] = '\0';
         break;
      }
   }
   for (int i = 1; i < POST_LANG_COUNT; i++) {
      if (strcmp(tag, POST_LANGS[i]) == 0) {
         s.post_lang = i;
         return;
      }
   }
}

static void
save_post_lang(void)
{
   char path[COBALT_PATH_MAX];
   if (!cobalt_data_path(path, sizeof(path), POST_LANG_FILE)) {
      return;
   }
   FILE *f = fopen(path, "wb");
   if (!f) {
      COBALT_LOGW("session: could not save the post language");
      return;
   }
   fprintf(f, "%s\n", POST_LANGS[s.post_lang]);
   fclose(f);
}

const char *
cobalt_session_post_lang(void)
{
   return POST_LANGS[s.post_lang];
}

void
cobalt_session_cycle_post_lang(void)
{
   s.post_lang = (s.post_lang + 1) % POST_LANG_COUNT;
   save_post_lang();
#ifdef COBALT_HAS_WOLFRAM
   if (s.wf) {
      wf_agent_set_post_langs(s.wf, POST_LANGS[s.post_lang]);
   }
#endif
}

#ifdef COBALT_HAS_WOLFRAM

/*
 * What kind of failure it was is Wolfram's call (wf_failure_classify, shared with
 * the other clients); the wording is Cobalt's. Each message names the most
 * likely cause and what to try. run_login additionally prepends the server's
 * message via wf_agent_last_error.
 */
static void
describe_failure(cobalt_job_result *r, wf_status status, cobalt_job_kind kind)
{
   if (status == WF_ERR_ALLOC) {
      set_message(r, "Out of memory.");
      return;
   }
   switch (wf_failure_classify(status, 0, NULL)) {
      case WF_FAIL_NETWORK:
      case WF_FAIL_TLS:
         set_message(r, "Could not reach the server. Check the console's "
                        "internet connection, and that a TLS trust store was "
                        "bundled with this build.");
         break;

      case WF_FAIL_TIMEOUT:
         set_message(r, "The server did not answer in time. Try again.");
         break;

      case WF_FAIL_BAD_CREDENTIALS:
      case WF_FAIL_OTHER:
         if (status != WF_ERR_AUTH && status != WF_ERR_HTTP) {
            /* No friendlier wording available: log the code so a hardware run
             * can be matched against wf_status in wolfram/xrpc.h. */
            set_message(r, "The request failed (wolfram status %d).", (int) status);
         } else if (kind == COBALT_JOB_LOGIN) {
            set_message(r, "The server rejected those details. Check the handle "
                           "and app password — an account password will not work "
                           "if two-factor is on.");
         } else {
            set_message(r, "The server refused that request. The post may be gone, or the "
                           "session may have expired - try again, then sign in "
                           "again if it keeps happening.");
         }
         break;

      case WF_FAIL_RATE_LIMIT:
         set_message(r, "The server is rate limiting this console. Wait a few "
                        "minutes and try again.");
         break;

      case WF_FAIL_SERVER:
         set_message(r, "The server had a problem. Try again in a few minutes.");
         break;

      case WF_FAIL_BAD_RESPONSE:
         set_message(r, "The server sent a reply Cobalt could not read.");
         break;

      case WF_FAIL_NONE:
      case WF_FAIL_NOT_READY:
      default:
         set_message(r, "The request failed (wolfram status %d).", (int) status);
         break;
   }
}

/*
 * Mirror the live session into the on-card store and the UI snapshot.
 *
 * Reads the credentials back out of the agent rather than being handed them,
 * because the agent refreshes tokens transparently mid-request: whatever it
 * holds now is newer than anything the caller could pass in. Called after every
 * job, so a refresh that happened during a timeline fetch is persisted too.
 */
static bool
publish_session(void)
{
   wf_session_data data;
   memset(&data, 0, sizeof(data));
   if (wf_agent_get_session_data(s.wf, &data) != WF_OK) {
      return false;
   }

   cobalt_stored_session stored;
   memset(&stored, 0, sizeof(stored));

   /* wf_agent_login re-points the client at the account's real PDS, discovered
    * from didDoc#atproto_pds — frequently not the host the user typed. Persist
    * where the tokens are actually valid, not the entry point. */
   char fallback[COBALT_SERVICE_MAX];
   SDL_LockMutex(s.lock);
   snprintf(fallback, sizeof(fallback), "%s", s.service);
   SDL_UnlockMutex(s.lock);

   const char *service = (data.pds_url && data.pds_url[0]) ? data.pds_url : fallback;
   /*
    * Refuse to persist anything that would not fit rather than truncating it.
    * A silently clipped refresh JWT is the worst of both: it saves, then fails
    * to resume on every subsequent boot, and the failure path wipes the store —
    * so the user retypes their app password forever with nothing on the
    * diagnostics screen explaining why. Bluesky's tokens are far under these
    * limits; a self-hosted PDS with more claims is what this guards.
    */
   const struct { const char *value; size_t limit; const char *name; } FIELDS[] = {
      { service,                                    sizeof(stored.service),     "PDS URL" },
      { data.handle ? data.handle : "",             sizeof(stored.handle),      "handle" },
      { data.did ? data.did : "",                   sizeof(stored.did),         "DID" },
      { data.access_jwt ? data.access_jwt : "",     sizeof(stored.access_jwt),  "access token" },
      { data.refresh_jwt ? data.refresh_jwt : "",   sizeof(stored.refresh_jwt), "refresh token" },
   };
   for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
      if (strlen(FIELDS[i].value) >= FIELDS[i].limit) {
         COBALT_LOGE("session: %s is %u bytes, over the %u the store holds — "
                     "refusing to save a truncated credential",
                     FIELDS[i].name, (unsigned) strlen(FIELDS[i].value),
                     (unsigned) FIELDS[i].limit - 1);
         wf_agent_session_data_free(&data);
         memset(&stored, 0, sizeof(stored));
         return false;
      }
   }

   snprintf(stored.service, sizeof(stored.service), "%s", service);
   snprintf(stored.handle, sizeof(stored.handle), "%s", data.handle ? data.handle : "");
   snprintf(stored.did, sizeof(stored.did), "%s", data.did ? data.did : "");
   snprintf(stored.access_jwt, sizeof(stored.access_jwt), "%s",
            data.access_jwt ? data.access_jwt : "");
   snprintf(stored.refresh_jwt, sizeof(stored.refresh_jwt), "%s",
            data.refresh_jwt ? data.refresh_jwt : "");

   wf_agent_session_data_free(&data);

   bool saved = cobalt_session_store_save(&stored);

   SDL_LockMutex(s.lock);
   snprintf(s.service, sizeof(s.service), "%s", stored.service);
   snprintf(s.handle, sizeof(s.handle), "%s", stored.handle);
   snprintf(s.did, sizeof(s.did), "%s", stored.did);
   SDL_UnlockMutex(s.lock);

   memset(&stored, 0, sizeof(stored));
   return saved;
}

static void
teardown_wf(void)
{
   if (s.wf) {
      wf_agent_free(s.wf);
      s.wf = NULL;
      s.prefs_loaded = false;
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

   if (s.have_ca && wf_agent_set_ca_bundle(agent, s.ca_path) != WF_OK) {
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

   if (wf_agent_set_post_langs(agent, POST_LANGS[s.post_lang]) != WF_OK) {
      COBALT_LOGW("session: could not set the post language");
   }

   return agent;
}

static void
run_login(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   teardown_wf();

   SDL_LockMutex(s.lock);
   snprintf(s.service, sizeof(s.service), "%s", in->service);
   SDL_UnlockMutex(s.lock);

   s.wf = new_wf_agent(in->service);
   s.prefs_loaded = false;
   if (!s.wf) {
      set_message(r, "Could not create the client.");
      return;
   }

   COBALT_LOGI("session: createSession at %s", in->service);
   wf_status status = wf_agent_login(s.wf, in->identifier, in->password);
   if (status != WF_OK) {
      COBALT_LOGW("session: login failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_LOGIN);
      /* The PDS's own wording ("Invalid identifier or password", "A sign in
       * code has been sent to your email") says more than our guess does. */
      const char *why = wf_agent_last_error(s.wf);
      if (why && *why) {
         char hint[COBALT_MESSAGE_MAX];
         snprintf(hint, sizeof hint, "%s", r->message);
         set_message(r, "Server said: %.160s\n%s", why, hint);
      }
      teardown_wf();
      return;
   }

   if (!publish_session()) {
      /* Signed in, but the credentials will not outlive this run. Worth saying
       * out loud rather than silently making the user retype next boot. */
      set_message(r, "Signed in, but the session could not be saved to the SD "
                     "card — you will need to sign in again next time.");
   }

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
   COBALT_LOGI("session: signed in as %s (%s)", s.handle, s.did);
}

/* Hooks for Wolfram's pairing driver. They run on this worker thread. */
static void
pair_on_code(const wf_oauth_pair_begin *begin, void *userdata)
{
   (void) userdata;
   SDL_LockMutex(s.lock);
   snprintf(s.pair_url, sizeof(s.pair_url), "%s", begin->pair_url);
   snprintf(s.pair_code, sizeof(s.pair_code), "%s", begin->pair_code);
   SDL_UnlockMutex(s.lock);
}

static int
pair_cancel(void *userdata)
{
   (void) userdata;
   /* Quitting mid-pairing stops polling instead of waiting out the window. */
   SDL_LockMutex(s.lock);
   const bool stop = s.stop;
   SDL_UnlockMutex(s.lock);
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
static void
run_oauth(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (strcmp(in->service, DEFAULT_SERVICE) == 0) {
      set_message(r, "Enter the OAuth node URL in Server / OAuth node.");
      return;
   }

   wf_xrpc_client *client = wf_xrpc_client_new(in->service);
   if (!client) {
      set_message(r, "Could not create the OAuth-node client.");
      return;
   }
   if (s.have_ca) {
      wf_xrpc_client_set_ca_bundle(client, s.ca_path);
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

   SDL_LockMutex(s.lock);
   s.pair_url[0] = '\0';
   s.pair_code[0] = '\0';
   SDL_UnlockMutex(s.lock);

   switch (st) {
      case WF_OK:
         break;
      case WF_ERR_AUTH:
         if (poll.message[0]) {
            set_message(r, "%s", poll.message);
         } else {
            set_message(r, "OAuth sign-in failed.");
         }
         wf_oauth_pair_poll_wipe(&poll);
         return;
      case WF_ERR_TIMEOUT:
         set_message(r, "The web sign-in request expired. Start it again.");
         return;
      case WF_ERR_STATE:
         set_message(r, "Sign-in was cancelled.");
         return;
      case WF_ERR_PARSE:
         set_message(r, "The OAuth node sent a response Cobalt could not use.");
         return;
      default:
         set_message(r, "The OAuth node could not start sign-in.");
         return;
   }

   teardown_wf();
   s.wf = new_wf_agent(poll.service);
   if (!s.wf) {
      wf_oauth_pair_poll_wipe(&poll);
      set_message(r, "Could not create a session for the OAuth node.");
      return;
   }
   const wf_status bearer = wf_agent_set_bearer(s.wf, poll.token, poll.handle, poll.did);
   if (bearer != WF_OK) {
      COBALT_LOGW("session: the node's session was refused (%d)", (int) bearer);
      wf_oauth_pair_poll_wipe(&poll);
      teardown_wf();
      set_message(r, "The OAuth node returned an unusable session.");
      return;
   }
   publish_session();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
   snprintf(r->message, sizeof(r->message), "Signed in as %.160s", poll.handle);
   wf_oauth_pair_poll_wipe(&poll);
}

static void
run_resume(cobalt_job_result *r, cobalt_auth_state *state)
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
         set_message(r, "The saved session could not be read. Sign in again.");
      } else {
         set_message(r, "No saved session was found.");
      }
      return;
   }

   teardown_wf();

   char service[COBALT_SERVICE_MAX];
   snprintf(service, sizeof(service), "%s",
            stored.service[0] ? stored.service : DEFAULT_SERVICE);

   SDL_LockMutex(s.lock);
   snprintf(s.service, sizeof(s.service), "%s", service);
   SDL_UnlockMutex(s.lock);

   s.wf = new_wf_agent(service);
   s.prefs_loaded = false;
   if (!s.wf) {
      memset(&stored, 0, sizeof(stored));
      set_message(r, "Could not create the client.");
      return;
   }

   /* Wolfram deep-copies this, so the stack copy can be wiped straight after. */
   wf_status status;
   if (stored.refresh_jwt[0] == '\0') {
      COBALT_LOGI("session: resuming OAuth-node session for %s at %s",
                  stored.handle, service);
      status = wf_agent_set_bearer(s.wf, stored.access_jwt,
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
      status = wf_agent_resume(s.wf, &data);
   }
   memset(&stored, 0, sizeof(stored));

   if (status != WF_OK) {
      COBALT_LOGW("session: resume failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_RESUME);
      teardown_wf();
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
   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
   COBALT_LOGI("session: resumed as %s", s.handle);
}


/*
 * How many posts to ask for per page. The window holds 60, and a smaller page
 * gets something on screen sooner on a console whose upstream is often a slow
 * wireless link — the cost of a second request is far less than the cost of
 * staring at a blank feed.
 */
#define TIMELINE_PAGE 20

/* Fetch the saved preferences once per sign-in. Failure is not fatal: the
 * feed is simply shown unfiltered. */
static void
ensure_prefs(void)
{
   if (s.prefs_loaded || !s.wf) {
      return;
   }
   wf_actor_preferences p;
   memset(&p, 0, sizeof(p));
   const wf_status st = wf_agent_get_actor_prefs_typed(s.wf, &p);
   if (st != WF_OK) {
      COBALT_LOGW("session: getPreferences failed (%d); feed unfiltered", (int) st);
      cobalt_prefs_clear(&s.prefs);
      return;
   }
   cobalt_prefs_from_wolfram(&s.prefs, &p, cobalt_time_now());
   wf_actor_preferences_free(&p);
   s.prefs_loaded = true;
}

static void
run_timeline(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in to see your timeline.");
      return;
   }

   /* Paging uses the cursor from the last page; a refresh deliberately does
    * not, so pulling down after an hour away gets the top of the feed rather
    * than resuming where the old cursor pointed. */
   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.feed.cursor[0] ? s.feed.cursor : NULL;
      SDL_UnlockMutex(s.lock);

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

   COBALT_LOGI("session: getTimeline limit=%d cursor=%s", TIMELINE_PAGE,
               cursor ? cursor : "(top)");
   wf_status status = wf_agent_get_timeline_typed(s.wf, TIMELINE_PAGE, cursor, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: getTimeline failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_TIMELINE);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   /* Resolved once per page rather than per post: a feed rendered across a
    * second boundary showing two different "now" values would be worse than
    * one that is a moment stale. */
   ensure_prefs();
   const int64_t now = cobalt_time_now();

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_feed_reset(&s.feed);
   }
   const int before = s.feed.count;
   const int added = cobalt_feed_append_from_wolfram(&s.feed, &list, now);
   cobalt_prefs_filter_feed(&s.prefs, &s.feed, before, true);
   /* A page that added nothing is the end as far as this client is concerned,
    * whatever cursor came back — otherwise a screen that pages on reaching the
    * last post would ask again immediately, and keep asking. */
   if (in->paging && added == 0) {
      s.feed.has_more = false;
      s.feed.cursor[0] = '\0';
   }
   const int total = s.feed.count;
   SDL_UnlockMutex(s.lock);

   wf_agent_feed_list_free(&list);

   COBALT_LOGI("session: timeline +%d posts (%d held)", added, total);

   if (total == 0) {
      set_message(r, "Your timeline is empty. Follow some accounts on another "
                     "device and they will show up here.");
   }

   /* The session refreshes its tokens transparently, so a fetch may have
    * rotated them; persist whatever the agent holds now. */
   publish_session();

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
static void
run_feed(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in to view feeds.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.feed.cursor[0] ? s.feed.cursor : NULL;
      SDL_UnlockMutex(s.lock);

      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_feed_list list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: getFeed %s limit=%d cursor=%s", in->uri, TIMELINE_PAGE,
               cursor ? cursor : "(top)");
   wf_status status = wf_agent_get_feed_typed(s.wf, in->uri, TIMELINE_PAGE, cursor, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: getFeed failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_FEED);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   ensure_prefs();
   const int64_t now = cobalt_time_now();

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_feed_reset(&s.feed);
   }
   const int before = s.feed.count;
   const int added = cobalt_feed_append_from_wolfram(&s.feed, &list, now);
   cobalt_prefs_filter_feed(&s.prefs, &s.feed, before, false);
   if (in->paging && added == 0) {
      s.feed.has_more = false;
      s.feed.cursor[0] = '\0';
   }
   const int total = s.feed.count;
   SDL_UnlockMutex(s.lock);

   wf_agent_feed_list_free(&list);

   COBALT_LOGI("session: feed +%d posts (%d held)", added, total);

   if (total == 0) {
      set_message(r, "This feed has no posts right now.");
   }

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}


static void
run_thread(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in to read threads.");
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
   wf_status status = wf_agent_get_post_thread_typed(s.wf, in->uri, 6, &thread);
   if (status != WF_OK) {
      COBALT_LOGW("session: getPostThread failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_THREAD);
      /* Drop whatever was loaded. The screen has already switched, so leaving
       * it would show a *different* conversation than the one asked for —
       * complete with its focus marker and actionable posts. */
      SDL_LockMutex(s.lock);
      cobalt_thread_reset(&s.conversation);
      SDL_UnlockMutex(s.lock);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(s.lock);
   cobalt_thread_from_wolfram(&s.conversation, &thread, now);
   const int count = s.conversation.count;
   const bool truncated = s.conversation.truncated;
   SDL_UnlockMutex(s.lock);

   wf_agent_thread_free(&thread);

   COBALT_LOGI("session: thread %d posts%s", count, truncated ? " (truncated)" : "");
   if (truncated) {
      set_message(r, "This conversation is longer than Cobalt can show.");
   }

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/*
 * Like and repost share everything but two function calls, so they share an
 * implementation. `undo` is decided by the caller from the post's viewer
 * state; passing the record URI in rather than looking it up again keeps the
 * worker from touching the feed before it has to.
 */
static void
run_interaction(const job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state, bool is_like)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const bool undo = in->record_uri[0] != '\0';
   const char *what = is_like ? "like" : "repost";
   wf_status status;
   char created[COBALT_POST_URI_MAX] = "";

   if (undo) {
      COBALT_LOGI("session: removing %s %s", what, in->record_uri);
      status = is_like ? wf_agent_unlike(s.wf, in->record_uri)
                       : wf_agent_delete_repost(s.wf, in->record_uri);
   } else {
      wf_agent_post_result result;
      memset(&result, 0, sizeof(result));

      COBALT_LOGI("session: %s %s", what, in->uri);
      status = is_like ? wf_agent_like(s.wf, in->uri, in->cid, &result)
                       : wf_agent_repost(s.wf, in->uri, in->cid, &result);

      if (status == WF_OK) {
         snprintf(created, sizeof(created), "%s", result.uri ? result.uri : "");
      }
      wf_agent_post_result_free(&result);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: %s failed (%d)", what, (int) status);
      /* Named so the message says which action failed — "the request failed"
       * on a screen with two buttons is not much help. */
      set_message(r, "Could not %s that post (wolfram status %d).",
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

   SDL_LockMutex(s.lock);
   if (is_like) {
      cobalt_feed_apply_like(&s.feed, in->uri, record);
      cobalt_thread_apply_like(&s.conversation, in->uri, record);
   } else {
      cobalt_feed_apply_repost(&s.feed, in->uri, record);
      cobalt_thread_apply_repost(&s.conversation, in->uri, record);
   }
   SDL_UnlockMutex(s.lock);

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}


static void
run_delete_post(const job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   COBALT_LOGI("session: deleting %s", in->uri);
   wf_status status = wf_agent_delete_post(s.wf, in->uri);
   if (status != WF_OK) {
      COBALT_LOGW("session: delete failed (%d)", (int) status);
      set_message(r, "Could not delete that post (wolfram status %d). It is "
                     "still there.", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   cobalt_feed_remove_post(&s.feed, in->uri);
   cobalt_feed_remove_post(&s.author_feed, in->uri);
   for (int i = 0; i < s.conversation.count; i++) {
      if (strcmp(s.conversation.posts[i].uri, in->uri) == 0) {
         /* The tree's indents and focus no longer line up once a row is gone;
          * the screen returns to where it came from rather than drawing it. */
         cobalt_thread_reset(&s.conversation);
         break;
      }
   }
   SDL_UnlockMutex(s.lock);

   publish_session();

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
   const wf_status st = wf_agent_upload_image_file(s.wf, path, alt, &embed);
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
   const wf_status st = wf_agent_set_reply_gate(s.wf, post_uri, (wf_reply_gate) gate);
   if (st != WF_OK) {
      COBALT_LOGW("session: reply gate failed (%d) for %s", (int) st,
                  post_uri ? post_uri : "(no uri)");
   }
}

static void
run_post_thread(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const char *texts[COBALT_THREAD_POSTS_MAX];
   for (int i = 0; i < in->thread_count; i++) {
      texts[i] = s.thread_texts[i];
   }

   wf_agent_post_result first;
   memset(&first, 0, sizeof(first));
   size_t posted = 0;
   const wf_status status = wf_agent_post_thread(
      s.wf, texts, (size_t) in->thread_count, &posted, &first, NULL);

   COBALT_LOGI("session: thread of %d: %d posted (status %d)", in->thread_count,
               (int) posted, (int) status);
   apply_reply_gate(in->reply_gate, first.uri);
   wf_agent_post_result_free(&first);
   *state = COBALT_AUTH_SIGNED_IN;

   if (status == WF_OK) {
      publish_session();
      r->ok = true;
      return;
   }
   if (posted == 0) {
      set_message(r, "Could not publish that (wolfram status %d). Nothing was "
                     "posted.", (int) status);
      return;
   }
   /* Part of it is public. Say so, and do not leave the draft up to be sent
    * again: that would post the first ones twice. */
   publish_session();
   set_message(r, "Only %d of %d posts went out (wolfram status %d). The rest "
                  "were not sent.", (int) posted, in->thread_count, (int) status);
   r->ok = true;
   r->partial = true;
}

static void
run_post(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (in->thread_count > 1) {
      run_post_thread(in, r, state);
      return;
   }
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   wf_agent_post_result result;
   memset(&result, 0, sizeof(result));

   cJSON *images = NULL;
   if (in->attach_path[0]) {
      images = upload_attachment(in->attach_path, in->attach_alt);
      if (!images) {
         set_message(r, "Could not upload that image. Nothing was posted.");
         *state = COBALT_AUTH_SIGNED_IN;
         return;
      }
   }

   wf_status status;
   if (in->quote) {
      COBALT_LOGI("session: quoting %s", in->uri);
      status = images ? wf_agent_quote_with_media(s.wf, in->text, in->uri,
                                                  in->cid, images, &result)
                      : wf_agent_quote(s.wf, in->text, in->uri, in->cid, &result);
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
                       s.wf, in->text, in->root_uri, in->root_cid, in->uri,
                       in->cid, embed_json, &result);
      free(embed_json);
   } else if (images) {
      char *embed_json = cJSON_PrintUnformatted(images);
      cJSON_Delete(images);
      images = NULL;
      status = embed_json
                  ? wf_agent_post_with_embed(s.wf, in->text, embed_json, &result)
                  : WF_ERR_ALLOC;
      free(embed_json);
   } else {
      COBALT_LOGI("session: posting %d bytes", (int) strlen(in->text));
      status = wf_agent_post(s.wf, in->text, &result);
   }
   /* quote_with_media takes the embed by pointer but not ownership. */
   if (images) {
      cJSON_Delete(images);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: post failed (%d)", (int) status);
      set_message(r, "Could not publish that (wolfram status %d). Nothing was "
                     "posted.", (int) status);
      wf_agent_post_result_free(&result);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   COBALT_LOGI("session: posted %s", result.uri ? result.uri : "(no uri)");

   apply_reply_gate(in->reply_gate, result.uri);

   wf_agent_post_result_free(&result);

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}


static void
run_notifications(const job_input *in, cobalt_job_result *r,
                  cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in to see notifications.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.notifications.cursor[0] ? s.notifications.cursor : NULL;
      SDL_UnlockMutex(s.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_notification_list list;
   memset(&list, 0, sizeof(list));

   COBALT_LOGI("session: listNotifications cursor=%s", cursor ? cursor : "(top)");
   wf_status status = wf_agent_list_notifications_typed(s.wf, TIMELINE_PAGE,
                                                        cursor, &list);
   if (status != WF_OK) {
      COBALT_LOGW("session: listNotifications failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_NOTIFICATIONS);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_notifications_reset(&s.notifications);
   }
   const int added =
      cobalt_notifications_append_from_wolfram(&s.notifications, &list, now);
   if (in->paging && added == 0) {
      s.notifications.has_more = false;
      s.notifications.cursor[0] = '\0';
   }
   const int total = s.notifications.count;
   SDL_UnlockMutex(s.lock);

   wf_agent_notification_list_free(&list);
   COBALT_LOGI("session: notifications +%d (%d held)", added, total);

   if (total == 0) {
      set_message(r, "No notifications yet.");
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
          wf_agent_update_seen_notifications(s.wf, seen_at) != WF_OK) {
         COBALT_LOGW("session: updateSeen failed — the unread badge may linger "
                     "on other clients");
      }
   }

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}


/* Replace s.author_feed with the posts for one profile tab. Failure leaves the
 * feed empty rather than showing another tab's posts under the wrong label. */
static void
fetch_author_feed(const char *actor, int tab, const char *pinned_uri)
{
   wf_agent_feed_list list;
   memset(&list, 0, sizeof(list));

   wf_status status;
   if (tab == COBALT_PROFILE_TAB_LIKES) {
      status = wf_agent_get_actor_likes_typed(s.wf, actor, TIMELINE_PAGE, NULL,
                                              &list);
   } else {
      status = wf_agent_get_author_feed_typed(s.wf, actor, TIMELINE_PAGE, NULL,
                                              cobalt_profile_tab_filter(tab),
                                              &list);
   }

   if (status == WF_OK) {
      const int64_t now = cobalt_time_now();
      SDL_LockMutex(s.lock);
      cobalt_feed_reset(&s.author_feed);
      cobalt_feed_append_from_wolfram(&s.author_feed, &list, now);
      SDL_UnlockMutex(s.lock);
      wf_agent_feed_list_free(&list);

      /* The Posts tab leads with the pinned post, as the official client does.
       * Best-effort: a failure here leaves the ordinary feed untouched. */
      if (pinned_uri && pinned_uri[0] && tab == COBALT_PROFILE_TAB_POSTS) {
         wf_agent_post_list pins;
         memset(&pins, 0, sizeof(pins));
         const char *uris[1] = { pinned_uri };
         if (wf_agent_get_posts_typed(s.wf, uris, 1, &pins) == WF_OK) {
            SDL_LockMutex(s.lock);
            cobalt_feed_pin_from_wolfram(&s.author_feed, &pins, now);
            SDL_UnlockMutex(s.lock);
            wf_agent_post_list_free(&pins);
         }
      }
   } else {
      COBALT_LOGW("session: author feed (tab %d) failed (%d) — showing the "
                  "profile without posts", tab, (int) status);
      SDL_LockMutex(s.lock);
      cobalt_feed_reset(&s.author_feed);
      SDL_UnlockMutex(s.lock);
   }
}

static void
run_profile_tab(const job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }
   SDL_LockMutex(s.lock);
   char pinned[COBALT_POST_URI_MAX];
   snprintf(pinned, sizeof(pinned), "%s", s.pinned_uri);
   SDL_UnlockMutex(s.lock);
   fetch_author_feed(in->uri, in->tab, pinned);
   publish_session();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

static void
run_profile(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   wf_agent_profile profile;
   memset(&profile, 0, sizeof(profile));

   COBALT_LOGI("session: getProfile %s", in->uri);
   wf_status status = wf_agent_get_profile(s.wf, in->uri, &profile);
   if (status != WF_OK) {
      COBALT_LOGW("session: getProfile failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_PROFILE);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   cobalt_profile_from_wolfram(&s.profile, &profile, s.did);
   SDL_UnlockMutex(s.lock);

   char pinned[COBALT_POST_URI_MAX];
   snprintf(pinned, sizeof(pinned), "%s",
            profile.pinned_post_uri ? profile.pinned_post_uri : "");
   SDL_LockMutex(s.lock);
   snprintf(s.pinned_uri, sizeof(s.pinned_uri), "%s", pinned);
   SDL_UnlockMutex(s.lock);
   wf_agent_profile_free(&profile);

   /*
    * The posts are a second request, but the same job. A failure here is not
    * fatal to the screen: the profile itself already loaded and is worth
    * showing, so the feed is left empty and the header stands on its own.
    */
   fetch_author_feed(in->uri, in->tab, pinned);

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

static void
run_follow(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const bool undo = in->record_uri[0] != '\0';
   wf_status status;
   char created[COBALT_POST_URI_MAX] = "";

   if (undo) {
      COBALT_LOGI("session: unfollowing %s", in->record_uri);
      status = wf_agent_unfollow(s.wf, in->record_uri);
   } else {
      wf_agent_post_result result;
      memset(&result, 0, sizeof(result));

      COBALT_LOGI("session: following %s", in->uri);
      status = wf_agent_follow(s.wf, in->uri, &result);
      if (status == WF_OK) {
         snprintf(created, sizeof(created), "%s", result.uri ? result.uri : "");
      }
      wf_agent_post_result_free(&result);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: follow failed (%d)", (int) status);
      set_message(r, "Could not %s (wolfram status %d).",
                  undo ? "unfollow" : "follow", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   cobalt_profile_apply_follow(&s.profile, undo ? NULL : created);
   SDL_UnlockMutex(s.lock);

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

static void
run_mute(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const bool mute = in->flag;
   COBALT_LOGI("session: %s %s", mute ? "muting" : "unmuting", in->uri);
   const wf_status status = mute ? wf_agent_mute_actor(s.wf, in->uri)
                                 : wf_agent_unmute_actor(s.wf, in->uri);

   if (status != WF_OK) {
      COBALT_LOGW("session: %s failed (%d)", mute ? "mute" : "unmute",
                  (int) status);
      set_message(r, "Could not %s (wolfram status %d).",
                  mute ? "mute" : "unmute", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   /* Only the loaded profile's own state, if this is the account it's
    * about — the same call also fires from the muted-accounts list, where
    * `in->uri` names whichever row was unmuted, not necessarily whoever's
    * profile (if any) happens to be loaded. */
   if (s.profile.loaded && strcmp(s.profile.did, in->uri) == 0) {
      cobalt_profile_apply_mute(&s.profile, mute);
   }
   if (!mute) {
      cobalt_actor_list_remove(&s.muted, in->uri);
   }
   SDL_UnlockMutex(s.lock);

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

static void
run_block(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const bool undo = in->record_uri[0] != '\0';
   wf_status status;
   char created[COBALT_POST_URI_MAX] = "";

   if (undo) {
      COBALT_LOGI("session: unblocking %s", in->record_uri);
      status = wf_agent_unblock(s.wf, in->record_uri);
   } else {
      wf_agent_post_result result;
      memset(&result, 0, sizeof(result));

      COBALT_LOGI("session: blocking %s", in->uri);
      status = wf_agent_block(s.wf, in->uri, &result);
      if (status == WF_OK) {
         snprintf(created, sizeof(created), "%s", result.uri ? result.uri : "");
      }
      wf_agent_post_result_free(&result);
   }

   if (status != WF_OK) {
      COBALT_LOGW("session: block failed (%d)", (int) status);
      set_message(r, "Could not %s (wolfram status %d).",
                  undo ? "unblock" : "block", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   if (s.profile.loaded && strcmp(s.profile.did, in->uri) == 0) {
      cobalt_profile_apply_block(&s.profile, undo ? NULL : created);
   }
   if (undo) {
      cobalt_actor_list_remove(&s.blocked, in->uri);
   }
   SDL_UnlockMutex(s.lock);

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* Shared by run_muted_list/run_blocked_list — the two calls are identical
 * apart from which Wolfram wrapper to call and which list to fill. */
static void
run_actor_list(const job_input *in, cobalt_job_result *r,
               cobalt_auth_state *state, cobalt_actor_list *list,
               wf_status (*fetch)(wf_agent *, const char *, int, const char *,
                                  wf_agent_actor_list *),
               const char *empty_message)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = list->cursor[0] ? list->cursor : NULL;
      SDL_UnlockMutex(s.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_actor_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      fetch(s.wf, in->uri[0] ? in->uri : NULL, TIMELINE_PAGE, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: actor list fetch failed (%d)", (int) status);
      set_message(r, "Could not load the list (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(list);
   }
   const int added = cobalt_actor_list_append_from_wolfram(list, &wf_list);
   if (in->paging && added == 0) {
      list->has_more = false;
      list->cursor[0] = '\0';
   }
   const int total = list->count;
   SDL_UnlockMutex(s.lock);

   wf_agent_actor_list_free(&wf_list);
   COBALT_LOGI("session: actor list +%d (%d held)", added, total);

   if (total == 0) {
      set_message(r, "%s", empty_message);
   }

   publish_session();

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

static void
run_muted_list(const job_input *in, cobalt_job_result *r,
               cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &s.muted, fetch_mutes,
                  "No muted accounts.");
}

static void
run_blocked_list(const job_input *in, cobalt_job_result *r,
                 cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &s.blocked, fetch_blocks,
                  "No blocked accounts.");
}

static void
run_followers(const job_input *in, cobalt_job_result *r,
              cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &s.followers, wf_agent_get_followers_typed,
                  "No followers yet.");
}

static void
run_following(const job_input *in, cobalt_job_result *r,
              cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &s.following, wf_agent_get_follows_typed,
                  "Not following anyone.");
}

/* getRepostedBy is the same shape as the followers fetches — a plain actor
 * list keyed by a URI instead of a DID — so it rides run_actor_list. */
static void
run_reposted_by(const job_input *in, cobalt_job_result *r,
                cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &s.likes, wf_agent_get_reposted_by_typed,
                  "No reposts yet.");
}

/* getLikes is not: Wolfram hands back wf_agent_actor_like_list, whose rows
 * carry the actor one level down, so it gets its own run rather than a
 * fetch_fn that would have to lie about the output type. */
static void
run_likes(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.likes.cursor[0] ? s.likes.cursor : NULL;
      SDL_UnlockMutex(s.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_actor_like_list wf_likes;
   memset(&wf_likes, 0, sizeof(wf_likes));

   const wf_status status =
      wf_agent_get_likes_typed(s.wf, in->uri, TIMELINE_PAGE, cursor, &wf_likes);
   if (status != WF_OK) {
      COBALT_LOGW("session: getLikes failed (%d)", (int) status);
      set_message(r, "Could not load the likes (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(&s.likes);
   }
   int added = 0;
   for (size_t i = 0; i < wf_likes.like_count; i++) {
      if (s.likes.count >= COBALT_ACTORS_MAX) {
         COBALT_LOGI("session: likes window full at %d, dropping the rest",
                      s.likes.count);
         break;
      }
      const wf_agent_profile_view *src = &wf_likes.likes[i].actor;
      cobalt_actor *out = &s.likes.actors[s.likes.count];
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
      s.likes.count++;
      added++;
   }
   if (s.likes.count >= COBALT_ACTORS_MAX) {
      s.likes.cursor[0] = '\0';
      s.likes.has_more = false;
   } else if (wf_likes.cursor && wf_likes.cursor[0]) {
      snprintf(s.likes.cursor, sizeof(s.likes.cursor), "%s", wf_likes.cursor);
      s.likes.has_more = true;
   } else {
      s.likes.cursor[0] = '\0';
      s.likes.has_more = false;
   }
   const int total = s.likes.count;
   SDL_UnlockMutex(s.lock);

   wf_agent_actor_like_list_free(&wf_likes);
   COBALT_LOGI("session: likes +%d (%d held)", added, total);

   if (total == 0) {
      set_message(r, "No likes yet.");
   }

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* Not routed through run_actor_list — searchActors takes a query string on
 * top of limit/cursor, so its Wolfram wrapper has a different shape than the
 * mutes/blocks fetchers run_actor_list is built around. A fresh (non-paging)
 * search always resets the held list, even to empty on a bad query, since a
 * stale result from the previous query left on screen would look like a
 * match for the new one. */
static void
run_search_posts(const job_input *in, cobalt_job_result *r,
                 cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in to search.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.feed.cursor[0] ? s.feed.cursor : NULL;
      SDL_UnlockMutex(s.lock);
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
   wf_status status = wf_agent_search_posts_typed(s.wf, in->text, TIMELINE_PAGE,
                                                  cursor, &list, &next);
   if (status != WF_OK) {
      COBALT_LOGW("session: searchPosts failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_SEARCH_POSTS);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const int64_t now = cobalt_time_now();

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_feed_reset(&s.feed);
   }
   const int added = cobalt_feed_append_posts_from_wolfram(&s.feed, &list, next, now);
   if (in->paging && added == 0) {
      s.feed.has_more = false;
      s.feed.cursor[0] = '\0';
   }
   const int total = s.feed.count;
   SDL_UnlockMutex(s.lock);

   free(next);
   wf_agent_post_list_free(&list);

   COBALT_LOGI("session: search +%d posts (%d held)", added, total);
   if (total == 0) {
      set_message(r, "No posts matched that search.");
   }

   publish_session();
   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

static void
run_search_actors(const job_input *in, cobalt_job_result *r,
                  cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   if (!in->paging && in->text[0] == '\0') {
      SDL_LockMutex(s.lock);
      cobalt_actor_list_reset(&s.search);
      s.search.has_more = false;
      s.search.cursor[0] = '\0';
      SDL_UnlockMutex(s.lock);
      *state = COBALT_AUTH_SIGNED_IN;
      r->ok = true;
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.search.cursor[0] ? s.search.cursor : NULL;
      SDL_UnlockMutex(s.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_actor_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_search_actors_typed(s.wf, in->text, TIMELINE_PAGE, cursor,
                                   &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: actor search failed (%d)", (int) status);
      set_message(r, "Search failed (wolfram status %d).", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(&s.search);
   }
   const int added = cobalt_actor_list_append_from_wolfram(&s.search, &wf_list);
   if (in->paging && added == 0) {
      s.search.has_more = false;
      s.search.cursor[0] = '\0';
   }
   const int total = s.search.count;
   SDL_UnlockMutex(s.lock);

   wf_agent_actor_list_free(&wf_list);
   COBALT_LOGI("session: search '%s' +%d (%d held)", in->text, added, total);

   if (total == 0) {
      set_message(r, "No accounts found for \"%s\".", in->text);
   }

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* The signed-in account's own lists (app.bsky.graph.getLists). */
static void
run_lists(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.lists.cursor[0] ? s.lists.cursor : NULL;
      SDL_UnlockMutex(s.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_list_view_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_get_lists_typed(s.wf, s.did, TIMELINE_PAGE, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: lists fetch failed (%d)", (int) status);
      set_message(r, "Could not load your lists (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_list_summary_list_reset(&s.lists);
   }
   const int added =
      cobalt_list_summary_list_append_from_wolfram(&s.lists, &wf_list);
   if (in->paging && added == 0) {
      s.lists.has_more = false;
      s.lists.cursor[0] = '\0';
   }
   const int total = s.lists.count;
   SDL_UnlockMutex(s.lock);

   wf_agent_list_view_list_free(&wf_list);
   COBALT_LOGI("session: lists +%d (%d held)", added, total);

   if (total == 0) {
      set_message(r, "You haven't made any lists yet.");
   }

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/*
 * The account's saved custom feeds. savedFeedsPrefV2 items of type "feed" carry
 * the generator's AT-URI in `value`; getFeedGenerators supplies display names.
 * Falls back to the URI's record key when a generator can't be resolved, so a
 * feed is never silently dropped from the picker.
 */
static void
run_saved_feeds(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   (void) in;
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   /* Read the raw preferences rather than the strict typed parse: a single
    * preference type the parser rejects (status 5 on a real account) must not
    * take the whole feed picker down with it. */
   char *prefs_json = NULL;
   const wf_status status = wf_agent_get_preferences(s.wf, &prefs_json);
   if (status != WF_OK || !prefs_json) {
      COBALT_LOGW("session: getPreferences failed (%d)", (int) status);
      set_message(r, "Could not load your saved feeds (wolfram status %d).", (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }
   cJSON *prefs = cJSON_Parse(prefs_json);
   free(prefs_json);
   if (!cJSON_IsArray(prefs)) {
      COBALT_LOGW("session: getPreferences returned something other than an array");
      cJSON_Delete(prefs);
      set_message(r, "Could not read your saved feeds.");
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   const char *uris[COBALT_SAVED_FEEDS_MAX];
   int n = 0;
   const cJSON *pref = NULL;
   cJSON_ArrayForEach(pref, prefs) {
      const cJSON *type = cJSON_GetObjectItemCaseSensitive(pref, "$type");
      if (!cJSON_IsString(type) || !strstr(type->valuestring, "savedFeedsPrefV2")) {
         continue;
      }
      const cJSON *items = cJSON_GetObjectItemCaseSensitive(pref, "items");
      const cJSON *it = NULL;
      cJSON_ArrayForEach(it, items) {
         const cJSON *kind = cJSON_GetObjectItemCaseSensitive(it, "type");
         const cJSON *value = cJSON_GetObjectItemCaseSensitive(it, "value");
         if (n < COBALT_SAVED_FEEDS_MAX && cJSON_IsString(kind) && cJSON_IsString(value) &&
             strcmp(kind->valuestring, "feed") == 0 && value->valuestring[0]) {
            uris[n++] = value->valuestring;
         }
      }
   }
   if (n == 0) {
      /* Older accounts only have the V1 list. */
      cJSON_ArrayForEach(pref, prefs) {
         const cJSON *type = cJSON_GetObjectItemCaseSensitive(pref, "$type");
         if (!cJSON_IsString(type) || !strstr(type->valuestring, "savedFeedsPref")) {
            continue;
         }
         const cJSON *saved = cJSON_GetObjectItemCaseSensitive(pref, "saved");
         const cJSON *it = NULL;
         cJSON_ArrayForEach(it, saved) {
            if (n < COBALT_SAVED_FEEDS_MAX && cJSON_IsString(it) && it->valuestring[0]) {
               uris[n++] = it->valuestring;
            }
         }
      }
   }

   wf_feedgen_generator_list gens;
   memset(&gens, 0, sizeof(gens));
   if (n > 0 && wf_feedgen_get_feed_generators_typed(s.wf, uris, (size_t) n, &gens) != WF_OK) {
      COBALT_LOGW("session: getFeedGenerators failed; using record keys as names");
      memset(&gens, 0, sizeof(gens));
   }

   SDL_LockMutex(s.lock);
   memset(&s.saved_feeds, 0, sizeof(s.saved_feeds));
   for (int i = 0; i < n; i++) {
      const char *name = NULL;
      for (size_t g = 0; g < gens.generator_count; g++) {
         if (gens.generators[g].uri && strcmp(gens.generators[g].uri, uris[i]) == 0) {
            name = gens.generators[g].display_name;
            break;
         }
      }
      if (!name || !name[0]) {
         const char *slash = strrchr(uris[i], '/');
         name = slash ? slash + 1 : uris[i];
      }
      const int k = s.saved_feeds.count++;
      snprintf(s.saved_feeds.feeds[k].label, sizeof(s.saved_feeds.feeds[k].label), "%s", name);
      snprintf(s.saved_feeds.feeds[k].uri, sizeof(s.saved_feeds.feeds[k].uri), "%s", uris[i]);
   }
   const int total = s.saved_feeds.count;
   SDL_UnlockMutex(s.lock);

   wf_feedgen_generator_list_free(&gens);
   cJSON_Delete(prefs);
   COBALT_LOGI("session: %d saved feeds", total);

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

/* One list's members (app.bsky.graph.getList). `in->uri` is the list's AT
 * URI, set by cobalt_session_begin_list_members. */
static void
run_list_members(const job_input *in, cobalt_job_result *r,
                 cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   const char *cursor = NULL;
   if (in->paging) {
      SDL_LockMutex(s.lock);
      cursor = s.list_members.cursor[0] ? s.list_members.cursor : NULL;
      SDL_UnlockMutex(s.lock);
      if (!cursor) {
         *state = COBALT_AUTH_SIGNED_IN;
         r->ok = true;
         return;
      }
   }

   wf_agent_list_item_list wf_list;
   memset(&wf_list, 0, sizeof(wf_list));

   const wf_status status =
      wf_agent_get_list_typed(s.wf, in->uri, TIMELINE_PAGE, cursor, &wf_list);
   if (status != WF_OK) {
      COBALT_LOGW("session: list members fetch failed (%d)", (int) status);
      set_message(r, "Could not load that list (wolfram status %d).",
                  (int) status);
      *state = COBALT_AUTH_SIGNED_IN;
      return;
   }

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_actor_list_reset(&s.list_members);
   }
   const int added = cobalt_actor_list_append_from_wolfram_list_items(
      &s.list_members, &wf_list);
   if (in->paging && added == 0) {
      s.list_members.has_more = false;
      s.list_members.cursor[0] = '\0';
   }
   const int total = s.list_members.count;
   SDL_UnlockMutex(s.lock);

   wf_agent_list_item_list_free(&wf_list);
   COBALT_LOGI("session: list members +%d (%d held)", added, total);

   if (total == 0) {
      set_message(r, "This list has no members.");
   }

   publish_session();

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
}

static void
run_logout(cobalt_job_result *r, cobalt_auth_state *state)
{
   if (s.wf) {
      /* Best effort. Wolfram clears the local session either way, and a user
       * who asked to sign out must end up signed out even if the PDS is
       * unreachable — so a failure here is logged, not surfaced. */
      wf_status status = wf_agent_logout(s.wf);
      if (status != WF_OK) {
         COBALT_LOGW("session: deleteSession failed (%d) — clearing locally anyway",
                     (int) status);
      }
      teardown_wf();
   }

   cobalt_session_store_clear();

   SDL_LockMutex(s.lock);
   s.handle[0] = '\0';
   s.did[0] = '\0';
   /* The feed belongs to the account that just signed out. */
   cobalt_feed_reset(&s.feed);
   s.feed.cursor[0] = '\0';
   s.feed.has_more = false;
   cobalt_thread_reset(&s.conversation);
   cobalt_notifications_reset(&s.notifications);
   cobalt_profile_reset(&s.profile);
   cobalt_feed_reset(&s.author_feed);
   cobalt_actor_list_reset(&s.muted);
   cobalt_actor_list_reset(&s.blocked);
   cobalt_actor_list_reset(&s.followers);
   cobalt_actor_list_reset(&s.following);
   s.follow_actor[0] = '\0';
   cobalt_actor_list_reset(&s.search);
   SDL_UnlockMutex(s.lock);

   *state = COBALT_AUTH_SIGNED_OUT;
   r->ok = true;
   COBALT_LOGI("session: signed out");
}

#endif /* COBALT_HAS_WOLFRAM */

/*
 * Runs off the frame loop. `state` carries the auth state in and out rather
 * than being poked directly, so the only writer of the shared copy is the
 * caller, under the lock.
 */
static cobalt_job_result
run_job(cobalt_job_kind kind, const job_input *in, cobalt_auth_state *state)
{
   cobalt_job_result r;
   memset(&r, 0, sizeof(r));
   r.kind = kind;

#ifdef COBALT_HAS_WOLFRAM
   switch (kind) {
      case COBALT_JOB_LOGIN:  run_login(in, &r, state);  break;
      case COBALT_JOB_OAUTH:  run_oauth(in, &r, state);  break;
      case COBALT_JOB_RESUME: run_resume(&r, state);     break;
      case COBALT_JOB_LOGOUT: run_logout(&r, state);     break;
      case COBALT_JOB_TIMELINE: run_timeline(in, &r, state); break;
      case COBALT_JOB_THREAD:   run_thread(in, &r, state);   break;
      case COBALT_JOB_LIKE:     run_interaction(in, &r, state, true);  break;
      case COBALT_JOB_REPOST:   run_interaction(in, &r, state, false); break;
      case COBALT_JOB_POST:     run_post(in, &r, state);      break;
      case COBALT_JOB_DELETE_POST: run_delete_post(in, &r, state); break;
      case COBALT_JOB_NOTIFICATIONS:
         run_notifications(in, &r, state);
         break;
      case COBALT_JOB_PROFILE:  run_profile(in, &r, state);   break;
      case COBALT_JOB_FOLLOW:   run_follow(in, &r, state);    break;
      case COBALT_JOB_MUTE:     run_mute(in, &r, state);      break;
      case COBALT_JOB_BLOCK:    run_block(in, &r, state);     break;
      case COBALT_JOB_MUTED_LIST:   run_muted_list(in, &r, state);   break;
      case COBALT_JOB_BLOCKED_LIST: run_blocked_list(in, &r, state); break;
      case COBALT_JOB_PROFILE_TAB:  run_profile_tab(in, &r, state); break;
      case COBALT_JOB_FOLLOWERS:    run_followers(in, &r, state);    break;
      case COBALT_JOB_FOLLOWING:    run_following(in, &r, state);    break;
      case COBALT_JOB_LIKES:        run_likes(in, &r, state);        break;
      case COBALT_JOB_REPOSTED_BY:  run_reposted_by(in, &r, state); break;
      case COBALT_JOB_SEARCH_ACTORS: run_search_actors(in, &r, state); break;
      case COBALT_JOB_FEED:     run_feed(in, &r, state);      break;
      case COBALT_JOB_SEARCH_POSTS: run_search_posts(in, &r, state); break;
      case COBALT_JOB_LISTS:        run_lists(in, &r, state);        break;
      case COBALT_JOB_SAVED_FEEDS:  run_saved_feeds(in, &r, state);  break;
      case COBALT_JOB_LIST_MEMBERS: run_list_members(in, &r, state); break;
      case COBALT_JOB_NONE:
      default:                                           break;
   }
#else
   (void) in;
   (void) state;
   set_message(&r, "This build has no ATProto SDK. Build Wolfram for Wii U "
                   "alongside Cobalt — see the README.");
#endif

   return r;
}

/* --- worker --- */

static int
worker_main(void *unused)
{
   (void) unused;
   cobalt_thread_make_background();

   SDL_LockMutex(s.lock);
   for (;;) {
      while (!s.stop && s.pending == COBALT_JOB_NONE) {
         SDL_CondWait(s.wake, s.lock);
      }
      if (s.stop) {
         break;
      }

      const cobalt_job_kind kind = s.pending;
      job_input in = s.input;
      cobalt_auth_state state = s.resting_state;
      s.pending = COBALT_JOB_NONE;
      memset(&s.input, 0, sizeof(s.input));
      SDL_UnlockMutex(s.lock);

      /* The lock is deliberately not held across the network call: the UI
       * thread polls state every frame and must not block behind curl. */
      const uint32_t job_t0 = SDL_GetTicks();
      cobalt_job_result result = run_job(kind, &in, &state);
      COBALT_LOGI("session: job %d took %u ms (%s)", (int) kind,
                  (unsigned) (SDL_GetTicks() - job_t0), result.ok ? "ok" : "failed");
      memset(&in, 0, sizeof(in));

      SDL_LockMutex(s.lock);
      s.result = result;
      s.have_result = true;
      s.busy = false;
      s.state = state;
   }
   SDL_UnlockMutex(s.lock);

   COBALT_LOGI("session: worker thread exiting");
   return 0;
}

/* --- lifecycle --- */

static void
resolve_ca_bundle(void)
{
   if (!cobalt_content_path(s.ca_path, sizeof(s.ca_path), CA_BUNDLE_FILE)) {
      COBALT_LOGE("session: no content root, so no TLS trust store");
      s.ca_path[0] = '\0';
      return;
   }

   FILE *f = fopen(s.ca_path, "rb");
   if (!f) {
      COBALT_LOGE("session: %s is missing — run `make cacert` and rebuild. "
                  "Without it every HTTPS request will fail verification, "
                  "because the Wii U has no system certificate store.",
                  s.ca_path);
      return;
   }
   fclose(f);

   s.have_ca = true;
   COBALT_LOGI("session: TLS trust store at %s", s.ca_path);
}

bool
cobalt_session_init(void)
{
   memset(&s, 0, sizeof(s));

   resolve_ca_bundle();
   load_post_lang();

   s.lock = SDL_CreateMutex();
   s.wake = SDL_CreateCond();
   if (!s.lock || !s.wake) {
      COBALT_LOGE("session: could not create synchronisation primitives: %s",
                  SDL_GetError());
      /* cobalt_session_shutdown() bails on an uninitialised module, so whatever
       * did get created has to be released here. */
      if (s.wake) {
         SDL_DestroyCond(s.wake);
         s.wake = NULL;
      }
      if (s.lock) {
         SDL_DestroyMutex(s.lock);
         s.lock = NULL;
      }
      s.blocker = "threading unavailable";
      return false;
   }

   s.thread = SDL_CreateThread(worker_main, "cobalt-net", NULL);
   if (!s.thread) {
      /*
       * Not fatal. Requests fall back to running on the calling thread, which
       * works but freezes the frame during a request. Reported on the
       * diagnostics screen so a hardware run can tell this apart from a hang.
       */
      COBALT_LOGE("session: SDL_CreateThread failed (%s) — network calls will "
                  "block the frame loop", SDL_GetError());
   }

   s.initialised = true;

   /*
    * Fail closed on anything that would leave a request weakly protected.
    * Order is deliberate — the first blocker found is the one reported, and
    * these run most-fundamental first.
    *
    * The entropy check is the one worth spelling out. Without a provisioned
    * seed, libcurl's mbedTLS falls back to devkitPro's tick-derived poll for
    * every client random and ephemeral key. Cobalt could still complete a
    * handshake in that state, and it would look completely normal to the
    * person using it, which is exactly why it must not.
    */
#ifndef COBALT_HAS_WOLFRAM
   s.blocker = "Wolfram not built in";
#else
   if (!s.have_ca) {
      s.blocker = "no TLS trust store";
   } else if (!cobalt_rng_ready()) {
      s.blocker = "no entropy seed";
   } else if (!wf_xrpc_tls_rng_supported()) {
      /* Wolfram compiles the hook for Wii U and checks libcurl's backend at
       * runtime, so this means curl is not the mbedTLS build it was linked
       * against — in which case the handshake RNG cannot be replaced. */
      s.blocker = "TLS RNG hook unavailable";
   }
#endif

   snprintf(s.service, sizeof(s.service), "%s", DEFAULT_SERVICE);

   COBALT_LOGI("session: up (threaded=%d, ca=%d, blocker=%s)",
               (int) (s.thread != NULL), (int) s.have_ca,
               s.blocker ? s.blocker : "none");
   return s.blocker == NULL;
}

void
cobalt_session_shutdown(void)
{
   if (!s.initialised) {
      return;
   }

   if (s.thread) {
      SDL_LockMutex(s.lock);
      s.stop = true;
      SDL_CondSignal(s.wake);
      const bool in_flight = s.busy;
      SDL_UnlockMutex(s.lock);

      if (in_flight) {
         /* curl has no cancellation here, so this waits out whatever is on the
          * wire. Logged because it is the one place shutdown can visibly take
          * seconds. */
         COBALT_LOGI("session: waiting for an in-flight request before exit");
      }

      SDL_WaitThread(s.thread, NULL);
      s.thread = NULL;
   }

#ifdef COBALT_HAS_WOLFRAM
   teardown_wf();
#endif

   if (s.wake) {
      SDL_DestroyCond(s.wake);
      s.wake = NULL;
   }
   if (s.lock) {
      SDL_DestroyMutex(s.lock);
      s.lock = NULL;
   }

   memset(&s, 0, sizeof(s));
}

/* --- request submission --- */

static bool
submit(cobalt_job_kind kind, const job_input *in)
{
   if (!s.initialised) {
      return false;
   }

   SDL_LockMutex(s.lock);
   if (s.busy) {
      SDL_UnlockMutex(s.lock);
      COBALT_LOGW("session: a request is already in flight");
      return false;
   }

   s.busy = true;
   s.resting_state = s.state;
   s.state = COBALT_AUTH_WORKING;

   if (s.thread) {
      s.pending = kind;
      s.input = *in;
      SDL_CondSignal(s.wake);
      SDL_UnlockMutex(s.lock);
      return true;
   }

   /* No worker thread: run it here. The frame loop stalls for the duration,
    * which is bad but strictly better than silently doing nothing. */
   cobalt_auth_state state = s.resting_state;
   SDL_UnlockMutex(s.lock);

   job_input local = *in;
   cobalt_job_result result = run_job(kind, &local, &state);
   memset(&local, 0, sizeof(local));

   SDL_LockMutex(s.lock);
   s.result = result;
   s.have_result = true;
   s.busy = false;
   s.state = state;
   SDL_UnlockMutex(s.lock);
   return true;
}

bool
cobalt_session_begin_login(const char *service, const char *identifier,
                           const char *password)
{
   if (!identifier || !identifier[0] || !password || !password[0]) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));

   if (!cobalt_session_normalise_service(service, in.service, sizeof(in.service))) {
      COBALT_LOGW("session: service URL too long");
      return false;
   }
   snprintf(in.identifier, sizeof(in.identifier), "%s", identifier);
   snprintf(in.password, sizeof(in.password), "%s", password);

   bool ok = submit(COBALT_JOB_LOGIN, &in);
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
   available = s.initialised;
#endif
   if (!oauth_node || !oauth_node[0] || !handle || !handle[0] ||
       cobalt_session_busy() || !available) {
      return false;
   }

   char service[COBALT_SERVICE_MAX];
   if (!cobalt_session_normalise_service(oauth_node, service, sizeof(service))) {
      return false;
   }

   SDL_LockMutex(s.lock);
   if (s.busy) {
      SDL_UnlockMutex(s.lock);
      return false;
   }
   memset(&s.input, 0, sizeof(s.input));
   snprintf(s.input.service, sizeof(s.input.service), "%s", service);
   snprintf(s.input.identifier, sizeof(s.input.identifier), "%s", handle);
   s.pending = COBALT_JOB_OAUTH;
   s.busy = true;
   s.have_result = false;
   s.resting_state = s.state;
   s.state = COBALT_AUTH_WORKING;
   s.pair_url[0] = '\0';
   s.pair_code[0] = '\0';
   SDL_CondSignal(s.wake);
   SDL_UnlockMutex(s.lock);
   return true;
}


bool
cobalt_session_begin_resume(void)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   return submit(COBALT_JOB_RESUME, &in);
}

bool
cobalt_session_begin_logout(void)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   return submit(COBALT_JOB_LOGOUT, &in);
}

bool
cobalt_session_begin_timeline(bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   if (!paging) {
      s.feed_source = 0;
      s.feed_arg[0] = '\0';
   }
   return submit(COBALT_JOB_TIMELINE, &in);
}

bool
cobalt_session_begin_feed(const char *feed_uri, bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", feed_uri ? feed_uri : "");
   in.paging = paging;
   if (!paging) {
      s.feed_source = 1;
      snprintf(s.feed_arg, sizeof(s.feed_arg), "%s", in.uri);
   }
   return submit(COBALT_JOB_FEED, &in);
}

bool
cobalt_session_begin_search_posts(const char *query, bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   snprintf(in.text, sizeof(in.text), "%s", query ? query : "");
   if (!paging) {
      s.feed_source = 2;
      snprintf(s.feed_arg, sizeof(s.feed_arg), "%s", in.text);
   }
   return submit(COBALT_JOB_SEARCH_POSTS, &in);
}

bool
cobalt_session_begin_feed_current(bool paging)
{
   switch (s.feed_source) {
      case 1: return cobalt_session_begin_feed(s.feed_arg, paging);
      case 2: return cobalt_session_begin_search_posts(s.feed_arg, paging);
      default: return cobalt_session_begin_timeline(paging);
   }
}

const cobalt_feed *
cobalt_session_feed(void)
{
   return &s.feed;
}

const cobalt_thread *
cobalt_session_thread(void)
{
   return &s.conversation;
}

const cobalt_notifications *
cobalt_session_notifications(void)
{
   return &s.notifications;
}

const cobalt_profile *
cobalt_session_profile(void)
{
   return &s.profile;
}

const cobalt_feed *
cobalt_session_author_feed(void)
{
   return &s.author_feed;
}

bool
cobalt_session_begin_profile(const char *actor)
{
   if (!actor || !actor[0]) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", actor);
   in.tab = COBALT_PROFILE_TAB_POSTS;
   SDL_LockMutex(s.lock);
   s.profile_tab = COBALT_PROFILE_TAB_POSTS;
   SDL_UnlockMutex(s.lock);
   return submit(COBALT_JOB_PROFILE, &in);
}

bool
cobalt_session_begin_profile_tab(int tab)
{
   if (tab < 0 || tab >= COBALT_PROFILE_TAB_COUNT || !s.profile.loaded ||
       !s.profile.did[0]) {
      return false;
   }
   if (tab == COBALT_PROFILE_TAB_LIKES && !s.profile.is_self) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", s.profile.did);
   in.tab = tab;
   SDL_LockMutex(s.lock);
   s.profile_tab = tab;
   SDL_UnlockMutex(s.lock);
   return submit(COBALT_JOB_PROFILE_TAB, &in);
}

int
cobalt_session_profile_tab(void)
{
   return s.profile_tab;
}

bool
cobalt_session_begin_follow(void)
{
   job_input in;
   memset(&in, 0, sizeof(in));

   SDL_LockMutex(s.lock);
   const bool have = s.profile.loaded && !s.profile.is_self && s.profile.did[0];
   if (have) {
      snprintf(in.uri, sizeof(in.uri), "%s", s.profile.did);
      snprintf(in.record_uri, sizeof(in.record_uri), "%s",
               s.profile.viewer_following);
   }
   SDL_UnlockMutex(s.lock);

   /* Nothing loaded, or it is the signed-in account — which has no follow
    * button, so this should not have been reachable. */
   if (!have) {
      return false;
   }
   return submit(COBALT_JOB_FOLLOW, &in);
}

bool
cobalt_session_begin_mute(void)
{
   job_input in;
   memset(&in, 0, sizeof(in));

   SDL_LockMutex(s.lock);
   const bool have = s.profile.loaded && !s.profile.is_self && s.profile.did[0];
   if (have) {
      snprintf(in.uri, sizeof(in.uri), "%s", s.profile.did);
      in.flag = !s.profile.viewer_muted;   /* toggle: mute if not muted */
   }
   SDL_UnlockMutex(s.lock);

   if (!have) {
      return false;
   }
   return submit(COBALT_JOB_MUTE, &in);
}

bool
cobalt_session_begin_block(void)
{
   job_input in;
   memset(&in, 0, sizeof(in));

   SDL_LockMutex(s.lock);
   const bool have = s.profile.loaded && !s.profile.is_self && s.profile.did[0];
   if (have) {
      snprintf(in.uri, sizeof(in.uri), "%s", s.profile.did);
      snprintf(in.record_uri, sizeof(in.record_uri), "%s",
               s.profile.viewer_blocking);
   }
   SDL_UnlockMutex(s.lock);

   if (!have) {
      return false;
   }
   return submit(COBALT_JOB_BLOCK, &in);
}

const cobalt_actor_list *
cobalt_session_muted_list(void)
{
   return &s.muted;
}

const cobalt_actor_list *
cobalt_session_blocked_list(void)
{
   return &s.blocked;
}

bool
cobalt_session_begin_muted_list(bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return submit(COBALT_JOB_MUTED_LIST, &in);
}

static bool
begin_follow_list(cobalt_job_kind kind, cobalt_actor_list *list,
                  const char *actor, bool paging)
{
   if (!actor || actor[0] == '\0') {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   snprintf(in.uri, sizeof(in.uri), "%s", actor);

   if (!paging) {
      SDL_LockMutex(s.lock);
      cobalt_actor_list_reset(list);
      snprintf(s.follow_actor, sizeof(s.follow_actor), "%s", actor);
      SDL_UnlockMutex(s.lock);
   }
   return submit(kind, &in);
}

/* Likes/reposted-by: same shape as begin_follow_list but keyed by a post
 * URI, and the two kinds share one list so there is no list argument. */
static bool
begin_likes_list(cobalt_job_kind kind, const char *uri, bool paging)
{
   if (!uri || uri[0] == '\0') {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   snprintf(in.uri, sizeof(in.uri), "%s", uri);

   if (!paging) {
      SDL_LockMutex(s.lock);
      cobalt_actor_list_reset(&s.likes);
      snprintf(s.likes_uri, sizeof(s.likes_uri), "%s", uri);
      SDL_UnlockMutex(s.lock);
   }
   return submit(kind, &in);
}

bool
cobalt_session_begin_followers(const char *actor, bool paging)
{
   return begin_follow_list(COBALT_JOB_FOLLOWERS, &s.followers, actor, paging);
}

bool
cobalt_session_begin_following(const char *actor, bool paging)
{
   return begin_follow_list(COBALT_JOB_FOLLOWING, &s.following, actor, paging);
}

const cobalt_actor_list *
cobalt_session_followers_list(void)
{
   return &s.followers;
}

const cobalt_actor_list *
cobalt_session_following_list(void)
{
   return &s.following;
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
   return &s.likes;
}

const char *
cobalt_session_follow_list_actor(void)
{
   return s.follow_actor;
}

bool
cobalt_session_begin_blocked_list(bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return submit(COBALT_JOB_BLOCKED_LIST, &in);
}

const cobalt_actor_list *
cobalt_session_search_results(void)
{
   return &s.search;
}

bool
cobalt_session_begin_search_actors(const char *query, bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   if (query) {
      snprintf(in.text, sizeof(in.text), "%s", query);
   }
   return submit(COBALT_JOB_SEARCH_ACTORS, &in);
}

const cobalt_list_summary_list *
cobalt_session_lists(void)
{
   return &s.lists;
}

const cobalt_saved_feeds *
cobalt_session_saved_feeds(void)
{
   return &s.saved_feeds;
}

bool
cobalt_session_begin_saved_feeds(void)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   return submit(COBALT_JOB_SAVED_FEEDS, &in);
}

bool
cobalt_session_begin_lists(bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return submit(COBALT_JOB_LISTS, &in);
}

const cobalt_actor_list *
cobalt_session_list_members(void)
{
   return &s.list_members;
}

bool
cobalt_session_begin_list_members(const char *list_uri, bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", list_uri ? list_uri : "");
   in.paging = paging;
   return submit(COBALT_JOB_LIST_MEMBERS, &in);
}

bool
cobalt_session_begin_unmute_actor(const char *did)
{
   if (!did || !did[0]) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", did);
   in.flag = false;   /* always undo — every row on this list is muted */
   return submit(COBALT_JOB_MUTE, &in);
}

bool
cobalt_session_begin_unblock_actor(const char *record_uri, const char *did)
{
   if (!record_uri || !record_uri[0]) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.record_uri, sizeof(in.record_uri), "%s", record_uri);
   /* Not used to decide undo (record_uri already does that) — carried so
    * run_block can drop the right row from s.blocked locally. */
   snprintf(in.uri, sizeof(in.uri), "%s", did ? did : "");
   return submit(COBALT_JOB_BLOCK, &in);
}

bool
cobalt_session_begin_notifications(bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return submit(COBALT_JOB_NOTIFICATIONS, &in);
}

bool
cobalt_session_begin_thread(const char *uri)
{
   if (!uri || !uri[0]) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", uri);
   return submit(COBALT_JOB_THREAD, &in);
}

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

   job_input in;
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
   SDL_LockMutex(s.lock);
   for (int i = 0; i < s.feed.count && !found; i++) {
      if (strcmp(s.feed.posts[i].uri, uri) == 0) {
         const char *record = is_like ? s.feed.posts[i].viewer_like
                                      : s.feed.posts[i].viewer_repost;
         snprintf(in.record_uri, sizeof(in.record_uri), "%s", record);
         found = true;
      }
   }
   for (int i = 0; i < s.conversation.count && !found; i++) {
      if (strcmp(s.conversation.posts[i].uri, uri) == 0) {
         const char *record = is_like ? s.conversation.posts[i].viewer_like
                                      : s.conversation.posts[i].viewer_repost;
         snprintf(in.record_uri, sizeof(in.record_uri), "%s", record);
         found = true;
      }
   }
   for (int i = 0; i < s.author_feed.count && !found; i++) {
      if (strcmp(s.author_feed.posts[i].uri, uri) == 0) {
         const char *record = is_like ? s.author_feed.posts[i].viewer_like
                                      : s.author_feed.posts[i].viewer_repost;
         snprintf(in.record_uri, sizeof(in.record_uri), "%s", record);
         found = true;
      }
   }
   SDL_UnlockMutex(s.lock);

   return submit(kind, &in);
}

bool
cobalt_session_begin_delete_post(const char *uri)
{
   if (!uri || !cobalt_post_uri_is_by(uri, s.did)) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.uri, sizeof(in.uri), "%s", uri);
   return submit(COBALT_JOB_DELETE_POST, &in);
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
      snprintf(s.thread_texts[i], sizeof(s.thread_texts[i]), "%s", texts[i]);
   }
   job_input in;
   memset(&in, 0, sizeof(in));
   in.thread_count = count;
   in.reply_gate = reply_gate;
   return submit(COBALT_JOB_POST, &in);
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

   job_input in;
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

   return submit(COBALT_JOB_POST, &in);
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

   job_input in;
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

   return submit(COBALT_JOB_POST, &in);
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

void
cobalt_session_lock(void)
{
   if (s.lock) {
      SDL_LockMutex(s.lock);
   }
}

void
cobalt_session_unlock(void)
{
   if (s.lock) {
      SDL_UnlockMutex(s.lock);
   }
}

bool
cobalt_session_poll(cobalt_job_result *out)
{
   if (!s.initialised || !s.lock) {
      return false;
   }

   SDL_LockMutex(s.lock);
   bool have = s.have_result;
   if (have) {
      if (out) {
         *out = s.result;
      }
      s.have_result = false;
      memset(&s.result, 0, sizeof(s.result));
   }
   SDL_UnlockMutex(s.lock);
   return have;
}
