#pragma once

/*
 * Curated lists (app.bsky.graph.list): the account's own lists, flattened for
 * a menu, and one list's members, which reuse cobalt_actor_list wholesale —
 * a list member is the same "avatar, name, handle" row shape as everything
 * else in actors.h, so there is no separate row type to define.
 *
 * The list-of-lists rows carry the record's purpose so the editor can show
 * and preserve whether a list is a curation or moderation list.
 */

#include "atproto/actors.h"
#include "atproto/feed.h"   /* COBALT_CURSOR_MAX, COBALT_POST_* */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_LISTS_MAX 30
#define COBALT_LIST_PURPOSE_MAX 64

#define COBALT_LIST_PURPOSE_CURATE "app.bsky.graph.defs#curatelist"
#define COBALT_LIST_PURPOSE_MODERATE "app.bsky.graph.defs#modlist"

/* A short label for a list purpose, and the purpose that follows it when the
 * editor cycles. Unknown purposes read as curate, the service default. */
const char *cobalt_list_purpose_label(const char *purpose);
const char *cobalt_list_purpose_next(const char *purpose);
/* A list write needs a non-empty name and one of the two supported purposes;
 * an empty description is valid. */
bool cobalt_list_edit_valid(const char *name, const char *description,
                            const char *purpose);

typedef struct {
   char uri[COBALT_POST_URI_MAX];
   char name[COBALT_POST_NAME_MAX];
   char description[COBALT_POST_NAME_MAX];
   char purpose[COBALT_LIST_PURPOSE_MAX];
   char avatar[COBALT_POST_AVATAR_MAX];
} cobalt_list_summary;

typedef struct {
   cobalt_list_summary lists[COBALT_LISTS_MAX];
   int count;
   char cursor[COBALT_CURSOR_MAX];
   bool has_more;
} cobalt_list_summary_list;

void cobalt_list_summary_list_reset(cobalt_list_summary_list *list);

bool cobalt_list_summary_list_can_page(const cobalt_list_summary_list *list);

#ifdef COBALT_HAS_WOLFRAM
/* Forward-declared rather than including <wolfram/list_typed.h>, so this
 * header stays usable from a build without the SDK — same reasoning as
 * actors.h's wolfram forward declarations. */
struct wf_agent_list_view_list;
int cobalt_list_summary_list_append_from_wolfram(
   cobalt_list_summary_list *list, const struct wf_agent_list_view_list *src);

struct wf_agent_list_item_list;
/* Flattens a getList response's members onto the end of `list`, reusing
 * cobalt_actor's row shape. Returns how many rows were actually appended. */
int cobalt_actor_list_append_from_wolfram_list_items(
   cobalt_actor_list *list, const struct wf_agent_list_item_list *src);
#endif

#ifdef __cplusplus
}
#endif
