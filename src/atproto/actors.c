#include "atproto/actors.h"
#include "util/log.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/agent.h>
#endif

#include <stdio.h>
#include <string.h>

void
cobalt_actor_list_reset(cobalt_actor_list *list)
{
   if (list) {
      list->count = 0;
   }
}

bool
cobalt_actor_list_can_page(const cobalt_actor_list *list)
{
   return list && list->has_more && list->count < COBALT_ACTORS_MAX;
}

bool
cobalt_actor_list_remove(cobalt_actor_list *list, const char *did)
{
   if (!list || !did) {
      return false;
   }

   for (int i = 0; i < list->count; i++) {
      if (strcmp(list->actors[i].did, did) != 0) {
         continue;
      }
      /* Shift the tail down rather than leaving a hole — the list is drawn
       * by index 0..count, so a hole would draw as a blank row. */
      for (int j = i; j < list->count - 1; j++) {
         list->actors[j] = list->actors[j + 1];
      }
      list->count--;
      return true;
   }
   return false;
}

#ifdef COBALT_HAS_WOLFRAM

int
cobalt_actor_list_append_from_wolfram(cobalt_actor_list *list,
                                      const struct wf_agent_actor_list *src)
{
   if (!list || !src) {
      return 0;
   }

   const wf_agent_actor_list *typed = (const wf_agent_actor_list *) src;
   int added = 0;

   for (size_t i = 0; i < typed->actor_count; i++) {
      if (list->count >= COBALT_ACTORS_MAX) {
         COBALT_LOGI("actors: window full at %d, dropping the rest of the page",
                     list->count);
         break;
      }

      const wf_agent_profile_view *src_actor = &typed->actors[i];
      cobalt_actor *out = &list->actors[list->count];
      memset(out, 0, sizeof(*out));

      snprintf(out->did, sizeof(out->did), "%s",
               src_actor->did ? src_actor->did : "");
      snprintf(out->handle, sizeof(out->handle), "@%s",
               src_actor->handle ? src_actor->handle : "");

      const char *display = src_actor->display_name;
      if (!display || display[0] == '\0') {
         display = src_actor->handle ? src_actor->handle : "";
      }
      cobalt_feed_copy_text(out->display_name, sizeof(out->display_name),
                            display);

      snprintf(out->avatar, sizeof(out->avatar), "%s",
               src_actor->avatar ? src_actor->avatar : "");
      snprintf(out->record_uri, sizeof(out->record_uri), "%s",
               src_actor->blocking ? src_actor->blocking : "");

      list->count++;
      added++;
   }

   if (list->count >= COBALT_ACTORS_MAX) {
      list->cursor[0] = '\0';
      list->has_more = false;
   } else if (typed->cursor && typed->cursor[0]) {
      snprintf(list->cursor, sizeof(list->cursor), "%s", typed->cursor);
      list->has_more = true;
   } else {
      list->cursor[0] = '\0';
      list->has_more = false;
   }

   return added;
}

#endif /* COBALT_HAS_WOLFRAM */
