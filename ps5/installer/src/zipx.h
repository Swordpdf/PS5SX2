/* PS5SX2 Installer: reading the release zip (miniz), with strict checks on every entry. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned index;
  char *name;      /* as in the zip, without a trailing '/' */
  const char *rel; /* the part after the top folder ("PPSA99203/eboot.bin"); "" for the top folder itself */
  int is_dir;
  uint64_t size;
} zentry;

typedef struct zarch zarch;

/* Opens and checks the whole zip: one top folder, no absolute paths, no "..", no links, no duplicates,
 * no encryption, sizes within limits. NULL on error (err_get()). */
zarch *zip_open_checked(const char *path);
void zip_close(zarch *z);
size_t zip_count(const zarch *z);
const zentry *zip_entry(const zarch *z, size_t i);
const char *zip_top(const zarch *z);
uint64_t zip_total_size(const zarch *z);
/* Extracts one file to dest (created, CRC-32 checked by miniz, fsync'ed); 0 on success. */
int zip_extract(zarch *z, const zentry *e, const char *dest);
