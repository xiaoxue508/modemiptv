#ifndef IPTVD_COMMON_H
#define IPTVD_COMMON_H
#include <stddef.h>
#include <time.h>
#include <stdint.h>

/* ---------------- dynamic buffer ---------------- */
typedef struct { char *p; size_t len, cap; } dbuf;

void dbuf_init(dbuf *b);
void dbuf_free(dbuf *b);
void dbuf_addn(dbuf *b, const void *d, size_t n);
void dbuf_add(dbuf *b, const char *s);
void dbuf_addc(dbuf *b, char c);
void dbuf_addf(dbuf *b, const char *fmt, ...);
char *dbuf_steal(dbuf *b);   /* NUL-terminate, transfer ownership, reset b */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* log line "[HH:MM:SS] msg" on stdout, flushed (procd captures it) */
void logmsg(const char *fmt, ...);

/* time helpers: fixed UTC+8, never reads TZ env (port-spec R16) */
void gm8(time_t t, struct tm *out);
time_t mk8(const struct tm *in);
time_t now_sec(void);
time_t mono_sec(void); /* CLOCK_MONOTONIC seconds; immune to NTP clock jumps */
void strftime8(char *out, size_t n, const char *fmt, time_t t);

int utf8_valid(const void *d, size_t n);

/* quote_plus (space -> '+', unreserved kept) -> malloc */
char *urlenc(const char *s);
void urlenc_to(dbuf *out, const char *s);
/* urllib.parse.quote: unreserved kept + extra safe chars */
void urlquote_to(dbuf *out, const char *s, const char *extra_safe);

int write_atomic(const char *path, const void *data, size_t n);
int mkdir_p(const char *path);
char *read_file(const char *path, size_t *len);   /* NUL-terminated; NULL if missing */

typedef struct { const char *k, *v; } kv;

/* ---------------- config (/userconfig/iptv/iptvd.conf, key=value) --------- */
typedef struct {
    char eas_host[64], epg_host[64];
    char eas_url[80], epg_base[80];          /* http://host:8080 */
    char userid[32], stbid[64], stbip[32], stbmac[24], stbmac_plain[24];
    char auth_key[16], stbtype[32], stbversion[64];
    char ua[64], xhr[64];
    int timeout;
    /* endpoints / templates (port-spec R22: NAS 地址全部可配) */
    char r2h[128];                /* http://192.168.123.1:5141 */
    char m3u_epg_url[160];        /* x-tvg-url */
    char bridge_tpl[240];         /* catchup-source template with {uid} */
    char gen_url[128];            /* XMLTV generator-info-url */
    int ttl_progs, ttl_tvod, ttl_epg, ttl_channels, min_channels;
    int epg_past, epg_future, worker_s, port, xmltv_wait_s;
    char upstream_interface[32];    /* bind+route platform traffic ("" = off) */
    int uplink_policy;              /* 0=bind only (modem: rule/table12 exists)
                                        1=program table1001 policy routes */
    char time_url[128];             /* "" = derive http://<epg_host>:8080/ */
    int stbip_auto;                 /* conf stbip=auto -> take interface addr */
    char data_dir[96], cache_dir[96];
    char session[160], channels[160], epg_file[160];
} conf_t;

extern conf_t g;
void conf_defaults(void);
int conf_load(const char *path);
void conf_resolve_paths(void);
int conf_valid_key(const char *key);   /* known conf keys (set whitelist) */
int conf_key_is_num(const char *key);  /* integer-valued conf keys */

#endif
