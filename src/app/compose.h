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
#include "atproto/session.h"
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

/*
 * Who can reply to a new top-level post. Not offered on replies — Bluesky
 * scopes a threadgate to the post it is attached to, and gating a reply's own
 * replies separately from the rest of the thread is confusing enough that the
 * official client does not offer it either; only the root post gets this
 * choice here.
 */
typedef enum {
   COBALT_REPLY_GATE_EVERYONE = 0,
   COBALT_REPLY_GATE_FOLLOWED_MENTIONED,
   COBALT_REPLY_GATE_NOBODY,
   COBALT_REPLY_GATE_COUNT,
} cobalt_reply_gate;

#define COBALT_PICKER_MAX      32
#define COBALT_PICKER_NAME_MAX 64

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

   /* Quoted post, if any. Independent of the reply fields: a quote is a new
    * top-level post that embeds another, never a reply. */
   char quote_uri[COBALT_POST_URI_MAX];
   char quote_cid[COBALT_POST_CID_MAX];
   char quote_handle[COBALT_POST_NAME_MAX];

   /* Only meaningful when this is a new top-level post; ignored on replies. */
   cobalt_reply_gate reply_gate;

   /*
    * A thread: the posts already written, in order. `text` is the one being
    * written, so the thread has thread_count + 1 posts. Top-level posts only,
    * and without images, because Wolfram's thread call posts plain text.
    */
   int thread_count;
   char thread_texts[COBALT_THREAD_POSTS_MAX - 1][COBALT_COMPOSE_BYTES];

   /* Attached image: full SD path, empty for none. New posts and quotes only. */
   char attach_path[COBALT_ATTACH_PATH_MAX];

   /* Alt text for the attached image, typed right after picking it. */
   char attach_alt[COBALT_ATTACH_ALT_MAX];
   bool alt_editing;
   cobalt_keyboard alt_kb;

   /* Image picker, a sub-mode of the confirmation. */
   bool picking;
   int picker_count;
   int picker_sel;
   int picker_too_large;   /* images skipped for size, to explain an empty list */
   char picker_dir[COBALT_ATTACH_PATH_MAX];
   char picker_names[COBALT_PICKER_MAX][COBALT_PICKER_NAME_MAX];
} cobalt_compose;

/* Open the picker over `dir`. */
void cobalt_compose_open_picker(cobalt_compose *compose, const char *dir);

/* Start a new top-level post. */
void cobalt_compose_init(cobalt_compose *compose);

/* Start a reply to `post`, carrying its conversation root. */
void cobalt_compose_reply_to(cobalt_compose *compose, const cobalt_post *post);

/* Start a post that quotes `post`. */
void cobalt_compose_quote(cobalt_compose *compose, const cobalt_post *post);

bool cobalt_compose_is_quote(const cobalt_compose *compose);

bool cobalt_compose_is_reply(const cobalt_compose *compose);

/*
 * Whether the post being written can be followed by another in a thread: a new
 * top-level post, not a quote, without an image, with room left.
 */
bool cobalt_compose_can_extend(const cobalt_compose *compose);

/* Keep the current text as the next post of a thread and start an empty one.
 * False, and nothing changes, when cobalt_compose_can_extend is false or the
 * text is empty. */
bool cobalt_compose_extend(cobalt_compose *compose);

/* The thread's texts in order, the current one last, for posting. Returns the
 * count; `out` holds pointers into `compose`. */
int cobalt_compose_thread_texts(const cobalt_compose *compose,
                                const char *out[COBALT_THREAD_POSTS_MAX]);

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
