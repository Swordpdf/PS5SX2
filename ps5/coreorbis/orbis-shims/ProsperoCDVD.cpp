// Orbis CDVD: .gz images need PCSX2's indexed-zlib reader (GzippedFileReader), which isn't
// built, so that backend refuses. Raw .iso/.bin works fully via the real InputIsoFile
// (kept in build); .chd since vk-285-108 (the real ChdFileReader over libchdr, with zlib and
// zstd vendored in ps5/third_party); .cso and .zso since vk-285-113 (the real CsoFileReader over
// the vendored inflate and lz4). Physical discs: OrbisIOCtlSrc.cpp.
#include "CDVD/GzippedFileReader.h"
#include "common/Error.h"

static bool Unsupported(Error* error, const char* what)
{
  Error::SetString(error, what);
  return false;
}

GzippedFileReader::GzippedFileReader() = default;
GzippedFileReader::~GzippedFileReader() = default;
bool GzippedFileReader::Open2(std::string filename, Error* error)
{
  (void)filename;
  return Unsupported(error, "GZIP images not supported on Orbis (use .cso, .zso or .chd)");
}
ThreadedFileReader::Chunk GzippedFileReader::ChunkForOffset(u64 offset)
{
  (void)offset;
  return ThreadedFileReader::Chunk{-1, 0, 0};
}
int GzippedFileReader::ReadChunk(void* dst, s64 chunkID)
{
  (void)dst;
  (void)chunkID;
  return -1;
}
void GzippedFileReader::Close2()
{
}
u32 GzippedFileReader::GetBlockCount() const
{
  return 0;
}
