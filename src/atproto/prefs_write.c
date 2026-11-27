#include "atproto/prefs_write.h"

#ifdef COBALT_HAS_WOLFRAM

#include <stdlib.h>
#include <string.h>

cJSON *
cobalt_prefs_read(wf_agent *wf)
{
   char *json = NULL;
   if (!wf || wf_agent_get_preferences(wf, &json) != WF_OK || !json) {
      free(json);
      return NULL;
   }
   cJSON *prefs = cJSON_Parse(json);
   free(json);
   if (!cJSON_IsArray(prefs)) {
      cJSON_Delete(prefs);
      return NULL;
   }
   return prefs;
}

static bool
is_type(const cJSON *pref, const char *type)
{
   const cJSON *ty = cJSON_GetObjectItemCaseSensitive(pref, "$type");
   return cJSON_IsString(ty) && ty->valuestring && strcmp(ty->valuestring, type) == 0;
}

cJSON *
cobalt_prefs_find(cJSON *prefs, const char *type)
{
   cJSON *it;
   cJSON_ArrayForEach(it, prefs) {
      if (is_type(it, type)) {
         return it;
      }
   }
   return NULL;
}

wf_status
cobalt_prefs_put_replacing(wf_agent *wf, cJSON *prefs, const char *type,
                           cJSON *replacement)
{
   if (!wf || !cJSON_IsArray(prefs) || !type) {
      cJSON_Delete(replacement);
      return WF_ERR_INVALID_ARG;
   }
   for (int i = cJSON_GetArraySize(prefs) - 1; i >= 0; i--) {
      if (is_type(cJSON_GetArrayItem(prefs, i), type)) {
         cJSON_DeleteItemFromArray(prefs, i);
      }
   }
   if (replacement) {
      cJSON_AddItemToArray(prefs, replacement);
   }
   char *json = cJSON_PrintUnformatted(prefs);
   if (!json) {
      return WF_ERR_ALLOC;
   }
   wf_response res = {0};
   const wf_status status = wf_agent_put_preferences(wf, json, &res);
   free(json);
   wf_response_free(&res);
   return status;
}

#endif
