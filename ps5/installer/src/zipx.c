/* PS5SX2 Installer: reading the release zip with miniz (reads through pread, extracts through write). */
#include "zipx.h"

#include "config.h"
#include "fsx.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miniz.h"

struct zarch {
  int fd;
  uint64_t file_size;
  mz_zip_archive zip;
  int zip_ready;
  zentry *e;
  size_t n;
  char top[256];
  uint64_t total;
};

static size_t zread(void *opaque, mz_uint64 ofs, void *buf, size_t n) {
  zarch *z = opaque;
  size_t done = 0;
  while (done < n) {
    const ssize_t r = pread(z->fd, (char *)buf + done, n - done, (off_t)(ofs + done));
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      break;
    done += (size_t)r;
  }
  return done;
}

static int name_ok(const char *s, int is_dir) {
  const size_t n = strlen(s);
  if (n == 0 || n > MAX_ENTRY_NAME || s[0] == '/')
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if (*p < 0x20 || *p == 0x7f || *p == '\\')
      return 0;
  /* components: not empty, not "." or ".." (a trailing '/' is allowed for folders) */
  const char *c = s;
  for (;;) {
    const char *slash = strchr(c, '/');
    const size_t len = slash ? (size_t)(slash - c) : strlen(c);
    if (len == 0) {
      if (!slash && is_dir && c != s)
        return 1; /* the trailing '/' of a folder */
      return 0;
    }
    if ((len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
      return 0;
    if (!slash)
      return 1;
    c = slash + 1;
  }
}

static int cmp_entry(const void *a, const void *b) {
  return strcmp(((const zentry *)a)->name, ((const zentry *)b)->name);
}

void zip_close(zarch *z) {
  if (!z)
    return;
  if (z->zip_ready)
    mz_zip_reader_end(&z->zip);
  for (size_t i = 0; i < z->n; i++)
    free(z->e[i].name);
  free(z->e);
  if (z->fd >= 0)
    close(z->fd);
  free(z);
}

zarch *zip_open_checked(const char *path) {
  zarch *z = calloc(1, sizeof(*z));
  if (!z) {
    err_set("out of memory");
    return NULL;
  }
  z->fd = open(path, O_RDONLY);
  if (z->fd < 0) {
    err_set("can't open the zip: %s", strerror(errno));
    free(z);
    return NULL;
  }
  struct stat st;
  if (fstat(z->fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    err_set("the zip isn't a file");
    zip_close(z);
    return NULL;
  }
  z->file_size = (uint64_t)st.st_size;
  mz_zip_zero_struct(&z->zip);
  z->zip.m_pRead = zread;
  z->zip.m_pIO_opaque = z;
  if (!mz_zip_reader_init(&z->zip, z->file_size, 0)) {
    err_set("the zip can't be read (%s)", mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));
    zip_close(z);
    return NULL;
  }
  z->zip_ready = 1;
  const mz_uint count = mz_zip_reader_get_num_files(&z->zip);
  if (count == 0 || count > MAX_ZIP_ENTRIES) {
    err_set("the zip has %u entries", (unsigned)count);
    zip_close(z);
    return NULL;
  }
  z->e = calloc(count, sizeof(zentry));
  if (!z->e) {
    err_set("out of memory");
    zip_close(z);
    return NULL;
  }
  for (mz_uint i = 0; i < count; i++) {
    mz_zip_archive_file_stat fs;
    if (!mz_zip_reader_file_stat(&z->zip, i, &fs)) {
      err_set("zip entry %u can't be read", (unsigned)i);
      zip_close(z);
      return NULL;
    }
    const int is_dir = mz_zip_reader_is_file_a_directory(&z->zip, i) ? 1 : 0;
    if (!name_ok(fs.m_filename, is_dir)) {
      err_set("the zip has an entry with a name we don't accept: %.100s", fs.m_filename);
      zip_close(z);
      return NULL;
    }
    if (fs.m_is_encrypted || !fs.m_is_supported) {
      err_set("the zip has an encrypted or unsupported entry: %.100s", fs.m_filename);
      zip_close(z);
      return NULL;
    }
    const unsigned host_os = (unsigned)(fs.m_version_made_by >> 8);
    const unsigned mode = (unsigned)(fs.m_external_attr >> 16);
    if (host_os == 3 /* Unix */ && (mode & 0170000) == 0120000) {
      err_set("the zip has a link, which we don't install: %.100s", fs.m_filename);
      zip_close(z);
      return NULL;
    }
    if (!is_dir && fs.m_uncomp_size > MAX_ZIP_BYTES) {
      err_set("a file in the zip is too big: %.100s", fs.m_filename);
      zip_close(z);
      return NULL;
    }
    z->total += fs.m_uncomp_size;
    if (z->total > MAX_UNZIPPED_BYTES) {
      err_set("the zip unpacks to more than %llu MB", (unsigned long long)(MAX_UNZIPPED_BYTES >> 20));
      zip_close(z);
      return NULL;
    }
    size_t len = strlen(fs.m_filename);
    if (is_dir && len && fs.m_filename[len - 1] == '/')
      len--;
    z->e[z->n].index = i;
    z->e[z->n].name = str_ndup(fs.m_filename, len);
    z->e[z->n].is_dir = is_dir;
    z->e[z->n].size = is_dir ? 0 : fs.m_uncomp_size;
    if (!z->e[z->n].name) {
      zip_close(z);
      return NULL;
    }
    z->n++;
  }
  /* One top folder for everything; no file at the top level of the zip. */
  for (size_t i = 0; i < z->n; i++) {
    const char *name = z->e[i].name;
    const char *slash = strchr(name, '/');
    const size_t tl = slash ? (size_t)(slash - name) : strlen(name);
    if (!slash && !z->e[i].is_dir) {
      err_set("the zip has a file outside its folder: %.100s", name);
      zip_close(z);
      return NULL;
    }
    if (tl >= sizeof(z->top)) {
      err_set("the zip's folder name is too long");
      zip_close(z);
      return NULL;
    }
    if (!z->top[0]) {
      memcpy(z->top, name, tl);
      z->top[tl] = '\0';
    } else if (strlen(z->top) != tl || strncmp(z->top, name, tl) != 0) {
      err_set("the zip has more than one top folder (%s and %.*s)", z->top, (int)tl, name);
      zip_close(z);
      return NULL;
    }
    z->e[i].rel = slash ? slash + 1 : name + tl;
  }
  /* No duplicates, and no file that is also a folder of another entry. */
  qsort(z->e, z->n, sizeof(zentry), cmp_entry);
  for (size_t i = 0; i + 1 < z->n; i++) {
    if (!strcmp(z->e[i].name, z->e[i + 1].name)) {
      err_set("the zip has the same name twice: %.100s", z->e[i].name);
      zip_close(z);
      return NULL;
    }
  }
  for (size_t i = 0; i < z->n; i++) {
    if (z->e[i].is_dir)
      continue;
    const size_t len = strlen(z->e[i].name);
    for (size_t k = i + 1; k < z->n; k++) {
      const int c = strncmp(z->e[k].name, z->e[i].name, len);
      if (c > 0)
        break;
      if (c == 0 && z->e[k].name[len] == '/') {
        err_set("the zip has %.100s as a file and as a folder", z->e[i].name);
        zip_close(z);
        return NULL;
      }
    }
  }
  return z;
}

size_t zip_count(const zarch *z) { return z->n; }
const zentry *zip_entry(const zarch *z, size_t i) { return i < z->n ? &z->e[i] : NULL; }
const char *zip_top(const zarch *z) { return z->top; }
uint64_t zip_total_size(const zarch *z) { return z->total; }

typedef struct {
  int fd;
  uint64_t written;
  int failed;
} wctx;

static size_t zwrite(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
  wctx *w = opaque;
  if (ofs != w->written || fs_write_all(w->fd, buf, n) != 0) {
    w->failed = errno ? errno : EIO;
    return 0;
  }
  w->written += n;
  return n;
}

int zip_extract(zarch *z, const zentry *e, const char *dest) {
  wctx w;
  w.fd = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666); /* staging is new for every install */
  w.written = 0;
  w.failed = 0;
  if (w.fd < 0) {
    err_set("can't create %s: %s", dest, strerror(errno));
    return -1;
  }
  errno = 0;
  const mz_bool ok = mz_zip_reader_extract_to_callback(&z->zip, e->index, zwrite, &w, 0);
  int rc = 0;
  if (!ok || w.written != e->size) {
    if (w.failed)
      err_set("can't write %s: %s", dest, strerror(w.failed));
    else
      err_set("%.100s in the zip is damaged (%s)", e->name,
              mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));
    rc = -1;
  } else if (fsync(w.fd) != 0) {
    err_set("can't save %s: %s", dest, strerror(errno));
    rc = -1;
  }
  close(w.fd);
  return rc;
}
