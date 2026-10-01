#ifndef IPTVD_SERVER_H
#define IPTVD_SERVER_H

/* blocking HTTP daemon (srcbox_bridge port). returns only on bind failure */
int server_run(void);

#endif
