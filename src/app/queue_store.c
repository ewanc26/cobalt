#include "app/queue_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Worst case per post: a 1024-byte text escaped to 2048 bytes, a timestamp, a
 * tab and a newline, plus the header and the end line. */
#define QUEUE_FILE_MAX (COBALT_POST_QUEUE_MAX * (2 * COBALT_POST_QUEUE_TEXT_MAX + 64) + 64)

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

/* Copy the escaped text in [s, s + n) into dst, undoing put_escaped. */
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

bool
cobalt_queue_encode(const cobalt_post_queue *q, char *out, size_t cap)
{
   size_t used = 0;
   char head[32];

   if (cap == 0 || q->count > COBALT_POST_QUEUE_MAX) {
      return false;
   }
   snprintf(head, sizeof(head), "cobalt-queue %d\n", COBALT_QUEUE_VERSION);
   if (!put(out, cap, &used, head, strlen(head))) {
      return false;
   }
   for (unsigned i = 0; i < q->count; i++) {
      char stamp[32];
      snprintf(stamp, sizeof(stamp), "%lld\t", q->items[i].queued_at);
      if (!put(out, cap, &used, stamp, strlen(stamp)) ||
          !put_escaped(out, cap, &used, q->items[i].text) ||
          !put(out, cap, &used, "\n", 1)) {
         return false;
      }
   }
   return put(out, cap, &used, "end\n", 4);
}

bool
cobalt_queue_decode(const char *data, size_t len, cobalt_post_queue *q)
{
   const char *p = data;
   const char *end = data + len;
   char version[32];

   cobalt_post_queue_init(q);
   snprintf(version, sizeof(version), "cobalt-queue %d\n", COBALT_QUEUE_VERSION);
   if (len < strlen(version) || memcmp(p, version, strlen(version)) != 0) {
      return false;
   }
   p += strlen(version);

   while (p < end) {
      const char *nl = memchr(p, '\n', (size_t)(end - p));
      if (!nl) {
         goto bad;  /* a truncated final line is not a line */
      }
      if (nl - p == 3 && memcmp(p, "end", 3) == 0) {
         return true;
      }
      {
         const char *tab = memchr(p, '\t', (size_t)(nl - p));
         char *stop = NULL;
         long long when;

         if (!tab || q->count >= COBALT_POST_QUEUE_MAX) {
            goto bad;
         }
         when = strtoll(p, &stop, 10);
         if (stop != tab) {
            goto bad;
         }
         if (!unescape(tab + 1, (size_t)(nl - tab - 1), q->items[q->count].text,
                       sizeof(q->items[q->count].text)) ||
             q->items[q->count].text[0] == '\0') {
            goto bad;
         }
         q->items[q->count].queued_at = when;
         q->count++;
      }
      p = nl + 1;
   }
bad:
   cobalt_post_queue_init(q);
   return false;
}

bool
cobalt_queue_save(const char *path, const cobalt_post_queue *q)
{
   static char buf[QUEUE_FILE_MAX];
   char tmp[512];
   FILE *f;
   size_t n;

   if (!cobalt_queue_encode(q, buf, sizeof(buf))) {
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
   /* Some filesystems will not rename over an existing file, so remove the old
    * one first. The temporary copy is what survives if the rename fails. */
   remove(path);
   if (rename(tmp, path) != 0) {
      return false;
   }
   return true;
}

bool
cobalt_queue_load(const char *path, cobalt_post_queue *q)
{
   static char buf[QUEUE_FILE_MAX + 1];
   FILE *f = fopen(path, "rb");
   size_t n;

   cobalt_post_queue_init(q);
   if (!f) {
      return true;  /* no file yet: nothing is queued */
   }
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   if (n == sizeof(buf) - 1) {
      return false;  /* longer than any valid queue */
   }
   return cobalt_queue_decode(buf, n, q);
}
