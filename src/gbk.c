#include "gbk.h"
#include "gbk_table.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>

static void utf8_put(dbuf *out, unsigned long cp)
{
    if (cp < 0x80) dbuf_addc(out, (char)cp);
    else if (cp < 0x800) {
        dbuf_addc(out, (char)(0xC0 | (cp >> 6)));
        dbuf_addc(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        dbuf_addc(out, (char)(0xE0 | (cp >> 12)));
        dbuf_addc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        dbuf_addc(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        dbuf_addc(out, (char)(0xF0 | (cp >> 18)));
        dbuf_addc(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        dbuf_addc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        dbuf_addc(out, (char)(0x80 | (cp & 0x3F)));
    }
}

static unsigned short gbk_lookup(unsigned char a, unsigned char b)
{
    if (a < GBK_LEAD_BASE || a >= GBK_LEAD_BASE + GBK_ROWS) return 0;
    if (b < GBK_TRAIL_BASE || b >= GBK_TRAIL_BASE + GBK_COLS) return 0;
    return gbk_tbl[(a - GBK_LEAD_BASE) * GBK_COLS + (b - GBK_TRAIL_BASE)];
}

static int gbk_core(const unsigned char *in, size_t n, dbuf *out, int gb2312_only)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = in[i];
        if (c < 0x80) { dbuf_addc(out, (char)c); i++; continue; }
        if (i + 1 >= n) return -1;
        unsigned char d = in[i + 1];
        if (gb2312_only) {
            if (c < 0xA1 || c > 0xF7 || d < 0xA1 || d > 0xFE) return -1;
        }
        unsigned short cp = gbk_lookup(c, d);
        if (!cp) return -1;
        utf8_put(out, cp);
        i += 2;
    }
    return 0;
}

int gbk_to_utf8(const unsigned char *in, size_t n, dbuf *out) { return gbk_core(in, n, out, 0); }
int gb2312_to_utf8(const unsigned char *in, size_t n, dbuf *out) { return gbk_core(in, n, out, 1); }

void latin1_to_utf8(const unsigned char *in, size_t n, dbuf *out)
{
    for (size_t i = 0; i < n; i++) {
        if (in[i] < 0x80) dbuf_addc(out, (char)in[i]);
        else {
            dbuf_addc(out, (char)(0xC0 | (in[i] >> 6)));
            dbuf_addc(out, (char)(0x80 | (in[i] & 0x3F)));
        }
    }
}

/* charset=([\w-]+) from Content-Type (epg.py:88) */
static void declared_charset(const char *ct, char *out, size_t n)
{
    out[0] = 0;
    if (!ct) return;
    size_t len = strlen(ct);
    for (size_t i = 0; i + 8 <= len; i++) {
        if (strncasecmp(ct + i, "charset=", 8) != 0) continue;
        const char *p = ct + i + 8;
        size_t k = 0;
        while (p[k] && ((p[k] >= 'A' && p[k] <= 'Z') || (p[k] >= 'a' && p[k] <= 'z') ||
                        (p[k] >= '0' && p[k] <= '9') || p[k] == '-' || p[k] == '_'))
            k++;
        if (k && k < n) { memcpy(out, p, k); out[k] = 0; return; }
    }
}

/* utf-8 with U+FFFD replacement (requests errors='replace' final fallback) */
static void utf8_replace(const unsigned char *in, size_t n, dbuf *out)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = in[i];
        if (c < 0x80) { dbuf_addc(out, (char)c); i++; continue; }
        int k;
        unsigned long cp;
        if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; }
        else { dbuf_add(out, "\xEF\xBF\xBD"); i++; continue; }
        if (i + (size_t)k >= n) { dbuf_add(out, "\xEF\xBF\xBD"); i = n; continue; }
        int ok = 1;
        for (int j = 1; j <= k; j++) {
            if ((in[i + j] & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (in[i + j] & 0x3F);
        }
        if (!ok) { dbuf_add(out, "\xEF\xBF\xBD"); i++; continue; }
        if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) ||
            (k == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) ||
            cp > 0x10FFFF) {
            dbuf_add(out, "\xEF\xBF\xBD"); i++; continue;
        }
        utf8_put(out, cp);
        i += (size_t)k + 1;
    }
}

void fix_encoding(const unsigned char *raw, size_t n, const char *content_type,
                  dbuf *out)
{
    if (utf8_valid(raw, n)) { dbuf_addn(out, raw, n); return; }

    char decl[64];
    declared_charset(content_type, decl, sizeof decl);
    char dl[64];
    size_t k = 0;
    for (; decl[k] && k < sizeof dl - 1; k++)
        dl[k] = (char)tolower((unsigned char)decl[k]);
    dl[k] = 0;

    if (dl[0]) {
        if (!strcmp(dl, "utf-8") || !strcmp(dl, "utf8")) {
            /* already failed strict utf-8 above; Python retries and fails again */
        } else if (!strcmp(dl, "gb2312")) {
            if (gb2312_to_utf8(raw, n, out) == 0) return;
        } else if (!strcmp(dl, "gbk") || !strcmp(dl, "x-gbk") || !strcmp(dl, "cp936") ||
                   !strcmp(dl, "gb18030")) {
            if (gbk_to_utf8(raw, n, out) == 0) return;
        } else if (!strcmp(dl, "iso-8859-1") || !strcmp(dl, "latin1") ||
                   !strcmp(dl, "latin-1")) {
            latin1_to_utf8(raw, n, out);
            return;
        }
        /* unknown charset -> Python LookupError -> fall through */
    }

    if (gbk_to_utf8(raw, n, out) == 0) return;

    latin1_to_utf8(raw, n, out);   /* always succeeds, mirrors latin-1 step */
    return;
}
