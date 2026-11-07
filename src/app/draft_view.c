#include "app/draft_view.h"

#include <stdio.h>

void
cobalt_draft_view_open(cobalt_draft_view *view)
{
   view->selected = 0;
   view->scroll = 0;
   view->delete_armed = false;
}

static const char *
draft_context(const cobalt_draft *draft)
{
   if (!draft) {
      return "";
   }
   switch (draft->kind) {
      case COBALT_DRAFT_REPLY:
         return draft->reply_to[0] ? "Reply" : "Reply";
      case COBALT_DRAFT_QUOTE:
         return "Quote";
      case COBALT_DRAFT_POST:
      default:
         return "New post";
   }
}

cobalt_draft_view_action
cobalt_draft_view_update(cobalt_draft_view *view, const cobalt_drafts *drafts,
                         const cobalt_input *in, unsigned *index)
{
   if (!view || !drafts || !in) {
      return COBALT_DRAFT_VIEW_STAY;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      view->delete_armed = false;
      return COBALT_DRAFT_VIEW_BACK;
   }
   if (drafts->count == 0) {
      view->selected = 0;
      view->scroll = 0;
      return COBALT_DRAFT_VIEW_STAY;
   }
   if (view->selected >= drafts->count) {
      view->selected = drafts->count - 1;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && view->selected > 0) {
      view->selected--;
      view->delete_armed = false;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
       view->selected + 1 < drafts->count) {
      view->selected++;
      view->delete_armed = false;
   }

   view->scroll = (view->selected / COBALT_DRAFT_ROWS) * COBALT_DRAFT_ROWS;

   if (cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
      if (!view->delete_armed) {
         view->delete_armed = true;
         return COBALT_DRAFT_VIEW_STAY;
      }
      view->delete_armed = false;
      if (index) {
         *index = view->selected;
      }
      return COBALT_DRAFT_VIEW_DELETE;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      view->delete_armed = false;
      if (index) {
         *index = view->selected;
      }
      return COBALT_DRAFT_VIEW_RESUME;
   }
   return COBALT_DRAFT_VIEW_STAY;
}

void
cobalt_draft_view_draw(const cobalt_draft_view *view, const cobalt_drafts *drafts,
                       cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = cobalt_content_top(r);
   const int width = m->width - 2 * m->pad_edge;
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   cobalt_draw_page_title(r, "Drafts",
                          drafts->count == 0 ? "Nothing saved" : NULL);

   if (drafts->count == 0) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               "Nothing saved. In a new post or reply, choose "
                               "Save draft to keep it here until you finish it.",
                               m->pad_edge, top + m->pad_tile, width, 3,
                               COBALT_COLOUR_TEXT);
      return;
   }

   int y = top;
   for (unsigned row = 0; row < COBALT_DRAFT_ROWS; row++) {
      const unsigned idx = view->scroll + row;
      const cobalt_draft *draft;
      const int row_h = 2 * body_h + caption_h + 3 * m->pad_tile / 2;
      SDL_Rect rect;
      char context[COBALT_POST_NAME_MAX + 16];
      bool focused;

      if (idx >= drafts->count) {
         break;
      }
      draft = cobalt_drafts_get(drafts, idx);
      focused = (idx == view->selected);
      rect.x = m->pad_edge;
      rect.y = y;
      rect.w = width;
      rect.h = row_h;
      cobalt_draw_tile(r, &rect, focused ? 1.0f : 0.0f);

      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, draft->text,
                               rect.x + m->pad_tile, rect.y + m->pad_tile / 2,
                               width - 2 * m->pad_tile, 2, COBALT_COLOUR_TEXT);

      if (draft->kind == COBALT_DRAFT_REPLY && draft->reply_to[0]) {
         snprintf(context, sizeof(context), "Reply to %s", draft->reply_to);
      } else if (draft->kind == COBALT_DRAFT_QUOTE && draft->quote_handle[0]) {
         snprintf(context, sizeof(context), "Quote of %s", draft->quote_handle);
      } else {
         snprintf(context, sizeof(context), "%s", draft_context(draft));
      }
      cobalt_draw_text(r, COBALT_FONT_CAPTION, context,
                       rect.x + m->pad_tile,
                       rect.y + rect.h - m->pad_tile / 2 - caption_h,
                       COBALT_COLOUR_TEXT_DIM);
      y += row_h + m->gap;
   }

   if (view->delete_armed) {
      cobalt_draw_hints(r, "X again: delete this draft    B: cancel");
   } else {
      cobalt_draw_hints(r, "A: continue   X: delete   Up/Down: choose");
   }
}
