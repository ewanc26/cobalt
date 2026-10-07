#include "ui/popup.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
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

   if (cobalt_popup_text_is_link(p->text)) {
      uint8_t *modules = NULL;
      int size = 0;
      if (wf_qr_encode(p->text, WF_QR_ECC_M, &modules, &size) == WF_OK && modules) {
         memcpy(p->qr, modules, (size_t) size * (size_t) size);
         p->qr_size = size;
      }
      free(modules);
   }
}

bool
cobalt_popup_text_is_link(const char *text)
{
   return text && (strncmp(text, "http://", 7) == 0 || strncmp(text, "https://", 8) == 0);
}

/* Largest whole-pixel module that fits `room` pixels with the four-module quiet
 * zone, at most `cap`; 0 when even one pixel per module does not fit. */
static int
qr_module_px(const cobalt_popup *p, int room, int cap)
{
   int px = p->qr_size > 0 ? room / (p->qr_size + 8) : 0;
   return px > cap ? cap : px;
}

static void
draw_qr(cobalt_render *r, const cobalt_popup *p, int module_px, int x, int y)
{
   const int quiet = 4 * module_px;
   const int side = p->qr_size * module_px + 2 * quiet;
   const SDL_Rect paper = { x, y, side, side };
   cobalt_fill_rect(r, &paper, (SDL_Color) { 0xFF, 0xFF, 0xFF, 0xFF });
   for (int row = 0; row < p->qr_size; row++) {
      for (int col = 0; col < p->qr_size; col++) {
         if (p->qr[row * p->qr_size + col]) {
            const SDL_Rect m = { x + quiet + col * module_px, y + quiet + row * module_px,
                                 module_px, module_px };
            cobalt_fill_rect(r, &m, (SDL_Color) { 0, 0, 0, 0xFF });
         }
      }
   }
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
      case COBALT_POPUP_FOLLOWERS:
      case COBALT_POPUP_FOLLOWING: return COBALT_ICON_USERS;
      case COBALT_POPUP_TAB:     return COBALT_ICON_LIST;
      case COBALT_POPUP_THREAD:  return COBALT_ICON_REPLY;
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
   /* A link's QR code takes whatever the two text lines leave, up to 6px modules. */
   int qr_px = 0;
   if (p->text_mode && p->qr_size > 0) {
      qr_px = qr_module_px(p, max_h - fixed - rows * row_h, 6);
      if (qr_px < 1) {
         qr_px = 0;
      }
   }
   const int qr_side = qr_px ? (p->qr_size + 8) * qr_px : 0;
   int shown = rows < max_rows ? rows : max_rows;
   int first = 0;
   if (!p->text_mode && p->selected >= shown) {
      first = p->selected - shown + 1;
   }
   const int h = fixed + shown * row_h + (qr_side ? qr_side + pad : 0);
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
      if (qr_side) {
         draw_qr(r, p, qr_px, p->panel.x + (w - qr_side) / 2, y + 2 * row_h + pad / 2);
      }
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
