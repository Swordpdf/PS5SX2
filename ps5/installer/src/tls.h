/* PS5SX2 Installer: TLS client connections (Mbed TLS), certificates checked against the built-in CA list. */
#pragma once

#include <stddef.h>
#include <sys/types.h>

typedef struct tls_conn tls_conn;

int tls_global_init(void); /* once; 0 on success */
void tls_global_free(void);

tls_conn *tls_open(const char *host, int port);
/* All bytes or -1. */
int tls_write_all(tls_conn *c, const void *buf, size_t len);
/* >0 bytes, 0 = the server closed, -1 = error. */
ssize_t tls_read(tls_conn *c, void *buf, size_t len);
void tls_close(tls_conn *c);
