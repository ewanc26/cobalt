#pragma once

/*
 * Content labels, turned into what a card does with them.
 *
 * The decision is Wolfram's (wolfram/moderation.h): its engine knows the
 * protocol's global label definitions, the adult-content gate, self-labels
 * and which labelers count. This keeps one moderation context for the signed
 * in account and flattens a post's labels into a cobalt_post_mod once, on the
 * worker, so drawing reads a couple of bytes and a reason string (AGENTS.md
 * §13, "The feed is flattened once").
 *
 * Until the account's preferences arrive the context is the safe default:
 * adult content off, Bluesky's moderation labeler subscribed.
 */

#include "atproto/feed.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A short name for a label value, for a card's cover: "Adult content" for
 * porn, "Graphic media" for graphic-media and gore, the value itself for
 * anything else. Never NULL. */
const char *cobalt_labels_reason(const char *value);

/* Whether `mod` covers any of the post (a filtered post that reaches a list
 * that does not drop it, such as a profile, is covered too). */
bool cobalt_post_mod_covers(const cobalt_post_mod *mod);

#ifdef COBALT_HAS_WOLFRAM

struct cJSON;
struct wf_actor_preferences;

/*
 * The signed-in account, which Wolfram's engine needs as `user_did`: without
 * it a `!no-unauthenticated` label (an author's "hide me from logged-out
 * viewers") applies to everyone, and the viewer's own posts are not
 * recognised as theirs. cobalt_labels_apply takes it from the session before
 * every decision; NULL or "" means signed out.
 */
void cobalt_labels_set_viewer(const char *did);

/* Reset the active context to the safe default. Safe to call repeatedly. */
void cobalt_labels_reset(void);

/* Rebuild the active context from the account's saved preferences: the
 * adult-content switch, subscribed labelers and per-label choices. A NULL
 * `prefs` is the safe default. Returns false, leaving the previous context in
 * place, when memory runs out. */
bool cobalt_labels_from_prefs(const struct wf_actor_preferences *prefs);

/* Whether the active context lets adult content through; for diagnostics and
 * tests. */
bool cobalt_labels_adult_enabled(void);

/*
 * Decide `out` for one post from its `labels` and its author's, as kept by
 * Wolfram's typed views (either may be NULL). `author_did` decides which
 * labels are self-applied. Malformed label items are skipped; values the
 * context has no definition for, from a source it does count, are listed in
 * out->unsupported rather than dropped.
 */
void cobalt_labels_apply(cobalt_post_mod *out, const char *uri,
                         const char *author_did, const struct cJSON *labels,
                         const struct cJSON *author_labels);

#endif

#ifdef __cplusplus
}
#endif
