#include "atproto/session.h"
#include "atproto/actors.h"
#include "atproto/feed.h"
#include "atproto/notifications.h"
#include "atproto/actor_profile.h"
#include "cache/session_store.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/rng.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/actor_typed.h>
#include <wolfram/agent.h>
#include <wolfram/feed_typed.h>
#include <wolfram/graph_typed.h>
#include <wolfram/moderation_typed.h>
#include <wolfram/session.h>
#include <wolfram/thread_typed.h>
#include <wolfram/xrpc.h>
#endif

#include <SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

   /* Composing. `text` is the post body; the refs are empty for a new post. */
   char text[COBALT_COMPOSE_TEXT_MAX];
   char root_uri[COBALT_POST_URI_MAX];
   char root_cid[COBALT_POST_CID_MAX];
} job_input;

static struct {
   bool initialised;

   /* Set once at init and read-only afterwards, so no locking. */
   char ca_path[COBALT_PATH_MAX];
   bool have_ca;
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

   /* Published by the worker alongside the auth state, and read by the
    * timeline screen. Guarded by `lock` on write, read while idle. */
   cobalt_feed feed;
   /* Named to stay clear of `thread`, which is the worker. */
   cobalt_thread conversation;
   cobalt_notifications notifications;
   cobalt_profile profile;
   cobalt_feed author_feed;
   cobalt_actor_list muted;
   cobalt_actor_list blocked;

#ifdef COBALT_HAS_WOLFRAM
   /* Owned by the worker while a job runs; only touched off-thread when idle. */
   wf_agent *wf;
#endif
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

#ifdef COBALT_HAS_WOLFRAM

/*
 * Wolfram reports transport and protocol failures as a single wf_status, so the
 * PDS's own XRPC error envelope ("InvalidLogin", "AuthFactorTokenRequired", …)
 * does not reach us from wf_session_login. These messages are therefore written
 * to be useful without it: each names the most likely cause and what to try.
 * Surfacing the real envelope needs a Wolfram-side change — see AGENTS.md §13.
 */
static void
describe_failure(cobalt_job_result *r, wf_status status, cobalt_job_kind kind)
{
   switch (status) {
      case WF_ERR_NETWORK:
         set_message(r, "Could not reach the server. Check the console's "
                        "internet connection, and that a TLS trust store was "
                        "bundled with this build.");
         break;

      case WF_ERR_TIMEOUT:
         set_message(r, "The server did not answer in time. Try again.");
         break;

      case WF_ERR_HTTP:
         if (kind == COBALT_JOB_LOGIN) {
            set_message(r, "The server rejected those details. Check the handle "
                           "and app password — an account password will not work "
                           "if two-factor is on.");
         } else {
            set_message(r, "The saved session is no longer valid. Sign in again.");
         }
         break;

      case WF_ERR_RATE_LIMIT:
         set_message(r, "The server is rate limiting this console. Wait a few "
                        "minutes and try again.");
         break;

      case WF_ERR_PARSE:
         set_message(r, "The server sent a reply Cobalt could not read.");
         break;

      case WF_ERR_ALLOC:
         set_message(r, "Out of memory.");
         break;

      default:
         /* No friendlier wording available: log the code so a hardware run can
          * be matched against wf_status in wolfram/xrpc.h. */
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
   if (rng != WF_OK) {
      COBALT_LOGE("session: could not install the TLS RNG (%d) — refusing to "
                  "hand the handshake to a tick-seeded generator", (int) rng);
      wf_agent_free(agent);
      return NULL;
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
   if (!s.wf) {
      set_message(r, "Could not create the client.");
      return;
   }

   COBALT_LOGI("session: createSession at %s", in->service);
   wf_status status = wf_agent_login(s.wf, in->identifier, in->password);
   if (status != WF_OK) {
      COBALT_LOGW("session: login failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_LOGIN);
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
   if (!s.wf) {
      memset(&stored, 0, sizeof(stored));
      set_message(r, "Could not create the client.");
      return;
   }

   /* Wolfram deep-copies this, so the stack copy can be wiped straight after. */
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
   wf_status status = wf_agent_resume(s.wf, &data);
   memset(&stored, 0, sizeof(stored));

   if (status != WF_OK) {
      COBALT_LOGW("session: resume failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_RESUME);
      teardown_wf();
      /* A refresh JWT the server has rejected is never coming back, so do not
       * leave it on the card to fail again on the next boot. Transport failures
       * are different — those are worth retrying with the same credentials. */
      if (status == WF_ERR_HTTP) {
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
   const int64_t now = cobalt_time_now();

   SDL_LockMutex(s.lock);
   if (!in->paging) {
      cobalt_feed_reset(&s.feed);
   }
   const int added = cobalt_feed_append_from_wolfram(&s.feed, &list, now);
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
run_post(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   if (!s.wf) {
      set_message(r, "Sign in first.");
      return;
   }

   wf_agent_post_result result;
   memset(&result, 0, sizeof(result));

   wf_status status;
   if (in->uri[0]) {
      /*
       * A reply. wf_agent_reply_refs rather than wf_agent_reply, because the
       * latter uses the parent as its own root — correct only when replying to
       * a top-level post, and silently wrong for a reply to a reply.
       */
      COBALT_LOGI("session: replying to %s (root %s)", in->uri, in->root_uri);
      status = wf_agent_reply_refs(s.wf, in->text, in->root_uri, in->root_cid,
                                   in->uri, in->cid, &result);
   } else {
      COBALT_LOGI("session: posting %d bytes", (int) strlen(in->text));
      status = wf_agent_post(s.wf, in->text, &result);
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
      if (cobalt_time_format_rfc3339(now, seen_at, sizeof(seen_at)) &&
          wf_agent_update_seen_notifications(s.wf, seen_at) != WF_OK) {
         COBALT_LOGW("session: updateSeen failed — the unread badge may linger "
                     "on other clients");
      }
   }

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

   wf_agent_profile_free(&profile);

   /*
    * The posts are a second request, but the same job. A failure here is not
    * fatal to the screen: the profile itself already loaded and is worth
    * showing, so the feed is left empty and the header stands on its own.
    */
   wf_agent_feed_list list;
   memset(&list, 0, sizeof(list));

   status = wf_agent_get_author_feed_typed(s.wf, in->uri, TIMELINE_PAGE, NULL,
                                           NULL, &list);
   if (status == WF_OK) {
      const int64_t now = cobalt_time_now();
      SDL_LockMutex(s.lock);
      cobalt_feed_reset(&s.author_feed);
      cobalt_feed_append_from_wolfram(&s.author_feed, &list, now);
      SDL_UnlockMutex(s.lock);
      wf_agent_feed_list_free(&list);
   } else {
      COBALT_LOGW("session: getAuthorFeed failed (%d) — showing the profile "
                  "without posts", (int) status);
      SDL_LockMutex(s.lock);
      cobalt_feed_reset(&s.author_feed);
      SDL_UnlockMutex(s.lock);
   }

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
               wf_status (*fetch)(wf_agent *, int, const char *,
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

   const wf_status status = fetch(s.wf, TIMELINE_PAGE, cursor, &wf_list);
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

static void
run_muted_list(const job_input *in, cobalt_job_result *r,
               cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &s.muted, wf_agent_get_mutes_typed,
                  "No muted accounts.");
}

static void
run_blocked_list(const job_input *in, cobalt_job_result *r,
                 cobalt_auth_state *state)
{
   run_actor_list(in, r, state, &s.blocked, wf_agent_get_blocks_typed,
                  "No blocked accounts.");
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
      case COBALT_JOB_RESUME: run_resume(&r, state);     break;
      case COBALT_JOB_LOGOUT: run_logout(&r, state);     break;
      case COBALT_JOB_TIMELINE: run_timeline(in, &r, state); break;
      case COBALT_JOB_THREAD:   run_thread(in, &r, state);   break;
      case COBALT_JOB_LIKE:     run_interaction(in, &r, state, true);  break;
      case COBALT_JOB_REPOST:   run_interaction(in, &r, state, false); break;
      case COBALT_JOB_POST:     run_post(in, &r, state);      break;
      case COBALT_JOB_NOTIFICATIONS:
         run_notifications(in, &r, state);
         break;
      case COBALT_JOB_PROFILE:  run_profile(in, &r, state);   break;
      case COBALT_JOB_FOLLOW:   run_follow(in, &r, state);    break;
      case COBALT_JOB_MUTE:     run_mute(in, &r, state);      break;
      case COBALT_JOB_BLOCK:    run_block(in, &r, state);     break;
      case COBALT_JOB_MUTED_LIST:   run_muted_list(in, &r, state);   break;
      case COBALT_JOB_BLOCKED_LIST: run_blocked_list(in, &r, state); break;
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
      cobalt_job_result result = run_job(kind, &in, &state);
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
   return submit(COBALT_JOB_TIMELINE, &in);
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
   return submit(COBALT_JOB_PROFILE, &in);
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

bool
cobalt_session_begin_blocked_list(bool paging)
{
   job_input in;
   memset(&in, 0, sizeof(in));
   in.paging = paging;
   return submit(COBALT_JOB_BLOCKED_LIST, &in);
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
cobalt_session_begin_post(const char *text, const char *parent_uri,
                          const char *parent_cid, const char *root_uri,
                          const char *root_cid)
{
   if (!text || !text[0]) {
      return false;
   }

   job_input in;
   memset(&in, 0, sizeof(in));
   snprintf(in.text, sizeof(in.text), "%s", text);

   const bool is_reply = parent_uri && parent_uri[0];
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
