#pragma once

/*
 * "Set up this Cobalt": the first-run screen that collects an entropy seed from
 * the GamePad when entropy.bin is missing. See util/entropy_gather.h for why,
 * and for what it does and does not claim.
 *
 * It saves the seed and then asks for a restart rather than carrying on,
 * because the generators are provisioned once at startup (AGENTS.md §13) and
 * starting them again mid-run would be a second, untested path.
 */

#include "input/input.h"
#include "ui/render.h"
#include "util/entropy_gather.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_ENTROPY_VIEW_COLLECTING = 0,
   COBALT_ENTROPY_VIEW_SAVED,
   COBALT_ENTROPY_VIEW_FAILED
} cobalt_entropy_view_state;

typedef enum {
   COBALT_ENTROPY_VIEW_STAY = 0,
   COBALT_ENTROPY_VIEW_SKIP,    /* B: carry on without a seed (no network) */
   COBALT_ENTROPY_VIEW_QUIT     /* A after saving: restart Cobalt */
} cobalt_entropy_view_action;

typedef struct {
   cobalt_entropy_view_state state;
   cobalt_gather gather;
   char path[256];
} cobalt_entropy_view;

/* `extra` is device-specific bytes to mix in (tick, time); may be NULL. */
void cobalt_entropy_view_init(cobalt_entropy_view *v, const char *seed_path,
                              const void *extra, size_t extra_len);

/* `tick` is a high-resolution counter for this frame. */
cobalt_entropy_view_action cobalt_entropy_view_update(cobalt_entropy_view *v,
                                                      const cobalt_input *in,
                                                      uint32_t tick);

void cobalt_entropy_view_draw(cobalt_entropy_view *v, cobalt_render *r,
                              cobalt_surface_id surface, int top);

#ifdef __cplusplus
}
#endif
