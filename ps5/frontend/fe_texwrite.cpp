// PS5 port frontend: the HD texture packs' files written by another process (see fe_texwrite.h). 2026-10-29, AI-assisted; needs testing on
// the console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_texwrite.h"
#include "fe_texwrite_proto.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{
namespace
{
static_assert(sizeof(tw_hello) == 32 && sizeof(tw_request) == 24 && sizeof(tw_reply) == 24, "the protocol's headers");

constexpr int kReplyMs = 120 * 1000; // an 8 MiB write at the slowest the console writes (2 MiB/s) is 4 s: this is a payload that has stopped

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Exactly n bytes within `ms` in all; false on a close, an error or the time running out.
bool RecvAll(int fd, void* buf, size_t n, int ms)
{
	uint8_t* p = static_cast<uint8_t*>(buf);
	const int64_t until = NowMs() + ms;
	while (n > 0)
	{
		const int64_t left = until - NowMs();
		if (left <= 0)
			return false;
		struct pollfd q = {fd, POLLIN, 0};
		const int r = poll(&q, 1, static_cast<int>(left));
		if (r < 0 && errno == EINTR)
			continue;
		if (r != 1)
			return false;
		const ssize_t got = recv(fd, p, n, 0);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return false;
		p += got;
		n -= static_cast<size_t>(got);
	}
	return true;
}

bool SendAll(int fd, const void* buf, size_t n)
{
	const uint8_t* p = static_cast<const uint8_t*>(buf);
	while (n > 0)
	{
#ifdef MSG_NOSIGNAL
		const ssize_t put = send(fd, p, n, MSG_NOSIGNAL);
#else
		const ssize_t put = send(fd, p, n, 0);
#endif
		if (put < 0 && errno == EINTR)
			continue;
		if (put <= 0)
			return false;
		p += put;
		n -= static_cast<size_t>(put);
	}
	return true;
}

std::string Hex(const uint8_t* p, size_t n)
{
	static const char kDigits[] = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < n; i++)
	{
		s += kDigits[p[i] >> 4];
		s += kDigits[p[i] & 15];
	}
	return s;
}

std::string ReadSmall(const std::string& path)
{
	std::string out;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return out;
	char buf[256];
	const size_t n = std::fread(buf, 1, sizeof(buf), f);
	out.assign(buf, n);
	std::fclose(f);
	return out;
}
} // namespace

std::string TexWriterRequest(int port, const std::string& token_hex, const std::string& root, const std::string& log_file)
{
	return std::string(TW_REQUEST_MARKER) + "\nport=" + std::to_string(port) + "\ntoken=" + token_hex + "\nroot=" + root + "\nlog=" + log_file + "\n";
}

std::vector<uint8_t> TexWriterPatched(const uint8_t* elf, size_t size, const std::string& request)
{
	const size_t mlen = sizeof(TW_REQUEST_MARKER) - 1;
	if (request.size() + 1 > TW_REQUEST_ROOM || request.find('\0') != std::string::npos)
		return {};
	for (size_t i = 0; i + TW_REQUEST_ROOM <= size; i++)
	{
		if (std::memcmp(elf + i, TW_REQUEST_MARKER, mlen) != 0)
			continue;
		size_t j = i + mlen;
		while (j < i + TW_REQUEST_ROOM && elf[j] == 0)
			j++;
		if (j != i + TW_REQUEST_ROOM)
			continue;
		std::vector<uint8_t> out(elf, elf + size);
		std::memcpy(out.data() + i, request.data(), request.size());
		out[i + request.size()] = 0;
		return out;
	}
	return {};
}

// ---- TexRemoteWriter

std::unique_ptr<TexRemoteWriter> TexRemoteWriter::Start(const std::string& root, const std::string& log_file, const Launch& launch, LogFn log,
	int wait_ms, std::string& why)
{
	if (root.empty() || root[0] != '/' || root.back() == '/' || !launch)
	{
		why = "no root folder or no way to start the writer";
		return nullptr;
	}
	const int ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0)
	{
		why = "socket: errno " + std::to_string(errno);
		return nullptr;
	}
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t alen = sizeof(addr);
	if (bind(ls, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 2) != 0 ||
		getsockname(ls, reinterpret_cast<struct sockaddr*>(&addr), &alen) != 0)
	{
		why = "can't listen on the loopback: errno " + std::to_string(errno);
		close(ls);
		return nullptr;
	}
	const int port = ntohs(addr.sin_port);
	uint8_t token[16];
	{
		std::random_device rd;
		for (uint8_t& b : token)
			b = static_cast<uint8_t>(rd());
		// the clock as well, so a random_device that is a fixed sequence still gives each start its own token
		const uint64_t t = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		for (int i = 0; i < 8; i++)
			token[i] ^= static_cast<uint8_t>(t >> (8 * i));
	}
	if (!launch(TexWriterRequest(port, Hex(token, sizeof(token)), root, log_file), why))
	{
		close(ls);
		return nullptr;
	}
	// The payload's connection (another, with a wrong hello, is closed and waited past).
	int fd = -1;
	tw_hello hello = {};
	const int64_t until = NowMs() + wait_ms;
	while (fd < 0)
	{
		const int64_t left = until - NowMs();
		struct pollfd q = {ls, POLLIN, 0};
		if (left <= 0 || poll(&q, 1, static_cast<int>(left)) != 1)
			break;
		const int c = accept(ls, nullptr, nullptr);
		if (c < 0)
			continue;
		if (RecvAll(c, &hello, sizeof(hello), 3000) && hello.magic == TW_HELLO_MAGIC && hello.version == TW_VERSION &&
			std::memcmp(hello.token, token, sizeof(token)) == 0 && hello.max_data >= TW_MAX_DATA)
			fd = c;
		else
			close(c);
	}
	close(ls);
	if (fd < 0)
	{
		why = "the writer did not connect within " + std::to_string(wait_ms / 1000.0).substr(0, 4) + " s";
		return nullptr;
	}
	const int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	const int sockbuf = 1 << 20;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
	std::unique_ptr<TexRemoteWriter> w(new TexRemoteWriter());
	w->m_fd = fd;
	w->m_root = root;
	w->m_pid = hello.pid;
	w->m_log = std::move(log);
	// Both must see the same folder (a title's /data may be a view of its own): the payload makes a file there that this process reads back.
	const std::string mark = ".ps5sx2-texwrite-" + Hex(token, 4), content = Hex(token + 4, 12);
	std::string body = mark;
	body += '\0';
	body += content;
	uint64_t value = 0;
	int err = 0;
	if (!w->Call(TW_MARK, 0, 0, 0, body.data(), body.size(), value, err))
	{
		why = err ? "the writer can't make a file in " + root + " (errno " + std::to_string(err) + ")" : std::string("the writer stopped answering");
		unlink((root + "/" + mark).c_str());
		return nullptr;
	}
	const std::string seen = ReadSmall(root + "/" + mark);
	unlink((root + "/" + mark).c_str());
	if (seen != content)
	{
		why = "the writer sees another " + root + " than the app";
		return nullptr;
	}
	return w;
}

TexRemoteWriter::~TexRemoteWriter()
{
	if (m_fd >= 0)
	{
		if (!m_broken)
		{
			uint64_t value = 0;
			int err = 0;
			Call(TW_BYE, 0, 0, 0, nullptr, 0, value, err);
		}
		close(m_fd);
	}
}

bool TexRemoteWriter::Under(const std::string& path, std::string& rel) const
{
	if (path.size() <= m_root.size() + 1 || path.compare(0, m_root.size(), m_root) != 0 || path[m_root.size()] != '/')
		return false;
	rel = path.substr(m_root.size() + 1);
	return rel.size() <= TW_MAX_PATH;
}

bool TexRemoteWriter::Call(uint16_t op, uint16_t flags, uint32_t handle, uint64_t offset, const void* data, size_t size, uint64_t& value, int& err)
{
	err = 0;
	value = 0;
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_broken)
		return false;
	tw_request q = {};
	q.magic = TW_REQ_MAGIC;
	q.op = op;
	q.flags = flags;
	q.handle = handle;
	q.len = static_cast<uint32_t>(size);
	q.offset = offset;
	tw_reply a = {};
	const bool ok = SendAll(m_fd, &q, sizeof(q)) && (size == 0 || SendAll(m_fd, data, size)) && RecvAll(m_fd, &a, sizeof(a), kReplyMs) &&
		a.magic == TW_REP_MAGIC && a.len == 0;
	if (!ok)
	{
		m_broken = true;
		shutdown(m_fd, SHUT_RDWR);
		if (m_log)
			m_log("[texpacks] the writer process stopped answering: the app writes the rest itself");
		return false;
	}
	value = a.value;
	err = a.err;
	if (op == TW_OPEN)
		value = a.handle;
	return err == 0;
}

bool TexRemoteWriter::Open(const std::string& path, bool truncate, uint32_t& handle, int& err)
{
	handle = 0;
	std::string rel;
	if (!Under(path, rel))
	{
		err = EPERM;
		return false;
	}
	uint64_t value = 0;
	if (!Call(TW_OPEN, static_cast<uint16_t>(TW_CREATE | (truncate ? TW_TRUNC : 0)), 0, 0, rel.data(), rel.size(), value, err))
		return false;
	handle = static_cast<uint32_t>(value);
	return handle != 0;
}

bool TexRemoteWriter::Write(uint32_t handle, uint64_t offset, const void* data, size_t size, int& err)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	while (size > 0)
	{
		const size_t take = size < TW_MAX_DATA ? size : TW_MAX_DATA;
		uint64_t value = 0;
		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = Call(TW_WRITE, 0, handle, offset, p, take, value, err);
		m_write_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
		if (!ok)
			return false;
		if (value != take)
		{
			err = EIO;
			return false;
		}
		m_bytes += take;
		p += take;
		offset += take;
		size -= take;
	}
	return true;
}

bool TexRemoteWriter::Fsync(uint32_t handle, int& err)
{
	uint64_t value = 0;
	return Call(TW_FSYNC, 0, handle, 0, nullptr, 0, value, err);
}

bool TexRemoteWriter::Close(uint32_t handle, int& err)
{
	uint64_t value = 0;
	return Call(TW_CLOSE, 0, handle, 0, nullptr, 0, value, err);
}

void TexRemoteWriter::Abort()
{
	if (!m_broken.exchange(true) && m_fd >= 0)
		shutdown(m_fd, SHUT_RDWR); // a Call waiting for its reply returns at once; the descriptor is closed by the destructor
}

// ---- TexOutFile

TexOutFile::~TexOutFile()
{
	if (IsOpen())
		Close();
}

bool TexOutFile::Fail(int err)
{
	m_errno = err ? err : EIO;
	return false;
}

bool TexOutFile::OpenLocal(bool truncate)
{
	const int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0);
#ifdef O_DIRECT
	if (m_direct)
		m_fd = open(m_path.c_str(), flags | O_DIRECT, 0666);
	if (m_fd < 0)
#endif
		m_fd = open(m_path.c_str(), flags, 0666);
	return m_fd >= 0 || Fail(errno);
}

bool TexOutFile::Open(const std::string& path, bool truncate, TexRemoteWriter* remote, bool direct, const TexRemoteWriter::LogFn& log)
{
	m_path = path;
	m_direct = direct;
	m_log = log;
	m_errno = 0;
	m_remote = remote;
	std::string rel;
	if (remote && remote->Usable() && remote->Under(path, rel))
	{
		int err = 0;
		if (remote->Open(path, truncate, m_handle, err))
			return true;
		if (m_log)
			m_log("[texpacks] the writer process can't open " + path + (err ? " (errno " + std::to_string(err) + ")" : std::string()) + ": the app writes it");
	}
	m_handle = 0;
	return OpenLocal(truncate);
}

bool TexOutFile::PWrite(const void* data, size_t size, uint64_t offset)
{
	if (size == 0)
		return true;
	if (m_handle)
	{
		int err = 0;
		if (m_remote->Write(m_handle, offset, data, size, err))
		{
			m_remote_bytes += size;
			return true;
		}
		if (err != 0)
			return Fail(err); // the file system's answer (the disk full ...): the same for the title
		// The payload went away: the title opens the same file (as it is) and writes this piece and the rest itself.
		m_handle = 0;
		if (!OpenLocal(false))
			return false;
	}
	const uint8_t* p = static_cast<const uint8_t*>(data);
	size_t at = 0;
	while (at < size)
	{
		const ssize_t w = pwrite(m_fd, p + at, size - at, static_cast<off_t>(offset + at));
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return Fail(w < 0 ? errno : EIO);
		at += static_cast<size_t>(w);
	}
	m_local_bytes += size;
	return true;
}

bool TexOutFile::Sync()
{
	if (m_handle)
	{
		int err = 0;
		if (m_remote->Fsync(m_handle, err))
			return true;
		if (err != 0)
			return Fail(err);
		m_handle = 0;
		return OpenLocal(false) && Sync();
	}
#ifndef _WIN32
	if (m_fd >= 0 && fdatasync(m_fd) != 0 && errno != EINVAL)
		return Fail(errno);
#endif
	return true;
}

bool TexOutFile::Close()
{
	bool ok = true;
	if (m_handle)
	{
		int err = 0;
		// a connection gone at the close: everything was confirmed written before it, so only the file system's own error counts
		if (!m_remote->Close(m_handle, err) && err != 0)
			ok = Fail(err);
		m_handle = 0;
	}
	if (m_fd >= 0)
	{
		if (close(m_fd) != 0)
			ok = Fail(errno);
		m_fd = -1;
	}
	return ok;
}
} // namespace fe
