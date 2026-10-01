#define _GNU_SOURCE
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/stat.h>

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- dbuf ---------------- */
void dbuf_init(dbuf *b) { b->p = NULL; b->len = b->cap = 0; }

static void dbuf_grow(dbuf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void dbuf_addn(dbuf *b, const void *d, size_t n)
{
    if (!n) return;
    dbuf_grow(b, n);
    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = 0;
}

void dbuf_add(dbuf *b, const char *s) { if (s) dbuf_addn(b, s, strlen(s)); }
void dbuf_addc(dbuf *b, char c) { dbuf_addn(b, &c, 1); }

void dbuf_addf(dbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    dbuf_grow(b, (size_t)n);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void dbuf_free(dbuf *b) { free(b->p); dbuf_init(b); }

char *dbuf_steal(dbuf *b)
{
    if (!b->p) return xstrdup("");
    char *p = b->p;
    p[b->len] = 0;
    dbuf_init(b);
    return p;
}

/* ---------------- alloc / log ---------------- */
void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "oom\n"); exit(1); }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "oom\n"); exit(1); }
    return q;
}

char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void logmsg(const char *fmt, ...)
{
    char ts[16];
    strftime8(ts, sizeof ts, "%H:%M:%S", now_sec());
    pthread_mutex_lock(&log_lock);
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* ---------------- time (fixed +8, port-spec R16) ---------------- */
time_t now_sec(void) { return time(NULL); }

time_t mono_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (time_t)ts.tv_sec;
    return time(NULL);
}

void gm8(time_t t, struct tm *out)
{
    time_t x = t + 8 * 3600;
    gmtime_r(&x, out);
}

time_t mk8(const struct tm *in)
{
    struct tm c = *in;
    return timegm(&c) - 8 * 3600;
}

void strftime8(char *out, size_t n, const char *fmt, time_t t)
{
    struct tm tm;
    gm8(t, &tm);
    strftime(out, n, fmt, &tm);
}

/* ---------------- misc ---------------- */
int utf8_valid(const void *d, size_t n)
{
    const unsigned char *s = d;
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        int k;
        if ((c & 0xE0) == 0xC0) k = 1;
        else if ((c & 0xF0) == 0xE0) k = 2;
        else if ((c & 0xF8) == 0xF0) k = 3;
        else return 0;
        if (i + (size_t)k >= n) return 0;
        for (int j = 1; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

/* urllib.parse.quote_plus */
static int unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

void urlenc_to(dbuf *out, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (unreserved(*p)) dbuf_addc(out, (char)*p);
        else if (*p == ' ') dbuf_addc(out, '+');
        else dbuf_addf(out, "%%%02X", *p);
    }
}

char *urlenc(const char *s)
{
    dbuf b;
    dbuf_init(&b);
    urlenc_to(&b, s);
    return dbuf_steal(&b);
}

void urlquote_to(dbuf *out, const char *s, const char *extra_safe)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (unreserved(*p) || *p == '/' ||
            (extra_safe && *p < 0x80 && strchr(extra_safe, *p)))
            dbuf_addc(out, (char)*p);
        else
            dbuf_addf(out, "%%%02X", *p);
    }
}

int write_atomic(const char *path, const void *data, size_t n)
{
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (n && fwrite(data, 1, n, f) != n) { fclose(f); remove(tmp); return -1; }
    if (fflush(f) != 0) { fclose(f); remove(tmp); return -1; }
    fclose(f);
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    dbuf b;
    dbuf_init(&b);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) dbuf_addn(&b, buf, n);
    fclose(f);
    if (!b.p) b.p = xstrdup("");
    if (len) *len = b.len;
    return dbuf_steal(&b);
}

int mkdir_p(const char *path)
{
    if (!path || !*path) return -1;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}
