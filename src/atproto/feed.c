#include "atproto/feed.h"
#include "util/log.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/feed_typed.h>
#include <wolfram/post_view_typed.h>
#include <wolfram/time.h>
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

bool
cobalt_post_uri_is_by(const char *uri, const char *did)
{
   if (!uri || !did || !did[0] || strncmp(uri, "at://", 5) != 0) {
      return false;
   }
   const size_t n = strlen(did);
   return strncmp(uri + 5, did, n) == 0 && uri[5 + n] == '/';
}

int
cobalt_feed_remove_post(cobalt_feed *feed, const char *uri)
{
   if (!feed || !uri || !uri[0]) {
      return 0;
   }

   int removed = 0;
   int out = 0;
   for (int i = 0; i < feed->count; i++) {
      if (strcmp(feed->posts[i].uri, uri) == 0) {
         removed++;
         continue;
      }
      if (out != i) {
         feed->posts[out] = feed->posts[i];
      }
      out++;
   }
   feed->count = out;
   return removed;
}

bool
cobalt_feed_prepend_pinned(cobalt_feed *feed, const cobalt_post *post)
{
   if (!feed || !post || !post->uri[0]) {
      return false;
   }

   cobalt_post pinned = *post;
   pinned.pinned = true;
   pinned.reposted_by[0] = '\0';

   cobalt_feed_remove_post(feed, pinned.uri);
   if (feed->count >= COBALT_FEED_MAX_POSTS) {
      feed->count = COBALT_FEED_MAX_POSTS - 1;
   }
   memmove(&feed->posts[1], &feed->posts[0],
           (size_t) feed->count * sizeof(feed->posts[0]));
   feed->posts[0] = pinned;
   feed->count++;
   return true;
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

bool
cobalt_post_has_card(const cobalt_post *post)
{
   return post->link.uri[0] || post->link.video;
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


void
cobalt_feed_set_quote(cobalt_post *post, const char *display_name,
                      const char *handle, const char *text)
{
   cobalt_post_quote *q = &post->quote;
   memset(q, 0, sizeof(*q));
   if (!handle || handle[0] == '\0') {
      return;
   }
   if (!display_name || display_name[0] == '\0') {
      display_name = handle;
   }
   cobalt_feed_copy_text(q->author, sizeof(q->author), display_name);
   snprintf(q->handle, sizeof(q->handle), "@%s", handle);
   cobalt_feed_copy_text(q->text, sizeof(q->text), text ? text : "");
   q->present = 1;
   if (strcmp(post->embed_note, "[quote]") == 0) {
      post->embed_note[0] = '\0';
   }
}

#ifdef COBALT_HAS_WOLFRAM

/* Fill post->facets from a typed record. Facets that fall outside the
 * (possibly truncated) text are dropped. */
static void
apply_facets(cobalt_post *post, const wf_post_record *rec)
{
   post->facet_count = 0;
   const int text_len = (int) strlen(post->text);
   for (size_t i = 0; i < rec->facet_count; i++) {
      if (post->facet_count >= COBALT_POST_FACETS_MAX) {
         break;
      }
      const wf_post_facet *src = &rec->facets[i];
      int start = src->byte_start;
      int end = src->byte_end;
      if (end > text_len) {
         end = text_len;
      }
      if (start < 0 || start >= end) {
         continue;
      }
      cobalt_post_facet *f = &post->facets[post->facet_count++];
      f->kind = src->kind == WF_POST_FACET_LINK      ? COBALT_FACET_LINK
                : src->kind == WF_POST_FACET_MENTION ? COBALT_FACET_MENTION
                                                     : COBALT_FACET_TAG;
      f->start = start;
      f->end = end;
      cobalt_feed_copy_text(f->target, sizeof(f->target),
                            src->target ? src->target : "");
   }
}

/*
 * Populate the drawable media (images, link card) and the quote from a typed
 * embed. The bracket note is derived from the embed's own $type.
 *
 * A pure quote or a video embed leaves both post->image_count and
 * post->link.uri empty, so the bracket note is the only thing shown for those.
 */
static void
apply_embed(cobalt_post *post, const wf_post_embed *embed)
{
   snprintf(post->embed_note, sizeof(post->embed_note), "%s",
            cobalt_feed_embed_note(embed->type));

   for (size_t i = 0; i < embed->image_count; i++) {
      if (post->image_count >= COBALT_POST_IMAGES_MAX) {
         break;
      }
      const wf_post_embed_image *src = &embed->images[i];
      cobalt_post_image *img = &post->images[post->image_count];
      snprintf(img->thumb, sizeof(img->thumb), "%s", src->thumb);
      cobalt_feed_copy_text(img->alt, sizeof(img->alt), src->alt ? src->alt : "");
      img->aspect_w = src->width;
      img->aspect_h = src->height;
      post->image_count++;
   }

   if (embed->has_external) {
      snprintf(post->link.uri, sizeof(post->link.uri), "%s", embed->external_uri);
      cobalt_feed_copy_text(post->link.title, sizeof(post->link.title),
                            embed->external_title ? embed->external_title : "");
      cobalt_feed_copy_text(post->link.description,
                            sizeof(post->link.description),
                            embed->external_description
                               ? embed->external_description
                               : "");
      snprintf(post->link.thumb, sizeof(post->link.thumb), "%s",
               embed->external_thumb ? embed->external_thumb : "");
   }

   /* A video cannot be played, but its poster frame can be drawn, on the link
    * card's layout. The title says so, so the card is not mistaken for a
    * link. */
   if (!post->link.uri[0] && embed->video_thumb && embed->video_thumb[0]) {
      post->link.video = true;
      snprintf(post->link.thumb, sizeof(post->link.thumb), "%s",
               embed->video_thumb);
      snprintf(post->link.title, sizeof(post->link.title),
               "Video: can't play on the Wii U");
      cobalt_feed_copy_text(post->link.description,
                            sizeof(post->link.description),
                            embed->video_alt ? embed->video_alt : "");
   }

   /* Real media is now drawn, so the bracket note that used to stand in for
    * it would only be clutter alongside it. Left alone for anything still
    * undrawable — video, and the quote half of recordWithMedia. */
   if (post->image_count > 0 || cobalt_post_has_card(post)) {
      post->embed_note[0] = '\0';
   }

   if (embed->has_quote) {
      cobalt_feed_set_quote(post, embed->quote_author_display_name,
                            embed->quote_author_handle, embed->quote_text);
   }
}

/* The embed arrives as an open cJSON subtree; Wolfram flattens it. A failed
 * read (allocation) leaves the post without media or note rather than
 * half-filled. */
static void
fill_embed(cobalt_post *post, const cJSON *embed_json)
{
   if (!embed_json) {
      return;
   }
   wf_post_embed embed;
   if (wf_post_embed_from_json(embed_json, &embed) != WF_OK) {
      return;
   }
   apply_embed(post, &embed);
   wf_post_embed_free(&embed);
}

/* Text, facets and age from a record. `indexed_at` is the fallback timestamp:
 * the record's own createdAt is when the author says they posted, which is
 * what every other client shows. Returns the record's thread root through
 * `root` for callers that want it (a thread node carries its reply ref inside
 * the record). */
static void
fill_record(cobalt_post *post, const cJSON *record_json, const char *indexed_at,
            int64_t now, wf_post_reply_root *root)
{
   wf_post_record rec;
   if (wf_post_record_from_json(record_json, &rec) != WF_OK) {
      memset(&rec, 0, sizeof(rec));
   }

   /* A post with no text is legitimate — an image-only post — so a missing
    * field is not an error. */
   cobalt_feed_copy_text(post->text, sizeof(post->text), rec.text ? rec.text : "");
   apply_facets(post, &rec);

   const char *created = rec.created_at ? rec.created_at : indexed_at;
   int64_t epoch = 0;
   if (created && wf_time_parse_rfc3339(created, &epoch) == WF_OK && now > 0) {
      wf_time_relative(epoch, now, post->age, sizeof(post->age));
   } else {
      /* An unparseable timestamp, or a console with no usable clock. Blank is
       * honest; a wrong age is not. */
      post->age[0] = '\0';
   }

   if (root) {
      *root = rec.reply_root;
      memset(&rec.reply_root, 0, sizeof(rec.reply_root));
   }
   wf_post_record_free(&rec);
}

/* Who reposted this, if the item is in the feed for that reason. */
static void
fill_reason(cobalt_post *post, const cJSON *reason_json)
{
   post->reposted_by[0] = '\0';

   wf_post_reason reason;
   if (wf_post_reason_from_json(reason_json, &reason) != WF_OK) {
      return;
   }
   /* reasonPin and anything added later are not attributions, so they get no
    * banner rather than a misleading one. */
   if (reason.is_repost) {
      const char *name = reason.by_display_name;
      if (!name || name[0] == '\0') {
         name = reason.by_handle;
      }
      if (name) {
         cobalt_feed_copy_text(post->reposted_by, sizeof(post->reposted_by), name);
      }
   }
   wf_post_reason_free(&reason);
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

   /* The record is open-shaped JSON that Wolfram keeps raw; the typed reader
    * flattens text, facets and timestamp. */
   fill_record(post, view->record, view->indexed_at, now, NULL);

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

   fill_embed(post, view->embed);

   /* Assume the post is its own root; a reply overwrites this below.
    * memcpy rather than snprintf: source and destination are members of the
    * same struct, which the compiler cannot prove do not overlap, and the
    * arrays are the same size by construction. */
   memcpy(post->root_uri, post->uri, sizeof(post->root_uri));
   memcpy(post->root_cid, post->cid, sizeof(post->root_cid));
}

/*
 * Apply a thread root read by Wolfram. Leaves the post as its own root when
 * there is none, which is the correct reading for a top-level post and a safe
 * fallback otherwise — a reply naming itself as root is wrong, but a reply
 * naming a *guessed* root would be wrong and hard to notice.
 */
static void
apply_root(cobalt_post *post, const wf_post_reply_root *root)
{
   if (root && root->uri && root->cid) {
      snprintf(post->root_uri, sizeof(post->root_uri), "%s", root->uri);
      snprintf(post->root_cid, sizeof(post->root_cid), "%s", root->cid);
   }
}

bool
cobalt_feed_pin_from_wolfram(cobalt_feed *feed,
                             const struct wf_agent_post_list *list,
                             int64_t now)
{
   if (!feed || !list || list->post_count == 0) {
      return false;
   }
   static cobalt_post post;
   fill_from_view(&post, &list->posts[0], now);
   return cobalt_feed_prepend_pinned(feed, &post);
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
      wf_post_reply_root root;
      if (wf_post_reply_root_from_json(item->reply, &root) == WF_OK) {
         apply_root(post, &root);
         wf_post_reply_root_free(&root);
      }

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


int
cobalt_feed_append_posts_from_wolfram(cobalt_feed *feed,
                                      const struct wf_agent_post_list *list,
                                      const char *next_cursor, int64_t now)
{
   if (!feed || !list) {
      return 0;
   }

   const wf_agent_post_list *typed = (const wf_agent_post_list *) list;
   int added = 0;

   for (size_t i = 0; i < typed->post_count; i++) {
      if (feed->count >= COBALT_FEED_MAX_POSTS) {
         break;
      }
      fill_from_view(&feed->posts[feed->count], &typed->posts[i], now);
      feed->count++;
      added++;
   }

   if (feed->count >= COBALT_FEED_MAX_POSTS || !next_cursor || !next_cursor[0]) {
      feed->cursor[0] = '\0';
      feed->has_more = false;
   } else {
      snprintf(feed->cursor, sizeof(feed->cursor), "%s", next_cursor);
      feed->has_more = true;
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

   wf_post_reply_root root;
   fill_record(post, view->record, view->indexed_at, now, &root);

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

   fill_embed(post, view->embed);

   memcpy(post->root_uri, post->uri, sizeof(post->root_uri));
   memcpy(post->root_cid, post->cid, sizeof(post->root_cid));
   /* A thread node carries its reply ref inside the record, unlike a feed
    * item, which carries it alongside. */
   apply_root(post, &root);
   wf_post_reply_root_free(&root);
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
