#pragma once

/* Build identity, stamped by the Makefile (src/util/buildinfo_gen.h). Hosts that
 * never ran the Makefile get "unknown" rather than a build failure. */

#ifdef __cplusplus
extern "C" {
#endif

const char *cobalt_version(void);        /* "0.2.0" */
const char *cobalt_build_commit(void);   /* short hash, "-dirty" if modified */
int cobalt_build_number(void);           /* commits on the branch; 0 unknown */
const char *cobalt_build_date(void);
/* "v0.2.0 - build 128 - b832c46 - 2026-10-03" */
const char *cobalt_build_describe(void);

#ifdef __cplusplus
}
#endif
