#pragma once

/*
 * A flattened list of actors — muted accounts, blocked accounts, and later
 * list members and search results, which all share this exact "avatar,
 * name, handle, one action" row shape.
 *
 * Same reasoning as feed.h and notifications.h: Wolfram hands back an owned
 * `wf_agent_actor_list` of heap strings, and the render loop wants fixed
 * buffers it can read every frame without touching an allocator.
 */

#include "atproto/feed.h"
#include "cache/session_store.h"   /* COBALT_DID_MAX */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_ACTORS_MAX 60

typedef struct {
   char did[COBALT_DID_MAX];
   char handle[COBALT_POST_NAME_MAX];   /* with a leading @ */
   char display_name[COBALT_POST_NAME_MAX];   /* display name, or the handle */
   char avatar[COBALT_POST_AVATAR_MAX]; /* URL, empty when unset */

   /*
    * The record an "undo" deletes — a block record URI on a blocked-accounts
    * list. Empty on a muted-accounts list: mute has no record, only a flag,
    * so there is nothing here to delete and unmuting instead takes the DID
    * directly (see cobalt_session_begin_unmute).
    */
   char record_uri[COBALT_POST_URI_MAX];
} cobalt_actor;

typedef struct {
   cobalt_actor actors[COBALT_ACTORS_MAX];
   int count;
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
} cobalt_actor_list;

void cobalt_actor_list_reset(cobalt_actor_list *list);

/* As cobalt_feed_can_page, for the same reason: the window is fixed, so once
 * full, paging further would append nothing while still holding a cursor. */
bool cobalt_actor_list_can_page(const cobalt_actor_list *list);

/* Drop the row for `did`, if present — an unmute/unblock removes the account
 * from the list it came from rather than waiting for a refetch. Returns
 * false if the row was not found (a refresh can replace the list while a
 * request is in flight, so it legitimately may not be there any more). */
bool cobalt_actor_list_remove(cobalt_actor_list *list, const char *did);

#ifdef COBALT_HAS_WOLFRAM
/*
 * Flatten a Wolfram actor list onto the end of `list`, stopping at
 * COBALT_ACTORS_MAX. Returns how many rows were actually appended.
 *
 * Forward-declared rather than including <wolfram/agent.h>, so this header
 * stays usable from a build without the SDK.
 */
struct wf_agent_actor_list;
int cobalt_actor_list_append_from_wolfram(cobalt_actor_list *list,
                                          const struct wf_agent_actor_list *src);
#endif

#ifdef __cplusplus
}
#endif
