#include "util/log.h"
#include "util/paths.h"

#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_module.h>
#include <whb/log_udp.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* One line at a time; the WHB sinks take a NUL-terminated string. */
#define LOG_LINE_MAX 512

static const char *const LEVEL_TAG[] = { "ERR", "WRN", "INF", "DBG" };

static cobalt_log_level s_level =
#ifdef COBALT_DEBUG
   COBALT_LOG_DEBUG;
#else
   COBALT_LOG_INFO;
#endif

static BOOL s_cafe_up = FALSE;
static BOOL s_udp_up = FALSE;
static BOOL s_module_up = FALSE;
static FILE *s_file = NULL;
static BOOL s_initialised = FALSE;

void
cobalt_log_init(void)
{
   if (s_initialised) {
      return;
    }

   /* Each sink is independent: a console with no LoggingModule installed, or
    * no network, should still get the others rather than losing logging
    * entirely. */
   s_cafe_up = WHBLogCafeInit();
   s_udp_up = WHBLogUdpInit();
   s_module_up = WHBLogModuleInit();

   char path[COBALT_PATH_MAX];
   if (cobalt_data_path(path, sizeof(path), "cobalt.log")) {
      s_file = fopen(path, "w");
   }

   s_initialised = TRUE;

   COBALT_LOGI("cobalt logging up: cafe=%d udp=%d module=%d file=%s",
               (int) s_cafe_up, (int) s_udp_up, (int) s_module_up,
               s_file ? path : "(none)");
}

void
cobalt_log_shutdown(void)
{
   if (!s_initialised) {
      return;
   }

   if (s_file) {
      fflush(s_file);
      fclose(s_file);
      s_file = NULL;
   }

   if (s_module_up) {
      WHBLogModuleDeinit();
      s_module_up = FALSE;
   }
   if (s_udp_up) {
      WHBLogUdpDeinit();
      s_udp_up = FALSE;
   }
   if (s_cafe_up) {
      WHBLogCafeDeinit();
      s_cafe_up = FALSE;
   }

   s_initialised = FALSE;
}

void
cobalt_log_set_level(cobalt_log_level level)
{
   s_level = level;
}

void
cobalt_log_write(cobalt_log_level level, const char *file, int line,
                 const char *fmt, ...)
{
   if (level > s_level) {
      return;
   }

   /* Strip the leading path so lines stay readable on a 854px-wide screen. */
   const char *base = strrchr(file, '/');
   base = base ? base + 1 : file;

   char line_buf[LOG_LINE_MAX];
   int used = snprintf(line_buf, sizeof(line_buf), "[%s] %s:%d: ",
                       LEVEL_TAG[level], base, line);
   if (used < 0) {
      return;
   }
   if ((size_t) used >= sizeof(line_buf)) {
      used = (int) sizeof(line_buf) - 1;
   }

   va_list args;
   va_start(args, fmt);
   vsnprintf(line_buf + used, sizeof(line_buf) - (size_t) used, fmt, args);
   va_end(args);

   /* WHBLogPrint fans out to whichever sinks came up. */
   WHBLogPrint(line_buf);

   if (s_file) {
      fprintf(s_file, "%s\n", line_buf);
      /* Flushed every line on purpose: the interesting case is reading this
       * file back after a hang or a crash, where a buffered tail is lost. */
      fflush(s_file);
   }
}
