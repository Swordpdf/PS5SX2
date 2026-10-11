// Orbis UI stubs: FullscreenUI needs a GPU device + fonts; bring-up runs
// headless (Null GS). All callers (Hotkeys/VMManager/Achievements/GSRenderer)
// work fine with no-ops.
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "ImGui/ImGuiOverlays.h"
#include "Input/InputManager.h"
#include "common/SmallString.h"
#include "Host.h"
#include "imgui_freetype.h"
#ifdef PS5SX2_ACHIEVEMENTS
#include "ProsperoNotify.h"
#endif

namespace InputRecordingUI
{
} // namespace InputRecordingUI

InputRecordingUI::InputRecordingData g_InputRecordingData{};

namespace Host
{
void AddOSDMessage(std::string message, float duration)
{
#ifdef PS5SX2_ACHIEVEMENTS
	if (message.starts_with("Achievements"))
		OrbisNotifyPlain(message.c_str());
#endif
	(void)message;
	(void)duration;
}
void AddKeyedOSDMessage(std::string key, std::string message, float duration)
{
#ifdef PS5SX2_ACHIEVEMENTS
	if (key.starts_with("retroachievements") || message.starts_with("Achievements"))
		OrbisNotifyPlain(message.c_str());
#endif
	(void)key;
	(void)message;
	(void)duration;
}
void AddIconOSDMessage(std::string key, const char* icon, const std::string_view message, float duration)
{
#ifdef PS5SX2_ACHIEVEMENTS
	if (key.starts_with("retroachievements") || message.starts_with("Achievements"))
		OrbisNotifyPlain(std::string(message).c_str());
#endif
	(void)key;
	(void)icon;
	(void)message;
	(void)duration;
}
void RemoveKeyedOSDMessage(std::string key)
{
  (void)key;
}
void ClearOSDMessages()
{
}
} // namespace Host

namespace FullscreenUI
{
void CheckForConfigChanges(const Pcsx2Config& old_config)
{
  (void)old_config;
}
void OnVMStarted()
{
}
void OnVMDestroyed()
{
}
void OnVMResumed()
{
}
void GameChanged(std::string title, std::string path, std::string serial, u32 disc_crc, u32 crc)
{
  (void)title;
  (void)path;
  (void)serial;
  (void)disc_crc;
  (void)crc;
}
void OpenPauseMenu()
{
}
bool OpenAchievementsWindow()
{
  return false;
}
bool OpenLeaderboardsWindow()
{
  return false;
}
void ReportStateLoadError(const std::string& message, std::optional<s32> slot, bool backup)
{
  (void)message;
  (void)slot;
  (void)backup;
}
void ReportStateSaveError(const std::string& message, std::optional<s32> slot)
{
  (void)message;
  (void)slot;
}
bool IsAchievementsWindowOpen()
{
  return false;
}
bool IsLeaderboardsWindowOpen()
{
  return false;
}
void ReturnToPreviousWindow()
{
}
void ReturnToMainWindow()
{
}
void SetStandardSelectionFooterText(bool back_instead_of_cancel)
{
  (void)back_instead_of_cancel;
}
void Render()
{
}
TinyString TimeToPrintableString(time_t t)
{
  (void)t;
  return TinyString();
}
} // namespace FullscreenUI

namespace ImGuiManager
{
bool InitializeFullscreenUI()
{
  return false;
}
bool Initialize()
{
  // Orbis: no ImGui backend - never create the ImGui context (it crashes in
  // ImVector push_back without a render backend). Return success (headless).
  return true;
}
bool InitializeFullscreenUIOnce()
{
  return false;
}// Headless: never intercept input, no software cursor.
void UpdateMousePosition(float x, float y)
{
  (void)x;
  (void)y;
}
bool ProcessPointerButtonEvent(InputBindingKey key, float value)
{
  (void)key;
  (void)value;
  return false;
}
bool ProcessPointerAxisEvent(InputBindingKey key, float value)
{
  (void)key;
  (void)value;
  return false;
}
bool ProcessHostKeyEvent(InputBindingKey key, float value)
{
  (void)key;
  (void)value;
  return false;
}
bool ProcessGenericInputEvent(GenericInputBinding key, InputLayout layout, float value, u32 controller_id)
{
  (void)key;
  (void)layout;
  (void)value;
  (void)controller_id;
  return false;
}
void ProcessGenericAxisEvent(GenericInputBinding negative_key, GenericInputBinding positive_key, InputLayout layout, float value, u32 controller_id)
{
  (void)negative_key;
  (void)positive_key;
  (void)layout;
  (void)value;
  (void)controller_id;
}
bool HasSoftwareCursor(u32 index)
{
  (void)index;
  return false;
}
void SkipFrame()
{
}
void Shutdown(bool clear_state)
{
  (void)clear_state;
}
void NewFrame()
{
}
void RenderOSD()
{
}
void WindowResized()
{
}
void RequestScaleUpdate()
{
}
void ReloadFonts()
{
}
float GetWindowWidth()
{
  return 0.0f;
}
float GetWindowHeight()
{
  return 0.0f;
}
void SetSoftwareCursor(u32 index, std::string image_path, float image_scale, u32 multiply_color)
{
  (void)index;
  (void)image_path;
  (void)image_scale;
  (void)multiply_color;
}
void ClearSoftwareCursor(u32 index)
{
  (void)index;
}
void SetSoftwareCursorPosition(u32 index, float pos_x, float pos_y)
{
  (void)index;
  (void)pos_x;
  (void)pos_y;
}
} // namespace ImGuiManager

namespace ImGuiFullscreen
{
void SetNotificationPosition(float horizontal_position, float vertical_position, float direction)
{
  (void)horizontal_position;
  (void)vertical_position;
  (void)direction;
}
void OpenProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value)
{
  (void)str_id;
  (void)message;
  (void)min;
  (void)max;
  (void)value;
}
void CloseProgressDialog(const char* str_id)
{
  (void)str_id;
}
void AddNotification(std::string key, float duration, std::string title, std::string description, std::string badge_path)
{
#ifdef PS5SX2_ACHIEVEMENTS
	// The PS5's toast reads the image itself (a /data path showed on a PS5 Pro, 2026-10-05), so a cached badge or
	// game icon goes along. Unlocks, masteries, the game summary and the login don't come here: Achievements.cpp
	// sends those itself (PS5Toast), with a web fallback for images not cached yet and the trophy sounds.
	// (AI-assisted)
	OrbisNotifyRich(title.c_str(), description.c_str(), OrbisToastIcon(badge_path).c_str());
#endif
	(void)title;
	(void)duration;
	(void)description;
	(void)key;
	(void)badge_path;
}
bool InvalidateCachedTexture(const std::string& name)
{
  (void)name;
  return false;
}
} // namespace ImGuiFullscreen

// ImGuiFullscreen helpers used by GSDevice debug overlay. Headless: null.
namespace ImGuiFullscreen
{
std::pair<ImFont*, float> g_medium_font{nullptr, 0.0f};
std::pair<ImFont*, float> g_large_font{nullptr, 0.0f};
float g_layout_scale = 1.0f;
GSTexture* GetCachedTextureAsync(std::string_view name)
{
  (void)name;
  return nullptr;
}
bool WantsToCloseMenu()
{
  return true;
}
bool BeginFullscreenWindow(float left, float top, float width, float height, const char* name,
  const ImVec4& background, float rounding, const ImVec2& padding, ImGuiWindowFlags flags)
{
  (void)left;
  (void)top;
  (void)width;
  (void)height;
  (void)name;
  (void)background;
  (void)rounding;
  (void)padding;
  (void)flags;
  return false;
}
bool BeginFullscreenWindow(const ImVec2& position, const ImVec2& size, const char* name,
  const ImVec4& background, float rounding, const ImVec2& padding, ImGuiWindowFlags flags)
{
  (void)position;
  (void)size;
  (void)name;
  (void)background;
  (void)rounding;
  (void)padding;
  (void)flags;
  return false;
}
bool MenuButtonFrame(const char* str_id, bool enabled, float height, bool* visible, bool* hovered, ImVec2* min, ImVec2* max,
  ImGuiButtonFlags flags, float hover_alpha)
{
  (void)str_id;
  (void)enabled;
  (void)height;
  (void)visible;
  (void)hovered;
  (void)min;
  (void)max;
  (void)flags;
  (void)hover_alpha;
  return false;
}
void EndFullscreenWindow()
{
}
void BeginMenuButtons(u32 num_items, float y_align, float x_padding, float y_padding, float item_height)
{
  (void)num_items;
  (void)y_align;
  (void)x_padding;
  (void)y_padding;
  (void)item_height;
}
void EndMenuButtons()
{
}
bool MenuHeadingButton(const char* title, const char* value, bool enabled, bool draw_line)
{
  (void)title;
  (void)value;
  (void)enabled;
  (void)draw_line;
  return false;
}
bool IsGamepadInputSource()
{
  return false;
}
ImGuiFullscreen::GamepadGlyphs GetGamepadGlyphs()
{
  return {};
}
void SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> items)
{
  (void)items;
}
void RenderTextClippedWithShadow(const ImVec2& pos_min, const ImVec2& pos_max, const char* text, const char* text_end,
  const ImVec2* text_size_if_known, const ImVec2& align, const ImRect* clip_rect)
{
  (void)pos_min;
  (void)pos_max;
  (void)text;
  (void)text_end;
  (void)text_size_if_known;
  (void)align;
  (void)clip_rect;
}
bool FloatingButton(const char* text, float x, float y, float width, float height,
  float anchor_x, float anchor_y, bool enabled, std::pair<ImFont*, float> font, ImVec2* out_position,
  bool repeat_button)
{
  (void)text;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  (void)anchor_x;
  (void)anchor_y;
  (void)enabled;
  (void)font;
  (void)out_position;
  (void)repeat_button;
  return false;
}
void BeginNavBar(float x_padding, float y_padding)
{
  (void)x_padding;
  (void)y_padding;
}
void EndNavBar()
{
}
bool NavTab(const char* title, bool is_active, bool enabled, float width, float height, const ImVec4& background,
  std::pair<ImFont*, float> font)
{
  (void)title;
  (void)is_active;
  (void)enabled;
  (void)width;
  (void)height;
  (void)background;
  (void)font;
  return false;
}

// ImGuiFullscreen globals (normally owned by the UI).
ImVec4 UIPrimaryColor{};
ImVec4 UIPrimaryDarkColor{};
ImVec4 UIPrimaryTextColor{};
ImVec4 UISecondaryColor{};
float g_rcp_layout_scale = 1.0f;
} // namespace ImGuiFullscreen

namespace ImGuiFreeType
{
const ImFontLoader* GetFontLoader()
{
  return nullptr;
}
} // namespace ImGuiFreeType

namespace InputRec
{
void log(const std::string& log, const float duration)
{
  (void)log;
  (void)duration;
}
void consoleLog(const std::string& log)
{
  (void)log;
}
void consoleMultiLog(const std::vector<std::string>& logs)
{
  (void)logs;
}
} // namespace InputRec
