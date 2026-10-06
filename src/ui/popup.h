#pragma once

/*
 * A modal list drawn over the current screen: the "More" menu on a post, and a
 * plain text page for showing a link's address. Items are copied in when it
 * opens, so nothing in it points into a feed buffer that a refresh could
 * rewrite.
 */

#include "input/input.h"
#include "ui/render.h"
#include "ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_POPUP_MAX 20
#define COBALT_POPUP_LABEL_MAX 72
#define COBALT_POPUP_ARG_MAX 128

typedef enum {
   COBALT_POPUP_PROFILE,
   COBALT_POPUP_MENTION,
   COBALT_POPUP_TAG,
   COBALT_POPUP_LINK,
   COBALT_POPUP_QUOTE,
   COBALT_POPUP_DELETE,
   COBALT_POPUP_IMAGE,
   COBALT_POPUP_LIKES,
   COBALT_POPUP_REPOSTS,
   COBALT_POPUP_COMPOSE,
   COBALT_POPUP_REFRESH,
   COBALT_POPUP_FOLLOWERS,
   COBALT_POPUP_FOLLOWING,
   COBALT_POPUP_TAB,
   COBALT_POPUP_THREAD,
} cobalt_popup_kind;

typedef struct {
   cobalt_popup_kind kind;
   char label[COBALT_POPUP_LABEL_MAX];
   char arg[COBALT_POPUP_ARG_MAX];
} cobalt_popup_item;

typedef struct {
   bool open;
   char title[COBALT_POPUP_LABEL_MAX];
   int count;
   int selected;
   cobalt_popup_item items[COBALT_POPUP_MAX];

   /* Text page mode, used to show a link's address. */
   bool text_mode;
   char text[COBALT_POPUP_ARG_MAX];

   SDL_Rect panel;
   SDL_Rect hit[COBALT_POPUP_MAX];
   bool hit_valid;
} cobalt_popup;

void cobalt_popup_open(cobalt_popup *p, const char *title);
void cobalt_popup_add(cobalt_popup *p, cobalt_popup_kind kind, const char *label,
                      const char *arg);
void cobalt_popup_show_text(cobalt_popup *p, const char *title, const char *text);
void cobalt_popup_close(cobalt_popup *p);

/* Returns the chosen index, -1 while nothing was chosen, -2 when closed. */
int cobalt_popup_update(cobalt_popup *p, const cobalt_input *in);

void cobalt_popup_draw(cobalt_popup *p, cobalt_render *r, cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
