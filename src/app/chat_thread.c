#include "app/chat_thread.h"
#include "atproto/session.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

/* A bubble never takes more lines than this, so one long message cannot push
 * the rest of the conversation off the screen. The wrapper cuts the text with an
 * ellipsis, as the post cards do. */
#define BUBBLE_LINES_MAX 6

void
cobalt_chat_thread_view_init(cobalt_chat_thread_view *view)
{
   if (view) {
      memset(view, 0, sizeof(*view));
      cobalt_listnav_init(&view->join_nav);
      cobalt_listnav_init(&view->member_nav);
   }
}

void
cobalt_chat_thread_view_open(cobalt_chat_thread_view *view, const char *title,
                             const char *convo_id, bool group)
{
   if (!view) {
      return;
   }
   const bool same = strcmp(view->convo_id, convo_id ? convo_id : "") == 0;

   snprintf(view->title, sizeof(view->title), "%s", title ? title : "");
   snprintf(view->convo_id, sizeof(view->convo_id), "%s", convo_id ? convo_id : "");
   view->group = group;
   view->offset = 0;
   view->replying = false;
   /* Opening a conversation shows the messages, never whatever sub-mode it was
    * left on. */
   view->mode = COBALT_CHAT_THREAD_MODE_MESSAGES;
   view->editing = false;
   view->edit_kind = COBALT_CHAT_THREAD_EDIT_NONE;
   view->group_pending = false;
   cobalt_listnav_rewind(&view->join_nav);
   cobalt_listnav_rewind(&view->member_nav);
   if (!same) {
      view->reply[0] = '\0';
   }
}

void
cobalt_chat_thread_view_sent(cobalt_chat_thread_view *view)
{
   if (view) {
      view->reply[0] = '\0';
   }
}

/* --- group join requests --- */

void
cobalt_chat_thread_decision_began(cobalt_chat_thread_view *view, bool began)
{
   if (view && began) {
      view->join_decision.pending = true;
   }
}

cobalt_chat_thread_decision_outcome
cobalt_chat_thread_decision_finished(cobalt_chat_thread_view *view, bool ok)
{
   if (!view || !view->join_decision.pending) {
      return COBALT_CHAT_THREAD_DECISION_NONE;
   }
   view->join_decision.pending = false;
   if (!ok) {
      return COBALT_CHAT_THREAD_DECISION_FAILED;
   }
   return view->join_decision.approve ? COBALT_CHAT_THREAD_DECISION_APPROVED
                                      : COBALT_CHAT_THREAD_DECISION_REJECTED;
}

/* Fill the decision from a row and report which action it is. The row is copied,
 * because the worker may remove it while the job runs. */
static cobalt_chat_thread_action
press_join_decision(cobalt_chat_thread_view *view,
                    const cobalt_chat_join_requests *requests, int index, bool approve)
{
   const cobalt_chat_join_request *row = &requests->items[index];
   snprintf(view->join_decision.did, sizeof(view->join_decision.did), "%s", row->did);
   snprintf(view->join_decision.name, sizeof(view->join_decision.name), "%s", row->name);
   view->join_decision.approve = approve;
   return approve ? COBALT_CHAT_THREAD_JOIN_APPROVE : COBALT_CHAT_THREAD_JOIN_REJECT;
}

/* --- group settings --- */

void
cobalt_chat_thread_group_began(cobalt_chat_thread_view *view, bool began)
{
   if (!view || view->mode != COBALT_CHAT_THREAD_MODE_GROUP_INFO) {
      return;
   }
   view->group_pending = began;
   if (!began && view->edit_kind != COBALT_CHAT_THREAD_EDIT_NONE) {
      /* The form was complete but no job started; keep the text editable. */
      view->editing = true;
      cobalt_keyboard_open(&view->edit_kb, view->edit_text, sizeof(view->edit_text),
                           false, false);
   }
}

bool
cobalt_chat_thread_group_finished(cobalt_chat_thread_view *view, bool ok)
{
   if (!view || !view->group_pending) {
      return false;
   }
   view->group_pending = false;
   if (ok) {
      view->editing = false;
      view->edit_kind = COBALT_CHAT_THREAD_EDIT_NONE;
      view->edit_text[0] = '\0';
   }
   return true;
}

/* Open the editor on the group's name, or on a blank member field. */
static void
open_group_edit(cobalt_chat_thread_view *view, cobalt_chat_thread_edit kind,
                const char *initial)
{
   view->edit_kind = kind;
   snprintf(view->edit_text, sizeof(view->edit_text), "%s", initial ? initial : "");
   view->editing = true;
   cobalt_keyboard_open(&view->edit_kb, view->edit_text, sizeof(view->edit_text),
                        false, false);
}

static cobalt_chat_thread_action
update_group_info(cobalt_chat_thread_view *view, const cobalt_input *in)
{
   const cobalt_chat_group *group = cobalt_session_chat_group();
   const bool working = cobalt_session_busy() || view->group_pending;

   if (view->editing) {
      switch (cobalt_keyboard_update(&view->edit_kb, in)) {
         case COBALT_KB_ACCEPTED:
            if (view->edit_text[0] == '\0') {
               return COBALT_CHAT_THREAD_STAY;
            }
            view->editing = false;
            return view->edit_kind == COBALT_CHAT_THREAD_EDIT_NAME
                      ? COBALT_CHAT_THREAD_GROUP_RENAME
                      : COBALT_CHAT_THREAD_GROUP_ADD;
         case COBALT_KB_CANCELLED:
            view->editing = false;
            view->edit_kind = COBALT_CHAT_THREAD_EDIT_NONE;
            view->edit_text[0] = '\0';
            break;
         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_CHAT_THREAD_STAY;
   }

   /* B returns to the conversation. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      view->mode = COBALT_CHAT_THREAD_MODE_MESSAGES;
      return COBALT_CHAT_THREAD_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !working) {
      COBALT_LOGI("chat thread: group info refresh requested");
      cobalt_session_begin_chat_group_info(view->convo_id);
      return COBALT_CHAT_THREAD_STAY;
   }

   if (working || !group->loaded) {
      return COBALT_CHAT_THREAD_STAY;
   }

   cobalt_listnav_move(&view->member_nav, in, group->member_count);
   cobalt_listnav_follow(&view->member_nav);

   const int sel = (int) view->member_nav.selected;
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      open_group_edit(view, COBALT_CHAT_THREAD_EDIT_NAME, group->name);
      return COBALT_CHAT_THREAD_STAY;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
      open_group_edit(view, COBALT_CHAT_THREAD_EDIT_MEMBERS, "");
      return COBALT_CHAT_THREAD_STAY;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_ALT_X) && sel < group->member_count &&
       !cobalt_chat_is_self(cobalt_session_did(), group->members[sel].did)) {
      return COBALT_CHAT_THREAD_GROUP_REMOVE;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_SHOULDER_R)) {
      return COBALT_CHAT_THREAD_JOIN_LINK;
   }

   return COBALT_CHAT_THREAD_STAY;
}

/* --- input --- */

static cobalt_chat_thread_action
update_join_requests(cobalt_chat_thread_view *view, const cobalt_input *in)
{
   const cobalt_chat_join_requests *requests = cobalt_session_chat_join_requests();
   const bool working = cobalt_session_busy() || view->join_decision.pending;

   /* B returns to the conversation, not out of it. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      view->mode = COBALT_CHAT_THREAD_MODE_MESSAGES;
      return COBALT_CHAT_THREAD_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !working) {
      COBALT_LOGI("chat thread: join requests refresh requested");
      if (cobalt_session_begin_chat_join_requests(view->convo_id, false)) {
         cobalt_listnav_rewind(&view->join_nav);
      }
      return COBALT_CHAT_THREAD_STAY;
   }

   if (requests->count == 0) {
      return COBALT_CHAT_THREAD_STAY;
   }

   cobalt_listnav_move(&view->join_nav, in, requests->count);
   cobalt_listnav_follow(&view->join_nav);

   const int sel = (int) view->join_nav.selected;
   if (!working && sel < requests->count) {
      if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
         return press_join_decision(view, requests, sel, true);
      }
      if (cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
         return press_join_decision(view, requests, sel, false);
      }
   }

   if (!working && cobalt_chat_join_requests_can_page(requests) &&
       sel >= requests->count - 1) {
      cobalt_session_begin_chat_join_requests(view->convo_id, true);
   }

   return COBALT_CHAT_THREAD_STAY;
}

cobalt_chat_thread_action
cobalt_chat_thread_view_update(cobalt_chat_thread_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_CHAT_THREAD_STAY;
   }

   /* While the keyboard is up it owns the input; B closes it, as it does on the
    * other screens that type. */
   if (view->replying) {
      switch (cobalt_keyboard_update(&view->kb, in)) {
         case COBALT_KB_ACCEPTED:
         case COBALT_KB_CANCELLED:
            view->replying = false;
            break;
         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_CHAT_THREAD_STAY;
   }

   if (view->mode == COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS) {
      return update_join_requests(view, in);
   }

   if (view->mode == COBALT_CHAT_THREAD_MODE_GROUP_INFO) {
      return update_group_info(view, in);
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_CHAT_THREAD_BACK;
   }

   const cobalt_chat_thread *thread = cobalt_session_chat_thread();
   const bool busy = cobalt_session_busy();

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM) && !busy) {
      view->replying = true;
      cobalt_keyboard_open(&view->kb, view->reply, sizeof(view->reply), false, false);
      return COBALT_CHAT_THREAD_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_ALT_Y) && !busy) {
      if (cobalt_chat_reply_valid(view->reply)) {
         COBALT_LOGI("chat thread: sending a reply");
         /* A refused send keeps the draft; the person can press Y again. */
         cobalt_session_begin_chat_send(view->convo_id, view->reply);
      }
      return COBALT_CHAT_THREAD_STAY;
   }

   /* X opens a group's join requests. A one-to-one conversation has none to
    * show, so its X stays inert rather than offering a list that can only be
    * refused. */
   if (cobalt_input_pressed(in, COBALT_BTN_ALT_X) && !busy && view->group) {
      COBALT_LOGI("chat thread: opening join requests");
      view->mode = COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS;
      cobalt_listnav_rewind(&view->join_nav);
      cobalt_session_begin_chat_join_requests(view->convo_id, false);
      return COBALT_CHAT_THREAD_STAY;
   }

   /* R opens the group's settings: rename, members and the join link. The
    * member list is only meaningful for a group, so a one-to-one conversation
    * leaves it inert. */
   if (cobalt_input_pressed(in, COBALT_BTN_SHOULDER_R) && !busy && view->group) {
      COBALT_LOGI("chat thread: opening group settings");
      view->mode = COBALT_CHAT_THREAD_MODE_GROUP_INFO;
      view->editing = false;
      view->edit_kind = COBALT_CHAT_THREAD_EDIT_NONE;
      cobalt_listnav_rewind(&view->member_nav);
      cobalt_session_begin_chat_group_info(view->convo_id);
      return COBALT_CHAT_THREAD_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !busy) {
      COBALT_LOGI("chat thread: refresh requested");
      cobalt_session_begin_chat_messages(view->convo_id);
      view->offset = 0;
      return COBALT_CHAT_THREAD_STAY;
   }

   /* Up goes back in time, one message at a time, until the oldest held. */
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && thread->count > 0 &&
       view->offset < thread->count - 1) {
      view->offset++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) && view->offset > 0) {
      view->offset--;
   }

   /* Touch: dragging the conversation does what Up and Down do. `shown` is the
    * bubble at the bottom of the screen counted from the oldest, which is the
    * direction a list drag already means, so a drag up goes forward in time. */
   if (thread->count > 0) {
      int shown = thread->count - 1 - view->offset;
      cobalt_input_drag_list(in, &shown, thread->count, NULL, 0);
      view->offset = thread->count - 1 - shown;
      if (view->offset < 0) {
         view->offset = 0;
      }
   }

   return COBALT_CHAT_THREAD_STAY;
}

/* --- drawing --- */

static int
bubble_lines(cobalt_render *r, const cobalt_chat_message *msg, int inner_w)
{
   int lines = cobalt_text_wrapped_lines(r, COBALT_FONT_BODY, msg->text, inner_w);
   if (lines < 1) {
      lines = 1;
   }
   /* As a remote, the GamePad is the index of the conversation the TV is
    * showing, so each message is one line here. */
   if (cobalt_render_remote(r)) {
      return 1;
   }
   return lines > BUBBLE_LINES_MAX ? BUBBLE_LINES_MAX : lines;
}

/*
 * The box the draft is typed into while the keyboard is up: one line tall,
 * sitting directly above the keyboard. The bubbles stop above it, so the draft
 * has a field of its own instead of a caret loose on the background.
 */
static SDL_Rect
draft_field(const cobalt_metrics *m, cobalt_surface_id surface, int body_h)
{
   const int h = body_h + m->pad_tile;
   const SDL_Rect box = { m->pad_edge,
                          m->height - cobalt_keyboard_height(surface) - h - m->gap,
                          m->width - 2 * m->pad_edge, h };
   return box;
}

static void
draw_bubbles(cobalt_chat_thread_view *view, cobalt_render *r, const cobalt_metrics *m,
              cobalt_surface_id surface)
{
   const cobalt_chat_thread *thread = cobalt_session_chat_thread();
   const int top = cobalt_content_top(r);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   const bool reader = cobalt_render_reader(r);
   /* The draft field and keyboard take the bottom while the person is writing;
    * otherwise the footer hints do. A reader-mode TV has no draft field — the
    * reply panel takes the keyboard's place, and the bubbles run to it. */
   const SDL_Rect draft = draft_field(m, surface, body_h);
   const SDL_Rect keys = { 0, m->height - cobalt_keyboard_height(surface), m->width,
                           cobalt_keyboard_height(surface) };
   /* The bubbles stop a gap above the footer's top edge, which is drawn at
    * height - pad_edge/2 - one pill tall (render.c). */
   const int footer_top = m->height - m->pad_edge / 2 - cobalt_pill_height(r);
   const int typing_top = reader ? keys.y : draft.y;
   const int bottom = view->replying ? typing_top - m->gap : footer_top - m->gap;

   if (thread->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge, m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..." : "No messages to show.",
                       card.x + m->pad_tile, card.y + m->pad_tile, COBALT_COLOUR_TEXT);
      return;
   }

   /* Draw from the newest held message upwards, skipping `offset` of them,
    * until the next bubble would cross the top. */
   const int newest = thread->count - 1 - view->offset;
   const int content_w = m->width - 2 * m->pad_edge;
   const int bubble_w = content_w * 3 / 4;
   /* While typing, the keyboard takes the GamePad's bottom half and there is
    * room for one line of the conversation at most; the bubbles give up their
    * padding and their age so that line stays on screen instead of an empty
    * panel behind the field. */
   const bool compact = view->replying && !cobalt_render_reader(r);
   const int pad = compact ? m->pad_tile / 2 : m->pad_tile;
   const int age_h = compact ? 0 : caption_h;
   const int inner_w = bubble_w - 2 * pad;
   int y = bottom;

   for (int i = newest; i >= 0; i--) {
      const cobalt_chat_message *msg = &thread->items[i];
      int lines = bubble_lines(r, msg, inner_w);
      int h = pad + age_h + lines * body_h + pad;

      if (y - h < top) {
         /* Only the newest message is worth cutting to fit: with the keyboard up
          * there may be room for one line, and that line should still show. */
         if (i != newest) {
            break;
         }
         const int fit = (y - top - 2 * pad - age_h) / body_h;
         if (fit < 1) {
            break;
         }
         lines = fit;
         h = pad + age_h + lines * body_h + pad;
      }
      y -= h;

      /* Alignment says who sent it; the focus highlight is left for selection. */
      SDL_Rect rect = { msg->mine ? m->width - m->pad_edge - bubble_w : m->pad_edge,
                        y, bubble_w, h };
      cobalt_draw_tile(r, &rect, 0.0f);

      const int text_x = rect.x + pad;
      if (msg->age[0] && age_h > 0) {
         int age_w = 0;
         cobalt_text_size(r, COBALT_FONT_CAPTION, msg->age, &age_w, NULL);
         cobalt_draw_text(r, COBALT_FONT_CAPTION, msg->age,
                          rect.x + rect.w - pad - age_w, rect.y + pad,
                          COBALT_COLOUR_TEXT_DIM);
      }
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, msg->text, text_x,
                               rect.y + pad + age_h, inner_w, lines,
                               COBALT_COLOUR_TEXT);

      y -= m->gap;
   }
}

/* One join request: the requester, and how long ago they asked. No avatar or
 * last message, because a request is not a conversation. */
static void
draw_join_row(cobalt_render *r, const cobalt_chat_join_request *item, const SDL_Rect *rect,
              bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;

   const int name_w = cobalt_draw_text(r, COBALT_FONT_BODY, item->name, left, y,
                                       COBALT_COLOUR_TEXT);

   if (item->handle[0]) {
      int tag_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, item->handle, &tag_w, NULL);
      const int tag_x = left + name_w + m->pad_tile / 2;
      if (tag_x + tag_w < right) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, item->handle, tag_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }

   if (item->age[0]) {
      int age_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, item->age, &age_w, NULL);
      const int age_x = right - age_w;
      if (age_x > left) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, item->age, age_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }
}

static void
draw_join_requests(cobalt_chat_thread_view *view, cobalt_render *r,
                   cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_chat_join_requests *requests = cobalt_session_chat_join_requests();
   const bool busy = cobalt_session_busy();

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Join requests", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_HEADER_TEXT);
   {
      char subtitle[96];
      if (busy) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else {
         cobalt_chat_requests_count_label(requests->count, subtitle, sizeof(subtitle));
      }
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_listnav_draw_begin(&view->join_nav, touchable);

   if (requests->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge, m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY, busy ? "Loading..." : "No join requests.",
                       card.x + m->pad_tile, card.y + m->pad_tile, COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->join_nav, touchable);
      cobalt_draw_hints(r, busy ? "Working..." : "+: refresh");
      return;
   }

   const int row_h = m->pad_tile + cobalt_font_line_height(r, COBALT_FONT_BODY) +
                     m->pad_tile;
   int y = top;
   int last_fitted = view->join_nav.scroll;

   for (int i = view->join_nav.scroll; i < requests->count; i++) {
      const cobalt_chat_join_request *item = &requests->items[i];

      if (y + row_h > bottom && i > view->join_nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, row_h };
      draw_join_row(r, item, &rect, i == view->join_nav.selected, m);
      cobalt_listnav_draw_add(&view->join_nav, touchable, &rect, i);

      last_fitted = i;
      y += row_h + m->gap / 2;
   }

   cobalt_listnav_draw_end(&view->join_nav, touchable, last_fitted);

   cobalt_draw_hints(r, busy ? "Working..."
                             : "A: approve   X: reject   +: refresh");
}

static void
draw_group_edit(cobalt_chat_thread_view *view, cobalt_render *r,
                 cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = cobalt_content_top(r);
   const bool name_edit = (view->edit_kind == COBALT_CHAT_THREAD_EDIT_NAME);
   const char *label = name_edit ? "Group name" : "Add members (handles or DIDs)";
   const char *placeholder = name_edit ? "Group name..." : "@handle, did:plc:...";

   if (cobalt_render_reader(r)) {
      const SDL_Rect panel = cobalt_typing_area(r);
      cobalt_draw_typing_panel(r, &panel, "Editing a group on the GamePad",
                               view->edit_text);
      return;
   }

   cobalt_draw_text(r, COBALT_FONT_CAPTION, label, m->pad_edge, top,
                    COBALT_COLOUR_TEXT_DIM);
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   SDL_Rect box = { m->pad_edge, top + label_h + m->line_gap,
                    m->width - 2 * m->pad_edge,
                    m->pad_tile * 2 + cobalt_font_line_height(r, COBALT_FONT_BODY) };
   cobalt_draw_tile(r, &box, 1.0f);

   char display[COBALT_CHAT_GROUP_MEMBERS_TEXT_MAX + 4];
   cobalt_keyboard_display_text(&view->edit_kb, display, sizeof(display));
   const bool empty = view->edit_text[0] == '\0';
   cobalt_draw_text(r, COBALT_FONT_BODY, empty ? placeholder : display,
                    box.x + m->pad_tile, box.y + m->pad_tile,
                    empty ? COBALT_COLOUR_TEXT_DIM : COBALT_COLOUR_TEXT);

   SDL_Rect keys = { m->pad_edge, box.y + box.h + m->gap,
                     m->width - 2 * m->pad_edge,
                     cobalt_keyboard_height(surface) };
   cobalt_keyboard_draw(&view->edit_kb, r, surface, &keys);
}

static void
draw_member_row(cobalt_render *r, const cobalt_chat_member *member, const SDL_Rect *rect,
                 bool focused, const cobalt_metrics *m, bool self)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;

   const int name_w = cobalt_draw_text(r, COBALT_FONT_BODY, member->name, left, y,
                                       COBALT_COLOUR_TEXT);
   if (self) {
      int you_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, "You", &you_w, NULL);
      const int you_x = right - you_w;
      if (you_x > left + name_w) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, "You", you_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }

   if (member->handle[0]) {
      int handle_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, member->handle, &handle_w, NULL);
      const int handle_x = left + name_w + m->pad_tile / 2;
      if (handle_x + handle_w < right) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, member->handle, handle_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }
}

static void
draw_group_info(cobalt_chat_thread_view *view, cobalt_render *r,
                 cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_chat_group *group = cobalt_session_chat_group();
   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Group", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_HEADER_TEXT);
   {
      const char *subtitle = group->loaded ? group->name : "Loading...";
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle[0] ? subtitle : view->title,
                       m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   if (view->editing) {
      draw_group_edit(view, r, surface);
      return;
   }

   if (!group->loaded) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge, m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..." : "Group details are unavailable.",
                       card.x + m->pad_tile, card.y + m->pad_tile, COBALT_COLOUR_TEXT);
      cobalt_draw_hints(r, cobalt_session_busy() ? "Working..." : "+: refresh");
      return;
   }

   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                     m->pad_tile * 2 + body_h + caption_h * 2 + m->line_gap };
   cobalt_draw_tile(r, &card, 0.0f);

   char line[128];
   snprintf(line, sizeof(line), "%d member%s",
            group->total_members > 0 ? group->total_members : group->member_count,
            (group->total_members == 1 || (group->total_members == 0 &&
                                           group->member_count == 1)) ? "" : "s");
   cobalt_draw_text(r, COBALT_FONT_BODY, line, card.x + m->pad_tile,
                    card.y + m->pad_tile, COBALT_COLOUR_TEXT);

   const char *link = "No join link";
   if (group->join_link_url[0]) {
      link = group->join_link_url;
   } else if (group->join_link_code[0]) {
      link = group->join_link_code;
   }
   const int link_y = card.y + m->pad_tile + body_h + m->line_gap;
   cobalt_draw_text(r, COBALT_FONT_CAPTION, link, card.x + m->pad_tile, link_y,
                    COBALT_COLOUR_TEXT_DIM);
   if (group->join_link_code[0]) {
      const char *state = group->join_link_enabled ? "enabled" : "disabled";
      int state_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, state, &state_w, NULL);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, state,
                       card.x + card.w - m->pad_tile - state_w, link_y,
                       COBALT_COLOUR_TEXT_DIM);
   }

   int y = card.y + card.h + m->gap;
   int last_fitted = view->member_nav.scroll;

   cobalt_listnav_draw_begin(&view->member_nav, touchable);

   if (group->member_count == 0) {
      SDL_Rect empty = { m->pad_edge, y, m->width - 2 * m->pad_edge, m->font_body * 3 };
      cobalt_draw_tile(r, &empty, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY, "No members to show.",
                       empty.x + m->pad_tile, empty.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->member_nav, touchable);
   } else {
      const int row_h = m->pad_tile + body_h + m->pad_tile;
      for (int i = view->member_nav.scroll; i < group->member_count; i++) {
         const cobalt_chat_member *member = &group->members[i];
         if (y + row_h > bottom && i > view->member_nav.scroll) {
            break;
         }
         SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, row_h };
         draw_member_row(r, member, &rect, i == view->member_nav.selected, m,
                         cobalt_chat_is_self(cobalt_session_did(), member->did));
         cobalt_listnav_draw_add(&view->member_nav, touchable, &rect, i);
         last_fitted = i;
         y += row_h + m->gap / 2;
      }
      cobalt_listnav_draw_end(&view->member_nav, touchable, last_fitted);
   }

   if (cobalt_session_busy() || view->group_pending) {
      cobalt_draw_hints(r, "Working...");
      return;
   }

   char hints[192];
   const char *link_hint = !group->join_link_code[0]
                              ? "R: create link"
                              : group->join_link_enabled ? "R: disable link"
                                                         : "R: enable link";
   snprintf(hints, sizeof(hints),
            "A: rename   Y: add   X: remove   %s   +: refresh", link_hint);
   cobalt_draw_hints(r, hints);
}

void
cobalt_chat_thread_view_draw(cobalt_chat_thread_view *view, cobalt_render *r,
                             cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);

   if (view->mode == COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS) {
      draw_join_requests(view, r, surface);
      return;
   }

   if (view->mode == COBALT_CHAT_THREAD_MODE_GROUP_INFO) {
      draw_group_info(view, r, surface);
      return;
   }

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Messages", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_HEADER_TEXT);
   {
      char subtitle[128];
      snprintf(subtitle, sizeof(subtitle), "%s",
               cobalt_session_busy() ? "Loading..." : view->title);
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int kb_h = cobalt_keyboard_height(surface);

   draw_bubbles(view, r, m, surface);

   if (view->replying) {
      char shown[COBALT_CHAT_REPLY_MAX];
      cobalt_keyboard_display_text(&view->kb, shown, sizeof(shown));

      const SDL_Rect keys = { 0, m->height - kb_h, m->width, kb_h };
      if (cobalt_render_reader(r)) {
         /* Split play: the keys and the draft field are on the GamePad, so the
          * TV shows the reply being written in the space they would take. Two
          * copies of the same draft on one screen would only be noise. */
         cobalt_draw_typing_panel(r, &keys, "Replying on the GamePad", shown);
         return;
      }

      const SDL_Rect draft = draft_field(m, surface, body_h);
      cobalt_draw_tile(r, &draft, 1.0f);
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, shown, draft.x + m->pad_tile,
                               draft.y + (draft.h - body_h) / 2,
                               draft.w - 2 * m->pad_tile, 1, COBALT_COLOUR_TEXT);

      cobalt_keyboard_draw(&view->kb, r, surface, &keys);
      return;
   }

   if (cobalt_session_busy()) {
      cobalt_draw_hints(r, "Working...");
   } else if (view->reply[0]) {
      cobalt_draw_hints(r, "A: edit reply   Y: send");
   } else if (view->group) {
      /* No "B: back": the header's Back pill is the same button, and on the
       * GamePad a fourth prompt pushes half the footer up into the band, which
       * is where the duplicate B pill used to end up. */
      cobalt_draw_hints(r, "A: reply   X: requests   R: settings   "
                           "Up/Down: older   +: refresh");
   } else {
      cobalt_draw_hints(r, "A: reply   Up/Down: older   +: refresh");
   }
}
