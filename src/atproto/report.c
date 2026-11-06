#include "atproto/report.h"

#include <stdio.h>
#include <string.h>

static const cobalt_report_reason REPORT_REASONS[] = {
   {"com.atproto.moderation.defs#reasonSpam", "Spam"},
   {"com.atproto.moderation.defs#reasonViolation", "Community rule violation"},
   {"com.atproto.moderation.defs#reasonMisleading", "Misleading content"},
   {"com.atproto.moderation.defs#reasonSexual", "Unwanted sexual content"},
   {"com.atproto.moderation.defs#reasonRude", "Harassment or rudeness"},
   {"com.atproto.moderation.defs#reasonOther", "Other"},
};

const cobalt_report_reason *
cobalt_report_reasons(int *count)
{
   if (count) {
      *count = (int) (sizeof REPORT_REASONS / sizeof REPORT_REASONS[0]);
   }
   return REPORT_REASONS;
}

bool
cobalt_report_reason_known(const char *reason_type)
{
   int count;
   const cobalt_report_reason *reasons = cobalt_report_reasons(&count);

   if (!reason_type) {
      return false;
   }
   for (int i = 0; i < count; i++) {
      if (strcmp(reasons[i].reason_type, reason_type) == 0) {
         return true;
      }
   }
   return false;
}

static void
set_reason(cobalt_report *out, const char *reason_type)
{
   if (reason_type && reason_type[0] && cobalt_report_reason_known(reason_type)) {
      snprintf(out->reason_type, sizeof out->reason_type, "%s", reason_type);
   }
}

bool
cobalt_report_post(cobalt_report *out, const char *uri, const char *cid,
                   const char *reason_type)
{
   if (!out) {
      return false;
   }
   memset(out, 0, sizeof *out);
   out->subject = COBALT_REPORT_POST;
   if (!uri || strncmp(uri, "at://", 5) != 0 || !cid || !cid[0] ||
       strlen(uri) >= sizeof out->uri || strlen(cid) >= sizeof out->cid) {
      memset(out, 0, sizeof *out);
      return false;
   }
   snprintf(out->uri, sizeof out->uri, "%s", uri);
   snprintf(out->cid, sizeof out->cid, "%s", cid);
   set_reason(out, reason_type);
   return true;
}

bool
cobalt_report_account(cobalt_report *out, const char *did, const char *reason_type)
{
   if (!out) {
      return false;
   }
   memset(out, 0, sizeof *out);
   out->subject = COBALT_REPORT_ACCOUNT;
   if (!did || strncmp(did, "did:", 4) != 0 || did[4] == '\0' ||
       strlen(did) >= sizeof out->did) {
      memset(out, 0, sizeof *out);
      return false;
   }
   snprintf(out->did, sizeof out->did, "%s", did);
   set_reason(out, reason_type);
   return true;
}

bool
cobalt_report_valid(const cobalt_report *report)
{
   if (!report || !cobalt_report_reason_known(report->reason_type)) {
      return false;
   }
   if (report->subject == COBALT_REPORT_POST) {
      return report->uri[0] && report->cid[0] && !report->did[0];
   }
   if (report->subject == COBALT_REPORT_ACCOUNT) {
      return report->did[0] && !report->uri[0] && !report->cid[0];
   }
   return false;
}
