// SMB shares through libsmb2, read-only, files under /smb/<server>/<share>/<path>.
// Address: smb://[[domain;]user@]server[:port]/share[/path][?version=2|3]. No password in it: passwords are in a
// separate file (SetCredentialsFile), one per line: "smb://user@server[/share]<TAB>password".
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

struct dirent;
struct stat;

namespace OrbisSmb
{
struct Address
{
	std::string server; // lower case
	uint16_t port = 0; // 0: 445
	std::string share;
	std::string path = "/"; // folder inside the share
	std::string user; // empty: Guest
	std::string domain;
	int version = 0; // 0: any, 2: SMB 2.x, 3: SMB 3.x
};
bool ParseAddress(const std::string& url, Address& out, std::string& error);
std::string MountPointOf(const Address& a);

void SetCredentialsFile(const std::string& path);
bool StoreCredential(const std::string& file, const std::string& url, const std::string& password, std::string& error);
bool HasCredential(const std::string& url); // in the file set by SetCredentialsFile

struct ShareInfo
{
	std::string url;
	std::string mount_point;
	std::string state;
	bool mounted = false;
};

std::vector<std::string> SetShares(const std::string& list, std::vector<std::string>* problems = nullptr);
std::vector<ShareInfo> MountAll(int timeout_ms);
std::vector<ShareInfo> Shares();

bool IsPath(const char* path);

int Stat(const char* path, struct stat* st, bool follow);

void* OpenDir(const char* path);
bool ReadDir(void* dir, struct dirent* out);
void CloseDir(void* dir);

int Open(const char* path);
ssize_t PRead(int handle, void* dst, size_t n, uint64_t off);
int FStat(int handle, struct stat* st);
int Close(int handle);

struct Stats
{
	uint64_t opens = 0, reads = 0, requests = 0, bytes = 0, cache_hits = 0, lookups = 0, reconnects = 0;
};
Stats GetStats();
} // namespace OrbisSmb
