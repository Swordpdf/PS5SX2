// Controller-operated account panel, independent of the emulator/runtime (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>
#include <string>

namespace fe
{
	struct AchievementAccountState
	{
		bool available = false;
		bool busy = false;
		bool authenticated = false;
		bool saved = false;
		std::string username;
		std::string message;
	};

	struct AchievementAccountService
	{
		std::function<AchievementAccountState()> state;
		std::function<bool(const std::string&, const std::string&)> login;
		std::function<void()> logout;
	};

	// All input is local to the console. No password passes through the LAN settings page.
	class AchievementAccountPanel
	{
	public:
		~AchievementAccountPanel() { ClearPassword(); }
		static constexpr int Columns = 12;
		static constexpr int KeyCount = 95; // printable ASCII, including space
		static constexpr char Keys[] =
			"1234567890-="
			"qwertyuiop[]"
			"asdfghjkl;'\\"
			"zxcvbnm,./` "
			"!@#$%^&*()_+"
			"QWERTYUIOP{}"
			"ASDFGHJKL:\"|"
			"ZXCVBNM<>?~";
		static constexpr int KeyIndex(char value)
		{
			for (int i = 0; i < KeyCount; i++)
				if (Keys[i] == value)
					return i;
			return 0;
		}
		bool open = false;
		bool editing = false;
		bool show_password = false;
		int row = 0;
		int key = KeyIndex('a');
		std::string username;
		std::string password;
		AchievementAccountState account;

		void Poll(const AchievementAccountService& service)
		{
			if (service.state)
				account = service.state();
		}
		void Open()
		{
			open = true;
			editing = false;
			row = 0;
			username = account.username;
			ClearPassword();
		}
		void Close()
		{
			open = editing = false;
			ClearPassword();
		}
		void Move(int dx, int dy)
		{
			if (account.busy)
				return;
			if (editing)
				key = std::clamp(key + dx + dy * Columns, 0, KeyCount - 1);
			else
				row = std::clamp(row + dy, 0, 3);
		}
		void Back()
		{
			if (account.busy)
				return; // The caller must finish login before starting the VM.
			if (editing)
				editing = false;
			else
				Close();
		}
		void Erase()
		{
			if (!editing || account.busy)
				return;
			std::string& value = row == 0 ? username : password;
			if (!value.empty())
			{
				value.back() = '\0';
				value.pop_back();
			}
		}
		void TogglePasswordVisibility()
		{
			if (!account.busy)
				show_password = !show_password;
		}
		void Accept(const AchievementAccountService& service)
		{
			if (account.busy)
				return;
			if (editing)
			{
				std::string& value = row == 0 ? username : password;
				if (value.size() < (row == 0 ? 128u : 256u))
					value += Keys[key];
			}
			else if (row < 2)
				editing = true;
			else if (row == 2 && !username.empty() && !password.empty() && service.login)
			{
				if (service.login(username, password))
				{
					Poll(service);
				}
			}
			else if (row == 3 && service.logout)
			{
				ClearPassword();
				service.logout();
				Poll(service);
			}
		}
		void ClearPassword()
		{
			show_password = false;
			std::fill(password.begin(), password.end(), '\0');
			password.clear();
		}
	};
} // namespace fe
