#pragma once

/*
 * The full-size image viewer: an overlay drawn over whatever screen opened
 * it, on both surfaces. It holds a copy of the post's image list rather than
 * a pointer into a feed buffer, so a background refresh cannot rewrite the
 * pictures out from under the person looking at them — the same reasoning as
 * the popup's item copies.
 *
 * The image is drawn contain-fitted into the surface's whole area, centred,
 * from the aspect ratio the server declared (the texture's real size once it
 * arrives, the declared ratio until then). The author's alt text is drawn
 * beneath the image when there is one; the count ("2 of 4") is drawn in the
 * corner when a post carries several pictures.
 */

#include "atproto/feed.h"
#include "input/input.h"
#include "ui/render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   bool open;

   /* Copied at open time; see the file comment. */
   cobalt_post_image images[COBALT_POST_IMAGES_MAX];
   int image_count;
   int index;

   /* Touch: any tap closes. The viewer is a deliberate look at one picture;
    * there is nothing in it precise enough to need a smaller target, and
    * Left/Right on the D-pad already cycle. */
   SDL_Rect hit_close;
   bool hit_valid;
} cobalt_imageview;

/* Opens on the post's first image. A no-op when the post carries none, so
 * callers do not each have to check. */
void cobalt_imageview_open(cobalt_imageview *view, const cobalt_post *post);
void cobalt_imageview_close(cobalt_imageview *view);

/* Returns true when the viewer consumed the input this frame; the screen
 * underneath must not also act on it. */
bool cobalt_imageview_update(cobalt_imageview *view, const cobalt_input *in);

void cobalt_imageview_draw(cobalt_imageview *view, cobalt_render *r);

#ifdef __cplusplus
}
#endif
