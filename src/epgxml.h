#ifndef IPTVD_EPGXML_H
#define IPTVD_EPGXML_H
#include "common.h"

/* build XMLTV from programs (srcbox_bridge.build_xmltv)
   returns 0 built, 1 another builder is running, -1 unreadable channels */
int epgxml_build(void);

/* one worker pass: rebuild when older than ttl_epg, then mark ready */
void epgxml_worker_once(void);
/* infinite loop: worker_once + sleep(worker_s) — run in a thread */
void epgxml_worker_loop(void);
/* ask the worker to rebuild on its next pass (LuCI "重建XMLTV" action) */
void epgxml_kick(void);

/* ready = serve /epg.xml without waiting */
int epgxml_ready(void);
void epgxml_mark_ready(void);
/* block until ready or timeout; returns ready state */
int epgxml_wait_ready(int timeout_s);

#endif
