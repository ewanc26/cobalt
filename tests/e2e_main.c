/*
 * Host end-to-end: drives the real session worker and Wolfram against a mock
 * PDS on localhost. Covers sign-in, timeline, post search and custom-feed
 * refresh through the same entry points the UI uses. Not run by `make test`;
 * see `make e2e`.
 */
#include "atproto/session.h"
#include "../../wolfram/test/mock_pds.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
   printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool
wait_job(cobalt_job_result *r)
{
   for (int i = 0; i < 2000; i++) {
      if (cobalt_session_poll(r)) return true;
      SDL_Delay(5);
   }
   return false;
}

static const char *
post_json(const char *n, const char *text)
{
   static char buf[4][1024];
   static int k;
   char *b = buf[k++ & 3];
   snprintf(b, 1024,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/%s\",\"cid\":\"bafy%s\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\","
      "\"displayName\":\"Alice\"},\"record\":{\"$type\":\"app.bsky.feed.post\","
      "\"text\":\"%s\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}", n, n, text);
   return b;
}

int
main(int argc, char **argv)
{
   (void) argc; (void) argv;
   /* Init reports a blocker on the host (no CA file, no console entropy, no
    * mbedTLS RNG hook). Those gate HTTPS on the console and are irrelevant to
    * plain HTTP against localhost; the worker runs regardless. */
   (void) cobalt_session_init();
   printf("session blocker: %s\n", cobalt_session_blocker() ? cobalt_session_blocker() : "none");

   wf_mock_pds *pds = NULL;
   int port = 0;
   CHECK(wf_mock_pds_start(&pds, &port) == WF_OK);

   wf_mock_pds_register(pds, "com.atproto.server.createSession",
      "{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\","
      "\"accessJwt\":\"a.b.c\",\"refreshJwt\":\"d.e.f\",\"active\":true}");

   char tl[2048];
   snprintf(tl, sizeof tl, "{\"feed\":[{\"post\":%s},{\"post\":%s}],\"cursor\":\"c1\"}",
            post_json("1", "hello timeline"), post_json("2", "second"));
   wf_mock_pds_register(pds, "app.bsky.feed.getTimeline", tl);

   char sr[1024];
   snprintf(sr, sizeof sr, "{\"posts\":[%s],\"cursor\":\"s1\"}",
            post_json("3", "found by search"));
   wf_mock_pds_register(pds, "app.bsky.feed.searchPosts", sr);

   char fd[1024];
   snprintf(fd, sizeof fd, "{\"feed\":[{\"post\":%s}]}",
            post_json("4", "from a custom feed"));
   wf_mock_pds_register(pds, "app.bsky.feed.getFeed", fd);

   char svc[64];
   snprintf(svc, sizeof svc, "http://127.0.0.1:%d", port);

   cobalt_job_result r;
   CHECK(cobalt_session_begin_login(svc, "alice.test", "app-pass"));
   CHECK(wait_job(&r));
   if (!r.ok) printf("login failed: %s\n", r.message);
   CHECK(r.ok);
   CHECK(cobalt_session_state() == COBALT_AUTH_SIGNED_IN);

   CHECK(cobalt_session_begin_timeline(false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_feed()->count == 2);
   CHECK(strcmp(cobalt_session_feed()->posts[0].text, "hello timeline") == 0);

   CHECK(cobalt_session_begin_search_posts("found", false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_feed()->count == 1);
   CHECK(strcmp(cobalt_session_feed()->posts[0].text, "found by search") == 0);
   CHECK(cobalt_session_feed()->has_more);

   /* Refresh follows the search, not the home timeline. */
   CHECK(cobalt_session_begin_feed_current(false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(strcmp(cobalt_session_feed()->posts[0].text, "found by search") == 0);

   CHECK(cobalt_session_begin_feed("at://did:plc:abc/app.bsky.feed.generator/g", false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(strcmp(cobalt_session_feed()->posts[0].text, "from a custom feed") == 0);
   CHECK(cobalt_session_begin_feed_current(false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(strcmp(cobalt_session_feed()->posts[0].text, "from a custom feed") == 0);

   CHECK(cobalt_session_begin_timeline(false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_feed()->count == 2);

   wf_mock_pds_free(pds);
   printf("%d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
