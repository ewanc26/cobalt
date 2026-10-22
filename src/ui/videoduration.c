#include "ui/videoduration.h"

#include "util/threadprio.h"

#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef COBALT_HAS_WOLFRAM
#include <wolfram/embed_typed.h>
#endif

/*
 * A playlist for a short clip is a few kilobytes; a feature-length one with
 * ten-second segments is about 900 lines. The cap is generous for either and
 * stops a hostile URL streaming forever.
 */
#define PLAYLIST_MAX_BYTES (256 * 1024)

/* A screen of posts is a few videos. Past this a new playlist is not queued,
 * so its poster simply shows no duration. */
#define SLOT_COUNT 64

typedef enum {
   SLOT_FREE = 0,
   SLOT_QUEUED,   /* asked for, not yet fetched */
   SLOT_LOADING,  /* a loader is fetching it */
   SLOT_KNOWN,    /* text is valid */
   SLOT_UNKNOWN   /* fetched and given up on; never retried */
} slot_state;

typedef struct {
   slot_state state;
   char uri[COBALT_VIDEO_DURATION_URI_MAX];
   char text[COBALT_VIDEO_DURATION_TEXT_MAX];
} slot;

static slot s_slots[SLOT_COUNT];
static SDL_mutex *s_lock;
static SDL_cond *s_wake;
static SDL_Thread *s_thread;
static bool s_quit;
static cobalt_video_fetch_fn s_fetch;   /* NULL: the network */
static size_t s_fetches;

/* Without a thread (tests, or a failed init) there is nothing to contend with,
 * so the lock is simply absent. */
static void
lock(void)
{
   if (s_lock) {
      SDL_LockMutex(s_lock);
   }
}

static void
unlock(void)
{
   if (s_lock) {
      SDL_UnlockMutex(s_lock);
   }
}

/* Caller holds the lock. */
static slot *
find_slot(const char *uri)
{
   for (int i = 0; i < SLOT_COUNT; i++) {
      if (s_slots[i].state != SLOT_FREE && strcmp(s_slots[i].uri, uri) == 0) {
         return &s_slots[i];
      }
   }
   return NULL;
}

/* Caller holds the lock. */
static int
next_queued(void)
{
   for (int i = 0; i < SLOT_COUNT; i++) {
      if (s_slots[i].state == SLOT_QUEUED) {
         return i;
      }
   }
   return -1;
}

static bool
network_fetch(const char *url, cobalt_http_response *out)
{
   return cobalt_http_get(url, PLAYLIST_MAX_BYTES, out);
}

bool
cobalt_video_duration_parse(const char *body, size_t len, double *seconds)
{
#ifdef COBALT_HAS_WOLFRAM
   if (!body || len == 0 || !seconds) {
      return false;
   }
   double total = 0.0;
   if (wf_embed_video_playlist_duration(body, len, &total) != WF_OK) {
      return false;
   }
   if (!isfinite(total) || total <= 0.0) {
      return false;
   }
   *seconds = total;
   return true;
#else
   (void)body;
   (void)len;
   (void)seconds;
   return false;
#endif
}

bool
cobalt_video_duration_format(double seconds, char *out, size_t out_size)
{
   if (!out || out_size == 0) {
      return false;
   }
   out[0] = '\0';
   if (!isfinite(seconds) || seconds < 0.0 || seconds > 1e9) {
      return false;
   }

   const long total = (long) (seconds + 0.5);
   const long hours = total / 3600;
   const long minutes = (total % 3600) / 60;
   const long secs = total % 60;
   int n;
   if (hours > 0) {
      n = snprintf(out, out_size, "%ld:%02ld:%02ld", hours, minutes, secs);
   } else {
      n = snprintf(out, out_size, "%ld:%02ld", minutes, secs);
   }
   if (n <= 0 || (size_t) n >= out_size) {
      /* Too long: snprintf has already written a truncated prefix. */
      out[0] = '\0';
      return false;
   }
   return true;
}

/* --- master playlists --- */

/* 9 digits is 999999999, which fits a 32-bit long as PowerPC's is. */
#define BANDWIDTH_DIGITS_MAX 9

/* Appends `len` bytes of `s` to `out` at `*n`, NUL-terminated, or fails when
 * they do not fit. */
static bool
append(char *out, size_t out_size, size_t *n, const char *s, size_t len)
{
   if (len >= out_size || *n > out_size - len - 1) {
      return false;
   }
   memcpy(out + *n, s, len);
   *n += len;
   out[*n] = '\0';
   return true;
}

/* The BANDWIDTH value in an attribute list (the text after "#EXT-X-STREAM-INF:").
 * A quoted value may hold commas, so the list is walked attribute by attribute
 * rather than split on commas. */
static bool
stream_bandwidth(const char *p, size_t n, long *out)
{
   size_t i = 0;
   while (i < n) {
      while (i < n && (p[i] == ',' || p[i] == ' ')) {
         i++;
      }
      const size_t key = i;
      while (i < n && p[i] != '=' && p[i] != ',') {
         i++;
      }
      const size_t key_len = i - key;
      if (i >= n || p[i] != '=') {
         continue;   /* a bare word with no value */
      }
      i++;

      size_t value, value_end;
      if (i < n && p[i] == '"') {
         value = ++i;
         while (i < n && p[i] != '"') {
            i++;
         }
         value_end = i;
         if (i < n) {
            i++;
         }
      } else {
         value = i;
         while (i < n && p[i] != ',') {
            i++;
         }
         value_end = i;
      }

      if (key_len == 9 && memcmp(p + key, "BANDWIDTH", 9) == 0) {
         if (value_end == value) {
            return false;
         }
         long v = 0;
         size_t digits = 0;
         for (size_t k = value; k < value_end; k++) {
            if (p[k] < '0' || p[k] > '9') {
               return false;
            }
            digits++;
            if (digits <= BANDWIDTH_DIGITS_MAX) {
               v = v * 10 + (p[k] - '0');
            }
         }
         if (digits > BANDWIDTH_DIGITS_MAX) {
            v = 999999999L;
         }
         *out = v;
         return true;
      }
   }
   return false;
}

/* Resolves a variant's URI reference against the playlist's own URL. */
static bool
resolve_uri(const char *base, const char *ref, size_t ref_len,
            char *out, size_t out_size)
{
   size_t n = 0;
   out[0] = '\0';

   if ((ref_len >= 8 && memcmp(ref, "https://", 8) == 0) ||
       (ref_len >= 7 && memcmp(ref, "http://", 7) == 0)) {
      return append(out, out_size, &n, ref, ref_len);
   }

   const char *scheme_sep = strstr(base, "://");
   const size_t after_scheme = scheme_sep ? (size_t) (scheme_sep - base) + 3 : 0;
   /* The base up to any query or fragment. */
   const size_t end = strcspn(base, "?#");

   if (ref_len >= 2 && ref[0] == '/' && ref[1] == '/') {
      /* Scheme-relative: keep the base's scheme, including its colon. */
      if (!scheme_sep) {
         return false;
      }
      return append(out, out_size, &n, base, (size_t) (scheme_sep - base) + 1) &&
             append(out, out_size, &n, ref, ref_len);
   }

   if (ref[0] == '/') {
      /* Root-relative: keep the base's scheme and host. */
      if (!scheme_sep) {
         return false;
      }
      size_t origin = end;
      if (end > after_scheme) {
         const char *slash = memchr(base + after_scheme, '/', end - after_scheme);
         if (slash) {
            origin = (size_t) (slash - base);
         }
      }
      return append(out, out_size, &n, base, origin) &&
             append(out, out_size, &n, ref, ref_len);
   }

   /* Relative: keep the base's directory, which ends at its last slash in the
    * path. A base with no path at all gets one. */
   size_t dir_end = 0;
   for (size_t k = after_scheme; k < end; k++) {
      if (base[k] == '/') {
         dir_end = k + 1;
      }
   }
   if (dir_end) {
      if (!append(out, out_size, &n, base, dir_end)) {
         return false;
      }
   } else {
      if (!append(out, out_size, &n, base, end) ||
          !append(out, out_size, &n, "/", 1)) {
         return false;
      }
   }
   return append(out, out_size, &n, ref, ref_len);
}

bool
cobalt_video_master_variant(const char *body, size_t len, const char *base_uri,
                            char *out, size_t out_size)
{
   static const char tag[] = "#EXT-X-STREAM-INF:";
   const size_t tag_len = sizeof(tag) - 1;

   if (!body || !base_uri || !out || out_size == 0) {
      return false;
   }
   out[0] = '\0';

   /* A variant with a BANDWIDTH beats one without; among those with one the
    * lowest wins; among those without, the first. */
   bool pending = false;
   bool pending_has_bw = false;
   long pending_bw = 0;
   bool have = false;
   bool best_has_bw = false;
   long best_bw = 0;
   size_t best_start = 0;
   size_t best_len = 0;

   size_t pos = 0;
   while (pos < len) {
      size_t end = pos;
      while (end < len && body[end] != '\n') {
         end++;
      }
      size_t a = pos;
      size_t b = end;
      pos = end + 1;
      while (b > a && (body[b - 1] == '\r' || body[b - 1] == ' ' || body[b - 1] == '\t')) {
         b--;
      }
      while (a < b && (body[a] == ' ' || body[a] == '\t')) {
         a++;
      }
      if (a == b) {
         continue;
      }

      if (b - a >= tag_len && memcmp(body + a, tag, tag_len) == 0) {
         pending = true;
         pending_bw = 0;
         pending_has_bw = stream_bandwidth(body + a + tag_len,
                                           b - a - tag_len, &pending_bw);
         continue;
      }
      if (body[a] == '#' || !pending) {
         continue;   /* another tag, a comment, or a URI with no entry before it */
      }

      /* The URI line that follows a STREAM-INF entry is that variant's URI. */
      pending = false;
      const bool take = !have ||
                        (pending_has_bw && (!best_has_bw || pending_bw < best_bw));
      if (take) {
         have = true;
         best_has_bw = pending_has_bw;
         best_bw = pending_bw;
         best_start = a;
         best_len = b - a;
      }
   }

   if (!have) {
      return false;
   }
   return resolve_uri(base_uri, body + best_start, best_len, out, out_size);
}

/* --- fetching --- */

/* Every HTTP request goes through here, so the counter is requests, not
 * queued playlists. Caller holds no lock. */
static bool
counted_fetch(cobalt_video_fetch_fn fetch, const char *url,
              cobalt_http_response *out)
{
   lock();
   s_fetches++;
   unlock();
   return fetch(url, out);
}

/*
 * The duration of the playlist at `uri`, in seconds. A master playlist is
 * followed to one variant and no further: the variant is parsed as it is, so a
 * variant that is itself a master gives no duration. That bounds a video at two
 * fetches.
 */
static bool
playlist_seconds(const char *uri, cobalt_video_fetch_fn fetch, double *seconds)
{
   cobalt_http_response response;
   memset(&response, 0, sizeof(response));
   bool found = false;

   if (counted_fetch(fetch, uri, &response) && response.data &&
       response.status == 200) {
      if (cobalt_video_duration_parse((const char *) response.data,
                                      response.size, seconds)) {
         found = true;
      } else {
         char variant[COBALT_VIDEO_DURATION_URI_MAX];
         if (cobalt_video_master_variant((const char *) response.data,
                                         response.size, uri,
                                         variant, sizeof(variant))) {
            cobalt_http_response_free(&response);
            memset(&response, 0, sizeof(response));
            if (counted_fetch(fetch, variant, &response) && response.data &&
                response.status == 200) {
               found = cobalt_video_duration_parse((const char *) response.data,
                                                   response.size, seconds);
            }
         }
      }
   }
   cobalt_http_response_free(&response);
   return found;
}

/*
 * Fetch and parse one playlist. Called with the lock NOT held; `uri` is a copy
 * the caller took while the slot was LOADING, so the slot cannot move under us.
 */
static void
resolve(int index, const char *uri, cobalt_video_fetch_fn fetch)
{
   char text[COBALT_VIDEO_DURATION_TEXT_MAX];
   text[0] = '\0';

   double seconds = 0.0;
   if (playlist_seconds(uri, fetch, &seconds)) {
      cobalt_video_duration_format(seconds, text, sizeof(text));
   }

   lock();
   slot *s = &s_slots[index];
   if (text[0]) {
      snprintf(s->text, sizeof(s->text), "%s", text);
      s->state = SLOT_KNOWN;
   } else {
      s->text[0] = '\0';
      s->state = SLOT_UNKNOWN;
   }
   unlock();
}

static int
loader_main(void *unused)
{
   (void) unused;
   cobalt_thread_make_background();

   lock();
   for (;;) {
      int index = -1;
      while (!s_quit && (index = next_queued()) < 0) {
         SDL_CondWait(s_wake, s_lock);
      }
      if (s_quit) {
         break;
      }

      /* Claim it before unlocking, so no other loader takes the same slot. */
      s_slots[index].state = SLOT_LOADING;
      char uri[COBALT_VIDEO_DURATION_URI_MAX];
      memcpy(uri, s_slots[index].uri, sizeof(uri));
      cobalt_video_fetch_fn fetch = s_fetch ? s_fetch : network_fetch;

      unlock();
      resolve(index, uri, fetch);
      lock();
   }
   unlock();
   return 0;
}

bool
cobalt_video_duration_init(void)
{
   if (s_thread) {
      return true;
   }
   s_lock = SDL_CreateMutex();
   s_wake = SDL_CreateCond();
   if (!s_lock || !s_wake) {
      SDL_DestroyCond(s_wake);
      SDL_DestroyMutex(s_lock);
      s_wake = NULL;
      s_lock = NULL;
      return false;
   }
   s_quit = false;
   s_thread = SDL_CreateThread(loader_main, "video-duration", NULL);
   if (!s_thread) {
      SDL_DestroyCond(s_wake);
      SDL_DestroyMutex(s_lock);
      s_wake = NULL;
      s_lock = NULL;
      return false;
   }
   return true;
}

void
cobalt_video_duration_shutdown(void)
{
   if (!s_thread) {
      return;
   }
   lock();
   s_quit = true;
   SDL_CondBroadcast(s_wake);
   unlock();
   SDL_WaitThread(s_thread, NULL);
   s_thread = NULL;
   SDL_DestroyCond(s_wake);
   SDL_DestroyMutex(s_lock);
   s_wake = NULL;
   s_lock = NULL;
}

const char *
cobalt_video_duration_lookup(const char *playlist_uri)
{
   if (!playlist_uri || !playlist_uri[0] ||
       strlen(playlist_uri) >= COBALT_VIDEO_DURATION_URI_MAX) {
      return NULL;
   }

   const char *text = NULL;
   lock();
   slot *s = find_slot(playlist_uri);
   if (s) {
      if (s->state == SLOT_KNOWN) {
         text = s->text;
      }
   } else {
      for (int i = 0; i < SLOT_COUNT; i++) {
         if (s_slots[i].state == SLOT_FREE) {
            s_slots[i].state = SLOT_QUEUED;
            snprintf(s_slots[i].uri, sizeof(s_slots[i].uri), "%s",
                     playlist_uri);
            if (s_wake) {
               SDL_CondSignal(s_wake);
            }
            break;
         }
      }
   }
   unlock();
   return text;
}

/* --- host tests --- */

void
cobalt_video_duration_set_fetcher(cobalt_video_fetch_fn fetch)
{
   s_fetch = fetch;
}

size_t
cobalt_video_duration_drain(void)
{
   size_t resolved = 0;
   for (;;) {
      lock();
      const int index = next_queued();
      if (index < 0) {
         unlock();
         break;
      }
      s_slots[index].state = SLOT_LOADING;
      char uri[COBALT_VIDEO_DURATION_URI_MAX];
      memcpy(uri, s_slots[index].uri, sizeof(uri));
      cobalt_video_fetch_fn fetch = s_fetch ? s_fetch : network_fetch;
      unlock();

      resolve(index, uri, fetch);
      resolved++;
   }
   return resolved;
}

size_t
cobalt_video_duration_fetch_count(void)
{
   return s_fetches;
}

void
cobalt_video_duration_reset_for_test(void)
{
   memset(s_slots, 0, sizeof(s_slots));
   s_fetches = 0;
}
