#include "net/http.h"
#include "util/log.h"
#include "util/rng.h"

#include <SDL.h>

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/failure.h>
#include <wolfram/xrpc.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Give up rather than hang a screen waiting on an image nobody will see. */
#define TOTAL_TIMEOUT_MS      30000
#define MAX_REDIRECTS         5
#define USER_AGENT            "cobalt (Wii U)"

/* Wolfram wants a base URL for every client; a generic GET never uses it. */
#define PLACEHOLDER_BASE_URL  "https://cobalt.invalid"

/* Downloaded bodies kept for a short while. The TV and GamePad each own an image
 * cache and ask for the same avatar within a frame of each other; this makes
 * that one download, not two. */
#define BODY_CACHE_SLOTS      32
#define BODY_CACHE_MAX_BYTES  (4u * 1024u * 1024u)
#define BODY_CACHE_MAX_ENTRY  (512u * 1024u)
#define BODY_URL_MAX          512

typedef enum { BODY_FREE, BODY_FETCHING, BODY_READY } body_state;

typedef struct {
   body_state state;
   char url[BODY_URL_MAX];
   unsigned char *data;
   size_t size;
   uint32_t stamp;
} body_slot;

static struct {
   bool initialised;
   cobalt_http_state state;
   char reason[128];

#ifdef COBALT_HAS_WOLFRAM
   /*
    * A client of our own, never the signed-in agent's: it carries no bearer
    * token, so nothing here can send the account's credentials to whatever host
    * a PDS-supplied URL points at. Wolfram snapshots its settings per request
    * and keeps no shared handle, so the image workers (and the session worker)
    * may use it concurrently without a lock of ours.
    */
   wf_xrpc_client *client;
#endif

   SDL_mutex *body_lock;
   SDL_cond *body_cond;
   body_slot bodies[BODY_CACHE_SLOTS];
   size_t body_bytes;
   uint32_t body_clock;
} s;

/* --- lifecycle --- */

bool
cobalt_http_init(const char *ca_path)
{
   if (s.initialised) {
      return true;
   }

   s.state = COBALT_HTTP_STATE_UNINITIALISED;
   s.reason[0] = '\0';

#ifdef COBALT_HAS_WOLFRAM
   s.client = wf_xrpc_client_new(PLACEHOLDER_BASE_URL);
   if (!s.client) {
      COBALT_LOGE("http: could not create the fetch client");
      snprintf(s.reason, sizeof(s.reason), "could not create the network client");
      return false;
   }

   /* The policy lives in Wolfram; these are only this app's values for it. */
   wf_xrpc_client_set_https_only(s.client, 1);
   wf_xrpc_client_set_max_redirects(s.client, MAX_REDIRECTS);
   wf_xrpc_client_set_total_timeout_ms(s.client, TOTAL_TIMEOUT_MS);
   if (wf_xrpc_client_set_user_agent(s.client, USER_AGENT) != WF_OK) {
      COBALT_LOGW("http: could not set the user agent");
   }

   if (ca_path && ca_path[0]) {
      wf_xrpc_client_set_ca_bundle(s.client, ca_path);
   } else {
      /* Keep running (image loads still degrade to placeholders), but say so
       * loudly: verification will fail and the reason must be on the screen,
       * not mistaken for a network problem. */
      s.state = COBALT_HTTP_STATE_NO_CA;
      snprintf(s.reason, sizeof(s.reason),
               "no TLS trust store - requests fail verification (run `make cacert`)");
      COBALT_LOGW("http: no CA bundle - image loads will fail verification");
   }

   /* The same DRBG the session installs; see util/rng.h. */
   wf_status rng = wf_xrpc_client_set_tls_rng(s.client, cobalt_rng_mbedtls, NULL);
#ifdef COBALT_E2E_HOST
   /* Host end-to-end only: a host libcurl has no mbedTLS hook. Never defined in
    * a Wii U build. */
   rng = WF_OK;
#endif
   if (rng != WF_OK) {
      s.state = COBALT_HTTP_STATE_NO_TLS_RNG;
      snprintf(s.reason, sizeof(s.reason),
               "no TLS handshake RNG (%d) - is the entropy seed present?", (int) rng);
      COBALT_LOGE("http: could not install the TLS RNG (%d) — refusing to hand "
                  "the handshake to a tick-seeded generator", (int) rng);
      wf_xrpc_client_free(s.client);
      s.client = NULL;
      return false;
   }

   if (s.state == COBALT_HTTP_STATE_UNINITIALISED) {
      s.state = COBALT_HTTP_STATE_READY;
      snprintf(s.reason, sizeof(s.reason), "ready");
   }
#else
   (void) ca_path;
   s.state = COBALT_HTTP_STATE_NO_WOLFRAM;
   snprintf(s.reason, sizeof(s.reason),
            "built without Wolfram support - nothing can fetch");
#endif

   if (!s.body_lock) {
      s.body_lock = SDL_CreateMutex();
      s.body_cond = SDL_CreateCond();
   }

   s.initialised = true;
   return true;
}

void
cobalt_http_shutdown(void)
{
#ifdef COBALT_HAS_WOLFRAM
   if (s.client) {
      wf_xrpc_client_free(s.client);
      s.client = NULL;
   }
#endif
   if (s.body_lock) {
      for (int i = 0; i < BODY_CACHE_SLOTS; i++) {
         free(s.bodies[i].data);
      }
      memset(s.bodies, 0, sizeof(s.bodies));
      s.body_bytes = 0;
      SDL_DestroyCond(s.body_cond);
      SDL_DestroyMutex(s.body_lock);
      s.body_cond = NULL;
      s.body_lock = NULL;
   }
   s.initialised = false;
   s.state = COBALT_HTTP_STATE_UNINITIALISED;
   s.reason[0] = '\0';
}

cobalt_http_state
cobalt_http_get_state(void)
{
   return s.state;
}

const char *
cobalt_http_reason(void)
{
   if (s.reason[0]) {
      return s.reason;
   }
   switch (s.state) {
   case COBALT_HTTP_STATE_READY:
      return "ready";
   case COBALT_HTTP_STATE_NO_WOLFRAM:
      return "built without Wolfram support - nothing can fetch";
   case COBALT_HTTP_STATE_NO_CA:
      return "no TLS trust store - run `make cacert`";
   case COBALT_HTTP_STATE_NO_TLS_RNG:
      return "no TLS handshake RNG - is the entropy seed present?";
   case COBALT_HTTP_STATE_UNINITIALISED:
   default:
      return "not initialised";
   }
}

/* --- requests --- */

#ifdef COBALT_HAS_WOLFRAM
/*
 * Turn a failed Wolfram fetch into a reason a person can act on. The update
 * path and the diagnostics screen both show this; without it every failure
 * reads as "could not reach the network", whether it was TLS verification, no
 * entropy seed, DNS or a rate limit.
 */
static void
fill_error(cobalt_http_response *out, const wf_status st, long http)
{
   out->error = (int) st;
   switch (wf_failure_classify(st, http, NULL)) {
   case WF_FAIL_TLS:
      snprintf(out->error_text, sizeof(out->error_text),
               "TLS handshake failed - does this build bundle a trust store?");
      return;
   case WF_FAIL_NETWORK:
      snprintf(out->error_text, sizeof(out->error_text),
               "network error (DNS or connect)");
      return;
   case WF_FAIL_TIMEOUT:
      snprintf(out->error_text, sizeof(out->error_text), "the request timed out");
      return;
   case WF_FAIL_RATE_LIMIT:
      snprintf(out->error_text, sizeof(out->error_text),
               "rate limited - wait and retry");
      return;
   case WF_FAIL_BAD_RESPONSE:
      snprintf(out->error_text, sizeof(out->error_text),
               "the server sent an unreadable answer");
      return;
   case WF_FAIL_SERVER:
      if (http != 0) {
         snprintf(out->error_text, sizeof(out->error_text),
                  "server error (HTTP %ld)", http);
      } else {
         snprintf(out->error_text, sizeof(out->error_text),
                  "the service did not answer");
      }
      return;
   case WF_FAIL_NOT_READY:
      snprintf(out->error_text, sizeof(out->error_text),
               "this build cannot fetch");
      return;
   default:
      break;
   }
   if (http != 0) {
      snprintf(out->error_text, sizeof(out->error_text),
               "server returned HTTP %ld", http);
   } else {
      snprintf(out->error_text, sizeof(out->error_text),
               "request failed (wolfram status %d)", (int) st);
   }
}
#endif

static bool
fetch_network(const char *url, size_t max_bytes, cobalt_http_response *out)
{
   if (!out) {
      return false;
   }
   memset(out, 0, sizeof(*out));

   if (!s.initialised || !url || !url[0] || max_bytes == 0) {
      /* If init was attempted and refused, that reason is the real answer;
       * otherwise the client was never brought up at all. */
      if (!s.initialised) {
         out->error = -1;
         snprintf(out->error_text, sizeof(out->error_text), "%s",
                  cobalt_http_reason());
      }
      return false;
   }

#ifdef COBALT_HAS_WOLFRAM
   /*
    * Wolfram refuses anything but https (redirects included), follows at most
    * MAX_REDIRECTS, gives up after TOTAL_TIMEOUT_MS, and aborts the transfer
    * the moment the body crosses `max_bytes` — the URL comes from a PDS
    * response, so a hostile one could point at an endless stream.
    */
   wf_response res = {0};
   const wf_status st = wf_http_get_limited(s.client, url, max_bytes, &res);
   if (st != WF_OK) {
      COBALT_LOGW("http: %.48s failed (status %d, http %ld)", url, (int) st,
                  res.status);
      fill_error(out, st, res.status);
      wf_response_free(&res);
      return false;
   }
   if (res.body_len == 0) {
      COBALT_LOGW("http: %.48s returned %ld (0 bytes)", url, res.status);
      if (res.status != 0) {
         snprintf(out->error_text, sizeof(out->error_text),
                  "server returned HTTP %ld (empty body)", res.status);
      } else {
         snprintf(out->error_text, sizeof(out->error_text),
                  "server returned an empty body");
      }
      wf_response_free(&res);
      return false;
   }

   /* Wolfram's body is heap-owned and NUL-terminated, which is exactly the
    * contract of cobalt_http_response; take it over rather than copy. */
   out->data = (unsigned char *) res.body;
   out->size = res.body_len;
   out->status = res.status;
   res.body = NULL;
   wf_response_free(&res); /* the header captures Wolfram may have made */
   return true;
#else
   COBALT_LOGW("http: built without Wolfram, cannot fetch %.48s", url);
   out->error = -1;
   snprintf(out->error_text, sizeof(out->error_text), "%s", cobalt_http_reason());
   return false;
#endif
}

/* Caller holds body_lock. */
static body_slot *
body_find(const char *url)
{
   for (int i = 0; i < BODY_CACHE_SLOTS; i++) {
      if (s.bodies[i].state != BODY_FREE && strcmp(s.bodies[i].url, url) == 0) {
         return &s.bodies[i];
      }
   }
   return NULL;
}

/* Caller holds body_lock. Evicts the oldest READY entries until one slot is
 * free and `incoming` more bytes fit; returns NULL if everything is in flight. */
static body_slot *
body_claim(size_t incoming)
{
   for (;;) {
      body_slot *free_slot = NULL, *oldest = NULL;
      for (int i = 0; i < BODY_CACHE_SLOTS; i++) {
         body_slot *b = &s.bodies[i];
         if (b->state == BODY_FREE) {
            if (!free_slot) free_slot = b;
         } else if (b->state == BODY_READY && (!oldest || b->stamp < oldest->stamp)) {
            oldest = b;
         }
      }
      if (free_slot && s.body_bytes + incoming <= BODY_CACHE_MAX_BYTES) {
         return free_slot;
      }
      if (!oldest) {
         return free_slot;
      }
      s.body_bytes -= oldest->size;
      free(oldest->data);
      memset(oldest, 0, sizeof(*oldest));
   }
}

static unsigned char *
copy_body(const unsigned char *data, size_t size)
{
   unsigned char *copy = (unsigned char *) malloc(size + 1);
   if (copy) {
      memcpy(copy, data, size);
      copy[size] = '\0';
   }
   return copy;
}

bool
cobalt_http_get(const char *url, size_t max_bytes, cobalt_http_response *out)
{
   if (!out) {
      return false;
   }
   memset(out, 0, sizeof(*out));

   if (!s.initialised || !url || !url[0] || strlen(url) >= BODY_URL_MAX ||
       max_bytes == 0 || !s.body_lock) {
      return fetch_network(url, max_bytes, out);
   }

   SDL_LockMutex(s.body_lock);
   for (;;) {
      body_slot *hit = body_find(url);
      if (!hit) {
         break;
      }
      if (hit->state == BODY_READY && hit->size <= max_bytes) {
         unsigned char *copy = copy_body(hit->data, hit->size);
         if (copy) {
            hit->stamp = ++s.body_clock;
            out->data = copy;
            out->size = hit->size;
            out->status = 200;
            SDL_UnlockMutex(s.body_lock);
            return true;
         }
         break;
      }
      if (hit->state == BODY_FETCHING) {
         /* Someone else is downloading exactly this; wait for them. */
         SDL_CondWait(s.body_cond, s.body_lock);
         continue;
      }
      break;
   }

   body_slot *mine = body_claim(0);
   if (mine) {
      memset(mine, 0, sizeof(*mine));
      mine->state = BODY_FETCHING;
      snprintf(mine->url, sizeof(mine->url), "%s", url);
   }
   SDL_UnlockMutex(s.body_lock);

   const bool ok = fetch_network(url, max_bytes, out);

   if (mine) {
      SDL_LockMutex(s.body_lock);
      if (ok && out->size <= BODY_CACHE_MAX_ENTRY) {
         unsigned char *keep = copy_body(out->data, out->size);
         if (keep) {
            /* Make room by evicting READY entries; `mine` is FETCHING so it is
             * never a victim. */
            while (s.body_bytes + out->size > BODY_CACHE_MAX_BYTES) {
               body_slot *oldest = NULL;
               for (int i = 0; i < BODY_CACHE_SLOTS; i++) {
                  if (s.bodies[i].state == BODY_READY &&
                      (!oldest || s.bodies[i].stamp < oldest->stamp)) {
                     oldest = &s.bodies[i];
                  }
               }
               if (!oldest) break;
               s.body_bytes -= oldest->size;
               free(oldest->data);
               memset(oldest, 0, sizeof(*oldest));
            }
            mine->data = keep;
            mine->size = out->size;
            mine->stamp = ++s.body_clock;
            mine->state = BODY_READY;
            s.body_bytes += out->size;
         } else {
            memset(mine, 0, sizeof(*mine));
         }
      } else {
         memset(mine, 0, sizeof(*mine));
      }
      SDL_CondBroadcast(s.body_cond);
      SDL_UnlockMutex(s.body_lock);
   }
   return ok;
}

void
cobalt_http_response_free(cobalt_http_response *response)
{
   if (response) {
      free(response->data);
      response->data = NULL;
      response->size = 0;
   }
}
