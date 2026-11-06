#include "app/drafts.h"

#include "util/log.h"

#include <string.h>

void
cobalt_drafts_init(cobalt_drafts *drafts)
{
   if (drafts) {
      memset(drafts, 0, sizeof(*drafts));
   }
}

bool
cobalt_draft_valid(const cobalt_draft *draft)
{
   if (!draft || draft->text[0] == '\0' ||
       strlen(draft->text) >= COBALT_DRAFT_TEXT_MAX) {
      return false;
   }
   switch (draft->kind) {
      case COBALT_DRAFT_REPLY:
         return draft->parent_uri[0] && draft->parent_cid[0] &&
                draft->root_uri[0] && draft->root_cid[0];
      case COBALT_DRAFT_QUOTE:
         return draft->quote_uri[0] && draft->quote_cid[0];
      case COBALT_DRAFT_POST:
         return true;
      default:
         return false;
   }
}

bool
cobalt_drafts_add(cobalt_drafts *drafts, const cobalt_draft *draft)
{
   if (!drafts || !cobalt_draft_valid(draft) ||
       drafts->count >= COBALT_DRAFT_MAX) {
      return false;
   }
   drafts->items[drafts->count] = *draft;
   drafts->count++;
   return true;
}

const cobalt_draft *
cobalt_drafts_get(const cobalt_drafts *drafts, unsigned index)
{
   if (!drafts || index >= drafts->count) {
      return NULL;
   }
   return &drafts->items[index];
}

bool
cobalt_drafts_replace(cobalt_drafts *drafts, unsigned index,
                      const cobalt_draft *draft)
{
   if (!drafts || index >= drafts->count || !cobalt_draft_valid(draft)) {
      return false;
   }
   drafts->items[index] = *draft;
   return true;
}

bool
cobalt_drafts_remove(cobalt_drafts *drafts, unsigned index)
{
   if (!drafts || index >= drafts->count) {
      return false;
   }
   for (unsigned i = index + 1; i < drafts->count; i++) {
      drafts->items[i - 1] = drafts->items[i];
   }
   drafts->count--;
   memset(&drafts->items[drafts->count], 0, sizeof(drafts->items[drafts->count]));
   return true;
}
