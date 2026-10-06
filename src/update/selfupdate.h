#pragma once

/*
 * Self-update core: Cobalt's manifest policy and the staged file replacement.
 * No SDL and no network, so all of it is tested on the host.
 *
 * Manifest parsing, version comparison and SHA-256 are Wolfram's
 * (`wolfram/update.h`, since v0.27.0). What stays here is the Wii U half:
 * Cobalt's policy for the manifest and the staged file replacement.
 *
 * What it does and does not protect against. The SHA-256 comes from the same
 * GitHub release as the file, over HTTPS. That catches a corrupt or truncated
 * download and a swapped asset, not a compromised release. The manifest's
 * `signature` field is reserved for a detached signature; nothing verifies one
 * yet, because the signing key is the owner's to create (needs-owner).
 */

#include "wolfram/update.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Updates come from this repository's releases and nowhere else. */
#define COBALT_UPDATE_MANIFEST_URL \
   "https://github.com/ewanc26/cobalt/releases/latest/download/update.json"
#define COBALT_UPDATE_ASSET_PREFIX "https://github.com/ewanc26/cobalt/releases/download/"

#define COBALT_UPDATE_MANIFEST_MAX 16384
#define COBALT_UPDATE_WUHB_MAX (24u * 1024u * 1024u)

typedef enum {
   COBALT_UPDATE_OK = 0,
   COBALT_UPDATE_BAD_MANIFEST, /* Wolfram refused the manifest (see docs/update.md there) */
   COBALT_UPDATE_BAD_POLICY    /* wrong app, or not under this repo's releases */
} cobalt_update_status;

const char *cobalt_update_status_string(cobalt_update_status st);

/*
 * Parse and validate update.json with Wolfram's wf_update_parse_manifest, under
 * Cobalt's policy: app "cobalt", an asset URL under COBALT_UPDATE_ASSET_PREFIX
 * and named `v<version>/<name>`, and at most `max_size` bytes. Nothing is
 * truncated. Version comparison and download checksums are Wolfram's too
 * (wf_update_compare_versions, wf_update_verify_*); use them directly.
 */
cobalt_update_status cobalt_update_parse_manifest(const char *body, size_t len,
                                                  unsigned long max_size,
                                                  wf_update_manifest *out);

/* Constant-time comparison of two digests. */
bool cobalt_sha256_equal(const unsigned char a[32], const unsigned char b[32]);

/* Digest of a file on disk. False if it cannot be read. */
bool cobalt_sha256_file(const char *path, unsigned char out[32]);

/*
 * Staged replacement of the installed .wuhb.
 *
 *   <stage>/cobalt.wuhb.part   being written; never trusted
 *   <stage>/cobalt.wuhb.new    written, re-read from disk and verified
 *   <stage>/cobalt.wuhb.old    the previous build, kept until the new one has run
 *
 * The installed file is only touched by cobalt_update_apply, and only with a
 * file that passed verification. FAT has no atomic replace, so apply is two
 * renames; cobalt_update_recover repairs the one gap between them.
 */
#define COBALT_UPDATE_PATH_MAX 320

typedef struct {
   char installed[COBALT_UPDATE_PATH_MAX];
   char part[COBALT_UPDATE_PATH_MAX];
   char staged[COBALT_UPDATE_PATH_MAX];
   char old[COBALT_UPDATE_PATH_MAX];
} cobalt_update_paths;

bool cobalt_update_paths_init(cobalt_update_paths *p, const char *installed,
                              const char *stage_dir);

typedef enum {
   COBALT_STAGE_OK = 0,
   COBALT_STAGE_WRITE_FAILED,
   COBALT_STAGE_MISMATCH,   /* the bytes written do not hash to `expect` */
   COBALT_STAGE_RENAME_FAILED
} cobalt_stage_result;

/*
 * Verify `data` against `expect`, write it, re-read the file and verify again,
 * then publish it as the staged file. On any failure the .part file is removed
 * and the installed build is untouched.
 */
cobalt_stage_result cobalt_update_stage(const cobalt_update_paths *p, const void *data,
                                        size_t size, const unsigned char expect[32]);

bool cobalt_update_has_staged(const cobalt_update_paths *p);

/* Move the installed build aside and put the staged one in its place. */
bool cobalt_update_apply(const cobalt_update_paths *p);

typedef enum {
   COBALT_RECOVER_NOTHING = 0,
   COBALT_RECOVER_RESTORED,     /* the installed file was missing; put .old back */
   COBALT_RECOVER_CLEANED       /* removed a stale .part or .new */
} cobalt_recover_result;

/* Run at startup, before anything else reads the stage directory. */
cobalt_recover_result cobalt_update_recover(const cobalt_update_paths *p);

/* The new build has started and run: drop the previous one. */
bool cobalt_update_commit(const cobalt_update_paths *p);

#ifdef __cplusplus
}
#endif
