// PS5 port (vk-285-135, AI-assisted): games on NFS shares. Testers asked for "nfs mounting": playing games straight off
// a PC or NAS share, without copying them to a drive.
//
// A share is an nfs:// address in gs.ini's PS5SX2/NfsShares (the settings page, or the shelf's folder picker), several
// separated by ';' or new lines:
//   nfs://192.168.1.10/volume1/PS2                NFS v3 through the server's portmapper (Linux, Synology, QNAP, TrueNAS,
//                                                 WinNFSd, haneWIN), else NFS v4 on port 2049
//   nfs://192.168.1.10/srv/games?version=4        NFS v4 only
//   nfs://pc/PS2?nfsport=2049&mountport=1058      fixed ports (no portmapper); also uid=, gid= (who the server sees)
// The share's files appear under /nfs/<host>/<path> (/nfs/192.168.1.10/volume1/PS2/...), and main-boot.cpp adds that
// folder to the game folders, so the shelf lists its games as it lists a drive's. The folder may be inside what the
// server exports (it mounts the longest part of the path it allows).
//
// The console's kernel has no NFS client, so the app is its own (libnfs, ps5/third_party/libnfs): link-vk.sh passes the
// C library's file calls through this file (ld.lld --wrap: fopen, fread, fseeko, ..., open, read, pread, lseek, fstat,
// close, stat, lstat, access, realpath; dirshim.cpp's opendir/readdir/closedir ask it too). A /nfs/ path goes to the
// server; every other path, and every FILE or descriptor that isn't one of its own, goes to the real call unchanged.
// Read-only: writing on a share fails with EROFS.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct dirent;

namespace OrbisNfs
{
// One nfs:// address, taken apart.
struct Address
{
	std::string host;       // "192.168.1.10" or a name
	std::string path = "/"; // "/volume1/PS2": what to mount (or a folder inside what the server exports)
	uint16_t nfsport = 0;   // 0: the portmapper's (v3) or 2049 (v4)
	uint16_t mountport = 0; // 0: the portmapper's
	int version = 0;        // 0: v3, else v4; 3 or 4: only that one
	int uid = -1, gid = -1; // -1: libnfs's (the app's own ids)
};
bool ParseAddress(const std::string& url, Address& out, std::string& error);
// Where an address's files appear: "/nfs/<host><path>" ("/nfs/192.168.1.10/volume1/PS2").
std::string MountPointOf(const Address& a);

struct ShareInfo
{
	std::string url;
	std::string mount_point;
	std::string state; // "not tried yet", "mounted /volume1 (NFS v3) in 0.04 s", "192.168.1.10 didn't answer: ..."
	bool mounted = false;
};

// The shares (replacing any set before), from PS5SX2/NfsShares. An address that doesn't parse is left out with a
// sentence in `problems`. Returns the mount points, in order, without repeats.
std::vector<std::string> SetShares(const std::string& list, std::vector<std::string>* problems = nullptr);
// Mounts every share not mounted yet, all at once, each giving up after about `timeout_ms`; the states after.
std::vector<ShareInfo> MountAll(int timeout_ms);
std::vector<ShareInfo> Shares();

// A path this file answers for: "/nfs" or under it, while shares are set.
bool IsPath(const char* path);

// dirshim.cpp's opendir/readdir/closedir for /nfs/ paths (null and errno when it can't be listed).
void* OpenDir(const char* path);
bool ReadDir(void* dir, struct dirent* out);
void CloseDir(void* dir);

// For the logs and the tests: what was read from the shares since the start.
struct Stats
{
	uint64_t opens = 0, reads = 0, rpcs = 0, bytes = 0, cache_hits = 0, lookups = 0;
};
Stats GetStats();
} // namespace OrbisNfs
