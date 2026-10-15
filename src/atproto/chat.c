#include "atproto/chat.h"
#include "util/log.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/chat_typed.h>
#include <wolfram/time.h>
#endif

#include <stdio.h>
#include <string.h>

/* Members past this are not looked at. A convo with more is a group, and the
 * inbox labels every group the same, so the rest would not change the row. */
#define COBALT_CHAT_MEMBERS_SCANNED 32

void
cobalt_chat_inbox_reset(cobalt_chat_inbox *inbox)
{
   if (inbox) {
      inbox->count = 0;
      inbox->unread = 0;
      inbox->cursor[0] = '\0';
      inbox->has_more = false;
   }
}

bool
cobalt_chat_inbox_can_page(const cobalt_chat_inbox *inbox)
{
   return inbox && inbox->has_more && inbox->count < COBALT_CHAT_CONVOS_MAX;
}

int
cobalt_chat_pick_counterpart(const char *const *member_dids, size_t count,
                             const char *viewer_did)
{
   if (!member_dids) {
      return -1;
   }

   for (size_t i = 0; i < count; i++) {
      const char *did = member_dids[i];
      if (!did || did[0] == '\0') {
         continue;
      }
      if (viewer_did && strcmp(did, viewer_did) == 0) {
         continue;
      }
      return (int) i;
   }

   return -1;
}

int
cobalt_chat_other_member_count(const char *const *member_dids, size_t count,
                               const char *viewer_did)
{
   int others = 0;

   if (!member_dids) {
      return 0;
   }

   for (size_t i = 0; i < count; i++) {
      const char *did = member_dids[i];
      if (!did || did[0] == '\0') {
         continue;
      }
      if (viewer_did && strcmp(did, viewer_did) == 0) {
         continue;
      }
      others++;
   }

   return others;
}

void
cobalt_chat_thread_reset(cobalt_chat_thread *thread)
{
   if (thread) {
      thread->convo_id[0] = '\0';
      thread->count = 0;
   }
}

#ifdef COBALT_HAS_WOLFRAM

int
cobalt_chat_thread_fill_from_wolfram(cobalt_chat_thread *out,
                                     const struct wf_chat_message_list *list,
                                     const char *viewer_did, int64_t now)
{
   const wf_chat_message_list *typed = (const wf_chat_message_list *) list;

   if (!out || !typed) {
      return 0;
   }

   /* The newest messages come first on the wire. Take the first `n` and write
    * them back to front, so index 0 ends up as the oldest of those held. */
   const size_t n = typed->message_count < COBALT_CHAT_MESSAGES_MAX
                       ? typed->message_count
                       : COBALT_CHAT_MESSAGES_MAX;
   out->count = 0;

   for (size_t k = 0; k < n; k++) {
      const wf_chat_message *src = &typed->messages[k];
      cobalt_chat_message *dst = &out->items[n - 1 - k];
      int64_t epoch = 0;

      memset(dst, 0, sizeof(*dst));
      cobalt_feed_copy_text(dst->text, sizeof(dst->text), src->text ? src->text : "");
      dst->mine = viewer_did && src->sender && strcmp(src->sender, viewer_did) == 0;
      if (src->sent_at && now > 0 && wf_time_parse_rfc3339(src->sent_at, &epoch) == WF_OK) {
         wf_time_relative(epoch, now, dst->age, sizeof(dst->age));
      }
   }

   out->count = (int) n;
   return out->count;
}

int
cobalt_chat_inbox_append_from_wolfram(cobalt_chat_inbox *out,
                                      const struct wf_chat_convo_list *list,
                                      const char *viewer_did)
{
   if (!out || !list) {
      return 0;
   }

   const wf_chat_convo_list *typed = (const wf_chat_convo_list *) list;
   int added = 0;

   for (size_t i = 0; i < typed->convo_count; i++) {
      if (out->count >= COBALT_CHAT_CONVOS_MAX) {
         COBALT_LOGI("chat: inbox full at %d", out->count);
         break;
      }

      const wf_chat_convo *src = &typed->convos[i];
      const size_t scanned = src->member_count < COBALT_CHAT_MEMBERS_SCANNED
                                ? src->member_count
                                : COBALT_CHAT_MEMBERS_SCANNED;
      const char *dids[COBALT_CHAT_MEMBERS_SCANNED];

      for (size_t m = 0; m < scanned; m++) {
         dids[m] = src->members[m].did;
      }

      int others = cobalt_chat_other_member_count(dids, scanned, viewer_did);
      if (src->member_count > COBALT_CHAT_MEMBERS_SCANNED && others < 2) {
         others = 2;
      }
      if (others == 0) {
         /* Nobody but the viewer: nothing to show or reply to. */
         continue;
      }

      cobalt_chat_convo *item = &out->items[out->count];
      memset(item, 0, sizeof(*item));
      snprintf(item->id, sizeof(item->id), "%s", src->id ? src->id : "");

      if (others > 1) {
         item->group = true;
         snprintf(item->name, sizeof(item->name), "Group");
      } else {
         const int at = cobalt_chat_pick_counterpart(dids, scanned, viewer_did);
         const wf_agent_profile_view *who = at >= 0 ? &src->members[at] : NULL;
         const char *handle = who && who->handle ? who->handle : "";
         const char *display = who && who->display_name ? who->display_name : "";

         if (display[0] == '\0') {
            display = handle;
         }
         cobalt_feed_copy_text(item->name, sizeof(item->name), display);
         snprintf(item->handle, sizeof(item->handle), "@%s", handle);
         snprintf(item->avatar, sizeof(item->avatar), "%s",
                  who && who->avatar ? who->avatar : "");
      }

      /* Arbitrary message text, so truncate on a code point, as the feed does. */
      if (src->last_message_text) {
         cobalt_feed_copy_text(item->preview, sizeof(item->preview),
                               src->last_message_text);
      }

      item->unread = src->has_unread_count ? src->unread_count : 0;
      if (item->unread < 0) {
         item->unread = 0;
      }
      out->unread += item->unread;

      out->count++;
      added++;
   }

   /* Cursor handling matches the notifications list: a full inbox clears it so
    * an auto-paging screen cannot request further pages forever. */
   if (out->count >= COBALT_CHAT_CONVOS_MAX) {
      out->cursor[0] = '\0';
      out->has_more = false;
   } else if (typed->cursor && typed->cursor[0]) {
      snprintf(out->cursor, sizeof(out->cursor), "%s", typed->cursor);
      out->has_more = true;
   } else {
      out->cursor[0] = '\0';
      out->has_more = false;
   }

   return added;
}

#endif /* COBALT_HAS_WOLFRAM */
