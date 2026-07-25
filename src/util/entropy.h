#pragma once

/*
 * Entropy seed persistence.
 *
 * The Wii U has no application-facing CSPRNG that Wolfram will accept:
 * devkitPro's mbedTLS backs mbedtls_hardware_poll with
 * srand(OSGetSystemTick())/rand(), so Wolfram fails closed on P-256 signing
 * and key generation until a real seed is provisioned. See AGENTS.md §13.
 *
 * The seed therefore lives on the SD card and is rotated on every boot. This
 * mirrors what Channel Blue already does on the Wii (source/app/entropy_seed.c
 * there) — same discipline, same file format, so the two projects do not drift
 * apart on a security-sensitive detail.
 *
 * The file is NOT distributable. `make bundle` generates a fresh one per
 * installation; shipping a common seed would give every console identical key
 * material.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_ENTROPY_SEED_SIZE 64

/*
 * Read exactly COBALT_ENTROPY_SEED_SIZE bytes. Returns false if the file is
 * missing or the wrong length, zeroing `seed` on every failure path so a
 * partial read can never be mistaken for entropy.
 */
bool cobalt_entropy_seed_load(const char *path,
                              unsigned char seed[COBALT_ENTROPY_SEED_SIZE]);

/*
 * Write via a temporary file and rename, so a power loss mid-write leaves the
 * previous seed intact rather than a truncated one.
 */
bool cobalt_entropy_seed_save(const char *path,
                              const unsigned char seed[COBALT_ENTROPY_SEED_SIZE]);

#ifdef __cplusplus
}
#endif
