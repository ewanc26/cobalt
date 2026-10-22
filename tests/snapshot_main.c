/*
 * Host render snapshot: drives the real app and renderer on SDL's dummy video
 * driver with the software renderer and writes the TV and GamePad frames as
 * BMPs. Checks the frames are drawn, not that they are pretty. See `make snapshot`.
 */
#include "app/app.h"
#include "app/app_internal.h"
#include "atproto/session.h"
#include "atproto/feed.h"
#include "ui/videoduration.h"
#include "../../wolfram/test/mock_pds.h"
#include <wolfram/feed_typed.h>
#include "input/input.h"
#include "ui/render.h"
#include "fixtures.h"
#include <SDL_ttf.h>

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
   printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(actual, expected) do { checks++; \
   if (!(actual) || strcmp((actual), (expected)) != 0) { failures++; \
   printf("FAIL %s:%d got \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, \
          (actual) ? (actual) : "(null)", (expected)); } } while (0)

/*
 * Video durations (ui/videoduration.h). The playlist fetch is replaced by a
 * fake, and no loader thread is started, so drain() resolves the queue on this
 * thread. The chain under test is real: a postView JSON is parsed by Wolfram,
 * flattened into a post, drawn (lookup), and then resolved.
 */
static const char *s_fake_body;
/* Answers any URL under /var/ (a master's variant), when set. */
static const char *s_fake_variant_body;
static int s_fake_calls;
static char s_fake_url[256];

static bool
fake_fetch(const char *url, cobalt_http_response *out)
{
   s_fake_calls++;
   snprintf(s_fake_url, sizeof s_fake_url, "%s", url);
   const char *body = (s_fake_variant_body && strstr(url, "/var/"))
                         ? s_fake_variant_body : s_fake_body;
   if (!body) return false;
   size_t n = strlen(body);
   out->data = malloc(n + 1);
   if (!out->data) return false;
   memcpy(out->data, body, n + 1);
   out->size = n;
   out->status = 200;
   return true;
}

static const char *const VOD_PLAYLIST =
   "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:60\n"
   "#EXTINF:60.000,\nseg0.ts\n#EXTINF:23.400,\nseg1.ts\n#EXT-X-ENDLIST\n";
static const char *const LIVE_PLAYLIST =
   "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:60\n"
   "#EXTINF:60.000,\nseg0.ts\n#EXTINF:23.400,\nseg1.ts\n";
static const char *const MASTER_PLAYLIST =
   "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\n"
   "var/360p.m3u8\n";

static const char *
video_post_json(const char *n, const char *playlist)
{
   static char buf[2400];
   snprintf(buf, sizeof buf,
      "{\"posts\":[{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/%s\","
      "\"cid\":\"bafy%s\",\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\"},"
      "\"record\":{\"$type\":\"app.bsky.feed.post\",\"text\":\"clip\","
      "\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.video#view\",\"cid\":\"bafyvid\","
      "\"playlist\":\"%s\",\"thumbnail\":\"https://video.test/th/%s.jpg\","
      "\"alt\":\"a clip\"},"
      "\"likeCount\":0,\"repostCount\":0,\"replyCount\":0,"
      "\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}]}", n, n, playlist, n);
   return buf;
}

static void
check_video_durations(void)
{
   cobalt_video_duration_set_fetcher(fake_fetch);
   cobalt_video_duration_reset_for_test();

   /* Parsing: the duration of a finished playlist is its EXTINF sum, and a
    * master or live playlist has none. Wolfram does the parsing. */
   double seconds = 0.0;
   CHECK(cobalt_video_duration_parse(VOD_PLAYLIST, strlen(VOD_PLAYLIST), &seconds));
   CHECK(seconds > 83.39 && seconds < 83.41);
   CHECK(!cobalt_video_duration_parse(MASTER_PLAYLIST, strlen(MASTER_PLAYLIST), &seconds));
   CHECK(!cobalt_video_duration_parse(LIVE_PLAYLIST, strlen(LIVE_PLAYLIST), &seconds));

   /* A feed flatten fetches nothing: the playlist URI is only recorded. */
   static cobalt_feed feed;
   static wf_agent_post_list list;
   memset(&feed, 0, sizeof feed);
   memset(&list, 0, sizeof list);
   const char *json = video_post_json("v1", "https://video.test/pl/v1.m3u8");
   CHECK(wf_agent_parse_posts(json, strlen(json), &list) == WF_OK);
   CHECK(cobalt_feed_append_posts_from_wolfram(&feed, &list, NULL, 0) == 1);
   CHECK(feed.count == 1);
   CHECK(feed.posts[0].link.video);
   CHECK_STR(feed.posts[0].link.video_playlist, "https://video.test/pl/v1.m3u8");
   CHECK(cobalt_video_duration_drain() == 0);
   CHECK(s_fake_calls == 0);

   /* The first draw queues the fetch, and nothing shows until it resolves. */
   s_fake_body = VOD_PLAYLIST;
   CHECK(cobalt_video_duration_lookup(feed.posts[0].link.video_playlist) == NULL);
   CHECK(s_fake_calls == 0);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK(s_fake_calls == 1);
   CHECK_STR(s_fake_url, "https://video.test/pl/v1.m3u8");
   CHECK_STR(cobalt_video_duration_lookup(feed.posts[0].link.video_playlist), "1:23");

   /* Later draws, frame after frame, read the stored text and fetch nothing. */
   for (int i = 0; i < 5; i++) {
      CHECK_STR(cobalt_video_duration_lookup(feed.posts[0].link.video_playlist), "1:23");
   }
   CHECK(cobalt_video_duration_drain() == 0);
   CHECK(s_fake_calls == 1);

   /* A master playlist shows no duration, and is not fetched again. */
   static cobalt_feed master_feed;
   static wf_agent_post_list master_list;
   memset(&master_feed, 0, sizeof master_feed);
   memset(&master_list, 0, sizeof master_list);
   const char *master_json = video_post_json("v2", "https://video.test/pl/v2.m3u8");
   CHECK(wf_agent_parse_posts(master_json, strlen(master_json), &master_list) == WF_OK);
   CHECK(cobalt_feed_append_posts_from_wolfram(&master_feed, &master_list, NULL, 0) == 1);
   s_fake_body = MASTER_PLAYLIST;
   CHECK(cobalt_video_duration_lookup(master_feed.posts[0].link.video_playlist) == NULL);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK(cobalt_video_duration_lookup(master_feed.posts[0].link.video_playlist) == NULL);
   CHECK(cobalt_video_duration_lookup(master_feed.posts[0].link.video_playlist) == NULL);
   CHECK(cobalt_video_duration_drain() == 0);
   /* The master is fetched, then its one variant, and nothing below that. */
   CHECK(s_fake_calls == 3);
   CHECK_STR(s_fake_url, "https://video.test/pl/var/360p.m3u8");

   /* A master whose variant is a finished playlist shows that variant's duration. */
   static cobalt_feed var_feed;
   static wf_agent_post_list var_list;
   memset(&var_feed, 0, sizeof var_feed);
   memset(&var_list, 0, sizeof var_list);
   const char *var_json = video_post_json("v5", "https://video.test/pl/v5.m3u8");
   CHECK(wf_agent_parse_posts(var_json, strlen(var_json), &var_list) == WF_OK);
   CHECK(cobalt_feed_append_posts_from_wolfram(&var_feed, &var_list, NULL, 0) == 1);
   s_fake_variant_body = VOD_PLAYLIST;
   CHECK(cobalt_video_duration_lookup(var_feed.posts[0].link.video_playlist) == NULL);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK_STR(cobalt_video_duration_lookup(var_feed.posts[0].link.video_playlist), "1:23");
   CHECK(s_fake_calls == 5);
   s_fake_variant_body = NULL;

   /* A failed fetch and a live playlist both leave the duration unknown. */
   s_fake_body = NULL;
   CHECK(cobalt_video_duration_lookup("https://video.test/pl/v3.m3u8") == NULL);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK(cobalt_video_duration_lookup("https://video.test/pl/v3.m3u8") == NULL);
   CHECK(s_fake_calls == 6);
   s_fake_body = LIVE_PLAYLIST;
   CHECK(cobalt_video_duration_lookup("https://video.test/pl/v4.m3u8") == NULL);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK(cobalt_video_duration_lookup("https://video.test/pl/v4.m3u8") == NULL);
   CHECK(s_fake_calls == 7);

   wf_agent_post_list_free(&list);
   wf_agent_post_list_free(&master_list);
   cobalt_video_duration_set_fetcher(NULL);
   cobalt_video_duration_reset_for_test();
}

static int
distinct_colours(SDL_Surface *s)
{
   int n = 0;
   uint32_t seen[64];
   const uint8_t *p = s->pixels;
   for (int y = 0; y < s->h; y += 3)
      for (int x = 0; x < s->w; x += 3) {
         uint32_t v = *(const uint32_t *) (p + y * s->pitch + x * 4);
         int k = 0;
         while (k < n && seen[k] != v) k++;
         if (k == n && n < 64) seen[n++] = v;
      }
   return n;
}

static cobalt_render *g_tv, *g_drc;
static cobalt_app *g_app;
static cobalt_input g_in;
static const char *g_dir;

static void
frame(cobalt_button press)
{
   uint32_t now = SDL_GetTicks();
   cobalt_input_begin_frame(&g_in, now);
   if (press != COBALT_BTN_COUNT) g_in.pressed[press] = true;
   cobalt_input_end_frame(&g_in, now);
   cobalt_app_update(g_app, &g_in, now);
   cobalt_render_begin(g_tv);
   cobalt_app_draw(g_app, g_tv, COBALT_SURFACE_TV);
   cobalt_render_end(g_tv);
   cobalt_render_begin(g_drc);
   cobalt_app_draw(g_app, g_drc, COBALT_SURFACE_DRC);
   cobalt_render_end(g_drc);
}

static void
settle(int n)
{
   for (int i = 0; i < n; i++) { frame(COBALT_BTN_COUNT); SDL_Delay(16); }
}

static void
shoot(const char *scene)
{
   cobalt_render *rs[2] = { g_tv, g_drc };
   const char *names[2] = { "tv", "drc" };
   for (int i = 0; i < 2; i++) {
      SDL_Renderer *rd = cobalt_render_sdl_renderer(rs[i]);
      CHECK(rd != NULL);
      if (!rd) continue;
      int w, h;
      SDL_GetRendererOutputSize(rd, &w, &h);
      SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
      CHECK(SDL_RenderReadPixels(rd, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0);
      int dc = distinct_colours(s);
      printf("%s/%s: %dx%d distinct colours (sampled) %d\n", scene, names[i], w, h, dc);
      CHECK(dc > 4);
      char path[512];
      snprintf(path, sizeof path, "%s/%s-%s.bmp", g_dir, scene, names[i]);
      SDL_SaveBMP(s, path);
      SDL_FreeSurface(s);
   }
}

/* Back is a no-op on home, so spamming it reaches home from anywhere. */
static void
go_home(void)
{
   for (int i = 0; i < 8; i++) {
      frame(COBALT_BTN_BACK);
      settle(4);
   }
}

/* A finger released at (x, y) in GamePad coordinates for one frame. */
static void
tap(int x, int y)
{
   uint32_t now = SDL_GetTicks();
   cobalt_input_begin_frame(&g_in, now);
   g_in.touch_x = x;
   g_in.touch_y = y;
   g_in.touch_ended = true;
   cobalt_input_end_frame(&g_in, now);
   cobalt_app_update(g_app, &g_in, now);
   cobalt_render_begin(g_tv);
   cobalt_app_draw(g_app, g_tv, COBALT_SURFACE_TV);
   cobalt_render_end(g_tv);
   cobalt_render_begin(g_drc);
   cobalt_app_draw(g_app, g_drc, COBALT_SURFACE_DRC);
   cobalt_render_end(g_drc);
}

static void
tap_rect_centre(const SDL_Rect *r)
{
   tap(r->x + r->w / 2, r->y + r->h / 2);
}

/* Home selection is linear and wraps; the app remembers it across screens.
 * Must match COBALT_HOME_MENU_COUNT in app/app_internal.h. */
#define HOME_MENU_COUNT 12

static void
open_home_item(int idx)
{
   go_home();
   {
      /* The GamePad list steps one row at a time, TV or not. */
      const int start = cobalt_app_home_selection(g_app);
      frame(COBALT_BTN_DOWN); settle(2);
      CHECK(cobalt_app_home_selection(g_app) == (start + 1) % COBALT_HOME_MENU_COUNT);
      frame(COBALT_BTN_UP); settle(2);
      CHECK(cobalt_app_home_selection(g_app) == start);
   }
   for (int n = (idx - cobalt_app_home_selection(g_app) + COBALT_HOME_MENU_COUNT) % COBALT_HOME_MENU_COUNT; n > 0; n--) {
      frame(COBALT_BTN_RIGHT);
      settle(2);
   }
   if (cobalt_app_home_selection(g_app) != idx) printf("nav: wanted %d got %d\n", idx, cobalt_app_home_selection(g_app));
   CHECK(cobalt_app_home_selection(g_app) == idx);
   settle(3);
   frame(COBALT_BTN_CONFIRM);
}


static const cobalt_popup *
popup(void)
{
   return cobalt_app_popup(g_app);
}

/* The row of the first open-popup item of this kind, or -1. */
static int
popup_index(cobalt_popup_kind kind)
{
   const cobalt_popup *p = popup();
   for (int i = 0; p && i < p->count; i++)
      if (p->items[i].kind == kind) return i;
   return -1;
}

/* Presses DOWN until the open popup's cursor sits on row idx. */
static void
popup_select(int idx)
{
   const cobalt_popup *p = popup();
   CHECK(p != NULL && idx >= 0 && idx < p->count);
   for (int n = 0; p && p->selected != idx && n < COBALT_POPUP_MAX; n++) {
      frame(COBALT_BTN_DOWN); settle(2);
      p = popup();
   }
   CHECK(p != NULL && p->selected == idx);
}

/* Reaches the timeline from anywhere, with no popup open. */
static void
open_timeline(void)
{
   go_home();
   open_home_item(0);
   settle(40);
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
   CHECK(popup() == NULL);
}

/* Moves the timeline cursor to card idx, one row at a time, and checks it. */
static void
select_timeline(int idx)
{
   int sel, scr;
   cobalt_app_timeline_position(g_app, &sel, &scr);
   for (int n = sel; n < idx; n++) { frame(COBALT_BTN_DOWN); settle(3); }
   for (int n = sel; n > idx; n--) { frame(COBALT_BTN_UP); settle(3); }
   cobalt_app_timeline_position(g_app, &sel, &scr);
   CHECK(sel == idx);
}

/* Opens the More menu on timeline card idx, checking the state first. */
static void
open_post_menu(int idx)
{
   open_timeline();
   select_timeline(idx);
   frame(COBALT_BTN_ALT_Y); settle(5);
   CHECK(popup() != NULL && popup()->open);
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
}

/* A checksum of the TV frame inside `rect`, and how many of its pixels are
 * dark enough to be text. Reads the frame the last update drew. */
static void
tv_region(const SDL_Rect *rect, uint32_t *sum, int *dark)
{
   SDL_Renderer *rd = cobalt_render_sdl_renderer(g_tv);
   int w, h;
   SDL_GetRendererOutputSize(rd, &w, &h);
   SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
   CHECK(SDL_RenderReadPixels(rd, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0);
   *sum = 2166136261u;
   *dark = 0;
   for (int y = rect->y; y < rect->y + rect->h && y < h; y++)
      for (int x = rect->x; x < rect->x + rect->w && x < w; x++) {
         const uint32_t v = *(const uint32_t *) ((const uint8_t *) s->pixels + y * s->pitch + x * 4);
         *sum = (*sum ^ v) * 16777619u;
         const int lum = (int) ((v >> 16) & 255) + (int) ((v >> 8) & 255) + (int) (v & 255);
         if (lum < 450) (*dark)++;
      }
   SDL_FreeSurface(s);
}

/* The TV's right-hand detail column, from the same metrics the draw uses. */
static SDL_Rect
tv_detail_column(void)
{
   const cobalt_metrics *m = cobalt_render_metrics(g_tv);
   const int list_w = (m->width - 2 * m->pad_edge - m->gap) * 2 / 5;
   const int x = m->pad_edge + list_w + m->gap;
   const int top = cobalt_content_top(g_tv);
   const SDL_Rect r = { x, top, m->width - m->pad_edge - x, m->height - top - m->pad_edge - 24 };
   return r;
}

/* Presses one button n times, a frame each. */
static void
press_n(cobalt_button btn, int n)
{
   for (int i = 0; i < n; i++) { frame(btn); settle(1); }
}

int
main(int argc, char **argv)
{
   g_dir = argc > 1 ? argv[1] : ".";
   const char *font = argc > 2 ? argv[2] : "../romfs/font.ttf";
   SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
   SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
   CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
   if (TTF_Init() != 0) { printf("TTF_Init failed\n"); return 1; }
   (void) cobalt_session_init();
   check_video_durations();

   g_tv = cobalt_render_create(COBALT_SURFACE_TV, font, false);
   g_drc = cobalt_render_create(COBALT_SURFACE_DRC, font, false);
   CHECK(g_tv && g_drc);
   if (!g_tv || !g_drc) return 1;
   CHECK(cobalt_render_has_font(g_tv));
   g_app = cobalt_app_create();
   CHECK(g_app != NULL);
   cobalt_input_init(&g_in);

   settle(5);
   shoot("home-signedout");

   wf_mock_pds *pds = NULL;
   int port = 0;
   CHECK(wf_mock_pds_start(&pds, &port) == WF_OK);
   register_fixtures(pds);

   char svc[64];
   snprintf(svc, sizeof svc, "http://127.0.0.1:%d", port);
   CHECK(cobalt_session_begin_login(svc, "alice.test", "app-pass"));
   /* Bounded by time, not by poll count: a loaded machine can take well over
    * a second to finish the login against the mock PDS (cobalt#226). */
   const uint32_t signin_deadline = SDL_GetTicks() + 10000;
   while (cobalt_session_state() != COBALT_AUTH_SIGNED_IN && SDL_GetTicks() < signin_deadline) {
      cobalt_job_result jr;
      (void) cobalt_session_poll(&jr);
      SDL_Delay(5);
   }
   if (cobalt_session_state() != COBALT_AUTH_SIGNED_IN)
      printf("sign-in did not finish within 10 s against the mock PDS (state %d)\n",
             (int) cobalt_session_state());
   CHECK(cobalt_session_state() == COBALT_AUTH_SIGNED_IN);
   settle(30);
   shoot("timeline");
   for (int k = 2; k <= 5; k++) {
      frame(COBALT_BTN_DOWN); settle(25);
      char nm[24]; snprintf(nm, sizeof nm, "timeline-%d", k);
      shoot(nm);
   }
   /* Backing out of a thread must return to the same card, not the top. */
   {
      int sel0, scr0, sel1, scr1;
      cobalt_app_timeline_position(g_app, &sel0, &scr0);
      CHECK(sel0 > 0);
      frame(COBALT_BTN_CONFIRM); settle(30);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_THREAD);
      frame(COBALT_BTN_BACK); settle(10);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
      cobalt_app_timeline_position(g_app, &sel1, &scr1);
      CHECK(sel1 == sel0 && scr1 == scr0);
      /* And through the More menu to a profile and back. */
      frame(COBALT_BTN_ALT_Y); settle(5);
      frame(COBALT_BTN_CONFIRM); settle(30);
      frame(COBALT_BTN_BACK); settle(10);
      cobalt_app_timeline_position(g_app, &sel1, &scr1);
      printf("timeline position %d/%d -> %d/%d, screen %d\n", sel0, scr0, sel1, scr1,
             (int) cobalt_app_screen(g_app));
      CHECK(sel1 == sel0 && scr1 == scr0);
   }
   for (int k = 0; k < 5; k++) { frame(COBALT_BTN_UP); settle(3); }
   settle(20);

   frame(COBALT_BTN_CONFIRM);
   settle(30);
   shoot("thread");
   for (int k = 0; k < 2; k++) { frame(COBALT_BTN_DOWN); settle(5); }
   shoot("thread-scrolled");
   for (int k = 0; k < 2; k++) { frame(COBALT_BTN_UP); settle(5); }

   frame(COBALT_BTN_BACK);
   settle(10);
   frame(COBALT_BTN_ALT_Y);
   settle(5);
   shoot("timeline-menu");

   /* A tap outside the popup's panel closes it without choosing anything. */
   {
      const cobalt_popup *p = cobalt_app_popup(g_app);
      CHECK(p != NULL && p->open && p->count >= 2);
      tap(5, 5);
      settle(3);
      CHECK(cobalt_app_popup(g_app) == NULL);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);

      /* A tap on a row picks it: row 0 is "View profile", which navigates. */
      frame(COBALT_BTN_ALT_Y);
      settle(5);
      p = cobalt_app_popup(g_app);
      CHECK(p != NULL && p->open && p->hit_valid);
      CHECK(p->hit[0].w > 0 && p->hit[0].h > 0);
      tap_rect_centre(&p->hit[0]);
      settle(40);
      CHECK(cobalt_app_popup(g_app) == NULL);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_PROFILE);
      /* The profile's More menu carries what X, Y and + used to do (cobalt#109). */
      {
         frame(COBALT_BTN_ALT_Y); settle(5);
         const cobalt_popup *p = cobalt_app_popup(g_app);
         CHECK(p != NULL && p->open);
         bool followers = false, following = false, tab = false;
         for (int i = 0; p && i < p->count; i++) {
            followers |= p->items[i].kind == COBALT_POPUP_FOLLOWERS;
            following |= p->items[i].kind == COBALT_POPUP_FOLLOWING;
            tab |= p->items[i].kind == COBALT_POPUP_TAB;
         }
         CHECK(followers && following && tab);
         shoot("profile-menu");
         frame(COBALT_BTN_BACK); settle(5);
         CHECK(cobalt_app_popup(g_app) == NULL);
         CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_PROFILE);
      }
      frame(COBALT_BTN_BACK);
      settle(10);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);

      /* The bottom-most row picks too, by its own rect. */
      frame(COBALT_BTN_ALT_Y);
      settle(5);
      p = cobalt_app_popup(g_app);
      CHECK(p != NULL && p->count >= 2);
      const int last = p->count - 1;
      tap_rect_centre(&p->hit[last]);
      settle(40);
      CHECK(cobalt_app_popup(g_app) == NULL);
      /* Reposted-by opens the actor-list screen; back returns to the timeline. */
      if (cobalt_app_screen(g_app) != COBALT_SCREEN_TIMELINE) {
         frame(COBALT_BTN_BACK);
         settle(10);
      }
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
   }

   /* Nested routes: timeline -> thread -> profile -> thread must unwind one
    * step at a time, back through each screen it came from, to the timeline.
    * A single return slot used to be overwritten by the inner thread, so B
    * bounced between the profile and the outer thread and never got back. */
   {
      frame(COBALT_BTN_CONFIRM); settle(30);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_THREAD);
      frame(COBALT_BTN_ALT_Y); settle(5);
      const int prof = popup_index(COBALT_POPUP_PROFILE);
      CHECK(prof >= 0);
      if (prof >= 0) {
         popup_select(prof);
         frame(COBALT_BTN_CONFIRM); settle(40);
      }
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_PROFILE);
      frame(COBALT_BTN_DOWN); settle(5);
      frame(COBALT_BTN_CONFIRM); settle(40);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_THREAD);
      frame(COBALT_BTN_BACK); settle(10);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_PROFILE);
      /* Going back to the outer thread fetches it again: the session held the
       * inner conversation, so without the refetch the outer screen showed it. */
      const int requests = cobalt_session_thread_requests();
      frame(COBALT_BTN_BACK);
      CHECK(cobalt_session_thread_requests() == requests + 1);
      settle(10);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_THREAD);
      frame(COBALT_BTN_BACK); settle(10);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
   }

   /* The TV's detail column (cobalt#111). A short post fits and has no "more"
    * line. A long one gets the line, and L and R scroll its text while the
    * selection stays where it is. */
   {
      int sel, scr;
      select_timeline(2); /* "third post with unicode": short */
      settle(3);
      CHECK(g_app->timeline.detail_max == 0);
      CHECK(g_app->timeline.detail_more.w == 0);

      select_timeline(3); /* the long post */
      settle(3);
      const int tmax = g_app->timeline.detail_max;
      const SDL_Rect more = g_app->timeline.detail_more;
      printf("timeline detail: max %d, more at %d,%d %dx%d\n", tmax, more.x, more.y, more.w, more.h);
      CHECK(tmax > 0);
      CHECK(more.w > 0 && more.h > 0);
      uint32_t more_sum; int more_dark;
      tv_region(&more, &more_sum, &more_dark);
      CHECK(more_dark >= 20); /* the marker's words are drawn, not just its rect recorded */

      const SDL_Rect col = tv_detail_column();
      uint32_t h0, h1, h2; int d0, d1, d2;
      tv_region(&col, &h0, &d0);

      /* (c) L and R scroll the text; the list selection does not move. */
      press_n(COBALT_BTN_SHOULDER_R, 2);
      cobalt_app_timeline_position(g_app, &sel, &scr);
      CHECK(g_app->timeline.detail_scroll == 2);
      CHECK(sel == 3);
      tv_region(&col, &h1, &d1);
      CHECK(h1 != h0);
      shoot("timeline-detail-scrolled");

      /* The offset stops at the text's end, and the list still does not move. */
      press_n(COBALT_BTN_SHOULDER_R, 20);
      CHECK(g_app->timeline.detail_scroll == tmax);
      cobalt_app_timeline_position(g_app, &sel, &scr);
      CHECK(sel == 3);
      /* One press back from the end is one line, not a jump: the state was
       * kept within the bound, not just clamped on the way to the screen. */
      press_n(COBALT_BTN_SHOULDER_L, 1);
      CHECK(g_app->timeline.detail_scroll == tmax - 1);
      press_n(COBALT_BTN_SHOULDER_L, 20);
      CHECK(g_app->timeline.detail_scroll == 0);
      tv_region(&col, &h2, &d2);
      CHECK(h2 == h0); /* back at the top, the same pixels as before */

      /* (d) Moving the selection starts the detail at the top again. */
      press_n(COBALT_BTN_SHOULDER_R, 20);
      CHECK(g_app->timeline.detail_scroll == tmax);
      frame(COBALT_BTN_DOWN); settle(3);
      cobalt_app_timeline_position(g_app, &sel, &scr);
      CHECK(sel == 4);
      CHECK(g_app->timeline.detail_scroll == 0);
      press_n(COBALT_BTN_SHOULDER_R, 20);
      frame(COBALT_BTN_UP); settle(3);
      cobalt_app_timeline_position(g_app, &sel, &scr);
      CHECK(sel == 3);
      CHECK(g_app->timeline.detail_scroll == 0);
      /* Leave the timeline on the long post's top, for the thread below. */
   }

   /* The thread's TV column scrolls the same way (cobalt#111). The fixture's
    * focused post is long, and its reply is what DOWN reaches. */
   {
      int sel0, sel;
      select_timeline(0);
      frame(COBALT_BTN_CONFIRM); settle(30);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_THREAD);
      settle(5);
      const int tmax = g_app->thread.detail_max;
      printf("thread detail: max %d\n", tmax);
      CHECK(tmax > 0);
      CHECK(g_app->thread.detail_more.w > 0);
      {
         uint32_t ms; int md;
         tv_region(&g_app->thread.detail_more, &ms, &md);
         CHECK(md >= 20);
      }

      const SDL_Rect col = tv_detail_column();
      uint32_t h0, h1; int d0, d1;
      tv_region(&col, &h0, &d0);
      sel0 = g_app->thread.nav.selected; /* no THREAD case in cobalt_app_view_position */
      press_n(COBALT_BTN_SHOULDER_R, 2);
      CHECK(g_app->thread.detail_scroll == 2);
      sel = g_app->thread.nav.selected;
      CHECK(sel == sel0);
      tv_region(&col, &h1, &d1);
      CHECK(h1 != h0);
      shoot("thread-detail-scrolled");
      press_n(COBALT_BTN_SHOULDER_R, 20);
      CHECK(g_app->thread.detail_scroll == tmax);
      press_n(COBALT_BTN_SHOULDER_L, 1);
      CHECK(g_app->thread.detail_scroll == tmax - 1);

      /* Moving to the reply, with DOWN, starts its column at the top. Up/Down
       * read the GamePad card's text before they leave a post, so keep pressing
       * until the selection has moved. */
      press_n(COBALT_BTN_SHOULDER_R, 20);
      CHECK(g_app->thread.detail_scroll == tmax);
      for (int k = 0; k < 200; k++) {
         sel = g_app->thread.nav.selected;
         if (sel != sel0) break;
         frame(COBALT_BTN_DOWN); settle(1);
      }
      sel = g_app->thread.nav.selected;
      CHECK(sel != sel0);
      CHECK(g_app->thread.detail_scroll == 0);

      frame(COBALT_BTN_BACK); settle(10);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
   }

   /* Facets: timeline card 1 carries a mention, a link and a tag. */
   open_post_menu(1);
   {
      const int link = popup_index(COBALT_POPUP_LINK);
      CHECK(popup_index(COBALT_POPUP_MENTION) >= 0);
      CHECK(popup_index(COBALT_POPUP_TAG) >= 0);
      CHECK(link >= 0);
      shoot("timeline-menu-facets");
      popup_select(link);
      frame(COBALT_BTN_CONFIRM); settle(3);
      const cobalt_popup *p = popup();
      CHECK(p != NULL && p->text_mode);
      CHECK(p != NULL && strstr(p->text, "example.com/page") != NULL);
      shoot("timeline-menu-link");
      frame(COBALT_BTN_BACK); settle(3);
      CHECK(popup() == NULL);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
   }

   /* Profile: card 0's menu opens the author's profile as its first row. */
   open_post_menu(0);
   {
      const int prof = popup_index(COBALT_POPUP_PROFILE);
      CHECK(prof >= 0);
      popup_select(prof);
      frame(COBALT_BTN_CONFIRM); settle(40);
      CHECK(popup() == NULL);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_PROFILE);
   }
   shoot("profile");

   frame(COBALT_BTN_BACK); settle(10);
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
   go_home();
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_HOME);
   shoot("home-signedin");

   open_home_item(5);
   settle(40);
   shoot("notifications");

   /* Y opens the More menu here as it does on the timeline (cobalt#109). */
   {
      frame(COBALT_BTN_ALT_Y); settle(5);
      const cobalt_popup *p = cobalt_app_popup(g_app);
      CHECK(p != NULL && p->open && p->count >= 2);
      bool has_profile = false, has_refresh = false;
      for (int i = 0; p && i < p->count; i++) {
         has_profile |= p->items[i].kind == COBALT_POPUP_PROFILE;
         has_refresh |= p->items[i].kind == COBALT_POPUP_REFRESH;
      }
      CHECK(has_profile && has_refresh);
      shoot("notifications-menu");
      frame(COBALT_BTN_BACK); settle(5);
      CHECK(cobalt_app_popup(g_app) == NULL);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_NOTIFICATIONS);
   }

   /* Opening a notification's thread or profile and backing out keeps the cursor
    * and scroll (cobalt#110). */
   {
      int sel0, scr0, sel1, scr1;
      frame(COBALT_BTN_DOWN); settle(3);
      cobalt_app_view_position(g_app, COBALT_SCREEN_NOTIFICATIONS, &sel0, &scr0);
      frame(COBALT_BTN_CONFIRM); settle(30);
      if (cobalt_app_screen(g_app) != COBALT_SCREEN_NOTIFICATIONS) {
         frame(COBALT_BTN_BACK); settle(10);
      }
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_NOTIFICATIONS);
      cobalt_app_view_position(g_app, COBALT_SCREEN_NOTIFICATIONS, &sel1, &scr1);
      printf("notifications position %d/%d -> %d/%d\n", sel0, scr0, sel1, scr1);
      CHECK(sel1 == sel0 && scr1 == scr0);
   }

   /* The DM inbox is read-only: the screen opens and says what it can do. */
   open_home_item(6);
   settle(40);
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_MESSAGES);
   shoot("messages");
   /* X starts a conversation: the keyboard takes the list's place. */
   frame(COBALT_BTN_ALT_X);
   settle(5);
   shoot("messages-start");
   tap(668, 372);
   settle(5);
   /* Opening the conversation fetches its messages and shows them as bubbles. */
   frame(COBALT_BTN_CONFIRM);
   settle(40);
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_CHAT_THREAD);
   shoot("chat-thread");
   /* A opens the keyboard for a reply. On the keyboard B is backspace, so it is
    * left by touching its Cancel key, which sits on the bottom row of the GamePad. */
   frame(COBALT_BTN_CONFIRM);
   settle(5);
   shoot("chat-reply");
   tap(694, 452);
   settle(5);

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(10);
   settle(10);
   shoot("diagnostics");

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(1);
   settle(10);
   shoot("compose");

   /* B is backspace on the keyboard, so leave through its Cancel key: the
    * focus starts on row 2, so two Downs reach the function row and Left
    * wraps from column 0 to Cancel. */
   for (int k = 0; k < 2; k++) {
      frame(COBALT_BTN_DOWN);
      settle(2);
   }
   frame(COBALT_BTN_LEFT);   /* wraps to Emoji, the last key */
   settle(2);
   frame(COBALT_BTN_LEFT);   /* then Cancel */
   settle(2);
   frame(COBALT_BTN_CONFIRM);
   settle(5);
   open_home_item(8);
   settle(10);
   shoot("account");

   /* Queued posts: an empty queue, then one with two saved posts. */
   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(7);
   settle(20);
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_QUEUE);
   shoot("queue-empty");
   frame(COBALT_BTN_BACK);
   settle(10);
   CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_HOME);
   cobalt_post_queue_add(&g_app->queue, "Saved for later: a draft I wrote on the bus.", 1700000000);
   cobalt_post_queue_add(&g_app->queue, "Second post, waiting until I am online.", 1700000600);
   open_home_item(7);
   settle(20);
   shoot("queue");

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(2);
   settle(10);
   shoot("search");

   for (int k = 0; k < 2; k++) { frame(COBALT_BTN_DOWN); settle(2); }
   frame(COBALT_BTN_LEFT);
   settle(2);
   frame(COBALT_BTN_LEFT);
   settle(2);
   frame(COBALT_BTN_CONFIRM);
   settle(5);
   open_home_item(3);
   settle(10);
   settle(30);
   CHECK(cobalt_session_saved_feeds()->count == 2);
   CHECK(strcmp(cobalt_session_saved_feeds()->feeds[0].label, "Cat Pics") == 0);
   CHECK(strcmp(cobalt_session_saved_feeds()->feeds[1].label, "poetry") == 0);
   shoot("feeds");

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(4);
   settle(40);
   shoot("lists");

   /* Tapping the header's Back pill on the GamePad leaves the screen. */
   {
      tap(800, 38);
      settle(3);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_HOME);
   }

   /* The Back pill is drawn on every screen that does not give B to the
    * keyboard, and tapping it must leave each one (cobalt#112). The home items
    * are: timeline 0, notifications 5, dms 6, queue 7, account 8, updates 9, diagnostics
    * 10, and the two lists, feeds 3 and lists 4. Compose and search are left
    * out on purpose: B types there, so they have no pill. */
   {
       static const int items[] = {0, 3, 4, 5, 6, 8, 9, 10};
      for (size_t k = 0; k < sizeof items / sizeof items[0]; k++) {
         open_home_item(items[k]);
         settle(20);
         CHECK(cobalt_app_screen(g_app) != COBALT_SCREEN_HOME);
         tap(800, 38);
         settle(3);
         if (cobalt_app_screen(g_app) != COBALT_SCREEN_HOME) {
            printf("back pill did not leave home item %d\n", items[k]);
         }
         CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_HOME);
      }
   }

   /* A tap where the Back pill would be does nothing on a screen without one:
    * the sign-in-style keyboard screens own B, and home has nowhere to go. */
   {
      go_home();
      settle(4);
      tap(800, 38);
      settle(3);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_HOME);
   }

   /* Tapping a hint pill is the button it names. The timeline footer carries
    * "A: thread  Y: more  Left: like  Right: repost", so a tap on "Y" opens
    * the post menu and a tap on "A" opens the thread. */
   {
      SDL_Rect r;
      char key[8];

      /* Home's own footer is "TV + GamePad": a status word, not a prompt, so
       * it names no button and must not be tappable. Checked here because
       * that is the case a mappable-key assumption would get wrong. */
      go_home();
      settle(4);
      CHECK(cobalt_hints_pill(0, &r, key, sizeof key) == 0);

      open_home_item(0); /* timeline */
      settle(40);

      int a = -1, y = -1;
      for (int i = 0; i < 16; i++) {
         if (cobalt_hints_pill(i, &r, key, sizeof key) != 1) break;
         if (a < 0 && strcmp(key, "A") == 0) a = i;
         if (y < 0 && strcmp(key, "Y") == 0) y = i;
      }
      CHECK(a >= 0);
      CHECK(y >= 0);

      if (y >= 0) {
         cobalt_hints_pill(y, &r, key, sizeof key);
         tap_rect_centre(&r);
         settle(5);
         CHECK(cobalt_app_popup(g_app) != NULL);
         /* While the popup is up it owns the frame: the tap that opened it
          * must not also have pressed the button underneath. */
         CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
         frame(COBALT_BTN_BACK);
         settle(3);
         CHECK(cobalt_app_popup(g_app) == NULL);
      }

      if (a >= 0) {
         cobalt_hints_pill(a, &r, key, sizeof key);
         tap_rect_centre(&r);
         settle(10);
         CHECK(cobalt_app_screen(g_app) != COBALT_SCREEN_TIMELINE);
         frame(COBALT_BTN_BACK);
         settle(10);
         CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_TIMELINE);
      }
   }

   wf_mock_pds_free(pds);
   cobalt_app_destroy(g_app);
   cobalt_render_destroy(g_tv);
   cobalt_render_destroy(g_drc);
   SDL_Quit();
   printf("snapshot: %d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
