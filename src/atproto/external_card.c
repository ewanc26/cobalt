#include "atproto/external_card.h"

#include "util/threadprio.h"

#include <SDL.h>
#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The AppView query is public and optional-auth. Cobalt's PDS requires auth
 * for the proxy, and the image HTTP client deliberately carries no token, so
 * asking the public AppView is the one path that needs neither a second
 * authenticated client nor the session worker's request slot.
 */
#define CARD_API "https://public.api.bsky.app"
#define CARD_PATH "/xrpc/app.bsky.embed.getEmbedExternalView"

/* Enough for four gallery items with long CDN URLs and a publication. */
#define CARD_BODY_MAX (128 * 1024)

/* A feed window has a handful of external cards; more than this keeps the
 * plain card rather than growing an unbounded cache. */
#define CARD_SLOTS 32

typedef enum {
   CARD_FREE = 0,
   CARD_QUEUED,
   CARD_LOADING,
   CARD_READY,
   CARD_UNKNOWN,   /* fetched and unsupported/failed; never retried */
} card_state;

typedef struct {
   card_state state;
   char url[COBALT_POST_LINK_URI_MAX];
   char canonical_uri[COBALT_POST_URI_MAX];
   cobalt_external_card card;
} card_slot;

static card_slot s_slots[CARD_SLOTS];
static SDL_mutex *s_lock;
static SDL_cond *s_wake;
static SDL_Thread *s_thread;
static bool s_quit;
static cobalt_external_card_fetch_fn s_fetch;   /* NULL: the network */
static size_t s_fetches;

/* Without a thread (tests, or a failed init) there is nothing to contend with,
 * so the lock is simply absent. */
static void
lock(void)
{
   if (s_lock) {
      SDL_LockMutex(s_lock);
   }
}

static void
unlock(void)
{
   if (s_lock) {
      SDL_UnlockMutex(s_lock);
   }
}

/* --- parsing --- */

static const char *
json_string(const cJSON *object, const char *key)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
   return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* A URL is not prose: truncating it would fetch a different resource, so an
 * overlong one is dropped rather than ellipsised. */
static bool
copy_url(const cJSON *object, const char *key, char *out, size_t out_size)
{
   const char *value = json_string(object, key);
   if (!value || !value[0] || strlen(value) >= out_size) {
      return false;
   }
   snprintf(out, out_size, "%s", value);
   return true;
}

static void
copy_text(const cJSON *object, const char *key, char *out, size_t out_size)
{
   const char *value = json_string(object, key);
   if (value && value[0]) {
      cobalt_feed_copy_text(out, out_size, value);
   }
}

static void
parse_article(const cJSON *data, cobalt_external_card *out)
{
   out->kind = COBALT_EXTERNAL_CARD_ARTICLE;
   copy_text(data, "title", out->title, sizeof(out->title));
   copy_text(data, "description", out->description, sizeof(out->description));
   copy_url(data, "image", out->image, sizeof(out->image));

   const cJSON *publisher = cJSON_GetObjectItemCaseSensitive(data, "publisher");
   if (cJSON_IsObject(publisher)) {
      copy_text(publisher, "title", out->source_title,
                sizeof(out->source_title));
   }

   const cJSON *reading = cJSON_GetObjectItemCaseSensitive(data, "readingTime");
   if (cJSON_IsNumber(reading) && reading->valuedouble > 0 &&
       reading->valuedouble <= 9999) {
      out->reading_time = (int) reading->valuedouble;
   }
}

static void
parse_gallery_item(const cJSON *item, cobalt_external_card *out)
{
   if (!cJSON_IsObject(item) || out->image_count >= COBALT_POST_IMAGES_MAX) {
      return;
   }

   cobalt_post_image *img = &out->images[out->image_count];
   if (!copy_url(item, "thumbnail", img->thumb, sizeof(img->thumb))) {
      return;
   }
   copy_text(item, "alt", img->alt, sizeof(img->alt));

   const cJSON *aspect = cJSON_GetObjectItemCaseSensitive(item, "aspectRatio");
   if (cJSON_IsObject(aspect)) {
      const cJSON *w = cJSON_GetObjectItemCaseSensitive(aspect, "width");
      const cJSON *h = cJSON_GetObjectItemCaseSensitive(aspect, "height");
      /* A ratio with a zero edge would divide by zero when the card picks a
       * shape; keep the unknown-size path instead. */
      if (cJSON_IsNumber(w) && cJSON_IsNumber(h) &&
          w->valuedouble > 0 && h->valuedouble > 0) {
         img->aspect_w = (int) w->valuedouble;
         img->aspect_h = (int) h->valuedouble;
      }
   }
   out->image_count++;
}

static void
parse_gallery(const cJSON *data, cobalt_external_card *out)
{
   out->kind = COBALT_EXTERNAL_CARD_GALLERY;
   copy_text(data, "title", out->title, sizeof(out->title));
   copy_text(data, "description", out->description, sizeof(out->description));

   const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
   if (cJSON_IsArray(items)) {
      const int count = cJSON_GetArraySize(items);
      out->photo_count = count > 999 ? 999 : count;
      for (int i = 0; i < count && i < COBALT_POST_IMAGES_MAX; i++) {
         parse_gallery_item(cJSON_GetArrayItem(items, i), out);
      }
   }

   if (out->image_count > 0) {
      snprintf(out->image, sizeof(out->image), "%s", out->images[0].thumb);
   }
}

static void
parse_livestream(const cJSON *data, cobalt_external_card *out)
{
   out->kind = COBALT_EXTERNAL_CARD_LIVESTREAM;
   copy_text(data, "title", out->title, sizeof(out->title));
   copy_text(data, "description", out->description, sizeof(out->description));
   copy_url(data, "image", out->image, sizeof(out->image));

   const cJSON *active = cJSON_GetObjectItemCaseSensitive(data, "active");
   out->live = cJSON_IsTrue(active);
}

bool
cobalt_external_card_parse(const char *json, size_t len,
                           cobalt_external_card *out)
{
   if (!json || len == 0 || !out) {
      return false;
   }
   memset(out, 0, sizeof(*out));

   cJSON *root = cJSON_ParseWithLength(json, len);
   if (!root) {
      return false;
   }

   const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
   const char *type = cJSON_IsObject(data) ? json_string(data, "$type") : NULL;
   bool ok = false;

   if (type) {
      /* A publication has no article body of its own. The legacy `view` that
       * comes with it is already what the post's plain card drew. */
      if (strstr(type, "app.bsky.embed.external#viewArticlePublication")) {
         ok = false;
      } else if (strstr(type, "app.bsky.embed.external#viewArticle")) {
         parse_article(data, out);
         ok = out->title[0] || out->description[0] || out->image[0];
      } else if (strstr(type, "app.bsky.embed.external#viewGallery")) {
         parse_gallery(data, out);
         ok = out->title[0] || out->description[0] || out->image_count > 0;
      } else if (strstr(type, "app.bsky.embed.external#viewLivestream")) {
         parse_livestream(data, out);
         ok = out->title[0] || out->description[0] || out->image[0];
      }
   }

   cJSON_Delete(root);
   if (!ok) {
      memset(out, 0, sizeof(*out));
   }
   return ok;
}

/* --- request URL --- */

static bool
unreserved(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
          c == '~';
}

static bool
percent_encode(const char *in, char *out, size_t out_size)
{
   static const char HEX[] = "0123456789ABCDEF";
   size_t n = 0;

   for (const unsigned char *p = (const unsigned char *) in; *p; p++) {
      if (unreserved(*p)) {
         if (n + 1 >= out_size) {
            return false;
         }
         out[n++] = (char) *p;
      } else {
         if (n + 3 >= out_size) {
            return false;
         }
         out[n++] = '%';
         out[n++] = HEX[*p >> 4];
         out[n++] = HEX[*p & 0x0F];
      }
   }
   out[n] = '\0';
   return true;
}

bool
cobalt_external_card_url(const char *url, const char *canonical_uri,
                         char *out, size_t out_size)
{
   if (!url || !url[0] || !canonical_uri || !canonical_uri[0] || !out ||
       out_size == 0) {
      return false;
   }
   out[0] = '\0';

   char encoded_url[COBALT_POST_LINK_URI_MAX * 3 + 1];
   char encoded_canonical[COBALT_POST_URI_MAX * 3 + 1];
   if (!percent_encode(url, encoded_url, sizeof(encoded_url)) ||
       !percent_encode(canonical_uri, encoded_canonical,
                       sizeof(encoded_canonical))) {
      return false;
   }

   const int n = snprintf(out, out_size, "%s%s?url=%s&canonicalUri=%s",
                          CARD_API, CARD_PATH, encoded_url,
                          encoded_canonical);
   if (n <= 0 || (size_t) n >= out_size) {
      out[0] = '\0';
      return false;
   }
   return true;
}

/* --- cache and loader --- */

/* Caller holds the lock. */
static card_slot *
find_slot(const char *url, const char *canonical_uri)
{
   for (int i = 0; i < CARD_SLOTS; i++) {
      if (s_slots[i].state != CARD_FREE &&
          strcmp(s_slots[i].url, url) == 0 &&
          strcmp(s_slots[i].canonical_uri, canonical_uri) == 0) {
         return &s_slots[i];
      }
   }
   return NULL;
}

/* Caller holds the lock. */
static int
next_queued(void)
{
   for (int i = 0; i < CARD_SLOTS; i++) {
      if (s_slots[i].state == CARD_QUEUED) {
         return i;
      }
   }
   return -1;
}

/* Every HTTP request goes through here, so the counter is requests, not
 * queued cards. Caller holds no lock. */
static bool
counted_fetch(cobalt_external_card_fetch_fn fetch, const char *url,
              cobalt_http_response *out)
{
   lock();
   s_fetches++;
   unlock();
   return fetch(url, out);
}

static bool
network_fetch(const char *url, cobalt_http_response *out)
{
   return cobalt_http_get(url, CARD_BODY_MAX, out);
}

/*
 * Fetch and parse one card. Called with the lock NOT held; the slot cannot move
 * while LOADING, and the keys were copied before the lock was released.
 */
static void
resolve(int index, const char *url, const char *canonical_uri,
        cobalt_external_card_fetch_fn fetch)
{
   cobalt_external_card card;
   memset(&card, 0, sizeof(card));
   bool ok = false;

   char request[COBALT_POST_LINK_URI_MAX * 3 + COBALT_POST_URI_MAX * 3 + 256];
   if (cobalt_external_card_url(url, canonical_uri, request,
                                sizeof(request))) {
      cobalt_http_response response;
      memset(&response, 0, sizeof(response));
      if (counted_fetch(fetch, request, &response) && response.data &&
          response.status == 200) {
         ok = cobalt_external_card_parse((const char *) response.data,
                                         response.size, &card);
      }
      cobalt_http_response_free(&response);
   }

   lock();
   card_slot *slot = &s_slots[index];
   if (ok) {
      slot->card = card;
      slot->state = CARD_READY;
   } else {
      memset(&slot->card, 0, sizeof(slot->card));
      slot->state = CARD_UNKNOWN;
   }
   unlock();
}

static int
loader_main(void *unused)
{
   (void) unused;
   cobalt_thread_make_background();

   lock();
   for (;;) {
      int index = -1;
      while (!s_quit && (index = next_queued()) < 0) {
         SDL_CondWait(s_wake, s_lock);
      }
      if (s_quit) {
         break;
      }

      /* Claim it before unlocking, so no other loader takes the same slot. */
      s_slots[index].state = CARD_LOADING;
      char url[COBALT_POST_LINK_URI_MAX];
      char canonical_uri[COBALT_POST_URI_MAX];
      memcpy(url, s_slots[index].url, sizeof(url));
      memcpy(canonical_uri, s_slots[index].canonical_uri,
             sizeof(canonical_uri));
      cobalt_external_card_fetch_fn fetch = s_fetch ? s_fetch : network_fetch;

      unlock();
      resolve(index, url, canonical_uri, fetch);
      lock();
   }
   unlock();
   return 0;
}

bool
cobalt_external_card_init(void)
{
   if (s_thread) {
      return true;
   }
   s_lock = SDL_CreateMutex();
   s_wake = SDL_CreateCond();
   if (!s_lock || !s_wake) {
      SDL_DestroyCond(s_wake);
      SDL_DestroyMutex(s_lock);
      s_wake = NULL;
      s_lock = NULL;
      return false;
   }
   s_quit = false;
   s_thread = SDL_CreateThread(loader_main, "external-card", NULL);
   if (!s_thread) {
      SDL_DestroyCond(s_wake);
      SDL_DestroyMutex(s_lock);
      s_wake = NULL;
      s_lock = NULL;
      return false;
   }
   return true;
}

void
cobalt_external_card_shutdown(void)
{
   if (!s_thread) {
      return;
   }
   lock();
   s_quit = true;
   SDL_CondBroadcast(s_wake);
   unlock();
   SDL_WaitThread(s_thread, NULL);
   s_thread = NULL;
   SDL_DestroyCond(s_wake);
   SDL_DestroyMutex(s_lock);
   s_wake = NULL;
   s_lock = NULL;
}

const cobalt_external_card *
cobalt_external_card_lookup(const char *url, const char *canonical_uri)
{
   if (!url || !url[0] || strlen(url) >= COBALT_POST_LINK_URI_MAX ||
       !canonical_uri || !canonical_uri[0] ||
       strlen(canonical_uri) >= COBALT_POST_URI_MAX) {
      return NULL;
   }

   const cobalt_external_card *card = NULL;
   lock();
   card_slot *slot = find_slot(url, canonical_uri);
   if (slot) {
      if (slot->state == CARD_READY) {
         card = &slot->card;
      }
   } else {
      for (int i = 0; i < CARD_SLOTS; i++) {
         if (s_slots[i].state == CARD_FREE) {
            s_slots[i].state = CARD_QUEUED;
            snprintf(s_slots[i].url, sizeof(s_slots[i].url), "%s", url);
            snprintf(s_slots[i].canonical_uri,
                     sizeof(s_slots[i].canonical_uri), "%s", canonical_uri);
            if (s_wake) {
               SDL_CondSignal(s_wake);
            }
            break;
         }
      }
   }
   unlock();
   return card;
}

/* --- host tests --- */

void
cobalt_external_card_set_fetcher(cobalt_external_card_fetch_fn fetch)
{
   s_fetch = fetch;
}

size_t
cobalt_external_card_drain(void)
{
   size_t resolved = 0;
   for (;;) {
      lock();
      const int index = next_queued();
      if (index < 0) {
         unlock();
         break;
      }
      s_slots[index].state = CARD_LOADING;
      char url[COBALT_POST_LINK_URI_MAX];
      char canonical_uri[COBALT_POST_URI_MAX];
      memcpy(url, s_slots[index].url, sizeof(url));
      memcpy(canonical_uri, s_slots[index].canonical_uri,
             sizeof(canonical_uri));
      cobalt_external_card_fetch_fn fetch = s_fetch ? s_fetch : network_fetch;
      unlock();

      resolve(index, url, canonical_uri, fetch);
      resolved++;
   }
   return resolved;
}

size_t
cobalt_external_card_fetch_count(void)
{
   return s_fetches;
}

void
cobalt_external_card_reset_for_test(void)
{
   memset(s_slots, 0, sizeof(s_slots));
   s_fetches = 0;
}
