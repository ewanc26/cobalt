#pragma once

/*
 * Avatars and thumbnails, fetched and decoded off the frame loop.
 *
 * The split matters and is not merely cautious. SDL's Wii U render and video
 * backends contain no locking of any kind — no mutex, no spinlock, nothing —
 * so a texture created from a worker thread would be writing into the same GX2
 * command buffer the frame is being built in. That is corruption, not tearing.
 * So:
 *
 *   - Loader threads do the HTTPS GET, the JPEG/PNG decode and the downscale.
 *     All three are slow — a decode alone is tens of milliseconds on a 1.24 GHz
 *     in-order CPU — and all three are pure CPU work on an SDL_Surface, which
 *     touches no renderer.
 *   - The **main thread** turns finished surfaces into textures, during
 *     cobalt_imagecache_pump(), and is the only thread that ever destroys one.
 *
 * Nothing here blocks. cobalt_imagecache_get() returns NULL until the image has
 * arrived, and callers draw a placeholder in the meantime rather than waiting.
 *
 * Images are downscaled before upload. A Bluesky avatar can be 1000x1000, which
 * is 4 MB as ARGB8888 — twenty-four of those would be 96 MB, far past what this
 * console will give an app. Scaling on the loader thread bounds the texture
 * budget by construction rather than by hoping.
 */

#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Entries and loader threads. Twenty-four covers a screen of cards several
 * times over, so scrolling back does not refetch. Three loaders because each
 * image is a separate TLS handshake to the CDN and the cost is almost all
 * round-trip latency — serially, a screen of eight avatars is several seconds —
 * but kept to three rather than more because the app runs *two* of these caches,
 * one per surface, and every thread here is a thread the session worker and the
 * frame loop are competing with on three cores.
 */
#define COBALT_IMAGECACHE_ENTRIES 24
#define COBALT_IMAGECACHE_LOADERS 3

/* Bluesky CDN URLs run to about 130 bytes; this leaves room and keeps the
 * whole slot table to a few kilobytes. */
#define COBALT_IMAGECACHE_URL_MAX 256

typedef enum {
   /* Preserve aspect ratio; the longest edge becomes max_dimension. */
   COBALT_IMAGE_FIT_CONTAIN = 0,
   /* Centre-crop to a square, then mask to a circle — how avatars are drawn. */
   COBALT_IMAGE_FIT_CIRCLE,
} cobalt_image_fit;

typedef struct cobalt_imagecache cobalt_imagecache;

/*
 * Bring up the decoders and the HTTP client this module needs, once, before any
 * cache is created. `ca_path` is the bundled trust store — the same one the
 * session uses, since an avatar fetch is as much a TLS connection as an XRPC
 * call is.
 *
 * Returns false when images cannot work at all. Not fatal: the app runs without
 * them and every card falls back to its lettered placeholder.
 */
bool cobalt_images_init(const char *ca_path);
void cobalt_images_shutdown(void);

/*
 * `max_dimension` is the longest edge an image is scaled to before upload.
 * 64 suits avatars; a thumbnail strip would want more. Images already smaller
 * than it are left alone rather than upscaled.
 *
 * Returns NULL if no loader thread could start — callers should treat that as
 * "no images" rather than as fatal, exactly as the app already does for the
 * other optional pieces.
 */
cobalt_imagecache *cobalt_imagecache_create(int max_dimension,
                                            cobalt_image_fit fit);
void cobalt_imagecache_destroy(cobalt_imagecache *cache);

/*
 * The texture for `url`, or NULL if it is not ready — which covers "never
 * asked", "still loading" and "failed". Requests the load on a miss.
 *
 * `renderer_owner` is the render context the texture belongs to. A texture
 * cannot be shared between the TV and GamePad renderers, so a cache serves one
 * surface; asking with a different one is a programming error and is refused.
 *
 * Main thread only.
 */
SDL_Texture *cobalt_imagecache_get(cobalt_imagecache *cache,
                                   cobalt_render *renderer_owner,
                                   const char *url, int *out_w, int *out_h);

/*
 * Upload whatever the loaders finished. Call once per frame before drawing.
 * Bounded per call so a burst of arrivals cannot spike a frame.
 *
 * Main thread only.
 */
void cobalt_imagecache_pump(cobalt_imagecache *cache, cobalt_render *owner);

/*
 * Drop every texture and abandon every load in flight. Required when the app
 * loses the foreground: textures do not survive a GX2 context release
 * (AGENTS.md §13), so anything cached against the old one has to go.
 *
 * Main thread only.
 */
void cobalt_imagecache_flush(cobalt_imagecache *cache);

/* Counters for the diagnostics line. Any out pointer may be NULL. */
void cobalt_imagecache_stats(cobalt_imagecache *cache, int *out_ready,
                             int *out_loading, int *out_failed);

/*
 * False on a build without SDL2_image, where decoding is compiled out and every
 * load fails. Lets a caller skip the avatar column entirely rather than show a
 * screen of placeholders that will never resolve.
 */
bool cobalt_imagecache_supported(void);

/* --- scaling, exposed so it can be tested --- */

/*
 * Work out the source crop and the destination size for `fit`. Split out
 * because it is all the arithmetic that can be wrong without being obviously
 * wrong: an off-by-one crop or a truncated aspect ratio looks plausible on a
 * screenshot and is only visible when you check the numbers.
 *
 * Never upscales: a source already inside `max_dimension` comes back at its
 * own size.
 */
void cobalt_image_fit_rects(cobalt_image_fit fit, int max_dimension, int src_w,
                            int src_h, SDL_Rect *out_src, int *out_dst_w,
                            int *out_dst_h);

/*
 * Crop, area-average downscale and (for CIRCLE) alpha-mask `src` into a new
 * ARGB8888 surface. Does not take ownership; returns NULL on failure.
 *
 * Runs on a loader thread in normal use — it touches no renderer.
 */
SDL_Surface *cobalt_image_resample(SDL_Surface *src, int max_dimension,
                                   cobalt_image_fit fit);

#ifdef __cplusplus
}
#endif
