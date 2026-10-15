#pragma once

#include "atproto/dms.h"
#include "input/input.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_DMS_ACTION_STAY = 0,
   COBALT_DMS_ACTION_BACK,
   COBALT_DMS_ACTION_OPEN_CONVO,
   COBALT_DMS_ACTION_COMPOSE_CLOSED,
} cobalt_dms_action;

typedef struct {
   cobalt_listnav nav;
   bool composing;
} cobalt_dms_view;

void cobalt_dms_view_init(cobalt_dms_view *view);
void cobalt_dms_view_open(cobalt_dms_view *view);

cobalt_dms_action cobalt_dms_view_update(cobalt_dms_view *view,
                                          const cobalt_input *in);
void cobalt_dms_view_draw(cobalt_dms_view *view, cobalt_render *r,
                           cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
