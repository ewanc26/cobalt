#include "util/rng.h"
#include "util/entropy.h"
#include "util/log.h"

#include <SDL.h>

#include <mbedtls/ctr_drbg.h>

#include <string.h>

/*
 * How much of the seed mbedTLS is allowed to ask for when seeding.
 *
 * mbedtls_ctr_drbg_seed() defaults to MBEDTLS_CTR_DRBG_ENTROPY_LEN (48 bytes
 * for AES-256) and then asks for a nonce of half that again, which would come
 * to 72 — more than the seed file holds. 32 bytes is the NIST minimum for a
 * 256-bit CTR_DRBG, and brings the total request (32 + 16) comfortably inside
 * COBALT_ENTROPY_SEED_SIZE.
 */
#define SEED_REQUEST_LEN 32

/* Mixed into the DRBG's derivation so this instance is distinct from any other
 * DRBG that might one day be seeded from the same file. */
#define PERSONALIZATION "cobalt.app.rng.v1"

static struct {
   bool ready;
   SDL_mutex *lock;
   mbedtls_ctr_drbg_context drbg;

   /* Held only across mbedtls_ctr_drbg_seed(), which consumes it through
    * seed_source() below and never touches it again. */
   const unsigned char *seed;
   size_t seed_len;
   size_t seed_taken;
} s;

static void
wipe(void *p, size_t n)
{
   volatile unsigned char *q = (volatile unsigned char *) p;
   while (n--) {
      *q++ = 0;
   }
}

/*
 * Entropy callback for seeding. Deliberately serves the file's bytes directly
 * instead of chaining to mbedtls_entropy_func(), which on this platform would
 * walk straight back into the broken hardware poll this module exists to avoid.
 *
 * Running out is a hard failure rather than a wrap-around: repeating seed bytes
 * would quietly weaken the derivation, and a different mbedTLS version asking
 * for more than expected is something a hardware run should be told about.
 */
static int
seed_source(void *data, unsigned char *output, size_t len)
{
   (void) data;

   if (!s.seed || s.seed_taken + len > s.seed_len) {
      COBALT_LOGE("rng: seeding wanted %u bytes with %u of %u left — the "
                  "linked mbedTLS asks for more than the seed file holds",
                  (unsigned) len, (unsigned) (s.seed_len - s.seed_taken),
                  (unsigned) s.seed_len);
      return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
   }

   memcpy(output, s.seed + s.seed_taken, len);
   s.seed_taken += len;
   return 0;
}

bool
cobalt_rng_init(const unsigned char *seed, size_t seed_len)
{
   if (!seed || seed_len == 0) {
      return false;
   }

   if (!s.lock) {
      s.lock = SDL_CreateMutex();
      if (!s.lock) {
         COBALT_LOGE("rng: could not create mutex: %s", SDL_GetError());
         return false;
      }
   }

   SDL_LockMutex(s.lock);

   if (s.ready) {
      /* Re-seeding: drop the old state rather than reseeding in place, so a
       * failure below cannot leave a half-updated generator in service. */
      mbedtls_ctr_drbg_free(&s.drbg);
      s.ready = false;
   }

   mbedtls_ctr_drbg_init(&s.drbg);
   mbedtls_ctr_drbg_set_entropy_len(&s.drbg, SEED_REQUEST_LEN);

   s.seed = seed;
   s.seed_len = seed_len;
   s.seed_taken = 0;

   int rc = mbedtls_ctr_drbg_seed(&s.drbg, seed_source, NULL,
                                  (const unsigned char *) PERSONALIZATION,
                                  strlen(PERSONALIZATION));

   s.seed = NULL;
   s.seed_len = 0;
   s.seed_taken = 0;

   if (rc != 0) {
      COBALT_LOGE("rng: ctr_drbg seeding failed (-0x%04x)", (unsigned) -rc);
      mbedtls_ctr_drbg_free(&s.drbg);
      SDL_UnlockMutex(s.lock);
      return false;
   }

   /* Prediction resistance would force a reseed from seed_source() on every
    * draw, and there is nothing left to reseed *from* — the seed file is a
    * one-shot. The rotate-on-boot cycle in atproto.c is what stops one seed
    * being reused across runs. */
   mbedtls_ctr_drbg_set_prediction_resistance(&s.drbg, MBEDTLS_CTR_DRBG_PR_OFF);

   s.ready = true;
   SDL_UnlockMutex(s.lock);

   COBALT_LOGI("rng: seeded from %u bytes of provisioned entropy",
               (unsigned) seed_len);
   return true;
}

void
cobalt_rng_shutdown(void)
{
   if (s.lock) {
      SDL_LockMutex(s.lock);
   }

   if (s.ready) {
      mbedtls_ctr_drbg_free(&s.drbg);
      s.ready = false;
   }

   if (s.lock) {
      SDL_UnlockMutex(s.lock);
      SDL_DestroyMutex(s.lock);
      s.lock = NULL;
   }
}

bool
cobalt_rng_ready(void)
{
   return s.ready;
}

bool
cobalt_rng_bytes(unsigned char *out, size_t len)
{
   if (!out || len == 0) {
      return false;
   }

   if (!s.ready || !s.lock) {
      wipe(out, len);
      COBALT_LOGE("rng: draw refused — no entropy seed has been provisioned");
      return false;
   }

   SDL_LockMutex(s.lock);
   int rc = mbedtls_ctr_drbg_random(&s.drbg, out, len);
   SDL_UnlockMutex(s.lock);

   if (rc != 0) {
      wipe(out, len);
      COBALT_LOGE("rng: draw failed (-0x%04x)", (unsigned) -rc);
      return false;
   }
   return true;
}

int
cobalt_rng_mbedtls(void *userdata, unsigned char *output, size_t len)
{
   (void) userdata;

   if (cobalt_rng_bytes(output, len)) {
      return 0;
   }
   /* Reported in mbedTLS's own vocabulary so a TLS failure caused by this is
    * legible in a handshake trace rather than showing up as a generic error. */
   return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
}
