#include "atproto/atproto.h"
#include "net/net.h"
#include "util/entropy.h"
#include "util/log.h"
#include "util/paths.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/platform.h>
#include <wolfram/tid.h>
#include <wolfram/version.h>
#include <wolfram/wiiu.h>
#endif

#include <stdio.h>
#include <string.h>

/*
 * Default PDS host for app-password sign-in. AGENTS.md §7 scopes v1 to app
 * passwords rather than full OAuth/handle-resolution; bsky.social covers the
 * common case (an account hosted on Bluesky's own PDS) but not a
 * self-hosted PDS reachable only via its own host. Letting the user type a
 * different host is a reasonable v2 addition, not implemented here.
 */
#define COBALT_DEFAULT_PDS_HOST "bsky.social"

/* Not distributable — one per installation. See util/entropy.h. */
#define ENTROPY_SEED_FILE "entropy.bin"

static cobalt_atproto_status s_status = COBALT_ATPROTO_ABSENT;

#ifdef COBALT_HAS_WOLFRAM

/*
 * Provision Wolfram's DRBG from the SD card seed, rotating it for next boot.
 *
 * The ordering is load -> set -> rotate -> save -> commit, and it matters.
 * The DRBG is deterministic, so booting twice on the same seed would
 * regenerate identical key material. Wolfram therefore hands back a
 * replacement seed and refuses to sign until commit confirms it reached
 * storage — if the console loses power between the rotate and the save, the
 * next boot comes up on the old seed and must not silently proceed.
 *
 * This mirrors Channel Blue's provision_wolfram_entropy() on the Wii, so the
 * two projects keep the same discipline on a security-sensitive path.
 */
static bool
provision_entropy(void)
{
   char path[COBALT_PATH_MAX];
   if (!cobalt_data_path(path, sizeof(path), ENTROPY_SEED_FILE)) {
      COBALT_LOGW("entropy: no writable data root — cannot provision a seed");
      return false;
   }

   unsigned char seed[COBALT_ENTROPY_SEED_SIZE];
   if (!cobalt_entropy_seed_load(path, seed)) {
      COBALT_LOGW("entropy: no seed at %s — run `make bundle` to generate one "
                  "per installation, then copy it to the SD card", path);
      return false;
   }

   wf_status status = wf_wiiu_set_entropy_seed(seed, sizeof(seed));
   /* Wipe our copy as soon as Wolfram has it, regardless of outcome. */
   memset(seed, 0, sizeof(seed));
   if (status != WF_OK) {
      COBALT_LOGE("entropy: wf_wiiu_set_entropy_seed failed (%d)", (int) status);
      return false;
   }

   status = wf_wiiu_rotate_entropy_seed(seed, sizeof(seed));
   if (status != WF_OK) {
      COBALT_LOGE("entropy: seed rotation failed (%d)", (int) status);
      memset(seed, 0, sizeof(seed));
      return false;
   }

   bool saved = cobalt_entropy_seed_save(path, seed);
   memset(seed, 0, sizeof(seed));
   if (!saved) {
      /* Deliberately do not commit: Wolfram stays failed-closed rather than
       * running on a seed the next boot would reuse. */
      COBALT_LOGE("entropy: rotated seed could not be persisted — signing stays "
                  "disabled to avoid reusing the previous seed");
      return false;
   }

   if (wf_wiiu_commit_entropy_rotation() != WF_OK) {
      COBALT_LOGE("entropy: commit failed after a successful save");
      return false;
   }

   COBALT_LOGI("entropy: seed provisioned and rotated");
   return true;
}

/* Defined further down, alongside the rest of the session-handling code;
 * forward-declared here since cobalt_atproto_init() calls it on the way up. */
static void restore_session(void);

bool
cobalt_atproto_init(void)
{
   wf_status status = wf_platform_init();
   if (status != WF_OK) {
      COBALT_LOGE("wolfram: wf_platform_init failed (%d)", (int) status);
      s_status = COBALT_ATPROTO_ERROR;
      return false;
   }

   /*
    * Smoke-test the platform layer on the way up. A TID is generated from
    * wf_platform_time_micros() and the platform mutex, so a plausible value
    * here means both are actually wired to wut rather than stubbed — worth
    * confirming in the log on the first hardware run, since a stubbed clock
    * silently produces record keys dated 1970.
    */
   char tid[15] = {0};
   if (wf_tid_now(tid) == WF_OK) {
      COBALT_LOGI("wolfram: platform up, sample TID %s (%llu us)", tid,
                  (unsigned long long) wf_platform_time_micros());
   } else {
      COBALT_LOGW("wolfram: wf_tid_now failed — platform clock or mutex is not working");
   }

   /*
    * Signing and key generation fail closed until a real entropy seed is
    * provisioned. devkitPro's mbedTLS for Wii U backs mbedtls_hardware_poll
    * with srand(OSGetSystemTick())/rand(), which is not usable entropy, so
    * Wolfram deliberately refuses to generate keys without one.
    *
    * A missing seed is not fatal. Read-only use (timeline, threads) does not
    * need signing, and app-password login does not either, since curl runs its
    * own TLS stack. It blocks only the paths that sign with a local key.
    */
   provision_entropy();

   if (!wf_wiiu_entropy_ready()) {
      COBALT_LOGW("wolfram: entropy not available — P-256 signing and key "
                  "generation will fail closed");
      s_status = COBALT_ATPROTO_NO_ENTROPY;
      restore_session();
      return true;
   }

   s_status = COBALT_ATPROTO_READY;
   restore_session();
   return true;
}

void
cobalt_atproto_shutdown(void)
{
   if (s_status != COBALT_ATPROTO_ABSENT) {
      wf_platform_shutdown();
   }
   s_status = COBALT_ATPROTO_ABSENT;
}

const char *
cobalt_atproto_sdk_version(void)
{
   return "wolfram " WOLFRAM_VERSION_STRING;
}

/*
 * ============================================================================
 * UNVERIFIED WOLFRAM SURFACE
 *
 * Everything below this notice down to the matching "END UNVERIFIED WOLFRAM
 * SURFACE" is a best-effort guess at a session + XRPC read/write API, written
 * without access to a ../wolfram checkout (it was not available in the
 * environment this was developed in). The types and function names follow
 * the naming already verified elsewhere in this file (wf_status, WF_OK,
 * wf_platform_*, wf_wiiu_*) but have NOT been checked against Wolfram's real
 * headers.
 *
 * Before building this file with COBALT_HAS_WOLFRAM defined against a real
 * Wolfram checkout:
 *   1. Compare every declaration below against wolfram/include/wolfram/*.h
 *      (likely something like wolfram/session.h or wolfram/xrpc.h — this
 *      guess does not know the real header name, hence no #include here).
 *   2. Fix names, field layouts, ownership/lifetime rules and error codes to
 *      match. A link error naming one of the wf_atp_* symbols below is the
 *      expected first sign this needs correcting.
 *   3. Once confirmed correct, delete this notice and switch to including
 *      Wolfram's real header instead of the local declarations.
 *
 * If Wolfram's actual API differs enough that a session must be created some
 * other way (e.g. requiring a client/handle object rather than free
 * functions), the callers below (cobalt_atproto_login, logout,
 * cobalt_atproto_fetch_timeline) are the only things that need to change —
 * nothing in app/ or ui/ talks to Wolfram directly.
 * ============================================================================
 */

typedef struct {
   char access_jwt[2048];
   char refresh_jwt[2048];
   char did[128];
   char handle[128];
} wf_atp_session_t;

typedef struct {
   char author_handle[128];
   char author_display_name[256];
   char text[1024];
   char created_at[40];
} wf_atp_feed_post_t;

/* POST com.atproto.server.createSession against `pds_host` (e.g. "bsky.social"). */
wf_status wf_atp_create_session(const char *pds_host, const char *identifier,
                                const char *app_password, wf_atp_session_t *out_session);

/* GET app.bsky.feed.getTimeline, most recent first, up to `max_posts` entries. */
wf_status wf_atp_get_timeline(const wf_atp_session_t *session, const char *pds_host,
                              wf_atp_feed_post_t *out_posts, size_t max_posts,
                              size_t *out_count);

/* ====================== END UNVERIFIED WOLFRAM SURFACE ==================== */

static bool s_have_session = false;
static wf_atp_session_t s_session;
static const char *s_pds_host = COBALT_DEFAULT_PDS_HOST;

#define SESSION_FILE "session.dat"

/*
 * Session storage is plaintext. AGENTS.md §7 asks for "encrypted or at
 * minimum not in plaintext" — this is a known gap, not an oversight: adding
 * real encryption needs a vetted key-management story (what key, stored
 * where, protected how) that does not exist yet, and a fake obfuscation layer
 * would be worse than being honest about the gap. Do not ship a release build
 * with this unaddressed.
 */
static bool
save_session_file(void)
{
   char path[COBALT_PATH_MAX];
   if (!cobalt_data_path(path, sizeof(path), SESSION_FILE)) {
      return false;
   }

   char tmp[COBALT_PATH_MAX];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);

   FILE *f = fopen(tmp, "wb");
   if (!f) {
      COBALT_LOGE("session: could not open %s for writing", tmp);
      return false;
   }
   size_t written = fwrite(&s_session, sizeof(s_session), 1, f);
   int close_rc = fclose(f);
   if (written != 1 || close_rc != 0) {
      COBALT_LOGE("session: write to %s failed", tmp);
      remove(tmp);
      return false;
   }
   if (rename(tmp, path) != 0) {
      COBALT_LOGE("session: could not replace %s", path);
      remove(tmp);
      return false;
   }
   return true;
}

static bool
load_session_file(void)
{
   char path[COBALT_PATH_MAX];
   if (!cobalt_data_path(path, sizeof(path), SESSION_FILE)) {
      return false;
   }

   FILE *f = fopen(path, "rb");
   if (!f) {
      return false;
   }
   wf_atp_session_t loaded;
   size_t got = fread(&loaded, sizeof(loaded), 1, f);
   fclose(f);
   if (got != 1) {
      return false;
   }

   /* Defensive NUL-termination: a truncated or foreign-format file must never
    * hand an unterminated string to a later "%s" format. */
   loaded.access_jwt[sizeof(loaded.access_jwt) - 1] = '\0';
   loaded.refresh_jwt[sizeof(loaded.refresh_jwt) - 1] = '\0';
   loaded.did[sizeof(loaded.did) - 1] = '\0';
   loaded.handle[sizeof(loaded.handle) - 1] = '\0';

   s_session = loaded;
   return true;
}

/* Called once from cobalt_atproto_init(). Restoring a session needs no
 * signing key, so this runs regardless of entropy status. */
static void
restore_session(void)
{
   if (load_session_file()) {
      s_have_session = true;
      COBALT_LOGI("atproto: restored session for %s", s_session.handle);
   }
}

cobalt_login_result
cobalt_atproto_login(const char *identifier, const char *app_password)
{
   if (s_status == COBALT_ATPROTO_ABSENT || s_status == COBALT_ATPROTO_ERROR) {
      return COBALT_LOGIN_UNAVAILABLE;
   }
   if (cobalt_net_get_status() != COBALT_NET_UP) {
      return COBALT_LOGIN_NETWORK_ERROR;
   }

   wf_atp_session_t session;
   memset(&session, 0, sizeof(session));
   wf_status status = wf_atp_create_session(s_pds_host, identifier, app_password, &session);
   if (status != WF_OK) {
      COBALT_LOGW("atproto: login failed (wf_status %d)", (int) status);
      return COBALT_LOGIN_BAD_CREDENTIALS;
   }

   s_session = session;
   s_have_session = true;

   if (!save_session_file()) {
      COBALT_LOGW("atproto: signed in but could not persist the session to SD "
                  "— signing in again will be needed next launch");
   }

   COBALT_LOGI("atproto: signed in as %s", s_session.handle);
   return COBALT_LOGIN_OK;
}

void
cobalt_atproto_logout(void)
{
   memset(&s_session, 0, sizeof(s_session));
   s_have_session = false;

   char path[COBALT_PATH_MAX];
   if (cobalt_data_path(path, sizeof(path), SESSION_FILE)) {
      remove(path);
   }
   COBALT_LOGI("atproto: signed out");
}

bool
cobalt_atproto_has_session(void)
{
   return s_have_session;
}

const char *
cobalt_atproto_session_handle(void)
{
   return s_have_session ? s_session.handle : NULL;
}

cobalt_timeline_result
cobalt_atproto_fetch_timeline(cobalt_feed_post *out_posts, int max_posts, int *out_count)
{
   if (out_count) {
      *out_count = 0;
   }
   if (!s_have_session) {
      return COBALT_TIMELINE_NOT_SIGNED_IN;
   }
   if (cobalt_net_get_status() != COBALT_NET_UP) {
      return COBALT_TIMELINE_NETWORK_ERROR;
   }

   size_t want = (size_t) (max_posts < COBALT_TIMELINE_MAX_POSTS ? max_posts
                                                                  : COBALT_TIMELINE_MAX_POSTS);
   wf_atp_feed_post_t wf_posts[COBALT_TIMELINE_MAX_POSTS];
   size_t got = 0;

   wf_status status = wf_atp_get_timeline(&s_session, s_pds_host, wf_posts, want, &got);
   if (status != WF_OK) {
      COBALT_LOGW("atproto: timeline fetch failed (wf_status %d)", (int) status);
      return COBALT_TIMELINE_NETWORK_ERROR;
   }

   for (size_t i = 0; i < got && (int) i < max_posts; i++) {
      snprintf(out_posts[i].author_handle, sizeof(out_posts[i].author_handle),
               "%s", wf_posts[i].author_handle);
      snprintf(out_posts[i].author_display_name, sizeof(out_posts[i].author_display_name),
               "%s", wf_posts[i].author_display_name);
      snprintf(out_posts[i].text, sizeof(out_posts[i].text), "%s", wf_posts[i].text);
      snprintf(out_posts[i].created_at, sizeof(out_posts[i].created_at),
               "%s", wf_posts[i].created_at);
   }

   if (out_count) {
      *out_count = (int) got;
   }
   return COBALT_TIMELINE_OK;
}

#else /* !COBALT_HAS_WOLFRAM */

bool
cobalt_atproto_init(void)
{
   COBALT_LOGW("built without wolfram — ATProto features are unavailable "
               "(build ../wolfram for Wii U, see the Makefile)");
   s_status = COBALT_ATPROTO_ABSENT;
   return false;
}

void
cobalt_atproto_shutdown(void)
{
   s_status = COBALT_ATPROTO_ABSENT;
}

const char *
cobalt_atproto_sdk_version(void)
{
   return "not built in";
}

cobalt_login_result
cobalt_atproto_login(const char *identifier, const char *app_password)
{
   (void) identifier;
   (void) app_password;
   return COBALT_LOGIN_UNAVAILABLE;
}

void
cobalt_atproto_logout(void)
{
}

bool
cobalt_atproto_has_session(void)
{
   return false;
}

const char *
cobalt_atproto_session_handle(void)
{
   return NULL;
}

cobalt_timeline_result
cobalt_atproto_fetch_timeline(cobalt_feed_post *out_posts, int max_posts, int *out_count)
{
   (void) out_posts;
   (void) max_posts;
   if (out_count) {
      *out_count = 0;
   }
   return COBALT_TIMELINE_UNAVAILABLE;
}

#endif /* COBALT_HAS_WOLFRAM */

cobalt_atproto_status
cobalt_atproto_get_status(void)
{
   return s_status;
}

const char *
cobalt_atproto_status_string(void)
{
   switch (s_status) {
      case COBALT_ATPROTO_READY:      return "ready";
      case COBALT_ATPROTO_NO_ENTROPY: return "ready (no signing key entropy)";
      case COBALT_ATPROTO_ERROR:      return "failed to initialise";
      case COBALT_ATPROTO_ABSENT:
      default:                        return "not built in";
   }
}
