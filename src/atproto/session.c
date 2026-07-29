#include "atproto/session.h"
#include "cache/session_store.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/rng.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/session.h>
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

#ifdef COBALT_HAS_WOLFRAM
   /* Owned by the worker while a job runs; only touched off-thread when idle. */
   wf_session *wf;
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
 * Runs on the worker thread with the lock not held, so the UI-visible strings
 * are written under it here rather than being edited in place while a frame
 * might be reading them.
 */
static bool
publish_session(const char *service)
{
   const wf_session_data *d = &s.wf->data;
   char resolved[COBALT_SERVICE_MAX];
   snprintf(resolved, sizeof(resolved), "%s", service);

   /* An account's real PDS is discovered at login from didDoc#atproto_pds and
    * is frequently not the host the user typed (every bsky.social handle on a
    * self-hosted PDS, for one). Re-point the client so subsequent calls go to
    * the right place, and persist that, not the entry point. */
   if (d->pds_url && d->pds_url[0] && strcmp(d->pds_url, resolved) != 0) {
      if (wf_xrpc_client_set_base_url(s.wf->client, d->pds_url) == WF_OK) {
         COBALT_LOGI("session: PDS resolved to %s", d->pds_url);
         snprintf(resolved, sizeof(resolved), "%s", d->pds_url);
      } else {
         COBALT_LOGW("session: could not re-point client at %s", d->pds_url);
      }
   }

   cobalt_stored_session stored;
   memset(&stored, 0, sizeof(stored));
   snprintf(stored.service, sizeof(stored.service), "%s", resolved);
   snprintf(stored.handle, sizeof(stored.handle), "%s", d->handle ? d->handle : "");
   snprintf(stored.did, sizeof(stored.did), "%s", d->did ? d->did : "");
   snprintf(stored.access_jwt, sizeof(stored.access_jwt), "%s",
            d->access_jwt ? d->access_jwt : "");
   snprintf(stored.refresh_jwt, sizeof(stored.refresh_jwt), "%s",
            d->refresh_jwt ? d->refresh_jwt : "");

   bool saved = cobalt_session_store_save(&stored);

   SDL_LockMutex(s.lock);
   snprintf(s.service, sizeof(s.service), "%s", stored.service);
   snprintf(s.handle, sizeof(s.handle), "%s", stored.handle);
   snprintf(s.did, sizeof(s.did), "%s", stored.did);
   SDL_UnlockMutex(s.lock);

   memset(&stored, 0, sizeof(stored));
   return saved;
}

/*
 * Installed on the XRPC client so an expired access token is refreshed and the
 * request re-issued once, transparently. Runs on the worker thread, inside the
 * request that tripped it; Wolfram's re-entrancy guard stops the refresh call
 * from recursing.
 */
static wf_status
refresh_handler(void *userdata)
{
   (void) userdata;
   COBALT_LOGI("session: access token expired, refreshing");

   wf_status status = wf_session_refresh(s.wf);
   if (status != WF_OK) {
      COBALT_LOGW("session: refresh failed (%d)", (int) status);
      return status;
   }

   char service[COBALT_SERVICE_MAX];
   SDL_LockMutex(s.lock);
   snprintf(service, sizeof(service), "%s", s.service);
   SDL_UnlockMutex(s.lock);

   /* Refresh rotates both tokens, so the stored copy is stale the moment this
    * succeeds. Persist immediately: losing power now would otherwise leave a
    * refresh JWT on the card that the server has already retired. */
   publish_session(service);
   return WF_OK;
}

static void
teardown_wf(void)
{
   if (s.wf) {
      wf_session_free(s.wf);
      s.wf = NULL;
   }
}

/* Create the wf_session and apply the settings every job needs. */
static wf_session *
new_wf_session(const char *service)
{
   wf_session *sess = wf_session_new(service);
   if (!sess) {
      return NULL;
   }

   if (s.have_ca) {
      wf_xrpc_client_set_ca_bundle(sess->client, s.ca_path);
   }

   /*
    * Hand libcurl's mbedTLS backend the application's DRBG for the handshake.
    * Without this it draws client randoms and ephemeral key-agreement material
    * from devkitPro's mbedtls_hardware_poll, which is the console's tick
    * counter — see util/rng.h. cobalt_session_init() has already refused to
    * come up if this could not work, so a failure here is a real surprise.
    */
   wf_status rng = wf_xrpc_client_set_tls_rng(sess->client, cobalt_rng_mbedtls, NULL);
   if (rng != WF_OK) {
      COBALT_LOGE("session: could not install the TLS RNG (%d) — refusing to "
                  "hand the handshake to a tick-seeded generator", (int) rng);
      wf_session_free(sess);
      return NULL;
   }

   wf_xrpc_client_set_refresh_handler(sess->client, refresh_handler, NULL);
   return sess;
}

static void
run_login(const job_input *in, cobalt_job_result *r, cobalt_auth_state *state)
{
   teardown_wf();

   s.wf = new_wf_session(in->service);
   if (!s.wf) {
      set_message(r, "Out of memory.");
      return;
   }

   COBALT_LOGI("session: createSession at %s", in->service);
   wf_status status = wf_session_login(s.wf, in->identifier, in->password);
   if (status != WF_OK) {
      COBALT_LOGW("session: login failed (%d)", (int) status);
      describe_failure(r, status, COBALT_JOB_LOGIN);
      teardown_wf();
      return;
   }

   if (!publish_session(in->service)) {
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
      set_message(r, "No saved session was found.");
      return;
   }

   teardown_wf();

   char service[COBALT_SERVICE_MAX];
   snprintf(service, sizeof(service), "%s",
            stored.service[0] ? stored.service : DEFAULT_SERVICE);

   s.wf = new_wf_session(service);
   if (!s.wf) {
      memset(&stored, 0, sizeof(stored));
      set_message(r, "Out of memory.");
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
   wf_status status = wf_session_resume(s.wf, &data);
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

   /* wf_session_resume refreshes as part of resuming, so the tokens in hand are
    * already newer than the ones just read off the card. */
   publish_session(service);

   *state = COBALT_AUTH_SIGNED_IN;
   r->ok = true;
   COBALT_LOGI("session: resumed as %s", s.handle);
}

static void
run_logout(cobalt_job_result *r, cobalt_auth_state *state)
{
   if (s.wf) {
      /* Best effort. Wolfram clears the local session either way, and a user
       * who asked to sign out must end up signed out even if the PDS is
       * unreachable — so a failure here is logged, not surfaced. */
      wf_status status = wf_session_delete(s.wf);
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
