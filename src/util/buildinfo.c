#include "util/buildinfo.h"

#include "util/version.h"

#include <stdio.h>

#if defined(__has_include)
#  if __has_include("util/buildinfo_gen.h")
#    include "util/buildinfo_gen.h"
#  endif
#endif

#ifndef COBALT_BUILD_COMMIT
#  define COBALT_BUILD_COMMIT "unknown"
#endif
#ifndef COBALT_BUILD_NUMBER
#  define COBALT_BUILD_NUMBER 0
#endif
#ifndef COBALT_BUILD_DATE
#  define COBALT_BUILD_DATE "unknown"
#endif

const char *cobalt_version(void) { return COBALT_VERSION; }
const char *cobalt_build_commit(void) { return COBALT_BUILD_COMMIT; }
int cobalt_build_number(void) { return COBALT_BUILD_NUMBER; }
const char *cobalt_build_date(void) { return COBALT_BUILD_DATE; }

const char *
cobalt_build_describe(void)
{
   static char text[96];
   if (COBALT_BUILD_NUMBER > 0) {
      snprintf(text, sizeof text, "v%s - build %d - %s - %s", COBALT_VERSION,
               COBALT_BUILD_NUMBER,
               COBALT_BUILD_COMMIT, COBALT_BUILD_DATE);
   } else {
      snprintf(text, sizeof text, "v%s - build %s - %s", COBALT_VERSION, COBALT_BUILD_COMMIT,
               COBALT_BUILD_DATE);
   }
   return text;
}
