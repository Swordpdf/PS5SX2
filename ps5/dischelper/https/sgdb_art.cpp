// The disc tile payload's SteamGridDB download (test 6): for a game's name, SteamGridDB's best-matching game and from it a hero (wide banner),
// a logo and a square grid, the way the 5S2CD fork asks for them (its filters: static, not nsfw, no humor, no epilepsy). Over PS5SX2's own HTTPS
// client (fe_https: mbedTLS and Mozilla's roots; the console's libSceHttp2 cannot check certificates after the jailbreak). The API key goes only
// in the Authorization header of requests to steamgriddb.com; it is never logged, and the header is taken off before a picture is fetched.
//
// Copyright (C) 2026 swordpdf. SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_https.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int klog_printf(const char* fmt, ...);
}

namespace
{
	std::string Norm(const std::string& s)
	{
		std::string o;
		for (unsigned char c : s)
		{
			if (std::isalnum(c))
				o += static_cast<char>(std::tolower(c));
			else if (c == '&')
				o += "and";
			else if (c == ' ' && !o.empty() && o.back() != ' ')
				o += ' ';
		}
		while (!o.empty() && o.back() == ' ')
			o.pop_back();
		return o;
	}

	double Alike(const std::string& a, const std::string& b)
	{
		if (a.empty() || b.empty())
			return 0.0;
		std::vector<int> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
		for (size_t i = 1; i <= a.size(); i++)
		{
			for (size_t j = 1; j <= b.size(); j++)
				cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : (prev[j] > cur[j - 1] ? prev[j] : cur[j - 1]);
			prev.swap(cur);
		}
		return 2.0 * prev[b.size()] / static_cast<double>(a.size() + b.size());
	}

	std::string UrlEncode(const std::string& s)
	{
		static const char hex[] = "0123456789ABCDEF";
		std::string o;
		for (unsigned char c : s)
		{
			if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				o += static_cast<char>(c);
			else
			{
				o += '%';
				o += hex[c >> 4];
				o += hex[c & 15];
			}
		}
		return o;
	}

	std::string JsonString(const std::string& j, size_t& pos)
	{
		std::string o;
		while (pos < j.size() && j[pos] != '"')
		{
			if (j[pos] == '\\' && pos + 1 < j.size())
			{
				const char e = j[pos + 1];
				pos += 2;
				if (e == 'u' && pos + 4 <= j.size())
				{
					const unsigned v = static_cast<unsigned>(std::strtoul(j.substr(pos, 4).c_str(), nullptr, 16));
					o += v < 128 ? static_cast<char>(v) : '?';
					pos += 4;
				}
				else if (e == 'n' || e == 't' || e == 'r')
					o += ' ';
				else
					o += e;
			}
			else
				o += j[pos++];
		}
		return o;
	}

	std::string FieldString(const std::string& j, const char* key, size_t from, size_t to)
	{
		const std::string k = std::string("\"") + key + "\":";
		const size_t at = j.find(k, from);
		if (at == std::string::npos || at >= to)
			return {};
		size_t p = at + k.size();
		while (p < j.size() && j[p] == ' ')
			p++;
		if (p >= j.size() || j[p] != '"')
			return {};
		p++;
		return JsonString(j, p);
	}

	long long FieldNumber(const std::string& j, const char* key, size_t from, size_t to)
	{
		const std::string k = std::string("\"") + key + "\":";
		const size_t at = j.find(k, from);
		if (at == std::string::npos || at >= to)
			return -1;
		size_t p = at + k.size();
		while (p < j.size() && j[p] == ' ')
			p++;
		if (p >= j.size() || !(std::isdigit(static_cast<unsigned char>(j[p])) || j[p] == '-'))
			return -1;
		return std::atoll(j.c_str() + p);
	}

	struct Item
	{
		long long id = -1, width = 0, height = 0, score = 0;
		std::string name, url;
	};

	std::vector<Item> Items(const std::string& j)
	{
		std::vector<Item> out;
		const size_t data = j.find("\"data\"");
		if (data == std::string::npos)
			return out;
		std::vector<size_t> starts;
		for (size_t p = j.find("\"id\":", data); p != std::string::npos; p = j.find("\"id\":", p + 5))
			starts.push_back(p);
		for (size_t i = 0; i < starts.size(); i++)
		{
			const size_t to = i + 1 < starts.size() ? starts[i + 1] : j.size();
			Item it;
			it.id = FieldNumber(j, "id", starts[i], to);
			it.width = FieldNumber(j, "width", starts[i], to);
			it.height = FieldNumber(j, "height", starts[i], to);
			it.score = FieldNumber(j, "score", starts[i], to);
			it.name = FieldString(j, "name", starts[i], to);
			it.url = FieldString(j, "url", starts[i], to);
			out.push_back(it);
		}
		return out;
	}

	const char* const kApi = "https://www.steamgriddb.com/api/v2";
	const char* const kFilter = "types=static&nsfw=false&humor=false&epilepsy=false";

	bool Fetch(fe::HttpsClient& c, const std::string& url, const std::string& key, std::string& body, std::string& error)
	{
		body.clear();
		if (!key.empty())
			fe::g_https_extra_headers = "Authorization: Bearer " + key + "\r\n";
		else
			fe::g_https_extra_headers.clear();
		const int status = c.Request("GET", url, "", "", [&body](const void* p, size_t n) {
			body.append(static_cast<const char*>(p), n);
			return body.size() <= (24u << 20);
		}, 40 * 1000, &error);
		fe::g_https_extra_headers.clear();
		if (status == 401 || status == 403)
		{
			error = "SteamGridDB refused the API key (HTTP " + std::to_string(status) + ")";
			return false;
		}
		if (status != 200)
		{
			if (error.empty())
				error = "HTTP " + std::to_string(status);
			return false;
		}
		return true;
	}

	bool IsPicture(const std::string& d)
	{
		static const unsigned char png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
		return (d.size() > 24 && std::memcmp(d.data(), png, 8) == 0) ||
			   (d.size() > 4 && static_cast<unsigned char>(d[0]) == 0xFF && static_cast<unsigned char>(d[1]) == 0xD8 && static_cast<unsigned char>(d[2]) == 0xFF);
	}

	bool WriteFile(const char* dest, const std::string& d)
	{
		const std::string tmp = std::string(dest) + ".tmp";
		const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			return false;
		const bool wrote = write(fd, d.data(), d.size()) == static_cast<ssize_t>(d.size());
		close(fd);
		if (!wrote || std::rename(tmp.c_str(), dest) != 0)
		{
			unlink(tmp.c_str());
			return false;
		}
		return true;
	}

	int g_pool = -1;

	struct Kind
	{
		const char* route;   // heroes, logos, grids
		const char* extra;   // the query beyond the filter
		long long min_w;     // a hero is taken from 3840 wide up when there is one
		bool square;
	};
} // namespace

// A PS2 game's cover from the public set PS5SX2's own shelf uses (github.com/xlenore/ps2-covers), saved at `dest`; no key is needed.
// Returns 0 when saved, 1 when there is no cover for the serial (HTTP 404), -1 when it could not be asked or saved (a line in `msg`).
extern "C" int cover_fetch(const char* serial, const char* dest, char* msg, size_t msgmax)
{
	std::string error;
	if (g_pool < 0)
	{
		sceNetInit();
		g_pool = sceNetPoolCreate("disctile-https", 64 * 1024, 0);
		if (g_pool < 0)
		{
			std::snprintf(msg, msgmax, "no network pool");
			return -1;
		}
	}
	fe::HttpsClient client(fe::MakeConsoleHttpsPlatform(g_pool, [](const std::string& s) { klog_printf("[disctile] %s\n", s.c_str()); }));
	client.user_agent = "PS5SX2-disctile/1.0";
	if (!client.Init(error))
	{
		std::snprintf(msg, msgmax, "TLS setup failed: %s", error.c_str());
		return -1;
	}
	const std::string url = std::string("https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/") + serial + ".jpg";
	std::string body;
	if (!Fetch(client, url, "", body, error))
	{
		std::snprintf(msg, msgmax, "%s", error.c_str());
		return error == "HTTP 404" ? 1 : -1;
	}
	if (!IsPicture(body))
	{
		std::snprintf(msg, msgmax, "the answer was not a picture");
		return -1;
	}
	if (!WriteFile(dest, body))
	{
		std::snprintf(msg, msgmax, "cannot write the cover");
		return -1;
	}
	std::snprintf(msg, msgmax, "%zu bytes", body.size());
	return 0;
}

// For `name`: the game's best SteamGridDB match, then its hero, logo and square grid saved at the given paths (any may be NULL, or not found).
// Returns a mask: 1 hero, 2 logo, 4 grid; or -1 when nothing could be asked (a line in `msg`, never with the key in it). `msg` also says the match.
extern "C" int sgdb_fetch_art(const char* key, const char* name, const char* hero, const char* logo, const char* grid, char* msg, size_t msgmax)
{
	std::string error;
	auto fail = [&](const std::string& why) {
		std::snprintf(msg, msgmax, "%s", why.c_str());
		return -1;
	};
	if (g_pool < 0)
	{
		sceNetInit();
		g_pool = sceNetPoolCreate("disctile-https", 64 * 1024, 0);
		if (g_pool < 0)
			return fail("no network pool");
	}
	fe::HttpsClient client(fe::MakeConsoleHttpsPlatform(g_pool, [](const std::string& s) { klog_printf("[disctile] %s\n", s.c_str()); }));
	client.user_agent = "PS5SX2-disctile/1.0";
	if (!client.Init(error))
		return fail("TLS setup failed: " + error);

	const std::string full = name;
	std::vector<std::string> terms{full};
	for (const char* sep : {": ", " - ", ":"})
	{
		const size_t at = full.find(sep);
		if (at != std::string::npos && at > 3)
		{
			terms.push_back(full.substr(0, at));
			break;
		}
	}
	long long game = -1;
	std::string game_name, body;
	double best = 0.0;
	for (const std::string& term : terms)
	{
		if (!Fetch(client, std::string(kApi) + "/search/autocomplete/" + UrlEncode(term), key, body, error))
			return fail("search: " + error);
		for (const Item& it : Items(body))
		{
			const double r = Alike(Norm(full), Norm(it.name));
			if (it.id >= 0 && r > best)
			{
				best = r;
				game = it.id;
				game_name = it.name;
			}
		}
		if (best >= 0.9)
			break;
	}
	if (game < 0 || best < 0.6)
		return fail("no SteamGridDB game close enough to \"" + full + "\" (best: \"" + game_name + "\")");

	const struct
	{
		Kind k;
		const char* dest;
		int bit;
	} jobs[3] = {
		{{"heroes", "&mimes=image/png,image/jpeg", 3840, false}, hero, 1},
		{{"logos", "&mimes=image/png", 0, false}, logo, 2},
		{{"grids", "&mimes=image/png,image/jpeg&dimensions=512x512,1024x1024", 0, true}, grid, 4},
	};
	int mask = 0;
	std::string got;
	for (const auto& job : jobs)
	{
		if (!job.dest)
			continue;
		if (!Fetch(client, std::string(kApi) + "/" + job.k.route + "/game/" + std::to_string(game) + "?" + kFilter + job.k.extra, key, body, error))
			continue;
		Item pick;
		for (const Item& it : Items(body))
		{
			if (it.url.empty())
				continue;
			const bool wide = job.k.min_w > 0 && it.width >= job.k.min_w;
			const bool pick_wide = job.k.min_w > 0 && pick.width >= job.k.min_w;
			if (pick.url.empty() || (wide && !pick_wide) || (wide == pick_wide && it.score > pick.score))
				pick = it;
		}
		if (pick.url.empty())
			continue;
		std::string pic;
		if (!Fetch(client, pick.url, std::string(), pic, error) || !IsPicture(pic) || !WriteFile(job.dest, pic))
			continue;
		mask |= job.bit;
		got += std::string(got.empty() ? "" : ", ") + job.k.route;
	}
	std::snprintf(msg, msgmax, "\"%s\" -> \"%s\" (%.0f%% alike): %s", name, game_name.c_str(), best * 100.0, got.empty() ? "no pictures" : got.c_str());
	return mask;
}
