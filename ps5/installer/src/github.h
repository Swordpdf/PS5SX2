/* PS5SX2 Installer: the latest release on GitHub (Swordpdf/PS5SX2), and its download. */
#pragma once

#include <stdint.h>

typedef struct {
  char tag[65];
  char title[200];
  char asset_name[201];
  char asset_url[2048];
  long long asset_size;
  char asset_sha256[65]; /* lowercase hex, from GitHub's "digest" */
  char published_at[32];
} gh_release;

/* 0 = found; -1 = err_get() has a sentence for the user. */
int gh_latest(gh_release *out);

typedef void (*gh_progress)(void *ctx, uint64_t got, uint64_t total);

/* Downloads to dest (via dest.part), checks the size and SHA-256; 0 on success. */
int gh_download(const gh_release *rel, const char *dest, gh_progress progress, void *ctx);
