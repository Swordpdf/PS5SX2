/* PS5SX2 Installer: what earlier test builds shipped (generated from history/<build>.json by tools/history.py). */
#pragma once

#include <stddef.h>

typedef struct {
  const char *key; /* "PCSX2/settings/Oni (USA).ini" */
  const char *sha; /* SHA-256, lowercase hex */
} shipped_file;

extern const shipped_file g_shipped_files[];
extern const size_t g_shipped_files_n;
extern const char *const g_shipped_flags[];
extern const size_t g_shipped_flags_n;
