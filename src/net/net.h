#pragma once

/*
 * Network availability.
 *
 * AGENTS.md §6 is explicit: check nn::ac before firing off any request, and
 * fail with a clear on-screen message rather than a hang or a crash when the
 * console has no network. Everything in net/ goes through here first.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_NET_UNKNOWN = 0,  /* not probed yet */
   COBALT_NET_DOWN,         /* AC reports no application connection */
   COBALT_NET_UP,           /* connected, address assigned */
   COBALT_NET_UNAVAILABLE,  /* AC itself failed to initialise */
} cobalt_net_status;

bool cobalt_net_init(void);
void cobalt_net_shutdown(void);

/*
 * Re-query AC. Cheap enough to call when opening a screen that needs the
 * network, but not something to do every frame.
 */
cobalt_net_status cobalt_net_refresh(void);

cobalt_net_status cobalt_net_get_status(void);

const char *cobalt_net_status_string(cobalt_net_status status);

/* Dotted-quad address of the console, or "0.0.0.0" when not connected. */
const char *cobalt_net_local_address(void);

#ifdef __cplusplus
}
#endif
