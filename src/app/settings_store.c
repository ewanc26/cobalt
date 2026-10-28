#include "app/settings_store.h"

#include <stdio.h>
#include <string.h>

static const char *const HEADER = "cobalt-settings 1\n";

void
cobalt_settings_defaults(cobalt_settings *s)
{
   if (s) {
      s->theme = COBALT_THEME_LIGHT;
      s->text_scale = COBALT_TEXT_SCALE_NORMAL;
      s->high_contrast = false;
   }
}

void
cobalt_settings_clamp(cobalt_settings *s)
{
   if (!s) {
      return;
   }
   if (s->theme != COBALT_THEME_LIGHT && s->theme != COBALT_THEME_DARK) {
      s->theme = COBALT_THEME_LIGHT;
   }
   switch (s->text_scale) {
      case COBALT_TEXT_SCALE_SMALL:
      case COBALT_TEXT_SCALE_NORMAL:
      case COBALT_TEXT_SCALE_LARGE:
         break;
      default:
         s->text_scale = COBALT_TEXT_SCALE_NORMAL;
         break;
   }
}

bool
cobalt_settings_encode(const cobalt_settings *s, char *out, size_t cap)
{
   cobalt_settings c;
   const char *theme;
   int n;

   if (!s || !out || cap == 0) {
      return false;
   }
   /* Encode what the app can actually apply, not whatever a caller left in
    * the struct, so the file this writes is one the decoder accepts. */
   c = *s;
   cobalt_settings_clamp(&c);
   theme = c.theme == COBALT_THEME_DARK ? "dark" : "light";
   n = snprintf(out, cap, "%stheme=%s\ntext_scale=%d\nhigh_contrast=%d\nend\n",
                HEADER, theme, (int) c.text_scale, c.high_contrast ? 1 : 0);
   return n > 0 && (size_t) n < cap;
}

/* Decimal digits only: no sign, spaces or trailing junk. */
static bool
parse_uint(const char *v, size_t n, int *out)
{
   int acc = 0;

   if (n == 0 || n > 3) {
      return false;
   }
   for (size_t i = 0; i < n; i++) {
      if (v[i] < '0' || v[i] > '9') {
         return false;
      }
      acc = acc * 10 + (v[i] - '0');
   }
   *out = acc;
   return true;
}

static bool
parse_bool(const char *v, size_t n, bool *out)
{
   if (n != 1 || (v[0] != '0' && v[0] != '1')) {
      return false;
   }
   *out = v[0] == '1';
   return true;
}

/* Apply one `key=value` line. Unknown keys and unusable values are skipped so
 * a file from a newer Cobalt still loads the settings this build understands. */
static void
apply_line(cobalt_settings *s, const char *line, size_t len)
{
   static const char THEME[] = "theme=";
   static const char TEXT_SCALE[] = "text_scale=";
   static const char HIGH_CONTRAST[] = "high_contrast=";
   const char *value;
   size_t value_len;
   int num = 0;
   bool flag = false;

   if (len >= sizeof(THEME) - 1 && memcmp(line, THEME, sizeof(THEME) - 1) == 0) {
      value = line + sizeof(THEME) - 1;
      value_len = len - (sizeof(THEME) - 1);
      if (value_len == 4 && memcmp(value, "dark", 4) == 0) {
         s->theme = COBALT_THEME_DARK;
      } else if (value_len == 5 && memcmp(value, "light", 5) == 0) {
         s->theme = COBALT_THEME_LIGHT;
      }
      return;
   }
   if (len >= sizeof(TEXT_SCALE) - 1 &&
       memcmp(line, TEXT_SCALE, sizeof(TEXT_SCALE) - 1) == 0) {
      value = line + sizeof(TEXT_SCALE) - 1;
      value_len = len - (sizeof(TEXT_SCALE) - 1);
      if (parse_uint(value, value_len, &num) &&
          (num == (int) COBALT_TEXT_SCALE_SMALL ||
           num == (int) COBALT_TEXT_SCALE_NORMAL ||
           num == (int) COBALT_TEXT_SCALE_LARGE)) {
         s->text_scale = (cobalt_text_scale) num;
      }
      return;
   }
   if (len >= sizeof(HIGH_CONTRAST) - 1 &&
       memcmp(line, HIGH_CONTRAST, sizeof(HIGH_CONTRAST) - 1) == 0) {
      value = line + sizeof(HIGH_CONTRAST) - 1;
      value_len = len - (sizeof(HIGH_CONTRAST) - 1);
      if (parse_bool(value, value_len, &flag)) {
         s->high_contrast = flag;
      }
   }
}

bool
cobalt_settings_decode(const char *data, size_t len, cobalt_settings *s)
{
   cobalt_settings parsed;
   size_t header_len = strlen(HEADER);
   const char *p;
   const char *end;

   if (!s) {
      return false;
   }
   cobalt_settings_defaults(s);
   if (!data || len < header_len || memcmp(data, HEADER, header_len) != 0) {
      return false;
   }
   cobalt_settings_defaults(&parsed);
   p = data + header_len;
   end = data + len;

   while (p < end) {
      const char *nl = memchr(p, '\n', (size_t) (end - p));
      size_t line_len;

      if (!nl) {
         /* A truncated final line is not a line. */
         return false;
      }
      line_len = (size_t) (nl - p);
      if (line_len == 3 && memcmp(p, "end", 3) == 0) {
         /* The marker must be the last line; trailing bytes are not silently
          * ignored, because that is how a half-written append hides. */
         if (nl + 1 != end) {
            return false;
         }
         cobalt_settings_clamp(&parsed);
         *s = parsed;
         return true;
      }
      apply_line(&parsed, p, line_len);
      p = nl + 1;
   }
   return false;
}

bool
cobalt_settings_save(const char *path, const cobalt_settings *s)
{
   char buf[COBALT_SETTINGS_FILE_MAX];
   char tmp[512];
   FILE *f;
   size_t n;

   if (!path || !cobalt_settings_encode(s, buf, sizeof(buf))) {
      return false;
   }
   n = strlen(buf);
   if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) {
      return false;
   }
   f = fopen(tmp, "wb");
   if (!f) {
      return false;
   }
   if (fwrite(buf, 1, n, f) != n) {
      fclose(f);
      remove(tmp);
      return false;
   }
   if (fclose(f) != 0) {
      remove(tmp);
      return false;
   }
   /* Some filesystems will not rename over an existing file, so remove the old
    * one first. The temporary copy is what survives if the rename fails. */
   remove(path);
   return rename(tmp, path) == 0;
}

bool
cobalt_settings_load(const char *path, cobalt_settings *s)
{
   char buf[COBALT_SETTINGS_FILE_MAX + 1];
   FILE *f;
   size_t n;

   if (!s) {
      return false;
   }
   cobalt_settings_defaults(s);
   if (!path) {
      return false;
   }
   f = fopen(path, "rb");
   if (!f) {
      return true; /* no file yet: defaults */
   }
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   if (n == sizeof(buf) - 1) {
      return false; /* longer than any valid settings file */
   }
   buf[n] = '\0';
   return cobalt_settings_decode(buf, n, s);
}
