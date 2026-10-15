/* Direct messages: conversation lists, messages and the helpers that flatten
 * Wolfram's chat responses into fixed-size structs for drawing. */

#include "atproto/dms.h"
#include "atproto/feed.h"
#include "util/clock.h"

#include <stdio.h>
#include <string.h>

void
cobalt_convos_reset(cobalt_convos *list)
{
   memset(list, 0, sizeof(*list));
}

bool
cobalt_convos_can_page(const cobalt_convos *list)
{
   return list && list->has_more && list->count < COBALT_CONVO_MAX;
}

void
cobalt_messages_reset(cobalt_messages *list)
{
   memset(list, 0, sizeof(*list));
}

#ifdef COBALT_HAS_WOLFRAM

#include <wolfram/chat_typed.h>
#include <wolfram/time.h>

static const wf_agent_profile_view *
pick_peer(const wf_chat_convo *wc, const char *my_did)
{
   for (size_t i = 0; i < wc->member_count; i++) {
      const wf_agent_profile_view *m = &wc->members[i];
      if (m->did && my_did && strcmp(m->did, my_did) != 0) {
         return m;
      }
   }
   return wc->member_count > 0 ? &wc->members[0] : NULL;
}

bool
cobalt_convos_append_from_wolfram(cobalt_convos *out, const wf_chat_convo_list *list,
                                 const char *my_did)
{
   for (size_t i = 0; i < list->convo_count && out->count < COBALT_CONVO_MAX; i++) {
      const wf_chat_convo *wc = &list->convos[i];
      cobalt_convo *o = &out->items[out->count];

      memset(o, 0, sizeof(*o));
      snprintf(o->id, sizeof o->id, "%s", wc->id ? wc->id : "");
      o->has_unread = wc->has_unread_count;
      o->unread = wc->has_unread_count ? wc->unread_count : 0;

      const wf_agent_profile_view *peer = pick_peer(wc, my_did);
      if (peer) {
         snprintf(o->handle, sizeof o->handle, "%s", peer->handle ? peer->handle : "");
         if (peer->display_name && peer->display_name[0]) {
            cobalt_feed_copy_text(o->name, sizeof o->name, peer->display_name);
         } else {
            cobalt_feed_copy_text(o->name, sizeof o->name, peer->handle ? peer->handle : "");
         }
         snprintf(o->avatar, sizeof o->avatar, "%s", peer->avatar ? peer->avatar : "");
      }
      if (wc->last_message_text) {
         cobalt_feed_copy_text(o->last_text, sizeof o->last_text, wc->last_message_text);
      }
      out->count++;
   }

   if (list->cursor) {
      snprintf(out->cursor, sizeof out->cursor, "%s", list->cursor);
      out->has_more = true;
   } else {
      out->has_more = false;
   }

   return true;
}

bool
cobalt_messages_append_from_wolfram(cobalt_messages *out, const wf_chat_message_list *list,
                                    const char *my_did)
{
   const int64_t now = cobalt_time_now();

   for (size_t i = 0; i < list->message_count && out->count < COBALT_DMS_MSG_MAX; i++) {
      const wf_chat_message *wm = &list->messages[i];
      cobalt_message *o = &out->items[out->count];

      memset(o, 0, sizeof(*o));
      snprintf(o->id, sizeof o->id, "%s", wm->id ? wm->id : "");
      cobalt_feed_copy_text(o->text, sizeof o->text, wm->text ? wm->text : "");
      snprintf(o->sender, sizeof o->sender, "%s", wm->sender ? wm->sender : "");
      o->is_mine = (my_did && wm->sender && strcmp(wm->sender, my_did) == 0);

      if (wm->sent_at) {
         int64_t epoch = 0;
         if (wf_time_parse_rfc3339(wm->sent_at, &epoch) == WF_OK && now > 0) {
            wf_time_relative(epoch, now, o->age, sizeof o->age);
         }
      }
      out->count++;
   }

   if (list->cursor) {
      snprintf(out->cursor, sizeof out->cursor, "%s", list->cursor);
      out->has_more = true;
   } else {
      out->has_more = false;
   }

   return true;
}

#endif /* COBALT_HAS_WOLFRAM */
