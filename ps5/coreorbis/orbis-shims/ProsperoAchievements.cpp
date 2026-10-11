// Persistent RetroAchievements credentials and shelf login worker (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cctype> // vk-285-134
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

namespace
{
	struct Account
	{
		std::mutex mutex;
		fe::AchievementAccountState state;
		std::unique_ptr<INISettingsInterface> credentials;
		pthread_t worker{};
		bool started = false;
	};
	Account& GetAccount()
	{
		// Runtime callbacks and settings layers outlive the shelf.
		static Account* account = new Account();
		return *account;
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
			Host::SetBaseBoolSettingValue("Achievements", "Enabled", true); // pr9n: on from now, with an account (review item 8)
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
	struct stat st;
	const bool exists = stat(path.c_str(), &st) == 0;
	const bool loaded = !exists || account.credentials->Load();
	const bool protected_file = !exists || chmod(path.c_str(), 0600) == 0;
	account.state.available = loaded && protected_file;
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
	if (!account.state.available)
		account.state.message = "Could not read or protect account storage.";
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
