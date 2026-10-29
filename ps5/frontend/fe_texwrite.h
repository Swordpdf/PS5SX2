// PS5 port frontend: the HD texture packs' files written by another process (2026-10-29, AI-assisted).
//
// Session reports of the texture pack unpacks showed write() holding at a flat 2 MB/s, and PS5SX3's measurements explain it: a title writes
// ~1.3 GiB at full speed, then ~2 MiB a second, and another process is not held back. A pack is written twice by the title (its download and its
// replacements.pak), so any pack past ~650 MB went at 2 MB/s for most of its bytes. TexRemoteWriter starts the payload of
// ps5/coreorbis/tex-writer/tex_writer.c (through the console's ELF loader, see ProsperoTexWriter.cpp) and hands it the bytes over the loopback;
// TexOutFile writes a file through it, or itself when there is none, and carries on itself when the payload goes away in the middle. The protocol
// is fe_texwrite_proto.h. Flag file texpack_titlewrites: the title writes everything itself, as before.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fe
{
// The text written over the payload's marker.
std::string TexWriterRequest(int port, const std::string& token_hex, const std::string& root, const std::string& log_file);
// The payload with `request` written over its marker (the marker, then zeros to the end of its TW_REQUEST_ROOM-byte array); empty when the file
// has no such place or the request does not fit.
std::vector<uint8_t> TexWriterPatched(const uint8_t* elf, size_t size, const std::string& request);

class TexRemoteWriter
{
public:
	// Starts the payload with the request text (the console: the ELF loader; the tests: a process of their own). False and `why` when it could not.
	using Launch = std::function<bool(const std::string& request, std::string& why)>;
	using LogFn = std::function<void(const std::string& line)>;

	// Listens on 127.0.0.1 (a port the system picks), launches the payload for `root` (an absolute folder, no '/' at its end) and waits up to
	// `wait_ms` for it to connect and say the token; then checks that it sees the same `root` (it writes a small file there that this process must
	// read back). Null and `why` when any of it fails: the caller writes itself.
	static std::unique_ptr<TexRemoteWriter> Start(const std::string& root, const std::string& log_file, const Launch& launch, LogFn log, int wait_ms,
		std::string& why);
	~TexRemoteWriter(); // says bye and closes
	TexRemoteWriter(const TexRemoteWriter&) = delete;
	TexRemoteWriter& operator=(const TexRemoteWriter&) = delete;

	const std::string& Root() const { return m_root; }
	// `path` (absolute) is under Root(): its part after "<root>/" in `rel`.
	bool Under(const std::string& path, std::string& rel) const;
	// False once the connection has failed (the payload ended, a reply did not come): no request is sent after that.
	bool Usable() const { return !m_broken.load(); }
	uint32_t Pid() const { return m_pid; }

	// Each: true when the payload did it. False with `err` = the payload's errno (the file system said no) or 0 when the connection failed
	// (Usable() is false then).
	bool Open(const std::string& path, bool truncate, uint32_t& handle, int& err);
	bool Write(uint32_t handle, uint64_t offset, const void* data, size_t size, int& err);
	bool Fsync(uint32_t handle, int& err);
	bool Close(uint32_t handle, int& err);
	// Fails the request in flight and every later one (another thread: the manager stopping).
	void Abort();

	uint64_t BytesWritten() const { return m_bytes.load(); }
	double WriteSeconds() const { return m_write_us.load() / 1e6; }

private:
	TexRemoteWriter() = default;
	bool Call(uint16_t op, uint16_t flags, uint32_t handle, uint64_t offset, const void* data, size_t size, uint64_t& value, int& err);

	int m_fd = -1;
	std::string m_root;
	uint32_t m_pid = 0;
	std::mutex m_mutex;
	std::atomic<bool> m_broken{false};
	std::atomic<uint64_t> m_bytes{0}, m_write_us{0};
	LogFn m_log;
};

// One file being written at offsets: through `remote` when it is given and usable (and the file is under its root), else with the title's own
// descriptor. A connection that fails in the middle hands the file to the title: it opens the same file (without truncating it) and writes the
// piece the payload did not confirm, and everything after it, itself. A refusal by the file system (ENOSPC ...) is an error either way.
class TexOutFile
{
public:
	TexOutFile() = default;
	~TexOutFile();
	TexOutFile(const TexOutFile&) = delete;
	TexOutFile& operator=(const TexOutFile&) = delete;

	// truncate: O_TRUNC (a new file); otherwise the file is kept as it is (a download that picks up where it stopped). direct: O_DIRECT for the
	// title's own descriptor (flag texpack_direct).
	bool Open(const std::string& path, bool truncate, TexRemoteWriter* remote, bool direct, const TexRemoteWriter::LogFn& log);
	bool PWrite(const void* data, size_t size, uint64_t offset);
	bool Sync();
	bool Close();
	bool IsOpen() const { return m_fd >= 0 || m_handle != 0; }
	bool Remote() const { return m_handle != 0; }
	int Errno() const { return m_errno; }
	uint64_t RemoteBytes() const { return m_remote_bytes; }
	uint64_t LocalBytes() const { return m_local_bytes; }

private:
	bool OpenLocal(bool truncate);
	bool Fail(int err);

	std::string m_path;
	TexRemoteWriter* m_remote = nullptr;
	TexRemoteWriter::LogFn m_log;
	uint32_t m_handle = 0;
	int m_fd = -1;
	bool m_direct = false;
	int m_errno = 0;
	uint64_t m_remote_bytes = 0, m_local_bytes = 0;
};
} // namespace fe
