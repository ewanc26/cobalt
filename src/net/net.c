#include "net/net.h"
#include "util/log.h"

#include <nn/ac.h>

#include <stdio.h>

/*
 * nn::ac ownership.
 *
 * WUT brings the network up on its own. Its socket devoptab initialiser
 * (__init_wut_socket, linked in as soon as anything references sockets — curl
 * does) runs socket_lib_init(), AddDevice(), ACInitialize() and then
 * ACConnectAsync(); the matching __fini_wut_socket calls ACClose() and
 * ACFinalize() at exit.
 *
 * So Cobalt must NOT call ACInitialize/ACFinalize itself. Doing so would
 * double-finalize AC against WUT's own teardown. This module therefore only
 * *queries* AC, and uses the synchronous ACConnect() as a fallback for the
 * case where WUT's async connect has not completed (or has failed) by the time
 * something actually needs the network.
 */

static cobalt_net_status s_status = COBALT_NET_UNKNOWN;
static char s_address[16] = "0.0.0.0";

static void
clear_address(void)
{
   snprintf(s_address, sizeof(s_address), "0.0.0.0");
}

bool
cobalt_net_init(void)
{
   /*
    * Nothing to initialise — see the note above. The first refresh reports
    * whether WUT's ACConnectAsync has landed yet.
    */
   cobalt_net_refresh();

   if (s_status != COBALT_NET_UP) {
      /*
       * WUT's connect is asynchronous, so "not up" at this point is expected
       * on a cold start rather than an error. A synchronous ACConnect both
       * waits for the in-flight attempt and retries if it failed.
       */
      COBALT_LOGI("network not up yet; running a synchronous ACConnect");
      NNResult result = ACConnect();
      if (NNResult_IsFailure(result)) {
         COBALT_LOGW("ACConnect failed (0x%08x)", (unsigned) result.value);
      }
      cobalt_net_refresh();
   }

   return s_status == COBALT_NET_UP;
}

void
cobalt_net_shutdown(void)
{
   /* Deliberately empty: WUT's __fini_wut_socket owns ACClose/ACFinalize. */
   s_status = COBALT_NET_UNKNOWN;
   clear_address();
}

cobalt_net_status
cobalt_net_refresh(void)
{
   BOOL connected = FALSE;
   NNResult result = ACIsApplicationConnected(&connected);
   if (NNResult_IsFailure(result)) {
      /* AC not being usable at all is a different failure from being offline,
       * and the diagnostics screen distinguishes them. */
      COBALT_LOGW("ACIsApplicationConnected failed (0x%08x)", (unsigned) result.value);
      s_status = COBALT_NET_UNAVAILABLE;
      clear_address();
      return s_status;
   }

   if (!connected) {
      s_status = COBALT_NET_DOWN;
      clear_address();
      COBALT_LOGW("network: console reports no application connection");
      return s_status;
   }

   uint32_t ip = 0;
   result = ACGetAssignedAddress(&ip);
   if (NNResult_IsFailure(result) || ip == 0) {
      /* Connected but no address is a real transient state (mid-DHCP). Report
       * it as down rather than claiming the network is up and then having
       * every request time out with a confusing error. */
      s_status = COBALT_NET_DOWN;
      clear_address();
      COBALT_LOGW("network: connected but no address assigned yet");
      return s_status;
   }

   snprintf(s_address, sizeof(s_address), "%u.%u.%u.%u",
            (unsigned) ((ip >> 24) & 0xFF), (unsigned) ((ip >> 16) & 0xFF),
            (unsigned) ((ip >> 8) & 0xFF), (unsigned) (ip & 0xFF));

   s_status = COBALT_NET_UP;
   COBALT_LOGI("network up at %s", s_address);
   return s_status;
}

cobalt_net_status
cobalt_net_get_status(void)
{
   return s_status;
}

const char *
cobalt_net_status_string(cobalt_net_status status)
{
   switch (status) {
      case COBALT_NET_UP:          return "Connected";
      case COBALT_NET_DOWN:        return "No connection";
      case COBALT_NET_UNAVAILABLE: return "Network unavailable";
      case COBALT_NET_UNKNOWN:
      default:                     return "Not checked";
   }
}

const char *
cobalt_net_local_address(void)
{
   return s_address;
}
