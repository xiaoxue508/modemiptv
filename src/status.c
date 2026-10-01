#include "status.h"
#include "cache.h"
#include "epgxml.h"
#include "json.h"
#include "platform.h"
#include "uplink.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifndef IPTVD_VERSION
#define IPTVD_VERSION "0.2.0"
#endif

void status_page(dbuf *out)
{
    char stale[32], chs[96];
    struct stat st;

    if (stat(g.epg_file, &st) == 0)
        snprintf(stale, sizeof stale, "%lds ago", (long)(now_sec() - st.st_mtime));
    else
        snprintf(stale, sizeof stale, "?");

    if (stat(g.channels, &st) == 0) {
        long n = (long)cache_channels_count();
        long real = n;
        jv *dup = cache_channels_dup();
        if (!dup) real = -1;
        jv_free(dup);
        snprintf(chs, sizeof chs, "%lds ago (ttl %ds, %ld ch)",
                 (long)(now_sec() - st.st_mtime), g.ttl_channels, real);
    } else {
        snprintf(chs, sizeof chs, "?");
    }

    dbuf_addf(out,
        "srcbox_bridge\n"
        "  /playlist.m3u   SrcBox playlist (4 groups, multicast only, r2h paths)\n"
        "  /full.m3u       r2h input table (3 groups, real URLs)\n"
        "  /epg.xml        XMLTV (built: %s)\n"
        "  /c?ch=UID&s=SEC catchup -> 302 r2h\n"
        "  channels.json   refreshed: %s\n"
        "  programs cache entries: %zu\n",
        stale, chs, cache_progs_count());
}

static time_t g_start;

void status_init(void)
{
    /* monotonic: wall clock jumps (NTP initial sync right after boot)
       must not inflate the reported service uptime */
    g_start = mono_sec();
}

void status_json(dbuf *out)
{
    if (!g_start) g_start = mono_sec();

    jv *o = jobj();
    char num[32];

#define NUM(obj, field, v) do { \
        snprintf(num, sizeof num, "%ld", (long)(v)); \
        jobj_set(obj, field, jnum(num)); \
    } while (0)

    jobj_set(o, "version", jstr(IPTVD_VERSION));
    NUM(o, "uptime_s", mono_sec() - g_start);
    NUM(o, "port", g.port);

    jv *ch = jobj();
    struct stat st;
    if (stat(g.channels, &st) == 0) NUM(ch, "age_s", now_sec() - st.st_mtime);
    NUM(ch, "count", cache_channels_count());
    NUM(ch, "ttl_s", g.ttl_channels);
    jobj_set(o, "channels", ch);

    jv *e = jobj();
    if (stat(g.epg_file, &st) == 0) NUM(e, "built_s", now_sec() - st.st_mtime);
    jobj_set(o, "epg", e);
    jobj_set(o, "epg_ready", jbool(epgxml_ready()));

    NUM(o, "programs_cache", cache_progs_count());
    jobj_set(o, "session", jbool(g_sess.have));

    jv *u = jobj();
    jobj_set(u, "interface", jstr(g.upstream_interface));
    jobj_set(u, "ip", jstr(uplink_ip()));
    jobj_set(o, "uplink", u);
#undef NUM

    char *s = json_dump_str(o, 1);
    jv_free(o);
    dbuf_add(out, s);
    dbuf_addc(out, '\n');
    free(s);
}
