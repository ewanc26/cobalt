/*
 * feeds.c -- custom feed discovery and management.
 *
 * Feed discovery uses app.bsky.unspecced.getSuggestedFeeds (via
 * wf_agent_get_suggested_feeds_typed) to find popular generators.
 * Feed management reads/writes savedFeedsPrefV2 via
 * wf_agent_get_preferences_typed / wf_agent_put_preferences_json.
 */

#include "atproto/feeds.h"
#include "atproto/session_internal.h"

#include "wolfram/agent.h"
#include "wolfram/unspecced_trends_typed.h"
#include "wolfram/feed_gen_typed.h"
#include "wolfram/feedgen_typed.h"
#include "wolfram/actor_prefs_typed.h"


#include <cJSON.h>
#include <string.h>

/* Session state for feed discovery. */
static cobalt_feed_discovery_page s_discovery_page;

/* Session state for saved feeds v2. */
static cobalt_saved_feeds_v2 s_saved_feeds_v2;

/* Session state for a single feed generator detail. */
static cobalt_feed_generator s_feed_generator;
static bool s_feed_generator_valid = false;

/* ------------------------------------------------------------------ */
/* Feed generator view -> cobalt_feed_generator */

static void
copy_generator_from_agent(cobalt_feed_generator *out, const wf_agent_generator_view *src)
{
    if (!out || !src) return;
    snprintf(out->uri, sizeof out->uri, "%s", src->uri ? src->uri : "");
    snprintf(out->did, sizeof out->did, "%s", src->did ? src->did : "");
    snprintf(out->display_name, sizeof out->display_name, "%s",
             src->display_name ? src->display_name : "");
    snprintf(out->description, sizeof out->description, "%s",
             src->description ? src->description : "");
    snprintf(out->avatar, sizeof out->avatar, "%s",
             src->avatar ? src->avatar : "");
    out->has_like_count = src->has_like_count != 0;
    out->like_count = src->has_like_count ? (int64_t) src->like_count : 0;
    /* wf_agent_generator_view doesn't have viewer_like or subscriber fields */
    out->has_viewer_like = false;
    out->viewer_like = false;
    out->has_subscriber_count = false;
    out->subscriber_count = 0;
    out->has_viewer_subscribed = false;
    out->viewer_subscribed = false;
}


/* ------------------------------------------------------------------ */
/* Feed discovery: suggested feeds */

void
cobalt_session_run_feed_discovery(const cobalt_job_input *in, cobalt_job_result *r,
                                   cobalt_auth_state *state)
{
    (void) in;
    if (!g_session.wf) {
        cobalt_session_set_message(r, "Sign in first.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    wf_agent_generator_view_list list = {0};
    const int limit = COBALT_FEED_DISCOVERY_PAGE;

    const wf_status status =
        wf_agent_get_suggested_feeds_typed(g_session.wf, limit, &list);

    if (status != WF_OK) {
        COBALT_LOGW("session: feed discovery failed (%d)", (int) status);
        cobalt_session_set_message(r, "Could not discover feeds (wolfram status %d).",
                                   (int) status);
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    SDL_LockMutex(g_session.lock);
    memset(&s_discovery_page, 0, sizeof(s_discovery_page));
    s_discovery_page.count = (int) list.generator_count;
    /* app.bsky.unspecced.getSuggestedFeeds doesn't support cursor pagination,
     * so has_more is always false. */
    s_discovery_page.has_more = false;
    s_discovery_page.cursor[0] = '\0';
    for (size_t i = 0; i < list.generator_count && i < COBALT_FEED_DISCOVERY_PAGE;
         i++) {
        copy_generator_from_agent(&s_discovery_page.generators[i], &list.generators[i]);
    }
    SDL_UnlockMutex(g_session.lock);

    wf_agent_generator_view_list_free(&list);

    COBALT_LOGI("session: discovered %d feeds", s_discovery_page.count);
    *state = COBALT_AUTH_SIGNED_IN;
    r->ok = true;
}

/* ------------------------------------------------------------------ */
/* Feed generator detail */

void
cobalt_session_run_feed_generator(const cobalt_job_input *in, cobalt_job_result *r,
                                   cobalt_auth_state *state)
{
    if (!g_session.wf) {
        cobalt_session_set_message(r, "Sign in first.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    wf_agent_generator_detail detail = {0};
    const wf_status status =
        wf_agent_get_feed_generator_typed(g_session.wf, in->uri, &detail);

    if (status != WF_OK) {
        COBALT_LOGW("session: feed generator fetch failed (%d)", (int) status);
        cobalt_session_set_message(r, "Could not load feed details (wolfram status %d).",
                                   (int) status);
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    SDL_LockMutex(g_session.lock);
    copy_generator_from_agent(&s_feed_generator, &detail.view);
    s_feed_generator_valid = true;
    SDL_UnlockMutex(g_session.lock);

    wf_agent_generator_detail_free(&detail);

    COBALT_LOGI("session: loaded feed generator %s", in->uri);
    *state = COBALT_AUTH_SIGNED_IN;
    r->ok = true;
}

/* ------------------------------------------------------------------ */
/* Saved feeds v2: read savedFeedsPrefV2 */

static void
copy_saved_feed_entry(cobalt_saved_feed_entry *out, const wf_actor_pref_saved_feed *src)
{
    if (!out || !src) return;
    snprintf(out->uri, sizeof out->uri, "%s", src->value ? src->value : "");
    /* display_name is not in the pref; we'll fill it from getFeedGenerators */
    out->display_name[0] = '\0';
    out->pinned = src->has_pinned && src->pinned;
}

void
cobalt_session_run_saved_feeds_v2(const cobalt_job_input *in, cobalt_job_result *r,
                                    cobalt_auth_state *state)
{
    (void) in;
    if (!g_session.wf) {
        cobalt_session_set_message(r, "Sign in first.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    wf_actor_preferences prefs = {0};
    const wf_status status = wf_agent_get_actor_prefs_typed(g_session.wf, &prefs);

    if (status != WF_OK) {
        COBALT_LOGW("session: saved feeds v2 fetch failed (%d)", (int) status);
        cobalt_session_set_message(r, "Could not load saved feeds (wolfram status %d).",
                                   (int) status);
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    SDL_LockMutex(g_session.lock);
    memset(&s_saved_feeds_v2, 0, sizeof(s_saved_feeds_v2));

    /* savedFeedsPrefV2: items[] with type=="feed" */
    for (size_t i = 0; i < prefs.saved_feeds_v2.item_count &&
         s_saved_feeds_v2.count < COBALT_SAVED_FEEDS_MAX; i++) {
        const wf_actor_pref_saved_feed *src = &prefs.saved_feeds_v2.items[i];
        if (src->type && strcmp(src->type, "feed") == 0) {
            copy_saved_feed_entry(&s_saved_feeds_v2.entries[s_saved_feeds_v2.count],
                                   src);
            s_saved_feeds_v2.count++;
        }
    }
    SDL_UnlockMutex(g_session.lock);

    /* Now resolve names via getFeedGenerators for any feed without a name */
    if (s_saved_feeds_v2.count > 0) {
        const char *uris[COBALT_SAVED_FEEDS_MAX];
        int uri_count = 0;
        for (int i = 0; i < s_saved_feeds_v2.count; i++) {
            if (s_saved_feeds_v2.entries[i].display_name[0] == '\0' &&
                s_saved_feeds_v2.entries[i].uri[0]) {
                uris[uri_count++] = s_saved_feeds_v2.entries[i].uri;
            }
        }
        if (uri_count > 0) {
            wf_feedgen_generator_list gens = {0};
            const wf_status st =
                wf_feedgen_get_feed_generators_typed(g_session.wf, uris, uri_count,
                                                      &gens);
            if (st == WF_OK) {
                SDL_LockMutex(g_session.lock);
                for (size_t g = 0; g < gens.generator_count; g++) {
                    const wf_feedgen_generator_view *v = &gens.generators[g];
                    for (int i = 0; i < s_saved_feeds_v2.count; i++) {
                        if (strcmp(v->uri, s_saved_feeds_v2.entries[i].uri) == 0 &&
                            v->display_name && v->display_name[0]) {
                            snprintf(s_saved_feeds_v2.entries[i].display_name,
                                     sizeof(s_saved_feeds_v2.entries[i].display_name),
                                     "%s", v->display_name);
                        }
                    }
                }
                SDL_UnlockMutex(g_session.lock);
                wf_feedgen_generator_list_free(&gens);
            }
        }
    }

    wf_actor_preferences_free(&prefs);

    COBALT_LOGI("session: loaded %d saved feeds v2", s_saved_feeds_v2.count);
    *state = COBALT_AUTH_SIGNED_IN;
    r->ok = true;
}

/* ------------------------------------------------------------------ */
/* Save/remove/pin feed: write savedFeedsPrefV2 */

/* Build a savedFeedsPrefV2 items array from the current state plus the
 * change. Returns a cJSON array the caller must free. */
static cJSON *
build_saved_feeds_v2_items(const char *feed_uri, bool pinned, bool remove)
{
    /* Fetch current preferences to preserve non-feed items */
    wf_actor_preferences prefs = {0};
    const wf_status status = wf_agent_get_actor_prefs_typed(g_session.wf, &prefs);
    if (status != WF_OK) {
        COBALT_LOGW("session: could not read preferences for feed save");
        return NULL;
    }

    cJSON *items = cJSON_AddArrayToObject(NULL, "items");
    if (!items) {
        wf_actor_preferences_free(&prefs);
        return NULL;
    }

    /* Copy existing items, modifying the target feed */
    for (size_t i = 0; i < prefs.saved_feeds_v2.item_count; i++) {
        const wf_actor_pref_saved_feed *src = &prefs.saved_feeds_v2.items[i];
        if (src->type && src->value) {
            if (strcmp(src->type, "feed") == 0 &&
                strcmp(src->value, feed_uri) == 0) {
                if (remove) {
                    /* Skip: removing this feed */
                    continue;
                }
                /* Update pinned state */
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "type", src->type);
                cJSON_AddStringToObject(item, "value", src->value);
                if (src->id) cJSON_AddStringToObject(item, "id", src->id);
                cJSON_AddBoolToObject(item, "pinned", pinned);
                cJSON_AddItemToArray(items, item);
            } else {
                /* Copy unchanged */
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "type", src->type);
                cJSON_AddStringToObject(item, "value", src->value);
                if (src->id) cJSON_AddStringToObject(item, "id", src->id);
                if (src->has_pinned) {
                    cJSON_AddBoolToObject(item, "pinned", src->pinned);
                }
                cJSON_AddItemToArray(items, item);
            }
        }
    }

    if (!remove) {
        /* Add new feed entry */
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "feed");
        cJSON_AddStringToObject(item, "value", feed_uri);
        cJSON_AddBoolToObject(item, "pinned", pinned);
        cJSON_AddItemToArray(items, item);
    }

    wf_actor_preferences_free(&prefs);
    return items;
}

void
cobalt_session_run_save_feed(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state)
{
    if (!g_session.wf) {
        cobalt_session_set_message(r, "Sign in first.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    cJSON *items = build_saved_feeds_v2_items(in->uri, in->paging, false);
    if (!items) {
        cobalt_session_set_message(r, "Could not prepare feed save.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    /* Build the full preferences object */
    cJSON *prefs = cJSON_CreateObject();
    cJSON_AddItemToObject(prefs, "savedFeedsPrefV2", items);

    char *json = cJSON_PrintUnformatted(prefs);
    cJSON_Delete(prefs);
    if (!json) {
        cobalt_session_set_message(r, "Could not serialize feed save.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    wf_response res = {0};
    const wf_status status = wf_agent_put_preferences(g_session.wf, json, &res);
    free(json);
    wf_response_free(&res);

    if (status != WF_OK) {
        COBALT_LOGW("session: feed save failed (%d)", (int) status);
        cobalt_session_set_message(r, "Could not save feed (wolfram status %d).",
                                   (int) status);
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    /* Invalidate cached saved feeds so the UI reloads */
    SDL_LockMutex(g_session.lock);
    memset(&s_saved_feeds_v2, 0, sizeof(s_saved_feeds_v2));
    SDL_UnlockMutex(g_session.lock);

    COBALT_LOGI("session: saved feed %s (pinned=%d)", in->uri, in->paging);
    *state = COBALT_AUTH_SIGNED_IN;
    r->ok = true;
}

void
cobalt_session_run_remove_feed(const cobalt_job_input *in, cobalt_job_result *r,
                                 cobalt_auth_state *state)
{
    if (!g_session.wf) {
        cobalt_session_set_message(r, "Sign in first.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    cJSON *items = build_saved_feeds_v2_items(in->uri, false, true);
    if (!items) {
        cobalt_session_set_message(r, "Could not prepare feed removal.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    cJSON *prefs = cJSON_CreateObject();
    cJSON_AddItemToObject(prefs, "savedFeedsPrefV2", items);

    char *json = cJSON_PrintUnformatted(prefs);
    cJSON_Delete(prefs);
    if (!json) {
        cobalt_session_set_message(r, "Could not serialize feed removal.");
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    wf_response res = {0};
    const wf_status status = wf_agent_put_preferences(g_session.wf, json, &res);
    free(json);
    wf_response_free(&res);

    if (status != WF_OK) {
        COBALT_LOGW("session: feed removal failed (%d)", (int) status);
        cobalt_session_set_message(r, "Could not remove feed (wolfram status %d).",
                                   (int) status);
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    /* Invalidate cache */
    SDL_LockMutex(g_session.lock);
    memset(&s_saved_feeds_v2, 0, sizeof(s_saved_feeds_v2));
    SDL_UnlockMutex(g_session.lock);

    COBALT_LOGI("session: removed feed %s", in->uri);
    *state = COBALT_AUTH_SIGNED_IN;
    r->ok = true;
}

void
cobalt_session_run_pin_feed(const cobalt_job_input *in, cobalt_job_result *r,
                             cobalt_auth_state *state)
{
    /* Reuse save_feed with the new pinned state */
    cobalt_job_input modified = *in;
    modified.paging = in->paging; /* paging encodes pinned as true or false */
    cobalt_session_run_save_feed(&modified, r, state);
}

/* ------------------------------------------------------------------ */
/* Public accessors */

const cobalt_feed_discovery_page *
cobalt_session_feed_discovery_results(void)
{
    return &s_discovery_page;
}

const cobalt_saved_feeds_v2 *
cobalt_session_saved_feeds_v2(void)
{
    return &s_saved_feeds_v2;
}

const cobalt_feed_generator *
cobalt_session_feed_generator(void)
{
    return s_feed_generator_valid ? &s_feed_generator : NULL;
}

/* ------------------------------------------------------------------ */
/* Public submission functions */

bool
cobalt_session_begin_feed_discovery(const char *cursor)
{
    cobalt_job_input in = {0};
    if (cursor && cursor[0]) {
        snprintf(in.cursor, sizeof in.cursor, "%s", cursor);
        in.paging = true;
    }
    return cobalt_session_submit(COBALT_JOB_FEED_DISCOVERY, &in);
}

bool
cobalt_session_begin_feed_generator(const char *feed_uri)
{
    cobalt_job_input in = {0};
    snprintf(in.uri, sizeof in.uri, "%s", feed_uri);
    return cobalt_session_submit(COBALT_JOB_FEED_GENERATOR, &in);
}

bool
cobalt_session_begin_saved_feeds_v2(void)
{
    cobalt_job_input in = {0};
    return cobalt_session_submit(COBALT_JOB_SAVED_FEEDS_V2, &in);
}

bool
cobalt_session_begin_save_feed(const char *feed_uri, bool pinned)
{
    cobalt_job_input in = {0};
    snprintf(in.uri, sizeof in.uri, "%s", feed_uri);
    in.paging = pinned; /* reuse paging as pinned flag */
    return cobalt_session_submit(COBALT_JOB_SAVE_FEED, &in);
}

bool
cobalt_session_begin_remove_feed(const char *feed_uri)
{
    cobalt_job_input in = {0};
    snprintf(in.uri, sizeof in.uri, "%s", feed_uri);
    return cobalt_session_submit(COBALT_JOB_REMOVE_FEED, &in);
}

bool
cobalt_session_begin_pin_feed(const char *feed_uri, bool pinned)
{
    cobalt_job_input in = {0};
    snprintf(in.uri, sizeof in.uri, "%s", feed_uri);
    in.paging = pinned;
    return cobalt_session_submit(COBALT_JOB_PIN_FEED, &in);
}