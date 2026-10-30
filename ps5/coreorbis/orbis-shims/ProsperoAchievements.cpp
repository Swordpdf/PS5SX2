// Persistent RetroAchievements credentials and shelf login worker (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cctype> // vk-285-134
#include <cerrno>
#include <cstdio>
#include "ProsperoAchievements.h"
#include "ProsperoNotify.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/Error.h"

#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
	struct Account
	{
		std::mutex mutex;
		fe::AchievementAccountState state;
		std::unique_ptr<INISettingsInterface> credentials;
		std::string path; // ra180: the credentials file, for its mode after each save
		pthread_t worker{};
		bool started = false;
	};
	Account& GetAccount()
	{
		// Runtime callbacks and settings layers outlive the shelf.
		static Account* account = new Account();
		return *account;
	}
	// ra180 (2026-10-30, AI-assisted): the credentials file's mode. It was 0600, and PS5SX2 doesn't always run under the same
	// uid: uid 0 when a HEN answers the elevation request, uid 1 (the app's own) when none does. A 0600 file written under one
	// uid was unreadable (and not chmod-able) under the other, so the account was gone (177's and 180's reports: consoles with a
	// saved account whose sessions under the other uid never tried the token sign-in). Readable by every uid now (only this app
	// reaches /data/PCSX2: other apps are sandboxed away from it), written by its owner; INISettingsInterface::Save makes a 0600
	// file (mkstemp), so the mode is set again after every save.
	constexpr mode_t kAccountFileMode = 0644;
	void ProtectAccountFile(const Account& account)
	{
		if (!account.path.empty())
			chmod(account.path.c_str(), kAccountFileMode); // best effort: a drive without Unix modes keeps its own
	}
	struct Login
	{
		std::string username;
		std::string password;
	};

	void* LoginWorker(void* userdata)
	{
		std::unique_ptr<Login> login(static_cast<Login*>(userdata));
		Account& account = GetAccount();
		std::vector<std::pair<std::string, std::string>> previous;
		{
			auto lock = Host::GetSecretsSettingsLock();
			previous = account.credentials->GetKeyValueList("Achievements");
		}
		Error error;
		const bool success = Achievements::Login(login->username.c_str(), login->password.c_str(), &error);
		std::fill(login->password.begin(), login->password.end(), '\0');
		if (!success)
		{
			// Failed atomic Save leaves the old file intact; restore the in-memory credentials too.
			{
				auto lock = Host::GetSecretsSettingsLock();
				account.credentials->SetKeyValueList("Achievements", previous);
			}
			std::string old_username, old_timestamp;
			for (const auto& [key, value] : previous)
			{
				if (key == "Username")
					old_username = value;
				else if (key == "LoginTimestamp")
					old_timestamp = value;
			}
			Host::SetBaseStringSettingValue("Achievements", "Username", old_username.c_str());
			Host::SetBaseStringSettingValue("Achievements", "LoginTimestamp", old_timestamp.c_str());
		}
		if (success)
		{
			Host::SetBaseBoolSettingValue("Achievements", "Enabled", true); // pr9n: on from now, with an account (review item 8)
			ProtectAccountFile(account); // ra180
		}
		std::lock_guard lock(account.mutex);
		account.state.busy = false;
		account.state.authenticated = success;
		if (success)
		{
			account.state.saved = true;
			account.state.username = login->username;
		}
		// Do not publish arbitrary server responses containing a submitted credential.
		// vk-285-134 (AI-assisted): what kind of failure it was, from rcheevos' own error names (never the server's text):
		// a tester in build 130's logs got eleven 401s and then a 429, and only saw "Login failed".
		const std::string why = error.GetDescription();
		const auto starts = [&why](const char* prefix) { return why.rfind(prefix, 0) == 0; };
		std::string lower = why;
		for (char& c : lower)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const char* message = "Login failed. Check your account, connection and writable storage.";
		if (starts("Invalid credentials"))
			message = "Wrong user name or password. Use your RetroAchievements user name (not your e-mail) and your password "
					  "(not the Web API key).";
		else if (lower.find("too many") != std::string::npos || lower.find("429") != std::string::npos)
			message = "Too many sign-in tries. Wait a few minutes, then try again.";
		else if (starts("Access denied") || starts("Expired token"))
			message = "RetroAchievements refused this account. Check it on retroachievements.org (is the e-mail verified?).";
		else if (starts("No response"))
			message = "No answer from RetroAchievements. Check the console's connection.";
		account.state.message = success ? "" : message;
		if (!success)
		{
			std::printf("[achievements] sign-in failed: %s\n", starts("Invalid credentials") ? "invalid credentials" : message);
			std::fflush(stdout);
		}
		return nullptr;
	}

	bool BeginLogin(const std::string& username, const std::string& password)
	{
		Account& account = GetAccount();
		{
			std::lock_guard lock(account.mutex);
			if (!account.state.available || account.state.busy)
				return false;
		}
		OrbisAchievementsWaitForLogin();
		std::lock_guard lock(account.mutex);
		// Only offered on the shelf, while there is no active VM/client.
		if (Achievements::IsActive())
			return false;
		auto login = std::make_unique<Login>(Login{username, password});
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 1024 * 1024);
		account.state.busy = true;
		account.state.message.clear();
		account.started = pthread_create(&account.worker, &attr, LoginWorker, login.get()) == 0;
		pthread_attr_destroy(&attr);
		if (!account.started)
		{
			account.state.busy = false;
			account.state.message = "Could not start login.";
			std::fill(login->password.begin(), login->password.end(), '\0');
			return false;
		}
		login.release();
		return true;
	}

	void Logout()
	{
		OrbisAchievementsWaitForLogin();
		Achievements::Logout();
		Host::SetBaseBoolSettingValue("Achievements", "Enabled", false); // pr9n: no account, no achievements (review item 8)
		Account& account = GetAccount();
		Error error;
		bool saved;
		{
			auto lock = Host::GetSecretsSettingsLock();
			saved = account.credentials->Save(&error);
		}
		if (saved)
			ProtectAccountFile(account); // ra180
		std::lock_guard lock(account.mutex);
		account.state.authenticated = false;
		account.state.saved = false;
		account.state.username.clear();
		account.state.message = saved ? "" : "Could not save logout. Check writable storage before restarting.";
	}
} // namespace

bool OrbisAchievementsInit(SettingsInterface& base, const std::string& directory)
{
	Account& account = GetAccount();
	const std::string path = directory + "/achievements-secrets.ini";
	account.credentials = std::make_unique<INISettingsInterface>(path);
	account.path = path;
	// ra180 (2026-10-30, AI-assisted; a tester on 180, and older builds too: "Could not read or protect account storage", signed out
	// and no sign-in possible): a saved account the app couldn't read (fopen refused: the file is another user's, 0600 from a
	// session that ran under another uid, or a drive that refuses it) or couldn't chmod (a file that isn't ours, a drive without
	// Unix modes) switched the whole account off: `available` false, so the shelf and the web page refused every sign-in for good,
	// with nothing in the logs. Now: protection is best effort, an unreadable file only costs the saved sign-in (a new sign-in
	// saves a fresh file over it: Save writes a temporary file and renames it, which needs the folder, not the old file), and
	// what happened goes to the log.
	struct stat st;
	const bool exists = stat(path.c_str(), &st) == 0;
	int read_errno = 0;
	bool loaded = !exists || account.credentials->Load();
	if (!loaded)
	{
		std::FILE* probe = std::fopen(path.c_str(), "rb");
		read_errno = probe ? 0 : errno;
		if (probe)
			std::fclose(probe);
		account.credentials = std::make_unique<INISettingsInterface>(path); // nothing half-read survives
	}
	int chmod_errno = 0;
	if (exists && loaded && (st.st_mode & 0777) != kAccountFileMode && chmod(path.c_str(), kAccountFileMode) != 0)
		chmod_errno = errno;
	if (exists)
	{
		std::printf("[achievements] account file: %s, owner uid %u (app uid %u), mode %o, chmod errno %d, open errno %d\n",
			loaded ? "read" : "NOT READABLE (sign in again)", static_cast<unsigned>(st.st_uid), static_cast<unsigned>(geteuid()),
			static_cast<unsigned>(st.st_mode & 07777), chmod_errno, read_errno);
		std::fflush(stdout);
	}
	account.state.available = true;
	{
		auto lock = Host::GetSettingsLock();
		Host::Internal::SetBaseSettingsLayer(&base);
		Host::Internal::SetSecretsSettingsLayer(account.credentials.get());
		base.SetStringValue("Achievements", "Username", account.credentials->GetStringValue("Achievements", "Username", "").c_str());
		base.SetStringValue("Achievements", "LoginTimestamp", account.credentials->GetStringValue("Achievements", "LoginTimestamp", "").c_str());
	}
	account.state.username = base.GetStringValue("Achievements", "Username", "");
	account.state.saved = !account.state.username.empty() &&
	                      !account.credentials->GetStringValue("Achievements", "Token", "").empty();
	if (!loaded)
		account.state.message = "The saved RetroAchievements sign-in couldn't be read. Sign in again.";
	{
		auto lock = Host::GetSettingsLock();
		OrbisAchievementsConfigure(base); // again, now that `saved` is known
	}
	return account.state.available;
}

void OrbisAchievementsConfigure(SettingsInterface& settings)
{
	// pr9n, after the PR #9 review (AI-assisted):
	//  - item 8: on only with a saved account. Without one, every game start paid for the network and TLS setup and
	//    hashed the ELF for nothing. Signing in on the shelf switches it on (LoginWorker), signing out off;
	//  - item 6: PCSX2's key for hardcore is ChallengeMode (Pcsx2Config.cpp); the PR wrote "HardcoreMode", which nothing
	//    reads. Softcore only: Achievements.cpp refuses hardcore under PS5SX2_ACHIEVEMENTS as well.
	const Account& account = GetAccount();
	settings.SetBoolValue("Achievements", "Enabled", account.state.available && account.state.saved);
	settings.SetBoolValue("Achievements", "ChallengeMode", false);
	settings.SetBoolValue("Achievements", "Overlays", false);
	settings.SetBoolValue("Achievements", "LBOverlays", false);
	// 2026-10-05: on, so the PS5 toasts ask for the system's trophy ding and platinum sound (Achievements.cpp,
	// PS5Toast). PCSX2's own sound files still don't play here: Common::PlaySoundAsync is a stub. (AI-assisted)
	settings.SetBoolValue("Achievements", "SoundEffects", true);
}

void OrbisAchievementsRestoreAccount(SettingsInterface& settings)
{
	// pr9n (AI-assisted), PR #9 review item 4: after every ini merge, [Achievements]' account keys are the secrets file's
	// again, and the ones that may only live there (the token) or nowhere (another server) are gone from the layer.
	Account& account = GetAccount();
	if (!account.credentials)
		return;
	std::string username, timestamp;
	{
		auto lock = Host::GetSecretsSettingsLock();
		username = account.credentials->GetStringValue("Achievements", "Username", "");
		timestamp = account.credentials->GetStringValue("Achievements", "LoginTimestamp", "");
	}
	settings.SetStringValue("Achievements", "Username", username.c_str());
	settings.SetStringValue("Achievements", "LoginTimestamp", timestamp.c_str());
	settings.DeleteValue("Achievements", "Token");
	settings.DeleteValue("Achievements", "Host");
}

void OrbisAchievementsCommit()
{
	Account& account = GetAccount();
	if (!account.credentials)
		return;
	const std::string username = Host::GetBaseStringSettingValue("Achievements", "Username");
	const std::string timestamp = Host::GetBaseStringSettingValue("Achievements", "LoginTimestamp");
	auto lock = Host::GetSecretsSettingsLock();
	// Username and token are atomically saved together by the login/logout callback.
	account.credentials->SetStringValue("Achievements", "Username", username.c_str());
	account.credentials->SetStringValue("Achievements", "LoginTimestamp", timestamp.c_str());
}

fe::AchievementAccountService OrbisAchievementsAccountService()
{
	return {[] {
				Account& account = GetAccount();
				std::lock_guard lock(account.mutex);
				return account.state;
			},
		BeginLogin, Logout};
}

void OrbisAchievementsWaitForLogin()
{
	Account& account = GetAccount();
	if (account.started)
	{
		pthread_join(account.worker, nullptr);
		account.started = false;
	}
}

void OrbisAchievementsLoginRequired()
{
	Account& account = GetAccount();
	{
		std::lock_guard lock(account.mutex);
		account.state.authenticated = false;
		account.state.message = "Sign in again from the game shelf.";
	}
	OrbisNotifyPlain("RetroAchievements: sign in again from the game shelf.");
}

void OrbisAchievementsLoginUnreachable()
{
	// pr9n (review item 9): the token sign-in got no answer. The account stays; Achievements.cpp tells the player.
	Account& account = GetAccount();
	std::lock_guard lock(account.mutex);
	account.state.message = "Couldn't reach RetroAchievements. Still signed in.";
}

void OrbisAchievementsLoginSuccess(const char* username)
{
	Account& account = GetAccount();
	std::lock_guard lock(account.mutex);
	account.state.authenticated = true;
	account.state.username = username;
}
