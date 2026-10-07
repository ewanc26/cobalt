#include "util/clock.h"

#include <time.h>

int64_t
cobalt_time_now(void)
{
   const time_t now = time(NULL);
   /* (time_t) -1 means the clock is unavailable. */
   return (now == (time_t) -1) ? 0 : (int64_t) now;
}
