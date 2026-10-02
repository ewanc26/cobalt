#ifndef COBALT_UTIL_THREADPRIO_H
#define COBALT_UTIL_THREADPRIO_H

#ifdef __WIIU__
#include <coreinit/thread.h>
#endif

/*
 * Call from the top of a background worker thread (network, image decode).
 *
 * Wii U threads of equal priority sharing a core are not time-sliced, so a
 * worker spending seconds in TLS or JPEG decode on the render thread's core
 * freezes the frame for that long — measured on hardware as a 6.9 s frame
 * during session resume. The main thread lives on core 1; this moves the
 * caller to cores 0 and 2 and below the main thread's priority.
 */
static inline void
cobalt_thread_make_background(void)
{
#ifdef __WIIU__
   OSThread *self = OSGetCurrentThread();
   OSSetThreadAffinity(self, (OSThreadAttributes) (OS_THREAD_ATTRIB_AFFINITY_CPU0 |
                                                   OS_THREAD_ATTRIB_AFFINITY_CPU2));
   int32_t prio = OSGetThreadPriority(self) + 4;
   OSSetThreadPriority(self, prio > 31 ? 31 : prio);
#endif
}

#endif
