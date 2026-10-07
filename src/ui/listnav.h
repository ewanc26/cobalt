#pragma once

/*
 * What every scrolling list screen shares: which row is selected, which is the
 * first drawn, and where the last frame put each row so a tap can find it.
 *
 * A screen embeds one of these, calls cobalt_listnav_move at the top of its
 * update and cobalt_listnav_follow at the bottom (its own buttons go between),
 * and brackets its draw loop with begin / add / end. The rule for keeping the
 * selection on screen lives here once: scrolling back is exact, scrolling
 * forward uses what the previous frame actually fitted, because row heights
 * vary and are only known after a draw.
 */

#include "input/input.h"

#include <SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rows a screen can hold: the most any of them keeps (a profile's header card
 * plus its posts). */
#define COBALT_LISTNAV_MAX 64

typedef struct {
   int selected;
   int scroll;          /* index of the first row drawn */
   int last_visible;    /* last row the previous frame fitted, -1 before one */

   /* Where the last GamePad frame drew each row, and which row that was. */
   SDL_Rect hit[COBALT_LISTNAV_MAX];
   int hit_index[COBALT_LISTNAV_MAX];
   int hit_count;
   bool hit_valid;
} cobalt_listnav;

/* Everything zero and nothing visible yet. */
void cobalt_listnav_init(cobalt_listnav *nav);

/* Back to the first row, keeping the hit rectangles of the frame already drawn. */
void cobalt_listnav_rewind(cobalt_listnav *nav);

/*
 * Apply this frame's input to a list of `count` rows: clamp the selection (a
 * refresh can return fewer rows than were on screen), follow a touch drag, move
 * with the D-pad, and select the row a tap landed on. A tap only selects;
 * opening a row is a second, deliberate press, so scrolling by touch on a dense
 * list is not a minefield. Returns the row a tap selected this frame, or -1.
 * Does nothing for an empty list.
 */
int cobalt_listnav_move(cobalt_listnav *nav, const cobalt_input *in, int count);

/* Scroll so the selection is on screen. Call after the screen's own handling,
 * because that may move the selection. */
void cobalt_listnav_follow(cobalt_listnav *nav);

/* The draw side. begin clears the hit rectangles (GamePad frames only: the TV
 * frame must not overwrite them); add records one row; end publishes them with
 * the last row that fitted. An empty list calls begin then empty. */
void cobalt_listnav_draw_begin(cobalt_listnav *nav, bool touchable);
void cobalt_listnav_draw_add(cobalt_listnav *nav, bool touchable, const SDL_Rect *rect,
                             int index);
void cobalt_listnav_draw_end(cobalt_listnav *nav, bool touchable, int last_fitted);
void cobalt_listnav_draw_empty(cobalt_listnav *nav, bool touchable);

#ifdef __cplusplus
}
#endif
