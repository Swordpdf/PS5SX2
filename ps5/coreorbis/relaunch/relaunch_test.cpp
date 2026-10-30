// PS5SX2 (2.11, AI-assisted): host test of ProsperoRelaunch.cpp with a fake ELF loader (no console). The loader listens like the console's (on a port
// the test script picks), reads the whole payload the app sends, saves it, makes it executable and RUNS it, so the real request the app patched in goes
// through the real payload (relaunch.c built for the PC with stand-ins for the console's calls) from end to end: the handshake file, the wait for the
// "closing app" (a child process of this test), the settle and the launch the stub logs.
//
// Build + run: test-relaunch.sh
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../orbis-shims/ProsperoRelaunch.cpp" // the code under test, with ORBIS_RELAUNCH_ELF set to the host payload (or a file without the marker)

#include <atomic>
#include <csignal>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <utime.h>
#include <vector>

std::string g_dir;

std::string OrbisLogPath(const char* name)
{
	return g_dir + "/" + name;
}

namespace
{
int g_fail = 0;
#define CHECK(cond, ...)                                                                                                                   \
	do                                                                                                                                     \
	{                                                                                                                                      \
		if (!(cond))                                                                                                                       \
		{                                                                                                                                  \
			g_fail++;                                                                                                                      \
			std::printf("FAIL line %d: %s -- ", __LINE__, #cond);                                                                          \
			std::printf(__VA_ARGS__);                                                                                                      \
			std::printf("\n");                                                                                                             \
		}                                                                                                                                  \
	} while (0)

double Now()
{
	return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string ReadFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

bool WaitFile(const std::string& path, double seconds)
{
	const double end = Now() + seconds;
	while (Now() < end)
	{
		if (Exists(path))
			return true;
		usleep(20000);
	}
	return Exists(path);
}

std::vector<std::string> ListDir(const std::string& dir)
{
	std::vector<std::string> names;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				names.emplace_back(e->d_name);
		closedir(d);
	}
	return names;
}

struct Launch
{
	std::string title;
	int user = 0;
	double at = 0;
};

std::vector<Launch> Launches(const std::string& dir)
{
	std::vector<Launch> out;
	std::istringstream in(ReadFile(dir + "/stub.log"));
	std::string line;
	while (std::getline(in, line))
	{
		char title[64];
		int user = 0;
		unsigned size = 0;
		long long at = 0;
		if (std::sscanf(line.c_str(), "launch %63s user %d size %u at %lld", title, &user, &size, &at) == 4)
			out.push_back({title, user, static_cast<double>(at) / 1000.0});
	}
	return out;
}

// The first line of a .done file ("status launched").
std::string Status(const std::string& state)
{
	const std::string text = ReadFile(state + ".done");
	return text.compare(0, 7, "status ") == 0 ? text.substr(7, text.find('\n') - 7) : std::string();
}

// The request in a payload the loader saved: from the marker to its NUL.
std::string RequestIn(const std::string& payload)
{
	const std::string blob = ReadFile(payload);
	const size_t at = blob.find("PS5SX2-RELAUNCH-REQUEST-V1\npid=");
	if (at == std::string::npos)
		return {};
	return std::string(blob.c_str() + at);
}

std::string Field(const std::string& request, const char* key)
{
	const std::string needle = std::string("\n") + key + "=";
	const size_t at = request.find(needle);
	if (at == std::string::npos)
		return {};
	const size_t from = at + needle.size();
	return request.substr(from, request.find('\n', from) - from);
}

// A process that plays the app which is closing: lives `seconds`, then is gone (SIGCHLD is ignored, so it does not even stay a zombie).
pid_t SpawnApp(double seconds)
{
	const pid_t pid = fork();
	if (pid == 0)
	{
		usleep(static_cast<useconds_t>(seconds * 1e6));
		_exit(0);
	}
	return pid;
}

// ---------------------------------------------------------------------------------------------------------------------------- the fake loader
struct LoaderCfg
{
	int delay_ms = 0;      // listens only after this long (before that the connect is refused, as when the supervisor has just killed the loader)
	bool execute = true;   // runs the payload (false: takes it and does nothing)
	bool hold_open = false; // keeps the connection open for a second after starting it (else it closes at once, as the fork's does)
	bool no_read = false;   // takes the connection and never reads from it (a stuck loader), for 6 s
};

class Loader
{
public:
	void Start(const LoaderCfg& cfg)
	{
		m_cfg = cfg;
		m_stop = false;
		m_ready = false;
		connections = 0;
		payloads.clear();
		m_thread = std::thread([this] { Run(); });
		if (cfg.delay_ms == 0)
			while (!m_ready && !m_bind_failed)
				usleep(2000);
		if (m_bind_failed)
		{
			std::printf("can't listen on 127.0.0.1:%u (errno %d)\n", static_cast<unsigned>(kLoaderPort), errno);
			std::exit(2);
		}
	}
	void Stop()
	{
		m_stop = true;
		if (m_thread.joinable())
			m_thread.join();
	}
	std::atomic<int> connections{0};
	std::vector<std::string> payloads;

private:
	void Run()
	{
		for (int waited = 0; waited < m_cfg.delay_ms && !m_stop; waited += 10)
			usleep(10000);
		const int s = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons(kLoaderPort);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 4) != 0)
		{
			m_bind_failed = true;
			close(s);
			return;
		}
		m_ready = true;
		while (!m_stop)
		{
			pollfd p = {s, POLLIN, 0};
			if (poll(&p, 1, 50) != 1)
				continue;
			const int c = accept(s, nullptr, nullptr);
			if (c < 0)
				continue;
			connections++;
			if (m_cfg.no_read)
			{
				usleep(6000000);
				close(c);
				continue;
			}
			std::string all;
			char buf[65536];
			ssize_t n;
			while ((n = recv(c, buf, sizeof(buf), 0)) > 0)
				all.append(buf, static_cast<size_t>(n));
			const std::string path = g_dir + "/payload-" + std::to_string(connections.load()) + ".elf";
			std::ofstream(path, std::ios::binary).write(all.data(), static_cast<std::streamsize>(all.size()));
			chmod(path.c_str(), 0755);
			payloads.push_back(path);
			if (m_cfg.execute)
			{
				const pid_t pid = fork();
				if (pid == 0)
				{
					dup2(c, 1); // the loader's connection is the payload's stdout and stderr
					dup2(c, 2);
					for (int fd = 3; fd < 256; fd++)
						close(fd);
					execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
					_exit(127);
				}
			}
			if (m_cfg.hold_open)
				usleep(1000000);
			close(c);
		}
		close(s);
	}
	LoaderCfg m_cfg;
	std::atomic<bool> m_stop{false}, m_ready{false}, m_bind_failed{false};
	std::thread m_thread;
};

// A scenario's folder and the stub's settings (the payloads the loader starts inherit them).
void Begin(const char* name, const char* closing = "PPSA99230")
{
	g_dir = std::string(std::getenv("RELAUNCH_TEST_DIR")) + "/" + name;
	mkdir(g_dir.c_str(), 0755);
	setenv("RELAUNCH_STUB_LOG", (g_dir + "/stub.log").c_str(), 1);
	setenv("RELAUNCH_STUB_OLD", closing, 1);
	setenv("RELAUNCH_STUB_LAUNCH", "PPSA99203", 1);
	unsetenv("RELAUNCH_STUB_APPFILE"); // the shell does not know the closing title: the process decides
	unsetenv("RELAUNCH_STUB_BIGFILE");
	unsetenv("RELAUNCH_STUB_RC");
	unsetenv("RELAUNCH_STUB_UPAFTER");
	unsetenv("RELAUNCH_STUB_QUIET");
	unsetenv("RELAUNCH_STUB_NULL");
}

void RunPayloadAndWait(const std::string& path)
{
	const pid_t pid = fork();
	if (pid == 0)
	{
		execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
		_exit(127);
	}
	for (int i = 0; i < 300 && kill(pid, 0) == 0; i++)
		usleep(20000);
}
} // namespace

int main()
{
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	CHECK(OrbisRelaunchBuilt(), "built");
	Loader loader;
	std::string why;

#ifdef RELAUNCH_TEST_NOMARKER
	// The embedded file has no request marker: nothing is sent.
	Begin("nomarker");
	loader.Start({});
	CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203", 4242, why) && why.find("marker") != std::string::npos, "no marker: false (%s)", why.c_str());
	CHECK(loader.connections == 0, "and nothing sent");
	loader.Stop();
	std::printf(g_fail ? "relaunch test (no marker): %d FAILED\n" : "relaunch test (no marker): all passed\n", g_fail);
	return g_fail ? 1 : 0;
#elif defined(RELAUNCH_TEST_STALL)
	// A loader that takes the connection and never reads (the embedded file is padded to 24 MB so the send really fills the buffers): the call does not
	// hang the way out. It gives up inside the budget (about 4.5 s), says why, and calls the request off.
	Begin("stall");
	loader.Start(LoaderCfg{0, false, false, true});
	const double t0 = Now();
	CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203", 4242, why), "a stuck loader: false");
	const double took = Now() - t0;
	CHECK(took > 3.0 && took < 6.0, "it gives up inside the budget (%.2f s)", took);
	CHECK(why.find("send") != std::string::npos, "reason: %s", why.c_str());
	bool cancelled = false;
	for (const std::string& name : ListDir(g_dir))
		cancelled = cancelled || (name.size() > 7 && name.compare(name.size() - 7, 7, ".cancel") == 0);
	CHECK(cancelled, "the request is called off (a .cancel note stands)");
	loader.Stop();
	std::printf(g_fail ? "relaunch test (stuck loader): %d FAILED\n" : "relaunch test (stuck loader): all passed\n", g_fail);
	return g_fail ? 1 : 0;
#else
	// 1. The loader is up and closes its end at once (as the fork's does): the handshake comes, the payload outlives the call, waits for the "app" and launches once.
	{
		Begin("happy");
		loader.Start({});
		const pid_t app = SpawnApp(1.0);
		const double t_app = Now();
		const double t0 = Now();
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "start: %s", why.c_str());
		CHECK(Now() - t0 < 1.2, "the handshake is quick (%.2f s)", Now() - t0);
		CHECK(loader.connections == 1 && loader.payloads.size() == 1, "one connection");
		const std::string req = loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]);
		const std::string state = Field(req, "state");
		CHECK(Field(req, "pid") == std::to_string(app), "pid in the request: '%s'", req.c_str());
		CHECK(Field(req, "wait") == "PPSA99230", "closing title");
		CHECK(Field(req, "launch") == "PPSA99203", "launch title");
		// 176: the defaults of OrbisRelaunchTiming (orbis_leave::kSettleDefaultMs, kMinExitMs, kRetryMs)
		CHECK(Field(req, "settle_ms") == "1000" && Field(req, "min_exit_ms") == "1600" && Field(req, "retry_ms") == "400" && Field(req, "limit_s") == "45",
			"settle, earliest launch after the exit, retry pause and limit: '%s'", req.c_str());
		CHECK(state.compare(0, g_dir.size() + 10, g_dir + "/relaunch-") == 0, "state prefix in the logs folder: '%s'", state.c_str());
		CHECK(Exists(state + ".started"), ".started is there");
		CHECK(Status(state).empty() && Launches(g_dir).empty(), "nothing done while the app still runs");
		CHECK(WaitFile(state + ".done", 8.0), ".done appears");
		const auto ls = Launches(g_dir);
		CHECK(ls.size() == 1, "exactly one launch (%zu)", ls.size());
		if (!ls.empty())
		{
			CHECK(ls[0].title == "PPSA99203", "the title launched: %s", ls[0].title.c_str());
			CHECK(ls[0].at >= t_app + 1.0 + 1.5, "not before 1.6 s after the app is gone (%.2f s after the app started)", ls[0].at - t_app);
			CHECK(ls[0].at <= t_app + 1.0 + 3.5, "and not much later (%.2f s)", ls[0].at - t_app);
		}
		CHECK(Status(state) == "launched", "done says launched: '%s'", Status(state).c_str());
		loader.Stop();
	}

	// 2. No loader (the connect is refused): false after the retry budget (about 3 s), the reason names the connect, and nothing is left behind.
	{
		Begin("noloader");
		const double t0 = Now();
		CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203", 4242, why), "no loader: false");
		const double took = Now() - t0;
		CHECK(took > 2.6 && took < 4.0, "the retries take about 3 s (%.2f s)", took);
		CHECK(why.find("connect") != std::string::npos && why.find("tries") != std::string::npos, "reason: %s", why.c_str());
		CHECK(ListDir(g_dir).empty(), "no state files left");
		OrbisRelaunchCancel(); // harmless: there is no request
		CHECK(ListDir(g_dir).empty(), "...and a cancel creates none");
	}

	// 3. The loader is being restarted (it listens after 0.6 s; before that every connect is refused): the retries get through.
	{
		Begin("churn");
		loader.Start(LoaderCfg{600});
		const pid_t app = SpawnApp(0.8);
		const double t0 = Now();
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "start after a refused connect: %s", why.c_str());
		const double took = Now() - t0;
		CHECK(took >= 0.5 && took < 3.0, "it took the retries (%.2f s)", took);
		CHECK(loader.connections == 1, "one connection");
		const std::string state = Field(loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]), "state");
		CHECK(WaitFile(state + ".done", 8.0) && Launches(g_dir).size() == 1, "and the payload launched");
		loader.Stop();
	}

	// 4. The loader takes the payload and never starts it: false after about 3 s, a cancel note stands, and a payload that starts LATE launches nothing.
	{
		Begin("nostart");
		loader.Start({0, false});
		const pid_t app = SpawnApp(0.3);
		const double t0 = Now();
		CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "no '.started': false");
		const double took = Now() - t0;
		CHECK(took > 2.6 && took < 4.2, "after the budget (%.2f s)", took);
		CHECK(why.find("did not answer") != std::string::npos, "reason: %s", why.c_str());
		CHECK(loader.payloads.size() == 1, "the payload was sent");
		const std::string state = Field(loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]), "state");
		CHECK(Exists(state + ".cancel"), "the cancel note stands");
		CHECK(!Exists(state + ".started"), "...and the payload never started");
		if (!loader.payloads.empty())
		{
			RunPayloadAndWait(loader.payloads[0]); // it starts late
			CHECK(Launches(g_dir).empty(), "a late payload launches nothing");
			CHECK(Status(state) == "cancelled", "and says so: '%s'", Status(state).c_str());
		}
		loader.Stop();
	}

	// 5. Cancel() after Start(): the payload ends without launching, even though the app is gone later.
	{
		Begin("cancel");
		loader.Start({});
		const pid_t app = SpawnApp(1.0);
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "start: %s", why.c_str());
		const std::string state = Field(loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]), "state");
		OrbisRelaunchCancel();
		OrbisRelaunchCancel(); // again: harmless
		CHECK(Exists(state + ".cancel"), "cancel note");
		CHECK(WaitFile(state + ".done", 3.0), "the payload ends");
		CHECK(Status(state) == "cancelled", "done says cancelled: '%s'", Status(state).c_str());
		usleep(2500000); // the app is gone by now, the settle would be over
		CHECK(Launches(g_dir).empty(), "nothing launched");
		loader.Stop();
	}

	// 6. Things the app refuses to send: a request that does not fit, a bad title, a pid that can't be this process.
	{
		Begin("refused");
		loader.Start({});
		const std::string saved = g_dir;
		g_dir = "/tmp/" + std::string(1100, 'x'); // a state path so long that the request does not fit the payload's room
		CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203", 4242, why) && why.find("too large") != std::string::npos, "too large: %s", why.c_str());
		g_dir = saved;
		CHECK(!OrbisRelaunchStart("PPSA 99230", "PPSA99203", 4242, why), "a bad closing title");
		CHECK(!OrbisRelaunchStart("PPSA99230", "", 4242, why), "no launch title");
		CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203\nstate=/x", 4242, why), "a line break in a title");
		CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203", 0, why), "pid 0");
		CHECK(loader.connections == 0, "none of them sent anything");
		loader.Stop();
	}

	// 7. Two Starts in a row (the first leave was given up, the second begins): different state prefixes; the second Start calls the first payload off,
	// which launches nothing; the second payload launches once when the app is gone.
	{
		Begin("two");
		loader.Start({});
		const pid_t app = SpawnApp(30.0);
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "first start: %s", why.c_str());
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "second start: %s", why.c_str());
		CHECK(loader.payloads.size() == 2, "two payloads");
		std::string s1, s2;
		if (loader.payloads.size() == 2)
		{
			s1 = Field(RequestIn(loader.payloads[0]), "state");
			s2 = Field(RequestIn(loader.payloads[1]), "state");
		}
		CHECK(!s1.empty() && !s2.empty() && s1 != s2, "different state prefixes: '%s' '%s'", s1.c_str(), s2.c_str());
		CHECK(WaitFile(s1 + ".done", 3.0) && Status(s1) == "cancelled", "the first payload was called off: '%s'", Status(s1).c_str());
		CHECK(!Exists(s2 + ".cancel"), "the second one was not");
		const double t_kill = Now();
		kill(app, SIGKILL);
		CHECK(WaitFile(s2 + ".done", 8.0), "the second payload ends");
		const auto ls = Launches(g_dir);
		CHECK(ls.size() == 1, "exactly one launch in all (%zu)", ls.size());
		if (!ls.empty())
			CHECK(ls[0].at >= t_kill + 1.4, "after the app was killed plus the settle (%.2f s)", ls[0].at - t_kill);
		CHECK(Status(s2) == "launched", "'%s'", Status(s2).c_str());
		loader.Stop();
	}

	// 8. A loader that keeps the connection open for a while: the same result.
	{
		Begin("hold");
		loader.Start({0, true, true});
		const pid_t app = SpawnApp(0.5);
		CHECK(OrbisRelaunchStart("", "PPSA99203", app, why), "start with no closing title: %s", why.c_str());
		const std::string req = loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]);
		const std::string state = Field(req, "state");
		CHECK(!req.empty() && Field(req, "wait").empty(), "the closing title may be empty");
		CHECK(WaitFile(state + ".done", 8.0) && Launches(g_dir).size() == 1, "and the payload launched");
		CHECK(Status(state) == "launched-unchecked" || Status(state) == "launched", "'%s'", Status(state).c_str());
		loader.Stop();
	}

	// 9. Notes of earlier requests older than ten minutes are removed; fresh ones and other files stay.
	{
		Begin("stale");
		loader.Start({});
		const time_t old_time = std::time(nullptr) - 11 * 60;
		const utimbuf old_tb = {old_time, old_time};
		for (const char* name : {"relaunch-1-1.started", "relaunch-1-1.cancel", "relaunch-1-1.done", "relaunch-1-1.done.tmp"})
		{
			Touch(g_dir + "/" + name);
			utime((g_dir + "/" + name).c_str(), &old_tb);
		}
		for (const char* name : {"relaunch.log", "unrelated.txt", "relaunch-notes.txt"})
		{
			Touch(g_dir + "/" + name);
			utime((g_dir + "/" + name).c_str(), &old_tb);
		}
		Touch(g_dir + "/relaunch-2-1.started"); // fresh
		const pid_t app = SpawnApp(0.3);
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "start: %s", why.c_str());
		CHECK(!Exists(g_dir + "/relaunch-1-1.started") && !Exists(g_dir + "/relaunch-1-1.cancel") && !Exists(g_dir + "/relaunch-1-1.done") &&
		          !Exists(g_dir + "/relaunch-1-1.done.tmp"),
			"the old notes are gone");
		CHECK(Exists(g_dir + "/relaunch-2-1.started"), "a fresh one stays");
		CHECK(Exists(g_dir + "/relaunch.log") && Exists(g_dir + "/unrelated.txt") && Exists(g_dir + "/relaunch-notes.txt"), "other files stay");
		const std::string state = Field(loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]), "state");
		WaitFile(state + ".done", 8.0);
		loader.Stop();
	}

	// 10. A payload that cannot launch (the console lacks sceLncUtilLaunchApp) gives no handshake: Start() returns false as soon as the payload says it has
	// ended (not after the whole budget), the reason names it, nothing is launched and the app keeps its own way out. A working payload after it is not affected.
	{
		Begin("nolaunch");
		loader.Start({});
		setenv("RELAUNCH_STUB_NULL", "sceLncUtilLaunchApp", 1);
		const pid_t app = SpawnApp(0.3);
		const double t0 = Now();
		CHECK(!OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why), "a payload that cannot launch: false");
		const double took = Now() - t0;
		CHECK(took < 2.0, "...at once, not after the whole budget (%.2f s)", took);
		CHECK(why.find("nolaunch") != std::string::npos, "reason: %s", why.c_str());
		const std::string state = Field(loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]), "state");
		CHECK(!state.empty() && !Exists(state + ".started") && Status(state) == "nolaunch", "no handshake file, .done says nolaunch: '%s'", Status(state).c_str());
		usleep(1800000); // the app is gone and a settle would be over
		CHECK(Launches(g_dir).empty(), "nothing launched");
		OrbisRelaunchCancel(); // nothing left to call off: harmless
		unsetenv("RELAUNCH_STUB_NULL");
		const pid_t app2 = SpawnApp(0.3);
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app2, why), "a working payload afterwards: %s", why.c_str());
		const std::string state2 = Field(loader.payloads.size() > 1 ? RequestIn(loader.payloads[1]) : std::string(), "state");
		CHECK(!state2.empty() && state2 != state && WaitFile(state2 + ".done", 8.0) && Launches(g_dir).size() == 1, "and it launched once");
		loader.Stop();
	}

	// 11. 176: the timing the caller gives goes into the request and through the real payload: settle 250 ms, no earliest-launch rule, retries 150 ms apart
	// (the launch is refused twice first); then a settle of 5000 ms (the longest the flag menusettle allows) is in the request as written, and a cancel ends it.
	{
		Begin("timing");
		loader.Start({});
		setenv("RELAUNCH_STUB_RC", "-3,-3,0", 1);
		OrbisRelaunchTiming fast;
		fast.settle_ms = 250;
		fast.min_exit_ms = 0;
		fast.retry_ms = 150;
		const pid_t app = SpawnApp(0.8);
		const double t_app = Now();
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app, why, fast), "start with explicit timing: %s", why.c_str());
		const std::string req = loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]);
		const std::string state = Field(req, "state");
		CHECK(Field(req, "settle_ms") == "250" && Field(req, "min_exit_ms") == "0" && Field(req, "retry_ms") == "150", "the explicit timing is in the request: '%s'", req.c_str());
		CHECK(WaitFile(state + ".done", 8.0), ".done appears");
		const auto ls = Launches(g_dir);
		CHECK(ls.size() == 3, "refused twice, taken the third time (%zu)", ls.size());
		if (ls.size() == 3)
		{
			CHECK(ls[0].at >= t_app + 0.8 + 0.2 && ls[0].at <= t_app + 0.8 + 1.2, "the first attempt: exit + settle, no 1.6 s rule (%.2f s after the app started)", ls[0].at - t_app);
			CHECK(ls[2].at - ls[1].at >= 0.12 && ls[2].at - ls[1].at <= 0.6 && ls[1].at - ls[0].at >= 0.12 && ls[1].at - ls[0].at <= 0.6, "retries about 150 ms apart (%.2f, %.2f s)",
				ls[1].at - ls[0].at, ls[2].at - ls[1].at);
		}
		CHECK(Status(state) == "launched", "done says launched: '%s'", Status(state).c_str());
		unsetenv("RELAUNCH_STUB_RC");
		loader.Stop();

		Begin("timing_long");
		loader.Start({});
		OrbisRelaunchTiming slow;
		slow.settle_ms = 5000;
		slow.min_exit_ms = 3000;
		slow.retry_ms = 2000;
		const pid_t app2 = SpawnApp(0.3);
		CHECK(OrbisRelaunchStart("PPSA99230", "PPSA99203", app2, why, slow), "start with the longest timing: %s", why.c_str());
		const std::string req2 = loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]);
		const std::string state2 = Field(req2, "state");
		CHECK(Field(req2, "settle_ms") == "5000" && Field(req2, "min_exit_ms") == "3000" && Field(req2, "retry_ms") == "2000", "in the request as written: '%s'", req2.c_str());
		usleep(1200000); // the app is gone, the payload is settling
		CHECK(Launches(g_dir).empty() && !Exists(state2 + ".done"), "nothing launched while it settles");
		OrbisRelaunchCancel();
		CHECK(WaitFile(state2 + ".done", 3.0) && Status(state2) == "cancelled", "the cancel ends it: '%s'", Status(state2).c_str());
		CHECK(Launches(g_dir).empty(), "and nothing was launched");
		loader.Stop();
	}

	// fix180: GUARD mode. The main app leaves by its own exec; the guard launches PPSA99203 only when no new process says it is up. (a) the exec brings the
	// app back: the new process's OrbisRelaunchConfirmCameBack tells the guard (logs/menu-guard.txt -> <state>.alive): nothing launched. (b) it brings
	// nothing (the 180 case): PPSA99203 is launched after the guard. (c) a start with no guard note: nothing to say. Shortened guard (2 s) for the test.
	{
		Begin("guard_back", "PPSA99203");
		loader.Start({});
		OrbisRelaunchTiming g;
		g.settle_ms = 100;
		g.min_exit_ms = 0;
		g.guard_ms = 2000;
		g.hard_ms = 4000;
		const pid_t app = SpawnApp(0.4);
		CHECK(OrbisRelaunchStart("PPSA99203", "PPSA99203", app, why, g), "guard start: %s", why.c_str());
		const std::string req = loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]);
		const std::string state = Field(req, "state");
		CHECK(Field(req, "guard_ms") == "2000" && Field(req, "hard_ms") == "4000", "guard fields in the request: '%s'", req.c_str());
		CHECK(ReadFile(g_dir + "/menu-guard.txt").compare(0, state.size(), state) == 0, "menu-guard.txt names the state");
		usleep(900000); // the old process went at 0.4 s; the new one comes up now
		const std::string said = OrbisRelaunchConfirmCameBack();
		CHECK(said.find("its guard was told") != std::string::npos, "the new start tells the guard: '%s'", said.c_str());
		CHECK(!Exists(g_dir + "/menu-guard.txt"), "and removes the note");
		CHECK(WaitFile(state + ".done", 4.0) && Status(state) == "came-back", "the guard ends: '%s'", Status(state).c_str());
		CHECK(Launches(g_dir).empty(), "nothing launched when the exec brought the app back");
		const std::string log = OrbisRelaunchLastLog();
		CHECK(log.find("guard mode") != std::string::npos && log.find("came-back") != std::string::npos, "the last request's log lines for boot.log: '%s'",
			log.c_str());
		CHECK(OrbisRelaunchConfirmCameBack().empty(), "a second start has no guard to tell");
		loader.Stop();
	}
	{
		Begin("guard_not_back", "PPSA99203");
		loader.Start({});
		OrbisRelaunchTiming g;
		g.settle_ms = 100;
		g.min_exit_ms = 0;
		g.guard_ms = 2000;
		g.hard_ms = 4000;
		const pid_t app = SpawnApp(0.4);
		const double t_app = Now();
		CHECK(OrbisRelaunchStart("PPSA99203", "PPSA99203", app, why, g), "guard start: %s", why.c_str());
		const std::string state = Field(loader.payloads.empty() ? std::string() : RequestIn(loader.payloads[0]), "state");
		CHECK(WaitFile(state + ".done", 8.0) && Status(state) == "launched", "nothing came back: the guard launched: '%s'", Status(state).c_str());
		const auto ls = Launches(g_dir);
		CHECK(ls.size() == 1 && ls[0].title == "PPSA99203", "PPSA99203 launched once (%zu)", ls.size());
		if (!ls.empty())
			CHECK(ls[0].at >= t_app + 0.4 + 2.0 - 0.1, "not before the guard (%.2f s after the app started)", ls[0].at - t_app);
		const std::string log = OrbisRelaunchLastLog();
		CHECK(log.find("no new process said it is up") != std::string::npos, "the log says why: '%s'", log.c_str());
		loader.Stop();
	}
	{
		Begin("guard_stale");
		if (FILE* f = std::fopen((g_dir + "/menu-guard.txt").c_str(), "w"))
		{
			std::fprintf(f, "%s/relaunch-1\n%lld\n", g_dir.c_str(), static_cast<long long>(std::time(nullptr)) - 600);
			std::fclose(f);
		}
		const std::string said = OrbisRelaunchConfirmCameBack();
		CHECK(said.find("too old") != std::string::npos && !Exists(g_dir + "/relaunch-1.alive"), "a 10-minute-old note tells nothing: '%s'", said.c_str());
		CHECK(!Exists(g_dir + "/menu-guard.txt"), "and is removed");
	}
	{
		// fix181: no loader (the connect refused): no guard note is left behind for the next start
		Begin("guard_noloader", "PPSA99203");
		OrbisRelaunchTiming g;
		g.guard_ms = 2000;
		const pid_t app = SpawnApp(0.3);
		CHECK(!OrbisRelaunchStart("PPSA99203", "PPSA99203", app, why, g), "no loader: false (%s)", why.c_str());
		CHECK(!Exists(g_dir + "/menu-guard.txt"), "and no menu-guard.txt");
	}

	std::printf(g_fail ? "relaunch test: %d FAILED\n" : "relaunch test: all passed\n", g_fail);
	return g_fail ? 1 : 0;
#endif
}
