#pragma once

/*
 * The small set of preferences that outlive a run: the light/dark theme, the
 * post-text scale and the high-contrast accessibility override.
 *
 * Format v1: a version line, one `key=value` line per setting, then an "end"
 * line. A missing file means defaults; a damaged one is refused whole rather
 * than half-loaded, and the app carries on with the defaults. New keys are
 * optional, so a file written before they existed still loads.
 */

#include "ui/theme.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_SETTINGS_VERSION 1
#define COBALT_SETTINGS_FILE_MAX 256

typedef struct {
   cobalt_theme theme;
   cobalt_text_scale text_scale;
   bool high_contrast;
} cobalt_settings;

void cobalt_settings_defaults(cobalt_settings *s);

/* Force every field back into a value the app can apply. Called after
 * decoding and before encoding, so a damaged file or a caller's out-of-range
 * struct cannot reach the palette or the renderer. */
void cobalt_settings_clamp(cobalt_settings *s);

/* Encode into `out` (NUL-terminated). False if it does not fit `cap`. */
bool cobalt_settings_encode(const cobalt_settings *s, char *out, size_t cap);

/* Decode `data`. False, with `s` at its defaults, for anything that is not a
 * whole, well-formed settings file. */
bool cobalt_settings_decode(const char *data, size_t len, cobalt_settings *s);

/* Write atomically through a temporary file. False on I/O failure. */
bool cobalt_settings_save(const char *path, const cobalt_settings *s);

/* Load `path`. A missing file is defaults and returns true; a file that
 * exists but does not decode returns false and leaves `s` at its defaults. */
bool cobalt_settings_load(const char *path, cobalt_settings *s);

#ifdef __cplusplus
}
#endif
