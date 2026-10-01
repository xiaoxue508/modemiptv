#include "cache.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------------- session ---------------- */

int cache_ensure_session(void)
{
    char err[512];
    err[0] = 0;
    if (plat_login(g.session, err, sizeof err) != 0) {
        logmsg("login failed: %s", err);
        return -1;
    }
    logmsg("session re-login ok");
    return 0;
}

/* ---------------- channels.json snapshot ---------------- */

static pthread_mutex_t ch_lock = PTHREAD_MUTEX_INITIALIZER;
static jv *g_chs;                 /* jarr of channel objects */
static char ch_key[96];

static void ch_stat_key(char *out, size_t n)
{
    struct stat st;
    if (stat(g.channels, &st) != 0) { snprintf(out, n, "missing"); return; }
    snprintf(out, n, "%lld.%ld:%lld", (long long)st.st_mtime,
             (long)st.st_mtim.tv_nsec, (long long)st.st_size);
}

static int ch_reload_locked(void)
{
    char key[96];
    ch_stat_key(key, sizeof key);
    if (g_chs && !strcmp(key, ch_key)) return 0;

    size_t len;
    char *raw = read_file(g.channels, &len);
    if (!raw) return -1;
    char *err = NULL;
    jv *data = json_parse(raw, len, &err);
    free(err);
    free(raw);
    if (!data) return -1;

    jv *arr = NULL;
    if (data->t == JARR) {
        arr = data;
    } else if (data->t == JOBJ) {
        const jv *c = jobj_get(data, "channels");
        if (c && c->t == JARR) arr = jv_clone(c);
        jv_free(data);
    } else {
        jv_free(data);
    }
    if (!arr) return -1;

    jv_free(g_chs);
    g_chs = arr;
    snprintf(ch_key, sizeof ch_key, "%s", key);
    return 0;
}

static void ch_invalidate(void)
{
    pthread_mutex_lock(&ch_lock);
    ch_key[0] = 0;
    pthread_mutex_unlock(&ch_lock);
}

jv *cache_channels_dup(void)
{
    pthread_mutex_lock(&ch_lock);
    jv *c = ch_reload_locked() == 0 ? jv_clone(g_chs) : NULL;
    pthread_mutex_unlock(&ch_lock);
    return c;
}

size_t cache_channels_count(void)
{
    pthread_mutex_lock(&ch_lock);
    size_t n = ch_reload_locked() == 0 && g_chs ? g_chs->n : 0;
    pthread_mutex_unlock(&ch_lock);
    return n;
}

long cache_channels_age(long *ttl_out)
{
    if (ttl_out) *ttl_out = g.ttl_channels;
    struct stat st;
    if (stat(g.channels, &st) != 0) return -1;
    return (long)(now_sec() - st.st_mtime);
}

/* ---------------- tvod cache ---------------- */

typedef struct { char *pc, *rtsp; long fetched; } tvod_ent;
static pthread_mutex_t tv_lock = PTHREAD_MUTEX_INITIALIZER;
static tvod_ent *g_tv;
static size_t g_tn, g_tcap;

char *cache_tvod_get(const char *prevuecode)
{
    char *out = NULL;
    pthread_mutex_lock(&tv_lock);
    for (size_t i = 0; i < g_tn; i++)
        if (!strcmp(g_tv[i].pc, prevuecode)) {
            if (now_sec() - g_tv[i].fetched < g.ttl_tvod)
                out = xstrdup(g_tv[i].rtsp);
            break;
        }
    pthread_mutex_unlock(&tv_lock);
    return out;
}

void cache_tvod_put(const char *prevuecode, const char *rtsp)
{
    pthread_mutex_lock(&tv_lock);
    for (size_t i = 0; i < g_tn; i++) {
        if (!strcmp(g_tv[i].pc, prevuecode)) {
            free(g_tv[i].rtsp);
            g_tv[i].rtsp = xstrdup(rtsp);
            g_tv[i].fetched = now_sec();
            pthread_mutex_unlock(&tv_lock);
            return;
        }
    }
    if (g_tn == g_tcap) {
        g_tcap = g_tcap ? g_tcap * 2 : 16;
        g_tv = xrealloc(g_tv, g_tcap * sizeof *g_tv);
    }
    g_tv[g_tn].pc = xstrdup(prevuecode);
    g_tv[g_tn].rtsp = xstrdup(rtsp);
    g_tv[g_tn].fetched = now_sec();
    g_tn++;
    pthread_mutex_unlock(&tv_lock);
}

void cache_tvod_clear(void)
{
    pthread_mutex_lock(&tv_lock);
    for (size_t i = 0; i < g_tn; i++) { free(g_tv[i].pc); free(g_tv[i].rtsp); }
    g_tn = 0;
    pthread_mutex_unlock(&tv_lock);
}

/* ---------------- programs cache ----------------
   Entries hold the progs JSON as *text* (not a parsed jv tree): the router
   only has ~35MB free and a parsed tree costs ~4x the raw JSON. The python
   bridge keeps parsed objects because the NAS has GBs; outputs are identical
   either way (get parses on hit, callers own the result). */

typedef struct { char key[96]; long fetched; char *pj; } prog_ent;
static pthread_mutex_t prog_lock = PTHREAD_MUTEX_INITIALIZER;
static prog_ent *g_pe;
static size_t g_pn, g_pcap;

static long prog_find(const char *key);

/* Keep today-(PROG_KEEP_DAYS-1)..today+epg_future program dates. XMLTV only
   needs today-1 and older catchup dates refetch on demand from the platform;
   the python bridge never prunes but the router has ~35MB RAM and /tmp is
   tmpfs (files ARE ram), so unbounded growth re-arms the OOM killer. */
#define PROG_KEEP_DAYS 3

static void prog_cutoff_ymd(char out[9])
{
    strftime8(out, 9, "%Y%m%d", now_sec() - (time_t)PROG_KEEP_DAYS * 86400);
}

/* key date part "YYYY.MM.DD" -> "YYYYMMDD"; 0 = malformed */
static int prog_key_ymd(const char *key, char out[9])
{
    const char *bar = strchr(key, '|');
    if (!bar) return 0;
    size_t j = 0;
    for (const char *p = bar + 1; *p && j < 8; p++)
        if (*p != '.') out[j++] = *p;
    out[j] = 0;
    return j == 8;
}

static jv *parse_progs_text(const char *t)
{
    if (!t) return NULL;
    char *err = NULL;
    jv *v = json_parse(t, strlen(t), &err);
    free(err);
    if (!v || v->t != JARR) { jv_free(v); return NULL; }
    return v;
}

/* compact json text; only_if_absent = python dict.setdefault */
static void prog_store(const char *key, long fetched, const jv *progs,
                       int only_if_absent)
{
    char *compact = json_dump_str(progs, -1);
    pthread_mutex_lock(&prog_lock);
    long idx = prog_find(key);
    if (idx < 0) {
        if (g_pn == g_pcap) {
            g_pcap = g_pcap ? g_pcap * 2 : 64;
            g_pe = xrealloc(g_pe, g_pcap * sizeof *g_pe);
        }
        snprintf(g_pe[g_pn].key, sizeof g_pe[g_pn].key, "%s", key);
        g_pe[g_pn].fetched = fetched;
        g_pe[g_pn].pj = compact;
        g_pn++;
        compact = NULL;
    } else if (!only_if_absent) {
        free(g_pe[idx].pj);
        g_pe[idx].pj = compact;
        g_pe[idx].fetched = fetched;
        compact = NULL;
    }
    pthread_mutex_unlock(&prog_lock);
    free(compact);
}

/* copy text out under the lock, parse outside; *stale_flag = was-stale */
static jv *prog_cache_take(const char *key, long now, int allow_stale,
                           int *stale_flag)
{
    char *text = NULL;
    int have = 0, fresh = 0;
    pthread_mutex_lock(&prog_lock);
    long idx = prog_find(key);
    if (idx >= 0) {
        have = 1;
        fresh = allow_stale || (now - g_pe[idx].fetched < (long)g.ttl_progs);
        if (g_pe[idx].pj) text = xstrdup(g_pe[idx].pj);
    }
    pthread_mutex_unlock(&prog_lock);
    if (!have) return NULL;
    jv *arr = parse_progs_text(text);
    free(text);
    if (!arr) arr = jarr();
    *stale_flag = !fresh;
    return arr;
}

static void prog_key(char *out, size_t n, const char *uid, const char *date)
{
    snprintf(out, n, "%s|%s", uid, date);
}

static long prog_find(const char *key)
{
    for (size_t i = 0; i < g_pn; i++)
        if (!strcmp(g_pe[i].key, key)) return (long)i;
    return -1;
}

static void prog_disk_write(const char *uid, const char *date, long fetched,
                            const jv *progs)
{
    char ymd[24];
    size_t j = 0;
    for (size_t i = 0; date[i] && j + 1 < sizeof ymd; i++)
        if (date[i] != '.') ymd[j++] = date[i];
    ymd[j] = 0;

    char path[320];
    snprintf(path, sizeof path, "%s/%s_%s.json", g.cache_dir, uid, ymd);
    jv *o = jobj();
    char num[32];
    snprintf(num, sizeof num, "%ld", fetched);
    jobj_set(o, "fetched", jnum(num));
    jobj_set(o, "progs", jv_clone(progs));
    char *s = json_dump_str(o, -1);
    jv_free(o);
    mkdir_p(g.cache_dir);
    write_atomic(path, s, strlen(s));
    free(s);
}

void cache_load_disk(void)
{
    char cut[9];
    prog_cutoff_ymd(cut);
    DIR *d = opendir(g.cache_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 14 || strcmp(e->d_name + n - 5, ".json")) continue;
        size_t sn = n - 5;
        if (sn < 10) continue;
        char stem[128];
        if (sn >= sizeof stem) continue;
        memcpy(stem, e->d_name, sn);
        stem[sn] = 0;
        char ymd[9];
        memcpy(ymd, stem + sn - 8, 8);
        ymd[8] = 0;
        if (strcmp(ymd, cut) < 0) {          /* rotated out: free the tmpfs */
            char old[320];
            snprintf(old, sizeof old, "%s/%s", g.cache_dir, e->d_name);
            unlink(old);
            continue;
        }
        char uid[64];
        size_t ul = sn - 9;
        if (ul == 0 || ul >= sizeof uid) continue;
        memcpy(uid, stem, ul);
        uid[ul] = 0;
        char date[20];
        snprintf(date, sizeof date, "%.4s.%.2s.%.2s", ymd, ymd + 4, ymd + 6);

        char path[320];
        snprintf(path, sizeof path, "%s/%s", g.cache_dir, e->d_name);
        size_t len;
        char *raw = read_file(path, &len);
        if (!raw) continue;
        char *err = NULL;
        jv *ent = json_parse(raw, len, &err);
        free(err);
        free(raw);
        if (!ent || ent->t != JOBJ) { jv_free(ent); continue; }
        const jv *pr = jobj_get(ent, "progs");
        if (!pr || pr->t != JARR) { jv_free(ent); continue; }

        char key[96];
        prog_key(key, sizeof key, uid, date);
        const jv *fw = jobj_get(ent, "fetched");
        long fetched = (fw && fw->t == JNUM && fw->num) ? atol(fw->num) : 0;
        prog_store(key, fetched, pr, 1);     /* setdefault */
        jv_free(ent);
    }
    closedir(d);
}

size_t cache_progs_count(void)
{
    pthread_mutex_lock(&prog_lock);
    size_t n = g_pn;
    pthread_mutex_unlock(&prog_lock);
    return n;
}

/* drop aged entries from memory AND unlink their tmpfs files; called on
   every XMLTV build (>= hourly) so long uptimes stay bounded too */
void cache_purge_old(void)
{
    char cut[9];
    prog_cutoff_ymd(cut);

    pthread_mutex_lock(&prog_lock);
    for (size_t i = 0; i < g_pn; ) {
        char ymd[9];
        if (prog_key_ymd(g_pe[i].key, ymd) && strcmp(ymd, cut) < 0) {
            free(g_pe[i].pj);
            g_pe[i] = g_pe[--g_pn];
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&prog_lock);

    DIR *d = opendir(g.cache_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 14 || strcmp(e->d_name + n - 5, ".json")) continue;
        char ymd[9];
        memcpy(ymd, e->d_name + n - 13, 8);
        ymd[8] = 0;
        if (strcmp(ymd, cut) < 0) {
            char path[320];
            snprintf(path, sizeof path, "%s/%s", g.cache_dir, e->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

/* channel uid -> ChannelID; owned string or NULL */
char *cache_channel_id(const char *uid)
{
    jv *chs = cache_channels_dup();
    if (!chs) return NULL;
    char *out = NULL;
    if (chs->t == JARR)
        for (size_t i = 0; i < chs->n; i++) {
            jv *c = chs->items[i];
            if (!c || c->t != JOBJ) continue;
            const jv *u = jobj_get(c, "user_channel_id");
            const jv *cid = jobj_get(c, "channel_id");
            if (u && u->t == JSTR && u->s && !strcmp(u->s, uid)) {
                if (cid && cid->t == JSTR && cid->s) out = xstrdup(cid->s);
                break;
            }
        }
    jv_free(chs);
    return out;
}

jv *cache_get_programs(const char *uid, const char *date,
                       int allow_stale, int retry_login)
{
    char key[96];
    prog_key(key, sizeof key, uid, date);
    int can_relogin = retry_login;
    jv *stale = NULL;

    for (;;) {
        long now = now_sec();
        int sf = 0;
        jv *got = prog_cache_take(key, now, allow_stale, &sf);
        if (got && !sf) { jv_free(stale); return got; }   /* fresh hit */
        jv_free(stale);
        stale = got;                                      /* may be NULL */

        char *chid = cache_channel_id(uid);
        jv *arr = chid ? plat_programs(chid, date) : NULL;
        free(chid);

        if (arr) {
            prog_store(key, now, arr, 0);
            prog_disk_write(uid, date, now, arr);
            jv_free(stale);
            return arr;
        }

        if (can_relogin) {
            can_relogin = 0;
            logmsg("programs(%s,%s) failed -> relogin+retry", uid, date);
            if (cache_ensure_session() != 0)
                return stale ? stale : (jv *)jarr();
            continue;
        }
        return stale ? stale : (jv *)jarr();
    }
}

/* ---------------- channels refresher ---------------- */

static pthread_mutex_t refresh_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t chan_count_of(const jv *data)
{
    if (!data) return 0;
    if (data->t == JARR) return data->n;
    if (data->t == JOBJ) {
        const jv *c = jobj_get(data, "channels");
        if (c && c->t == JARR) return c->n;
        return data->nm;
    }
    return 0;
}

int cache_refresh_channels(int force)
{
    if (g.ttl_channels <= 0 && !force) return 0;
    int refreshed = 0;

    pthread_mutex_lock(&refresh_lock);
    long age = cache_channels_age(NULL);
    if (!force && age >= 0 && age < g.ttl_channels) {
        pthread_mutex_unlock(&refresh_lock);
        return 0;
    }

    /* session dies after ~3h and signed RTSP urls inside channels.json carry
       it: re-login BEFORE signing so every published table is born fresh */
    if (cache_ensure_session() == 0) {
        cache_tvod_clear();
        logmsg("pre-refresh re-login ok (tvod cache cleared)");
    } else {
        logmsg("pre-refresh re-login failed, will try old session");
    }

    char last[400];
    last[0] = 0;

    for (int attempt = 1; attempt <= 2; attempt++) {
        if (attempt == 2 && cache_ensure_session() != 0) {
            snprintf(last, sizeof last, "relogin failed");
            continue;
        }
        plat_invalidate_frameset();
        dbuf out;
        dbuf_init(&out);
        if (plat_channels_json(&out, NULL) != 0) {
            snprintf(last, sizeof last, "channels fetch failed");
            dbuf_free(&out);
            continue;
        }
        char *err = NULL;
        jv *data = json_parse(out.p ? out.p : "", out.len, &err);
        free(err);
        if (!data) {
            snprintf(last, sizeof last, "channels json parse failed");
            dbuf_free(&out);
            continue;
        }
        size_t n = chan_count_of(data);
        jv_free(data);
        if ((int)n < g.min_channels) {
            snprintf(last, sizeof last, "only %d channels (login page?)", (int)n);
            dbuf_free(&out);
            continue;
        }
        mkdir_p(g.data_dir);
        if (!out.p || write_atomic(g.channels, out.p, out.len) != 0) {
            snprintf(last, sizeof last, "write %s failed", g.channels);
            dbuf_free(&out);
            continue;
        }
        dbuf_free(&out);
        ch_invalidate();
        logmsg("channels refreshed: %d channels (was %lds old)",
               (int)n, age < 0 ? -1 : age);
        refreshed = 1;
        break;
    }
    if (!refreshed) {
        logmsg("channels refresh FAILED (kept old file): %s", last);
    }
    pthread_mutex_unlock(&refresh_lock);
    return refreshed;
}
