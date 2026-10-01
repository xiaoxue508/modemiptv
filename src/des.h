#ifndef IPTVD_DES_H
#define IPTVD_DES_H
#include <stddef.h>

/* DES-ECB, len must be a multiple of 8 */
void des_ecb_encrypt(const unsigned char key[8], const unsigned char *in,
                     size_t len, unsigned char *out);

/* port-spec §2.2: PKCS#5 pad + DES-ECB + hex upper.
   pt = rnd $ challenge $ userid $ stbid $ stbip $ mac_plain $$CTC
   out must hold 2*ceil((ptlen+?)/8)... caller passes buffer of size n. */
char *des_sign(const char *challenge, const char *rnd,
               const char *userid, const char *stbid, const char *stbip,
               const char *mac_plain, const char *key, const char *tail,
               char *out, size_t outsz);

#endif
