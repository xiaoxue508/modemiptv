#include "cache.h"
#include "catchup.h"
#include "common.h"
#include "epgxml.h"
#include "gbk.h"
#include "http.h"
#include "json.h"
#include "platform.h"
#include "playlist.h"
#include "server.h"
#include "status.h"
#include "uplink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPTVD_VERSION "0.2.0"

static void usage(void)
{
    fputs(
        "usage: iptvd [global opts] <command> [opts]\n"
        "\n"
        "commands:\n"
        "  login       run EPG login, save session file\n"
        "  channels    fetch channel table -> channels.json\n"
        "  programs    catchup program list for channel/date\n"
        "  tvod        TVOD catchup -> rtsp url\n"
        "  live-sdp    live SDP url for a channel\n"
        "  playlist    build full M3U playlist\n"
        "  epg         build XMLTV EPG file\n"
        "  serve       run HTTP daemon (/status, playlist, epg, ...)\n"
        "  status      print status lines (daemon state); --json for machine use\n"
        "  config      print conf as JSON; 'config set k=v ...' / 'config write FILE' update\n"
        "  sign        print Authenticator hex for a challenge\n"
        "  selftest    offline golden vectors (CI)\n"
        "\n"
        "global opts:\n"
        "  --config PATH    config file (default /userconfig/iptv/iptvd.conf)\n"
        "  --session PATH   session json path\n"
        "  --eas HOST       EAS host\n"
        "  --epg HOST       EPG host or base url\n"
        "  --userid ID      STB userid\n"
        "  --stbid ID       STB stbid\n"
        "  --stbip IP       STB ip\n"
        "  --data-dir DIR   persistent dir (/userconfig/iptv)\n"
        "  --cache-dir DIR  volatile dir (/tmp/iptvd)\n"
        "  --port N         listen port\n"
        "  --version        print version\n",
        stdout);
}

static const char *optval(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s: missing value\n", argv[*i]);
        exit(2);
    }
    return argv[++(*i)];
}

/* ---------------- selftest (port-spec T1/T2 golden vectors) ------------- */

static int fail(const char *name, const char *got, const char *want)
{
    fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", name,
            got && *got ? got : "(empty)", want);
    return 1;
}

static int selftest(void)
{
    static const struct {
        const char *ch, *rnd, *want;
    } vec[] = {
        { "d41d8cd98f00b204e9800998ecf8427e", "12345678",
          "FFF971A50D2B1FB93708C8B175ADA4812585188CD0347261DF62FA22550F902D"
          "4E4D4FBDA0448D896654E67511807CE5A37B36D0543643915C475F3CED401439"
          "566C72EDB3B6156A7696ED90920BFD45F7BEBE133111C7AF9A00DCC97A1EB23C"
          "56EF5944658B38BFCD7286C8716F9A5BDABDDCFA033C48F9" },
        { "0123456789ABCDEF", "00000000",
          "DBEFF291C6F9F336DBA56F4C3E313014C36BFAAAC972A746627A663504958FC5"
          "A37B36D0543643915C475F3CED401439566C72EDB3B6156A7696ED90920BFD45"
          "F7BEBE133111C7AF9A00DCC97A1EB23C56EF5944658B38BFCD7286C8716F9A5B"
          "DABDDCFA033C48F9" },
        { "X", "00000000",
          "DBEFF291C6F9F336A46CF3AAA6F601CEA86ADCB22E8A904520956AF9BB50E299"
          "F17CF5328ECFA05FC69D1F33C6E26844A10A8645A3A9529A67A2D683503EB458"
          "7747AA5DE3D5C653FD612A6794BFCC7F481F6E5683E60014" },
    };
    int rc = 0;

    conf_defaults();
    conf_resolve_paths();

    for (size_t i = 0; i < sizeof vec / sizeof *vec; i++) {
        char name[32], *got = plat_sign(vec[i].ch, vec[i].rnd);
        snprintf(name, sizeof name, "sign[%zu]", i);
        if (strcmp(got, vec[i].want)) rc |= fail(name, got, vec[i].want);
        else printf("ok   %s (%zu hex)\n", name, strlen(got));
        free(got);
    }

    if (strcmp(g.stbmac_plain, "6CEFC689337E"))
        rc |= fail("stbmac_plain", g.stbmac_plain, "6CEFC689337E");
    else
        puts("ok   stbmac_plain");

    {   /* GBK strict decode */
        static const unsigned char gz[] = { 0xD6, 0xD0 };   /* GBK "中" */
        dbuf b;
        dbuf_init(&b);
        gbk_to_utf8(gz, sizeof gz, &b);
        if (b.len != 3 || memcmp(b.p, "\xE4\xB8\xAD", 3))
            rc |= fail("gbk_to_utf8", b.p, "中");
        else
            puts("ok   gbk_to_utf8");
        dbuf_free(&b);
    }

    {   /* fix_encoding fallback chain: gbk -> utf-8 */
        static const unsigned char raw[] = { 0xD6, 0xD0, 0xCE, 0xC4 };  /* 中文 */
        dbuf b;
        dbuf_init(&b);
        fix_encoding(raw, sizeof raw, NULL, &b);
        if (b.len != 6 || memcmp(b.p, "\xE4\xB8\xAD\xE6\x96\x87", 6))
            rc |= fail("fix_encoding", b.p, "中文");
        else
            puts("ok   fix_encoding");
        dbuf_free(&b);
    }

    {   /* json dump == python json.dumps(..., ensure_ascii=False, indent=1) */
        jv *o = jobj();
        char *s;
        const char *want = "{\n \"a\": 1,\n \"b\": \"中\"\n}";
        jobj_set(o, "a", jnum("1"));
        jobj_set(o, "b", jstr("中"));
        s = json_dump_str(o, 1);
        jv_free(o);
        if (strcmp(s, want)) rc |= fail("json_dump", s, want);
        else
            puts("ok   json_dump indent=1");
        free(s);
    }

    {   /* quote_plus */
        char *s = urlenc("a b+c/d");
        if (strcmp(s, "a+b%2Bc%2Fd")) rc |= fail("urlenc", s, "a+b%2Bc%2Fd");
        else
            puts("ok   urlenc quote_plus");
        free(s);
    }

    {   /* fixed UTC+8 time helpers */
        time_t t = 1759200000;
        struct tm tm8;
        gm8(t, &tm8);
        if (mk8(&tm8) != t) rc |= fail("gm8/mk8", "roundtrip", "identity");
        else
            puts("ok   gm8/mk8 roundtrip");
        gm8(0, &tm8);
        if (tm8.tm_hour != 8) rc |= fail("gm8(0).hour", "not 8", "8");
        else
            puts("ok   gm8 utc+8 offset");
    }

    if (rc) fprintf(stderr, "selftest FAILED\n");
    else printf("selftest all passed\n");
    return rc;
}

/* ------------------------- sign subcommand ------------------------------ */

static int cmd_sign(int argc, char **argv)
{
    const char *ch = NULL, *rnd = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--challenge")) ch = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--rnd")) rnd = optval(argc, argv, &i);
        else {
            fprintf(stderr, "sign: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!ch) {
        fprintf(stderr, "sign: --challenge required\n");
        return 2;
    }
    char *hex = plat_sign(ch, rnd);
    puts(hex);
    free(hex);
    return 0;
}


/* ------------------------- shared helpers -------------------------------- */

/* epg.py _channel_code: channel_code wins, else channels.json lookup */
static char *resolve_channel_code(const char *channel, const char *chcode)
{
    if (chcode && *chcode) return xstrdup(chcode);
    const char *want = channel ? channel : "None";

    size_t len;
    char *raw = read_file(g.channels, &len);
    if (!raw) {
        fprintf(stderr, "channel %s not found in channels.json\n", want);
        return NULL;
    }
    char *err = NULL;
    jv *data = json_parse(raw, len, &err);
    free(err);
    free(raw);
    jv *chs = NULL;
    if (data && data->t == JARR) chs = data;
    else if (data && data->t == JOBJ) {
        const jv *c = jobj_get(data, "channels");
        if (c && c->t == JARR) chs = jv_clone(c);
        jv_free(data);
    } else {
        jv_free(data);
    }
    char *code = NULL;
    if (chs && chs->t == JARR)
        for (size_t i = 0; i < chs->n; i++) {
            jv *c = chs->items[i];
            if (!c || c->t != JOBJ) continue;
            const jv *u = jobj_get(c, "user_channel_id");
            const jv *cid = jobj_get(c, "channel_id");
            const char *us = u && u->t == JSTR ? u->s
                           : (u && u->t == JNUM && u->num ? u->num : NULL);
            if (us && !strcmp(us, want)) {
                if (cid && cid->t == JSTR && cid->s) code = xstrdup(cid->s);
                break;
            }
        }
    jv_free(chs);
    if (!code)
        fprintf(stderr, "channel %s not found in channels.json\n", want);
    return code;
}

static jv *load_channels_file(void)
{
    size_t len;
    char *raw = read_file(g.channels, &len);
    if (!raw) return NULL;
    char *err = NULL;
    jv *data = json_parse(raw, len, &err);
    free(err);
    free(raw);
    if (!data) return NULL;
    if (data->t == JARR) return data;
    if (data->t == JOBJ) {
        const jv *c = jobj_get(data, "channels");
        jv *r = (c && c->t == JARR) ? jv_clone(c) : NULL;
        jv_free(data);
        return r;
    }
    jv_free(data);
    return NULL;
}

static const char *jval(const jv *c, const char *k)
{
    const jv *v = c && c->t == JOBJ ? jobj_get(c, k) : NULL;
    if (!v || v->t == JNULL) return "";
    if (v->t == JSTR) return v->s ? v->s : "";
    if (v->t == JNUM) return v->num ? v->num : "0";
    if (v->t == JBOOL) return v->b ? "true" : "false";
    return "";
}

/* ------------------------- login ----------------------------------------- */

static int cmd_login(int argc, char **argv)
{
    const char *save = g.session;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--save")) save = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--auth-mode")) {
            const char *m = optval(argc, argv, &i);
            if (strcmp(m, "sign")) {
                fprintf(stderr, "login: only --auth-mode sign is supported\n");
                return 2;
            }
        } else {
            fprintf(stderr, "login: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    char err[512];
    err[0] = 0;
    if (plat_login(save, err, sizeof err) != 0) {
        fprintf(stderr, "LOGIN FAILED: %s\n", err);
        return 1;
    }
    /* epg.py prints user_token/user_group/epg_group/challenge/authenticator/
       channels; challenge+authenticator are not retained in-process */
    int nch = 0;
    dbuf dummy;
    dbuf_init(&dummy);
    if (plat_channels_json(&dummy, &nch) != 0) nch = 0;
    dbuf_free(&dummy);

    jv *o = jobj();
    jobj_set(o, "user_token", jstr(g_sess.user_token));
    jobj_set(o, "user_group", jstr(g_sess.user_grp));
    jobj_set(o, "epg_group", jstr(g_sess.epg_grp));
    char num[16];
    snprintf(num, sizeof num, "%d", nch);
    jobj_set(o, "channels", jnum(num));
    char *s = json_dump_str(o, 1);
    jv_free(o);
    puts(s);
    free(s);
    return 0;
}

/* ------------------------- channels -------------------------------------- */

static int cmd_channels(int argc, char **argv)
{
    const char *out_path = g.channels;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--out")) out_path = optval(argc, argv, &i);
        else {
            fprintf(stderr, "channels: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    dbuf out;
    dbuf_init(&out);
    int n = 0;
    if (plat_channels_json(&out, &n) != 0) {
        fprintf(stderr, "channels fetch failed\n");
        dbuf_free(&out);
        return 1;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f || (out.len && fwrite(out.p, 1, out.len, f) != out.len)) {
        if (f) fclose(f);
        fprintf(stderr, "cannot write %s\n", out_path);
        dbuf_free(&out);
        return 1;
    }
    fclose(f);
    printf("channels: %d -> %s\n", n, out_path);

    char *err = NULL;
    jv *data = json_parse(out.p ? out.p : "", out.len, &err);
    free(err);
    dbuf_free(&out);
    jv *chs = NULL;
    if (data && data->t == JARR) chs = data;
    else if (data && data->t == JOBJ) {
        const jv *c = jobj_get(data, "channels");
        if (c && c->t == JARR) chs = jv_clone(c);
        jv_free(data);
    } else {
        jv_free(data);
    }
    if (chs && chs->t == JARR) {
        for (size_t i = 0; i < chs->n && i < 5; i++) {
            jv *c = chs->items[i];
            printf("  %4s %s %s fcc=%s\n", jval(c, "user_channel_id"),
                   jval(c, "name"), jval(c, "channel_url"), jval(c, "fcc"));
        }
    }
    jv_free(chs);
    return 0;
}

/* ------------------------- programs -------------------------------------- */

static int cmd_programs(int argc, char **argv)
{
    const char *channel = NULL, *chcode = NULL, *date = NULL;
    int as_json = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--channel")) channel = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--channel-code")) chcode = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--date")) date = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--json")) as_json = 1;
        else {
            fprintf(stderr, "programs: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!date) {
        fprintf(stderr, "programs: --date required\n");
        return 2;
    }
    char *code = resolve_channel_code(channel, chcode);
    if (!code) return 1;

    jv *progs = plat_programs(code, date);
    free(code);
    if (!progs) return 1;

    if (as_json) {
        char *s = json_dump_str(progs, 1);
        puts(s);
        free(s);
    } else if (progs->t == JARR) {
        for (size_t i = 0; i < progs->n; i++) {
            jv *p = progs->items[i];
            printf("%s  %s-%.5s  st=%s  %s\n", jval(p, "prevuecode"),
                   jval(p, "showtime"), jval(p, "endtime"), jval(p, "status"),
                   jval(p, "prevuename"));
        }
    }
    printf("# %zu programs\n", progs->t == JARR ? progs->n : 0);
    jv_free(progs);
    return 0;
}

/* ------------------------- tvod ------------------------------------------ */

static int cmd_tvod(int argc, char **argv)
{
    const char *pc = NULL, *channel = NULL, *chcode = NULL, *mixno = NULL;
    const char *columncode = "1D04", *date = NULL;
    int isfromchannel = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--prevuecode")) pc = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--channel")) channel = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--channel-code")) chcode = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--mixno")) mixno = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--columncode")) columncode = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--date")) date = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--isfromchannel")) isfromchannel = 1;
        else {
            fprintf(stderr, "tvod: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!pc || !date) {
        fprintf(stderr, "tvod: --prevuecode and --date required\n");
        return 2;
    }
    char *code = resolve_channel_code(channel, chcode);
    if (!code) return 1;

    /* mixno: --mixno | --channel | lookup by code | "1" (epg.py _channel_user_id) */
    char *mixbuf = NULL;
    if (!mixno) {
        if (channel) {
            mixno = channel;
        } else {
            mixno = "1";
            jv *chs = load_channels_file();
            if (chs && chs->t == JARR)
                for (size_t i = 0; i < chs->n; i++) {
                    jv *c = chs->items[i];
                    if (!c || c->t != JOBJ) continue;
                    const jv *cid = jobj_get(c, "channel_id");
                    if (cid && cid->t == JSTR && cid->s && !strcmp(cid->s, code)) {
                        mixbuf = xstrdup(jval(c, "user_channel_id"));
                        mixno = mixbuf;
                        break;
                    }
                }
            jv_free(chs);
        }
    }

    dbuf out;
    dbuf_init(&out);
    int rc = plat_tvod(pc, code, mixno, columncode, date, isfromchannel, &out);
    free(code);
    free(mixbuf);
    if (rc != 0) {
        fprintf(stderr, "tvod resolve failed for %s\n", pc);
        dbuf_free(&out);
        return 1;
    }
    puts(out.p ? out.p : "");
    dbuf_free(&out);
    return 0;
}

/* ------------------------- live-sdp -------------------------------------- */

static int cmd_live_sdp(int argc, char **argv)
{
    const char *channel = NULL, *channel_id = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--channel")) channel = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "--channel-id")) channel_id = optval(argc, argv, &i);
        else {
            fprintf(stderr, "live-sdp: unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    jv *chs = NULL;
    if (g_sess.have) {   /* --session given -> live epg.channels() */
        dbuf out;
        dbuf_init(&out);
        int n = 0;
        if (plat_channels_json(&out, &n) == 0) {
            char *err = NULL;
            jv *data = json_parse(out.p ? out.p : "", out.len, &err);
            free(err);
            if (data && data->t == JARR) chs = data;
            else if (data && data->t == JOBJ) {
                const jv *c = jobj_get(data, "channels");
                if (c && c->t == JARR) chs = jv_clone(c);
                jv_free(data);
            } else {
                jv_free(data);
            }
        }
        dbuf_free(&out);
    } else {
        chs = load_channels_file();
    }

    jv *hit = NULL;
    if (chs && chs->t == JARR)
        for (size_t i = 0; i < chs->n && !hit; i++) {
            jv *c = chs->items[i];
            if (!c || c->t != JOBJ) continue;
            if (channel && !strcmp(jval(c, "user_channel_id"), channel)) hit = c;
            else if (!channel && channel_id &&
                     !strcmp(jval(c, "channel_id"), channel_id)) hit = c;
        }
    if (!hit) {
        fprintf(stderr, "channel not found\n");
        jv_free(chs);
        return 1;
    }
    printf("%s %s\n  igmp %s\n  sdp  %s\n", jval(hit, "user_channel_id"),
           jval(hit, "name"), jval(hit, "channel_url"), jval(hit, "sdp"));
    jv_free(chs);
    return 0;
}

/* ------------------------- playlist / epg / status ----------------------- */

static int cmd_playlist(int argc, char **argv)
{
    int r2h_http = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--r2h-http")) r2h_http = 1;
        else {
            fprintf(stderr, "playlist: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    dbuf out;
    dbuf_init(&out);
    int nm = 0, nf = 0, nu = 0;
    if (playlist_build(r2h_http, &out, &nm, &nf, &nu) != 0) {
        fprintf(stderr, "%s\n", out.p ? out.p : "playlist build failed");
        dbuf_free(&out);
        return 1;
    }
    fwrite(out.p, 1, out.len, stdout);
    dbuf_free(&out);
    fprintf(stderr, "mcast=%d fcc=%d unicast=%d\n", nm, nf, nu);
    return 0;
}

static int cmd_epg(void)
{
    cache_load_disk();
    int rc = epgxml_build();
    if (rc == 0) epgxml_mark_ready();
    return rc == 0 ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    int as_json = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) as_json = 1;
        else {
            fprintf(stderr, "status: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    cache_load_disk();
    dbuf out;
    dbuf_init(&out);
    if (as_json) status_json(&out);
    else status_page(&out);
    fwrite(out.p, 1, out.len, stdout);
    dbuf_free(&out);
    return 0;
}

/* ---------------- config (conf file dump/merge for luci-app-iptvd) ------- */

static int is_integer(const char *s)
{
    if (!*s) return 0;
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* trim a raw line into a buffer, return 1 when it carries key=value */
static int line_key(const char *s, size_t l, char *kb, size_t kbsz)
{
    while (l && (*s == ' ' || *s == '\t')) { s++; l--; }
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) l--;
    if (!l || *s == '#' || *s == ';') return 0;
    const char *eq = memchr(s, '=', l);
    if (!eq) return 0;
    size_t klen = (size_t)(eq - s);
    while (klen && (s[klen - 1] == ' ' || s[klen - 1] == '\t')) klen--;
    if (!klen || klen + 1 > kbsz) return 0;
    memcpy(kb, s, klen);
    kb[klen] = 0;
    return 1;
}

/* whole-file sanity check: every non-comment line needs key=value, integer
   keys need integer values. Unknown keys pass (forward compatible, like
   conf_load). Prints "config: line N: ..." and returns -1 on the first bad
   line — callers must not write the file in that case. */
static int conf_validate_text(const char *text)
{
    int lineno = 0;
    const char *p = text ? text : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        lineno++;
        char kb[128];
        if (line_key(p, len, kb, sizeof kb)) {
            const char *eq = memchr(p, '=', len);
            const char *val = eq + 1;
            size_t vl = (size_t)(p + len - val);
            while (vl && (*val == ' ' || *val == '\t')) { val++; vl--; }
            while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '\t' ||
                          val[vl - 1] == '\r')) vl--;
            char vbuf[512];
            if (vl >= sizeof vbuf) {
                fprintf(stderr, "config: line %d: value too long\n", lineno);
                return -1;
            }
            memcpy(vbuf, val, vl);
            vbuf[vl] = 0;
            /* inline comment " # ..." like conf_load strips */
            char *hash = strstr(vbuf, " #");
            if (hash) { *hash = 0; while (hash > vbuf && (hash[-1] == ' ' || hash[-1] == '\t')) *--hash = 0; }
            if (conf_key_is_num(kb) && !is_integer(vbuf)) {
                fprintf(stderr, "config: line %d: %s must be an integer\n",
                        lineno, kb);
                return -1;
            }
        } else {
            const char *s = p; size_t l = len;
            while (l && (*s == ' ' || *s == '\t')) { s++; l--; }
            while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) l--;
            if (l && *s != '#' && *s != ';') {
                fprintf(stderr, "config: line %d: expected key=value\n", lineno);
                return -1;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

/* merge key=val into conf text: replace the first matching line (drop any
   later duplicates so conf_load's last-wins can't resurrect them), else
   append. Always returns a fresh malloc'd buffer. */
static char *conf_merge_kv(const char *raw, const char *key, const char *val)
{
    dbuf out;
    dbuf_init(&out);
    int found = 0;
    const char *p = raw ? raw : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char kb[128];
        if (line_key(p, len, kb, sizeof kb) && !strcmp(kb, key)) {
            if (!found) {
                dbuf_addf(&out, "%s=%s", key, val);
                if (nl) dbuf_addc(&out, '\n');
                found = 1;
            }
            /* later duplicate: drop (conf_load is last-wins) */
        } else {
            dbuf_addn(&out, p, len);
            if (nl) dbuf_addc(&out, '\n');
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (!found) {
        if (out.len && out.p && out.p[out.len - 1] != '\n') dbuf_addc(&out, '\n');
        dbuf_addf(&out, "%s=%s\n", key, val);
    }
    if (!out.p) out.p = xstrdup("");
    return out.p;
}

static int cmd_config(int argc, char **argv, const char *path)
{
    if (argc == 0) {
        jv *o = jobj();
        char num[32];
#define KS(k, v) jobj_set(o, k, jstr(v))
#define KN(k, v) do { snprintf(num, sizeof num, "%d", (int)(v)); \
                      jobj_set(o, k, jnum(num)); } while (0)
        KS("eas_host", g.eas_host);
        KS("epg_host", g.epg_host);
        KS("userid", g.userid);
        KS("stbid", g.stbid);
        KS("stbip", g.stbip_auto ? "auto" : g.stbip);
        KS("stbmac", g.stbmac);
        KS("auth_key", g.auth_key);
        KS("stbtype", g.stbtype);
        KS("stbversion", g.stbversion);
        KS("ua", g.ua);
        KS("xhr", g.xhr);
        KN("timeout", g.timeout);
        KS("r2h", g.r2h);
        KS("m3u_epg_url", g.m3u_epg_url);
        KS("bridge_tpl", g.bridge_tpl);
        KS("gen_url", g.gen_url);
        KN("ttl_progs", g.ttl_progs);
        KN("ttl_tvod", g.ttl_tvod);
        KN("ttl_epg", g.ttl_epg);
        KN("ttl_channels", g.ttl_channels);
        KN("min_channels", g.min_channels);
        KN("epg_past", g.epg_past);
        KN("epg_future", g.epg_future);
        KN("worker_s", g.worker_s);
        KN("port", g.port);
        KN("xmltv_wait_s", g.xmltv_wait_s);
        KS("upstream_interface", g.upstream_interface);
        KN("uplink_policy", g.uplink_policy);
        KS("data_dir", g.data_dir);
        KS("cache_dir", g.cache_dir);
        KS("session", g.session);
        KS("channels", g.channels);
        KS("epg_file", g.epg_file);
#undef KS
#undef KN
        char *s = json_dump_str(o, -1);
        jv_free(o);
        puts(s);
        free(s);
        return 0;
    }

    if (!strcmp(argv[0], "set")) {
        if (argc < 2) {
            fprintf(stderr, "config set: expected key=value\n");
            return 2;
        }
        for (int i = 1; i < argc; i++) {
            const char *kv = argv[i];
            const char *eq = strchr(kv, '=');
            if (!eq) {
                fprintf(stderr, "config set: expected key=value: %s\n", kv);
                return 2;
            }
            size_t klen = (size_t)(eq - kv);
            if (!klen || klen >= 128) {
                fprintf(stderr, "config set: bad key\n");
                return 2;
            }
            char key[128];
            memcpy(key, kv, klen);
            key[klen] = 0;
            const char *val = eq + 1;
            if (strchr(val, '\n') || strchr(val, '\r')) {
                fprintf(stderr, "config set: value must be single-line\n");
                return 2;
            }
            if (!conf_valid_key(key)) {
                fprintf(stderr, "config set: unknown key '%s'\n", key);
                return 2;
            }
            if (conf_key_is_num(key) && !is_integer(val)) {
                fprintf(stderr, "config set: %s must be an integer\n", key);
                return 2;
            }
        }
        size_t len;
        char *raw = read_file(path, &len);
        char *acc = raw ? raw : xstrdup("");
        for (int i = 1; i < argc; i++) {
            const char *eq = strchr(argv[i], '=');
            char key[128];
            size_t klen = (size_t)(eq - argv[i]);
            memcpy(key, argv[i], klen);
            key[klen] = 0;
            char *m = conf_merge_kv(acc, key, eq + 1);
            free(acc);
            acc = m;
        }
        if (conf_validate_text(acc) != 0) {
            free(acc);
            return 1;
        }
        if (write_atomic(path, acc, strlen(acc)) != 0) {
            fprintf(stderr, "config set: write %s failed\n", path);
            free(acc);
            return 1;
        }
        free(acc);
        puts("ok");
        return 0;
    }

    if (!strcmp(argv[0], "write")) {
        if (argc != 2) {
            fprintf(stderr, "config write: expected SOURCE file\n");
            return 2;
        }
        size_t len;
        char *src = read_file(argv[1], &len);
        if (!src) {
            fprintf(stderr, "config write: cannot read %s\n", argv[1]);
            return 1;
        }
        if (conf_validate_text(src) != 0) {
            free(src);
            return 1;
        }
        if (write_atomic(path, src, strlen(src)) != 0) {
            fprintf(stderr, "config write: write %s failed\n", path);
            free(src);
            return 1;
        }
        free(src);
        puts("ok");
        return 0;
    }

    fprintf(stderr, "config: unknown argument %s\n", argv[0]);
    return 2;
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *config = "/userconfig/iptv/iptvd.conf";
    const char *cmd = NULL;
    int session_given = 0;
    int i;

    conf_defaults();

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') { cmd = a; i++; break; }
        if (!strcmp(a, "--version")) { printf("iptvd " IPTVD_VERSION "\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        if (!strcmp(a, "--config")) { config = optval(argc, argv, &i); continue; }
        if (!strcmp(a, "--session")) { session_given = 1; snprintf(g.session, sizeof g.session, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--eas")) { snprintf(g.eas_host, sizeof g.eas_host, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--epg")) { snprintf(g.epg_host, sizeof g.epg_host, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--userid")) { snprintf(g.userid, sizeof g.userid, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--stbid")) { snprintf(g.stbid, sizeof g.stbid, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--stbip")) { snprintf(g.stbip, sizeof g.stbip, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--data-dir")) { snprintf(g.data_dir, sizeof g.data_dir, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--cache-dir")) { snprintf(g.cache_dir, sizeof g.cache_dir, "%s", optval(argc, argv, &i)); continue; }
        if (!strcmp(a, "--port")) { g.port = atoi(optval(argc, argv, &i)); continue; }
        fprintf(stderr, "unknown option: %s\n", a);
        usage();
        return 2;
    }

    if (!cmd) { usage(); return 2; }

    int offline = !strcmp(cmd, "selftest") || !strcmp(cmd, "sign");

    if (!offline) {
        if (conf_load(config) < 0 && strcmp(config, "/userconfig/iptv/iptvd.conf"))
            logmsg("warning: cannot read %s, using defaults", config);
    }
    conf_resolve_paths();

    if (!strcmp(cmd, "selftest")) return selftest();
    if (!strcmp(cmd, "sign")) return cmd_sign(argc - i, argv + i);
    if (!strcmp(cmd, "config")) return cmd_config(argc - i, argv + i, config);

    if (strcmp(cmd, "login") && strcmp(cmd, "channels") &&
        strcmp(cmd, "programs") && strcmp(cmd, "tvod") &&
        strcmp(cmd, "live-sdp") && strcmp(cmd, "playlist") &&
        strcmp(cmd, "epg") && strcmp(cmd, "serve") && strcmp(cmd, "status")) {
        fprintf(stderr, "%s: unknown command\n", cmd);
        return 2;
    }

    uplink_ensure(1);
    http_init();

    if (!strcmp(cmd, "login")) return cmd_login(argc - i, argv + i);

    /* epg.py: --session given -> load it or die (after the login branch) */
    if (session_given && plat_load_session(g.session) != 0) {
        fprintf(stderr, "session file %s not found (run 'login' first)\n", g.session);
        return 1;
    }

    if (!strcmp(cmd, "channels")) return cmd_channels(argc - i, argv + i);
    if (!strcmp(cmd, "programs")) return cmd_programs(argc - i, argv + i);
    if (!strcmp(cmd, "tvod")) return cmd_tvod(argc - i, argv + i);
    if (!strcmp(cmd, "live-sdp")) return cmd_live_sdp(argc - i, argv + i);
    if (!strcmp(cmd, "playlist")) return cmd_playlist(argc - i, argv + i);
    if (!strcmp(cmd, "epg")) return cmd_epg();
    if (!strcmp(cmd, "serve")) return server_run() == 0 ? 0 : 1;
    return cmd_status(argc - i, argv + i);
}
