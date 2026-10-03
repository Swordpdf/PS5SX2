// Real INI persistence and account worker, simulated RA login (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5/coreorbis/orbis-shims/ProsperoAchievements.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"

#include <cassert>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
	SettingsInterface* base = nullptr;
	SettingsInterface* secrets = nullptr;
	std::mutex base_mutex, secrets_mutex;
	bool fail_rename = false;
} // namespace
namespace Log
{
	void Writev(LOGLEVEL, ConsoleColors, const char*, va_list) {}
} // namespace Log
void pxOnAssertFail(const char*, int, const char*, const char*) { std::abort(); }
void OrbisNotifyPlain(const char*) {}

namespace FileSystem
{
	ManagedCFilePtr OpenManagedCFile(const char* filename, const char* mode, Error*)
	{
		return ManagedCFilePtr(fopen(filename, mode));
	}
	bool DeleteFilePath(const char* path, Error*) { return unlink(path) == 0; }
	bool RenamePath(const char* old_path, const char* new_path, Error*)
	{
		return !fail_rename && rename(old_path, new_path) == 0;
	}
} // namespace FileSystem
namespace Host
{
	std::unique_lock<std::mutex> GetSettingsLock() { return std::unique_lock(base_mutex); }
	std::unique_lock<std::mutex> GetSecretsSettingsLock() { return std::unique_lock(secrets_mutex); }
	std::string GetBaseStringSettingValue(const char* section, const char* key, const char* fallback)
	{
		auto lock = GetSettingsLock();
		return base->GetStringValue(section, key, fallback);
	}
	void SetBaseStringSettingValue(const char* section, const char* key, const char* value)
	{
		auto lock = GetSettingsLock();
		base->SetStringValue(section, key, value);
	}
	namespace Internal
	{
		void SetBaseSettingsLayer(SettingsInterface* si) { base = si; }
		void SetSecretsSettingsLayer(SettingsInterface* si) { secrets = si; }
	} // namespace Internal
} // namespace Host
namespace Achievements
{
	bool IsActive() { return false; }
	bool Login(const char* username, const char* password, Error* error)
	{
		assert(std::string(password) == "synthetic-password");
		Host::SetBaseStringSettingValue("Achievements", "Username", username);
		Host::SetBaseStringSettingValue("Achievements", "LoginTimestamp", "123");
		OrbisAchievementsCommit();
		secrets->SetStringValue("Achievements", "Token", "synthetic-token");
		if (!secrets->Save(error))
			return false;
		OrbisAchievementsLoginSuccess(username);
		return true;
	}
	void Logout()
	{
		Host::SetBaseStringSettingValue("Achievements", "Username", "");
		Host::SetBaseStringSettingValue("Achievements", "LoginTimestamp", "");
		OrbisAchievementsCommit();
		secrets->DeleteValue("Achievements", "Token");
		secrets->Save();
	}
} // namespace Achievements

int main(int argc, char** argv)
{
	assert(argc == 2);
	const std::string directory = argv[1];
	assert(mkdir(directory.c_str(), 0700) == 0);
	const std::string path = directory + "/achievements-secrets.ini";
	{
		INISettingsInterface remembered(path);
		remembered.SetStringValue("Achievements", "Username", "remembered");
		remembered.SetStringValue("Achievements", "Token", "remembered-token");
		assert(remembered.Save());
	}
	MemorySettingsInterface settings;
	assert(OrbisAchievementsInit(settings, directory));
	assert(settings.GetBoolValue("Achievements", "Enabled", false));
	assert(!settings.GetBoolValue("Achievements", "HardcoreMode", true));
	auto service = OrbisAchievementsAccountService();
	assert(service.state().saved && !service.state().authenticated);
	assert(service.login("new-user", "synthetic-password"));
	OrbisAchievementsWaitForLogin();
	assert(service.state().authenticated && service.state().username == "new-user");
	{
		INISettingsInterface stored(path);
		assert(stored.Load());
		assert(stored.GetStringValue("Achievements", "Username", "") == "new-user");
		assert(stored.GetStringValue("Achievements", "Token", "") == "synthetic-token");
	}
	struct stat st;
	assert(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
	std::ifstream file(path);
	const std::string contents((std::istreambuf_iterator<char>(file)), {});
	assert(contents.find("synthetic-password") == std::string::npos);
	// A failed atomic replacement must not pair a new username with the old token.
	fail_rename = true;
	assert(service.login("failed-user", "synthetic-password"));
	OrbisAchievementsWaitForLogin();
	assert(!service.state().authenticated && service.state().saved);
	assert(service.state().username == "new-user");
	assert(settings.GetStringValue("Achievements", "Username", "") == "new-user");
	assert(secrets->GetStringValue("Achievements", "Username", "") == "new-user");
	{
		INISettingsInterface stored(path);
		assert(stored.Load());
		assert(stored.GetStringValue("Achievements", "Username", "") == "new-user");
	}
	fail_rename = false;
	service.logout();
	assert(!service.state().saved && !service.state().authenticated);
	INISettingsInterface cleared(path);
	assert(cleared.Load());
	assert(cleared.GetStringValue("Achievements", "Username", "").empty());
	assert(cleared.GetStringValue("Achievements", "Token", "").empty());
}
