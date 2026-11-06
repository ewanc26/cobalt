#include "atproto/starter_packs.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/graph_social_typed.h>
#include <cJSON.h>
#endif

#include <stdio.h>
#include <string.h>

void
cobalt_starter_pack_reset(cobalt_starter_pack *pack)
{
   if (pack) {
      memset(pack, 0, sizeof(*pack));
   }
}

int
cobalt_starter_pack_row_count(const cobalt_starter_pack *pack,
                              const cobalt_actor_list *members)
{
   const int member_rows = members ? members->count : 0;
   const int feed_rows = pack ? pack->feed_count : 0;

   return member_rows + feed_rows;
}

static bool
starter_pack_at_uri(const char *link, char *out, size_t out_size)
{
   static const char PREFIX[] = "at://";
   static const char COLLECTION[] = "app.bsky.graph.starterpack";
   const char *authority;
   const char *collection;
   const char *rkey;
   size_t len;

   if (!link || !out || out_size == 0 || strncmp(link, PREFIX, sizeof(PREFIX) - 1) != 0) {
      return false;
   }
   authority = link + sizeof(PREFIX) - 1;
   collection = strchr(authority, '/');
   if (!collection || collection == authority) {
      return false;
   }
   collection++;
   if (strncmp(collection, COLLECTION, sizeof(COLLECTION) - 1) != 0 ||
       collection[sizeof(COLLECTION) - 1] != '/') {
      return false;
   }
   rkey = collection + sizeof(COLLECTION);
   if (!rkey[0] || strchr(rkey, '/')) {
      return false;
   }
   len = strlen(link);
   if (len >= out_size) {
      return false;
   }
   memcpy(out, link, len + 1);
   return true;
}

static bool
starter_pack_web_uri(const char *link, char *out, size_t out_size)
{
   static const char HTTPS_PREFIX[] = "https://bsky.app/starter-pack/";
   static const char HTTP_PREFIX[] = "http://bsky.app/starter-pack/";
   const char *path;
   const char *authority_end;
   const char *rkey_end;
   size_t authority_len;
   size_t rkey_len;
   int written;

   if (!link || !out || out_size == 0) {
      return false;
   }
   if (strncmp(link, HTTPS_PREFIX, sizeof(HTTPS_PREFIX) - 1) == 0) {
      path = link + sizeof(HTTPS_PREFIX) - 1;
   } else if (strncmp(link, HTTP_PREFIX, sizeof(HTTP_PREFIX) - 1) == 0) {
      path = link + sizeof(HTTP_PREFIX) - 1;
   } else {
      return false;
   }

   authority_end = strchr(path, '/');
   if (!authority_end || authority_end == path) {
      return false;
   }
   rkey_end = strpbrk(authority_end + 1, "?#");
   if (!rkey_end) {
      rkey_end = path + strlen(path);
   }
   if (rkey_end <= authority_end + 1 || strchr(authority_end + 1, '/')) {
      return false;
   }
   authority_len = (size_t) (authority_end - path);
   rkey_len = (size_t) (rkey_end - (authority_end + 1));

   written = snprintf(out, out_size, "at://%.*s/app.bsky.graph.starterpack/%.*s",
                      (int) authority_len, path, (int) rkey_len, authority_end + 1);
   return written > 0 && (size_t) written < out_size;
}

bool
cobalt_starter_pack_uri_from_link(const char *link, char *out, size_t out_size)
{
   if (!out || out_size == 0) {
      return false;
   }
   out[0] = '\0';
   return starter_pack_at_uri(link, out, out_size) ||
          starter_pack_web_uri(link, out, out_size);
}

#ifdef COBALT_HAS_WOLFRAM

int
cobalt_starter_pack_fill_from_wolfram(
   cobalt_starter_pack *pack, const struct wf_graph_starter_pack_view *src)
{
   const wf_graph_starter_pack_view *view = src;
   const cJSON *record;
   const cJSON *list;
   const cJSON *feeds;
   const cJSON *feed;
   int added = 0;

   if (!pack || !view) {
      return 0;
   }
   cobalt_starter_pack_reset(pack);

   snprintf(pack->uri, sizeof(pack->uri), "%s", view->uri ? view->uri : "");
   cobalt_feed_copy_text(pack->name, sizeof(pack->name),
                         view->name && view->name[0] ? view->name : "Starter pack");
   cobalt_feed_copy_text(pack->description, sizeof(pack->description),
                         view->description ? view->description : "");

   snprintf(pack->creator.did, sizeof(pack->creator.did), "%s",
            view->creator.did ? view->creator.did : "");
   snprintf(pack->creator.handle, sizeof(pack->creator.handle), "@%s",
            view->creator.handle ? view->creator.handle : "");
   cobalt_feed_copy_text(pack->creator.display_name,
                         sizeof(pack->creator.display_name),
                         view->creator.display_name && view->creator.display_name[0]
                            ? view->creator.display_name
                            : (view->creator.handle ? view->creator.handle : ""));
   snprintf(pack->creator.avatar, sizeof(pack->creator.avatar), "%s",
            view->creator.avatar ? view->creator.avatar : "");

   if (view->has_list_item_count && view->list_item_count > 0) {
      pack->member_count = view->list_item_count > 1000000
                              ? 1000000
                              : (int) view->list_item_count;
   }

   record = view->record;
   if (!record) {
      return 0;
   }
   list = cJSON_GetObjectItemCaseSensitive(record, "list");
   if (cJSON_IsString(list) && list->valuestring) {
      snprintf(pack->list_uri, sizeof(pack->list_uri), "%s", list->valuestring);
   }

   feeds = cJSON_GetObjectItemCaseSensitive(record, "feeds");
   if (!cJSON_IsArray(feeds)) {
      return 0;
   }
   cJSON_ArrayForEach(feed, feeds) {
      const cJSON *uri = cJSON_GetObjectItemCaseSensitive(feed, "uri");

      if (pack->feed_count >= COBALT_STARTER_PACK_FEEDS_MAX) {
         break;
      }
      if (cJSON_IsString(uri) && uri->valuestring && uri->valuestring[0]) {
         snprintf(pack->feeds[pack->feed_count], sizeof(pack->feeds[0]), "%s",
                  uri->valuestring);
         pack->feed_count++;
         added++;
      }
   }
   return added;
}

#endif /* COBALT_HAS_WOLFRAM */
