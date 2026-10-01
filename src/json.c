#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------- constructors ---------------- */
static jv *jnew(jtype t)
{
    jv *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->t = t;
    return v;
}

jv *jnull(void) { return jnew(JNULL); }
jv *jbool(int b) { jv *v = jnew(JBOOL); v->b = b; return v; }
jv *jnum(const char *raw) { jv *v = jnew(JNUM); v->num = xstrdup(raw); return v; }
jv *jstr(const char *s) { return jstrn(s, s ? strlen(s) : 0); }
jv *jstrn(const char *s, size_t n)
{
    jv *v = jnew(JSTR);
    v->s = xmalloc(n + 1);
    if (n) memcpy(v->s, s, n);
    v->s[n] = 0;
    v->slen = n;
    return v;
}
jv *jarr(void) { return jnew(JARR); }
jv *jobj(void) { return jnew(JOBJ); }

void jv_free(jv *v)
{
    if (!v) return;
    free(v->num);
    free(v->s);
    for (size_t i = 0; i < v->n; i++) jv_free(v->items[i]);
    free(v->items);
    for (size_t i = 0; i < v->nm; i++) { free(v->ms[i].k); jv_free(v->ms[i].v); }
    free(v->ms);
    free(v);
}

void jarr_push(jv *a, jv *v)
{
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->items = xrealloc(a->items, a->cap * sizeof *a->items); }
    a->items[a->n++] = v;
}

void jobj_set(jv *o, const char *k, jv *v)
{
    for (size_t i = 0; i < o->nm; i++)
        if (strcmp(o->ms[i].k, k) == 0) { jv_free(o->ms[i].v); o->ms[i].v = v; return; }
    if (o->nm == o->mcap) { o->mcap = o->mcap ? o->mcap * 2 : 8; o->ms = xrealloc(o->ms, o->mcap * sizeof *o->ms); }
    o->ms[o->nm].k = xstrdup(k);
    o->ms[o->nm].v = v;
    o->nm++;
}

const jv *jobj_get(const jv *o, const char *k)
{
    if (!o || o->t != JOBJ) return NULL;
    for (size_t i = 0; i < o->nm; i++)
        if (strcmp(o->ms[i].k, k) == 0) return o->ms[i].v;
    return NULL;
}

/* ---------------- parser ---------------- */
typedef struct {
    const char *s;
    size_t n, i;
    char err[160];
} pstate;

static jv *parse_value(pstate *p);

static void perr(pstate *p, const char *msg)
{
    if (!p->err[0]) snprintf(p->err, sizeof p->err, "%s at %zu", msg, p->i);
}

static void skip_ws(pstate *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_emit(dbuf *out, unsigned long cp)
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

static int parse_hex4(pstate *p, unsigned long *out)
{
    if (p->i + 4 > p->n) { perr(p, "bad \\u"); return -1; }
    unsigned long v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(p->s[p->i++]);
        if (h < 0) { perr(p, "bad \\u"); return -1; }
        v = (v << 4) | (unsigned long)h;
    }
    *out = v;
    return 0;
}

static jv *parse_string(pstate *p)
{
    if (p->s[p->i] != '"') { perr(p, "expected string"); return NULL; }
    p->i++;
    dbuf b;
    dbuf_init(&b);
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') { p->i++; jv *v = jstrn(b.p ? b.p : "", b.len); dbuf_free(&b); return v; }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) break;
            char e = p->s[p->i++];
            switch (e) {
            case '"': dbuf_addc(&b, '"'); break;
            case '\\': dbuf_addc(&b, '\\'); break;
            case '/': dbuf_addc(&b, '/'); break;
            case 'b': dbuf_addc(&b, '\b'); break;
            case 'f': dbuf_addc(&b, '\f'); break;
            case 'n': dbuf_addc(&b, '\n'); break;
            case 'r': dbuf_addc(&b, '\r'); break;
            case 't': dbuf_addc(&b, '\t'); break;
            case 'u': {
                unsigned long cp;
                if (parse_hex4(p, &cp) != 0) { dbuf_free(&b); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 1 < p->n &&
                    p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    p->i += 2;
                    unsigned long lo;
                    if (parse_hex4(p, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8_emit(&b, cp);
                break;
            }
            default: perr(p, "bad escape"); dbuf_free(&b); return NULL;
            }
            continue;
        }
        dbuf_addc(&b, (char)c);
        p->i++;
    }
    perr(p, "unterminated string");
    dbuf_free(&b);
    return NULL;
}

static jv *parse_number(pstate *p)
{
    size_t start = p->i;
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    if (p->i >= p->n || p->s[p->i] < '0' || p->s[p->i] > '9') { perr(p, "bad number"); return NULL; }
    if (p->s[p->i] == '0') p->i++;
    else while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        if (p->i >= p->n || p->s[p->i] < '0' || p->s[p->i] > '9') { perr(p, "bad number"); return NULL; }
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (p->i >= p->n || p->s[p->i] < '0' || p->s[p->i] > '9') { perr(p, "bad number"); return NULL; }
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    return jstrn(p->s + start, p->i - start); /* placeholder, fixed below */
}

static jv *parse_value(pstate *p)
{
    skip_ws(p);
    if (p->i >= p->n) { perr(p, "eof"); return NULL; }
    char c = p->s[p->i];
    if (c == '{') {
        p->i++;
        jv *o = jobj();
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; return o; }
        for (;;) {
            skip_ws(p);
            jv *k = parse_string(p);
            if (!k) { jv_free(o); return NULL; }
            skip_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') { perr(p, "expected :"); jv_free(k); jv_free(o); return NULL; }
            p->i++;
            jv *v = parse_value(p);
            if (!v) { jv_free(k); jv_free(o); return NULL; }
            jobj_set(o, k->s, v);
            jv_free(k);
            skip_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; return o; }
            perr(p, "expected , or }");
            jv_free(o);
            return NULL;
        }
    }
    if (c == '[') {
        p->i++;
        jv *a = jarr();
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; return a; }
        for (;;) {
            jv *v = parse_value(p);
            if (!v) { jv_free(a); return NULL; }
            jarr_push(a, v);
            skip_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; return a; }
            perr(p, "expected , or ]");
            jv_free(a);
            return NULL;
        }
    }
    if (c == '"') return parse_string(p);
    if (c == 't' && p->i + 4 <= p->n && memcmp(p->s + p->i, "true", 4) == 0) { p->i += 4; return jbool(1); }
    if (c == 'f' && p->i + 5 <= p->n && memcmp(p->s + p->i, "false", 5) == 0) { p->i += 5; return jbool(0); }
    if (c == 'n' && p->i + 4 <= p->n && memcmp(p->s + p->i, "null", 4) == 0) { p->i += 4; return jnull(); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = p->i;
        jv *num = parse_number(p);
        if (!num) return NULL;
        /* replace placeholder string with JNUM holding raw text */
        jv *v = jnew(JNUM);
        v->num = xmalloc(p->i - start + 1);
        memcpy(v->num, p->s + start, p->i - start);
        v->num[p->i - start] = 0;
        jv_free(num);
        return v;
    }
    perr(p, "unexpected char");
    return NULL;
}

jv *json_parse(const char *s, size_t n, char **err)
{
    pstate p;
    p.s = s;
    p.n = n;
    p.i = 0;
    p.err[0] = 0;
    jv *v = parse_value(&p);
    if (v) {
        skip_ws(&p);
        if (p.i < p.n && !p.err[0]) { jv_free(v); v = NULL; snprintf(p.err, sizeof p.err, "trailing at %zu", p.i); }
    }
    if (!v && err) *err = xstrdup(p.err[0] ? p.err : "parse error");
    return v;
}

/* ---------------- dump ---------------- */
void json_dump_str_escaped(dbuf *out, const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    dbuf_addc(out, '"');
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c == '"') { dbuf_add(out, "\\\""); i++; }
        else if (c == '\\') { dbuf_add(out, "\\\\"); i++; }
        else if (c == '\b') { dbuf_add(out, "\\b"); i++; }
        else if (c == '\f') { dbuf_add(out, "\\f"); i++; }
        else if (c == '\n') { dbuf_add(out, "\\n"); i++; }
        else if (c == '\r') { dbuf_add(out, "\\r"); i++; }
        else if (c == '\t') { dbuf_add(out, "\\t"); i++; }
        else if (c < 0x20) { dbuf_addf(out, "\\u%04x", c); i++; }
        else if (c == 0xE2 && i + 2 < n && p[i + 1] == 0x80 &&
                 (p[i + 2] == 0xA8 || p[i + 2] == 0xA9)) {
            dbuf_addf(out, "\\u%04x", p[i + 2] == 0xA8 ? 0x2028 : 0x2029);
            i += 3;
        } else { dbuf_addc(out, (char)c); i++; }
    }
    dbuf_addc(out, '"');
}

static void dump_indent(dbuf *out, int depth, int indent)
{
    if (indent >= 0)
        for (int i = 0; i < depth * indent; i++) dbuf_addc(out, ' ');
}

static void dump(const jv *v, dbuf *out, int indent, int depth)
{
    if (!v) { dbuf_add(out, "null"); return; }
    switch (v->t) {
    case JNULL: dbuf_add(out, "null"); break;
    case JBOOL: dbuf_add(out, v->b ? "true" : "false"); break;
    case JNUM: dbuf_add(out, v->num ? v->num : "null"); break;
    case JSTR: json_dump_str_escaped(out, v->s ? v->s : "", v->slen); break;
    case JARR:
        if (!v->n) { dbuf_add(out, "[]"); break; }
        dbuf_addc(out, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (indent >= 0) { dbuf_addc(out, '\n'); dump_indent(out, depth + 1, indent); }
            dump(v->items[i], out, indent, depth + 1);
            if (i + 1 < v->n) dbuf_addc(out, ',');
        }
        if (indent >= 0) { dbuf_addc(out, '\n'); dump_indent(out, depth, indent); }
        dbuf_addc(out, ']');
        break;
    case JOBJ:
        if (!v->nm) { dbuf_add(out, "{}"); break; }
        dbuf_addc(out, '{');
        for (size_t i = 0; i < v->nm; i++) {
            if (indent >= 0) { dbuf_addc(out, '\n'); dump_indent(out, depth + 1, indent); }
            json_dump_str_escaped(out, v->ms[i].k, strlen(v->ms[i].k));
            dbuf_add(out, indent >= 0 ? ": " : ":");
            dump(v->ms[i].v, out, indent, depth + 1);
            if (i + 1 < v->nm) dbuf_addc(out, ',');
        }
        if (indent >= 0) { dbuf_addc(out, '\n'); dump_indent(out, depth, indent); }
        dbuf_addc(out, '}');
        break;
    }
}

void json_dump(const jv *v, dbuf *out, int indent) { dump(v, out, indent, 0); }

char *json_dump_str(const jv *v, int indent)
{
    dbuf b;
    dbuf_init(&b);
    dump(v, &b, indent, 0);
    return dbuf_steal(&b);
}

jv *jv_clone(const jv *v)
{
    if (!v) return NULL;
    switch (v->t) {
    case JNULL: return jnull();
    case JBOOL: return jbool(v->b);
    case JNUM:  return jnum(v->num ? v->num : "null");
    case JSTR:  return jstrn(v->s ? v->s : "", v->slen);
    case JARR: {
        jv *a = jarr();
        for (size_t i = 0; i < v->n; i++) jarr_push(a, jv_clone(v->items[i]));
        return a;
    }
    case JOBJ: {
        jv *o = jobj();
        for (size_t i = 0; i < v->nm; i++)
            jobj_set(o, v->ms[i].k, jv_clone(v->ms[i].v));
        return o;
    }
    }
    return NULL;
}
