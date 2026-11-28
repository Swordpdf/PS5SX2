// Host unit test for ps5/coreorbis/orbis-shims/radv_dirshim.cpp (AI-assisted).
// Verifies /nfs/ paths route to OrbisNfs while non-/nfs/ paths forward untouched to ps5_* platform calls.
//
// Compile & run on a POSIX host:
//   c++ -std=c++20 -Wall -Wextra -I ps5/coreorbis/include-orbis ps5/coreorbis/orbis-shims/radv_dirshim.cpp ps5/coreorbis/tests/nfs/test_radv_dirshim.cpp -o /tmp/test_radv_dirshim
//   /tmp/test_radv_dirshim

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
	bool closed = false;
};

int g_nfs_open_calls = 0;
int g_nfs_close_calls = 0;
bool g_nfs_fail_open = false;
MockNfsHandle* g_last_nfs = nullptr;

int g_ps5_open_calls = 0;
int g_ps5_read_calls = 0;
int g_ps5_close_calls = 0;
int g_ps5_dirfd_calls = 0;
int g_ps5_rewind_calls = 0;
int g_ps5_fake_storage = 42;
DIR* const g_ps5_sentinel = reinterpret_cast<DIR*>(&g_ps5_fake_storage);
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
	auto* h = new MockNfsHandle{{"Game1.chd", "Game2.iso"}, 0, false};
	g_last_nfs = h;
	return h;
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
	(void)path;
	g_ps5_open_calls++;
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
	// 1. /nfs/ path goes to OrbisNfs, never to ps5_*
	DIR* nd = opendir("/nfs/192.168.11.186/volume1/public/emulation/ps2/games");
	assert(nd != nullptr);
	assert(g_nfs_open_calls == 1);
	assert(g_ps5_open_calls == 0);

	errno = 0;
	assert(dirfd(nd) == -1 && errno == ENOTSUP);
	assert(g_ps5_dirfd_calls == 0);

	rewinddir(nd);
	assert(g_ps5_rewind_calls == 0);

	struct dirent* e1 = readdir(nd);
	assert(e1 != nullptr && std::strcmp(e1->d_name, "Game1.chd") == 0);
	struct dirent* e2 = readdir(nd);
	assert(e2 != nullptr && std::strcmp(e2->d_name, "Game2.iso") == 0);
	assert(readdir(nd) == nullptr);
	assert(closedir(nd) == 0);
	assert(g_nfs_close_calls == 1);
	assert(g_ps5_close_calls == 0);

	// 2. Non-/nfs path passes untouched DIR* to ps5_* (preserving RADV dirfd/rewinddir)
	DIR* ld = opendir("/data/PCSX2/radv-shader-cache");
	assert(ld == g_ps5_sentinel);
	assert(g_ps5_open_calls == 1);
	assert(dirfd(ld) == 77 && g_ps5_dirfd_calls == 1);
	rewinddir(ld);
	assert(g_ps5_rewind_calls == 1);
	struct dirent* le = readdir(ld);
	assert(le != nullptr && std::strcmp(le->d_name, "shader.bin") == 0);
	assert(readdir(ld) == nullptr);
	assert(closedir(ld) == 0 && g_ps5_close_calls == 1);

	// 3. Failed NFS opendir returns nullptr cleanly
	g_nfs_fail_open = true;
	errno = 0;
	assert(opendir("/nfs/192.168.11.186/missing") == nullptr);
	assert(errno == ENOENT);
	assert(g_ps5_open_calls == 1);

	std::printf("test_radv_dirshim: all assertions passed\n");
	return 0;
}
