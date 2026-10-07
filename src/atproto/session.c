#include "atproto/session_internal.h"
#include "util/threadprio.h"

/* Bundled by `make cacert` into romfs/. Without it curl cannot verify any
 * certificate on this platform — see tools/fetch_cacert.sh. */
#define CA_BUNDLE_FILE "cacert.pem"

#define DEFAULT_SERVICE "https://bsky.social"

cobalt_session_core g_session;

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
   if (!g_session.lock) {
      return COBALT_AUTH_SIGNED_OUT;
   }
   SDL_LockMutex(g_session.lock);
   cobalt_auth_state state = g_session.state;
   SDL_UnlockMutex(g_session.lock);
   return state;
}

bool
cobalt_session_busy(void)
{
   if (!g_session.lock) {
      return false;
   }
   SDL_LockMutex(g_session.lock);
   bool busy = g_session.busy;
   SDL_UnlockMutex(g_session.lock);
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
   return g_session.handle;
}

const char *
cobalt_session_did(void)
{
   return g_session.did;
}

const char *
cobalt_session_pair_url(void)
{
   return g_session.pair_url;
}

const char *
cobalt_session_pair_code(void)
{
   return g_session.pair_code;
}

const char *
cobalt_session_service(void)
{
   return g_session.service[0] ? g_session.service : DEFAULT_SERVICE;
}

bool
cobalt_session_available(void)
{
   return g_session.initialised && g_session.blocker == NULL;
}

const char *
cobalt_session_blocker(void)
{
   return g_session.blocker;
}

const char *
cobalt_session_ca_path(void)
{
   return g_session.have_ca ? g_session.ca_path : NULL;
}

bool
cobalt_session_threaded(void)
{
   return g_session.thread != NULL;
}

bool
cobalt_session_has_saved(void)
{
   return cobalt_session_store_exists();
}

/* --- the jobs themselves --- */

void
cobalt_session_set_message(cobalt_job_result *r, const char *fmt, ...)
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
   g_session.post_lang = 0;
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
         g_session.post_lang = i;
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
   fprintf(f, "%s\n", POST_LANGS[g_session.post_lang]);
   fclose(f);
}

const char *
cobalt_session_post_lang(void)
{
   return POST_LANGS[g_session.post_lang];
}

void
cobalt_session_cycle_post_lang(void)
{
   g_session.post_lang = (g_session.post_lang + 1) % POST_LANG_COUNT;
   save_post_lang();
#ifdef COBALT_HAS_WOLFRAM
   if (g_session.wf) {
      wf_agent_set_post_langs(g_session.wf, POST_LANGS[g_session.post_lang]);
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
void
cobalt_session_describe_failure(cobalt_job_result *r, wf_status status, cobalt_job_kind kind)
{
   if (status == WF_ERR_ALLOC) {
      cobalt_session_set_message(r, "Out of memory.");
      return;
   }
   switch (wf_failure_classify(status, 0, NULL)) {
      case WF_FAIL_NETWORK:
      case WF_FAIL_TLS:
         cobalt_session_set_message(r, "Could not reach the server. Check the console's "
                        "internet connection, and that a TLS trust store was "
                        "bundled with this build.");
         break;

      case WF_FAIL_TIMEOUT:
         cobalt_session_set_message(r, "The server did not answer in time. Try again.");
         break;

      case WF_FAIL_BAD_CREDENTIALS:
      case WF_FAIL_OTHER:
         if (status != WF_ERR_AUTH && status != WF_ERR_HTTP) {
            /* No friendlier wording available: log the code so a hardware run
             * can be matched against wf_status in wolfram/xrpc.h. */
            cobalt_session_set_message(r, "The request failed (wolfram status %d).", (int) status);
         } else if (kind == COBALT_JOB_LOGIN) {
            cobalt_session_set_message(r, "The server rejected those details. Check the handle "
                           "and app password — an account password will not work "
                           "if two-factor is on.");
         } else {
            cobalt_session_set_message(r, "The server refused that request. The post may be gone, or the "
                           "session may have expired - try again, then sign in "
                           "again if it keeps happening.");
         }
         break;

      case WF_FAIL_RATE_LIMIT:
         cobalt_session_set_message(r, "The server is rate limiting this console. Wait a few "
                        "minutes and try again.");
         break;

      case WF_FAIL_SERVER:
         cobalt_session_set_message(r, "The server had a problem. Try again in a few minutes.");
         break;

      case WF_FAIL_BAD_RESPONSE:
         cobalt_session_set_message(r, "The server sent a reply Cobalt could not read.");
         break;

      case WF_FAIL_NONE:
      case WF_FAIL_NOT_READY:
      default:
         cobalt_session_set_message(r, "The request failed (wolfram status %d).", (int) status);
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
bool
cobalt_session_publish(void)
{
   wf_session_data data;
   memset(&data, 0, sizeof(data));
   if (wf_agent_get_session_data(g_session.wf, &data) != WF_OK) {
      return false;
   }

   cobalt_stored_session stored;
   memset(&stored, 0, sizeof(stored));

   /* wf_agent_login re-points the client at the account's real PDS, discovered
    * from didDoc#atproto_pds — frequently not the host the user typed. Persist
    * where the tokens are actually valid, not the entry point. */
   char fallback[COBALT_SERVICE_MAX];
   SDL_LockMutex(g_session.lock);
   snprintf(fallback, sizeof(fallback), "%s", g_session.service);
   SDL_UnlockMutex(g_session.lock);

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

   SDL_LockMutex(g_session.lock);
   snprintf(g_session.service, sizeof(g_session.service), "%s", stored.service);
   snprintf(g_session.handle, sizeof(g_session.handle), "%s", stored.handle);
   snprintf(g_session.did, sizeof(g_session.did), "%s", stored.did);
   SDL_UnlockMutex(g_session.lock);

   memset(&stored, 0, sizeof(stored));
   return saved;
}


#endif /* COBALT_HAS_WOLFRAM */

/*
 * Runs off the frame loop. `state` carries the auth state in and out rather
 * than being poked directly, so the only writer of the shared copy is the
 * caller, under the lock.
 */
static cobalt_job_result
run_job(cobalt_job_kind kind, const cobalt_job_input *in, cobalt_auth_state *state)
{
   cobalt_job_result r;
   memset(&r, 0, sizeof(r));
   r.kind = kind;

#ifdef COBALT_HAS_WOLFRAM
   switch (kind) {
      case COBALT_JOB_LOGIN:  cobalt_session_run_login(in, &r, state);  break;
      case COBALT_JOB_OAUTH:  cobalt_session_run_oauth(in, &r, state);  break;
      case COBALT_JOB_RESUME: cobalt_session_run_resume(&r, state);     break;
      case COBALT_JOB_LOGOUT: cobalt_session_run_logout(&r, state);     break;
      case COBALT_JOB_TIMELINE: cobalt_session_run_timeline(in, &r, state); break;
      case COBALT_JOB_THREAD:   cobalt_session_run_thread(in, &r, state);   break;
      case COBALT_JOB_LIKE:     cobalt_session_run_interaction(in, &r, state, true);  break;
      case COBALT_JOB_REPOST:   cobalt_session_run_interaction(in, &r, state, false); break;
      case COBALT_JOB_POST:     cobalt_session_run_post(in, &r, state);      break;
      case COBALT_JOB_DELETE_POST: cobalt_session_run_delete_post(in, &r, state); break;
      case COBALT_JOB_NOTIFICATIONS:
         cobalt_session_run_notifications(in, &r, state);
         break;
      case COBALT_JOB_PROFILE:  cobalt_session_run_profile(in, &r, state);   break;
      case COBALT_JOB_FOLLOW:   cobalt_session_run_follow(in, &r, state);    break;
      case COBALT_JOB_MUTE:     cobalt_session_run_mute(in, &r, state);      break;
      case COBALT_JOB_BLOCK:    cobalt_session_run_block(in, &r, state);     break;
      case COBALT_JOB_MUTED_LIST:   cobalt_session_run_muted_list(in, &r, state);   break;
      case COBALT_JOB_BLOCKED_LIST: cobalt_session_run_blocked_list(in, &r, state); break;
      case COBALT_JOB_PROFILE_TAB:  cobalt_session_run_profile_tab(in, &r, state); break;
      case COBALT_JOB_FOLLOWERS:    cobalt_session_run_followers(in, &r, state);    break;
      case COBALT_JOB_FOLLOWING:    cobalt_session_run_following(in, &r, state);    break;
      case COBALT_JOB_LIKES:        cobalt_session_run_likes(in, &r, state);        break;
      case COBALT_JOB_REPOSTED_BY:  cobalt_session_run_reposted_by(in, &r, state); break;
      case COBALT_JOB_SEARCH_ACTORS: cobalt_session_run_search_actors(in, &r, state); break;
      case COBALT_JOB_FEED:     cobalt_session_run_feed(in, &r, state);      break;
      case COBALT_JOB_SEARCH_POSTS: cobalt_session_run_search_posts(in, &r, state); break;
      case COBALT_JOB_LISTS:        cobalt_session_run_lists(in, &r, state);        break;
      case COBALT_JOB_SAVED_FEEDS:  cobalt_session_run_saved_feeds(in, &r, state);  break;
      case COBALT_JOB_LIST_MEMBERS: cobalt_session_run_list_members(in, &r, state); break;
      case COBALT_JOB_NONE:
      default:                                           break;
   }
#else
   (void) in;
   (void) state;
   cobalt_session_set_message(&r, "This build has no ATProto SDK. Build Wolfram for Wii U "
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

   SDL_LockMutex(g_session.lock);
   for (;;) {
      while (!g_session.stop && g_session.pending == COBALT_JOB_NONE) {
         SDL_CondWait(g_session.wake, g_session.lock);
      }
      if (g_session.stop) {
         break;
      }

      const cobalt_job_kind kind = g_session.pending;
      cobalt_job_input in = g_session.input;
      cobalt_auth_state state = g_session.resting_state;
      g_session.pending = COBALT_JOB_NONE;
      memset(&g_session.input, 0, sizeof(g_session.input));
      SDL_UnlockMutex(g_session.lock);

      /* The lock is deliberately not held across the network call: the UI
       * thread polls state every frame and must not block behind curl. */
      const uint32_t job_t0 = SDL_GetTicks();
      cobalt_job_result result = run_job(kind, &in, &state);
      COBALT_LOGI("session: job %d took %u ms (%s)", (int) kind,
                  (unsigned) (SDL_GetTicks() - job_t0), result.ok ? "ok" : "failed");
      memset(&in, 0, sizeof(in));

      SDL_LockMutex(g_session.lock);
      g_session.result = result;
      g_session.have_result = true;
      g_session.busy = false;
      g_session.state = state;
   }
   SDL_UnlockMutex(g_session.lock);

   COBALT_LOGI("session: worker thread exiting");
   return 0;
}

/* --- lifecycle --- */

static void
resolve_ca_bundle(void)
{
   if (!cobalt_content_path(g_session.ca_path, sizeof(g_session.ca_path), CA_BUNDLE_FILE)) {
      COBALT_LOGE("session: no content root, so no TLS trust store");
      g_session.ca_path[0] = '\0';
      return;
   }

   FILE *f = fopen(g_session.ca_path, "rb");
   if (!f) {
      COBALT_LOGE("session: %s is missing — run `make cacert` and rebuild. "
                  "Without it every HTTPS request will fail verification, "
                  "because the Wii U has no system certificate store.",
                  g_session.ca_path);
      return;
   }
   fclose(f);

   g_session.have_ca = true;
   COBALT_LOGI("session: TLS trust store at %s", g_session.ca_path);
}

bool
cobalt_session_init(void)
{
   memset(&g_session, 0, sizeof(g_session));

   resolve_ca_bundle();
   load_post_lang();

   g_session.lock = SDL_CreateMutex();
   g_session.wake = SDL_CreateCond();
   if (!g_session.lock || !g_session.wake) {
      COBALT_LOGE("session: could not create synchronisation primitives: %s",
                  SDL_GetError());
      /* cobalt_session_shutdown() bails on an uninitialised module, so whatever
       * did get created has to be released here. */
      if (g_session.wake) {
         SDL_DestroyCond(g_session.wake);
         g_session.wake = NULL;
      }
      if (g_session.lock) {
         SDL_DestroyMutex(g_session.lock);
         g_session.lock = NULL;
      }
      g_session.blocker = "threading unavailable";
      return false;
   }

   g_session.thread = SDL_CreateThread(worker_main, "cobalt-net", NULL);
   if (!g_session.thread) {
      /*
       * Not fatal. Requests fall back to running on the calling thread, which
       * works but freezes the frame during a request. Reported on the
       * diagnostics screen so a hardware run can tell this apart from a hang.
       */
      COBALT_LOGE("session: SDL_CreateThread failed (%s) — network calls will "
                  "block the frame loop", SDL_GetError());
   }

   g_session.initialised = true;

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
   g_session.blocker = "Wolfram not built in";
#else
   if (!g_session.have_ca) {
      g_session.blocker = "no TLS trust store";
   } else if (!cobalt_rng_ready()) {
      g_session.blocker = "no entropy seed";
   } else if (!wf_xrpc_tls_rng_supported()) {
      /* Wolfram compiles the hook for Wii U and checks libcurl's backend at
       * runtime, so this means curl is not the mbedTLS build it was linked
       * against — in which case the handshake RNG cannot be replaced. */
      g_session.blocker = "TLS RNG hook unavailable";
   }
#endif

   snprintf(g_session.service, sizeof(g_session.service), "%s", DEFAULT_SERVICE);

   COBALT_LOGI("session: up (threaded=%d, ca=%d, blocker=%s)",
               (int) (g_session.thread != NULL), (int) g_session.have_ca,
               g_session.blocker ? g_session.blocker : "none");
   return g_session.blocker == NULL;
}

void
cobalt_session_shutdown(void)
{
   if (!g_session.initialised) {
      return;
   }

   if (g_session.thread) {
      SDL_LockMutex(g_session.lock);
      g_session.stop = true;
      SDL_CondSignal(g_session.wake);
      const bool in_flight = g_session.busy;
      SDL_UnlockMutex(g_session.lock);

      if (in_flight) {
         /* curl has no cancellation here, so this waits out whatever is on the
          * wire. Logged because it is the one place shutdown can visibly take
          * seconds. */
         COBALT_LOGI("session: waiting for an in-flight request before exit");
      }

      SDL_WaitThread(g_session.thread, NULL);
      g_session.thread = NULL;
   }

#ifdef COBALT_HAS_WOLFRAM
   cobalt_session_teardown_agent();
#endif

   if (g_session.wake) {
      SDL_DestroyCond(g_session.wake);
      g_session.wake = NULL;
   }
   if (g_session.lock) {
      SDL_DestroyMutex(g_session.lock);
      g_session.lock = NULL;
   }

   memset(&g_session, 0, sizeof(g_session));
}

/* --- request submission --- */

bool
cobalt_session_submit(cobalt_job_kind kind, const cobalt_job_input *in)
{
   if (!g_session.initialised) {
      return false;
   }

   SDL_LockMutex(g_session.lock);
   if (g_session.busy) {
      SDL_UnlockMutex(g_session.lock);
      COBALT_LOGW("session: a request is already in flight");
      return false;
   }

   g_session.busy = true;
   g_session.resting_state = g_session.state;
   g_session.state = COBALT_AUTH_WORKING;

   if (g_session.thread) {
      g_session.pending = kind;
      g_session.input = *in;
      SDL_CondSignal(g_session.wake);
      SDL_UnlockMutex(g_session.lock);
      return true;
   }

   /* No worker thread: run it here. The frame loop stalls for the duration,
    * which is bad but strictly better than silently doing nothing. */
   cobalt_auth_state state = g_session.resting_state;
   SDL_UnlockMutex(g_session.lock);

   cobalt_job_input local = *in;
   cobalt_job_result result = run_job(kind, &local, &state);
   memset(&local, 0, sizeof(local));

   SDL_LockMutex(g_session.lock);
   g_session.result = result;
   g_session.have_result = true;
   g_session.busy = false;
   g_session.state = state;
   SDL_UnlockMutex(g_session.lock);
   return true;
}


void
cobalt_session_lock(void)
{
   if (g_session.lock) {
      SDL_LockMutex(g_session.lock);
   }
}

void
cobalt_session_unlock(void)
{
   if (g_session.lock) {
      SDL_UnlockMutex(g_session.lock);
   }
}

bool
cobalt_session_poll(cobalt_job_result *out)
{
   if (!g_session.initialised || !g_session.lock) {
      return false;
   }

   SDL_LockMutex(g_session.lock);
   bool have = g_session.have_result;
   if (have) {
      if (out) {
         *out = g_session.result;
      }
      g_session.have_result = false;
      memset(&g_session.result, 0, sizeof(g_session.result));
   }
   SDL_UnlockMutex(g_session.lock);
   return have;
}
