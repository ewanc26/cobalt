#pragma once

/*
 * What the session's files share and nothing else may use: the job a request
 * carries in, the one state block behind the lock, and the few helpers the
 * job runners call back into. The public interface is atproto/session.h.
 *
 * The session is split by what a job is about, not by layer: session.c owns
 * the worker thread, the lock, init and the queue; session_auth.c signs in and
 * out; session_read.c reads feeds, threads, notifications and post search;
 * session_profile.c is a profile and what you do to it; session_lists.c is
 * people lists, curated lists and saved feeds; session_post.c writes. Each
 * holds the runner (off the frame loop, Wolfram only) and the begin_ call (on
 * it) for its jobs.
 */

#include "atproto/session.h"
#include "atproto/actors.h"
#include "atproto/feed.h"
#include "atproto/prefs.h"
#include "atproto/notifications.h"
#include "atproto/actor_profile.h"
#include "cache/session_store.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/rng.h"

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/actor_prefs_typed.h>
#include <wolfram/actor_typed.h>
#include <wolfram/feed_gen_typed.h>
#include <wolfram/agent.h>
#include <wolfram/attach.h>
#include <wolfram/embed.h>
#include <wolfram/failure.h>
#include <wolfram/feed_typed.h>
#include <wolfram/graph_typed.h>
#include <wolfram/list_typed.h>
#include <wolfram/moderation_typed.h>
#include <wolfram/oauth_pairing.h>
#include <wolfram/saved_feeds.h>
#include <wolfram/session.h>
#include <wolfram/thread_typed.h>
#include <wolfram/threadgate_postgate.h>
#include <wolfram/time.h>
#include <wolfram/xrpc.h>
#endif

#include <SDL.h>
#include <cJSON.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* What a job carries in. Copied out of the request under the lock so the
 * worker never reads a buffer the main thread might be rewriting. */
typedef struct {
   char service[COBALT_SERVICE_MAX];
   char identifier[COBALT_IDENTIFIER_MAX];
   char password[COBALT_PASSWORD_MAX];
   /* Timeline only: append the next page rather than replacing the feed. */
   bool paging;

   /* Interactions and thread fetches: which post, and for an undo, which
    * record of the viewer's own to delete. */
   char uri[COBALT_POST_URI_MAX];
   char cid[COBALT_POST_CID_MAX];
   char record_uri[COBALT_POST_URI_MAX];

   /* Mute only: the target state (true = mute, false = unmute). Unlike
    * follow/block, mute has no record URI for record_uri's emptiness to
    * signal an undo with, so the direction is carried explicitly. */
   bool flag;

   /* A thread of new posts: how many, with the texts in s.thread_texts (too big
    * to copy with every job; only one job runs at a time and a new one is
    * refused while one is in flight). Zero for everything else. */
   int thread_count;

   /* Composing. `text` is the post body; the refs are empty for a new post. */
   char text[COBALT_COMPOSE_TEXT_MAX];
   char root_uri[COBALT_POST_URI_MAX];
   char root_cid[COBALT_POST_CID_MAX];

   /* Reply-control for a new top-level post; ignored on a reply. Matches
    * `cobalt_reply_gate`: 0 everyone, 1 followed/mentioned, 2 nobody. */
   int reply_gate;

   /* Profile tab switches: a cobalt_profile_tab. */
   int tab;

   /* Composing a quote post: `uri`/`cid` name the quoted post, not a parent. */
   bool quote;

   /* Optional image to attach (SD path); empty for none. */
   char attach_path[COBALT_ATTACH_PATH_MAX];
   char attach_alt[COBALT_ATTACH_ALT_MAX];
} cobalt_job_input;

typedef struct {
   bool initialised;

   /* The texts of a thread being posted; see job_input.thread_count. */
   char thread_texts[COBALT_THREAD_POSTS_MAX][COBALT_COMPOSE_TEXT_MAX];

   /* Set once at init and read-only afterwards, so no locking. */
   char ca_path[COBALT_PATH_MAX];
   bool have_ca;

   /* The loaded profile's pinned post, so a tab switch can re-add it. */
   char pinned_uri[COBALT_POST_URI_MAX];

   /* Index into POST_LANGS; 0 is "none". Applied to each new agent. */
   int post_lang;
   const char *blocker;

   SDL_mutex *lock;
   SDL_cond *wake;
   SDL_Thread *thread;
   bool stop;

   cobalt_job_kind pending;
   cobalt_job_input input;

   bool busy;
   bool have_result;
   cobalt_job_result result;

   /* Published by the worker, read by the UI. Guarded by `lock`. */
   cobalt_auth_state state;
   /* What `state` was before the in-flight request set it to WORKING. A job
    * that fails leaves it alone, so a failed refresh does not sign the user
    * out and a failed sign-in does not claim they are signed in. */
   cobalt_auth_state resting_state;
   char handle[COBALT_HANDLE_MAX];
   char did[COBALT_DID_MAX];
   char service[COBALT_SERVICE_MAX];
   char pair_url[COBALT_OAUTH_URL_MAX];
   char pair_code[COBALT_OAUTH_CODE_MAX];

   /* Published by the worker alongside the auth state, and read by the
    * timeline screen. Guarded by `lock` on write, read while idle. */
   cobalt_feed feed;
   /* Named to stay clear of `thread`, which is the worker. */
   cobalt_thread conversation;
   cobalt_notifications notifications;
   cobalt_profile profile;
   cobalt_feed author_feed;
   int profile_tab;
   cobalt_actor_list muted;
   cobalt_actor_list blocked;
   cobalt_actor_list followers;
   cobalt_actor_list following;
   char follow_actor[COBALT_POST_URI_MAX];
   /* getLikes/getRepostedBy share this one list: only one is on screen at
    * a time, and a fetch always resets it, so there is no cross-talk. */
   cobalt_actor_list likes;
   char likes_uri[COBALT_POST_URI_MAX];
   /* What the shared feed window holds, so refresh and paging re-fetch the
    * same source. Written and read on the UI thread only. */
   int feed_source;            /* 0 home, 1 custom feed, 2 post search */
   char feed_arg[COBALT_POST_URI_MAX];
   cobalt_actor_list search;
   cobalt_list_summary_list lists;
   cobalt_saved_feeds saved_feeds;
   cobalt_actor_list list_members;

#ifdef COBALT_HAS_WOLFRAM
   /* Owned by the worker while a job runs; only touched off-thread when idle. */
   wf_agent *wf;
#endif

   /* Worker-only: the account's muted words and hide-reposts, fetched once per
    * sign-in. */
   cobalt_prefs prefs;
   bool prefs_loaded;
} cobalt_session_core;

extern cobalt_session_core g_session;

/*
 * How many posts to ask for per page. The window holds 60, and a smaller page
 * gets something on screen sooner on a console whose upstream is often a slow
 * wireless link — the cost of a second request is far less than the cost of
 * staring at a blank feed.
 */
#define COBALT_SESSION_PAGE 20

/* Core, for the runners. */
void cobalt_session_set_message(cobalt_job_result *r, const char *fmt, ...)
   __attribute__((format(printf, 2, 3)));
bool cobalt_session_submit(cobalt_job_kind kind, const cobalt_job_input *in);

#ifdef COBALT_HAS_WOLFRAM
/* Core, for the runners (they need Wolfram's status type). */
void cobalt_session_describe_failure(cobalt_job_result *r, wf_status status, cobalt_job_kind kind);
bool cobalt_session_publish(void);

/* The runners, called by the worker through run_job in session.c. Each runs
 * off the frame loop and takes `state` in and out; see run_job. */
void cobalt_session_run_block(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_blocked_list(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_delete_post(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_feed(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_follow(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_followers(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_following(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_interaction(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state, bool is_like);
void cobalt_session_run_likes(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_list_members(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_lists(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_mute(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_muted_list(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_notifications(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_post(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_profile(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_profile_tab(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_reposted_by(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_saved_feeds(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_search_actors(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_search_posts(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_thread(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_timeline(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);

/* session_auth.c: sign-in, sign-out and the agent they build. */
void cobalt_session_run_login(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_oauth(const cobalt_job_input *in, cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_resume(cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_run_logout(cobalt_job_result *r, cobalt_auth_state *state);
void cobalt_session_teardown_agent(void);
#endif
