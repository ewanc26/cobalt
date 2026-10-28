#pragma once

/*
 * The Credits screen: every repository in the Cobalt/Indigo/Platinum stack,
 * with the full address printed rather than hidden behind a button. A console
 * cannot open a browser, so a link is only useful here if it can be read and
 * typed on another machine; there is nothing to press.
 *
 * Kept apart from the home menu and Settings because it is a screen of its own,
 * and apart from the session because it never fetches anything.
 */

#include "ui/render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   const char *name;
   const char *url;
} cobalt_credit_repo;

/* The stack's repositories in reading order. The list is static and complete:
 * a missing entry is a build error in the host tests, not a runtime fallback. */
int cobalt_credits_repo_count(void);
const cobalt_credit_repo *cobalt_credits_repo(int index);

void cobalt_credits_view_draw(cobalt_render *r);

#ifdef __cplusplus
}
#endif
