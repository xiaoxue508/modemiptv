#include "epgxml.h"
#include "cache.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t b_lock = PTHREAD_MUTEX_INITIALIZER;
static int building;

static pthread_mutex_t r_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t r_cv = PTHREAD_COND_INITIALIZER;
static int ready_flag;

int epgxml_ready(void)
{
    pthread_mutex_lock(&r_lock);
    int r = ready_flag;
    pthread_mutex_unlock(&r_lock);
    return r;
}

void epgxml_mark_ready(void)
{
    pthread_mutex_lock(&r_lock);
    ready_flag = 1;
    pthread_cond_broadcast(&r_cv);
    pthread_mutex_unlock(&r_lock);
}

int epgxml_wait_ready(int timeout_s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_s;
    pthread_mutex_lock(&r_lock);
    while (!ready_flag) {
        if (pthread_cond_timedwait(&r_cv, &r_lock, &ts) == ETIMEDOUT) break;
    }
    int r = ready_flag;
    pthread_mutex_unlock(&r_lock);
    return r;
}

/* ---------------- helpers ---------------- */

static double now_d(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* epg.py parse_bt: "%Y.%m.%d %H:%M:%S" as aware UTC+8 -> epoch */
static int parse_bt(const char *s, time_t *out)
{
    if (!s || strlen(s) != 19) return -1;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    const char *end = strptime(s, "%Y.%m.%d %H:%M:%S", &tm);
    if (!end || *end) return -1;
    *out = mk8(&tm);
    return 0;
}

static void xml_escape(dbuf *o, const char *s)
{
    for (; *s; s++) {
        if (*s == '&') dbuf_add(o, "&amp;");
        else if (*s == '<') dbuf_add(o, "&lt;");
        else if (*s == '>') dbuf_add(o, "&gt;");
        else dbuf_addc(o, *s);
    }
}

static const char *jstr_or_null(const jv *v)
{
    if (!v || v->t == JNULL) return NULL;
    if (v->t == JSTR) return v->s ? v->s : "";
    if (v->t == JNUM) return v->num ? v->num : "0";
    if (v->t == JBOOL) return v->b ? "true" : "false";
    return NULL;
}

/* python str(c.get("user_channel_id")) */
static void uid_of(const jv *c, char *out, size_t n)
{
    const char *s = jstr_or_null(jobj_get(c, "user_channel_id"));
    snprintf(out, n, "%s", s ? s : "None");
}

static long uid_sortkey(const char *u)
{
    if (!*u) return 0;
    for (const char *p = u; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return strtol(u, NULL, 10);
}

typedef struct { char uid[64]; char *name; long k; size_t i; } idx_ent;

static int idx_cmp(const void *a, const void *b)
{
    const idx_ent *x = a, *y = b;
    if (x->k < y->k) return -1;
    if (x->k > y->k) return 1;
    return x->i < y->i ? -1 : 1;
}

/* channel_index() + sorted(idx, key=int(u) if isdigit else 0) */
static idx_ent *idx_build(size_t *n_out)
{
    jv *chs = cache_channels_dup();
    if (!chs || chs->t != JARR) { jv_free(chs); return NULL; }
    idx_ent *idx = xmalloc((chs->n + 1) * sizeof *idx);
    size_t n = 0;
    for (size_t i = 0; i < chs->n; i++) {
        jv *c = chs->items[i];
        if (!c || c->t != JOBJ) continue;
        uid_of(c, idx[n].uid, sizeof idx[n].uid);
        const char *nm = jstr_or_null(jobj_get(c, "name"));
        idx[n].name = xstrdup((nm && *nm) ? nm : idx[n].uid);
        idx[n].k = uid_sortkey(idx[n].uid);
        idx[n].i = n;
        n++;
    }
    jv_free(chs);
    qsort(idx, n, sizeof *idx, idx_cmp);
    *n_out = n;
    return idx;
}

typedef struct { char uid[64]; char *name; char date[16]; } tgt_t;

/* targets: uid in index order, name deduped, dates today+k, k=-past..future */
static tgt_t *targets_build(const idx_ent *idx, size_t n_idx, size_t *n_out)
{
    size_t cap = n_idx * (size_t)(g.epg_past + g.epg_future + 1) + 1;
    tgt_t *t = xmalloc(cap * sizeof *t);
    size_t n = 0;
    char **seen = xmalloc((n_idx + 1) * sizeof *seen);
    size_t nseen = 0;

    time_t today8;
    {
        struct tm tm;
        gm8(now_sec(), &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        today8 = mk8(&tm);
    }
    for (size_t i = 0; i < n_idx; i++) {
        int dup = 0;
        for (size_t j = 0; j < nseen; j++)
            if (!strcmp(seen[j], idx[i].name)) { dup = 1; break; }
        if (dup) continue;
        seen[nseen++] = idx[i].name;

        for (int k = -g.epg_past; k <= g.epg_future; k++) {
            snprintf(t[n].uid, sizeof t[n].uid, "%s", idx[i].uid);
            t[n].name = xstrdup(idx[i].name);
            strftime8(t[n].date, sizeof t[n].date, "%Y.%m.%d",
                      today8 + (time_t)k * 86400);
            n++;
        }
    }
    free(seen);
    *n_out = n;
    return t;
}

/* ---------------- 4-thread fetch ---------------- */

typedef struct {
    tgt_t *t;
    jv **res;
    size_t n, next;
    pthread_mutex_t *lk;
    pthread_cond_t *cv;
} fetch_ctx;

static void *fetch_worker(void *arg)
{
    fetch_ctx *c = arg;
    for (;;) {
        pthread_mutex_lock(c->lk);
        size_t i = c->next++;
        pthread_mutex_unlock(c->lk);
        if (i >= c->n) return NULL;
        jv *p = cache_get_programs(c->t[i].uid, c->t[i].date, 1, 1);
        pthread_mutex_lock(c->lk);
        c->res[i] = p;
        pthread_cond_signal(c->cv);
        pthread_mutex_unlock(c->lk);
    }
}

/* ---------------- assembly ---------------- */

typedef struct { time_t b, e; char *title; long seq; } itm_t;
typedef struct { char *name; itm_t *it; size_t n, cap; } bn_t;

static bn_t *bn_find(bn_t **v, size_t *n, const char *name)
{
    for (size_t i = 0; i < *n; i++)
        if (!strcmp((*v)[i].name, name)) return &(*v)[i];
    *v = xrealloc(*v, (*n + 1) * sizeof **v);
    bn_t *slot = &(*v)[(*n)++];
    slot->name = xstrdup(name);
    slot->it = NULL;
    slot->n = slot->cap = 0;
    return slot;
}

static void bn_push(bn_t *s, time_t b, time_t e, const char *title, long seq)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->it = xrealloc(s->it, s->cap * sizeof *s->it);
    }
    s->it[s->n].b = b;
    s->it[s->n].e = e;
    s->it[s->n].title = xstrdup(title);
    s->it[s->n].seq = seq;
    s->n++;
}

static int itm_cmp(const void *a, const void *b)
{
    const itm_t *x = a, *y = b;
    if (x->b != y->b) return x->b < y->b ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

typedef struct { const char *name; size_t slot; } cname_ent;
static int cname_cmp(const void *a, const void *b)
{
    const cname_ent *x = a, *y = b;
    return strcmp(x->name, y->name);
}

/* by_name.setdefault(name) + extend(parsed programs); main thread only */
static void assemble_target(bn_t **bn, size_t *n_bn, const tgt_t *t,
                            const jv *progs, long *seq)
{
    bn_t *slot = bn_find(bn, n_bn, t->name);
    if (!progs || progs->t != JARR) return;
    for (size_t j = 0; j < progs->n; j++) {
        jv *p = progs->items[j];
        if (!p || p->t != JOBJ) continue;
        time_t b, e;
        const jv *bv = jobj_get(p, "begintime");
        const jv *ev = jobj_get(p, "endtime");
        if (parse_bt(bv && bv->t == JSTR ? bv->s : NULL, &b) != 0) continue;
        if (parse_bt(ev && ev->t == JSTR ? ev->s : NULL, &e) != 0) continue;
        const jv *pv = jobj_get(p, "prevuename");
        const char *title = jstr_or_null(pv);
        bn_push(slot, b, e, title ? title : "", (*seq)++);
    }
}

int epgxml_build(void)
{
    pthread_mutex_lock(&b_lock);
    if (building) { pthread_mutex_unlock(&b_lock); return 1; }
    building = 1;
    pthread_mutex_unlock(&b_lock);

    cache_purge_old();

    int rc = 0;
    double t0 = now_d();
    idx_ent *idx = NULL;
    tgt_t *tgts = NULL;
    jv **res = NULL;
    bn_t *bn = NULL;
    size_t n_idx = 0, n_t = 0, n_bn = 0;

    idx = idx_build(&n_idx);
    if (!idx) {
        logmsg("xmltv build failed: cannot read %s", g.channels);
        rc = -1;
        goto done;
    }
    tgts = targets_build(idx, n_idx, &n_t);
    res = xmalloc((n_t + 1) * sizeof *res);
    memset(res, 0, (n_t + 1) * sizeof *res);

    {   /* N fetch workers + in-order streaming assembly: each parsed result
           is freed right after its titles are extracted (router only has
           ~35MB free, holding all 500+ parsed entries at once OOMs). */
        pthread_mutex_t lk;
        pthread_cond_t cv;
        pthread_mutex_init(&lk, NULL);
        pthread_cond_init(&cv, NULL);
        fetch_ctx ctx = { tgts, res, n_t, 0, &lk, &cv };
        pthread_t th[4];
        int nth = n_t < 4 ? (int)n_t : 4;
        int ncreated = 0;
        for (int i = 0; i < nth; i++)
            if (pthread_create(&th[i], NULL, fetch_worker, &ctx) == 0)
                ncreated++;
            else
                break;
        long seq = 0;
        for (size_t i = 0; i < n_t; i++) {
            if (ncreated > 0) {
                pthread_mutex_lock(&lk);
                while (!res[i]) pthread_cond_wait(&cv, &lk);
                pthread_mutex_unlock(&lk);
            } else {   /* no workers (pthread_create failed): fetch inline */
                res[i] = cache_get_programs(tgts[i].uid, tgts[i].date, 1, 1);
            }
            jv *progs = res[i];
            res[i] = NULL;
            assemble_target(&bn, &n_bn, &tgts[i], progs, &seq);
            jv_free(progs);
        }
        for (int i = 0; i < ncreated; i++) pthread_join(th[i], NULL);
        pthread_cond_destroy(&cv);
        pthread_mutex_destroy(&lk);
    }

    {   /* build the document: "\n".join(out), no trailing newline */
        dbuf o;
        dbuf_init(&o);
        dbuf_add(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        dbuf_add(&o, "<tv generator-info-name=\"srcbox-bridge\" generator-info-url=\"");
        dbuf_add(&o, g.gen_url);
        dbuf_add(&o, "\">\n");

        cname_ent *cn = xmalloc((n_bn + 1) * sizeof *cn);
        for (size_t i = 0; i < n_bn; i++) { cn[i].name = bn[i].name; cn[i].slot = i; }
        qsort(cn, n_bn, sizeof *cn, cname_cmp);
        for (size_t i = 0; i < n_bn; i++) {
            const char *name = cn[i].name;
            dbuf_add(&o, "<channel id=\"");
            xml_escape(&o, name);
            dbuf_add(&o, "\"><display-name>");
            xml_escape(&o, name);
            dbuf_add(&o, "</display-name></channel>\n");
        }

        for (size_t i = 0; i < n_bn; i++) {   /* insertion order = by_name order */
            bn_t *s = &bn[i];
            qsort(s->it, s->n, sizeof *s->it, itm_cmp);
            for (size_t j = 0; j < s->n; j++) {
                if (s->it[j].e <= s->it[j].b) continue;
                char bs[24], es[24];
                strftime8(bs, sizeof bs, "%Y%m%d%H%M%S", s->it[j].b);
                strftime8(es, sizeof es, "%Y%m%d%H%M%S", s->it[j].e);
                dbuf_addf(&o, "<programme start=\"%s +0800\" stop=\"%s +0800\" channel=\"",
                          bs, es);
                xml_escape(&o, s->name);
                dbuf_add(&o, "\"><title lang=\"zh\">");
                xml_escape(&o, s->it[j].title);
                dbuf_add(&o, "</title></programme>\n");
            }
        }
        dbuf_add(&o, "</tv>");
        free(cn);

        mkdir_p(g.cache_dir);
        if (write_atomic(g.epg_file, o.p ? o.p : "", o.len) != 0) {
            logmsg("xmltv build failed: cannot write %s", g.epg_file);
            rc = -1;
        } else {
            logmsg("xmltv built: %d channels, %.1fs -> %s", (int)n_bn,
                   now_d() - t0, g.epg_file);
        }
        dbuf_free(&o);
    }

done:
    for (size_t i = 0; i < n_t; i++) {
        free(tgts[i].name);
        if (res) jv_free(res[i]);
    }
    for (size_t i = 0; i < n_bn; i++) {
        for (size_t j = 0; j < bn[i].n; j++) free(bn[i].it[j].title);
        free(bn[i].it);
        free(bn[i].name);
    }
    free(bn);
    free(res);
    free(tgts);
    if (idx) {
        for (size_t i = 0; i < n_idx; i++) free(idx[i].name);
        free(idx);
    }
    pthread_mutex_lock(&b_lock);
    building = 0;
    pthread_mutex_unlock(&b_lock);
    return rc;
}

static volatile int epg_kick;

void epgxml_kick(void)
{
    epg_kick = 1;
}

static void worker_once(int force)
{
    struct stat st;
    int fresh = stat(g.epg_file, &st) == 0 &&
                (long)(now_sec() - st.st_mtime) < g.ttl_epg;
    if (fresh && !force) return;
    int rc = epgxml_build();
    if (rc >= 0) epgxml_mark_ready();
    else logmsg("epg_worker error: build failed (rc=%d)", rc);
}

void epgxml_worker_once(void)
{
    worker_once(0);
}

void epgxml_worker_loop(void)
{
    for (;;) {
        int kick = epg_kick;
        epg_kick = 0;
        worker_once(kick);
        /* sleep in 1s slices so an action kick is noticed within a second */
        int wait = g.worker_s > 0 ? g.worker_s : 120;
        while (wait-- > 0 && !epg_kick) sleep(1);
    }
}
