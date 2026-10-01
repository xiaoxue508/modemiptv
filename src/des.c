/* Public-domain style DES (ECB), written for iptvd.
 * Verified against pycryptodome via golden vectors (port-spec §6 T1/T2). */
#include "des.h"
#include "common.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const unsigned char IP[64] = {
    58,50,42,34,26,18,10,2, 60,52,44,36,28,20,12,4,
    62,54,46,38,30,22,14,6, 64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1,  59,51,43,35,27,19,11,3,
    61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7 };

static const unsigned char FP[64] = {
    40,8,48,16,56,24,64,32, 39,7,47,15,55,23,63,31,
    38,6,46,14,54,22,62,30, 37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28, 35,3,43,11,51,19,59,27,
    34,2,42,10,50,18,58,26, 33,1,41,9,49,17,57,25 };

static const unsigned char E[48] = {
    32,1,2,3,4,5, 4,5,6,7,8,9, 8,9,10,11,12,13,
    12,13,14,15,16,17, 16,17,18,19,20,21,
    20,21,22,23,24,25, 24,25,26,27,28,29,
    28,29,30,31,32,1 };

static const unsigned char P[32] = {
    16,7,20,21,29,12,28,17, 1,15,23,26,5,18,31,10,
    2,8,24,14,32,27,3,9,    19,13,30,6,22,11,4,25 };

static const unsigned char PC1[56] = {
    57,49,41,33,25,17,9, 1,58,50,42,34,26,18,
    10,2,59,51,43,35,27, 19,11,3,60,52,44,36,
    63,55,47,39,31,23,15, 7,62,54,46,38,30,22,
    14,6,61,53,45,37,29, 21,13,5,28,20,12,4 };

static const unsigned char PC2[48] = {
    14,17,11,24,1,5, 3,28,15,6,21,10,
    23,19,12,4,26,8, 16,7,27,20,13,2,
    41,52,31,37,47,55, 30,40,51,45,33,48,
    44,49,39,56,34,53, 46,42,50,36,29,32 };

static const unsigned char SH[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};

static const unsigned char S[8][64] = {
{14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7, 0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
 4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0, 15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
{15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10, 3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
 0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15, 13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
{10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8, 13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
 13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7, 1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
{7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15, 13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
 10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4, 3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
{2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9, 14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
 4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14, 11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
{12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11, 10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
 9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6, 4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
{4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1, 13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
 1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2, 6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
{13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7, 1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
 7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8, 2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}};

static uint64_t load64be(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void store64be(uint64_t v, unsigned char *p)
{
    for (int i = 7; i >= 0; i--) { p[i] = (unsigned char)(v & 0xFF); v >>= 8; }
}

/* permute: table entries are 1-based source positions, MSB-first.
   nin = source width (bits), nout = output bits (taken from table head). */
static uint64_t perm(uint64_t in, const unsigned char *t, int nin, int nout)
{
    uint64_t out = 0;
    for (int i = 0; i < nout; i++) {
        int src = t[i] - 1;
        out = (out << 1) | ((in >> (nin - 1 - src)) & 1u);
    }
    return out;
}

static uint32_t sbox48(uint64_t v48)
{
    uint32_t out = 0;
    for (int i = 0; i < 8; i++) {
        unsigned b = (unsigned)((v48 >> (42 - 6 * i)) & 0x3F);
        int row = (int)(((b >> 4) & 2) | (b & 1));
        int col = (int)((b >> 1) & 0xF);
        out = (out << 4) | S[i][row * 16 + col];
    }
    return out;
}

static void des_block(const unsigned char key[8], const unsigned char in[8],
                      unsigned char out[8])
{
    uint64_t k = load64be(key);
    uint64_t ip = perm(load64be(in), IP, 64, 64);
    uint32_t l = (uint32_t)(ip >> 32), r = (uint32_t)ip;

    uint64_t k56 = perm(k, PC1, 64, 56);
    uint32_t c = (uint32_t)(k56 >> 28) & 0x0FFFFFFF;
    uint32_t d = (uint32_t)k56 & 0x0FFFFFFF;

    for (int round = 0; round < 16; round++) {
        int s = SH[round];
        c = ((c << s) | (c >> (28 - s))) & 0x0FFFFFFF;
        d = ((d << s) | (d >> (28 - s))) & 0x0FFFFFFF;
        uint64_t subk = perm(((uint64_t)c << 28) | d, PC2, 56, 48);
        uint64_t x = perm(r, E, 32, 48) ^ subk;
        uint32_t sb = sbox48(x);
        uint32_t f = (uint32_t)perm(sb, P, 32, 32);
        uint32_t nr = l ^ f;
        l = r;
        r = nr;
    }
    uint64_t pre = ((uint64_t)r << 32) | l;   /* final swap */
    store64be(perm(pre, FP, 64, 64), out);
}

void des_ecb_encrypt(const unsigned char key[8], const unsigned char *in,
                     size_t len, unsigned char *out)
{
    for (size_t i = 0; i + 8 <= len; i += 8)
        des_block(key, in + i, out + i);
}

char *des_sign(const char *challenge, const char *rnd,
               const char *userid, const char *stbid, const char *stbip,
               const char *mac_plain, const char *key, const char *tail,
               char *out, size_t outsz)
{
    dbuf pt;
    dbuf_init(&pt);
    dbuf_addf(&pt, "%s$%s$%s$%s$%s$%s%s", rnd, challenge, userid, stbid,
              stbip, mac_plain, tail);          /* mac_plain 后直接接 $$CTC (R10) */
    size_t n = pt.len;
    size_t pad = 8 - (n % 8);                   /* %8==0 时补 8 字节 (R09) */
    char *raw = xmalloc(n + pad);
    memcpy(raw, pt.p ? pt.p : "", n);
    memset(raw + n, (int)pad, pad);
    dbuf_free(&pt);

    size_t clen = n + pad;
    unsigned char *ct = xmalloc(clen);
    des_ecb_encrypt((const unsigned char *)key, (const unsigned char *)raw,
                    clen, ct);
    free(raw);
    if (outsz < clen * 2 + 1) { free(ct); out[0] = 0; return out; }
    for (size_t i = 0; i < clen; i++) sprintf(out + 2 * i, "%02X", ct[i]);
    out[clen * 2] = 0;
    free(ct);
    return out;
}
