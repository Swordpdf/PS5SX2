// PS5 port frontend: a game's patches and cheats from the internet (2026-10-08, AI-assisted). See fe_patchdl.h.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_patchdl.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{
namespace
{
std::string Hex64(uint64_t v)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
	return buf;
}

// What the manifest keeps of a file's bytes (FNV-1a, 64 bits: telling our own write from someone else's, not security).
std::string Fingerprint(const std::string& data)
{
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : data)
	{
		h ^= c;
		h *= 1099511628211ull;
	}
	return Hex64(h) + ":" + std::to_string(data.size());
}

bool ReadWhole(const std::string& path, std::string& out)
{
	out.clear();
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buf[16384];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	const bool ok = !std::ferror(f);
	std::fclose(f);
	return ok;
}

// Written beside, then renamed over: a cut write never leaves half a file.
bool WriteWhole(const std::string& path, const std::string& data, std::string& error)
{
	const std::string part = path + ".part";
	FILE* f = std::fopen(part.c_str(), "wb");
	if (!f)
	{
		error = "can't write " + part + " (errno " + std::to_string(errno) + ")";
		return false;
	}
	const bool wrote = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	const bool flushed = std::fflush(f) == 0;
	std::fclose(f);
	if (!wrote || !flushed || std::rename(part.c_str(), path.c_str()) != 0)
	{
		error = "can't write " + path + " (errno " + std::to_string(errno) + ")";
		std::remove(part.c_str());
		return false;
	}
	return true;
}

void MakeDir(const std::string& dir)
{
	if (!dir.empty())
		mkdir(dir.c_str(), 0777);
}

std::string Trimmed(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
		a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
		b--;
	return s.substr(a, b - a);
}

bool StartsWithNoCase(const std::string& s, const char* prefix)
{
	const size_t n = std::strlen(prefix);
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; i++)
	{
		const char a = static_cast<char>(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
		if (a != prefix[i])
			return false;
	}
	return true;
}

// A line as PCSX2's reader sees it (Patch::TrimPatchLine): spaces off both ends, then a "//" comment cut off (the spaces
// before it stay, so "[60 FPS] // x" is no group header to it either).
std::string PnachTrim(const std::string& line)
{
	std::string t = Trimmed(line);
	const size_t comment = t.find("//");
	if (comment != std::string::npos)
		t.erase(comment);
	return t;
}

// A group header to PCSX2: "[name]", or "[]" (which it reads as no name: its lines would apply by themselves).
bool IsGroupHeader(const std::string& trimmed)
{
	return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
}

// A line PCSX2 applies as a patch (patch=, dpatch=, or a cheat's same form), as Patch.cpp's reader takes them.
bool IsPatchLine(const std::string& line)
{
	const std::string t = PnachTrim(line);
	return StartsWithNoCase(t, "patch") || StartsWithNoCase(t, "dpatch");
}
} // namespace

const std::vector<OnlinePatchSource>& OnlinePatchSources()
{
	static const std::vector<OnlinePatchSource> sources = {
		{"PCSX2", "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/%s.pnach", false, "", ""},
		{"Gabominated", "https://raw.githubusercontent.com/Gabominated/PCSX2/main/PCSX2%20Patches/%s.pnach", false, "_gabominated",
			"Gabominated"},
		{"cheats", "https://raw.githubusercontent.com/xs1l3n7x/pcsx2_cheats_collection/main/cheats/%s.pnach", true, "", ""},
	};
	return sources;
}

std::string OnlinePatchUrl(const OnlinePatchSource& src, const std::string& key)
{
	std::string url = src.url;
	const size_t at = url.find("%s");
	if (at != std::string::npos)
		url.replace(at, 2, key);
	return url;
}

uint32_t ElfCrc(const std::vector<uint8_t>& elf)
{
	uint32_t crc = 0;
	for (size_t i = 0; i + 4 <= elf.size(); i += 4)
		crc ^= static_cast<uint32_t>(elf[i]) | (static_cast<uint32_t>(elf[i + 1]) << 8) | (static_cast<uint32_t>(elf[i + 2]) << 16) |
		       (static_cast<uint32_t>(elf[i + 3]) << 24);
	return crc;
}

bool LooksLikePnach(const std::string& text)
{
	return ClassifyPnach(text) == PnachKind::Patches;
}

PnachKind ClassifyPnach(const std::string& text)
{
	if (text.empty() || text.size() > (1u << 20) || text.find('\0') != std::string::npos)
		return PnachKind::NotPnach;
	bool pnach = false;
	size_t at = 0;
	while (at < text.size())
	{
		size_t nl = text.find('\n', at);
		if (nl == std::string::npos)
			nl = text.size();
		const std::string line = text.substr(at, nl - at);
		at = nl + 1;
		if (IsPatchLine(line))
			return PnachKind::Patches;
		// What a pnach has besides patches: its title, a group, or a patch line commented out ("//patch=1,EE,...").
		std::string t = Trimmed(line);
		while (t.size() >= 2 && t[0] == '/' && t[1] == '/')
			t = Trimmed(t.substr(2));
		if (StartsWithNoCase(t, "gametitle=") || StartsWithNoCase(t, "patch=") || StartsWithNoCase(t, "dpatch=") ||
			IsGroupHeader(PnachTrim(t)))
			pnach = true;
	}
	return pnach ? PnachKind::Empty : PnachKind::NotPnach;
}

std::string TagPnachGroups(const std::string& text, const std::string& tag, const std::string& unnamed)
{
	std::string out;
	bool in_group = false, opened_unnamed = false;
	int unnamed_groups = 0; // PCSX2 keeps only the first of two groups with one name: each unnamed one gets its own
	const auto unnamed_name = [&]() {
		return ++unnamed_groups == 1 ? unnamed : unnamed + " " + std::to_string(unnamed_groups);
	};
	size_t at = 0;
	while (at < text.size())
	{
		size_t nl = text.find('\n', at);
		const bool last = nl == std::string::npos;
		if (last)
			nl = text.size();
		std::string line = text.substr(at, nl - at);
		at = nl + 1;
		const std::string t = PnachTrim(line);
		if (IsGroupHeader(t))
		{
			const std::string name = t.substr(1, t.size() - 2);
			in_group = true;
			line = name.empty() ? "[" + unnamed_name() + "]" : "[" + name + " (" + tag + ")]";
		}
		else if (!in_group && !opened_unnamed && IsPatchLine(t))
		{
			// Patch lines before any group would apply by themselves: they go under one group of their own.
			out += "[" + unnamed_name() + "]\n";
			opened_unnamed = true;
			in_group = true;
		}
		out += line;
		if (!last)
			out += "\n";
	}
	return out;
}

int CountPnachGroups(const std::string& text, bool* unnamed)
{
	int groups = 0;
	bool in_group = false, loose = false;
	size_t at = 0;
	while (at < text.size())
	{
		size_t nl = text.find('\n', at);
		if (nl == std::string::npos)
			nl = text.size();
		const std::string t = PnachTrim(text.substr(at, nl - at));
		at = nl + 1;
		if (IsGroupHeader(t))
		{
			groups++;
			in_group = true;
		}
		else if (!in_group && IsPatchLine(t))
			loose = true;
	}
	if (unnamed)
		*unnamed = loose;
	return groups;
}

OnlinePatches::OnlinePatches(OnlinePatchPlatform platform, std::string patches_dir, std::string cheats_dir, std::string manifest)
	: m_platform(std::move(platform))
	, m_patches(std::move(patches_dir))
	, m_cheats(std::move(cheats_dir))
	, m_manifest_path(std::move(manifest))
{
	LoadManifest();
}

OnlinePatches::~OnlinePatches()
{
	Stop(2000);
	if (m_thread.joinable())
		m_thread.detach(); // a request that never ends (no network timeout) must not hang the app's end
}

void OnlinePatches::Log(const std::string& line) const
{
	if (m_platform.log)
		m_platform.log("[patches] " + line);
}

void OnlinePatches::LoadManifest()
{
	std::string text;
	if (m_manifest_path.empty() || !ReadWhole(m_manifest_path, text))
		return;
	size_t at = 0;
	while (at < text.size())
	{
		size_t nl = text.find('\n', at);
		if (nl == std::string::npos)
			nl = text.size();
		const std::string line = text.substr(at, nl - at);
		at = nl + 1;
		const size_t tab = line.find('\t');
		if (tab != std::string::npos && tab > 0)
			m_manifest[line.substr(0, tab)] = Trimmed(line.substr(tab + 1));
	}
}

void OnlinePatches::SaveManifest() const
{
	if (m_manifest_path.empty())
		return;
	std::string text = "# Patch and cheat files PS5SX2 downloaded (the sheet's \"Get patches and cheats\"), and what it wrote in each:\n"
	                   "# a file that still holds that is replaced when its source changes; one changed since is left alone.\n";
	for (const auto& [path, print] : m_manifest)
		text += path + "\t" + print + "\n";
	const size_t slash = m_manifest_path.rfind('/');
	if (slash != std::string::npos)
		MakeDir(m_manifest_path.substr(0, slash));
	std::string error;
	if (!WriteWhole(m_manifest_path, text, error))
		Log(error);
}

OnlinePatchStatus OnlinePatches::Status(const std::string& serial) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto it = m_status.find(serial);
	return it == m_status.end() ? OnlinePatchStatus{} : it->second;
}

void OnlinePatches::SetStatus(const std::string& serial, OnlinePatchStatus::State state, const std::string& message)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_status[serial] = {state, message};
}

bool OnlinePatches::Begin(const GameInfo& game)
{
	if (game.serial.empty() || !m_platform.get_text)
		return false;
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_stop)
		return false;
	auto& st = m_status[game.serial];
	if (st.state == OnlinePatchStatus::State::Working)
		return false;
	st = {OnlinePatchStatus::State::Working, "Waiting"};
	m_queue.push_back(game);
	if (!m_running)
	{
		if (m_thread.joinable())
			m_thread.join();
		m_running = true;
		m_thread = std::thread([this] { Run(); });
	}
	m_cv.notify_all();
	return true;
}

bool OnlinePatches::Stop(int wait_ms)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_stop = true;
	m_queue.clear();
	m_cv.notify_all();
	const bool ended = m_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [this] { return !m_running; });
	lock.unlock();
	if (ended && m_thread.joinable())
		m_thread.join();
	return ended;
}

void OnlinePatches::Run()
{
	if (m_platform.thread_start)
		m_platform.thread_start();
	for (;;)
	{
		GameInfo game;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			if (m_stop || m_queue.empty())
			{
				m_running = false;
				m_cv.notify_all();
				return;
			}
			game = m_queue.front();
			m_queue.pop_front();
		}
		const OnlinePatchStatus result = Fetch(game);
		SetStatus(game.serial, result.state, result.message);
	}
}

uint32_t OnlinePatches::DiscCrc(const std::string& image, std::string& error)
{
	std::string name;
	std::vector<uint8_t> bytes;
	if (!ReadAchievementExecutable(image, name, bytes) || bytes.size() < 4)
	{
		error = "the game's executable couldn't be read from the disc image";
		return 0;
	}
	return ElfCrc(bytes);
}

OnlinePatchStatus OnlinePatches::Fetch(const GameInfo& game)
{
	SetStatus(game.serial, OnlinePatchStatus::State::Working, "Reading the game's CRC");
	std::string error;
	const uint32_t crc = DiscCrc(game.path, error);
	if (crc == 0)
	{
		Log(game.serial + ": " + error);
		return {OnlinePatchStatus::State::Failed, "Couldn't read the game's CRC"};
	}
	return FetchWithCrc(game.serial, crc);
}

OnlinePatchStatus OnlinePatches::FetchWithCrc(const std::string& serial, uint32_t crc)
{
	char key[48];
	std::snprintf(key, sizeof(key), "%s_%08X", serial.c_str(), crc);
	Log(std::string(key) + ": looking online");
	int found_patches = 0, found_cheats = 0, fetched = 0, kept = 0, failed = 0, missing = 0, empty = 0;
	std::string why;
	for (const OnlinePatchSource& src : OnlinePatchSources())
	{
		SetStatus(serial, OnlinePatchStatus::State::Working, std::string("Asking ") + src.name);
		const std::string url = OnlinePatchUrl(src, key); // vk-285-134b: not snprintf (see fe_patchdl.h)
		std::string body;
		const int status = m_platform.get_text(url, body);
		if (status == 404)
		{
			missing++;
			Log(std::string(key) + ": " + src.name + ": none (404)");
			continue;
		}
		if (status == 200 && ClassifyPnach(body) == PnachKind::Empty)
		{
			// vk-285-135: a file with nothing to apply (every line commented out) is no patch for this game: like a 404.
			missing++;
			empty++;
			Log(std::string(key) + ": " + src.name + ": its file has no patch turned on (" + std::to_string(body.size()) +
				" bytes, every patch line commented out): nothing to fetch");
			continue;
		}
		if (status != 200 || !LooksLikePnach(body))
		{
			failed++;
			why = status == 200 ? std::string(src.name) + " sent something that isn't a patch file"
			                    : std::string(src.name) + (status < 0 ? ": no connection" : ": HTTP " + std::to_string(status));
			Log(std::string(key) + ": " + why);
			continue;
		}
		std::string text = body;
		if (src.tag[0] != '\0')
			text = TagPnachGroups(text, src.tag, src.tag);
		if (text.empty() || text.back() != '\n')
			text += "\n";
		const std::string dir = src.cheats ? m_cheats : m_patches;
		MakeDir(dir);
		const std::string path = dir + "/" + key + src.suffix + ".pnach";
		const int groups = CountPnachGroups(text);
		std::string current;
		const bool exists = ReadWhole(path, current);
		const auto mine = m_manifest.find(path);
		if (exists && current == text)
		{
			Log(std::string(key) + ": " + src.name + ": " + path + " is up to date");
		}
		else if (exists && (mine == m_manifest.end() || mine->second != Fingerprint(current)))
		{
			kept++;
			Log(std::string(key) + ": " + src.name + ": " + path + " is there and wasn't written by this app: left as it is");
			(src.cheats ? found_cheats : found_patches) += groups;
			continue;
		}
		else
		{
			std::string werr;
			if (!WriteWhole(path, text, werr))
			{
				failed++;
				why = werr;
				Log(std::string(key) + ": " + src.name + ": " + werr);
				continue;
			}
			m_manifest[path] = Fingerprint(text);
			SaveManifest();
			Log(std::string(key) + ": " + src.name + ": " + path + (exists ? " updated" : " written") + " (" + std::to_string(groups) +
				" group(s), " + std::to_string(body.size()) + " bytes)");
		}
		fetched++;
		(src.cheats ? found_cheats : found_patches) += groups;
	}
	std::string message;
	if (found_patches || found_cheats)
	{
		message = std::to_string(found_patches) + (found_patches == 1 ? " patch" : " patches") + ", " + std::to_string(found_cheats) +
		          (found_cheats == 1 ? " cheat" : " cheats");
		if (kept)
			message += " (your own files kept)";
		if (failed)
			message += "; " + why;
		Log(std::string(key) + ": " + message);
		return {OnlinePatchStatus::State::Done, message};
	}
	if (failed)
		return {OnlinePatchStatus::State::Failed, why};
	(void)fetched;
	(void)missing;
	// vk-285-135: say so when a source does have a file, with nothing turned on in it (Ratchet & Clank in PCSX2's list).
	return {OnlinePatchStatus::State::Done, empty ? "Nothing online for this game (its patches there are turned off)" : "Nothing online for this game"};
}

OnlinePatchService OnlinePatches::Service()
{
	OnlinePatchService s;
	s.status = [this](const std::string& serial) { return Status(serial); };
	s.begin = [this](const GameInfo& g) { return Begin(g); };
	return s;
}
} // namespace fe
