#include "app/app_internal.h"
#include "app/queue_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The offline queue's file in the app's data directory. */
static bool
queue_file(char *out, size_t size)
{
   return cobalt_data_path(out, size, "queue.dat");
}

/* Restore the queue from the SD card at start. A damaged file is kept beside
 * the new one as queue.dat.bad rather than overwritten, so nothing the person
 * saved is destroyed by a bad write. */
static void
load_queue(cobalt_app *app)
{
   char path[256];
   char bad[272];

   cobalt_post_queue_init(&app->queue);
   if (!queue_file(path, sizeof(path))) {
      return;
   }
   if (!cobalt_queue_load(path, &app->queue)) {
      snprintf(bad, sizeof(bad), "%s.bad", path);
      rename(path, bad);
   }
}

/* Focus animation, in units of "fraction of the way there per frame". */
#define FOCUS_RATE 0.25f

cobalt_app *
cobalt_app_create(void)
{
   cobalt_app *app = (cobalt_app *) calloc(1, sizeof(cobalt_app));
   if (!app) {
      COBALT_LOGE("out of memory allocating app state");
      return NULL;
   }

   app->screen = COBALT_SCREEN_HOME;
   app->display = COBALT_DISPLAY_DUAL;
   app->selected = 1; /* Sign in — the one thing worth doing on run one. */

   cobalt_signin_init(&app->signin);

   /* An install made without `make bundle` has no entropy seed, and without one
    * Cobalt will not use the network: ask for one before anything else. */
   {
      char seed_path[256];
      if (cobalt_atproto_needs_seed() && cobalt_atproto_seed_path(seed_path, sizeof(seed_path))) {
         const uint64_t extra[2] = {(uint64_t) SDL_GetPerformanceCounter(), (uint64_t) SDL_GetTicks()};
         cobalt_entropy_view_init(&app->entropy, seed_path, extra, sizeof(extra));
         app->screen = COBALT_SCREEN_ENTROPY;
      }
   }
   cobalt_timeline_init(&app->timeline);
   cobalt_thread_view_init(&app->thread);
   cobalt_notify_view_init(&app->notify);
   cobalt_messages_view_init(&app->messages);
   cobalt_chat_thread_view_init(&app->chat_view);
   cobalt_profile_view_init(&app->profile);
   cobalt_graph_view_init(&app->graph);
   cobalt_search_view_init(&app->search);
   cobalt_lists_view_init(&app->lists);
   cobalt_queue_view_open(&app->queue_view);
   load_queue(app);

   curl_version_info_data *curl_info = curl_version_info(CURLVERSION_NOW);
   snprintf(app->curl_version, sizeof(app->curl_version), "curl %s / %s",
            curl_info ? curl_info->version : "?",
            (curl_info && curl_info->ssl_version) ? curl_info->ssl_version : "no TLS");

   SDL_version linked;
   SDL_GetVersion(&linked);
   snprintf(app->sdl_version, sizeof(app->sdl_version), "SDL %u.%u.%u",
            linked.major, linked.minor, linked.patch);

   COBALT_LOGI("app up: %s, %s", app->sdl_version, app->curl_version);
   return app;
}

/* A thread or profile opened from `from` pushes it, so B from the new screen
 * goes back to it. When the stack is full the oldest return is dropped rather
 * than the newest, which is the one B is about to need. */
/* The uri of the post the thread on screen was opened on, which is what
 * cobalt_session_begin_thread takes to fetch that conversation again. Empty
 * when no thread is loaded. Read it before a new thread is requested, since
 * the request replaces the one on screen. */
void
cobalt_app_current_thread_uri(char *out, size_t cap)
{
   const cobalt_thread *thread = cobalt_session_thread();

   if (!out || cap == 0) {
      return;
   }
   out[0] = '\0';
   if (thread->focus >= 0 && thread->focus < thread->count) {
      snprintf(out, cap, "%s", thread->posts[thread->focus].uri);
   }
}

/* A thread or profile opened from `from` pushes it, so B from the new screen
 * goes back to it. `thread_uri` is the conversation `from` showed, when it was
 * a thread. When the stack is full the oldest return is dropped rather than the
 * newest, which is the one B is about to need. */
void
cobalt_app_push_return(cobalt_app *app, cobalt_screen from, const char *thread_uri)
{
   cobalt_return *entry;

   if (!app) {
      return;
   }
   if (app->return_depth == COBALT_RETURN_DEPTH) {
      memmove(app->return_stack, app->return_stack + 1,
              (COBALT_RETURN_DEPTH - 1) * sizeof(app->return_stack[0]));
      app->return_depth--;
   }
   entry = &app->return_stack[app->return_depth++];
   entry->screen = from;
   snprintf(entry->thread_uri, sizeof entry->thread_uri, "%s", thread_uri ? thread_uri : "");
}

/* Returns the screen to go back to. Going back to a thread also fetches that
 * conversation again, since the session may hold another one by now. */
cobalt_screen
cobalt_app_pop_return(cobalt_app *app)
{
   const cobalt_return *entry;

   if (!app || app->return_depth == 0) {
      return COBALT_SCREEN_HOME;
   }
   entry = &app->return_stack[--app->return_depth];
   if (entry->screen == COBALT_SCREEN_THREAD && entry->thread_uri[0]) {
      cobalt_session_begin_thread(entry->thread_uri);
   }
   return entry->screen;
}

cobalt_screen
cobalt_app_screen(const cobalt_app *app)
{
   return app ? app->screen : COBALT_SCREEN_HOME;
}

void
cobalt_app_view_position(const cobalt_app *app, cobalt_screen screen, int *selected,
                         int *scroll)
{
   int sel = 0;
   int scr = 0;

   if (app) {
      switch (screen) {
         case COBALT_SCREEN_TIMELINE:
            sel = app->timeline.nav.selected;
            scr = app->timeline.nav.scroll;
            break;
         case COBALT_SCREEN_NOTIFICATIONS:
            sel = app->notify.nav.selected;
            scr = app->notify.nav.scroll;
            break;
         case COBALT_SCREEN_PROFILE:
            sel = app->profile.nav.selected;
            scr = app->profile.nav.scroll;
            break;
         default:
            break;
      }
   }
   if (selected) {
      *selected = sel;
   }
   if (scroll) {
      *scroll = scr;
   }
}

void
cobalt_app_timeline_position(const cobalt_app *app, int *selected, int *scroll)
{
   cobalt_app_view_position(app, COBALT_SCREEN_TIMELINE, selected, scroll);
}

const cobalt_popup *
cobalt_app_popup(const cobalt_app *app)
{
   return app && app->popup.open ? &app->popup : NULL;
}

int
cobalt_app_home_selection(const cobalt_app *app)
{
   return app ? app->selected : 0;
}

void
cobalt_app_destroy(cobalt_app *app)
{
   if (app) {
      /* The password buffer lives in this allocation; do not hand it back to
       * the heap still holding one. */
      cobalt_signin_clear_password(&app->signin);
   }
   free(app);
}

bool
cobalt_app_should_quit(const cobalt_app *app)
{
   return app && app->quit;
}

static void
set_notice(cobalt_app *app, const char *message, bool is_error)
{
   snprintf(app->notice, sizeof(app->notice), "%s", message ? message : "");
   app->notice_is_error = is_error;
}

/* --- session plumbing --- */

/* Mirror the queue to the SD card. A failed write is said out loud, because the
 * post then lives only until the app closes and the person should know that. */
static void
persist_queue(cobalt_app *app)
{
   char path[256];

   if (!queue_file(path, sizeof(path)) || !cobalt_queue_save(path, &app->queue)) {
      set_notice(app, "Could not save the queue to the SD card.", true);
   }
}

/* Start sending the queued post at `index`. It is held in queue_inflight until
 * the post's result arrives, and the SD copy is only rewritten then: if the app
 * closes mid-send, the post is still in the file and comes back to the queue. */
static void
queue_send(cobalt_app *app, unsigned index)
{
   cobalt_queued_post post;

   if (app->queue_send_pending) {
      set_notice(app, "A queued post is already being sent.", true);
      return;
   }
   if (!cobalt_post_queue_take(&app->queue, index, &post)) {
      return;
   }
   if (!cobalt_session_begin_post(post.text, "", "", "", "", 0, "", "")) {
      /* It never started, so it goes back where the queue can still hold it. */
      cobalt_post_queue_add(&app->queue, post.text, post.queued_at);
      set_notice(app, "Could not start that post. It is still queued.", true);
      return;
   }
   app->queue_inflight = post;
   app->queue_send_pending = true;
   set_notice(app, "Sending...", false);
}

static void
cobalt_app_update_queue(cobalt_app *app, const cobalt_input *in)
{
   unsigned index = 0;

   switch (cobalt_queue_view_update(&app->queue_view, &app->queue, in, &index)) {
      case COBALT_QUEUE_BACK:
         app->screen = COBALT_SCREEN_HOME;
         break;
      case COBALT_QUEUE_SEND:
         queue_send(app, index);
         break;
      case COBALT_QUEUE_DISCARD:
         if (cobalt_post_queue_discard(&app->queue, index)) {
            persist_queue(app);
            set_notice(app, "Discarded.", false);
         }
         break;
      default:
         break;
   }
}

static void
handle_job_result(cobalt_app *app, const cobalt_job_result *result)
{
   switch (result->kind) {
      case COBALT_JOB_OAUTH:
      case COBALT_JOB_LOGIN:
         if (result->ok) {
            /* Wolfram has its own copy now, so this one has no reason to live
             * any longer — and it is about to sit in an idle screen's state. */
            cobalt_signin_clear_password(&app->signin);
            cobalt_signin_set_status(&app->signin, "", false);

            /* Land on the feed, not on a confirmation screen. Signing in is a
             * means to an end, and the account details are one menu entry
             * away for anyone who wants them. */
            cobalt_timeline_rewind(&app->timeline);
            app->screen = COBALT_SCREEN_TIMELINE;
            cobalt_session_begin_timeline(false);
            app->viewing_custom_feed = false;
            app->viewing_search = false;

            char message[COBALT_MESSAGE_MAX];
            if (result->message[0]) {
               /* Signed in, but with a caveat worth repeating verbatim. */
               snprintf(message, sizeof(message), "%s", result->message);
               set_notice(app, message, true);
            } else {
               snprintf(message, sizeof(message), "Signed in as %s",
                        cobalt_session_handle());
               set_notice(app, message, false);
            }
         } else {
            cobalt_signin_set_status(&app->signin, result->message, true);
         }
         break;

      case COBALT_JOB_RESUME:
         if (result->ok) {
            char message[COBALT_MESSAGE_MAX];
            snprintf(message, sizeof(message), "Signed in as %s",
                     cobalt_session_handle());
            set_notice(app, message, false);
            /* Warm the feed while the user is still looking at the menu, so
             * opening it is instant rather than a spinner. */
            cobalt_timeline_rewind(&app->timeline);
            cobalt_session_begin_timeline(false);
            app->viewing_custom_feed = false;
            app->viewing_search = false;
         } else {
            /* A failed resume is not an error the user asked for, so it lands
             * on the home screen as a notice rather than throwing them into
             * the sign-in form. */
            set_notice(app, result->message, true);
         }
         break;

      case COBALT_JOB_LOGOUT:
         set_notice(app, "Signed out.", false);
         cobalt_signin_init(&app->signin);
         cobalt_timeline_init(&app->timeline);
   cobalt_thread_view_init(&app->thread);
   cobalt_notify_view_init(&app->notify);
   cobalt_messages_view_init(&app->messages);
   cobalt_chat_thread_view_init(&app->chat_view);
   cobalt_profile_view_init(&app->profile);
   cobalt_graph_view_init(&app->graph);
   cobalt_search_view_init(&app->search);
   cobalt_lists_view_init(&app->lists);
         app->screen = COBALT_SCREEN_HOME;
         app->selected = 1;
         break;

      case COBALT_JOB_POST:
         if (app->queue_send_pending) {
            /* A queued post, not one from the compose screen: the queue decides
             * what happens to it, and the compose screen is left alone. */
            app->queue_send_pending = false;
            if (result->ok) {
               persist_queue(app);
               set_notice(app, "Sent from the queue.", false);
            } else {
               cobalt_post_queue_add(&app->queue, app->queue_inflight.text,
                                     app->queue_inflight.queued_at);
               persist_queue(app);
               set_notice(app, result->message[0]
                                  ? result->message
                                  : "That post did not send. It is still queued.",
                          true);
            }
            app->screen = COBALT_SCREEN_QUEUE;
            break;
         }
         if (result->ok) {
            if (result->partial) {
               set_notice(app, result->message, true);
            } else {
               set_notice(app, cobalt_compose_is_reply(&app->compose)
                                  ? "Reply posted."
                               : cobalt_compose_is_quote(&app->compose)
                                  ? "Quote posted."
                               : app->compose.thread_count > 0
                                  ? "Thread posted." : "Posted.", false);
            }
            /* Back to where composing started, and refresh so the new post is
             * actually visible rather than only claimed. */
            app->screen = app->compose_return;
            if (app->compose_return == COBALT_SCREEN_THREAD &&
                app->compose.parent_uri[0]) {
               /* The refetch re-roots on the parent, which is usually a much
                * shorter conversation than the one being read — without this
                * the cursor stays where it was and lands past the end. */
               cobalt_thread_view_reset(&app->thread);
               cobalt_session_begin_thread(app->compose.parent_uri);
            } else {
               cobalt_session_begin_timeline(false);
               app->viewing_custom_feed = false;
            app->viewing_search = false;
               cobalt_timeline_rewind(&app->timeline);
            }
            cobalt_compose_init(&app->compose);
         } else {
            /* Stay on the compose screen with the text intact — a failed post
             * must not silently eat something someone typed on a D-pad. */
            set_notice(app, result->message, true);
         }
         break;

      case COBALT_JOB_DELETE_POST:
         if (result->ok) {
            set_notice(app, "Post deleted.", false);
            /* The loaded conversation was cleared if it held the post, so a
             * thread screen has nothing left to show. */
            if (app->screen == COBALT_SCREEN_THREAD) {
               app->screen = cobalt_app_pop_return(app);
            }
         } else {
            set_notice(app, result->message, true);
         }
         break;

      case COBALT_JOB_CHAT_SEND:
         /* A sent reply clears the draft and reloads the thread so it shows. A
          * failed one keeps the draft, so the person can send it again. */
         if (result->ok) {
            cobalt_chat_thread_view_sent(&app->chat_view);
            cobalt_session_begin_chat_messages(app->chat_view.convo_id);
         } else {
            set_notice(app, result->message, true);
         }
         break;

      case COBALT_JOB_CHAT_START: {
         /* The name is read first: a finished start clears the field. */
         char name[COBALT_CHAT_TARGET_MAX];
         cobalt_messages_start_name(&app->messages.start, name, sizeof(name));
         const cobalt_messages_start_outcome outcome =
            cobalt_messages_start_finished(&app->messages.start, result->ok,
                                           cobalt_session_chat_started_convo());
         if (outcome == COBALT_MESSAGES_START_OPEN) {
            const char *convo = cobalt_session_chat_started_convo();
            if (app->screen != COBALT_SCREEN_MESSAGES) {
               /* The person left Messages while the job ran. Do not pull them
                * into a thread they did not ask to see. */
               set_notice(app, "Conversation started. Refresh Messages to see it.", false);
            } else if (cobalt_session_begin_chat_messages(convo)) {
               /* A conversation started from the inbox is one-to-one. */
               cobalt_chat_thread_view_open(&app->chat_view, name, convo, false);
               app->screen = COBALT_SCREEN_CHAT_THREAD;
               COBALT_LOGI("messages: opened a new conversation");
            } else {
               set_notice(app, "Conversation started, but it could not be opened. "
                               "Refresh Messages, then open it.", true);
            }
         } else if (outcome == COBALT_MESSAGES_START_FAILED) {
            set_notice(app, result->message[0] ? result->message
                                               : "Could not open a conversation with that account.",
                       true);
         }
         break;
      }

      case COBALT_JOB_CHAT_ACCEPT:
      case COBALT_JOB_CHAT_DECLINE: {
         /* The name and id are copied out first: a finished decision clears
          * nothing, but the request is gone from the list by now. */
         char id[COBALT_CHAT_ID_MAX];
         char name[COBALT_POST_NAME_MAX];
         snprintf(id, sizeof(id), "%s", app->messages.decision.convo_id);
         snprintf(name, sizeof(name), "%s", app->messages.decision.name);
         const cobalt_messages_decision_outcome outcome =
            cobalt_messages_decision_finished(&app->messages, result->ok);
         if (outcome == COBALT_MESSAGES_DECISION_ACCEPTED) {
            if (app->screen != COBALT_SCREEN_MESSAGES) {
               set_notice(app, "Request accepted. Open Messages to see it.", false);
            } else if (cobalt_session_begin_chat_messages(id)) {
               cobalt_chat_thread_view_open(&app->chat_view, name, id,
                                            app->messages.decision.group);
               app->screen = COBALT_SCREEN_CHAT_THREAD;
               COBALT_LOGI("messages: accepted a request and opened it");
            } else {
               set_notice(app, "Request accepted, but it could not be opened. "
                               "Open Messages, then open it.", true);
            }
         } else if (outcome == COBALT_MESSAGES_DECISION_DECLINED) {
            set_notice(app, "Request declined.", false);
         } else if (outcome == COBALT_MESSAGES_DECISION_FAILED) {
            set_notice(app, result->message[0] ? result->message
                                               : "Could not update that request.",
                       true);
         }
         break;
      }

      case COBALT_JOB_CHAT_JOIN_APPROVE:
      case COBALT_JOB_CHAT_JOIN_REJECT: {
         /* The row has already left the loaded list; this only says which way
          * the decision went, or why it did not. */
         const cobalt_chat_thread_decision_outcome outcome =
            cobalt_chat_thread_decision_finished(&app->chat_view, result->ok);
         if (outcome == COBALT_CHAT_THREAD_DECISION_APPROVED) {
            set_notice(app, "Request approved.", false);
         } else if (outcome == COBALT_CHAT_THREAD_DECISION_REJECTED) {
            set_notice(app, "Request rejected.", false);
         } else if (outcome == COBALT_CHAT_THREAD_DECISION_FAILED) {
            set_notice(app, result->message[0] ? result->message
                                               : "Could not update that join request.",
                       true);
         }
         break;
      }

      case COBALT_JOB_NOTIFICATIONS:
      case COBALT_JOB_CHAT_INBOX:
      case COBALT_JOB_CHAT_MESSAGES:
      case COBALT_JOB_CHAT_JOIN_REQUESTS:
      case COBALT_JOB_PROFILE:
      case COBALT_JOB_FOLLOW:
      case COBALT_JOB_THREAD:
      case COBALT_JOB_LIKE:
      case COBALT_JOB_REPOST:
         /* Success is visible in the card itself — the count moved and the
          * marker appeared — so only failures are worth saying out loud. */
         if (!result->ok) {
            set_notice(app, result->message, true);
         } else if (result->message[0]) {
            set_notice(app, result->message, false);
         }
         break;

      case COBALT_JOB_TIMELINE:
         if (!result->ok) {
            /* A notice rather than an error screen: a failed refresh should
             * leave whatever was already on screen readable. */
            set_notice(app, result->message, true);
         } else {
            /* Carries the "your timeline is empty" explanation on success. */
            set_notice(app, result->message, result->message[0] != '\0');
         }
         break;

      case COBALT_JOB_NONE:
      default:
         break;
   }
}


/* The header's back pill, the Miiverse corner button. Screens where B types
 * (compose, sign-in, search) keep the keyboard's own Cancel instead. */
static SDL_Rect s_back_hit;
static bool s_back_hit_valid = false;

static bool
screen_has_back_pill(cobalt_screen s)
{
   return s != COBALT_SCREEN_HOME && s != COBALT_SCREEN_COMPOSE &&
          s != COBALT_SCREEN_SIGN_IN && s != COBALT_SCREEN_SEARCH &&
          /* Strokes land anywhere on the panel; a corner pill would end setup. */
          s != COBALT_SCREEN_ENTROPY;
}

static void
draw_back_pill(cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int w = cobalt_pill_width(r, "B", "Back");
   SDL_Rect pill = { m->width - m->pad_edge - w, m->pad_edge, w,
                     cobalt_pill_height(r) };
   cobalt_draw_pill(r, "B", "Back", pill.x, pill.y);
   if (surface == COBALT_SURFACE_DRC) {
      s_back_hit = pill;
      s_back_hit_valid = true;
   }
}

static void
app_update_inner(cobalt_app *app, const cobalt_input *in, uint32_t now_ms)
{
   if (!app || !in) {
      return;
   }

   (void) now_ms;
   app->frames++;

   cobalt_input tapped_back;
   if (s_back_hit_valid && screen_has_back_pill(app->screen) &&
       cobalt_input_tapped(in, &s_back_hit)) {
      tapped_back = *in;
      tapped_back.pressed[COBALT_BTN_BACK] = true;
      tapped_back.touch_ended = false;
      in = &tapped_back;
   }

   /* A tap on a hint pill (the "A: thread" style prompts) is that button.
    * Routed after the overlay gates below, not here: the image viewer and the
    * popup are both drawn over the screen's own footer, so a tap on a pill
    * while one of them is up belongs to the overlay. Pressing the button as
    * well would dismiss the popup *and* act on what was underneath it. */
   cobalt_input tapped_hint;
   const cobalt_input *hint_in = in;

   if (in->quit_requested) {
      app->quit = true;
      return;
   }

   if (!app->resume_attempted) {
      app->resume_attempted = true;
      if (cobalt_session_available() && cobalt_session_has_saved()) {
         COBALT_LOGI("app: stored session found, resuming");
         cobalt_session_begin_resume();
      }
   }

   cobalt_job_result result;
   if (cobalt_session_poll(&result)) {
      COBALT_LOGI("app: job %d finished ok=%d", (int) result.kind, (int) result.ok);
      handle_job_result(app, &result);
   }

   /*
    * Screens read the worker's feed, thread and notification buffers directly,
    * and the worker rewrites them in place while they are on screen — a like
    * moves a count, a refresh clears and refills the list. Holding the session
    * lock across the whole update is what makes that safe; the lock is never
    * held across network I/O, so this costs nothing.
    */
   cobalt_session_lock();

   /* The viewer is an overlay over whatever screen opened it, so it takes
    * the input before the popup and the screen both. Its update returns
    * whether it consumed the frame; the screen underneath must not also act
    * on the same presses. */
   if (app->imageview.open) {
      cobalt_imageview_update(&app->imageview, in);
      cobalt_session_unlock();
      return;
   }

   if (app->popup.open) {
      const int chosen = cobalt_popup_update(&app->popup, in);
      if (chosen >= 0) {
         cobalt_app_popup_choose(app, chosen);
      }
      cobalt_session_unlock();
      return;
   }

   {
      cobalt_button btns[4];
      const int n = cobalt_hints_tapped(in, btns, 4);
      if (n > 0) {
         tapped_hint = *in;
         for (int i = 0; i < n; i++) {
            tapped_hint.pressed[btns[i]] = true;
         }
         tapped_hint.touch_ended = false;
         hint_in = &tapped_hint;
      }
   }
   in = hint_in;

   switch (app->screen) {
      case COBALT_SCREEN_ENTROPY:
         switch (cobalt_entropy_view_update(&app->entropy, in, (uint32_t) SDL_GetPerformanceCounter())) {
            case COBALT_ENTROPY_VIEW_SKIP:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_ENTROPY_VIEW_QUIT:
               app->quit = true;
               break;
            case COBALT_ENTROPY_VIEW_STAY:
               break;
         }
         break;

      case COBALT_SCREEN_DIAGNOSTICS:
         if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
            app->screen = COBALT_SCREEN_HOME;
         }
         break;

      case COBALT_SCREEN_PROFILE:
         switch (cobalt_profile_view_update(&app->profile, in)) {
            case COBALT_PROFILE_VIEW_BACK:
               app->screen = cobalt_app_pop_return(app);
               break;
            case COBALT_PROFILE_VIEW_OPEN_THREAD:
               cobalt_thread_view_reset(&app->thread);
               cobalt_app_push_return(app, COBALT_SCREEN_PROFILE, NULL);
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_PROFILE_VIEW_MENU:
               cobalt_app_open_profile_menu(app);
               break;
            case COBALT_PROFILE_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_TIMELINE:
         switch (cobalt_timeline_update(&app->timeline, in)) {
            case COBALT_TIMELINE_BACK:
               app->screen = app->viewing_custom_feed ? COBALT_SCREEN_FEEDS
                                                      : COBALT_SCREEN_HOME;
               break;
            case COBALT_TIMELINE_OPEN_THREAD:
               cobalt_thread_view_reset(&app->thread);
               cobalt_app_push_return(app, COBALT_SCREEN_TIMELINE, NULL);
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_TIMELINE_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               cobalt_app_push_return(app, COBALT_SCREEN_TIMELINE, NULL);
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_TIMELINE_COMPOSE:
               cobalt_compose_init(&app->compose);
               app->compose_return = COBALT_SCREEN_TIMELINE;
               app->screen = COBALT_SCREEN_COMPOSE;
               break;
            case COBALT_TIMELINE_MENU: {
               const cobalt_feed *feed = cobalt_session_feed();
               if (app->timeline.nav.selected < feed->count) {
                  cobalt_app_open_post_menu(app, &feed->posts[app->timeline.nav.selected], false);
               }
               break;
            }
            case COBALT_TIMELINE_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_THREAD:
         switch (cobalt_thread_view_update(&app->thread, in)) {
            case COBALT_THREAD_VIEW_BACK:
               app->screen = cobalt_app_pop_return(app);
               break;
            case COBALT_THREAD_VIEW_QUOTE: {
               const cobalt_thread *conv = cobalt_session_thread();
               if (app->thread.nav.selected < conv->count) {
                  cobalt_compose_quote(&app->compose,
                                       &conv->posts[app->thread.nav.selected]);
                  if (cobalt_compose_is_quote(&app->compose)) {
                     app->compose_return = COBALT_SCREEN_THREAD;
                     app->screen = COBALT_SCREEN_COMPOSE;
                  }
               }
               break;
            }
            case COBALT_THREAD_VIEW_REPLY: {
               const cobalt_thread *conv = cobalt_session_thread();
               if (app->thread.nav.selected < conv->count) {
                  cobalt_compose_reply_to(&app->compose,
                                          &conv->posts[app->thread.nav.selected]);
                  app->compose_return = COBALT_SCREEN_THREAD;
                  app->screen = COBALT_SCREEN_COMPOSE;
               }
               break;
            }
            case COBALT_THREAD_VIEW_MENU: {
               const cobalt_thread *conv = cobalt_session_thread();
               if (app->thread.nav.selected < conv->count) {
                  cobalt_app_open_post_menu(app, &conv->posts[app->thread.nav.selected], true);
               }
               break;
            }
            case COBALT_THREAD_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_NOTIFICATIONS:
         switch (cobalt_notify_view_update(&app->notify, in)) {
            case COBALT_NOTIFY_BACK:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_NOTIFY_OPEN_THREAD:
               cobalt_thread_view_reset(&app->thread);
               cobalt_app_push_return(app, COBALT_SCREEN_NOTIFICATIONS, NULL);
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_NOTIFY_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               cobalt_app_push_return(app, COBALT_SCREEN_NOTIFICATIONS, NULL);
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_NOTIFY_MENU: {
               const cobalt_notifications *list = cobalt_session_notifications();
               if (app->notify.nav.selected < list->count) {
                  cobalt_app_open_notification_menu(app, &list->items[app->notify.nav.selected]);
               }
               break;
            }
            case COBALT_NOTIFY_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_MESSAGES:
         switch (cobalt_messages_view_update(&app->messages, in)) {
            case COBALT_MESSAGES_BACK:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_MESSAGES_OPEN: {
               const cobalt_chat_inbox *inbox = cobalt_session_chat_inbox();
               const int sel = (int) app->messages.nav.selected;
               if (sel < inbox->count &&
                   cobalt_session_begin_chat_messages(inbox->items[sel].id)) {
                  cobalt_chat_thread_view_open(&app->chat_view, inbox->items[sel].name,
                                               inbox->items[sel].id,
                                               inbox->items[sel].group);
                  app->screen = COBALT_SCREEN_CHAT_THREAD;
                  COBALT_LOGI("messages: opened a conversation");
               }
               break;
            }
            case COBALT_MESSAGES_START: {
               const bool began = cobalt_session_begin_chat_start(app->messages.start.target);
               cobalt_messages_start_began(&app->messages.start, began);
               if (!began) {
                  set_notice(app, "Could not start that conversation. Check the handle or DID.",
                             true);
               }
               break;
            }
            case COBALT_MESSAGES_ACCEPT:
            case COBALT_MESSAGES_DECLINE: {
               /* The view has filled in the target. A refused begin changes
                * nothing, so the request stays where it is. */
               const bool accept = app->messages.decision.accept;
               const bool began = accept
                  ? cobalt_session_begin_chat_accept(app->messages.decision.convo_id)
                  : cobalt_session_begin_chat_decline(app->messages.decision.convo_id);
               cobalt_messages_decision_began(&app->messages, began);
               if (!began) {
                  set_notice(app, "Could not update that request. Wait for the last request to finish.",
                             true);
               }
               break;
            }
            case COBALT_MESSAGES_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_CHAT_THREAD:
         switch (cobalt_chat_thread_view_update(&app->chat_view, in)) {
            case COBALT_CHAT_THREAD_BACK:
               app->screen = COBALT_SCREEN_MESSAGES;
               break;
            case COBALT_CHAT_THREAD_JOIN_APPROVE:
            case COBALT_CHAT_THREAD_JOIN_REJECT: {
               /* The view has filled in the target. A refused begin changes
                * nothing, so the request stays where it is. */
               const bool approve = app->chat_view.join_decision.approve;
               const bool began = approve
                  ? cobalt_session_begin_chat_join_approve(
                       app->chat_view.convo_id, app->chat_view.join_decision.did)
                  : cobalt_session_begin_chat_join_reject(
                       app->chat_view.convo_id, app->chat_view.join_decision.did);
               cobalt_chat_thread_decision_began(&app->chat_view, began);
               if (!began) {
                  set_notice(app, "Could not update that join request. Wait for the "
                                  "last request to finish.", true);
               }
               break;
            }
            case COBALT_CHAT_THREAD_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_COMPOSE:
         switch (cobalt_compose_update(&app->compose, in)) {
            case COBALT_COMPOSE_CANCELLED:
               app->screen = app->compose_return;
               break;
            case COBALT_COMPOSE_SUBMIT:
               if (app->compose.thread_count > 0) {
                  const char *texts[COBALT_THREAD_POSTS_MAX];
                  const int n = cobalt_compose_thread_texts(&app->compose, texts);
                  if (!cobalt_session_begin_post_thread(
                         texts, n, (int) app->compose.reply_gate)) {
                     set_notice(app, "Could not start that thread.", true);
                  }
                  break;
               }
               if (!(cobalt_compose_is_quote(&app->compose)
                        ? cobalt_session_begin_quote(
                             app->compose.text, app->compose.quote_uri,
                             app->compose.quote_cid,
                             (int) app->compose.reply_gate,
                             app->compose.attach_path, app->compose.attach_alt)
                        : cobalt_session_begin_post(
                             app->compose.text, app->compose.parent_uri,
                             app->compose.parent_cid, app->compose.root_uri,
                             app->compose.root_cid,
                             (int) app->compose.reply_gate,
                             app->compose.attach_path, app->compose.attach_alt))) {
                  set_notice(app, "Could not start that post.", true);
               }
               break;
            case COBALT_COMPOSE_SAVE_LATER:
               if (app->queue_send_pending) {
                  set_notice(app, "Wait for the queued post to finish sending.", true);
                  break;
               }
               if (app->compose.text[0] == '\0') {
                  set_notice(app, "Nothing to save.", true);
                  break;
               }
               if (strlen(app->compose.text) >= COBALT_POST_QUEUE_TEXT_MAX) {
                  set_notice(app, "That post is too long to save for later.", true);
                  break;
               }
               if (!cobalt_post_queue_add(&app->queue, app->compose.text,
                                          (long long) time(NULL))) {
                  set_notice(app, "The queue is full. Send or discard a post first.",
                             true);
                  break;
               }
               persist_queue(app);
               cobalt_compose_init(&app->compose);
               cobalt_queue_view_open(&app->queue_view);
               app->screen = COBALT_SCREEN_QUEUE;
               set_notice(app, "Saved for later.", false);
               break;
            case COBALT_COMPOSE_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_QUEUE:
         cobalt_app_update_queue(app, in);
         break;

      case COBALT_SCREEN_SIGN_IN:
         cobalt_app_update_signin(app, in);
         break;

      case COBALT_SCREEN_ACCOUNT:
         cobalt_app_update_account(app, in);
         break;

      case COBALT_SCREEN_FEEDS:
         cobalt_app_update_feeds(app, in);
         break;

      case COBALT_SCREEN_MUTED_LIST:
      case COBALT_SCREEN_BLOCKED_LIST:
         switch (cobalt_graph_view_update(&app->graph, in)) {
            case COBALT_GRAPH_VIEW_BACK:
               app->screen = COBALT_SCREEN_ACCOUNT;
               break;
            case COBALT_GRAPH_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_FOLLOWS_LIST:
         switch (cobalt_graph_view_update(&app->graph, in)) {
            case COBALT_GRAPH_VIEW_BACK:
               /* The profile on screen may have been replaced by one opened
                * from this list, so go back to the one the list belongs to. */
               cobalt_session_begin_profile(app->graph.actor);
               cobalt_listnav_restore(&app->profile.nav, app->follows_profile_selected,
                                      app->follows_profile_scroll);
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_GRAPH_VIEW_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               cobalt_app_push_return(app, COBALT_SCREEN_FOLLOWS_LIST, NULL);
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_GRAPH_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_LIKES_LIST:
         switch (cobalt_graph_view_update(&app->graph, in)) {
            case COBALT_GRAPH_VIEW_BACK:
               app->screen = app->likes_return;
               break;
            case COBALT_GRAPH_VIEW_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               cobalt_app_push_return(app, COBALT_SCREEN_LIKES_LIST, NULL);
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_GRAPH_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_SEARCH:
         switch (cobalt_search_view_update(&app->search, in)) {
            case COBALT_SEARCH_VIEW_BACK:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_SEARCH_VIEW_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               cobalt_app_push_return(app, COBALT_SCREEN_SEARCH, NULL);
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_SEARCH_VIEW_OPEN_POSTS:
               cobalt_timeline_rewind(&app->timeline);
               app->viewing_search = true;
               app->screen = COBALT_SCREEN_TIMELINE;
               break;
            case COBALT_SEARCH_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_LISTS:
         switch (cobalt_lists_view_update(&app->lists, in)) {
            case COBALT_LISTS_VIEW_BACK:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_LISTS_VIEW_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               cobalt_app_push_return(app, COBALT_SCREEN_LISTS, NULL);
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_LISTS_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_HOME:
      default:
         cobalt_app_update_home(app, in);
         break;
   }

   cobalt_session_unlock();

   /* Ease focus toward the selection so tiles settle rather than snap. */
   for (int i = 0; i < COBALT_HOME_MENU_COUNT; i++) {
      float target = (i == app->selected) ? 1.0f : 0.0f;
      app->focus[i] += (target - app->focus[i]) * FOCUS_RATE;
   }
}

/* UI sounds are derived from what the frame did, not sprinkled through every
 * screen: a screen change is a select or a back, a held-direction step is a
 * tick, and a like or repost press on a post list is its own chime. */
void
cobalt_app_update(cobalt_app *app, const cobalt_input *in, uint32_t now_ms)
{
   if (!app || !in) {
      return;
   }
   const cobalt_screen before = app->screen;
   const bool had_notice = app->notice[0] != '\0';
   const bool was_busy = cobalt_session_busy();
   const bool back_tap = s_back_hit_valid && screen_has_back_pill(before) &&
                         cobalt_input_tapped(in, &s_back_hit);

   app_update_inner(app, in, now_ms);

   if (app->screen != before) {
      cobalt_sound_play(in->pressed[COBALT_BTN_BACK] || back_tap
                           ? COBALT_SFX_BACK : COBALT_SFX_SELECT);
   } else if (!was_busy && (before == COBALT_SCREEN_TIMELINE ||
                            before == COBALT_SCREEN_THREAD) &&
              (in->pressed[COBALT_BTN_LEFT] || in->pressed[COBALT_BTN_RIGHT])) {
      cobalt_sound_play(in->pressed[COBALT_BTN_LEFT] ? COBALT_SFX_LIKE
                                                     : COBALT_SFX_REPOST);
   } else if (in->pressed[COBALT_BTN_UP] || in->pressed[COBALT_BTN_DOWN]) {
      cobalt_sound_play(COBALT_SFX_MOVE);
   }
   if (!had_notice && app->notice[0]) {
      cobalt_sound_play(COBALT_SFX_NOTICE);
   }
}

/* --- drawing --- */

void
cobalt_app_draw_header(cobalt_render *r, const char *subtitle)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Cobalt", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE);

   int title_h = cobalt_font_line_height(r, COBALT_FONT_TITLE);
   {
      char ver[24];
      int title_w = 0;
      snprintf(ver, sizeof ver, "v%s", cobalt_version());
      cobalt_text_size(r, COBALT_FONT_TITLE, "Cobalt", &title_w, NULL);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, ver,
                       m->pad_edge + title_w + m->pad_tile,
                       m->pad_edge + title_h -
                          cobalt_font_line_height(r, COBALT_FONT_CAPTION) * 3 / 2,
                       (SDL_Color) { 0xFF, 0xFF, 0xFF, 0xB0 });
   }
   SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                    m->pad_edge + title_h - m->line_gap, dim);
}

/* The one-line status the home and account screens share. */
void
cobalt_app_draw_notice(const cobalt_app *app, cobalt_render *r, int y, int width)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   if (cobalt_session_busy()) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, "Working...", m->pad_edge, y,
                       COBALT_COLOUR_TEXT_DIM);
      return;
   }

   if (app->notice[0] == '\0') {
      return;
   }

   cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, app->notice, m->pad_edge, y,
                            width, 2,
                            app->notice_is_error ? COBALT_COLOUR_ERROR
                                                 : COBALT_COLOUR_TEXT_DIM);
}

void
cobalt_app_draw(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   if (!app || !r) {
      return;
   }

   if (surface == COBALT_SURFACE_TV && app->display == COBALT_DISPLAY_GAMEPAD) {
      cobalt_app_draw_tv_idle(r);
      return;
   }

   /* Same reason as cobalt_app_update: the buffers being drawn belong to the
    * worker and it edits them in place. Both surfaces are drawn per frame, so
    * this is taken twice. */
   cobalt_session_lock();

   /* Drawn after the screen switch so it covers whatever is underneath, on
    * both surfaces — the TV and the GamePad show the same picture, which is
    * the point of looking at one. */
   if (app->imageview.open) {
      cobalt_imageview_draw(&app->imageview, r);
      cobalt_session_unlock();
      return;
   }

   switch (app->screen) {
      case COBALT_SCREEN_ENTROPY:
         cobalt_app_draw_header(r, "Set up");
         cobalt_entropy_view_draw(&app->entropy, r, surface, cobalt_content_top(r));
         break;

      case COBALT_SCREEN_DIAGNOSTICS:
         cobalt_app_draw_diagnostics(app, r, surface);
         break;

      case COBALT_SCREEN_TIMELINE:
         cobalt_timeline_draw(&app->timeline, r, surface);
         break;

      case COBALT_SCREEN_THREAD:
         cobalt_thread_view_draw(&app->thread, r, surface);
         break;

      case COBALT_SCREEN_COMPOSE:
         cobalt_compose_draw(&app->compose, r, surface);
         break;

      case COBALT_SCREEN_NOTIFICATIONS:
         cobalt_notify_view_draw(&app->notify, r, surface);
         break;

      case COBALT_SCREEN_MESSAGES:
         cobalt_messages_view_draw(&app->messages, r, surface);
         break;

      case COBALT_SCREEN_CHAT_THREAD:
         cobalt_chat_thread_view_draw(&app->chat_view, r, surface);
         break;

      case COBALT_SCREEN_PROFILE:
         cobalt_profile_view_draw(&app->profile, r, surface);
         break;

      case COBALT_SCREEN_SIGN_IN:
         cobalt_signin_draw(&app->signin, r, surface);
         break;

      case COBALT_SCREEN_ACCOUNT:
         cobalt_app_draw_account(app, r, surface);
         break;

      case COBALT_SCREEN_FEEDS:
         cobalt_app_draw_feeds(app, r, surface);
         break;

      case COBALT_SCREEN_MUTED_LIST:
      case COBALT_SCREEN_BLOCKED_LIST:
      case COBALT_SCREEN_FOLLOWS_LIST:
      case COBALT_SCREEN_LIKES_LIST:
         cobalt_graph_view_draw(&app->graph, r, surface);
         break;

      case COBALT_SCREEN_SEARCH:
         cobalt_search_view_draw(&app->search, r, surface);
         break;

      case COBALT_SCREEN_QUEUE:
         cobalt_queue_view_draw(&app->queue_view, &app->queue, r);
         break;

      case COBALT_SCREEN_LISTS:
         cobalt_lists_view_draw(&app->lists, r, surface);
         break;

      case COBALT_SCREEN_HOME:
      default:
         if (surface == COBALT_SURFACE_DRC) {
            cobalt_app_draw_home_drc(app, r);
         } else {
            cobalt_app_draw_home_tv(app, r);
         }
         break;
   }

   if (screen_has_back_pill(app->screen)) {
      draw_back_pill(r, surface);
   } else if (surface == COBALT_SURFACE_DRC) {
      s_back_hit_valid = false;
   }

   cobalt_popup_draw(&app->popup, r, surface);

   cobalt_session_unlock();
}
