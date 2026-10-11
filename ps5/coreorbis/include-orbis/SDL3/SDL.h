#pragma once
// Orbis SDL3 parse shim: just enough for Input/SDLInputSource.h to compile.
// SDL is replaced by libScePad (ProsperoInput.cpp); nothing here executes.
#include <stdint.h>

typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Haptic SDL_Haptic;
typedef uint32_t SDL_JoystickID;

typedef struct SDL_GamepadAxisEvent
{
  uint32_t type;
} SDL_GamepadAxisEvent;
typedef struct SDL_GamepadButtonEvent
{
  uint32_t type;
} SDL_GamepadButtonEvent;
typedef struct SDL_JoyAxisEvent
{
  uint32_t type;
} SDL_JoyAxisEvent;
typedef struct SDL_JoyButtonEvent
{
  uint32_t type;
} SDL_JoyButtonEvent;
typedef struct SDL_JoyHatEvent
{
  uint32_t type;
} SDL_JoyHatEvent;

// Parse-only stand-in: real haptics come from libScePad. Never constructed
// in our build (usb-pad-sdl-ff.cpp excluded).
typedef struct SDL_HapticEffect
{
  uint32_t type;
  char data[96];
} SDL_HapticEffect;

typedef union SDL_Event
{
  uint32_t type;
  char padding[56];
} SDL_Event;
