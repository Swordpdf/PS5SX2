// PS5SX2 (vk-285-73): PS5 notifications from a worker thread. vk-285-74 removed vk-285-73's
// notification test (L3+R3 + D-pad Right/Left and its ten variants); the worker stays for
// RetroAchievements.
//
// Two ways to put a toast on the PS5's screen, both public (the ps5-payload-dev SDK declares them and
// ships a sample of each):
//   - the kernel toast, sceKernelSendNotificationRequest(0, request, 0xc30, 0): a 0xc30-byte request whose
//     text starts at byte 0x2d. It is what the port has used since the start ("PS5SX2: starting"). The
//     PS4 layout (PS4-Notify's documentation) also has a "use icon" byte at 0x2c and an icon URI at
//     0x42d; whether the PS5 honours them is not known yet.
//   - the rich toast, libSceNotification's sceNotificationSend(user, logged, json): a JSON payload that
//     the system UI's notification overlay draws (its klog says "[notification] Post7 ... useCaseId=
//     <id> buflen=<bytes>"). The layout here is the SDK's notify sample (LightningMods, GPL-3.0-or-later):
//     rawData.viewTemplateType "InteractiveToastTemplateB", useCaseId "IDC", viewData { icon {type "Url",
//     parameters.url} | {type "Predefined", parameters.icon}, message.body, subMessage.body }, plus
//     createdDateTime and localNotificationId.
//     2026-10-05 (AI-assisted), from test payloads on a PS5 Pro with someone watching the screen:
//       - only a toast sent as logged (kept in the notification list) shows; nine sent not logged, in the
//         "Downloads", "ServiceFeedback" and "Trophies" channels, never appeared;
//       - channelType "ServiceFeedback" and "Trophies" showed during a game ("Downloads" is filtered by the
//         PS5's settings and Do Not Disturb);
//       - the icon can be a PNG by its /data/... or /user/data/... path, or an https URL;
//       - rawData.soundEffect "psfx_trophy_toast" plays the trophy ding and "psfx_platinum_trophy_toast" the
//         platinum trophy sound (ShadowMountPlus's TOASTS.md describes the field; it uses "none").
// libSceNotification is loaded when the first rich toast is sent (sceKernelLoadStartModule), so a console
// without it only logs that and falls back to the kernel toast.
// 2026-10-05: on 11.40 (and 12.00) that load is refused, 0x80020063 (rtld: "syscall load_prx failed due to
// 0x00000063", as for libSceKeyboard and libSceMouse), and the kernel toast shows as a plain debug message. Then the
// toast goes through the relay (notify-relay/notify_relay.c): a small payload embedded here, with the toast's JSON
// written into it, sent to the console's ELF loader on 127.0.0.1:9021, which runs it as a system process where the
// library loads. The kernel toast stays the last resort (no loader listening). (AI-assisted)
//
// Callers only queue: the worker thread does the system calls, so the pad thread and the CPU thread
// never wait on the system UI. Every send goes to boot.log ("[notify] ...") with its result.

#include "ProsperoNotify.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);
int sceUserServiceGetForegroundUser(int32_t* user);
void orbis_event_log(const char* line) __attribute__((weak)); // frontend/fe_ps5.cpp: settings.log
}

#ifdef ORBIS_NOTIFY_RELAY_ELF
// The notification relay payload (notify-relay/notify_relay.c), built and named by Makefile.vk.
__asm__(".section .rodata.orbis_notify_relay_elf,\"a\",@progbits\n"
		".balign 16\n"
		".global orbis_notify_relay_elf\norbis_notify_relay_elf:\n"
		".incbin \"" ORBIS_NOTIFY_RELAY_ELF "\"\n"
		".global orbis_notify_relay_elf_end\norbis_notify_relay_elf_end:\n"
		".byte 0\n"
		".previous\n");
extern "C" const uint8_t orbis_notify_relay_elf[], orbis_notify_relay_elf_end[];
#endif

namespace
{
	// The kernel toast's request (0xc30 bytes; the PS4 names, as documented by PS4-Notify).
#pragma pack(push, 1)
	struct KernelToast
	{
		int32_t type; // 0: a plain message
		int32_t req_id;
		int32_t priority;
		int32_t msg_id;
		int32_t target_id;
		int32_t user_id;
		int32_t unk1;
		int32_t unk2;
		int32_t app_id;
		int32_t error_num;
		int32_t unk3;
		uint8_t use_icon_image_uri; // 0x2c
		char message[1024]; // 0x2d
		char icon_uri[1024]; // 0x42d
		char unk[1024];
		uint8_t pad[3];
	};
#pragma pack(pop)
	static_assert(sizeof(KernelToast) == 0xc30, "the kernel toast request is 0xc30 bytes");
	static_assert(offsetof(KernelToast, message) == 0x2d, "its text starts at 0x2d");
	static_assert(offsetof(KernelToast, icon_uri) == 0x42d, "its icon URI starts at 0x42d");

	constexpr int32_t USER_SYSTEM = 0xfe; // the SDK sample's SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM

	enum class Api : uint8_t
	{
		Kernel,
		Rich,
	};

	struct Request
	{
		Api api = Api::Kernel;
		std::string message; // the kernel toast's whole text, or the rich toast's first line
		std::string sub_message;
		std::string icon; // a path or URL; for a predefined icon, its name
		bool icon_predefined = false;
		std::string sound; // rawData.soundEffect: a system sound id, "none", or empty for the system's default
		std::string channel; // rawData.channelType; empty: ORBIS_TOAST_CHANNEL_SERVICE
		bool preview_view = false; // add the sample's platformViews.previewDisabled view
		bool logged = false; // sceNotificationSend's second argument (keep it in the notification list)
		bool foreground_user = false; // send to the foreground user instead of the system's id
	};

	constexpr size_t MAX_QUEUED = 24; // 2026-10-05: more, now that rich toasts are spaced (a burst of unlocks waits its turn)
	// The queue lives on the heap and is never freed: the worker waits on it for the whole process, so
	// its lock and condition variable must not be destroyed at exit while the worker still waits.
	struct Shared
	{
		std::mutex lock;
		std::condition_variable wake;
		std::deque<Request> queue; // under lock
		bool thread_started = false; // under lock
		// 2026-10-05 (AI-assisted): the pacing of rich toasts (Worker). swordpdf: the first two toasts of a game (the
		// RetroAchievements login, then its game summary 3.1 s later, a second into the game) showed no picture, while the
		// same JSON with the same cached PNG showed it later from a test payload, in both channels, 7 s apart, as unlock
		// toasts mid-game do. So a rich toast waits until the hold set at the game's start has passed (OrbisNotifyHold)
		// and until kRichGap after the one before.
		std::chrono::steady_clock::time_point hold_until{}; // under lock
		std::chrono::steady_clock::time_point last_rich{}; // the worker's own
	};
	constexpr std::chrono::milliseconds kRichGap{6500};
	Shared& S()
	{
		static Shared* const s = new Shared();
		return *s;
	}

	using NotificationSend = int (*)(int32_t user, bool logged, const char* payload);

	// A system library by name, from the directories the ps5-payload-dev SDK's loader searches
	// (the same list as ProsperoKbdMouse.cpp).
	int LoadModule(const char* name)
	{
		static const char* const dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
			"/system_ex/priv_ex/lib/"};
		for (const char* dir : dirs)
		{
			const std::string path = std::string(dir) + name;
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
				continue;
			int res = 0;
			const int handle = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &res);
			printf("[notify] %s: load %#x (start result %d)\n", path.c_str(), static_cast<unsigned>(handle), res);
			if (handle >= 0)
				return handle;
		}
		printf("[notify] %s: not found or not loadable\n", name);
		return -1;
	}

	NotificationSend RichSender()
	{
		static bool s_tried = false;
		static NotificationSend s_send = nullptr;
		if (!s_tried)
		{
			s_tried = true;
			const int module = LoadModule("libSceNotification.sprx");
			if (module >= 0)
			{
				void* address = nullptr;
				const int rc = sceKernelDlsym(module, "sceNotificationSend", &address);
				printf("[notify] sceNotificationSend: dlsym %#x at %p\n", static_cast<unsigned>(rc), address);
				if (rc == 0)
					s_send = reinterpret_cast<NotificationSend>(address);
			}
			fflush(stdout);
		}
		return s_send;
	}

	std::string Quote(const std::string& s)
	{
		std::string out = "\"";
		for (const unsigned char c : s)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += static_cast<char>(c);
			}
			else if (c == '\n')
				out += "\\n";
			else if (c < 0x20)
			{
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", c);
				out += esc;
			}
			else
				out += static_cast<char>(c); // UTF-8 passes through
		}
		return out + "\"";
	}

	std::string IconJson(const Request& r)
	{
		if (r.icon_predefined)
			return "{\"type\":\"Predefined\",\"parameters\":{\"icon\":" + Quote(r.icon) + "}}";
		return "{\"type\":\"Url\",\"parameters\":{\"url\":" + Quote(r.icon) + "}}";
	}

	std::string RichPayload(const Request& r)
	{
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		struct tm tm;
		gmtime_r(&ts.tv_sec, &tm);
		char created[48];
		snprintf(created, sizeof(created), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ts.tv_nsec / 1000000));
		static std::atomic<unsigned> s_count{0};
		char id[16];
		snprintf(id, sizeof(id), "%u",
			static_cast<unsigned>(100000000u + (static_cast<unsigned>(ts.tv_sec) % 800000u) * 1000u + s_count.fetch_add(1) % 1000u));

		std::string j = "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":" +
		                Quote(r.channel.empty() ? ORBIS_TOAST_CHANNEL_SERVICE : r.channel) + ",\"useCaseId\":\"IDC\",";
		if (!r.sound.empty())
			j += "\"soundEffect\":" + Quote(r.sound) + ",";
		j += "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,\"viewData\":{";
		if (!r.icon.empty())
			j += "\"icon\":" + IconJson(r) + ",";
		j += "\"message\":{\"body\":" + Quote(r.message) + "}";
		if (!r.sub_message.empty())
			j += ",\"subMessage\":{\"body\":" + Quote(r.sub_message) + "}";
		j += "}";
		if (r.preview_view)
			j += ",\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":"
			     "{\"icon\":\"download\"}},\"message\":{\"body\":" + Quote(r.message) + "}}}}";
		j += "},\"createdDateTime\":" + Quote(created) + ",\"localNotificationId\":" + Quote(id) + "}";
		return j;
	}

	int SendKernel(const Request& r)
	{
		KernelToast req;
		memset(&req, 0, sizeof(req));
		snprintf(req.message, sizeof(req.message), "%s", r.message.c_str());
		if (!r.icon.empty())
		{
			req.use_icon_image_uri = 1;
			snprintf(req.icon_uri, sizeof(req.icon_uri), "%s", r.icon.c_str());
		}
		return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
	}

#ifdef ORBIS_NOTIFY_RELAY_ELF
	// The relay payload (Makefile.vk builds it): see notify-relay/notify_relay.c and the comment at the top.
	constexpr char RELAY_MARKER[] = "PS5SX2-NOTIFY-RELAY-JSON-V1";
	constexpr size_t RELAY_ROOM = 16384; // g_payload's size in notify_relay.c
	constexpr uint16_t RELAY_PORT = 9021; // the ELF loader's port (elfldr, etaHEN's), as the bridge and the README use
	// The loader keeps the connection until the payload ends (it sleeps 3 s after posting); waiting for that keeps the
	// toasts in order, an unlock before the mastery it completes. The worker thread does the waiting.
	constexpr int RELAY_CONNECT_MS = 500, RELAY_DONE_MS = 5000;

	// Where the JSON goes in the embedded file, found once: the marker, then zeros to the end of the array.
	// Returns SIZE_MAX if the build's relay doesn't look right.
	size_t RelayOffset()
	{
		static const size_t s_offset = [] {
			const size_t size = static_cast<size_t>(orbis_notify_relay_elf_end - orbis_notify_relay_elf);
			const size_t mlen = sizeof(RELAY_MARKER) - 1;
			for (size_t i = 0; i + RELAY_ROOM <= size; i++)
			{
				if (memcmp(orbis_notify_relay_elf + i, RELAY_MARKER, mlen) != 0)
					continue;
				size_t j = i + mlen;
				while (j < i + RELAY_ROOM && orbis_notify_relay_elf[j] == 0)
					j++;
				if (j == i + RELAY_ROOM)
					return i; // the marker, then zeros to the end of the array
			}
			return SIZE_MAX;
		}();
		return s_offset;
	}

	// Sends the relay with this JSON to the ELF loader. True once the loader took the file and closed the
	// connection (the payload has run); the payload's own result isn't reported back. why: the reason on failure.
	bool SendViaLoader(const std::string& json, std::string& why)
	{
		const size_t offset = RelayOffset();
		if (offset == SIZE_MAX)
		{
			why = "the embedded relay has no room marker";
			return false;
		}
		if (json.size() >= RELAY_ROOM)
		{
			why = "payload too large for the relay (" + std::to_string(json.size()) + " bytes)";
			return false;
		}
		std::vector<uint8_t> elf(orbis_notify_relay_elf, orbis_notify_relay_elf_end);
		memcpy(elf.data() + offset, json.data(), json.size());
		elf[offset + json.size()] = 0;

		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
		{
			why = "socket: errno " + std::to_string(errno);
			return false;
		}
		struct sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(RELAY_PORT);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		const auto fail = [&](const char* step) {
			why = std::string(step) + ": errno " + std::to_string(errno);
			close(fd);
			return false;
		};
		// A connect that can't hang: non-blocking, then up to RELAY_CONNECT_MS for it to finish.
		const int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
			return fail("fcntl");
		if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS)
			return fail("connect"); // ECONNREFUSED: no loader on this console
		struct pollfd p = {fd, POLLOUT, 0};
		if (poll(&p, 1, RELAY_CONNECT_MS) != 1)
		{
			errno = ETIMEDOUT;
			return fail("connect");
		}
		int err = 0;
		socklen_t len = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
		{
			errno = err;
			return fail("connect");
		}
		fcntl(fd, F_SETFL, flags); // blocking from here; the send is a local copy of ~100 KB
		for (size_t sent = 0; sent < elf.size();)
		{
			const ssize_t n = send(fd, elf.data() + sent, elf.size() - sent, 0);
			if (n <= 0)
				return fail("send");
			sent += static_cast<size_t>(n);
		}
		shutdown(fd, SHUT_WR);
		// The loader closes the connection once the payload has ended (its output would come back on it).
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(RELAY_DONE_MS);
		for (;;)
		{
			const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
				deadline - std::chrono::steady_clock::now()).count());
			struct pollfd q = {fd, POLLIN, 0};
			if (left <= 0 || poll(&q, 1, left) != 1)
				break; // still running after RELAY_DONE_MS: go on, the toast is very likely out
			char buf[256];
			if (recv(fd, buf, sizeof(buf), 0) <= 0)
				break;
		}
		close(fd);
		return true;
	}
#endif

	// pr9n: one send at a time, the worker's or OrbisNotifyRichNow's (the relay goes through the ELF loader's one port).
	std::mutex& SendMutex()
	{
		static std::mutex* const m = new std::mutex();
		return *m;
	}

	void SendLocked(Request r);
	void Send(Request r)
	{
		std::lock_guard<std::mutex> lock(SendMutex());
		SendLocked(std::move(r));
	}

	void SendLocked(Request r)
	{
		const auto t0 = std::chrono::steady_clock::now();
		int rc = 0;
		const char* how = "kernel";
		std::string payload;
		int32_t user = USER_SYSTEM;
		std::string relay_why;
		if (r.api == Api::Rich)
		{
			const NotificationSend send = RichSender();
#ifdef ORBIS_NOTIFY_RELAY_ELF
			if (!send)
			{
				// This process can't load libSceNotification: the relay sends the same payload (to 0xFE, logged).
				payload = RichPayload(r);
				r.logged = true;
				if (SendViaLoader(payload, relay_why))
					how = "relay (ELF loader 127.0.0.1:9021)";
				else
					payload.clear();
			}
#endif
			if (send)
			{
				if (r.foreground_user)
				{
					int32_t fg = -1;
					const int urc = sceUserServiceGetForegroundUser(&fg);
					if (urc == 0 && fg >= 0)
						user = fg;
					else
						printf("[notify] foreground user: rc %#x, sending to the system id\n", static_cast<unsigned>(urc));
				}
				payload = RichPayload(r);
				rc = send(user, r.logged, payload.c_str());
				how = "rich";
			}
			else if (payload.empty())
			{
				// No rich toasts on this console: the same text as a kernel toast, so something shows.
				Request k;
				k.message = r.message + (r.sub_message.empty() ? "" : "\n" + r.sub_message);
				rc = SendKernel(k);
				how = "kernel (rich unavailable)";
				if (!relay_why.empty())
					printf("[notify] relay not sent: %s\n", relay_why.c_str());
			}
		}
		else
			rc = SendKernel(r);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		char line[512];
		snprintf(line, sizeof(line), "notification: %s, rc %#x, %.1f ms", how, static_cast<unsigned>(rc), ms);
		printf("[notify] %s\n", line);
		if (!r.icon.empty())
			printf("[notify]   icon %s%s\n", r.icon_predefined ? "predefined " : "", r.icon.c_str());
		if (!payload.empty())
			printf("[notify]   user %#x, logged %d, payload %zu bytes (klog: Post7 ... buflen): %s\n", static_cast<unsigned>(user),
				r.logged ? 1 : 0, payload.size(), payload.c_str());
		fflush(stdout);
	}

	void* Worker(void*)
	{
		for (;;)
		{
			Request r;
			std::chrono::steady_clock::time_point hold{};
			{
				Shared& s = S();
				std::unique_lock<std::mutex> lock(s.lock);
				s.wake.wait(lock, [&s] { return !s.queue.empty(); });
				r = std::move(s.queue.front());
				s.queue.pop_front();
				hold = s.hold_until;
			}
			if (r.api == Api::Rich)
			{
				// The pacing (Shared): the later of the game's hold and the gap after the last rich toast.
				Shared& s = S();
				auto when = hold;
				if (s.last_rich.time_since_epoch().count() != 0 && s.last_rich + kRichGap > when)
					when = s.last_rich + kRichGap;
				const auto now = std::chrono::steady_clock::now();
				if (when > now)
				{
					printf("[notify] waiting %.1f s before the next toast\n", std::chrono::duration<double>(when - now).count());
					fflush(stdout);
					std::this_thread::sleep_until(when);
				}
				Send(std::move(r));
				s.last_rich = std::chrono::steady_clock::now();
			}
			else
				Send(std::move(r));
		}
		return nullptr;
	}

	void Queue(Request r)
	{
		Shared& s = S();
		std::lock_guard<std::mutex> lock(s.lock);
		if (!s.thread_started)
		{
			pthread_t t;
			if (pthread_create(&t, nullptr, Worker, nullptr) != 0)
			{
				printf("[notify] worker thread: pthread_create failed, notification dropped\n");
				return;
			}
			pthread_detach(t);
			s.thread_started = true;
		}
		if (s.queue.size() >= MAX_QUEUED)
		{
			printf("[notify] queue full, notification dropped\n");
			return;
		}
		s.queue.push_back(std::move(r));
		s.wake.notify_one();
	}
} // namespace

void OrbisNotifyHold(int seconds)
{
	Shared& s = S();
	std::lock_guard<std::mutex> lock(s.lock);
	s.hold_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

void OrbisNotifyPlain(const char* text)
{
	Request r;
	r.api = Api::Kernel;
	r.message = text ? text : "";
	Queue(std::move(r));
}

void OrbisNotifyRichNow(const char* message, const char* sub_message, const char* icon, const char* sound, const char* channel)
{
	Request r;
	r.api = Api::Rich;
	r.message = message ? message : "";
	r.sub_message = sub_message ? sub_message : "";
	r.icon = icon ? icon : "";
	r.sound = sound ? sound : "";
	r.channel = channel ? channel : "";
	r.logged = true;
	printf("[notify] sending now (the app is about to close)\n");
	Send(std::move(r)); // waits for a send of the worker's in progress, then this one; no pacing
}

void OrbisNotifyRich(const char* message, const char* sub_message, const char* icon, const char* sound, const char* channel)
{
	Request r;
	r.api = Api::Rich;
	r.message = message ? message : "";
	r.sub_message = sub_message ? sub_message : "";
	r.icon = icon ? icon : "";
	r.sound = sound ? sound : "";
	r.channel = channel ? channel : "";
	r.logged = true; // a toast sent not logged never showed on the Pro (2026-10-05)
	Queue(std::move(r));
}
