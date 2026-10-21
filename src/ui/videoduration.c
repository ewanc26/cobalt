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

/*
 * Fetch and parse one playlist. Called with the lock NOT held; `uri` is a copy
 * the caller took while the slot was LOADING, so the slot cannot move under us.
 */
static void
resolve(int index, const char *uri, cobalt_video_fetch_fn fetch)
{
   cobalt_http_response response;
   memset(&response, 0, sizeof(response));

   char text[COBALT_VIDEO_DURATION_TEXT_MAX];
   text[0] = '\0';

   const bool fetched = fetch(uri, &response);
   if (fetched && response.data && response.status == 200) {
      double seconds = 0.0;
      if (cobalt_video_duration_parse((const char *) response.data,
                                      response.size, &seconds)) {
         cobalt_video_duration_format(seconds, text, sizeof(text));
      }
   }
   cobalt_http_response_free(&response);

   lock();
   s_fetches++;
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
