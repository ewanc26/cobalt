/* Shared mock-PDS fixtures for the snapshot run and the live simulator. */
#pragma once

#include "../../wolfram/test/mock_pds.h"
#include <stdio.h>

static const char *
post_json(const char *n, const char *text)
{
   static char buf[4][2000];
   static int k;
   char *b = buf[k++ & 3];
   snprintf(b, 2000,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/%s\",\"cid\":\"bafy%s\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\","
      "\"displayName\":\"Ewan Croft\"},\"record\":{\"$type\":\"app.bsky.feed.post\","
      "\"text\":\"%s\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"likeCount\":12,\"repostCount\":3,\"replyCount\":2,"
      "\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}", n, n, text);
   return b;
}

static const char *
post_json_facets(const char *n, const char *text, const char *facets)
{
   static char buf[2][2400];
   static int k;
   char *b = buf[k++ & 1];
   snprintf(b, 2400,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/%s\",\"cid\":\"bafy%s\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\","
      "\"displayName\":\"Ewan Croft\"},\"record\":{\"$type\":\"app.bsky.feed.post\","
      "\"text\":\"%s\",\"facets\":%s,\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"likeCount\":1,\"repostCount\":0,\"replyCount\":0,"
      "\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}", n, n, text, facets);
   return b;
}

static void
register_fixtures(wf_mock_pds *pds)
{
   wf_mock_pds_register(pds, "com.atproto.server.createSession",
      "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\","
      "\"accessJwt\":\"a.b.c\",\"refreshJwt\":\"d.e.f\",\"active\":true}");
   static char longp[1600];
   snprintf(longp, sizeof longp, "%s", post_json("4", "A very long post that keeps going and going so the card has to clip or wrap it sensibly: lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua ut enim ad minim veniam quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat duis aute irure dolor in reprehenderit."));
   static char imgp[2200];
   snprintf(imgp, sizeof imgp,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/5\",\"cid\":\"bafy5\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "\"record\":{\"text\":\"Photos and a link\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.images#view\",\"images\":[{\"thumb\":\"https://127.0.0.1:1/a.jpg\","
      "\"fullsize\":\"https://127.0.0.1:1/a.jpg\",\"alt\":\"A cat on a sofa\"},{\"thumb\":\"https://127.0.0.1:1/b.jpg\","
      "\"fullsize\":\"https://127.0.0.1:1/b.jpg\",\"alt\":\"\"}]},"
      "\"likeCount\":1,\"repostCount\":0,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   static char extp[2200];
   snprintf(extp, sizeof extp,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/6\",\"cid\":\"bafy6\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "\"record\":{\"text\":\"A link card\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.external#view\",\"external\":{\"uri\":\"https://example.com/some/very/long/path/to/an/article\","
      "\"title\":\"An article with a rather long title that needs truncating somewhere\",\"description\":\"A description that is also long enough to need clipping in the card.\","
      "\"thumb\":\"https://127.0.0.1:1/t.jpg\",\"readingTime\":4,"
      "\"source\":{\"$type\":\"app.bsky.embed.external#viewExternalSource\",\"uri\":\"https://example.com\",\"title\":\"Example Press\"}}},"
      "\"likeCount\":0,\"repostCount\":0,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   /* A gallery is a separate view shape from images#view. The first four are
    * drawn; the fixture carries three so the row is visibly a gallery. */
   static char galp[2800];
   snprintf(galp, sizeof galp,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/7\",\"cid\":\"bafy7\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "\"record\":{\"text\":\"Three pictures from the gallery\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.gallery#view\",\"items\":["
      "{\"thumbnail\":\"https://127.0.0.1:1/g1.jpg\",\"fullsize\":\"https://127.0.0.1:1/g1.jpg\",\"alt\":\"Gallery one\",\"aspectRatio\":{\"width\":4,\"height\":3}},"
      "{\"thumbnail\":\"https://127.0.0.1:1/g2.jpg\",\"fullsize\":\"https://127.0.0.1:1/g2.jpg\",\"alt\":\"Gallery two\"},"
      "{\"thumbnail\":\"https://127.0.0.1:1/g3.jpg\",\"fullsize\":\"https://127.0.0.1:1/g3.jpg\",\"alt\":\"Gallery three\"}]},"
      "\"likeCount\":2,\"repostCount\":0,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   /* Links backed by Atmosphere records. Each plain view has the title and
    * host a post itself carries; the fake getEmbedExternalView response in
    * snapshot_main.c supplies the richer data the card upgrades to. */
   static char hy_article[2800];
   snprintf(hy_article, sizeof hy_article,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/8\",\"cid\":\"bafy8\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "\"record\":{\"text\":\"An article behind a link\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.external#view\",\"external\":{\"uri\":\"https://example.com/article\","
      "\"title\":\"A plain article card\",\"description\":\"The post view has only the link title.\","
      "\"thumb\":\"https://127.0.0.1:1/article.jpg\","
      "\"associatedRefs\":[{\"uri\":\"at://did:plc:abc/site.standard.document/article\",\"cid\":\"bafyart\"}]}},"
      "\"likeCount\":5,\"repostCount\":1,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   static char hy_gallery[3000];
   snprintf(hy_gallery, sizeof hy_gallery,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/9\",\"cid\":\"bafy9\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "\"record\":{\"text\":\"A gallery behind a link\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.external#view\",\"external\":{\"uri\":\"https://example.com/gallery\","
      "\"title\":\"A plain gallery card\",\"description\":\"The post view has only the link title.\","
      "\"thumb\":\"https://127.0.0.1:1/gallery.jpg\","
      "\"associatedRefs\":[{\"uri\":\"at://did:plc:abc/social.grain.gallery/3g\",\"cid\":\"bafygal\"}]}},"
      "\"likeCount\":8,\"repostCount\":0,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   static char hy_live[2800];
   snprintf(hy_live, sizeof hy_live,
      "{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/10\",\"cid\":\"bafy10\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "\"record\":{\"text\":\"A livestream behind a link\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"embed\":{\"$type\":\"app.bsky.embed.external#view\",\"external\":{\"uri\":\"https://example.com/live\","
      "\"title\":\"A plain livestream card\",\"description\":\"The post view has only the link title.\","
      "\"thumb\":\"https://127.0.0.1:1/live.jpg\","
      "\"associatedRefs\":[{\"uri\":\"at://did:plc:abc/app.bsky.video.livestream/3live\",\"cid\":\"bafylive\"}]}},"
      "\"likeCount\":3,\"repostCount\":0,\"replyCount\":0,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}");
   static char tl[24000];
   snprintf(tl, sizeof tl, "{\"feed\":[{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s},{\"post\":%s}],\"cursor\":\"c1\"}",
      post_json("1", "Hello from the Wii U. A longer post that should wrap across several lines on both the television and the GamePad so we can see how the card handles it."),
      post_json_facets("2", "Hi @bob.test see https://example.com/page and #wiiu rocks", "[{\"index\":{\"byteStart\":3,\"byteEnd\":12},\"features\":[{\"$type\":\"app.bsky.richtext.facet#mention\",\"did\":\"did:plc:bob\"}]},{\"index\":{\"byteStart\":17,\"byteEnd\":41},\"features\":[{\"$type\":\"app.bsky.richtext.facet#link\",\"uri\":\"https://example.com/page\"}]},{\"index\":{\"byteStart\":46,\"byteEnd\":51},\"features\":[{\"$type\":\"app.bsky.richtext.facet#tag\",\"tag\":\"wiiu\"}]}]"), post_json("3", "third post with unicode: caf\u00e9 \u65e5\u672c\u8a9e \\ud83d\\ude00"), longp, imgp, extp, galp, hy_article, hy_gallery, hy_live, post_json("11", "One more post after the hydrated cards, so reaching them does not page."));
   wf_mock_pds_register(pds, "app.bsky.feed.getTimeline", tl);
   char th[6000];
   snprintf(th, sizeof th, "{\"thread\":{\"$type\":\"app.bsky.feed.defs#threadViewPost\",\"post\":%s,"
      "\"replies\":[{\"$type\":\"app.bsky.feed.defs#threadViewPost\",\"post\":%s,\"replies\":[]}]}}",
      post_json("1", "Line 1 of a long thread post that must scroll. Line 2 of a long thread post that must scroll. Line 3 of a long thread post that must scroll. Line 4 of a long thread post that must scroll. Line 5 of a long thread post that must scroll. Line 6 of a long thread post that must scroll. Line 7 of a long thread post that must scroll. Line 8 of a long thread post that must scroll. Line 9 of a long thread post that must scroll. Line 10 of a long thread post that must scroll. Line 11 of a long thread post that must scroll. Line 12 of a long thread post that must scroll. Line 13 of a long thread post that must scroll. Line 14 of a long thread post that must scroll. Line 15 of a long thread post that must scroll. Line 16 of a long thread post that must scroll."), post_json("9", "A reply in the thread."));
   wf_mock_pds_register(pds, "app.bsky.feed.getPostThread", th);
   wf_mock_pds_register(pds, "app.bsky.actor.getProfile",
      "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\","
      "\"description\":\"Poet, developer, Wii U enjoyer.\",\"followersCount\":120,\"followsCount\":80,\"postsCount\":456}");
   wf_mock_pds_register(pds, "app.bsky.feed.getAuthorFeed", tl);
   wf_mock_pds_register(pds, "app.bsky.notification.listNotifications",
      "{\"notifications\":[{\"uri\":\"at://did:plc:abc/app.bsky.feed.like/1\",\"cid\":\"c1\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "\"reason\":\"like\",\"reasonSubject\":\"at://did:plc:abc/app.bsky.feed.post/1\",\"record\":{},"
      "\"isRead\":false,\"indexedAt\":\"2026-10-01T10:00:00.000Z\"},"
      "{\"uri\":\"at://did:plc:abc/app.bsky.graph.follow/2\",\"cid\":\"c2\","
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"bob.test\",\"displayName\":\"Bob\"},"
      "\"reason\":\"follow\",\"record\":{},\"isRead\":true,\"indexedAt\":\"2026-10-01T09:00:00.000Z\"}]}");
   /* Direct messages: one conversation with a message from Bob, and the
    * thread itself, newest first as the server sends it. */
   wf_mock_pds_register(pds, "chat.bsky.convo.listConvos",
      "{\"convos\":[{\"id\":\"convo-bob\",\"rev\":\"3\","
      "\"members\":[{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"},"
      "{\"did\":\"did:plc:bob\",\"handle\":\"bob.test\",\"displayName\":\"Bob\"}],"
      "\"lastMessage\":{\"$type\":\"chat.bsky.convo.defs#messageView\",\"id\":\"m3\",\"rev\":\"3\","
      "\"text\":\"See you Saturday. Bring the good scissors.\",\"sender\":{\"did\":\"did:plc:bob\"},"
      "\"sentAt\":\"2026-10-01T10:00:00.000Z\"},\"unreadCount\":2}]}");
   wf_mock_pds_register(pds, "chat.bsky.convo.getMessages",
      "{\"messages\":["
      "{\"$type\":\"chat.bsky.convo.defs#messageView\",\"id\":\"m3\",\"rev\":\"3\","
      "\"text\":\"See you Saturday. Bring the good scissors.\",\"sender\":{\"did\":\"did:plc:bob\"},\"sentAt\":\"2026-10-01T10:00:00.000Z\"},"
      "{\"$type\":\"chat.bsky.convo.defs#messageView\",\"id\":\"m2\",\"rev\":\"2\","
      "\"text\":\"Yes, still on for Saturday?\",\"sender\":{\"did\":\"did:plc:abc\"},\"sentAt\":\"2026-10-01T09:30:00.000Z\"},"
      "{\"$type\":\"chat.bsky.convo.defs#messageView\",\"id\":\"m1\",\"rev\":\"1\","
      "\"text\":\"Hello! Did you get my note about the hedge?\",\"sender\":{\"did\":\"did:plc:bob\"},\"sentAt\":\"2026-10-01T09:00:00.000Z\"}"
      "]}");
   char sr[2048];
   snprintf(sr, sizeof sr, "{\"actors\":[{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\",\"displayName\":\"Ewan Croft\"}]}");
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
      "\"creator\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\"}}]}");

}
