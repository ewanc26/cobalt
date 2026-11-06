#pragma once

/*
 * Composer drafts on the SD card.
 *
 * Format v1: a version line, one tab-separated line per draft, then an "end"
 * line. Every text field is escaped (backslash, newline, carriage return and
 * tab), so a post can contain any of them without breaking the record. The
 * final "end" line and the version make a truncated or incompatible file fail
 * as a whole instead of loading a prefix. Writes go to a temporary file and are
 * renamed into place, so a crash leaves the previous list intact.
 */

#include "app/drafts.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_DRAFTS_VERSION 1

/* The largest escaped draft, with room for every field and its separators. */
#define COBALT_DRAFTS_FILE_MAX \
   (COBALT_DRAFT_MAX * (2 * (COBALT_DRAFT_TEXT_MAX + 4 * COBALT_POST_URI_MAX + \
                             2 * COBALT_POST_CID_MAX + 2 * COBALT_POST_NAME_MAX) + 96) + 64)

bool cobalt_drafts_encode(const cobalt_drafts *drafts, char *out, size_t cap);

/* False, with `drafts` left empty, for anything that is not a whole,
 * well-formed list. */
bool cobalt_drafts_decode(const char *data, size_t len, cobalt_drafts *drafts);

bool cobalt_drafts_save(const char *path, const cobalt_drafts *drafts);

/* A missing file is an empty list and returns true. A file that exists but does
 * not decode returns false and leaves `drafts` empty. */
bool cobalt_drafts_load(const char *path, cobalt_drafts *drafts);

#ifdef __cplusplus
}
#endif
