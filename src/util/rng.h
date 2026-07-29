#pragma once

/*
 * The application's random number generator.
 *
 * Why Cobalt has to own one
 * -------------------------
 * devkitPro's Wii U mbedTLS is built with MBEDTLS_NO_PLATFORM_ENTROPY, and the
 * `mbedtls_hardware_poll` it supplies in its place is, per byte:
 *
 *     srand(OSGetSystemTick());
 *     output[i] = rand() & 0xff;
 *
 * That is the *only* source in the entropy pool — nothing sits behind it. So
 * every draw from mbedtls_entropy_func() on this console is a function of the
 * tick counter, which an attacker can narrow down to a small window from the
 * console's uptime. It cannot be replaced at link time either: the devkitPro
 * patch puts that function in library/entropy.c, the same translation unit
 * that registers it, so a competing definition collides and `ld --wrap` does
 * not redirect an intra-unit reference.
 *
 * Cobalt therefore runs its own CTR-DRBG, seeded from the installation-unique
 * 64-byte seed on the SD card (see util/entropy.h) rather than from mbedTLS's
 * pool. Everything in the app that needs unpredictable bytes draws from here:
 *
 *   - the TLS handshake, via wf_xrpc_client_set_tls_rng() — client randoms and
 *     ephemeral key agreement for every HTTPS request;
 *   - the credential store's device key and CTR nonces (cache/session_store.c).
 *
 * Wolfram's P-256 signing is seeded separately through wf_wiiu_set_entropy_seed()
 * from the same file, and keeps its own rotate/commit discipline.
 *
 * Without a seed this stays unavailable and every caller fails closed. That is
 * deliberate: an HTTPS request Cobalt knows is weakly randomised is worse than
 * one it declines to make, because the user cannot tell the difference.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seed the DRBG. `seed` should be COBALT_ENTROPY_SEED_SIZE bytes of real
 * entropy; the caller keeps ownership and should wipe its copy afterwards.
 *
 * Safe to call again to re-seed. Returns false if seeding failed, in which
 * case the generator stays unavailable rather than falling back to anything
 * weaker.
 */
bool cobalt_rng_init(const unsigned char *seed, size_t seed_len);

void cobalt_rng_shutdown(void);

/* False until a successful cobalt_rng_init(). */
bool cobalt_rng_ready(void);

/*
 * Draw `len` bytes. Returns false if the generator is unseeded or the draw
 * failed; `out` is zeroed on failure so a partial result cannot be mistaken
 * for randomness.
 */
bool cobalt_rng_bytes(unsigned char *out, size_t len);

/*
 * The same generator in mbedTLS's `f_rng` shape: 0 on success, non-zero on
 * failure. This is what gets handed to wf_xrpc_client_set_tls_rng(), which
 * passes it straight to mbedtls_ssl_conf_rng(). `userdata` is ignored — the
 * generator is a singleton, since there is one seed per installation.
 *
 * Thread-safe: libcurl calls this from whichever thread is running a request,
 * which for Cobalt is the session worker.
 */
int cobalt_rng_mbedtls(void *userdata, unsigned char *output, size_t len);

#ifdef __cplusplus
}
#endif
