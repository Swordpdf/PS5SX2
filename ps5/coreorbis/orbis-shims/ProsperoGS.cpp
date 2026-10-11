#include <cstdio>
#include <cstring>
#include <vector>
// Orbis GS shims: HW skip-count tables live in the HW renderer (excluded).
// Returning 0 = no skip/replacement (correct for Null/SW bring-up).
#include "GS/Renderers/HW/GSHwHack.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSPng.h"
#include "GSDumpReplayer.h"
#include "R5900.h"
#include "common/Error.h"
#include "common/Image.h"
#include <string>

RGBA8Image::RGBA8Image()
{
}
RGBA8Image::RGBA8Image(RGBA8Image&& move)
{
  (void)move;
}
bool RGBA8Image::SaveToFile(const char* filename, u8 quality) const
{
  (void)filename;
  (void)quality;
  return false;
}
bool RGBA8Image::SaveToFile(const char* filename, std::FILE* fp, u8 quality) const
{
  (void)filename;
  (void)fp;
  (void)quality;
  return false;
}

// Video capture needs ffmpeg (absent); bring-up never captures.
bool GSCapture::IsCapturing()
{
  return false;
}
const Threading::ThreadHandle& GSCapture::GetEncoderThreadHandle()
{
  static const Threading::ThreadHandle handle;
  return handle;
}
void GSCapture::DeliverAudioPacket(const float* frames)
{
  (void)frames;
}
void GSCapture::EndCapture()
{
}
bool GSCapture::IsCapturingVideo()
{
  return false;
}
GSVector2i GSCapture::GetSize()
{
  return GSVector2i(0);
}
std::string GSCapture::GetNextCaptureFileName()
{
  return std::string();
}
void GSCapture::Flush()
{
}
bool GSCapture::DeliverVideoFrame(GSTexture* stex)
{
  (void)stex;
  return false;
}
bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
  (void)fps;
  (void)recommendedResolution;
  (void)aspect;
  (void)filename;
  return false;
}

// GS dumps need zlib/lzma/zstd (absent); bring-up never dumps.
bool GSDumpBase::VSync(int field, bool last, const GSPrivRegSet* regs)
{
  (void)field;
  (void)last;
  (void)regs;
  return false;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateUncompressedDump(const std::string& fn, const std::string& serial,
  unsigned int crc, unsigned int screenshot_width, unsigned int screenshot_height, const unsigned int* screenshot_pixels,
  const freezeData& fd, const GSPrivRegSet* regs)
{
  (void)fn;
  (void)serial;
  (void)crc;
  (void)screenshot_width;
  (void)screenshot_height;
  (void)screenshot_pixels;
  (void)fd;
  (void)regs;
  return nullptr;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateXzDump(const std::string& fn, const std::string& serial,
  unsigned int crc, unsigned int screenshot_width, unsigned int screenshot_height, const unsigned int* screenshot_pixels,
  const freezeData& fd, const GSPrivRegSet* regs)
{
  (void)fn;
  (void)serial;
  (void)crc;
  (void)screenshot_width;
  (void)screenshot_height;
  (void)screenshot_pixels;
  (void)fd;
  (void)regs;
  return nullptr;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateZstDump(const std::string& fn, const std::string& serial,
  unsigned int crc, unsigned int screenshot_width, unsigned int screenshot_height, const unsigned int* screenshot_pixels,
  const freezeData& fd, const GSPrivRegSet* regs)
{
  (void)fn;
  (void)serial;
  (void)crc;
  (void)screenshot_width;
  (void)screenshot_height;
  (void)screenshot_pixels;
  (void)fd;
  (void)regs;
  return nullptr;
}

// Never constructed (Null renderer selected); satisfies the factory switch.
void GSDumpBase::Transfer(int index, const u8* mem, size_t size)
{
  (void)index;
  (void)mem;
  (void)size;
}

// PS5 port (vk-285-107): texture replacements. PCSX2's loaders (GSTextureReplacementLoaders.cpp) need libpng,
// which isn't built for the PS5, so until now every file in textures/<serial>/replacements was skipped. This
// loads PNG files with stb_image, the decoder the shelf already uses for covers, the way PCSX2's PNG loader does:
// 8-bit RGBA, and alpha 0x80 (the PS2's opaque) for a file without an alpha channel. (PCSX2's loader handles
// only 8-bit RGB and RGBA files; stb also takes grey, palette and 16-bit ones, converted to 8-bit RGBA.)
// vk-285-113: DDS files load too (OrbisDDS.h): the driver has no BC formats, so BC1, BC2, BC3 and BC7 textures are
// decoded to RGBA8 on the CPU, the uncompressed kinds are converted, and the file's mip levels are kept when the
// game uses mipmaps for the texture. Dumping textures stays off (SavePNGImage below). Needs proper testing.
#include "OrbisDDS.h"
#include "OrbisDeferredLog.h" // OrbisDeferredPrintf: the game's log lines go through the deferred log
#include <atomic>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../../frontend/third_party/stb_image.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include <climits>

// pr9l (2026-10-05, AI-assisted): a replacement inside the game's one-file pack, <texture folder>/replacements.pak, is named
// "ps5pak:..." (GSTextureReplacements.cpp, pcsx2/OrbisTexturePak.h) and read from there; any other name is a file. Both
// loaders below decode from memory. Needs proper testing.
bool OrbisPakName(const std::string& filename);
bool OrbisPakRead(const std::string& filename, std::vector<u8>& out);
static std::optional<std::vector<u8>> OrbisReadReplacement(const std::string& filename)
{
  if (!OrbisPakName(filename))
    return FileSystem::ReadBinaryFile(filename.c_str());
  std::vector<u8> data;
  if (!OrbisPakRead(filename, data))
  {
    static std::atomic<unsigned> s_reported{0};
    if (s_reported.fetch_add(1) < 5)
      OrbisDeferredPrintf("[texrep] couldn't read %s from the pack\n", filename.c_str());
    return std::nullopt;
  }
  return data;
}

static bool OrbisPNGLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
  (void)only_base_image; // a PNG holds one level; the mip levels are files of their own
  std::optional<std::vector<u8>> file = OrbisReadReplacement(filename);
  if (!file || file->empty() || file->size() > static_cast<size_t>(INT_MAX))
    return false;
  int w = 0, h = 0, comp = 0;
  stbi_uc* const px = stbi_load_from_memory(file->data(), static_cast<int>(file->size()), &w, &h, &comp, 4);
  if (!px)
    return false;
  if (w <= 0 || h <= 0)
  {
    stbi_image_free(px);
    return false;
  }
  const u32 pitch = static_cast<u32>(w) * 4u;
  tex->width = static_cast<u32>(w);
  tex->height = static_cast<u32>(h);
  tex->format = GSTexture::Format::Color;
  tex->pitch = pitch;
  tex->data.assign(px, px + static_cast<size_t>(pitch) * static_cast<size_t>(h));
  stbi_image_free(px);
  if (comp == 1 || comp == 3) // no alpha in the file: opaque, as PCSX2's loader makes RGB files
  {
    for (size_t i = 3; i < tex->data.size(); i += 4)
      tex->data[i] = 0x80;
  }
  return true;
}

// 2026-10-08 (AI-assisted): a PNG file as RGBA8 pixels (R in the low byte, as GSTexture::Format::Color takes them), for the
// RetroAchievements challenge icons and the overlay picture (GSRenderer.cpp; called on a thread of their own, never the GS
// thread). Its size is checked before it is decoded: false, with `why`, when it can't be read or decoded or is bigger than
// max_w x max_h (a badge is 64x64, an overlay at most 1920x1080).
bool OrbisLoadPng(const std::string& path, int max_w, int max_h, std::vector<u32>& rgba, int& w, int& h, std::string& why)
{
  std::optional<std::vector<u8>> file = FileSystem::ReadBinaryFile(path.c_str());
  if (!file || file->empty())
  {
    why = "can't be read";
    return false;
  }
  if (file->size() > (16u << 20))
  {
    why = "over 16 MB";
    return false;
  }
  int comp = 0;
  if (!stbi_info_from_memory(file->data(), static_cast<int>(file->size()), &w, &h, &comp))
  {
    why = "not a PNG";
    return false;
  }
  if (w <= 0 || h <= 0 || w > max_w || h > max_h)
  {
    why = std::to_string(w) + "x" + std::to_string(h) + ", bigger than " + std::to_string(max_w) + "x" + std::to_string(max_h);
    return false;
  }
  stbi_uc* const px = stbi_load_from_memory(file->data(), static_cast<int>(file->size()), &w, &h, &comp, 4);
  file.reset(); // the file's bytes go before the pixels are copied
  if (!px)
  {
    why = "couldn't be decoded";
    return false;
  }
  rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  std::memcpy(rgba.data(), px, rgba.size() * 4);
  stbi_image_free(px);
  return true;
}

// vk-285-113: a DDS file (BC1/2/3/7 or uncompressed) as an RGBA8 replacement with its mip levels.
static bool OrbisDDSLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
  std::optional<std::vector<u8>> file = OrbisReadReplacement(filename);
  if (!file || file->empty())
    return false;
  std::vector<OrbisDDS::Image> levels;
  const char* why = "";
  if (!OrbisDDS::Decode(file->data(), file->size(), only_base_image, levels, why))
  {
    static std::atomic<unsigned> s_reported{0};
    if (s_reported.fetch_add(1) < 10)
      OrbisDeferredPrintf("[texrep] %s: %s\n", filename.c_str(), why);
    return false;
  }
  OrbisDDS::Image& base = levels[0];
  tex->width = base.width;
  tex->height = base.height;
  tex->format = GSTexture::Format::Color;
  tex->pitch = base.width * 4u;
  tex->data = std::move(base.rgba);
  for (size_t i = 1; i < levels.size(); i++)
  {
    GSTextureReplacements::ReplacementTexture::MipData md;
    md.width = levels[i].width;
    md.height = levels[i].height;
    md.pitch = levels[i].width * 4u;
    md.data = std::move(levels[i].rgba);
    tex->mips.push_back(std::move(md));
  }
  return true;
}

GSTextureReplacements::ReplacementTextureLoader GSTextureReplacements::GetLoader(const std::string_view filename)
{
  const std::string_view ext = Path::GetExtension(filename);
  if (ext.size() == 3 && StringUtil::Strncasecmp(ext.data(), "png", 3) == 0)
    return OrbisPNGLoader;
  if (ext.size() == 3 && StringUtil::Strncasecmp(ext.data(), "dds", 3) == 0)
    return OrbisDDSLoader;
  return nullptr;
}
bool GSTextureReplacements::SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch)
{
  (void)filename;
  (void)width;
  (void)height;
  (void)buffer;
  (void)pitch;
  return false;
}

void GSDumpReplayer::RenderUI()
{
}bool GSDumpReplayer::IsRunner()
{
  return false;
}
void GSDumpBase::ReadFIFO(u32 size)
{
  (void)size;
}

namespace GSPng
{
// eerec-262: no libpng here -> write an uncompressed 32-bit BMP (BGRA, top-down) so HW dumps work.
bool Save(GSPng::Format fmt, const std::string& file, const u8* image, int w, int h, int pitch, int compression, bool rb_swapped)
{
  (void)compression;
  if (!image || w <= 0 || h <= 0)
    return false;
  const bool r8 = (fmt == GSPng::R8I_PNG || fmt == GSPng::R8I_PNG);
  std::string fn = file;
  if (fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".png") == 0)
    fn.replace(fn.size() - 4, 4, ".bmp");
  FILE* f = std::fopen(fn.c_str(), "wb");
  if (!f)
    return false;
  const u32 img = static_cast<u32>(w) * static_cast<u32>(h) * 4u;
  u8 hdr[54] = {};
  const u32 fsz = 54 + img, off = 54, dib = 40;
  const s32 nh = -h;
  hdr[0] = 'B'; hdr[1] = 'M';
  std::memcpy(hdr + 2, &fsz, 4); std::memcpy(hdr + 10, &off, 4); std::memcpy(hdr + 14, &dib, 4);
  std::memcpy(hdr + 18, &w, 4); std::memcpy(hdr + 22, &nh, 4);
  hdr[26] = 1; hdr[28] = 32; std::memcpy(hdr + 34, &img, 4);
  std::fwrite(hdr, 1, 54, f);
  std::vector<u8> row(static_cast<size_t>(w) * 4);
  for (int y = 0; y < h; y++)
  {
    const u8* src = image + static_cast<size_t>(y) * pitch;
    for (int x = 0; x < w; x++)
    {
      if (r8)
      {
        row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = src[x];
        row[x * 4 + 3] = 255;
      }
      else
      {
        const u8 r = rb_swapped ? src[x * 4 + 2] : src[x * 4 + 0];
        const u8 b = rb_swapped ? src[x * 4 + 0] : src[x * 4 + 2];
        row[x * 4 + 0] = b; row[x * 4 + 1] = src[x * 4 + 1]; row[x * 4 + 2] = r; row[x * 4 + 3] = src[x * 4 + 3];
      }
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}
} // namespace GSPng

// GS dump replay is dev-only (needs lzma); bring-up never replays.
namespace GSDumpReplayer
{
bool IsReplayingDump()
{
  return false;
}
bool Initialize(const char* filename, Error* error)
{
  (void)filename;
  Error::SetString(error, "GS dumps unsupported on Orbis");
  return false;
}
std::string GetDumpSerial()
{
  return std::string();
}
u32 GetDumpCRC()
{
  return 0;
}
void Shutdown()
{
}
bool ChangeDump(const char* filename)
{
  (void)filename;
  return false;
}
} // namespace GSDumpReplayer

R5900cpu GSDumpReplayerCpu{};
