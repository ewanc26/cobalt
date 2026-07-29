#include "ui/keyboard.h"

#include <stdio.h>
#include <string.h>

#define CHAR_ROWS 4
#define KB_ROWS   (CHAR_ROWS + 1)   /* character rows plus the function row */

#define LAYER_LOWER   0
#define LAYER_UPPER   1
#define LAYER_SYMBOLS 2

/*
 * Character rows.
 *
 * Digits sit on the top row of every layer rather than behind the symbol
 * toggle, because the two things people type into this app most are a handle
 * and an app password, and app passwords are hyphen-separated groups of
 * alphanumerics. For the same reason '-', '_' and '@' stay on the letter
 * layers instead of being banished to symbols.
 */
static const char *const LAYERS[3][CHAR_ROWS] = {
   { "1234567890", "qwertyuiop", "asdfghjkl.", "zxcvbnm-_@" },
   { "1234567890", "QWERTYUIOP", "ASDFGHJKL.", "ZXCVBNM-_@" },
   { "1234567890", "!@#$%^&*()", "-_=+[]{};:", "'\"\\|/?<>,~" },
};

typedef enum {
   K_CHAR = 0,
   K_SHIFT,
   K_LAYER,
   K_SPACE,
   K_BACKSPACE,
   K_OK,
   K_CANCEL,
} key_kind;

/*
 * The label is a small inline array rather than a pointer so a key_def stays
 * valid after being returned by value — a character key's label is built on
 * the fly and would otherwise have to live in a shared static buffer.
 */
typedef struct {
   char label[8];
   key_kind kind;
   int span;         /* width in grid units */
   char text;        /* K_CHAR only */
} key_def;

/*
 * The function row. Spans are grid units within the row, so the wide space bar
 * stays wide at both surface sizes without a second layout table.
 */
static const key_def FUNCTION_ROW[] = {
   { "Shift",  K_SHIFT,     2, 0 },
   { "#+=",    K_LAYER,     2, 0 },
   { "Space",  K_SPACE,     6, 0 },
   { "Del",    K_BACKSPACE, 2, 0 },
   { "OK",     K_OK,        2, 0 },
   { "Cancel", K_CANCEL,    3, 0 },
};

#define FUNCTION_KEYS ((int) (sizeof(FUNCTION_ROW) / sizeof(FUNCTION_ROW[0])))

static int
row_length(int layer, int row)
{
   if (row < CHAR_ROWS) {
      return (int) strlen(LAYERS[layer][row]);
   }
   return FUNCTION_KEYS;
}

/* Row `row`, column `col` of the current layer, resolved into a key. */
static key_def
key_at(int layer, int row, int col)
{
   if (row < CHAR_ROWS) {
      key_def k;
      memset(&k, 0, sizeof(k));
      k.label[0] = LAYERS[layer][row][col];
      k.kind = K_CHAR;
      k.span = 1;
      k.text = k.label[0];
      return k;
   }

   key_def k = FUNCTION_ROW[col];
   /* The two mode keys name where they will take you, not where you are. */
   if (k.kind == K_LAYER) {
      snprintf(k.label, sizeof(k.label), "%s",
               (layer == LAYER_SYMBOLS) ? "ABC" : "#+=");
   } else if (k.kind == K_SHIFT) {
      snprintf(k.label, sizeof(k.label), "%s",
               (layer == LAYER_UPPER) ? "abc" : "ABC");
   }
   return k;
}

static int
row_span_total(int layer, int row)
{
   if (row < CHAR_ROWS) {
      return row_length(layer, row);
   }
   int total = 0;
   for (int i = 0; i < FUNCTION_KEYS; i++) {
      total += FUNCTION_ROW[i].span;
   }
   return total;
}

/* --- geometry --- */

static int
key_height(const cobalt_metrics *m)
{
   /* Two lines of body text tall: comfortably above a fingertip on the
    * GamePad's 854x480 panel, and still readable across a room on the TV. */
   return m->font_body * 2;
}

static int
key_gap(const cobalt_metrics *m)
{
   int gap = m->gap / 2;
   return gap < 4 ? 4 : gap;
}

int
cobalt_keyboard_height(cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_metrics_for(surface);
   return KB_ROWS * key_height(m) + (KB_ROWS - 1) * key_gap(m);
}

/* --- editing --- */

static size_t
buffer_length(const cobalt_keyboard *kb)
{
   return (kb->buffer && kb->capacity > 0) ? strlen(kb->buffer) : 0;
}

static void
append_char(cobalt_keyboard *kb, char c)
{
   size_t len = buffer_length(kb);
   if (len + 1 >= kb->capacity) {
      return; /* Full. Silently ignoring is better than truncating elsewhere. */
   }
   kb->buffer[len] = c;
   kb->buffer[len + 1] = '\0';
}

static void
backspace(cobalt_keyboard *kb)
{
   size_t len = buffer_length(kb);
   if (len == 0) {
      return;
   }

   /* Every key on this keyboard is ASCII, but the buffer can also be seeded
    * from stored text, so step back over a whole UTF-8 sequence rather than
    * one byte and risk leaving a dangling continuation byte behind. */
   size_t i = len - 1;
   while (i > 0 && ((unsigned char) kb->buffer[i] & 0xC0) == 0x80) {
      i--;
   }
   kb->buffer[i] = '\0';
}

/* --- lifecycle --- */

void
cobalt_keyboard_open(cobalt_keyboard *kb, char *buffer, size_t capacity, bool masked)
{
   if (!kb) {
      return;
   }

   memset(kb, 0, sizeof(*kb));
   kb->buffer = buffer;
   kb->capacity = capacity;
   kb->masked = masked;
   kb->layer = LAYER_LOWER;
   /* Open on the home row rather than a corner: fewer presses to anywhere. */
   kb->row = 2;
   kb->col = 0;

   if (buffer && capacity > 0) {
      buffer[capacity - 1] = '\0';
   }
}

/* --- input --- */

static cobalt_kb_result
press(cobalt_keyboard *kb, key_def key)
{
   switch (key.kind) {
      case K_CHAR:
         append_char(kb, key.text);
         break;

      case K_SHIFT:
         /* Sticky, not one-shot. App passwords are all lowercase and handles
          * usually are too, so a caps toggle surprises people less than a
          * shift that silently switches back mid-word. */
         kb->layer = (kb->layer == LAYER_UPPER) ? LAYER_LOWER : LAYER_UPPER;
         break;

      case K_LAYER:
         kb->layer = (kb->layer == LAYER_SYMBOLS) ? LAYER_LOWER : LAYER_SYMBOLS;
         break;

      case K_SPACE:
         append_char(kb, ' ');
         break;

      case K_BACKSPACE:
         backspace(kb);
         break;

      case K_OK:
         return COBALT_KB_ACCEPTED;

      case K_CANCEL:
         return COBALT_KB_CANCELLED;
   }

   /* Switching layer can leave the cursor past the end of the new row. */
   int len = row_length(kb->layer, kb->row);
   if (kb->col >= len) {
      kb->col = len - 1;
   }
   return COBALT_KB_IDLE;
}

cobalt_kb_result
cobalt_keyboard_update(cobalt_keyboard *kb, const cobalt_input *in)
{
   if (!kb || !in || !kb->buffer) {
      return COBALT_KB_IDLE;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
      int len = row_length(kb->layer, kb->row);
      kb->col = (kb->col + len - 1) % len;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
      int len = row_length(kb->layer, kb->row);
      kb->col = (kb->col + 1) % len;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      kb->row = (kb->row + KB_ROWS - 1) % KB_ROWS;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      kb->row = (kb->row + 1) % KB_ROWS;
   }

   /* Rows are not all the same length, so clamp after any vertical move. */
   int len = row_length(kb->layer, kb->row);
   if (kb->col >= len) {
      kb->col = len - 1;
   }
   if (kb->col < 0) {
      kb->col = 0;
   }

   /* B is backspace, which is the console convention and saves walking the
    * focus over to the Del key for every correction. Backing out of the field
    * entirely is the Cancel key. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      backspace(kb);
   }

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      return press(kb, key_at(kb->layer, kb->row, kb->col));
   }

   if (kb->hit_valid && in->touch_ended) {
      for (int i = 0; i < kb->hit_count; i++) {
         if (cobalt_input_tapped(in, &kb->hit[i])) {
            kb->row = kb->hit_row[i];
            kb->col = kb->hit_col[i];
            return press(kb, key_at(kb->layer, kb->row, kb->col));
         }
      }
   }

   return COBALT_KB_IDLE;
}

/* --- drawing --- */

void
cobalt_keyboard_draw(cobalt_keyboard *kb, cobalt_render *r,
                     cobalt_surface_id surface, const SDL_Rect *area)
{
   if (!kb || !r || !area) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int gap = key_gap(m);
   const int kh = key_height(m);
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   if (touchable) {
      kb->hit_count = 0;
   }

   for (int row = 0; row < KB_ROWS; row++) {
      const int count = row_length(kb->layer, row);
      const int units = row_span_total(kb->layer, row);
      const int y = area->y + row * (kh + gap);

      /* Unit width is derived from this row's own span total, so the function
       * row's wide keys line up with the character grid above it. */
      const int span_gaps = gap * (count - 1);
      const int unit_w = (units > 0) ? (area->w - span_gaps) / units : 0;
      if (unit_w <= 0) {
         continue;
      }

      int x = area->x;
      for (int col = 0; col < count; col++) {
         const key_def key = key_at(kb->layer, row, col);
         const int kw = unit_w * key.span;
         SDL_Rect rect = { x, y, kw, kh };

         const bool focused = (row == kb->row && col == kb->col);
         cobalt_draw_tile(r, &rect, focused ? 1.0f : 0.0f);

         SDL_Color colour = focused ? COBALT_COLOUR_ACCENT : COBALT_COLOUR_TEXT;
         const int label_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
         cobalt_draw_text_centred(r, COBALT_FONT_BODY, key.label,
                                  rect.x, rect.y + (kh - label_h) / 2, kw, colour);

         if (touchable && kb->hit_count < COBALT_KB_MAX_KEYS) {
            kb->hit[kb->hit_count] = rect;
            kb->hit_row[kb->hit_count] = (short) row;
            kb->hit_col[kb->hit_count] = (short) col;
            kb->hit_count++;
         }

         x += kw + gap;
      }
   }

   if (touchable) {
      kb->hit_valid = true;
   }
}

void
cobalt_keyboard_display_text(const cobalt_keyboard *kb, char *out, size_t out_size)
{
   if (!out || out_size == 0) {
      return;
   }
   out[0] = '\0';

   if (!kb || !kb->buffer) {
      return;
   }

   /* One byte for the caret, one for the terminator. */
   if (out_size < 3) {
      snprintf(out, out_size, "_");
      return;
   }
   const size_t room = out_size - 2;

   size_t len = strlen(kb->buffer);
   size_t start = 0;

   if (kb->masked) {
      /* Mask by character, not by byte, so a multi-byte password does not draw
       * more dots than it has characters. */
      size_t chars = 0;
      for (size_t i = 0; i < len; i++) {
         if (((unsigned char) kb->buffer[i] & 0xC0) != 0x80) {
            chars++;
         }
      }
      if (chars > room) {
         chars = room;
      }
      memset(out, '*', chars);
      out[chars] = '_';
      out[chars + 1] = '\0';
      return;
   }

   /* Overlong input scrolls: keep the tail, which is where the caret is.
    * Step forward to a codepoint boundary so the window never opens on a
    * continuation byte. */
   if (len > room) {
      start = len - room;
      while (start < len && ((unsigned char) kb->buffer[start] & 0xC0) == 0x80) {
         start++;
      }
   }

   size_t copied = len - start;
   memcpy(out, kb->buffer + start, copied);
   out[copied] = '_';
   out[copied + 1] = '\0';
}
