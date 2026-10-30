// PS5 port frontend: updating PS5SX2 itself from the shelf (vk-285-180, AI-assisted). See fe_update.h.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_update.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace fe
{
namespace
{
// ---- a small, strict JSON reader: enough for GitHub's release answer (as ps5/installer/src/json.c) ----

struct JValue
{
	enum class Type
	{
		Null,
		False,
		True,
		Number,
		String,
		Array,
		Object
	};
	Type type = Type::Null;
	std::string text; // String: decoded UTF-8; Number: as written
	std::vector<std::string> keys; // Object: the member names, in order ...
	std::vector<JValue> items;     // ... and their values (Array: the items)

	const JValue* Get(const char* key) const
	{
		if (type != Type::Object)
			return nullptr;
		for (size_t i = 0; i < keys.size() && i < items.size(); i++)
			if (keys[i] == key)
				return &items[i];
		return nullptr;
	}
	const std::string* Str() const
	{
		return type == Type::String && text.find('\0') == std::string::npos ? &text : nullptr;
	}
	bool Int(long long& out) const
	{
		if (type != Type::Number || text.empty() || text.size() > 18)
			return false;
		size_t i = text[0] == '-' ? 1 : 0;
		if (i == text.size())
			return false;
		long long v = 0;
		for (; i < text.size(); i++)
		{
			if (text[i] < '0' || text[i] > '9')
				return false; // a fraction or an exponent: not an integer
			v = v * 10 + (text[i] - '0');
		}
		out = text[0] == '-' ? -v : v;
		return true;
	}
};

struct JParser
{
	explicit JParser(const std::string& text) : s(text) {}
	const std::string& s;
	size_t at = 0;
	int depth = 0;
	std::string error;

	bool Fail(const char* why)
	{
		if (error.empty())
			error = why;
		return false;
	}
	void Ws()
	{
		while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r'))
			at++;
	}
	static void Utf8(std::string& out, uint32_t cp)
	{
		if (cp < 0x80)
			out += static_cast<char>(cp);
		else if (cp < 0x800)
		{
			out += static_cast<char>(0xc0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3f));
		}
		else if (cp < 0x10000)
		{
			out += static_cast<char>(0xe0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (cp & 0x3f));
		}
		else
		{
			out += static_cast<char>(0xf0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (cp & 0x3f));
		}
	}
	bool Hex4(uint32_t& v)
	{
		if (at + 4 > s.size())
			return false;
		v = 0;
		for (int i = 0; i < 4; i++)
		{
			const char c = s[at++];
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= static_cast<uint32_t>(c - 'A' + 10);
			else
				return false;
		}
		return true;
	}
	// At the opening quote.
	bool String(std::string& out)
	{
		at++;
		while (at < s.size())
		{
			const unsigned char c = static_cast<unsigned char>(s[at]);
			if (c == '"')
			{
				at++;
				return true;
			}
			if (c < 0x20)
				return Fail("a control character in a string");
			if (c != '\\')
			{
				out += static_cast<char>(c);
				at++;
				continue;
			}
			if (at + 1 >= s.size())
				break;
			const char e = s[at + 1];
			at += 2;
			switch (e)
			{
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u':
				{
					uint32_t cp = 0;
					if (!Hex4(cp))
						return Fail("a bad \\u escape");
					if (cp >= 0xd800 && cp <= 0xdbff)
					{
						uint32_t lo = 0;
						const size_t keep = at;
						bool pair = at + 1 < s.size() && s[at] == '\\' && s[at + 1] == 'u';
						if (pair)
						{
							at += 2;
							pair = Hex4(lo) && lo >= 0xdc00 && lo <= 0xdfff;
						}
						if (pair)
							cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
						else
						{
							at = keep;
							cp = 0xfffd;
						}
					}
					else if (cp >= 0xdc00 && cp <= 0xdfff)
						cp = 0xfffd;
					Utf8(out, cp);
					break;
				}
				default:
					return Fail("a bad escape");
			}
		}
		return Fail("an unfinished string");
	}
	bool Number(std::string& out)
	{
		const size_t start = at;
		if (at < s.size() && s[at] == '-')
			at++;
		if (at >= s.size() || s[at] < '0' || s[at] > '9')
			return Fail("a bad number");
		if (s[at] == '0')
			at++;
		else
			while (at < s.size() && s[at] >= '0' && s[at] <= '9')
				at++;
		if (at < s.size() && s[at] == '.')
		{
			at++;
			if (at >= s.size() || s[at] < '0' || s[at] > '9')
				return Fail("a bad number");
			while (at < s.size() && s[at] >= '0' && s[at] <= '9')
				at++;
		}
		if (at < s.size() && (s[at] == 'e' || s[at] == 'E'))
		{
			at++;
			if (at < s.size() && (s[at] == '+' || s[at] == '-'))
				at++;
			if (at >= s.size() || s[at] < '0' || s[at] > '9')
				return Fail("a bad number");
			while (at < s.size() && s[at] >= '0' && s[at] <= '9')
				at++;
		}
		out.assign(s, start, at - start);
		return true;
	}
	bool Literal(const char* w)
	{
		const size_t n = std::strlen(w);
		if (s.compare(at, n, w) != 0)
			return Fail("an unexpected character");
		at += n;
		return true;
	}
	bool Value(JValue& v)
	{
		Ws();
		if (at >= s.size())
			return Fail("an unexpected end");
		const char c = s[at];
		if (c == '"')
		{
			v.type = JValue::Type::String;
			return String(v.text);
		}
		if (c == '-' || (c >= '0' && c <= '9'))
		{
			v.type = JValue::Type::Number;
			return Number(v.text);
		}
		if (c == 't')
		{
			v.type = JValue::Type::True;
			return Literal("true");
		}
		if (c == 'f')
		{
			v.type = JValue::Type::False;
			return Literal("false");
		}
		if (c == 'n')
		{
			v.type = JValue::Type::Null;
			return Literal("null");
		}
		if (c != '{' && c != '[')
			return Fail("an unexpected character");
		if (++depth > 32)
			return Fail("nested too deep");
		const bool object = c == '{';
		v.type = object ? JValue::Type::Object : JValue::Type::Array;
		const char close = object ? '}' : ']';
		at++;
		Ws();
		if (at < s.size() && s[at] == close)
		{
			at++;
			depth--;
			return true;
		}
		for (;;)
		{
			if (object)
			{
				Ws();
				std::string key;
				if (at >= s.size() || s[at] != '"' || !String(key))
					return Fail("expected a member name");
				Ws();
				if (at >= s.size() || s[at] != ':')
					return Fail("expected ':'");
				at++;
				v.keys.push_back(std::move(key));
				v.items.emplace_back();
				if (!Value(v.items.back()))
					return false;
			}
			else
			{
				v.items.emplace_back();
				if (!Value(v.items.back()))
					return false;
			}
			Ws();
			if (at < s.size() && s[at] == ',')
			{
				at++;
				continue;
			}
			if (at < s.size() && s[at] == close)
			{
				at++;
				depth--;
				return true;
			}
			return Fail("expected ',' or the end of a list");
		}
	}
};

bool SafeName(const std::string& s, size_t max)
{
	if (s.empty() || s.size() > max || s[0] == '.')
		return false;
	for (const char c : s)
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == '+'))
			return false;
	return true;
}

bool EndsWithZip(const std::string& n)
{
	if (n.size() < 5)
		return false;
	std::string tail = n.substr(n.size() - 4);
	for (char& c : tail)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return tail == ".zip";
}

// "vk-285-112" -> prefix "vk" and the numbers 285, 112 (app.c split_tag).
bool SplitTag(const std::string& t, std::string& prefix, std::vector<long long>& nums)
{
	prefix.clear();
	nums.clear();
	size_t p = 0;
	for (;;)
	{
		const size_t dash = t.find('-', p);
		const std::string part = t.substr(p, dash == std::string::npos ? std::string::npos : dash - p);
		bool digits = !part.empty() && part.size() <= 12;
		for (const char c : part)
			digits = digits && c >= '0' && c <= '9';
		if (digits)
		{
			if (nums.size() == 8)
				return false;
			nums.push_back(std::stoll(part));
		}
		else
		{
			if (!nums.empty() || part.empty() || prefix.size() + part.size() + 1 >= 64)
				return false; // words after numbers, or empty parts: not our kind of tag
			if (!prefix.empty())
				prefix += '-';
			prefix += part;
		}
		if (dash == std::string::npos)
			break;
		p = dash + 1;
	}
	return !nums.empty();
}
} // namespace

bool ParseLatestRelease(const std::string& json, const std::string& download_prefix, ReleaseInfo& out, std::string& error)
{
	out = {};
	JParser p{json};
	JValue root;
	if (!p.Value(root))
	{
		error = "GitHub's answer couldn't be read (" + p.error + ")";
		return false;
	}
	p.Ws();
	if (p.at != json.size() || root.type != JValue::Type::Object)
	{
		error = "GitHub's answer couldn't be read (not one JSON object)";
		return false;
	}
	const std::string* tag = root.Get("tag_name") ? root.Get("tag_name")->Str() : nullptr;
	if (!tag || !SafeName(*tag, 64))
	{
		error = "the release has no usable tag";
		return false;
	}
	if (const JValue* d = root.Get("draft"); d && d->type == JValue::Type::True)
	{
		error = "the latest release is a draft";
		return false;
	}
	out.tag = *tag;
	const std::string* title = root.Get("name") ? root.Get("name")->Str() : nullptr;
	out.title = title && !title->empty() ? *title : *tag;
	if (out.title.size() > 100)
		out.title.resize(100);
	for (char& c : out.title)
		if (static_cast<unsigned char>(c) < 0x20)
			c = ' ';
	if (const JValue* pub = root.Get("published_at"); pub && pub->Str())
		out.published_at = *pub->Str();

	const JValue* assets = root.Get("assets");
	const JValue* pick = nullptr;
	int zips = 0;
	if (assets && assets->type == JValue::Type::Array)
		for (const JValue& a : assets->items)
		{
			const std::string* name = a.Get("name") ? a.Get("name")->Str() : nullptr;
			const std::string* state = a.Get("state") ? a.Get("state")->Str() : nullptr;
			if (!name || !EndsWithZip(*name))
				continue;
			if (state && *state != "uploaded")
				continue;
			zips++;
			if (!pick && name->compare(0, 7, "PS5SX2-") == 0)
				pick = &a;
		}
	if (!pick)
	{
		error = zips ? "the release has no PS5SX2 zip" : "the release has no zip file";
		return false;
	}
	const std::string name = *pick->Get("name")->Str();
	const std::string* url = pick->Get("browser_download_url") ? pick->Get("browser_download_url")->Str() : nullptr;
	const std::string* digest = pick->Get("digest") ? pick->Get("digest")->Str() : nullptr;
	long long size = 0;
	if (!SafeName(name, 200))
	{
		error = "the zip's name has characters the installer doesn't accept";
		return false;
	}
	if (!pick->Get("size") || !pick->Get("size")->Int(size) || size < 1024 || static_cast<unsigned long long>(size) > kUpdateMaxZipBytes)
	{
		error = "the zip's size isn't plausible";
		return false;
	}
	bool hex = digest && digest->size() == 7 + 64 && digest->compare(0, 7, "sha256:") == 0;
	std::string sha = hex ? digest->substr(7) : std::string();
	for (char& c : sha)
	{
		hex = hex && std::isxdigit(static_cast<unsigned char>(c));
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (!hex)
	{
		error = "GitHub gave no SHA-256 for the zip, so it can't be checked";
		return false;
	}
	if (!url || *url != download_prefix + out.tag + "/" + name)
	{
		error = "the zip's address isn't this repository's release download";
		return false;
	}
	out.asset_name = name;
	out.asset_url = *url;
	out.size = static_cast<uint64_t>(size);
	out.sha256 = sha;
	// installer-dl: the release's PS5SX2Installer.elf, when it has a usable one (not having one doesn't refuse the release: the app may ship its own)
	if (assets && assets->type == JValue::Type::Array)
		for (const JValue& a : assets->items)
		{
			const std::string* iname = a.Get("name") ? a.Get("name")->Str() : nullptr;
			const std::string* istate = a.Get("state") ? a.Get("state")->Str() : nullptr;
			const std::string* iurl = a.Get("browser_download_url") ? a.Get("browser_download_url")->Str() : nullptr;
			const std::string* idigest = a.Get("digest") ? a.Get("digest")->Str() : nullptr;
			long long isize = 0;
			if (!iname || *iname != kInstallerAssetName || (istate && *istate != "uploaded"))
				continue;
			if (!a.Get("size") || !a.Get("size")->Int(isize) || isize <= 4096 || static_cast<unsigned long long>(isize) > kInstallerMaxBytes)
				continue;
			if (!iurl || *iurl != download_prefix + out.tag + "/" + kInstallerAssetName)
				continue;
			std::string isha;
			if (idigest)
			{
				bool ok = idigest->size() == 7 + 64 && idigest->compare(0, 7, "sha256:") == 0;
				isha = ok ? idigest->substr(7) : std::string();
				for (char& c : isha)
				{
					ok = ok && std::isxdigit(static_cast<unsigned char>(c));
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				if (!ok)
					continue; // a digest that isn't a SHA-256: not trusted
			}
			out.installer_url = *iurl;
			out.installer_size = static_cast<uint64_t>(isize);
			out.installer_sha256 = isha;
			break;
		}
	return true;
}

// installer-dl: FIPS 180-4 SHA-256 (the installer checks the zip itself, in ps5/installer; this one is for the installer the app fetches).
std::string Sha256Hex(const uint8_t* data, size_t n)
{
	static const uint32_t k[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
		0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351,
		0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
		0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3,
		0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	auto rotr = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
	auto block = [&](const uint8_t* b) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (static_cast<uint32_t>(b[i * 4]) << 24) | (static_cast<uint32_t>(b[i * 4 + 1]) << 16) | (static_cast<uint32_t>(b[i * 4 + 2]) << 8) |
				   b[i * 4 + 3];
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = bb;
			bb = a;
			a = t1 + t2;
		}
		h[0] += a;
		h[1] += bb;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	};
	size_t i = 0;
	for (; i + 64 <= n; i += 64)
		block(data + i);
	uint8_t tail[128] = {};
	const size_t rest = n - i;
	if (rest)
		std::memcpy(tail, data + i, rest);
	tail[rest] = 0x80;
	const size_t len = rest + 9 <= 64 ? 64 : 128;
	const uint64_t bits = static_cast<uint64_t>(n) * 8;
	for (int j = 0; j < 8; j++)
		tail[len - 1 - j] = static_cast<uint8_t>(bits >> (8 * j));
	block(tail);
	if (len == 128)
		block(tail + 64);
	static const char* const hex = "0123456789abcdef";
	std::string out;
	for (const uint32_t v : h)
		for (int sh = 28; sh >= 0; sh -= 4)
			out += hex[(v >> sh) & 0xf];
	return out;
}

int CompareBuildTags(const std::string& a, const std::string& b)
{
	if (a == b)
		return 0;
	std::string pa, pb;
	std::vector<long long> na, nb;
	if (!SplitTag(a, pa, na) || !SplitTag(b, pb, nb) || pa != pb || na.size() != nb.size())
		return 2;
	for (size_t i = 0; i < na.size(); i++)
		if (na[i] != nb[i])
			return na[i] < nb[i] ? -1 : 1;
	return 0;
}

const char* UpdateStateName(UpdateStatus::State s)
{
	using S = UpdateStatus::State;
	switch (s)
	{
		case S::Idle: return "idle";
		case S::Checking: return "checking";
		case S::Current: return "up to date";
		case S::Newer: return "newer than the release";
		case S::Available: return "available";
		case S::Failed: return "failed";
		case S::Interrupted: return "an install was cut off";
		case S::Starting: return "starting the installer";
		case S::Started: return "the installer runs";
		case S::StartFailed: return "the installer didn't start";
		case S::DownloadingInstaller: return "downloading the installer";
	}
	return "?";
}

UpdateStatus JudgeRelease(const std::string& this_tag, int http_status, const std::string& body, const std::string& fetch_error,
	const std::string& download_prefix)
{
	UpdateStatus st;
	using S = UpdateStatus::State;
	st.state = S::Failed;
	if (http_status < 0)
	{
		st.error = "couldn't reach GitHub" + (fetch_error.empty() ? std::string() : " (" + fetch_error + ")");
		return st;
	}
	if (http_status == 404)
	{
		st.error = "no release is published yet";
		return st;
	}
	if (http_status == 403 || http_status == 429)
	{
		st.error = "GitHub's hourly limit for this network is used up; try again in an hour";
		return st;
	}
	if (http_status != 200)
	{
		st.error = "GitHub answered HTTP " + std::to_string(http_status);
		return st;
	}
	if (!ParseLatestRelease(body, download_prefix, st.latest, st.error))
		return st;
	const int cmp = CompareBuildTags(this_tag, st.latest.tag);
	if (cmp == 2)
	{
		// The installer would install it (it can't compare either, so it treats the build as not installed); say so plainly.
		st.state = S::Available;
		return st;
	}
	st.state = cmp == 0 ? S::Current : cmp > 0 ? S::Newer : S::Available;
	return st;
}

// ---- the updater ----

Updater::Updater(UpdatePlatform platform, std::string this_tag, std::string work_dir, std::string api_url, std::string download_prefix)
	: m_platform(std::move(platform))
	, m_tag(std::move(this_tag))
	, m_work(std::move(work_dir))
	, m_api(std::move(api_url))
	, m_prefix(std::move(download_prefix))
{
	if (m_platform.exists && m_platform.exists(m_work + "/journal.txt"))
	{
		m_status.state = UpdateStatus::State::Interrupted;
		Log("an install was cut off (" + m_work + "/journal.txt): the row offers the installer to undo or finish it");
	}
}

Updater::~Updater()
{
	if (m_thread.joinable())
		m_thread.join();
}

void Updater::Log(const std::string& line) const
{
	if (m_platform.log)
		m_platform.log(line);
}

UpdateStatus Updater::Status() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_status;
}

UpdateStatus Updater::Look(bool automatic)
{
	std::string body, error;
	const int http = m_platform.get_text ? m_platform.get_text(m_api, body, error) : -1;
	UpdateStatus st = JudgeRelease(m_tag, http, body, m_platform.get_text ? error : "no network client", m_prefix);
	st.automatic = automatic;
	Log(std::string("update check") + (automatic ? " (at start)" : "") + ": this build " + m_tag + ", latest release " +
		(st.latest.tag.empty() ? std::string("?") : st.latest.tag + " (" + st.latest.asset_name + ", " + std::to_string(st.latest.size) + " bytes)") +
		": " + UpdateStateName(st.state) + (st.error.empty() ? std::string() : " - " + st.error));
	return st;
}

bool Updater::Check(bool automatic)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		using S = UpdateStatus::State;
		if (m_running || m_status.state == S::Starting || m_status.state == S::Started)
			return false;
		// A cut-off install stays the row's news until the installer has dealt with it.
		if (m_status.state == S::Interrupted && automatic)
			return false;
		m_running = true;
		m_status.state = S::Checking;
		m_status.error.clear();
		m_status.automatic = automatic;
	}
	if (m_thread.joinable())
		m_thread.join();
	m_thread = std::thread([this, automatic]() {
		if (m_platform.thread_start)
			m_platform.thread_start();
		UpdateStatus st = Look(automatic);
		if (automatic && st.state == UpdateStatus::State::Available && m_platform.notify)
			m_platform.notify(st.latest.title + " (" + st.latest.tag + ")");
		std::lock_guard<std::mutex> lock(m_mutex);
		m_status = std::move(st);
		m_running = false;
		m_cv.notify_all();
	});
	return true;
}

bool Updater::Install()
{
	using S = UpdateStatus::State;
	std::string expect;
	ReleaseInfo release;
	const bool have = !m_platform.installer_present || m_platform.installer_present();
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_running || (m_status.state != S::Available && m_status.state != S::Interrupted && m_status.state != S::StartFailed))
			return false;
		expect = m_status.latest.tag;
		release = m_status.latest;
		m_status.state = have ? S::Starting : S::DownloadingInstaller;
		m_status.error.clear();
		if (!have)
			m_running = true;
	}
	if (have)
		return StartInstaller(expect);
	// installer-dl (owner, 2026-10-30; a tester who copied only eboot.bin: "it said it did not have the elf file"): no PS5SX2Installer.elf next to the
	// eboot: fetched from the release on the worker, put in the app's folder, then started as usual. The row says "Downloading the installer...".
	Log("update: no PS5SX2Installer.elf in /app0 or the app's folder: it is fetched from the release first");
	if (m_thread.joinable())
		m_thread.join();
	m_thread = std::thread([this, expect, release]() {
		if (m_platform.thread_start)
			m_platform.thread_start();
		std::string error;
		if (FetchInstaller(release, error))
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_status.state = S::Starting;
			}
			StartInstaller(expect);
		}
		else
		{
			Log("update: the installer couldn't be fetched: " + error);
			std::lock_guard<std::mutex> lock(m_mutex);
			m_status.state = S::StartFailed;
			m_status.error = error;
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		m_running = false;
		m_cv.notify_all();
	});
	return true;
}

bool Updater::StartInstaller(const std::string& expect)
{
	using S = UpdateStatus::State;
	std::string detail;
	const int r = m_platform.start_installer ? m_platform.start_installer(expect, detail) : 1;
	Log("update: the installer " + std::string(r == 0 ? "started" : "didn't start") + " for " + (expect.empty() ? "the latest" : expect) + ": " + detail);
	std::lock_guard<std::mutex> lock(m_mutex);
	if (r == 0)
	{
		m_status.state = S::Started;
		m_status.error.clear();
		return true;
	}
	m_status.state = S::StartFailed;
	m_status.error = r == 1 ? "this PS5SX2 has no PS5SX2Installer.elf" : r == 2 ? "PS5SX2Installer.elf is not one this PS5SX2 can start" :
		r == 3 ? "no ELF loader answered on port 9021" : "the ELF loader stopped taking it";
	return false;
}

bool Updater::FetchInstaller(ReleaseInfo release, std::string& error)
{
	if (release.installer_url.empty() && release.tag.empty())
	{
		// a cut-off install offered before any look: the release is looked at now
		Log("installer: the release isn't known yet: looking at it");
		const UpdateStatus st = Look(false);
		if (st.latest.tag.empty())
		{
			error = "couldn't look at the release for its installer" + (st.error.empty() ? std::string() : ": " + st.error);
			return false;
		}
		release = st.latest;
	}
	if (release.installer_url.empty())
	{
		error = "release " + release.tag + " has no PS5SX2Installer.elf to download";
		return false;
	}
	if (!m_platform.get_file || !m_platform.write_file || !m_platform.rename_file)
	{
		error = "this PS5SX2 can't download files";
		return false;
	}
	Log("installer: downloading " + release.installer_url + " (" + std::to_string(release.installer_size) + " bytes, SHA-256 " +
		(release.installer_sha256.empty() ? std::string("not given by GitHub: only the size is checked") : release.installer_sha256) + ")");
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<uint8_t> data;
	std::string fetch_error;
	const int http = m_platform.get_file(release.installer_url, kInstallerMaxBytes, data, fetch_error);
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (http < 0)
	{
		error = "couldn't download the installer" + (fetch_error.empty() ? std::string() : " (" + fetch_error + ")");
		return false;
	}
	if (http != 200)
	{
		error = "GitHub answered HTTP " + std::to_string(http) + " for the installer";
		return false;
	}
	char took[64];
	std::snprintf(took, sizeof(took), "%.1f s", secs);
	Log("installer: " + std::to_string(data.size()) + " bytes in " + took);
	if (data.size() != release.installer_size)
	{
		error = "the installer downloaded is " + std::to_string(data.size()) + " bytes, the release says " + std::to_string(release.installer_size);
		return false;
	}
	if (!release.installer_sha256.empty())
	{
		const std::string got = Sha256Hex(data.data(), data.size());
		if (got != release.installer_sha256)
		{
			error = "the installer downloaded doesn't match GitHub's SHA-256";
			Log("installer: SHA-256 " + got + ", GitHub's " + release.installer_sha256);
			return false;
		}
		Log("installer: SHA-256 matches GitHub's");
	}
	if (data.size() < 4 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0)
	{
		error = "the file downloaded is not an ELF";
		return false;
	}
	const std::string dest = m_platform.installer_dest.empty() ? std::string(kInstallerDest) : m_platform.installer_dest;
	const std::string part = dest + ".part";
	std::string write_error;
	if (!m_platform.write_file(part, data, write_error))
	{
		error = "couldn't save the installer (" + write_error + ")";
		return false;
	}
	if (!m_platform.rename_file(part, dest))
	{
		error = "couldn't put the installer in place as " + dest;
		return false;
	}
	Log("installer: saved as " + dest + "; it is started as usual");
	return true;
}

bool Updater::LogUpload() const
{
	return !(m_platform.exists && m_platform.exists(m_work + "/no-log-upload"));
}

bool Updater::SetLogUpload(bool on)
{
	const bool ok = m_platform.set_file && m_platform.set_file(m_work + "/no-log-upload", !on);
	Log(std::string("session logs: ") + (on ? "on" : "off") + (ok ? "" : " (the switch file couldn't be changed)"));
	return ok;
}

bool Updater::Stop(int wait_ms)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	const bool done = m_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [this] { return !m_running; });
	lock.unlock();
	if (done && m_thread.joinable())
		m_thread.join();
	return done; // still running: the caller leaves this object alive (the destructor would wait for the look)
}

UpdateService Updater::Service()
{
	UpdateService s;
	s.status = [this] { return Status(); };
	s.check = [this](bool automatic) { return Check(automatic); };
	s.install = [this] { return Install(); };
	s.log_upload = [this] { return LogUpload(); };
	s.set_log_upload = [this](bool on) { return SetLogUpload(on); };
	s.quit = m_platform.quit;
	s.this_tag = m_tag;
	return s;
}
} // namespace fe
