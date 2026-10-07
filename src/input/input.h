#pragma once

/*
 * Input abstraction.
 *
 * AGENTS.md §5 requires every screen to be usable two ways: touch on the
 * GamePad, and D-pad/buttons for someone on a Pro Controller with the GamePad
 * out of view. Screens therefore never read SDL events directly — they read a
 * cobalt_input snapshot that both paths feed into.
 *
 * Aroma swallows HOME before it reaches the app (AGENTS.md §3), so there is no
 * HOME button here; quitting goes through ProcUI plus an explicit in-app exit.
 */

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_BTN_UP = 0,
   COBALT_BTN_DOWN,
   COBALT_BTN_LEFT,
   COBALT_BTN_RIGHT,
   COBALT_BTN_CONFIRM,
   COBALT_BTN_BACK,
   COBALT_BTN_MENU,
   /*
    * The GamePad's X and Y. The D-pad plus A/B/+ was full once posts became
    * interactive — like, repost, open, back and refresh took every binding a
    * list screen had — and burying the rest behind a menu would make them
    * unreachable one-handed, which is how this app is actually held.
    */
   COBALT_BTN_ALT_X,
   COBALT_BTN_ALT_Y,
   COBALT_BTN_COUNT,
} cobalt_button;

typedef struct {
   bool held[COBALT_BTN_COUNT];
   bool pressed[COBALT_BTN_COUNT];  /* rising edge, plus auto-repeat */
   uint32_t held_since[COBALT_BTN_COUNT];
   uint32_t next_repeat[COBALT_BTN_COUNT];

   /* Touch, already mapped into GamePad pixel coordinates (854x480). */
   bool touch_down;
   bool touch_began;
   bool touch_ended;
   int touch_x;
   int touch_y;
   /* The touch that just ended was a drag, not a tap. Set with touch_ended; a
    * drag is never also a tap, so cobalt_input_tapped is false for it. */
   bool touch_dragged;

   bool quit_requested;
} cobalt_input;

void cobalt_input_init(cobalt_input *in);
void cobalt_input_shutdown(void);

/* Clear per-frame edges. Call once before pumping events. */
void cobalt_input_begin_frame(cobalt_input *in, uint32_t now_ms);

void cobalt_input_handle_event(cobalt_input *in, const SDL_Event *event);

/* Apply auto-repeat for held directions. Call after all events are handled. */
void cobalt_input_end_frame(cobalt_input *in, uint32_t now_ms);

static inline bool
cobalt_input_pressed(const cobalt_input *in, cobalt_button btn)
{
   return in->pressed[btn];
}

static inline bool
cobalt_input_held(const cobalt_input *in, cobalt_button btn)
{
   return in->held[btn];
}

/*
 * Scroll a list by touch: move `*selected` by the whole rows a finger dragged
 * along the screen has covered this frame (up is forward, down is back),
 * clamped to 0..count-1. One row is the average height of the rows drawn last
 * frame (`hit`, `hit_count`; NULL or 0 for a default), so the list follows the
 * finger at about its speed whatever the card heights. Call once per frame,
 * from the screen that owns the list.
 */
void cobalt_input_drag_list(const cobalt_input *in, int *selected, int count,
                            const SDL_Rect *hit, int hit_count);

/* True if a touch was released inside `rect` this frame (GamePad coords). A drag
 * that happens to end there is not a tap. */
bool cobalt_input_tapped(const cobalt_input *in, const SDL_Rect *rect);

#ifdef __cplusplus
}
#endif
