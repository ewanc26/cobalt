#include "ui/theme.h"

const SDL_Color COBALT_COLOUR_BG_TOP     = { 0x1C, 0x5A, 0xA8, 0xFF };
const SDL_Color COBALT_COLOUR_BG_BOTTOM  = { 0x0B, 0x2E, 0x5C, 0xFF };
const SDL_Color COBALT_COLOUR_TILE       = { 0xF4, 0xF8, 0xFC, 0xFF };
const SDL_Color COBALT_COLOUR_TILE_FOCUS = { 0xFF, 0xFF, 0xFF, 0xFF };
const SDL_Color COBALT_COLOUR_TILE_EDGE  = { 0xB8, 0xCC, 0xE0, 0xFF };
const SDL_Color COBALT_COLOUR_TEXT       = { 0x18, 0x28, 0x38, 0xFF };
const SDL_Color COBALT_COLOUR_TEXT_DIM   = { 0x5A, 0x6C, 0x7E, 0xFF };
const SDL_Color COBALT_COLOUR_ACCENT     = { 0x3E, 0x8E, 0xDE, 0xFF };
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
   .font_title   = 34,
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
