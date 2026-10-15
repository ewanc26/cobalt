#pragma once

/*
 * The offline post queue: posts a person wrote while the console had no
 * connection, kept until they choose to send or discard them.
 *
 * This module is storage only, and that is deliberate. It has no network
 * access and no way to publish: the only thing that leaves the queue is
 * cobalt_post_queue_take(), called for one post at a time by a person's own
 * choice. AGENTS.md is firm that a queued action must never publish without the
 * person understanding that it was queued, so nothing here sends anything on
 * its own, and nothing flushes the queue as a whole.
 *
 * Text is refused rather than cut when it is too long. A post that is silently
 * shortened is not the post the person wrote.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_POST_QUEUE_MAX 16
#define COBALT_POST_QUEUE_TEXT_MAX 1024

typedef struct {
   char text[COBALT_POST_QUEUE_TEXT_MAX];
   /* Seconds since the epoch, from the caller's clock; for ordering and for
    * showing how long a post has been waiting. */
   long long queued_at;
} cobalt_queued_post;

typedef struct {
   cobalt_queued_post items[COBALT_POST_QUEUE_MAX];
   unsigned count;
} cobalt_post_queue;

void cobalt_post_queue_init(cobalt_post_queue *q);

/* Append a post. False, with nothing stored, when the queue is full, the text
 * is empty, or the text does not fit COBALT_POST_QUEUE_TEXT_MAX - 1 bytes. */
bool cobalt_post_queue_add(cobalt_post_queue *q, const char *text, long long now);

/* The post at `index` (oldest first), or NULL when out of range. */
const cobalt_queued_post *cobalt_post_queue_get(const cobalt_post_queue *q,
                                                unsigned index);

/* Discard the post at `index`. The rest keep their order. False when out of
 * range. */
bool cobalt_post_queue_discard(cobalt_post_queue *q, unsigned index);

/* Remove the post at `index` and copy it to `out`, for the person to send.
 * False when out of range. The caller must send it only on the person's own
 * request, and must put it back with cobalt_post_queue_add if the send fails. */
bool cobalt_post_queue_take(cobalt_post_queue *q, unsigned index,
                            cobalt_queued_post *out);

#ifdef __cplusplus
}
#endif
