#include "update/selfupdate.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *
cobalt_update_status_string(cobalt_update_status st)
{
   switch (st) {
      case COBALT_UPDATE_OK:         return "ok";
      case COBALT_UPDATE_BAD_JSON:   return "the update manifest is not valid JSON";
      case COBALT_UPDATE_BAD_SCHEMA: return "the update manifest is a format this Cobalt does not know";
      case COBALT_UPDATE_BAD_FIELD:  return "the update manifest has a missing or oversized field";
      case COBALT_UPDATE_BAD_URL:    return "the update points outside Cobalt's releases";
      case COBALT_UPDATE_BAD_VERSION: return "the update has a malformed version";
      case COBALT_UPDATE_BAD_SIZE:   return "the update has an unusable size";
      case COBALT_UPDATE_BAD_APP:    return "the update is for another application";
   }
   return "unknown";
}

/* --- versions ----------------------------------------------------------- */

typedef struct {
   unsigned long major, minor, patch;
   char pre[32]; /* empty when this is a release */
} semver;

static bool
parse_num(const char **p, unsigned long *out)
{
   const char *s = *p;
   unsigned long v = 0;
   size_t n = 0;

   while (*s >= '0' && *s <= '9') {
      v = v * 10 + (unsigned long) (*s - '0');
      s++;
      if (++n > 9) {
         return false;
      }
   }
   /* No empty numbers, and no leading zeros (semver). */
   if (n == 0 || (n > 1 && **p == '0')) {
      return false;
   }
   *out = v;
   *p = s;
   return true;
}

static bool
parse_semver(const char *s, semver *v)
{
   memset(v, 0, sizeof(*v));
   if (!s) {
      return false;
   }
   if (!parse_num(&s, &v->major) || *s++ != '.' || !parse_num(&s, &v->minor) ||
       *s++ != '.' || !parse_num(&s, &v->patch)) {
      return false;
   }
   if (*s == '-') {
      s++;
      size_t n = strlen(s);
      if (n == 0 || n >= sizeof(v->pre)) {
         return false;
      }
      for (size_t i = 0; i < n; i++) {
         const char c = s[i];
         if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) {
            return false;
         }
      }
      memcpy(v->pre, s, n + 1);
      return true;
   }
   return *s == '\0';
}

static int
cmp_ul(unsigned long a, unsigned long b)
{
   return a < b ? -1 : a > b ? 1 : 0;
}

static bool
all_digits(const char *s, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      if (s[i] < '0' || s[i] > '9') {
         return false;
      }
   }
   return n > 0;
}

/* Pre-release identifiers, dot by dot: numeric compare numerically and sort
 * below alphanumeric; a shorter list sorts below a longer one it prefixes. */
static int
cmp_pre(const char *a, const char *b)
{
   for (;;) {
      const char *ea = strchr(a, '.');
      const char *eb = strchr(b, '.');
      size_t la = ea ? (size_t) (ea - a) : strlen(a);
      size_t lb = eb ? (size_t) (eb - b) : strlen(b);
      const bool na = all_digits(a, la);
      const bool nb = all_digits(b, lb);
      int c;

      if (na && nb) {
         if (la != lb) {
            c = la < lb ? -1 : 1; /* no leading zeros, so longer is larger */
         } else {
            c = strncmp(a, b, la);
         }
      } else if (na != nb) {
         c = na ? -1 : 1;
      } else {
         size_t m = la < lb ? la : lb;
         c = strncmp(a, b, m);
         if (c == 0 && la != lb) {
            c = la < lb ? -1 : 1;
         }
      }
      if (c != 0) {
         return c < 0 ? -1 : 1;
      }
      if (!ea || !eb) {
         return (!ea && !eb) ? 0 : (!ea ? -1 : 1);
      }
      a = ea + 1;
      b = eb + 1;
   }
}

int
cobalt_update_compare_versions(const char *a, const char *b, bool *ok)
{
   semver x, y;

   if (ok) {
      *ok = true;
   }
   if (!parse_semver(a, &x) || !parse_semver(b, &y)) {
      if (ok) {
         *ok = false;
      }
      return 0;
   }
   int c;
   if ((c = cmp_ul(x.major, y.major)) != 0 || (c = cmp_ul(x.minor, y.minor)) != 0 ||
       (c = cmp_ul(x.patch, y.patch)) != 0) {
      return c;
   }
   if (!x.pre[0] && !y.pre[0]) {
      return 0;
   }
   if (!x.pre[0]) {
      return 1; /* a release outranks its own pre-releases */
   }
   if (!y.pre[0]) {
      return -1;
   }
   return cmp_pre(x.pre, y.pre);
}

/* --- SHA-256 (FIPS 180-4) ----------------------------------------------- */

static const uint32_t K[64] = {
   0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
   0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
   0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
   0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
   0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
   0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
   0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
   0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
   0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
   0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha_block(cobalt_sha256 *c, const unsigned char *p)
{
   uint32_t w[64];
   uint32_t a, b, cc, d, e, f, g, h;

   for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t) p[i * 4] << 24) | ((uint32_t) p[i * 4 + 1] << 16) |
             ((uint32_t) p[i * 4 + 2] << 8) | (uint32_t) p[i * 4 + 3];
   }
   for (int i = 16; i < 64; i++) {
      uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
   }
   a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
   e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
   for (int i = 0; i < 64; i++) {
      uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + K[i] + w[i];
      uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
      uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
      uint32_t t2 = S0 + maj;
      h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
   }
   c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
   c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void
cobalt_sha256_init(cobalt_sha256 *c)
{
   static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
   memcpy(c->state, init, sizeof(init));
   c->bits = 0;
   c->used = 0;
}

void
cobalt_sha256_update(cobalt_sha256 *c, const void *data, size_t len)
{
   const unsigned char *p = data;

   c->bits += (uint64_t) len * 8;
   while (len > 0) {
      size_t n = 64 - c->used;
      if (n > len) {
         n = len;
      }
      memcpy(c->buf + c->used, p, n);
      c->used += n;
      p += n;
      len -= n;
      if (c->used == 64) {
         sha_block(c, c->buf);
         c->used = 0;
      }
   }
}

void
cobalt_sha256_final(cobalt_sha256 *c, unsigned char out[32])
{
   const uint64_t bits = c->bits;
   unsigned char pad = 0x80;
   unsigned char zero = 0;
   unsigned char len[8];

   cobalt_sha256_update(c, &pad, 1);
   while (c->used != 56) {
      cobalt_sha256_update(c, &zero, 1);
   }
   for (int i = 0; i < 8; i++) {
      len[i] = (unsigned char) (bits >> (56 - 8 * i));
   }
   cobalt_sha256_update(c, len, 8);
   for (int i = 0; i < 8; i++) {
      out[i * 4] = (unsigned char) (c->state[i] >> 24);
      out[i * 4 + 1] = (unsigned char) (c->state[i] >> 16);
      out[i * 4 + 2] = (unsigned char) (c->state[i] >> 8);
      out[i * 4 + 3] = (unsigned char) c->state[i];
   }
}

bool
cobalt_sha256_equal(const unsigned char a[32], const unsigned char b[32])
{
   unsigned char diff = 0;

   for (int i = 0; i < 32; i++) {
      diff |= (unsigned char) (a[i] ^ b[i]);
   }
   return diff == 0;
}

static int
hexval(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

bool
cobalt_sha256_from_hex(const char *hex, unsigned char out[32])
{
   if (!hex || strlen(hex) != 64) {
      return false;
   }
   for (int i = 0; i < 32; i++) {
      int hi = hexval(hex[i * 2]);
      int lo = hexval(hex[i * 2 + 1]);
      if (hi < 0 || lo < 0) {
         return false;
      }
      out[i] = (unsigned char) (hi * 16 + lo);
   }
   return true;
}

bool
cobalt_sha256_file(const char *path, unsigned char out[32])
{
   FILE *f = fopen(path, "rb");
   unsigned char buf[4096];
   cobalt_sha256 c;
   size_t n;

   if (!f) {
      return false;
   }
   cobalt_sha256_init(&c);
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      cobalt_sha256_update(&c, buf, n);
   }
   const bool failed = ferror(f) != 0;
   fclose(f);
   if (failed) {
      return false;
   }
   cobalt_sha256_final(&c, out);
   return true;
}

/* --- manifest ----------------------------------------------------------- */

static bool
copy_string(const cJSON *item, char *dst, size_t cap)
{
   if (!cJSON_IsString(item) || !item->valuestring) {
      return false;
   }
   const size_t n = strlen(item->valuestring);
   if (n == 0 || n >= cap) {
      return false;
   }
   memcpy(dst, item->valuestring, n + 1);
   return true;
}

static bool
name_is_safe(const char *s)
{
   /* The name becomes part of a URL check and is shown on screen; keep it plain. */
   for (; *s; s++) {
      const char c = *s;
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '.' || c == '-' || c == '_')) {
         return false;
      }
   }
   return true;
}

cobalt_update_status
cobalt_update_parse_manifest(const char *body, size_t len, unsigned long max_size,
                             cobalt_update_manifest *out)
{
   cobalt_update_status st = COBALT_UPDATE_OK;
   cJSON *root;
   const cJSON *schema, *app, *asset, *size, *sha;
   char hex[80];

   memset(out, 0, sizeof(*out));
   if (!body || len == 0 || len > COBALT_UPDATE_MANIFEST_MAX) {
      return COBALT_UPDATE_BAD_JSON;
   }
   root = cJSON_ParseWithLength(body, len);
   if (!root || !cJSON_IsObject(root)) {
      cJSON_Delete(root);
      return COBALT_UPDATE_BAD_JSON;
   }

   schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
   if (!cJSON_IsNumber(schema) || schema->valuedouble != 1.0) {
      st = COBALT_UPDATE_BAD_SCHEMA;
      goto done;
   }
   app = cJSON_GetObjectItemCaseSensitive(root, "app");
   if (!cJSON_IsString(app) || !app->valuestring || strcmp(app->valuestring, "cobalt") != 0) {
      st = COBALT_UPDATE_BAD_APP;
      goto done;
   }
   if (!copy_string(cJSON_GetObjectItemCaseSensitive(root, "version"), out->version,
                    sizeof(out->version))) {
      st = COBALT_UPDATE_BAD_FIELD;
      goto done;
   }
   {
      bool ok;
      cobalt_update_compare_versions(out->version, "0.0.0", &ok);
      if (!ok) {
         st = COBALT_UPDATE_BAD_VERSION;
         goto done;
      }
   }
   /* Notes are optional, but if present must fit. They are only ever displayed. */
   {
      const cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");
      if (notes && !cJSON_IsNull(notes) && !copy_string(notes, out->notes, sizeof(out->notes))) {
         st = COBALT_UPDATE_BAD_FIELD;
         goto done;
      }
   }
   asset = cJSON_GetObjectItemCaseSensitive(root, "asset");
   if (!cJSON_IsObject(asset) ||
       !copy_string(cJSON_GetObjectItemCaseSensitive(asset, "name"), out->name,
                    sizeof(out->name)) ||
       !name_is_safe(out->name) ||
       !copy_string(cJSON_GetObjectItemCaseSensitive(asset, "url"), out->url, sizeof(out->url))) {
      st = COBALT_UPDATE_BAD_FIELD;
      goto done;
   }
   {
      char want[sizeof(out->url)];
      const int n = snprintf(want, sizeof(want), "%sv%s/%s", COBALT_UPDATE_ASSET_PREFIX,
                             out->version, out->name);
      if (n < 0 || (size_t) n >= sizeof(want) || strcmp(want, out->url) != 0) {
         st = COBALT_UPDATE_BAD_URL;
         goto done;
      }
   }
   size = cJSON_GetObjectItemCaseSensitive(asset, "size");
   if (!cJSON_IsNumber(size) || size->valuedouble < 1.0 ||
       size->valuedouble > (double) max_size) {
      st = COBALT_UPDATE_BAD_SIZE;
      goto done;
   }
   out->size = (unsigned long) size->valuedouble;
   sha = cJSON_GetObjectItemCaseSensitive(asset, "sha256");
   if (!copy_string(sha, hex, sizeof(hex)) || !cobalt_sha256_from_hex(hex, out->sha256)) {
      st = COBALT_UPDATE_BAD_FIELD;
      goto done;
   }

done:
   cJSON_Delete(root);
   if (st != COBALT_UPDATE_OK) {
      memset(out, 0, sizeof(*out));
   }
   return st;
}

/* --- staging ------------------------------------------------------------ */

static bool
join(char *dst, const char *a, const char *b)
{
   const int n = snprintf(dst, COBALT_UPDATE_PATH_MAX, "%s%s", a, b);
   return n > 0 && n < COBALT_UPDATE_PATH_MAX;
}

bool
cobalt_update_paths_init(cobalt_update_paths *p, const char *installed, const char *stage_dir)
{
   char base[COBALT_UPDATE_PATH_MAX];
   const char *slash;

   memset(p, 0, sizeof(*p));
   if (!installed || !installed[0] || !stage_dir || !stage_dir[0]) {
      return false;
   }
   slash = strrchr(installed, '/');
   const int n = snprintf(base, sizeof(base), "%s/%s", stage_dir, slash ? slash + 1 : installed);
   if (n <= 0 || (size_t) n >= sizeof(base) || !join(p->installed, installed, "") ||
       !join(p->part, base, ".part") || !join(p->staged, base, ".new") ||
       !join(p->old, base, ".old")) {
      memset(p, 0, sizeof(*p));
      return false;
   }
   return true;
}

static bool
exists(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (f) {
      fclose(f);
      return true;
   }
   return false;
}

cobalt_stage_result
cobalt_update_stage(const cobalt_update_paths *p, const void *data, size_t size,
                    const unsigned char expect[32])
{
   unsigned char got[32];
   cobalt_sha256 c;

   /* Check the bytes in memory first, so a bad download never reaches the card. */
   cobalt_sha256_init(&c);
   cobalt_sha256_update(&c, data, size);
   cobalt_sha256_final(&c, got);
   if (!cobalt_sha256_equal(got, expect)) {
      return COBALT_STAGE_MISMATCH;
   }

   FILE *f = fopen(p->part, "wb");
   if (!f) {
      return COBALT_STAGE_WRITE_FAILED;
   }
   const bool wrote = fwrite(data, 1, size, f) == size;
   const bool closed = fclose(f) == 0;
   if (!wrote || !closed) {
      remove(p->part);
      return COBALT_STAGE_WRITE_FAILED;
   }
   /* And again from the card: a short or corrupt write shows up here. */
   if (!cobalt_sha256_file(p->part, got) || !cobalt_sha256_equal(got, expect)) {
      remove(p->part);
      return COBALT_STAGE_MISMATCH;
   }
   remove(p->staged);
   if (rename(p->part, p->staged) != 0) {
      remove(p->part);
      return COBALT_STAGE_RENAME_FAILED;
   }
   return COBALT_STAGE_OK;
}

bool
cobalt_update_has_staged(const cobalt_update_paths *p)
{
   return exists(p->staged);
}

bool
cobalt_update_apply(const cobalt_update_paths *p)
{
   if (!exists(p->staged) || !exists(p->installed)) {
      return false;
   }
   remove(p->old);
   if (rename(p->installed, p->old) != 0) {
      return false;
   }
   if (rename(p->staged, p->installed) != 0) {
      /* Put the working build back; the staged file stays for another try. */
      rename(p->old, p->installed);
      return false;
   }
   return true;
}

cobalt_recover_result
cobalt_update_recover(const cobalt_update_paths *p)
{
   cobalt_recover_result r = COBALT_RECOVER_NOTHING;

   if (!exists(p->installed) && exists(p->old)) {
      if (rename(p->old, p->installed) == 0) {
         r = COBALT_RECOVER_RESTORED;
      }
   }
   /* A .part was never trusted. A .new that was not applied is an update that
    * was interrupted; re-downloading it is cheap and re-verifying it is not
    * needed. */
   if (exists(p->part)) {
      remove(p->part);
      if (r == COBALT_RECOVER_NOTHING) r = COBALT_RECOVER_CLEANED;
   }
   if (exists(p->staged)) {
      remove(p->staged);
      if (r == COBALT_RECOVER_NOTHING) r = COBALT_RECOVER_CLEANED;
   }
   return r;
}

bool
cobalt_update_commit(const cobalt_update_paths *p)
{
   if (!exists(p->old)) {
      return false;
   }
   return remove(p->old) == 0;
}
