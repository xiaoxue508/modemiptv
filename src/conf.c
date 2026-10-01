#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

conf_t g;

void conf_defaults(void)
{
    memset(&g, 0, sizeof g);
    strcpy(g.eas_host, "124.132.240.38");
    strcpy(g.epg_host, "60.212.113.86");
    strcpy(g.userid, "053502686836");
    strcpy(g.stbid, "000004370020A32000EF6CEFC689337E");
    strcpy(g.stbip, "10.156.22.20");
    strcpy(g.stbmac, "6C:EF:C6:89:33:7E");
    strcpy(g.auth_key, "68682600");
    strcpy(g.stbtype, "S-010W-AV2A");
    strcpy(g.stbversion, "S-010W-AV2A_SW_SD_A_R2.03.19");
    strcpy(g.ua, "webkit;Resolution(PAL,720P,1080P)");
    strcpy(g.xhr, "com.android.smart.terminal.iptv");
    g.timeout = 15;
    strcpy(g.r2h, "http://192.168.123.1:5141");
    strcpy(g.m3u_epg_url, "http://192.168.1.1:5150/epg.xml");
    strcpy(g.bridge_tpl, "http://192.168.1.1:5150/c?ch={uid}&s=${(b)yyyyMMddHHmmss}&u=${timestamp}");
    strcpy(g.gen_url, "http://192.168.1.1:5150");
    g.ttl_progs = 900;
    g.ttl_tvod = 3600;
    g.ttl_epg = 6 * 3600;
    g.ttl_channels = 7200;
    g.min_channels = 150;
    g.epg_past = 1;
    g.epg_future = 1;
    g.worker_s = 120;
    g.port = 5150;
    g.xmltv_wait_s = 180;
    strcpy(g.data_dir, "/userconfig/iptv");
    strcpy(g.cache_dir, "/tmp/iptvd");
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    chomp(s);
    return s;
}

#define SETSTR(field, val) do { snprintf(g.field, sizeof g.field, "%s", val); } while (0)

static void set_kv(const char *key, const char *val)
{
    if (!strcmp(key, "eas_host")) SETSTR(eas_host, val);
    else if (!strcmp(key, "epg_host")) SETSTR(epg_host, val);
    else if (!strcmp(key, "userid")) SETSTR(userid, val);
    else if (!strcmp(key, "stbid")) SETSTR(stbid, val);
    else if (!strcmp(key, "stbip")) {
        if (!strcmp(val, "auto")) { g.stbip_auto = 1; g.stbip[0] = 0; }
        else { g.stbip_auto = 0; SETSTR(stbip, val); }
    }
    else if (!strcmp(key, "stbmac")) SETSTR(stbmac, val);
    else if (!strcmp(key, "auth_key")) SETSTR(auth_key, val);
    else if (!strcmp(key, "stbtype")) SETSTR(stbtype, val);
    else if (!strcmp(key, "stbversion")) SETSTR(stbversion, val);
    else if (!strcmp(key, "ua")) SETSTR(ua, val);
    else if (!strcmp(key, "xhr")) SETSTR(xhr, val);
    else if (!strcmp(key, "timeout")) g.timeout = atoi(val);
    else if (!strcmp(key, "r2h")) SETSTR(r2h, val);
    else if (!strcmp(key, "m3u_epg_url")) SETSTR(m3u_epg_url, val);
    else if (!strcmp(key, "bridge_tpl")) SETSTR(bridge_tpl, val);
    else if (!strcmp(key, "gen_url")) SETSTR(gen_url, val);
    else if (!strcmp(key, "ttl_progs")) g.ttl_progs = atoi(val);
    else if (!strcmp(key, "ttl_tvod")) g.ttl_tvod = atoi(val);
    else if (!strcmp(key, "ttl_epg")) g.ttl_epg = atoi(val);
    else if (!strcmp(key, "ttl_channels")) g.ttl_channels = atoi(val);
    else if (!strcmp(key, "min_channels")) g.min_channels = atoi(val);
    else if (!strcmp(key, "epg_past")) g.epg_past = atoi(val);
    else if (!strcmp(key, "epg_future")) g.epg_future = atoi(val);
    else if (!strcmp(key, "worker_s")) g.worker_s = atoi(val);
    else if (!strcmp(key, "port")) g.port = atoi(val);
    else if (!strcmp(key, "xmltv_wait_s")) g.xmltv_wait_s = atoi(val);
    else if (!strcmp(key, "upstream_interface")) SETSTR(upstream_interface, val);
    else if (!strcmp(key, "uplink_policy")) g.uplink_policy = atoi(val);
    else if (!strcmp(key, "data_dir")) SETSTR(data_dir, val);
    else if (!strcmp(key, "cache_dir")) SETSTR(cache_dir, val);
    else if (!strcmp(key, "session")) SETSTR(session, val);
    else if (!strcmp(key, "channels")) SETSTR(channels, val);
    else if (!strcmp(key, "epg_file")) SETSTR(epg_file, val);
    /* unknown keys ignored (forward compatible) */
}

int conf_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == '\n' || *s == '\r' || *s == 0) continue;
        chomp(s);
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);
        /* strip inline comment " # ..." only when preceded by space */
        char *hash = strstr(val, " #");
        if (hash) { *hash = 0; chomp(val); }
        set_kv(key, val);
    }
    fclose(f);
    return 0;
}

static const char *const conf_keys[] = {
    "eas_host", "epg_host", "userid", "stbid", "stbip", "stbmac", "auth_key",
    "stbtype", "stbversion", "ua", "xhr", "timeout", "r2h", "m3u_epg_url",
    "bridge_tpl", "gen_url", "ttl_progs", "ttl_tvod", "ttl_epg",
    "ttl_channels", "min_channels", "epg_past", "epg_future", "worker_s",
    "port", "xmltv_wait_s", "upstream_interface", "uplink_policy", "data_dir",
    "cache_dir", "session", "channels", "epg_file", NULL
};

int conf_valid_key(const char *key)
{
    for (int i = 0; conf_keys[i]; i++)
        if (!strcmp(conf_keys[i], key)) return 1;
    return 0;
}

int conf_key_is_num(const char *key)
{
    return !strcmp(key, "timeout") || !strcmp(key, "ttl_progs") ||
           !strcmp(key, "ttl_tvod") || !strcmp(key, "ttl_epg") ||
           !strcmp(key, "ttl_channels") || !strcmp(key, "min_channels") ||
           !strcmp(key, "epg_past") || !strcmp(key, "epg_future") ||
           !strcmp(key, "worker_s") || !strcmp(key, "port") ||
           !strcmp(key, "xmltv_wait_s") || !strcmp(key, "uplink_policy");
}

void conf_resolve_paths(void)
{
    snprintf(g.eas_url, sizeof g.eas_url, "http://%s:8080", g.eas_host);
    snprintf(g.epg_base, sizeof g.epg_base, "http://%s:8080", g.epg_host);
    /* STBMAC_PLAIN = STBMAC without colons (epg.py:65) */
    size_t k = 0;
    for (size_t i = 0; g.stbmac[i] && k + 1 < sizeof g.stbmac_plain; i++)
        if (g.stbmac[i] != ':') g.stbmac_plain[k++] = g.stbmac[i];
    g.stbmac_plain[k] = 0;
    if (!g.session[0]) snprintf(g.session, sizeof g.session, "%s/session.json", g.data_dir);
    if (!g.channels[0]) snprintf(g.channels, sizeof g.channels, "%s/channels.json", g.data_dir);
    if (!g.epg_file[0]) snprintf(g.epg_file, sizeof g.epg_file, "%s/srcbox_epg.xml", g.cache_dir);
}
