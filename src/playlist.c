#include "playlist.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char **v; size_t n, cap; } strlist;

static void sl_push(strlist *s, const char *line)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->v = xrealloc(s->v, s->cap * sizeof *s->v);
    }
    s->v[s->n++] = xstrdup(line);
}

static void sl_free(strlist *s)
{
    for (size_t i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

static const char *jval_str(const jv *v)
{
    if (!v || v->t == JNULL) return NULL;
    if (v->t == JSTR) return v->s ? v->s : "";
    if (v->t == JNUM) return v->num ? v->num : "0";
    return NULL;
}

/* Python: c.get(k) or ""  /  c.get("name") or c.get("channel_id") */
static const char *fld(const jv *c, const char *k)
{
    const char *s = jval_str(jobj_get(c, k));
    return s ? s : "";
}

static const char *ch_name(const jv *c)
{
    const char *s = jval_str(jobj_get(c, "name"));
    if (s && *s) return s;
    s = jval_str(jobj_get(c, "channel_id"));
    return s ? s : "";
}

/* sorted(chs, key=keyf): int(user_channel_id or 0), ValueError -> 0, stable */
static long keyf(const jv *c)
{
    const char *s = jval_str(jobj_get(c, "user_channel_id"));
    if (!s || !*s) return 0;
    char *end = NULL;
    long x = strtol(s, &end, 10);
    if (end == s || !end || *end) return 0;
    return x;
}

typedef struct { const jv *c; long k; size_t i; } sortent;

static int key_cmp(const void *a, const void *b)
{
    const sortent *x = a, *y = b;
    if (x->k < y->k) return -1;
    if (x->k > y->k) return 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

static const char *cat_of(const char *name)
{
    if (!strncmp(name, "CCTV", 4) || !strncmp(name, "CGTN", 4) || !strncmp(name, "CHC", 3))
        return "央视";
    if (strstr(name, "山东") || strstr(name, "齐鲁") || strstr(name, "烟台"))
        return "山东本地";
    if (strstr(name, "卫视"))
        return "卫视";
    return "其他";
}

static void attrs(dbuf *o, const char *name, const char *group, const char *uid)
{
    dbuf_addf(o, "tvg-id=\"%s\" tvg-name=\"%s\" group-title=\"%s\" "
                 "catchup=\"default\" catchup-source=\"", name, name, group);
    const char *t = g.bridge_tpl;
    while (*t) {
        const char *ph = strstr(t, "{uid}");
        if (!ph) { dbuf_add(o, t); break; }
        dbuf_addn(o, t, (size_t)(ph - t));
        dbuf_add(o, uid);
        t = ph + 5;
    }
    dbuf_addc(o, '"');
}

static void push_extinf(strlist *lines, const char *name, const char *group,
                        const char *uid)
{
    dbuf l;
    dbuf_init(&l);
    dbuf_add(&l, "#EXTINF:-1 ");
    attrs(&l, name, group, uid);
    dbuf_add(&l, ",");
    dbuf_add(&l, name);
    sl_push(lines, l.p ? l.p : "");
    dbuf_free(&l);
}

static void join_into(dbuf *out, const strlist *lines)
{
    for (size_t i = 0; i < lines->n; i++) {
        if (i) dbuf_addc(out, '\n');
        dbuf_add(out, lines->v[i]);
    }
}

int playlist_build(int r2h_http, dbuf *out, int *n_m, int *n_f, int *n_u)
{
    int nm = 0, nf = 0, nu = 0;
    size_t len;
    char *raw = read_file(g.channels, &len);
    if (!raw) {
        dbuf_addf(out, "cannot read %s", g.channels);
        return -1;
    }
    char *err = NULL;
    jv *data = json_parse(raw, len, &err);
    free(err);
    free(raw);
    if (!data) {
        dbuf_addf(out, "bad json in %s", g.channels);
        return -1;
    }

    jv *chs = NULL;
    if (data->t == JARR) {
        chs = data;
    } else if (data->t == JOBJ) {
        const jv *c = jobj_get(data, "channels");
        if (c && c->t == JARR) chs = jv_clone(c);
        jv_free(data);
    } else {
        jv_free(data);
    }
    if (!chs) {
        dbuf_addf(out, "channels list missing in %s", g.channels);
        return -1;
    }

    /* stable sort by int(user_channel_id) */
    sortent *se = xmalloc(chs->n * sizeof *se);
    for (size_t i = 0; i < chs->n; i++) {
        se[i].c = chs->items[i];
        se[i].k = se[i].c && se[i].c->t == JOBJ ? keyf(se[i].c) : 0;
        se[i].i = i;
    }
    qsort(se, chs->n, sizeof *se, key_cmp);

    strlist lines = {0};
    dbuf tmp;
    dbuf_init(&tmp);
    dbuf_addf(&tmp, "#EXTM3U x-tvg-url=\"%s\"", g.m3u_epg_url);
    sl_push(&lines, tmp.p ? tmp.p : "");
    dbuf_free(&tmp);
    dbuf_init(&tmp);
    dbuf_addf(&tmp, "# generated from channels.json (%zu channels)", chs->n);
    sl_push(&lines, tmp.p ? tmp.p : "");
    dbuf_free(&tmp);
    sl_push(&lines, "");

    if (r2h_http) {
        static const char *CATS[] = { "央视", "卫视", "山东本地", "其他" };
        for (size_t ci = 0; ci < 4; ci++) {
            strlist body = {0};
            for (size_t i = 0; i < chs->n; i++) {
                const jv *c = se[i].c;
                if (!c || c->t != JOBJ) continue;
                const char *name = ch_name(c);
                const char *igmp = fld(c, "channel_url");
                if (strncmp(igmp, "igmp://", 7)) continue;
                if (strcmp(cat_of(name), CATS[ci])) continue;
                const char *uid = jval_str(jobj_get(c, "user_channel_id"));
                push_extinf(&body, name, CATS[ci], uid ? uid : "");
                dbuf path, u;
                dbuf_init(&path);
                dbuf_init(&u);
                dbuf_add(&path, "组播/");
                dbuf_add(&path, name);
                dbuf_add(&u, g.r2h);
                dbuf_addc(&u, '/');
                urlquote_to(&u, path.p ? path.p : "", "/");
                dbuf_free(&path);
                sl_push(&body, u.p ? u.p : "");
                dbuf_free(&u);
                nm++;
            }
            if (body.n) {
                dbuf h;
                dbuf_init(&h);
                dbuf_addf(&h, "# ==================== %s ====================", CATS[ci]);
                sl_push(&lines, h.p ? h.p : "");
                dbuf_free(&h);
                for (size_t i = 0; i < body.n; i++) sl_push(&lines, body.v[i]);
                sl_push(&lines, "");
            }
            sl_free(&body);
        }
    } else {
        static const struct { const char *tag, *group; } GRP[] = {
            { "mcast", "组播" }, { "fcc", "组播FCC" }, { "uc", "单播" },
        };
        for (int gi = 0; gi < 3; gi++) {
            dbuf h;
            dbuf_init(&h);
            dbuf_addf(&h, "# ==================== %s ====================", GRP[gi].group);
            sl_push(&lines, h.p ? h.p : "");
            dbuf_free(&h);
            for (size_t i = 0; i < chs->n; i++) {
                const jv *c = se[i].c;
                if (!c || c->t != JOBJ) continue;
                const char *name = ch_name(c);
                const char *igmp = fld(c, "channel_url");
                const char *sdp = fld(c, "sdp");
                const char *fcc = fld(c, "fcc");
                const char *uid = jval_str(jobj_get(c, "user_channel_id"));
                uid = uid ? uid : "";
                if (!strcmp(GRP[gi].tag, "mcast")) {
                    if (strncmp(igmp, "igmp://", 7)) continue;
                    push_extinf(&lines, name, GRP[gi].group, uid);
                    dbuf u;
                    dbuf_init(&u);
                    dbuf_addf(&u, "rtp://%s", igmp + 7);
                    sl_push(&lines, u.p ? u.p : "");
                    dbuf_free(&u);
                    nm++;
                } else if (!strcmp(GRP[gi].tag, "fcc")) {
                    if (strncmp(igmp, "igmp://", 7)) continue;
                    if (!*fcc || !strcmp(fcc, "None") || !strcmp(fcc, "None:None")) continue;
                    push_extinf(&lines, name, GRP[gi].group, uid);
                    dbuf u;
                    dbuf_init(&u);
                    dbuf_addf(&u, "rtp://%s/?fcc=%s", igmp + 7, fcc);
                    sl_push(&lines, u.p ? u.p : "");
                    dbuf_free(&u);
                    nf++;
                } else {
                    if (strncmp(sdp, "rtsp://", 7)) continue;
                    push_extinf(&lines, name, GRP[gi].group, uid);
                    sl_push(&lines, sdp);
                    nu++;
                }
            }
            sl_push(&lines, "");
        }
    }

    join_into(out, &lines);
    sl_free(&lines);
    free(se);
    jv_free(chs);

    if (n_m) *n_m = nm;
    if (n_f) *n_f = nf;
    if (n_u) *n_u = nu;
    return 0;
}
