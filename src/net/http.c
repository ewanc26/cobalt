#include "net/http.h"
#include "util/log.h"
#include "util/rng.h"

#include <SDL.h>
#include <curl/curl.h>

/* Same condition Wolfram uses: the TLS RNG hook only makes sense where libcurl
 * is genuinely mbedTLS-backed, which on this project means the console. */
#if !defined(COBALT_CURL_MBEDTLS) && defined(__WIIU__)
#define COBALT_CURL_MBEDTLS 1
#endif

#if defined(COBALT_CURL_MBEDTLS)
#include <mbedtls/ssl.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Give up rather than hang a screen waiting on an image nobody will see. */
#define CONNECT_TIMEOUT_SECONDS 10
#define TOTAL_TIMEOUT_SECONDS   30

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
   bool curl_global;
   char ca_path[512];
   bool have_ca;

   /* One connection / TLS-session / DNS cache for every handle on every thread.
    * A fresh easy handle per request meant a full TLS handshake plus a re-parse
    * of the CA bundle each time, which is seconds of CPU on this console. */
   CURLSH *share;
   SDL_mutex *share_locks[CURL_LOCK_DATA_LAST];

   SDL_mutex *body_lock;
   SDL_cond *body_cond;
   body_slot bodies[BODY_CACHE_SLOTS];
   size_t body_bytes;
   uint32_t body_clock;
} s;

static void
share_lock(CURL *handle, curl_lock_data data, curl_lock_access access, void *user)
{
   (void) handle; (void) access; (void) user;
   if (data < CURL_LOCK_DATA_LAST && s.share_locks[data]) {
      SDL_LockMutex(s.share_locks[data]);
   }
}

static void
share_unlock(CURL *handle, curl_lock_data data, void *user)
{
   (void) handle; (void) user;
   if (data < CURL_LOCK_DATA_LAST && s.share_locks[data]) {
      SDL_UnlockMutex(s.share_locks[data]);
   }
}

/* --- write callback --- */

typedef struct {
   unsigned char *data;
   size_t size;
   size_t capacity;
   size_t limit;
   bool overflowed;
} buffer;

static size_t
on_data(char *chunk, size_t size, size_t count, void *userdata)
{
   buffer *buf = (buffer *) userdata;
   const size_t incoming = size * count;

   if (buf->size + incoming > buf->limit) {
      /* Returning short aborts the transfer, which is the point: the cap has
       * to stop the download, not just refuse the result afterwards. */
      buf->overflowed = true;
      return 0;
   }

   if (buf->size + incoming + 1 > buf->capacity) {
      size_t wanted = buf->capacity ? buf->capacity * 2 : 16384;
      while (wanted < buf->size + incoming + 1) {
         wanted *= 2;
      }
      if (wanted > buf->limit + 1) {
         wanted = buf->limit + 1;
      }
      unsigned char *grown = (unsigned char *) realloc(buf->data, wanted);
      if (!grown) {
         return 0;
      }
      buf->data = grown;
      buf->capacity = wanted;
   }

   memcpy(buf->data + buf->size, chunk, incoming);
   buf->size += incoming;
   buf->data[buf->size] = '\0';
   return incoming;
}

/* --- TLS --- */

#if defined(COBALT_CURL_MBEDTLS)
static int
curl_uses_mbedtls(void)
{
   const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
   return info && info->ssl_version &&
          strncmp(info->ssl_version, "mbedTLS", 7) == 0;
}

/*
 * curl calls this after its own mbedtls_ssl_conf_rng() and before
 * mbedtls_ssl_setup(), so installing ours here covers the whole handshake —
 * the same mechanism Wolfram uses for its own requests.
 */
static CURLcode
tls_ctx_cb(CURL *curl, void *ssl_ctx, void *userdata)
{
   (void) curl;
   (void) userdata;
   if (ssl_ctx) {
      mbedtls_ssl_conf_rng((mbedtls_ssl_config *) ssl_ctx, cobalt_rng_mbedtls,
                           NULL);
   }
   return CURLE_OK;
}
#endif

/* --- lifecycle --- */

bool
cobalt_http_init(const char *ca_path)
{
   if (!s.curl_global) {
      /*
       * Explicit rather than relying on curl_easy_init's implicit
       * initialisation, which is not thread-safe — and this module is called
       * from a worker while the session worker may be mid-request.
       */
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
         COBALT_LOGE("http: curl_global_init failed");
         return false;
      }
      s.curl_global = true;
   }

   s.have_ca = false;
   s.ca_path[0] = '\0';
   if (ca_path && ca_path[0]) {
      snprintf(s.ca_path, sizeof(s.ca_path), "%s", ca_path);
      s.have_ca = true;
   } else {
      COBALT_LOGW("http: no CA bundle — image loads will fail verification");
   }

   if (!s.share) {
      for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) {
         s.share_locks[i] = SDL_CreateMutex();
      }
      s.share = curl_share_init();
      if (s.share) {
         curl_share_setopt(s.share, CURLSHOPT_LOCKFUNC, share_lock);
         curl_share_setopt(s.share, CURLSHOPT_UNLOCKFUNC, share_unlock);
         curl_share_setopt(s.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
         curl_share_setopt(s.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
         curl_share_setopt(s.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
      }
   }
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
   if (s.share) {
      curl_share_cleanup(s.share);
      s.share = NULL;
   }
   for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) {
      if (s.share_locks[i]) {
         SDL_DestroyMutex(s.share_locks[i]);
         s.share_locks[i] = NULL;
      }
   }
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
   if (s.curl_global) {
      curl_global_cleanup();
      s.curl_global = false;
   }
   s.initialised = false;
}

/* --- requests --- */

static bool
fetch_network(const char *url, size_t max_bytes, cobalt_http_response *out)
{
   if (!out) {
      return false;
   }
   memset(out, 0, sizeof(*out));

   if (!s.initialised || !url || !url[0] || max_bytes == 0) {
      return false;
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      return false;
   }

   buffer buf;
   memset(&buf, 0, sizeof(buf));
   buf.limit = max_bytes;

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "cobalt (Wii U)");
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long) CONNECT_TIMEOUT_SECONDS);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) TOTAL_TIMEOUT_SECONDS);
   /* Curl's signal-based timeouts are not safe off the main thread. */
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   if (s.share) {
      curl_easy_setopt(curl, CURLOPT_SHARE, s.share);
   }

   /*
    * Only https. A PDS could hand back an http:// URL — by mistake or not —
    * and silently fetching it would leak which posts are being read to anyone
    * on the path.
    */
   curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
   curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");

   if (s.have_ca) {
      curl_easy_setopt(curl, CURLOPT_CAINFO, s.ca_path);
   }

#if defined(COBALT_CURL_MBEDTLS)
   if (curl_uses_mbedtls()) {
      curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, tls_ctx_cb);
   }
#endif

   const CURLcode rc = curl_easy_perform(curl);
   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   curl_easy_cleanup(curl);

   if (rc != CURLE_OK) {
      if (buf.overflowed) {
         COBALT_LOGW("http: %s exceeded %u bytes, aborted", url,
                     (unsigned) max_bytes);
      } else {
         COBALT_LOGW("http: %s failed (%s)", url, curl_easy_strerror(rc));
      }
      free(buf.data);
      return false;
   }

   if (status < 200 || status >= 300 || buf.size == 0) {
      COBALT_LOGW("http: %s returned %ld (%u bytes)", url, status,
                  (unsigned) buf.size);
      free(buf.data);
      return false;
   }

   out->data = buf.data;
   out->size = buf.size;
   out->status = status;
   return true;
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
