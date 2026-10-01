#ifndef IPTVD_HTTP_H
#define IPTVD_HTTP_H
#include "common.h"

typedef struct {
    long code;
    dbuf body;
    char ctype[160];
} http_resp;

void http_init(void);
/* hdrs: NULL-terminated kv array ({NULL,NULL} end). body may be NULL. */
int http_get(const char *url, const kv *hdrs, http_resp *r);
int http_post(const char *url, const char *body, size_t body_len, const kv *hdrs,
              http_resp *r);
void http_resp_free(http_resp *r);
/* decode body with fix_encoding (epg.py _fix_encoding) into out */
void http_text(const http_resp *r, dbuf *out);

/* cookie jar: Python requests session dict semantics (name -> value) */
void cookies_clear(void);
void cookies_set(const char *name, const char *value);
void cookies_each(void (*cb)(const char *, const char *, void *), void *ud);
size_t cookies_count(void);

#endif
