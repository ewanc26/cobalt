#include "atproto/notifications.h"
#include "util/log.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/agent.h>
#endif

#include <stdio.h>
#include <string.h>

void
cobalt_notifications_reset(cobalt_notifications *list)
{
   if (list) {
      list->count = 0;
      list->unread = 0;
   }
}

const char *
cobalt_notification_summary(const char *reason)
{
   if (!reason) {
      return "";
   }

   static const struct {
      const char *reason;
      const char *summary;
   } WORDING[] = {
      { "like",               "liked your post" },
      { "repost",             "reposted your post" },
      { "follow",             "followed you" },
      { "mention",            "mentioned you" },
      { "reply",              "replied to you" },
      { "quote",              "quoted your post" },
      { "starterpack-joined", "joined using your starter pack" },
      { "verified",           "verified your account" },
      { "unverified",         "removed your verification" },
      { "like-via-repost",    "liked a repost of your post" },
      { "repost-via-repost",  "reposted a repost of your post" },
      { "subscribed-post",    "posted, and you are subscribed" },
   };

   for (size_t i = 0; i < sizeof(WORDING) / sizeof(WORDING[0]); i++) {
      if (strcmp(reason, WORDING[i].reason) == 0) {
         return WORDING[i].summary;
      }
   }

   /*
    * An unrecognised reason is shown as-is. Bluesky adds these over time, and
    * a row reading "did something" would be less useful than the raw word —
    * which at least says what happened, even if it reads like a lexicon.
    */
   return reason;
}

bool
cobalt_notification_subject_is_self(const char *reason)
{
   if (!reason) {
      return false;
   }
   /*
    * A reply, mention or quote *is* a post, and it is the one to open. A like
    * or repost points at something the viewer wrote, which lives in
    * reasonSubject. Getting this backwards opens a plausible-looking wrong
    * post, which is worse than opening nothing.
    */
   return strcmp(reason, "reply") == 0 || strcmp(reason, "mention") == 0 ||
          strcmp(reason, "quote") == 0 || strcmp(reason, "subscribed-post") == 0;
}

#ifdef COBALT_HAS_WOLFRAM

static const char *
json_string(const cJSON *object, const char *key)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
   return (item && cJSON_IsString(item) && item->valuestring) ? item->valuestring
                                                              : NULL;
}

int
cobalt_notifications_append_from_wolfram(
   cobalt_notifications *out, const struct wf_agent_notification_list *list,
   int64_t now)
{
   if (!out || !list) {
      return 0;
   }

   const wf_agent_notification_list *typed =
      (const wf_agent_notification_list *) list;
   int added = 0;

   for (size_t i = 0; i < typed->notification_count; i++) {
      if (out->count >= COBALT_NOTIFICATIONS_MAX) {
         COBALT_LOGI("notifications: window full at %d", out->count);
         break;
      }

      const wf_agent_notification *src = &typed->notifications[i];
      cobalt_notification *item = &out->items[out->count];
      memset(item, 0, sizeof(*item));

      const char *display = src->author.display_name;
      const char *handle = src->author.handle ? src->author.handle : "";
      if (!display || display[0] == '\0') {
         display = handle;
      }
      cobalt_feed_copy_text(item->actor, sizeof(item->actor), display);
      snprintf(item->handle, sizeof(item->handle), "@%s", handle);

      snprintf(item->summary, sizeof(item->summary), "%s",
               cobalt_notification_summary(src->reason));

      /* A like has no words; a reply does. Both are normal. */
      const char *text = json_string(src->record, "text");
      if (text) {
         cobalt_feed_copy_text(item->text, sizeof(item->text), text);
      }

      const char *created = json_string(src->record, "createdAt");
      if (!created) {
         created = src->indexed_at;
      }
      int64_t epoch = 0;
      if (created && cobalt_time_parse_rfc3339(created, &epoch) && now > 0) {
         cobalt_time_relative(epoch, now, item->age, sizeof(item->age));
      }

      const char *subject = cobalt_notification_subject_is_self(src->reason)
                               ? src->uri
                               : src->reason_subject;
      if (subject) {
         snprintf(item->subject_uri, sizeof(item->subject_uri), "%s", subject);
      }

      item->unread = !src->is_read;
      if (item->unread) {
         out->unread++;
      }

      out->count++;
      added++;
   }

   if (typed->cursor && typed->cursor[0]) {
      snprintf(out->cursor, sizeof(out->cursor), "%s", typed->cursor);
      out->has_more = true;
   } else {
      out->cursor[0] = '\0';
      out->has_more = false;
   }

   return added;
}

#endif /* COBALT_HAS_WOLFRAM */
