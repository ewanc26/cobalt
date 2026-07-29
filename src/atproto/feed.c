#include "atproto/feed.h"
#include "util/log.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/feed_typed.h>
#endif

#include <stdio.h>
#include <string.h>

#define ELLIPSIS "..."

/* U+00B7 MIDDLE DOT. In Latin-1 and therefore in essentially every font,
 * including the placeholder — worth caring about, since a missing glyph in the
 * counts line would show as tofu on every post at once. */
#define SEPARATOR " \xC2\xB7 "

void
cobalt_feed_reset(cobalt_feed *feed)
{
   if (feed) {
      feed->count = 0;
   }
}

/* True if `b` is a UTF-8 continuation byte (10xxxxxx). */
static bool
is_continuation(unsigned char b)
{
   return (b & 0xC0) == 0x80;
}

void
cobalt_feed_copy_text(char *out, size_t out_size, const char *text)
{
   if (!out || out_size == 0) {
      return;
   }
   out[0] = '\0';
   if (!text) {
      return;
   }

   const size_t len = strlen(text);
   if (len < out_size) {
      memcpy(out, text, len + 1);
      return;
   }

   /* Truncating. Leave room for the ellipsis and the terminator, then walk
    * back to a codepoint boundary — post text is arbitrary UTF-8 and a cut
    * through a multi-byte sequence renders as tofu. */
   const size_t ellipsis_len = strlen(ELLIPSIS);
   if (out_size <= ellipsis_len + 1) {
      return; /* No room to say anything meaningful. */
   }

   size_t cut = out_size - ellipsis_len - 1;
   while (cut > 0 && is_continuation((unsigned char) text[cut])) {
      cut--;
   }

   memcpy(out, text, cut);
   memcpy(out + cut, ELLIPSIS, ellipsis_len + 1);
}

/* Append "<n> <singular|plural>" to a counts line, with the separator if the
 * line already has something on it. Silently does nothing if it will not fit,
 * so a long line loses its tail rather than being cut mid-word. */
static void
append_count(char *out, size_t out_size, int value, const char *singular,
             const char *plural)
{
   if (value <= 0) {
      return;
   }

   char piece[48];
   snprintf(piece, sizeof(piece), "%s%d %s", out[0] ? SEPARATOR : "", value,
            value == 1 ? singular : plural);

   const size_t used = strlen(out);
   if (used + strlen(piece) + 1 > out_size) {
      return;
   }
   memcpy(out + used, piece, strlen(piece) + 1);
}

void
cobalt_feed_format_counts(char *out, size_t out_size, int replies, int reposts,
                          int likes)
{
   if (!out || out_size == 0) {
      return;
   }
   out[0] = '\0';

   append_count(out, out_size, replies, "reply", "replies");
   append_count(out, out_size, reposts, "repost", "reposts");
   append_count(out, out_size, likes, "like", "likes");
}

const char *
cobalt_feed_embed_note(const char *type)
{
   if (!type) {
      return "";
   }

   /*
    * Matched on prefix, because the wire carries the view variants
    * ("app.bsky.embed.images#view") and, for record embeds, several more
    * ("#viewRecord", "#viewNotFound", …). Order matters: recordWithMedia is a
    * longer prefix than record and has to be tested first.
    */
   static const struct {
      const char *prefix;
      const char *note;
   } NOTES[] = {
      { "app.bsky.embed.recordWithMedia", "[quote + media]" },
      { "app.bsky.embed.images",          "[image]" },
      { "app.bsky.embed.video",           "[video]" },
      { "app.bsky.embed.external",        "[link]" },
      { "app.bsky.embed.record",          "[quote]" },
   };

   for (size_t i = 0; i < sizeof(NOTES) / sizeof(NOTES[0]); i++) {
      const size_t n = strlen(NOTES[i].prefix);
      if (strncmp(type, NOTES[i].prefix, n) == 0) {
         return NOTES[i].note;
      }
   }

   /* Deliberately not a guess. An unknown embed draws nothing rather than
    * claiming to be something it is not. */
   return "";
}

#ifdef COBALT_HAS_WOLFRAM

/* Read a string member, returning NULL rather than an empty string when it is
 * absent, so callers can tell "not sent" from "sent empty". */
static const char *
json_string(const cJSON *object, const char *key)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
   return (item && cJSON_IsString(item) && item->valuestring) ? item->valuestring
                                                              : NULL;
}

/* Who reposted this, if the item is in the feed for that reason. */
static void
fill_reason(cobalt_post *post, const cJSON *reason)
{
   post->reposted_by[0] = '\0';
   if (!reason) {
      return;
   }

   const char *type = json_string(reason, "$type");
   if (!type || strncmp(type, "app.bsky.feed.defs#reasonRepost", 31) != 0) {
      /* reasonPin and anything added later are not attributions, so they get
       * no banner rather than a misleading one. */
      return;
   }

   const cJSON *by = cJSON_GetObjectItemCaseSensitive(reason, "by");
   if (!by) {
      return;
   }

   const char *name = json_string(by, "displayName");
   if (!name || name[0] == '\0') {
      name = json_string(by, "handle");
   }
   if (name) {
      cobalt_feed_copy_text(post->reposted_by, sizeof(post->reposted_by), name);
   }
}

static void
fill_from_view(cobalt_post *post, const wf_agent_post_view *view, int64_t now)
{
   memset(post, 0, sizeof(*post));

   snprintf(post->uri, sizeof(post->uri), "%s", view->uri ? view->uri : "");
   snprintf(post->cid, sizeof(post->cid), "%s", view->cid ? view->cid : "");

   /* A display name is optional and frequently absent; the handle always
    * exists, so it is the fallback rather than showing an empty line. */
   const char *display = view->author.display_name;
   const char *handle = view->author.handle ? view->author.handle : "";
   if (!display || display[0] == '\0') {
      display = handle;
   }
   cobalt_feed_copy_text(post->author, sizeof(post->author), display);
   snprintf(post->handle, sizeof(post->handle), "@%s", handle);

   /* The post text lives in the record, which Wolfram keeps as raw JSON
    * because a record can be any shape. A post with no text is legitimate —
    * an image-only post — so a missing field is not an error. */
   const char *text = json_string(view->record, "text");
   cobalt_feed_copy_text(post->text, sizeof(post->text), text ? text : "");

   /*
    * Prefer the record's own createdAt over indexedAt: it is when the author
    * says they posted, which is what every other client shows. Fall back to
    * indexedAt when the record does not carry one.
    */
   const char *created = json_string(view->record, "createdAt");
   if (!created) {
      created = view->indexed_at;
   }

   int64_t epoch = 0;
   if (created && cobalt_time_parse_rfc3339(created, &epoch) && now > 0) {
      cobalt_time_relative(epoch, now, post->age, sizeof(post->age));
   } else {
      /* An unparseable timestamp, or a console with no usable clock. Blank is
       * honest; a wrong age is not. */
      post->age[0] = '\0';
   }

   cobalt_feed_format_counts(post->meta, sizeof(post->meta),
                             view->has_reply_count ? view->reply_count : 0,
                             view->has_repost_count ? view->repost_count : 0,
                             view->has_like_count ? view->like_count : 0);

   if (view->embed) {
      snprintf(post->embed_note, sizeof(post->embed_note), "%s",
               cobalt_feed_embed_note(json_string(view->embed, "$type")));
   }
}

int
cobalt_feed_append_from_wolfram(cobalt_feed *feed,
                                const struct wf_agent_feed_list *list,
                                int64_t now)
{
   if (!feed || !list) {
      return 0;
   }

   const wf_agent_feed_list *typed = (const wf_agent_feed_list *) list;
   int added = 0;

   for (size_t i = 0; i < typed->item_count; i++) {
      if (feed->count >= COBALT_FEED_MAX_POSTS) {
         COBALT_LOGI("feed: window full at %d posts, dropping the rest of the page",
                     feed->count);
         break;
      }

      const wf_agent_feed_item *item = &typed->items[i];
      cobalt_post *post = &feed->posts[feed->count];

      fill_from_view(post, &item->post, now);
      fill_reason(post, item->reason);

      feed->count++;
      added++;
   }

   /* An absent cursor is the server saying there is nothing after this page,
    * which is what stops the UI offering "load more" forever. */
   if (typed->cursor && typed->cursor[0]) {
      snprintf(feed->cursor, sizeof(feed->cursor), "%s", typed->cursor);
      feed->has_more = true;
   } else {
      feed->cursor[0] = '\0';
      feed->has_more = false;
   }

   return added;
}

#endif /* COBALT_HAS_WOLFRAM */
