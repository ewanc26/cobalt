#include "app/post_queue.h"

#include <string.h>

void
cobalt_post_queue_init(cobalt_post_queue *q)
{
   memset(q, 0, sizeof(*q));
}

bool
cobalt_post_queue_add(cobalt_post_queue *q, const char *text, long long now)
{
   size_t len;

   if (!q || !text || q->count >= COBALT_POST_QUEUE_MAX) {
      return false;
   }
   len = strlen(text);
   if (len == 0 || len >= COBALT_POST_QUEUE_TEXT_MAX) {
      return false;
   }

   cobalt_queued_post *p = &q->items[q->count];
   memcpy(p->text, text, len + 1);
   p->queued_at = now;
   q->count++;
   return true;
}

const cobalt_queued_post *
cobalt_post_queue_get(const cobalt_post_queue *q, unsigned index)
{
   if (!q || index >= q->count) {
      return NULL;
   }
   return &q->items[index];
}

/* Close the gap left at `index`, keeping the order of the rest. */
static void
remove_at(cobalt_post_queue *q, unsigned index)
{
   for (unsigned i = index + 1; i < q->count; i++) {
      q->items[i - 1] = q->items[i];
   }
   q->count--;
   memset(&q->items[q->count], 0, sizeof(q->items[q->count]));
}

bool
cobalt_post_queue_discard(cobalt_post_queue *q, unsigned index)
{
   if (!q || index >= q->count) {
      return false;
   }
   remove_at(q, index);
   return true;
}

bool
cobalt_post_queue_take(cobalt_post_queue *q, unsigned index, cobalt_queued_post *out)
{
   if (!q || !out || index >= q->count) {
      return false;
   }
   *out = q->items[index];
   remove_at(q, index);
   return true;
}
