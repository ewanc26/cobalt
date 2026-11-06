#pragma once

/*
 * What a moderation report is about and why. Pure data and checks only: the
 * session worker turns this into Wolfram's createReport call, and the menu
 * builds its reason list from the same table, so the two cannot disagree.
 */

#include "atproto/feed.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_REPORT_REASON_MAX 64
#define COBALT_REPORT_DID_MAX   128

typedef enum {
   COBALT_REPORT_POST = 0,
   COBALT_REPORT_ACCOUNT,
} cobalt_report_subject;

typedef struct {
   const char *reason_type; /* the full com.atproto.moderation.defs NSID */
   const char *label;       /* what the menu shows */
} cobalt_report_reason;

/* One report ready to send. Exactly one of the post fields and the DID is
 * filled, according to `subject`. */
typedef struct {
   cobalt_report_subject subject;
   char uri[COBALT_POST_URI_MAX];
   char cid[COBALT_POST_CID_MAX];
   char did[COBALT_REPORT_DID_MAX];
   char reason_type[COBALT_REPORT_REASON_MAX];
} cobalt_report;

/* The supported reasons, in menu order. `count` is optional. */
const cobalt_report_reason *cobalt_report_reasons(int *count);

/* True when `reason_type` is one of the standard reason NSIDs above. */
bool cobalt_report_reason_known(const char *reason_type);

/* Fill a target, with `reason_type` optional so the menu can hold the target
 * while it asks for a reason. Both return false and clear `out` on a malformed
 * target. */
bool cobalt_report_post(cobalt_report *out, const char *uri, const char *cid,
                        const char *reason_type);
bool cobalt_report_account(cobalt_report *out, const char *did,
                           const char *reason_type);

/* True when the subject is complete and the reason is supported. */
bool cobalt_report_valid(const cobalt_report *report);

#ifdef __cplusplus
}
#endif
