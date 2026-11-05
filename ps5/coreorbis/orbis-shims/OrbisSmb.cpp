// SMB shares through libsmb2 (see OrbisSmb.h). Needs proper testing on the console.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisSmb.h"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace OrbisSmb
{
namespace
{
constexpr int kSlots = 64;
constexpr size_t kMinAhead = 64 * 1024;
constexpr size_t kMaxAhead = 2 * 1024 * 1024;
constexpr size_t kMaxRequest = 1024 * 1024;
constexpr int kInFlight = 8;
constexpr int kIoTimeoutMs = 10000;
constexpr double kRetryAfter = 30.0;
constexpr double kListingSeconds = 30.0;
constexpr int kSocketBuffer = 512 * 1024;

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
	std::printf("[smb] %s\n", line);
	std::fflush(stdout);
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

void Wipe(std::string& s)
{
	volatile char* p = s.data();
	for (size_t i = 0; i < s.size(); i++)
		p[i] = 0;
	s.clear();
}

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

std::string Unescape(const std::string& raw)
{
	std::string out;
	for (size_t i = 0; i < raw.size(); i++)
	{
		if (raw[i] == '%' && i + 2 < raw.size() && Hex(raw[i + 1]) >= 0 && Hex(raw[i + 2]) >= 0)
		{
			out += static_cast<char>(Hex(raw[i + 1]) * 16 + Hex(raw[i + 2]));
			i += 2;
		}
		else
			out += raw[i];
	}
	return out;
}

bool Number(const std::string& s, long lo, long hi, long& out)
{
	if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string::npos)
		return false;
	out = std::strtol(s.c_str(), nullptr, 10);
	return out >= lo && out <= hi;
}

bool ParseUrl(const std::string& url_in, Address& out, bool share_optional, std::string& error)
{
	out = Address();
	const std::string url = Trim(url_in);
	if (url.size() < 7 || Lower(url.substr(0, 6)) != "smb://")
	{
		error = "doesn't start with smb://";
		return false;
	}
	size_t at = 6;
	const size_t end_auth = url.find_first_of("/?", at);
	std::string auth = url.substr(at, end_auth == std::string::npos ? std::string::npos : end_auth - at);
	const size_t user_end = auth.rfind('@');
	if (user_end != std::string::npos)
	{
		std::string who = auth.substr(0, user_end);
		auth = auth.substr(user_end + 1);
		if (who.find(':') != std::string::npos)
		{
			error = "a password can't be in the address (it would be kept in gs.ini): it goes in the SMB passwords' file";
			return false;
		}
		const size_t semi = who.find(';');
		if (semi != std::string::npos)
		{
			out.domain = Unescape(who.substr(0, semi));
			who = who.substr(semi + 1);
		}
		out.user = Unescape(who);
		if (out.user.empty() || (semi != std::string::npos && out.domain.empty()))
		{
			error = "an empty user name or domain before '@'";
			return false;
		}
		for (const std::string* s : {&out.user, &out.domain})
		{
			for (const char c : *s)
			{
				if (static_cast<unsigned char>(c) < 32 || c == 127 || c == '\\' || c == '/' || c == '@')
				{
					error = "the user name or domain has a character it can't have";
					return false;
				}
			}
		}
	}
	if (!auth.empty() && auth[0] == '[')
	{
		error = "IPv6 addresses aren't supported";
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
		out.port = static_cast<uint16_t>(port);
		auth.resize(colon);
	}
	if (auth.empty() || auth.size() > 253 ||
		auth.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
	{
		error = "no server name or IP address after smb://";
		return false;
	}
	out.server = Lower(auth);
	at = end_auth == std::string::npos ? url.size() : end_auth;
	const size_t q = url.find('?', at);
	const std::string path = Unescape(url.substr(at, q == std::string::npos ? std::string::npos : q - at));
	std::vector<std::string> comps;
	if (path.find('\0') != std::string::npos || path.find('\\') != std::string::npos || !Components(path, comps))
	{
		error = "the path climbs above the share, or has a '\\'";
		return false;
	}
	if (comps.empty())
	{
		if (!share_optional)
		{
			error = "no share after the server: smb://<server>/<share>";
			return false;
		}
	}
	else
	{
		out.share = comps[0];
		out.path = comps.size() > 1 ? Join(comps, 1, comps.size()) : std::string("/");
	}
	if (q != std::string::npos)
	{
		const std::string query = url.substr(q + 1);
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
			if (key == "version" && (value == "2" || value == "3"))
				out.version = value == "3" ? 3 : 2;
			else
			{
				error = "\"" + kv + "\" isn't version=2|3";
				return false;
			}
		}
	}
	return true;
}

std::string Redact(const std::string& url)
{
	const size_t start = url.find("://");
	const size_t from = start == std::string::npos ? 0 : start + 3;
	const size_t end = url.find_first_of("/?", from);
	const size_t at = url.rfind('@', end == std::string::npos ? std::string::npos : end);
	if (at == std::string::npos || at < from)
		return url;
	const size_t colon = url.find(':', from);
	if (colon == std::string::npos || colon > at)
		return url;
	return url.substr(0, colon + 1) + "***" + url.substr(at);
}

std::string KeyOf(const Address& a)
{
	std::string k = "smb://";
	if (!a.domain.empty())
		k += a.domain + ";";
	k += a.user + "@" + a.server;
	if (a.port)
		k += ":" + std::to_string(a.port);
	if (!a.share.empty())
		k += "/" + a.share;
	return k;
}

int KeyMatch(const Address& key, const Address& a)
{
	if (key.server != a.server || key.port != a.port || Lower(key.user) != Lower(a.user) || Lower(key.domain) != Lower(a.domain))
		return 0;
	if (key.share.empty())
		return 1;
	return Lower(key.share) == Lower(a.share) ? 2 : 0;
}

std::mutex g_credentials_mutex;
std::string g_credentials_file;

bool ReadWhole(const std::string& path, std::string& out)
{
	out.clear();
	FILE* fp = std::fopen(path.c_str(), "rb");
	if (!fp)
		return errno == ENOENT;
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0 && out.size() < 1024 * 1024)
		out.append(buf, n);
	const bool ok = !std::ferror(fp);
	std::fclose(fp);
	std::memset(buf, 0, sizeof(buf));
	return ok;
}

bool ParseLine(const std::string& line_in, Address& key, std::string& password)
{
	std::string line = line_in;
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	const size_t tab = line.find('\t');
	if (line.empty() || line[0] == '#' || tab == std::string::npos)
		return false;
	std::string error;
	if (!ParseUrl(line.substr(0, tab), key, true, error) || key.user.empty())
		return false;
	password = line.substr(tab + 1);
	return true;
}

bool LookupPassword(const Address& a, std::string& password)
{
	password.clear();
	if (a.user.empty())
		return false;
	std::string file;
	{
		std::lock_guard<std::mutex> lock(g_credentials_mutex);
		file = g_credentials_file;
	}
	std::string text;
	if (file.empty() || !ReadWhole(file, text))
		return false;
	int best = 0;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at), pw;
		at = end + 1;
		Address key;
		if (ParseLine(line, key, pw))
		{
			const int m = KeyMatch(key, a);
			if (m > best)
			{
				best = m;
				Wipe(password);
				password = pw;
			}
		}
		Wipe(pw);
		Wipe(line);
	}
	Wipe(text);
	return best > 0;
}

// libsmb2 keeps all contexts in one unsynchronized list (init/destroy).
std::mutex g_context_mutex;

smb2_context* NewContext()
{
	std::lock_guard<std::mutex> lock(g_context_mutex);
	return smb2_init_context();
}

void DestroyContext(smb2_context* ctx)
{
	std::lock_guard<std::mutex> lock(g_context_mutex);
	smb2_destroy_context(ctx);
}

struct Call
{
	bool done = false;
	int status = 0;
	void* data = nullptr;
	uint64_t progress = 0;
};

void CallDone(smb2_context*, int status, void* data, void* private_data)
{
	Call* c = static_cast<Call*>(private_data);
	c->done = true;
	c->status = status;
	c->data = data;
	c->progress++;
}

// On false the caller must destroy the context while the callbacks' targets are still alive.
template <class Done>
bool Serve(smb2_context* ctx, Done done, const uint64_t& progress, int timeout_ms, std::string& why)
{
	uint64_t seen = progress;
	double deadline = Now() + timeout_ms / 1000.0;
	while (!done())
	{
		if (progress != seen)
		{
			seen = progress;
			deadline = Now() + timeout_ms / 1000.0;
		}
		const double left = deadline - Now();
		if (left <= 0)
		{
			char text[64];
			std::snprintf(text, sizeof(text), "no answer in %.1f s", timeout_ms / 1000.0);
			why = text;
			return false;
		}
		pollfd p = {smb2_get_fd(ctx), static_cast<short>(smb2_which_events(ctx)), 0};
		if (p.fd < 0)
		{
			why = "no connection";
			return false;
		}
		const int r = poll(&p, 1, std::max(1, std::min(100, static_cast<int>(left * 1000.0))));
		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			why = "poll failed (errno " + std::to_string(errno) + ")";
			return false;
		}
		if (r > 0 && smb2_service(ctx, p.revents) < 0)
		{
			const char* e = smb2_get_error(ctx);
			why = e && *e ? e : "the connection failed";
			return false;
		}
	}
	return true;
}

const char* DialectName(uint16_t d)
{
	switch (d)
	{
		case SMB2_VERSION_0202: return "SMB 2.0.2";
		case SMB2_VERSION_0210: return "SMB 2.1";
		case SMB2_VERSION_0300: return "SMB 3.0";
		case SMB2_VERSION_0302: return "SMB 3.0.2";
		case SMB2_VERSION_0311: return "SMB 3.1.1";
		default: return "SMB";
	}
}

std::atomic<uint64_t> g_opens{0}, g_reads{0}, g_requests{0}, g_bytes{0}, g_cache_hits{0}, g_lookups{0}, g_reconnects{0};

struct Share
{
	std::string url;
	Address addr;
	std::string mount_point;
	std::mutex mutex;
	smb2_context* ctx = nullptr;
	bool mounted = false;
	uint64_t generation = 0;
	bool dropped = false;
	double retry_after = 0;
	std::string state = "not tried yet";

	~Share()
	{
		if (ctx)
			DestroyContext(ctx);
	}
};

std::mutex g_shares_mutex;
std::vector<std::shared_ptr<Share>> g_shares;
std::atomic<int> g_share_count{0};

bool Active()
{
	return g_share_count.load(std::memory_order_relaxed) > 0;
}

struct Where
{
	std::string normal;
	std::shared_ptr<Share> share;
	std::string rest;
	std::vector<std::string> children;
};

bool Resolve(const char* path, Where& w)
{
	std::vector<std::string> c;
	if (!Components(path, c) || c.empty() || c[0] != "smb")
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

std::string Inner(const Share& s, const std::string& rest)
{
	std::string p = (s.addr.path == "/" ? std::string() : s.addr.path) + rest;
	while (!p.empty() && p[0] == '/')
		p.erase(0, 1);
	return p;
}

void Drop(Share& s, const std::string& why)
{
	Log("%s: connection dropped: %s", s.mount_point.c_str(), why.c_str());
	if (s.ctx)
		DestroyContext(s.ctx);
	s.ctx = nullptr;
	s.mounted = false;
	s.generation++;
	s.dropped = true;
	s.state = "connection lost: " + why;
}

bool ConnectLocked(Share& s, int timeout_ms)
{
	if (s.mounted)
		return true;
	const double t0 = Now();
	if (t0 < s.retry_after)
		return false;
	std::string password;
	const bool has_password = LookupPassword(s.addr, password);
	const std::string user = s.addr.user.empty() ? std::string("Guest") : s.addr.user;
	std::string server = s.addr.server;
	if (s.addr.port)
		server += ":" + std::to_string(s.addr.port);
	struct Try
	{
		const char* password;
		const char* how;
	};
	std::vector<Try> tries;
	if (has_password)
		tries.push_back({password.c_str(), "with its password"});
	else
	{
		tries.push_back({"", "with an empty password"});
		tries.push_back({nullptr, "anonymously"});
	}
	std::string why;
	for (const Try& t : tries)
	{
		smb2_context* ctx = NewContext();
		if (!ctx)
		{
			why = "no memory for an SMB context";
			break;
		}
		smb2_set_timeout(ctx, kIoTimeoutMs / 1000);
		smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
		if (s.addr.version)
			smb2_set_version(ctx, s.addr.version == 3 ? SMB2_VERSION_ANY3 : SMB2_VERSION_ANY2);
		smb2_set_user(ctx, user.c_str());
		smb2_set_password(ctx, t.password);
		if (!s.addr.domain.empty())
			smb2_set_domain(ctx, s.addr.domain.c_str());
		Call c, sc;
		smb2_stat_64 st = {};
		std::string err;
		bool ok = smb2_connect_share_async(ctx, server.c_str(), s.addr.share.c_str(), user.c_str(), CallDone, &c) == 0;
		if (!ok)
			err = smb2_get_error(ctx);
		else if (!Serve(ctx, [&c]() { return c.done; }, c.progress, timeout_ms, err))
			ok = false;
		else if (c.status != 0)
		{
			ok = false;
			const char* e = smb2_get_error(ctx);
			err = e && *e ? e : std::strerror(-c.status);
		}
		if (ok && s.addr.path != "/")
		{
			const std::string inner = Inner(s, "");
			if (smb2_stat_async(ctx, inner.c_str(), &st, CallDone, &sc) != 0 ||
				!Serve(ctx, [&sc]() { return sc.done; }, sc.progress, timeout_ms, err))
				ok = false;
			else if (sc.status != 0 || st.smb2_type != SMB2_TYPE_DIRECTORY)
			{
				ok = false;
				err = "there is no folder " + s.addr.path + " in the share";
			}
		}
		if (ok)
		{
			const int fd = smb2_get_fd(ctx);
			int size = kSocketBuffer;
			setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
			s.ctx = ctx;
			s.mounted = true;
			if (s.dropped)
				g_reconnects++;
			s.dropped = false;
			char state[512];
			std::snprintf(state, sizeof(state), "connected (%s) %s%s in %.2f s, reads of up to %u KB",
				DialectName(smb2_get_dialect(ctx)), s.addr.user.empty() ? "as Guest " : "", t.how, Now() - t0,
				smb2_get_max_read_size(ctx) / 1024);
			s.state = state;
			Log("%s: %s", s.mount_point.c_str(), state);
			Wipe(password);
			return true;
		}
		// Try the next login only if the server negotiated, then refused.
		const bool answered = c.done && c.status != 0 && smb2_get_dialect(ctx) != 0;
		DestroyContext(ctx);
		const std::string this_why = std::string(t.how) + ": " + err;
		Log("%s: %s", s.mount_point.c_str(), this_why.c_str());
		why = why.empty() ? this_why : why + "; then " + this_why;
		if (!answered)
			break;
	}
	Wipe(password);
	s.retry_after = Now() + kRetryAfter;
	s.state = "not connected: " + why;
	return false;
}

template <class Start>
int Run(Share& s, Call& c, Start start)
{
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!ConnectLocked(s, kIoTimeoutMs))
			return -EIO;
		c = Call();
		g_requests++;
		const int rc = start(s.ctx);
		if (rc < 0)
			return rc;
		std::string why;
		if (!Serve(s.ctx, [&c]() { return c.done; }, c.progress, kIoTimeoutMs, why))
			Drop(s, why);
		// libsmb2 reports a lost session or share as ENETRESET.
		else if (c.status == -ENETRESET)
			Drop(s, "the server ended the session or the share");
		else
			return c.status;
	}
	return -EIO;
}

struct Attr
{
	uint64_t mode = 0, size = 0, ino = 0, nlink = 1;
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
	static std::unordered_map<std::string, Listing> listings;
	return listings;
}

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

Attr FromSmb(const smb2_stat_64& st)
{
	Attr a;
	a.mode = st.smb2_type == SMB2_TYPE_DIRECTORY ? (S_IFDIR | 0555) : st.smb2_type == SMB2_TYPE_LINK ? (S_IFLNK | 0444) : (S_IFREG | 0444);
	a.size = st.smb2_size;
	a.ino = st.smb2_ino;
	a.nlink = st.smb2_nlink;
	a.mtime = static_cast<int64_t>(st.smb2_mtime);
	a.mtime_nsec = static_cast<uint32_t>(st.smb2_mtime_nsec);
	return a;
}

void Fill(struct stat* st, const Attr& a)
{
	std::memset(st, 0, sizeof(*st));
	st->st_mode = static_cast<mode_t>((a.mode & S_IFMT) | (a.mode & 0555));
	st->st_size = static_cast<off_t>(a.size);
	st->st_ino = static_cast<ino_t>(a.ino);
	st->st_nlink = static_cast<nlink_t>(a.nlink ? a.nlink : 1);
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

struct DirItem
{
	std::string name;
	unsigned char type = DT_UNKNOWN;
	uint64_t ino = 0;
};
struct SmbDir
{
	std::vector<DirItem> items;
	size_t next = 0;
};

struct SmbFile
{
	std::mutex mutex;
	std::shared_ptr<Share> share;
	smb2fh* fh = nullptr;
	uint64_t generation = 0;
	std::string path, inner;
	Attr attr;
	std::vector<uint8_t> window;
	uint64_t window_at = 0;
	size_t window_len = 0;
	uint64_t last_end = UINT64_MAX;
	size_t ahead = kMinAhead;
	uint64_t reads = 0, bytes = 0, fetches = 0;
	double fetch_seconds = 0;
	int failures_logged = 0;
};

std::mutex g_slots_mutex;
std::shared_ptr<SmbFile> g_slots[kSlots];

std::shared_ptr<SmbFile> InSlot(int handle)
{
	if (handle < 0 || handle >= kSlots)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_slots_mutex);
	return g_slots[handle];
}

bool ReopenLocked(Share& s, SmbFile& f)
{
	Call c;
	const int rc = Run(s, c, [&](smb2_context* ctx) { return smb2_open_async(ctx, f.inner.c_str(), O_RDONLY, CallDone, &c); });
	if (rc != 0 || !c.data)
	{
		errno = rc < 0 ? -rc : EIO;
		return false;
	}
	f.fh = static_cast<smb2fh*>(c.data);
	f.generation = s.generation;
	Log("reopened %s", f.path.c_str());
	return true;
}

struct ReadJob;
struct Piece
{
	ReadJob* job;
	uint8_t* dst;
	uint64_t off;
	size_t len, done;
};
struct ReadJob
{
	smb2_context* ctx;
	smb2fh* fh;
	std::vector<Piece> pieces;
	size_t next = 0;
	int pending = 0, error = 0;
	bool eof = false;
	uint64_t progress = 0;
};

void PieceDone(smb2_context*, int status, void*, void* private_data);

void Send(Piece& p)
{
	ReadJob& j = *p.job;
	const int rc = smb2_pread_async(j.ctx, j.fh, p.dst + p.done, static_cast<uint32_t>(p.len - p.done), p.off + p.done,
		PieceDone, &p);
	g_requests++;
	if (rc < 0)
	{
		if (!j.error)
			j.error = -rc;
		return;
	}
	j.pending++;
}

void SendMore(ReadJob& j)
{
	while (j.pending < kInFlight && j.next < j.pieces.size() && !j.eof && !j.error)
		Send(j.pieces[j.next++]);
}

void PieceDone(smb2_context*, int status, void*, void* private_data)
{
	Piece& p = *static_cast<Piece*>(private_data);
	ReadJob& j = *p.job;
	j.pending--;
	j.progress++;
	if (status < 0)
	{
		if (!j.error)
			j.error = -status;
		return;
	}
	if (status == 0)
	{
		j.eof = true;
		return;
	}
	if (static_cast<size_t>(status) > p.len - p.done)
	{
		j.error = EIO;
		return;
	}
	p.done += static_cast<size_t>(status);
	if (p.done < p.len)
		Send(p);
	else
		SendMore(j);
}

ssize_t Fetch(SmbFile& f, uint8_t* dst, size_t n, uint64_t off)
{
	Share& s = *f.share;
	std::lock_guard<std::mutex> lock(s.mutex);
	const double t0 = Now();
	ssize_t result = -1;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if ((!s.mounted || f.generation != s.generation) && !ReopenLocked(s, f))
			break;
		size_t block = std::min<size_t>(smb2_get_max_read_size(s.ctx), kMaxRequest);
		if (smb2_get_dialect(s.ctx) <= SMB2_VERSION_0202)
			block = std::min<size_t>(block, 65536);
		if (block == 0)
			block = 65536;
		ReadJob j;
		j.ctx = s.ctx;
		j.fh = f.fh;
		j.pieces.reserve((n + block - 1) / block);
		for (size_t at = 0; at < n; at += block)
			j.pieces.push_back({&j, dst + at, off + at, std::min(block, n - at), 0});
		SendMore(j);
		std::string why;
		if (!Serve(s.ctx, [&j]() { return j.pending == 0; }, j.progress, kIoTimeoutMs, why))
		{
			// Before `j` goes out of scope: destroying the context completes its pending reads.
			Drop(s, why);
			if (f.failures_logged++ < 5)
				Log("reading %s at %llu: %s%s", f.path.c_str(), static_cast<unsigned long long>(off), why.c_str(),
					attempt == 0 ? "; again on a new connection" : "");
			errno = EIO;
			continue;
		}
		if (j.error == ENETRESET)
		{
			Drop(s, "the server ended the session or the share");
			errno = EIO;
			continue;
		}
		size_t got = 0;
		for (const Piece& p : j.pieces)
		{
			got += p.done;
			if (p.done < p.len)
				break;
		}
		if (j.error && got == 0)
		{
			if (f.failures_logged++ < 5)
				Log("reading %s at %llu: %s", f.path.c_str(), static_cast<unsigned long long>(off), std::strerror(j.error));
			errno = j.error;
			break;
		}
		g_bytes += got;
		result = static_cast<ssize_t>(got);
		break;
	}
	f.fetches++;
	f.fetch_seconds += Now() - t0;
	return result;
}

ssize_t ReadAt(SmbFile& f, uint8_t* dst, size_t n, uint64_t off)
{
	f.reads++;
	g_reads++;
	if (n == 0 || off >= f.attr.size)
		return 0;
	if (n > f.attr.size - off)
		n = static_cast<size_t>(f.attr.size - off);
	f.ahead = off == f.last_end ? std::min(f.ahead * 2, kMaxAhead) : kMinAhead;
	size_t done = 0;
	bool failed = false;
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
			failed = got < 0;
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
			failed = got < 0;
			f.window_len = 0;
			break;
		}
		f.window_at = at;
		f.window_len = static_cast<size_t>(got);
	}
	f.last_end = off + done;
	f.bytes += done;
	if (done == 0 && failed)
		return -1;
	return static_cast<ssize_t>(done);
}
}

bool ParseAddress(const std::string& url, Address& out, std::string& error)
{
	return ParseUrl(url, out, false, error);
}

std::string MountPointOf(const Address& a)
{
	return "/smb/" + a.server + "/" + a.share + (a.path == "/" ? std::string() : a.path);
}

void SetCredentialsFile(const std::string& path)
{
	std::lock_guard<std::mutex> lock(g_credentials_mutex);
	g_credentials_file = path;
}

bool StoreCredential(const std::string& file, const std::string& url, const std::string& password, std::string& error)
{
	Address a;
	if (!ParseUrl(url, a, true, error))
		return false;
	if (a.user.empty())
	{
		error = "a password needs a user name: smb://<user>@<server>/<share>";
		return false;
	}
	if (password.find_first_of(std::string("\r\n\0", 3)) != std::string::npos)
	{
		error = "a password can't have a line break";
		return false;
	}
	std::string old;
	if (!ReadWhole(file, old))
	{
		error = "can't read " + file + " (errno " + std::to_string(errno) + ")";
		return false;
	}
	std::string text = old.empty() ? "# PS5SX2: the SMB shares' passwords (ps5/coreorbis/include-orbis/OrbisSmb.h): an address, a tab, "
	                                 "the password.\n"
	                               : std::string();
	size_t at = 0;
	while (at < old.size())
	{
		size_t end = old.find('\n', at);
		if (end == std::string::npos)
			end = old.size();
		std::string line = old.substr(at, end - at), pw;
		at = end + 1;
		Address key;
		const bool same = ParseLine(line, key, pw) && key.share.empty() == a.share.empty() && KeyMatch(key, a) > 0;
		if (!same)
			text += line + "\n";
		Wipe(pw);
		Wipe(line);
	}
	Wipe(old);
	if (!password.empty())
		text += KeyOf(a) + "\t" + password + "\n";
	const std::string temp = file + ".tmp";
	const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	bool ok = fd >= 0 && fchmod(fd, 0600) == 0;
	for (size_t done = 0; ok && done < text.size();)
	{
		const ssize_t w = ::write(fd, text.data() + done, text.size() - done);
		if (w < 0 && errno == EINTR)
			continue;
		ok = w > 0;
		done += ok ? static_cast<size_t>(w) : 0;
	}
	ok = ok && fsync(fd) == 0;
	const int saved = errno;
	if (fd >= 0 && ::close(fd) != 0)
		ok = false;
	Wipe(text);
	if (!ok || std::rename(temp.c_str(), file.c_str()) != 0)
	{
		error = "can't write " + file + " (errno " + std::to_string(ok ? errno : saved) + ")";
		::unlink(temp.c_str());
		return false;
	}
	Log("%s %s%s%s", password.empty() ? "password removed for" : "password set for", a.server.c_str(), a.share.empty() ? "" : "/",
		a.share.c_str());
	return true;
}

bool HasCredential(const std::string& url)
{
	Address a;
	std::string error, password;
	if (!ParseUrl(url, a, false, error))
		return false;
	const bool has = LookupPassword(a, password);
	Wipe(password);
	return has;
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
			Log("share \"%s\" left out: %s", Redact(item).c_str(), error.c_str());
			if (problems)
				problems->push_back(Redact(item) + ": " + error);
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
		g_shares = std::move(shares);
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
			s->retry_after = 0;
			ConnectLocked(*s, timeout_ms);
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
	return path && Active() && std::strncmp(path, "/smb", 4) == 0 && (path[4] == '\0' || path[4] == '/');
}

int Stat(const char* path, struct stat* st, bool follow)
{
	(void)follow;
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
	const std::string inner = Inner(s, w.rest);
	std::lock_guard<std::mutex> lock(s.mutex);
	g_lookups++;
	smb2_stat_64 ss = {};
	Call c;
	const int rc = Run(s, c, [&](smb2_context* ctx) { return smb2_stat_async(ctx, inner.c_str(), &ss, CallDone, &c); });
	if (rc != 0)
	{
		errno = rc < 0 ? -rc : EIO;
		return -1;
	}
	Fill(st, FromSmb(ss));
	return 0;
}

void* OpenDir(const char* path)
{
	Where w;
	if (!Resolve(path, w))
	{
		errno = ENOENT;
		return nullptr;
	}
	auto d = std::make_unique<SmbDir>();
	if (!w.share)
	{
		for (const std::string& c : w.children)
			d->items.push_back({c, DT_DIR, 0});
		return d.release();
	}
	Share& s = *w.share;
	const std::string inner = Inner(s, w.rest);
	std::unordered_map<std::string, Attr> names;
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		Call c;
		const int rc = Run(s, c, [&](smb2_context* ctx) { return smb2_opendir_async(ctx, inner.c_str(), CallDone, &c); });
		if (rc != 0 || !c.data)
		{
			errno = rc < 0 ? -rc : EIO;
			return nullptr;
		}
		smb2dir* sd = static_cast<smb2dir*>(c.data);
		while (const smb2dirent* e = smb2_readdir(s.ctx, sd))
		{
			if (!e->name || !std::strcmp(e->name, ".") || !std::strcmp(e->name, ".."))
				continue;
			const Attr a = FromSmb(e->st);
			const unsigned char type = e->st.smb2_type == SMB2_TYPE_DIRECTORY ? DT_DIR : e->st.smb2_type == SMB2_TYPE_LINK ? DT_LNK : DT_REG;
			d->items.push_back({e->name, type, e->st.smb2_ino});
			names.emplace(e->name, a);
		}
		smb2_closedir(s.ctx, sd);
	}
	Remember(w.normal, std::move(names));
	return d.release();
}

bool ReadDir(void* dir, struct dirent* out)
{
	SmbDir* d = static_cast<SmbDir*>(dir);
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
	delete static_cast<SmbDir*>(dir);
}

int Open(const char* path)
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
	auto f = std::make_shared<SmbFile>();
	f->share = w.share;
	f->path = w.normal;
	f->inner = Inner(*w.share, w.rest);
	{
		Share& s = *w.share;
		std::lock_guard<std::mutex> lock(s.mutex);
		if (!ReopenLocked(s, *f))
		{
			// libsmb2 maps STATUS_FILE_IS_A_DIRECTORY to EPERM.
			const int saved = errno;
			if (saved == EPERM)
			{
				smb2_stat_64 st = {};
				Call c;
				const int rc = Run(s, c, [&](smb2_context* ctx) { return smb2_stat_async(ctx, f->inner.c_str(), &st, CallDone, &c); });
				errno = rc == 0 && st.smb2_type == SMB2_TYPE_DIRECTORY ? EISDIR : saved;
			}
			return -1;
		}
		smb2_stat_64 st = {};
		Call c;
		std::string why;
		g_requests++;
		if (smb2_fstat_async(s.ctx, f->fh, &st, CallDone, &c) != 0 ||
			!Serve(s.ctx, [&c]() { return c.done; }, c.progress, kIoTimeoutMs, why))
		{
			if (!why.empty())
				Drop(s, why);
			errno = EIO;
			return -1;
		}
		if (c.status != 0 || st.smb2_type == SMB2_TYPE_DIRECTORY)
		{
			Call cc;
			if (smb2_close_async(s.ctx, f->fh, CallDone, &cc) == 0 && !Serve(s.ctx, [&cc]() { return cc.done; }, cc.progress, kIoTimeoutMs, why))
				Drop(s, why);
			errno = c.status != 0 ? (c.status < 0 ? -c.status : EIO) : EISDIR;
			return -1;
		}
		f->attr = FromSmb(st);
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
		if (f->share->mounted && f->generation == f->share->generation)
		{
			Call cc;
			std::string why;
			if (smb2_close_async(f->share->ctx, f->fh, CallDone, &cc) == 0 &&
				!Serve(f->share->ctx, [&cc]() { return cc.done; }, cc.progress, kIoTimeoutMs, why))
				Drop(*f->share, why);
		}
		Log("too many files open on shares (%d): %s not opened", kSlots, f->path.c_str());
		errno = EMFILE;
		return -1;
	}
	g_opens++;
	Log("opened %s (%llu bytes)", f->path.c_str(), static_cast<unsigned long long>(f->attr.size));
	return slot;
}

ssize_t PRead(int handle, void* dst, size_t n, uint64_t off)
{
	const std::shared_ptr<SmbFile> f = InSlot(handle);
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	return ReadAt(*f, static_cast<uint8_t*>(dst), n, off);
}

int FStat(int handle, struct stat* st)
{
	const std::shared_ptr<SmbFile> f = InSlot(handle);
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	Fill(st, f->attr);
	return 0;
}

int Close(int handle)
{
	std::shared_ptr<SmbFile> f;
	if (handle >= 0 && handle < kSlots)
	{
		std::lock_guard<std::mutex> lock(g_slots_mutex);
		f = std::move(g_slots[handle]);
		g_slots[handle].reset();
	}
	if (!f)
	{
		errno = EBADF;
		return -1;
	}
	std::lock_guard<std::mutex> flock(f->mutex);
	{
		Share& s = *f->share;
		std::lock_guard<std::mutex> lock(s.mutex);
		if (s.mounted && f->generation == s.generation && f->fh)
		{
			Call c;
			std::string why;
			g_requests++;
			if (smb2_close_async(s.ctx, f->fh, CallDone, &c) == 0 && !Serve(s.ctx, [&c]() { return c.done; }, c.progress, kIoTimeoutMs, why))
				Drop(s, why);
		}
		f->fh = nullptr;
	}
	if (f->reads)
		Log("closed %s: %llu reads, %.1f MB in %llu fetches, %.2f s waiting (%.1f MB/s)", f->path.c_str(),
			static_cast<unsigned long long>(f->reads), static_cast<double>(f->bytes) / 1048576.0,
			static_cast<unsigned long long>(f->fetches), f->fetch_seconds,
			f->fetch_seconds > 0 ? static_cast<double>(f->bytes) / 1048576.0 / f->fetch_seconds : 0.0);
	return 0;
}

Stats GetStats()
{
	Stats s;
	s.opens = g_opens;
	s.reads = g_reads;
	s.requests = g_requests;
	s.bytes = g_bytes;
	s.cache_hits = g_cache_hits;
	s.lookups = g_lookups;
	s.reconnects = g_reconnects;
	return s;
}
}
