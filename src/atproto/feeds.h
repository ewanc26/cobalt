/*
 * feeds.h -- custom feed discovery and management.
 *
 * Feed discovery: search and browse feed generators.
 * Feed management: save/remove/pin feeds via preferences.
 */

#ifndef COBALT_FEEDS_H
#define COBALT_FEEDS_H

#include "atproto/session.h"
#include "atproto/feed.h"
#include "cache/session_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of discovered feed generators per page. */
#define COBALT_FEED_DISCOVERY_PAGE 25

/* Maximum number of saved feeds (matches the v2 saved-feeds preference limit). */
#define COBALT_SAVED_FEEDS_MAX 16

/* A discovered feed generator (app.bsky.feed.defs#generatorView). */
typedef struct {
    char uri[COBALT_POST_URI_MAX];
    char did[COBALT_DID_MAX];
    char display_name[COBALT_POST_NAME_MAX];
    char description[COBALT_POST_LINK_DESC_MAX];
    char avatar[COBALT_POST_URI_MAX];
    bool has_like_count;
    int64_t like_count;
    bool has_viewer_like;
    bool viewer_like;
    bool has_subscriber_count;
    int64_t subscriber_count;
    bool has_viewer_subscribed;
    bool viewer_subscribed;
} cobalt_feed_generator;

/* A page of discovered feed generators. */
typedef struct {
    int count;
    bool has_more;
    char cursor[COBALT_CURSOR_MAX];
    cobalt_feed_generator generators[COBALT_FEED_DISCOVERY_PAGE];
} cobalt_feed_discovery_page;

/* A saved feed entry (from the v2 saved-feeds preference items where type == "feed"). */
typedef struct {
    char uri[COBALT_POST_URI_MAX];
    char display_name[COBALT_POST_NAME_MAX];
    bool pinned;
} cobalt_saved_feed_entry;

/* The account's saved feeds list (the v2 saved-feeds preference). */
typedef struct {
    int count;
    cobalt_saved_feed_entry entries[COBALT_SAVED_FEEDS_MAX];
} cobalt_saved_feeds_v2;

/* Feed discovery result. */
const cobalt_feed_discovery_page *cobalt_session_feed_discovery_results(void);

/* Saved feeds v2 result. */
const cobalt_saved_feeds_v2 *cobalt_session_saved_feeds_v2(void);

/* Discovery: begin fetching suggested/popular feed generators. */
bool cobalt_session_begin_feed_discovery(const char *cursor);

/* Discovery: begin fetching a specific feed generator's details. */
bool cobalt_session_begin_feed_generator(const char *feed_uri);

/* Management: begin fetching the account's v2 saved-feeds preference. */
bool cobalt_session_begin_saved_feeds_v2(void);

/* Management: save a feed (add to the v2 saved-feeds preference). */
bool cobalt_session_begin_save_feed(const char *feed_uri, bool pinned);

/* Management: remove a feed from the v2 saved-feeds preference. */
bool cobalt_session_begin_remove_feed(const char *feed_uri);

/* Management: pin/unpin a saved feed. */
bool cobalt_session_begin_pin_feed(const char *feed_uri, bool pinned);

#ifdef __cplusplus
}
#endif

#endif /* COBALT_FEEDS_H */
