#include "util/paths.h"
#include "util/log.h"

#include <whb/sdcard.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_PROBES 8

/* Candidate content roots, most specific first. /vol/content is where Aroma
 * mounts a WUHB's payload; the SD paths cover a bare-RPX dev launch. */
static const char *const CONTENT_CANDIDATES[] = {
   "fs:/vol/content",
   "/vol/content",
   "fs:/vol/external01/wiiu/apps/cobalt/content",
   "fs:/vol/external01/wiiu/apps/cobalt",
   "content",
};

static char s_content_root[COBALT_PATH_MAX];
static char s_data_root[COBALT_PATH_MAX];
static bool s_have_content = false;
static bool s_have_data = false;
static bool s_sd_mounted = false;

/* Probe results are stashed rather than logged inline, because paths have to
 * resolve before logging can open its file sink. */
static struct {
   char path[COBALT_PATH_MAX];
   bool found;
} s_probes[MAX_PROBES];
static int s_probe_count = 0;

static bool
join(char *out, size_t out_size, const char *root, const char *rel)
{
   if (!out || out_size == 0 || !root) {
      return false;
   }

   int written;
   if (!rel || rel[0] == '\0') {
      written = snprintf(out, out_size, "%s", root);
   } else {
      written = snprintf(out, out_size, "%s/%s", root, rel);
   }

   /* Truncation would silently point at the wrong file, so treat it as a
    * hard failure rather than letting a clipped path through. */
   return written > 0 && (size_t) written < out_size;
}

static bool
file_exists(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      return false;
   }
   fclose(f);
   return true;
}

static void
record_probe(const char *path, bool found)
{
   if (s_probe_count >= MAX_PROBES) {
      return;
   }
   snprintf(s_probes[s_probe_count].path, COBALT_PATH_MAX, "%s", path);
   s_probes[s_probe_count].found = found;
   s_probe_count++;
}

static bool
find_content_root(void)
{
   const size_t count = sizeof(CONTENT_CANDIDATES) / sizeof(CONTENT_CANDIDATES[0]);

   for (size_t i = 0; i < count; i++) {
      char marker[COBALT_PATH_MAX];
      if (!join(marker, sizeof(marker), CONTENT_CANDIDATES[i], COBALT_CONTENT_MARKER)) {
         continue;
      }

      bool found = file_exists(marker);
      record_probe(marker, found);

      if (found) {
         snprintf(s_content_root, sizeof(s_content_root), "%s", CONTENT_CANDIDATES[i]);
         return true;
      }
   }

   return false;
}

static bool
find_data_root(void)
{
   if (s_sd_mounted) {
      const char *mount = WHBGetSdCardMountPath();
      if (mount && mount[0] != '\0') {
         if (join(s_data_root, sizeof(s_data_root), mount, "wiiu/apps/cobalt")) {
            /* Best-effort: the directory usually already exists because the
             * WUHB was launched from it. mkdir failing is not fatal. */
            mkdir(s_data_root, 0777);
            return true;
         }
      }
   }

   /* No SD card: fall back to the content root so logs and cache at least have
    * a defined home, even if it turns out to be read-only. */
   if (s_have_content) {
      snprintf(s_data_root, sizeof(s_data_root), "%s", s_content_root);
      return true;
   }

   return false;
}

bool
cobalt_paths_init(void)
{
   s_sd_mounted = WHBMountSdCard();

   s_have_content = find_content_root();
   s_have_data = find_data_root();

   return s_have_content;
}

void
cobalt_paths_shutdown(void)
{
   if (s_sd_mounted) {
      WHBUnmountSdCard();
      s_sd_mounted = false;
   }
   s_have_content = false;
   s_have_data = false;
}

void
cobalt_paths_log(void)
{
   COBALT_LOGI("paths: sd card mounted=%d", (int) s_sd_mounted);

   for (int i = 0; i < s_probe_count; i++) {
      COBALT_LOGI("paths: probe %s -> %s", s_probes[i].path,
                  s_probes[i].found ? "FOUND" : "missing");
   }

   if (s_have_content) {
      COBALT_LOGI("paths: content root = %s", s_content_root);
   } else {
      COBALT_LOGE("paths: no content root found — bundled assets are unavailable");
   }

   if (s_have_data) {
      COBALT_LOGI("paths: data root = %s", s_data_root);
   } else {
      COBALT_LOGE("paths: no writable data root — session and cache cannot persist");
   }
}

bool
cobalt_content_path(char *out, size_t out_size, const char *rel)
{
   if (!s_have_content) {
      return false;
   }
   return join(out, out_size, s_content_root, rel);
}

bool
cobalt_data_path(char *out, size_t out_size, const char *rel)
{
   if (!s_have_data) {
      return false;
   }
   return join(out, out_size, s_data_root, rel);
}

const char *
cobalt_content_root(void)
{
   return s_have_content ? s_content_root : NULL;
}

const char *
cobalt_data_root(void)
{
   return s_have_data ? s_data_root : NULL;
}
