#pragma once

/*
 * Starter packs (app.bsky.graph.starterpack). A pack is a named list of
 * suggested accounts plus a handful of feed references. Wolfram returns the
 * pack record and its creator; the members are the pack's underlying list and
 * are fetched a page at a time into a bounded cobalt_actor_list.
 *
 * Read/use only: Cobalt can open a pack, browse its members and open the
 * included feeds. Creating or editing one is out of scope until the app has a
 * list editor to build the required record from.
 */

#include "atproto/actors.h"
#include "atproto/feed.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_STARTER_PACK_FEEDS_MAX 8
#define COBALT_STARTER_PACK_DESC_MAX (COBALT_POST_LINK_DESC_MAX * 2)

typedef struct {
   char uri[COBALT_POST_URI_MAX];
   char name[COBALT_POST_NAME_MAX];
   char description[COBALT_STARTER_PACK_DESC_MAX];
   cobalt_actor creator;
   char list_uri[COBALT_POST_URI_MAX];
   int member_count;
   char feeds[COBALT_STARTER_PACK_FEEDS_MAX][COBALT_POST_URI_MAX];
   int feed_count;
} cobalt_starter_pack;

void cobalt_starter_pack_reset(cobalt_starter_pack *pack);

/*
 * Accepts either an app.bsky.graph.starterpack AT-URI or a
 * https://bsky.app/starter-pack/<handle-or-did>/<rkey> link and writes the
 * canonical AT-URI to `out`. Returns false for any other link, an incomplete
 * path, or a URI too long for `out`.
 */
bool cobalt_starter_pack_uri_from_link(const char *link, char *out, size_t out_size);

/* Total rows a pack screen can navigate: members first, then feeds. */
int cobalt_starter_pack_row_count(const cobalt_starter_pack *pack,
                                  const cobalt_actor_list *members);

#ifdef COBALT_HAS_WOLFRAM
/* Forward-declared rather than including graph_social_typed.h, so this header
 * stays usable from a build without the SDK. */
struct wf_graph_starter_pack_view;
int cobalt_starter_pack_fill_from_wolfram(
   cobalt_starter_pack *pack, const struct wf_graph_starter_pack_view *src);
#endif

#ifdef __cplusplus
}
#endif
