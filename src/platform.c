#define _GNU_SOURCE
#include "platform.h"
#include "http.h"
#include "des.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

session_t g_sess;
pthread_mutex_t g_login_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t frameset_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_frameset = NULL;          /* utf-8 frameset_builder output */

void plat_invalidate_frameset(void)
{
    pthread_mutex_lock(&frameset_lock);
    free(g_frameset);
    g_frameset = NULL;
    pthread_mutex_unlock(&frameset_lock);
}

/* ---------------- headers ---------------- */
static const kv *bh(void)
{
    static kv h[5];
    h[0].k = "User-Agent"; h[0].v = g.ua;
    h[1].k = "Accept";
    h[1].v = "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8";
    h[2].k = "Accept-Language"; h[2].v = "zh-CN,en-US;q=0.8";
    h[3].k = "X-Requested-With"; h[3].v = g.xhr;
    h[4].k = NULL; h[4].v = NULL;
    return h;
}

/* merge base headers with extras ({NULL,NULL}-terminated); caller frees */
static kv *merge_hdr(const kv *extra)
{
    int n = 0;
    while (bh()[n].k) n++;
    int m = 0;
    if (extra) while (extra[m].k) m++;
    kv *out = xmalloc(sizeof(kv) * (size_t)(n + m + 1));
    for (int i = 0; i < n; i++) out[i] = bh()[i];
    for (int i = 0; i < m; i++) out[n + i] = extra[i];
    out[n + m].k = NULL;
    out[n + m].v = NULL;
    return out;
}

static void addh(kv *arr, int idx, const char *k, const char *v)
{
    arr[idx].k = k;
    arr[idx].v = v;
}

/* ---------------- fetch helpers ---------------- */
static int get_text(const char *url, const kv *extra, dbuf *text, http_resp *r_out)
{
    kv *hdr = merge_hdr(extra);
    int rc = http_get(url, hdr, r_out);
    free(hdr);
    if (rc != 0) return -1;
    http_text(r_out, text);
    return 0;
}

static int post_text(const char *url, const char *body, const kv *extra,
                     dbuf *text, http_resp *r_out)
{
    kv hdr[16];
    int i = 0;
    if (extra)
        for (int j = 0; extra[j].k && i < 13; j++) addh(hdr, i++, extra[j].k, extra[j].v);
    addh(hdr, i++, "Content-Type", "application/x-www-form-urlencoded");
    hdr[i].k = NULL;
    kv *merged = merge_hdr(hdr);
    int rc = http_post(url, body, strlen(body), merged, r_out);
    free(merged);
    if (rc != 0) return -1;
    http_text(r_out, text);
    return 0;
}

/* find "prefix" then read up to (not including) the next quote character */
static int extract_quoted(const char *text, const char *prefix, char *out, size_t n)
{
    const char *p = strstr(text, prefix);
    if (!p) return -1;
    p += strlen(prefix);
    const char *q = strchr(p, '\'');
    if (!q) return -1;
    size_t len = (size_t)(q - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return 0;
}

static int extract_digits(const char *text, const char *prefix, char *out, size_t n)
{
    const char *p = strstr(text, prefix);
    if (!p) return -1;
    p += strlen(prefix);
    size_t k = 0;
    while (p[k] >= '0' && p[k] <= '9') k++;
    if (!k || p[k] != '\'') return -1;
    if (k >= n) k = n - 1;
    memcpy(out, p, k);
    out[k] = 0;
    return 0;
}

/* ---------------- rnd / sign ---------------- */
static void make_rnd(char *out /* 9 bytes */)
{
    unsigned int v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) v = (unsigned)rand();
        close(fd);
    } else {
        v = (unsigned)rand();
    }
    snprintf(out, 9, "%08u", v % 100000000u);
}

char *plat_sign(const char *challenge, const char *rnd)
{
    char rbuf[9];
    if (!rnd) { make_rnd(rbuf); rnd = rbuf; }
    char *out = xmalloc(4096);
    des_sign(challenge, rnd, g.userid, g.stbid, g.stbip, g.stbmac_plain,
             g.auth_key, "$$CTC", out, 4096);
    return out;
}

/* ---------------- C1/C2 ---------------- */
int plat_challenge(char *out, size_t n)
{
    dbuf url, text;
    dbuf_init(&url);
    dbuf_init(&text);
    dbuf_addf(&url, "%s/iptvepg/platform/index.jsp?UserID=%s&Action=Login",
              g.eas_url, g.userid);
    http_resp r;
    if (get_text(url.p, NULL, &text, &r) != 0) {
        dbuf_free(&url); dbuf_free(&text);
        return -1;
    }
    http_resp_free(&r);
    dbuf_free(&text);
    dbuf_free(&url);

    dbuf url2, ref;
    dbuf_init(&url2);
    dbuf_init(&ref);
    dbuf_addf(&url2, "%s/iptvepg/platform/getencrypttoken.jsp?UserID=%s&Action=Login"
              "&TerminalFlag=1&TerminalOsType=0&STBID=&stbtype=", g.eas_url, g.userid);
    dbuf_addf(&ref, "%s/iptvepg/platform/index.jsp?UserID=%s&Action=Login",
              g.eas_url, g.userid);

    kv extra[3];
    addh(extra, 0, "Referer", ref.p);
    extra[1].k = NULL;

    dbuf text2;
    dbuf_init(&text2);
    if (get_text(url2.p, extra, &text2, &r) != 0) {
        dbuf_free(&url2); dbuf_free(&ref); dbuf_free(&text2);
        return -1;
    }
    int rc = extract_quoted(text2.p ? text2.p : "", "GetAuthInfo('", out, n);
    if (rc != 0) {
        fprintf(stderr, "no challenge in getencrypttoken.jsp:\n%.800s\n",
                text2.p ? text2.p : "");
    }
    http_resp_free(&r);
    dbuf_free(&url2); dbuf_free(&ref); dbuf_free(&text2);
    return rc;
}

/* ---------------- frameset (L6 / channels re-post) ---------------- */
static int post_frameset(int with_origin, dbuf *text_out)
{
    dbuf body;
    dbuf_init(&body);
    urlenc_to(&body, "MAIN_WIN_SRC"); dbuf_addc(&body, '=');
    urlenc_to(&body, "/iptvepg/frame205/channel_start.jsp?tempno=-1");
    dbuf_add(&body, "&");
    urlenc_to(&body, "NEED_UPDATE_STB"); dbuf_add(&body, "=1&");
    urlenc_to(&body, "BUILD_ACTION"); dbuf_add(&body, "=");
    urlenc_to(&body, "FRAMESET_BUILDER");
    dbuf_add(&body, "&");
    urlenc_to(&body, "hdmistatus"); dbuf_add(&body, "=");
    urlenc_to(&body, "undefined");

    dbuf url;
    dbuf_init(&url);
    dbuf_addf(&url, "%s/iptvepg/function/frameset_builder.jsp", g.epg_base);

    kv extra[4];
    int i = 0;
    if (with_origin) {
        addh(extra, i++, "Origin", g.eas_url);
        addh(extra, i++, "Referer", NULL);   /* filled below */
    }
    extra[i].k = NULL;

    dbuf ref;
    dbuf_init(&ref);
    if (with_origin) {
        dbuf_addf(&ref, "%s/iptvepg/function/frameset_judger.jsp", g.epg_base);
        extra[1].v = ref.p;
    }

    http_resp r;
    int rc = post_text(url.p, body.p, extra, text_out, &r);
    http_resp_free(&r);
    dbuf_free(&body); dbuf_free(&url); dbuf_free(&ref);
    return rc;
}

/* ---------------- login C1..C6 (epg.py:174-233) ---------------- */
static int login_locked(const char *save_path, char *errbuf, size_t errsz)
{
    char challenge[512];
    if (plat_challenge(challenge, sizeof challenge) != 0) {
        snprintf(errbuf, errsz, "challenge failed");
        return -1;
    }
    char *auth = plat_sign(challenge, NULL);

    /* L1 auth.jsp */
    dbuf url, body, ref;
    dbuf_init(&url);
    dbuf_init(&body);
    dbuf_init(&ref);
    dbuf_addf(&url, "%s/iptvepg/platform/auth.jsp?easip=%s&ipVersion=4&networkid=1&serterminalno=89",
              g.epg_base, g.eas_host);
    dbuf_add(&body, "UserID="); urlenc_to(&body, g.userid);
    dbuf_add(&body, "&Authenticator="); urlenc_to(&body, auth);
    dbuf_add(&body, "&StbIP="); urlenc_to(&body, g.stbip);
    dbuf_addf(&ref, "%s/iptvepg/platform/getencrypttoken.jsp?UserID=%s&Action=Login",
              g.eas_url, g.userid);

    kv extra[4];
    addh(extra, 0, "Origin", g.eas_url);
    addh(extra, 1, "Referer", ref.p);
    extra[2].k = NULL;

    dbuf text;
    dbuf_init(&text);
    http_resp r;
    int rc = post_text(url.p, body.p, extra, &text, &r);
    if (rc != 0) {
        snprintf(errbuf, errsz, "auth.jsp transport error");
        goto fail1;
    }
    char tok[512];
    if (extract_quoted(text.p ? text.p : "", "jsSetConfig('UserToken','", tok, sizeof tok) != 0) {
        snprintf(errbuf, errsz,
                 "auth.jsp did not return a UserToken (status=%ld); challenge=%.100s auth=%.32s...",
                 r.code, challenge, auth);
        goto fail1;
    }
    snprintf(g_sess.user_token, sizeof g_sess.user_token, "%s", tok);

    char ug[16], eg[16];
    if (extract_digits(text.p ? text.p : "", "jsSetConfig('UserGroupNMB','", ug, sizeof ug) != 0)
        snprintf(ug, sizeof ug, "27");
    if (extract_digits(text.p ? text.p : "", "jsSetConfig('EPGGroupNMB','", eg, sizeof eg) != 0)
        snprintf(eg, sizeof eg, "27");
    snprintf(g_sess.user_grp, sizeof g_sess.user_grp, "%s", ug);
    snprintf(g_sess.epg_grp, sizeof g_sess.epg_grp, "%s", eg);
    http_resp_free(&r);
    dbuf_free(&text);

    /* L2 index.jsp */
    dbuf q;
    dbuf_init(&q);
    dbuf_add(&q, "UserGroupNMB="); urlenc_to(&q, g_sess.user_grp);
    dbuf_add(&q, "&EPGGroupNMB="); urlenc_to(&q, g_sess.epg_grp);
    dbuf_add(&q, "&UserToken="); urlenc_to(&q, g_sess.user_token);
    dbuf_add(&q, "&UserID="); urlenc_to(&q, g.userid);
    dbuf_add(&q, "&STBID="); urlenc_to(&q, g.stbid);
    dbuf_add(&q, "&easip="); urlenc_to(&q, g.eas_host);
    dbuf_add(&q, "&networkid=1&loadbalanced=-1");

    dbuf idx_url;
    dbuf_init(&idx_url);
    dbuf_addf(&idx_url, "%s/iptvepg/function/index.jsp?", g.epg_base);
    dbuf_add(&idx_url, q.p);

    dbuf text2;
    dbuf_init(&text2);
    if (get_text(idx_url.p, NULL, &text2, &r) != 0) {
        snprintf(errbuf, errsz, "index.jsp transport error");
        dbuf_free(&q); dbuf_free(&idx_url); dbuf_free(&text2);
        goto fail1;
    }
    http_resp_free(&r);
    dbuf_free(&text2);

    /* L3 funcportalauth.jsp */
    dbuf body3, ref3;
    dbuf_init(&body3);
    dbuf_init(&ref3);
    const char *bk[10] = {"UserToken", "UserID", "STBID", "stbinfo", "prmid",
                          "easip", "networkid", "stbtype", "drmsupplier", "stbversion"};
    const char *bv[10] = {g_sess.user_token, g.userid, g.stbid, "", "",
                          g.eas_host, "1", g.stbtype, "", g.stbversion};
    for (int i2 = 0; i2 < 10; i2++) {
        if (i2) dbuf_add(&body3, "&");
        urlenc_to(&body3, bk[i2]);
        dbuf_addc(&body3, '=');
        urlenc_to(&body3, bv[i2]);
    }
    dbuf_add(&ref3, idx_url.p);

    kv extra3[4];
    addh(extra3, 0, "Origin", g.epg_base);
    addh(extra3, 1, "Referer", ref3.p);
    extra3[2].k = NULL;

    dbuf url3;
    dbuf_init(&url3);
    dbuf_addf(&url3, "%s/iptvepg/function/funcportalauth.jsp", g.epg_base);

    dbuf text3;
    dbuf_init(&text3);
    if (post_text(url3.p, body3.p, extra3, &text3, &r) != 0) {
        snprintf(errbuf, errsz, "funcportalauth.jsp transport error");
        dbuf_free(&body3); dbuf_free(&ref3); dbuf_free(&url3); dbuf_free(&text3);
        dbuf_free(&q); dbuf_free(&idx_url);
        goto fail1;
    }
    http_resp_free(&r);
    dbuf_free(&text3);
    dbuf_free(&body3); dbuf_free(&ref3); dbuf_free(&url3);
    dbuf_free(&q); dbuf_free(&idx_url);

    /* L4 frame.jsp */
    dbuf url4;
    dbuf_init(&url4);
    dbuf_addf(&url4, "%s/iptvepg/function/frame.jsp", g.epg_base);
    dbuf text4;
    dbuf_init(&text4);
    if (get_text(url4.p, NULL, &text4, &r) != 0) {
        snprintf(errbuf, errsz, "frame.jsp transport error");
        dbuf_free(&url4); dbuf_free(&text4);
        goto fail1;
    }
    http_resp_free(&r);
    dbuf_free(&text4);
    dbuf_free(&url4);

    /* L5 frameset_judger.jsp */
    dbuf url5, ref5;
    dbuf_init(&url5);
    dbuf_init(&ref5);
    dbuf_addf(&url5, "%s/iptvepg/function/frameset_judger.jsp", g.epg_base);
    dbuf_addf(&ref5, "%s/iptvepg/function/frame.jsp", g.epg_base);
    kv extra5[4];
    addh(extra5, 0, "Origin", g.epg_base);
    addh(extra5, 1, "Referer", ref5.p);
    extra5[2].k = NULL;
    dbuf text5;
    dbuf_init(&text5);
    if (post_text(url5.p, "picturetype=1%2C3%2C5", extra5, &text5, &r) != 0) {
        snprintf(errbuf, errsz, "frameset_judger transport error");
        dbuf_free(&url5); dbuf_free(&ref5); dbuf_free(&text5);
        goto fail1;
    }
    http_resp_free(&r);
    dbuf_free(&text5);
    dbuf_free(&url5);
    dbuf_free(&ref5);

    /* L6 frameset_builder.jsp */
    dbuf fs;
    dbuf_init(&fs);
    if (post_frameset(1, &fs) != 0) {
        snprintf(errbuf, errsz, "frameset_builder transport error");
        dbuf_free(&fs);
        goto fail1;
    }
    pthread_mutex_lock(&frameset_lock);
    free(g_frameset);
    g_frameset = dbuf_steal(&fs);
    pthread_mutex_unlock(&frameset_lock);

    g_sess.have = 1;
    if (save_path && plat_save_session(save_path) != 0) {
        snprintf(errbuf, errsz, "cannot save session %s", save_path);
        free(auth);
        dbuf_free(&url); dbuf_free(&body); dbuf_free(&ref);
        return -1;
    }
    free(auth);
    dbuf_free(&url); dbuf_free(&body); dbuf_free(&ref);
    return 0;

fail1:
    free(auth);
    http_resp_free(&r);
    dbuf_free(&text);
    dbuf_free(&url); dbuf_free(&body); dbuf_free(&ref);
    return -1;
}

int plat_login(const char *save_path, char *errbuf, size_t errsz)
{
    pthread_mutex_lock(&g_login_lock);
    int rc = login_locked(save_path, errbuf, errsz);
    pthread_mutex_unlock(&g_login_lock);
    return rc;
}

/* ---------------- session file ---------------- */
static void cb_cookie(const char *name, const char *value, void *ud)
{
    jv *o = ud;
    jobj_set(o, name, jstr(value));
}

int plat_save_session(const char *path)
{
    jv *o = jobj();
    jv *ck = jobj();
    cookies_each(cb_cookie, ck);
    jobj_set(o, "cookies", ck);
    jobj_set(o, "user_token", jstr(g_sess.user_token));
    jv *gn = jarr();
    jarr_push(gn, jstr(g_sess.user_grp));
    jarr_push(gn, jstr(g_sess.epg_grp));
    jobj_set(o, "group_nmb", gn);
    char *s = json_dump_str(o, 1);          /* indent=1, no trailing newline */
    jv_free(o);
    int rc = write_atomic(path, s, strlen(s));
    free(s);
    return rc;
}

int plat_load_session(const char *path)
{
    size_t len;
    char *data = read_file(path, &len);
    if (!data) return -1;
    char *err = NULL;
    jv *o = json_parse(data, len, &err);
    free(data);
    if (!o) { free(err); return -1; }
    cookies_clear();
    const jv *ck = jobj_get(o, "cookies");
    if (ck && ck->t == JOBJ)
        for (size_t i = 0; i < ck->nm; i++)
            if (ck->ms[i].v && ck->ms[i].v->t == JSTR)
                cookies_set(ck->ms[i].k, ck->ms[i].v->s);
    const jv *tok = jobj_get(o, "user_token");
    snprintf(g_sess.user_token, sizeof g_sess.user_token, "%s",
             (tok && tok->t == JSTR && tok->s) ? tok->s : "");
    const jv *gn = jobj_get(o, "group_nmb");
    if (gn && gn->t == JARR && gn->n >= 2 &&
        gn->items[0]->t == JSTR && gn->items[1]->t == JSTR) {
        snprintf(g_sess.user_grp, sizeof g_sess.user_grp, "%s", gn->items[0]->s);
        snprintf(g_sess.epg_grp, sizeof g_sess.epg_grp, "%s", gn->items[1]->s);
    } else {
        snprintf(g_sess.user_grp, sizeof g_sess.user_grp, "27");
        snprintf(g_sess.epg_grp, sizeof g_sess.epg_grp, "27");
    }
    g_sess.have = g_sess.user_token[0] != 0;
    jv_free(o);
    return 0;
}

/* ---------------- channels (epg.py:251-308) ---------------- */
jv *plat_parse_channels(const char *text);

jv *plat_parse_channels(const char *text)
{
    jv *out = jarr();
    const char *p = text;
    const char *PREFIX = "jsSetConfig('Channel','";
    while ((p = strstr(p, PREFIX)) != NULL) {
        p += strlen(PREFIX);
        const char *end = strstr(p, "');");
        if (!end) break;
        /* inner: (\w+)="([^"]*)" first-occurrence-wins (dict semantics) */
        jv *o = jobj();
        const char *q = p;
        while (q < end) {
            const char *eq = memchr(q, '=', (size_t)(end - q));
            if (!eq) break;
            /* key = \w+ directly before '=' */
            const char *ks = eq;
            while (ks > q && (isalnum((unsigned char)ks[-1]) || ks[-1] == '_')) ks--;
            if (ks == eq) { q = eq + 1; continue; }
            const char *vs = eq + 1;
            if (vs >= end || *vs != '"') { q = eq + 1; continue; }
            const char *ve = memchr(vs + 1, '"', (size_t)(end - vs - 1));
            if (!ve) break;
            char key[64];
            size_t kl = (size_t)(eq - ks);
            if (kl >= sizeof key) kl = sizeof key - 1;
            memcpy(key, ks, kl);
            key[kl] = 0;
            jv *old = (jv *)jobj_get(o, key);
            jv *val = jstrn(vs + 1, (size_t)(ve - vs - 1));
            if (old) {
                /* replace in place, keep position */
                for (size_t i = 0; i < o->nm; i++)
                    if (!strcmp(o->ms[i].k, key)) { jv_free(o->ms[i].v); o->ms[i].v = val; break; }
            } else {
                jobj_set(o, key, val);
            }
            q = ve + 1;
        }

        jv *row = jobj();
        const char *fields[] = {"ChannelID", "ChannelName", "UserChannelID",
                                "ChannelURL", "TimeShift", "TimeShiftLength",
                                "FCCEnable", "FCCFunction"};
        const char *outk[] = {"channel_id", "name", "user_channel_id",
                              "channel_url", "timeshift", "timeshift_len",
                              "fcc_enable", "fcc_function"};
        for (int i = 0; i < 8; i++) {
            const jv *v = jobj_get(o, fields[i]);
            jobj_set(row, outk[i], v ? jstrn(v->s ? v->s : "", v->slen) : jnull());
        }
        /* sdp = ChannelSDP.split("|")[-1] */
        const jv *sdpv = jobj_get(o, "ChannelSDP");
        const char *sdp = sdpv && sdpv->s ? sdpv->s : "";
        const char *last = strrchr(sdp, '|');
        jobj_set(row, "sdp", jstr(last ? last + 1 : sdp));
        /* fcc = f"{ChannelFCCIP}:{ChannelFCCPort}" (missing -> "None:None") */
        const jv *ip = jobj_get(o, "ChannelFCCIP");
        const jv *port = jobj_get(o, "ChannelFCCPort");
        dbuf fb;
        dbuf_init(&fb);
        dbuf_add(&fb, (ip && ip->t == JSTR) ? (ip->s ? ip->s : "") : "None");
        dbuf_addc(&fb, ':');
        dbuf_add(&fb, (port && port->t == JSTR) ? (port->s ? port->s : "") : "None");
        jobj_set(row, "fcc", jstrn(fb.p ? fb.p : "", fb.len));
        dbuf_free(&fb);
        jarr_push(out, row);
        jv_free(o);
        p = end + 3;
    }
    return out;
}

int plat_channels_json(dbuf *out, int *count)
{
    pthread_mutex_lock(&frameset_lock);
    char *fs = g_frameset ? xstrdup(g_frameset) : NULL;
    pthread_mutex_unlock(&frameset_lock);
    if (!fs) {
        dbuf t;
        dbuf_init(&t);
        if (post_frameset(0, &t) != 0) { dbuf_free(&t); return -1; }
        fs = dbuf_steal(&t);
        pthread_mutex_lock(&frameset_lock);
        free(g_frameset);
        g_frameset = xstrdup(fs);
        pthread_mutex_unlock(&frameset_lock);
    }
    jv *arr = plat_parse_channels(fs);
    free(fs);
    if (count) *count = (int)arr->n;
    json_dump(arr, out, 1);
    jv_free(arr);
    return 0;
}

/* ---------------- programs (epg.py:260-268) ---------------- */
jv *plat_programs(const char *channel_code, const char *date)
{
    dbuf url;
    dbuf_init(&url);
    dbuf_addf(&url, "%s/iptvepg/frame205/action/getchannelprogram.jsp?channelcode=",
              g.epg_base);
    urlenc_to(&url, channel_code);
    dbuf_add(&url, "&currdate=");
    urlenc_to(&url, date);

    kv extra[4];
    addh(extra, 0, "Referer", NULL);
    addh(extra, 1, "Accept", "*/*");
    extra[2].k = NULL;
    dbuf ref;
    dbuf_init(&ref);
    dbuf_addf(&ref, "%s/iptvepg/function/frameset_builder.jsp", g.epg_base);
    extra[0].v = ref.p;

    dbuf text;
    dbuf_init(&text);
    http_resp r;
    int rc = get_text(url.p, extra, &text, &r);
    http_resp_free(&r);
    dbuf_free(&url);
    dbuf_free(&ref);
    if (rc != 0) { dbuf_free(&text); return NULL; }

    const char *s = text.p ? text.p : "";
    const char *pv = strstr(s, "prevuelist:");
    if (!pv) {
        fprintf(stderr, "no prevuelist in response:\n%.600s\n", s);
        dbuf_free(&text);
        return NULL;
    }
    pv += strlen("prevuelist:");
    jv *arr = jarr();
    const char *p = pv;
    /* mirror re.findall(r"\{[^{}]*\}", tail): match stops at first '}' that is
       not preceded by an inner '{'; an inner '{' restarts the match there. */
    for (;;) {
        const char *o = strchr(p, '{');
        if (!o) break;
        const char *j = o + 1;
        while (*j && *j != '{' && *j != '}') j++;
        if (!*j || *j == '{') { p = j; continue; }
        char *err = NULL;
        size_t n = (size_t)(j - o + 1);
        char *chunk = xmalloc(n + 1);
        memcpy(chunk, o, n);
        chunk[n] = 0;
        jv *obj = json_parse(chunk, n, &err);
        free(chunk);
        if (obj) jarr_push(arr, obj);
        else free(err);
        p = j + 1;
    }
    dbuf_free(&text);
    return arr;
}

/* ---------------- tvod (epg.py:270-283) ---------------- */
int plat_tvod(const char *prevuecode, const char *channel_code,
              const char *mixno, const char *columncode, const char *date,
              int isfromchannel, dbuf *out)
{
    /* params in insertion order */
    const char *keys[6];
    const char *vals[6];
    int n = 0;
    keys[n] = "prevuecode"; vals[n++] = prevuecode;
    keys[n] = "channelcode"; vals[n++] = channel_code;
    int k0 = n;
    if (isfromchannel) { keys[n] = "isfromchannel"; vals[n++] = "1"; }
    keys[n] = "mixno"; vals[n++] = mixno ? mixno : "";
    keys[n] = "columncode"; vals[n++] = columncode ? columncode : "1D04";
    keys[n] = "currdate"; vals[n++] = date;
    (void)k0;

    dbuf q;
    dbuf_init(&q);
    for (int i = 0; i < n; i++) {
        if (i) dbuf_addc(&q, '&');
        urlenc_to(&q, keys[i]);
        dbuf_addc(&q, '=');
        urlenc_to(&q, vals[i]);
    }

    dbuf transit, url;
    dbuf_init(&transit);
    dbuf_init(&url);
    dbuf_addf(&transit, "%s/iptvepg/frame205/control_transit_tvodplay.jsp?", g.epg_base);
    dbuf_add(&transit, q.p);
    dbuf_addf(&url, "%s/iptvepg/frame205/tvodplay.jsp?", g.epg_base);
    dbuf_add(&url, q.p);

    kv extra[3];
    addh(extra, 0, "Referer", transit.p);
    extra[1].k = NULL;

    dbuf text;
    dbuf_init(&text);
    http_resp r;
    int rc = get_text(url.p, extra, &text, &r);
    http_resp_free(&r);
    dbuf_free(&url);
    dbuf_free(&transit);
    dbuf_free(&q);
    if (rc != 0) { dbuf_free(&text); return -1; }

    const char *s = text.p ? text.p : "";
    const char *p = strstr(s, "top.jsPlayTVOD(\"");
    if (!p) {
        fprintf(stderr, "no jsPlayTVOD in tvodplay.jsp:\n%.800s\n", s);
        dbuf_free(&text);
        return -1;
    }
    p += strlen("top.jsPlayTVOD(\"");
    const char *e = strchr(p, '"');
    if (!e) { dbuf_free(&text); return -1; }
    /* python: re.search(r'top\.jsPlayTVOD\("([^"]+)"\)') — empty capture
       ("") does not match and raises; whitespace-only still matches */
    if (e == p) {
        fprintf(stderr, "no jsPlayTVOD in tvodplay.jsp:\n%.800s\n", s);
        dbuf_free(&text);
        return -1;
    }
    /* re.sub(r"\s+", "", captured) */
    for (const char *k = p; k < e; k++)
        if (!(*k == ' ' || *k == '\t' || *k == '\n' || *k == '\r' || *k == '\f' || *k == '\v'))
            dbuf_addc(out, *k);
    dbuf_free(&text);
    return 0;
}
