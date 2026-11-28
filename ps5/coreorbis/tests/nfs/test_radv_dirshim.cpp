// Host unit test for ps5/coreorbis/orbis-shims/radv_dirshim.cpp (AI-assisted).
// Run via: ps5/coreorbis/tests/nfs/test-radv-dirshim.sh

#include "OrbisNfs.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

namespace
{
struct MockNfsHandle
{
	std::vector<std::string> items;
	size_t idx = 0;
};

int g_nfs_open_calls = 0;
int g_nfs_close_calls = 0;
bool g_nfs_fail_open = false;

int g_ps5_open_calls = 0;
int g_ps5_read_calls = 0;
int g_ps5_close_calls = 0;
int g_ps5_dirfd_calls = 0;
int g_ps5_rewind_calls = 0;
std::string g_last_ps5_path;
DIR* g_ps5_sentinel = nullptr;
struct dirent g_ps5_entry = {};
} // namespace

namespace OrbisNfs
{
bool IsPath(const char* path)
{
	return path && std::strncmp(path, "/nfs", 4) == 0 && (path[4] == '\0' || path[4] == '/');
}

void* OpenDir(const char* path)
{
	g_nfs_open_calls++;
	if (g_nfs_fail_open || !path)
	{
		errno = ENOENT;
		return nullptr;
	}
	if (std::strstr(path, "/sub"))
		return new MockNfsHandle{{"SubGame.chd"}, 0};
	return new MockNfsHandle{{"Game1.chd", "Game2.iso"}, 0};
}

bool ReadDir(void* dir, struct dirent* out)
{
	auto* h = static_cast<MockNfsHandle*>(dir);
	if (!h || h->idx >= h->items.size())
		return false;
	std::memset(out, 0, sizeof(*out));
	const std::string& name = h->items[h->idx++];
	std::strncpy(out->d_name, name.c_str(), sizeof(out->d_name) - 1);
	out->d_type = DT_REG;
	return true;
}

void CloseDir(void* dir)
{
	auto* h = static_cast<MockNfsHandle*>(dir);
	g_nfs_close_calls++;
	delete h;
}
} // namespace OrbisNfs

extern "C" {
DIR* ps5_opendir(const char* path)
{
	g_last_ps5_path = path ? path : "";
	g_ps5_open_calls++;
	g_ps5_read_calls = 0;
	// Allocate a distinct sentinel per open so glibc's __attribute__((malloc)) on opendir holds under -O1+.
	g_ps5_sentinel = reinterpret_cast<DIR*>(new int(42));
	return g_ps5_sentinel;
}

struct dirent* ps5_readdir(DIR* dirp)
{
	assert(dirp == g_ps5_sentinel);
	g_ps5_read_calls++;
	if (g_ps5_read_calls > 1)
		return nullptr;
	std::memset(&g_ps5_entry, 0, sizeof(g_ps5_entry));
	std::strcpy(g_ps5_entry.d_name, "shader.bin");
	return &g_ps5_entry;
}

int ps5_closedir(DIR* dirp)
{
	assert(dirp == g_ps5_sentinel);
	g_ps5_close_calls++;
	delete reinterpret_cast<int*>(dirp);
	g_ps5_sentinel = nullptr;
	return 0;
}

int ps5_dirfd(DIR* dirp)
{
	assert(dirp == g_ps5_sentinel);
	g_ps5_dirfd_calls++;
	return 77;
}

void ps5_rewinddir(DIR* dirp)
{
	assert(dirp == g_ps5_sentinel);
	g_ps5_rewind_calls++;
}
}

int main()
{
	// 1. Interleaved NFS + nested NFS + local RADV handles do not collide
	DIR* nd1 = opendir("/nfs/192.168.11.186/volume1/public/emulation/ps2/games");
	assert(nd1 != nullptr);
	assert(g_nfs_open_calls == 1 && g_ps5_open_calls == 0);

	DIR* nd2 = opendir("/nfs/192.168.11.186/volume1/public/emulation/ps2/games/sub");
	assert(nd2 != nullptr && nd2 != nd1);
	assert(g_nfs_open_calls == 2 && g_ps5_open_calls == 0);

	DIR* ld = opendir("/data/PCSX2/radv-shader-cache");
	assert(ld == g_ps5_sentinel);
	assert(g_ps5_open_calls == 1 && g_last_ps5_path == "/data/PCSX2/radv-shader-cache");

	// dirfd and rewinddir on NFS handle are safe and never touch ps5_*
	errno = 0;
	assert(dirfd(nd1) == -1 && errno == ENOTSUP);
	assert(g_ps5_dirfd_calls == 0);
	rewinddir(nd1);
	assert(g_ps5_rewind_calls == 0);

	// dirfd and rewinddir on local handle pass untouched DIR* to ps5_* (RADV requirement)
	assert(dirfd(ld) == 77 && g_ps5_dirfd_calls == 1);
	rewinddir(ld);
	assert(g_ps5_rewind_calls == 1);

	// Interleaved reads keep independent dirent buffers per open DIR
	struct dirent* e1 = readdir(nd1);
	struct dirent* esub = readdir(nd2);
	struct dirent* le = readdir(ld);
	assert(e1 && std::strcmp(e1->d_name, "Game1.chd") == 0);
	assert(esub && std::strcmp(esub->d_name, "SubGame.chd") == 0);
	assert(le && std::strcmp(le->d_name, "shader.bin") == 0);
	struct dirent* e2 = readdir(nd1);
	assert(e2 && std::strcmp(e2->d_name, "Game2.iso") == 0);
	assert(readdir(nd1) == nullptr);
	assert(readdir(nd2) == nullptr);
	assert(readdir(ld) == nullptr);

	assert(closedir(nd2) == 0);
	assert(closedir(nd1) == 0);
	assert(g_nfs_close_calls == 2 && g_ps5_close_calls == 0);
	assert(closedir(ld) == 0 && g_ps5_close_calls == 1);

	// 2. Non-/nfs prefix lookalike (/nfsfoo) goes to ps5_*, not OrbisNfs
	DIR* ld2 = opendir("/nfsfoo");
	assert(ld2 == g_ps5_sentinel && g_ps5_open_calls == 2 && g_nfs_open_calls == 2);
	assert(closedir(ld2) == 0 && g_ps5_close_calls == 2);

	// 3. Failed NFS opendir returns nullptr with errno preserved and no leak
	g_nfs_fail_open = true;
	errno = 0;
	assert(opendir("/nfs/192.168.11.186/missing") == nullptr);
	assert(errno == ENOENT);
	assert(g_ps5_open_calls == 2);

	std::printf("ALL PASSED\n");
	return 0;
}
