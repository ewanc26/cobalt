#pragma once

/*
 * The sign-in screen.
 *
 * Two flows share this screen (AGENTS.md §7): an app password typed on Cobalt's
 * own keyboard, or, with the password left empty, OAuth through a hosted
 * Wolfram OAuth node whose URL goes in the service field.
 *
 * The screen has two modes and they are not a style choice: the GamePad panel
 * is 854x480, and a field list plus a full keyboard does not fit on it at a
 * readable size. So it shows the fields, and when a field is picked it hands
 * the whole screen over to that one field and its keyboard. Both surfaces use
 * the same mode logic, since a Pro Controller user watching the TV needs to see
 * exactly what the focus is doing.
 */

#include "atproto/session.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Server, identifier, app password, the show-password toggle, then the sign-in button. */
#define COBALT_SIGNIN_ROWS 5

typedef enum {
   COBALT_SIGNIN_STAY = 0,
   COBALT_SIGNIN_BACK,       /* the user backed out of the screen */
   COBALT_SIGNIN_SUBMIT,     /* the user asked to sign in */
} cobalt_signin_action;

typedef struct {
   char service[COBALT_SERVICE_MAX];
   char identifier[COBALT_IDENTIFIER_MAX];
   char password[COBALT_PASSWORD_MAX];

   int focus;        /* 0..COBALT_SIGNIN_ROWS-1 */
   int editing;      /* field index being edited, or -1 */
   bool show_password; /* opt-in: draw the app password in plain text */
   cobalt_keyboard kb;

   char status[COBALT_MESSAGE_MAX];
   bool status_is_error;

   SDL_Rect hit[COBALT_SIGNIN_ROWS];
   bool hit_valid;
} cobalt_signin;

void cobalt_signin_init(cobalt_signin *s);

/* Zero the password in place. Called on success and on leaving the screen. */
void cobalt_signin_clear_password(cobalt_signin *s);

void cobalt_signin_set_status(cobalt_signin *s, const char *message, bool is_error);

cobalt_signin_action cobalt_signin_update(cobalt_signin *s, const cobalt_input *in);

void cobalt_signin_draw(cobalt_signin *s, cobalt_render *r, cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
