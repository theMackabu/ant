#ifndef TLSUV_BORINGSSL_KEYS_H
#define TLSUV_BORINGSSL_KEYS_H

#include <tlsuv/tls_engine.h>

struct cert_s {
  TLSUV_CERT_API
  X509_STORE *cert;
  char *text;
};

struct pub_key_s {
  TLSUV_PUBKEY_API
  EVP_PKEY *pkey;
};

struct priv_key_s {
  TLSUV_PRIVKEY_API
  EVP_PKEY *pkey;
};

const char *tls_error(unsigned long code);

void pub_key_init(struct pub_key_s *pubkey);
void cert_init(struct cert_s *c);

int gen_key(tlsuv_private_key_t *key);
int load_key(tlsuv_private_key_t *key, const char *keydata, size_t keydatalen);
int verify_signature(EVP_PKEY *pk, enum hash_algo md, const char *data, size_t datalen, const char *sig, size_t siglen);

#endif
