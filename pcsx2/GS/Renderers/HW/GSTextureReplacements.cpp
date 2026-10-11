// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/AlignedMalloc.h"
#include "common/Console.h"
#include "common/HashCombine.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/ScopedGuard.h"
#include "common/TextureDecompress.h"

#include "Config.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "GS/GSExtra.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "VMManager.h"

#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <thread>

// this is a #define instead of a variable to avoid warnings from non-literal format strings
#define TEXTURE_FILENAME_FORMAT_STRING "%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_REGION_FORMAT_STRING "%" PRIx64 "-r%ux%u-%08x"
#define TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-r%ux%u-%08x"
#define TEXTURE_FILENAME_OLD_REGION_FORMAT_STRING "%" PRIx64 "-r%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_OLD_REGION_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-r%" PRIx64 "-%08x"
#define TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME "replacements"
#define TEXTURE_DUMP_SUBDIRECTORY_NAME "dumps"

namespace
{
	struct TextureName // 32 bytes
	{
		u64 TEX0Hash;
		u64 CLUTHash;
		u32 region_width;
		u32 region_height;

		union
		{
			struct
			{
				u32 TEX0_PSM : 6;
				u32 TEX0_TW : 4;
				u32 TEX0_TH : 4;
				u32 unused0 : 1; // was TCC
				u32 TEXA_TA0 : 8;
				u32 TEXA_AEM : 1;
				u32 TEXA_TA1 : 8;
			};
			u32 bits;
		};
		u32 miplevel;

		__fi u32 Width() const { return (region_width ? region_width : (1u << TEX0_TW)); }
		__fi u32 Height() const { return (region_height ? region_height : (1u << TEX0_TH)); }
		__fi bool HasPalette() const { return (GSLocalMemory::m_psm[TEX0_PSM].pal > 0); }
		__fi bool HasRegion() const { return (region_width != 0 || region_height != 0); }

		__fi bool operator==(const TextureName& rhs) const { return BitEqual(*this, rhs); }
		__fi bool operator!=(const TextureName& rhs) const { return !BitEqual(*this, rhs); }
		__fi bool operator<(const TextureName& rhs) const { return (std::memcmp(this, &rhs, sizeof(*this)) < 0); }

		__fi void RemoveUnusedBits()
		{
			// Remove bits which were previously present, but no longer used.
			unused0 = 0;
		}
	};
	static_assert(sizeof(TextureName) == 32, "ReplacementTextureName is expected size");
} // namespace

namespace std
{
	template <>
	struct hash<TextureName>
	{
		std::size_t operator()(const TextureName& val) const
		{
			std::size_t h = 0;
			HashCombine(h, val.TEX0Hash, val.CLUTHash,
				static_cast<u64>(val.region_width) | (static_cast<u64>(val.region_height) << 32),
				static_cast<u64>(val.bits) | (static_cast<u64>(val.miplevel) << 32));
			return h;
		}
	};
} // namespace std

namespace GSTextureReplacements
{
	static TextureName CreateTextureName(const GSTextureCache::HashCacheKey& hash, u32 miplevel);
	static GSTextureCache::HashCacheKey HashCacheKeyFromTextureName(const TextureName& tn);
	static std::optional<TextureName> ParseReplacementName(const std::string& filename);
	static std::string GetGameTextureDirectory();
	static std::string GetDumpFilename(const TextureName& name, u32 level);
	template <GSTexture::Format format>
	std::pair<u8, u8> GetBCAlphaMinMax(ReplacementTexture& rtex);
	static void SetReplacementTextureAlphaMinMax(ReplacementTexture& rtex);
	static std::optional<ReplacementTexture> LoadReplacementTexture(const TextureName& name, const std::string& filename, bool only_base_image);
	static void QueueAsyncReplacementTextureLoad(const TextureName& name, const std::string& filename, bool mipmap, bool cache_only);
	static void PrecacheReplacementTextures();
	static void ClearReplacementTextures();

	static void StartWorkerThread();
	static void StopWorkerThread();
	static void QueueWorkerThreadItem(std::function<void()> fn, bool high_priority);
	static void WorkerThreadEntryPoint();
	static void SyncWorkerThread();
	static void CancelPendingLoadsAndDumps();

	static std::string s_current_serial;

	/// Textures that have been dumped, to save stat() calls.
	static std::unordered_set<TextureName> s_dumped_textures;
	static std::mutex s_dumped_textures_mutex;

	/// Lookup map of texture names to replacements, if they exist.
	static std::unordered_map<TextureName, std::string> s_replacement_texture_filenames;

	/// Lookup map of texture names without CLUT hash, to know when we need to disable paltex.
	static std::unordered_set<TextureName> s_replacement_textures_without_clut_hash;

	/// Lookup map of texture names to replacement data which has been cached.
	static std::unordered_map<TextureName, ReplacementTexture> s_replacement_texture_cache;
	static std::mutex s_replacement_texture_cache_mutex;

	/// List of textures that are pending asynchronous load. Second element is whether we're only precaching.
	static std::unordered_map<TextureName, bool> s_pending_async_load_textures;

	/// List of textures that we have asynchronously loaded and can now be injected back into the TC.
	/// Second element is whether the texture should be created with mipmaps.
	static std::vector<std::pair<TextureName, bool>> s_async_loaded_textures;

	/// Loader/dumper thread.
	static std::thread s_worker_thread;
	static std::mutex s_worker_thread_mutex;
	static std::condition_variable s_worker_thread_cv;
	static std::deque<std::pair<std::function<void()>, bool>> s_worker_thread_queue;
	static bool s_worker_thread_running = false;
}; // namespace GSTextureReplacements

#ifdef ORBIS_VULKAN
// PS5 port (vk-285-111): what the texture replacements do, in boot.log. A pack in the right folder with the
// right names (swordpdf's Ratchet & Clank one, PNG) showed no sign of loading, and PCSX2 itself says what it
// found only in debug builds. These lines say how many files the scan found, whether the game's textures
// match them (and, for a miss whose texture hash is in the pack, which part of the name differs), and
// whether the files load and reach the GPU. Diagnostics only; needs proper testing.
#include "OrbisDeferredLog.h"
#include "OrbisTexturePak.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
// vk-285-112 (GSRenderer.cpp): the loader thread runs on the pin layout's helper CPUs. It starts with its
// creator's CPUs, the GS thread's, and a 16 MB PNG takes a while to decode.
void OrbisHelperThreadAdd(pthread_t thread);
void OrbisHelperThreadRemove(pthread_t thread);
namespace
{
	struct OrbisTexRep
	{
		std::atomic<u32> lookups{0}, hits{0}, queued{0}, loaded{0}, load_failed{0}, created{0}, create_failed{0};
		std::atomic<u64> load_us{0}, load_pixels{0};
		u32 miss_logged = 0, near_logged = 0, hit_logged = 0; // the GS thread only
		u32 last[7] = {};
		std::chrono::steady_clock::time_point last_report{};
		std::unordered_multimap<u64, TextureName> by_tex0; // the pack's names by texture hash, for the near misses
	};
	OrbisTexRep s_orbis_texrep;

	std::string OrbisTexName(const TextureName& n)
	{
		if (n.HasRegion())
			return n.HasPalette() ? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING, n.TEX0Hash,
										n.CLUTHash, n.region_width, n.region_height, n.bits) :
									StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_FORMAT_STRING, n.TEX0Hash,
										n.region_width, n.region_height, n.bits);
		return n.HasPalette() ? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_CLUT_FORMAT_STRING, n.TEX0Hash, n.CLUTHash, n.bits) :
								StringUtil::StdStringFromFormat(TEXTURE_FILENAME_FORMAT_STRING, n.TEX0Hash, n.bits);
	}

	// vk-285-112: the decoded copies s_replacement_texture_cache keeps, in bytes (under its mutex). PCSX2 keeps
	// every loaded replacement in memory until the game changes; the PS5 app has ~150 MB of heap left while a game
	// runs, and one 2048x2048 replacement is 16 MB (swordpdf's R&C pack has dozens). Past this budget a copy is
	// dropped once it's on the GPU, and a texture the hash cache lets go is read from its file again when the game
	// uses it next. Needs proper testing.
	size_t s_orbis_cache_bytes = 0;
	constexpr size_t ORBIS_CACHE_BUDGET = 48u << 20;

	size_t OrbisReplacementBytes(const GSTextureReplacements::ReplacementTexture& r)
	{
		size_t n = r.data.size();
		for (const auto& m : r.mips)
			n += m.data.size();
		return n;
	}

	// pr9l (2026-10-05, AI-assisted): the game's one-file pack, <game's texture folder>/replacements.pak (OrbisTexturePak.h,
	// written by the texture pack manager: a new file costs the PS5 app 10 ms to a second, so a pack of 15,000 files took
	// most of an hour to unpack as files). Open while its replacements are in use; a replacement in it is named
	// "ps5pak:<generation>:<offset>:<size>/<its path in the pack>" in s_replacement_texture_filenames, and the PS5's loaders
	// (ps5/coreorbis/orbis-shims/ProsperoGS.cpp; GSTextureReplacementLoaders.cpp isn't built) read such a name's bytes with
	// OrbisPakRead. Needs proper testing.
	struct OrbisPak
	{
		int fd = -1;
		u32 generation = 0;
		~OrbisPak()
		{
			if (fd >= 0)
				close(fd);
		}
	};
	std::mutex s_orbis_pak_mutex;
	std::shared_ptr<OrbisPak> s_orbis_pak; // under s_orbis_pak_mutex: the loader thread reads through a copy
	u32 s_orbis_pak_generation = 0; // the GS thread only
	constexpr char ORBIS_PAK_PREFIX[] = "ps5pak:";
	constexpr size_t ORBIS_PAK_PREFIX_LEN = sizeof(ORBIS_PAK_PREFIX) - 1;

	void OrbisPakClose()
	{
		std::lock_guard<std::mutex> lock(s_orbis_pak_mutex);
		s_orbis_pak.reset();
	}

	bool OrbisPakNumber(const char*& p, char end, unsigned long long& out)
	{
		char* stop = nullptr;
		if (*p < '0' || *p > '9')
			return false;
		out = std::strtoull(p, &stop, 10);
		if (!stop || *stop != end)
			return false;
		p = stop + 1;
		return true;
	}
} // namespace

bool OrbisPakName(const std::string& filename)
{
	return filename.compare(0, ORBIS_PAK_PREFIX_LEN, ORBIS_PAK_PREFIX) == 0;
}

// The bytes of a "ps5pak:..." replacement, from the pack open now (false when the game's pack changed since it was named).
bool OrbisPakRead(const std::string& filename, std::vector<u8>& out)
{
	if (!OrbisPakName(filename))
		return false;
	const char* p = filename.c_str() + ORBIS_PAK_PREFIX_LEN;
	unsigned long long generation = 0, offset = 0, size = 0;
	if (!OrbisPakNumber(p, ':', generation) || !OrbisPakNumber(p, ':', offset) || !OrbisPakNumber(p, '/', size) || size > (1ull << 30))
		return false;
	std::shared_ptr<OrbisPak> pak;
	{
		std::lock_guard<std::mutex> lock(s_orbis_pak_mutex);
		pak = s_orbis_pak;
	}
	if (!pak || pak->generation != generation)
		return false;
	out.resize(static_cast<size_t>(size));
	return size == 0 || OrbisTexturePak::PreadAll(pak->fd, out.data(), out.size(), offset);
}

namespace
{
	// Once in 5 s at most, when something changed: the counters.
	void OrbisTexRepReport(bool force)
	{
		OrbisTexRep& r = s_orbis_texrep;
		const u32 now_vals[7] = {r.lookups.load(), r.hits.load(), r.queued.load(), r.loaded.load(), r.load_failed.load(),
			r.created.load(), r.create_failed.load()};
		const auto now = std::chrono::steady_clock::now();
		if (!force && (std::memcmp(now_vals, r.last, sizeof(r.last)) == 0 || now - r.last_report < std::chrono::seconds(5)))
			return;
		std::memcpy(r.last, now_vals, sizeof(r.last));
		r.last_report = now;
		const u32 n = now_vals[3];
		OrbisDeferredPrintf("[texrep] lookups %u, hits %u, loads queued %u, loaded %u (avg %.0f ms, %.1f Mpx), failed %u, "
							"on the GPU %u, GPU texture failed %u\n",
			now_vals[0], now_vals[1], now_vals[2], n, n ? r.load_us.load() / 1000.0 / n : 0.0, r.load_pixels.load() / 1e6,
			now_vals[4], now_vals[5], now_vals[6]);
	}
} // namespace
#endif

TextureName GSTextureReplacements::CreateTextureName(const GSTextureCache::HashCacheKey& hash, u32 miplevel)
{
	TextureName name;
	name.bits = 0;
	name.TEX0_PSM = hash.TEX0.PSM;
	name.TEX0_TW = hash.TEX0.TW;
	name.TEX0_TH = hash.TEX0.TH;
	name.TEXA_TA0 = hash.TEXA.TA0;
	name.TEXA_AEM = hash.TEXA.AEM;
	name.TEXA_TA1 = hash.TEXA.TA1;
	name.TEX0Hash = hash.TEX0Hash;
	name.CLUTHash = name.HasPalette() ? hash.CLUTHash : 0;
	name.miplevel = miplevel;
	name.region_width = hash.region_width;
	name.region_height = hash.region_height;
	return name;
}

GSTextureCache::HashCacheKey GSTextureReplacements::HashCacheKeyFromTextureName(const TextureName& tn)
{
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[tn.TEX0_PSM];
	GSTextureCache::HashCacheKey key = {};
	key.TEX0.PSM = tn.TEX0_PSM;
	key.TEX0.TW = tn.TEX0_TW;
	key.TEX0.TH = tn.TEX0_TH;
	if (psm_s.pal == 0 && psm_s.fmt > 0)
	{
		key.TEXA.TA0 = tn.TEXA_TA0;
		key.TEXA.AEM = tn.TEXA_AEM;
		key.TEXA.TA1 = tn.TEXA_TA1;
	}
	key.TEX0Hash = tn.TEX0Hash;
	key.CLUTHash = tn.HasPalette() ? tn.CLUTHash : 0;
	key.region_width = tn.region_width;
	key.region_height = tn.region_height;
	return key;
}

std::optional<TextureName> GSTextureReplacements::ParseReplacementName(const std::string& filename)
{
	TextureName ret;
	ret.miplevel = 0;

	GSTextureCache::SourceRegion full_region;

	char extension_dot;
	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash,
			&ret.region_width, &ret.region_height, &ret.bits, &extension_dot) == 6 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		return ret;
	}

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_REGION_FORMAT_STRING "%c", &ret.TEX0Hash,
			&ret.region_width, &ret.region_height, &ret.bits, &extension_dot) == 5 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.CLUTHash = 0;
		return ret;
	}

	// Allow loading of dumped textures from older versions that included the full region bits.
	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_OLD_REGION_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash,
			&full_region.bits, &ret.bits, &extension_dot) == 5 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.region_width = static_cast<u32>(full_region.GetWidth());
		ret.region_height = static_cast<u32>(full_region.GetHeight());
		return ret;
	}

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_OLD_REGION_FORMAT_STRING "%c", &ret.TEX0Hash, &full_region.bits,
			&ret.bits, &extension_dot) == 4 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.CLUTHash = 0;
		ret.region_width = static_cast<u32>(full_region.GetWidth());
		ret.region_height = static_cast<u32>(full_region.GetHeight());
		return ret;
	}

	ret.region_width = 0;
	ret.region_height = 0;

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash, &ret.bits,
			&extension_dot) == 4 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		return ret;
	}

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.bits, &extension_dot) ==
			3 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.CLUTHash = 0;
		return ret;
	}

	return std::nullopt;
}

#ifdef ORBIS_VULKAN
// vk-285-113: a game's texture pack outside /data/PCSX2/textures: on a USB drive, or where the PS5SX2/TexturesDir
// setting says (main-boot.cpp, orbis-shims/OrbisTextureRoots.h). Found again at each ReloadReplacementMap; empty: the
// pack, if there is one, is in the textures folder.
extern std::string OrbisTexturesGameDir(const std::string& serial, std::string& how);
static std::string s_orbis_game_dir;
#endif

std::string GSTextureReplacements::GetGameTextureDirectory()
{
#ifdef ORBIS_VULKAN
	if (!s_orbis_game_dir.empty())
		return s_orbis_game_dir;
#endif
	return Path::Combine(EmuFolders::Textures, s_current_serial);
}

std::string GSTextureReplacements::GetDumpFilename(const TextureName& name, u32 level)
{
	std::string ret;
	if (s_current_serial.empty())
		return ret;

	const std::string game_dir(GetGameTextureDirectory());
	const std::string game_subdir(Path::Combine(game_dir, TEXTURE_DUMP_SUBDIRECTORY_NAME));

	if (!FileSystem::DirectoryExists(game_subdir.c_str()))
	{
		// create both dumps and replacements
		if (!FileSystem::CreateDirectoryPath(game_dir.c_str(), false) ||
			!FileSystem::EnsureDirectoryExists(game_subdir.c_str(), false) ||
			!FileSystem::EnsureDirectoryExists(Path::Combine(game_dir, TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME).c_str(), false))
		{
			// if it fails to create, we're not going to be able to use it anyway
			return ret;
		}
	}

	std::string filename;
	if (name.HasRegion())
	{
		if (name.HasPalette())
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "-mip%u.png",
					name.TEX0Hash, name.CLUTHash, name.region_width, name.region_height, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING ".png",
					name.TEX0Hash, name.CLUTHash, name.region_width, name.region_height, name.bits);
		}
		else
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_FORMAT_STRING "-mip%u.png",
					name.TEX0Hash, name.region_width, name.region_height, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_FORMAT_STRING ".png",
					name.TEX0Hash, name.region_width, name.region_height, name.bits);
		}
	}
	else
	{
		if (name.HasPalette())
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_CLUT_FORMAT_STRING "-mip%u.png",
				                                  name.TEX0Hash, name.CLUTHash, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_CLUT_FORMAT_STRING ".png",
				                                  name.TEX0Hash, name.CLUTHash, name.bits);
		}
		else
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_FORMAT_STRING "-mip%u.png",
				                                  name.TEX0Hash, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_FORMAT_STRING ".png",
				                                  name.TEX0Hash, name.bits);
		}
	}

	ret = Path::Combine(game_subdir, filename);

	return ret;
}

void GSTextureReplacements::Initialize()
{
	s_current_serial = VMManager::GetDiscSerial();

	if (GSConfig.DumpReplaceableTextures || GSConfig.LoadTextureReplacements)
		StartWorkerThread();

	ReloadReplacementMap();
}

void GSTextureReplacements::GameChanged()
{
	std::string new_serial = VMManager::GetDiscSerial();
	if (s_current_serial == new_serial)
		return;

	s_current_serial = std::move(new_serial);
	ReloadReplacementMap();
	ClearDumpedTextureList();
}

/// If the given file exists in the given directory, but with a different case than the original file, write its path to `*output` and return true.
static bool GetWrongCasePath(std::string* output, const char* dir, std::string_view file, FileSystem::FindResultsArray* reuseme)
{
	if (FileSystem::FindFiles(dir, "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, reuseme))
	{
		for (const FILESYSTEM_FIND_DATA& fd : *reuseme)
		{
			std::string_view name = Path::GetFileName(fd.FileName);
			if (name.size() != file.size())
				continue;
			if (0 == strncmp(name.data(), file.data(), name.size()))
				continue;
			if (0 == StringUtil::Strncasecmp(name.data(), file.data(), name.size()))
			{
				*output = fd.FileName;
				return true;
			}
		}
	}
	return false;
}

void GSTextureReplacements::ReloadReplacementMap()
{
	SyncWorkerThread();
#ifdef ORBIS_VULKAN
	s_orbis_game_dir.clear();
	OrbisPakClose();
#endif

	// clear out the caches
	{
		s_replacement_texture_filenames.clear();
		s_replacement_textures_without_clut_hash.clear();

		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		s_replacement_texture_cache.clear();
		s_pending_async_load_textures.clear();
		s_async_loaded_textures.clear();
#ifdef ORBIS_VULKAN
		s_orbis_cache_bytes = 0;
#endif
	}

	// can't replace bios textures.
	if (s_current_serial.empty() || !GSConfig.LoadTextureReplacements)
	{
#ifdef ORBIS_VULKAN
		s_orbis_texrep.by_tex0.clear();
		OrbisDeferredPrintf("[texrep] no scan: serial '%s', LoadTextureReplacements %d\n", s_current_serial.c_str(),
			GSConfig.LoadTextureReplacements ? 1 : 0);
#endif
		return;
	}

#ifdef ORBIS_VULKAN
	{
		std::string how;
		s_orbis_game_dir = OrbisTexturesGameDir(s_current_serial, how);
		if (!s_orbis_game_dir.empty())
			OrbisDeferredPrintf("[texrep] %s: the texture pack is in %s (%s)\n", s_current_serial.c_str(), s_orbis_game_dir.c_str(), how.c_str());
	}
#endif
	const std::string texture_dir = GetGameTextureDirectory();
	const std::string replacement_dir(Path::Combine(texture_dir, TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME));

	FileSystem::FindResultsArray files;

	// For some reason texture pack authors think it's a good idea to rename the replacements directory to something with the wrong case...
	std::string wrong_case_path;
	const std::string* right_case_path = nullptr;
	if (
#ifdef ORBIS_VULKAN
		s_orbis_game_dir.empty() && // vk-285-113: a pack found on a drive was found in whatever case its names have
#endif
		GetWrongCasePath(&wrong_case_path, EmuFolders::Textures.c_str(), s_current_serial, &files))
		right_case_path = &texture_dir;
	else if (GetWrongCasePath(&wrong_case_path, texture_dir.c_str(), TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME, &files))
		right_case_path = &replacement_dir;
	if (right_case_path)
	{
		Host::AddKeyedOSDMessage("TextureReplacementDirCaseMismatch",
			fmt::format(TRANSLATE_FS("TextureReplacement", "Texture replacement directory {} will not work on case sensitive filesystems.\n"
			                                               "Rename it to {} to remove this warning."),
			            wrong_case_path, *right_case_path),
			Host::OSD_WARNING_DURATION);
	}

#ifdef ORBIS_VULKAN
	s_orbis_texrep.by_tex0.clear();
	u32 orbis_no_loader = 0, orbis_bad_name = 0;
	std::string orbis_example_skipped;
	const auto orbis_t0 = std::chrono::steady_clock::now();
	// pr9l: the one-file pack beside replacements/ (see OrbisPak above).
	std::vector<OrbisTexturePak::Entry> orbis_pak_entries;
	u32 orbis_pak_generation = 0;
	const std::string orbis_pak_path(Path::Combine(texture_dir, OrbisTexturePak::kFileName));
	if (const int fd = open(orbis_pak_path.c_str(), O_RDONLY); fd >= 0)
	{
		auto pak = std::make_shared<OrbisPak>();
		pak->fd = fd;
		std::string error;
		if (OrbisTexturePak::ReadIndex(fd, orbis_pak_entries, error))
		{
			pak->generation = orbis_pak_generation = ++s_orbis_pak_generation;
			std::lock_guard<std::mutex> lock(s_orbis_pak_mutex);
			s_orbis_pak = std::move(pak);
		}
		else
		{
			orbis_pak_entries.clear();
			OrbisDeferredPrintf("[texrep] %s: %s isn't used: %s\n", s_current_serial.c_str(), orbis_pak_path.c_str(), error.c_str());
		}
	}
#endif
	if (!FileSystem::FindFiles(replacement_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE, &files))
	{
#ifdef ORBIS_VULKAN
		files.clear();
		if (orbis_pak_entries.empty())
		{
			OrbisDeferredPrintf("[texrep] %s: no files found in %s\n", s_current_serial.c_str(), replacement_dir.c_str());
			return;
		}
#else
		return;
#endif
	}

	std::string filename;
	for (FILESYSTEM_FIND_DATA& fd : files)
	{
		// file format we can handle?
		filename = Path::GetFileName(fd.FileName);
		if (!GetLoader(filename))
		{
#ifdef ORBIS_VULKAN
			orbis_no_loader++;
			if (orbis_example_skipped.empty())
				orbis_example_skipped = filename;
#endif
			continue;
		}

		// parse the name if it's valid
		std::optional<TextureName> name = ParseReplacementName(filename);
		if (!name.has_value())
		{
#ifdef ORBIS_VULKAN
			orbis_bad_name++;
			if (orbis_example_skipped.empty())
				orbis_example_skipped = filename;
#endif
			continue;
		}
#ifdef ORBIS_VULKAN
		s_orbis_texrep.by_tex0.emplace(name->TEX0Hash, name.value());
#endif

		DbgCon.WriteLn("Found %ux%u replacement '%.*s'", name->Width(), name->Height(), static_cast<int>(filename.size()), filename.data());
		s_replacement_texture_filenames.emplace(name.value(), std::move(fd.FileName));

		// zero out the CLUT hash, because we need this for checking if there's any replacements with this hash when using paltex
		name->CLUTHash = 0;
		s_replacement_textures_without_clut_hash.insert(name.value());
	}
#ifdef ORBIS_VULKAN
	// pr9l: the pack's replacements, after the loose files (a loose file of the same name wins). From the last entry back, so
	// of two entries with one name the later is used, as when the pack was unpacked into files.
	if (!orbis_pak_entries.empty())
	{
		const size_t before = s_replacement_texture_filenames.size();
		u32 pak_no_loader = 0, pak_bad_name = 0;
		for (auto it = orbis_pak_entries.rbegin(); it != orbis_pak_entries.rend(); ++it)
		{
			const size_t slash = it->name.rfind('/');
			filename = slash == std::string::npos ? it->name : it->name.substr(slash + 1);
			if (!GetLoader(filename))
			{
				pak_no_loader++;
				if (orbis_example_skipped.empty())
					orbis_example_skipped = filename;
				continue;
			}
			std::optional<TextureName> name = ParseReplacementName(filename);
			if (!name.has_value())
			{
				pak_bad_name++;
				if (orbis_example_skipped.empty())
					orbis_example_skipped = filename;
				continue;
			}
			s_orbis_texrep.by_tex0.emplace(name->TEX0Hash, name.value());
			s_replacement_texture_filenames.emplace(name.value(),
				StringUtil::StdStringFromFormat("%s%u:%llu:%llu/%s", ORBIS_PAK_PREFIX, orbis_pak_generation,
					static_cast<unsigned long long>(it->offset), static_cast<unsigned long long>(it->size), it->name.c_str()));
			name->CLUTHash = 0;
			s_replacement_textures_without_clut_hash.insert(name.value());
		}
		orbis_no_loader += pak_no_loader;
		orbis_bad_name += pak_bad_name;
		OrbisDeferredPrintf("[texrep] %s: %s: %zu files, %zu replacement textures from it (%u not PNG or DDS, %u not named like a "
							"replacement)\n",
			s_current_serial.c_str(), orbis_pak_path.c_str(), orbis_pak_entries.size(), s_replacement_texture_filenames.size() - before,
			pak_no_loader, pak_bad_name);
	}
	OrbisDeferredPrintf("[texrep] %s: %zu replacement textures in %s (%zu files: %u not PNG or DDS, %u not named like a "
						"replacement%s%s%s); scan %.0f ms; async %d, precache %d, GPU palettes %d, preloading %d\n",
		s_current_serial.c_str(), s_replacement_texture_filenames.size(), replacement_dir.c_str(), files.size(),
		orbis_no_loader, orbis_bad_name, orbis_example_skipped.empty() ? "" : ", e.g. '",
		orbis_example_skipped.c_str(), orbis_example_skipped.empty() ? "" : "'",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - orbis_t0).count(),
		GSConfig.LoadTextureReplacementsAsync ? 1 : 0, GSConfig.PrecacheTextureReplacements ? 1 : 0,
		GSConfig.GPUPaletteConversion ? 1 : 0, static_cast<int>(GSConfig.TexturePreloading));
#endif

	if (!s_replacement_texture_filenames.empty())
	{
		if (GSConfig.PrecacheTextureReplacements)
			PrecacheReplacementTextures();

		// log a warning when paltex is on and preloading is off, since we'll be disabling paltex
		if (GSConfig.GPUPaletteConversion && GSConfig.TexturePreloading != TexturePreloadingLevel::Full)
		{
			Console.Warning("Replacement textures were found, and GPU palette conversion is enabled without full preloading.");
			Console.Warning("Palette textures will be disabled. Please enable full preloading or disable GPU palette conversion.");
		}
	}
}

void GSTextureReplacements::UpdateConfig(Pcsx2Config::GSOptions& old_config)
{
	// get rid of worker thread if it's no longer needed
	if (s_worker_thread_running && !GSConfig.DumpReplaceableTextures && !GSConfig.LoadTextureReplacements)
		StopWorkerThread();
	if (!s_worker_thread_running && (GSConfig.DumpReplaceableTextures || GSConfig.LoadTextureReplacements))
		StartWorkerThread();

	if ((!GSConfig.DumpReplaceableTextures && old_config.DumpReplaceableTextures) ||
		(!GSConfig.LoadTextureReplacements && old_config.LoadTextureReplacements))
	{
		CancelPendingLoadsAndDumps();
	}

	if (GSConfig.LoadTextureReplacements && !old_config.LoadTextureReplacements)
		ReloadReplacementMap();
	else if (!GSConfig.LoadTextureReplacements && old_config.LoadTextureReplacements)
		ClearReplacementTextures();

	if (!GSConfig.DumpReplaceableTextures && old_config.DumpReplaceableTextures)
		ClearDumpedTextureList();

	if (GSConfig.LoadTextureReplacements && GSConfig.PrecacheTextureReplacements && !old_config.PrecacheTextureReplacements)
		PrecacheReplacementTextures();
}

void GSTextureReplacements::Shutdown()
{
	StopWorkerThread();

	std::string().swap(s_current_serial);
	ClearReplacementTextures();
	ClearDumpedTextureList();
}

u32 GSTextureReplacements::CalcMipmapLevelsForReplacement(u32 width, u32 height)
{
	return static_cast<u32>(std::log2(std::max(width, height))) + 1u;
}

bool GSTextureReplacements::HasAnyReplacementTextures()
{
	return !s_replacement_texture_filenames.empty();
}

bool GSTextureReplacements::HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash)
{
	const TextureName name(CreateTextureName(hash.WithRemovedCLUTHash(), 0));
	return s_replacement_textures_without_clut_hash.find(name) != s_replacement_textures_without_clut_hash.end();
}

GSTexture* GSTextureReplacements::LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap,
	bool* pending, std::pair<u8, u8>* alpha_minmax)
{
	const TextureName name(CreateTextureName(hash, 0));
	*pending = false;

	// replacement for this name exists?
	auto fnit = s_replacement_texture_filenames.find(name);
#ifdef ORBIS_VULKAN
	OrbisTexRep& orbis_r = s_orbis_texrep;
	orbis_r.lookups.fetch_add(1, std::memory_order_relaxed);
	if (fnit == s_replacement_texture_filenames.end())
	{
		// A texture whose hash is in the pack under another name says which part differs; the first few
		// plain misses show the names the game's textures get here.
		const auto range = orbis_r.by_tex0.equal_range(name.TEX0Hash);
		if (range.first != range.second && orbis_r.near_logged < 24)
		{
			orbis_r.near_logged++;
			OrbisDeferredPrintf("[texrep] near miss: the game's %s, the pack's %s\n", OrbisTexName(name).c_str(),
				OrbisTexName(range.first->second).c_str());
		}
		else if (range.first == range.second && orbis_r.miss_logged < 12)
		{
			orbis_r.miss_logged++;
			OrbisDeferredPrintf("[texrep] miss: %s (%ux%u)\n", OrbisTexName(name).c_str(), name.Width(), name.Height());
		}
	}
	else
	{
		orbis_r.hits.fetch_add(1, std::memory_order_relaxed);
		if (orbis_r.hit_logged < 10)
		{
			orbis_r.hit_logged++;
			OrbisDeferredPrintf("[texrep] hit: %s -> %s\n", OrbisTexName(name).c_str(), fnit->second.c_str());
		}
	}
#endif
	if (fnit == s_replacement_texture_filenames.end())
		return nullptr;

	// try the full cache first, to avoid reloading from disk
	{
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		auto it = s_replacement_texture_cache.find(name);
		if (it != s_replacement_texture_cache.end())
		{
			// replacement is cached, can immediately upload to host GPU
			*alpha_minmax = it->second.alpha_minmax;
			return CreateReplacementTexture(it->second, mipmap);
		}
	}

	// load asynchronously?
	if (GSConfig.LoadTextureReplacementsAsync)
	{
		// replacement will be injected into the TC later on
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
#ifdef ORBIS_VULKAN
		orbis_r.queued.fetch_add(1, std::memory_order_relaxed);
#endif
		QueueAsyncReplacementTextureLoad(name, fnit->second, mipmap, false);

		*pending = true;
		return nullptr;
	}
	else
	{
		// synchronous load
		std::optional<ReplacementTexture> replacement(LoadReplacementTexture(name, fnit->second, !mipmap));
		if (!replacement.has_value())
			return nullptr;

		// insert into cache
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
#ifdef ORBIS_VULKAN
		const auto cit = s_replacement_texture_cache.emplace(name, std::move(replacement.value())).first;
		const ReplacementTexture& rtex = cit->second;
		s_orbis_cache_bytes += OrbisReplacementBytes(rtex);
		*alpha_minmax = rtex.alpha_minmax;
		GSTexture* const orbis_tex = CreateReplacementTexture(rtex, mipmap);
		if (s_orbis_cache_bytes > ORBIS_CACHE_BUDGET) // vk-285-112: on the GPU now; keep no copy past the budget
		{
			s_orbis_cache_bytes -= OrbisReplacementBytes(cit->second);
			s_replacement_texture_cache.erase(cit);
		}
		return orbis_tex;
#else
		const ReplacementTexture& rtex = s_replacement_texture_cache.emplace(name, std::move(replacement.value())).first->second;

		// and upload to gpu
		*alpha_minmax = rtex.alpha_minmax;
		return CreateReplacementTexture(rtex, mipmap);
#endif
	}
}

template <GSTexture::Format format>
std::pair<u8, u8> GSTextureReplacements::GetBCAlphaMinMax(ReplacementTexture& rtex)
{
	constexpr u32 BC_BLOCK_SIZE = 4;
	constexpr u32 BC_BLOCK_BYTES = (format == GSTexture::Format::BC1) ? 8 : 16;

	const u32 blocks_wide = (rtex.width + (BC_BLOCK_SIZE - 1)) / BC_BLOCK_SIZE;
	const u32 blocks_high = (rtex.height + (BC_BLOCK_SIZE - 1)) / BC_BLOCK_SIZE;

	GSVector4i minc = GSVector4i::xffffffff();
	GSVector4i maxc = GSVector4i::zero();

	for (u32 y = 0; y < blocks_high; y++)
	{
		const u8* block_in = rtex.data.data() + y * rtex.pitch;
		alignas(16) u8 block_pixels_out[BC_BLOCK_SIZE * BC_BLOCK_SIZE * sizeof(u32)];

		for (u32 x = 0; x < blocks_wide; x++, block_in += BC_BLOCK_BYTES)
		{
			switch (format)
			{
				case GSTexture::Format::BC1:
					DecompressBlockBC1(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;
				case GSTexture::Format::BC2:
					DecompressBlockBC2(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;
				case GSTexture::Format::BC3:
					DecompressBlockBC3(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;

				case GSTexture::Format::BC7:
					bc7decomp::unpack_bc7(block_in, reinterpret_cast<bc7decomp::color_rgba*>(block_pixels_out));
					break;
			}

			const u8* out_ptr = block_pixels_out;
			for (u32 i = 0; i < ((BC_BLOCK_SIZE * BC_BLOCK_SIZE * sizeof(u32)) / sizeof(GSVector4i)); i++)
			{
				const GSVector4i v = GSVector4i::load<true>(out_ptr);
				out_ptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}
		}
	}

	return std::make_pair<u8, u8>(static_cast<u8>(minc.minv_u32() >> 24), static_cast<u8>(maxc.maxv_u32() >> 24));
}

void GSTextureReplacements::SetReplacementTextureAlphaMinMax(ReplacementTexture& rtex)
{
	switch (rtex.format)
	{
		case GSTexture::Format::BC1:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC1>(rtex);
			break;

		case GSTexture::Format::BC2:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC2>(rtex);
			break;

		case GSTexture::Format::BC3:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC3>(rtex);
			break;

		case GSTexture::Format::BC7:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC7>(rtex);
			break;

		default:
			pxAssert(rtex.format == GSTexture::Format::Color);
			rtex.alpha_minmax = GSGetRGBA8AlphaMinMax(rtex.data.data(), rtex.width, rtex.height, rtex.pitch);
			break;
	}
}

std::optional<GSTextureReplacements::ReplacementTexture> GSTextureReplacements::LoadReplacementTexture(const TextureName& name, const std::string& filename, bool only_base_image)
{
	ReplacementTextureLoader loader = GetLoader(filename);
	if (!loader)
		return std::nullopt;

	ReplacementTexture rtex;
#ifdef ORBIS_VULKAN
	const auto orbis_t0 = std::chrono::steady_clock::now();
#endif
	if (!loader(filename.c_str(), &rtex, only_base_image))
	{
		Console.Warning("Failed to load replacement texture %s", filename.c_str());
#ifdef ORBIS_VULKAN
		if (s_orbis_texrep.load_failed.fetch_add(1) < 10)
			OrbisDeferredPrintf("[texrep] failed to load %s\n", filename.c_str());
#endif
		return std::nullopt;
	}
#ifdef ORBIS_VULKAN
	s_orbis_texrep.load_us.fetch_add(static_cast<u64>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - orbis_t0).count()));
	s_orbis_texrep.load_pixels.fetch_add(static_cast<u64>(rtex.width) * rtex.height);
	if (s_orbis_texrep.loaded.fetch_add(1) < 5)
		OrbisDeferredPrintf("[texrep] loaded %s: %ux%u\n", filename.c_str(), rtex.width, rtex.height);
#endif

	SetReplacementTextureAlphaMinMax(rtex);

	return rtex;
}

void GSTextureReplacements::QueueAsyncReplacementTextureLoad(const TextureName& name, const std::string& filename, bool mipmap, bool cache_only)
{
	// check the pending list, so we don't queue it up multiple times
	auto it = s_pending_async_load_textures.find(name);
	if (it != s_pending_async_load_textures.end())
	{
		// remove from queue if it's cache-only, so we bump it to the front of the work items
		if (!cache_only && it->second)
		{
			s_pending_async_load_textures.erase(it);
		}
		else
		{
			it->second &= cache_only;
			return;
		}
	}

	s_pending_async_load_textures.emplace(name, cache_only);
	QueueWorkerThreadItem([name, filename, mipmap]() {
		// actually load the file, this is what will take the time
		std::optional<ReplacementTexture> replacement(LoadReplacementTexture(name, filename, !mipmap));

		// check the pending set, there's a race here if we disable replacements while loading otherwise
		// also check the full replacement list, if async loading is off, it might already be in there
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		auto it = s_pending_async_load_textures.find(name);
		if (it == s_pending_async_load_textures.end() ||
			s_replacement_texture_cache.find(name) != s_replacement_texture_cache.end())
		{
			if (it != s_pending_async_load_textures.end())
				s_pending_async_load_textures.erase(it);

			return;
		}

		// insert into the cache and queue for later injection
		if (replacement.has_value())
		{
#ifdef ORBIS_VULKAN
			s_orbis_cache_bytes += OrbisReplacementBytes(replacement.value());
#endif
			s_replacement_texture_cache.emplace(name, std::move(replacement.value()));
			s_async_loaded_textures.emplace_back(name, mipmap);
		}
		else
		{
			// loading failed, so clear it from the pending list
			s_pending_async_load_textures.erase(name);
		}
	}, !cache_only);
}

void GSTextureReplacements::PrecacheReplacementTextures()
{
#ifdef ORBIS_VULKAN
	// vk-285-112: loading a whole pack into memory up front doesn't fit the PS5 app's heap (see
	// ORBIS_CACHE_BUDGET); replacements load when the game first uses them.
	OrbisDeferredPrintf("[texrep] preloading every replacement is off on the PS5; they load as the game uses them\n");
	return;
#endif
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);

	// predict whether the requests will come with mipmaps
	// TODO: This will be wrong for hw mipmap games like Jak.
	const bool mipmap = GSConfig.HWMipmap || GSConfig.TriFilter == TriFiltering::Forced;

	// pretty simple, just go through the filenames and if any aren't cached, cache them
	for (const auto& it : s_replacement_texture_filenames)
	{
		if (s_replacement_texture_cache.find(it.first) != s_replacement_texture_cache.end())
			continue;

		// precaching always goes async.. for now
		QueueAsyncReplacementTextureLoad(it.first, it.second, mipmap, true);
	}
}

void GSTextureReplacements::ClearReplacementTextures()
{
	s_replacement_texture_filenames.clear();
	s_replacement_textures_without_clut_hash.clear();
#ifdef ORBIS_VULKAN
	OrbisPakClose(); // a load still running keeps its own reference
#endif

	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	s_replacement_texture_cache.clear();
	s_pending_async_load_textures.clear();
	s_async_loaded_textures.clear();
#ifdef ORBIS_VULKAN
	s_orbis_cache_bytes = 0;
#endif
}

GSTexture* GSTextureReplacements::CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap)
{
	// can't use generated mipmaps with compressed formats, because they can't be rendered to
	// in the future I guess we could decompress the dds and generate them... but there's no reason that modders can't generate mips in dds
	if (mipmap && GSTexture::IsCompressedFormat(rtex.format) && rtex.mips.empty())
	{
		static bool log_once = false;
		if (!log_once)
		{
			Console.Warning("Disabling autogenerated mipmaps on one or more compressed replacement textures.");
			Host::AddIconOSDMessage("DisablingReplacementAutoGeneratedMipmap", ICON_FA_CIRCLE_EXCLAMATION,
				TRANSLATE_SV("GS", "Disabling autogenerated mipmaps on one or more compressed replacement textures. "
								   "Please generate mipmaps when compressing your textures."),
				Host::OSD_WARNING_DURATION);
			log_once = true;
		}

		mipmap = false;
	}

	GSTexture* tex = g_gs_device->CreateTexture(rtex.width, rtex.height, static_cast<int>(rtex.mips.size()) + 1, rtex.format);
#ifdef ORBIS_VULKAN
	if (!tex)
	{
		if (s_orbis_texrep.create_failed.fetch_add(1) < 10)
			OrbisDeferredPrintf("[texrep] no GPU texture for a %ux%u replacement\n", rtex.width, rtex.height);
	}
	else
		s_orbis_texrep.created.fetch_add(1, std::memory_order_relaxed);
#endif
	if (!tex)
		return nullptr;

	// upload base level
	tex->Update(GSVector4i(0, 0, rtex.width, rtex.height), rtex.data.data(), rtex.pitch);

	// and the mips if they're present in the replacement texture
	if (!rtex.mips.empty())
	{
		for (u32 i = 0; i < static_cast<u32>(rtex.mips.size()); i++)
		{
			const ReplacementTexture::MipData& mip = rtex.mips[i];
			tex->Update(GSVector4i(0, 0, static_cast<int>(mip.width), static_cast<int>(mip.height)), mip.data.data(), mip.pitch, i + 1);
		}
	}

	return tex;
}

void GSTextureReplacements::ProcessAsyncLoadedTextures()
{
	// this holds the lock while doing the upload, but it should be reasonably quick
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	for (const auto& [name, mipmap] : s_async_loaded_textures)
	{
		// no longer pending!
		const auto pit = s_pending_async_load_textures.find(name);
		if (pit != s_pending_async_load_textures.end())
		{
			const bool cache_only = pit->second;
			s_pending_async_load_textures.erase(pit);

			// if we were precaching, don't inject into the TC if we didn't actually get requested
			if (cache_only)
				continue;
		}

		// we should be in the cache now, lock and loaded
		auto it = s_replacement_texture_cache.find(name);
		if (it == s_replacement_texture_cache.end())
			continue;

		// upload and inject into TC
		GSTexture* tex = CreateReplacementTexture(it->second, mipmap);
		if (tex)
			g_texture_cache->InjectHashCacheTexture(HashCacheKeyFromTextureName(name), tex, it->second.alpha_minmax);
#ifdef ORBIS_VULKAN
		// vk-285-112: on the GPU now; keep no copy past the budget
		if (s_orbis_cache_bytes > ORBIS_CACHE_BUDGET)
		{
			s_orbis_cache_bytes -= OrbisReplacementBytes(it->second);
			s_replacement_texture_cache.erase(it);
		}
#endif
	}
	s_async_loaded_textures.clear();
#ifdef ORBIS_VULKAN
	lock.unlock();
	OrbisTexRepReport(false);
#endif
}

void GSTextureReplacements::DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level)
{
	// check if it's been dumped or replaced already
	const TextureName name(CreateTextureName(hash, level));
	{
		std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
		if (s_dumped_textures.find(name) != s_dumped_textures.end() || s_replacement_texture_filenames.find(name) != s_replacement_texture_filenames.end())
			return;

		s_dumped_textures.insert(name);
	}

	// already exists on disk?
	std::string filename(GetDumpFilename(name, level));
	if (filename.empty() || FileSystem::FileExists(filename.c_str()))
		return;

	const std::string_view title(Path::GetFileTitle(filename));
	DevCon.WriteLn("Dumping %ux%u texture '%.*s'.", name.Width(), name.Height(), static_cast<int>(title.size()), title.data());

	// compute width/height
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	const GSVector2i& bs = psm.bs;
	const int tw = region.HasX() ? region.GetWidth() : (1 << TEX0.TW);
	const int th = region.HasY() ? region.GetHeight() : (1 << TEX0.TH);
	const GSVector4i rect(region.GetRect(tw, th));
	const GSVector4i block_rect(rect.ralign<Align_Outside>(bs));
	const int read_width = block_rect.width();
	const int read_height = block_rect.height();
	const u32 pitch = static_cast<u32>(read_width) * sizeof(u32);

	// use per-texture buffer so we can compress the texture asynchronously and not block the GS thread
	// must be 32 byte aligned for ReadTexture().
	u8* buffer = static_cast<u8*>(_aligned_malloc(pitch * static_cast<u32>(read_height), 32));
	psm.rtx(mem, mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM), block_rect, buffer, pitch, TEXA);

	// okay, now we can actually dump it
	const u32 buffer_offset = ((rect.top - block_rect.top) * pitch) + ((rect.left - block_rect.left) * sizeof(u32));
	QueueWorkerThreadItem([filename = std::move(filename), tw, th, pitch, buffer, buffer_offset]() {
		if (!SavePNGImage(filename.c_str(), tw, th, buffer + buffer_offset, pitch))
			Console.Error(fmt::format("Failed to dump texture to '{}'.", filename));
		_aligned_free(buffer);
	}, false);
}

void GSTextureReplacements::ClearDumpedTextureList()
{
	std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
	s_dumped_textures.clear();
}

u32 GSTextureReplacements::GetDumpedTextureCount()
{
	std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
	return static_cast<u32>(s_dumped_textures.size());
}

u32 GSTextureReplacements::GetLoadedTextureCount()
{
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	return static_cast<u32>(s_replacement_texture_cache.size());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Worker Thread
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void GSTextureReplacements::StartWorkerThread()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);

	if (s_worker_thread.joinable())
		return;

	s_worker_thread_running = true;
	s_worker_thread = std::thread(WorkerThreadEntryPoint);
#ifdef ORBIS_VULKAN
	OrbisHelperThreadAdd(s_worker_thread.native_handle()); // vk-285-112
#endif
}

void GSTextureReplacements::StopWorkerThread()
{
	{
		std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
		if (!s_worker_thread.joinable())
			return;

		s_worker_thread_running = false;
		s_worker_thread_cv.notify_one();
	}

#ifdef ORBIS_VULKAN
	OrbisHelperThreadRemove(s_worker_thread.native_handle()); // vk-285-112: before the thread ends
#endif
	s_worker_thread.join();

	// clear out workery-things too
	CancelPendingLoadsAndDumps();
}

void GSTextureReplacements::QueueWorkerThreadItem(std::function<void()> fn, bool high_priority)
{
	pxAssert(s_worker_thread.joinable());

	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	if (!high_priority)
	{
		// Low priority => throw on end.
		s_worker_thread_queue.emplace_back(std::move(fn), false);
	}
	else
	{
		auto iter = s_worker_thread_queue.rbegin();
		for (; iter != s_worker_thread_queue.rend(); ++iter)
		{
			// Found our first high priority item?
			if (iter->second)
			{
				// Insert after here!
				break;
			}
		}

		if (iter != s_worker_thread_queue.rend())
		{
			// Insert after the last high priority item. Remember base() points to the next element.
			s_worker_thread_queue.insert(iter.base(), std::make_pair(std::move(fn), true));
		}
		else
		{
			// All low-priority => insert at beginning.
			s_worker_thread_queue.emplace_front(std::move(fn), true);
		}
	}

	s_worker_thread_cv.notify_one();
}

void GSTextureReplacements::WorkerThreadEntryPoint()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	while (s_worker_thread_running)
	{
		if (s_worker_thread_queue.empty())
		{
			s_worker_thread_cv.wait(lock);
			continue;
		}

		std::function<void()> fn = std::move(s_worker_thread_queue.front().first);
		s_worker_thread_queue.pop_front();
		lock.unlock();
		fn();
		lock.lock();
	}
}

void GSTextureReplacements::SyncWorkerThread()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	if (!s_worker_thread.joinable())
		return;

	// not the most efficient by far, but it only gets called on config changes, so whatever
	for (;;)
	{
		if (s_worker_thread_queue.empty())
			break;

		lock.unlock();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		lock.lock();
	}
}

void GSTextureReplacements::CancelPendingLoadsAndDumps()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	while (!s_worker_thread_queue.empty())
		s_worker_thread_queue.pop_back();
	s_async_loaded_textures.clear();
	s_pending_async_load_textures.clear();
}
