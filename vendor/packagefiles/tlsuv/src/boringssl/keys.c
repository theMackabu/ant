#define OPENSSL_SUPPRESS_DEPRECATED

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/asn1.h>
#include <openssl/x509.h>

#include <assert.h>
#include <string.h>

#include "../alloc.h"
#include "../um_debug.h"
#include "keys.h"

static const char *cert_to_text(const struct tlsuv_certificate_s *cert);
static int cert_to_pem(const struct tlsuv_certificate_s *cert, int full, char **pem, size_t *pemlen);
static void cert_free(tlsuv_certificate_t cert);
static int cert_verify(const struct tlsuv_certificate_s *cert, enum hash_algo md, const char *data, size_t datalen, const char *sig, size_t siglen);
static int cert_exp(const struct tlsuv_certificate_s *cert, struct tm *time);

static struct cert_s cert_API = {
  .free = cert_free,
  .verify = cert_verify,
  .to_pem = cert_to_pem,
  .get_text = cert_to_text,
  .get_expiration = cert_exp,
};

static int pubkey_to_pem(tlsuv_public_key_t pub, char **pem, size_t *pemlen);
static void pubkey_free(tlsuv_public_key_t k);
static int pubkey_verify(tlsuv_public_key_t pk, enum hash_algo md, const char *data, size_t datalen, const char *sig, size_t siglen);

static struct pub_key_s PUB_KEY_API = {
  .free = pubkey_free,
  .verify = pubkey_verify,
  .to_pem = pubkey_to_pem,
};

static void privkey_free(tlsuv_private_key_t k);
static tlsuv_public_key_t privkey_pubkey(tlsuv_private_key_t pk);
static int privkey_to_pem(tlsuv_private_key_t pk, char **pem, size_t *pemlen);
static int privkey_sign(tlsuv_private_key_t pk, enum hash_algo md, const char *data, size_t datalen, char *sig, size_t *siglen);
static int privkey_get_cert(tlsuv_private_key_t pk, tlsuv_certificate_t *cert);
static int privkey_store_cert(tlsuv_private_key_t pk, tlsuv_certificate_t cert);

static struct priv_key_s PRIV_KEY_API = {
  .free = privkey_free,
  .to_pem = privkey_to_pem,
  .pubkey = privkey_pubkey,
  .sign = privkey_sign,
  .get_certificate = privkey_get_cert,
  .store_certificate = privkey_store_cert,
};

static tlsuv_private_key_t new_private_key(EVP_PKEY *pkey) {
  struct priv_key_s *private_key = tlsuv__calloc(1, sizeof(struct priv_key_s));
  *private_key = PRIV_KEY_API;
  private_key->pkey = pkey;
  return (tlsuv_private_key_t)private_key;
}

void pub_key_init(struct pub_key_s *pubkey) {
  *pubkey = PUB_KEY_API;
}

void cert_init(struct cert_s *c) {
  *c = cert_API;
}

static void pubkey_free(tlsuv_public_key_t k) {
  struct pub_key_s *pub = (struct pub_key_s *)k;
  EVP_PKEY_free(pub->pkey);
  tlsuv__free(pub);
}

static int verify_ecdsa_sig(EC_KEY *ec, const EVP_MD *hash, const char *data, size_t datalen, const char *sig, size_t siglen) {
  int rc;
  ECDSA_SIG *ecdsa_sig = NULL;
  EVP_MD_CTX *digestor = EVP_MD_CTX_new();
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len;

  EVP_DigestInit(digestor, hash);
  EVP_DigestUpdate(digestor, data, datalen);
  rc = EVP_DigestFinal(digestor, digest, &digest_len);

  if (rc == 1) {
    BIGNUM *r = BN_bin2bn((const uint8_t *)sig, (int)(siglen / 2), NULL);
    BIGNUM *s = BN_bin2bn((const uint8_t *)sig + siglen / 2, (int)(siglen / 2), NULL);

    ecdsa_sig = ECDSA_SIG_new();
    ECDSA_SIG_set0(ecdsa_sig, r, s);
    rc = ECDSA_do_verify(digest, (int)digest_len, ecdsa_sig, ec);
  }

  ECDSA_SIG_free(ecdsa_sig);
  EVP_MD_CTX_free(digestor);
  return rc == 1 ? 0 : -1;
}

int verify_signature(EVP_PKEY *pk, enum hash_algo md, const char *data, size_t datalen, const char *sig, size_t siglen) {
  const EVP_MD *hash = NULL;
  switch (md) {
    case hash_SHA256: hash = EVP_sha256(); break;
    case hash_SHA384: hash = EVP_sha384(); break;
    case hash_SHA512: hash = EVP_sha512(); break;
    default: break;
  }

  EVP_MD_CTX *digestor = EVP_MD_CTX_new();
  if (EVP_DigestVerifyInit(digestor, NULL, hash, NULL, pk) != 1 ||
    EVP_DigestVerifyUpdate(digestor, data, datalen) != 1) {
    UM_LOG(WARN, "failed to create digest: %s", tls_error(ERR_get_error()));
    EVP_MD_CTX_free(digestor);
    return -1;
  }

  int rc = EVP_DigestVerifyFinal(digestor, (const uint8_t *)sig, siglen);
  EVP_MD_CTX_free(digestor);

  if (rc != 1 && EVP_PKEY_id(pk) == EVP_PKEY_EC) {
    const uint8_t *p = (const uint8_t *)sig;
    ECDSA_SIG *ecdsa_sig = d2i_ECDSA_SIG(NULL, &p, (int)siglen);
    if (ecdsa_sig == NULL) {
      EC_KEY *ec = EVP_PKEY_get1_EC_KEY(pk);
      int verified = verify_ecdsa_sig(ec, hash, data, datalen, sig, siglen);
      EC_KEY_free(ec);
      return verified;
    }
    ECDSA_SIG_free(ecdsa_sig);
  }

  return rc == 1 ? 0 : -1;
}

static int pubkey_verify(tlsuv_public_key_t pk, enum hash_algo md, const char *data, size_t datalen, const char *sig, size_t siglen) {
  struct pub_key_s *pub = (struct pub_key_s *)pk;
  return verify_signature(pub->pkey, md, data, datalen, sig, siglen);
}

static void privkey_free(tlsuv_private_key_t k) {
  struct priv_key_s *priv = (struct priv_key_s *)k;
  EVP_PKEY_free(priv->pkey);
  tlsuv__free(priv);
}

static int privkey_sign(tlsuv_private_key_t pk, enum hash_algo md, const char *data, size_t datalen, char *sig, size_t *siglen) {
  struct priv_key_s *priv = (struct priv_key_s *)pk;
  int rc = 0;
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  EVP_PKEY_CTX *pctx = NULL;

  const EVP_MD *hash = NULL;
  switch (md) {
    case hash_SHA256: hash = EVP_sha256(); break;
    case hash_SHA384: hash = EVP_sha384(); break;
    case hash_SHA512: hash = EVP_sha512(); break;
    default: break;
  }

  if ((EVP_DigestSignInit(digest, &pctx, hash, NULL, priv->pkey) != 1) ||
    (EVP_DigestSignUpdate(digest, data, datalen) != 1)) {
    unsigned long err = ERR_get_error();
    UM_LOG(WARN, "failed to setup digest %ld/%s", err, ERR_lib_error_string(err));
    rc = -1;
  }

  if (rc == 0 && EVP_DigestSignFinal(digest, (uint8_t *)sig, siglen) != 1) {
    unsigned long err = ERR_get_error();
    UM_LOG(WARN, "failed to sign digest %ld/%s", err, ERR_lib_error_string(err));
    rc = -1;
  }

  EVP_MD_CTX_free(digest);
  return rc;
}

static tlsuv_public_key_t privkey_pubkey(tlsuv_private_key_t pk) {
  struct priv_key_s *priv = (struct priv_key_s *)pk;
  struct pub_key_s *pub = tlsuv__calloc(1, sizeof(*pub));
  pub_key_init(pub);

  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PUBKEY(bio, priv->pkey);
  pub->pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  BIO_free_all(bio);

  return (tlsuv_public_key_t)pub;
}

static int privkey_to_pem(tlsuv_private_key_t pk, char **pem, size_t *pemlen) {
  BIO *b = BIO_new(BIO_s_mem());
  struct priv_key_s *privkey = (struct priv_key_s *)pk;

  *pem = NULL;
  *pemlen = 0;

  if (!PEM_write_bio_PKCS8PrivateKey(b, privkey->pkey, NULL, NULL, 0, NULL, NULL)) {
    unsigned long err = ERR_get_error();
    UM_LOG(WARN, "failed to generate PEM for private key: %ld/%s", err, ERR_lib_error_string(err));
  } else {
    size_t len = BIO_ctrl_pending(b);
    *pem = tlsuv__calloc(1, len + 1);
    BIO_read(b, *pem, (int)len);
    *pemlen = len;
  }

  BIO_free(b);
  return *pem != NULL ? 0 : -1;
}

static int pubkey_to_pem(tlsuv_public_key_t pub, char **pem, size_t *pemlen) {
  BIO *b = BIO_new(BIO_s_mem());
  struct pub_key_s *pubkey = (struct pub_key_s *)pub;

  *pem = NULL;
  *pemlen = 0;

  if (!PEM_write_bio_PUBKEY(b, pubkey->pkey)) {
    unsigned long err = ERR_get_error();
    UM_LOG(WARN, "failed to generate PEM for public key: %ld/%s", err, ERR_lib_error_string(err));
  } else {
    size_t len = BIO_ctrl_pending(b);
    *pem = tlsuv__calloc(1, len + 1);
    BIO_read(b, *pem, (int)len);
    *pemlen = len;
  }

  BIO_free(b);
  return *pem != NULL ? 0 : -1;
}

int load_key(tlsuv_private_key_t *key, const char *keydata, size_t keydatalen) {
  BIO *kb;
  int rc = 0;
  FILE *kf = fopen(keydata, "r");
  if (kf != NULL) {
    kb = BIO_new_fp(kf, 1);
  } else {
    kb = BIO_new_mem_buf(keydata, (int)keydatalen);
  }

  EVP_PKEY *pk = NULL;
  if (!PEM_read_bio_PrivateKey(kb, &pk, NULL, NULL)) {
    unsigned long err = ERR_get_error();
    UM_LOG(WARN, "failed to load key: %ld/%s", err, ERR_lib_error_string(err));
    rc = -1;
  } else {
    *key = new_private_key(pk);
  }

  BIO_free(kb);
  return rc;
}

int gen_key(tlsuv_private_key_t *key) {
  int rc = 0;
  EVP_PKEY *pk = EVP_PKEY_new();
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
  EVP_PKEY_keygen_init(pctx);
  EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);

  if (!EVP_PKEY_keygen(pctx, &pk)) {
    uint32_t err = ERR_get_error();
    UM_LOG(ERR, "failed to generate key: %d(%s)", err, tls_error(err));
    rc = -1;
    EVP_PKEY_free(pk);
  }

  if (rc == 0) {
    *key = new_private_key(pk);
  }

  EVP_PKEY_CTX_free(pctx);
  return rc;
}

static int privkey_get_cert(tlsuv_private_key_t pk, tlsuv_certificate_t *cert) {
  (void)pk;
  (void)cert;
  return -1;
}

static int privkey_store_cert(tlsuv_private_key_t pk, tlsuv_certificate_t cert) {
  (void)pk;
  (void)cert;
  return -1;
}

static const char *cert_to_text(const struct tlsuv_certificate_s *cert) {
  struct cert_s *c = (struct cert_s *)cert;

  if (c->text) return c->text;

  STACK_OF(X509_OBJECT) *s = X509_STORE_get0_objects(c->cert);
  X509 *x509 = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(s, 0));
  BIO *bio = BIO_new(BIO_s_mem());
  X509_print_ex(bio, x509, 0, X509_FLAG_NO_HEADER | X509_FLAG_NO_SIGDUMP | X509_FLAG_NO_SIGNAME);

  int len = BIO_pending(bio);
  c->text = tlsuv__malloc((size_t)len + 1);
  BIO_read(bio, c->text, len);
  c->text[len] = '\0';
  BIO_free(bio);

  return c->text;
}

static void cert_free(tlsuv_certificate_t cert) {
  struct cert_s *c = (struct cert_s *)cert;
  if (c->cert != NULL) {
    X509_STORE_free(c->cert);
  }
  tlsuv__free(c->text);
  tlsuv__free(c);
}

static int cert_to_pem(const struct tlsuv_certificate_s *cert, int full_chain, char **pem, size_t *pemlen) {
  X509_STORE *store = ((struct cert_s *)cert)->cert;
  BIO *pembio = BIO_new(BIO_s_mem());
  STACK_OF(X509_OBJECT) *s = X509_STORE_get0_objects(store);

  for (int i = 0; i < sk_X509_OBJECT_num(s); i++) {
    X509 *c = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(s, i));
    PEM_write_bio_X509(pembio, c);
    if (!full_chain) {
      break;
    }
  }

  *pemlen = BIO_ctrl_pending(pembio);
  *pem = tlsuv__calloc(1, *pemlen + 1);
  BIO_read(pembio, *pem, (int)*pemlen);
  BIO_free_all(pembio);
  return 0;
}

static int cert_verify(const struct tlsuv_certificate_s *cert, enum hash_algo md, const char *data, size_t datalen, const char *sig, size_t siglen) {
  X509_STORE *store = ((struct cert_s *)cert)->cert;
  STACK_OF(X509_OBJECT) *s = X509_STORE_get0_objects(store);
  X509 *c = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(s, 0));
  EVP_PKEY *pk = X509_get_pubkey(c);
  if (pk == NULL) {
    unsigned long err = ERR_peek_error();
    UM_LOG(WARN, "no pub key: %ld/%s", err, ERR_lib_error_string(err));
    return -1;
  }

  int rc = verify_signature(pk, md, data, datalen, sig, siglen);
  EVP_PKEY_free(pk);
  return rc;
}

static int cert_exp(const struct tlsuv_certificate_s *cert, struct tm *time) {
  if (time == NULL || cert == NULL) {
    return -1;
  }

  X509_STORE *store = ((struct cert_s *)cert)->cert;
  STACK_OF(X509_OBJECT) *s = X509_STORE_get0_objects(store);
  X509 *c = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(s, 0));
  const ASN1_TIME *notAfter = X509_get0_notAfter(c);
  ASN1_GENERALIZEDTIME *gt = ASN1_TIME_to_generalizedtime(notAfter, NULL);
  if (gt == NULL) {
    return -1;
  }

  const char *v = (const char *)ASN1_STRING_get0_data((const ASN1_STRING *)gt);
  size_t len = (size_t)ASN1_STRING_length((const ASN1_STRING *)gt);
  if (v == NULL || len < 14) {
    ASN1_GENERALIZEDTIME_free(gt);
    return -1;
  }

  memset(time, 0, sizeof(*time));
  time->tm_year = (v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0') - 1900;
  time->tm_mon = ((v[4] - '0') * 10 + (v[5] - '0')) - 1;
  time->tm_mday = (v[6] - '0') * 10 + (v[7] - '0');
  time->tm_hour = (v[8] - '0') * 10 + (v[9] - '0');
  time->tm_min = (v[10] - '0') * 10 + (v[11] - '0');
  time->tm_sec = (v[12] - '0') * 10 + (v[13] - '0');

  ASN1_GENERALIZEDTIME_free(gt);
  return 0;
}
