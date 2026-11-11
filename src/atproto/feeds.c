/*
 * feeds.c -- custom feed discovery and management.
 *
 * Feed discovery uses app.bsky.unspecced.getSuggestedFeeds (via
 * wf_agent_get_suggested_feeds_typed) to find popular generators.
 * Feed management reads the saved feeds via wf_agent_get_saved_feeds and
 * writes the preference through putPreferences.
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

#ifdef COBALT_HAS_WOLFRAM

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
/* Saved feeds v2: the account's saved custom feeds, via Wolfram */

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

    /* The saved-feeds preference walk is Wolfram's (wolfram/saved_feeds.h). */
    wf_saved_feed feeds[COBALT_SAVED_FEEDS_MAX];
    size_t n = 0;
    const wf_status status =
        wf_agent_get_saved_feeds(g_session.wf, feeds, COBALT_SAVED_FEEDS_MAX, &n);
    if (status != WF_OK) {
        COBALT_LOGW("session: saved feeds v2 fetch failed (%d)", (int) status);
        cobalt_session_set_message(r, "Could not load saved feeds (wolfram status %d).",
                                   (int) status);
        *state = COBALT_AUTH_SIGNED_IN;
        return;
    }

    SDL_LockMutex(g_session.lock);
    memset(&s_saved_feeds_v2, 0, sizeof(s_saved_feeds_v2));
    for (size_t i = 0; i < n && s_saved_feeds_v2.count < COBALT_SAVED_FEEDS_MAX; i++) {
        cobalt_saved_feed_entry *e = &s_saved_feeds_v2.entries[s_saved_feeds_v2.count++];
        snprintf(e->uri, sizeof e->uri, "%s", feeds[i].uri);
        snprintf(e->display_name, sizeof e->display_name, "%s", feeds[i].name);
        e->pinned = false;
    }
    SDL_UnlockMutex(g_session.lock);

    COBALT_LOGI("session: loaded %d saved feeds v2", s_saved_feeds_v2.count);
    *state = COBALT_AUTH_SIGNED_IN;
    r->ok = true;
}

/* ------------------------------------------------------------------ */
/* Save/remove/pin feed: write the v2 saved-feeds preference */

/* Build the v2 saved-feeds items array from the current state plus the
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

    /* putPreferences replaces the whole preferences array, so round-trip the
     * current one and swap in the modified v2 saved-feeds preference. */
    char *cur_json = NULL;
    cJSON *prefs = NULL;
    if (wf_agent_get_preferences(g_session.wf, &cur_json) == WF_OK && cur_json) {
        prefs = cJSON_Parse(cur_json);
        free(cur_json);
    }
    if (!prefs || !cJSON_IsArray(prefs)) {
        if (prefs) cJSON_Delete(prefs);
        prefs = cJSON_CreateArray();
    }
    cJSON *v2 = cJSON_CreateObject();
    cJSON_AddItemToObject(v2, "$type", cJSON_CreateString("app.bsky.actor.defs#savedFeedsPrefV2"));
    cJSON_AddItemToObject(v2, "items", items);
    /* remove any existing v2 saved-feeds pref, then append the new one */
    for (int i = cJSON_GetArraySize(prefs) - 1; i >= 0; i--) {
        cJSON *it = cJSON_GetArrayItem(prefs, i);
        cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "$type");
        if (ty && cJSON_IsString(ty) && ty->valuestring &&
            strstr(ty->valuestring, "savedFeedsPrefV2")) {
            cJSON_DeleteItemFromArray(prefs, i);
        }
    }
    cJSON_AddItemToArray(prefs, v2);

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

    /* putPreferences replaces the whole preferences array, so round-trip the
     * current one and swap in the modified v2 saved-feeds preference. */
    char *cur_json = NULL;
    cJSON *prefs = NULL;
    if (wf_agent_get_preferences(g_session.wf, &cur_json) == WF_OK && cur_json) {
        prefs = cJSON_Parse(cur_json);
        free(cur_json);
    }
    if (!prefs || !cJSON_IsArray(prefs)) {
        if (prefs) cJSON_Delete(prefs);
        prefs = cJSON_CreateArray();
    }
    cJSON *v2 = cJSON_CreateObject();
    cJSON_AddItemToObject(v2, "$type", cJSON_CreateString("app.bsky.actor.defs#savedFeedsPrefV2"));
    cJSON_AddItemToObject(v2, "items", items);
    /* remove any existing v2 saved-feeds pref, then append the new one */
    for (int i = cJSON_GetArraySize(prefs) - 1; i >= 0; i--) {
        cJSON *it = cJSON_GetArrayItem(prefs, i);
        cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "$type");
        if (ty && cJSON_IsString(ty) && ty->valuestring &&
            strstr(ty->valuestring, "savedFeedsPrefV2")) {
            cJSON_DeleteItemFromArray(prefs, i);
        }
    }
    cJSON_AddItemToArray(prefs, v2);

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

#endif /* COBALT_HAS_WOLFRAM */

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