/*
 * Host end-to-end: drives the real session worker and Wolfram against a mock
 * PDS on localhost. Covers sign-in, timeline, post search and custom-feed
 * refresh through the same entry points the UI uses. Not run by `make test`;
 * see `make e2e`.
 */
#include "app/chat_thread.h"
#include "app/messages.h"
#include "atproto/prefs.h"
#include "atproto/session.h"
#include "input/input.h"
#include "../../wolfram/test/mock_pds.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
   printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b)) != 0) { failures++; \
   printf("FAIL %s:%d \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

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
      "\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\","
      "\"displayName\":\"Ewan\"},\"record\":{\"$type\":\"app.bsky.feed.post\","
      "\"text\":\"%s\",\"createdAt\":\"2026-10-01T10:00:00.000Z\"},"
      "\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}", n, n, text);
   return b;
}

/* Muted-word matching is Wolfram's wf_mod_match_mute_words; these pin the
 * behaviour Cobalt relies on, so a Wolfram change that breaks it shows here. */
static void
test_muted_words(void)
{
   cobalt_prefs p;
   const char *tags[] = {"Spoilers"};

   cobalt_prefs_clear(&p);
   CHECK(!cobalt_prefs_text_is_muted(&p, "anything", NULL, 0));
   CHECK(wf_muted_list_add(&p.muted, "cat", true, false, false, NULL));
   CHECK(cobalt_prefs_text_is_muted(&p, "I like my Cat.", NULL, 0));
   CHECK(cobalt_prefs_text_is_muted(&p, "cat", NULL, 0));
   CHECK(!cobalt_prefs_text_is_muted(&p, "a category of things", NULL, 0));
   CHECK(!cobalt_prefs_text_is_muted(&p, "concatenate", NULL, 0));

   cobalt_prefs_clear(&p);
   CHECK(wf_muted_list_add(&p.muted, "good morning", true, false, false, NULL));
   CHECK(cobalt_prefs_text_is_muted(&p, "oh, GOOD MORNING all", NULL, 0));
   CHECK(!cobalt_prefs_text_is_muted(&p, "good evening", NULL, 0));

   cobalt_prefs_clear(&p);
   CHECK(wf_muted_list_add(&p.muted, "#spoilers", false, true, false, NULL));
   CHECK(cobalt_prefs_text_is_muted(&p, "text", tags, 1));
   CHECK(!cobalt_prefs_text_is_muted(&p, "spoilers in text", NULL, 0));

   /* And through the feed filter, which is what the timeline calls. */
   static cobalt_feed feed;
   memset(&feed, 0, sizeof(feed));
   cobalt_prefs_clear(&p);
   CHECK(wf_muted_list_add(&p.muted, "ban", true, false, false, NULL));
   for (int i = 0; i < 4; i++) snprintf(feed.posts[i].text, sizeof(feed.posts[i].text), "post %d", i);
   snprintf(feed.posts[1].text, sizeof(feed.posts[1].text), "a BAN here");
   feed.count = 4;
   CHECK(cobalt_prefs_filter_feed(&p, &feed, 1, false) == 1);
   CHECK(feed.count == 3);
   CHECK(strcmp(feed.posts[1].text, "post 2") == 0);
}

/* --- a counting loopback server for the message request tests ---
 *
 * Wolfram's mock PDS keeps only the last request and keys by NSID, so a test
 * that must say how many accepts were sent needs its own server. Each
 * connection is one request, answered with Connection: close. A route can be
 * held: its request is counted on arrival, and the answer waits until
 * cs_release, which is how a test keeps a job in flight on purpose. */
#define CS_ROUTES 12

typedef struct {
   char nsid[80];
   int status;
   char body[1024];
   int hits;
   bool hold;
} cs_route;

typedef struct {
   int fd;
   int port;
   SDL_Thread *thread;
   SDL_mutex *lock;
   bool stop;
   bool release;
   cs_route routes[CS_ROUTES];
   int route_count;
} cs_server;

static cs_route *
cs_find_locked(cs_server *s, const char *nsid)
{
   for (int i = 0; i < s->route_count; i++) {
      if (strcmp(s->routes[i].nsid, nsid) == 0) {
         return &s->routes[i];
      }
   }
   return NULL;
}

/* Set the answer for an NSID. A held route stops at its next request. */
static void
cs_set(cs_server *s, const char *nsid, int status, const char *body)
{
   SDL_LockMutex(s->lock);
   cs_route *rt = cs_find_locked(s, nsid);
   if (!rt && s->route_count < CS_ROUTES) {
      rt = &s->routes[s->route_count++];
      memset(rt, 0, sizeof(*rt));
      snprintf(rt->nsid, sizeof(rt->nsid), "%s", nsid);
   }
   if (rt) {
      rt->status = status;
      snprintf(rt->body, sizeof(rt->body), "%s", body);
   }
   SDL_UnlockMutex(s->lock);
}

/* The next request to this NSID is held until cs_release. */
static void
cs_hold(cs_server *s, const char *nsid)
{
   SDL_LockMutex(s->lock);
   cs_route *rt = cs_find_locked(s, nsid);
   if (rt) {
      rt->hold = true;
   }
   s->release = false;
   SDL_UnlockMutex(s->lock);
}

static void
cs_release(cs_server *s)
{
   SDL_LockMutex(s->lock);
   for (int i = 0; i < s->route_count; i++) {
      s->routes[i].hold = false;
   }
   s->release = true;
   SDL_UnlockMutex(s->lock);
}

static int
cs_hits(cs_server *s, const char *nsid)
{
   SDL_LockMutex(s->lock);
   cs_route *rt = cs_find_locked(s, nsid);
   const int hits = rt ? rt->hits : 0;
   SDL_UnlockMutex(s->lock);
   return hits;
}

/* Wait until an NSID has been requested `n` times, or give up after two seconds. */
static bool
cs_wait_hits(cs_server *s, const char *nsid, int n)
{
   for (int i = 0; i < 400; i++) {
      if (cs_hits(s, nsid) >= n) {
         return true;
      }
      SDL_Delay(5);
   }
   return false;
}

static void
cs_handle(cs_server *s, int fd)
{
   char buf[8192];
   size_t len = 0;
   for (;;) {
      const ssize_t got = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
      if (got <= 0) {
         break;
      }
      len += (size_t) got;
      buf[len] = '\0';
      const char *end = strstr(buf, "\r\n\r\n");
      if (!end) {
         continue;
      }
      const size_t head = (size_t) (end - buf) + 4;
      const char *cl = strstr(buf, "Content-Length:");
      const size_t want = cl ? strtoul(cl + 15, NULL, 10) : 0;
      if (len >= head + want) {
         break;
      }
   }
   buf[len] = '\0';

   char method[8], path[256];
   if (sscanf(buf, "%7s %255s", method, path) != 2) {
      return;
   }
   char nsid[80] = "";
   const char *prefix = "/xrpc/";
   if (strncmp(path, prefix, strlen(prefix)) == 0) {
      const size_t n = strcspn(path + strlen(prefix), "?");
      if (n < sizeof(nsid)) {
         memcpy(nsid, path + strlen(prefix), n);
         nsid[n] = '\0';
      }
   }

   int status = 400;
   char body[1024];
   snprintf(body, sizeof(body), "%s", "{\"error\":\"InvalidRequest\",\"message\":\"unknown NSID\"}");
   bool hold = false;
   SDL_LockMutex(s->lock);
   cs_route *rt = cs_find_locked(s, nsid);
   if (rt) {
      rt->hits++;
      status = rt->status;
      snprintf(body, sizeof(body), "%s", rt->body);
      hold = rt->hold;
   }
   SDL_UnlockMutex(s->lock);

   for (;;) {
      if (!hold) {
         break;
      }
      SDL_LockMutex(s->lock);
      const bool go = s->release || s->stop;
      SDL_UnlockMutex(s->lock);
      if (go) {
         break;
      }
      SDL_Delay(5);
   }

   char head[256];
   const int n = snprintf(head, sizeof(head),
      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
      "Connection: close\r\n\r\n", status, status == 200 ? "OK" : "Error", strlen(body));
   send(fd, head, (size_t) n, 0);
   send(fd, body, strlen(body), 0);
}

static int
cs_thread(void *arg)
{
   cs_server *s = arg;
   for (;;) {
      SDL_LockMutex(s->lock);
      const bool stop = s->stop;
      SDL_UnlockMutex(s->lock);
      if (stop) {
         break;
      }
      fd_set rf;
      FD_ZERO(&rf);
      FD_SET(s->fd, &rf);
      struct timeval tv = { 0, 50000 };
      if (select(s->fd + 1, &rf, NULL, NULL, &tv) <= 0) {
         continue;
      }
      const int c = accept(s->fd, NULL, NULL);
      if (c < 0) {
         continue;
      }
      cs_handle(s, c);
      close(c);
   }
   return 0;
}

static bool
cs_start(cs_server *s)
{
   memset(s, 0, sizeof(*s));
   s->fd = socket(AF_INET, SOCK_STREAM, 0);
   if (s->fd < 0) {
      return false;
   }
   int one = 1;
   setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in addr;
   socklen_t len = sizeof(addr);
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   if (bind(s->fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(s->fd, 8) != 0 ||
       getsockname(s->fd, (struct sockaddr *) &addr, &len) != 0) {
      close(s->fd);
      return false;
   }
   s->port = ntohs(addr.sin_port);
   s->lock = SDL_CreateMutex();
   s->thread = SDL_CreateThread(cs_thread, "cs", s);
   return s->lock && s->thread;
}

static void
cs_stop(cs_server *s)
{
   SDL_LockMutex(s->lock);
   s->stop = true;
   SDL_UnlockMutex(s->lock);
   if (s->thread) {
      SDL_WaitThread(s->thread, NULL);
   }
   close(s->fd);
   SDL_DestroyMutex(s->lock);
}

/* A convo view with the viewer and one other member, for the chat list routes. */
static const char *
convo_json(const char *id, const char *name, int unread)
{
   static char buf[4][512];
   static int k;
   char *b = buf[k++ & 3];
   snprintf(b, 512,
      "{\"id\":\"%s\",\"rev\":\"1\",\"unreadCount\":%d,\"members\":["
      "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\"},"
      "{\"did\":\"did:plc:%s\",\"handle\":\"%s.test\",\"displayName\":\"%s\"}]}",
      id, unread, name, name, name);
   return b;
}

/* A request list with one convo, for the request route. */
static const char *
convo_list_one(const char *id, const char *name)
{
   static char buf[640];
   snprintf(buf, sizeof buf, "{\"requests\":[%s]}", convo_json(id, name, 0));
   return buf;
}

/* Accepting or declining a request: one job at a time, and only on a button. */
static cobalt_input
press(cobalt_button btn)
{
   cobalt_input in;
   memset(&in, 0, sizeof(in));
   in.pressed[btn] = true;
   in.held[btn] = true;
   return in;
}

static void
test_message_requests(void)
{
   cs_server s;
   CHECK(cs_start(&s));
   char svc[64];
   snprintf(svc, sizeof svc, "http://127.0.0.1:%d", s.port);

   cs_set(&s, "com.atproto.server.createSession", 200,
      "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\","
      "\"accessJwt\":\"a.b.c\",\"refreshJwt\":\"d.e.f\",\"active\":true}");
   cs_set(&s, "chat.bsky.convo.listConvos", 200, "{\"convos\":[]}");
   cs_set(&s, "chat.bsky.convo.acceptConvo", 200, "{\"rev\":\"r1\"}");
   cs_set(&s, "chat.bsky.convo.leaveConvo", 200, "{\"convoId\":\"c2\",\"rev\":\"r2\"}");

   char two[1100];
   snprintf(two, sizeof two, "{\"requests\":[%s,%s]}", convo_json("c1", "alice", 2),
            convo_json("c2", "bob", 0));
   cs_set(&s, "chat.bsky.convo.listConvoRequests", 200, two);

   cobalt_job_result r;
   CHECK(cobalt_session_begin_login(svc, "ewancroft.test", "app-pass"));
   CHECK(wait_job(&r) && r.ok);

   /* One accepted conversation, so an accepted request has an inbox to join. */
   cs_set(&s, "chat.bsky.convo.listConvos", 200,
          "{\"convos\":[" "{\"id\":\"c9\",\"rev\":\"1\",\"unreadCount\":0,\"members\":["
          "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\"},"
          "{\"did\":\"did:plc:carol\",\"handle\":\"carol.test\",\"displayName\":\"carol\"}]}]}");
   CHECK(cobalt_session_begin_chat_inbox(false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_inbox()->count == 1);

   cobalt_messages_view view;
   cobalt_messages_view_init(&view);
   cobalt_input none;
   memset(&none, 0, sizeof(none));

   /* Y opens the request list. Entering it fetches the list and nothing else:
    * no accept and no decline, however many frames pass. */
   cobalt_input y = press(COBALT_BTN_ALT_Y);
   CHECK(cobalt_messages_view_update(&view, &y) == COBALT_MESSAGES_STAY);
   CHECK(view.mode == COBALT_MESSAGES_MODE_REQUESTS);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_requests()->count == 2);
   for (int i = 0; i < 5; i++) {
      cobalt_messages_view_update(&view, &none);
   }
   CHECK(cs_hits(&s, "chat.bsky.convo.listConvoRequests") == 1);
   CHECK(cs_hits(&s, "chat.bsky.convo.acceptConvo") == 0);
   CHECK(cs_hits(&s, "chat.bsky.convo.leaveConvo") == 0);

   /* Leaving and re-entering with the list already loaded sends nothing but a
    * fetch: the first entry above saw an empty list. */
   cobalt_input back0 = press(COBALT_BTN_BACK);
   CHECK(cobalt_messages_view_update(&view, &back0) == COBALT_MESSAGES_STAY);
   CHECK(view.mode == COBALT_MESSAGES_MODE_INBOX);
   CHECK(cobalt_messages_view_update(&view, &y) == COBALT_MESSAGES_STAY);
   CHECK(view.mode == COBALT_MESSAGES_MODE_REQUESTS);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_requests()->count == 2);
   for (int i = 0; i < 5; i++) {
      cobalt_messages_view_update(&view, &none);
   }
   CHECK(cs_hits(&s, "chat.bsky.convo.listConvoRequests") == 2);
   CHECK(cs_hits(&s, "chat.bsky.convo.acceptConvo") == 0);
   CHECK(cs_hits(&s, "chat.bsky.convo.leaveConvo") == 0);

   /* A accepts the first request. The accept is held in flight: a second A is
    * inert, and no other accept or decline can start. */
   cs_hold(&s, "chat.bsky.convo.acceptConvo");
   cobalt_input a = press(COBALT_BTN_CONFIRM);
   CHECK(cobalt_messages_view_update(&view, &a) == COBALT_MESSAGES_ACCEPT);
   CHECK_STR(view.decision.convo_id, "c1");
   CHECK(view.decision.accept);
   const bool began = cobalt_session_begin_chat_accept(view.decision.convo_id);
   cobalt_messages_decision_began(&view, began);
   CHECK(began);
   CHECK(view.decision.pending);
   CHECK(cobalt_messages_view_update(&view, &a) == COBALT_MESSAGES_STAY);
   CHECK(!cobalt_session_begin_chat_accept("c2"));
   CHECK(!cobalt_session_begin_chat_decline("c2"));
   CHECK(cs_wait_hits(&s, "chat.bsky.convo.acceptConvo", 1));
   cs_release(&s);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_messages_decision_finished(&view, r.ok) == COBALT_MESSAGES_DECISION_ACCEPTED);
   CHECK(cs_hits(&s, "chat.bsky.convo.acceptConvo") == 1);
   CHECK(cs_hits(&s, "chat.bsky.convo.leaveConvo") == 0);

   /* The accepted row left the requests and joined the front of the inbox. */
   CHECK(cobalt_session_chat_requests()->count == 1);
   CHECK_STR(cobalt_session_chat_requests()->items[0].id, "c2");
   CHECK(cobalt_session_chat_inbox()->count == 2);
   CHECK_STR(cobalt_session_chat_inbox()->items[0].id, "c1");

   /* X declines the remaining request. That is one leave, no accept, and the
    * inbox is left alone. */
   cobalt_input x = press(COBALT_BTN_ALT_X);
   CHECK(cobalt_messages_view_update(&view, &x) == COBALT_MESSAGES_DECLINE);
   CHECK_STR(view.decision.convo_id, "c2");
   CHECK(!view.decision.accept);
   const bool declined = cobalt_session_begin_chat_decline(view.decision.convo_id);
   cobalt_messages_decision_began(&view, declined);
   CHECK(declined);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_messages_decision_finished(&view, r.ok) == COBALT_MESSAGES_DECISION_DECLINED);
   CHECK(cs_hits(&s, "chat.bsky.convo.leaveConvo") == 1);
   CHECK(cs_hits(&s, "chat.bsky.convo.acceptConvo") == 1);
   CHECK(cobalt_session_chat_requests()->count == 0);
   CHECK(cobalt_session_chat_inbox()->count == 2);

   /* With no requests left, A has nothing to press. B returns to the inbox. */
   cobalt_input enter = press(COBALT_BTN_CONFIRM);
   CHECK(cobalt_messages_view_update(&view, &enter) == COBALT_MESSAGES_STAY);
   CHECK(!view.decision.pending);
   cobalt_input back = press(COBALT_BTN_BACK);
   CHECK(cobalt_messages_view_update(&view, &back) == COBALT_MESSAGES_STAY);
   CHECK(view.mode == COBALT_MESSAGES_MODE_INBOX);

   /* A failure keeps the request, says why, and is not retried by itself. */
   cs_set(&s, "chat.bsky.convo.listConvoRequests", 200,
          "{\"requests\":[" "{\"id\":\"c3\",\"rev\":\"1\",\"unreadCount\":0,\"members\":["
          "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\"},"
          "{\"did\":\"did:plc:dave\",\"handle\":\"dave.test\",\"displayName\":\"dave\"}]}]}");
   cs_set(&s, "chat.bsky.convo.acceptConvo", 500,
          "{\"error\":\"InternalServerError\",\"message\":\"try later\"}");
   cobalt_input y2 = press(COBALT_BTN_ALT_Y);
   CHECK(cobalt_messages_view_update(&view, &y2) == COBALT_MESSAGES_STAY);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_requests()->count == 1);

   cobalt_input a3 = press(COBALT_BTN_CONFIRM);
   CHECK(cobalt_messages_view_update(&view, &a3) == COBALT_MESSAGES_ACCEPT);
   const bool failing = cobalt_session_begin_chat_accept(view.decision.convo_id);
   cobalt_messages_decision_began(&view, failing);
   CHECK(failing);
   CHECK(wait_job(&r));
   CHECK(!r.ok);
   CHECK(r.message[0] != '\0');
   CHECK(cobalt_messages_decision_finished(&view, r.ok) == COBALT_MESSAGES_DECISION_FAILED);
   CHECK(cobalt_session_chat_requests()->count == 1);
   CHECK_STR(cobalt_session_chat_requests()->items[0].id, "c3");
   SDL_Delay(300);
   for (int i = 0; i < 3; i++) {
      cobalt_messages_view_update(&view, &none);
   }
   CHECK(cs_hits(&s, "chat.bsky.convo.acceptConvo") == 2);

   /* Pressed again on purpose, and the server now answers with an empty object,
    * which is a success. */
   cs_set(&s, "chat.bsky.convo.acceptConvo", 200, "{}");
   CHECK(cobalt_messages_view_update(&view, &a3) == COBALT_MESSAGES_ACCEPT);
   const bool retried = cobalt_session_begin_chat_accept(view.decision.convo_id);
   cobalt_messages_decision_began(&view, retried);
   CHECK(retried);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_messages_decision_finished(&view, r.ok) == COBALT_MESSAGES_DECISION_ACCEPTED);
   CHECK_STR(view.decision.name, "dave");
   CHECK(cs_hits(&s, "chat.bsky.convo.acceptConvo") == 3);
   CHECK(cobalt_session_chat_requests()->count == 0);
   CHECK(cobalt_session_chat_inbox()->count == 3);
   CHECK_STR(cobalt_session_chat_inbox()->items[0].id, "c3");

   /* With the inbox not loaded, an accepted request is not joined to it: the
    * inbox is fetched when Messages opens, and one row would hide the rest. */
   cs_set(&s, "chat.bsky.convo.listConvos", 200, "{\"convos\":[]}");
   CHECK(cobalt_session_begin_chat_inbox(false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_inbox()->count == 0);
   cs_set(&s, "chat.bsky.convo.listConvoRequests", 200,
          convo_list_one("c4", "erin"));
   CHECK(cobalt_session_begin_chat_requests(false));
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_begin_chat_accept("c4"));
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_requests()->count == 0);
   CHECK(cobalt_session_chat_inbox()->count == 0);

   cs_stop(&s);
}

/* A group convo view: the viewer and two others, so it is more than a pair and
 * so the inbox marks it a group. */
static const char *
group_convo_json(const char *id)
{
   static char buf[640];
   snprintf(buf, sizeof buf,
      "{\"id\":\"%s\",\"rev\":\"1\",\"unreadCount\":0,\"members\":["
      "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\"},"
      "{\"did\":\"did:plc:bob\",\"handle\":\"bob.test\",\"displayName\":\"bob\"},"
      "{\"did\":\"did:plc:carol\",\"handle\":\"carol.test\",\"displayName\":\"carol\"}]}",
      id);
   return buf;
}

/* A group's two pending join requests, for the listJoinRequests route. */
static const char *
join_requests_two(void)
{
   return "{\"requests\":["
      "{\"convoId\":\"g1\",\"requestedBy\":{\"did\":\"did:plc:bob\","
      "\"handle\":\"bob.test\",\"displayName\":\"bob\"},"
      "\"requestedAt\":\"2026-10-01T10:00:00.000Z\"},"
      "{\"convoId\":\"g1\",\"requestedBy\":{\"did\":\"did:plc:dave\","
      "\"handle\":\"dave.test\",\"displayName\":\"dave\"},"
      "\"requestedAt\":\"2026-10-01T10:00:00.000Z\"}]}";
}

/* A group's join requests: opened from the conversation, one decision per press,
 * and only a group offers it at all. */
static void
test_group_join_requests(void)
{
   cs_server s;
   CHECK(cs_start(&s));
   char svc[64];
   snprintf(svc, sizeof svc, "http://127.0.0.1:%d", s.port);

   cs_set(&s, "com.atproto.server.createSession", 200,
      "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\","
      "\"accessJwt\":\"a.b.c\",\"refreshJwt\":\"d.e.f\",\"active\":true}");
   char convos[700];
   snprintf(convos, sizeof convos, "{\"convos\":[%s]}", group_convo_json("g1"));
   cs_set(&s, "chat.bsky.convo.listConvos", 200, convos);
   cs_set(&s, "chat.bsky.group.listJoinRequests", 200, join_requests_two());

   char approved[720];
   snprintf(approved, sizeof approved, "{\"convo\":%s}", group_convo_json("g1"));
   cs_set(&s, "chat.bsky.group.approveJoinRequest", 200, approved);
   cs_set(&s, "chat.bsky.group.rejectJoinRequest", 200, "{}");

   cobalt_job_result r;
   CHECK(cobalt_session_begin_login(svc, "ewancroft.test", "app-pass"));
   CHECK(wait_job(&r) && r.ok);

   cobalt_chat_thread_view view;
   cobalt_chat_thread_view_init(&view);
   cobalt_input none;
   memset(&none, 0, sizeof none);

   /* X opens the join requests and fetches them once. Entering decides nothing,
    * however many frames pass. */
   cobalt_chat_thread_view_open(&view, "Group", "g1", true);
   CHECK(view.group);
   cobalt_input x = press(COBALT_BTN_ALT_X);
   CHECK(cobalt_chat_thread_view_update(&view, &x) == COBALT_CHAT_THREAD_STAY);
   CHECK(view.mode == COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_join_requests()->count == 2);
   CHECK_STR(cobalt_session_chat_join_requests()->items[0].did, "did:plc:bob");
   CHECK_STR(cobalt_session_chat_join_requests()->items[0].name, "bob");
   CHECK_STR(cobalt_session_chat_join_requests()->items[1].did, "did:plc:dave");
   for (int i = 0; i < 5; i++) {
      cobalt_chat_thread_view_update(&view, &none);
   }
   CHECK(cs_hits(&s, "chat.bsky.group.listJoinRequests") == 1);
   CHECK(cs_hits(&s, "chat.bsky.group.approveJoinRequest") == 0);
   CHECK(cs_hits(&s, "chat.bsky.group.rejectJoinRequest") == 0);

   /* A approves the highlighted request, which then leaves the list. */
   cobalt_input a = press(COBALT_BTN_CONFIRM);
   CHECK(cobalt_chat_thread_view_update(&view, &a) == COBALT_CHAT_THREAD_JOIN_APPROVE);
   CHECK_STR(view.join_decision.did, "did:plc:bob");
   CHECK(view.join_decision.approve);
   const bool began = cobalt_session_begin_chat_join_approve(view.convo_id,
                                                             view.join_decision.did);
   cobalt_chat_thread_decision_began(&view, began);
   CHECK(began);
   CHECK(view.join_decision.pending);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_chat_thread_decision_finished(&view, r.ok) == COBALT_CHAT_THREAD_DECISION_APPROVED);
   CHECK(cs_hits(&s, "chat.bsky.group.approveJoinRequest") == 1);
   CHECK(cobalt_session_chat_join_requests()->count == 1);
   CHECK_STR(cobalt_session_chat_join_requests()->items[0].did, "did:plc:dave");

   /* X rejects what is left: one reject, no further approve. */
   CHECK(cobalt_chat_thread_view_update(&view, &x) == COBALT_CHAT_THREAD_JOIN_REJECT);
   CHECK_STR(view.join_decision.did, "did:plc:dave");
   CHECK(!view.join_decision.approve);
   const bool rejected = cobalt_session_begin_chat_join_reject(view.convo_id,
                                                               view.join_decision.did);
   cobalt_chat_thread_decision_began(&view, rejected);
   CHECK(rejected);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_chat_thread_decision_finished(&view, r.ok) == COBALT_CHAT_THREAD_DECISION_REJECTED);
   CHECK(cs_hits(&s, "chat.bsky.group.rejectJoinRequest") == 1);
   CHECK(cs_hits(&s, "chat.bsky.group.approveJoinRequest") == 1);
   CHECK(cobalt_session_chat_join_requests()->count == 0);

   /* B leaves the sub-mode for the conversation, not out of it. */
   cobalt_input back = press(COBALT_BTN_BACK);
   CHECK(cobalt_chat_thread_view_update(&view, &back) == COBALT_CHAT_THREAD_STAY);
   CHECK(view.mode == COBALT_CHAT_THREAD_MODE_MESSAGES);

   /* A one-to-one conversation has no join requests, so its X does nothing. */
   cobalt_chat_thread_view_open(&view, "bob", "d1", false);
   CHECK(!view.group);
   CHECK(cobalt_chat_thread_view_update(&view, &x) == COBALT_CHAT_THREAD_STAY);
   CHECK(view.mode == COBALT_CHAT_THREAD_MODE_MESSAGES);
   CHECK(cs_hits(&s, "chat.bsky.group.listJoinRequests") == 1);

   /* A failed approve keeps the request, says why, and is not retried by
    * itself. */
   cs_set(&s, "chat.bsky.group.listJoinRequests", 200, join_requests_two());
   cs_set(&s, "chat.bsky.group.approveJoinRequest", 500,
          "{\"error\":\"InternalServerError\",\"message\":\"try later\"}");
   cobalt_chat_thread_view_open(&view, "Group", "g1", true);
   CHECK(cobalt_chat_thread_view_update(&view, &x) == COBALT_CHAT_THREAD_STAY);
   CHECK(wait_job(&r) && r.ok);
   CHECK(cobalt_session_chat_join_requests()->count == 2);
   CHECK(cobalt_chat_thread_view_update(&view, &a) == COBALT_CHAT_THREAD_JOIN_APPROVE);
   const bool failing = cobalt_session_begin_chat_join_approve(view.convo_id,
                                                               view.join_decision.did);
   cobalt_chat_thread_decision_began(&view, failing);
   CHECK(failing);
   CHECK(wait_job(&r));
   CHECK(!r.ok);
   CHECK(r.message[0] != '\0');
   CHECK(cobalt_chat_thread_decision_finished(&view, r.ok) == COBALT_CHAT_THREAD_DECISION_FAILED);
   CHECK(cobalt_session_chat_join_requests()->count == 2);
   SDL_Delay(300);
   for (int i = 0; i < 3; i++) {
      cobalt_chat_thread_view_update(&view, &none);
   }
   CHECK(cs_hits(&s, "chat.bsky.group.approveJoinRequest") == 2);

   cs_stop(&s);
}

int
main(int argc, char **argv)
{
   (void) argc; (void) argv;
   test_muted_words();
   /* Init reports a blocker on the host (no CA file, no console entropy, no
    * mbedTLS RNG hook). Those gate HTTPS on the console and are irrelevant to
    * plain HTTP against localhost; the worker runs regardless. */
   (void) cobalt_session_init();
   printf("session blocker: %s\n", cobalt_session_blocker() ? cobalt_session_blocker() : "none");

   wf_mock_pds *pds = NULL;
   int port = 0;
   CHECK(wf_mock_pds_start(&pds, &port) == WF_OK);

   wf_mock_pds_register(pds, "com.atproto.server.createSession",
      "{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\","
      "\"accessJwt\":\"a.b.c\",\"refreshJwt\":\"d.e.f\",\"active\":true}");

   char tl[2048];
   snprintf(tl, sizeof tl, "{\"feed\":[{\"post\":%s},{\"post\":%s}],\"cursor\":\"c1\"}",
            post_json("1", "hello timeline"), post_json("2", "second"));
   wf_mock_pds_register(pds, "app.bsky.feed.getTimeline", tl);

   char sr[1024];
   snprintf(sr, sizeof sr, "{\"posts\":[{\"uri\":\"at://did:plc:abc/app.bsky.feed.post/3\","
      "\"cid\":\"bafy3\",\"author\":{\"did\":\"did:plc:abc\",\"handle\":\"ewancroft.uk\"},"
      "\"record\":{\"text\":\"found by search\",\"createdAt\":\"2026-10-01T10:00:00.000Z\","
      "\"facets\":[{\"index\":{\"byteStart\":9,\"byteEnd\":15},\"features\":"
      "[{\"$type\":\"app.bsky.richtext.facet#link\",\"uri\":\"https://x.io\"}]},"
      "{\"index\":{\"byteStart\":3,\"byteEnd\":99},\"features\":"
      "[{\"$type\":\"app.bsky.richtext.facet#tag\",\"tag\":\"t\"}]},"
      "{\"index\":{\"byteStart\":40,\"byteEnd\":50},\"features\":"
      "[{\"$type\":\"app.bsky.richtext.facet#mention\",\"did\":\"did:plc:b\"}]}]},"
      "\"likeCount\":0,\"repostCount\":0,\"replyCount\":0,"
      "\"indexedAt\":\"2026-10-01T10:00:00.000Z\"}],\"cursor\":\"s1\"}");
   wf_mock_pds_register(pds, "app.bsky.feed.searchPosts", sr);

   char fd[1024];
   snprintf(fd, sizeof fd, "{\"feed\":[{\"post\":%s}]}",
            post_json("4", "from a custom feed"));
   wf_mock_pds_register(pds, "app.bsky.feed.getFeed", fd);

   char svc[64];
   snprintf(svc, sizeof svc, "http://127.0.0.1:%d", port);

   cobalt_job_result r;

   /* A PDS with no createSession answers with an error envelope; its message
    * should reach the sign-in screen. */
   {
      wf_mock_pds *bad = NULL;
      int bad_port = 0;
      char bad_svc[64];
      CHECK(wf_mock_pds_start(&bad, &bad_port) == WF_OK);
      snprintf(bad_svc, sizeof bad_svc, "http://127.0.0.1:%d", bad_port);
      CHECK(cobalt_session_begin_login(bad_svc, "ewancroft.test", "nope"));
      CHECK(wait_job(&r));
      CHECK(!r.ok);
      printf("failed login message: %s\n", r.message);
      CHECK(strstr(r.message, "Server said:") != NULL);
      wf_mock_pds_stop(bad);
      wf_mock_pds_free(bad);
   }

   /* OAuth through a node, driven by Wolfram's pairing client. A node that
    * reports a terminal error ends the attempt with its message; one that
    * completes yields a signed-in session for the handle it names. */
   {
      wf_mock_pds *node = NULL;
      int node_port = 0;
      char node_url[64];
      CHECK(wf_mock_pds_start(&node, &node_port) == WF_OK);
      snprintf(node_url, sizeof node_url, "http://127.0.0.1:%d", node_port);
      wf_mock_pds_register(node, "uk.ewancroft.oauth.begin",
         "{\"pair_code\":\"abc123\",\"pair_url\":\"https://auth.example.com/pair/abc123\"}");
      wf_mock_pds_register(node, "uk.ewancroft.oauth.poll",
         "{\"status\":\"error\",\"message\":\"This pairing request expired.\"}");
      CHECK(cobalt_session_begin_oauth(node_url, "ewancroft.uk"));
      CHECK(wait_job(&r));
      CHECK(!r.ok);
      CHECK(strstr(r.message, "expired") != NULL);
      CHECK(cobalt_session_pair_url()[0] == '\0');

      char done[512];
      snprintf(done, sizeof done,
         "{\"status\":\"complete\",\"token\":\"node-bearer\",\"handle\":\"ewancroft.uk\","
         "\"did\":\"did:plc:abcdefghijklmnopqrstuvwx\",\"service\":\"%s\"}", node_url);
      wf_mock_pds_register(node, "uk.ewancroft.oauth.poll", done);
      CHECK(cobalt_session_begin_oauth(node_url, "ewancroft.uk"));
      CHECK(wait_job(&r));
      if (!r.ok) printf("oauth failed: %s\n", r.message);
      CHECK(r.ok);
      CHECK(cobalt_session_state() == COBALT_AUTH_SIGNED_IN);
      CHECK(strcmp(cobalt_session_handle(), "ewancroft.uk") == 0);
      CHECK(cobalt_session_pair_code()[0] == '\0');
      wf_mock_pds_stop(node);
      wf_mock_pds_free(node);
   }

   CHECK(cobalt_session_begin_login(svc, "ewancroft.test", "app-pass"));
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
   /* Facets: link kept, end clipped to the text, out-of-range start dropped. */
   CHECK(cobalt_session_feed()->posts[0].facet_count == 2);
   CHECK(cobalt_session_feed()->posts[0].facets[0].kind == COBALT_FACET_LINK);
   CHECK(cobalt_session_feed()->posts[0].facets[1].kind == COBALT_FACET_TAG);
   CHECK(cobalt_session_feed()->posts[0].facets[1].end == 15);
   CHECK(strcmp(cobalt_session_feed()->posts[0].facets[0].target, "https://x.io") == 0);
   CHECK(strcmp(cobalt_session_feed()->posts[0].facets[1].target, "t") == 0);
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

   test_message_requests();
   test_group_join_requests();

   wf_mock_pds_free(pds);
   printf("%d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
