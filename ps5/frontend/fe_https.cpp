// PS5 port frontend: our own HTTPS GET client (see fe_https.h). 2026-10-05, AI-assisted; needs proper testing on the
// console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_https.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h" // only the MBEDTLS_ERR_NET_* codes (the build has no MBEDTLS_NET_C)
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace fe
{
	namespace
	{
		// Mozilla's roots for TLS servers (see the file's header).
		const char kBundledRoots[] =
#include "fe_https_roots.inc"
			;

		constexpr size_t kMaxHeader = 64 * 1024;
		constexpr int kMaxRedirects = 5;

		std::string Lower(std::string s)
		{
			for (char& c : s)
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			return s;
		}

		std::string Trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
				a++;
			while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
				b--;
			return s.substr(a, b - a);
		}

		// mbedTLS's multi-line texts on one line.
		std::string OneLine(const char* text)
		{
			std::string out;
			for (const char* p = text; *p; p++)
			{
				if (*p == '\n')
				{
					if (!out.empty() && out.back() != ' ')
						out += "; ";
				}
				else
					out += *p;
			}
			while (!out.empty() && (out.back() == ' ' || out.back() == ';'))
				out.pop_back();
			return out;
		}

		std::string MbedError(int ret)
		{
			char text[160];
			mbedtls_strerror(ret, text, sizeof(text));
			char code[24];
			std::snprintf(code, sizeof(code), " (-0x%04x)", static_cast<unsigned>(-ret));
			return std::string(text) + code;
		}

		int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		bool AllDigits(const std::string& s)
		{
			return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
		}

		bool ParseU64(const std::string& s, uint64_t& out)
		{
			if (!AllDigits(s) || s.size() > 19)
				return false;
			out = std::strtoull(s.c_str(), nullptr, 10);
			return true;
		}

		// "<scheme>://": where an absolute address's scheme ends (before any '/', '?' or '#'), or npos.
		size_t SchemeEnd(const std::string& s)
		{
			const size_t p = s.find("://");
			if (p == std::string::npos || p == 0)
				return std::string::npos;
			for (size_t i = 0; i < p; i++)
			{
				const char c = s[i];
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
					return std::string::npos;
			}
			return p;
		}
	} // namespace

	bool ParseHttpsUrl(const std::string& url, HttpsUrl& out)
	{
		if (url.size() <= 8 || Lower(url.substr(0, 8)) != "https://")
			return false;
		const size_t end = url.find_first_of("/?#", 8);
		const std::string authority = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
		if (authority.empty() || authority.find('@') != std::string::npos || authority[0] == '[')
			return false; // no user info; no IPv6 literals (the console's sockets here are IPv4)
		std::string host = authority;
		uint16_t port = 443;
		const size_t colon = host.rfind(':');
		if (colon != std::string::npos)
		{
			const std::string digits = host.substr(colon + 1);
			host.resize(colon);
			uint64_t v = 0;
			if (!ParseU64(digits, v) || v == 0 || v > 65535)
				return false;
			port = static_cast<uint16_t>(v);
		}
		if (host.empty() || host.size() > 253)
			return false;
		for (const char c : host)
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
				return false;
		std::string path = end == std::string::npos ? std::string("/") : url.substr(end);
		const size_t hash = path.find('#');
		if (hash != std::string::npos)
			path.resize(hash);
		if (path.empty() || path[0] != '/')
			path = "/" + path;
		for (const char c : path)
		{
			const unsigned char u = static_cast<unsigned char>(c);
			if (u <= 0x20 || u == 0x7f)
				return false; // nothing that could break the request line
		}
		out.host = Lower(host);
		out.port = port;
		out.path = path;
		return true;
	}

	std::string ResolveHttpsLocation(const HttpsUrl& base, const std::string& location)
	{
		const std::string loc = Trim(location);
		if (loc.empty())
			return {};
		HttpsUrl check;
		if (SchemeEnd(loc) != std::string::npos)
			return ParseHttpsUrl(loc, check) ? loc : std::string(); // https:// only: never down to http://
		if (loc.size() >= 2 && loc[0] == '/' && loc[1] == '/')
		{
			const std::string url = "https:" + loc;
			return ParseHttpsUrl(url, check) ? url : std::string();
		}
		std::string url = "https://" + base.host + (base.port != 443 ? ":" + std::to_string(base.port) : std::string());
		if (loc[0] == '/')
			url += loc;
		else
		{
			std::string folder = base.path.substr(0, base.path.find('?'));
			folder.resize(folder.rfind('/') + 1);
			url += folder + loc;
		}
		return ParseHttpsUrl(url, check) ? url : std::string();
	}

	struct HttpsClient::Impl
	{
		HttpsPlatform platform;
		std::mutex request_mutex; // one request at a time, and Init
		bool tried = false, ready = false;
		std::string init_error;
		size_t root_count = 0;
		mbedtls_x509_crt roots;
		mbedtls_entropy_context entropy;
		mbedtls_ctr_drbg_context drbg;
		mbedtls_ssl_config conf;

		std::mutex fd_mutex; // the socket in flight, for Abort
		int fd = -1;
		std::atomic<bool> aborted{false};
		bool timed_out = false; // the last socket error was a timeout
		int64_t deadline = 0; // the request's total limit (NowMs), or 0
		int io_ms = 30000; // the limit for each wait for data
		std::set<std::string> hosts_seen; // the TLS line once per server

		// The socket's reads, buffered: mbedTLS asks for a record's 5-byte header, then its body.
		std::vector<unsigned char> sock_buf = std::vector<unsigned char>(64 * 1024);
		size_t sock_pos = 0, sock_len = 0;

		Impl()
		{
			mbedtls_x509_crt_init(&roots);
			mbedtls_entropy_init(&entropy);
			mbedtls_ctr_drbg_init(&drbg);
			mbedtls_ssl_config_init(&conf);
		}
		~Impl()
		{
			mbedtls_ssl_config_free(&conf);
			mbedtls_ctr_drbg_free(&drbg);
			mbedtls_entropy_free(&entropy);
			mbedtls_x509_crt_free(&roots);
		}

		void Log(const std::string& line)
		{
			if (platform.log)
				platform.log(line);
		}

		static int Entropy(void* data, unsigned char* out, size_t len, size_t* olen)
		{
			Impl* self = static_cast<Impl*>(data);
			if (!self->platform.random || !self->platform.random(out, len))
				return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
			*olen = len;
			return 0;
		}

		// Only logs: the result is mbedTLS's own (the flags are never changed here).
		static int VerifyLog(void* data, mbedtls_x509_crt* crt, int depth, uint32_t* flags)
		{
			if (*flags != 0)
			{
				char name[256] = {}, info[512] = {};
				mbedtls_x509_dn_gets(name, sizeof(name), &crt->subject);
				mbedtls_x509_crt_verify_info(info, sizeof(info), "", *flags);
				static_cast<Impl*>(data)->Log("[https] certificate " + std::to_string(depth) + " (" + name + "): " + OneLine(info));
			}
			return 0;
		}

		static int BioSend(void* data, const unsigned char* buf, size_t len)
		{
			Impl* self = static_cast<Impl*>(data);
#ifdef MSG_NOSIGNAL
			constexpr int flags = MSG_NOSIGNAL; // a closed connection is an error, never a SIGPIPE
#else
			constexpr int flags = 0;
#endif
			for (;;)
			{
				const ssize_t n = ::send(self->fd, buf, len, flags);
				if (n >= 0)
					return static_cast<int>(n);
				if (errno == EINTR)
					continue;
				self->timed_out = errno == EAGAIN || errno == EWOULDBLOCK;
				return MBEDTLS_ERR_NET_SEND_FAILED;
			}
		}

		static int BioRecv(void* data, unsigned char* buf, size_t len)
		{
			Impl* self = static_cast<Impl*>(data);
			if (self->sock_pos == self->sock_len)
			{
				for (;;)
				{
					// Wait for data at most the per-wait limit, and never past the request's total limit.
					int64_t wait = self->io_ms;
					if (self->deadline != 0)
						wait = std::max<int64_t>(0, std::min<int64_t>(wait, self->deadline - NowMs()));
					pollfd p{self->fd, POLLIN, 0};
					const int ready = ::poll(&p, 1, static_cast<int>(wait));
					if (ready == 0)
					{
						self->timed_out = true;
						return MBEDTLS_ERR_NET_RECV_FAILED;
					}
					if (ready < 0)
					{
						if (errno == EINTR)
							continue;
						return MBEDTLS_ERR_NET_RECV_FAILED;
					}
					const ssize_t n = ::recv(self->fd, self->sock_buf.data(), self->sock_buf.size(), 0);
					if (n > 0)
					{
						self->sock_pos = 0;
						self->sock_len = static_cast<size_t>(n);
						break;
					}
					if (n == 0)
						return 0; // the server closed the connection
					if (errno == EINTR)
						continue;
					self->timed_out = errno == EAGAIN || errno == EWOULDBLOCK;
					return MBEDTLS_ERR_NET_RECV_FAILED;
				}
			}
			const size_t take = std::min(len, self->sock_len - self->sock_pos);
			std::memcpy(buf, self->sock_buf.data() + self->sock_pos, take);
			self->sock_pos += take;
			return static_cast<int>(take);
		}

		// The decrypted answer, buffered, for the header lines, the chunk lines and the body.
		struct Reader
		{
			Impl* self;
			mbedtls_ssl_context* ssl;
			int64_t deadline;
			std::vector<unsigned char> buf = std::vector<unsigned char>(64 * 1024);
			size_t pos = 0, len = 0;
			int failure = 0; // the code when Fill fails

			size_t Avail() const { return len - pos; }
			const unsigned char* Data() const { return buf.data() + pos; }

			// More bytes: > 0 available, 0 at the end of the answer, < 0 (kRead/kAborted) on an error.
			int Fill()
			{
				if (pos < len)
					return static_cast<int>(len - pos);
				pos = len = 0;
				for (;;)
				{
					if (self->aborted)
						return failure = kAborted;
					if (deadline != 0 && NowMs() > deadline)
					{
						self->timed_out = true;
						return failure = kRead;
					}
					const int r = mbedtls_ssl_read(ssl, buf.data(), buf.size());
					if (r > 0)
					{
						len = static_cast<size_t>(r);
						return r;
					}
					if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
						return self->aborted ? (failure = kAborted) : 0; // Abort() shuts the socket: that end isn't the answer's
					if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
						continue;
					return failure = self->aborted ? kAborted : kRead;
				}
			}

			// One line without its CR LF; false at the end or on an error (`failure` set then, or 0 at a clean end).
			bool Line(std::string& line, size_t max = 4096)
			{
				line.clear();
				for (;;)
				{
					const int r = Fill();
					if (r <= 0)
						return false;
					const unsigned char* p = Data();
					const unsigned char* nl = static_cast<const unsigned char*>(std::memchr(p, '\n', Avail()));
					const size_t take = nl ? static_cast<size_t>(nl - p) + 1 : Avail();
					line.append(reinterpret_cast<const char*>(p), take);
					pos += take;
					if (nl)
					{
						line.pop_back();
						if (!line.empty() && line.back() == '\r')
							line.pop_back();
						return true;
					}
					if (line.size() > max)
					{
						failure = kRead;
						return false;
					}
				}
			}
		};

		// The body of a 2xx answer to the sink: `remaining` bytes, or up to the end when `to_end`. 1 when done (or the
		// sink stopped it), else kRead/kAborted (`error` set).
		static int Forward(Reader& rd, uint64_t remaining, bool to_end, const std::function<bool(const void*, size_t)>& sink,
			bool& stopped, uint64_t& received, std::string& error)
		{
			while (to_end || remaining > 0)
			{
				const int r = rd.Fill();
				if (r < 0)
				{
					error = rd.self->timed_out ? "the server stopped sending (timed out)" : "the connection broke";
					return r;
				}
				if (r == 0)
				{
					if (to_end)
						return 1;
					error = "the answer ended " + std::to_string(remaining) + " bytes early";
					return kRead;
				}
				const size_t take = to_end ? rd.Avail() : static_cast<size_t>(std::min<uint64_t>(rd.Avail(), remaining));
				const bool more = sink(rd.Data(), take);
				rd.pos += take;
				received += take;
				if (!to_end)
					remaining -= take;
				if (!more)
				{
					stopped = true;
					return 1;
				}
			}
			return 1;
		}

		// What one request asks: Get's ranged download, or Request's method and body (pr9n).
		struct Spec
		{
			std::string method = "GET";
			std::string body, content_type, user_agent;
			bool ranged = false;
			uint64_t offset = 0, length = 0;
			bool follow_redirects = true; // a 3xx with a Location comes back with `location` set
			bool any_status_body = false; // the body of every answer to the sink, not only a 2xx one's
			bool report_untrusted = false; // kUntrusted for a certificate the roots don't vouch for, else kConnect
		};

		// One request on one connection. A redirect's status comes back with `location` set (when the spec follows them).
		int Once(const HttpsUrl& u, const Spec& spec, const std::function<bool(const void*, size_t)>& sink, int64_t deadline,
			int connect_ms, int io_ms, std::string& location, std::string& error)
		{
			const bool ranged = spec.ranged;
			const uint64_t offset = spec.offset, length = spec.length;
			if (aborted)
				return kAborted;
			timed_out = false;
			this->deadline = deadline;
			this->io_ms = io_ms;
			const int sock = platform.connect ? platform.connect(u.host, u.port, connect_ms, io_ms, error) : -1;
			if (sock < 0)
			{
				if (aborted)
					return kAborted;
				if (error.empty())
					error = "no connection to " + u.host;
				Log("[https] " + error);
				return kConnect;
			}
			{
				std::lock_guard<std::mutex> lock(fd_mutex);
				fd = sock;
			}
			sock_pos = sock_len = 0;
			mbedtls_ssl_context ssl;
			mbedtls_ssl_init(&ssl);
			struct Cleanup
			{
				Impl* self;
				mbedtls_ssl_context* ssl;
				int sock;
				~Cleanup()
				{
					mbedtls_ssl_free(ssl);
					{
						std::lock_guard<std::mutex> lock(self->fd_mutex);
						self->fd = -1;
					}
					::close(sock);
				}
			} cleanup{this, &ssl, sock};
			if (aborted)
				return kAborted;

			int ret = mbedtls_ssl_setup(&ssl, &conf);
			if (ret == 0)
				ret = mbedtls_ssl_set_hostname(&ssl, u.host.c_str()); // the name the certificate must carry, and SNI
			if (ret != 0)
			{
				error = "TLS setup failed: " + MbedError(ret);
				Log("[https] " + error);
				return kConnect;
			}
			mbedtls_ssl_set_bio(&ssl, this, BioSend, BioRecv, nullptr);
			while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
			{
				if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
					continue;
				if (aborted)
					return kAborted;
				if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
				{
					char info[512] = {};
					mbedtls_x509_crt_verify_info(info, sizeof(info), "", mbedtls_ssl_get_verify_result(&ssl));
					error = u.host + "'s certificate isn't trusted: " + OneLine(info);
					Log("[https] " + error);
					return spec.report_untrusted ? kUntrusted : kConnect;
				}
				else if (timed_out)
					error = "TLS with " + u.host + " timed out";
				else
					error = "TLS with " + u.host + " failed: " + MbedError(ret);
				Log("[https] " + error);
				return kConnect;
			}
			if (hosts_seen.insert(u.host).second)
				Log("[https] " + u.host + ": " + mbedtls_ssl_get_version(&ssl) + " " + mbedtls_ssl_get_ciphersuite(&ssl) + ", certificate checked");

			std::string request = spec.method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host +
								  (u.port != 443 ? ":" + std::to_string(u.port) : std::string()) + "\r\nUser-Agent: " +
								  (spec.user_agent.empty() ? std::string("PS5SX2/1.0") : spec.user_agent) +
								  "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
			if (ranged)
				request += "Range: bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1) + "\r\n";
			if (!spec.body.empty() || spec.method == "POST")
				request += "Content-Type: " + (spec.content_type.empty() ? std::string("application/octet-stream") : spec.content_type) +
						   "\r\nContent-Length: " + std::to_string(spec.body.size()) + "\r\n";
			request += "\r\n";
			request += spec.body;
			size_t sent = 0;
			while (sent < request.size())
			{
				ret = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(request.data()) + sent, request.size() - sent);
				if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
					continue;
				if (ret <= 0)
				{
					if (aborted)
						return kAborted;
					error = "the request to " + u.host + " didn't go out: " + MbedError(ret);
					Log("[https] " + error);
					return kRead;
				}
				sent += static_cast<size_t>(ret);
			}

			Reader rd{this, &ssl, deadline};
			int status = 0;
			std::string head;
			for (;;) // a 1xx answer is followed by the real one
			{
				head.clear();
				for (;;)
				{
					const int r = rd.Fill();
					if (r <= 0)
					{
						if (r == kAborted)
							return kAborted;
						error = r == 0 ? u.host + " closed the connection before answering" :
										 (timed_out ? u.host + " didn't answer (timed out)" : "the connection to " + u.host + " broke");
						Log("[https] " + error);
						return kRead;
					}
					const size_t before = head.size();
					head.append(reinterpret_cast<const char*>(rd.Data()), rd.Avail());
					const size_t end = head.find("\r\n\r\n", before >= 3 ? before - 3 : 0);
					if (end == std::string::npos)
					{
						rd.pos = rd.len;
						if (head.size() > kMaxHeader)
						{
							error = u.host + "'s answer has no end to its header";
							Log("[https] " + error);
							return kRead;
						}
						continue;
					}
					rd.pos += end + 4 - before;
					head.resize(end + 4);
					break;
				}
				// "HTTP/1.1 206 Partial Content"
				if (head.compare(0, 5, "HTTP/") != 0 || head.find(' ') == std::string::npos)
				{
					error = u.host + " didn't answer in HTTP";
					Log("[https] " + error);
					return kRead;
				}
				const size_t sp = head.find(' ');
				const std::string code = head.substr(sp + 1, 3);
				if (!AllDigits(code) || code.size() != 3)
				{
					error = u.host + " answered without a status";
					Log("[https] " + error);
					return kRead;
				}
				status = std::atoi(code.c_str());
				if (status >= 200 || status == 101)
					break;
			}

			// The header lines this needs.
			bool chunked = false, have_length = false, have_range = false;
			uint64_t content_length = 0, range_start = 0;
			size_t line_start = head.find("\r\n") + 2;
			while (line_start < head.size())
			{
				const size_t line_end = head.find("\r\n", line_start);
				if (line_end == std::string::npos || line_end == line_start)
					break;
				const std::string line = head.substr(line_start, line_end - line_start);
				line_start = line_end + 2;
				const size_t colon = line.find(':');
				if (colon == std::string::npos)
					continue;
				const std::string name = Lower(Trim(line.substr(0, colon)));
				const std::string value = Trim(line.substr(colon + 1));
				if (name == "transfer-encoding")
					chunked = Lower(value).find("chunked") != std::string::npos;
				else if (name == "content-length")
					have_length = ParseU64(value, content_length);
				else if (name == "location")
					location = value;
				else if (name == "content-range")
				{
					// "bytes 33554432-67108863/876873405"
					const std::string v = Lower(value);
					if (v.compare(0, 6, "bytes ") == 0)
					{
						const size_t dash = v.find('-', 6);
						if (dash != std::string::npos)
							have_range = ParseU64(v.substr(6, dash - 6), range_start);
					}
				}
			}

			if (spec.follow_redirects && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !location.empty())
				return status;
			location.clear();
			if (spec.any_status_body ? (status < 200 || status == 204 || status == 304) : (status < 200 || status >= 300 || status == 204))
				return status; // the caller only needs the status (the connection closes)
			if (ranged && offset > 0 && status == 200)
			{
				error = u.host + " ignored the range and sent the whole file";
				Log("[https] " + error);
				return kIgnoredRange; // never the file's start where a later part belongs
			}
			if (ranged && status == 206 && have_range && range_start != offset)
			{
				error = u.host + " sent bytes from " + std::to_string(range_start) + " instead of " + std::to_string(offset);
				Log("[https] " + error);
				return kRead;
			}

			bool stopped = false;
			uint64_t received = 0;
			int r = 1;
			if (chunked)
			{
				std::string line;
				for (;;)
				{
					if (!rd.Line(line))
					{
						r = rd.failure == kAborted ? kAborted : kRead;
						error = "the chunked answer broke off";
						break;
					}
					const std::string size_text = Trim(line.substr(0, line.find(';')));
					char* end = nullptr;
					const unsigned long long size = std::strtoull(size_text.c_str(), &end, 16);
					if (size_text.empty() || size_text.size() > 15 || (end && *end != '\0'))
					{
						r = kRead;
						error = "a chunk's size made no sense";
						break;
					}
					if (size == 0)
					{
						while (rd.Line(line) && !line.empty())
						{
						} // the trailer lines
						break;
					}
					r = Forward(rd, size, false, sink, stopped, received, error);
					if (r != 1 || stopped)
						break;
					if (!rd.Line(line) || !line.empty())
					{
						r = rd.failure == kAborted ? kAborted : kRead;
						error = "a chunk didn't end where it said";
						break;
					}
				}
			}
			else if (have_length)
				r = Forward(rd, content_length, false, sink, stopped, received, error);
			else
				r = Forward(rd, 0, true, sink, stopped, received, error);
			if (r < 0)
			{
				if (r != kAborted)
					Log("[https] " + u.host + ": " + error + " after " + std::to_string(received) + " bytes");
				return r;
			}
			return status;
		}
	};

	HttpsClient::HttpsClient(HttpsPlatform platform)
		: m(std::make_unique<Impl>())
	{
		m->platform = std::move(platform);
	}

	HttpsClient::~HttpsClient() = default;

	bool HttpsClient::Init(std::string& error)
	{
		std::lock_guard<std::mutex> lock(m->request_mutex);
		if (m->tried)
		{
			error = m->init_error;
			return m->ready;
		}
		m->tried = true;
		int ret = 0;
		if (!m_skip_bundled)
		{
			ret = mbedtls_x509_crt_parse(&m->roots, reinterpret_cast<const unsigned char*>(kBundledRoots), sizeof(kBundledRoots));
			if (ret < 0)
			{
				m->init_error = error = "the bundled root certificates don't read: " + MbedError(ret);
				m->Log("[https] " + error);
				return false;
			}
			if (ret > 0)
				m->Log("[https] " + std::to_string(ret) + " of the bundled root certificates didn't read");
		}
		for (const mbedtls_x509_crt* c = &m->roots; c != nullptr && c->raw.len > 0; c = c->next)
			m->root_count++;
		ret = mbedtls_entropy_add_source(&m->entropy, Impl::Entropy, m.get(), 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
		if (ret == 0)
		{
			static const char kPersonal[] = "PS5SX2 https";
			ret = mbedtls_ctr_drbg_seed(&m->drbg, mbedtls_entropy_func, &m->entropy, reinterpret_cast<const unsigned char*>(kPersonal),
				sizeof(kPersonal) - 1);
		}
		if (ret != 0)
		{
			m->init_error = error = "no random numbers from the system: " + MbedError(ret);
			m->Log("[https] " + error);
			return false;
		}
		ret = mbedtls_ssl_config_defaults(&m->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
		if (ret != 0)
		{
			m->init_error = error = "TLS setup failed: " + MbedError(ret);
			m->Log("[https] " + error);
			return false;
		}
		// AES-GCM first (the PS5's CPU has AES-NI and PCLMUL), ChaCha20-Poly1305 after: all forward-secret AEAD suites.
		static const int kSuites[] = {MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
			MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
			MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, 0};
		mbedtls_ssl_conf_ciphersuites(&m->conf, kSuites);
		mbedtls_ssl_conf_authmode(&m->conf, MBEDTLS_SSL_VERIFY_REQUIRED); // always: a server the roots don't vouch for is refused
		mbedtls_ssl_conf_ca_chain(&m->conf, &m->roots, nullptr);
		mbedtls_ssl_conf_rng(&m->conf, mbedtls_ctr_drbg_random, &m->drbg);
		mbedtls_ssl_conf_verify(&m->conf, Impl::VerifyLog, m.get());
		m->ready = true;
		m->Log("[https] ready: " + std::to_string(m->root_count) + " root certificates");
		return true;
	}

	bool HttpsClient::AddRoots(const char* pem, std::string& error)
	{
		std::lock_guard<std::mutex> lock(m->request_mutex);
		const int ret = mbedtls_x509_crt_parse(&m->roots, reinterpret_cast<const unsigned char*>(pem), std::strlen(pem) + 1);
		if (ret != 0)
		{
			error = ret < 0 ? MbedError(ret) : std::to_string(ret) + " certificates didn't read";
			return false;
		}
		return true;
	}

	int HttpsClient::Get(const std::string& url, bool ranged, uint64_t offset, uint64_t length,
		const std::function<bool(const void*, size_t)>& sink, int total_timeout_ms, std::string* error)
	{
		std::lock_guard<std::mutex> lock(m->request_mutex);
		m->aborted = false;
		std::string scratch;
		std::string& err = error ? *error : scratch;
		err.clear();
		if (!m->ready)
		{
			err = m->init_error.empty() ? "HTTPS isn't set up" : m->init_error;
			return kConnect;
		}
		if (ranged && length == 0)
		{
			err = "an empty range";
			return kRead;
		}
		const int64_t deadline = total_timeout_ms > 0 ? NowMs() + total_timeout_ms : 0;
		std::string current = url;
		for (int hop = 0; hop <= kMaxRedirects; hop++)
		{
			HttpsUrl u;
			if (!ParseHttpsUrl(current, u))
			{
				err = "not an https:// address: " + current;
				m->Log("[https] " + err);
				return kConnect;
			}
			std::string location;
			Impl::Spec spec;
			spec.user_agent = user_agent;
			spec.ranged = ranged;
			spec.offset = offset;
			spec.length = length;
			const int status = m->Once(u, spec, sink, deadline, connect_timeout_ms, io_timeout_ms, location, err);
			if (location.empty())
				return status;
			const std::string next = ResolveHttpsLocation(u, location);
			HttpsUrl n;
			if (next.empty() || !ParseHttpsUrl(next, n))
			{
				err = u.host + " sent the download to an address that isn't https://";
				m->Log("[https] " + err);
				return kConnect;
			}
			if (n.host != u.host)
				m->Log("[https] " + u.host + " sends it to " + n.host);
			current = next;
		}
		err = "too many redirects";
		m->Log("[https] " + err);
		return kRead;
	}

	int HttpsClient::Request(const std::string& method, const std::string& url, const std::string& body, const std::string& content_type,
		const std::function<bool(const void*, size_t)>& sink, int total_timeout_ms, std::string* error)
	{
		std::lock_guard<std::mutex> lock(m->request_mutex);
		m->aborted = false;
		std::string scratch;
		std::string& err = error ? *error : scratch;
		err.clear();
		if (!m->ready)
		{
			err = m->init_error.empty() ? "HTTPS isn't set up" : m->init_error;
			return kConnect;
		}
		HttpsUrl u;
		if (method.empty() || method.find_first_of(" \r\n") != std::string::npos || !ParseHttpsUrl(url, u))
		{
			err = "not an https:// address";
			return kUntrusted; // never sent, never worth retrying
		}
		Impl::Spec spec;
		spec.method = method;
		spec.body = body;
		spec.content_type = content_type;
		spec.user_agent = user_agent;
		spec.follow_redirects = false;
		spec.any_status_body = true;
		spec.report_untrusted = true;
		std::string location;
		return m->Once(u, spec, sink, total_timeout_ms > 0 ? NowMs() + total_timeout_ms : 0, connect_timeout_ms, io_timeout_ms, location, err);
	}

	void HttpsClient::Abort()
	{
		m->aborted = true;
		std::lock_guard<std::mutex> lock(m->fd_mutex);
		if (m->fd >= 0)
			::shutdown(m->fd, SHUT_RDWR);
	}
} // namespace fe
