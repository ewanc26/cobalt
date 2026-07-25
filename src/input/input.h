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

/* True if a touch was released inside `rect` this frame (GamePad coords). */
bool cobalt_input_tapped(const cobalt_input *in, const SDL_Rect *rect);

#ifdef __cplusplus
}
#endif
