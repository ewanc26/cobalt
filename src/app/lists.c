#include "app/lists.h"
#include "atproto/session.h"
#include "ui/postcard.h"   /* cobalt_avatar_draw */
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_lists_view_init(cobalt_lists_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   cobalt_listnav_init(&view->nav);
}

void
cobalt_lists_view_open(cobalt_lists_view *view)
{
   if (!view) {
      return;
   }
   view->browsing_members = false;
   view->open_uri[0] = '\0';
   view->open_name[0] = '\0';
   view->open_description[0] = '\0';
   view->open_purpose[0] = '\0';
   view->editing = false;
   view->creating = false;
   view->keyboard_active = false;
   view->add_active = false;
   view->saving = false;
   view->remove_confirm = -1;
   view->status[0] = '\0';
   view->status_error = false;
   cobalt_listnav_rewind(&view->nav);

   if (cobalt_session_lists()->count == 0) {
      cobalt_session_begin_lists(false);
   }
}

bool
cobalt_lists_view_edit_valid(const char *name, const char *description,
                             const char *purpose)
{
   return cobalt_list_edit_valid(name, description, purpose);
}

static void
set_status(cobalt_lists_view *view, bool error, const char *text)
{
   snprintf(view->status, sizeof(view->status), "%s", text ? text : "");
   view->status_error = error;
}

void
cobalt_lists_view_result(cobalt_lists_view *view, cobalt_job_kind kind,
                         bool ok, const char *message)
{
   if (!view) {
      return;
   }
   view->saving = false;
   set_status(view, !ok, message && message[0] ? message
                                               : (ok ? "Done." : "That did not work."));
   if (!ok) {
      return;
   }

   switch (kind) {
      case COBALT_JOB_LIST_CREATE:
      case COBALT_JOB_LIST_UPDATE:
         view->editing = false;
         view->keyboard_active = false;
         view->creating = false;
         cobalt_listnav_rewind(&view->nav);
         cobalt_session_begin_lists(false);
         break;
      case COBALT_JOB_LIST_ADD_MEMBER:
      case COBALT_JOB_LIST_REMOVE_MEMBER:
         view->remove_confirm = -1;
         if (view->open_uri[0]) {
            cobalt_session_begin_list_members(view->open_uri, false);
         }
         break;
      default:
         break;
   }
}

static void
begin_edit(cobalt_lists_view *view, const cobalt_list_summary *list)
{
   view->editing = true;
   view->creating = list == NULL;
   view->edit_field = 0;
   view->keyboard_active = false;
   view->add_active = false;
   view->status[0] = '\0';
   view->status_error = false;
   view->open_uri[0] = '\0';
   if (list) {
      snprintf(view->open_uri, sizeof(view->open_uri), "%s", list->uri);
      snprintf(view->edit_name, sizeof(view->edit_name), "%s", list->name);
      snprintf(view->edit_description, sizeof(view->edit_description), "%s",
               list->description);
      snprintf(view->edit_purpose, sizeof(view->edit_purpose), "%s",
               list->purpose[0] ? list->purpose : COBALT_LIST_PURPOSE_CURATE);
   } else {
      view->edit_name[0] = '\0';
      view->edit_description[0] = '\0';
      snprintf(view->edit_purpose, sizeof(view->edit_purpose), "%s",
               COBALT_LIST_PURPOSE_CURATE);
   }
}

static void
save_edit(cobalt_lists_view *view)
{
   if (!cobalt_lists_view_edit_valid(view->edit_name, view->edit_description,
                                     view->edit_purpose)) {
      set_status(view, true, "A list needs a name.");
      return;
   }
   view->saving = true;
   view->keyboard_active = false;
   if (view->creating) {
      if (!cobalt_session_begin_list_create(view->edit_name,
                                            view->edit_description,
                                            view->edit_purpose)) {
         view->saving = false;
         set_status(view, true, "Could not start creating the list.");
      }
   } else if (!cobalt_session_begin_list_update(view->open_uri, view->edit_name,
                                                view->edit_description,
                                                view->edit_purpose)) {
      view->saving = false;
      set_status(view, true, "Could not start saving the list.");
   }
}

/* --- update --- */

static cobalt_lists_view_action
update_editor(cobalt_lists_view *view, const cobalt_input *in)
{
   if (view->saving) {
      return COBALT_LISTS_VIEW_STAY;
   }

   if (view->keyboard_active) {
      switch (cobalt_keyboard_update(&view->kb, in)) {
         case COBALT_KB_ACCEPTED:
            view->keyboard_active = false;
            break;
         case COBALT_KB_CANCELLED:
            view->keyboard_active = false;
            break;
         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_LISTS_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      view->editing = false;
      view->creating = false;
      view->status[0] = '\0';
      return COBALT_LISTS_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) ||
       cobalt_input_pressed(in, COBALT_BTN_UP)) {
      view->edit_field = (view->edit_field + 1) % 3;
      return COBALT_LISTS_VIEW_STAY;
   }

   if (view->edit_field == 2 &&
       (cobalt_input_pressed(in, COBALT_BTN_LEFT) ||
        cobalt_input_pressed(in, COBALT_BTN_RIGHT))) {
      snprintf(view->edit_purpose, sizeof(view->edit_purpose), "%s",
               cobalt_list_purpose_next(view->edit_purpose));
      return COBALT_LISTS_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      if (view->edit_field == 2) {
         snprintf(view->edit_purpose, sizeof(view->edit_purpose), "%s",
                  cobalt_list_purpose_next(view->edit_purpose));
      } else {
         view->keyboard_active = true;
         if (view->edit_field == 0) {
            cobalt_keyboard_open(&view->kb, view->edit_name,
                                 sizeof(view->edit_name), false, false);
         } else {
            cobalt_keyboard_open(&view->kb, view->edit_description,
                                 sizeof(view->edit_description), false, true);
         }
      }
      return COBALT_LISTS_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
      save_edit(view);
   }
   return COBALT_LISTS_VIEW_STAY;
}

static cobalt_lists_view_action
update_list_of_lists(cobalt_lists_view *view, const cobalt_input *in)
{
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_LISTS_VIEW_BACK;
   }

   const cobalt_list_summary_list *lists = cobalt_session_lists();
   const bool busy = cobalt_session_busy();

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
      begin_edit(view, NULL);
      return COBALT_LISTS_VIEW_STAY;
   }

   if (lists->count == 0) {
      return COBALT_LISTS_VIEW_STAY;
   }

   const int tapped_index = cobalt_listnav_move(&view->nav, in, lists->count);

   cobalt_listnav_follow(&view->nav);

   if (!busy && cobalt_list_summary_list_can_page(lists) &&
       view->nav.selected >= lists->count - 1) {
      cobalt_session_begin_lists(true);
   }

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
      begin_edit(view, &lists->lists[view->nav.selected]);
      return COBALT_LISTS_VIEW_STAY;
   }

   bool open = tapped_index >= 0 || cobalt_input_pressed(in, COBALT_BTN_CONFIRM);
   if (!busy && open && view->nav.selected < lists->count) {
      const cobalt_list_summary *list = &lists->lists[view->nav.selected];
      if (list->uri[0]) {
         COBALT_LOGI("lists: opening %s", list->name);
         snprintf(view->open_uri, sizeof(view->open_uri), "%s", list->uri);
         snprintf(view->open_name, sizeof(view->open_name), "%s", list->name);
         snprintf(view->open_description, sizeof(view->open_description), "%s",
                  list->description);
         snprintf(view->open_purpose, sizeof(view->open_purpose), "%s",
                  list->purpose);
         view->browsing_members = true;
         view->status[0] = '\0';
         view->status_error = false;
         view->remove_confirm = -1;
         cobalt_listnav_rewind(&view->nav);
         cobalt_session_begin_list_members(list->uri, false);
      }
   }

   return COBALT_LISTS_VIEW_STAY;
}

static cobalt_lists_view_action
update_members(cobalt_lists_view *view, const cobalt_input *in)
{
   if (view->saving) {
      return COBALT_LISTS_VIEW_STAY;
   }

   if (view->add_active) {
      switch (cobalt_keyboard_update(&view->kb, in)) {
         case COBALT_KB_ACCEPTED:
            view->add_active = false;
            if (view->add_subject[0]) {
               view->saving = true;
               if (!cobalt_session_begin_list_add_member(view->open_uri,
                                                         view->add_subject)) {
                  view->saving = false;
                  set_status(view, true, "Could not start adding that account.");
               } else {
                  set_status(view, false, "Adding account...");
               }
            }
            break;
         case COBALT_KB_CANCELLED:
            view->add_active = false;
            break;
         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_LISTS_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      if (view->remove_confirm >= 0) {
         view->remove_confirm = -1;
         view->status[0] = '\0';
         return COBALT_LISTS_VIEW_STAY;
      }
      view->browsing_members = false;
      view->remove_confirm = -1;
      view->status[0] = '\0';
      cobalt_listnav_rewind(&view->nav);
      return COBALT_LISTS_VIEW_STAY;
   }

   const cobalt_actor_list *members = cobalt_session_list_members();
   const bool busy = cobalt_session_busy();

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
      view->add_active = true;
      view->add_subject[0] = '\0';
      view->remove_confirm = -1;
      view->status[0] = '\0';
      view->status_error = false;
      cobalt_keyboard_open(&view->kb, view->add_subject,
                           sizeof(view->add_subject), false, false);
      return COBALT_LISTS_VIEW_STAY;
   }

   if (members->count == 0) {
      return COBALT_LISTS_VIEW_STAY;
   }

   const bool tapped = cobalt_listnav_move(&view->nav, in, members->count) >= 0;

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
      if (view->remove_confirm == view->nav.selected) {
         const char *item_uri =
            cobalt_session_list_member_uri(view->nav.selected);
         view->remove_confirm = -1;
         if (item_uri[0]) {
            view->saving = true;
            if (!cobalt_session_begin_list_remove_member(item_uri)) {
               view->saving = false;
               set_status(view, true, "Could not start removing that account.");
            } else {
               set_status(view, false, "Removing account...");
            }
         } else {
            set_status(view, true, "That member has no removable record.");
         }
      } else {
         view->remove_confirm = view->nav.selected;
         set_status(view, true, "Press X again to remove this account.");
      }
      return COBALT_LISTS_VIEW_STAY;
   }

   if (tapped || cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      const cobalt_actor *actor = &members->actors[view->nav.selected];
      if (actor->did[0]) {
         cobalt_session_begin_profile(actor->did);
         return COBALT_LISTS_VIEW_OPEN_PROFILE;
      }
   }

   cobalt_listnav_follow(&view->nav);

   if (!busy && cobalt_actor_list_can_page(members) &&
       view->nav.selected >= members->count - 1) {
      cobalt_session_begin_list_members(view->open_uri, true);
   }

   return COBALT_LISTS_VIEW_STAY;
}

cobalt_lists_view_action
cobalt_lists_view_update(cobalt_lists_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_LISTS_VIEW_STAY;
   }
   if (view->editing) {
      return update_editor(view, in);
   }
   if (view->saving) {
      return COBALT_LISTS_VIEW_STAY;
   }
   return view->browsing_members ? update_members(view, in)
                                 : update_list_of_lists(view, in);
}

/* --- drawing --- */

#define ROW_AVATAR_SIDE(m) ((m)->font_body * 2)
#define ROW_AVATAR_GAP(m)  ((m)->pad_tile / 2)

static int
row_height(const cobalt_metrics *m)
{
   const int h = m->pad_tile + ROW_AVATAR_SIDE(m) + m->pad_tile;
   return h > 0 ? h : m->font_body * 3;
}

static void
draw_actor_row(cobalt_render *r, const cobalt_actor *actor,
               const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;

   cobalt_avatar_draw(r, actor->avatar, actor->display_name, actor->handle,
                      left, y, ROW_AVATAR_SIDE(m));
   const int text_left = left + ROW_AVATAR_SIDE(m) + ROW_AVATAR_GAP(m);
   if (right - text_left <= 0) {
      return;
   }

   const int name_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int handle_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int side = ROW_AVATAR_SIDE(m);
   const int text_y = y + (side - (name_h + handle_h)) / 2;

   cobalt_draw_text(r, COBALT_FONT_BODY, actor->display_name, text_left,
                    text_y, COBALT_COLOUR_TEXT);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, actor->handle, text_left,
                    text_y + name_h, COBALT_COLOUR_TEXT_DIM);
}

static void
draw_list_row(cobalt_render *r, const cobalt_list_summary *list,
              const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;

   cobalt_avatar_draw(r, list->avatar, list->name, list->name, left, y,
                      ROW_AVATAR_SIDE(m));
   const int text_left = left + ROW_AVATAR_SIDE(m) + ROW_AVATAR_GAP(m);
   if (right - text_left <= 0) {
      return;
   }

   const int name_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int side = ROW_AVATAR_SIDE(m);
   const bool has_desc = list->description[0] != '\0';
   const int desc_h = has_desc ? cobalt_font_line_height(r, COBALT_FONT_CAPTION) : 0;
   const int text_y = y + (side - (name_h + desc_h)) / 2;

   cobalt_draw_text(r, COBALT_FONT_BODY, list->name, text_left, text_y,
                    COBALT_COLOUR_TEXT);
   if (has_desc) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, list->description, text_left,
                       text_y + name_h, COBALT_COLOUR_TEXT_DIM);
   }
}

static void
draw_hint_line(cobalt_lists_view *view, cobalt_render *r, const char *controls)
{
   char hint[COBALT_LIST_STATUS_MAX + 64];

   if (view->status[0]) {
      snprintf(hint, sizeof(hint), "%s  %s", view->status, controls ? controls : "");
      cobalt_draw_hints(r, hint);
   } else if (controls) {
      cobalt_draw_hints(r, controls);
   }
}

static void
draw_editor(cobalt_lists_view *view, cobalt_render *r,
            cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   const int width = m->width - 2 * m->pad_edge;
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   cobalt_draw_text(r, COBALT_FONT_TITLE,
                    view->creating ? "New list" : "Edit list", m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_HEADER_TEXT);

   if (view->keyboard_active) {
      const bool purpose = view->edit_field == 2;
      const char *value = purpose ? cobalt_list_purpose_label(view->edit_purpose)
                                  : (view->edit_field == 0
                                        ? view->edit_name
                                        : view->edit_description);
      SDL_Rect field = { m->pad_edge, top, width,
                         m->pad_tile + label_h +
                            cobalt_font_line_height(r, COBALT_FONT_BODY) };
      cobalt_draw_tile(r, &field, 1.0f);
      cobalt_draw_text(r, COBALT_FONT_CAPTION,
                       purpose ? "Purpose"
                               : (view->edit_field == 0 ? "Name" : "Description"),
                       field.x + m->pad_tile, field.y + m->pad_tile,
                       COBALT_COLOUR_TEXT_DIM);
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, value,
                               field.x + m->pad_tile,
                               field.y + m->pad_tile + label_h,
                               field.w - 2 * m->pad_tile, 2,
                               COBALT_COLOUR_TEXT);

      const int kb_h = cobalt_keyboard_height(surface);
      SDL_Rect keys = { m->pad_edge, bottom - kb_h, width, kb_h };
      cobalt_keyboard_draw(&view->kb, r, surface, &keys);
      cobalt_draw_hints(r, "OK: keep   B: cancel");
      return;
   }

   const int lines = 2;
   int y = top;
   for (int i = 0; i < 3; i++) {
      const bool purpose = i == 2;
      const char *label = i == 0 ? "Name" : (i == 1 ? "Description" : "Purpose");
      const char *value = i == 0 ? view->edit_name
                                 : (i == 1 ? view->edit_description
                                           : cobalt_list_purpose_label(view->edit_purpose));
      SDL_Rect tile = { m->pad_edge, y, width,
                        m->pad_tile + label_h +
                           (purpose ? 1 : lines) *
                              cobalt_font_line_height(r, COBALT_FONT_BODY) };
      cobalt_draw_tile(r, &tile, view->edit_field == i ? 1.0f : 0.0f);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, label,
                       tile.x + m->pad_tile, tile.y + m->pad_tile,
                       COBALT_COLOUR_TEXT_DIM);
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               value[0] ? value : "(not set)",
                               tile.x + m->pad_tile, tile.y + m->pad_tile + label_h,
                               tile.w - 2 * m->pad_tile, purpose ? 1 : lines,
                               value[0] ? COBALT_COLOUR_TEXT
                                        : COBALT_COLOUR_TEXT_DIM);
      y += tile.h + m->gap / 2;
   }

   draw_hint_line(view, r, "A: edit   L/R: purpose   Y: save   B: cancel");
}

static void
draw_add_member(cobalt_lists_view *view, cobalt_render *r,
                cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   const int width = m->width - 2 * m->pad_edge;
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Add member", m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_HEADER_TEXT);

   SDL_Rect field = { m->pad_edge, top, width,
                      m->pad_tile + label_h +
                         cobalt_font_line_height(r, COBALT_FONT_BODY) };
   cobalt_draw_tile(r, &field, 1.0f);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, "Handle or DID",
                    field.x + m->pad_tile, field.y + m->pad_tile,
                    COBALT_COLOUR_TEXT_DIM);
   cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                            view->add_subject[0] ? view->add_subject
                                                 : "(type an account)",
                            field.x + m->pad_tile,
                            field.y + m->pad_tile + label_h,
                            field.w - 2 * m->pad_tile, 1,
                            view->add_subject[0] ? COBALT_COLOUR_TEXT
                                                 : COBALT_COLOUR_TEXT_DIM);

   const int kb_h = cobalt_keyboard_height(surface);
   SDL_Rect keys = { m->pad_edge, bottom - kb_h, width, kb_h };
   cobalt_keyboard_draw(&view->kb, r, surface, &keys);
   cobalt_draw_hints(r, "OK: add   B: cancel");
}

static void
draw_list_of_lists(cobalt_lists_view *view, cobalt_render *r,
                   cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_list_summary_list *lists = cobalt_session_lists();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Your lists", m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_HEADER_TEXT);

   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (lists->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..."
                                             : "You haven't made any lists yet.",
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->nav, touchable);
      draw_hint_line(view, r, "X: create   B: back");
      return;
   }

   const int h = row_height(m);
   int y = top;
   int last_fitted = view->nav.scroll;

   for (int i = view->nav.scroll; i < lists->count; i++) {
      if (y + h > bottom && i > view->nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_list_row(r, &lists->lists[i], &rect, i == view->nav.selected, m);

      cobalt_listnav_draw_add(&view->nav, touchable, &rect, i);

      last_fitted = i;
      y += h + m->gap / 2;
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);

   draw_hint_line(view, r, cobalt_session_busy()
                             ? "Working..."
                             : "X: create   Y: edit   A: open");
}

static void
draw_members(cobalt_lists_view *view, cobalt_render *r,
             cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_actor_list *members = cobalt_session_list_members();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, view->open_name, m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_HEADER_TEXT);

   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (members->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..."
                                             : "This list has no members.",
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->nav, touchable);
      draw_hint_line(view, r, "Y: add member   B: back");
      return;
   }

   const int h = row_height(m);
   int y = top;
   int last_fitted = view->nav.scroll;

   for (int i = view->nav.scroll; i < members->count; i++) {
      if (y + h > bottom && i > view->nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_actor_row(r, &members->actors[i], &rect, i == view->nav.selected, m);

      cobalt_listnav_draw_add(&view->nav, touchable, &rect, i);

      last_fitted = i;
      y += h + m->gap / 2;
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);

   draw_hint_line(view, r, cobalt_session_busy() || view->saving
                             ? "Working..."
                             : (view->remove_confirm >= 0
                                   ? "X: confirm remove   B: cancel"
                                   : "Y: add   X: remove   A: profile"));
}

void
cobalt_lists_view_draw(cobalt_lists_view *view, cobalt_render *r,
                       cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }
   if (view->editing) {
      draw_editor(view, r, surface);
   } else if (view->browsing_members && view->add_active) {
      draw_add_member(view, r, surface);
   } else if (view->browsing_members) {
      draw_members(view, r, surface);
   } else {
      draw_list_of_lists(view, r, surface);
   }
}
