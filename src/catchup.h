#ifndef IPTVD_CATCHUP_H
#define IPTVD_CATCHUP_H
#include "common.h"

/* srcbox_bridge /c handler.
   raw_s / raw_u: query params (either may be NULL).
   200 -> *redirect filled; 400/404/502 -> *err filled with the python message */
int catchup_handle(const char *uid, const char *raw_s, const char *raw_u,
                   dbuf *redirect, char *err, size_t errsz);

/* exposed for tests */
int catchup_parse_ts(const char *raw, time_t *out);

#endif
