#include "app/draft_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
put(char *out, size_t cap, size_t *used, const char *s, size_t n)
{
   if (*used + n + 1 > cap) {
      return false;
   }
   memcpy(out + *used, s, n);
   *used += n;
   out[*used] = '\0';
   return true;
}

static bool
put_escaped(char *out, size_t cap, size_t *used, const char *s)
{
   for (const char *p = s; *p; p++) {
      const char *rep = NULL;
      switch (*p) {
         case '\\': rep = "\\\\"; break;
         case '\n': rep = "\\n";  break;
         case '\r': rep = "\\r";  break;
         case '\t': rep = "\\t";  break;
         default:   break;
      }
      if (rep ? !put(out, cap, used, rep, 2) : !put(out, cap, used, p, 1)) {
         return false;
      }
   }
   return true;
}

static bool
unescape(const char *s, size_t n, char *dst, size_t cap)
{
   size_t o = 0;

   for (size_t i = 0; i < n; i++) {
      char c = s[i];
      if (c == '\\') {
         if (++i >= n) {
            return false;
         }
         switch (s[i]) {
            case '\\': c = '\\'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            default:   return false;
         }
      }
      if (o + 1 >= cap) {
         return false;
      }
      dst[o++] = c;
   }
   dst[o] = '\0';
   return true;
}

static bool
put_field(char *out, size_t cap, size_t *used, const char *s)
{
   return put_escaped(out, cap, used, s) && put(out, cap, used, "\t", 1);
}

static bool
put_number(char *out, size_t cap, size_t *used, long long n)
{
   char buf[32];

   snprintf(buf, sizeof(buf), "%lld\t", n);
   return put(out, cap, used, buf, strlen(buf));
}

bool
cobalt_drafts_encode(const cobalt_drafts *drafts, char *out, size_t cap)
{
   size_t used = 0;
   char head[32];

   if (!drafts || !out || cap == 0 || drafts->count > COBALT_DRAFT_MAX) {
      return false;
   }
   snprintf(head, sizeof(head), "cobalt-drafts %d\n", COBALT_DRAFTS_VERSION);
   if (!put(out, cap, &used, head, strlen(head))) {
      return false;
   }
   for (unsigned i = 0; i < drafts->count; i++) {
      const cobalt_draft *d = &drafts->items[i];
      if (!cobalt_draft_valid(d) ||
          !put_number(out, cap, &used, d->saved_at) ||
          !put_number(out, cap, &used, (long long) d->kind) ||
          !put_number(out, cap, &used, (long long) d->reply_gate) ||
          !put_field(out, cap, &used, d->text) ||
          !put_field(out, cap, &used, d->parent_uri) ||
          !put_field(out, cap, &used, d->parent_cid) ||
          !put_field(out, cap, &used, d->root_uri) ||
          !put_field(out, cap, &used, d->root_cid) ||
          !put_field(out, cap, &used, d->reply_to) ||
          !put_field(out, cap, &used, d->quote_uri) ||
          !put_field(out, cap, &used, d->quote_cid) ||
          !put_escaped(out, cap, &used, d->quote_handle) ||
          !put(out, cap, &used, "\n", 1)) {
         return false;
      }
   }
   return put(out, cap, &used, "end\n", 4);
}

/* Split one record into exactly 12 fields. False on too few or too many. */
static bool
split_fields(const char *line, size_t len, const char **field, size_t *field_len)
{
   int n = 0;
   const char *start = line;

   for (size_t i = 0; i <= len; i++) {
      if (i == len || line[i] == '\t') {
         if (n >= 12) {
            return false;
         }
         field[n] = start;
         field_len[n] = (size_t) (&line[i] - start);
         n++;
         start = line + i + 1;
      }
   }
   return n == 12;
}

static bool
parse_number(const char *s, size_t n, long long *out)
{
   char buf[32];
   char *end = NULL;

   if (n == 0 || n >= sizeof(buf)) {
      return false;
   }
   memcpy(buf, s, n);
   buf[n] = '\0';
   *out = strtoll(buf, &end, 10);
   return end && *end == '\0';
}

bool
cobalt_drafts_decode(const char *data, size_t len, cobalt_drafts *drafts)
{
   const char *version = "cobalt-drafts 1\n";
   const size_t version_len = strlen(version);
   const char *p;
   const char *end;

   cobalt_drafts_init(drafts);
   if (!data || !drafts || len < version_len ||
       memcmp(data, version, version_len) != 0) {
      return false;
   }
   p = data + version_len;
   end = data + len;

   while (p < end) {
      const char *nl = memchr(p, '\n', (size_t) (end - p));
      const char *field[12];
      size_t field_len[12];
      long long stamp, kind, gate;
      cobalt_draft *d;

      if (!nl) {
         goto bad;
      }
      if (nl - p == 3 && memcmp(p, "end", 3) == 0) {
         return true;
      }
      if (drafts->count >= COBALT_DRAFT_MAX ||
          !split_fields(p, (size_t) (nl - p), field, field_len) ||
          !parse_number(field[0], field_len[0], &stamp) ||
          !parse_number(field[1], field_len[1], &kind) ||
          !parse_number(field[2], field_len[2], &gate) ||
          kind < COBALT_DRAFT_POST || kind > COBALT_DRAFT_QUOTE) {
         goto bad;
      }

      d = &drafts->items[drafts->count];
      memset(d, 0, sizeof(*d));
      d->saved_at = stamp;
      d->kind = (cobalt_draft_kind) kind;
      d->reply_gate = (int) gate;
      if (!unescape(field[3], field_len[3], d->text, sizeof(d->text)) ||
          !unescape(field[4], field_len[4], d->parent_uri, sizeof(d->parent_uri)) ||
          !unescape(field[5], field_len[5], d->parent_cid, sizeof(d->parent_cid)) ||
          !unescape(field[6], field_len[6], d->root_uri, sizeof(d->root_uri)) ||
          !unescape(field[7], field_len[7], d->root_cid, sizeof(d->root_cid)) ||
          !unescape(field[8], field_len[8], d->reply_to, sizeof(d->reply_to)) ||
          !unescape(field[9], field_len[9], d->quote_uri, sizeof(d->quote_uri)) ||
          !unescape(field[10], field_len[10], d->quote_cid, sizeof(d->quote_cid)) ||
          !unescape(field[11], field_len[11], d->quote_handle, sizeof(d->quote_handle)) ||
          !cobalt_draft_valid(d)) {
         goto bad;
      }
      drafts->count++;
      p = nl + 1;
   }

bad:
   cobalt_drafts_init(drafts);
   return false;
}

bool
cobalt_drafts_save(const char *path, const cobalt_drafts *drafts)
{
   static char buf[COBALT_DRAFTS_FILE_MAX];
   char tmp[512];
   FILE *f;
   size_t n;

   if (!path || !cobalt_drafts_encode(drafts, buf, sizeof(buf))) {
      return false;
   }
   n = strlen(buf);
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
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
   remove(path);
   return rename(tmp, path) == 0;
}

bool
cobalt_drafts_load(const char *path, cobalt_drafts *drafts)
{
   static char buf[COBALT_DRAFTS_FILE_MAX + 1];
   FILE *f;
   size_t n;

   cobalt_drafts_init(drafts);
   if (!path) {
      return false;
   }
   f = fopen(path, "rb");
   if (!f) {
      return true;
   }
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   if (n == sizeof(buf) - 1) {
      return false;
   }
   return cobalt_drafts_decode(buf, n, drafts);
}
