#include "atproto/curated_lists.h"
#include "util/log.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/agent.h>
#include <wolfram/list_typed.h>
#endif

#include <stdio.h>
#include <string.h>

void
cobalt_list_summary_list_reset(cobalt_list_summary_list *list)
{
   if (list) {
      list->count = 0;
   }
}

bool
cobalt_list_summary_list_can_page(const cobalt_list_summary_list *list)
{
   return list && list->has_more && list->count < COBALT_LISTS_MAX;
}

#ifdef COBALT_HAS_WOLFRAM

int
cobalt_list_summary_list_append_from_wolfram(
   cobalt_list_summary_list *list, const struct wf_agent_list_view_list *src)
{
   if (!list || !src) {
      return 0;
   }

   const wf_agent_list_view_list *typed =
      (const wf_agent_list_view_list *) src;
   int added = 0;

   for (size_t i = 0; i < typed->list_count; i++) {
      if (list->count >= COBALT_LISTS_MAX) {
         COBALT_LOGI("lists: window full at %d, dropping the rest of the page",
                     list->count);
         break;
      }

      const wf_agent_list_view *src_list = &typed->lists[i];
      cobalt_list_summary *out = &list->lists[list->count];
      memset(out, 0, sizeof(*out));

      snprintf(out->uri, sizeof(out->uri), "%s",
               src_list->uri ? src_list->uri : "");
      cobalt_feed_copy_text(out->name, sizeof(out->name),
                            src_list->name ? src_list->name : "");
      cobalt_feed_copy_text(out->description, sizeof(out->description),
                            src_list->description ? src_list->description
                                                   : "");
      snprintf(out->avatar, sizeof(out->avatar), "%s",
               src_list->avatar ? src_list->avatar : "");

      list->count++;
      added++;
   }

   if (list->count >= COBALT_LISTS_MAX) {
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

int
cobalt_actor_list_append_from_wolfram_list_items(
   cobalt_actor_list *list, const struct wf_agent_list_item_list *src)
{
   if (!list || !src) {
      return 0;
   }

   const wf_agent_list_item_list *typed =
      (const wf_agent_list_item_list *) src;
   int added = 0;

   for (size_t i = 0; i < typed->item_count; i++) {
      if (list->count >= COBALT_ACTORS_MAX) {
         COBALT_LOGI("lists: member window full at %d, dropping the rest of "
                     "the page",
                     list->count);
         break;
      }

      const wf_agent_profile_view *src_actor = &typed->items[i].subject;
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
      /* A list member has a listitem record, not a block/mute record — there
       * is nothing here for an "undo" action to delete, so record_uri stays
       * empty, same as a search result. */
      out->record_uri[0] = '\0';

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
