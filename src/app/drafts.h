#pragma once

/*
 * Composer drafts: text someone started and chose to finish later.
 *
 * A draft is intentionally not a queued post. Nothing here can publish, and
 * the app never sends one unless a person resumes it and presses Post. The
 * list is local and private, like the offline queue, but it has its own file
 * and its own screen so a half-written reply never looks like something that
 * is waiting to go out.
 *
 * This first version keeps text and the reply/quote context needed to send it
 * to the same place. Images and multi-post threads are refused rather than
 * partially saved: both would need a larger bounded store, and a draft that
 * silently came back with one of its posts missing would be worse than one
 * that was not offered.
 */

#include "atproto/feed.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_DRAFT_MAX 8
#define COBALT_DRAFT_TEXT_MAX 3001

typedef enum {
   COBALT_DRAFT_POST = 0,
   COBALT_DRAFT_REPLY,
   COBALT_DRAFT_QUOTE,
} cobalt_draft_kind;

typedef struct {
   char text[COBALT_DRAFT_TEXT_MAX];
   long long saved_at;

   cobalt_draft_kind kind;
   int reply_gate;

   /* Reply target, when kind is COBALT_DRAFT_REPLY. */
   char parent_uri[COBALT_POST_URI_MAX];
   char parent_cid[COBALT_POST_CID_MAX];
   char root_uri[COBALT_POST_URI_MAX];
   char root_cid[COBALT_POST_CID_MAX];
   char reply_to[COBALT_POST_NAME_MAX];

   /* Quoted post, when kind is COBALT_DRAFT_QUOTE. */
   char quote_uri[COBALT_POST_URI_MAX];
   char quote_cid[COBALT_POST_CID_MAX];
   char quote_handle[COBALT_POST_NAME_MAX];
} cobalt_draft;

typedef struct {
   cobalt_draft items[COBALT_DRAFT_MAX];
   unsigned count;
} cobalt_drafts;

void cobalt_drafts_init(cobalt_drafts *drafts);

/* True when the draft has text and the context its kind needs. */
bool cobalt_draft_valid(const cobalt_draft *draft);

/* Append a draft (oldest first). False, with nothing stored, when the list is
 * full or the draft is not valid. */
bool cobalt_drafts_add(cobalt_drafts *drafts, const cobalt_draft *draft);

/* The draft at `index`, or NULL when out of range. */
const cobalt_draft *cobalt_drafts_get(const cobalt_drafts *drafts, unsigned index);

/* Replace the draft at `index`. False when out of range or invalid. */
bool cobalt_drafts_replace(cobalt_drafts *drafts, unsigned index,
                           const cobalt_draft *draft);

/* Remove the draft at `index`; the rest keep their order. */
bool cobalt_drafts_remove(cobalt_drafts *drafts, unsigned index);

#ifdef __cplusplus
}
#endif
