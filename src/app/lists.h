#pragma once

/*
 * The signed-in account's lists (app.bsky.graph.list): browse the account's
 * own lists, drill into one to see its members, and create or edit a list's
 * name, description and purpose. Members can be added by handle/DID and
 * removed from the member view.
 *
 * Two sub-modes on one screen, the same shape search.c uses for "pick one
 * thing, then look at what it opens onto": `browsing_members` false shows
 * the list-of-lists menu, true shows the selected list's members. B from
 * browsing members returns to the list-of-lists rather than leaving the
 * screen.
 */

#include "atproto/actors.h"
#include "atproto/curated_lists.h"
#include "atproto/session.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_LIST_SUBJECT_MAX 256
#define COBALT_LIST_STATUS_MAX 192

typedef enum {
   COBALT_LISTS_VIEW_STAY = 0,
   COBALT_LISTS_VIEW_BACK,
   COBALT_LISTS_VIEW_OPEN_PROFILE,
} cobalt_lists_view_action;

typedef struct {
   cobalt_listnav nav;
   bool browsing_members;   /* false: list-of-lists; true: one list's members */
   char open_uri[COBALT_POST_URI_MAX];   /* the list currently drilled into */
   char open_name[COBALT_POST_NAME_MAX];
   char open_description[COBALT_POST_NAME_MAX];
   char open_purpose[COBALT_LIST_PURPOSE_MAX];

   /* The inline list editor. `creating` chooses POST from PUT. */
   bool editing;
   bool creating;
   int edit_field;          /* 0 name, 1 description, 2 purpose */
   char edit_name[COBALT_POST_NAME_MAX];
   char edit_description[COBALT_POST_NAME_MAX];
   char edit_purpose[COBALT_LIST_PURPOSE_MAX];
   cobalt_keyboard kb;
   bool keyboard_active;
   bool saving;

   /* Adding a member: the keyboard collects a handle or DID. */
   bool add_active;
   char add_subject[COBALT_LIST_SUBJECT_MAX];

   /* Removing takes two presses, so a stray X cannot remove an account. */
   int remove_confirm;

   char status[COBALT_LIST_STATUS_MAX];
   bool status_error;
} cobalt_lists_view;

void cobalt_lists_view_init(cobalt_lists_view *view);

/* Rewinds to the list-of-lists and, if nothing is held yet, kicks off the
 * first fetch — the same "only fetch if there is nothing to show" rule the
 * timeline/notifications/muted-blocked entry points already follow. */
void cobalt_lists_view_open(cobalt_lists_view *view);

cobalt_lists_view_action cobalt_lists_view_update(cobalt_lists_view *view,
                                                   const cobalt_input *in);

/* The write finished. Clears the saving flag, shows the message, and refreshes
 * the affected rows on success. */
void cobalt_lists_view_result(cobalt_lists_view *view, cobalt_job_kind kind,
                              bool ok, const char *message);

/* Empty names are refused; description and purpose are optional. Host-tested. */
bool cobalt_lists_view_edit_valid(const char *name, const char *description,
                                  const char *purpose);

void cobalt_lists_view_draw(cobalt_lists_view *view, cobalt_render *r,
                            cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
