#pragma once

/*
 * Release the .wuhb this build is running from.
 *
 * Aroma loads a .wuhb by mounting its content at /vol/content and holds that
 * mount for the life of the process. While it is held the file cannot be
 * renamed or overwritten (V10lator/NUSspli#24) — which is exactly what putting
 * a self-update in place does. Aroma exposes the release through its
 * RPXLoadingModule ("homebrew_rpx_loader"), the same call the module makes for
 * itself when an application ends; asking for it early is what lets the update
 * be applied on the quit path.
 *
 * Returns true if the release was requested (the module is present). False means
 * nothing here can release it — a bare-RPX install, an emulator, or a loader
 * without the module — and the caller should still attempt the replacement,
 * which works wherever the running executable is not held mounted.
 *
 * Once this returns true the /vol/content redirection is off and this process
 * can no longer read its own bundled assets, so call it only when nothing else
 * will read them: on the quit path, immediately before the replacement. It is a
 * no-op that returns false on the host.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool cobalt_wuhb_unmount_running(void);

#ifdef __cplusplus
}
#endif
