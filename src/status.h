#ifndef IPTVD_STATUS_H
#define IPTVD_STATUS_H
#include "common.h"

/* 7-line "/" text page (byte-compatible with srcbox_bridge) */
void status_page(dbuf *out);
/* JSON status (iptvd extension) */
void status_json(dbuf *out);
/* pin uptime start to serve startup (call once from server_run) */
void status_init(void);

#endif
