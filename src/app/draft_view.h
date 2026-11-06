#pragma once

/* The Drafts screen: locally saved compositions, oldest first. */

#include "app/drafts.h"
#include "input/input.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_DRAFT_ROWS 3

typedef enum {
   COBALT_DRAFT_VIEW_STAY = 0,
   COBALT_DRAFT_VIEW_BACK,
   COBALT_DRAFT_VIEW_RESUME,  /* the person chose to continue the selected draft */
   COBALT_DRAFT_VIEW_DELETE,  /* the person pressed delete twice */
} cobalt_draft_view_action;

typedef struct {
   unsigned selected;
   unsigned scroll;
   bool delete_armed;
} cobalt_draft_view;

void cobalt_draft_view_open(cobalt_draft_view *view);

/* On RESUME or DELETE, `*index` is the draft the action applies to. Delete is
 * armed by the first press and only returned by the second press on the same
 * draft. */
cobalt_draft_view_action cobalt_draft_view_update(cobalt_draft_view *view,
                                                  const cobalt_drafts *drafts,
                                                  const cobalt_input *in,
                                                  unsigned *index);

void cobalt_draft_view_draw(const cobalt_draft_view *view,
                            const cobalt_drafts *drafts, cobalt_render *r);

#ifdef __cplusplus
}
#endif
