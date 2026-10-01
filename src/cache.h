#ifndef IPTVD_CACHE_H
#define IPTVD_CACHE_H
#include "common.h"
#include "json.h"

/* re-login under the global login lock; 0 = ok */
int cache_ensure_session(void);

/* ---- channels.json snapshot (auto reload on mtime/size change) ---- */
jv *cache_channels_dup(void);          /* deep copy jarr of channel objs; NULL if unreadable */
size_t cache_channels_count(void);     /* 0 if unreadable */
char *cache_channel_id(const char *uid); /* user_channel_id -> ChannelID (owned) or NULL */
long cache_channels_age(long *ttl_out);/* seconds; -1 if missing (also returns ttl) */
int cache_refresh_channels(int force); /* srcbox_bridge.refresh_channels; 0 = refreshed */

/* ---- programs cache (memory + /tmp/iptvd/<uid>_<yyyymmdd>.json) ---- */
/* returns owned jarr (caller jv_free); NULL only when nothing usable */
jv *cache_get_programs(const char *uid, const char *date,
                       int allow_stale, int retry_login);
void cache_load_disk(void);
void cache_purge_old(void);       /* drop entries/files older than keep window */
size_t cache_progs_count(void);

/* ---- tvod rtsp cache (TTL_TOD) ---- */
char *cache_tvod_get(const char *prevuecode);          /* owned copy or NULL */
void cache_tvod_put(const char *prevuecode, const char *rtsp);
void cache_tvod_clear(void);

#endif
