#include "util/entropy.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool
cobalt_entropy_seed_load(const char *path, unsigned char seed[COBALT_ENTROPY_SEED_SIZE])
{
   if (!path || !path[0] || !seed) {
      return false;
   }

   FILE *file = fopen(path, "rb");
   if (!file) {
      /* Absent on a first run, or on an install that never got a seed. Not an
       * error here; the caller decides how loudly to complain. */
      return false;
   }

   long size = -1;
   if (fseek(file, 0, SEEK_END) != 0 ||
       (size = ftell(file)) != COBALT_ENTROPY_SEED_SIZE ||
       fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      memset(seed, 0, COBALT_ENTROPY_SEED_SIZE);
      COBALT_LOGE("entropy: %s is %ld bytes, expected %d — refusing to use it",
                  path, size, COBALT_ENTROPY_SEED_SIZE);
      return false;
   }

   size_t count = fread(seed, 1, COBALT_ENTROPY_SEED_SIZE, file);
   if (fclose(file) != 0 || count != COBALT_ENTROPY_SEED_SIZE) {
      /* A short read must never be padded out and used. */
      memset(seed, 0, COBALT_ENTROPY_SEED_SIZE);
      COBALT_LOGE("entropy: short read from %s", path);
      return false;
   }

   return true;
}

bool
cobalt_entropy_seed_save(const char *path,
                         const unsigned char seed[COBALT_ENTROPY_SEED_SIZE])
{
   if (!path || !path[0] || !seed) {
      return false;
   }

   size_t path_length = strlen(path);
   char *temporary = (char *) malloc(path_length + 5);
   if (!temporary) {
      return false;
   }
   memcpy(temporary, path, path_length);
   memcpy(temporary + path_length, ".tmp", 5);

   FILE *file = fopen(temporary, "wb");
   if (!file) {
      COBALT_LOGE("entropy: cannot open %s for writing", temporary);
      free(temporary);
      return false;
   }

   bool failed = fwrite(seed, 1, COBALT_ENTROPY_SEED_SIZE, file) !=
                 COBALT_ENTROPY_SEED_SIZE;
   if (fflush(file) != 0) {
      failed = true;
   }

   /*
    * Push the bytes to the card before the rename makes them visible. fflush
    * only clears stdio's buffer; without this barrier a power loss can land
    * the rename while the data is still cached, leaving a zero-length or
    * stale seed. The next boot would then reuse the previous seed and
    * regenerate identical key material — the exact failure the rotate cycle
    * exists to prevent.
    *
    * wut routes fsync through __wut_fsa_fsync to FSAFlushFile, so this is
    * real on hardware. A failure is deliberately not fatal: the data is
    * already flushed, and refusing to save on a platform that cannot provide
    * the barrier would lose the seed entirely rather than merely risk it.
    */
   if (!failed) {
      (void) fsync(fileno(file));
   }

   if (fclose(file) != 0) {
      failed = true;
   }

   /* Rename only after the bytes are known to be on the card: an interrupted
    * write then leaves the old seed in place instead of a truncated one. */
   if (!failed && rename(temporary, path) != 0) {
      failed = true;
   }
   if (failed) {
      remove(temporary);
      COBALT_LOGE("entropy: failed to persist a rotated seed to %s", path);
   }

   free(temporary);
   return !failed;
}
