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

const cobalt_chat_convo *
cobalt_chat_inbox_find(const cobalt_chat_inbox *inbox, const char *convo_id)
{
   if (!inbox || !convo_id) {
      return NULL;
   }
   for (int i = 0; i < inbox->count && i < COBALT_CHAT_CONVOS_MAX; i++) {
      if (strcmp(inbox->items[i].id, convo_id) == 0) {
         return &inbox->items[i];
      }
   }
   return NULL;
}

bool
cobalt_chat_inbox_remove(cobalt_chat_inbox *inbox, const char *convo_id)
{
   if (!inbox || !convo_id) {
      return false;
   }
   for (int i = 0; i < inbox->count && i < COBALT_CHAT_CONVOS_MAX; i++) {
      if (strcmp(inbox->items[i].id, convo_id) != 0) {
         continue;
      }
      const int unread = inbox->items[i].unread > 0 ? inbox->items[i].unread : 0;
      inbox->unread = inbox->unread > unread ? inbox->unread - unread : 0;
      memmove(&inbox->items[i], &inbox->items[i + 1],
              (size_t) (inbox->count - i - 1) * sizeof(inbox->items[0]));
      inbox->count--;
      return true;
   }
   return false;
}

bool
cobalt_chat_inbox_add_front(cobalt_chat_inbox *inbox, const cobalt_chat_convo *item)
{
   if (!inbox || !item || cobalt_chat_inbox_find(inbox, item->id)) {
      return false;
   }
   int n = inbox->count < COBALT_CHAT_CONVOS_MAX ? inbox->count : COBALT_CHAT_CONVOS_MAX - 1;
   if (n < inbox->count) {
      /* Full: the last row goes, and its unread messages leave the total. */
      const int dropped = inbox->items[n].unread > 0 ? inbox->items[n].unread : 0;
      inbox->unread = inbox->unread > dropped ? inbox->unread - dropped : 0;
   }
   memmove(&inbox->items[1], &inbox->items[0], (size_t) n * sizeof(inbox->items[0]));
   inbox->items[0] = *item;
   inbox->count = n + 1;
   if (item->unread > 0) {
      inbox->unread += item->unread;
   }
   return true;
}

bool
cobalt_chat_requests_count_label(int count, char *out, size_t cap)
{
   if (!out || cap == 0) {
      return false;
   }

   int written;
   if (count <= 0) {
      written = snprintf(out, cap, "No requests");
   } else if (count == 1) {
      written = snprintf(out, cap, "1 request");
   } else {
      written = snprintf(out, cap, "%d requests", count);
   }

   return written >= 0 && (size_t) written < cap;
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

bool
cobalt_chat_reply_valid(const char *text)
{
   if (!text || text[0] == '\0' || strlen(text) >= COBALT_CHAT_REPLY_MAX) {
      return false;
   }
   for (const char *p = text; *p; p++) {
      if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
         return true;
      }
   }
   return false;
}

static bool
chat_is_alnum(char c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* did:<method>:<id>, with a lowercase method and an id of DID characters. */
static bool
chat_did_valid(const char *s)
{
   if (strncmp(s, "did:", 4) != 0) {
      return false;
   }

   const char *p = s + 4;
   const char *method = p;
   while (*p >= 'a' && *p <= 'z') {
      p++;
   }
   if (p == method || *p != ':') {
      return false;
   }

   const char *id = ++p;
   for (; *p; p++) {
      const char c = *p;
      const bool ok = chat_is_alnum(c) || c == '.' || c == '_' || c == ':' ||
                      c == '%' || c == '-';
      if (!ok) {
         return false;
      }
   }
   return p > id && p[-1] != ':';
}

/* atproto handle syntax: labels of letters, digits and hyphens, each 1 to 63
 * bytes and not starting or ending with a hyphen, at most 253 bytes in all,
 * at least two labels, and a last label that starts with a letter. */
static bool
chat_handle_valid(const char *s)
{
   const size_t total = strlen(s);
   if (total == 0 || total > 253) {
      return false;
   }

   int labels = 0;
   const char *label = s;
   for (const char *p = s;; p++) {
      if (*p == '.' || *p == '\0') {
         const size_t n = (size_t) (p - label);
         if (n == 0 || n > 63 || label[0] == '-' || p[-1] == '-') {
            return false;
         }
         for (const char *q = label; q < p; q++) {
            if (!chat_is_alnum(*q) && *q != '-') {
               return false;
            }
         }
         labels++;
         if (*p == '\0') {
            break;
         }
         label = p + 1;
      }
   }

   if (labels < 2) {
      return false;
   }
   const char *tld = strrchr(s, '.') + 1;
   return (*tld >= 'a' && *tld <= 'z') || (*tld >= 'A' && *tld <= 'Z');
}

bool
cobalt_chat_target_normalise(const char *input, char *out, size_t cap)
{
   if (!input || !out || cap == 0) {
      return false;
   }
   out[0] = '\0';

   const char *start = input;
   const char *end = input + strlen(input);
   while (start < end && (*start == ' ' || *start == '\t')) {
      start++;
   }
   while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\n' || end[-1] == '\r')) {
      end--;
   }
   if (start < end && *start == '@') {
      start++;
   }

   const size_t len = (size_t) (end - start);
   if (len == 0 || len >= cap) {
      return false;
   }
   memcpy(out, start, len);
   out[len] = '\0';
   return true;
}

cobalt_chat_target_kind
cobalt_chat_target_kind_of(const char *target)
{
   if (!target || target[0] == '\0' || strlen(target) >= COBALT_CHAT_TARGET_MAX) {
      return COBALT_CHAT_TARGET_INVALID;
   }
   if (chat_did_valid(target)) {
      return COBALT_CHAT_TARGET_DID;
   }
   if (chat_handle_valid(target)) {
      return COBALT_CHAT_TARGET_HANDLE;
   }
   return COBALT_CHAT_TARGET_INVALID;
}

bool
cobalt_chat_is_self(const char *viewer_did, const char *target_did)
{
   return viewer_did && target_did && viewer_did[0] != '\0' &&
          strcmp(viewer_did, target_did) == 0;
}

bool
cobalt_chat_convo_id_copy(char *out, size_t cap, const char *id)
{
   if (!out || cap == 0) {
      return false;
   }
   out[0] = '\0';

   if (!id || id[0] == '\0') {
      return false;
   }
   const size_t len = strlen(id);
   if (len >= cap || len >= COBALT_CHAT_ID_MAX) {
      return false;
   }
   for (const char *p = id; *p; p++) {
      /* Anything at or below a space, and anything past '~', is refused. A
       * negative char (a high byte on a signed char) falls under the first. */
      if (*p <= ' ' || *p > '~') {
         return false;
      }
   }
   memcpy(out, id, len + 1);
   return true;
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
