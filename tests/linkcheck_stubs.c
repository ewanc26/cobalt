/*
 * Stand-ins for symbols that only exist on the console, so the link check has
 * something to resolve against.
 *
 * Two groups, and neither is a shortcut:
 *
 *   - net/net.c is excluded from the sweep because it includes <nn/ac.h>, which
 *     only exists inside devkitPro. app.c calls into it for the diagnostics
 *     screen, so those calls need a definition here.
 *   - wf_wiiu_* live in Wolfram's *Wii U* build (src/crypto/wiiu_random.c).
 *     A host build of Wolfram does not contain them, by design — they are the
 *     Wii U entropy provisioning API.
 *
 * Nothing here is ever executed. The link check builds a binary and does not
 * run it; the point is only that every undefined symbol resolves, which is
 * what catches a function deleted while callers remained.
 */

#include "net/net.h"

#include <stddef.h>

cobalt_net_status
cobalt_net_get_status(void)
{
   return COBALT_NET_UNKNOWN;
}

const char *
cobalt_net_status_string(cobalt_net_status status)
{
   (void) status;
   return "stub";
}

const char *
cobalt_net_local_address(void)
{
   return "0.0.0.0";
}

cobalt_net_status
cobalt_net_refresh(void)
{
   return COBALT_NET_UNKNOWN;
}

bool
cobalt_net_init(void)
{
   return false;
}

void
cobalt_net_shutdown(void)
{
}

#ifdef COBALT_HAS_WOLFRAM

/* Mirrors <wolfram/wiiu.h>. Declared here rather than by including that header
 * so a signature change there shows up as a conflicting-types error, which is
 * exactly the drift this check exists to catch. */
#include <wolfram/wiiu.h>

wf_status
wf_wiiu_set_entropy_seed(const unsigned char *seed, size_t seed_len)
{
   (void) seed;
   (void) seed_len;
   return WF_ERR_NOT_IMPLEMENTED;
}

wf_status
wf_wiiu_rotate_entropy_seed(unsigned char *out, size_t out_len)
{
   (void) out;
   (void) out_len;
   return WF_ERR_NOT_IMPLEMENTED;
}

wf_status
wf_wiiu_commit_entropy_rotation(void)
{
   return WF_ERR_NOT_IMPLEMENTED;
}

int
wf_wiiu_entropy_ready(void)
{
   return 0;
}

#endif /* COBALT_HAS_WOLFRAM */
