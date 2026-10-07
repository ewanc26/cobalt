#include "app/app_internal.h"

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
   cobalt_update_view_init(&app->update);
   cobalt_update_view_startup(&app->update, cobalt_data_root());

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
   cobalt_profile_view_init(&app->profile);
   cobalt_graph_view_init(&app->graph);
   cobalt_search_view_init(&app->search);
   cobalt_lists_view_init(&app->lists);

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
      /* A verified update replaces the build only now, with the app closing. */
      cobalt_update_view_apply_on_quit(&app->update);
      cobalt_update_view_destroy(&app->update);
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

static void
handle_job_result(cobalt_app *app, const cobalt_job_result *result)
{
   switch (result->kind) {
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
   cobalt_profile_view_init(&app->profile);
   cobalt_graph_view_init(&app->graph);
   cobalt_search_view_init(&app->search);
   cobalt_lists_view_init(&app->lists);
         app->screen = COBALT_SCREEN_HOME;
         app->selected = 1;
         break;

      case COBALT_JOB_POST:
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
               app->screen = app->thread_return;
            }
         } else {
            set_notice(app, result->message, true);
         }
         break;

      case COBALT_JOB_NOTIFICATIONS:
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
   cobalt_update_view_tick(&app->update);

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

      case COBALT_SCREEN_UPDATE:
         if (cobalt_update_view_update(&app->update, in) == COBALT_UPDATE_VIEW_BACK) {
            app->screen = COBALT_SCREEN_HOME;
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
               app->screen = app->profile_return;
               break;
            case COBALT_PROFILE_VIEW_OPEN_THREAD:
               cobalt_thread_view_reset(&app->thread);
               app->thread_return = COBALT_SCREEN_PROFILE;
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
               app->thread_return = COBALT_SCREEN_TIMELINE;
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_TIMELINE_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               app->profile_return = COBALT_SCREEN_TIMELINE;
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
               app->screen = app->thread_return;
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
               app->thread_return = COBALT_SCREEN_NOTIFICATIONS;
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_NOTIFY_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               app->profile_return = COBALT_SCREEN_NOTIFICATIONS;
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
            case COBALT_COMPOSE_STAY:
            default:
               break;
         }
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
               cobalt_profile_view_rewind(&app->profile);
               app->profile_return = app->follows_profile_return;
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_GRAPH_VIEW_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               app->profile_return = COBALT_SCREEN_FOLLOWS_LIST;
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
               app->profile_return = COBALT_SCREEN_LIKES_LIST;
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
               app->profile_return = COBALT_SCREEN_SEARCH;
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
               app->profile_return = COBALT_SCREEN_LISTS;
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
                    COBALT_COLOUR_TILE_FOCUS);

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

      case COBALT_SCREEN_UPDATE:
         cobalt_app_draw_header(r, "Updates");
         cobalt_update_view_draw(&app->update, r, cobalt_content_top(r));
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
