#pragma once

/*
 * Feed discovery and management: browse the suggested feed generators,
 * inspect one, and save / pin / remove it. Two modes on one screen, the same
 * shape lists.c uses: `browsing_detail` false shows the discovered generators,
 * true shows the selected generator's details and actions. B from the detail
 * returns to the browse list rather than leaving the screen.
 */

#include "atproto/feeds.h"
#include "atproto/session.h"
#include "input/input.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_FEED_STATUS_MAX 192

typedef enum {
   COBALT_FEED_DISCOVERY_VIEW_STAY = 0,
   COBALT_FEED_DISCOVERY_VIEW_BACK,      /* leave for the feeds picker */
   COBALT_FEED_DISCOVERY_VIEW_OPEN_FEED, /* open the inspected feed's timeline */
} cobalt_feed_discovery_view_action;

typedef struct {
   cobalt_listnav nav;
   bool browsing_detail;   /* false: discovered generators; true: one generator */
   char open_uri[COBALT_POST_URI_MAX];   /* the generator currently inspected */

   bool saving;
   /* Removing takes two presses, so a stray X cannot remove a feed. */
   int remove_confirm;

   char status[COBALT_FEED_STATUS_MAX];
   bool status_error;
} cobalt_feed_discovery_view;

void cobalt_feed_discovery_view_init(cobalt_feed_discovery_view *view);

/* Rewinds to the browse list and, if no page is held yet, fetches one. */
void cobalt_feed_discovery_view_open(cobalt_feed_discovery_view *view);

/* Jump straight into one generator's detail, e.g. from the feeds picker's
 * "manage" action on a saved feed. */
void cobalt_feed_discovery_view_open_feed(cobalt_feed_discovery_view *view,
                                          const char *feed_uri);

cobalt_feed_discovery_view_action
cobalt_feed_discovery_view_update(cobalt_feed_discovery_view *view,
                                  const cobalt_input *in);

/* The write finished. Clears the saving flag, shows the message, and refreshes
 * the saved-feeds lists on a successful mutation. */
void cobalt_feed_discovery_view_result(cobalt_feed_discovery_view *view,
                                       cobalt_job_kind kind, bool ok,
                                       const char *message);

/* Saved state of a feed URI in the v2 saved-feeds list: -1 not saved,
 * 0 saved, 1 saved and pinned. Host-tested. */
int cobalt_feed_saved_state(const char *feed_uri);

void cobalt_feed_discovery_view_draw(cobalt_feed_discovery_view *view,
                                     cobalt_render *r,
                                     cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
