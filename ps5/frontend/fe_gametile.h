// PS5 port frontend: game tiles from the shelf (2.12, helper revision 171g; AI-assisted; swordpdf: "add an option to add a game from the shelf to the
// xmb (square - and then option on) it should utilize the existing mounting scheme we have, we should also be able to delete it, again, from the shelf").
//
// One header with every rule in one place, so that the shelf's "Home screen tile" row (fe_app.cpp), the settings page's copy of it (fe_web.cpp, index.html),
// the tile's own start (coreorbis/main-boot.cpp) and the PC tests (host/gametile_test.cpp) cannot drift apart. The other half is the PS5SX2 Disc Helper
// (tile6/gametile.c); what the two sides write and read, byte for byte, is plans/gametiles_contract.md (its sections 2 to 9 are what is below).
//
// In short: the helper makes a COPY of PS5SX2's own app under a title ID of PPSA99251..PPSA99260, registers it through ShadowMount+ and tells the shelf what
// it is doing through two small files in /data/PCSX2/tiles/ (alive.txt, state.txt). The shelf asks with ONE FILE PER PRESS (request-<ms>-<n>.txt, made as
// .tmp and renamed in) and never opens one that is there. A pressed tile starts PS5SX2 from the copy, which reads /app0/game-tile.txt (ParseMarker) and
// starts exactly that game with no shelf. Game tiles are ON by default; the flag file flags/gametile_off switches them off (fe_ps5.cpp clears AppConfig::tiles_dir then).
//
// Every function is inline and every constant inline constexpr: five translation units include this file. No exceptions, no RTTI, no big stack arrays.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fe::gametile
{
// ---- names, sizes and times (contract sections 2, 4 and 7) ------------------------------------------------------------------------------------------

inline constexpr const char* kStateFile = "state.txt";           // helper -> shelf: what the helper knows (4.2)
inline constexpr const char* kAliveFile = "alive.txt";           // helper -> shelf: the helper is there (4.3)
inline constexpr const char* kRequestPrefix = "request-";        // shelf -> helper: request-<ms13>-<nnn>.txt (4.1)
inline constexpr const char* kMarkerFile = "game-tile.txt";      // in the tile's folder (4.4); /app0/game-tile.txt from the tile's own process

inline constexpr double kTakeS = 15.0;           // an own request file still in the folder after this long: "No answer", and it is taken back
inline constexpr double kAppearS = 90.0;         // taken, and no line for the game (add) / the line unchanged (remove) after this long: "Stuck"
inline constexpr double kRetryGraceS = 5.0;      // after a retry the old failure's text is ignored for this long after the take
inline constexpr double kShutdownWithdrawS = 3.0; // App::Shutdown takes back own request files older than this
inline constexpr double kPollBusyS = 1.0;        // the poll while an ask is pending or the shown row is busy
inline constexpr double kPollIdleS = 3.0;        // ... otherwise (the sheet is open and nothing happens)
inline constexpr long long kAliveFreshS = 60;    // alive.txt is "the helper is there" for this long after it was written

inline constexpr size_t kMaxPath = 511;    // bytes, after the normalizing
inline constexpr size_t kMaxTitle = 120;   // bytes sent (the helper cuts at 127)
inline constexpr size_t kMaxSerial = 15;   // characters; a longer serial is not sent
inline constexpr size_t kMaxCover = 299;   // bytes of the cover's path
inline constexpr size_t kMaxFile = 65536;  // state.txt is read up to this
inline constexpr size_t kMarkerMax = 1599; // game-tile.txt is read up to this (main-boot.cpp's buffer is 1600)

inline constexpr int kFirstTile = 99251; // PPSA99251 .. PPSA99260: the ten game tiles
inline constexpr int kLastTile = 99260;

// ---- text: paths, serials, titles, cover paths ------------------------------------------------------------------------------------------------------

inline bool IsControl(unsigned char c)
{
	return c < 0x20 || c == 0x7F;
}

// "//" collapsed, no trailing '/' (the helper's gt_norm_path; both sides do it before they compare or check anything).
inline std::string NormalizePath(const std::string& p)
{
	std::string out;
	out.reserve(p.size());
	for (const char c : p)
	{
		if (c == '/' && !out.empty() && out.back() == '/')
			continue;
		out += c;
	}
	while (out.size() > 1 && out.back() == '/')
		out.pop_back();
	return out;
}

// The last component, and it without its extension.
inline std::string FileNameOf(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string StemOf(const std::string& path)
{
	std::string name = FileNameOf(path);
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos && dot > 0)
		name.erase(dot);
	return name;
}

inline bool HasDotDot(const std::string& path)
{
	size_t at = 0;
	while (at <= path.size())
	{
		size_t slash = path.find('/', at);
		if (slash == std::string::npos)
			slash = path.size();
		if (slash - at == 2 && path[at] == '.' && path[at + 1] == '.')
			return true;
		at = slash + 1;
	}
	return false;
}

// Where a game can be for the helper (its GT_ROOTS): the console's data folder and the drives. Nothing in the app changes this; the PC tests add their own temporary
// folder to it (gametile_web_test.cpp), because /data and /mnt cannot be made there.
inline std::vector<std::string>& GameRoots()
{
	static std::vector<std::string> roots = {"/data/", "/mnt/"};
	return roots;
}

// A game the helper accepts (the helper's gt_path_ok, after the normalizing): absolute, under /data/ or /mnt/ with at least one more byte, at most 511 bytes,
// no control byte (so no TAB or line break), no ".." as a whole component (a name like "..hidden" is fine). /dev/ and /nfs/ are not eligible.
inline bool PathOk(const std::string& path, std::string* why = nullptr)
{
	auto no = [&](const char* what) {
		if (why)
			*why = what;
		return false;
	};
	const std::string p = NormalizePath(path);
	if (p.empty() || p[0] != '/')
		return no("not an absolute path");
	if (p.size() > kMaxPath)
		return no("too long");
	bool under = false;
	for (const std::string& root : GameRoots())
		under = under || (p.compare(0, root.size(), root) == 0 && p.size() > root.size());
	if (!under)
		return no("only files under /data or /mnt");
	for (const char c : p)
		if (IsControl(static_cast<unsigned char>(c)))
			return no("a control character");
	if (HasDotDot(p))
		return no("a .. component");
	return true;
}

inline bool IsElfFile(const std::string& file)
{
	if (file.size() < 4)
		return false;
	const char* e = file.c_str() + file.size() - 4;
	return e[0] == '.' && (e[1] | 0x20) == 'e' && (e[2] | 0x20) == 'l' && (e[3] | 0x20) == 'f';
}

// Which games get the row (the sheet and the page share it): a plain image file the helper can use. The file itself is not looked at (the helper does).
inline bool RowEligible(const std::string& path, const std::string& file, const std::string& damaged)
{
	return damaged.empty() && !IsElfFile(file) && PathOk(path);
}

// [A-Za-z0-9._-]{1,15}, else "" (the helper empties a serial it cannot use; the shelf sends none rather than a cut one).
inline std::string CleanSerial(const std::string& serial)
{
	if (serial.empty() || serial.size() > kMaxSerial)
		return {};
	for (const unsigned char c : serial)
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-'))
			return {};
	return serial;
}

// A title for one line: TAB, line breaks and every other control byte become a space, trimmed, at most 120 bytes and never cut inside a UTF-8 character.
inline std::string CleanTitle(const std::string& title)
{
	std::string s;
	s.reserve(title.size());
	for (const unsigned char c : title)
		s += IsControl(c) ? ' ' : static_cast<char>(c);
	const size_t first = s.find_first_not_of(' ');
	if (first == std::string::npos)
		return {};
	s = s.substr(first, s.find_last_not_of(' ') - first + 1);
	if (s.size() > kMaxTitle)
	{
		size_t cut = kMaxTitle;
		while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
			cut--;
		s.resize(cut);
		const size_t last = s.find_last_not_of(' ');
		s.resize(last == std::string::npos ? 0 : last + 1);
	}
	return s;
}

// The title the request carries: the shelf's, else the file's name, else a word. Never empty.
inline std::string TitleFor(const std::string& title, const std::string& path)
{
	std::string t = CleanTitle(title);
	if (t.empty())
		t = CleanTitle(StemOf(path));
	return t.empty() ? std::string("Game") : t;
}

// A cover the helper will read: absolute, at most 299 bytes, no ".." anywhere (the helper is stricter than for games), no control byte.
inline bool CoverPathOk(const std::string& cover)
{
	if (cover.empty() || cover[0] != '/' || cover.size() > kMaxCover || cover.find("..") != std::string::npos)
		return false;
	for (const unsigned char c : cover)
		if (IsControl(c))
			return false;
	return true;
}

inline bool HasExtension(const std::string& name, const char* ext)
{
	const size_t n = std::strlen(ext);
	if (name.size() <= n)
		return false;
	for (size_t i = 0; i < n; i++)
		if ((name[name.size() - n + i] | 0x20) != ext[i])
			return false;
	return true;
}

// The shelf's cover for a game as the request's fifth field: a plain .jpg/.jpeg/.png file that is not empty (the helper reads those and others; WebP not).
inline bool CoverFileOk(const std::string& cover)
{
	if (!CoverPathOk(cover) || !(HasExtension(cover, ".jpg") || HasExtension(cover, ".jpeg") || HasExtension(cover, ".png")))
		return false;
	struct stat st = {};
	return stat(cover.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// "PPSA99251" .. "PPSA99260" (the id of a slot).
inline bool TileIdInRange(const std::string& id)
{
	if (id.size() != 9 || id.compare(0, 4, "PPSA") != 0)
		return false;
	for (size_t i = 4; i < 9; i++)
		if (id[i] < '0' || id[i] > '9')
			return false;
	const int n = std::atoi(id.c_str() + 4);
	return n >= kFirstTile && n <= kLastTile;
}

inline std::string TileIdOf(int slot)
{
	return "PPSA" + std::to_string(kFirstTile + slot);
}

// ---- the request files: one file per press (contract 4.1) -----------------------------------------------------------------------------------------------

// "v1\nadd\t<path>\t<serial>\t<title>[\t<cover>]\n": the path normalized, the serial empty when unfit, the title cleaned (never empty), the cover only when it is a
// file the helper can read. Empty when the path is not one the helper accepts.
inline std::string FormatAdd(const std::string& path, const std::string& serial, const std::string& title, const std::string& cover = std::string())
{
	if (!PathOk(path))
		return {};
	const std::string p = NormalizePath(path);
	std::string out = "v1\nadd\t" + p + "\t" + CleanSerial(serial) + "\t" + TitleFor(title, p);
	if (CoverPathOk(cover))
		out += "\t" + cover;
	return out + "\n";
}

// "v1\nremove\t<game path or tile id>\n". By id also for a game the shelf no longer lists.
inline std::string FormatRemove(const std::string& path_or_id)
{
	if (TileIdInRange(path_or_id))
		return "v1\nremove\t" + path_or_id + "\n";
	if (!PathOk(path_or_id))
		return {};
	return "v1\nremove\t" + NormalizePath(path_or_id) + "\n";
}

inline double WallSeconds()
{
	return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline long long WallMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// request-<ms, 13 digits>-<counter, 3 digits>.txt: the names sort in the order they were written.
inline std::string RequestName(long long ms, int counter)
{
	char name[64];
	std::snprintf(name, sizeof(name), "request-%013lld-%03d.txt", ms, ((counter % 1000) + 1000) % 1000);
	return name;
}

// Every request file this process wrote and has not seen taken or taken back (the shelf's and the page's together), for App::Shutdown.
struct Written
{
	std::string dir, name;
	double wall = 0; // WallSeconds() at the write
};

inline std::mutex& RegistryMutex()
{
	static std::mutex m;
	return m;
}

inline std::vector<Written>& Registry()
{
	static std::vector<Written> v;
	return v;
}

inline int& RequestCounter()
{
	static int n = 0;
	return n;
}

inline void RegistryForgetLocked(const std::string& dir, const std::string& name)
{
	std::vector<Written>& v = Registry();
	v.erase(std::remove_if(v.begin(), v.end(), [&](const Written& w) { return w.dir == dir && w.name == name; }), v.end());
}

// Writes `text` as one new request file in `dir` (the folder must be there: the helper makes it, the shelf does not): name.tmp (0666), written and closed with
// every result checked (no fsync), then renamed to the final name. Any failure removes the .tmp and gives the reason in *error. *name gets the file's name.
// `ms` < 0: the clock. Nothing is ever appended to or rewritten in a file that is there.
inline bool QueueRequest(const std::string& dir, const std::string& text, std::string* name, std::string* error, long long ms = -1)
{
	std::lock_guard<std::mutex> lock(RegistryMutex());
	const std::string file = RequestName(ms < 0 ? WallMs() : ms, RequestCounter()++);
	const std::string final_path = dir + "/" + file, tmp = final_path + ".tmp";
	auto fail = [&](int e) {
		if (error)
			*error = std::strerror(e);
		return false;
	};
	const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return fail(errno);
	fchmod(fd, 0666); // (the umask must not take the helper's read away)
	size_t done = 0;
	while (done < text.size())
	{
		const ssize_t n = write(fd, text.data() + done, text.size() - done);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			const int e = errno;
			close(fd);
			unlink(tmp.c_str());
			return fail(e);
		}
		done += static_cast<size_t>(n);
	}
	if (close(fd) != 0)
	{
		const int e = errno;
		unlink(tmp.c_str());
		return fail(e);
	}
	if (rename(tmp.c_str(), final_path.c_str()) != 0)
	{
		const int e = errno;
		unlink(tmp.c_str());
		return fail(e);
	}
	std::vector<Written>& v = Registry();
	v.push_back({dir, file, WallSeconds()});
	if (v.size() > 64)
		v.erase(v.begin());
	if (name)
		*name = file;
	return true;
}

// Is the file still there (not taken)? Any other answer than "no such file" counts as there.
inline bool RequestPresent(const std::string& dir, const std::string& name)
{
	struct stat st = {};
	if (stat((dir + "/" + name).c_str(), &st) == 0)
		return true;
	return errno != ENOENT;
}

enum class WithdrawResult
{
	Withdrawn,    // the file was there and is gone
	AlreadyTaken, // no such file: the helper took it just now
	Failed,
};

// Takes an own, unanswered request file back. ENOENT means the helper took it a moment ago.
inline WithdrawResult WithdrawFile(const std::string& dir, const std::string& name)
{
	if (unlink((dir + "/" + name).c_str()) == 0)
	{
		std::lock_guard<std::mutex> lock(RegistryMutex());
		RegistryForgetLocked(dir, name);
		return WithdrawResult::Withdrawn;
	}
	if (errno == ENOENT)
	{
		std::lock_guard<std::mutex> lock(RegistryMutex());
		RegistryForgetLocked(dir, name);
		return WithdrawResult::AlreadyTaken;
	}
	return WithdrawResult::Failed;
}

// App::Shutdown: every request file of `dir` this process wrote that is older than `min_age_s` (a live helper has taken its file long before) is taken back.
// Younger ones stay (the helper may be taking them). Returns how many were removed.
inline int WithdrawOwn(const std::string& dir, double min_age_s, double wall_now)
{
	std::lock_guard<std::mutex> lock(RegistryMutex());
	int removed = 0;
	std::vector<Written>& v = Registry();
	for (size_t i = 0; i < v.size();)
	{
		if (v[i].dir == dir && wall_now - v[i].wall >= min_age_s)
		{
			if (unlink((dir + "/" + v[i].name).c_str()) == 0)
				removed++;
			v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
		}
		else
			i++;
	}
	return removed;
}

// ---- helper files: alive.txt and state.txt (contract 4.2 and 4.3) ---------------------------------------------------------------------------------------

inline bool ReadSmall(const std::string& path, size_t max, std::string& out)
{
	out.clear();
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	char buf[2048];
	for (;;)
	{
		const ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			close(fd);
			return false;
		}
		if (n == 0)
			break;
		out.append(buf, static_cast<size_t>(n));
		if (out.size() >= max)
		{
			out.resize(max);
			break;
		}
	}
	close(fd);
	return true;
}

// "<unix time> <pid> gametile 1": the first line of alive.txt, four fields with the word and the version of this contract.
inline bool AliveLineOk(const std::string& text)
{
	std::string line = text.substr(0, text.find('\n'));
	std::vector<std::string> f;
	std::string cur;
	for (const char c : line)
	{
		if (c == ' ' || c == '\t' || c == '\r')
		{
			if (!cur.empty())
				f.push_back(cur);
			cur.clear();
		}
		else
			cur += c;
	}
	if (!cur.empty())
		f.push_back(cur);
	if (f.size() < 4 || f[2] != "gametile" || f[3] != "1")
		return false;
	for (int i = 0; i < 2; i++)
		for (const char c : f[static_cast<size_t>(i)])
			if (c < '0' || c > '9')
				return false;
	return true;
}

// The helper is there: alive.txt exists, its first line is right and it is at most 60 s old (a time in the future counts as 0 s). `wall_now` is the clock in
// seconds, the same the file's modification time is in.
inline bool AliveFresh(const std::string& dir, long long wall_now)
{
	const std::string path = dir + "/" + kAliveFile;
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return false;
	long long age = wall_now - static_cast<long long>(st.st_mtime);
	if (age < 0)
		age = 0;
	if (age > kAliveFreshS)
		return false;
	std::string text;
	return ReadSmall(path, 256, text) && AliveLineOk(text);
}

enum class Kind
{
	Ok,
	Installing,
	Removing,
	Failed,
};

// One line of state.txt: <tile id or ->, <game path>, <serial>, <title>, <status>.
struct Entry
{
	std::string tile, path, serial, title, status, reason;
	Kind kind = Kind::Ok;
};

// The lines of state.txt's text. A line needs five TAB-separated fields (more are ignored); blank lines, comments and a "v1" line are skipped; a line whose status
// word is not ok, installing, removing or failed[:reason] is ignored. The path is normalized.
inline std::vector<Entry> ParseState(const std::string& text)
{
	std::vector<Entry> out;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		std::string line = text.substr(pos, nl - pos);
		pos = nl + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#' || line == "v1")
			continue;
		std::vector<std::string> f;
		size_t at = 0;
		for (;;)
		{
			const size_t tab = line.find('\t', at);
			if (tab == std::string::npos)
			{
				f.push_back(line.substr(at));
				break;
			}
			f.push_back(line.substr(at, tab - at));
			at = tab + 1;
		}
		if (f.size() < 5)
			continue;
		Entry e;
		e.tile = f[0];
		e.path = NormalizePath(f[1]);
		e.serial = f[2];
		e.title = f[3];
		e.status = f[4];
		if (e.status == "ok")
			e.kind = Kind::Ok;
		else if (e.status == "installing")
			e.kind = Kind::Installing;
		else if (e.status == "removing")
			e.kind = Kind::Removing;
		else if (e.status == "failed" || e.status.compare(0, 7, "failed:") == 0)
		{
			e.kind = Kind::Failed;
			e.reason = e.status.size() > 7 ? e.status.substr(7) : std::string();
		}
		else
			continue;
		out.push_back(std::move(e));
	}
	return out;
}

// The line for a game (its normalized path): ok beats installing beats removing beats failed. Null when there is none.
inline const Entry* FindByPath(const std::vector<Entry>& entries, const std::string& path)
{
	const Entry* best = nullptr;
	for (const Entry& e : entries)
		if (e.path == path && (!best || static_cast<int>(e.kind) < static_cast<int>(best->kind)))
			best = &e;
	return best;
}

// state.txt as the shelf sees it, read again only when the file changed (its inode, size and times down to the nanosecond: the PC harness rewrites it faster
// than a second), or when the caller asks (`force`, `always_read`: an ask is pending). A missing file is "no tiles" (the caller has alive.txt to know better).
class StateReader
{
public:
	bool exists = false;
	std::vector<Entry> entries;

	// True when the entries were read again.
	bool Refresh(const std::string& dir, bool force, bool always_read)
	{
		const std::string path = dir + "/" + kStateFile;
		struct stat st = {};
		if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) // (a pipe or a folder named state.txt is no state: opening a pipe for reading waits for a writer for ever; cross test, 2026-10-16)
		{
			const bool changed = exists || !entries.empty();
			exists = false;
			entries.clear();
			m_have = false;
			return changed;
		}
		const Sig sig = {static_cast<uint64_t>(st.st_ino), static_cast<int64_t>(st.st_size), static_cast<int64_t>(st.st_mtim.tv_sec),
			static_cast<int64_t>(st.st_mtim.tv_nsec), static_cast<int64_t>(st.st_ctim.tv_sec), static_cast<int64_t>(st.st_ctim.tv_nsec)};
		if (!force && !always_read && m_have && exists && sig == m_sig)
			return false;
		std::string text;
		if (!ReadSmall(path, kMaxFile, text))
		{
			exists = false;
			entries.clear();
			m_have = false;
			return true;
		}
		entries = ParseState(text);
		exists = true;
		m_sig = sig;
		m_have = true;
		return true;
	}

private:
	struct Sig
	{
		uint64_t ino;
		int64_t size, mtime_s, mtime_ns, ctime_s, ctime_ns;
		bool operator==(const Sig& o) const
		{
			return ino == o.ino && size == o.size && mtime_s == o.mtime_s && mtime_ns == o.mtime_ns && ctime_s == o.ctime_s && ctime_ns == o.ctime_ns;
		}
	};
	Sig m_sig = {};
	bool m_have = false;
};

// ---- the row: phases, the view and what the shelf and the page say (contract 7 and 8) -----------------------------------------------------------------

enum class Phase
{
	Off = 0,      // helper there, no ask, no line
	Asking = 1,   // the own request file is in the folder (up to 15 s)
	Waiting = 2,  // taken, no answer yet (or an old tile of the game is being removed first)
	Installing = 3,
	On = 4,
	Failed = 5,
	Removing = 6,
	NoAnswer = 7, // the file was not taken in 15 s (and has been taken back)
	Stuck = 8,    // taken, but no result in 90 s
	NoHelper = 9, // alive.txt is not fresh: the helper that makes game tiles is not running (also: nothing is written)
};

enum class Can
{
	None,
	Add,
	Remove,
};

struct View
{
	Phase phase = Phase::Off;
	Can can = Can::Add;
	bool busy = false;           // Asking, Waiting, Installing, Removing: presses are not taken
	bool withdraw = false;       // Evaluate wants the own request file taken back (once)
	bool ask_remove = false;     // the ask in work is a remove
	bool behind_removal = false; // Waiting for an earlier tile of this game to be removed
	std::string tile;            // PPSA992nn when the helper has a line for the game
	std::string reason;          // Failed: the word after "failed:"
};

// Asks the shelf (or the page) has made, one per game path, and what became of them. The caller passes the folder's facts in: whether the helper is there, the
// lines of state.txt, whether the own request file is still in the folder. All times are one clock (the shelf's); a clock that goes back is never a negative age.
class Tracker
{
public:
	// `file` is the request file just written. `current` is the game's line in state.txt when the ask is made (a failed one is remembered: after a retry the
	// helper's old failure is not taken for the new answer).
	void Ask(const std::string& path, bool add, const std::string& file, double now, const Entry* current)
	{
		Record r;
		r.add = add;
		r.file = file;
		r.asked = now;
		if (current && current->kind == Kind::Failed)
			r.prior_failed = current->status;
		m_asks[path] = r;
	}

	// The result of the caller's WithdrawFile for a view that said `withdraw`.
	void WithdrawDone(const std::string& path, WithdrawResult r, double now)
	{
		const auto it = m_asks.find(path);
		if (it == m_asks.end())
			return;
		Record& a = it->second;
		if (r == WithdrawResult::Withdrawn)
		{
			a.withdrawn = true;
			a.pending = false; // nothing is left to look for fast (while the helper is away Evaluate had kept it pending for this very file)
		}
		else if (r == WithdrawResult::AlreadyTaken)
		{
			a.withdraw_sent = false;
			if (a.taken < 0)
				a.taken = std::max(a.asked, now); // it was taken just now
		}
		else
			a.withdraw_sent = false; // tried again at the next look
	}

	View Evaluate(const std::string& path, const std::vector<Entry>& entries, bool helper_alive, bool file_present, double now)
	{
		View v;
		const auto it = m_asks.find(path);
		Record* a = it == m_asks.end() ? nullptr : &it->second;
		if (a)
		{
			if (now < a->asked)
				a->asked = now;
			if (!file_present && !a->withdrawn && a->taken < 0)
				a->taken = now;
			v.ask_remove = !a->add;
		}
		if (!helper_alive)
		{
			// The lines of state.txt are valid only while the helper is there. The own file still has its time limit.
			v.phase = Phase::NoHelper;
			v.can = Can::None;
			if (a)
			{
				v.withdraw = WantsWithdraw(*a, file_present, now);
				// Only an own file still in the folder needs the one-second looks (its 15 s limit). Without this an ask that was pending when the helper went away stayed
				// pending, and the shelf looked at the folder once a second, with the sheet closed, until the helper came back (reviewer, 2026-10-16).
				a->pending = file_present && !a->withdrawn;
			}
			return v;
		}
		const Entry* e = FindByPath(entries, path);
		if (e && e->tile != "-")
			v.tile = e->tile;
		if (!a)
		{
			if (!e)
			{
				v.phase = Phase::Off;
				v.can = Can::Add;
				return v;
			}
			switch (e->kind)
			{
				case Kind::Ok:
					v.phase = Phase::On;
					v.can = Can::Remove;
					break;
				case Kind::Installing:
					v.phase = Phase::Installing;
					v.can = Can::None;
					v.busy = true;
					break;
				case Kind::Removing:
					v.phase = Phase::Removing;
					v.can = Can::None;
					v.busy = true;
					break;
				case Kind::Failed:
					v.phase = Phase::Failed;
					v.can = Can::Add;
					v.reason = e->reason;
					break;
			}
			return v;
		}
		const double age = now - a->asked;
		const double since_take = a->taken >= 0 ? now - a->taken : 0.0;
		const bool untaken = file_present && !a->withdrawn;
		if (a->add)
		{
			if (e && e->kind == Kind::Ok)
			{
				v.phase = Phase::On;
				v.can = Can::Remove;
				m_asks.erase(it);
				return v;
			}
			if (e && e->kind == Kind::Installing)
			{
				v.phase = Phase::Installing;
				v.can = Can::None;
				v.busy = true;
				a->pending = false; // the helper has it: nothing to look for fast any more
				return v;
			}
			const bool stale_failure = e && e->kind == Kind::Failed && !a->prior_failed.empty() && e->status == a->prior_failed &&
			                           (untaken || since_take < kRetryGraceS);
			if (e && e->kind == Kind::Failed && !stale_failure)
			{
				v.phase = Phase::Failed;
				v.can = Can::Add;
				v.reason = e->reason;
				m_asks.erase(it);
				return v;
			}
			if (untaken)
			{
				if (age <= kTakeS)
				{
					v.phase = Phase::Asking;
					v.can = Can::None;
					v.busy = true;
					a->pending = true;
				}
				else
				{
					v.phase = Phase::NoAnswer;
					v.can = Can::Add;
					v.withdraw = WantsWithdraw(*a, file_present, now);
					a->pending = false;
				}
				return v;
			}
			if (a->withdrawn)
			{
				v.phase = Phase::NoAnswer;
				v.can = Can::Add;
				a->pending = false;
				return v;
			}
			// Taken.
			if (e && e->kind == Kind::Removing)
			{
				v.phase = Phase::Waiting; // an earlier tile of this game is taken away first
				v.behind_removal = true;
				v.can = Can::None;
				v.busy = true;
				a->pending = false;
				a->taken = now; // the wait for the build starts when the removal is over, not when the file was taken (that may have been the whole session ago)
				return v;
			}
			if (since_take > kAppearS)
			{
				v.phase = Phase::Stuck;
				v.can = Can::Add;
				a->pending = false;
				return v;
			}
			v.phase = Phase::Waiting;
			v.can = Can::None;
			v.busy = true;
			a->pending = true;
			return v;
		}
		// A remove.
		if (untaken)
		{
			if (age <= kTakeS)
			{
				v.phase = Phase::Asking;
				v.can = Can::None;
				v.busy = true;
				a->pending = true;
			}
			else
			{
				v.phase = Phase::NoAnswer;
				v.can = Can::Remove;
				v.withdraw = WantsWithdraw(*a, file_present, now);
				a->pending = false;
			}
			return v;
		}
		if (a->withdrawn)
		{
			v.phase = Phase::NoAnswer;
			v.can = Can::Remove;
			a->pending = false;
			return v;
		}
		if (!e)
		{
			v.phase = Phase::Off;
			v.can = Can::Add;
			m_asks.erase(it);
			return v;
		}
		if (e->kind == Kind::Failed)
		{
			v.phase = Phase::Failed;
			v.can = Can::Add;
			v.reason = e->reason;
			m_asks.erase(it);
			return v;
		}
		if (e->kind == Kind::Removing)
		{
			v.phase = Phase::Removing;
			v.can = Can::None;
			v.busy = true;
			a->pending = false;
			return v;
		}
		// Still ok or installing after the take: the helper sets "removing" within a second unless a build runs first.
		if (since_take > kAppearS)
		{
			v.phase = Phase::Stuck;
			v.can = Can::Remove;
			a->pending = false;
			return v;
		}
		v.phase = Phase::Removing;
		v.can = Can::None;
		v.busy = true;
		a->pending = true;
		return v;
	}

	bool Has(const std::string& path) const { return m_asks.find(path) != m_asks.end(); }

	std::string FileOf(const std::string& path) const
	{
		const auto it = m_asks.find(path);
		return it == m_asks.end() ? std::string() : it->second.file;
	}

	// Every game with an ask in the books (also those that ended in "No answer" or "Stuck": a late answer still resolves them).
	std::vector<std::string> Paths() const
	{
		std::vector<std::string> out;
		for (const auto& kv : m_asks)
			out.push_back(kv.first);
		return out;
	}

	// Asks still to be followed closely: the file has not been taken yet, or it has and no line for the game has shown up. Polling at 1 s is for these;
	// "Installing" and the like need nothing but the sheet's own look (the tile appears only after PS5SX2 is closed). "No answer" and "Stuck" are not pending.
	int PendingCount() const
	{
		int n = 0;
		for (const auto& kv : m_asks)
			n += kv.second.pending ? 1 : 0;
		return n;
	}
	bool AnyPending() const { return PendingCount() > 0; }

	void Forget(const std::string& path) { m_asks.erase(path); }

private:
	struct Record
	{
		bool add = true;
		std::string file;
		double asked = 0;
		double taken = -1;           // when the own file was first seen gone
		bool withdrawn = false;      // taken back by the shelf (the file is gone because of that, not because the helper took it)
		bool withdraw_sent = false;  // the view asked for it
		bool pending = true;
		std::string prior_failed;    // the failure present when a retry was asked
	};

	static bool WantsWithdraw(Record& a, bool file_present, double now)
	{
		if (!file_present || a.withdrawn || a.withdraw_sent || now - a.asked <= kTakeS)
			return false;
		a.withdraw_sent = true;
		return true;
	}

	std::map<std::string, Record> m_asks;
};

inline const char* PhaseName(Phase p)
{
	switch (p)
	{
		case Phase::Off: return "off";
		case Phase::Asking: return "asking";
		case Phase::Waiting: return "waiting";
		case Phase::Installing: return "installing";
		case Phase::On: return "on";
		case Phase::Failed: return "failed";
		case Phase::Removing: return "removing";
		case Phase::NoAnswer: return "noanswer";
		case Phase::Stuck: return "stuck";
		case Phase::NoHelper: return "nohelper";
	}
	return "off";
}

inline const char* CanName(Can c)
{
	return c == Can::Add ? "add" : c == Can::Remove ? "remove" : "none";
}

// What the row shows as its value (the sheet and the page).
inline std::string ValueText(const View& v)
{
	switch (v.phase)
	{
		case Phase::Off: return "Off";
		case Phase::Asking:
		case Phase::Waiting: return v.ask_remove ? "Removing..." : "Adding...";
		case Phase::Installing: return "Installing...";
		case Phase::On: return "On";
		case Phase::Failed: return "Failed";
		case Phase::Removing: return "Removing...";
		case Phase::NoAnswer: return "No answer";
		case Phase::Stuck: return "Stuck";
		case Phase::NoHelper: return "No helper";
	}
	return {};
}

// The helper's failure words (a closed set) in plain sentences.
inline std::string ReasonText(const std::string& reason)
{
	if (reason == "nogame")
		return "The helper cannot find the game file (is its drive connected?).";
	if (reason == "badpath")
		return "The helper cannot use this path (only files under /data or on a drive under /mnt).";
	if (reason == "limit")
		return "All 10 game tiles are in use: take one off first.";
	if (reason == "nospace")
		return "Not enough free space on /data (about 200 MB are needed).";
	if (reason == "build")
		return "Building the tile failed (the helper's log has the details).";
	if (reason == "disabled")
		return "Game tiles are switched off in the helper (flag gametile_off).";
	if (reason == "busy")
		return "The helper's request queue is full: try again in a minute.";
	if (reason == "damaged")
		return "The tile's folder was damaged and has been taken down: add it again.";
	return {};
}

// The help line under the row (the sheet and the page). `own_tile`: this process was started from this game's tile.
inline std::string HelpText(const View& v, const std::string& title, bool own_tile = false)
{
	switch (v.phase)
	{
		case Phase::Off:
			return "Puts " + title +
			       " on the PS5 home screen as its own tile: press it there and this game starts, no shelf. The tile appears about 30 seconds after you close PS5SX2.";
		case Phase::Asking:
			return "Asked the helper.";
		case Phase::Waiting:
			return v.behind_removal ? "The helper has the request. A tile that was removed a moment ago is finished first."
			                        : "The helper has the request.";
		case Phase::Installing:
			return "The tile is built. It appears on the home screen about 30 seconds after you close PS5SX2: the console adds it only when no game is running, and "
			       "PS5SX2 counts as one, also in the background.";
		case Phase::On:
			if (own_tile)
				return "On the home screen. This is the tile this game was started from: take it off from the main app.";
			return "On the home screen. Cross takes it off again (it goes away about 30 seconds after you close PS5SX2). If the game runs from the tile, close it first.";
		case Phase::Failed:
		{
			if (v.reason.empty())
				return "Failed";
			const std::string text = ReasonText(v.reason);
			return text.empty() ? "Failed (" + v.reason + ")" : "Failed: " + text;
		}
		case Phase::Removing:
			return "The tile is hidden now and is taken away about 30 seconds after you close PS5SX2.";
		case Phase::NoAnswer:
			return "The helper did not take the request within 15 seconds. Load the newest PS5SX2 Disc Helper (171g), then press Cross to try again.";
		case Phase::Stuck:
			if (v.ask_remove)
				return "The tile is still on the home screen. If the game runs from it, close it first, then try again.";
			return "The helper took the request but the tile did not show up in time. Press Cross to try again; disctile.log (flag kp_log) says why.";
		case Phase::NoHelper:
			return "The helper that makes game tiles is not running yet. The app loads it at start; if it did not, load the PS5SX2 Disc Helper (171k) by hand.";
	}
	return {};
}

// How often the state is looked at (App::PollTiles): not at all unless the row is shown or an ask is pending; at once when forced (the sheet opening); not
// while a texture pack is unpacked (the menu thread's file access stalls then); every second while something is going on, every three otherwise.
inline bool PollDue(bool wanted, bool pending, bool view_busy, bool disk_busy, bool force, double now, double last)
{
	if (!wanted)
		return false;
	if (force)
		return true;
	if (disk_busy)
		return false;
	if (now < last)
		return true; // the clock went back
	return now - last >= (pending || view_busy ? kPollBusyS : kPollIdleS);
}

// ---- the tile's own start (main-boot.cpp): /app0/game-tile.txt (contract 4.4 and 9) -----------------------------------------------------------------------

struct Marker
{
	std::string path;    // line 1: the game's path, spaces kept, only CR/LF trimmed
	std::string tile_id; // tile=PPSA992nn (6 to 12 letters and digits), else ""
	std::string serial;  // serial=
	std::string title;   // title=
};

// False when there is no usable first line (empty, or the buffer filled with no line break). Unknown or missing keys are ignored.
inline bool ParseMarker(const std::string& text, Marker& m)
{
	m = Marker();
	if (text.size() >= kMarkerMax && text.find('\n') == std::string::npos)
		return false;
	size_t pos = 0;
	int line = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		std::string l = text.substr(pos, nl - pos);
		pos = nl + 1;
		while (!l.empty() && l.back() == '\r')
			l.pop_back();
		if (line == 0)
			m.path = l;
		else if (l.compare(0, 5, "tile=") == 0)
		{
			const std::string id = l.substr(5);
			bool good = id.size() >= 6 && id.size() <= 12;
			for (const unsigned char c : id)
				good = good && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
			if (good)
				m.tile_id = id;
		}
		else if (l.compare(0, 7, "serial=") == 0)
			m.serial = l.substr(7);
		else if (l.compare(0, 6, "title=") == 0)
			m.title = l.substr(6);
		line++;
	}
	return !m.path.empty();
}

// Can this game start now? `why` gets "missing" (not there: a drive that is out or late) or "invalid" (not something that can start). is_image_name: fe::IsDiscImageName.
inline bool CheckLaunchPath(const std::string& path, std::string* why, bool (*is_image_name)(const char*))
{
	auto no = [&](const char* what) {
		if (why)
			*why = what;
		return false;
	};
	if (path.empty() || path[0] != '/' || HasDotDot(path) || path.compare(0, 5, "/dev/") == 0)
		return no("invalid");
	for (const unsigned char c : path)
		if (IsControl(c))
			return no("invalid");
	const std::string name = FileNameOf(path);
	if (name.empty() || IsElfFile(name) || (is_image_name && !is_image_name(name.c_str())))
		return no("invalid");
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return no("missing");
	if (!S_ISREG(st.st_mode) || st.st_size <= 0)
		return no("invalid");
	return true;
}

// The folder this process was started from (its tile): /data/homebrew/<id> with the same game-tile.txt as /app0 showed before the jailbreak (`seen`: its inode,
// size and time) or the same first line. The id from tile= is the one candidate when it is in the range; otherwise the ten ids of the range. Exactly one folder
// must count; two or none give "" (never guessed).
inline std::string FindOwnTileDir(const std::string& root, const Marker& m, const struct stat& seen)
{
	std::vector<std::string> candidates;
	if (TileIdInRange(m.tile_id))
		candidates.push_back(m.tile_id);
	else
		for (int s = 0; s < kLastTile - kFirstTile + 1; s++)
			candidates.push_back(TileIdOf(s));
	int hits = 0;
	std::string found;
	for (const std::string& id : candidates)
	{
		const std::string dir = root + "/" + id;
		const std::string file = dir + "/" + kMarkerFile;
		struct stat st = {};
		if (stat(file.c_str(), &st) != 0)
			continue;
		bool same = seen.st_ino != 0 && st.st_ino == seen.st_ino && st.st_size == seen.st_size && st.st_mtime == seen.st_mtime;
		if (!same && !m.path.empty())
		{
			std::string text;
			Marker other;
			same = ReadSmall(file, kMarkerMax, text) && ParseMarker(text, other) && other.path == m.path;
		}
		if (same)
		{
			hits++;
			found = dir;
		}
	}
	return hits == 1 ? found : std::string();
}
} // namespace fe::gametile
