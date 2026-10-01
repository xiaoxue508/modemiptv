#include "catchup.h"
#include "cache.h"
#include "json.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------- _parse_ts ---------------- */

static int digits_only(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* strict "NNNN" + sep consumption helper */
static int take2(const char **pp, int *out)
{
    const char *p = *pp;
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return -1;
    *out = (p[0] - '0') * 10 + (p[1] - '0');
    *pp = p + 2;
    return 0;
}

/* ISO-8601 subset of datetime.fromisoformat + astimezone(TZ8).
   naive datetimes are treated as UTC+8 (China deployment). */
static int parse_iso(const char *s, time_t *out)
{
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, se = 0;
    const char *p = s;
    char *end = NULL;
    y = (int)strtol(p, &end, 10);
    if (end - p != 4 || *end != '-') return -1;
    p = end + 1;
    if (take2(&p, &mo) != 0 || *p != '-') return -1;
    p++;
    if (take2(&p, &d) != 0) return -1;
    if (*p != 'T' && *p != 't' && *p != ' ') return -1;
    p++;
    if (take2(&p, &hh) != 0 || *p != ':') return -1;
    p++;
    if (take2(&p, &mi) != 0) return -1;
    if (*p == ':') {
        p++;
        if (take2(&p, &se) != 0) return -1;
    }
    if (*p == '.') {                     /* .ffffff */
        p++;
        if (!isdigit((unsigned char)*p)) return -1;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (y < 1970 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        hh > 23 || mi > 59 || se > 60)
        return -1;

    int off_s = 8 * 3600;                    /* default UTC+8 */
    if (*p == 'Z' || *p == 'z') {
        p++;
        off_s = 0;
    } else if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1;
        p++;
        int oh, om = 0;
        if (p[0] && p[1] && p[2] == ':') {
            if (take2(&p, &oh) != 0 || *p != ':') return -1;
            p++;
            if (take2(&p, &om) != 0) return -1;
        } else {
            if (take2(&p, &oh) != 0) return -1;
            if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1])) {
                if (take2(&p, &om) != 0) return -1;
            }
        }
        if (oh > 23 || om > 59) return -1;
        off_s = sign * (oh * 3600 + om * 60);
    }
    if (*p) return -1;

    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mi;
    tm.tm_sec = se;
    time_t epoch = timegm(&tm);
    if (epoch == (time_t)-1) return -1;
    *out = epoch - off_s;
    return 0;
}

int catchup_parse_ts(const char *raw, time_t *out)
{
    if (!raw) return -1;
    while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') raw++;
    char buf[80];
    snprintf(buf, sizeof buf, "%s", raw);
    size_t n = strlen(buf);
    while (n && (buf[n-1] == ' ' || buf[n-1] == '\t' ||
                 buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = 0;
    if (!n) return -1;

    if (digits_only(buf)) {
        if (n == 14) {
            struct tm tm;
            memset(&tm, 0, sizeof tm);
            if (!strptime(buf, "%Y%m%d%H%M%S", &tm)) return -1;
            *out = mk8(&tm);
            return 0;
        }
        if (n == 10 || n == 11 || n == 13) {
            long long v = atoll(buf);
            if (n == 13) v /= 1000;
            if (v < 0) return -1;
            struct tm tm;
            gm8((time_t)v, &tm);
            if (tm.tm_year < 70 || tm.tm_year > 2100 - 1900) return -1;
            *out = (time_t)v;
            return 0;
        }
        return -1;
    }
    return parse_iso(buf, out);
}

/* ---------------- find / tvod / wrap ---------------- */

typedef struct {
    char prevuecode[64];
    char prevuename[256];
    time_t b, e;
    char date[16];
    int found;
} prog_hit;

/* find_program: dates = [t, t-1d, t+1d] */
static void find_program(const char *uid, time_t t, prog_hit *hit)
{
    memset(hit, 0, sizeof *hit);
    for (int k = 0; k <= 2; k++) {
        int off = k == 0 ? 0 : (k == 1 ? -1 : 1);
        char date[16];
        strftime8(date, sizeof date, "%Y.%m.%d", t + (time_t)off * 86400);
        jv *progs = cache_get_programs(uid, date, 0, 1);
        if (progs && progs->t == JARR)
            for (size_t i = 0; i < progs->n; i++) {
                jv *p = progs->items[i];
                if (!p || p->t != JOBJ) continue;
                const jv *bv = jobj_get(p, "begintime");
                const jv *ev = jobj_get(p, "endtime");
                time_t b, e;
                if (!bv || bv->t != JSTR || !ev || ev->t != JSTR) continue;
                if (strlen(bv->s) != 19 || strlen(ev->s) != 19) continue;
                struct tm tmb, tme;
                memset(&tmb, 0, sizeof tmb);
                memset(&tme, 0, sizeof tme);
                if (!strptime(bv->s, "%Y.%m.%d %H:%M:%S", &tmb)) continue;
                if (!strptime(ev->s, "%Y.%m.%d %H:%M:%S", &tme)) continue;
                b = mk8(&tmb);
                e = mk8(&tme);
                if (b <= t && t < e) {
                    const jv *pc = jobj_get(p, "prevuecode");
                    const jv *pn = jobj_get(p, "prevuename");
                    snprintf(hit->prevuecode, sizeof hit->prevuecode, "%s",
                             pc && pc->t == JSTR && pc->s ? pc->s : "");
                    snprintf(hit->prevuename, sizeof hit->prevuename, "%s",
                             pn && pn->t == JSTR && pn->s ? pn->s : "");
                    hit->b = b;
                    hit->e = e;
                    snprintf(hit->date, sizeof hit->date, "%s", date);
                    hit->found = 1;
                    jv_free(progs);
                    return;
                }
            }
        jv_free(progs);
    }
}

/* resolve_tvod: cache -> per-date plat_tvod -> relogin once */
static char *resolve_tvod(const char *pc, const char *uid,
                          const char **date_cands, int ncands, int retry_login)
{
    char *hit = cache_tvod_get(pc);
    if (hit) return hit;

    char *chid = cache_channel_id(uid);
    char last[320];
    last[0] = 0;

    for (int i = 0; i < ncands; i++) {
        if (!chid) {
            snprintf(last, sizeof last, "channel %s not in channels.json", uid);
            continue;
        }
        dbuf out;
        dbuf_init(&out);
        int rc = plat_tvod(pc, chid, uid, "1D04", date_cands[i], 0, &out);
        if (rc == 0 && out.p && !strncmp(out.p, "rtsp://", 7)) {
            char *rtsp = xstrdup(out.p);
            dbuf_free(&out);
            free(chid);
            cache_tvod_put(pc, rtsp);
            return rtsp;
        }
        snprintf(last, sizeof last, "%s", out.p && *out.p ? out.p : "no rtsp");
        dbuf_free(&out);
    }
    free(chid);

    if (retry_login) {
        logmsg("tvod(%s) failed (%s) -> relogin+retry", pc, last);
        if (cache_ensure_session() != 0) return NULL;
        return resolve_tvod(pc, uid, date_cands, ncands, 0);
    }
    logmsg("tvod(%s) final failure: %s", pc, last);
    return NULL;
}

/* wrap_r2h: rtsp://host/path -> r2h/rtsp/host/path[?|&]r2h-start=N */
static int wrap_r2h(const char *rtsp, long offset, dbuf *out)
{
    const char *rest = rtsp + 7;
    const char *slash = strchr(rest, '/');
    if (!slash) return -1;
    dbuf_addf(out, "%s/rtsp/%.*s%s", g.r2h, (int)(slash - rest), rest, slash);
    if (offset > 0)
        dbuf_addf(out, "%sr2h-start=%ld", strchr(out->p, '?') ? "&" : "?", offset);
    return 0;
}

/* ---------------- entry ---------------- */

int catchup_handle(const char *uid, const char *raw_s, const char *raw_u,
                   dbuf *redirect, char *err, size_t errsz)
{
    time_t t;
    if (catchup_parse_ts(raw_s, &t) != 0 && catchup_parse_ts(raw_u, &t) != 0) {
        snprintf(err, errsz, "bad s");
        return 400;
    }

    int uid_ok = uid && *uid;
    if (uid_ok) {
        jv *chs = cache_channels_dup();
        uid_ok = 0;
        if (chs && chs->t == JARR)
            for (size_t i = 0; i < chs->n; i++) {
                jv *c = chs->items[i];
                if (!c || c->t != JOBJ) continue;
                const jv *u = jobj_get(c, "user_channel_id");
                const char *us = u && u->t == JSTR ? u->s : NULL;
                if (u && u->t == JNUM && u->num) us = u->num;
                if (us && !strcmp(us, uid)) { uid_ok = 1; break; }
            }
        jv_free(chs);
    }
    if (!uid_ok) {
        snprintf(err, errsz, "unknown channel '%s'", uid ? uid : "");
        return 404;
    }

    prog_hit h;
    find_program(uid, t, &h);
    if (!h.found) {
        char when[24];
        strftime8(when, sizeof when, "%Y-%m-%d %H:%M:%S", t);
        snprintf(err, errsz, "no program for %s at %s", uid, when);
        return 404;
    }
    if (!h.prevuecode[0]) {
        snprintf(err, errsz, "program without prevuecode");
        return 404;
    }

    /* tvod --date must match the program day; try begintime day first */
    char d_beg[16], d_t[16];
    strftime8(d_beg, sizeof d_beg, "%Y.%m.%d", h.b);
    strftime8(d_t, sizeof d_t, "%Y.%m.%d", t);
    const char *cands[3];
    int nc = 0;
    cands[nc++] = d_beg;
    cands[nc++] = h.date;
    if (strcmp(d_t, d_beg) && strcmp(d_t, h.date)) cands[nc++] = d_t;

    char *rtsp = resolve_tvod(h.prevuecode, uid, cands, nc, 1);
    if (!rtsp) {
        snprintf(err, errsz, "tvod resolve failed for %s (%s)",
                 h.prevuecode, h.prevuename);
        return 502;
    }

    long offset = (long)(t - h.b);
    if (offset < 0) offset = 0;
    long span = (long)(h.e - h.b);
    if (offset > span - 30) offset = span - 30 > 0 ? span - 30 : 0;

    dbuf r2h;
    dbuf_init(&r2h);
    if (wrap_r2h(rtsp, offset, &r2h) != 0) {
        snprintf(err, errsz, "bad rtsp: %s", rtsp);
        dbuf_free(&r2h);
        free(rtsp);
        return 502;
    }
    free(rtsp);

    char bm[16];
    strftime8(bm, sizeof bm, "%m-%d %H:%M", h.b);
    logmsg("catchup ch=%s %s %s -> off=%lds", uid, h.prevuename, bm, offset);

    dbuf_add(redirect, r2h.p ? r2h.p : "");
    dbuf_free(&r2h);
    return 200;
}
