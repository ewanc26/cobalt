#include "ui/listnav.h"

#include "atproto/feed.h"

#include <string.h>

void
cobalt_listnav_init(cobalt_listnav *nav)
{
   if (!nav) {
      return;
   }
   memset(nav, 0, sizeof(*nav));
   nav->last_visible = -1;
}

void
cobalt_listnav_rewind(cobalt_listnav *nav)
{
   if (!nav) {
      return;
   }
   nav->selected = 0;
   nav->scroll = 0;
   nav->last_visible = -1;
}

void
cobalt_listnav_restore(cobalt_listnav *nav, int selected, int scroll)
{
   if (!nav) {
      return;
   }
   if (selected < 0) {
      selected = 0;
   }
   if (scroll < 0) {
      scroll = 0;
   }
   if (scroll > selected) {
      scroll = selected;
   }
   nav->selected = selected;
   nav->scroll = scroll;
   nav->last_visible = -1;
}

int
cobalt_listnav_move(cobalt_listnav *nav, const cobalt_input *in, int count)
{
   int tapped = -1;

   if (!nav || !in || count <= 0) {
      return -1;
   }

   cobalt_list_clamp(&nav->selected, &nav->scroll, count);
   cobalt_input_drag_list(in, &nav->selected, count, nav->hit_valid ? nav->hit : NULL,
                          nav->hit_count);

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) && nav->selected < count - 1) {
      nav->selected++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && nav->selected > 0) {
      nav->selected--;
   }

   if (nav->hit_valid && in->touch_ended) {
      for (int i = 0; i < nav->hit_count; i++) {
         if (cobalt_input_tapped(in, &nav->hit[i])) {
            nav->selected = nav->hit_index[i];
            tapped = nav->hit_index[i];
            break;
         }
      }
   }
   return tapped;
}

void
cobalt_listnav_follow(cobalt_listnav *nav)
{
   if (!nav) {
      return;
   }
   if (nav->selected < nav->scroll) {
      nav->scroll = nav->selected;
   } else if (nav->last_visible >= 0 && nav->selected > nav->last_visible) {
      nav->scroll += nav->selected - nav->last_visible;
   }
   if (nav->scroll > nav->selected) {
      nav->scroll = nav->selected;
   }
   if (nav->scroll < 0) {
      nav->scroll = 0;
   }
}

void
cobalt_listnav_draw_begin(cobalt_listnav *nav, bool touchable)
{
   if (nav && touchable) {
      nav->hit_count = 0;
   }
}

void
cobalt_listnav_draw_add(cobalt_listnav *nav, bool touchable, const SDL_Rect *rect, int index)
{
   if (nav && touchable && rect && nav->hit_count < COBALT_LISTNAV_MAX) {
      nav->hit[nav->hit_count] = *rect;
      nav->hit_index[nav->hit_count] = index;
      nav->hit_count++;
   }
}

void
cobalt_listnav_draw_end(cobalt_listnav *nav, bool touchable, int last_fitted)
{
   if (nav && touchable) {
      nav->hit_valid = true;
      nav->last_visible = last_fitted;
   }
}

void
cobalt_listnav_draw_empty(cobalt_listnav *nav, bool touchable)
{
   if (nav && touchable) {
      nav->last_visible = -1;
   }
}
