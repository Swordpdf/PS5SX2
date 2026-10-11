/* PS5SX2 Installer: TLS client connections with Mbed TLS.
 *
 * Certificates are always checked (MBEDTLS_SSL_VERIFY_REQUIRED) against the Mozilla CA list compiled in
 * (ca/cacert.pem -> cacert_data.c), with the server name. TLS 1.2 or 1.3. */
#include "tls.h"

#include "log.h"
#include "net.h"
#include "plat.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h" /* error codes only; the module itself isn't built */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

extern const char g_cacert_pem[];
extern const size_t g_cacert_pem_size; /* including the final NUL */

struct tls_conn {
  int fd;
  mbedtls_ssl_context ssl;
};

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_ca;
static mbedtls_ssl_config g_conf;
static int g_ready;

/* The only entropy source (MBEDTLS_ENTROPY_HARDWARE_ALT + MBEDTLS_NO_PLATFORM_ENTROPY). */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
  (void)data;
  if (plat_random(output, len) != 0) {
    *olen = 0;
    return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  }
  *olen = len;
  return 0;
}

static void mbed_err(const char *what, int rc) {
  char buf[160];
  mbedtls_strerror(rc, buf, sizeof(buf));
  err_set("%s: %s (-0x%04x)", what, buf, (unsigned)-rc);
}

int tls_global_init(void) {
  if (g_ready)
    return 0;
  const psa_status_t ps = psa_crypto_init();
  if (ps != PSA_SUCCESS) {
    err_set("TLS setup failed (psa_crypto_init %d): no random numbers from the system?", (int)ps);
    return -1;
  }
  mbedtls_entropy_init(&g_entropy);
  mbedtls_ctr_drbg_init(&g_drbg);
  mbedtls_x509_crt_init(&g_ca);
  mbedtls_ssl_config_init(&g_conf);

  static const char pers[] = "PS5SX2-Installer";
  int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char *)pers,
                                 sizeof(pers) - 1);
  if (rc != 0) {
    mbed_err("TLS setup failed (random numbers)", rc);
    return -1;
  }
  rc = mbedtls_x509_crt_parse(&g_ca, (const unsigned char *)g_cacert_pem, g_cacert_pem_size);
  if (rc < 0) {
    mbed_err("TLS setup failed (CA list)", rc);
    return -1;
  }
  if (rc > 0)
    log_line("tls: %d certificates of the CA list could not be read (the others are used)", rc);
  const char *extra = plat_test_env("PS5SX2_TEST_CA_FILE"); /* host tests only */
  if (extra) {
    sbuf pem;
    sb_init(&pem);
    FILE *f = fopen(extra, "rb");
    if (f) {
      char tmp[4096];
      size_t n;
      while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
        sb_append(&pem, tmp, n);
      fclose(f);
      rc = mbedtls_x509_crt_parse(&g_ca, (const unsigned char *)pem.data, pem.len + 1);
      log_line("tls: test CA file %s: rc=%d", extra, rc);
    }
    sb_free(&pem);
  }
  rc = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) {
    mbed_err("TLS setup failed (config)", rc);
    return -1;
  }
  mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&g_conf, &g_ca, NULL);
  mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
  mbedtls_ssl_conf_min_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_2);
  g_ready = 1;
  return 0;
}

void tls_global_free(void) {
  if (!g_ready)
    return;
  mbedtls_ssl_config_free(&g_conf);
  mbedtls_x509_crt_free(&g_ca);
  mbedtls_ctr_drbg_free(&g_drbg);
  mbedtls_entropy_free(&g_entropy);
  mbedtls_psa_crypto_free();
  g_ready = 0;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  tls_conn *c = ctx;
  const ssize_t w = net_send(c->fd, buf, len);
  if (w < 0)
    return net_timed_out() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED;
  return (int)w;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
  tls_conn *c = ctx;
  const ssize_t r = net_recv(c->fd, buf, len);
  if (r < 0)
    return net_timed_out() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
  return (int)r; /* 0 = closed */
}

tls_conn *tls_open(const char *host, int port) {
  if (!g_ready && tls_global_init() != 0)
    return NULL;
  tls_conn *c = calloc(1, sizeof(*c));
  if (!c) {
    err_set("out of memory");
    return NULL;
  }
  mbedtls_ssl_init(&c->ssl);
  c->fd = net_connect(host, port, 15000, 30000);
  if (c->fd < 0) {
    mbedtls_ssl_free(&c->ssl);
    free(c);
    return NULL;
  }
  int rc = mbedtls_ssl_setup(&c->ssl, &g_conf);
  if (rc == 0)
    rc = mbedtls_ssl_set_hostname(&c->ssl, host);
  if (rc != 0) {
    mbed_err("TLS setup failed", rc);
    tls_close(c);
    return NULL;
  }
  mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
  while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
      continue;
    const uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0 && flags != (uint32_t)-1) {
      char info[512];
      mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
      for (char *p = info; *p; p++)
        if (*p == '\n')
          *p = ' ';
      const int clock = (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) != 0;
      err_set("%s's certificate was not accepted: %s%s", host, str_trim(info),
              clock ? " - is the PS5's date and time right?" : "");
    } else {
      char what[160];
      snprintf(what, sizeof(what), "secure connection to %s failed", host);
      mbed_err(what, rc);
    }
    tls_close(c);
    return NULL;
  }
  return c;
}

int tls_write_all(tls_conn *c, const void *buf, size_t len) {
  const unsigned char *p = buf;
  while (len) {
    const int rc = mbedtls_ssl_write(&c->ssl, p, len);
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
      continue;
    if (rc < 0) {
      mbed_err("sending failed", rc);
      return -1;
    }
    p += rc;
    len -= (size_t)rc;
  }
  return 0;
}

ssize_t tls_read(tls_conn *c, void *buf, size_t len) {
  for (;;) {
    const int rc = mbedtls_ssl_read(&c->ssl, buf, len);
    if (rc > 0)
      return rc;
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF)
      return 0;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
        rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
      continue;
    if (rc == MBEDTLS_ERR_SSL_TIMEOUT)
      err_set("the server stopped answering (time-out)");
    else
      mbed_err("receiving failed", rc);
    return -1;
  }
}

void tls_close(tls_conn *c) {
  if (!c)
    return;
  if (c->fd >= 0) {
    (void)mbedtls_ssl_close_notify(&c->ssl);
    net_close(c->fd);
  }
  mbedtls_ssl_free(&c->ssl);
  free(c);
}
