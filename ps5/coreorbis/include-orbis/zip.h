#pragma once
// Orbis libzip parse+link shim: compressed archives unsupported (no zlib).
// Raw files work; zip-backed features fail gracefully at runtime.
#include <stddef.h>
#include <stdint.h>

typedef struct zip zip_t;
typedef struct zip_file zip_file_t;
typedef struct zip_source zip_source_t;
typedef struct zip_error
{
  int zip_err;
  int sys_err;
} zip_error_t;
typedef uint32_t zip_flags_t;
typedef uint64_t zip_uint64_t;
typedef int64_t zip_int64_t;
typedef struct zip_stat
{
  uint64_t valid;
  const char* name;
  uint64_t index;
  uint64_t size;
  uint64_t comp_size;
} zip_stat_t;

#define ZIP_RDONLY 2u
#define ZIP_FL_NOCASE 32u

#ifdef __cplusplus
extern "C" {
#endif

zip_source_t* zip_source_file_create(const char* fname, uint64_t start, int64_t len, zip_error_t* error);
zip_t* zip_open_from_source(zip_source_t* src, int flags, zip_error_t* error);
const char* zip_error_strerror(zip_error_t* err);
void zip_source_free(zip_source_t* src);
int zip_close(zip_t* archive);
void zip_discard(zip_t* archive);
zip_source_t* zip_source_buffer_create(const void* data, uint64_t len, int freep, zip_error_t* error);
zip_file_t* zip_fopen(zip_t* archive, const char* fname, zip_flags_t flags);
zip_file_t* zip_fopen_index(zip_t* archive, uint64_t index, zip_flags_t flags);
int zip_fclose(zip_file_t* file);
int64_t zip_fread(zip_file_t* file, void* buf, uint64_t nbytes);
int64_t zip_name_locate(zip_t* archive, const char* fname, zip_flags_t flags);
int zip_stat_index(zip_t* archive, uint64_t index, zip_flags_t flags, zip_stat_t* sb);

#ifdef __cplusplus
}
#endif
