#pragma once

/*
 * Changing one of the account's saved preferences.
 *
 * app.bsky.actor.putPreferences replaces the whole array, so every change is a
 * read-modify-write of the raw preferences, copied verbatim so that types and
 * fields Cobalt does not understand survive. The read is not optional: a write
 * built from a failed read would replace everything the account has saved
 * (muted words, feeds, label choices) with the one preference being changed.
 */

#ifdef COBALT_HAS_WOLFRAM

#include <cJSON.h>
#include <wolfram/agent.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The account's preferences array, owned by the caller. NULL when it could
 * not be read or was not an array: the caller must then change nothing. */
cJSON *cobalt_prefs_read(wf_agent *wf);

/* The first preference of `type` in `prefs` (borrowed), or NULL. */
cJSON *cobalt_prefs_find(cJSON *prefs, const char *type);

/*
 * Remove every preference of `type` from `prefs`, append `replacement` (taken,
 * even on failure; NULL just removes) and PUT the result. `prefs` stays the
 * caller's. Returns Wolfram's status; WF_ERR_ALLOC if serialising failed.
 */
wf_status cobalt_prefs_put_replacing(wf_agent *wf, cJSON *prefs, const char *type,
                                     cJSON *replacement);

/*
 * As cobalt_prefs_put_replacing, but only entries of `type` whose `key` string
 * field equals `value` are replaced. Used where one type holds many entries,
 * such as a content-label preference per label.
 */
wf_status cobalt_prefs_put_replacing_match(wf_agent *wf, cJSON *prefs, const char *type,
                                           const char *key, const char *value,
                                           cJSON *replacement);

#ifdef __cplusplus
}
#endif

#endif
