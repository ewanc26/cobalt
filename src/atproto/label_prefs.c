#include "atproto/feeds.h"
#include "atproto/prefs_write.h"
#include "atproto/session_internal.h"
#include "util/log.h"

#include <string.h>

#define CONTENT_LABEL_PREF "app.bsky.actor.defs#contentLabelPref"
#define ADULT_CONTENT_PREF "app.bsky.actor.defs#adultContentPref"

#ifdef COBALT_HAS_WOLFRAM

static bool
visibility_ok(const char *v)
{
   return strcmp(v, "hide") == 0 || strcmp(v, "warn") == 0 || strcmp(v, "ignore") == 0;
}

void
cobalt_session_run_label_pref(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state)
{
   *state = COBALT_AUTH_SIGNED_IN;
   if (!g_session.wf) {
      cobalt_session_set_message(r, "Sign in first.");
      return;
   }
   const bool adult = strcmp(in->uri, "adult") == 0;
   if (adult ? !(strcmp(in->label_value, "on") == 0 || strcmp(in->label_value, "off") == 0)
             : !visibility_ok(in->label_value)) {
      cobalt_session_set_message(r, "That is not a content label setting.");
      return;
   }

   cJSON *prefs = cobalt_prefs_read(g_session.wf);
   if (!prefs) {
      /* A write built from a failed read would replace every other preference. */
      cobalt_session_set_message(r, "Could not read your preferences, so nothing was "
                                    "changed. Try again.");
      return;
   }

   cJSON *pref = cJSON_CreateObject();
   wf_status status;
   if (adult) {
      cJSON_AddStringToObject(pref, "$type", ADULT_CONTENT_PREF);
      cJSON_AddBoolToObject(pref, "enabled", strcmp(in->label_value, "on") == 0);
      status = cobalt_prefs_put_replacing(g_session.wf, prefs, ADULT_CONTENT_PREF, pref);
   } else {
      cJSON_AddStringToObject(pref, "$type", CONTENT_LABEL_PREF);
      cJSON_AddStringToObject(pref, "label", in->uri);
      cJSON_AddStringToObject(pref, "visibility", in->label_value);
      status = cobalt_prefs_put_replacing_match(g_session.wf, prefs, CONTENT_LABEL_PREF,
                                                "label", in->uri, pref);
   }
   cJSON_Delete(prefs);

   if (status != WF_OK) {
      COBALT_LOGW("session: label preference %s not saved (%d)", in->uri, (int) status);
      cobalt_session_set_message(r, "Could not save that setting (wolfram status %d). "
                                    "It is unchanged.", (int) status);
      return;
   }

   /* Rebuild the label context from what the server now holds, so the screen and
    * the filters agree with the account, not with what was asked for. */
   cobalt_session_refresh_label_context();
   COBALT_LOGI("session: label preference %s set to %s", in->uri, in->label_value);
   r->ok = true;
}

#else

void
cobalt_session_run_label_pref(const cobalt_job_input *in, cobalt_job_result *r,
                              cobalt_auth_state *state)
{
   (void) in;
   *state = COBALT_AUTH_SIGNED_IN;
   cobalt_session_set_message(r, "Label settings need the Wolfram build.");
}

#endif

bool
cobalt_session_begin_label_pref(const char *label, const char *visibility)
{
   cobalt_job_input in = {0};
   snprintf(in.uri, sizeof in.uri, "%s", label ? label : "");
   snprintf(in.label_value, sizeof in.label_value, "%s", visibility ? visibility : "");
   return cobalt_session_submit(COBALT_JOB_LABEL_PREF, &in);
}
