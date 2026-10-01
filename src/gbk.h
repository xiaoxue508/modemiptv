#ifndef IPTVD_GBK_H
#define IPTVD_GBK_H
#include "common.h"

/* strict decode: 0 = ok, -1 = invalid byte sequence (mirrors Python .decode) */
int gbk_to_utf8(const unsigned char *in, size_t n, dbuf *out);
int gb2312_to_utf8(const unsigned char *in, size_t n, dbuf *out);
void latin1_to_utf8(const unsigned char *in, size_t n, dbuf *out);

/* port-spec §0 / epg.py:81-100: utf-8 -> declared -> gbk -> latin-1 ->
   utf-8 with replacement.  out receives UTF-8. */
void fix_encoding(const unsigned char *raw, size_t n, const char *content_type,
                  dbuf *out);

#endif
