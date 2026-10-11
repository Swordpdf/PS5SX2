// PS5SX2 (vk-285-72, vk-285-113): the PS5's USB keyboard and mouse, for the PS2.
//
// Two ways in, chosen by the PS5SX2/KeyboardMouse setting (see OrbisKbdMouseConfigure):
//
//  * As the PS2's USB keyboard and mouse (vk-285-72). main-boot.cpp puts PCSX2's USB HID keyboard on the PS2's USB port 1
//    and its HID mouse on port 2 (USB1/Type=hidkbd, USB2/Type=hidmouse; the flag file nousbkbm or a game's own settings
//    take them off). A thread reads the console's keyboard and mouse libraries every 4 ms and queues what changed;
//    OrbisKbdMousePump, at each vsync on the CPU thread (StubHost.cpp's Host::PumpMessagesOnCPUThread), hands it to the USB
//    devices there, where the emulated USB controller reads them. Games that support a USB keyboard or mouse (Half-Life,
//    Unreal Tournament, the online games' chat) see a standard HID keyboard and mouse; the others never look.
//
//  * As the PS2 controller (vk-285-113): what PCSX2 does on a PC, where "keyboard and mouse support" means the keys press the
//    pad's buttons. The keys are PCSX2's defaults (pcsx2/Input/InputManager.cpp, GetKeyboardGenericBindingMapping): the
//    arrow keys are the D-pad, W A S D the left stick, T F G H the right stick, Return Start, Backspace Select, I J K L
//    Triangle Square Cross Circle, Q and E the shoulder buttons, 1 and 3 the triggers, 2 and 4 the stick clicks. The mouse
//    moves the right stick (or the left, or nothing), its buttons are R1 and L1 (or R2 and L2) and the wheel's click R3.
//    F1 saves the state, F3 loads it and Esc held for a second goes back to the menu (a keyboard has no touchpad).
//    Which of the two a game gets is "Auto" by default: the controller, until the game reads the PS2's USB keyboard (or
//    mouse) -- then that one stays with the game. The pad thread (main-boot.cpp) merges OrbisKbdMousePadState into the
//    DualSense's state.
//
// The libraries are loaded while the game runs (sceKernelLoadStartModule, sceKernelDlsym), so a console that has none of them
// only logs it. Firmware 12.00 answers that load with 0x80020063 (the v112 logs: eight consoles, none on 10.60 or older);
// the thread logs the SDK versions the kernel compares and tries libkernel's sceKernelLoadStartModuleForSysmodule.
//
// The prototypes and data layouts are the public reimplementations' (the ps5-payload-dev SDK port's keyboard, and KytyPS5's
// keyboard and mouse), not Sony's headers:
//   - sceKeyboardReadState fills 96 bytes: "connected" at 16, the modifier byte at 28 (bit 0 left Ctrl ...
//     bit 7 right GUI, as in a USB boot report) and up to 16 HID usages (u16) at 32 (the two agree on
//     these; what the first 16 bytes hold beyond a timestamp they do not, so nothing reads them);
//   - sceMouseRead fills up to num 40-byte records, returns how many: a timestamp, "connected" at 8,
//     buttons at 12 (bit 0 primary, 1 secondary, 2 the middle one), then the x, y and wheel deltas at
//     16, 20 and 24 (s32).
// The buffers are larger than those sizes, and the first records a console returns go to boot.log, so a
// firmware whose layout differs shows it there instead of writing past the buffer.

#include "ProsperoKbdMouse.h"
#include "ProsperoKbdMap.h"

#include "OrbisPaths.h"

#include "USB/USB.h"
#include "USB/qemu-usb/hid.h"
#include "Input/InputManager.h"
#include "Config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// pcsx2/USB/usb-hid/usb-hid.cpp: how often a game has read the emulated keyboard's and mouse's interrupt endpoint.
extern std::atomic<unsigned> g_orbis_hid_kbd_polls;
extern std::atomic<unsigned> g_orbis_hid_mouse_polls;

extern "C" {
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);
int sceKernelGetModuleList(int32_t* handles, size_t max, size_t* count);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* user);
int sceUserServiceGetForegroundUser(int32_t* user);
}

namespace
{
	using namespace orbis_kbm;

	using KeyboardInit = int (*)();
	using KeyboardOpen = int (*)(int32_t user, int32_t type, int32_t index, const void* param);
	using KeyboardReadState = int (*)(int32_t handle, void* state);
	using MouseInit = int (*)();
	using MouseOpen = int (*)(int32_t user, int32_t type, int32_t index, const void* param);
	using MouseRead = int (*)(int32_t handle, void* data, int32_t num);

	constexpr size_t KEYBOARD_STATE_BYTES = 96;
	constexpr size_t MOUSE_RECORD_BYTES = 40;
	// Records asked for at a time, into a buffer with room for this many records of 128 bytes: a
	// firmware whose records are larger than the 40 bytes read here still writes inside it. A mouse
	// reporting at 1000 Hz leaves 4 or 5 in 4 ms.
	constexpr int MOUSE_RECORDS = 8;
	constexpr size_t MOUSE_BUFFER_BYTES = MOUSE_RECORDS * 128;
	constexpr uint32_t ESDKVERSION = 0x80020063u; // sceKernelLoadStartModule on firmware 12.00

	enum class Kind : uint8_t
	{
		Key,
		Button,
		Wheel,
	};
	struct Event
	{
		Kind kind;
		uint16_t code; // a HID usage (Key) or an InputButton (Button)
		float value;
	};

	std::mutex s_lock;
	std::vector<Event> s_events; // under s_lock
	long s_dx = 0, s_dy = 0; // under s_lock: the mouse movement since the last pump
	std::atomic<bool> s_running{false};
	constexpr size_t MAX_QUEUED = 512;

	// vk-285-113: the controller mode. Set by OrbisKbdMouseConfigure (settings), read by the thread and the pump.
	std::atomic<int> s_mode{0}, s_aim{1}, s_speed{2}, s_buttons{0};
	// The PS2's USB keyboard and mouse are being read by the game (the pump sees the polls at vsync).
	std::atomic<bool> s_usb_kbd_used{false}, s_usb_mouse_used{false};
	// What the thread publishes for the pad thread: ScePad button bits, and l2, r2, lx, ly, rx, ry a byte each.
	constexpr uint64_t IDLE_AXES = (128ull << 16) | (128ull << 24) | (128ull << 32) | (128ull << 40);
	std::atomic<bool> s_pad_enabled{false};
	std::atomic<uint32_t> s_pad_buttons{0};
	std::atomic<uint64_t> s_pad_axes{IDLE_AXES};
	std::atomic<int> s_hotkey{0};

	void Queue(Kind kind, uint16_t code, float value)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (s_events.size() < MAX_QUEUED)
			s_events.push_back({kind, code, value});
	}

	// A system library by name, from the directories the ps5-payload-dev SDK's loader searches.
	const char* const kLibraryDirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
		"/system_ex/priv_ex/lib/"};

	// An export of libkernel (or any loaded module) by name, for the calls the SDK's stubs list but an older firmware may
	// not have: looked up through the module list, so a missing one is a null and the eboot's imports stay as they were.
	template <typename F>
	F LoadedExport(const char* name)
	{
		int32_t handles[256];
		size_t count = 0;
		if (sceKernelGetModuleList(handles, 256, &count) != 0)
			return nullptr;
		for (size_t i = 0; i < count && i < 256; i++)
		{
			void* address = nullptr;
			if (sceKernelDlsym(handles[i], name, &address) == 0 && address)
				return reinterpret_cast<F>(address);
		}
		return nullptr;
	}

	// The SDK versions the kernel weighs when it loads a library: the process's (what this eboot says it was built with),
	// the newest the system allows, and the libraries' own. Logged once, where the load fails on firmware 12.00 (0x63 is
	// SCE_KERNEL_ERROR_ESDKVERSION).
	void LogSdkVersions()
	{
		using GetVersion = int (*)(uint32_t*);
		static const char* const names[] = {"sceKernelGetCompiledSdkVersion", "sceKernelGetProsperoCompiledSdkVersion",
			"sceKernelGetPs4CompiledSdkVersion", "sceKernelGetAllowedSdkVersionOnSystem"};
		for (const char* name : names)
		{
			const GetVersion fn = LoadedExport<GetVersion>(name);
			if (!fn)
			{
				printf("[kbm] sdk: %s: not in libkernel\n", name);
				continue;
			}
			uint32_t value[16] = {};
			const int rc = fn(value);
			printf("[kbm] sdk: %s: rc %#x, value %#x\n", name, static_cast<unsigned>(rc), value[0]);
		}
		using ByPath = int (*)(const char*, uint32_t*);
		const ByPath by_path = LoadedExport<ByPath>("sceKernelGetCompiledSdkVersionByPath");
		if (by_path)
		{
			static const char* const libraries[] = {"libSceKeyboard.sprx", "libSceMouse.sprx", "libScePad.sprx",
				"libSceUserService.sprx"};
			for (const char* library : libraries)
			{
				const std::string path = std::string(kLibraryDirs[0]) + library;
				uint32_t value[16] = {};
				const int rc = by_path(path.c_str(), value);
				printf("[kbm] sdk: %s: rc %#x, value %#x\n", library, static_cast<unsigned>(rc), value[0]);
			}
		}
		fflush(stdout);
	}

	// Firmware 12.00 refuses sceKernelLoadStartModule for these two libraries. libkernel's own variant for libSceSysmodule,
	// which the system uses for the libraries a title asks for by number, may not be refused. Tried once per app start, and
	// not again after a start that never came back from it: the marker file is written before the call and changed after,
	// so a call that crashes the app does not do it on every start.
	int LoadForSysmodule(const std::string& path)
	{
		using LoadStart = int (*)(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
		static bool s_tried = false;
		if (s_tried)
			return -1;
		s_tried = true;
		const std::string marker = OrbisLogPath("kbm-fallback.txt");
		{
			FILE* f = fopen(marker.c_str(), "rb");
			if (f)
			{
				char line[64] = {};
				const size_t n = fread(line, 1, sizeof(line) - 1, f);
				fclose(f);
				line[n] = 0;
				if (strncmp(line, "started", 7) == 0)
				{
					printf("[kbm] sceKernelLoadStartModuleForSysmodule: skipped, the last start never came back from it\n");
					fflush(stdout);
					return -1;
				}
			}
		}
		const LoadStart fn = LoadedExport<LoadStart>("sceKernelLoadStartModuleForSysmodule");
		if (!fn)
		{
			printf("[kbm] sceKernelLoadStartModuleForSysmodule: not in libkernel\n");
			fflush(stdout);
			return -1;
		}
		if (FILE* f = fopen(marker.c_str(), "wb"))
		{
			fputs("started\n", f);
			fclose(f);
		}
		int res = 0;
		const int handle = fn(path.c_str(), 0, nullptr, 0, nullptr, &res);
		if (FILE* f = fopen(marker.c_str(), "wb"))
		{
			fprintf(f, "done %#x\n", static_cast<unsigned>(handle));
			fclose(f);
		}
		printf("[kbm] %s: sceKernelLoadStartModuleForSysmodule %#x (start result %d)\n", path.c_str(),
			static_cast<unsigned>(handle), res);
		fflush(stdout);
		return handle;
	}

	int LoadModule(const char* name)
	{
		bool refused = false;
		std::string first_path;
		for (const char* dir : kLibraryDirs)
		{
			const std::string path = std::string(dir) + name;
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
				continue;
			if (first_path.empty())
				first_path = path;
			int res = 0;
			const int handle = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &res);
			printf("[kbm] %s: load %#x (start result %d)\n", path.c_str(), static_cast<unsigned>(handle), res);
			if (handle >= 0)
				return handle;
			if (static_cast<uint32_t>(handle) == ESDKVERSION)
				refused = true;
		}
		if (refused)
		{
			const int handle = LoadForSysmodule(first_path);
			if (handle >= 0)
				return handle;
		}
		printf("[kbm] %s: not found or not loadable\n", name);
		return -1;
	}

	template <typename F>
	bool Symbol(int module, const char* name, F& fn)
	{
		void* address = nullptr;
		const int rc = sceKernelDlsym(module, name, &address);
		fn = reinterpret_cast<F>(address);
		if (rc != 0 || !address)
			printf("[kbm] %s: dlsym %#x\n", name, static_cast<unsigned>(rc));
		return rc == 0 && address;
	}

	void HexDump(const char* what, const uint8_t* bytes, size_t count)
	{
		printf("[kbm] %s:", what);
		for (size_t i = 0; i < count; i++)
			printf("%s%02x", (i % 8) == 0 ? " " : "", bytes[i]);
		printf("\n");
	}

	// The keys a keyboard state holds down: the modifiers as usages 0xe0-0xe7, then the key codes.
	int PressedKeys(const uint8_t* state, uint16_t* keys)
	{
		int n = 0;
		if (state[16] == 0) // no keyboard
			return 0;
		uint32_t modifiers;
		std::memcpy(&modifiers, state + 28, sizeof(modifiers));
		for (int bit = 0; bit < 8; bit++)
		{
			if (modifiers & (1u << bit))
				keys[n++] = static_cast<uint16_t>(0xe0 + bit);
		}
		for (int i = 0; i < 16; i++)
		{
			uint16_t code;
			std::memcpy(&code, state + 32 + i * 2, sizeof(code));
			// 1-3 are the report's error codes (roll-over), and the modifiers came from their byte.
			if (code >= 4 && code < 0xe0)
				keys[n++] = code;
		}
		return n;
	}

	bool Contains(const uint16_t* keys, int n, uint16_t key)
	{
		for (int i = 0; i < n; i++)
		{
			if (keys[i] == key)
				return true;
		}
		return false;
	}

	void* Thread(void*)
	{
		int32_t user = -1;
		sceUserServiceInitialize(nullptr);
		if (sceUserServiceGetInitialUser(&user) != 0 || user < 0)
			sceUserServiceGetForegroundUser(&user);

		LogSdkVersions();

		KeyboardInit keyboard_init = nullptr;
		KeyboardOpen keyboard_open = nullptr;
		KeyboardReadState keyboard_read = nullptr;
		int keyboard = -1;
		const int keyboard_module = LoadModule("libSceKeyboard.sprx");
		if (keyboard_module >= 0 && Symbol(keyboard_module, "sceKeyboardInit", keyboard_init) &&
			Symbol(keyboard_module, "sceKeyboardOpen", keyboard_open) &&
			Symbol(keyboard_module, "sceKeyboardReadState", keyboard_read))
		{
			const int init = keyboard_init();
			uint8_t param[16] = {};
			keyboard = keyboard_open(user, 0, 0, param);
			printf("[kbm] keyboard: init %#x, open for user %d: %#x\n", static_cast<unsigned>(init), user,
				static_cast<unsigned>(keyboard));
		}

		MouseInit mouse_init = nullptr;
		MouseOpen mouse_open = nullptr;
		MouseRead mouse_read = nullptr;
		int mouse = -1;
		const int mouse_module = LoadModule("libSceMouse.sprx");
		if (mouse_module >= 0 && Symbol(mouse_module, "sceMouseInit", mouse_init) &&
			Symbol(mouse_module, "sceMouseOpen", mouse_open) && Symbol(mouse_module, "sceMouseRead", mouse_read))
		{
			const int init = mouse_init();
			// behaviorFlag 1: every mouse through this one handle.
			uint8_t param[8] = {1};
			mouse = mouse_open(user, 0, 0, param);
			if (mouse < 0)
			{
				std::memset(param, 0, sizeof(param));
				mouse = mouse_open(user, 0, 0, param);
			}
			printf("[kbm] mouse: init %#x, open for user %d: %#x\n", static_cast<unsigned>(init), user,
				static_cast<unsigned>(mouse));
		}
		fflush(stdout);
		if (keyboard < 0 && mouse < 0)
			return nullptr;
		s_running.store(true, std::memory_order_release);

		alignas(8) uint8_t state[256] = {};
		alignas(8) uint8_t records[MOUSE_BUFFER_BYTES] = {};
		uint16_t keys[24] = {}, previous[24] = {};
		int key_count = 0, previous_count = 0;
		uint32_t buttons = 0;
		bool keyboard_seen = false, mouse_seen = false;
		unsigned keyboard_errors = 0, mouse_errors = 0;
		AimState aim;
		Hotkeys hotkeys;
		bool was_keys_to_pad = false, was_mouse_to_pad = false;
		unsigned mode_lines = 0;
		for (;;)
		{
			long poll_dx = 0, poll_dy = 0;
			if (keyboard >= 0)
			{
				std::memset(state, 0, sizeof(state));
				const int rc = keyboard_read(keyboard, state);
				if (rc == 0)
				{
					key_count = PressedKeys(state, keys);
					if (!keyboard_seen && state[16] != 0)
					{
						keyboard_seen = true;
						HexDump("first connected keyboard state", state, KEYBOARD_STATE_BYTES + 8);
						fflush(stdout);
					}
					for (int i = 0; i < previous_count; i++)
					{
						if (!Contains(keys, key_count, previous[i]))
							Queue(Kind::Key, previous[i], 0.0f);
					}
					for (int i = 0; i < key_count; i++)
					{
						if (!Contains(previous, previous_count, keys[i]))
							Queue(Kind::Key, keys[i], 1.0f);
					}
					std::memcpy(previous, keys, sizeof(keys));
					previous_count = key_count;
				}
				else if (keyboard_errors++ < 5)
				{
					printf("[kbm] keyboard read %#x\n", static_cast<unsigned>(rc));
					fflush(stdout);
				}
			}
			if (mouse >= 0)
			{
				const int count = mouse_read(mouse, records, MOUSE_RECORDS);
				if (count > 0 && count <= MOUSE_RECORDS)
				{
					long dx = 0, dy = 0;
					for (int i = 0; i < count; i++)
					{
						const uint8_t* const record = records + static_cast<size_t>(i) * MOUSE_RECORD_BYTES;
						if (record[8] == 0) // not connected
							continue;
						if (!mouse_seen)
						{
							mouse_seen = true;
							HexDump("first connected mouse records", records,
								std::min<size_t>(static_cast<size_t>(count) * MOUSE_RECORD_BYTES + 8, 96));
							fflush(stdout);
						}
						uint32_t now;
						int32_t x, y, wheel;
						std::memcpy(&now, record + 12, 4);
						std::memcpy(&x, record + 16, 4);
						std::memcpy(&y, record + 20, 4);
						std::memcpy(&wheel, record + 24, 4);
						// A delta no mouse reports in 4 ms is a record read at the wrong place.
						if (x < -4096 || x > 4096 || y < -4096 || y > 4096)
							continue;
						dx += x;
						dy += y;
						static constexpr struct
						{
							uint32_t mask;
							InputButton button;
						} map[] = {{0x1, INPUT_BUTTON_LEFT}, {0x2, INPUT_BUTTON_RIGHT}, {0x4, INPUT_BUTTON_MIDDLE}};
						for (const auto& m : map)
						{
							if ((now ^ buttons) & m.mask)
								Queue(Kind::Button, static_cast<uint16_t>(m.button), (now & m.mask) ? 1.0f : 0.0f);
						}
						buttons = now;
						for (int notch = 0; notch < (wheel < 0 ? -wheel : wheel) && notch < 16; notch++)
							Queue(Kind::Wheel, 0, wheel > 0 ? 1.0f : -1.0f);
					}
					if (dx != 0 || dy != 0)
					{
						std::lock_guard<std::mutex> lock(s_lock);
						s_dx += dx;
						s_dy += dy;
					}
					poll_dx = dx;
					poll_dy = dy;
				}
				else if (count < 0 && mouse_errors++ < 5)
				{
					printf("[kbm] mouse read %#x\n", static_cast<unsigned>(count));
					fflush(stdout);
				}
			}

			// ---- The controller: what the keys held and the mouse make of it, for the pad thread.
			{
				const int mode = s_mode.load(std::memory_order_relaxed);
				const bool keys_to_pad = mode == 1 || (mode == 0 && !s_usb_kbd_used.load(std::memory_order_relaxed));
				const bool mouse_to_pad = mode == 1 || (mode == 0 && !s_usb_mouse_used.load(std::memory_order_relaxed));
				if ((keys_to_pad != was_keys_to_pad || mouse_to_pad != was_mouse_to_pad) && mode_lines++ < 24)
				{
					printf("[kbm] controller: keyboard %s, mouse %s (mode %d; the game reads the PS2's USB keyboard: %s, mouse: %s)\n",
						keys_to_pad ? "on" : "off", mouse_to_pad ? "on" : "off", mode,
						s_usb_kbd_used.load(std::memory_order_relaxed) ? "yes" : "no",
						s_usb_mouse_used.load(std::memory_order_relaxed) ? "yes" : "no");
					fflush(stdout);
				}
				was_keys_to_pad = keys_to_pad;
				was_mouse_to_pad = mouse_to_pad;

				PadInputs in;
				in.keys = keys;
				in.key_count = key_count;
				in.mouse_buttons = buttons;
				in.keys_to_pad = keys_to_pad;
				in.mouse_to_pad = mouse_to_pad;
				in.aim = s_aim.load(std::memory_order_relaxed);
				in.speed = s_speed.load(std::memory_order_relaxed);
				in.triggers = s_buttons.load(std::memory_order_relaxed) == 1;
				const PadOut pad = ComposePad(in, poll_dx, poll_dy, aim);
				const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				if (const int hotkey = hotkeys.Update(keys, key_count, keys_to_pad, now_ms))
					s_hotkey.store(hotkey, std::memory_order_release);

				const uint64_t axes = static_cast<uint64_t>(pad.l2) | (static_cast<uint64_t>(pad.r2) << 8) |
									  (static_cast<uint64_t>(pad.lx) << 16) | (static_cast<uint64_t>(pad.ly) << 24) |
									  (static_cast<uint64_t>(pad.rx) << 32) | (static_cast<uint64_t>(pad.ry) << 40);
				s_pad_buttons.store(pad.buttons, std::memory_order_relaxed);
				s_pad_axes.store(axes, std::memory_order_relaxed);
				s_pad_enabled.store(keys_to_pad || mouse_to_pad, std::memory_order_release);
			}
			usleep(4000);
		}
		return nullptr;
	}
} // namespace

void OrbisKbdMouseStart()
{
	static bool started = false;
	if (started)
		return;
	started = true;
	pthread_t thread;
	if (pthread_create(&thread, nullptr, Thread, nullptr) == 0)
		pthread_detach(thread);
}

void OrbisKbdMouseConfigure(int mode, int aim, int speed, int buttons)
{
	mode = std::max(0, std::min(3, mode));
	aim = std::max(0, std::min(2, aim));
	speed = std::max(1, std::min(4, speed));
	buttons = std::max(0, std::min(1, buttons));
	const int old_mode = s_mode.exchange(mode, std::memory_order_relaxed);
	const int old_aim = s_aim.exchange(aim, std::memory_order_relaxed);
	const int old_speed = s_speed.exchange(speed, std::memory_order_relaxed);
	const int old_buttons = s_buttons.exchange(buttons, std::memory_order_relaxed);
	if (old_mode != mode || old_aim != aim || old_speed != speed || old_buttons != buttons)
	{
		static const char* const modes[] = {"auto", "controller only", "USB devices only", "off"};
		static const char* const aims[] = {"nothing", "the right stick", "the left stick"};
		printf("[kbm] settings: keyboard and mouse %s; the mouse moves %s at speed %d, its buttons are %s "
			   "(PS5SX2/KeyboardMouse, MouseAim, MouseSpeed, MouseButtons)\n",
			modes[mode], aims[aim], speed, buttons == 1 ? "R2 and L2" : "R1 and L1");
		fflush(stdout);
	}
}

bool OrbisKbdMousePadState(OrbisKbdMousePad& out)
{
	if (!s_pad_enabled.load(std::memory_order_acquire))
		return false;
	const uint64_t axes = s_pad_axes.load(std::memory_order_relaxed);
	out.buttons = s_pad_buttons.load(std::memory_order_relaxed);
	out.l2 = static_cast<uint8_t>(axes);
	out.r2 = static_cast<uint8_t>(axes >> 8);
	out.lx = static_cast<uint8_t>(axes >> 16);
	out.ly = static_cast<uint8_t>(axes >> 24);
	out.rx = static_cast<uint8_t>(axes >> 32);
	out.ry = static_cast<uint8_t>(axes >> 40);
	return true;
}

int OrbisKbdMouseTakeHotkey()
{
	return s_hotkey.exchange(0, std::memory_order_acquire);
}

// At vsync on the CPU thread: what the thread queued, into the ports whose device takes it; and whether a game reads
// the PS2's USB keyboard or mouse (a game that does keeps that one, in the Auto mode).
void OrbisKbdMousePump()
{
	if (!s_running.load(std::memory_order_acquire))
		return;

	// A device counts as read for three seconds after a game's last poll of it.
	{
		static unsigned s_kbd_polls = 0, s_mouse_polls = 0;
		static unsigned s_kbd_idle = 1u << 20, s_mouse_idle = 1u << 20;
		constexpr unsigned IN_USE_FRAMES = 180;
		const unsigned kbd = g_orbis_hid_kbd_polls.load(std::memory_order_relaxed);
		const unsigned mouse = g_orbis_hid_mouse_polls.load(std::memory_order_relaxed);
		s_kbd_idle = kbd != s_kbd_polls ? 0 : std::min(s_kbd_idle + 1, 1u << 20);
		s_mouse_idle = mouse != s_mouse_polls ? 0 : std::min(s_mouse_idle + 1, 1u << 20);
		s_kbd_polls = kbd;
		s_mouse_polls = mouse;
		const bool kbd_used = s_kbd_idle < IN_USE_FRAMES, mouse_used = s_mouse_idle < IN_USE_FRAMES;
		if (kbd_used != s_usb_kbd_used.exchange(kbd_used, std::memory_order_relaxed))
		{
			printf("[kbm] the game %s the PS2's USB keyboard\n", kbd_used ? "reads" : "stopped reading");
			fflush(stdout);
		}
		if (mouse_used != s_usb_mouse_used.exchange(mouse_used, std::memory_order_relaxed))
		{
			printf("[kbm] the game %s the PS2's USB mouse\n", mouse_used ? "reads" : "stopped reading");
			fflush(stdout);
		}
	}

	std::vector<Event> events;
	long dx, dy;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		events.swap(s_events);
		dx = s_dx;
		dy = s_dy;
		s_dx = s_dy = 0;
	}
	// Controller only and Off: nothing for the PS2's USB devices (main-boot.cpp takes them off the ports as well).
	const int mode = s_mode.load(std::memory_order_relaxed);
	if (mode == 1 || mode == 3)
		return;
	static const s32 keyboard_type = USB::DeviceTypeNameToIndex("hidkbd");
	static const s32 mouse_type = USB::DeviceTypeNameToIndex("hidmouse");
	for (u32 port = 0; port < USB::NUM_PORTS; port++)
	{
		const s32 type = EmuConfig.USB.Ports[port].DeviceType;
		if (type == keyboard_type)
		{
			for (const Event& e : events)
			{
				if (e.kind == Kind::Key)
					USB::SetDeviceBindValue(port, e.code, e.value);
			}
		}
		else if (type == mouse_type)
		{
			for (const Event& e : events)
			{
				if (e.kind == Kind::Button)
					USB::SetDeviceBindValue(port, e.code, e.value);
				else if (e.kind == Kind::Wheel)
					USB::SetDeviceBindValue(port, INPUT_BUTTON__MAX + static_cast<u32>(InputPointerAxis::WheelY), e.value);
			}
			if (dx != 0)
				USB::SetDeviceBindValue(port, INPUT_BUTTON__MAX + static_cast<u32>(InputPointerAxis::X), static_cast<float>(dx));
			if (dy != 0)
				USB::SetDeviceBindValue(port, INPUT_BUTTON__MAX + static_cast<u32>(InputPointerAxis::Y), static_cast<float>(dy));
		}
	}
}
