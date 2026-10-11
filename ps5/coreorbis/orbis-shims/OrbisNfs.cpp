// PS5 port (vk-285-135, AI-assisted): games on NFS shares, through libnfs. See OrbisNfs.h.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisNfs.h"

#include <nfsc/libnfs.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// The C library's own calls (ld.lld --wrap=<name> sends <name> here as __wrap_<name>, and __real_<name> to the library).
extern "C" {
FILE* __real_fopen(const char* path, const char* mode);
FILE* __real_freopen(const char* path, const char* mode, FILE* fp);
int __real_fclose(FILE* fp);
size_t __real_fread(void* ptr, size_t size, size_t n, FILE* fp);
size_t __real_fwrite(const void* ptr, size_t size, size_t n, FILE* fp);
int __real_fseek(FILE* fp, long off, int whence);
int __real_fseeko(FILE* fp, off_t off, int whence);
long __real_ftell(FILE* fp);
off_t __real_ftello(FILE* fp);
void __real_rewind(FILE* fp);
int __real_feof(FILE* fp);
int __real_ferror(FILE* fp);
int __real_fileno(FILE* fp);
int __real_fflush(FILE* fp);
int __real_fgetc(FILE* fp);
char* __real_fgets(char* s, int n, FILE* fp);
int __real_fputc(int c, FILE* fp);
int __real_fputs(const char* s, FILE* fp);
int __real_vfprintf(FILE* fp, const char* fmt, va_list ap);
int __real_setvbuf(FILE* fp, char* buf, int mode, size_t size);
void __real_flockfile(FILE* fp);
void __real_funlockfile(FILE* fp);
int __real_open(const char* path, int flags, ...);
int __real_close(int fd);
ssize_t __real_read(int fd, void* buf, size_t n);
ssize_t __real_pread(int fd, void* buf, size_t n, off_t off);
off_t __real_lseek(int fd, off_t off, int whence);
int __real_fstat(int fd, struct stat* st);
int __real_stat(const char* path, struct stat* st);
int __real_lstat(const char* path, struct stat* st);
int __real_access(const char* path, int mode);
char* __real_realpath(const char* path, char* resolved);
}

namespace OrbisNfs
{
namespace
{
// ---- limits and times ----
constexpr int kSlots = 64;              // files open at once (FILE and descriptors together)
constexpr int kFdBase = 0x3d000000;     // this file's descriptors: far above any the kernel hands out
constexpr size_t kMinAhead = 64 * 1024; // the read-ahead window: from 64 KB, doubling while reads follow each other...
constexpr size_t kMaxAhead = 1024 * 1024; // ...up to 1 MB; a read this big or bigger goes straight to the caller
constexpr int kIoTimeoutMs = 10000;     // one call to the server (libnfs counts whole seconds)
constexpr double kRetryAfter = 30.0;    // a share that didn't mount isn't tried again sooner (each try can take seconds)
constexpr double kListingSeconds = 30.0; // what a listing says about its folder's names is trusted this long

double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	std::printf("[nfs] %s\n", line);
	__real_fflush(stdout);
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// "/a//b/./c/" -> {"a", "b", "c"}; false when ".." climbs above the first component.
bool Components(const std::string& path, std::vector<std::string>& out)
{
	out.clear();
	size_t at = 0;
	while (at <= path.size())
	{
		size_t end = path.find('/', at);
		if (end == std::string::npos)
			end = path.size();
		const std::string c = path.substr(at, end - at);
		at = end + 1;
		if (c.empty() || c == ".")
			continue;
		if (c == "..")
		{
			if (out.empty())
				return false;
			out.pop_back();
			continue;
		}
		out.push_back(c);
	}
	return true;
}

std::string Join(const std::vector<std::string>& c, size_t from, size_t to)
{
	std::string s;
	for (size_t i = from; i < to; i++)
		s += "/" + c[i];
	return s;
}

// ---- shares ----
struct Share
{
	std::string url;
	Address addr;
	std::string mount_point;
	std::mutex mutex; // the context: one call at a time; also guards everything below
	nfs_context* nfs = nullptr;
	bool mounted = false;
	std::string export_path; // what the server mounted
	std::string base;        // the folder in it the address names ("" or "/PS2")
	int version_used = 0;
	double retry_after = 0;
	std::string state = "not tried yet";

	~Share()
	{
		// The last holder is gone: the share list replaced it and its last file closed.
		if (nfs)
			nfs_destroy_context(nfs);
	}
};

std::mutex g_shares_mutex;
std::vector<std::shared_ptr<Share>> g_shares;
std::atomic<int> g_share_count{0};

std::atomic<uint64_t> g_opens{0}, g_reads{0}, g_rpcs{0}, g_bytes{0}, g_cache_hits{0}, g_lookups{0};

bool Active()
{
	return g_share_count.load(std::memory_order_relaxed) > 0;
}

// A path below /nfs: the share it's on (the longest mount point that holds it) and the rest, or a folder above shares.
struct Where
{
	std::string normal;              // "/nfs/192.168.1.10/volume1/PS2/x.iso"
	std::shared_ptr<Share> share;    // null for a folder above shares
	std::string rest;                // below the share's mount point: "" or "/x.iso"
	std::vector<std::string> children; // a folder above shares: what's in it
};

bool Resolve(const char* path, Where& w)
{
	std::vector<std::string> c;
	if (!Components(path, c) || c.empty() || c[0] != "nfs")
		return false;
	w.normal = Join(c, 0, c.size());
	std::lock_guard<std::mutex> lock(g_shares_mutex);
	size_t best = 0;
	for (const std::shared_ptr<Share>& s : g_shares)
	{
		const std::string& mp = s->mount_point;
		if (w.normal == mp || (w.normal.size() > mp.size() && w.normal.compare(0, mp.size(), mp) == 0 && w.normal[mp.size()] == '/'))
		{
			if (mp.size() > best)
			{
				best = mp.size();
				w.share = s;
				w.rest = w.normal.substr(mp.size());
			}
		}
		else if (mp.size() > w.normal.size() && mp.compare(0, w.normal.size(), w.normal) == 0 && mp[w.normal.size()] == '/')
		{
			const size_t start = w.normal.size() + 1, end = mp.find('/', start);
			const std::string child = mp.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (std::find(w.children.begin(), w.children.end(), child) == w.children.end())
				w.children.push_back(child);
		}
	}
	if (w.share)
		w.children.clear();
	return w.share || !w.children.empty();
}

// The path to give libnfs, on the mounted share.
std::string Inner(const Share& s, const std::string& rest)
{
	const std::string p = s.base + rest;
	return p.empty() ? std::string("/") : p;
}

nfs_context* NewContext(const Address& a, int version, int timeout_ms)
{
	nfs_context* nfs = nfs_init_context();
	if (!nfs)
		return nullptr;
	nfs_set_version(nfs, version == 4 ? 4 : 3); // NFS_V4 and NFS_V3, defined in the raw protocol headers
	if (a.nfsport)
		nfs_set_nfsport(nfs, a.nfsport);
	if (a.mountport)
		nfs_set_mountport(nfs, a.mountport);
	if (a.uid >= 0)
		nfs_set_uid(nfs, a.uid);
	if (a.gid >= 0)
		nfs_set_gid(nfs, a.gid);
	// No look for exports mounted inside the share (an extra MOUNT call some servers don't answer, go-nfs among them).
	nfs_set_auto_traverse_mounts(nfs, 0);
	nfs_set_timeout(nfs, std::max(1000, timeout_ms));
	return nfs;
}

// nfs_mount with a time limit. libnfs's own limit (nfs_set_timeout) counts from its first request, so a server that
// never answers the connection held the sync nfs_mount until the kernel gave up on it (the PC test: 135 s a try).
// The async mount, served here until it ends or `timeout_ms` passes.
struct MountWait
{
	bool done = false;
	int status = 0;
	std::string error;
};

void MountDone(int status, nfs_context* nfs, void* data, void* private_data)
{
	MountWait* w = static_cast<MountWait*>(private_data);
	w->done = true;
	w->status = status;
	if (status != 0)
	{
		const char* d = static_cast<const char*>(data);
		const char* e = nfs_get_error(nfs);
		if (d && *d)
			w->error = d;
		else if (e && *e)
			w->error = e;
		else
			w->error = "the connection failed (error " + std::to_string(status) + ")";
	}
}

int MountTimed(nfs_context* nfs, const std::string& host, const std::string& exp, int timeout_ms, std::string& error)
{
	MountWait w;
	if (nfs_mount_async(nfs, host.c_str(), exp.c_str(), MountDone, &w) != 0)
	{
		error = nfs_get_error(nfs) ? nfs_get_error(nfs) : "the mount didn't start";
		return -EIO;
	}
	const double deadline = Now() + timeout_ms / 1000.0;
	while (!w.done)
	{
		const double left = deadline - Now();
		if (left <= 0)
		{
			char why[96];
			std::snprintf(why, sizeof(why), "no answer in %.1f s", timeout_ms / 1000.0);
			error = why;
			return -ETIMEDOUT;
		}
		pollfd p = {nfs_get_fd(nfs), static_cast<short>(nfs_which_events(nfs)), 0};
		const int r = poll(&p, 1, std::max(1, std::min(100, static_cast<int>(left * 1000.0))));
		if (r < 0 && errno != EINTR)
		{
			error = "poll failed (errno " + std::to_string(errno) + ")";
			return -EIO;
		}
		if (nfs_service(nfs, r > 0 ? p.revents : 0) < 0 && !w.done)
		{
			error = nfs_get_error(nfs) ? nfs_get_error(nfs) : "the connection failed";
			return -EIO;
		}
	}
	if (w.status != 0)
		error = w.error;
	return w.status;
}

// Mounts `s` if it isn't (its mutex held). The address's path is tried whole, then shorter while the server refuses
// it (a FreeBSD server exports only the folder itself unless -alldirs), keeping the rest as the folder inside. NFS v4
// when v3 got no answer at all and the address didn't ask for a version.
bool MountLocked(Share& s, int timeout_ms)
{
	if (s.mounted)
		return true;
	const double t0 = Now();
	if (t0 < s.retry_after)
		return false;
	std::vector<std::string> comps;
	Components(s.addr.path, comps);
	std::string why;
	bool answered = false; // the server's MOUNT service said something (a refusal), so v4 wouldn't help
	bool silent = false;   // nothing answered at all (no refusal of the connection either): v4 wouldn't get further
	const std::vector<int> versions = s.addr.version == 4 ? std::vector<int>{4} : s.addr.version == 3 ? std::vector<int>{3}
	                                                                                                   : std::vector<int>{3, 4};
	for (const int version : versions)
	{
		if (version == 4 && (answered || silent) && s.addr.version == 0)
			break;
		for (size_t keep = comps.size();; keep--)
		{
			const std::string exp = keep ? Join(comps, 0, keep) : std::string("/");
			const std::string base = Join(comps, keep, comps.size());
			nfs_context* nfs = NewContext(s.addr, version, timeout_ms);
			if (!nfs)
			{
				why = "no memory for an NFS context";
				break;
			}
			std::string err;
			int rc = MountTimed(nfs, s.addr.host, exp, timeout_ms, err);
			if (rc == 0 && !base.empty())
			{
				nfs_stat_64 st = {};
				if (nfs_stat64(nfs, base.c_str(), &st) != 0 || !S_ISDIR(st.nfs_mode))
				{
					rc = -ENOENT;
					err = "there is no folder " + base + " in " + exp;
				}
			}
			g_rpcs++;
			if (rc == 0)
			{
				nfs_set_timeout(nfs, kIoTimeoutMs);
				s.nfs = nfs;
				s.mounted = true;
				s.export_path = exp;
				s.base = base;
				s.version_used = version;
				char state[512];
				std::snprintf(state, sizeof(state), "mounted %s (NFS v%d)%s%s in %.2f s", exp.c_str(), version,
					base.empty() ? "" : ", folder ", base.c_str(), Now() - t0);
				s.state = state;
				Log("%s: %s, its files under %s", s.url.c_str(), state, s.mount_point.c_str());
				return true;
			}
			nfs_destroy_context(nfs);
			// A refusal comes from the server's MOUNT service (MNT3ERR_*); anything else is the network or the server's
			// absence, and a shorter path wouldn't change that.
			const bool refused = err.find("Mount failed with error") != std::string::npos || err.find("there is no folder") == 0 ||
			                     err.find("NFS4ERR") != std::string::npos;
			answered = answered || refused;
			silent = silent || rc == -ETIMEDOUT;
			const std::string this_why = "NFS v" + std::to_string(version) + " " + exp + ": " + err;
			Log("%s: %s", s.url.c_str(), this_why.c_str());
			// The state names the first try's error (the whole path, v3) and the last's when they differ.
			if (why.empty())
				why = this_why;
			else if (why.find(this_why) == std::string::npos)
				why = why.substr(0, why.find("; then ")) + "; then " + this_why;
			if (!refused || keep == 0 || version == 4)
				break;
		}
	}
	s.retry_after = Now() + kRetryAfter;
	s.state = "not mounted: " + why;
	return false;
}

// ---- what listings said: a folder's names and their attributes, for stat() without asking the server again ----
struct Attr
{
	uint64_t mode = 0, size = 0, ino = 0, nlink = 1, uid = 0, gid = 0;
	int64_t mtime = 0;
	uint32_t mtime_nsec = 0;
};
struct Listing
{
	double until = 0;
	std::unordered_map<std::string, Attr> names;
};
std::mutex g_cache_mutex;
std::unordered_map<std::string, Listing>& Listings()
{
	static std::unordered_map<std::string, Listing> listings; // made on first use (the wrappers can run before main)
	return listings;
}

// 1: known (in `a`); 0: known not to be there; -1: not known.
int Cached(const std::string& normal, Attr& a)
{
	const size_t slash = normal.rfind('/');
	if (slash == std::string::npos || slash == 0)
		return -1;
	std::lock_guard<std::mutex> lock(g_cache_mutex);
	auto& listings = Listings();
	const auto it = listings.find(normal.substr(0, slash));
	if (it == listings.end())
		return -1;
	if (Now() > it->second.until)
	{
		listings.erase(it);
		return -1;
	}
	const auto e = it->second.names.find(normal.substr(slash + 1));
	g_cache_hits++;
	if (e == it->second.names.end())
		return 0;
	a = e->second;
	return 1;
}

void Remember(const std::string& dir, std::unordered_map<std::string, Attr>&& names)
{
	std::lock_guard<std::mutex> lock(g_cache_mutex);
	auto& listings = Listings();
	if (listings.size() > 4096)
		listings.clear();
	Listing& l = listings[dir];
	l.until = Now() + kListingSeconds;
	l.names = std::move(names);
}

Attr FromNfs(const nfs_stat_64& st)
{
	Attr a;
	a.mode = st.nfs_mode;
	a.size = st.nfs_size;
	a.ino = st.nfs_ino;
	a.nlink = st.nfs_nlink;
	a.uid = st.nfs_uid;
	a.gid = st.nfs_gid;
	a.mtime = static_cast<int64_t>(st.nfs_mtime);
	a.mtime_nsec = static_cast<uint32_t>(st.nfs_mtime_nsec);
	return a;
}

void Fill(struct stat* st, const Attr& a)
{
	std::memset(st, 0, sizeof(*st));
	// Read-only for everyone here: nothing on a share can be written through this file.
	st->st_mode = static_cast<mode_t>((a.mode & S_IFMT) | (a.mode & 0555));
	st->st_size = static_cast<off_t>(a.size);
	st->st_ino = static_cast<ino_t>(a.ino);
	st->st_nlink = static_cast<nlink_t>(a.nlink ? a.nlink : 1);
	st->st_uid = static_cast<uid_t>(a.uid);
	st->st_gid = static_cast<gid_t>(a.gid);
	st->st_mtim.tv_sec = static_cast<time_t>(a.mtime);
	st->st_mtim.tv_nsec = static_cast<long>(a.mtime_nsec);
	st->st_atim = st->st_mtim;
	st->st_ctim = st->st_mtim;
	st->st_blksize = 65536;
	st->st_blocks = static_cast<blkcnt_t>((a.size + 511) / 512);
}

void FillFolder(struct stat* st)
{
	Attr a;
	a.mode = S_IFDIR | 0555;
	a.nlink = 2;
	Fill(st, a);
}

int StatPath(const char* path, struct stat* st, bool follow)
{
	Where w;
	if (!Resolve(path, w))
	{
		errno = ENOENT;
		return -1;
	}
	if (!w.share)
	{
		FillFolder(st);
		return 0;
	}
	Attr a;
	const int known = w.rest.empty() ? -1 : Cached(w.normal, a);
	if (known == 1)
	{
		Fill(st, a);
		return 0;
	}
	if (known == 0)
	{
		errno = ENOENT;
		return -1;
	}
	Share& s = *w.share;
	std::lock_guard<std::mutex> lock(s.mutex);
	if (!MountLocked(s, kIoTimeoutMs))
	{
		errno = EIO;
		return -1;
	}
	nfs_stat_64 ns = {};
	const std::string inner = Inner(s, w.rest);
	g_lookups++;
	g_rpcs++;
	const int rc = follow ? nfs_stat64(s.nfs, inner.c_str(), &ns) : nfs_lstat64(s.nfs, inner.c_str(), &ns);
	if (rc != 0)
	{
		errno = rc < 0 ? -rc : EIO;
		return -1;
	}
	Fill(st, FromNfs(ns));
	return 0;
}

// ---- folders ----
struct DirItem
{
	std::string name;
	unsigned char type = DT_UNKNOWN;
	uint64_t ino = 0;
};
struct NfsDir
{
	std::vector<DirItem> items;
	size_t next = 0;
};

unsigned char TypeOf(uint64_t mode)
{
	switch (mode & S_IFMT)
	{
		case S_IFDIR:
			return DT_DIR;
		case S_IFREG:
			return DT_REG;
		case S_IFLNK:
			return DT_LNK;
		default:
			return DT_UNKNOWN;
	}
}

// ---- files ----
struct NfsFile
{
	std::mutex mutex;
	std::shared_ptr<Share> share;
	nfsfh* fh = nullptr;
	std::string path;
	bool is_fd = false;
	Attr attr;
	uint64_t pos = 0;
	bool eof = false, error = false;
	std::vector<uint8_t> window;
	uint64_t window_at = 0;
	size_t window_len = 0;
	uint64_t last_end = UINT64_MAX;
	size_t ahead = kMinAhead;
	uint64_t reads = 0, bytes = 0, fetches = 0;
	double fetch_seconds = 0;
	int failures_logged = 0;
};

FILE g_files[kSlots];          // what this file's FILE pointers point at (never the C library's)
std::mutex g_slots_mutex;
std::shared_ptr<NfsFile> g_slots[kSlots];

int SlotOfFile(const FILE* fp)
{
	const uintptr_t p = reinterpret_cast<uintptr_t>(fp), first = reinterpret_cast<uintptr_t>(&g_files[0]);
	if (p < first || p >= first + sizeof(g_files) || (p - first) % sizeof(FILE) != 0)
		return -1;
	return static_cast<int>((p - first) / sizeof(FILE));
}

int SlotOfFd(int fd)
{
	return fd >= kFdBase && fd < kFdBase + kSlots ? fd - kFdBase : -1;
}

std::shared_ptr<NfsFile> InSlot(int slot)
{
	std::lock_guard<std::mutex> lock(g_slots_mutex);
	return g_slots[slot];
}

// The C library's macros read a FILE's own flags in C code (feof, ferror): kept in step with the file's.
void MirrorFlags(int slot, const NfsFile& f)
{
#if defined(__PROSPERO__)
	FILE& fp = g_files[slot];
	fp._flags = static_cast<short>((fp._flags & ~(__SEOF | __SERR)) | (f.eof ? __SEOF : 0) | (f.error ? __SERR : 0));
#else
	(void)slot;
	(void)f;
#endif
}

// `n` bytes at `off` from the server, straight into `dst` (fewer at the end of the file; -1 when nothing came).
ssize_t Fetch(NfsFile& f, uint8_t* dst, size_t n, uint64_t off)
{
	Share& s = *f.share;
	std::lock_guard<std::mutex> lock(s.mutex);
	const double t0 = Now();
	size_t got = 0;
	while (got < n)
	{
		// nfs_pread answers at most the server's read size (often 64 KB to 1 MB) a call.
		const int r = nfs_pread(s.nfs, f.fh, dst + got, n - got, off + got);
		g_rpcs++;
		if (r < 0)
		{
			if (f.failures_logged++ < 5)
				Log("reading %s at %llu: %s", f.path.c_str(), static_cast<unsigned long long>(off + got),
					nfs_get_error(s.nfs) ? nfs_get_error(s.nfs) : "?");
			f.error = true;
			errno = -r > 0 ? -r : EIO;
			break;
		}
		if (r == 0)
			break;
		got += static_cast<size_t>(r);
	}
	f.fetches++;
	f.fetch_seconds += Now() - t0;
	g_bytes += got;
	return got ? static_cast<ssize_t>(got) : (f.error ? -1 : 0);
}

// Up to `n` bytes at `off` (the file's mutex held): from the window when it has them, else the server.
ssize_t ReadAt(NfsFile& f, uint8_t* dst, size_t n, uint64_t off)
{
	f.reads++;
	g_reads++;
	if (n == 0 || off >= f.attr.size)
		return 0;
	if (n > f.attr.size - off)
		n = static_cast<size_t>(f.attr.size - off);
	f.ahead = off == f.last_end ? std::min(f.ahead * 2, kMaxAhead) : kMinAhead;
	size_t done = 0;
	while (done < n)
	{
		const uint64_t at = off + done;
		if (f.window_len && at >= f.window_at && at < f.window_at + f.window_len)
		{
			const size_t k = static_cast<size_t>(std::min<uint64_t>(n - done, f.window_at + f.window_len - at));
			std::memcpy(dst + done, f.window.data() + (at - f.window_at), k);
			done += k;
			continue;
		}
		const size_t left = n - done;
		if (left >= kMaxAhead)
		{
			const ssize_t got = Fetch(f, dst + done, left, at);
			if (got <= 0)
				break;
			done += static_cast<size_t>(got);
			if (static_cast<size_t>(got) < left)
				break;
			continue;
		}
		const size_t want = static_cast<size_t>(std::min<uint64_t>(std::min(std::max(left, f.ahead), kMaxAhead), f.attr.size - at));
		if (f.window.size() < kMaxAhead)
			f.window.resize(kMaxAhead);
		const ssize_t got = Fetch(f, f.window.data(), want, at);
		if (got <= 0)
		{
			f.window_len = 0;
			break;
		}
		f.window_at = at;
		f.window_len = static_cast<size_t>(got);
	}
	f.last_end = off + done;
	f.bytes += done;
	if (done == 0 && f.error)
		return -1;
	return static_cast<ssize_t>(done);
}

// Opens `path` for reading into a free slot (its number), or -1 and errno.
int OpenFile(const char* path, bool is_fd)
{
	Where w;
	if (!Resolve(path, w))
	{
		errno = ENOENT;
		return -1;
	}
	if (!w.share)
	{
		errno = EISDIR;
		return -1;
	}
	auto f = std::make_shared<NfsFile>();
	f->share = w.share;
	f->path = w.normal;
	f->is_fd = is_fd;
	{
		Share& s = *w.share;
		std::lock_guard<std::mutex> lock(s.mutex);
		if (!MountLocked(s, kIoTimeoutMs))
		{
			errno = EIO;
			return -1;
		}
		const std::string inner = Inner(s, w.rest);
		nfsfh* fh = nullptr;
		g_rpcs++;
		int rc = nfs_open(s.nfs, inner.c_str(), O_RDONLY, &fh);
		if (rc != 0)
		{
			errno = rc < 0 ? -rc : EIO;
			return -1;
		}
		nfs_stat_64 st = {};
		rc = nfs_fstat64(s.nfs, fh, &st);
		if (rc != 0 || S_ISDIR(st.nfs_mode))
		{
			nfs_close(s.nfs, fh);
			errno = rc != 0 ? (rc < 0 ? -rc : EIO) : EISDIR;
			return -1;
		}
		f->fh = fh;
		f->attr = FromNfs(st);
	}
	int slot = -1;
	{
		std::lock_guard<std::mutex> lock(g_slots_mutex);
		for (int i = 0; i < kSlots; i++)
		{
			if (!g_slots[i])
			{
				slot = i;
				g_slots[i] = f;
				break;
			}
		}
	}
	if (slot < 0)
	{
		std::lock_guard<std::mutex> lock(f->share->mutex);
		nfs_close(f->share->nfs, f->fh);
		Log("too many files open on shares (%d): %s not opened", kSlots, f->path.c_str());
		errno = EMFILE;
		return -1;
	}
#if defined(__PROSPERO__)
	std::memset(&g_files[slot], 0, sizeof(FILE));
	g_files[slot]._flags = __SRD;
	g_files[slot]._file = -1;
#endif
	g_opens++;
	Log("opened %s (%llu bytes)", f->path.c_str(), static_cast<unsigned long long>(f->attr.size));
	return slot;
}

int CloseSlot(int slot)
{
	std::shared_ptr<NfsFile> f;
	{
		std::lock_guard<std::mutex> lock(g_slots_mutex);
		f = std::move(g_slots[slot]);
		g_slots[slot].reset();
	}
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> flock(f->mutex);
	{
		std::lock_guard<std::mutex> lock(f->share->mutex);
		if (f->fh)
			nfs_close(f->share->nfs, f->fh);
		f->fh = nullptr;
	}
	if (f->reads)
		Log("closed %s: %llu reads, %.1f MB in %llu requests, %.2f s waiting (%.1f MB/s)", f->path.c_str(),
			static_cast<unsigned long long>(f->reads), static_cast<double>(f->bytes) / 1048576.0,
			static_cast<unsigned long long>(f->fetches), f->fetch_seconds,
			f->fetch_seconds > 0 ? static_cast<double>(f->bytes) / 1048576.0 / f->fetch_seconds : 0.0);
	return 0;
}

bool Seek(NfsFile& f, int64_t off, int whence)
{
	int64_t base = 0;
	if (whence == SEEK_CUR)
		base = static_cast<int64_t>(f.pos);
	else if (whence == SEEK_END)
		base = static_cast<int64_t>(f.attr.size);
	else if (whence != SEEK_SET)
	{
		errno = EINVAL;
		return false;
	}
	const int64_t to = base + off;
	if (to < 0)
	{
		errno = EINVAL;
		return false;
	}
	f.pos = static_cast<uint64_t>(to);
	f.eof = false;
	return true;
}

bool ReadOnlyMode(const char* mode)
{
	return mode && std::strpbrk(mode, "wa+") == nullptr;
}

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
		a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
		b--;
	return s.substr(a, b - a);
}

int Hex(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool Number(const std::string& s, long lo, long hi, long& out)
{
	if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string::npos)
		return false;
	out = std::strtol(s.c_str(), nullptr, 10);
	return out >= lo && out <= hi;
}
} // namespace

bool ParseAddress(const std::string& url_in, Address& out, std::string& error)
{
	out = Address();
	const std::string url = Trim(url_in);
	if (url.size() < 7 || Lower(url.substr(0, 6)) != "nfs://")
	{
		error = "doesn't start with nfs://";
		return false;
	}
	size_t at = 6;
	const size_t end_auth = url.find_first_of("/?", at);
	std::string auth = url.substr(at, end_auth == std::string::npos ? std::string::npos : end_auth - at);
	if (auth.find('@') != std::string::npos || (!auth.empty() && auth[0] == '['))
	{
		error = "user names and IPv6 addresses aren't supported";
		return false;
	}
	const size_t colon = auth.rfind(':');
	if (colon != std::string::npos)
	{
		long port = 0;
		if (!Number(auth.substr(colon + 1), 1, 65535, port))
		{
			error = "the port after ':' isn't a number from 1 to 65535";
			return false;
		}
		out.nfsport = static_cast<uint16_t>(port);
		auth.resize(colon);
	}
	if (auth.empty() || auth.size() > 253 ||
		auth.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
	{
		error = "no server name or IP address after nfs://";
		return false;
	}
	out.host = auth;
	at = end_auth == std::string::npos ? url.size() : end_auth;
	const size_t q = url.find('?', at);
	std::string raw = url.substr(at, q == std::string::npos ? std::string::npos : q - at);
	std::string path;
	for (size_t i = 0; i < raw.size(); i++)
	{
		if (raw[i] == '%' && i + 2 < raw.size() && Hex(raw[i + 1]) >= 0 && Hex(raw[i + 2]) >= 0)
		{
			path += static_cast<char>(Hex(raw[i + 1]) * 16 + Hex(raw[i + 2]));
			i += 2;
		}
		else
			path += raw[i];
	}
	std::vector<std::string> comps;
	if (path.find('\0') != std::string::npos || !Components(path, comps))
	{
		error = "the path climbs above the server's root";
		return false;
	}
	out.path = comps.empty() ? std::string("/") : Join(comps, 0, comps.size());
	if (q != std::string::npos)
	{
		std::string query = url.substr(q + 1);
		size_t p = 0;
		while (p <= query.size())
		{
			size_t amp = query.find('&', p);
			if (amp == std::string::npos)
				amp = query.size();
			const std::string kv = query.substr(p, amp - p);
			p = amp + 1;
			if (kv.empty())
				continue;
			const size_t eq = kv.find('=');
			const std::string key = Lower(kv.substr(0, eq));
			const std::string value = eq == std::string::npos ? std::string() : kv.substr(eq + 1);
			long v = 0;
			if (key == "version" && (value == "3" || value == "4"))
				out.version = value == "4" ? 4 : 3;
			else if (key == "nfsport" && Number(value, 1, 65535, v))
				out.nfsport = static_cast<uint16_t>(v);
			else if (key == "mountport" && Number(value, 1, 65535, v))
				out.mountport = static_cast<uint16_t>(v);
			else if (key == "uid" && Number(value, 0, 0x7fffffff, v))
				out.uid = static_cast<int>(v);
			else if (key == "gid" && Number(value, 0, 0x7fffffff, v))
				out.gid = static_cast<int>(v);
			else
			{
				error = "\"" + kv + "\" isn't one of version=3|4, nfsport=, mountport=, uid=, gid=";
				return false;
			}
		}
	}
	return true;
}

std::string MountPointOf(const Address& a)
{
	return "/nfs/" + a.host + (a.path == "/" ? std::string() : a.path);
}

std::vector<std::string> SetShares(const std::string& list, std::vector<std::string>* problems)
{
	std::vector<std::shared_ptr<Share>> shares;
	std::vector<std::string> points;
	size_t at = 0;
	while (at <= list.size())
	{
		size_t end = list.find_first_of(";\n", at);
		if (end == std::string::npos)
			end = list.size();
		const std::string item = Trim(list.substr(at, end - at));
		at = end + 1;
		if (item.empty())
			continue;
		auto s = std::make_shared<Share>();
		std::string error;
		if (!ParseAddress(item, s->addr, error))
		{
			Log("share \"%s\" left out: %s", item.c_str(), error.c_str());
			if (problems)
				problems->push_back(item + ": " + error);
			continue;
		}
		s->url = item;
		s->mount_point = MountPointOf(s->addr);
		if (std::find(points.begin(), points.end(), s->mount_point) != points.end())
			continue;
		points.push_back(s->mount_point);
		shares.push_back(std::move(s));
	}
	{
		std::lock_guard<std::mutex> lock(g_shares_mutex);
		g_shares = std::move(shares); // shares still holding open files stay alive with them
		g_share_count.store(static_cast<int>(g_shares.size()));
	}
	{
		std::lock_guard<std::mutex> lock(g_cache_mutex);
		Listings().clear();
	}
	return points;
}

std::vector<ShareInfo> MountAll(int timeout_ms)
{
	std::vector<std::shared_ptr<Share>> shares;
	{
		std::lock_guard<std::mutex> lock(g_shares_mutex);
		shares = g_shares;
	}
	std::vector<std::thread> threads;
	for (const std::shared_ptr<Share>& s : shares)
	{
		threads.emplace_back([s, timeout_ms]() {
			std::lock_guard<std::mutex> lock(s->mutex);
			s->retry_after = 0; // asked for now
			MountLocked(*s, timeout_ms);
		});
	}
	for (std::thread& t : threads)
		t.join();
	return Shares();
}

std::vector<ShareInfo> Shares()
{
	std::vector<std::shared_ptr<Share>> shares;
	{
		std::lock_guard<std::mutex> lock(g_shares_mutex);
		shares = g_shares;
	}
	std::vector<ShareInfo> out;
	for (const std::shared_ptr<Share>& s : shares)
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		out.push_back({s->url, s->mount_point, s->state, s->mounted});
	}
	return out;
}

bool IsPath(const char* path)
{
	return path && Active() && std::strncmp(path, "/nfs", 4) == 0 && (path[4] == '\0' || path[4] == '/');
}

void* OpenDir(const char* path)
{
	Where w;
	if (!Resolve(path, w))
	{
		errno = ENOENT;
		return nullptr;
	}
	auto d = std::make_unique<NfsDir>();
	if (!w.share)
	{
		for (const std::string& c : w.children)
			d->items.push_back({c, DT_DIR, 0});
		return d.release();
	}
	Share& s = *w.share;
	std::unordered_map<std::string, Attr> names;
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		if (!MountLocked(s, kIoTimeoutMs))
		{
			errno = EIO;
			return nullptr;
		}
		const std::string inner = Inner(s, w.rest);
		nfsdir* nd = nullptr;
		g_rpcs++;
		const int rc = nfs_opendir(s.nfs, inner.c_str(), &nd);
		if (rc != 0)
		{
			errno = rc < 0 ? -rc : EIO;
			return nullptr;
		}
		while (const nfsdirent* e = nfs_readdir(s.nfs, nd))
		{
			if (!e->name || !std::strcmp(e->name, ".") || !std::strcmp(e->name, ".."))
				continue;
			Attr a;
			a.mode = e->mode;
			a.size = e->size;
			a.ino = e->inode;
			a.nlink = e->nlink;
			a.uid = e->uid;
			a.gid = e->gid;
			a.mtime = e->mtime.tv_sec;
			a.mtime_nsec = e->mtime_nsec;
			d->items.push_back({e->name, TypeOf(e->mode), e->inode});
			names.emplace(e->name, a);
		}
		nfs_closedir(s.nfs, nd);
	}
	Remember(w.normal, std::move(names));
	return d.release();
}

bool ReadDir(void* dir, struct dirent* out)
{
	NfsDir* d = static_cast<NfsDir*>(dir);
	if (!d || d->next >= d->items.size())
		return false;
	const DirItem& it = d->items[d->next++];
	std::memset(out, 0, sizeof(*out));
	const size_t n = std::min(it.name.size(), sizeof(out->d_name) - 1);
	std::memcpy(out->d_name, it.name.data(), n);
#if defined(__PROSPERO__)
	out->d_fileno = static_cast<uint32_t>(it.ino ? it.ino : d->next);
	out->d_namlen = static_cast<uint8_t>(n);
#else
	out->d_ino = static_cast<ino_t>(it.ino ? it.ino : d->next);
#endif
	out->d_reclen = sizeof(*out);
	out->d_type = it.type;
	return true;
}

void CloseDir(void* dir)
{
	delete static_cast<NfsDir*>(dir);
}

Stats GetStats()
{
	Stats s;
	s.opens = g_opens;
	s.reads = g_reads;
	s.rpcs = g_rpcs;
	s.bytes = g_bytes;
	s.cache_hits = g_cache_hits;
	s.lookups = g_lookups;
	return s;
}
} // namespace OrbisNfs

// ---- the wrapped calls ----
using namespace OrbisNfs;

extern "C" {
FILE* __wrap_fopen(const char* path, const char* mode)
{
	if (!IsPath(path))
		return __real_fopen(path, mode);
	if (!ReadOnlyMode(mode))
	{
		errno = EROFS;
		return nullptr;
	}
	const int slot = OpenFile(path, false);
	return slot < 0 ? nullptr : &g_files[slot];
}

FILE* __wrap_freopen(const char* path, const char* mode, FILE* fp)
{
	if (SlotOfFile(fp) >= 0 || IsPath(path))
	{
		errno = ENOTSUP;
		return nullptr;
	}
	return __real_freopen(path, mode, fp);
}

int __wrap_fclose(FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_fclose(fp);
	return CloseSlot(slot) == 0 ? 0 : EOF;
}

size_t __wrap_fread(void* ptr, size_t size, size_t n, FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_fread(ptr, size, n, fp);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f || size == 0 || n == 0)
		return 0;
	if (n > SIZE_MAX / size)
	{
		errno = EOVERFLOW;
		return 0;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	const size_t want = size * n;
	const ssize_t got = ReadAt(*f, static_cast<uint8_t*>(ptr), want, f->pos);
	if (got > 0)
		f->pos += static_cast<uint64_t>(got);
	if (got >= 0 && static_cast<size_t>(got) < want && !f->error)
		f->eof = true;
	MirrorFlags(slot, *f);
	return got > 0 ? static_cast<size_t>(got) / size : 0;
}

size_t __wrap_fwrite(const void* ptr, size_t size, size_t n, FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_fwrite(ptr, size, n, fp);
	errno = EBADF;
	return 0;
}

int __wrap_fseeko(FILE* fp, off_t off, int whence)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_fseeko(fp, off, whence);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	const bool ok = Seek(*f, static_cast<int64_t>(off), whence);
	MirrorFlags(slot, *f);
	return ok ? 0 : -1;
}

int __wrap_fseek(FILE* fp, long off, int whence)
{
	if (SlotOfFile(fp) < 0)
		return __real_fseek(fp, off, whence);
	return __wrap_fseeko(fp, static_cast<off_t>(off), whence);
}

off_t __wrap_ftello(FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_ftello(fp);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	return static_cast<off_t>(f->pos);
}

long __wrap_ftell(FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		return __real_ftell(fp);
	const off_t pos = __wrap_ftello(fp);
	if (pos > static_cast<off_t>(LONG_MAX))
	{
		errno = EOVERFLOW;
		return -1;
	}
	return static_cast<long>(pos);
}

void __wrap_rewind(FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
	{
		__real_rewind(fp);
		return;
	}
	if (const std::shared_ptr<NfsFile> f = InSlot(slot))
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		f->pos = 0;
		f->eof = f->error = false;
		MirrorFlags(slot, *f);
	}
}

int __wrap_feof(FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_feof(fp);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	return f && f->eof ? 1 : 0;
}

int __wrap_ferror(FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_ferror(fp);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	return f && f->error ? 1 : 0;
}

int __wrap_fileno(FILE* fp)
{
	const int slot = SlotOfFile(fp);
	if (slot < 0)
		return __real_fileno(fp);
	return kFdBase + slot; // fstat() on it answers; close() on it doesn't close the FILE
}

int __wrap_fflush(FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		return __real_fflush(fp);
	return 0;
}

int __wrap_setvbuf(FILE* fp, char* buf, int mode, size_t size)
{
	if (SlotOfFile(fp) < 0)
		return __real_setvbuf(fp, buf, mode, size);
	return 0;
}

void __wrap_flockfile(FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		__real_flockfile(fp);
}

void __wrap_funlockfile(FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		__real_funlockfile(fp);
}

int __wrap_fgetc(FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		return __real_fgetc(fp);
	unsigned char c = 0;
	return __wrap_fread(&c, 1, 1, fp) == 1 ? c : EOF;
}

char* __wrap_fgets(char* s, int n, FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		return __real_fgets(s, n, fp);
	if (!s || n <= 0)
	{
		errno = EINVAL;
		return nullptr;
	}
	int i = 0;
	while (i < n - 1)
	{
		const int c = __wrap_fgetc(fp);
		if (c == EOF)
			break;
		s[i++] = static_cast<char>(c);
		if (c == '\n')
			break;
	}
	if (i == 0)
		return nullptr;
	s[i] = '\0';
	return s;
}

int __wrap_fputc(int c, FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		return __real_fputc(c, fp);
	errno = EBADF;
	return EOF;
}

int __wrap_fputs(const char* s, FILE* fp)
{
	if (SlotOfFile(fp) < 0)
		return __real_fputs(s, fp);
	errno = EBADF;
	return EOF;
}

int __wrap_vfprintf(FILE* fp, const char* fmt, va_list ap)
{
	if (SlotOfFile(fp) < 0)
		return __real_vfprintf(fp, fmt, ap);
	errno = EBADF;
	return -1;
}

int __wrap_fprintf(FILE* fp, const char* fmt, ...)
{
	if (SlotOfFile(fp) >= 0)
	{
		errno = EBADF;
		return -1;
	}
	va_list ap;
	va_start(ap, fmt);
	const int r = __real_vfprintf(fp, fmt, ap);
	va_end(ap);
	return r;
}

int __wrap_open(const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT)
	{
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	if (!IsPath(path))
		return __real_open(path, flags, mode);
	if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND)))
	{
		errno = EROFS;
		return -1;
	}
	if (flags & O_DIRECTORY)
	{
		errno = ENOTSUP;
		return -1;
	}
	const int slot = OpenFile(path, true);
	return slot < 0 ? -1 : kFdBase + slot;
}

int __wrap_close(int fd)
{
	const int slot = SlotOfFd(fd);
	if (slot < 0)
		return __real_close(fd);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f || !f->is_fd)
	{
		errno = EBADF; // a FILE's number from fileno(): fclose() closes it
		return -1;
	}
	return CloseSlot(slot);
}

ssize_t __wrap_read(int fd, void* buf, size_t n)
{
	const int slot = SlotOfFd(fd);
	if (slot < 0)
		return __real_read(fd, buf, n);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	const ssize_t got = ReadAt(*f, static_cast<uint8_t*>(buf), n, f->pos);
	if (got > 0)
		f->pos += static_cast<uint64_t>(got);
	return got;
}

ssize_t __wrap_pread(int fd, void* buf, size_t n, off_t off)
{
	const int slot = SlotOfFd(fd);
	if (slot < 0)
		return __real_pread(fd, buf, n, off);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f || off < 0)
	{
		errno = f ? EINVAL : EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	return ReadAt(*f, static_cast<uint8_t*>(buf), n, static_cast<uint64_t>(off));
}

off_t __wrap_lseek(int fd, off_t off, int whence)
{
	const int slot = SlotOfFd(fd);
	if (slot < 0)
		return __real_lseek(fd, off, whence);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	if (!Seek(*f, static_cast<int64_t>(off), whence))
		return -1;
	return static_cast<off_t>(f->pos);
}

int __wrap_fstat(int fd, struct stat* st)
{
	const int slot = SlotOfFd(fd);
	if (slot < 0)
		return __real_fstat(fd, st);
	const std::shared_ptr<NfsFile> f = InSlot(slot);
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	Fill(st, f->attr);
	return 0;
}

int __wrap_stat(const char* path, struct stat* st)
{
	if (!IsPath(path))
		return __real_stat(path, st);
	return StatPath(path, st, true);
}

int __wrap_lstat(const char* path, struct stat* st)
{
	if (!IsPath(path))
		return __real_lstat(path, st);
	return StatPath(path, st, false);
}

int __wrap_access(const char* path, int mode)
{
	if (!IsPath(path))
		return __real_access(path, mode);
	struct stat st;
	if (StatPath(path, &st, true) != 0)
		return -1;
	if (mode & W_OK)
	{
		errno = EROFS;
		return -1;
	}
	return 0;
}

char* __wrap_realpath(const char* path, char* resolved)
{
	if (!IsPath(path))
		return __real_realpath(path, resolved);
	std::vector<std::string> c;
	struct stat st;
	if (!Components(path, c) || StatPath(path, &st, true) != 0)
	{
		if (errno == 0)
			errno = ENOENT;
		return nullptr;
	}
	const std::string normal = Join(c, 0, c.size());
	if (normal.size() >= PATH_MAX)
	{
		errno = ENAMETOOLONG;
		return nullptr;
	}
	char* out = resolved ? resolved : static_cast<char*>(std::malloc(PATH_MAX));
	if (!out)
	{
		errno = ENOMEM;
		return nullptr;
	}
	std::memcpy(out, normal.c_str(), normal.size() + 1);
	return out;
}

#if defined(ORBIS_NFS_WRAP_DIRS)
// The PC test's opendir/readdir/closedir (the console's are dirshim.cpp's, which ask OpenDir/ReadDir/CloseDir).
DIR* __real_opendir(const char* path);
struct dirent* __real_readdir(DIR* d);
int __real_closedir(DIR* d);
struct FakeDir
{
	void* nfs;
	struct dirent entry;
};
static std::mutex s_fake_dirs_mutex;
static std::vector<FakeDir*> s_fake_dirs;
static FakeDir* AsFake(DIR* d)
{
	std::lock_guard<std::mutex> lock(s_fake_dirs_mutex);
	FakeDir* f = reinterpret_cast<FakeDir*>(d);
	return std::find(s_fake_dirs.begin(), s_fake_dirs.end(), f) != s_fake_dirs.end() ? f : nullptr;
}
DIR* __wrap_opendir(const char* path)
{
	if (!IsPath(path))
		return __real_opendir(path);
	void* nfs = OrbisNfs::OpenDir(path);
	if (!nfs)
		return nullptr;
	FakeDir* f = new FakeDir{nfs, {}};
	std::lock_guard<std::mutex> lock(s_fake_dirs_mutex);
	s_fake_dirs.push_back(f);
	return reinterpret_cast<DIR*>(f);
}
struct dirent* __wrap_readdir(DIR* d)
{
	FakeDir* f = AsFake(d);
	if (!f)
		return __real_readdir(d);
	return OrbisNfs::ReadDir(f->nfs, &f->entry) ? &f->entry : nullptr;
}
int __wrap_closedir(DIR* d)
{
	FakeDir* f = AsFake(d);
	if (!f)
		return __real_closedir(d);
	{
		std::lock_guard<std::mutex> lock(s_fake_dirs_mutex);
		s_fake_dirs.erase(std::find(s_fake_dirs.begin(), s_fake_dirs.end(), f));
	}
	OrbisNfs::CloseDir(f->nfs);
	delete f;
	return 0;
}
#endif
} // extern "C"
