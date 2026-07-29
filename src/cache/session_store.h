#pragma once

/*
 * Persisted PDS credentials.
 *
 * AGENTS.md §7 asks for the session/refresh token to be stored on the SD card
 * "encrypted or at minimum not in plaintext next to other save data", with a
 * sign-out that wipes it. This module is that.
 *
 * What the encryption is, and is not
 * ----------------------------------
 * The payload is AES-256-CTR under a 32-byte key held in `device.key`, which is
 * generated once per installation and lives in the same directory. That is
 * deliberate and it is worth being blunt about what it buys:
 *
 *   - It does defend against the credential leaking incidentally: a log dump, a
 *     screenshot of the card's contents, a stray copy of session.dat, someone
 *     opening the file in a text editor. That is the realistic exposure for a
 *     token sitting on a removable card in a console.
 *   - It does NOT defend against anyone who has the whole SD card. Both files
 *     are right there. The Wii U gives homebrew no keystore, no secure element
 *     and no per-title secret to bind to, so there is nothing to derive a key
 *     from that an attacker with the card would not also have.
 *
 * Treating this as real at-rest encryption would be wrong. Treating it as
 * "not plaintext", which is exactly the bar §7 sets, is accurate.
 *
 * Sign-out overwrites both files before unlinking them, so the tokens do not
 * survive as recoverable data in the directory entry.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ATProto access/refresh JWTs run to a few hundred bytes; this leaves room for
 * a PDS that is more generous with claims without going anywhere near the
 * console's memory budget. */
#define COBALT_JWT_MAX      1024
#define COBALT_HANDLE_MAX    128
#define COBALT_DID_MAX       128
#define COBALT_SERVICE_MAX   192

typedef struct {
   /* The PDS these tokens are valid for. Stored because an account's real PDS
    * is discovered at login (from didDoc#atproto_pds) and is often not the
    * entry-point host the user typed. */
   char service[COBALT_SERVICE_MAX];
   char handle[COBALT_HANDLE_MAX];
   char did[COBALT_DID_MAX];
   char access_jwt[COBALT_JWT_MAX];
   char refresh_jwt[COBALT_JWT_MAX];
} cobalt_stored_session;

/*
 * Write the credentials to the data root. Writes to a temporary file and
 * renames, so a power loss mid-write leaves the previous session intact rather
 * than a half-written one.
 */
bool cobalt_session_store_save(const cobalt_stored_session *session);

/*
 * Read the credentials back. Returns false — with `out` fully zeroed — if the
 * file is absent, truncated, from a different format version, or does not
 * decrypt cleanly under the current device key.
 */
bool cobalt_session_store_load(cobalt_stored_session *out);

/* True if a stored session file is present. Does not attempt to decrypt it. */
bool cobalt_session_store_exists(void);

/*
 * Wipe the stored session. Overwrites the file's bytes before unlinking, and
 * removes the device key too so that any copy of session.dat taken beforehand
 * is left without its key. Returns true if nothing is left behind.
 */
bool cobalt_session_store_clear(void);

#ifdef __cplusplus
}
#endif
