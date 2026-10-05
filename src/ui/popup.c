#include "ui/popup.h"
#include "ui/theme.h"

#include <stdio.h>
#include <string.h>

static void
copy_utf8(char *out, size_t size, const char *src)
{
   if (!src) {
      src = "";
   }
   size_t len = strlen(src);
   if (len >= size) {
      len = size - 1;
      while (len > 0 && ((unsigned char) src[len] & 0xC0) == 0x80) {
         len--;
      }
   }
   memcpy(out, src, len);
   out[len] = '\0';
}

void
cobalt_popup_open(cobalt_popup *p, const char *title)
{
   memset(p, 0, sizeof(*p));
   p->open = true;
   copy_utf8(p->title, sizeof(p->title), title);
}

void
cobalt_popup_add(cobalt_popup *p, cobalt_popup_kind kind, const char *label,
                 const char *arg)
{
   if (p->count >= COBALT_POPUP_MAX) {
      return;
   }
   cobalt_popup_item *it = &p->items[p->count++];
   it->kind = kind;
   copy_utf8(it->label, sizeof(it->label), label);
   copy_utf8(it->arg, sizeof(it->arg), arg);
}

void
cobalt_popup_show_text(cobalt_popup *p, const char *title, const char *text)
{
   cobalt_popup_open(p, title);
   p->text_mode = true;
   copy_utf8(p->text, sizeof(p->text), text);
}

void
cobalt_popup_close(cobalt_popup *p)
{
   p->open = false;
   p->hit_valid = false;
}

int
cobalt_popup_update(cobalt_popup *p, const cobalt_input *in)
{
   if (!p->open) {
      return -1;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_BACK) ||
       (p->text_mode && cobalt_input_pressed(in, COBALT_BTN_CONFIRM))) {
      cobalt_popup_close(p);
      return -2;
   }
   if (p->text_mode || p->count == 0) {
      return -1;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) && p->selected < p->count - 1) {
      p->selected++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && p->selected > 0) {
      p->selected--;
   }
   if (p->hit_valid && in->touch_ended) {
      for (int i = 0; i < p->count; i++) {
         if (cobalt_input_tapped(in, &p->hit[i])) {
            p->selected = i;
            return i;
         }
      }
      if (!cobalt_input_tapped(in, &p->panel)) {
         cobalt_popup_close(p);
         return -2;
      }
   }
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      return p->selected;
   }
   return -1;
}

static const char *
icon_for(cobalt_popup_kind kind)
{
   switch (kind) {
      case COBALT_POPUP_PROFILE: return COBALT_ICON_USER;
      case COBALT_POPUP_MENTION: return COBALT_ICON_USER;
      case COBALT_POPUP_TAG:     return COBALT_ICON_HASH;
      case COBALT_POPUP_LINK:    return COBALT_ICON_LIST;
      case COBALT_POPUP_QUOTE:   return COBALT_ICON_PENCIL;
      case COBALT_POPUP_DELETE:  return COBALT_ICON_WARNING;
      case COBALT_POPUP_IMAGE:   return COBALT_ICON_IMAGE;
      case COBALT_POPUP_LIKES:   return COBALT_ICON_LIKE;
      case COBALT_POPUP_REPOSTS: return COBALT_ICON_REPOST;
      case COBALT_POPUP_COMPOSE: return COBALT_ICON_PENCIL;
      case COBALT_POPUP_REFRESH: return COBALT_ICON_HOUSE;
   }
   return COBALT_ICON_LIST;
}

void
cobalt_popup_draw(cobalt_popup *p, cobalt_render *r, cobalt_surface_id surface)
{
   if (!p->open) {
      return;
   }
   const bool touchable = (surface == COBALT_SURFACE_DRC);
   const cobalt_metrics *m = cobalt_render_metrics(r);

   const SDL_Rect all = { 0, 0, m->width, m->height };
   const SDL_Color scrim = { 0x10, 0x18, 0x28, 0xB4 };
   cobalt_fill_rect(r, &all, scrim);

   const int title_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);
   const int row_h = cobalt_font_line_height(r, COBALT_FONT_BODY) + m->pad_tile;
   const int pill_h = cobalt_pill_height(r);
   const int pad = m->pad_tile;

   const int w = m->width * 3 / 4;
   int rows = p->text_mode ? 2 : p->count;
   int max_h = m->height - 2 * m->pad_edge;
   int fixed = pad + title_h + pad + pad + pill_h + pad;
   int max_rows = (max_h - fixed) / row_h;
   if (max_rows < 1) {
      max_rows = 1;
   }
   int shown = rows < max_rows ? rows : max_rows;
   int first = 0;
   if (!p->text_mode && p->selected >= shown) {
      first = p->selected - shown + 1;
   }
   const int h = fixed + shown * row_h;
   p->panel = (SDL_Rect) { (m->width - w) / 2, (m->height - h) / 2, w, h };
   cobalt_draw_tile(r, &p->panel, 0.0f);

   int y = p->panel.y + pad;
   cobalt_draw_text(r, COBALT_FONT_HEADING, p->title, p->panel.x + pad, y,
                    COBALT_COLOUR_TEXT_DIM);
   y += title_h + pad;

   if (touchable) {
      p->hit_valid = false;
   }
   if (p->text_mode) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, p->text, p->panel.x + pad, y,
                               w - 2 * pad, 2, COBALT_COLOUR_TEXT);
   } else {
      for (int i = first; i < p->count && i < first + shown; i++) {
         SDL_Rect row = { p->panel.x + pad / 2, y, w - pad, row_h };
         if (i == p->selected) {
            SDL_Color sel = COBALT_COLOUR_ACCENT;
            sel.a = 40;
            cobalt_fill_rect(r, &row, sel);
         }
         const int icon_w = cobalt_draw_text(
            r, COBALT_FONT_ICON, icon_for(p->items[i].kind), row.x + pad,
            y + pad / 2, i == p->selected ? COBALT_COLOUR_ACCENT : COBALT_COLOUR_TEXT_DIM);
         cobalt_draw_text(r, COBALT_FONT_BODY, p->items[i].label,
                          row.x + pad + icon_w + pad, y + pad / 2, COBALT_COLOUR_TEXT);
         if (touchable) {
            p->hit[i] = row;
            p->hit_valid = true;
         }
         y += row_h;
      }
   }

   const int py = p->panel.y + h - pad - pill_h;
   int px = p->panel.x + pad;
   if (!p->text_mode) {
      cobalt_draw_pill(r, "A", "Select", px, py);
      px += cobalt_pill_width(r, "A", "Select") + 16;
   }
   cobalt_draw_pill(r, "B", "Close", px, py);
}
