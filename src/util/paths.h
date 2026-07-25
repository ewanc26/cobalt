#pragma once

/*
 * Filesystem path resolution.
 *
 * Cobalt has to find its assets in two quite different situations and cannot
 * tell them apart at compile time:
 *
 *   - Launched as a WUHB (the supported path, AGENTS.md §3): Aroma mounts the
 *     bundle's content directory at /vol/content.
 *   - Launched as a bare RPX during development: there is no content mount,
 *     and the assets sit next to the executable on the SD card.
 *
 * Rather than guess, cobalt_paths_init() probes each candidate root for a
 * sentinel file and remembers the one that answered. Every probe is recorded
 * and dumped by cobalt_paths_log() once logging is up, so a failure to find
 * assets on real hardware shows up as an explicit list of what was tried
 * instead of a bare "font load failed".
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_PATH_MAX 256

/* File that must exist in the content directory for a root to be accepted. */
#define COBALT_CONTENT_MARKER "content.marker"

/*
 * Mount the SD card and locate the content and data roots.
 * Returns true if a content root was found. Data-root resolution is
 * independent and falls back to the content root when the SD card is
 * unavailable, so logging still has somewhere to go.
 */
bool cobalt_paths_init(void);

void cobalt_paths_shutdown(void);

/* Dump the probe results. Call once after cobalt_log_init(). */
void cobalt_paths_log(void);

/* Read-only bundled assets (fonts, CA bundle, icons). */
bool cobalt_content_path(char *out, size_t out_size, const char *rel);

/* Writable per-app storage on SD (logs, session, cache). */
bool cobalt_data_path(char *out, size_t out_size, const char *rel);

/* NULL until cobalt_paths_init() succeeds. */
const char *cobalt_content_root(void);
const char *cobalt_data_root(void);

#ifdef __cplusplus
}
#endif
