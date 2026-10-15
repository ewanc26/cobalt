#pragma once

/*
 * The Queued posts screen: the posts saved with "Save for later", oldest first.
 *
 * The screen only reports what the person chose. Sending and discarding change
 * the queue in the app, which owns the network and the storage, so a send
 * never happens from inside this file. Nothing here retries; a post that fails
 * to send comes back to the queue and stays until the person sends or discards
 * it again.
 */

#include "app/post_queue.h"
#include "input/input.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_QUEUE_STAY = 0,
   COBALT_QUEUE_BACK,
   COBALT_QUEUE_SEND,     /* the person chose to send the selected post */
   COBALT_QUEUE_DISCARD,  /* the person chose to throw the selected post away */
} cobalt_queue_action;

typedef struct {
   unsigned selected;
} cobalt_queue_view;

void cobalt_queue_view_open(cobalt_queue_view *view);

/* On SEND or DISCARD, `*index` is the post the action applies to. */
cobalt_queue_action cobalt_queue_view_update(cobalt_queue_view *view,
                                             const cobalt_post_queue *queue,
                                             const cobalt_input *in,
                                             unsigned *index);

void cobalt_queue_view_draw(const cobalt_queue_view *view,
                            const cobalt_post_queue *queue, cobalt_render *r);

#ifdef __cplusplus
}
#endif
