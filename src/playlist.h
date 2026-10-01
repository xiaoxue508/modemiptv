#ifndef IPTVD_PLAYLIST_H
#define IPTVD_PLAYLIST_H
#include "common.h"

/* gen_full_m3u.py port.
   r2h_http=0 -> rtp2httpd input table (mcast/fcc/uc, 3 groups)
   r2h_http=1 -> SrcBox playlist (4 flat cats, multicast r2h paths)
   returns 0 and fills out + entry counts; -1 when channels.json unreadable */
int playlist_build(int r2h_http, dbuf *out, int *n_mcast, int *n_fcc, int *n_uc);

#endif
