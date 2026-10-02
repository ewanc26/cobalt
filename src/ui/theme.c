#include "ui/theme.h"

/* Wii U menu / Miiverse: light grey-white ground, white cards, a cobalt-blue
 * header band, and soft grey text. */
const SDL_Color COBALT_COLOUR_BG_TOP     = { 0xF2, 0xF5, 0xF6, 0xFF };
const SDL_Color COBALT_COLOUR_BG_BOTTOM  = { 0xDC, 0xE3, 0xE7, 0xFF };
const SDL_Color COBALT_COLOUR_BAND_TOP   = { 0x2F, 0x78, 0xD9, 0xFF };
const SDL_Color COBALT_COLOUR_BAND_BOTTOM = { 0x0F, 0x4C, 0xB0, 0xFF };
const SDL_Color COBALT_COLOUR_TILE       = { 0xFF, 0xFF, 0xFF, 0xFF };
const SDL_Color COBALT_COLOUR_TILE_FOCUS = { 0xFF, 0xFF, 0xFF, 0xFF };
const SDL_Color COBALT_COLOUR_TILE_EDGE  = { 0xCF, 0xD8, 0xDC, 0xFF };
const SDL_Color COBALT_COLOUR_TEXT       = { 0x33, 0x3B, 0x40, 0xFF };
const SDL_Color COBALT_COLOUR_TEXT_DIM   = { 0x56, 0x64, 0x70, 0xFF };
const SDL_Color COBALT_COLOUR_ACCENT     = { 0x1E, 0x6B, 0xE0, 0xFF };
const SDL_Color COBALT_COLOUR_ACCENT_TEXT = { 0x0B, 0x4A, 0xA8, 0xFF };
const SDL_Color COBALT_COLOUR_ERROR      = { 0xD9, 0x4B, 0x4B, 0xFF };

/*
 * TV metrics assume a living-room viewing distance: fewer, larger things.
 * The 28px body size is roughly the smallest that stays comfortable on a 720p
 * output at typical seating distance.
 */
static const cobalt_metrics TV_METRICS = {
   .width        = COBALT_TV_WIDTH,
   .height       = COBALT_TV_HEIGHT,
   .font_title   = 52,
   .font_heading = 34,
   .font_body    = 28,
   .font_caption = 22,
   .pad_edge     = 48,
   .pad_tile     = 24,
   .gap          = 20,
   .tile_radius  = 16,
   .line_gap     = 6,
};

/*
 * The GamePad is not a scaled-down TV. It is a smaller panel held much closer,
 * so it takes a *denser* layout with proportionally smaller margins — scaling
 * the TV metrics by 854/1280 would waste most of the panel on padding.
 */
static const cobalt_metrics DRC_METRICS = {
   .width        = COBALT_DRC_WIDTH,
   .height       = COBALT_DRC_HEIGHT,
   .font_title   = 30,
   .font_heading = 26,
   .font_body    = 21,
   .font_caption = 17,
   .pad_edge     = 20,
   .pad_tile     = 14,
   .gap          = 12,
   .tile_radius  = 12,
   .line_gap     = 4,
};

const cobalt_metrics *
cobalt_metrics_for(cobalt_surface_id surface)
{
   return (surface == COBALT_SURFACE_DRC) ? &DRC_METRICS : &TV_METRICS;
}
