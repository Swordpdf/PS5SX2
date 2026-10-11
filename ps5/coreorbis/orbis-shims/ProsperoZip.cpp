// Orbis libzip stubs: archives unsupported (no zlib). All entry points
// fail gracefully; raw .iso/.bin and unpacked content work normally.
#include "zip.h"
#include <cstring>

// eerec-282: an in-memory "file" so SaveState.cpp's FreezeIn code (zip_fread) can read state entries.
struct zip_file
{
  const unsigned char* data;
  uint64_t size;
  uint64_t pos;
};

extern "C" zip_file_t* orbis_zip_mem_fopen(const void* data, uint64_t size)
{
  return new zip_file{static_cast<const unsigned char*>(data), size, 0};
}

extern "C" zip_source_t* zip_source_file_create(const char* fname, uint64_t start, int64_t len, zip_error_t* error)
{
  (void)fname;
  (void)start;
  (void)len;
  (void)error;
  return 0;
}
extern "C" zip_t* zip_open_from_source(zip_source_t* src, int flags, zip_error_t* error)
{
  (void)src;
  (void)flags;
  (void)error;
  return 0;
}
extern "C" const char* zip_error_strerror(zip_error_t* err)
{
  (void)err;
  return "zip unsupported on Orbis";
}
extern "C" void zip_source_free(zip_source_t* src)
{
  (void)src;
}
extern "C" int zip_close(zip_t* archive)
{
  (void)archive;
  return -1;
}
extern "C" void zip_discard(zip_t* archive)
{
  (void)archive;
}
extern "C" zip_source_t* zip_source_buffer_create(const void* data, uint64_t len, int freep, zip_error_t* error)
{
  (void)data;
  (void)len;
  (void)freep;
  (void)error;
  return 0;
}
extern "C" zip_file_t* zip_fopen(zip_t* archive, const char* fname, zip_flags_t flags)
{
  (void)archive;
  (void)fname;
  (void)flags;
  return 0;
}
extern "C" zip_file_t* zip_fopen_index(zip_t* archive, uint64_t index, zip_flags_t flags)
{
  (void)archive;
  (void)index;
  (void)flags;
  return 0;
}
extern "C" int zip_fclose(zip_file_t* file)
{
  delete file; // eerec-282
  return 0;
}
extern "C" int64_t zip_fread(zip_file_t* file, void* buf, uint64_t nbytes)
{
  if (!file) // eerec-282
    return -1;
  const uint64_t left = file->size - file->pos;
  const uint64_t n = nbytes < left ? nbytes : left;
  if (n)
    std::memcpy(buf, file->data + file->pos, static_cast<size_t>(n));
  file->pos += n;
  return static_cast<int64_t>(n);
}
extern "C" int64_t zip_name_locate(zip_t* archive, const char* fname, zip_flags_t flags)
{
  (void)archive;
  (void)fname;
  (void)flags;
  return -1;
}
extern "C" int zip_stat_index(zip_t* archive, uint64_t index, zip_flags_t flags, zip_stat_t* sb)
{
  (void)archive;
  (void)index;
  (void)flags;
  (void)sb;
  return -1;
}
