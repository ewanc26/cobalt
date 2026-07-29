#include "ui/imagecache.h"

#include "net/http.h"
#include "util/log.h"

#include <SDL.h>

/*
 * SDL2_image is present on the console and on a developer machine that has the
 * package, but not necessarily on a bare host checkout. Rather than drop this
 * file from the compile sweep — which is how the modules needing devkitPro
 * headers are handled, and which would leave the threading and eviction logic
 * with no coverage at all — the decoder call is the only thing gated. The rest
 * of the file compiles and is checked everywhere.
 */
#if !defined(COBALT_HAS_SDL_IMAGE) && (defined(__WIIU__) || defined(COBALT_FORCE_SDL_IMAGE))
#define COBALT_HAS_SDL_IMAGE 1
#endif

#if defined(COBALT_HAS_SDL_IMAGE)
#include <SDL_image.h>
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * An avatar_thumbnail from the Bluesky CDN is around 30 KB. A megabyte is
 * generous for anything this cache is asked to hold and still small enough
 * that twenty-four of them in flight could not exhaust the heap.
 */
#define MAX_IMAGE_BYTES (1024u * 1024u)

/* Two uploads a frame. A texture upload on this console is a GX2 copy plus an
 * invalidate; a burst of twelve arriving at once would be a visible hitch. */
#define UPLOADS_PER_FRAME 2

typedef enum {
   SLOT_FREE = 0,
   SLOT_QUEUED,    /* wanted, no loader has picked it up */
   SLOT_LOADING,   /* a loader owns it */
   SLOT_DECODED,   /* surface ready, awaiting the main thread */
   SLOT_READY,     /* texture live */
   SLOT_FAILED,    /* fetch or decode failed; do not retry until evicted */
} slot_state;

typedef struct {
   char url[COBALT_IMAGECACHE_URL_MAX];
   slot_state state;
   /*
    * Bumped every time the slot is reassigned or flushed. A loader captures it
    * before releasing the lock and re-checks it afterwards, so a result that
    * arrives for a request nobody wants any more is discarded instead of being
    * written over whatever took the slot.
    */
   uint32_t generation;
   uint32_t last_used;
   SDL_Surface *surface;
   SDL_Texture *texture;
   int w;
   int h;
} slot;

struct cobalt_imagecache {
   SDL_mutex *lock;
   SDL_cond *wake;
   SDL_Thread *loaders[COBALT_IMAGECACHE_LOADERS];
   int loader_count;
   bool quit;

   int max_dimension;
   cobalt_image_fit fit;

   /* Bound on first use; a texture belongs to exactly one renderer. */
   cobalt_render *owner;
   bool warned_owner;

   uint32_t clock;
   slot slots[COBALT_IMAGECACHE_ENTRIES];
};

/* --- library lifecycle --- */

static bool s_images_up;

bool
cobalt_images_init(const char *ca_path)
{
   if (s_images_up) {
      return true;
   }

   if (!cobalt_http_init(ca_path)) {
      return false;
   }

#if defined(COBALT_HAS_SDL_IMAGE)
   /*
    * devkitPro builds SDL2_image with its decoders linked in rather than
    * dlopened — there is no dynamic loader here — so IMG_Init cannot fail for
    * a missing shared object, but it still has to run for the decoders to
    * register. A partial result is worth saying out loud: WEBP missing means
    * a subset of avatars silently never appear.
    */
   const int wanted = IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP;
   const int got = IMG_Init(wanted);
   if ((got & wanted) != wanted) {
      COBALT_LOGW("imagecache: decoders %s%s%sunavailable (%s)",
                  (got & IMG_INIT_JPG) ? "" : "jpg ",
                  (got & IMG_INIT_PNG) ? "" : "png ",
                  (got & IMG_INIT_WEBP) ? "" : "webp ", IMG_GetError());
   }
   if (got == 0) {
      cobalt_http_shutdown();
      return false;
   }
#endif

   s_images_up = true;
   return true;
}

void
cobalt_images_shutdown(void)
{
   if (!s_images_up) {
      return;
   }
#if defined(COBALT_HAS_SDL_IMAGE)
   IMG_Quit();
#endif
   cobalt_http_shutdown();
   s_images_up = false;
}

/* --- scaling --- */

/*
 * Area-average downscale. SDL_BlitScaled is nearest-neighbour for this case,
 * and 1000x1000 down to 64x64 by nearest throws away 99.6% of the pixels: the
 * result is a shimmering mess of whichever samples happened to land on the
 * grid. Averaging is a handful of milliseconds on a loader thread and is the
 * difference between an avatar that reads and one that does not.
 *
 * Colour is averaged premultiplied. Averaging straight RGBA pulls the colour
 * of fully transparent pixels — often black — into the edges of anything with
 * an alpha channel.
 */
static SDL_Surface *
box_downscale(SDL_Surface *src, int sx, int sy, int sw, int sh, int dw, int dh)
{
   if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
      return NULL;
   }

   SDL_Surface *dst =
      SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ARGB8888);
   if (!dst) {
      COBALT_LOGW("imagecache: %dx%d surface allocation failed: %s", dw, dh,
                  SDL_GetError());
      return NULL;
   }

   const uint8_t *src_base = (const uint8_t *) src->pixels;
   uint8_t *dst_base = (uint8_t *) dst->pixels;

   for (int y = 0; y < dh; y++) {
      int y0 = sy + (int) ((int64_t) y * sh / dh);
      int y1 = sy + (int) ((int64_t) (y + 1) * sh / dh);
      if (y1 > sy + sh) y1 = sy + sh;
      if (y1 <= y0) y1 = y0 + 1;

      uint32_t *out = (uint32_t *) (dst_base + (size_t) y * (size_t) dst->pitch);

      for (int x = 0; x < dw; x++) {
         int x0 = sx + (int) ((int64_t) x * sw / dw);
         int x1 = sx + (int) ((int64_t) (x + 1) * sw / dw);
         if (x1 > sx + sw) x1 = sx + sw;
         if (x1 <= x0) x1 = x0 + 1;

         uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0, count = 0;

         for (int yy = y0; yy < y1; yy++) {
            const uint32_t *in =
               (const uint32_t *) (src_base + (size_t) yy * (size_t) src->pitch);
            for (int xx = x0; xx < x1; xx++) {
               const uint32_t p = in[xx];
               const uint32_t a = (p >> 24) & 0xFFu;
               sum_a += a;
               sum_r += (((p >> 16) & 0xFFu) * a) / 255u;
               sum_g += (((p >> 8) & 0xFFu) * a) / 255u;
               sum_b += ((p & 0xFFu) * a) / 255u;
               count++;
            }
         }

         if (count == 0) {
            out[x] = 0;
            continue;
         }

         /* Round the averages rather than truncating. Truncating loses half a
          * level on every channel of every pixel, which on a photo is a
          * uniform darkening — small, but present in every image the app
          * ever draws. */
         const uint32_t a = (sum_a + count / 2u) / count;
         if (a == 0) {
            out[x] = 0;
            continue;
         }

         /* Undo the premultiply, rounding again for the same reason. */
         uint32_t r = (((sum_r + count / 2u) / count) * 255u + a / 2u) / a;
         uint32_t g = (((sum_g + count / 2u) / count) * 255u + a / 2u) / a;
         uint32_t b = (((sum_b + count / 2u) / count) * 255u + a / 2u) / a;
         if (r > 255u) r = 255u;
         if (g > 255u) g = 255u;
         if (b > 255u) b = 255u;

         out[x] = (a << 24) | (r << 16) | (g << 8) | b;
      }
   }

   return dst;
}

/*
 * Punch the square down to a circle in the alpha channel, with a one-pixel
 * feather. At 64 px an unfeathered circle is a visible staircase — the same
 * reason build_corner() in render.c antialiases its corner.
 */
static void
mask_circle(SDL_Surface *surface)
{
   if (!surface) {
      return;
   }

   const float cx = (float) surface->w * 0.5f;
   const float cy = (float) surface->h * 0.5f;
   const float radius = (float) ((surface->w < surface->h) ? surface->w
                                                           : surface->h) * 0.5f;

   uint8_t *base = (uint8_t *) surface->pixels;

   for (int y = 0; y < surface->h; y++) {
      uint32_t *row = (uint32_t *) (base + (size_t) y * (size_t) surface->pitch);
      const float dy = ((float) y + 0.5f) - cy;

      for (int x = 0; x < surface->w; x++) {
         const float dx = ((float) x + 0.5f) - cx;
         const float cover = radius - sqrtf(dx * dx + dy * dy) + 0.5f;

         if (cover >= 1.0f) {
            continue;
         }
         if (cover <= 0.0f) {
            row[x] = 0;
            continue;
         }

         const uint32_t p = row[x];
         const uint32_t a = (uint32_t) ((float) ((p >> 24) & 0xFFu) * cover);
         row[x] = (a << 24) | (p & 0x00FFFFFFu);
      }
   }
}

void
cobalt_image_fit_rects(cobalt_image_fit fit, int max_dimension, int src_w,
                       int src_h, SDL_Rect *out_src, int *out_dst_w,
                       int *out_dst_h)
{
   if (out_src) {
      out_src->x = out_src->y = out_src->w = out_src->h = 0;
   }
   if (out_dst_w) *out_dst_w = 0;
   if (out_dst_h) *out_dst_h = 0;

   if (src_w <= 0 || src_h <= 0 || max_dimension <= 0) {
      return;
   }

   int sx = 0, sy = 0, sw = src_w, sh = src_h;
   int dw, dh;

   if (fit == COBALT_IMAGE_FIT_CIRCLE) {
      /* Centre-crop to a square first. Bluesky's avatar URLs are already
       * square, but a self-hosted PDS is under no obligation to agree. */
      const int side = (sw < sh) ? sw : sh;
      sx = (sw - side) / 2;
      sy = (sh - side) / 2;
      sw = side;
      sh = side;
      dw = dh = (side < max_dimension) ? side : max_dimension;
   } else {
      const int longest = (sw > sh) ? sw : sh;
      if (longest <= max_dimension) {
         dw = sw;
         dh = sh;
      } else {
         /* Round rather than truncate, so a 3:2 image does not come back at
          * an aspect ratio a pixel off. */
         dw = (int) (((int64_t) sw * max_dimension + longest / 2) / longest);
         dh = (int) (((int64_t) sh * max_dimension + longest / 2) / longest);
         if (dw < 1) dw = 1;
         if (dh < 1) dh = 1;
      }
   }

   if (out_src) {
      out_src->x = sx;
      out_src->y = sy;
      out_src->w = sw;
      out_src->h = sh;
   }
   if (out_dst_w) *out_dst_w = dw;
   if (out_dst_h) *out_dst_h = dh;
}

SDL_Surface *
cobalt_image_resample(SDL_Surface *src, int max_dimension, cobalt_image_fit fit)
{
   if (!src || src->format->format != SDL_PIXELFORMAT_ARGB8888) {
      return NULL;
   }

   SDL_Rect crop;
   int dw = 0, dh = 0;
   cobalt_image_fit_rects(fit, max_dimension, src->w, src->h, &crop, &dw, &dh);

   SDL_Surface *out =
      box_downscale(src, crop.x, crop.y, crop.w, crop.h, dw, dh);
   if (out && fit == COBALT_IMAGE_FIT_CIRCLE) {
      mask_circle(out);
   }
   return out;
}

/* --- loading --- */

static SDL_Surface *
decode(const unsigned char *data, size_t size)
{
#if defined(COBALT_HAS_SDL_IMAGE)
   if (size > (size_t) INT32_MAX) {
      return NULL;
   }

   SDL_RWops *rw = SDL_RWFromConstMem(data, (int) size);
   if (!rw) {
      return NULL;
   }

   /* freesrc = 1: IMG_Load_RW closes the RWops on both paths. The memory it
    * wraps is the caller's and outlives the call. */
   SDL_Surface *surface = IMG_Load_RW(rw, 1);
   if (!surface) {
      COBALT_LOGW("imagecache: decode failed: %s", IMG_GetError());
   }
   return surface;
#else
   (void) data;
   (void) size;
   return NULL;
#endif
}

static SDL_Surface *
load_one(const char *url, int max_dimension, cobalt_image_fit fit)
{
   cobalt_http_response response;
   if (!cobalt_http_get(url, MAX_IMAGE_BYTES, &response)) {
      return NULL;
   }

   SDL_Surface *decoded = decode(response.data, response.size);
   cobalt_http_response_free(&response);
   if (!decoded) {
      return NULL;
   }

   /* Normalise before touching pixels: a decoder may hand back 24-bit RGB,
    * palettised data or a different channel order, and the scaler reads
    * 32-bit words directly. */
   SDL_Surface *rgba =
      SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_ARGB8888, 0);
   SDL_FreeSurface(decoded);
   if (!rgba) {
      COBALT_LOGW("imagecache: format conversion failed: %s", SDL_GetError());
      return NULL;
   }

   SDL_Surface *scaled = cobalt_image_resample(rgba, max_dimension, fit);
   SDL_FreeSurface(rgba);
   return scaled;
}

/* Caller holds the lock. */
static int
next_queued(const cobalt_imagecache *cache)
{
   for (int i = 0; i < COBALT_IMAGECACHE_ENTRIES; i++) {
      if (cache->slots[i].state == SLOT_QUEUED) {
         return i;
      }
   }
   return -1;
}

static int
loader_main(void *userdata)
{
   cobalt_imagecache *cache = (cobalt_imagecache *) userdata;

   SDL_LockMutex(cache->lock);
   for (;;) {
      int index = -1;
      while (!cache->quit && (index = next_queued(cache)) < 0) {
         SDL_CondWait(cache->wake, cache->lock);
      }
      if (cache->quit) {
         break;
      }

      /* Claim it before unlocking, so no other loader takes the same slot. */
      cache->slots[index].state = SLOT_LOADING;

      const uint32_t generation = cache->slots[index].generation;
      const int max_dimension = cache->max_dimension;
      const cobalt_image_fit fit = cache->fit;
      char url[COBALT_IMAGECACHE_URL_MAX];
      memcpy(url, cache->slots[index].url, sizeof(url));

      SDL_UnlockMutex(cache->lock);
      SDL_Surface *surface = load_one(url, max_dimension, fit);
      SDL_LockMutex(cache->lock);

      slot *s = &cache->slots[index];
      if (s->generation != generation || s->state != SLOT_LOADING) {
         /* Evicted or flushed while we were off the lock. */
         SDL_FreeSurface(surface);
      } else if (surface) {
         s->surface = surface;
         s->state = SLOT_DECODED;
      } else {
         s->state = SLOT_FAILED;
      }
   }
   SDL_UnlockMutex(cache->lock);

   return 0;
}

/* --- slots --- */

/*
 * Caller holds the lock, and is on the main thread — this is the only place
 * besides pump/flush/destroy that touches a texture.
 */
static void
release_slot(slot *s)
{
   if (s->texture) {
      SDL_DestroyTexture(s->texture);
      s->texture = NULL;
   }
   if (s->surface) {
      SDL_FreeSurface(s->surface);
      s->surface = NULL;
   }
   s->url[0] = '\0';
   s->state = SLOT_FREE;
   s->w = 0;
   s->h = 0;
   s->generation++;
}

/* Caller holds the lock. */
static void
request_locked(cobalt_imagecache *cache, const char *url)
{
   int chosen = -1;
   uint32_t oldest = 0;

   for (int i = 0; i < COBALT_IMAGECACHE_ENTRIES; i++) {
      const slot *s = &cache->slots[i];
      if (s->state == SLOT_FREE) {
         chosen = i;
         break;
      }
      /* Only finished slots may be reused. Taking one mid-flight would throw
       * away a request that is already paid for, and a screen scrolling fast
       * enough to wrap the table would then never settle. */
      if (s->state != SLOT_READY && s->state != SLOT_FAILED) {
         continue;
      }
      if (chosen < 0 || s->last_used < oldest) {
         chosen = i;
         oldest = s->last_used;
      }
   }

   if (chosen < 0) {
      /* Every slot is in flight. The caller asks again next frame. */
      return;
   }

   slot *s = &cache->slots[chosen];
   release_slot(s);
   snprintf(s->url, sizeof(s->url), "%s", url);
   s->state = SLOT_QUEUED;
   s->last_used = ++cache->clock;

   SDL_CondSignal(cache->wake);
}

/* --- lifecycle --- */

cobalt_imagecache *
cobalt_imagecache_create(int max_dimension, cobalt_image_fit fit)
{
   if (max_dimension <= 0) {
      return NULL;
   }

   cobalt_imagecache *cache =
      (cobalt_imagecache *) calloc(1, sizeof(cobalt_imagecache));
   if (!cache) {
      return NULL;
   }

   cache->max_dimension = max_dimension;
   cache->fit = fit;

   cache->lock = SDL_CreateMutex();
   cache->wake = SDL_CreateCond();
   if (!cache->lock || !cache->wake) {
      COBALT_LOGE("imagecache: mutex/cond creation failed: %s", SDL_GetError());
      cobalt_imagecache_destroy(cache);
      return NULL;
   }

   for (int i = 0; i < COBALT_IMAGECACHE_LOADERS; i++) {
      char name[24];
      snprintf(name, sizeof(name), "cobalt-img-%d", i);
      SDL_Thread *thread = SDL_CreateThread(loader_main, name, cache);
      if (!thread) {
         break;
      }
      cache->loaders[cache->loader_count++] = thread;
   }

   if (cache->loader_count == 0) {
      COBALT_LOGE("imagecache: no loader thread started: %s", SDL_GetError());
      cobalt_imagecache_destroy(cache);
      return NULL;
   }
   if (cache->loader_count < COBALT_IMAGECACHE_LOADERS) {
      COBALT_LOGW("imagecache: %d of %d loaders started; images will be slower",
                  cache->loader_count, COBALT_IMAGECACHE_LOADERS);
   }

   return cache;
}

void
cobalt_imagecache_destroy(cobalt_imagecache *cache)
{
   if (!cache) {
      return;
   }

   if (cache->lock) {
      SDL_LockMutex(cache->lock);
      cache->quit = true;
      if (cache->wake) {
         SDL_CondBroadcast(cache->wake);
      }
      SDL_UnlockMutex(cache->lock);
   }

   /* Joined before anything is freed: a loader still off the lock holds a
    * pointer to this struct. */
   for (int i = 0; i < cache->loader_count; i++) {
      SDL_WaitThread(cache->loaders[i], NULL);
   }

   for (int i = 0; i < COBALT_IMAGECACHE_ENTRIES; i++) {
      release_slot(&cache->slots[i]);
   }

   if (cache->wake) {
      SDL_DestroyCond(cache->wake);
   }
   if (cache->lock) {
      SDL_DestroyMutex(cache->lock);
   }
   free(cache);
}

/* --- use --- */

/* Caller holds the lock. Returns false if `owner` is not the bound one. */
static bool
bind_owner(cobalt_imagecache *cache, cobalt_render *owner)
{
   if (!cache->owner) {
      cache->owner = owner;
      return true;
   }
   if (cache->owner == owner) {
      return true;
   }
   if (!cache->warned_owner) {
      cache->warned_owner = true;
      COBALT_LOGE("imagecache: asked by a second render context; ignoring it");
   }
   return false;
}

SDL_Texture *
cobalt_imagecache_get(cobalt_imagecache *cache, cobalt_render *renderer_owner,
                      const char *url, int *out_w, int *out_h)
{
   if (out_w) *out_w = 0;
   if (out_h) *out_h = 0;

   if (!cache || !renderer_owner || !url || !url[0]) {
      return NULL;
   }
   if (strlen(url) >= COBALT_IMAGECACHE_URL_MAX) {
      /* Truncating would collide two different images onto one slot. */
      return NULL;
   }

   SDL_Texture *texture = NULL;
   SDL_LockMutex(cache->lock);

   if (bind_owner(cache, renderer_owner)) {
      int found = -1;
      for (int i = 0; i < COBALT_IMAGECACHE_ENTRIES; i++) {
         slot *s = &cache->slots[i];
         if (s->state != SLOT_FREE && strcmp(s->url, url) == 0) {
            found = i;
            break;
         }
      }

      if (found < 0) {
         request_locked(cache, url);
      } else {
         slot *s = &cache->slots[found];
         s->last_used = ++cache->clock;
         if (s->state == SLOT_READY) {
            texture = s->texture;
            if (out_w) *out_w = s->w;
            if (out_h) *out_h = s->h;
         }
      }
   }

   SDL_UnlockMutex(cache->lock);
   return texture;
}

void
cobalt_imagecache_pump(cobalt_imagecache *cache, cobalt_render *owner)
{
   if (!cache || !owner) {
      return;
   }

   int uploaded = 0;
   SDL_LockMutex(cache->lock);

   if (!bind_owner(cache, owner)) {
      SDL_UnlockMutex(cache->lock);
      return;
   }

   for (int i = 0; i < COBALT_IMAGECACHE_ENTRIES && uploaded < UPLOADS_PER_FRAME;
        i++) {
      slot *s = &cache->slots[i];
      if (s->state != SLOT_DECODED || !s->surface) {
         continue;
      }

      /* Take the surface out and drop the lock across the upload: a GX2 copy
       * is not something to hold a mutex four loader threads want through. */
      SDL_Surface *surface = s->surface;
      s->surface = NULL;
      const uint32_t generation = s->generation;

      SDL_UnlockMutex(cache->lock);
      SDL_Texture *texture = cobalt_render_upload(owner, surface);
      const int w = surface->w;
      const int h = surface->h;
      SDL_FreeSurface(surface);
      SDL_LockMutex(cache->lock);

      uploaded++;

      /* Nothing but this thread reassigns a slot, so the generation cannot
       * have moved — checked anyway, because the cost is a compare and the
       * failure would be a texture drawn for the wrong post. */
      if (cache->slots[i].generation != generation) {
         if (texture) {
            SDL_DestroyTexture(texture);
         }
         continue;
      }

      if (texture) {
         cache->slots[i].texture = texture;
         cache->slots[i].w = w;
         cache->slots[i].h = h;
         cache->slots[i].state = SLOT_READY;
      } else {
         cache->slots[i].state = SLOT_FAILED;
      }
   }

   SDL_UnlockMutex(cache->lock);
}

void
cobalt_imagecache_flush(cobalt_imagecache *cache)
{
   if (!cache) {
      return;
   }

   SDL_LockMutex(cache->lock);
   for (int i = 0; i < COBALT_IMAGECACHE_ENTRIES; i++) {
      /* release_slot() bumps the generation, which is what makes this safe
       * against a loader that is mid-fetch: its result is dropped on return
       * rather than landing in a slot the new renderer now owns. */
      release_slot(&cache->slots[i]);
   }
   cache->owner = NULL;
   SDL_UnlockMutex(cache->lock);
}

void
cobalt_imagecache_stats(cobalt_imagecache *cache, int *out_ready,
                        int *out_loading, int *out_failed)
{
   int ready = 0, loading = 0, failed = 0;

   if (cache) {
      SDL_LockMutex(cache->lock);
      for (int i = 0; i < COBALT_IMAGECACHE_ENTRIES; i++) {
         switch (cache->slots[i].state) {
         case SLOT_READY:   ready++;   break;
         case SLOT_FAILED:  failed++;  break;
         case SLOT_QUEUED:
         case SLOT_LOADING:
         case SLOT_DECODED: loading++; break;
         case SLOT_FREE:               break;
         }
      }
      SDL_UnlockMutex(cache->lock);
   }

   if (out_ready)   *out_ready = ready;
   if (out_loading) *out_loading = loading;
   if (out_failed)  *out_failed = failed;
}

bool
cobalt_imagecache_supported(void)
{
#if defined(COBALT_HAS_SDL_IMAGE)
   return true;
#else
   return false;
#endif
}
