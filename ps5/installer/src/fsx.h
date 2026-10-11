/* PS5SX2 Installer: file helpers. Nothing here deletes outside /data/PS5SX2-Installer. */
#pragma once

#include "util.h"

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

int fs_exists(const char *p);   /* anything there (no symlink follow) */
int fs_is_dir(const char *p);   /* a directory (links followed) */
int fs_is_file(const char *p);  /* a regular file (a symlink is not) */
int64_t fs_size(const char *p); /* -1 when not a regular file */
time_t fs_mtime(const char *p); /* 0 when missing */

int fs_mkdirs(const char *p, mode_t mode);
int fs_mkdirs_parent(const char *file, mode_t mode);

/* The whole file; fails if it is bigger than max. */
int fs_read_file(const char *p, size_t max, sbuf *out);
/* At most cap bytes: the first head bytes, a marker line, then the end. */
int fs_read_capped(const char *p, size_t cap, size_t head, sbuf *out, int64_t *size, time_t *mtime);

int fs_write_all(int fd, const void *d, size_t n);
/* p.tmp, fsync, rename over p. */
int fs_write_atomic(const char *p, const void *d, size_t n, mode_t mode);
int fs_append_sync(const char *p, const char *line); /* append + fsync (the journal) */

/* SHA-256 of a file as 64 lowercase hex digits. */
int fs_sha256(const char *p, char hex[65]);

/* rename() + fsync of both folders; across file systems: a complete copy (via <dst>.part), then the source goes. */
int fs_move(const char *src, const char *dst);
/* fsync of the folder holding path (makes a rename durable before the next journal line). */
int fs_dir_sync(const char *path_in_dir);
/* 1 if p is inside /data/PS5SX2-Installer with no link on the way. */
int fs_is_work_path(const char *p);
/* A new file in the work folder (an old one of that name is removed first; never through a link). */
int fs_create_work_file(const char *p, mode_t mode);

/* Recursive delete, only for paths inside /data/PS5SX2-Installer/ (refuses anything else). */
int fs_remove_work_tree(const char *p);
int fs_remove_work_file(const char *p);

/* Calls cb for each entry except . and .. ; cb returns non-zero to stop. */
typedef int (*fs_list_cb)(void *ctx, const char *name, int is_dir);
int fs_list(const char *dir, fs_list_cb cb, void *ctx);

/* Free bytes for an unprivileged writer on the file system holding p; 0 if unknown. */
uint64_t fs_free_bytes(const char *p);

/* flock(LOCK_EX|LOCK_NB): 0 = we hold it (fd kept open), 1 = someone else holds it, -1 = error. */
int fs_lock(const char *p, int *fd_out);
void fs_unlock(int fd);
