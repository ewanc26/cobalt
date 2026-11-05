#pragma once

/*
 * An actor's profile and their posts.
 *
 * The header is a card of its own above the post list rather than a separate
 * screen, so following someone and reading what they write are one place. It
 * scrolls with the list — pinning it would cost a third of the GamePad's
 * height on a screen whose point is the posts underneath.
 *
 * Selection index 0 is the header (where the follow button lives); everything
 * after it is a post. That keeps one cursor for the whole screen rather than
 * a focus that has to move between two regions.
 */

#include "atproto/feed.h"
#include "atproto/actor_profile.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_PROFILE_VIEW_STAY = 0,
   COBALT_PROFILE_VIEW_BACK,
   COBALT_PROFILE_VIEW_OPEN_THREAD,
   COBALT_PROFILE_VIEW_MENU,
} cobalt_profile_action;

typedef struct {
   cobalt_listnav nav;   /* row 0 is the header card, 1.. are posts */
} cobalt_profile_view;

void cobalt_profile_view_init(cobalt_profile_view *view);
void cobalt_profile_view_rewind(cobalt_profile_view *view);

cobalt_profile_action cobalt_profile_view_update(cobalt_profile_view *view,
                                                 const cobalt_input *in);

void cobalt_profile_view_draw(cobalt_profile_view *view, cobalt_render *r,
                              cobalt_surface_id surface);

/* The post the cursor is on, or NULL when it is on the header. */
const cobalt_post *cobalt_profile_view_selected_post(const cobalt_profile_view *view);

/*
 * The signed-in account's profile editor. Text only: display name and bio.
 * Avatar and banner uploads are blob work with their own memory budget and are
 * tracked separately rather than made a condition of editing the text.
 *
 * The buffers bound what can be typed; cobalt_profile_edit_valid() is the
 * second check before a write, and is host-tested at both limits. Empty is a
 * valid value: clearing a display name or bio is a real edit.
 */
typedef enum {
   COBALT_PROFILE_EDIT_STAY = 0,
   COBALT_PROFILE_EDIT_BACK,
   COBALT_PROFILE_EDIT_SAVE,
} cobalt_profile_edit_action;

typedef struct {
   char display_name[COBALT_PROFILE_NAME_MAX + 1];
   char description[COBALT_PROFILE_BIO_MAX];
   cobalt_keyboard kb;
   int field;       /* 0 display name, 1 description */
   bool editing;    /* the keyboard owns input this frame */
   bool saving;     /* the write is in flight; the fields stay as typed */
} cobalt_profile_edit;

void cobalt_profile_edit_open(cobalt_profile_edit *edit, const cobalt_profile *profile);
bool cobalt_profile_edit_valid(const char *display_name, const char *description);
cobalt_profile_edit_action cobalt_profile_edit_update(cobalt_profile_edit *edit,
                                                      const cobalt_input *in);
void cobalt_profile_edit_draw(cobalt_profile_edit *edit, cobalt_render *r,
                              cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
