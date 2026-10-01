#ifndef IPTVD_JSON_H
#define IPTVD_JSON_H
#include "common.h"

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } jtype;

typedef struct jv jv;
typedef struct { char *k; jv *v; } jmember;

struct jv {
    jtype t;
    int b;              /* JBOOL */
    char *num;          /* JNUM: raw source text (kept verbatim on dump) */
    char *s;            /* JSTR */
    size_t slen;
    jv **items;         /* JARR */
    size_t n, cap;
    jmember *ms;        /* JOBJ, insertion order preserved */
    size_t nm, mcap;
};

jv *json_parse(const char *s, size_t n, char **err);
void jv_free(jv *v);

jv *jnull(void);
jv *jbool(int b);
jv *jnum(const char *raw);
jv *jstr(const char *s);              /* UTF-8 */
jv *jstrn(const char *s, size_t n);
jv *jarr(void);
jv *jobj(void);
void jarr_push(jv *a, jv *v);
void jobj_set(jv *o, const char *k, jv *v);   /* replace in place if key exists */
const jv *jobj_get(const jv *o, const char *k);

/* indent >= 0 : python json.dumps(..., indent=indent) layout
   indent < 0  : compact (default json.dumps) */
void json_dump(const jv *v, dbuf *out, int indent);
char *json_dump_str(const jv *v, int indent);

/* python json.dumps(str, ensure_ascii=False) escaping of one string */
void json_dump_str_escaped(dbuf *out, const char *s, size_t n);

jv *jv_clone(const jv *v);      /* deep copy; NULL in -> NULL out */

#endif
