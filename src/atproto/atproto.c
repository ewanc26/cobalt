#include "atproto/atproto.h"
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
      return true;
   }

   s_status = COBALT_ATPROTO_READY;
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
