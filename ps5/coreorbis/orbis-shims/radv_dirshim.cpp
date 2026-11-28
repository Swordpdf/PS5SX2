// PS5 port, RADV link only (link-radv.sh, AI-assisted): directory calls in front of libps5platform.a's
// ps5_opendir/ps5_readdir/ps5_closedir/ps5_dirfd/ps5_rewinddir so /nfs/ paths are served by OrbisNfs.cpp.
//
// link-radv.sh leaves out dirshim.cpp so RADV can use the platform layer's DIR layout (dirfd/rewinddir).
// For any path outside /nfs, opendir returns the platform's own DIR* untouched and every directory call
// forwards directly to ps5_*. For /nfs paths, opendir returns a tracked handle backed by OrbisNfs.
// Hidden visibility matches radv-bindings.S: the eboot exports nothing.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisNfs.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <mutex>
#include <vector>

extern "C" {
DIR* ps5_opendir(const char* path);
struct dirent* ps5_readdir(DIR* dirp);
int ps5_closedir(DIR* dirp);
int ps5_dirfd(DIR* dirp);
void ps5_rewinddir(DIR* dirp);
}

namespace
{
struct FakeDir
{
	void* nfs;
	struct dirent entry;
};

std::mutex s_fake_dirs_mutex;
std::vector<FakeDir*> s_fake_dirs;

FakeDir* AsFake(DIR* d)
{
	std::lock_guard<std::mutex> lock(s_fake_dirs_mutex);
	FakeDir* f = reinterpret_cast<FakeDir*>(d);
	return std::find(s_fake_dirs.begin(), s_fake_dirs.end(), f) != s_fake_dirs.end() ? f : nullptr;
}
} // namespace

extern "C" {
__attribute__((visibility("hidden"))) DIR* opendir(const char* path)
{
	if (!OrbisNfs::IsPath(path))
		return ps5_opendir(path);
	void* nfs = OrbisNfs::OpenDir(path);
	if (!nfs)
		return nullptr;
	FakeDir* f = new FakeDir{nfs, {}};
	std::lock_guard<std::mutex> lock(s_fake_dirs_mutex);
	s_fake_dirs.push_back(f);
	return reinterpret_cast<DIR*>(f);
}

__attribute__((visibility("hidden"))) struct dirent* readdir(DIR* dirp)
{
	FakeDir* f = AsFake(dirp);
	if (!f)
		return ps5_readdir(dirp);
	return OrbisNfs::ReadDir(f->nfs, &f->entry) ? &f->entry : nullptr;
}

__attribute__((visibility("hidden"))) int closedir(DIR* dirp)
{
	FakeDir* f = nullptr;
	{
		std::lock_guard<std::mutex> lock(s_fake_dirs_mutex);
		FakeDir* cand = reinterpret_cast<FakeDir*>(dirp);
		auto it = std::find(s_fake_dirs.begin(), s_fake_dirs.end(), cand);
		if (it != s_fake_dirs.end())
		{
			f = *it;
			s_fake_dirs.erase(it);
		}
	}
	if (!f)
		return ps5_closedir(dirp);
	OrbisNfs::CloseDir(f->nfs);
	delete f;
	return 0;
}

__attribute__((visibility("hidden"))) int dirfd(DIR* dirp)
{
	if (AsFake(dirp))
	{
		errno = ENOTSUP;
		return -1;
	}
	return ps5_dirfd(dirp);
}

__attribute__((visibility("hidden"))) void rewinddir(DIR* dirp)
{
	if (AsFake(dirp))
		return;
	ps5_rewinddir(dirp);
}
} // extern "C"
