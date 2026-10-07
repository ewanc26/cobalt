/* The More menus on a post, a profile and a notification, and what choosing from a popup does. */

#include "app/app_internal.h"

/* Copy of the text a facet covers, for a menu label. */
static void
facet_label(const cobalt_post *post, const cobalt_post_facet *f, char *out,
            size_t size)
{
   const int len = (int) strlen(post->text);
   int s = f->start < len ? f->start : len;
   int e = f->end < len ? f->end : len;
   size_t n = e > s ? (size_t) (e - s) : 0;
   if (n >= size) {
      n = size - 1;
      while (n > 0 && ((unsigned char) post->text[s + n] & 0xC0) == 0x80) {
         n--;
      }
   }
   memcpy(out, post->text + s, n);
   out[n] = '\0';
}

void
cobalt_app_open_post_menu(cobalt_app *app, const cobalt_post *post, bool in_thread)
{
   const char *handle = post->handle[0] == '@' ? post->handle + 1 : post->handle;
   char label[COBALT_POPUP_LABEL_MAX];
   cobalt_popup_open(&app->popup, "More");
   app->popup_screen = app->screen;
   app->popup_in_thread = in_thread;
   if (handle[0]) {
      snprintf(label, sizeof(label), "View profile @%s", handle);
      cobalt_popup_add(&app->popup, COBALT_POPUP_PROFILE, label, handle);
   }
   if (post->image_count > 0) {
      cobalt_popup_add(&app->popup, COBALT_POPUP_IMAGE, "View image", "");
   }
   for (int i = 0; i < post->facet_count; i++) {
      const cobalt_post_facet *f = &post->facets[i];
      if (!f->target[0]) {
         continue;
      }
      facet_label(post, f, label, sizeof(label));
      cobalt_popup_add(&app->popup,
                       f->kind == COBALT_FACET_LINK ? COBALT_POPUP_LINK
                       : f->kind == COBALT_FACET_MENTION ? COBALT_POPUP_MENTION
                                                         : COBALT_POPUP_TAG,
                       label, f->target);
   }
   if (app->screen == COBALT_SCREEN_TIMELINE) {
      cobalt_popup_add(&app->popup, COBALT_POPUP_COMPOSE, "New post", "");
      cobalt_popup_add(&app->popup, COBALT_POPUP_REFRESH, "Refresh timeline", "");
   }
   if (post->like_count > 0) {
      char label[COBALT_POPUP_LABEL_MAX];
      snprintf(label, sizeof(label), "Liked by (%d)", post->like_count);
      cobalt_popup_add(&app->popup, COBALT_POPUP_LIKES, label, post->uri);
   }
   if (post->repost_count > 0) {
      char label[COBALT_POPUP_LABEL_MAX];
      snprintf(label, sizeof(label), "Reposted by (%d)", post->repost_count);
      cobalt_popup_add(&app->popup, COBALT_POPUP_REPOSTS, label, post->uri);
   }
   if (in_thread) {
      cobalt_popup_add(&app->popup, COBALT_POPUP_QUOTE, "Quote post", post->uri);
      if (cobalt_post_uri_is_by(post->uri, cobalt_session_did())) {
         cobalt_popup_add(&app->popup, COBALT_POPUP_DELETE, "Delete post",
                          post->uri);
      }
   }
}

/* The profile's More menu (cobalt#109): the post's own actions when the cursor is
 * on a post, then the account's lists and tab, which used to be X, Y and +. */
void
cobalt_app_open_profile_menu(cobalt_app *app)
{
   const cobalt_post *post = cobalt_profile_view_selected_post(&app->profile);
   if (post) {
      cobalt_app_open_post_menu(app, post, false);
   } else {
      cobalt_popup_open(&app->popup, "More");
      app->popup_screen = app->screen;
      app->popup_in_thread = false;
   }
   const cobalt_profile *profile = cobalt_session_profile();
   char label[COBALT_POPUP_LABEL_MAX];
   if (profile->did[0]) {
      cobalt_popup_add(&app->popup, COBALT_POPUP_FOLLOWERS, "Followers", "");
      cobalt_popup_add(&app->popup, COBALT_POPUP_FOLLOWING, "Following", "");
   }
   snprintf(label, sizeof(label), "Show %s",
            cobalt_profile_tab_name(cobalt_profile_tab_next(cobalt_session_profile_tab(),
                                                            profile->is_self)));
   cobalt_popup_add(&app->popup, COBALT_POPUP_TAB, label, "");
}

/* A notification's More menu: who it is from, and the post it is about. */
void
cobalt_app_open_notification_menu(cobalt_app *app, const cobalt_notification *item)
{
   char label[COBALT_POPUP_LABEL_MAX];
   cobalt_popup_open(&app->popup, "More");
   app->popup_screen = app->screen;
   app->popup_in_thread = false;
   if (item->actor_did[0]) {
      snprintf(label, sizeof(label), "View profile %s", item->handle[0] ? item->handle : item->actor);
      cobalt_popup_add(&app->popup, COBALT_POPUP_PROFILE, label, item->actor_did);
   }
   if (item->subject_uri[0]) {
      cobalt_popup_add(&app->popup, COBALT_POPUP_THREAD, "Open post", item->subject_uri);
   }
   cobalt_popup_add(&app->popup, COBALT_POPUP_REFRESH, "Refresh notifications", "");
}

void
cobalt_app_popup_choose(cobalt_app *app, int index)
{
   const cobalt_popup_item *it = &app->popup.items[index];
   switch (it->kind) {
      case COBALT_POPUP_PROFILE:
      case COBALT_POPUP_MENTION:
         if (cobalt_session_begin_profile(it->arg)) {
            cobalt_profile_view_rewind(&app->profile);
            app->profile_return = app->popup_screen;
            app->screen = COBALT_SCREEN_PROFILE;
            cobalt_popup_close(&app->popup);
         }
         break;
      case COBALT_POPUP_TAG: {
         char query[COBALT_POPUP_ARG_MAX + 2];
         snprintf(query, sizeof(query), "#%s", it->arg);
         if (cobalt_session_begin_search_posts(query, false)) {
            cobalt_timeline_rewind(&app->timeline);
            app->viewing_search = true;
            app->screen = COBALT_SCREEN_TIMELINE;
            cobalt_popup_close(&app->popup);
         }
         break;
      }
      case COBALT_POPUP_LINK: {
         char arg[COBALT_POPUP_ARG_MAX];
         snprintf(arg, sizeof(arg), "%s", it->arg);
         cobalt_popup_show_text(&app->popup, "Link", arg);
         break;
      }
      case COBALT_POPUP_QUOTE: {
         const cobalt_thread *conv = cobalt_session_thread();
         cobalt_popup_close(&app->popup);
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
      case COBALT_POPUP_IMAGE: {
         /* The post the menu was opened on is the one the popup remembers
          * the screen for: the timeline's or the thread's selected card.
          * Re-derived here rather than copied into the popup so a refresh
          * that moved the selection cannot make the viewer show a different
          * post's pictures than the menu named. */
         const cobalt_post *post = NULL;
         if (app->popup_screen == COBALT_SCREEN_PROFILE) {
            post = cobalt_profile_view_selected_post(&app->profile);
         } else if (app->popup_in_thread) {
            const cobalt_thread *conv = cobalt_session_thread();
            if (app->thread.nav.selected < conv->count) {
               post = &conv->posts[app->thread.nav.selected];
            }
         } else {
            const cobalt_feed *feed = cobalt_session_feed();
            if (app->timeline.nav.selected < feed->count) {
               post = &feed->posts[app->timeline.nav.selected];
            }
         }
         if (post) {
            cobalt_imageview_open(&app->imageview, post);
            cobalt_popup_close(&app->popup);
         }
         break;
      }
      case COBALT_POPUP_LIKES:
      case COBALT_POPUP_REPOSTS: {
         cobalt_graph_view_open_likes(
            &app->graph,
            it->kind == COBALT_POPUP_LIKES ? COBALT_GRAPH_LIKES
                                            : COBALT_GRAPH_REPOSTED,
            it->arg);
         app->likes_return = app->popup_screen;
         app->screen = COBALT_SCREEN_LIKES_LIST;
         cobalt_popup_close(&app->popup);
         break;
      }
      case COBALT_POPUP_COMPOSE:
         cobalt_popup_close(&app->popup);
         cobalt_compose_init(&app->compose);
         app->compose_return = COBALT_SCREEN_TIMELINE;
         app->screen = COBALT_SCREEN_COMPOSE;
         break;
      case COBALT_POPUP_REFRESH:
         cobalt_popup_close(&app->popup);
         if (cobalt_session_busy()) {
            break;
         }
         if (app->popup_screen == COBALT_SCREEN_NOTIFICATIONS) {
            if (cobalt_session_begin_notifications(false)) {
               cobalt_notify_view_rewind(&app->notify);
            }
         } else if (cobalt_session_begin_feed_current(false)) {
            cobalt_timeline_rewind(&app->timeline);
         }
         break;
      case COBALT_POPUP_THREAD:
         if (cobalt_session_begin_thread(it->arg)) {
            cobalt_thread_view_reset(&app->thread);
            app->thread_return = app->popup_screen;
            app->screen = COBALT_SCREEN_THREAD;
            cobalt_popup_close(&app->popup);
         }
         break;
      case COBALT_POPUP_FOLLOWERS:
      case COBALT_POPUP_FOLLOWING:
         app->follows_profile_return = app->profile_return;
         cobalt_graph_view_open_follows(&app->graph,
                                        it->kind == COBALT_POPUP_FOLLOWERS
                                           ? COBALT_GRAPH_FOLLOWERS
                                           : COBALT_GRAPH_FOLLOWING,
                                        cobalt_session_profile()->did);
         app->screen = COBALT_SCREEN_FOLLOWS_LIST;
         cobalt_popup_close(&app->popup);
         break;
      case COBALT_POPUP_TAB:
         cobalt_popup_close(&app->popup);
         if (!cobalt_session_busy() &&
             cobalt_session_begin_profile_tab(cobalt_profile_tab_next(
                cobalt_session_profile_tab(), cobalt_session_profile()->is_self))) {
            cobalt_profile_view_rewind(&app->profile);
         }
         break;
      case COBALT_POPUP_DELETE:
         snprintf(app->thread.delete_uri, sizeof(app->thread.delete_uri), "%s",
                  it->arg);
         app->thread.confirm_delete = true;
         cobalt_popup_close(&app->popup);
         break;
   }
}
