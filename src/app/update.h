#pragma once

/*
 * The "Check for updates" screen and the worker behind it.
 *
 * Flow, and why it is shaped this way (AGENTS.md §13, "Updates"):
 *
 *   IDLE -> CHECKING -> AVAILABLE | CURRENT | FAILED
 *   AVAILABLE --(user presses A)--> DOWNLOADING -> READY | FAILED
 *   READY: the verified file is staged; it replaces the installed build when
 *          Cobalt quits, and the previous build is kept until the new one runs.
 *
 * Nothing is downloaded until the user has seen the version and notes and chosen
 * to; nothing replaces the running build until the file has been verified twice.
 * The network work runs on its own thread and the frame loop only reads state.
 */

#include "input/input.h"
#include "ui/render.h"
#include "update/selfupdate.h"

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_UPDATE_IDLE = 0,
   COBALT_UPDATE_CHECKING,
   COBALT_UPDATE_AVAILABLE,
   COBALT_UPDATE_CURRENT,
   COBALT_UPDATE_DOWNLOADING,
   COBALT_UPDATE_READY,
   COBALT_UPDATE_FAILED
} cobalt_update_state;

typedef enum {
   COBALT_UPDATE_VIEW_STAY = 0,
   COBALT_UPDATE_VIEW_BACK
} cobalt_update_view_action;

/* Fetch `url` into a heap buffer (free with free()). The default is
 * cobalt_http_get; tests replace it. */
typedef bool (*cobalt_update_fetch_fn)(const char *url, size_t max_bytes,
                                       unsigned char **data, size_t *size);

typedef struct {
   cobalt_update_state state;       /* read under `lock` */
   wf_update_manifest manifest;
   cobalt_update_paths paths;
   bool have_paths;
   char message[160];
   char running[32];                /* the version of this build */
   cobalt_update_fetch_fn fetch;
   bool threaded;                   /* false runs jobs inline, for tests */
   bool committed;                  /* .old dropped after the first frame */
   SDL_mutex *lock;
   SDL_Thread *thread;
   int job;                         /* 0 none, 1 check, 2 install */
} cobalt_update_view;

void cobalt_update_view_init(cobalt_update_view *v);
void cobalt_update_view_destroy(cobalt_update_view *v);

/* Resolve the installed and staging paths and repair an interrupted update. */
void cobalt_update_view_startup(cobalt_update_view *v, const char *data_root);

/* Called every frame: after the first, drops the previous build kept by apply. */
void cobalt_update_view_tick(cobalt_update_view *v);

/* The screen was opened: start a check. */
void cobalt_update_view_open(cobalt_update_view *v);

cobalt_update_view_action cobalt_update_view_update(cobalt_update_view *v,
                                                    const cobalt_input *in);
void cobalt_update_view_draw(cobalt_update_view *v, cobalt_render *r, int top);

cobalt_update_state cobalt_update_view_state(cobalt_update_view *v);

/* On quit: put a verified, staged update in place. Returns true if it did. */
bool cobalt_update_view_apply_on_quit(cobalt_update_view *v);

#ifdef __cplusplus
}
#endif
