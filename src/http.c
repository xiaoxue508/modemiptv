#define _GNU_SOURCE
#include "http.h"
#include "gbk.h"
#include "uplink.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

/* ---------------- cookie jar ---------------- */
typedef struct { char *name, *value; } ck_t;
static ck_t *cks;
static size_t ncks, cks_cap;
static pthread_mutex_t ck_lock = PTHREAD_MUTEX_INITIALIZER;

void cookies_clear(void)
{
    pthread_mutex_lock(&ck_lock);
    for (size_t i = 0; i < ncks; i++) { free(cks[i].name); free(cks[i].value); }
    ncks = 0;
    pthread_mutex_unlock(&ck_lock);
}

void cookies_set(const char *name, const char *value)
{
    pthread_mutex_lock(&ck_lock);
    for (size_t i = 0; i < ncks; i++)
        if (!strcmp(cks[i].name, name)) {
            free(cks[i].value);
            cks[i].value = xstrdup(value);
            pthread_mutex_unlock(&ck_lock);
            return;
        }
    if (ncks == cks_cap) {
        cks_cap = cks_cap ? cks_cap * 2 : 8;
        cks = xrealloc(cks, cks_cap * sizeof *cks);
    }
    cks[ncks].name = xstrdup(name);
    cks[ncks].value = xstrdup(value);
    ncks++;
    pthread_mutex_unlock(&ck_lock);
}

void cookies_each(void (*cb)(const char *, const char *, void *), void *ud)
{
    pthread_mutex_lock(&ck_lock);
    for (size_t i = 0; i < ncks; i++) cb(cks[i].name, cks[i].value, ud);
    pthread_mutex_unlock(&ck_lock);
}

size_t cookies_count(void)
{
    pthread_mutex_lock(&ck_lock);
    size_t n = ncks;
    pthread_mutex_unlock(&ck_lock);
    return n;
}

static char *cookie_header(void)
{
    dbuf b;
    dbuf_init(&b);
    pthread_mutex_lock(&ck_lock);
    for (size_t i = 0; i < ncks; i++) {
        if (i) dbuf_add(&b, "; ");
        dbuf_add(&b, cks[i].name);
        dbuf_addc(&b, '=');
        dbuf_add(&b, cks[i].value);
    }
    pthread_mutex_unlock(&ck_lock);
    return b.p ? dbuf_steal(&b) : NULL;
}

/* ---------------- transfer ---------------- */
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static int inited = 0;

void http_init(void)
{
    pthread_mutex_lock(&init_lock);
    if (!inited) { curl_global_init(CURL_GLOBAL_ALL); inited = 1; }
    pthread_mutex_unlock(&init_lock);
}

typedef struct { dbuf *out; http_resp *r; } cbarg;

static size_t body_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    cbarg *a = ud;
    dbuf_addn(a->out, ptr, size * nmemb);
    return size * nmemb;
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    cbarg *a = ud;
    size_t n = size * nmemb;
    http_resp *r = a->r;
    if (n > 15 && strncasecmp(ptr, "Content-Type:", 13) == 0) {
        size_t k = n;
        while (k && (ptr[k - 1] == '\r' || ptr[k - 1] == '\n')) k--;
        if (k >= sizeof r->ctype) k = sizeof r->ctype - 1;
        memcpy(r->ctype, ptr, k);
        r->ctype[k] = 0;
    } else if (n > 6 && strncasecmp(ptr, "Date:", 5) == 0) {
        const char *p = ptr + 5;
        while (p < ptr + n && *p == ' ') p++;
        size_t k = (size_t)(ptr + n - p);
        while (k && (p[k - 1] == '\r' || p[k - 1] == '\n')) k--;
        if (k >= sizeof r->date) k = sizeof r->date - 1;
        memcpy(r->date, p, k);
        r->date[k] = 0;
    } else if (n > 12 && strncasecmp(ptr, "Set-Cookie:", 11) == 0) {
        /* name=value; ...  (requests session keeps name/value only) */
        const char *p = ptr + 11;
        while (*p == ' ') p++;
        const char *eq = strchr(p, '=');
        if (eq) {
            char name[128];
            size_t k = (size_t)(eq - p);
            const char *semi = memchr(p, ';', n - 11);
            if (k >= sizeof name) k = sizeof name - 1;
            memcpy(name, p, k);
            name[k] = 0;
            const char *vstart = eq + 1;
            const char *vend = semi ? semi : ptr + n;
            char val[512];
            size_t vk = (size_t)(vend - vstart);
            while (vk && (vstart[vk - 1] == '\r' || vstart[vk - 1] == ' ')) vk--;
            if (vk >= sizeof val) vk = sizeof val - 1;
            memcpy(val, vstart, vk);
            val[vk] = 0;
            if (name[0]) cookies_set(name, val);
        }
    }
    return n;
}

static int perform(const char *url, const char *method, const char *body,
                   size_t body_len, const kv *hdrs, http_resp *r)
{
    memset(r, 0, sizeof *r);
    dbuf_init(&r->body);
    r->code = 0;
    r->ctype[0] = 0;

    CURL *c = curl_easy_init();
    if (!c) return -1;

    dbuf hdrbuf;
    dbuf_init(&hdrbuf);
    struct curl_slist *slist = NULL;
    for (const kv *h = hdrs; h && h->k; h++) {
        char line[1024];
        snprintf(line, sizeof line, "%s: %s", h->k, h->v);
        slist = curl_slist_append(slist, line);
    }
    char *ck = cookie_header();
    if (ck && *ck) {
        char line[4096];
        snprintf(line, sizeof line, "Cookie: %s", ck);
        slist = curl_slist_append(slist, line);
    }
    free(ck);
    if (!strcmp(method, "POST"))
        slist = curl_slist_append(slist, "Expect:");   /* requests never sends Expect */

    cbarg a = { &r->body, r };
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, slist);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 30L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)g.timeout);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &a);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &a);
    if (uplink_ip()[0])
        curl_easy_setopt(c, CURLOPT_INTERFACE, uplink_ip());  /* bind to uplink */
    if (!strcmp(method, "POST")) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->code);
    curl_easy_cleanup(c);
    curl_slist_free_all(slist);
    dbuf_free(&hdrbuf);
    return rc == CURLE_OK ? 0 : -1;
}

int http_get(const char *url, const kv *hdrs, http_resp *r)
{
    return perform(url, "GET", NULL, 0, hdrs, r);
}

int http_post(const char *url, const char *body, size_t body_len, const kv *hdrs,
              http_resp *r)
{
    return perform(url, "POST", body, body_len, hdrs, r);
}

void http_resp_free(http_resp *r) { dbuf_free(&r->body); }

void http_text(const http_resp *r, dbuf *out)
{
    fix_encoding((const unsigned char *)r->body.p ? (const unsigned char *)r->body.p : (const unsigned char *)"",
                 r->body.len, r->ctype, out);
}

/* ---------------- wall-clock sync ---------------- */
#define TIME_OK_MIN 1577836800L   /* 2020-01-01 */
#define TIME_OK_MAX 4102444800L   /* 2100-01-01 */

int http_time_sync(void)
{
    if (!g.time_url[0]) return -1;
    http_resp r;
    if (http_get(g.time_url, NULL, &r) != 0) {
        http_resp_free(&r);
        logmsg("time sync: GET %s failed", g.time_url);
        return -1;
    }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    char *end = strptime(r.date, "%a, %d %b %Y %H:%M:%S", &tm);
    time_t t = end ? timegm(&tm) : 0;
    http_resp_free(&r);
    if (t < TIME_OK_MIN || t > TIME_OK_MAX) {
        logmsg("time sync: bad Date '%s'", r.date);
        return -1;
    }
    time_t cur = time(NULL);
    if (cur >= TIME_OK_MIN && cur < TIME_OK_MAX && t - cur > -3600 && t - cur < 3600) {
        logmsg("time ok (drift %lds)", (long)(t - cur));
        return 0;
    }
    struct timeval tv = { t, 0 };
    if (settimeofday(&tv, NULL) != 0) {
        logmsg("time sync: settimeofday: %s", strerror(errno));
        return -1;
    }
    logmsg("time synced from %s: %s (%ld)", g.time_url, r.date, (long)t);
    return 0;
}
