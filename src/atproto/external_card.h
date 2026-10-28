#pragma once

/*
 * A hydrated external card: the article, gallery or livestream data the
 * AppView returns for a link that carries Atmosphere records.
 *
 * The post's own `app.bsky.embed.external#view` is enough for a plain link
 * card. A post that embedded a Standard.site document, a Grain gallery or a
 * livestream also carries `associatedRefs`, and the AppView can resolve the
 * canonical record behind one of those refs into richer `data` (viewArticle,
 * viewGallery or viewLivestream). That query is optional-auth and public, so
 * this module asks the public AppView directly with no account credentials.
 *
 * The HTTP fetch happens off the frame loop, only when a card is drawn, and
 * once per (url, canonical uri). A result is kept for the life of the process;
 * a failed or unsupported response is remembered too and never retried. The
 * caller draws the plain link card until a hydrated card is ready.
 */

#include "atproto/feed.h"
#include "net/http.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_EXTERNAL_CARD_ARTICLE = 0,
   COBALT_EXTERNAL_CARD_GALLERY,
   COBALT_EXTERNAL_CARD_LIVESTREAM,
} cobalt_external_card_kind;

typedef struct {
   cobalt_external_card_kind kind;

   char title[COBALT_POST_LINK_TITLE_MAX];
   char description[COBALT_POST_LINK_DESC_MAX];

   /* An article's or livestream's image. Empty when the view has none. */
   char image[COBALT_POST_THUMB_MAX];

   /* A gallery's items. Only the first COBALT_POST_IMAGES_MAX are kept, but
    * photo_count is the full number so the card can say "12 photos". */
   cobalt_post_image images[COBALT_POST_IMAGES_MAX];
   int image_count;
   int photo_count;

   /* An article's publication, and its estimated reading time in minutes. */
   char source_title[COBALT_POST_NAME_MAX];
   int reading_time;

   /* A livestream's state at the time the AppView answered. */
   bool live;
} cobalt_external_card;

/*
 * Parse one app.bsky.embed.getEmbedExternalView response body. Returns false
 * for an empty `{}`, an unsupported `data` kind (such as a publication) or a
 * malformed body, leaving `out` zeroed. Pure: no I/O and no allocation beyond
 * cJSON's own.
 */
bool cobalt_external_card_parse(const char *json, size_t len,
                                cobalt_external_card *out);

/*
 * Build the public AppView request for a link. `url` is the external URL the
 * post embeds; `canonical_uri` is the first `associatedRefs` record. Both are
 * percent-encoded. Exposed for the URL tests.
 */
bool cobalt_external_card_url(const char *url, const char *canonical_uri,
                              char *out, size_t out_size);

/* Starts the loader thread. Returns false when it cannot start, in which case
 * lookups stay NULL and nothing is fetched. Call after cobalt_images_init(),
 * which brings up the HTTP client the fetch uses. */
bool cobalt_external_card_init(void);
void cobalt_external_card_shutdown(void);

/*
 * The hydrated card for this link, or NULL while it is unknown or was not
 * available. The first call for a pair queues the fetch; later calls only read.
 * The returned pointer stays valid for the life of the process.
 *
 * Main thread only.
 */
const cobalt_external_card *cobalt_external_card_lookup(
   const char *url, const char *canonical_uri);

/* --- for the host tests only --- */

typedef bool (*cobalt_external_card_fetch_fn)(const char *url,
                                              cobalt_http_response *out);

/* Replaces the network fetch. NULL restores it. */
void cobalt_external_card_set_fetcher(cobalt_external_card_fetch_fn fetch);
/* Resolves every queued card on the calling thread, without the loader.
 * Returns how many it resolved. Only valid when the loader is not running. */
size_t cobalt_external_card_drain(void);
/* How many fetches have run since the last reset. */
size_t cobalt_external_card_fetch_count(void);
/* Clears the table and the counter. Only valid when the loader is not running. */
void cobalt_external_card_reset_for_test(void);

#ifdef __cplusplus
}
#endif
