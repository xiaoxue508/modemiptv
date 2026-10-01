#ifndef IPTVD_PLATFORM_H
#define IPTVD_PLATFORM_H
#include <pthread.h>
#include "common.h"
#include "json.h"

typedef struct {
    char user_token[512];
    char user_grp[16];
    char epg_grp[16];
    int have;
} session_t;

extern session_t g_sess;
extern pthread_mutex_t g_login_lock;   /* R07: 登录路径全局互斥 */

/* C1+C2 -> challenge (epg.py:129-141) */
int plat_challenge(char *out, size_t n);

/* sign hex (epg.py:143-153), rnd 8 digits; rnd==NULL -> random */
char *plat_sign(const char *challenge, const char *rnd);

/* full login C1..C6 (epg.py:174-233); save_path may be NULL */
int plat_login(const char *save_path, char *errbuf, size_t errsz);

int plat_load_session(const char *path);
int plat_save_session(const char *path);

/* frameset channel table -> channels.json bytes; *count = number of channels */
int plat_channels_json(dbuf *out, int *count);
/* jsSetConfig('Channel','...') rows -> jarr of row objects (caller frees) */
jv *plat_parse_channels(const char *text);

/* programs (epg.py:260-268) -> jarr of program objects; caller frees */
jv *plat_programs(const char *channel_code, const char *date);

/* tvod (epg.py:270-283) -> rtsp url into out */
int plat_tvod(const char *prevuecode, const char *channel_code,
              const char *mixno, const char *columncode, const char *date,
              int isfromchannel, dbuf *out);

/* drop frameset cache (forces next channels to re-POST frameset_builder) */
void plat_invalidate_frameset(void);

#endif
