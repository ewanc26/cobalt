/*
 * Host stand-ins for the two modules that cannot leave the console.
 *
 * util/paths.c talks to WHBMountSdCard and util/log.c to Cafe OS, the Aroma
 * logging module and a UDP socket — none of which exist off-hardware. Every
 * other module under test is platform-independent by construction, which is the
 * property AGENTS.md §10 asks for, so these two stubs are all it takes to run
 * the rest on a build machine.
 */

#include "util/log.h"
#include "util/paths.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static char s_root[COBALT_PATH_MAX] = ".";
static int s_log_verbose = 0;

/* Point both roots at a scratch directory for the duration of a test. */
void
cobalt_test_set_root(const char *path)
{
   snprintf(s_root, sizeof(s_root), "%s", path);
   mkdir(s_root, 0777);
}

void
cobalt_test_log_verbose(int on)
{
   s_log_verbose = on;
}

static bool
join(char *out, size_t out_size, const char *rel)
{
   int written = snprintf(out, out_size, "%s/%s", s_root, rel ? rel : "");
   return written > 0 && (size_t) written < out_size;
}

bool
cobalt_paths_init(void)
{
   return true;
}

void
cobalt_paths_shutdown(void)
{
}

void
cobalt_paths_log(void)
{
}

bool
cobalt_content_path(char *out, size_t out_size, const char *rel)
{
   return join(out, out_size, rel);
}

bool
cobalt_data_path(char *out, size_t out_size, const char *rel)
{
   return join(out, out_size, rel);
}

const char *
cobalt_content_root(void)
{
   return s_root;
}

const char *
cobalt_data_root(void)
{
   return s_root;
}

void
cobalt_log_init(void)
{
}

void
cobalt_log_shutdown(void)
{
}

void
cobalt_log_set_level(cobalt_log_level level)
{
   (void) level;
}

void
cobalt_log_write(cobalt_log_level level, const char *file, int line,
                 const char *fmt, ...)
{
   if (!s_log_verbose) {
      return;
   }

   static const char *const NAMES[] = { "E", "W", "I", "D" };
   const char *base = strrchr(file, '/');
   fprintf(stderr, "  [%s] %s:%d ", NAMES[level], base ? base + 1 : file, line);

   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}
