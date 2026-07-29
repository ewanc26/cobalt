#pragma once

/*
 * On-screen keyboard.
 *
 * Cobalt draws its own rather than calling the Wii U's swkbd overlay. Three
 * reasons, in order of weight:
 *
 *   1. swkbd is a C++ nn:: API that wants to composite itself into the app's
 *      GX2 render passes. Cobalt reaches GX2 only through SDL2 (AGENTS.md §6)
 *      and does not own the render loop at that level, so driving swkbd would
 *      mean punching through the abstraction the rest of the app is built on.
 *   2. AGENTS.md §11 already flags the swkbd overlay as slow and suggests a
 *      GamePad touch keyboard as the faster option worth doing early.
 *   3. Drawing it ourselves means it obeys the two-mode rule in §5 for free: it
 *      lays out for whichever surface it is drawn on, and it is fully operable
 *      from the D-pad for someone on a Pro Controller who cannot touch it.
 *
 * Both input paths are always live — touch on the GamePad, and a moving focus
 * driven by the D-pad or stick — because §5 requires every screen to work both
 * ways rather than picking one.
 */

#include "input/input.h"
#include "ui/render.h"
#include "ui/theme.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Four character rows of at most ten, plus the function row. */
#define COBALT_KB_MAX_KEYS 64

typedef enum {
   COBALT_KB_IDLE = 0,
   COBALT_KB_ACCEPTED,   /* the user pressed OK */
   COBALT_KB_CANCELLED,  /* the user backed out; the buffer is unchanged */
} cobalt_kb_result;

typedef struct {
   /* Caller-owned edit target. The keyboard never allocates. */
   char *buffer;
   size_t capacity;

   /* Draw the contents as dots. Set for password fields. */
   bool masked;

   int layer;   /* 0 lower, 1 upper, 2 symbols */
   int row;
   int col;

   /* Hit rectangles in GamePad pixels, recomputed every time the GamePad
    * surface is drawn so touch and the drawn layout cannot drift apart. */
   SDL_Rect hit[COBALT_KB_MAX_KEYS];
   short hit_row[COBALT_KB_MAX_KEYS];
   short hit_col[COBALT_KB_MAX_KEYS];
   int hit_count;
   bool hit_valid;
} cobalt_keyboard;

/*
 * Point the keyboard at a buffer. `buffer` must already hold a NUL-terminated
 * string (possibly empty) and stays owned by the caller.
 */
void cobalt_keyboard_open(cobalt_keyboard *kb, char *buffer, size_t capacity,
                          bool masked);

/* Handle one frame of input. Returns IDLE until the user commits or backs out. */
cobalt_kb_result cobalt_keyboard_update(cobalt_keyboard *kb, const cobalt_input *in);

/*
 * Draw the key grid into `area`. Call once per surface per frame; the GamePad
 * pass is the one that refreshes the touch rectangles.
 */
void cobalt_keyboard_draw(cobalt_keyboard *kb, cobalt_render *r,
                          cobalt_surface_id surface, const SDL_Rect *area);

/*
 * Fill `out` with what should be shown in the text field: the buffer itself,
 * or one dot per character when masked. Appends a caret so an empty field still
 * reads as focused. Pure — unit tested on the host.
 */
void cobalt_keyboard_display_text(const cobalt_keyboard *kb, char *out,
                                  size_t out_size);

/* Height the grid needs for `width`, so a screen can lay out around it. */
int cobalt_keyboard_height(cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
