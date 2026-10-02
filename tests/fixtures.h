/* Shared mock-PDS fixtures for the snapshot run and the live simulator. */
#pragma once

#include "../../wolfram/test/mock_pds.h"
#include <stdio.h>

static const char *
post_json(const char *n, const char *text)
{
   static char buf[4][1400];
   static int k;
   char *b = buf[k++ & 3];
   snprintf(b, 1400,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/%s\",\"cid\":\"bafy%s\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\","
      "\"displayName\":\"Alice Example\"},\"record\":{\"$type\":\"app.bsky.feed.post\","
      "\"text\":\"%s\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"likeCount\":12,\"repostCount\":3,\"replyCount\":2,"
      "\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}", n, n, text);
   return b;
}

static void
register_fixtures(wf_mock_pds *pds)
{
   wf_mock_pds_register(pds, "com.atproto.server.createSession",
      "{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\","
      "\"accessJwt\":\"a.b.c\",\"refreshJwt\":\"d.e.f\",\"active\":true}");
   static char longp[1600];
   snprintf(longp, sizeof longp, "%s", post_json("4", "A very long post that keeps going and going so the card has to clip or wrap it sensibly: lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua ut enim ad minim veniam quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat duis aute irure dolor in reprehenderit."));
   static char imgp[2200];
   snprintf(imgp, sizeof imgp,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/5\",\"cid\":\"bafy5\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\",\"displayName\":\"Alice Example\"},"
      "\"record\":{\"text\":\"Photos and a link\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.images#view\",\"images\":[{\"thumb\":\"https://127.0.0.1:1/a.jpg\","
      "\"fullsize\":\"https://127.0.0.1:1/a.jpg\",\"alt\":\"A cat on a sofa\"},{\"thumb\":\"https://127.0.0.1:1/b.jpg\","
      "\"fullsize\":\"https://127.0.0.1:1/b.jpg\",\"alt\":\"\"}]},"
      "\"likeCount\":1,\"repostCount\":0,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   static char extp[2200];
   snprintf(extp, sizeof extp,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/6\",\"cid\":\"bafy6\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\",\"displayName\":\"Alice Example\"},"
      "\"record\":{\"text\":\"A link card\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.external#view\",\"external\":{\"uri\":\"https://example.com/some/very/long/path/to/an/article\","
      "\"title\":\"An article with a rather long title that needs truncating somewhere\",\"description\":\"A description that is also long enough to need clipping in the card.\","
      "\"thumb\":\"https://127.0.0.1:1/t.jpg\"}},"
      "\"likeCount\":0,\"repostCount\":0,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   static char tl[9000];
   snprintf(tl, sizeof tl, "{\"feed\":[{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s}],\"cursor\":\"c1\"}",
      post_json("1", "Hello from the Wii U. A longer post that should wrap across several lines on both the television and the GamePad so we can see how the card handles it."),
      post_json("2", "second post"), post_json("3", "third post with unicode: caf\u00e9 \u65e5\u672c\u8a9e \\ud83d\\ude00"), longp, imgp, extp);
   wf_mock_pds_register(pds, "app.bsky.feed.getTimeline", tl);
   char th[4000];
   snprintf(th, sizeof th, "{\"thread\":{\"$type\":\"app.bsky.feed.defs#threadViewPost\",\"post\":%s,"
      "\"replies\":[{\"$type\":\"app.bsky.feed.defs#threadViewPost\",\"post\":%s,\"replies\":[]}]}}",
      post_json("1", "Hello from the Wii U."), post_json("9", "A reply in the thread."));
   wf_mock_pds_register(pds, "app.bsky.feed.getPostThread", th);
   wf_mock_pds_register(pds, "app.bsky.actor.getProfile",
      "{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\",\"displayName\":\"Alice Example\","
      "\"description\":\"Poet, developer, Wii U enjoyer.\",\"followersCount\":120,\"followsCount\":80,\"postsCount\":456}");
   wf_mock_pds_register(pds, "app.bsky.feed.getAuthorFeed", tl);
   wf_mock_pds_register(pds, "app.bsky.notification.listNotifications",
      "{\"notifications\":[{\"uri\":\"at://did:plc:abc/app.bsky.feed.like/1\",\"cid\":\"c1\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\",\"displayName\":\"Alice Example\"},"
      "\"reason\":\"like\",\"reasonSubject\":\"at://did:plc:abc/app.bsky.feed.post/1\",\"record\":{},"
      "\"isRead\":false,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"},"
      "{\"uri\":\"at://did:plc:abc/app.bsky.graph.follow/2\",\"cid\":\"c2\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"bob.test\",\"displayName\":\"Bob\"},"
      "\"reason\":\"follow\",\"record\":{},\"isRead\":true,\"indexedAt\":\"2026-10-01T09:00:00.000Z\"}]}");
   char sr[2048];
   snprintf(sr, sizeof sr, "{\"actors\":[{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\",\"displayName\":\"Alice Example\"}]}");
   wf_mock_pds_register(pds, "app.bsky.actor.searchActors", sr);

   wf_mock_pds_register(pds, "app.bsky.feed.getFeed", tl);
   wf_mock_pds_register(pds, "app.bsky.actor.getPreferences",
      "{\"preferences\":[{\"$type\":\"app.bsky.actor.defs#savedFeedsPrefV2\",\"items\":["
      "{\"type\":\"timeline\",\"value\":\"following\",\"pinned\":true,\"id\":\"a\"},"
      "{\"type\":\"feed\",\"value\":\"at://did:plc:x/app.bsky.feed.generator/cats\",\"pinned\":true,\"id\":\"b\"},"
      "{\"type\":\"feed\",\"value\":\"at://did:plc:x/app.bsky.feed.generator/poetry\",\"pinned\":false,\"id\":\"c\"}]}]}");
   wf_mock_pds_register(pds, "app.bsky.feed.getFeedGenerators",
      "{\"feeds\":[{\"uri\":\"at://did:plc:x/app.bsky.feed.generator/cats\",\"cid\":\"g1\","
      "\"did\":\"did:web:x\",\"creator\":{\"did\":\"did:plc:x\",\"handle\":\"x.test\"},"
      "\"displayName\":\"Cat Pics\",\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}]}");
   wf_mock_pds_register(pds, "app.bsky.graph.getLists",
      "{\"lists\":[{\"uri\":\"at://did:plc:abc/app.bsky.graph.list/1\",\"cid\":\"l1\","
      "\"name\":\"Friends\",\"purpose\":\"app.bsky.graph.defs#curatelist\","
      "\"description\":\"People worth following\",\"listItemCount\":3,"
      "\"creator\":{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\"}}]}");

}
