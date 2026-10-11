/* PS5SX2 Installer: file helpers. */
#include "fsx.h"

#include "paths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "mbedtls/sha256.h"

int fs_exists(const char *p) {
  struct stat st;
  return lstat(p, &st) == 0;
}

int fs_is_dir(const char *p) {
  /* follows links: on the PS5 /data may itself be a link (to /user/data) */
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int fs_is_file(const char *p) {
  struct stat st;
  return lstat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int64_t fs_size(const char *p) {
  struct stat st;
  if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode))
    return -1;
  return (int64_t)st.st_size;
}

time_t fs_mtime(const char *p) {
  struct stat st;
  if (lstat(p, &st) != 0)
    return 0;
  return st.st_mtime;
}

int fs_mkdirs(const char *p, mode_t mode) {
  char tmp[PATH_LEN];
  if (str_copy(tmp, sizeof(tmp), p) >= sizeof(tmp)) {
    err_set("path too long: %s", p);
    return -1;
  }
  size_t n = strlen(tmp);
  while (n > 1 && tmp[n - 1] == '/')
    tmp[--n] = '\0';
  for (char *s = tmp + 1; *s; s++) {
    if (*s != '/')
      continue;
    *s = '\0';
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
      err_set("can't create folder %s: %s", tmp, strerror(errno));
      return -1;
    }
    *s = '/';
  }
  if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
    err_set("can't create folder %s: %s", tmp, strerror(errno));
    return -1;
  }
  if (!fs_is_dir(tmp)) {
    err_set("%s is there but isn't a folder", tmp);
    return -1;
  }
  return 0;
}

int fs_mkdirs_parent(const char *file, mode_t mode) {
  char dir[PATH_LEN];
  if (str_copy(dir, sizeof(dir), file) >= sizeof(dir)) {
    err_set("path too long: %s", file);
    return -1;
  }
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir)
    return 0;
  *slash = '\0';
  return fs_mkdirs(dir, mode);
}

static int read_fd(int fd, void *buf, size_t n, size_t *got) {
  size_t off = 0;
  while (off < n) {
    const ssize_t r = read(fd, (char *)buf + off, n - off);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      break;
    off += (size_t)r;
  }
  *got = off;
  return 0;
}

int fs_read_file(const char *p, size_t max, sbuf *out) {
  sb_clear(out);
  const int fd = open(p, O_RDONLY);
  if (fd < 0) {
    err_set("can't open %s: %s", p, strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    err_set("%s isn't a regular file", p);
    return -1;
  }
  if ((uint64_t)st.st_size > max) {
    close(fd);
    err_set("%s is too big (%lld bytes)", p, (long long)st.st_size);
    return -1;
  }
  if (sb_reserve(out, (size_t)st.st_size) != 0) {
    close(fd);
    return -1;
  }
  size_t got = 0;
  if (read_fd(fd, out->data, (size_t)st.st_size, &got) != 0) {
    err_set("can't read %s: %s", p, strerror(errno));
    close(fd);
    return -1;
  }
  close(fd);
  out->len = got;
  out->data[got] = '\0';
  return 0;
}

int fs_read_capped(const char *p, size_t cap, size_t head, sbuf *out, int64_t *size, time_t *mtime) {
  sb_clear(out);
  const int fd = open(p, O_RDONLY);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return -1;
  }
  if (size)
    *size = (int64_t)st.st_size;
  if (mtime)
    *mtime = st.st_mtime;
  const uint64_t total = (uint64_t)st.st_size;
  int rc = 0;
  if (total <= cap) {
    if (sb_reserve(out, (size_t)total) != 0) {
      close(fd);
      return -1;
    }
    size_t got = 0;
    rc = read_fd(fd, out->data, (size_t)total, &got);
    out->len = got;
    out->data[got] = '\0';
  } else {
    if (head > cap / 2)
      head = cap / 2;
    const size_t tail = cap - head;
    if (sb_reserve(out, cap + 128) != 0) {
      close(fd);
      return -1;
    }
    size_t got = 0;
    rc = read_fd(fd, out->data, head, &got);
    out->len = got;
    char mark[128];
    const int m = snprintf(mark, sizeof(mark), "\n[... %llu bytes left out ...]\n",
                           (unsigned long long)(total - head - tail));
    memcpy(out->data + out->len, mark, (size_t)m);
    out->len += (size_t)m;
    if (rc == 0 && lseek(fd, (off_t)(total - tail), SEEK_SET) >= 0) {
      rc = read_fd(fd, out->data + out->len, tail, &got);
      out->len += got;
    }
    out->data[out->len] = '\0';
  }
  close(fd);
  return rc;
}

int fs_write_all(int fd, const void *d, size_t n) {
  const char *p = d;
  while (n) {
    const ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

/* Only inside the work folder: "<work>/" prefix, no "." or ".." components, and no link on the way (the work
 * folder itself and every folder between it and the last component must be real folders), so a link planted in
 * the work folder can't send a delete or a write anywhere else. */
static int inside_work(const char *p) {
  const size_t w = strlen(g_p.work);
  if (!g_p.work[0] || strncmp(p, g_p.work, w) != 0 || p[w] != '/' || p[w + 1] == '\0')
    return 0;
  const char *s = p + w;
  while (*s) {
    while (*s == '/')
      s++;
    const char *e = strchr(s, '/');
    const size_t n = e ? (size_t)(e - s) : strlen(s);
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
      return 0;
    s += n;
  }
  struct stat st;
  if (lstat(g_p.work, &st) != 0 || !S_ISDIR(st.st_mode))
    return 0;
  char tmp[PATH_LEN];
  if (str_copy(tmp, sizeof(tmp), p) >= sizeof(tmp))
    return 0;
  for (char *c = tmp + w + 1; *c; c++) {
    if (*c != '/')
      continue;
    *c = '\0';
    const int ok = lstat(tmp, &st) != 0 ? errno == ENOENT : S_ISDIR(st.st_mode);
    *c = '/';
    if (!ok)
      return 0;
  }
  return 1;
}

int fs_is_work_path(const char *p) { return inside_work(p); }

int fs_dir_sync(const char *path_in_dir) {
  char dir[PATH_LEN];
  if (str_copy(dir, sizeof(dir), path_in_dir) >= sizeof(dir))
    return -1;
  char *slash = strrchr(dir, '/');
  if (!slash)
    return -1;
  if (slash == dir)
    slash[1] = '\0';
  else
    *slash = '\0';
  const int fd = open(dir, O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return -1;
  const int rc = fsync(fd);
  close(fd);
  return rc;
}

/* A new file of ours in the work folder: never through a link, never over something else's inode. */
static int create_work_file(const char *p, mode_t mode) {
  if (!inside_work(p)) {
    err_set("refused to write %s (not in the installer's folder)", p);
    return -1;
  }
  if (unlink(p) != 0 && errno != ENOENT) {
    err_set("can't replace %s: %s", p, strerror(errno));
    return -1;
  }
  const int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
  if (fd < 0)
    err_set("can't create %s: %s", p, strerror(errno));
  return fd;
}

int fs_create_work_file(const char *p, mode_t mode) { return create_work_file(p, mode); }

int fs_write_atomic(const char *p, const void *d, size_t n, mode_t mode) {
  char tmp[PATH_LEN + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", p);
  const int fd = create_work_file(tmp, mode);
  if (fd < 0)
    return -1;
  if (fs_write_all(fd, d, n) != 0 || fsync(fd) != 0) {
    err_set("can't write %s: %s", tmp, strerror(errno));
    close(fd);
    unlink(tmp);
    return -1;
  }
  close(fd);
  if (rename(tmp, p) != 0) {
    err_set("can't replace %s: %s", p, strerror(errno));
    unlink(tmp);
    return -1;
  }
  fs_dir_sync(p);
  return 0;
}

int fs_append_sync(const char *p, const char *line) {
  if (!inside_work(p)) {
    err_set("refused to write %s (not in the installer's folder)", p);
    return -1;
  }
  const int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0666);
  if (fd < 0) {
    err_set("can't write %s: %s", p, strerror(errno));
    return -1;
  }
  const int rc = (fs_write_all(fd, line, strlen(line)) == 0 && fsync(fd) == 0) ? 0 : -1;
  if (rc != 0)
    err_set("can't write %s: %s", p, strerror(errno));
  close(fd);
  return rc;
}

int fs_sha256(const char *p, char hex[65]) {
  const int fd = open(p, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    err_set("can't open %s: %s", p, strerror(errno));
    return -1;
  }
  static unsigned char buf[1 << 16];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  int rc = 0;
  for (;;) {
    const ssize_t r = read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR)
        continue;
      err_set("can't read %s: %s", p, strerror(errno));
      rc = -1;
      break;
    }
    if (r == 0)
      break;
    mbedtls_sha256_update(&ctx, buf, (size_t)r);
  }
  close(fd);
  if (rc == 0) {
    unsigned char digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    hex_encode(hex, digest, 32);
  }
  mbedtls_sha256_free(&ctx);
  return rc;
}

/* Across file systems: the copy goes to <dst>.part (created new, never through a link), is fsync'ed, and only
 * then renamed to dst, so dst is either missing or complete. The .part is removed on any failure. */
static int copy_file(const char *src, const char *dst) {
  if (fs_exists(dst)) {
    errno = EEXIST;
    return -1;
  }
  const int in = open(src, O_RDONLY | O_NOFOLLOW);
  if (in < 0)
    return -1;
  struct stat st;
  if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(in);
    errno = EINVAL;
    return -1;
  }
  char part[PATH_LEN + 8];
  snprintf(part, sizeof(part), "%s.part", dst);
  const int out = open(part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, st.st_mode & 0777);
  if (out < 0) {
    close(in);
    return -1;
  }
  static char buf[1 << 16];
  int rc = 0;
  for (;;) {
    const ssize_t r = read(in, buf, sizeof(buf));
    if (r < 0 && errno == EINTR)
      continue;
    if (r < 0) {
      rc = -1;
      break;
    }
    if (r == 0)
      break;
    if (fs_write_all(out, buf, (size_t)r) != 0) {
      rc = -1;
      break;
    }
  }
  if (rc == 0 && fsync(out) != 0)
    rc = -1;
  const int e = errno;
  close(in);
  close(out);
  if (rc == 0 && rename(part, dst) != 0)
    rc = -1;
  if (rc != 0) {
    const int e2 = errno ? errno : e;
    unlink(part); /* the partial copy we just made */
    errno = e2;
    return -1;
  }
  fs_dir_sync(dst);
  return 0;
}

int fs_move(const char *src, const char *dst) {
  if (rename(src, dst) == 0) {
    fs_dir_sync(dst);
    fs_dir_sync(src);
    return 0;
  }
  if (errno != EXDEV) {
    err_set("can't move %s to %s: %s", src, dst, strerror(errno));
    return -1;
  }
  if (copy_file(src, dst) != 0) {
    err_set("can't copy %s to %s: %s", src, dst, strerror(errno));
    return -1;
  }
  if (unlink(src) != 0) {
    err_set("copied %s to %s but can't remove the original: %s", src, dst, strerror(errno));
    return -1;
  }
  fs_dir_sync(src);
  return 0;
}

int fs_remove_work_file(const char *p) {
  if (!inside_work(p)) {
    err_set("refused to delete %s (not in the installer's folder)", p);
    return -1;
  }
  if (unlink(p) != 0 && errno != ENOENT) {
    err_set("can't delete %s: %s", p, strerror(errno));
    return -1;
  }
  return 0;
}

static int remove_tree(const char *p, int depth) {
  struct stat st;
  if (lstat(p, &st) != 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode))
    return unlink(p); /* a file, or a link itself (never what it points to) */
  if (depth > 32)
    return -1;
  DIR *d = opendir(p);
  if (!d)
    return -1;
  int rc = 0;
  struct dirent *e;
  char child[PATH_LEN];
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if (path_join(child, sizeof(child), p, e->d_name) != 0 || remove_tree(child, depth + 1) != 0)
      rc = -1;
  }
  closedir(d);
  if (rmdir(p) != 0)
    rc = -1;
  return rc;
}

int fs_remove_work_tree(const char *p) {
  if (!inside_work(p)) {
    err_set("refused to delete %s (not in the installer's folder)", p);
    return -1;
  }
  if (remove_tree(p, 0) != 0) {
    err_set("can't delete all of %s", p);
    return -1;
  }
  return 0;
}

int fs_list(const char *dir, fs_list_cb cb, void *ctx) {
  DIR *d = opendir(dir);
  if (!d) {
    err_set("can't list %s: %s", dir, strerror(errno));
    return -1;
  }
  struct dirent *e;
  char child[PATH_LEN];
  int rc = 0;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    int is_dir = 0;
    if (path_join(child, sizeof(child), dir, e->d_name) == 0)
      is_dir = fs_is_dir(child);
    if (cb(ctx, e->d_name, is_dir) != 0) {
      rc = 1;
      break;
    }
  }
  closedir(d);
  return rc;
}

uint64_t fs_free_bytes(const char *p) {
  struct statvfs sv;
  if (statvfs(p, &sv) != 0)
    return 0;
  const uint64_t unit = sv.f_frsize ? (uint64_t)sv.f_frsize : (uint64_t)sv.f_bsize;
  const uint64_t free_bytes = (uint64_t)sv.f_bavail * unit;
  /* A number this small is more likely a misread than a full /data: treat it as unknown (writes still fail
   * cleanly if the drive really is full). */
  return free_bytes < (1u << 20) ? 0 : free_bytes;
}

int fs_lock(const char *p, int *fd_out) {
  const int fd = open(p, O_RDWR | O_CREAT | O_NOFOLLOW, 0666);
  if (fd < 0) {
    err_set("can't open %s: %s", p, strerror(errno));
    return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int e = errno;
    close(fd);
    if (e == EWOULDBLOCK || e == EAGAIN)
      return 1;
    err_set("can't lock %s: %s", p, strerror(e));
    return -1;
  }
  *fd_out = fd;
  return 0;
}

void fs_unlock(int fd) {
  if (fd >= 0) {
    flock(fd, LOCK_UN);
    close(fd);
  }
}
