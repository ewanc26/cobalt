#pragma once

/*
 * The offline queue on the SD card, so a saved post survives closing the app.
 *
 * Format v1: a version line, then one line per post, then an "end" line. The
 * text is escaped (backslash, newline, carriage return and tab) so one post is
 * always one line. A file that does not decode is ignored and the queue starts
 * empty: a damaged queue must not stop the app from starting. The file is
 * written to a temporary name and renamed, so a crash leaves the old one whole.
 */

#include "app/post_queue.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_QUEUE_VERSION 1

/* Encode `q` into `out` (NUL-terminated). False if it does not fit `cap`. */
bool cobalt_queue_encode(const cobalt_post_queue *q, char *out, size_t cap);

/* Decode `data`. False, with `q` left empty, for anything that is not a whole,
 * well-formed queue. */
bool cobalt_queue_decode(const char *data, size_t len, cobalt_post_queue *q);

/* Write the queue to `path`, atomically. False on I/O failure. */
bool cobalt_queue_save(const char *path, const cobalt_post_queue *q);

/* Load `path` into `q`. A missing file is an empty queue and returns true; a
 * file that exists but does not decode returns false and leaves `q` empty. */
bool cobalt_queue_load(const char *path, cobalt_post_queue *q);

#ifdef __cplusplus
}
#endif
