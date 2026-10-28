#include "app/credits_view.h"

#include "app/app_internal.h"

#include <stddef.h>

/*
 * The stack in reading order: the two console clients, the SDK they share, the
 * Mac client and the Wii client that came before Cobalt. The URLs are the
 * canonical repositories, not release or documentation pages, because those
 * move and a repository is what someone would clone.
 */
static const cobalt_credit_repo REPOS[] = {
   { "Cobalt",       "https://github.com/ewanc26/cobalt" },
   { "Indigo",       "https://github.com/ewanc26/indigo" },
   { "Wolfram",      "https://github.com/ewanc26/wolfram" },
   { "Platinum",     "https://github.com/ewanc26/platinum" },
   { "Channel Blue", "https://github.com/ewanc26/channel-blue" },
};

int
cobalt_credits_repo_count(void)
{
   return (int) (sizeof(REPOS) / sizeof(REPOS[0]));
}

const cobalt_credit_repo *
cobalt_credits_repo(int index)
{
   if (index < 0 || index >= cobalt_credits_repo_count()) {
      return NULL;
   }
   return &REPOS[index];
}

void
cobalt_credits_view_draw(cobalt_render *r)
{
   if (!r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int width = m->width - 2 * m->pad_edge;
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int pad = m->pad_tile / 2;

   cobalt_app_draw_header(r, "Credits");

   int y = cobalt_content_top(r);
   y += cobalt_draw_text_wrapped(
      r, COBALT_FONT_CAPTION,
      "Cobalt, Indigo, Platinum and Channel Blue are separate clients over one "
      "C AT Protocol SDK. The full repository address is printed for each, so "
      "it can be read from the screen and typed on another machine.",
      m->pad_edge, y, width, 3, COBALT_COLOUR_TEXT_DIM);
   y += m->gap * 2;

   const int count = cobalt_credits_repo_count();
   for (int i = 0; i < count; i++) {
      const cobalt_credit_repo *repo = cobalt_credits_repo(i);
      int name_w = 0;
      int url_w = 0;

      cobalt_text_size(r, COBALT_FONT_BODY, repo->name, &name_w, NULL);
      cobalt_text_size(r, COBALT_FONT_CAPTION, repo->url, &url_w, NULL);

      const int inner_w = width - 2 * pad;
      const bool same_line = name_w + m->gap + url_w <= inner_w;
      const int row_h = same_line ? body_h + 2 * pad
                                  : body_h + caption_h + 2 * pad;

      SDL_Rect row = { m->pad_edge, y, width, row_h };

      cobalt_draw_tile(r, &row, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY, repo->name,
                       row.x + pad, row.y + pad, COBALT_COLOUR_TEXT);
      if (same_line) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, repo->url,
                          row.x + row.w - pad - url_w,
                          row.y + pad + (body_h - caption_h) / 2,
                          COBALT_COLOUR_ACCENT_TEXT);
      } else {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, repo->url,
                          row.x + pad, row.y + pad + body_h,
                          COBALT_COLOUR_ACCENT_TEXT);
      }

      y += row_h + m->gap / 2;
   }
}
