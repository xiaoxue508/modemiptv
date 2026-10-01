#ifndef IPTVD_UPLINK_H
#define IPTVD_UPLINK_H
#include "common.h"

/* upstream_interface feature (empty g.upstream_interface = off).
   uplink_ensure() binds our source IP to the interface (used by http.c
   CURLOPT_INTERFACE + stbip=auto). uplink_policy=1 additionally programs
   a source-based rule (pref 1000 -> table 1001, single default via the
   interface gateway); uplink_policy=0 (modem) adds no routes — the
   platform IPoE connection already owns rule/table (from <iface-ip>
   lookup <IPTV-table>) and cspd reprograms it on connection changes. */
void uplink_ensure(int log_changes);   /* idempotent; start + every ~60s */
const char *uplink_ip(void);           /* current source IP or "" */
int uplink_dev_ok(const char *s);      /* name safe to embed in shell */

#endif
