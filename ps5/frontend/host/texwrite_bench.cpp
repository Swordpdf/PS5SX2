// PS5 port frontend, host tests (2026-10-29, AI-assisted): how long an HD pack takes to download and unpack under the console's write limits for a
// title (write_model.c, by LD_PRELOAD), with the app writing it itself (the flag texpack_titlewrites) and with the writer process (fe_texwrite.h).
// texwrite-bench.sh builds and runs it; the numbers are a model's, not the console's.
//
//   texwrite_bench <pack .zip> <serial> <work dir> titlewrites|writer
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_texpacks.h"
#include "fe_texwrite.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace fe;
using State = TexturePackStatus::State;

static double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string ReadAll(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

int main(int argc, char** argv)
{
	if (argc < 5)
	{
		std::printf("usage: texwrite_bench <pack .zip> <serial> <work dir> titlewrites|writer\n");
		return 2;
	}
	const std::string pack = argv[1], serial = argv[2], work = argv[3], mode = argv[4];
	const std::string dir = pack.substr(0, pack.rfind('/')), name = pack.substr(pack.rfind('/') + 1);
	const std::string data = ReadAll(pack);
	mkdir(work.c_str(), 0777);
	const std::string textures = work + "/textures";
	mkdir(textures.c_str(), 0777);

	TexturePackPlatform p;
	const std::string list = "{\"files\":[{\"name\":\"" + name + "\",\"source\":\"original\",\"size\":\"" + std::to_string(data.size()) + "\",\"md5\":\"" +
		TexturePackMd5(data.data(), data.size()) + "\",\"format\":\"ZIP\",\"filecount\":\"1\"}]}";
	p.get_text = [&](const std::string& url, std::string& body) {
		body = list;
		return url == TexturePackMetadataUrl() ? 200 : 404;
	};
	// the network's blocks are TLS records: 16 KiB at most
	p.get_range = [&](const std::string&, uint64_t offset, uint64_t length, const std::function<bool(const void*, size_t)>& sink) {
		uint64_t at = offset;
		const uint64_t end = std::min<uint64_t>(offset + length, data.size());
		while (at < end)
		{
			const size_t n = static_cast<size_t>(std::min<uint64_t>(16384, end - at));
			if (!sink(data.data() + at, n))
				return -2;
			at += n;
		}
		return 206;
	};
	p.free_bytes = [](const std::string&) { return UINT64_MAX; };
	p.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
	p.flag = [mode](const char* n) { return mode == "titlewrites" && std::string(n) == "texpack_titlewrites"; };
	p.launch_writer = [work](const std::string& request, std::string& why) {
		const char* bin = std::getenv("TEX_WRITER_BIN");
		const std::string elf = bin ? ReadAll(bin) : std::string();
		const std::vector<uint8_t> patched = TexWriterPatched(reinterpret_cast<const uint8_t*>(elf.data()), elf.size(), request);
		if (patched.empty())
		{
			why = "no TEX_WRITER_BIN";
			return false;
		}
		const std::string path = work + "/tex_writer.elf";
		std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(patched.data()), static_cast<std::streamsize>(patched.size()));
		chmod(path.c_str(), 0755);
		const pid_t pid = fork();
		if (pid == 0)
		{
			unsetenv("LD_PRELOAD"); // a process of its own: not held back
			execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}
		std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
		return pid > 0;
	};
	TexturePackManager m(p, textures, textures + "/.ps5sx2-downloads", work + "/list.json");
	m.Start();
	while (m.Status(serial).state != State::Available)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const double t0 = Now();
	m.Begin(serial, 0, "", "", "bench");
	State s;
	while ((s = m.Status(serial).state) != State::Installed && s != State::Failed && s != State::NeedSpace)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const double secs = Now() - t0;
	std::printf("BENCH %s: %s in %.1f s (%.1f MiB pack)\n", mode.c_str(), s == State::Installed ? "installed" : "FAILED", secs, data.size() / 1048576.0);
	m.Stop(5000);
	return s == State::Installed ? 0 : 1;
}
