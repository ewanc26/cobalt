#include "net/http.h"
#include "util/log.h"
#include "util/rng.h"

#include <curl/curl.h>

/* Same condition Wolfram uses: the TLS RNG hook only makes sense where libcurl
 * is genuinely mbedTLS-backed, which on this project means the console. */
#if !defined(COBALT_CURL_MBEDTLS) && defined(__WIIU__)
#define COBALT_CURL_MBEDTLS 1
#endif

#if defined(COBALT_CURL_MBEDTLS)
#include <mbedtls/ssl.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Give up rather than hang a screen waiting on an image nobody will see. */
#define CONNECT_TIMEOUT_SECONDS 10
#define TOTAL_TIMEOUT_SECONDS   30

static struct {
   bool initialised;
   bool curl_global;
   char ca_path[512];
   bool have_ca;
} s;

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

   s.initialised = true;
   return true;
}

void
cobalt_http_shutdown(void)
{
   if (s.curl_global) {
      curl_global_cleanup();
      s.curl_global = false;
   }
   s.initialised = false;
}

/* --- requests --- */

bool
cobalt_http_get(const char *url, size_t max_bytes, cobalt_http_response *out)
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

void
cobalt_http_response_free(cobalt_http_response *response)
{
   if (response) {
      free(response->data);
      response->data = NULL;
      response->size = 0;
   }
}
