#include "util/wuhb.h"

#ifdef __WIIU__

#include <coreinit/dynload.h>

/*
 * Aroma's RPXLoadingModule exports RL_UnmountCurrentRunningBundle as a WUMS
 * function. WUMS re-exports each one as a plain dynamic symbol with the same
 * name, reachable through wut's OSDynLoad, so a single call needs no WUMS client
 * library (this is what wiiu-env's librpxloader does internally). The module is
 * part of base Aroma and is already present whenever a .wuhb is running.
 */
typedef int (*cobalt_wuhb_unmount_fn)(void);

static OSDynLoad_Module s_module;
static cobalt_wuhb_unmount_fn s_unmount;
static bool s_probed;

bool
cobalt_wuhb_unmount_running(void)
{
   void *sym = NULL;

   if (!s_probed) {
      s_probed = true;
      /* The handle is kept, not released: it has to stay acquired for the
       * export pointer to remain valid until it is called. */
      if (OSDynLoad_Acquire("homebrew_rpx_loader", &s_module) == OS_DYNLOAD_OK &&
          OSDynLoad_FindExport(s_module, OS_DYNLOAD_EXPORT_FUNC,
                               "RL_UnmountCurrentRunningBundle", &sym) == OS_DYNLOAD_OK) {
         s_unmount = (cobalt_wuhb_unmount_fn) sym;
      }
   }
   if (!s_unmount) {
      return false;
   }
   /* RL_UnmountCurrentRunningBundle reports success as RPX_LOADER_RESULT_SUCCESS
    * (0). It does not return for its own failure modes: the module OSFatals if
    * the content redirection cannot be removed. */
   return s_unmount() == 0;
}

#else /* !__WIIU__ */

bool
cobalt_wuhb_unmount_running(void)
{
   return false;
}

#endif
