/* PS5SX2 Installer: TCP connections with time limits. */
#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Connected socket, or -1 (err_get() says why). io_timeout_ms applies to every later send/recv. */
int net_connect(const char *host, int port, int connect_timeout_ms, int io_timeout_ms);
/* -1 on error or time-out; 0 when the peer closed. */
ssize_t net_recv(int fd, void *buf, size_t len);
ssize_t net_send(int fd, const void *buf, size_t len);
void net_close(int fd);
/* 1 when the last net_recv/net_send failed because of the time limit. */
int net_timed_out(void);
