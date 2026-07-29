#include "cache/session_store.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/rng.h"

#include <mbedtls/aes.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SESSION_FILE "session.dat"
#define DEVICE_KEY_FILE "device.key"

#define DEVICE_KEY_SIZE 32   /* AES-256 */
#define NONCE_SIZE      16

/* File header: magic, version, reserved, nonce. */
#define FILE_MAGIC   "COBALTSN"
#define FILE_MAGIC_LEN 8
#define FILE_VERSION 1
#define HEADER_SIZE  (FILE_MAGIC_LEN + 1 + 7 + NONCE_SIZE)

/*
 * Sits at the head of the *decrypted* payload. Its job is to tell a wrong or
 * missing device key apart from a corrupt file: CTR decryption under the wrong
 * key succeeds mechanically and hands back plausible-looking bytes, so without
 * a known marker we would happily load garbage into a credential struct.
 */
#define PAYLOAD_MAGIC 0x434F424Cu   /* 'C' 'O' 'B' 'L' */

/* Comfortably above the sum of the field maxima plus their length prefixes. */
#define PAYLOAD_MAX 4096

/* --- small helpers --- */

/*
 * memset on a buffer that is dead afterwards is exactly the store a compiler is
 * entitled to delete. Going through a volatile pointer keeps it.
 */
static void
wipe(void *p, size_t n)
{
   volatile unsigned char *q = (volatile unsigned char *) p;
   while (n--) {
      *q++ = 0;
   }
}

static void
put_u16(unsigned char *p, uint16_t v)
{
   p[0] = (unsigned char) (v >> 8);
   p[1] = (unsigned char) (v & 0xFF);
}

static uint16_t
get_u16(const unsigned char *p)
{
   return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

static void
put_u32(unsigned char *p, uint32_t v)
{
   p[0] = (unsigned char) (v >> 24);
   p[1] = (unsigned char) (v >> 16);
   p[2] = (unsigned char) (v >> 8);
   p[3] = (unsigned char) (v & 0xFF);
}

static uint32_t
get_u32(const unsigned char *p)
{
   return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
          ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

/* FNV-1a. Not a MAC — it catches truncation and bit rot, nothing adversarial. */
static uint32_t
checksum(const unsigned char *data, size_t len)
{
   uint32_t h = 2166136261u;
   for (size_t i = 0; i < len; i++) {
      h ^= data[i];
      h *= 16777619u;
   }
   return h;
}

static bool
data_file(char *out, size_t out_size, const char *name)
{
   if (!cobalt_data_path(out, out_size, name)) {
      COBALT_LOGW("session store: no writable data root for %s", name);
      return false;
   }
   return true;
}

/* --- randomness --- */

/*
 * Both the device key and the CTR nonce come from the application's own
 * generator (util/rng.h), NOT from mbedtls_entropy_func().
 *
 * That distinction is the whole point. mbedTLS's pool on this platform is a
 * single source that reduces to srand(OSGetSystemTick()), so a device key
 * drawn from it would be recoverable from the console's uptime at the moment
 * the file was created — which is a far weaker guarantee than the one
 * session_store.h describes. A nonce from it might not even be unique, and
 * CTR nonce reuse under one key leaks the XOR of the two sessions.
 *
 * cobalt_rng_bytes() fails rather than substituting anything weaker, so a
 * console with no provisioned seed does not get a quietly guessable key file.
 */
static bool
random_bytes(unsigned char *out, size_t len, const char *purpose)
{
   if (!cobalt_rng_bytes(out, len)) {
      COBALT_LOGE("session store: no randomness available for %s — an entropy "
                  "seed must be provisioned first", purpose);
      return false;
   }
   return true;
}

/* --- device key --- */

/* Load the per-installation key, creating it on first use. */
static bool
device_key(unsigned char key[DEVICE_KEY_SIZE])
{
   char path[COBALT_PATH_MAX];
   if (!data_file(path, sizeof(path), DEVICE_KEY_FILE)) {
      return false;
   }

   FILE *f = fopen(path, "rb");
   if (f) {
      size_t read = fread(key, 1, DEVICE_KEY_SIZE, f);
      /* A trailing byte means this is not our file; treat it as unusable
       * rather than silently keying off a prefix of something else. */
      bool exact = (read == DEVICE_KEY_SIZE) && (fgetc(f) == EOF);
      fclose(f);
      if (exact) {
         return true;
      }
      COBALT_LOGW("session store: %s is not %d bytes — regenerating", path,
                  DEVICE_KEY_SIZE);
      wipe(key, DEVICE_KEY_SIZE);
   }

   if (!random_bytes(key, DEVICE_KEY_SIZE, "cobalt.device.key")) {
      return false;
   }

   f = fopen(path, "wb");
   if (!f) {
      COBALT_LOGE("session store: cannot create %s", path);
      wipe(key, DEVICE_KEY_SIZE);
      return false;
   }

   bool written = fwrite(key, 1, DEVICE_KEY_SIZE, f) == DEVICE_KEY_SIZE;
   if (fclose(f) != 0) {
      written = false;
   }

   if (!written) {
      COBALT_LOGE("session store: failed to write %s", path);
      remove(path);
      wipe(key, DEVICE_KEY_SIZE);
      return false;
   }

   COBALT_LOGI("session store: generated a new device key");
   return true;
}

/* --- payload codec --- */

static bool
append_field(unsigned char *buf, size_t cap, size_t *len, const char *value)
{
   size_t n = value ? strlen(value) : 0;
   if (n > 0xFFFF || *len + 2 + n > cap) {
      return false;
   }
   put_u16(buf + *len, (uint16_t) n);
   *len += 2;
   if (n > 0) {
      memcpy(buf + *len, value, n);
      *len += n;
   }
   return true;
}

static bool
read_field(const unsigned char *buf, size_t len, size_t *pos, char *out, size_t out_size)
{
   if (*pos + 2 > len) {
      return false;
   }
   size_t n = get_u16(buf + *pos);
   *pos += 2;
   /* >= because the copy has to leave room for the terminator. */
   if (*pos + n > len || n >= out_size) {
      return false;
   }
   memcpy(out, buf + *pos, n);
   out[n] = '\0';
   *pos += n;
   return true;
}

static bool
encode_payload(const cobalt_stored_session *s, unsigned char *buf, size_t cap,
               size_t *out_len)
{
   size_t len = 0;

   if (cap < 4) {
      return false;
   }
   put_u32(buf, PAYLOAD_MAGIC);
   len = 4;

   if (!append_field(buf, cap, &len, s->service) ||
       !append_field(buf, cap, &len, s->handle) ||
       !append_field(buf, cap, &len, s->did) ||
       !append_field(buf, cap, &len, s->access_jwt) ||
       !append_field(buf, cap, &len, s->refresh_jwt)) {
      return false;
   }

   if (len + 4 > cap) {
      return false;
   }
   put_u32(buf + len, checksum(buf, len));
   len += 4;

   *out_len = len;
   return true;
}

static bool
decode_payload(const unsigned char *buf, size_t len, cobalt_stored_session *out)
{
   if (len < 8 || get_u32(buf) != PAYLOAD_MAGIC) {
      return false;
   }

   size_t body = len - 4;
   if (get_u32(buf + body) != checksum(buf, body)) {
      return false;
   }

   size_t pos = 4;
   return read_field(buf, body, &pos, out->service, sizeof(out->service)) &&
          read_field(buf, body, &pos, out->handle, sizeof(out->handle)) &&
          read_field(buf, body, &pos, out->did, sizeof(out->did)) &&
          read_field(buf, body, &pos, out->access_jwt, sizeof(out->access_jwt)) &&
          read_field(buf, body, &pos, out->refresh_jwt, sizeof(out->refresh_jwt)) &&
          pos == body;
}

/* CTR is its own inverse, so one routine serves save and load. */
static bool
crypt_in_place(const unsigned char key[DEVICE_KEY_SIZE],
               const unsigned char nonce[NONCE_SIZE],
               unsigned char *data, size_t len)
{
   mbedtls_aes_context aes;
   unsigned char counter[NONCE_SIZE];
   unsigned char stream[16];
   size_t offset = 0;

   memcpy(counter, nonce, NONCE_SIZE);
   memset(stream, 0, sizeof(stream));

   mbedtls_aes_init(&aes);
   int rc = mbedtls_aes_setkey_enc(&aes, key, DEVICE_KEY_SIZE * 8);
   if (rc == 0) {
      rc = mbedtls_aes_crypt_ctr(&aes, len, &offset, counter, stream, data, data);
   }
   mbedtls_aes_free(&aes);

   wipe(counter, sizeof(counter));
   wipe(stream, sizeof(stream));

   if (rc != 0) {
      COBALT_LOGE("session store: AES-CTR failed (-0x%04x)", (unsigned) -rc);
      return false;
   }
   return true;
}

/* --- public API --- */

bool
cobalt_session_store_save(const cobalt_stored_session *session)
{
   if (!session) {
      return false;
   }

   char path[COBALT_PATH_MAX];
   char tmp[COBALT_PATH_MAX];
   if (!data_file(path, sizeof(path), SESSION_FILE) ||
       !data_file(tmp, sizeof(tmp), SESSION_FILE ".tmp")) {
      return false;
   }

   unsigned char key[DEVICE_KEY_SIZE];
   if (!device_key(key)) {
      return false;
   }

   unsigned char payload[PAYLOAD_MAX];
   size_t payload_len = 0;
   bool ok = encode_payload(session, payload, sizeof(payload), &payload_len);
   if (!ok) {
      COBALT_LOGE("session store: credentials do not fit the payload buffer");
      goto done;
   }

   unsigned char header[HEADER_SIZE];
   memset(header, 0, sizeof(header));
   memcpy(header, FILE_MAGIC, FILE_MAGIC_LEN);
   header[FILE_MAGIC_LEN] = FILE_VERSION;

   ok = random_bytes(header + FILE_MAGIC_LEN + 8, NONCE_SIZE, "cobalt.session.nonce");
   if (!ok) {
      goto done;
   }

   ok = crypt_in_place(key, header + FILE_MAGIC_LEN + 8, payload, payload_len);
   if (!ok) {
      goto done;
   }

   /* Temp file plus rename, matching what util/entropy.c does for the seed:
    * losing power mid-write must not cost the user their existing session. */
   FILE *f = fopen(tmp, "wb");
   if (!f) {
      COBALT_LOGE("session store: cannot open %s for writing", tmp);
      ok = false;
      goto done;
   }

   ok = fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
        fwrite(payload, 1, payload_len, f) == payload_len;
   if (fclose(f) != 0) {
      ok = false;
   }

   if (!ok) {
      COBALT_LOGE("session store: failed to write %s", tmp);
      remove(tmp);
      goto done;
   }

   /* rename() will not clobber an existing file on every libc, and does not on
    * the Wii U's FAT devoptab, so clear the way first. */
   remove(path);
   if (rename(tmp, path) != 0) {
      COBALT_LOGE("session store: failed to rename %s -> %s", tmp, path);
      remove(tmp);
      ok = false;
      goto done;
   }

   COBALT_LOGI("session store: saved credentials for %s", session->handle);

done:
   wipe(key, sizeof(key));
   wipe(payload, sizeof(payload));
   return ok;
}

bool
cobalt_session_store_load(cobalt_stored_session *out)
{
   if (!out) {
      return false;
   }
   memset(out, 0, sizeof(*out));

   char path[COBALT_PATH_MAX];
   if (!data_file(path, sizeof(path), SESSION_FILE)) {
      return false;
   }

   FILE *f = fopen(path, "rb");
   if (!f) {
      return false; /* No stored session is the normal first-run case. */
   }

   unsigned char header[HEADER_SIZE];
   unsigned char payload[PAYLOAD_MAX];
   bool ok = false;
   size_t payload_len = 0;

   if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
      COBALT_LOGW("session store: %s is truncated", path);
      fclose(f);
      goto done;
   }

   if (memcmp(header, FILE_MAGIC, FILE_MAGIC_LEN) != 0) {
      COBALT_LOGW("session store: %s is not a session file", path);
      fclose(f);
      goto done;
   }

   if (header[FILE_MAGIC_LEN] != FILE_VERSION) {
      COBALT_LOGW("session store: %s is format version %u, expected %u — "
                  "sign in again", path, (unsigned) header[FILE_MAGIC_LEN],
                  (unsigned) FILE_VERSION);
      fclose(f);
      goto done;
   }

   payload_len = fread(payload, 1, sizeof(payload), f);
   fclose(f);

   if (payload_len < 8) {
      COBALT_LOGW("session store: %s has no payload", path);
      goto done;
   }

   unsigned char key[DEVICE_KEY_SIZE];
   if (!device_key(key)) {
      goto done;
   }

   ok = crypt_in_place(key, header + FILE_MAGIC_LEN + 8, payload, payload_len);
   wipe(key, sizeof(key));
   if (!ok) {
      goto done;
   }

   ok = decode_payload(payload, payload_len, out);
   if (!ok) {
      /* Either the file is corrupt or device.key is not the one it was written
       * under — from here the two are indistinguishable, and the remedy is the
       * same either way. */
      COBALT_LOGW("session store: %s did not decode — signing in again is needed",
                  path);
      memset(out, 0, sizeof(*out));
      goto done;
   }

   COBALT_LOGI("session store: loaded credentials for %s", out->handle);

done:
   wipe(payload, sizeof(payload));
   return ok;
}

bool
cobalt_session_store_exists(void)
{
   char path[COBALT_PATH_MAX];
   if (!data_file(path, sizeof(path), SESSION_FILE)) {
      return false;
   }

   FILE *f = fopen(path, "rb");
   if (!f) {
      return false;
   }
   fclose(f);
   return true;
}

/*
 * Overwrite the file's bytes in place before unlinking. On the SD card's FAT
 * filesystem an unlink alone leaves the ciphertext sitting in the clusters, so
 * a "sign out" that only removed the directory entry would not be one.
 */
static bool
shred(const char *path)
{
   FILE *f = fopen(path, "r+b");
   if (!f) {
      /* Nothing there is the desired end state. */
      return true;
   }

   if (fseek(f, 0, SEEK_END) != 0) {
      fclose(f);
      remove(path);
      return false;
   }

   long size = ftell(f);
   if (size > 0 && fseek(f, 0, SEEK_SET) == 0) {
      unsigned char zeros[256];
      memset(zeros, 0, sizeof(zeros));
      long remaining = size;
      while (remaining > 0) {
         size_t chunk = (remaining > (long) sizeof(zeros)) ? sizeof(zeros)
                                                           : (size_t) remaining;
         if (fwrite(zeros, 1, chunk, f) != chunk) {
            break;
         }
         remaining -= (long) chunk;
      }
      fflush(f);
   }

   fclose(f);
   return remove(path) == 0;
}

bool
cobalt_session_store_clear(void)
{
   char session_path[COBALT_PATH_MAX];
   char key_path[COBALT_PATH_MAX];

   if (!data_file(session_path, sizeof(session_path), SESSION_FILE) ||
       !data_file(key_path, sizeof(key_path), DEVICE_KEY_FILE)) {
      return false;
   }

   /* Order matters only for the log message; both are gone either way. */
   bool ok = shred(session_path);
   ok = shred(key_path) && ok;

   COBALT_LOGI("session store: cleared (%s)", ok ? "ok" : "with errors");
   return ok;
}
