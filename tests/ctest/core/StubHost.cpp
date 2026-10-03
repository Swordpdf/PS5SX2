#include <cstdio>
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/Achievements.h"
#include "pcsx2/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/VMManager.h"
#include "common/Error.h"
#include <atomic>
#include <string>
#include "common/ProgressCallback.h"
#ifdef PS5SX2_ACHIEVEMENTS
#include "ps5/coreorbis/orbis-shims/ProsperoAchievements.h"
#endif

void Host::CommitBaseSettingChanges()
{
#ifdef PS5SX2_ACHIEVEMENTS
	OrbisAchievementsCommit();
#endif
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
#ifdef PS5SX2_ACHIEVEMENTS
	// LoadCoreSettings already ran; enforce the initial port's supported modes.
	EmuConfig.Achievements.HardcoreMode = false;
	EmuConfig.Achievements.Overlays = false;
	EmuConfig.Achievements.LBOverlays = false;
	EmuConfig.Achievements.SoundEffects = false;
#endif
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	std::printf("[host-info] %.*s: %.*s\n", (int)title.size(), title.data(), (int)message.size(), message.data());
	std::fflush(stdout);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	std::printf("[host-error] %.*s: %.*s\n", (int)title.size(), title.data(), (int)message.size(), message.data());
	std::fflush(stdout);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return std::nullopt;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	// Orbis/PS5: no window system, but ps5-opengl's EGL surface IS a real
	// presentable fullscreen surface (VideoOut). PCSX2 treats Surfaceless as
	// "headless, never present" (GSDeviceOGL::BeginPresent/SetSwapInterval/
	// RenderBlankFrame early-out on it), so report a windowed type. Nothing on
	// this platform branches on the specific value.
	WindowInfo wi;
	wi.type = WindowInfo::Type::X11;
	wi.surface_width = 1920;
	wi.surface_height = 1080;
	wi.surface_scale = 1.0f;
	wi.surface_refresh_rate = 60.0f;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
		const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
}

void Host::RunOnGSThread(std::function<void()> function)
{
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
}

// eerec-282: savestate requests from the pad thread (1 = save slot 1, 2 = load slot 1), run here at
// vsync on the CPU thread as PCSX2's hotkeys run through RunOnCPUThread. vk-285-48: 3 = back to the
// menu (the port's main-boot.cpp stops the VM and re-executes the app into its frontend).
std::atomic<int> g_orbis_state_request{0};
extern std::atomic<int> g_orbis_fast_speed; // vk-285-118 (main-boot.cpp): PS5SX2/FastSpeed
extern std::atomic<int> g_orbis_rb_auto_request; // vk-285-118 (GSRenderer.cpp OrbisReadbackAutoSecond)
void OrbisReadbackAutoCpu(); // vk-285-118 (main-boot.cpp)
void OrbisOSDLabel(const char* text);
void OrbisBackToMenuCpu();

extern std::atomic<int> g_orbis_gsini_reload, g_orbis_pin_request; // eerec-285 (GSRenderer.cpp)
void orbis_reload_gs_ini_cpu(); // eerec-285 (main-boot.cpp)
void OrbisCpuSample(int slot);
void OrbisApplyPinning(int mode);
#ifdef ORBIS_VULKAN
void OrbisEEProfStart(); // vk-285-8 (the port's orbis_eeprof.cpp): once, on this thread
void OrbisWidescreenTick(); // vk-285-12 (pcsx2/OrbisWidescreen.cpp)
#endif
void OrbisKbdMousePump(); // vk-285-72 (the port's ProsperoKbdMouse.cpp)
void Host::PumpMessagesOnCPUThread()
{
	OrbisCpuSample(0); // eerec-285: EE thread
	OrbisKbdMousePump(); // vk-285-72: the PS5's USB keyboard and mouse into the PS2's USB HID devices
#ifdef ORBIS_VULKAN
	OrbisEEProfStart();
	OrbisWidescreenTick();
#endif
	if (g_orbis_gsini_reload.exchange(0, std::memory_order_acq_rel))
		orbis_reload_gs_ini_cpu();
	if (const int pin = g_orbis_pin_request.exchange(-1, std::memory_order_acq_rel); pin >= 0)
		OrbisApplyPinning(pin);
	if (g_orbis_rb_auto_request.exchange(0, std::memory_order_acq_rel))
		OrbisReadbackAutoCpu();
	const int req = g_orbis_state_request.exchange(0, std::memory_order_acq_rel);
	if (req == 1)
	{
		bool failed = false;
		VMManager::SaveStateToSlot(1, false, [&failed](const std::string& err) {
			failed = true;
			std::printf("[state] save failed: %s\n", err.c_str());
		});
		OrbisOSDLabel(failed ? "SAVE FAILED" : "SAVED");
		std::fflush(stdout);
	}
	else if (req == 2)
	{
		Error err;
		if (VMManager::LoadStateFromSlot(1, false, &err))
			OrbisOSDLabel("LOADED");
		else
		{
			std::printf("[state] load failed: %s\n", err.GetDescription().c_str());
			OrbisOSDLabel("LOAD FAILED");
		}
		std::fflush(stdout);
	}
	else if (req == 3)
		OrbisBackToMenuCpu();
	else if (req == 4)
	{
		// vk-285-118 (AI-assisted): fast forward on or off (the Controls tab's fast forward combo, experimental: for skipping
		// videos). PS5SX2/FastSpeed: 0 as fast as the console runs it (Unlimited), 2..8 that many times full speed (Turbo).
		if (VMManager::GetLimiterMode() != LimiterModeType::Nominal)
		{
			VMManager::SetLimiterMode(LimiterModeType::Nominal);
			OrbisOSDLabel("NORMAL SPEED");
			std::printf("[state] fast forward off\n");
		}
		else
		{
			const int x = g_orbis_fast_speed.load(std::memory_order_relaxed);
			if (x >= 2)
			{
				EmuConfig.EmulationSpeed.TurboScalar = static_cast<float>(x);
				VMManager::SetLimiterMode(LimiterModeType::Turbo);
			}
			else
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
			OrbisOSDLabel(x >= 2 ? "FAST FORWARD" : "FAST FORWARD (MAX)");
			std::printf("[state] fast forward on (%s)\n", x >= 2 ? (std::to_string(x) + "x").c_str() : "unlimited");
		}
		std::fflush(stdout);
	}
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
#ifdef PS5SX2_ACHIEVEMENTS
	OrbisAchievementsLoginRequired();
#endif
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
#ifdef PS5SX2_ACHIEVEMENTS
	OrbisAchievementsLoginSuccess(username);
#endif
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : lhs.size() < rhs.size() ? -1 : 0;
}

// PS5 port (vk-285-72): the host's key codes are USB HID usages (page 7), which is what the console's
// keyboard library reports (the port's ProsperoKbdMouse.cpp). The names are the ones the USB HID keyboard
// device asks for (USB/usb-hid/usb-hid.cpp, s_qkeycode_names), so a key the console reports reaches the
// PS2's keyboard as the same key. Consumer-page keys (media, browser) have no usage here.
namespace
{
	struct OrbisHostKey
	{
		const char* name;
		u32 usage;
	};
	constexpr OrbisHostKey s_orbis_host_keys[] = {
		{"A", 0x04}, {"B", 0x05}, {"C", 0x06}, {"D", 0x07}, {"E", 0x08}, {"F", 0x09}, {"G", 0x0a}, {"H", 0x0b},
		{"I", 0x0c}, {"J", 0x0d}, {"K", 0x0e}, {"L", 0x0f}, {"M", 0x10}, {"N", 0x11}, {"O", 0x12}, {"P", 0x13},
		{"Q", 0x14}, {"R", 0x15}, {"S", 0x16}, {"T", 0x17}, {"U", 0x18}, {"V", 0x19}, {"W", 0x1a}, {"X", 0x1b},
		{"Y", 0x1c}, {"Z", 0x1d}, {"1", 0x1e}, {"2", 0x1f}, {"3", 0x20}, {"4", 0x21}, {"5", 0x22}, {"6", 0x23},
		{"7", 0x24}, {"8", 0x25}, {"9", 0x26}, {"0", 0x27}, {"Return", 0x28}, {"Escape", 0x29},
		{"Backspace", 0x2a}, {"Tab", 0x2b}, {"Space", 0x2c}, {"Minus", 0x2d}, {"Equal", 0x2e},
		{"BracketLeft", 0x2f}, {"BracketRight", 0x30}, {"Backslash", 0x31}, {"Semicolon", 0x33},
		{"Apostrophe", 0x34}, {"Agrave", 0x35}, {"Comma", 0x36}, {"Period", 0x37}, {"Slash", 0x38},
		{"Caps_lock", 0x39}, {"F1", 0x3a}, {"F2", 0x3b}, {"F3", 0x3c}, {"F4", 0x3d}, {"F5", 0x3e}, {"F6", 0x3f},
		{"F7", 0x40}, {"F8", 0x41}, {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45}, {"Print", 0x46},
		{"Scroll_lock", 0x47}, {"Pause", 0x48}, {"Insert", 0x49}, {"Home", 0x4a}, {"PageUp", 0x4b},
		{"Delete", 0x4c}, {"End", 0x4d}, {"PageDown", 0x4e}, {"Right", 0x4f}, {"Left", 0x50}, {"Down", 0x51},
		{"Up", 0x52}, {"Num_lock", 0x53}, {"NumpadSlash", 0x54}, {"NumpadAsterisk", 0x55},
		{"NumpadMinus", 0x56}, {"NumpadPlus", 0x57}, {"NumpadReturn", 0x58}, {"Numpad1", 0x59},
		{"Numpad2", 0x5a}, {"Numpad3", 0x5b}, {"Numpad4", 0x5c}, {"Numpad5", 0x5d}, {"Numpad6", 0x5e},
		{"Numpad7", 0x5f}, {"Numpad8", 0x60}, {"Numpad9", 0x61}, {"Numpad0", 0x62}, {"NumpadPeriod", 0x63},
		{"Less", 0x64}, {"Compose", 0x65}, {"Power", 0x66}, {"NumpadEqual", 0x67}, {"Help", 0x75},
		{"Menu", 0x76}, {"Front", 0x77}, {"Stop", 0x78}, {"Again", 0x79}, {"Undo", 0x7a}, {"Cut", 0x7b},
		{"Copy", 0x7c}, {"Paste", 0x7d}, {"Find", 0x7e}, {"AudioMute", 0x7f}, {"VolumeUp", 0x80},
		{"VolumeDown", 0x81}, {"NumpadComma", 0x85}, {"Ro", 0x87}, {"KatakanaHiragana", 0x88}, {"Yen", 0x89},
		{"Henkan", 0x8a}, {"Muhenkan", 0x8b}, {"Hiragana", 0x93}, {"Sysrq", 0x9a}, {"Control", 0xe0},
		{"Shift", 0xe1}, {"Alt", 0xe2}, {"Meta", 0xe3}, {"Control_r", 0xe4}, {"Shift_r", 0xe5}, {"Alt_r", 0xe6},
	};
} // namespace

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	for (const OrbisHostKey& key : s_orbis_host_keys)
	{
		if (str == key.name)
			return key.usage;
	}
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	for (const OrbisHostKey& key : s_orbis_host_keys)
	{
		if (code == key.usage)
			return std::string(key.name);
	}
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
