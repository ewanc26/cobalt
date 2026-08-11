#include "atproto/feed.h"
#include "util/log.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/feed_typed.h>
#include <wolfram/thread_typed.h>
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

void
cobalt_thread_reset(cobalt_thread *thread)
{
   if (thread) {
      thread->count = 0;
      thread->focus = 0;
      thread->truncated = false;
   }
}

void
cobalt_list_clamp(int *selected, int *scroll, int count)
{
   if (!selected || !scroll) {
      return;
   }

   if (count <= 0) {
      *selected = -1;
      *scroll = 0;
      return;
   }

   if (*selected >= count) {
      *selected = count - 1;
   }
   if (*selected < 0) {
      *selected = 0;
   }

   /* Scrolled past the end: go back to the top rather than to the last row.
    * Landing mid-list after a refresh would be more disorienting than
    * starting again from the beginning of new content. */
   if (*scroll >= count || *scroll < 0) {
      *scroll = 0;
   }
   if (*scroll > *selected) {
      *scroll = *selected;
   }
}

bool
cobalt_feed_can_page(const cobalt_feed *feed)
{
   return feed && feed->has_more && feed->count < COBALT_FEED_MAX_POSTS;
}

/* Rebuild the drawn counts line after a local change. */
static void
refresh_meta(cobalt_post *post)
{
   cobalt_feed_format_counts(post->meta, sizeof(post->meta), post->reply_count,
                             post->repost_count, post->like_count);
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

/* The post-level half of an interaction, shared by the feed and the thread —
 * the same post is frequently on screen in both at once. */
static void
apply_to_post(cobalt_post *post, const char *record_uri, bool is_like)
{
   char *slot = is_like ? post->viewer_like : post->viewer_repost;
   const size_t slot_size = is_like ? sizeof(post->viewer_like)
                                    : sizeof(post->viewer_repost);
   int *count = is_like ? &post->like_count : &post->repost_count;

   const bool before = slot[0] != '\0';
   const bool now = record_uri && record_uri[0];

   snprintf(slot, slot_size, "%s", now ? record_uri : "");

   /* Only move the count when the state actually changed, so a duplicate
    * confirmation cannot drive it away from the server's value. */
   if (now && !before) {
      (*count)++;
   } else if (!now && before && *count > 0) {
      (*count)--;
   }

   refresh_meta(post);
}

static cobalt_post *
find_post(cobalt_post *posts, int count, const char *post_uri)
{
   if (!posts || !post_uri) {
      return NULL;
   }
   for (int i = 0; i < count; i++) {
      if (strcmp(posts[i].uri, post_uri) == 0) {
         return &posts[i];
      }
   }
   /* A refresh can replace the feed while a request is in flight, so the post
    * legitimately may not be here any more. */
   return NULL;
}

bool
cobalt_feed_apply_like(cobalt_feed *feed, const char *post_uri,
                       const char *record_uri)
{
   cobalt_post *post = feed ? find_post(feed->posts, feed->count, post_uri) : NULL;
   if (!post) {
      return false;
   }
   apply_to_post(post, record_uri, true);
   return true;
}

bool
cobalt_feed_apply_repost(cobalt_feed *feed, const char *post_uri,
                         const char *record_uri)
{
   cobalt_post *post = feed ? find_post(feed->posts, feed->count, post_uri) : NULL;
   if (!post) {
      return false;
   }
   apply_to_post(post, record_uri, false);
   return true;
}

bool
cobalt_thread_apply_like(cobalt_thread *thread, const char *post_uri,
                         const char *record_uri)
{
   cobalt_post *post = thread ? find_post(thread->posts, thread->count, post_uri)
                              : NULL;
   if (!post) {
      return false;
   }
   apply_to_post(post, record_uri, true);
   return true;
}

bool
cobalt_thread_apply_repost(cobalt_thread *thread, const char *post_uri,
                           const char *record_uri)
{
   cobalt_post *post = thread ? find_post(thread->posts, thread->count, post_uri)
                              : NULL;
   if (!post) {
      return false;
   }
   apply_to_post(post, record_uri, false);
   return true;
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

void
cobalt_feed_link_domain(const char *uri, char *out, size_t out_size)
{
   if (!out || out_size == 0) {
      return;
   }
   out[0] = '\0';
   if (!uri || !uri[0]) {
      return;
   }

   const char *scheme_end = strstr(uri, "://");
   const char *host = scheme_end ? scheme_end + 3 : uri;

   /* Strip "user@" if present before the host, so a URI that carries one
    * (legal, if unusual for a link-card target) doesn't leak into the
    * display string. */
   const char *at = strchr(host, '@');
   const char *slash = strchr(host, '/');
   if (at && (!slash || at < slash)) {
      host = at + 1;
   }

   if (strncmp(host, "www.", 4) == 0) {
      host += 4;
   }

   size_t len = 0;
   while (host[len] && host[len] != '/' && host[len] != ':' &&
          host[len] != '?' && host[len] != '#') {
      len++;
   }
   if (len == 0) {
      return;
   }
   if (len >= out_size) {
      len = out_size - 1;
   }
   memcpy(out, host, len);
   out[len] = '\0';
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

static int
json_int(const cJSON *object, const char *key)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
   return (item && cJSON_IsNumber(item)) ? item->valueint : 0;
}

/* Fill post->images[]/image_count from an app.bsky.embed.images#view. */
static void
fill_embed_images(cobalt_post *post, const cJSON *images_view)
{
   const cJSON *images = cJSON_GetObjectItemCaseSensitive(images_view, "images");
   if (!cJSON_IsArray(images)) {
      return;
   }

   const cJSON *item;
   cJSON_ArrayForEach(item, images) {
      if (post->image_count >= COBALT_POST_IMAGES_MAX) {
         break;
      }
      const char *thumb = json_string(item, "thumb");
      if (!thumb || !thumb[0]) {
         continue;
      }

      cobalt_post_image *img = &post->images[post->image_count];
      snprintf(img->thumb, sizeof(img->thumb), "%s", thumb);

      const cJSON *ratio = cJSON_GetObjectItemCaseSensitive(item, "aspectRatio");
      img->aspect_w = json_int(ratio, "width");
      img->aspect_h = json_int(ratio, "height");

      post->image_count++;
   }
}

/* Fill post->link from an app.bsky.embed.external#view. */
static void
fill_embed_external(cobalt_post *post, const cJSON *external_view)
{
   const cJSON *external =
      cJSON_GetObjectItemCaseSensitive(external_view, "external");
   const char *uri = json_string(external, "uri");
   if (!uri || !uri[0]) {
      return;
   }

   snprintf(post->link.uri, sizeof(post->link.uri), "%s", uri);

   const char *title = json_string(external, "title");
   cobalt_feed_copy_text(post->link.title, sizeof(post->link.title),
                         title ? title : "");
   const char *desc = json_string(external, "description");
   cobalt_feed_copy_text(post->link.description, sizeof(post->link.description),
                         desc ? desc : "");
   const char *thumb = json_string(external, "thumb");
   snprintf(post->link.thumb, sizeof(post->link.thumb), "%s", thumb ? thumb : "");
}

/*
 * Populate the drawable media (images, link card) from a post's embed.
 *
 * `recordWithMedia` carries its media under a nested "media" object rather
 * than at the embed's own top level, so the $type driving the switch below is
 * read from there instead when that's the shape in hand. A pure quote or a
 * video embed leaves both post->image_count and post->link.uri empty — the
 * bracket note cobalt_feed_embed_note produced is the only thing shown for
 * those, same as before this function existed.
 */
static void
fill_embed_media(cobalt_post *post, const cJSON *embed)
{
   if (!embed) {
      return;
   }

   const char *type = json_string(embed, "$type");
   const cJSON *media = embed;

   if (type && strncmp(type, "app.bsky.embed.recordWithMedia",
                       strlen("app.bsky.embed.recordWithMedia")) == 0) {
      media = cJSON_GetObjectItemCaseSensitive(embed, "media");
      type = json_string(media, "$type");
   }
   if (!type) {
      return;
   }

   if (strncmp(type, "app.bsky.embed.images", strlen("app.bsky.embed.images")) ==
       0) {
      fill_embed_images(post, media);
   } else if (strncmp(type, "app.bsky.embed.external",
                      strlen("app.bsky.embed.external")) == 0) {
      fill_embed_external(post, media);
   }

   /* Real media is now drawn, so the bracket note that used to stand in for
    * it would only be clutter alongside it. Left alone for anything still
    * undrawable — video, and the quote half of recordWithMedia. */
   if (post->image_count > 0 || post->link.uri[0]) {
      post->embed_note[0] = '\0';
   }
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
   snprintf(post->avatar, sizeof(post->avatar), "%s",
            view->author.avatar ? view->author.avatar : "");

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

   post->reply_count = view->has_reply_count ? view->reply_count : 0;
   post->repost_count = view->has_repost_count ? view->repost_count : 0;
   post->like_count = view->has_like_count ? view->like_count : 0;
   refresh_meta(post);

   /* The viewer's own like/repost records. Without these an interaction is
    * one-way: the UI cannot show what has already been done, and there is no
    * record URI to delete to undo it. */
   snprintf(post->viewer_like, sizeof(post->viewer_like), "%s",
            view->viewer.like ? view->viewer.like : "");
   snprintf(post->viewer_repost, sizeof(post->viewer_repost), "%s",
            view->viewer.repost ? view->viewer.repost : "");

   if (view->embed) {
      snprintf(post->embed_note, sizeof(post->embed_note), "%s",
               cobalt_feed_embed_note(json_string(view->embed, "$type")));
      fill_embed_media(post, view->embed);
   }

   /* Assume the post is its own root; a reply overwrites this below.
    * memcpy rather than snprintf: source and destination are members of the
    * same struct, which the compiler cannot prove do not overlap, and the
    * arrays are the same size by construction. */
   memcpy(post->root_uri, post->uri, sizeof(post->root_uri));
   memcpy(post->root_cid, post->cid, sizeof(post->root_cid));
}

/*
 * Pull the thread root out of a replyRef ({ root: {uri,cid}, parent: {...} }).
 * Leaves the post as its own root when the ref is absent or malformed, which
 * is the correct reading for a top-level post and a safe fallback otherwise —
 * a reply naming itself as root is wrong, but a reply naming a *guessed* root
 * would be wrong and hard to notice.
 */
static void
fill_root(cobalt_post *post, const cJSON *reply_ref)
{
   if (!reply_ref) {
      return;
   }
   const cJSON *root = cJSON_GetObjectItemCaseSensitive(reply_ref, "root");
   if (!root) {
      return;
   }
   const char *uri = json_string(root, "uri");
   const char *cid = json_string(root, "cid");
   if (uri && cid) {
      snprintf(post->root_uri, sizeof(post->root_uri), "%s", uri);
      snprintf(post->root_cid, sizeof(post->root_cid), "%s", cid);
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
      /* The feed sends the reply ref alongside the post rather than inside the
       * record, so it is read from the item. */
      fill_root(post, item->reply);

      feed->count++;
      added++;
   }

   /*
    * An absent cursor is the server saying there is nothing after this page.
    *
    * A full window has to clear it too, and that is not obvious: the window is
    * fixed at COBALT_FEED_MAX_POSTS, so once it fills, every further page
    * appends nothing while still handing back a cursor. Leaving has_more set
    * would let a screen that auto-pages at the end of the list request the
    * next page forever — spinning the worker, keeping `busy` true so no
    * interaction ever runs, and earning a rate limit.
    */
   if (feed->count >= COBALT_FEED_MAX_POSTS) {
      COBALT_LOGI("feed: window full at %d posts, no further paging",
                  feed->count);
      feed->cursor[0] = '\0';
      feed->has_more = false;
   } else if (typed->cursor && typed->cursor[0]) {
      snprintf(feed->cursor, sizeof(feed->cursor), "%s", typed->cursor);
      feed->has_more = true;
   } else {
      feed->cursor[0] = '\0';
      feed->has_more = false;
   }

   return added;
}


/* --- threads --- */

/*
 * A thread node carries the same fields as a feed post view under different
 * names, so this mirrors fill_from_view rather than sharing with it — the two
 * Wolfram structs are not related by inheritance and a shared helper would
 * need a parameter for every field anyway.
 */
static void
fill_from_thread_post(cobalt_post *post, const wf_agent_thread_post *view,
                      int64_t now)
{
   memset(post, 0, sizeof(*post));

   snprintf(post->uri, sizeof(post->uri), "%s", view->uri ? view->uri : "");
   snprintf(post->cid, sizeof(post->cid), "%s", view->cid ? view->cid : "");

   const char *display = view->author.display_name;
   const char *handle = view->author.handle ? view->author.handle : "";
   if (!display || display[0] == '\0') {
      display = handle;
   }
   cobalt_feed_copy_text(post->author, sizeof(post->author), display);
   snprintf(post->handle, sizeof(post->handle), "@%s", handle);
   snprintf(post->avatar, sizeof(post->avatar), "%s",
            view->author.avatar ? view->author.avatar : "");

   const char *text = json_string(view->record, "text");
   cobalt_feed_copy_text(post->text, sizeof(post->text), text ? text : "");

   const char *created = json_string(view->record, "createdAt");
   if (!created) {
      created = view->indexed_at;
   }
   int64_t epoch = 0;
   if (created && cobalt_time_parse_rfc3339(created, &epoch) && now > 0) {
      cobalt_time_relative(epoch, now, post->age, sizeof(post->age));
   }

   /* A thread post view always sends its counts, unlike a feed view where they
    * are optional, so there are no has_* flags to consult here. */
   post->reply_count = view->reply_count;
   post->repost_count = view->repost_count;
   post->like_count = view->like_count;
   refresh_meta(post);

   snprintf(post->viewer_like, sizeof(post->viewer_like), "%s",
            view->viewer_like ? view->viewer_like : "");
   snprintf(post->viewer_repost, sizeof(post->viewer_repost), "%s",
            view->viewer_repost ? view->viewer_repost : "");

   if (view->embed) {
      snprintf(post->embed_note, sizeof(post->embed_note), "%s",
               cobalt_feed_embed_note(json_string(view->embed, "$type")));
      fill_embed_media(post, view->embed);
   }

   memcpy(post->root_uri, post->uri, sizeof(post->root_uri));
   memcpy(post->root_cid, post->cid, sizeof(post->root_cid));
   /* A thread node carries its reply ref inside the record, unlike a feed
    * item, which carries it alongside. */
   fill_root(post, cJSON_GetObjectItemCaseSensitive(view->record, "reply"));
}

/* Append one node. Returns false once the buffer is full. */
static bool
push_node(cobalt_thread *out, const wf_agent_thread_node *node, int depth,
          int64_t now)
{
   if (out->count >= COBALT_THREAD_MAX_POSTS) {
      out->truncated = true;
      return false;
   }

   cobalt_post *post = &out->posts[out->count];

   if (node->kind == WF_AGENT_THREAD_KIND_POST) {
      fill_from_thread_post(post, &node->post, now);
   } else {
      /*
       * A blocked or deleted post still occupies a place in the conversation.
       * Showing a placeholder keeps the reply structure honest — dropping it
       * would silently reattach its replies to the wrong parent.
       */
      memset(post, 0, sizeof(*post));
      snprintf(post->uri, sizeof(post->uri), "%s", node->uri ? node->uri : "");
      snprintf(post->author, sizeof(post->author), "%s",
               node->kind == WF_AGENT_THREAD_KIND_BLOCKED ? "Blocked post"
                                                          : "Deleted post");
      snprintf(post->text, sizeof(post->text), "%s",
               node->kind == WF_AGENT_THREAD_KIND_BLOCKED
                  ? "You cannot see this post."
                  : "This post is no longer available.");
   }

   out->depth[out->count] = (unsigned char) (depth > COBALT_THREAD_MAX_DEPTH
                                                ? COBALT_THREAD_MAX_DEPTH
                                                : depth);
   out->count++;
   return true;
}

/* Replies, depth-first, so a reply sits directly under what it answers. */
static bool
push_replies(cobalt_thread *out, const wf_agent_thread_node *node, int depth,
             int64_t now)
{
   for (size_t i = 0; i < node->replies_count; i++) {
      const wf_agent_thread_node *reply = &node->replies[i];
      if (!push_node(out, reply, depth, now)) {
         return false;
      }
      if (!push_replies(out, reply, depth + 1, now)) {
         return false;
      }
   }
   return true;
}

void
cobalt_thread_from_wolfram(cobalt_thread *out, const struct wf_agent_thread *src,
                           int64_t now)
{
   if (!out) {
      return;
   }
   cobalt_thread_reset(out);
   if (!src) {
      return;
   }

   const wf_agent_thread *typed = (const wf_agent_thread *) src;
   const wf_agent_thread_node *root = &typed->root;

   /*
    * Ancestors are a linked list running the wrong way — each node points at
    * its parent — so they are collected upward and emitted in reverse. The cap
    * is the buffer's, since a long chain would otherwise crowd out the replies
    * the user actually opened the thread to read.
    */
   const wf_agent_thread_node *ancestors[COBALT_THREAD_MAX_POSTS];
   int ancestor_count = 0;
   const wf_agent_thread_node *p = root->parent;
   for (; p && ancestor_count < COBALT_THREAD_MAX_POSTS / 2; p = p->parent) {
      ancestors[ancestor_count++] = p;
   }
   /* Stopping early here drops the top of the conversation, which looks
    * identical to a thread that simply starts at an odd reply unless it is
    * flagged — push_node only notices when the buffer itself fills. */
   if (p) {
      out->truncated = true;
   }

   for (int i = ancestor_count - 1; i >= 0; i--) {
      if (!push_node(out, ancestors[i], 0, now)) {
         return;
      }
   }

   out->focus = out->count;
   if (!push_node(out, root, 0, now)) {
      return;
   }

   push_replies(out, root, 1, now);
}

#endif /* COBALT_HAS_WOLFRAM */
