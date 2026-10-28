#include "ui/theme.h"

/* Wii U menu / Miiverse: light grey-white ground, white cards, a cobalt-blue
 * header band, and soft grey text. */
SDL_Color COBALT_COLOUR_BG_TOP     = { 0xF2, 0xF5, 0xF6, 0xFF };
SDL_Color COBALT_COLOUR_BG_BOTTOM  = { 0xDC, 0xE3, 0xE7, 0xFF };
SDL_Color COBALT_COLOUR_BAND_TOP   = { 0x2F, 0x78, 0xD9, 0xFF };
SDL_Color COBALT_COLOUR_BAND_BOTTOM = { 0x0F, 0x4C, 0xB0, 0xFF };
SDL_Color COBALT_COLOUR_TILE       = { 0xFF, 0xFF, 0xFF, 0xFF };
SDL_Color COBALT_COLOUR_TILE_EDGE  = { 0xCF, 0xD8, 0xDC, 0xFF };
SDL_Color COBALT_COLOUR_TEXT       = { 0x33, 0x3B, 0x40, 0xFF };
SDL_Color COBALT_COLOUR_TEXT_DIM   = { 0x56, 0x64, 0x70, 0xFF };
SDL_Color COBALT_COLOUR_HEADER_TEXT = { 0xFF, 0xFF, 0xFF, 0xFF };
SDL_Color COBALT_COLOUR_ACCENT     = { 0x1E, 0x6B, 0xE0, 0xFF };
SDL_Color COBALT_COLOUR_ACCENT_TEXT = { 0x0B, 0x4A, 0xA8, 0xFF };
SDL_Color COBALT_COLOUR_ON_ACCENT  = { 0xFF, 0xFF, 0xFF, 0xFF };
SDL_Color COBALT_COLOUR_ERROR      = { 0xD9, 0x4B, 0x4B, 0xFF };
SDL_Color COBALT_COLOUR_PILL       = { 0xFF, 0xFF, 0xFF, 0xE6 };
SDL_Color COBALT_COLOUR_CHIP       = { 0xEE, 0xF1, 0xF3, 0xFF };

static cobalt_theme s_theme = COBALT_THEME_LIGHT;
static bool s_high_contrast = false;
static unsigned s_generation = 1;

/* The same roles in a dark room: near-black ground, raised tiles, a deeper
 * cobalt band, and light text. The accent and error colours are raised in
 * luminance so they still read against the dark surfaces. */
static const SDL_Color DARK_BG_TOP      = { 0x17, 0x1B, 0x22, 0xFF };
static const SDL_Color DARK_BG_BOTTOM   = { 0x0D, 0x10, 0x16, 0xFF };
static const SDL_Color DARK_BAND_TOP    = { 0x1C, 0x50, 0x9E, 0xFF };
static const SDL_Color DARK_BAND_BOTTOM = { 0x0A, 0x2C, 0x66, 0xFF };
static const SDL_Color DARK_TILE        = { 0x23, 0x29, 0x33, 0xFF };
static const SDL_Color DARK_TILE_EDGE   = { 0x3B, 0x45, 0x54, 0xFF };
static const SDL_Color DARK_TEXT        = { 0xF2, 0xF5, 0xF6, 0xFF };
static const SDL_Color DARK_TEXT_DIM    = { 0xB5, 0xC0, 0xCC, 0xFF };
static const SDL_Color DARK_ACCENT      = { 0x5A, 0x9B, 0xF0, 0xFF };
static const SDL_Color DARK_ACCENT_TEXT = { 0x8E, 0xBE, 0xFF, 0xFF };
static const SDL_Color DARK_ON_ACCENT   = { 0x0B, 0x12, 0x1C, 0xFF };
static const SDL_Color DARK_ERROR       = { 0xFF, 0x8A, 0x80, 0xFF };
static const SDL_Color DARK_PILL        = { 0x23, 0x29, 0x33, 0xF0 };
static const SDL_Color DARK_CHIP        = { 0x2E, 0x36, 0x42, 0xFF };

/*
 * High contrast is its own palette, not a light/dark variant: pure black
 * ground, near-white text and accents lifted far enough to read at a distance.
 * The band stays a little lighter than the ground so the title and Back pill
 * still have an edge to sit on.
 */
static const SDL_Color CONTRAST_BG_TOP      = { 0x00, 0x00, 0x00, 0xFF };
static const SDL_Color CONTRAST_BG_BOTTOM   = { 0x00, 0x00, 0x00, 0xFF };
static const SDL_Color CONTRAST_BAND_TOP    = { 0x1E, 0x25, 0x33, 0xFF };
static const SDL_Color CONTRAST_BAND_BOTTOM = { 0x08, 0x0B, 0x11, 0xFF };
static const SDL_Color CONTRAST_TILE        = { 0x12, 0x16, 0x1E, 0xFF };
static const SDL_Color CONTRAST_TILE_EDGE   = { 0x70, 0x7A, 0x8C, 0xFF };
static const SDL_Color CONTRAST_TEXT        = { 0xFF, 0xFF, 0xFF, 0xFF };
static const SDL_Color CONTRAST_TEXT_DIM    = { 0xD2, 0xD8, 0xE4, 0xFF };
static const SDL_Color CONTRAST_ACCENT      = { 0x8A, 0xBC, 0xFF, 0xFF };
static const SDL_Color CONTRAST_ACCENT_TEXT = { 0xA8, 0xCC, 0xFF, 0xFF };
static const SDL_Color CONTRAST_ON_ACCENT   = { 0x05, 0x08, 0x0D, 0xFF };
static const SDL_Color CONTRAST_ERROR       = { 0xFF, 0xAA, 0xA2, 0xFF };
static const SDL_Color CONTRAST_PILL        = { 0x1C, 0x22, 0x30, 0xF0 };
static const SDL_Color CONTRAST_CHIP        = { 0x26, 0x2E, 0x3C, 0xFF };

static void
apply_palette(void)
{
   s_generation++;
   if (s_high_contrast) {
      COBALT_COLOUR_BG_TOP = CONTRAST_BG_TOP;
      COBALT_COLOUR_BG_BOTTOM = CONTRAST_BG_BOTTOM;
      COBALT_COLOUR_BAND_TOP = CONTRAST_BAND_TOP;
      COBALT_COLOUR_BAND_BOTTOM = CONTRAST_BAND_BOTTOM;
      COBALT_COLOUR_TILE = CONTRAST_TILE;
      COBALT_COLOUR_TILE_EDGE = CONTRAST_TILE_EDGE;
      COBALT_COLOUR_TEXT = CONTRAST_TEXT;
      COBALT_COLOUR_TEXT_DIM = CONTRAST_TEXT_DIM;
      COBALT_COLOUR_HEADER_TEXT = CONTRAST_TEXT;
      COBALT_COLOUR_ACCENT = CONTRAST_ACCENT;
      COBALT_COLOUR_ACCENT_TEXT = CONTRAST_ACCENT_TEXT;
      COBALT_COLOUR_ON_ACCENT = CONTRAST_ON_ACCENT;
      COBALT_COLOUR_ERROR = CONTRAST_ERROR;
      COBALT_COLOUR_PILL = CONTRAST_PILL;
      COBALT_COLOUR_CHIP = CONTRAST_CHIP;
   } else if (s_theme == COBALT_THEME_DARK) {
      COBALT_COLOUR_BG_TOP = DARK_BG_TOP;
      COBALT_COLOUR_BG_BOTTOM = DARK_BG_BOTTOM;
      COBALT_COLOUR_BAND_TOP = DARK_BAND_TOP;
      COBALT_COLOUR_BAND_BOTTOM = DARK_BAND_BOTTOM;
      COBALT_COLOUR_TILE = DARK_TILE;
      COBALT_COLOUR_TILE_EDGE = DARK_TILE_EDGE;
      COBALT_COLOUR_TEXT = DARK_TEXT;
      COBALT_COLOUR_TEXT_DIM = DARK_TEXT_DIM;
      COBALT_COLOUR_HEADER_TEXT = DARK_TEXT;
      COBALT_COLOUR_ACCENT = DARK_ACCENT;
      COBALT_COLOUR_ACCENT_TEXT = DARK_ACCENT_TEXT;
      COBALT_COLOUR_ON_ACCENT = DARK_ON_ACCENT;
      COBALT_COLOUR_ERROR = DARK_ERROR;
      COBALT_COLOUR_PILL = DARK_PILL;
      COBALT_COLOUR_CHIP = DARK_CHIP;
   } else {
      COBALT_COLOUR_BG_TOP = (SDL_Color) { 0xF2, 0xF5, 0xF6, 0xFF };
      COBALT_COLOUR_BG_BOTTOM = (SDL_Color) { 0xDC, 0xE3, 0xE7, 0xFF };
      COBALT_COLOUR_BAND_TOP = (SDL_Color) { 0x2F, 0x78, 0xD9, 0xFF };
      COBALT_COLOUR_BAND_BOTTOM = (SDL_Color) { 0x0F, 0x4C, 0xB0, 0xFF };
      COBALT_COLOUR_TILE = (SDL_Color) { 0xFF, 0xFF, 0xFF, 0xFF };
      COBALT_COLOUR_TILE_EDGE = (SDL_Color) { 0xCF, 0xD8, 0xDC, 0xFF };
      COBALT_COLOUR_TEXT = (SDL_Color) { 0x33, 0x3B, 0x40, 0xFF };
      COBALT_COLOUR_TEXT_DIM = (SDL_Color) { 0x56, 0x64, 0x70, 0xFF };
      COBALT_COLOUR_HEADER_TEXT = (SDL_Color) { 0xFF, 0xFF, 0xFF, 0xFF };
      COBALT_COLOUR_ACCENT = (SDL_Color) { 0x1E, 0x6B, 0xE0, 0xFF };
      COBALT_COLOUR_ACCENT_TEXT = (SDL_Color) { 0x0B, 0x4A, 0xA8, 0xFF };
      COBALT_COLOUR_ON_ACCENT = (SDL_Color) { 0xFF, 0xFF, 0xFF, 0xFF };
      COBALT_COLOUR_ERROR = (SDL_Color) { 0xD9, 0x4B, 0x4B, 0xFF };
      COBALT_COLOUR_PILL = (SDL_Color) { 0xFF, 0xFF, 0xFF, 0xE6 };
      COBALT_COLOUR_CHIP = (SDL_Color) { 0xEE, 0xF1, 0xF3, 0xFF };
   }
}

void
cobalt_theme_apply(cobalt_theme theme)
{
   s_theme = theme == COBALT_THEME_DARK ? COBALT_THEME_DARK : COBALT_THEME_LIGHT;
   apply_palette();
}

void
cobalt_theme_set_high_contrast(bool on)
{
   if (s_high_contrast == on) {
      return;
   }
   s_high_contrast = on;
   apply_palette();
}

bool
cobalt_theme_high_contrast(void)
{
   return s_high_contrast;
}

cobalt_theme
cobalt_theme_current(void)
{
   return s_theme;
}

unsigned
cobalt_theme_generation(void)
{
   return s_generation;
}

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
