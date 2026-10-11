// Orbis input shims: SDL backend replaced by libScePad (later).
// Only ResetRGBForAllPlayers is referenced outside SDLInputSource.cpp.
#include "Input/SDLInputSource.h"
#include "USB/usb-pad/usb-pad-sdl-ff.h"
#include "common/SmallString.h"
#include <mutex>

void SDLInputSource::ResetRGBForAllPlayers(SettingsInterface& si)
{
  (void)si;
}
u32 SDLInputSource::GetRGBForPlayerId(SettingsInterface& si, u32 player_id)
{
  (void)si;
  (void)player_id;
  return 0;
}

// Headless: source exists (InputManager constructs it) but never fires.
SDLInputSource::SDLInputSource() = default;
SDLInputSource::~SDLInputSource() = default;
bool SDLInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  (void)si;
  (void)settings_lock;
  return false;
}
void SDLInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  (void)si;
  (void)settings_lock;
}
bool SDLInputSource::ReloadDevices()
{
  return false;
}
void SDLInputSource::Shutdown()
{
}
bool SDLInputSource::IsInitialized()
{
  return false;
}
void SDLInputSource::PollEvents()
{
}
std::vector<std::pair<std::string, std::string>> SDLInputSource::EnumerateDevices()
{
  return {};
}
std::vector<InputBindingKey> SDLInputSource::EnumerateMotors()
{
  return {};
}
bool SDLInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
  (void)device;
  (void)mapping;
  return false;
}
InputLayout SDLInputSource::GetControllerLayout(u32 index)
{
  (void)index;
  return InputLayout::Playstation;
}
void SDLInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
  (void)key;
  (void)intensity;
}
void SDLInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
  (void)large_key;
  (void)small_key;
  (void)large_intensity;
  (void)small_intensity;
}
std::optional<InputBindingKey> SDLInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
  (void)device;
  (void)binding;
  return std::nullopt;
}
TinyString SDLInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
  (void)key;
  (void)display;
  (void)migration;
  return TinyString();
}
TinyString SDLInputSource::ConvertKeyToIcon(InputBindingKey key)
{
  (void)key;
  return TinyString();
}

namespace usb_pad
{
std::unique_ptr<SDLFFDevice> SDLFFDevice::Create(const std::string_view device)
{
  (void)device;
  return nullptr;
}
SDLFFDevice::~SDLFFDevice() = default;
void SDLFFDevice::SetConstantForce(int level)
{
  (void)level;
}
void SDLFFDevice::SetSpringForce(const parsed_ff_data& ff)
{
  (void)ff;
}
void SDLFFDevice::SetDamperForce(const parsed_ff_data& ff)
{
  (void)ff;
}
void SDLFFDevice::SetFrictionForce(const parsed_ff_data& ff)
{
  (void)ff;
}
void SDLFFDevice::SetAutoCenter(int value)
{
  (void)value;
}
void SDLFFDevice::DisableForce(EffectID force)
{
  (void)force;
}
} // namespace usb_pad
