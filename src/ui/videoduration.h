#pragma once

/*
 * The length of a video post, for its poster frame.
 *
 * app.bsky.embed.video#view carries no duration. Its `playlist` is an HLS
 * (m3u8) playlist, and the length is the sum of its #EXTINF lines. Wolfram
 * parses that body (wf_embed_video_playlist_duration) but does no I/O, so this
 * module does the fetch.
 *
 * The fetch happens off the frame loop, on a loader thread, and only for a
 * video that is actually drawn: cobalt_video_duration_lookup() queues a
 * playlist the first time it is asked about, and never again. The result is
 * kept per playlist URI, so a playlist is fetched at most once however many
 * frames or posts ask for it. A master playlist, a live one, a failed fetch or
 * an unparseable body all leave the duration unknown, and the poster then shows
 * no duration.
 *
 * Main thread only for lookup. The loader thread never touches the frame loop.
 */

#include <stdbool.h>
#include <stddef.h>

#include "net/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A playlist URI from a PDS response is bounded the same way as an image URL. */
#define COBALT_VIDEO_DURATION_URI_MAX 256
/* "h:mm:ss" plus NUL, with room to spare. */
#define COBALT_VIDEO_DURATION_TEXT_MAX 16

/* Starts the loader thread. Returns false when it cannot start, in which case
 * lookups stay NULL and nothing is fetched. Call after cobalt_images_init(),
 * which brings up the HTTP client the fetch uses. */
bool cobalt_video_duration_init(void);
void cobalt_video_duration_shutdown(void);

/*
 * The duration text for `playlist_uri` ("1:23", "1:02:03"), or NULL while it
 * is unknown. A first call for a URI queues the fetch; later calls only read.
 * When the table is full a new URI is not queued, so it stays NULL.
 *
 * The returned pointer stays valid for the life of the process once non-NULL.
 *
 * Main thread only.
 */
const char *cobalt_video_duration_lookup(const char *playlist_uri);

/* "m:ss" under an hour, "h:mm:ss" from an hour, rounded to the nearest second.
 * Returns false (leaving `out` empty) for a negative or non-finite value, or
 * when the text would not fit. */
bool cobalt_video_duration_format(double seconds, char *out, size_t out_size);

/* The duration of an m3u8 playlist body, in seconds. False when Wolfram cannot
 * give one (a master playlist, a live playlist, or not a playlist), and always
 * false on a build without Wolfram. */
bool cobalt_video_duration_parse(const char *body, size_t len, double *seconds);

/* --- for the host tests only --- */

typedef bool (*cobalt_video_fetch_fn)(const char *url,
                                      cobalt_http_response *out);

/* Replaces the network fetch. NULL restores it. */
void cobalt_video_duration_set_fetcher(cobalt_video_fetch_fn fetch);
/* Resolves every queued playlist on the calling thread, without the loader.
 * Returns how many it resolved. Only valid when the loader is not running. */
size_t cobalt_video_duration_drain(void);
/* How many fetches have run since the last reset. */
size_t cobalt_video_duration_fetch_count(void);
/* Clears the table and the counter. Only valid when the loader is not running. */
void cobalt_video_duration_reset_for_test(void);

#ifdef __cplusplus
}
#endif
