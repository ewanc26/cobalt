#pragma once

/*
 * Writing a post or a reply.
 *
 * Two modes, for the same reason the sign-in screen has two: a full keyboard
 * and anything else do not both fit on an 854x480 panel at a readable size.
 * Editing gives the whole screen to the text and the keys; committing swaps to
 * a confirmation.
 *
 * The confirmation is not padding. Posting is public and irreversible, and OK
 * on a games-console keyboard is one D-pad slip away from a key someone was
 * aiming at. Every other destructive-and-public action in this app asks first;
 * this one should too.
 */

#include "atproto/feed.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bluesky's limit is 300 graphemes, and the byte budget behind it is 3000.
 * Cobalt counts codepoints — close enough that the only disagreements are
 * emoji sequences and combining marks, and erring towards refusing a post the
 * server would have taken is better than the reverse.
 */
#define COBALT_COMPOSE_GRAPHEMES 300
#define COBALT_COMPOSE_BYTES     3001

typedef enum {
   COBALT_COMPOSE_STAY = 0,
   COBALT_COMPOSE_CANCELLED,  /* backed out; nothing was sent */
   COBALT_COMPOSE_SUBMIT,     /* the user confirmed */
} cobalt_compose_action;

typedef struct {
   char text[COBALT_COMPOSE_BYTES];
   cobalt_keyboard kb;
   bool confirming;
   int confirm_choice;   /* 0 post, 1 keep editing, 2 discard */

   /*
    * Reply target. Empty `parent_uri` means this is a new top-level post.
    * A reply record names both its parent and the thread root; naming the
    * wrong root puts the reply in the wrong conversation for everyone else.
    */
   char parent_uri[COBALT_POST_URI_MAX];
   char parent_cid[COBALT_POST_CID_MAX];
   char root_uri[COBALT_POST_URI_MAX];
   char root_cid[COBALT_POST_CID_MAX];
   char reply_to[COBALT_POST_NAME_MAX];   /* handle, for the header */
} cobalt_compose;

/* Start a new top-level post. */
void cobalt_compose_init(cobalt_compose *compose);

/* Start a reply to `post`, carrying its conversation root. */
void cobalt_compose_reply_to(cobalt_compose *compose, const cobalt_post *post);

bool cobalt_compose_is_reply(const cobalt_compose *compose);

/*
 * Codepoints left before the limit; negative when over. Exposed because it is
 * the one piece of this screen worth testing off-console.
 */
int cobalt_compose_remaining(const cobalt_compose *compose);

cobalt_compose_action cobalt_compose_update(cobalt_compose *compose,
                                            const cobalt_input *in);

void cobalt_compose_draw(cobalt_compose *compose, cobalt_render *r,
                         cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
