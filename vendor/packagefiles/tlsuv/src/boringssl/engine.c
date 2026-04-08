#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>

#include "../alloc.h"
#include "../keychain.h"
#include "../um_debug.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "keys.h"

#if _WIN32
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#endif

#if __APPLE__
#include <Security/Security.h>
#endif

struct boringssl_ctx {
  tls_context api;
  SSL_CTX *ctx;
  int (*cert_verify_f)(const struct tlsuv_certificate_s *cert, void *v_ctx);
  void *verify_ctx;
  unsigned char *alpn_protocols;
};

struct boringssl_engine {
  struct tlsuv_engine_s api;
  SSL *ssl;
  char *alpn;
  BIO *bio;
  bool handshake_started;
  io_ctx io;
  io_read read_f;
  io_write write_f;
  unsigned long error;
};

static int is_self_signed(X509 *cert);
static bool host_is_ip_literal(const char *host);
static const char *name_str(const X509_NAME *n);
static void init_ssl_context(struct boringssl_ctx *c, const char *cabuf, size_t cabuf_len);
static int tls_set_own_cert(tls_context *ctx, tlsuv_private_key_t key, tlsuv_certificate_t cert);
static int set_ca_bundle(tls_context *tls, const char *ca, size_t ca_len);
tlsuv_engine_t new_boringssl_engine(tls_context *ctx, const char *host);
static int parse_pkcs7_certs(tlsuv_certificate_t *chain, const char *pkcs7, size_t pkcs7len);
static int load_cert(tlsuv_certificate_t *cert, const char *buf, size_t buflen);
static int generate_csr(tlsuv_private_key_t key, char **pem, size_t *pemlen, ...);
static int tls_set_partial_vfy(tls_context *ctx, int allow);
static void tls_set_cert_verify(tls_context *ctx, int (*verify_f)(const struct tlsuv_certificate_s *cert, void *v_ctx), void *v_ctx);

static void set_io(tlsuv_engine_t self, io_ctx io, io_read rdf, io_write wrtf);
static void set_io_fd(tlsuv_engine_t self, tlsuv_sock_t fd);
static void set_protocols(tlsuv_engine_t self, const char **protocols, int len);
static tls_handshake_state tls_hs_state(tlsuv_engine_t engine);
static tls_handshake_state tls_continue_hs(tlsuv_engine_t self);
static const char *tls_get_alpn(tlsuv_engine_t self);
static int tls_write(tlsuv_engine_t self, const char *data, size_t data_len);
static int tls_read(tlsuv_engine_t self, char *out, size_t *out_bytes, size_t maxout);
static int tls_close(tlsuv_engine_t self);
static int tls_reset(tlsuv_engine_t self);
static void tls_free(tlsuv_engine_t self);
static void tls_free_ctx(tls_context *ctx);
static const char *tls_lib_version();
static const char *tls_eng_error(tlsuv_engine_t self);
static void msg_cb(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg);
static void info_cb(const SSL *s, int where, int ret);
static BIO_METHOD *BIO_s_engine(void);
#if _WIN32
static X509_STORE *load_system_certs();
#endif

static tls_context boringssl_context_api = {
  .version = tls_lib_version,
  .strerror = (const char *(*)(long))tls_error,
  .new_engine = new_boringssl_engine,
  .free_ctx = tls_free_ctx,
  .set_ca_bundle = set_ca_bundle,
  .set_own_cert = tls_set_own_cert,
  .allow_partial_chain = tls_set_partial_vfy,
  .set_cert_verify = tls_set_cert_verify,
  .parse_pkcs7_certs = parse_pkcs7_certs,
  .load_cert = load_cert,
  .generate_key = gen_key,
  .load_key = load_key,
  .generate_csr_to_pem = generate_csr,
};

static struct tlsuv_engine_s boringssl_engine_api = {
  .set_io = set_io,
  .set_io_fd = set_io_fd,
  .set_protocols = set_protocols,
  .handshake_state = tls_hs_state,
  .handshake = tls_continue_hs,
  .get_alpn = tls_get_alpn,
  .close = tls_close,
  .write = tls_write,
  .read = tls_read,
  .reset = tls_reset,
  .free = tls_free,
  .strerror = tls_eng_error,
};

int configure_boringssl() {
  return 0;
}

static const char *tls_lib_version() {
  return OpenSSL_version(OPENSSL_VERSION);
}

const char *tls_error(unsigned long code) {
  static char err_buf[32];
  if (code == 0) return "no error";

  const char *lib_err = ERR_lib_error_string(code);
  if (lib_err) return lib_err;

  snprintf(err_buf, sizeof(err_buf), "error[%lX]", code);
  return err_buf;
}

static const char *tls_eng_error(tlsuv_engine_t self) {
  struct boringssl_engine *e = (struct boringssl_engine *)self;
  const char *err = ERR_reason_error_string(e->error);
  return err;
}

tls_context *new_boringssl_ctx(const char *ca, size_t ca_len) {
  OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT, NULL);

  struct boringssl_ctx *c = tlsuv__calloc(1, sizeof(struct boringssl_ctx));
  c->api = boringssl_context_api;
  init_ssl_context(c, ca, ca_len);
  return &c->api;
}

static X509_STORE *load_certs(const char *buf, size_t buf_len) {
  X509_STORE *certs = X509_STORE_new();
  X509 *c;
  struct stat fstat;

  if (stat(buf, &fstat) == 0) {
    if (fstat.st_mode & S_IFREG) {
      if (!X509_STORE_load_locations(certs, buf, NULL)) {
        UM_LOG(ERR, "failed to load certs from [%s]", buf);
      }
    } else if (fstat.st_mode & S_IFDIR) {
      X509_LOOKUP *lu = X509_STORE_add_lookup(certs, X509_LOOKUP_hash_dir());
      if (lu == NULL || X509_LOOKUP_add_dir(lu, buf, X509_FILETYPE_PEM) != 1) {
        UM_LOG(ERR, "failed to load cert directory from [%s]", buf);
      }
    } else {
      UM_LOG(ERR, "cert bundle[%s] is not a regular file", buf);
    }
  } else {
    BIO *crt_bio = BIO_new_mem_buf(buf, (int)buf_len);
    while ((c = PEM_read_bio_X509(crt_bio, NULL, NULL, NULL)) != NULL) {
      int root = is_self_signed(c);
      UM_LOG(VERB, "%s root[%s]", name_str(X509_get_subject_name(c)), root ? "true" : "false");
      X509_STORE_add_cert(certs, c);
      X509_free(c);
    }
    BIO_free(crt_bio);
  }

  return certs;
}

static int load_cert(tlsuv_certificate_t *cert, const char *buf, size_t buflen) {
  X509_STORE *store = load_certs(buf, buflen);
  STACK_OF(X509_OBJECT) *certs = X509_STORE_get0_objects(store);
  int count = sk_X509_OBJECT_num(certs);
  if (count == 0) {
    X509_STORE_free(store);
    return -1;
  }

  struct cert_s *crt = tlsuv__calloc(1, sizeof(*crt));
  cert_init(crt);
  crt->cert = store;
  *cert = (tlsuv_certificate_t)crt;
  return 0;
}

static int is_self_signed(X509 *cert) {
  X509_NAME *subj = X509_get_subject_name(cert);
  X509_NAME *issuer = X509_get_issuer_name(cert);
  if (X509_NAME_cmp(subj, issuer) != 0) {
    return 0;
  }

  EVP_PKEY *pub = X509_get_pubkey(cert);
  if (pub == NULL) {
    return 0;
  }
  int ok = X509_verify(cert, pub);
  EVP_PKEY_free(pub);
  return ok == 1;
}

static const char *name_str(const X509_NAME *n) {
  static char buf[1024];
  BIO *b = BIO_new(BIO_s_mem());
  X509_NAME_print(b, n, 0);
  BIO_read(b, buf, sizeof(buf));
  BIO_free(b);
  return buf;
}

#if __APPLE__
static int apple_ca_verify(int pre_verify, X509_STORE_CTX *st) {
  (void)pre_verify;
  CFMutableArrayRef certs = CFArrayCreateMutable(kCFAllocatorDefault, 10, &kCFTypeArrayCallBacks);

  STACK_OF(X509) *chain = X509_STORE_CTX_get1_chain(st);
  for (int i = 0; i < sk_X509_num(chain); i++) {
    X509 *x = sk_X509_value(chain, i);
    uint8_t *der = NULL;
    int der_len = i2d_X509(x, &der);
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, der, der_len);
    OPENSSL_free(der);

    SecCertificateRef c = SecCertificateCreateWithData(kCFAllocatorDefault, d);
    CFArrayAppendValue(certs, c);
    CFRelease(d);
    CFRelease(c);
  }
  sk_X509_pop_free(chain, X509_free);

  SecTrustRef trust;
  SecPolicyRef policy = SecPolicyCreateBasicX509();
  CFErrorRef err = NULL;
  bool result = SecTrustCreateWithCertificates(certs, policy, &trust) == errSecSuccess &&
                  SecTrustEvaluateWithError(trust, &err);
  CFRelease(trust);
  CFRelease(policy);
  CFRelease(certs);
  return result;
}
#endif

static int set_ca_bundle(tls_context *tls, const char *ca, size_t ca_len) {
  struct boringssl_ctx *c = (struct boringssl_ctx *)tls;
  SSL_CTX *ctx = c->ctx;

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  if (ca != NULL) {
    X509_STORE *store = load_certs(ca, ca_len);
    SSL_CTX_set0_verify_cert_store(ctx, store);
  } else {
#if _WIN32
    X509_STORE *sys_ca = load_system_certs();
    if (sys_ca != NULL) {
      SSL_CTX_set0_verify_cert_store(ctx, sys_ca);
    }
#elif __APPLE__
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, apple_ca_verify);
#elif defined(ANDROID) || defined(__ANDROID__)
    X509_STORE *ca_store = SSL_CTX_get_cert_store(ctx);
    X509_LOOKUP *lu = X509_STORE_add_lookup(ca_store, X509_LOOKUP_hash_dir());
    if (lu != NULL) {
      X509_LOOKUP_add_dir(lu, "/etc/security/cacerts", X509_FILETYPE_PEM);
    }
#else
    SSL_CTX_set_default_verify_paths(ctx);
#endif
  }

  return 0;
}

static void init_ssl_context(struct boringssl_ctx *c, const char *cabuf, size_t cabuf_len) {
  const SSL_METHOD *method = TLS_client_method();
  SSL_CTX *ctx = SSL_CTX_new(method);
  if (ctx == NULL) {
    ERR_print_errors_fp(stderr);
    UM_LOG(ERR, "FATAL: failed to create SSL_CTX: %s", tls_error(ERR_get_error()));
    abort();
  }

  SSL_CTX_set_app_data(ctx, c);
  c->ctx = ctx;
  set_ca_bundle((tls_context *)c, cabuf, cabuf_len);
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

  char *tls_debug = getenv("TLS_DEBUG");
  if (tls_debug) {
    SSL_CTX_set_msg_callback(ctx, msg_cb);
    SSL_CTX_set_info_callback(ctx, info_cb);
  }
}

typedef struct string_int_pair_st {
  const char *name;
  int retval;
} STRINT_PAIR;

static const char *lookup(int val, const STRINT_PAIR *list, const char *def) {
  for (; list->name; ++list) {
    if (list->retval == val) return list->name;
  }
  return def;
}

static STRINT_PAIR handshakes[] = {
  {", HelloRequest", SSL3_MT_HELLO_REQUEST},
  {", ClientHello", SSL3_MT_CLIENT_HELLO},
  {", ServerHello", SSL3_MT_SERVER_HELLO},
  {", HelloVerifyRequest", DTLS1_MT_HELLO_VERIFY_REQUEST},
  {", NewSessionTicket", SSL3_MT_NEWSESSION_TICKET},
  {", EndOfEarlyData", SSL3_MT_END_OF_EARLY_DATA},
  {", EncryptedExtensions", SSL3_MT_ENCRYPTED_EXTENSIONS},
  {", Certificate", SSL3_MT_CERTIFICATE},
  {", ServerKeyExchange", SSL3_MT_SERVER_KEY_EXCHANGE},
  {", CertificateRequest", SSL3_MT_CERTIFICATE_REQUEST},
  {", ServerHelloDone", SSL3_MT_SERVER_DONE},
  {", CertificateVerify", SSL3_MT_CERTIFICATE_VERIFY},
  {", ClientKeyExchange", SSL3_MT_CLIENT_KEY_EXCHANGE},
  {", Finished", SSL3_MT_FINISHED},
  {", CertificateStatus", SSL3_MT_CERTIFICATE_STATUS},
  {", SupplementalData", SSL3_MT_SUPPLEMENTAL_DATA},
  {", KeyUpdate", SSL3_MT_KEY_UPDATE},
#ifndef OPENSSL_NO_NEXTPROTONEG
  {", NextProto", SSL3_MT_NEXT_PROTO},
#endif
  {", MessageHash", SSL3_MT_MESSAGE_HASH},
  {NULL}
};

static STRINT_PAIR alert_types[] = {
  {" close_notify", 0},
  {" end_of_early_data", 1},
  {" unexpected_message", 10},
  {" bad_record_mac", 20},
  {" decryption_failed", 21},
  {" record_overflow", 22},
  {" decompression_failure", 30},
  {" handshake_failure", 40},
  {" bad_certificate", 42},
  {" unsupported_certificate", 43},
  {" certificate_revoked", 44},
  {" certificate_expired", 45},
  {" certificate_unknown", 46},
  {" illegal_parameter", 47},
  {" unknown_ca", 48},
  {" access_denied", 49},
  {" decode_error", 50},
  {" decrypt_error", 51},
  {" export_restriction", 60},
  {" protocol_version", 70},
  {" insufficient_security", 71},
  {" internal_error", 80},
  {" inappropriate_fallback", 86},
  {" user_canceled", 90},
  {" no_renegotiation", 100},
  {" missing_extension", 109},
  {" unsupported_extension", 110},
  {" certificate_unobtainable", 111},
  {" unrecognized_name", 112},
  {" bad_certificate_status_response", 113},
  {" bad_certificate_hash_value", 114},
  {" unknown_psk_identity", 115},
  {" certificate_required", 116},
  {NULL}
};

static STRINT_PAIR ssl_versions[] = {
  {"SSL 3.0", SSL3_VERSION},
  {"TLS 1.0", TLS1_VERSION},
  {"TLS 1.1", TLS1_1_VERSION},
  {"TLS 1.2", TLS1_2_VERSION},
  {"TLS 1.3", TLS1_3_VERSION},
  {"DTLS 1.0", DTLS1_VERSION},
  {NULL}
};

static void msg_cb(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg) {
  (void)ssl;
  (void)arg;
  const char *str_write_p = write_p ? ">>>" : "<<<";
  const char *str_content_type = "", *str_details1 = "", *str_details2 = "";
  const char *str_version = lookup(version, ssl_versions, "???");
  const unsigned char *bp = buf;

  if (version == SSL3_VERSION || version == TLS1_VERSION || version == TLS1_1_VERSION ||
    version == TLS1_2_VERSION || version == TLS1_3_VERSION ||
    version == DTLS1_VERSION) {
    switch (content_type) {
      case 20: str_content_type = ", ChangeCipherSpec"; break;
      case 21:
        str_content_type = ", Alert";
        str_details1 = ", ???";
        if (len == 2) {
          switch (bp[0]) {
            case 1: str_details1 = ", warning"; break;
            case 2: str_details1 = ", fatal"; break;
          }
          str_details2 = lookup((int)bp[1], alert_types, " ???");
        }
        break;
      case 22:
        str_content_type = ", Handshake";
        str_details1 = "???";
        if (len > 0) str_details1 = lookup((int)bp[0], handshakes, "???");
        break;
      case 23: str_content_type = ", ApplicationData"; break;
    }
  } else if (version == 0 && content_type == SSL3_RT_HEADER) {
    str_version = "";
    str_content_type = "TLS Header";
  }

  UM_LOG(TRACE, "%s %s%s [length %04lx]%s%s", str_write_p, str_version, str_content_type, (unsigned long)len, str_details1, str_details2);
}

void info_cb(const SSL *s, int where, int ret) {
  const char *str;
  int w = where & ~SSL_ST_MASK;

  if (w & SSL_ST_CONNECT) str = "SSL_connect";
  else if (w & SSL_ST_ACCEPT) str = "SSL_accept";
  else str = "undefined";

  if (where & SSL_CB_LOOP) {
    UM_LOG(TRACE, "%s:%s", str, SSL_state_string_long(s));
  } else if (where & SSL_CB_ALERT) {
    str = (where & SSL_CB_READ) ? "read" : "write";
    UM_LOG(VERB, "SSL3 alert %s:%s:%s", str, SSL_alert_type_string_long(ret), SSL_alert_desc_string_long(ret));
  } else if (where & SSL_CB_EXIT) {
    if (ret == 0) UM_LOG(VERB, "%s:failed in %s", str, SSL_state_string_long(s));
    else if (ret < 0) UM_LOG(VERB, "%s:error in %s", str, SSL_state_string_long(s));
  }
}

tlsuv_engine_t new_boringssl_engine(tls_context *ctx, const char *host) {
  struct boringssl_ctx *context = (struct boringssl_ctx *)ctx;
  struct boringssl_engine *engine = tlsuv__calloc(1, sizeof(struct boringssl_engine));
  engine->api = boringssl_engine_api;
  engine->ssl = SSL_new(context->ctx);

  if (host && *host) {
    if (host_is_ip_literal(host)) {
      X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(engine->ssl), host);
    } else {
      SSL_set_tlsext_host_name(engine->ssl, host);
      SSL_set1_host(engine->ssl, host);
    }
  }
  SSL_set_connect_state(engine->ssl);
  SSL_set_app_data(engine->ssl, engine);

  return &engine->api;
}

static bool host_is_ip_literal(const char *host) {
  struct sockaddr_in addr4;
  struct sockaddr_in6 addr6;

  return host != NULL &&
    (uv_inet_pton(AF_INET, host, &addr4.sin_addr) == 0 ||
     uv_inet_pton(AF_INET6, host, &addr6.sin6_addr) == 0);
}

static void set_io(tlsuv_engine_t self, io_ctx io, io_read rdf, io_write wrtf) {
  struct boringssl_engine *e = (struct boringssl_engine *)self;
  assert(e->bio == NULL);

#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
  SSL_set_options(e->ssl, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
  e->bio = BIO_new(BIO_s_engine());
  BIO_set_data(e->bio, e);
  BIO_set_init(e->bio, true);
  SSL_set_bio(e->ssl, e->bio, e->bio);

  e->io = io;
  e->read_f = rdf;
  e->write_f = wrtf;
}

static void set_io_fd(tlsuv_engine_t self, tlsuv_sock_t fd) {
  struct boringssl_engine *e = (struct boringssl_engine *)self;
  assert(e->bio == NULL);
  e->bio = BIO_new_socket((int)fd, false);
  SSL_set_bio(e->ssl, e->bio, e->bio);
}

static void set_protocols(tlsuv_engine_t self, const char **protocols, int len) {
  struct boringssl_engine *e = (struct boringssl_engine *)self;
  size_t protolen = 0;
  for (int i = 0; i < len; i++) protolen += strlen(protocols[i]) + 1;

  unsigned char *alpn_protocols = tlsuv__malloc(protolen + 1);
  unsigned char *p = alpn_protocols;
  for (int i = 0; i < len; i++) {
    size_t plen = strlen(protocols[i]);
    *p++ = (unsigned char)plen;
    strncpy((char *)p, protocols[i], plen);
    p += plen;
  }
  *p = 0;
  SSL_set_alpn_protos(e->ssl, alpn_protocols, (unsigned int)strlen((char *)alpn_protocols));
  tlsuv__free(alpn_protocols);
}

static int cert_verify_cb(X509_STORE_CTX *certs, void *ctx) {
  struct boringssl_ctx *c = ctx;
  X509_STORE *store = X509_STORE_new();
  X509 *crt = X509_STORE_CTX_get0_cert(certs);
  X509_STORE_add_cert(store, crt);

  char n[1024];
  X509_NAME_oneline(X509_get_subject_name(crt), n, 1024);
  UM_LOG(VERB, "verifying %s", n);

  int rc = 1;
  struct cert_s cert;
  cert_init(&cert);
  cert.cert = store;
  if (c->cert_verify_f && c->cert_verify_f((const struct tlsuv_certificate_s *)&cert, c->verify_ctx) != 0) {
    UM_LOG(WARN, "verify failed for certificate[%s]", n);
    rc = 0;
  }
  X509_STORE_free(store);
  return rc;
}

int tls_set_partial_vfy(tls_context *ctx, int allow) {
  struct boringssl_ctx *c = (struct boringssl_ctx *)ctx;
  X509_VERIFY_PARAM *vfy = SSL_CTX_get0_param(c->ctx);
  if (allow) X509_VERIFY_PARAM_set_flags(vfy, X509_V_FLAG_PARTIAL_CHAIN);
  else X509_VERIFY_PARAM_clear_flags(vfy, X509_V_FLAG_PARTIAL_CHAIN);
  return 0;
}

static void tls_set_cert_verify(tls_context *ctx, int (*verify_f)(const struct tlsuv_certificate_s *cert, void *v_ctx), void *v_ctx) {
  struct boringssl_ctx *c = (struct boringssl_ctx *)ctx;
  c->cert_verify_f = verify_f;
  c->verify_ctx = v_ctx;
  SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_cert_verify_callback(c->ctx, cert_verify_cb, c);
}

static void tls_free_ctx(tls_context *ctx) {
  struct boringssl_ctx *c = (struct boringssl_ctx *)ctx;
  if (c->alpn_protocols) {
    tlsuv__free(c->alpn_protocols);
  }
  SSL_CTX_free(c->ctx);
  tlsuv__free(c);
}

static int tls_reset(tlsuv_engine_t self) {
  struct boringssl_engine *e = (struct boringssl_engine *)self;
  ERR_clear_error();
  e->bio = NULL;
  if (!SSL_clear(e->ssl)) {
    int err = SSL_get_error(e->ssl, 0);
    UM_LOG(ERR, "error resetting TLS engine: %d(%s)", err, tls_error(err));
    return -1;
  }
  return 0;
}

static void tls_free(tlsuv_engine_t self) {
  struct boringssl_engine *e = (struct boringssl_engine *)self;
  SSL_free(e->ssl);
  if (e->alpn) {
    tlsuv__free(e->alpn);
  }
  tlsuv__free(e);
}

#define SSL_OP_CHECK(op, desc) do { \
  if ((op) != 1) { \
    uint32_t err = ERR_get_error(); \
    UM_LOG(ERR, "failed to " desc ": %d(%s)", err, tls_error(err)); \
    return TLS_ERR; \
  } \
} while (0)

static X509 *tls_set_cert_internal(SSL_CTX *ssl, X509_STORE *store) {
  STACK_OF(X509_OBJECT) *certs = X509_STORE_get0_objects(store);
  X509 *crt = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(certs, 0));
  SSL_CTX_use_certificate(ssl, crt);

  for (int i = 1; i < sk_X509_OBJECT_num(certs); i++) {
    X509 *x509 = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(certs, i));
    X509_up_ref(x509);
    SSL_CTX_add_extra_chain_cert(ssl, x509);
  }
  return crt;
}

static int tls_set_own_cert(tls_context *ctx, tlsuv_private_key_t key, tlsuv_certificate_t cert) {
  struct boringssl_ctx *c = (struct boringssl_ctx *)ctx;
  SSL_CTX *ssl = c->ctx;

  SSL_CTX_use_PrivateKey(ssl, NULL);
  SSL_CTX_use_certificate(ssl, NULL);
  SSL_CTX_clear_chain_certs(ssl);

  if (key == NULL) {
    return 0;
  }

  struct cert_s *crt = (struct cert_s *)cert;
  X509_STORE *store = NULL;
  if (crt == NULL) {
    if (key->get_certificate) {
      if (key->get_certificate(key, (tlsuv_certificate_t *)&crt) != 0) {
        return -1;
      }
      store = crt->cert;
      free(crt);
    }
  } else {
    store = crt->cert;
    X509_STORE_up_ref(crt->cert);
  }

  if (store == NULL) {
    return -1;
  }

  struct priv_key_s *pk = (struct priv_key_s *)key;
  X509 *certs = tls_set_cert_internal(ssl, store);
  X509_STORE_free(store);

  SSL_OP_CHECK(X509_check_private_key(certs, pk->pkey), "verify key/cert combo");
  SSL_OP_CHECK(SSL_CTX_use_PrivateKey(ssl, pk->pkey), "set private key");
  return 0;
}

static tls_handshake_state tls_hs_state(tlsuv_engine_t engine) {
  struct boringssl_engine *eng = (struct boringssl_engine *)engine;
  if (SSL_is_init_finished(eng->ssl)) return TLS_HS_COMPLETE;
  return eng->handshake_started ? TLS_HS_CONTINUE : TLS_HS_BEFORE;
}

static int print_err_cb(const char *e, size_t len, void *v) {
  (void)v;
  UM_LOG(WARN, "%.*s", (int)len, e);
  return 1;
}

static tls_handshake_state tls_continue_hs(tlsuv_engine_t self) {
  struct boringssl_engine *eng = (struct boringssl_engine *)self;
  ERR_clear_error();
  eng->handshake_started = true;

  int rc = SSL_do_handshake(eng->ssl);
  if (rc != 1) {
    eng->error = ERR_peek_error();
    ERR_print_errors_cb(print_err_cb, NULL);
  }

  if (rc == 1) {
    eng->error = 0;
    return TLS_HS_COMPLETE;
  }

  int err = SSL_get_error(eng->ssl, rc);
  if (rc == 0) {
    UM_LOG(ERR, "boringssl: handshake was terminated: %s", tls_error(eng->error));
    return TLS_HS_ERROR;
  }

  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return TLS_HS_CONTINUE;
  }

  UM_LOG(ERR, "boringssl: handshake was terminated: %s", tls_error(eng->error));
  return TLS_HS_ERROR;
}

static const char *tls_get_alpn(tlsuv_engine_t self) {
  struct boringssl_engine *eng = (struct boringssl_engine *)self;
  const unsigned char *proto;
  unsigned int protolen;
  SSL_get0_alpn_selected(eng->ssl, &proto, &protolen);

  eng->alpn = tlsuv__calloc(1, protolen + 1);
  strncpy(eng->alpn, (const char *)proto, protolen);
  return eng->alpn;
}

static int tls_write(tlsuv_engine_t self, const char *data, size_t data_len) {
  struct boringssl_engine *eng = (struct boringssl_engine *)self;
  ERR_clear_error();

  if (data_len > INT_MAX) data_len = INT_MAX;

  size_t wrote = 0;
  while (data_len > wrote) {
    int remaining = (int)(data_len - wrote);
    int ret = SSL_write(eng->ssl, data + wrote, remaining);
    if (ret <= 0) {
      int err = SSL_get_error(eng->ssl, ret);
      if (err == SSL_ERROR_WANT_WRITE) {
        return wrote > 0 ? (int)wrote : TLS_AGAIN;
      }
      eng->error = ERR_peek_last_error();
      UM_LOG(ERR, "boringssl: write[%d] error: %s", err, tls_error(eng->error));
      return -1;
    }
    wrote += (size_t)ret;
  }

  return (int)wrote;
}

static int tls_read(tlsuv_engine_t self, char *out, size_t *out_bytes, size_t maxout) {
  struct boringssl_engine *eng = (struct boringssl_engine *)self;
  int err = SSL_ERROR_NONE;
  uint8_t *writep = (uint8_t *)out;
  size_t total_out = 0;

  ERR_clear_error();
  while (maxout - total_out > 0) {
    int want = (int)(maxout - total_out);
    int rc = SSL_read(eng->ssl, writep, want);
    if (rc <= 0) {
      err = SSL_get_error(eng->ssl, rc);
      break;
    }
    total_out += (size_t)rc;
    writep += rc;
  }

  if (total_out > 0) {
    *out_bytes = total_out;
    return SSL_pending(eng->ssl) ? TLS_MORE_AVAILABLE : TLS_OK;
  }

  *out_bytes = 0;
  if (SSL_get_shutdown(eng->ssl)) return TLS_EOF;

  unsigned long err_code = ERR_peek_last_error();
  switch (err) {
    case SSL_ERROR_NONE:
      UM_LOG(WARN, "boringssl read: SSL_ERROR_NONE with 0 bytes read");
      return TLS_OK;
    case SSL_ERROR_ZERO_RETURN:
      return TLS_EOF;
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_READ:
      return TLS_AGAIN;
    case SSL_ERROR_SYSCALL:
    case SSL_ERROR_SSL:
      eng->error = err_code;
      UM_LOG(WARN, "boringssl read[%d]: %lX/%s", err, err_code, tls_error(err_code));
      return TLS_ERR;
    default:
      UM_LOG(WARN, "boringssl read: unexpected err[%d] code[%lX]", err, err_code);
      break;
  }
  return TLS_OK;
}

static int tls_close(tlsuv_engine_t self) {
  struct boringssl_engine *eng = (struct boringssl_engine *)self;
  ERR_clear_error();
  int rc = SSL_shutdown(eng->ssl);
  if (rc < 0) {
    int err = SSL_get_error(eng->ssl, rc);
    unsigned long err_code = ERR_peek_last_error();
    UM_LOG(WARN, "boringssl shutdown[%d]: %lX/%s", err, err_code, tls_error(err_code));
  }
  return 0;
}

static int parse_pkcs7_certs(tlsuv_certificate_t *chain, const char *pkcs7buf, size_t pkcs7len) {
  BIO *buf = BIO_new_mem_buf(pkcs7buf, (int)pkcs7len);
  PKCS7 *pkcs7 = PEM_read_bio_PKCS7(buf, NULL, NULL, NULL);
  if (pkcs7 == NULL) {
    BIO_free(buf);
    buf = BIO_new_mem_buf(pkcs7buf, (int)pkcs7len);
    pkcs7 = d2i_PKCS7_bio(buf, NULL);
  }

  if (pkcs7 == NULL) {
    BIO_free(buf);
    return -1;
  }

  STACK_OF(X509) *certs;
  if (PKCS7_type_is_signed(pkcs7)) certs = pkcs7->d.sign->cert;
  else if (PKCS7_type_is_signedAndEnveloped(pkcs7)) certs = pkcs7->d.signed_and_enveloped->cert;
  else {
    BIO_free(buf);
    PKCS7_free(pkcs7);
    return -1;
  }

  X509_STORE *store = X509_STORE_new();
  for (int i = 0; i < sk_X509_num(certs); i++) {
    X509 *c = sk_X509_value(certs, i);
    X509_STORE_add_cert(store, c);
  }

  struct cert_s *c = tlsuv__calloc(1, sizeof(*c));
  cert_init(c);
  c->cert = store;
  *chain = (tlsuv_certificate_t)c;

  PKCS7_free(pkcs7);
  BIO_free(buf);
  return 0;
}

static int generate_csr(tlsuv_private_key_t key, char **pem, size_t *pemlen, ...) {
  struct priv_key_s *privkey = (struct priv_key_s *)key;
  int ret = 0;
  const char *op = NULL;
  EVP_PKEY *pk = privkey->pkey;
  X509_REQ *req = X509_REQ_new();
  X509_NAME *subj = X509_REQ_get_subject_name(req);
  BIO *b = BIO_new(BIO_s_mem());

  va_list va;
  va_start(va, pemlen);
  while (true) {
    char *id = va_arg(va, char *);
    if (id == NULL) break;
    const uint8_t *val = va_arg(va, uint8_t *);
    if (val == NULL) break;
    X509_NAME_add_entry_by_txt(subj, id, MBSTRING_ASC, val, -1, -1, 0);
  }
  va_end(va);

#define ssl_check(OP) do { \
  op = #OP; \
  if ((OP) == 0) { \
    ret = ERR_get_error(); \
    goto on_error; \
  } \
} while (0)

  ssl_check(X509_REQ_set_pubkey(req, pk));
  ssl_check(X509_REQ_sign(req, pk, EVP_sha256()));
  ssl_check(PEM_write_bio_X509_REQ(b, req));

on_error:
  if (ret) {
    UM_LOG(WARN, "%s => %s", op, tls_error(ret));
  } else {
    size_t len = BIO_ctrl_pending(b);
    *pem = tlsuv__calloc(1, len + 1);
    BIO_read(b, *pem, (int)len);
    if (pemlen) *pemlen = len;
  }

  BIO_free(b);
  X509_REQ_free(req);
  return ret;
}

#if _WIN32
#include <wincrypt.h>
#pragma comment (lib, "crypt32.lib")

static X509_STORE *load_system_certs() {
  X509_STORE *store = X509_STORE_new();
  X509 *c;
  HCERTSTORE hCertStore;
  PCCERT_CONTEXT pCertContext = NULL;

  if (!(hCertStore = CertOpenSystemStore(0, "ROOT"))) {
    UM_LOG(ERR, "The first system store did not open.");
    return store;
  }

  while ((pCertContext = CertEnumCertificatesInStore(hCertStore, pCertContext)) != NULL) {
    c = d2i_X509(NULL, (const uint8_t **)&pCertContext->pbCertEncoded, (long)pCertContext->cbCertEncoded);
    X509_STORE_add_cert(store, c);
  }
  CertFreeCertificateContext(pCertContext);
  CertCloseStore(hCertStore, 0);

  return store;
}
#endif

static int engine_bio_write(BIO *b, const char *data, int len) {
  struct boringssl_engine *e = BIO_get_data(b);
  assert(e);
  assert(e->write_f);

  ssize_t r = e->write_f(e->io, data, (size_t)len);
  if (r > 0) {
    return (int)r;
  }
  if (r == 0) {
    BIO_set_retry_write(b);
  }
  return -1;
}

static int engine_bio_read(BIO *b, char *data, int len) {
  struct boringssl_engine *e = BIO_get_data(b);
  assert(e);
  assert(e->read_f);

  ssize_t r = e->read_f(e->io, data, (size_t)len);
  if (r > 0) {
    return (int)r;
  }
  if (r == 0) {
    BIO_set_retry_read(b);
  }
  return -1;
}

static int engine_bio_puts(BIO *b, const char *str) {
  return engine_bio_write(b, str, (int)strlen(str));
}

static long engine_bio_ctrl(BIO *b, int cmd, long num, void *ptr) {
  (void)b;
  (void)num;
  (void)ptr;
  switch (cmd) {
    case BIO_CTRL_FLUSH:
    case BIO_CTRL_DGRAM_QUERY_MTU:
      return 1;
    default:
      return 0;
  }
}

static int engine_bio_create(BIO *b) {
  BIO_set_init(b, 0);
  BIO_set_data(b, NULL);
  return 1;
}

static int engine_bio_destroy(BIO *b) {
  if (b == NULL) return 0;
  BIO_set_data(b, NULL);
  BIO_set_init(b, 0);
  return 1;
}

static BIO_METHOD *BIO_s_engine(void) {
  static BIO_METHOD *engine = NULL;
  if (engine == NULL) {
    engine = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "tlsuv-engine");
    BIO_meth_set_write(engine, engine_bio_write);
    BIO_meth_set_read(engine, engine_bio_read);
    BIO_meth_set_puts(engine, engine_bio_puts);
    BIO_meth_set_ctrl(engine, engine_bio_ctrl);
    BIO_meth_set_create(engine, engine_bio_create);
    BIO_meth_set_destroy(engine, engine_bio_destroy);
  }
  return engine;
}
