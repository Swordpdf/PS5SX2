/* PS5SX2 Installer: TCP connections with time limits (getaddrinfo from the payload SDK's libc). */
#include "net.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int g_timed_out;

int net_timed_out(void) { return g_timed_out; }

static int connect_one(const struct addrinfo *ai, int connect_timeout_ms, int io_timeout_ms) {
  const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) {
    err_set("socket: %s", strerror(errno));
    return -1;
  }
#ifdef SO_NOSIGPIPE
  {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    err_set("fcntl: %s", strerror(errno));
    close(fd);
    return -1;
  }
  int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
  if (rc != 0 && errno != EINPROGRESS) {
    err_set("connect: %s", strerror(errno));
    close(fd);
    return -1;
  }
  if (rc != 0) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    do
      rc = poll(&pfd, 1, connect_timeout_ms);
    while (rc < 0 && errno == EINTR);
    if (rc == 0) {
      err_set("connection timed out");
      close(fd);
      return -1;
    }
    if (rc < 0) {
      err_set("poll: %s", strerror(errno));
      close(fd);
      return -1;
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
      err_set("connect: %s", strerror(soerr ? soerr : errno));
      close(fd);
      return -1;
    }
  }
  if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
    err_set("fcntl: %s", strerror(errno));
    close(fd);
    return -1;
  }
  struct timeval tv;
  tv.tv_sec = io_timeout_ms / 1000;
  tv.tv_usec = (io_timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

int net_connect(const char *host, int port, int connect_timeout_ms, int io_timeout_ms) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; /* the console's resolver path is IPv4 */
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);
  const int gai = getaddrinfo(host, portstr, &hints, &res);
  if (gai != 0 || !res) {
    err_set("can't find %s (DNS error %d): is the PS5 online?", host, gai);
    return -1;
  }
  int fd = -1;
  char last[256] = "";
  for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = connect_one(ai, connect_timeout_ms, io_timeout_ms);
    if (fd >= 0)
      break;
    str_copy(last, sizeof(last), err_get());
  }
  freeaddrinfo(res);
  if (fd < 0)
    err_set("can't connect to %s: %s", host, last[0] ? last : "no address");
  return fd;
}

ssize_t net_recv(int fd, void *buf, size_t len) {
  g_timed_out = 0;
  for (;;) {
    const ssize_t r = recv(fd, buf, len, 0);
    if (r >= 0)
      return r;
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      g_timed_out = 1;
    return -1;
  }
}

ssize_t net_send(int fd, const void *buf, size_t len) {
  g_timed_out = 0;
  for (;;) {
    const ssize_t w = send(fd, buf, len, MSG_NOSIGNAL);
    if (w >= 0)
      return w;
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      g_timed_out = 1;
    return -1;
  }
}

void net_close(int fd) {
  if (fd >= 0)
    close(fd);
}
