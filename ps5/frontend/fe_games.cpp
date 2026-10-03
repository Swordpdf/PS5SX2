// PS5 port frontend: the game list (see fe_games.h).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_games.h"

#include "libchdr/chd.h" // vk-285-108: CHD images (3rdparty/libchdr)
#include "lz4.h"          // vk-285-113: ZSO images (ps5/third_party/lz4)
#include "zlib.h"         // vk-285-113: CSO images (ps5/third_party/zlib, inflate only)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace fe
{
namespace
{
// `name` ends in `ext` (".iso", lower case), in any case, and isn't hidden.
bool HasExtension(const char* name, const char* ext)
{
	const size_t n = std::strlen(name), e = std::strlen(ext);
	if (n <= e || name[0] == '.')
		return false;
	for (size_t i = 0; i < e; i++)
		if (std::tolower(static_cast<unsigned char>(name[n - e + i])) != ext[i])
			return false;
	return true;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

bool ReadAt(int fd, uint64_t offset, void* buf, size_t len)
{
	uint8_t* p = static_cast<uint8_t*>(buf);
	while (len)
	{
		const ssize_t n = pread(fd, p, len, static_cast<off_t>(offset));
		if (n <= 0)
			return false;
		p += n;
		offset += static_cast<uint64_t>(n);
		len -= static_cast<size_t>(n);
	}
	return true;
}

uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

const char* const kRegions[] = {"USA", "Europe", "Japan", "Korea", "Asia", "World", "Australia", "France", "Germany",
	"Italy", "Spain", "UK", "Canada", "Brazil", "Russia", "China", "Taiwan", "Netherlands", "Sweden"};
} // namespace

namespace
{
// A disc image's 2048-byte data sectors, read by number.
class SectorReader
{
public:
	virtual ~SectorReader() = default;
	virtual bool Read(uint32_t lba, void* buf, size_t len) = 0; // `len` bytes from the start of sector `lba`
};

class IsoSectors final : public SectorReader
{
public:
	explicit IsoSectors(int fd)
		: m_fd(fd)
	{
	}
	bool Read(uint32_t lba, void* buf, size_t len) override { return ReadAt(m_fd, static_cast<uint64_t>(lba) * 2048, buf, len); }

private:
	int m_fd;
};

// vk-285-108: a CHD through libchdr. A DVD image (chdman createdvd) holds 2048-byte units; a CD image
// (createcd) 2448-byte frames (2352 of sector, 96 of subcode). In a frame the data starts 24 bytes in for a
// raw mode 2 track (the PS2's CDs from a .cue/.bin), 16 for raw mode 1, and at 0 for a track chdman got as
// 2048-byte sectors (vk-285-109: createcd from an .iso, which people do with DVD games too), 8 for 2336-byte
// mode 2. Read() stays inside one sector, so a CD sector never spans two frames.
class ChdSectors final : public SectorReader
{
public:
	~ChdSectors() override
	{
		if (m_chd)
			chd_close(m_chd);
	}

	bool Open(const std::string& path)
	{
		const chd_error err = chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &m_chd);
		if (err != CHDERR_NONE)
		{
			m_chd = nullptr; // a CHD that needs its parent reads as no serial (PCSX2 finds the parent itself)
			m_what = std::string("libchdr can't open it: ") + chd_error_string(err);
			return false;
		}
		const chd_header* const h = chd_get_header(m_chd);
		if (!h || h->hunkbytes == 0 || h->unitbytes == 0)
		{
			m_what = "no hunk or unit size in its header";
			return false;
		}
		m_unit = h->unitbytes;
		m_hunk_bytes = h->hunkbytes;
		m_buf.resize(m_hunk_bytes);
		char meta[256] = {};
		uint32_t len = 0;
		if (chd_get_metadata(m_chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, nullptr, nullptr) == CHDERR_NONE ||
			chd_get_metadata(m_chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1, &len, nullptr, nullptr) == CHDERR_NONE)
			m_track = meta;
		char codecs[64] = {};
		for (int i = 0; i < 4; i++)
		{
			const uint32_t c = h->compression[i];
			if (c == 0)
				continue;
			// v5: a four-letter code (zlib, lzma, cdlz...); v1-v4: a number (1 zlib, 2 zlib+, 3 A/V).
			char tag[16];
			const bool fourcc = ((c >> 24) & 0xff) >= 0x20 && ((c >> 16) & 0xff) >= 0x20 && ((c >> 8) & 0xff) >= 0x20 && (c & 0xff) >= 0x20;
			if (fourcc)
				std::snprintf(tag, sizeof(tag), "%c%c%c%c", static_cast<char>(c >> 24), static_cast<char>(c >> 16), static_cast<char>(c >> 8),
					static_cast<char>(c));
			else
				std::snprintf(tag, sizeof(tag), "#%u", c);
			std::snprintf(codecs + std::strlen(codecs), sizeof(codecs) - std::strlen(codecs), "%s%s", codecs[0] ? "," : "", tag);
		}
		char what[512];
		std::snprintf(what, sizeof(what), "v%u, %u-byte units, %u-byte hunks, %llu MB of data, codecs %s%s%s", h->version,
			h->unitbytes, h->hunkbytes, static_cast<unsigned long long>(h->logicalbytes >> 20), codecs[0] ? codecs : "none",
			m_track.empty() ? "" : ", track ", m_track.c_str());
		m_what = what;
		if (m_unit != 2352 && m_unit != 2448)
			return true; // 2048-byte sectors back to back
		// A CD: the data offset that puts the volume descriptor ("\1CD001") at sector 16, the track type's first.
		// The TYPE field ("TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE ..."; not PGTYPE, the pregap's type).
		std::string type;
		const size_t t = m_track.find(" TYPE:");
		if (t != std::string::npos)
			type = m_track.substr(t + 6, m_track.find(' ', t + 6) - (t + 6));
		uint32_t first = 24;
		if (type == "MODE1_RAW")
			first = 16;
		else if (type == "MODE1" || type == "MODE2_FORM1")
			first = 0;
		else if (type == "MODE2" || type == "MODE2_FORM_MIX")
			first = 8;
		for (const uint32_t offset : {first, 24u, 16u, 0u, 8u})
		{
			m_offset = offset;
			uint8_t pvd[6];
			if (Read(16, pvd, sizeof(pvd)) && pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0)
			{
				m_what += ", data " + std::to_string(offset) + " bytes into each frame";
				return true;
			}
		}
		m_what += ", no ISO 9660 volume descriptor at sector 16 at any data offset";
		m_offset = first;
		return true;
	}

	// What the image is, for the log (vk-285-109): the header, the codecs, the first track, the data offset.
	const std::string& What() const { return m_what; }

	bool Read(uint32_t lba, void* buf, size_t len) override
	{
		if (len > 2048)
		{
			// Sector by sector (a CD's sectors aren't contiguous).
			uint8_t* p = static_cast<uint8_t*>(buf);
			for (; len; lba++)
			{
				const size_t n = std::min<size_t>(len, 2048);
				if (!Read(lba, p, n))
					return false;
				p += n;
				len -= n;
			}
			return true;
		}
		const bool raw = m_unit == 2352 || m_unit == 2448;
		uint64_t pos = raw ? static_cast<uint64_t>(lba) * m_unit + m_offset : static_cast<uint64_t>(lba) * 2048;
		uint8_t* p = static_cast<uint8_t*>(buf);
		while (len)
		{
			const uint64_t hunk = pos / m_hunk_bytes;
			const size_t in = static_cast<size_t>(pos % m_hunk_bytes);
			if (hunk > 0xffffffffull)
				return false;
			if (hunk != m_cached)
			{
				if (chd_read(m_chd, static_cast<uint32_t>(hunk), m_buf.data()) != CHDERR_NONE)
					return false;
				m_cached = hunk;
			}
			const size_t n = std::min<size_t>(len, m_hunk_bytes - in);
			std::memcpy(p, m_buf.data() + in, n);
			p += n;
			pos += n;
			len -= n;
		}
		return true;
	}

private:
	chd_file* m_chd = nullptr;
	uint32_t m_unit = 0;
	uint32_t m_hunk_bytes = 0;
	uint32_t m_offset = 0;
	uint64_t m_cached = ~0ull; // the hunk in m_buf
	std::vector<uint8_t> m_buf;
	std::string m_track; // the first track's metadata (CD images)
	std::string m_what;
};

// vk-285-113: a CSO (zlib blocks) or ZSO (LZ4 blocks) image. A 24-byte header ("CISO" or "ZISO", header size,
// total bytes, block size, version, index shift), then an index of one 32-bit offset per block and one more (the
// end): offset << shift, bit 31 set when the block is stored as it is. The blocks follow, each padded to the shift.
// PCSX2's own CsoFileReader plays the game; this reads the volume descriptor and SYSTEM.CNF for the shelf.
class CsoSectors final : public SectorReader
{
public:
	~CsoSectors() override
	{
		if (m_fd >= 0)
			close(m_fd);
	}

	bool Open(const std::string& path)
	{
		m_fd = open(path.c_str(), O_RDONLY);
		if (m_fd < 0)
			return false;
		uint8_t h[24];
		if (!ReadAt(m_fd, 0, h, sizeof(h)))
			return false;
		m_lz4 = std::memcmp(h, "ZISO", 4) == 0;
		if (!m_lz4 && std::memcmp(h, "CISO", 4) != 0)
			return false;
		m_total = Le32(h + 8) | (static_cast<uint64_t>(Le32(h + 12)) << 32);
		m_frame = Le32(h + 16);
		m_shift = h[21];
		if (m_frame < 2048 || m_frame > (1u << 20) || (m_frame & (m_frame - 1)) != 0 || m_shift > 8 || m_total == 0)
			return false;
		m_frames = static_cast<uint32_t>((m_total + m_frame - 1) / m_frame);
		m_out.resize(m_frame);
		m_in.resize(static_cast<size_t>(m_frame) * 2 + 4096);
		return true;
	}

	bool Read(uint32_t lba, void* buf, size_t len) override
	{
		uint64_t pos = static_cast<uint64_t>(lba) * 2048;
		uint8_t* p = static_cast<uint8_t*>(buf);
		while (len)
		{
			const uint64_t frame = pos / m_frame;
			const size_t in = static_cast<size_t>(pos % m_frame);
			if (frame >= m_frames || !Load(static_cast<uint32_t>(frame)) || in >= m_out_len)
				return false;
			const size_t n = std::min<size_t>(len, m_out_len - in);
			std::memcpy(p, m_out.data() + in, n);
			p += n;
			pos += n;
			len -= n;
		}
		return true;
	}

private:
	// Frame `index` into m_out (m_out_len bytes: the last frame may be short).
	bool Load(uint32_t index)
	{
		if (index == m_cached)
			return true;
		uint8_t e[8];
		if (!ReadAt(m_fd, 24 + static_cast<uint64_t>(index) * 4, e, sizeof(e)))
			return false;
		const uint32_t a = Le32(e), b = Le32(e + 4);
		const bool stored = (a & 0x80000000u) != 0;
		const uint64_t begin = static_cast<uint64_t>(a & 0x7fffffffu) << m_shift;
		const uint64_t end = static_cast<uint64_t>(b & 0x7fffffffu) << m_shift;
		if (end <= begin || end - begin > m_in.size())
			return false;
		const size_t size = static_cast<size_t>(end - begin);
		const size_t want = static_cast<size_t>(std::min<uint64_t>(m_frame, m_total - static_cast<uint64_t>(index) * m_frame));
		if (!ReadAt(m_fd, begin, m_in.data(), size))
			return false;
		m_cached = ~0u;
		if (stored)
		{
			if (size < want)
				return false;
			std::memcpy(m_out.data(), m_in.data(), want);
			m_out_len = want;
		}
		else if (m_lz4)
		{
			// The block may be followed by padding: the partial call stops at `want` bytes of output.
			const int got = LZ4_decompress_safe_partial(reinterpret_cast<const char*>(m_in.data()), reinterpret_cast<char*>(m_out.data()),
				static_cast<int>(size), static_cast<int>(want), static_cast<int>(m_frame));
			if (got < static_cast<int>(want))
				return false;
			m_out_len = want;
		}
		else
		{
			z_stream z = {};
			if (inflateInit2(&z, -15) != Z_OK)
				return false;
			z.next_in = m_in.data();
			z.avail_in = static_cast<uInt>(size);
			z.next_out = m_out.data();
			z.avail_out = static_cast<uInt>(m_frame);
			const int rc = inflate(&z, Z_FINISH);
			const size_t got = z.total_out;
			inflateEnd(&z);
			if ((rc != Z_STREAM_END && rc != Z_BUF_ERROR) || got < want)
				return false;
			m_out_len = want;
		}
		m_cached = index;
		return true;
	}

	int m_fd = -1;
	bool m_lz4 = false;
	uint64_t m_total = 0; // the image's size when uncompressed
	uint32_t m_frame = 0; // bytes per block
	uint32_t m_frames = 0;
	uint8_t m_shift = 0;
	uint32_t m_cached = ~0u;
	size_t m_out_len = 0;
	std::vector<uint8_t> m_in, m_out;
};

// "SLUS-21351" from SYSTEM.CNF's BOOT2 line, found in the root directory of the ISO 9660 file system.
std::string SerialFromDisc(SectorReader& disc)
{
	std::string serial;
	uint8_t pvd[2048];
	if (!disc.Read(16, pvd, sizeof(pvd)) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
		return serial;
	const uint8_t* root = pvd + 156;
	const uint32_t root_lba = Le32(root + 2);
	const uint32_t root_len = std::min<uint32_t>(Le32(root + 10), 64 * 2048);
	std::vector<uint8_t> dir(root_len);
	if (!root_len || !disc.Read(root_lba, dir.data(), dir.size()))
		return serial;
	for (size_t off = 0; off < dir.size();)
	{
		const uint8_t len = dir[off];
		if (len == 0)
		{
			off = (off / 2048 + 1) * 2048; // records don't cross sectors
			continue;
		}
		if (off + len > dir.size() || len < 34)
			break;
		const uint8_t name_len = dir[off + 32];
		std::string name(reinterpret_cast<const char*>(&dir[off + 33]), std::min<size_t>(name_len, len - 33));
		if (Lower(name).rfind("system.cnf", 0) == 0)
		{
			const uint32_t lba = Le32(&dir[off + 2]);
			const uint32_t size = std::min<uint32_t>(Le32(&dir[off + 10]), 4096);
			std::string cnf(size, '\0');
			if (disc.Read(lba, cnf.data(), size))
			{
				// BOOT2 = cdrom0:\SLUS_213.51;1
				const size_t b = cnf.find("BOOT2");
				const size_t s = cnf.find('\\', b == std::string::npos ? 0 : b);
				if (b != std::string::npos && s != std::string::npos)
				{
					std::string elf;
					for (size_t i = s + 1; i < cnf.size() && cnf[i] != ';' && cnf[i] != '\r' && cnf[i] != '\n'; i++)
						elf += cnf[i];
					// SLUS_213.51 -> SLUS-21351
					std::string out;
					for (char c : elf)
					{
						if (c == '_')
							out += '-';
						else if (c != '.')
							out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
					}
					if (out.size() >= 9 && out.size() <= 12)
						serial = out;
				}
			}
			break;
		}
		off += len;
	}
	return serial;
}

// vk-285-108: a CHD's serial costs its map's decompression (tens of milliseconds for a DVD), so the ones
// found are kept in a file, one "<path>\t<size>\t<mtime>\t<serial>" line each (a later line wins).
std::mutex s_serial_mutex;
std::string s_serial_file;
bool s_serial_loaded = false;
std::unordered_map<std::string, std::string> s_serials; // "<path>\t<size>\t<mtime>" -> serial

std::string SerialKey(const std::string& path)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return {};
	return path + "\t" + std::to_string(static_cast<long long>(st.st_size)) + "\t" +
	       std::to_string(static_cast<long long>(st.st_mtime));
}

void LoadSerialsLocked()
{
	if (s_serial_loaded || s_serial_file.empty())
		return;
	s_serial_loaded = true;
	FILE* f = std::fopen(s_serial_file.c_str(), "rb");
	if (!f)
		return;
	char line[1024];
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
			l.pop_back();
		const size_t tab = l.rfind('\t');
		if (tab != std::string::npos && tab > 0 && tab + 1 < l.size())
			s_serials[l.substr(0, tab)] = l.substr(tab + 1);
	}
	std::fclose(f);
}

std::string ChdSerial(const std::string& path)
{
	const std::string key = SerialKey(path);
	if (!key.empty())
	{
		std::lock_guard<std::mutex> lock(s_serial_mutex);
		LoadSerialsLocked();
		const auto it = s_serials.find(key);
		if (it != s_serials.end())
			return it->second;
	}
	std::string serial;
	if (HasExtension(path.c_str(), ".chd"))
	{
		ChdSectors disc;
		serial = disc.Open(path) ? SerialFromDisc(disc) : std::string();
	}
	else
	{
		CsoSectors disc; // vk-285-113
		serial = disc.Open(path) ? SerialFromDisc(disc) : std::string();
	}
	if (!serial.empty() && !key.empty())
	{
		std::lock_guard<std::mutex> lock(s_serial_mutex);
		s_serials[key] = serial;
		if (!s_serial_file.empty())
		{
			const size_t slash = s_serial_file.rfind('/');
			if (slash != std::string::npos)
				mkdir(s_serial_file.substr(0, slash).c_str(), 0777);
			if (FILE* f = std::fopen(s_serial_file.c_str(), "ab"))
			{
				const std::string line = key + "\t" + serial + "\n";
				std::fwrite(line.data(), 1, line.size(), f);
				std::fclose(f);
			}
		}
	}
	return serial;
}
} // namespace

std::string DescribeImage(const std::string& image_path)
{
	if (!HasExtension(image_path.c_str(), ".chd"))
		return {};
	ChdSectors disc;
	disc.Open(image_path);
	return disc.What();
}

bool IsDiscImageName(const char* name)
{
	return HasExtension(name, ".iso") || HasExtension(name, ".chd") || HasExtension(name, ".cso") || HasExtension(name, ".zso");
}

void SetSerialCacheFile(const std::string& path)
{
	std::lock_guard<std::mutex> lock(s_serial_mutex);
	if (path == s_serial_file)
		return;
	s_serial_file = path;
	s_serial_loaded = false;
	s_serials.clear();
}

std::string ReadSerial(const std::string& image_path)
{
	if (HasExtension(image_path.c_str(), ".chd") || HasExtension(image_path.c_str(), ".cso") || HasExtension(image_path.c_str(), ".zso"))
		return ChdSerial(image_path); // vk-285-113: the compressed formats keep their serials in the cache file
	const int fd = open(image_path.c_str(), O_RDONLY);
	if (fd < 0)
		return {};
	IsoSectors disc(fd);
	const std::string serial = SerialFromDisc(disc);
	close(fd);
	return serial;
}

// Read ISO9660 files without touching the emulator's global CDVD state. (AI-assisted)
bool ReadAchievementExecutable(const std::string& path, std::string& name, std::vector<uint8_t>& bytes)
{
	name.clear();
	bytes.clear();
	auto read = [&](SectorReader& disc) {
		uint8_t pvd[2048];
		if (!disc.Read(16, pvd, sizeof(pvd)) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
			return false;
		const uint32_t root_lba = Le32(pvd + 158), root_size = Le32(pvd + 166);
		auto find = [&](uint32_t directory, uint32_t length, const std::string& wanted,
						uint32_t& lba, uint32_t& size, bool& is_dir) {
			if (!length || length > 1024 * 1024)
				return false;
			std::vector<uint8_t> data(length);
			if (!disc.Read(directory, data.data(), data.size()))
				return false;
			for (size_t off = 0; off < data.size();)
			{
				const unsigned n = data[off];
				if (!n)
				{
					off = (off / 2048 + 1) * 2048;
					continue;
				}
				if (n < 34 || off + n > data.size() || data[off + 32] > n - 33)
					return false;
				std::string entry(reinterpret_cast<const char*>(data.data() + off + 33), data[off + 32]);
				const auto version = entry.find(';');
				if (version != std::string::npos)
					entry.resize(version);
				if (Lower(entry) == Lower(wanted))
				{
					// Multi-extent executables require a different reader; don't hash partial data.
					if (data[off + 25] & 0x80)
						return false;
					lba = Le32(data.data() + off + 2);
					size = Le32(data.data() + off + 10);
					is_dir = (data[off + 25] & 2) != 0;
					return true;
				}
				off += n;
			}
			return false;
		};
		uint32_t lba = 0, size = 0;
		bool directory = false;
		if (!find(root_lba, root_size, "SYSTEM.CNF", lba, size, directory) || directory || !size || size > 4096)
			return false;
		std::string cnf(size, '\0');
		if (!disc.Read(lba, cnf.data(), size))
			return false;
		const auto boot = cnf.find("BOOT2");
		const auto colon = cnf.find(':', boot);
		if (boot == std::string::npos || colon == std::string::npos)
			return false;
		size_t start = colon + 1;
		while (start < cnf.size() && (cnf[start] == '\\' || cnf[start] == '/'))
			++start;
		const auto end = cnf.find_first_of(";\r\n", start);
		std::string executable = Trim(cnf.substr(start, end - start));
		std::replace(executable.begin(), executable.end(), '\\', '/');
		if (executable.empty())
			return false;
		uint32_t dir_lba = root_lba, dir_size = root_size;
		size_t part = 0;
		for (;;)
		{
			const auto slash = executable.find('/', part);
			const auto component = executable.substr(part, slash - part);
			if (component.empty() || component == "." || component == ".." ||
				!find(dir_lba, dir_size, component, lba, size, directory))
				return false;
			if (slash == std::string::npos)
			{
				if (directory || !size)
					return false;
				bytes.resize(std::min<uint32_t>(size, 64 * 1024 * 1024));
				if (!disc.Read(lba, bytes.data(), bytes.size()))
				{
					bytes.clear();
					return false;
				}
				name = component;
				return true;
			}
			if (!directory)
				return false;
			dir_lba = lba;
			dir_size = size;
			part = slash + 1;
		}
	};
	if (HasExtension(path.c_str(), ".chd"))
	{
		ChdSectors disc;
		return disc.Open(path) && read(disc);
	}
	if (HasExtension(path.c_str(), ".cso") || HasExtension(path.c_str(), ".zso"))
	{
		CsoSectors disc;
		return disc.Open(path) && read(disc);
	}
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	IsoSectors disc(fd);
	const bool ok = read(disc);
	close(fd);
	return ok;
}

void MakeTitle(const std::string& stem, std::string& title, std::string& region, std::string& extra)
{
	std::string base = stem;
	std::vector<std::string> groups;
	// Bracketed groups at the end: "(USA) (En,Ja)" or "[!]".
	for (;;)
	{
		const std::string t = Trim(base);
		if (t.empty() || (t.back() != ')' && t.back() != ']'))
		{
			base = t;
			break;
		}
		const char open = t.back() == ')' ? '(' : '[';
		const size_t o = t.rfind(open);
		if (o == std::string::npos || o == 0)
		{
			base = t;
			break;
		}
		groups.insert(groups.begin(), t.substr(o + 1, t.size() - o - 2));
		base = t.substr(0, o);
	}
	region.clear();
	extra.clear();
	for (const std::string& g : groups)
	{
		bool is_region = false;
		for (const char* r : kRegions)
			if (g.find(r) == 0)
				is_region = true;
		if (is_region && region.empty())
			region = g;
		else
			extra += (extra.empty() ? "" : ", ") + g;
	}
	// "Lord of the Rings, The - The Two Towers" -> "The Lord of the Rings - The Two Towers".
	const size_t dash = base.find(" - ");
	std::string first = dash == std::string::npos ? base : base.substr(0, dash);
	std::string rest = dash == std::string::npos ? std::string() : base.substr(dash + 3);
	for (const char* art : {", The", ", A", ", An"})
	{
		const size_t al = std::strlen(art);
		if (first.size() > al && first.compare(first.size() - al, al, art) == 0)
		{
			first = std::string(art + 2) + " " + first.substr(0, first.size() - al);
			break;
		}
	}
	// The first " - " is a subtitle's colon; later ones stay.
	title = rest.empty() ? first : first + ": " + rest;
	if (title.empty())
		title = stem;
}

namespace
{
// vk-285-113: the game database's entries, serial -> English name (name-en, else name) and region.
struct DbEntry
{
	std::string name;
	std::string region; // PCSX2's "NTSC-U", "PAL-E", ...
};
std::mutex s_db_mutex;
std::string s_db_file;
bool s_db_loaded = false;
std::unordered_map<std::string, DbEntry> s_db;

// The value of a `key: "text"` line's double-quoted scalar (GameIndex.yaml has no escapes), or a bare one up to " #".
std::string YamlValue(const char* p, const char* end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p < end && *p == '"')
	{
		p++;
		const char* q = p;
		while (q < end && *q != '"')
			q++;
		return std::string(p, q);
	}
	const char* q = p;
	while (q < end && !(*q == '#' && q > p && q[-1] == ' '))
		q++;
	while (q > p && (q[-1] == ' ' || q[-1] == '\r'))
		q--;
	return std::string(p, q);
}

void LoadDbLocked()
{
	if (s_db_loaded || s_db_file.empty())
		return;
	s_db_loaded = true;
	FILE* f = std::fopen(s_db_file.c_str(), "rb");
	if (!f)
		return;
	std::string text;
	// On the heap, not the stack: this runs on the settings page's server thread too (/api/games), and a 64 KB array
	// there overflowed its stack and ended the app the first time the page opened (vk-285-113).
	std::vector<char> buf(65536);
	size_t n;
	while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
		text.append(buf.data(), n);
	std::fclose(f);
	std::string serial, name, name_en, region;
	const auto flush = [&]() {
		if (!serial.empty() && !(name_en.empty() && name.empty()))
			s_db[serial] = DbEntry{name_en.empty() ? name : name_en, region};
		serial.clear();
		name.clear();
		name_en.clear();
		region.clear();
	};
	const char* p = text.data();
	const char* const end = p + text.size();
	while (p < end)
	{
		const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
		if (!eol)
			eol = end;
		if (p < eol && *p != ' ' && *p != '#' && *p != '\r' && *p != '\t')
		{
			// "SLUS-20552:" (maybe followed by a comment) starts an entry.
			const char* colon = static_cast<const char*>(std::memchr(p, ':', static_cast<size_t>(eol - p)));
			flush();
			if (colon && colon > p)
				serial.assign(p, colon);
		}
		else if (!serial.empty())
		{
			const char* q = p;
			while (q < eol && *q == ' ')
				q++;
			const auto is_key = [&](const char* key) {
				const size_t kl = std::strlen(key);
				return static_cast<size_t>(eol - q) > kl && std::memcmp(q, key, kl) == 0;
			};
			if (is_key("name-en:"))
				name_en = YamlValue(q + 8, eol);
			else if (is_key("name-sort:"))
			{
			}
			else if (is_key("name:"))
				name = YamlValue(q + 5, eol);
			else if (is_key("region:"))
				region = YamlValue(q + 7, eol);
		}
		p = eol + 1;
	}
	flush();
}

// The shelf's region word for the database's region code ("" when it doesn't say).
const char* DbRegion(const std::string& r)
{
	if (r == "NTSC-U")
		return "USA";
	if (r == "NTSC-J")
		return "Japan";
	if (r == "NTSC-K")
		return "Korea";
	if (r == "NTSC-C")
		return "China";
	if (r == "PAL-A")
		return "Australia";
	if (r == "PAL-F")
		return "France";
	if (r == "PAL-G")
		return "Germany";
	if (r == "PAL-I")
		return "Italy";
	if (r == "PAL-S")
		return "Spain";
	if (r == "PAL-R")
		return "Russia";
	if (r.compare(0, 4, "PAL-") == 0 && r != "PAL-Unk")
		return "Europe"; // PAL-E, PAL-M5 (multi-language), ...
	return "";
}

// A title to compare: letters and digits only, lower case, "&" as "and"; kana, kanji and Hangul bytes stay as they are.
std::string CompareForm(const std::string& s)
{
	std::string out;
	for (const char ch : s)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c == '&')
			out += "and";
		else if (std::isalnum(c))
			out += static_cast<char>(std::tolower(c));
		else if (c >= 0x80)
			out += ch;
	}
	return out;
}

// Whether a title has Japanese, Chinese or Korean letters (UTF-8 lead bytes 0xE3-0xED), which the shelf's font may lack.
bool HasCjk(const std::string& s)
{
	for (const char ch : s)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c >= 0xE3 && c <= 0xED)
			return true;
	}
	return false;
}
} // namespace

void SetGameDbFile(const std::string& path)
{
	std::lock_guard<std::mutex> lock(s_db_mutex);
	if (path == s_db_file)
		return;
	s_db_file = path;
	s_db_loaded = false;
	s_db.clear();
}

bool ApplyGameDbTitle(GameInfo& g)
{
	if (g.serial.empty())
		return false;
	DbEntry entry;
	{
		std::lock_guard<std::mutex> lock(s_db_mutex);
		LoadDbLocked();
		const auto it = s_db.find(g.serial);
		if (it == s_db.end())
			return false;
		entry = it->second;
	}
	std::string title, region, extra;
	MakeTitle(entry.name, title, region, extra);
	if (g.region.empty())
		g.region = DbRegion(entry.region);
	const std::string db_form = CompareForm(title);
	if (db_form.empty() || CompareForm(g.title).find(db_form) != std::string::npos)
		return false; // the file's name already holds the game's
	if (HasCjk(title) && !HasCjk(g.title))
		return false; // only a Japanese name known: the file's Latin one reads better on the shelf
	g.title = title;
	if (g.extra.empty())
		g.extra = extra;
	return true;
}

void SortGames(std::vector<GameInfo>& games)
{
	std::sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const std::string la = Lower(a.title), lb = Lower(b.title);
		return la != lb ? la < lb : a.file < b.file;
	});
}

namespace
{
// vk-285-115 (AI-assisted): the disc images one folder down too ("games/Ratchet & Clank (USA)/Ratchet & Clank.iso", a
// USB drive's PS2/ or ISO/ folder). 1.50's logs: 29 consoles never found a game ("0 disc image(s), 0 on USB" on
// every start), most with a USB drive that had files but no image at its root. PCSX2's own folders are skipped, and at
// most 64 subfolders of a folder are opened (a backup drive's whole tree isn't walked).
bool SkipSubfolder(const char* name)
{
	if (name[0] == '.' || name[0] == '$')
		return true;
	static const char* const kSkip[] = {"bios", "cache", "cheats", "cheats_ws", "covers", "flags", "inputprofiles", "lang",
		"logs", "memcards", "patches", "resources", "settings", "snaps", "sstates", "savestates", "textures", "videos",
		"System Volume Information", "LOST.DIR"};
	for (const char* skip : kSkip)
	{
		if (Lower(name) == Lower(skip))
			return true;
	}
	return false;
}

void ScanDir(const std::string& dir, std::vector<GameInfo>& games, std::vector<std::string>* subdirs)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	while (const dirent* e = readdir(d))
	{
		if (!IsDiscImageName(e->d_name))
		{
			if (subdirs && subdirs->size() < 64 && !SkipSubfolder(e->d_name))
			{
				bool is_dir = e->d_type == DT_DIR;
				if (e->d_type == DT_UNKNOWN)
				{
					struct stat st = {};
					is_dir = stat((dir + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
				}
				if (is_dir)
					subdirs->emplace_back(e->d_name);
			}
			continue;
		}
		const std::string file = e->d_name;
		if (std::any_of(games.begin(), games.end(), [&](const GameInfo& g) { return g.file == file; }))
			continue;
		GameInfo g;
		g.path = dir + "/" + file;
		struct stat st = {};
		if (stat(g.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		g.file = file;
		g.stem = file.substr(0, file.size() - 4); // ".iso", ".chd", ".cso", ".zso"
		g.bytes = static_cast<uint64_t>(st.st_size);
		MakeTitle(g.stem, g.title, g.region, g.extra);
		games.push_back(g);
	}
	closedir(d);
}
} // namespace

std::vector<GameInfo> ScanGames(const std::vector<std::string>& dirs)
{
	std::vector<GameInfo> games;
	for (const std::string& dir : dirs)
	{
		std::vector<std::string> subdirs;
		ScanDir(dir, games, &subdirs);
		std::sort(subdirs.begin(), subdirs.end());
		for (const std::string& sub : subdirs)
			ScanDir(dir + "/" + sub, games, nullptr);
	}
	SortGames(games);
	return games;
}

namespace
{
// The value of `key=` in a settings file (the last one wins), or empty.
std::string IniValue(const std::string& path, const char* key)
{
	std::string value;
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
		return value;
	char line[512];
	const size_t kl = std::strlen(key);
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l = Trim(line);
		if (l.compare(0, kl, key) == 0 && l.size() > kl && l[kl] == '=')
			value = Trim(l.substr(kl + 1));
	}
	std::fclose(f);
	return value;
}

bool IniHasLine(const std::string& path, const char* line_text)
{
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
		return false;
	char line[512];
	bool found = false;
	while (!found && std::fgets(line, sizeof(line), f))
		found = Lower(Trim(line)) == Lower(line_text);
	std::fclose(f);
	return found;
}

bool FileHas(const std::string& path, const char* text)
{
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
		return false;
	std::string all;
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && all.size() < (1u << 20))
		all.append(buf, n);
	std::fclose(f);
	return all.find(text) != std::string::npos;
}
} // namespace

void ReadBadges(GameInfo& g, const std::string& settings_dir, const std::string& gs_ini, const std::string& patches_dir)
{
	g.badges.clear();
	const std::string ini = settings_dir + "/" + g.stem + ".ini";
	std::string scale = IniValue(ini, "upscale_multiplier");
	if (scale.empty())
		scale = IniValue(gs_ini, "upscale_multiplier");
	if (!scale.empty() && scale != "1")
		g.badges.push_back(scale + "x");
	bool widescreen = false;
	if (!g.serial.empty() && IniValue(gs_ini, "EmuCore/EnableWideScreenPatches") == "true")
	{
		if (DIR* d = opendir(patches_dir.c_str()))
		{
			while (const dirent* e = readdir(d))
				if (std::strncmp(e->d_name, g.serial.c_str(), g.serial.size()) == 0 &&
					FileHas(patches_dir + "/" + e->d_name, "[Widescreen 16:9]"))
					widescreen = true;
			closedir(d);
		}
	}
	if (widescreen)
		g.badges.push_back("16:9");
	if (IniHasLine(ini, "Patches/Enable=60 FPS"))
		g.badges.push_back("60 FPS");
}
} // namespace fe
