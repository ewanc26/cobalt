#include "update/selfupdate.h"

#include "update/update_key.h"

#include <stdio.h>
#include <string.h>

const char *
cobalt_update_status_string(cobalt_update_status st)
{
   switch (st) {
      case COBALT_UPDATE_OK:       return "ok";
      case COBALT_UPDATE_BAD_MANIFEST: return "the update manifest was refused";
      case COBALT_UPDATE_BAD_POLICY:  return "the update points outside Cobalt's releases";
      case COBALT_UPDATE_BAD_SIGNATURE: return "the update is not signed by Cobalt's release key";
   }
   return "unknown";
}

bool
cobalt_update_public_key(unsigned char out[WF_UPDATE_PUBLIC_KEY_LEN])
{
   /* 64 hex characters decode to 32 bytes: the same shape as a SHA-256. */
   return strlen(COBALT_UPDATE_PUBLIC_KEY_HEX) == 64 &&
          wf_sha256_from_hex(COBALT_UPDATE_PUBLIC_KEY_HEX, 64, out) == WF_OK;
}

cobalt_update_status
cobalt_update_verify_manifest(const char *body, size_t len, const char *sig, size_t sig_len,
                              const unsigned char pk[WF_UPDATE_PUBLIC_KEY_LEN])
{
   if (!body || len == 0 || len > COBALT_UPDATE_MANIFEST_MAX || !sig || !pk) {
      return COBALT_UPDATE_BAD_SIGNATURE;
   }
   return wf_update_verify_signature(body, len, sig, sig_len, pk) == WF_OK
             ? COBALT_UPDATE_OK
             : COBALT_UPDATE_BAD_SIGNATURE;
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
                             wf_update_manifest *out)
{
   wf_update_policy policy;
   char want[WF_UPDATE_URL_MAX];

   if (!body || len == 0 || len > COBALT_UPDATE_MANIFEST_MAX) {
      memset(out, 0, sizeof(*out));
      return COBALT_UPDATE_BAD_MANIFEST;
   }
   memset(&policy, 0, sizeof(policy));
   policy.max_size = max_size;
   policy.app = "cobalt";
   policy.url_prefix = COBALT_UPDATE_ASSET_PREFIX;
   const wf_status st = wf_update_parse_manifest(body, len, &policy, out);
   if (st == WF_ERR_VALIDATION) {
      memset(out, 0, sizeof(*out));
      return COBALT_UPDATE_BAD_POLICY;
   }
   if (st != WF_OK) {
      return COBALT_UPDATE_BAD_MANIFEST;
   }
   /* Wolfram checks the repository prefix; Cobalt also pins the layout its release
    * script produces, v<version>/<asset name>, and a plain asset name. */
   const int n = snprintf(want, sizeof(want), "%sv%s/%s", COBALT_UPDATE_ASSET_PREFIX,
                          out->version, out->asset.name);
   if (!name_is_safe(out->asset.name) || n < 0 || (size_t) n >= sizeof(want) ||
       strcmp(want, out->asset.url) != 0) {
      memset(out, 0, sizeof(*out));
      return COBALT_UPDATE_BAD_POLICY;
   }
   return COBALT_UPDATE_OK;
}

bool
cobalt_sha256_file(const char *path, unsigned char out[32])
{
   FILE *f = fopen(path, "rb");
   unsigned char buf[4096];
   wf_sha256 c;
   size_t n;

   if (!f) {
      return false;
   }
   wf_sha256_init(&c);
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      wf_sha256_update(&c, buf, n);
   }
   const bool failed = ferror(f) != 0;
   fclose(f);
   if (failed) {
      return false;
   }
   wf_sha256_final(&c, out);
   return true;
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

   /* Check the bytes in memory first, so a bad download never reaches the card. */
   wf_sha256_buffer(data, size, got);
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
