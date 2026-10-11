// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <cstdio>
#include <cstring>

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "CDVD/CDVD.h"

#include "Common.h"
#include "BiosTools.h"
#include "Config.h"

static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
static constexpr u32 DIRENTRY_SIZE = 16;

// --------------------------------------------------------------------------------------
// romdir structure (packing required!)
// --------------------------------------------------------------------------------------
//
#pragma pack(push, 1)

struct romdir
{
	char fileName[10];
	u16 extInfoSize;
	u32 fileSize;
};

#pragma pack(pop)

static_assert(sizeof(romdir) == DIRENTRY_SIZE, "romdir struct not packed to 16 bytes");

u32 BiosVersion;
u32 BiosChecksum;
u32 BiosRegion;
ConfigParam configParams1;
Config2Param configParams2;
bool ParamsRead;
bool NoOSD;
bool AllowParams1;
bool AllowParams2;
std::string BiosDescription;
std::string BiosZone;
std::string BiosSerial;
std::string BiosPath;
BiosDebugInformation CurrentBiosInformation;
std::vector<u8> BiosRom;

void ReadOSDConfigParames()
{
	if (ParamsRead)
		return;

	ParamsRead = true;

	u8 params[16];
	cdvdReadLanguageParams(params);

	configParams1.UC[0] = params[1] & 0x1F; // SPDIF, Screen mode, RGB/Comp, Jap/Eng Switch (Early bios).
	configParams1.ps1drvConfig = params[0]; // PS1 Mode Settings.
	configParams1.version = (params[2] & 0xE0) >> 5; // OSD Ver (Not sure but best guess).
	configParams1.language = params[2] & 0x1F; // Language.
	configParams1.timezoneOffset = params[4] | ((u32)(params[3] & 0x7) << 8);  // Timezone offset in minutes.
	configParams1.timeZoneID = params[6]; // ID for time zone selection
	
	// Region settings for time/date and extended language
	configParams2.UC[1] = ((u32)params[3] & 0x78) << 1; // Daylight Savings, 24hr clock, Date format
	configParams2.daylightSavings = configParams2.UC[1] & 0x10 ? 1 : 0;
	configParams2.timeFormat = configParams2.UC[1] & 0x20 ? 1 : 0;
	configParams2.dateFormat = configParams2.UC[1] & 0x80 ? 2 : (configParams2.UC[1] & 0x40 ? 1 : 0);
	// FIXME: format, version and language are set manually by the bios. Not sure if any game needs them, but it seems to set version to 2 and duplicate the language value.
	configParams2.version = 2;
	configParams2.language = configParams1.language;
}

static bool LoadBiosVersion(std::FILE* fp, u32& version, std::string& description, u32& region, std::string& zone, std::string& serial)
{
	romdir rd;
	for (u32 i = 0; i < 512 * 1024; i++)
	{
		if (std::fread(&rd, sizeof(rd), 1, fp) != 1)
			return false;

		if (std::strncmp(rd.fileName, "RESET", sizeof(rd.fileName)) == 0)
			break; /* found romdir */
	}

	s64 fileOffset = 0;
	s64 fileSize = FileSystem::FSize64(fp);
	bool foundRomVer = false;
	char romver[14 + 1] = {}; // ascii version loaded from disk.
	char extinfo[15 + 1] = {}; // ascii version loaded from disk.

	// ensure it's a null-terminated and not zero-length string
	while (rd.fileName[0] != '\0' && strnlen(rd.fileName, sizeof(rd.fileName)) != sizeof(rd.fileName))
	{
		if (std::strncmp(rd.fileName, "EXTINFO", sizeof(rd.fileName)) == 0)
		{
			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset + 0x10, SEEK_SET) != 0 ||
				std::fread(extinfo, 15, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
			{
				break;
			}
			serial = extinfo;
		}

		if (std::strncmp(rd.fileName, "ROMVER", sizeof(rd.fileName)) == 0)
		{

			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset, SEEK_SET) != 0 ||
				std::fread(romver, 14, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
			{
				break;
			}

			foundRomVer = true;
		}

		if ((rd.fileSize % 0x10) == 0)
			fileOffset += rd.fileSize;
		else
			fileOffset += (rd.fileSize + 0x10) & 0xfffffff0;

		if (std::fread(&rd, sizeof(rd), 1, fp) != 1)
			break;
	}

	fileOffset -= ((rd.fileSize + 0x10) & 0xfffffff0) - rd.fileSize;

	if (foundRomVer)
	{
		switch (romver[4])
		{
			// clang-format off
			case 'J': zone = "Japan";  region = 0;  break;
			case 'A': zone = "USA";    region = 1;  break;
			case 'E': zone = "Europe"; region = 2;  break;
			// case 'E': zone = "Oceania";region = 3;  break; // Not implemented
			case 'H': zone = "Asia";   region = 4;  break;
			// case 'E': zone = "Russia"; region = 3;  break; // Not implemented
			case 'C': zone = "China";  region = 6;  break;
			// case 'A': zone = "Mexico"; region = 7;  break; // Not implemented
			case 'T': zone = (romver[5]=='Z') ? "COH-H" : "T10K";   region = 8;  break;
			case 'X': zone = "Test";   region = 9;  break;
			case 'P': zone = "Free";   region = 10; break;
			// clang-format on
			default:
				zone.clear();
				zone += romver[4];
				region = 0;
				break;
		}
		// TODO: some regions can be detected only from rom1
		/* switch (rom1:DVDID[4])
		{
			// clang-format off
			case 'O': zone = "Oceania";region = 3;  break;
			case 'R': zone = "Russia"; region = 5;  break;
			case 'M': zone = "Mexico"; region = 7;  break;
			// clang-format on
		} */

		char vermaj[3] = {romver[0], romver[1], 0};
		char vermin[3] = {romver[2], romver[3], 0};
		description = StringUtil::StdStringFromFormat("%-7s v%s.%s(%c%c/%c%c/%c%c%c%c)  %s %s",
			zone.c_str(),
			vermaj, vermin,
			romver[12], romver[13], // day
			romver[10], romver[11], // month
			romver[6], romver[7], romver[8], romver[9], // year!
			(romver[5] == 'C') ? "Console" : (romver[5] == 'D') ? "Devel" :
																  "",
			serial.c_str());

		version = static_cast<u32>(strtol(vermaj, (char**)NULL, 0) << 8);
		version |= strtol(vermin, (char**)NULL, 0);

		Console.WriteLn("BIOS Found: %s", description.c_str());
	}
	else
		return false;

	if (fileSize < (int)fileOffset)
	{
		description += StringUtil::StdStringFromFormat(" %d%%", (((int)fileSize * 100) / (int)fileOffset));
		// we force users to have correct bioses,
		// not that lame scph10000 of 513KB ;-)
	}

	return true;
}

static void ChecksumIt(u32& result, u32 offset, u32 size)
{
	const u8* srcdata = &BiosRom[offset];
	pxAssume((size & 3) == 0);
	for (size_t i = 0; i < size / 4; ++i)
		result ^= reinterpret_cast<const u32*>(srcdata)[i];
}

// Attempts to load a BIOS rom sub-component, by trying multiple combinations of base
// filename and extension.  The bios specified in the user's configuration is used as
// the base.
//
// Parameters:
//   ext - extension of the sub-component to load. Valid options are rom1 and rom2.
//
static void LoadExtraRom(const char* ext, u32 offset, u32 size)
{
	// Try first a basic extension concatenation (normally results in something like name.bin.rom1)
	std::string Bios1(StringUtil::StdStringFromFormat("%s.%s", BiosPath.c_str(), ext));

	s64 filesize;
	if ((filesize = FileSystem::GetPathFileSize(Bios1.c_str())) <= 0)
	{
		// Try the name properly extensioned next (name.rom1)
		Bios1 = Path::ReplaceExtension(BiosPath, ext);
		if ((filesize = FileSystem::GetPathFileSize(Bios1.c_str())) <= 0)
		{
			Console.WriteLn(Color_Gray, "BIOS %s module not found, skipping...", ext);
			return;
		}
	}

	BiosRom.resize(offset + size);

	auto fp = FileSystem::OpenManagedCFileTryIgnoreCase(Bios1.c_str(), "rb");
	if (!fp || std::fread(&BiosRom[offset], static_cast<size_t>(std::min<s64>(size, filesize)), 1, fp.get()) != 1)
	{
		Console.Warning("BIOS Warning: %s could not be read (permission denied?)", ext);
		return;
	}
	// Checksum for ROM1, ROM2?  Rama says no, Gigaherz says yes.  I'm not sure either way.  --air
	//ChecksumIt( BiosChecksum, dest );
}

static void LoadIrx(const std::string& filename, u8* dest, size_t maxSize)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "rb");
	if (fp)
	{
		const s64 filesize = FileSystem::FSize64(fp.get());
		const s64 readSize = std::min(filesize, static_cast<s64>(maxSize));
		if (std::fread(dest, readSize, 1, fp.get()) == 1)
			return;
	}

	Console.Warning("IRX Warning: %s could not be read", filename.c_str());
	return;
}

#ifdef __PROSPERO__
// PS5 port (vk-285-115, AI-assisted): where else a BIOS may be, and what was there when none is found. 1.50's logs:
// 689 starts on 146 consoles failed with "PCSX2 requires a PlayStation 2 BIOS" (26 consoles never got past it); in
// 494 of the 591 reports the BIOS folder had no file at all, and the log didn't say what it held. The BIOS folder is
// searched as before; then one folder down in it (a BIOS pack unzipped into its own folder), the top folder
// /data/PCSX2, a folder named bios in another case ("BIOS": /data tells cases apart), and USB drives (their root,
// bios/, PS5SX2/ and PS5SX2/bios/). Each file of a BIOS's size is checked the way the BIOS folder's are.
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <algorithm>
#include <vector>
#include "fmt/format.h"

namespace
{
std::vector<std::string> OrbisNames(const std::string& dir)
{
	std::vector<std::string> names;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			if (e->d_name[0] != '.' && e->d_name[0] != '$')
				names.emplace_back(e->d_name);
		}
		closedir(d);
	}
	std::sort(names.begin(), names.end());
	return names;
}

bool OrbisIsDir(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A BIOS among the files directly in `dir`. `seen` collects what was looked at, for the log.
std::string OrbisBiosIn(const std::string& dir, std::string* seen)
{
	u32 version, region;
	std::string description, zone;
	for (const std::string& name : OrbisNames(dir))
	{
		const std::string path = dir + "/" + name;
		struct stat st = {};
		if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (st.st_size < MIN_BIOS_SIZE || st.st_size > MAX_BIOS_SIZE)
			continue;
		if (IsBIOS(path.c_str(), version, description, region, zone))
		{
			Console.WriteLn("Using BIOS '%s' (%s %s), found outside the BIOS folder", path.c_str(), description.c_str(), zone.c_str());
			return path;
		}
		if (seen->size() < 600)
			*seen += fmt::format("{}{} ({} bytes, not a PS2 BIOS)", seen->empty() ? "" : "; ", path, static_cast<long long>(st.st_size));
	}
	return std::string();
}

std::string OrbisFindBiosElsewhere(std::string* seen)
{
	std::vector<std::string> dirs;
	const std::string& bios = EmuFolders::Bios;
	// vk-285-134 (AI-assisted): the BIOS folder's folders three levels down (64 at most), not one: build 130's logs had
	// BIOS packs unpacked as folders of folders ("ps2 bios usa/", "SCPH-30003_BIOS_V3_UK_120_(PAL)/", ...).
	{
		std::vector<std::pair<std::string, int>> todo = {{bios, 0}};
		for (size_t i = 0; i < todo.size() && dirs.size() < 64; i++)
		{
			const auto [dir, depth] = todo[i];
			if (depth >= 3)
				continue;
			for (const std::string& name : OrbisNames(dir))
			{
				if (dirs.size() >= 64)
					break;
				if (OrbisIsDir(dir + "/" + name))
				{
					dirs.push_back(dir + "/" + name);
					todo.emplace_back(dir + "/" + name, depth + 1);
				}
			}
		}
	}
	const std::string top = "/data/PCSX2";
	if (bios != top)
		dirs.push_back(top);
	for (const std::string& name : OrbisNames(top))
	{
		if (strcasecmp(name.c_str(), "bios") == 0 && top + "/" + name != bios && OrbisIsDir(top + "/" + name))
			dirs.push_back(top + "/" + name);
	}
	// 2026-10-08 (AI-assisted): and the PS5's extended storage / M.2 drives (/mnt/ext0, /mnt/ext1), as the USB drives.
	for (int i = 0; i < 10; i++)
	{
		const std::string root = i < 8 ? fmt::format("/mnt/usb{}", i) : fmt::format("/mnt/ext{}", i - 8);
		const std::vector<std::string> names = OrbisNames(root);
		if (names.empty())
			continue;
		dirs.push_back(root);
		for (const std::string& name : names)
		{
			const bool is_bios = strcasecmp(name.c_str(), "bios") == 0;
			// vk-285-118 (AI-assisted): PCSX2/bios too, a drive laid out like /data/PCSX2 (as its games/ and textures/).
			const bool is_ps5sx2 = strcasecmp(name.c_str(), "ps5sx2") == 0 || strcasecmp(name.c_str(), "pcsx2") == 0;
			if (!(is_bios || is_ps5sx2) || !OrbisIsDir(root + "/" + name))
				continue;
			dirs.push_back(root + "/" + name);
			if (is_ps5sx2)
			{
				for (const std::string& sub : OrbisNames(root + "/" + name))
				{
					if (strcasecmp(sub.c_str(), "bios") == 0 && OrbisIsDir(root + "/" + name + "/" + sub))
						dirs.push_back(root + "/" + name + "/" + sub);
				}
			}
		}
	}
	for (const std::string& dir : dirs)
	{
		std::string path = OrbisBiosIn(dir, seen);
		if (!path.empty())
			return path;
	}
	return std::string();
}

// What the BIOS folder holds (names and sizes), for the log.
std::string OrbisDescribeFolder(const std::string& dir)
{
	if (!OrbisIsDir(dir))
		return "the folder isn't there";
	std::string out;
	int n = 0;
	for (const std::string& name : OrbisNames(dir))
	{
		if (++n > 20)
		{
			out += "; ...";
			break;
		}
		struct stat st = {};
		const std::string path = dir + "/" + name;
		const bool ok = stat(path.c_str(), &st) == 0;
		out += fmt::format("{}{}{}", out.empty() ? "" : "; ", name,
			!ok ? " (unreadable)" : S_ISDIR(st.st_mode) ? "/ (a folder)" : fmt::format(" ({} bytes)", static_cast<long long>(st.st_size)));
	}
	return out.empty() ? "empty" : out;
}
} // namespace
#endif

static std::string FindBiosImage()
{
	Console.WriteLn("Searching for a BIOS image in '%s'...", EmuFolders::Bios.c_str());

	FileSystem::FindResultsArray results;
	if (!FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
#ifdef __PROSPERO__
		results.clear(); // vk-285-115: nothing there; look elsewhere below
#else
		return std::string();
#endif

	u32 version, region;
	std::string description, zone;
	for (const FILESYSTEM_FIND_DATA& fd : results)
	{
		if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
			continue;

		if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
		{
			Console.WriteLn("Using BIOS '%s' (%s %s)", fd.FileName.c_str(), description.c_str(), zone.c_str());
			return std::move(fd.FileName);
		}
	}

#ifdef __PROSPERO__
	// vk-285-115: the other places, then what the BIOS folder holds (see OrbisFindBiosElsewhere).
	{
		std::string seen;
		std::string path = OrbisFindBiosElsewhere(&seen);
		if (!path.empty())
			return path;
		Console.Error("Unable to auto locate a BIOS image. In '%s': %s", EmuFolders::Bios.c_str(),
			OrbisDescribeFolder(EmuFolders::Bios).c_str());
		if (!seen.empty())
			Console.Error("Files of a BIOS's size that aren't a PS2 BIOS: %s", seen.c_str());
		return std::string();
	}
#endif
	Console.Error("Unable to auto locate a BIOS image");
	return std::string();
}

bool IsBIOS(const char* filename, u32& version, std::string& description, u32& region, std::string& zone)
{
	std::string serial;
	const auto fp = FileSystem::OpenManagedCFile(filename, "rb");
	if (!fp)
		return false;

	// FPS2BIOS is smaller and of variable size
	//if (inway.Length() < 512*1024) return false;
	return LoadBiosVersion(fp.get(), version, description, region, zone, serial);
}

bool IsBIOSAvailable(const std::string& full_path)
{
	// We can't use EmuConfig here since it may not be loaded yet.
	if (!full_path.empty() && FileSystem::FileExists(full_path.c_str()))
		return true;

	// No bios configured or the configured name is missing, check for one in the BIOS directory.
	const std::string auto_path(FindBiosImage());
	return !auto_path.empty() && FileSystem::FileExists(auto_path.c_str());
}

// Loads the configured bios rom file into PS2 memory.  PS2 memory must be allocated prior to
// this method being called.
//
// Remarks:
//   This function does not fail if rom1 or rom2 files are missing, since none are
//   explicitly required for most emulation tasks.
//
// Exceptions:
//   BadStream - Thrown if the primary bios file (usually .bin) is not found, corrupted, etc.
//
bool LoadBIOS()
{
	pxAssertMsg(eeMem->ROM, "PS2 system memory has not been initialized yet.");

	std::string path = EmuConfig.FullpathToBios();
	if (path.empty() || !FileSystem::FileExists(path.c_str()))
	{
		if (!path.empty())
		{
			Console.Warning("Configured BIOS '%s' does not exist, trying to find an alternative.",
				EmuConfig.BaseFilenames.Bios.c_str());
		}

		path = FindBiosImage();
		if (path.empty())
			return false;
	}

	auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
	if (!fp)
		return false;

	const s64 filesize = FileSystem::FSize64(fp.get());
	if (filesize <= 0)
		return false;

	LoadBiosVersion(fp.get(), BiosVersion, BiosDescription, BiosRegion, BiosZone, BiosSerial);

	BiosRom.resize(Ps2MemSize::Rom);

	if (FileSystem::FSeek64(fp.get(), 0, SEEK_SET) ||
		std::fread(BiosRom.data(), static_cast<size_t>(std::min<s64>(Ps2MemSize::Rom, filesize)), 1, fp.get()) != 1)
	{
		return false;
	}

	// If file is less than 2mb it doesn't have an OSD (Devel consoles)
	// So skip HLEing OSDSys Param stuff
	if (filesize < 2465792)
		NoOSD = true;
	else
		NoOSD = false;

	BiosChecksum = 0;
	ChecksumIt(BiosChecksum, 0, Ps2MemSize::Rom);
	BiosPath = std::move(path);

	//injectIRX("host.irx");	//not fully tested; still buggy

	LoadExtraRom("rom1", Ps2MemSize::Rom, Ps2MemSize::Rom1);
	LoadExtraRom("rom2", Ps2MemSize::Rom + Ps2MemSize::Rom1, Ps2MemSize::Rom2);
	return true;
}

void CopyBIOSToMemory()
{
	if (BiosRom.size() >= Ps2MemSize::Rom)
	{
		std::memcpy(eeMem->ROM, BiosRom.data(), sizeof(eeMem->ROM));
		if (BiosRom.size() >= (Ps2MemSize::Rom + Ps2MemSize::Rom1))
		{
			std::memcpy(eeMem->ROM1, BiosRom.data() + Ps2MemSize::Rom, sizeof(eeMem->ROM1));
			if (BiosRom.size() >= (Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2))
				std::memcpy(eeMem->ROM2, BiosRom.data() + Ps2MemSize::Rom + Ps2MemSize::Rom1, sizeof(eeMem->ROM2));
		}
	}

	if (EmuConfig.CurrentIRX.length() > 3)
		LoadIrx(EmuConfig.CurrentIRX, &eeMem->ROM[0x3C0000], sizeof(eeMem->ROM) - 0x3C0000);

	CurrentBiosInformation.eeThreadListAddr = 0;
}
