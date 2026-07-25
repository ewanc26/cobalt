#pragma once

/*
 * Logging.
 *
 * There is no emulator in this project's workflow (AGENTS.md §10), so every
 * build goes straight to real hardware and a botched run is expensive. Logging
 * is therefore deliberately loud during development and fans out to every sink
 * WUT gives us, because which one is actually reachable depends on how the
 * console is being driven at the time:
 *
 *   - Cafe OS   — shows up in a Cafe/decaf-style log consumer.
 *   - UDP       — broadcast on port 4405, readable from a PC on the same LAN
 *                 with `nc -ul 4405`. This is the one that makes a headless
 *                 real-hardware debug loop bearable.
 *   - Module    — Aroma's LoggingModule, when it is installed.
 *   - SD file   — sd:/wiiu/apps/cobalt/cobalt.log, survives a hard crash and
 *                 can be read back off the card afterwards.
 *
 * Verbose levels compile out entirely in release builds (see COBALT_DEBUG).
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_LOG_ERROR = 0,
   COBALT_LOG_WARN,
   COBALT_LOG_INFO,
   COBALT_LOG_DEBUG,
} cobalt_log_level;

/* Bring up every available sink. Safe to call once, early in main(). */
void cobalt_log_init(void);

/* Flush and tear down all sinks. Safe to call even if init partly failed. */
void cobalt_log_shutdown(void);

/* Runtime floor; messages above this level are dropped. */
void cobalt_log_set_level(cobalt_log_level level);

void cobalt_log_write(cobalt_log_level level,
                      const char *file,
                      int line,
                      const char *fmt,
                      ...) __attribute__((format(printf, 4, 5)));

#define COBALT_LOGE(...) cobalt_log_write(COBALT_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define COBALT_LOGW(...) cobalt_log_write(COBALT_LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define COBALT_LOGI(...) cobalt_log_write(COBALT_LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)

#ifdef COBALT_DEBUG
#define COBALT_LOGD(...) cobalt_log_write(COBALT_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
/* Compiled out, but still type-checked so debug logs cannot rot silently. */
#define COBALT_LOGD(...)                                                       \
   do {                                                                        \
      if (0) {                                                                 \
         cobalt_log_write(COBALT_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__);  \
      }                                                                        \
   } while (0)
#endif

#ifdef __cplusplus
}
#endif
