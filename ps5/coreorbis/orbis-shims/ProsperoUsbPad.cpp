// PS5SX2 (vk-285-140, AI-assisted): USB controllers the PS5 itself doesn't take as controllers.
//
// The PS5's controller library (ScePad) only reports the controllers the system supports: DualSense, DualShock 4 and the
// licensed pads. A guitar modded to act as a PS3 controller, a PS3 Guitar Hero guitar or an Xbox 360 pad gets power and
// nothing else, so PS5SX2 never saw it. This thread reads such a device itself, over USB, three ways, the first that works:
//
//   1. /dev/uhidN: the kernel's generic HID driver, if it took the device: read() gives its reports, ioctl its descriptor.
//   2. /dev/ugenB.A and its endpoints: FreeBSD's generic USB access. The interface's kernel driver is detached, then the
//      interrupt IN endpoint is read through its own node (/dev/usb/B.A.E), or through the USB_FS ioctls on the control node
//      when that node isn't there (what FreeBSD's libusb does).
//   3. libSceUsbd, the system's libusb-like library, loaded at run time (as the keyboard's is). Its calls are taken to be
//      libusb 1.0's, as on the PS4. A marker file keeps a crash in it from repeating at every start (flag nousbd skips it).
//
// What is never touched: Sony's PS4/PS5 controllers (the system's), keyboards and mice (the keyboard code's), and devices
// without a HID gamepad or XInput interface (USB drives, hubs). Only the chosen interface's driver is detached.
//
// The reports are decoded in OrbisUsbPadDecode.h. A guitar found at the first look (main-boot waits up to a second for it)
// makes PS2 port 1 PCSX2's Guitar controller; anything else, or a guitar plugged in later, is added to player 1's controller
// as the keyboard's is. Flag files: nousbpad (off), usbguitar / usbpad (the device is / isn't a guitar), nousbd, nougen.
//
// Everything goes to boot.log as [usbpad] lines: the device nodes the app can see, each device looked at and why it was
// taken or left, the reports of the first seconds and every change for a while, so a tester's log shows what a device
// sends even when the mapping is wrong.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoUsbPad.h"
#include "ProsperoNotify.h"

#include "OrbisPaths.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);
}

namespace
{
	using namespace orbis_usbpad;

	constexpr uint8_t kCurrentConfig = 0xFF; // USB_GET_FULL_DESC: the active configuration
	constexpr uint16_t kShortOk = 0x0004;    // USB_SHORT_XFER_OK
	constexpr int kReadTimeoutMs = 250;

	std::mutex s_lock;
	OrbisUsbPadOut s_out; // under s_lock
	bool s_connected = false;
	std::atomic<bool> s_started{false};
	std::mutex s_scan_lock;
	std::condition_variable s_scan_cv;
	bool s_first_scan_done = false; // under s_scan_lock
	bool s_first_scan_guitar = false;

	void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	void Log(const char* fmt, ...)
	{
		char line[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);
		printf("[usbpad] %s\n", line);
		fflush(stdout);
	}

	std::string Hex(const uint8_t* p, size_t n, size_t max = 64)
	{
		std::string s;
		char b[4];
		for (size_t i = 0; i < n && i < max; i++)
		{
			snprintf(b, sizeof(b), "%02x", p[i]);
			if (i && (i % 8) == 0)
				s += ' ';
			s += b;
		}
		if (n > max)
			s += " ...";
		return s;
	}

	// ---- the device being read ---------------------------------------------------------------------------------------------

	enum class Route : uint8_t
	{
		None,
		Uhid,
		UgenNode,
		UgenFs,
		Usbd,
	};

	const char* RouteName(Route r)
	{
		switch (r)
		{
			case Route::Uhid: return "uhid";
			case Route::UgenNode: return "ugen endpoint node";
			case Route::UgenFs: return "ugen USB_FS";
			case Route::Usbd: return "libSceUsbd";
			default: return "none";
		}
	}

	// libSceUsbd, libusb 1.0's calls under Sony's names.
	struct Usbd
	{
		bool tried = false, ok = false;
		int (*Init)() = nullptr;
		long (*GetDeviceList)(void*** list) = nullptr;
		void (*FreeDeviceList)(void** list, int unref) = nullptr;
		int (*GetDeviceDescriptor)(void* dev, void* desc) = nullptr;
		int (*Open)(void* dev, void** handle) = nullptr;
		void (*Close)(void* handle) = nullptr;
		int (*KernelDriverActive)(void* handle, int iface) = nullptr;
		int (*DetachKernelDriver)(void* handle, int iface) = nullptr;
		int (*ClaimInterface)(void* handle, int iface) = nullptr;
		int (*ControlTransfer)(void* handle, uint8_t type, uint8_t req, uint16_t value, uint16_t index, unsigned char* data,
			uint16_t len, unsigned int timeout) = nullptr;
		int (*InterruptTransfer)(void* handle, unsigned char ep, unsigned char* data, int len, int* done, unsigned int timeout) = nullptr;
	} s_usbd;

	struct Device
	{
		Route route = Route::None;
		std::string name;
		int ctl = -1, ep = -1;
		void* usbd = nullptr;
		uint16_t vid = 0, pid = 0;
		std::string product;
		Target target;
		Format format = Format::None;
		HidLayout layout;
		bool guitar = false;
		bool fs_open = false;
		usb_fs_endpoint fs_ep[1] = {};
		std::vector<uint8_t> fs_buf;
		void* fs_ptr = nullptr;
		uint32_t fs_len = 0;
	};

	void CloseDevice(Device& d)
	{
		if (d.fs_open && d.ctl >= 0)
		{
			usb_fs_close c = {};
			c.ep_index = 0;
			ioctl(d.ctl, USB_FS_CLOSE, &c);
			usb_fs_uninit u = {};
			ioctl(d.ctl, USB_FS_UNINIT, &u);
		}
		if (d.ep >= 0)
			close(d.ep);
		if (d.ctl >= 0)
			close(d.ctl);
		if (d.usbd && s_usbd.Close)
			s_usbd.Close(d.usbd);
		d = Device();
	}

	// Which decoding a device gets, and whether it is a guitar, from what is known of it. False: not ours.
	bool Classify(Device& d, const uint8_t* report_desc, size_t report_desc_len)
	{
		if (SystemOwned(d.vid, d.pid))
		{
			Log("%s: %04x:%04x is one of Sony's PS4/PS5 controllers: the system's", d.name.c_str(), d.vid, d.pid);
			return false;
		}
		if (d.target.xinput)
			d.format = Format::XInput;
		else if (d.vid == 0x054C && d.pid == 0x0268)
			d.format = Format::Ds3;
		else
		{
			if (report_desc_len > 0)
				d.layout = ParseHid(report_desc, report_desc_len);
			if (d.layout.keyboard && !d.layout.gamepad)
			{
				Log("%s: a keyboard or mouse: the keyboard code's", d.name.c_str());
				return false;
			}
			if (d.layout.ok && d.layout.gamepad)
				d.format = Format::Hid;
			else if (d.vid == 0x12BA || LooksLikeGuitar(d.vid, d.pid, d.product))
				d.format = Format::Ps3Fixed; // no usable descriptor, but a PS3 guitar's layout is known
			else
			{
				Log("%s: %04x:%04x \"%s\": no gamepad in its report descriptor (%zu bytes): left alone", d.name.c_str(), d.vid,
					d.pid, d.product.c_str(), report_desc_len);
				return false;
			}
		}
		d.guitar = OrbisFlag("usbguitar") ? true : OrbisFlag("usbpad") ? false : LooksLikeGuitar(d.vid, d.pid, d.product);
		return true;
	}

	void LogTaken(const Device& d, size_t desc_len)
	{
		Log("taken: %s via %s: %04x:%04x \"%s\", interface %u, endpoint %#x (%u bytes), %s, as a %s%s", d.name.c_str(),
			RouteName(d.route), d.vid, d.pid, d.product.c_str(), d.target.iface, d.target.ep_in, d.target.max_packet,
			FormatName(d.format), d.guitar ? "guitar" : "controller",
			OrbisFlag("usbguitar") ? " (flag usbguitar)" : OrbisFlag("usbpad") ? " (flag usbpad)" : "");
		if (d.format == Format::Hid)
		{
			int buttons = 0, axes = 0, hats = 0;
			for (const HidField& f : d.layout.fields)
			{
				buttons += f.page == 9;
				axes += f.page == 1 && f.usage >= 0x30 && f.usage <= 0x35;
				hats += f.page == 1 && f.usage == 0x39;
			}
			Log("  report descriptor %zu bytes: %d buttons, %d axes, %d hat%s, %s", desc_len, buttons, axes, hats,
				hats == 1 ? "" : "s", d.layout.has_ids ? "report IDs" : "no report IDs");
		}
	}

	// ---- 1. uhid --------------------------------------------------------------------------------------------------------------

	bool TryUhid(const std::string& node, Device& d)
	{
		const std::string path = "/dev/" + node;
		const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
		if (fd < 0)
		{
			Log("%s: open: errno %d", path.c_str(), errno);
			return false;
		}
		d = Device();
		d.name = path;
		usb_device_info di = {};
		if (ioctl(fd, USB_GET_DEVICEINFO, &di) == 0)
		{
			d.vid = di.udi_vendorNo;
			d.pid = di.udi_productNo;
			d.product = di.udi_product;
		}
		std::vector<uint8_t> desc(4096);
		usb_gen_descriptor gd = {};
		gd.ugd_data = desc.data();
		gd.ugd_maxlen = static_cast<uint16_t>(desc.size());
		const int rc = ioctl(fd, USB_GET_REPORT_DESC, &gd);
		const size_t len = rc == 0 ? std::min<size_t>(gd.ugd_actlen, desc.size()) : 0;
		Log("%s: %04x:%04x \"%s\", report descriptor %s (%zu bytes)", path.c_str(), d.vid, d.pid, d.product.c_str(),
			rc == 0 ? "read" : "not read", len);
		d.target.iface = 0;
		d.target.max_packet = 64;
		if (!Classify(d, desc.data(), len))
		{
			close(fd);
			d = Device();
			return false;
		}
		d.route = Route::Uhid;
		d.ep = fd;
		LogTaken(d, len);
		return true;
	}

	// ---- 2. ugen ----------------------------------------------------------------------------------------------------------------

	bool UgenRequest(int fd, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void* data, uint16_t len, uint16_t* actual)
	{
		usb_ctl_request r = {};
		r.ucr_data = data;
		r.ucr_flags = kShortOk;
		r.ucr_request.bmRequestType = type;
		r.ucr_request.bRequest = req;
		r.ucr_request.wValue[0] = value & 0xFF;
		r.ucr_request.wValue[1] = value >> 8;
		r.ucr_request.wIndex[0] = index & 0xFF;
		r.ucr_request.wIndex[1] = index >> 8;
		r.ucr_request.wLength[0] = len & 0xFF;
		r.ucr_request.wLength[1] = len >> 8;
		if (ioctl(fd, USB_DO_REQUEST, &r) != 0)
			return false;
		if (actual)
			*actual = r.ucr_actlen;
		return true;
	}

	// A DualShock 3 sends nothing until it is told to (feature report 0xf4).
	void Ds3Enable(Device& d)
	{
		uint8_t on[4] = {0x42, 0x0C, 0x00, 0x00};
		bool ok = false;
		if (d.ctl >= 0)
			ok = UgenRequest(d.ctl, 0x21, 0x09, 0x03F4, d.target.iface, on, sizeof(on), nullptr);
		else if (d.usbd && s_usbd.ControlTransfer)
			ok = s_usbd.ControlTransfer(d.usbd, 0x21, 0x09, 0x03F4, d.target.iface, on, sizeof(on), 500) >= 0;
		Log("DualShock 3: reports turned on: %s", ok ? "yes" : "no");
	}

	bool UgenFsOpen(Device& d)
	{
		usb_fs_init in = {};
		in.pEndpoints = d.fs_ep;
		in.ep_index_max = 1;
		if (ioctl(d.ctl, USB_FS_INIT, &in) != 0)
		{
			Log("%s: USB_FS_INIT: errno %d", d.name.c_str(), errno);
			return false;
		}
		usb_fs_open op = {};
		op.max_bufsize = std::max<uint32_t>(d.target.max_packet, 64);
		op.max_frames = 1;
		op.ep_index = 0;
		op.ep_no = d.target.ep_in;
		if (ioctl(d.ctl, USB_FS_OPEN, &op) != 0)
		{
			Log("%s: USB_FS_OPEN endpoint %#x: errno %d", d.name.c_str(), d.target.ep_in, errno);
			usb_fs_uninit u = {};
			ioctl(d.ctl, USB_FS_UNINIT, &u);
			return false;
		}
		d.fs_open = true;
		d.fs_buf.assign(op.max_bufsize, 0);
		return true;
	}

	bool TryUgen(const std::string& node, Device& d)
	{
		const std::string path = "/dev/" + node;
		const int fd = open(path.c_str(), O_RDWR);
		if (fd < 0)
		{
			Log("%s: open: errno %d", path.c_str(), errno);
			return false;
		}
		usb_device_info di = {};
		if (ioctl(fd, USB_GET_DEVICEINFO, &di) != 0)
		{
			Log("%s: USB_GET_DEVICEINFO: errno %d", path.c_str(), errno);
			close(fd);
			return false;
		}
		d = Device();
		d.name = path;
		d.vid = di.udi_vendorNo;
		d.pid = di.udi_productNo;
		d.product = di.udi_product;
		if (di.udi_class == 9 || di.udi_class == 8)
		{
			close(fd);
			d = Device();
			return false; // hubs and storage: not even logged each scan
		}
		std::vector<uint8_t> cfg(4096);
		usb_gen_descriptor gd = {};
		gd.ugd_data = cfg.data();
		gd.ugd_maxlen = static_cast<uint16_t>(cfg.size());
		gd.ugd_config_index = kCurrentConfig;
		const bool have_cfg = ioctl(fd, USB_GET_FULL_DESC, &gd) == 0;
		const size_t cfg_len = have_cfg ? std::min<size_t>(gd.ugd_actlen, cfg.size()) : 0;
		d.target = FindTarget(cfg.data(), cfg_len);
		Log("%s: %04x:%04x \"%s\" (\"%s\"), class %u, bus %u address %u, configuration %s (%zu bytes): %s", path.c_str(), d.vid,
			d.pid, d.product.c_str(), di.udi_vendor, di.udi_class, di.udi_bus, di.udi_addr, have_cfg ? "read" : "not read",
			cfg_len, d.target.ok ? "a gamepad interface" : "no gamepad interface");
		if (!d.target.ok)
		{
			if (have_cfg && cfg_len)
				Log("  configuration: %s", Hex(cfg.data(), cfg_len, 96).c_str());
			close(fd);
			d = Device();
			return false;
		}
		std::vector<uint8_t> desc(4096);
		uint16_t desc_len = 0;
		if (!d.target.xinput)
		{
			const uint16_t want = d.target.report_desc_len ? d.target.report_desc_len : 1024;
			if (!UgenRequest(fd, 0x81, 0x06, 0x2200, d.target.iface, desc.data(), std::min<uint16_t>(want, 4096), &desc_len))
				Log("%s: report descriptor: errno %d", path.c_str(), errno);
		}
		if (!Classify(d, desc.data(), desc_len))
		{
			close(fd);
			d = Device();
			return false;
		}
		d.ctl = fd;
		// The interface's kernel driver (if any) lets go of it; nothing else of the device is touched.
		int iface = d.target.iface;
		if (ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &iface) == 0)
		{
			iface = d.target.iface;
			const int rc = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
			Log("%s: interface %u had a kernel driver: detach %s (errno %d)", path.c_str(), d.target.iface, rc == 0 ? "done" : "failed",
				rc == 0 ? 0 : errno);
		}
		iface = d.target.iface;
		ioctl(fd, USB_CLAIM_INTERFACE, &iface);
		// The endpoint's own node first (read() on it), then USB_FS on the control node.
		char ep_path[64];
		snprintf(ep_path, sizeof(ep_path), "/dev/usb/%u.%u.%u", di.udi_bus, di.udi_addr, d.target.ep_in & 0x0F);
		const int ep = open(ep_path, O_RDONLY | O_NONBLOCK);
		if (ep >= 0)
		{
			int one = 1;
			ioctl(ep, USB_SET_RX_SHORT_XFER, &one);
			d.ep = ep;
			d.route = Route::UgenNode;
		}
		else
		{
			Log("%s: open: errno %d, trying USB_FS", ep_path, errno);
			if (!UgenFsOpen(d))
			{
				CloseDevice(d);
				return false;
			}
			d.route = Route::UgenFs;
		}
		if (d.format == Format::Ds3)
			Ds3Enable(d);
		LogTaken(d, desc_len);
		return true;
	}

	// ---- 3. libSceUsbd --------------------------------------------------------------------------------------------------------

	template <typename F>
	void Sym(int module, const char* name, F& fn)
	{
		void* a = nullptr;
		if (sceKernelDlsym(module, name, &a) == 0 && a)
			fn = reinterpret_cast<F>(a);
		else
			Log("libSceUsbd: %s not found", name);
	}

	bool UsbdLoad()
	{
		if (s_usbd.tried)
			return s_usbd.ok;
		s_usbd.tried = true;
		if (OrbisFlag("nousbd"))
		{
			Log("libSceUsbd: skipped (flag nousbd)");
			return false;
		}
		// A start that never came back from libSceUsbd (a call whose shape isn't what this code thinks) isn't repeated.
		const std::string marker = OrbisLogPath("usbd-marker.txt");
		if (FILE* f = fopen(marker.c_str(), "rb"))
		{
			char line[32] = {};
			const size_t n = fread(line, 1, sizeof(line) - 1, f);
			fclose(f);
			line[n] = 0;
			if (strncmp(line, "started", 7) == 0)
			{
				Log("libSceUsbd: skipped: the last start never came back from it (delete logs/usbd-marker.txt to try again)");
				return false;
			}
		}
		int module = -1;
		static const char* const dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/"};
		for (const char* dir : dirs)
		{
			const std::string path = std::string(dir) + "libSceUsbd.sprx";
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
				continue;
			int res = 0;
			module = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &res);
			Log("libSceUsbd: %s: load %#x (start result %d)", path.c_str(), static_cast<unsigned>(module), res);
			if (module >= 0)
				break;
		}
		if (module < 0)
		{
			Log("libSceUsbd: not loadable");
			return false;
		}
		Sym(module, "sceUsbdInit", s_usbd.Init);
		Sym(module, "sceUsbdGetDeviceList", s_usbd.GetDeviceList);
		Sym(module, "sceUsbdFreeDeviceList", s_usbd.FreeDeviceList);
		Sym(module, "sceUsbdGetDeviceDescriptor", s_usbd.GetDeviceDescriptor);
		Sym(module, "sceUsbdOpen", s_usbd.Open);
		Sym(module, "sceUsbdClose", s_usbd.Close);
		Sym(module, "sceUsbdKernelDriverActive", s_usbd.KernelDriverActive);
		Sym(module, "sceUsbdDetachKernelDriver", s_usbd.DetachKernelDriver);
		Sym(module, "sceUsbdClaimInterface", s_usbd.ClaimInterface);
		Sym(module, "sceUsbdControlTransfer", s_usbd.ControlTransfer);
		Sym(module, "sceUsbdInterruptTransfer", s_usbd.InterruptTransfer);
		if (!s_usbd.Init || !s_usbd.GetDeviceList || !s_usbd.FreeDeviceList || !s_usbd.GetDeviceDescriptor || !s_usbd.Open ||
			!s_usbd.Close || !s_usbd.ControlTransfer || !s_usbd.InterruptTransfer)
			return false;
		if (FILE* f = fopen(marker.c_str(), "wb"))
		{
			fputs("started\n", f);
			fclose(f);
		}
		const int rc = s_usbd.Init();
		Log("libSceUsbd: sceUsbdInit %#x", static_cast<unsigned>(rc));
		s_usbd.ok = rc >= 0;
		return s_usbd.ok;
	}

	// The marker says "done" once a scan through libSceUsbd has come back.
	void UsbdMarkDone()
	{
		static bool done = false;
		if (done)
			return;
		done = true;
		if (FILE* f = fopen(OrbisLogPath("usbd-marker.txt").c_str(), "wb"))
		{
			fputs("done\n", f);
			fclose(f);
		}
	}

	std::string UsbdString(void* h, uint8_t index)
	{
		if (!index)
			return {};
		unsigned char buf[256] = {};
		const int n = s_usbd.ControlTransfer(h, 0x80, 0x06, static_cast<uint16_t>(0x0300 | index), 0x0409, buf, sizeof(buf), 500);
		std::string s;
		for (int i = 2; i + 1 < n && i < static_cast<int>(sizeof(buf)) - 1; i += 2)
			s += buf[i] >= 0x20 && buf[i] < 0x7F && buf[i + 1] == 0 ? static_cast<char>(buf[i]) : '?';
		return s;
	}

	bool TryUsbd(Device& out, std::vector<std::string>& seen)
	{
		if (!UsbdLoad())
			return false;
		void** list = nullptr;
		const long count = s_usbd.GetDeviceList(&list);
		UsbdMarkDone();
		static long s_last_count = -2;
		if (count != s_last_count)
		{
			Log("libSceUsbd: %ld devices", count);
			s_last_count = count;
		}
		if (count <= 0 || !list)
			return false;
		bool taken = false;
		for (long i = 0; i < count && !taken; i++)
		{
			uint8_t dd[64] = {}; // libusb_device_descriptor (18 bytes), with room to spare
			if (s_usbd.GetDeviceDescriptor(list[i], dd) < 0)
				continue;
			const uint16_t vid = static_cast<uint16_t>(dd[8] | (dd[9] << 8)), pid = static_cast<uint16_t>(dd[10] | (dd[11] << 8));
			char key[32];
			snprintf(key, sizeof(key), "usbd %04x:%04x #%ld", vid, pid, i);
			const bool first_time = std::find(seen.begin(), seen.end(), key) == seen.end();
			if (dd[4] == 9 || SystemOwned(vid, pid))
				continue;
			void* h = nullptr;
			if (s_usbd.Open(list[i], &h) < 0 || !h)
			{
				if (first_time)
				{
					Log("libSceUsbd: %04x:%04x: open failed", vid, pid);
					seen.push_back(key);
				}
				continue;
			}
			Device d;
			d.name = key;
			d.usbd = h;
			d.vid = vid;
			d.pid = pid;
			d.product = UsbdString(h, dd[15]);
			unsigned char cfg[1024] = {};
			const int cfg_len = s_usbd.ControlTransfer(h, 0x80, 0x06, 0x0200, 0, cfg, sizeof(cfg), 500);
			d.target = cfg_len > 0 ? FindTarget(cfg, static_cast<size_t>(cfg_len)) : Target();
			if (first_time)
			{
				Log("libSceUsbd: %04x:%04x \"%s\", configuration %d bytes: %s", vid, pid, d.product.c_str(), cfg_len,
					d.target.ok ? "a gamepad interface" : "no gamepad interface");
				seen.push_back(key);
			}
			if (!d.target.ok)
			{
				s_usbd.Close(h);
				continue;
			}
			unsigned char desc[4096] = {};
			int desc_len = 0;
			if (!d.target.xinput)
			{
				const uint16_t want = d.target.report_desc_len ? d.target.report_desc_len : 1024;
				desc_len = s_usbd.ControlTransfer(h, 0x81, 0x06, 0x2200, d.target.iface, desc, std::min<uint16_t>(want, 4096), 500);
			}
			if (!Classify(d, desc, desc_len > 0 ? static_cast<size_t>(desc_len) : 0))
			{
				s_usbd.Close(h);
				continue;
			}
			if (s_usbd.KernelDriverActive && s_usbd.DetachKernelDriver && s_usbd.KernelDriverActive(h, d.target.iface) == 1)
				Log("libSceUsbd: detach kernel driver: %d", s_usbd.DetachKernelDriver(h, d.target.iface));
			if (s_usbd.ClaimInterface)
			{
				const int rc = s_usbd.ClaimInterface(h, d.target.iface);
				if (rc < 0)
					Log("libSceUsbd: claim interface %u: %d", d.target.iface, rc);
			}
			d.route = Route::Usbd;
			if (d.format == Format::Ds3)
				Ds3Enable(d);
			LogTaken(d, desc_len > 0 ? static_cast<size_t>(desc_len) : 0);
			out = d;
			taken = true;
		}
		s_usbd.FreeDeviceList(list, 1);
		return taken;
	}

	// ---- the scan ------------------------------------------------------------------------------------------------------------

	void ListNodes(std::vector<std::string>& uhid, std::vector<std::string>& ugen, bool log)
	{
		std::string all;
		if (DIR* dir = opendir("/dev"))
		{
			while (dirent* e = readdir(dir))
			{
				const std::string n = e->d_name;
				if (n.rfind("uhid", 0) == 0)
					uhid.push_back(n);
				else if (n.rfind("ugen", 0) == 0)
					ugen.push_back(n);
				if (log && (n.find("usb") != std::string::npos || n.find("ugen") != std::string::npos ||
				               n.find("hid") != std::string::npos || n.find("kbd") != std::string::npos))
					all += " " + n;
			}
			closedir(dir);
		}
		else if (log)
			Log("/dev: opendir errno %d", errno);
		std::sort(uhid.begin(), uhid.end());
		std::sort(ugen.begin(), ugen.end());
		if (log)
		{
			Log("/dev's USB nodes:%s", all.empty() ? " none" : all.c_str());
			std::string usb;
			if (DIR* dir = opendir("/dev/usb"))
			{
				while (dirent* e = readdir(dir))
					if (e->d_name[0] != '.')
						usb += std::string(" ") + e->d_name;
				closedir(dir);
			}
			Log("/dev/usb:%s", usb.empty() ? " (none or not readable)" : usb.c_str());
		}
	}

	bool Scan(Device& d, std::vector<std::string>& seen, bool first)
	{
		std::vector<std::string> uhid, ugen;
		ListNodes(uhid, ugen, first);
		for (const std::string& n : uhid)
		{
			const bool first_time = std::find(seen.begin(), seen.end(), n) == seen.end();
			if (!first_time)
				continue; // a uhid node that wasn't a gamepad stays one; a new device gets a new node
			seen.push_back(n);
			if (TryUhid(n, d))
				return true;
		}
		if (!OrbisFlag("nougen"))
		{
			for (const std::string& n : ugen)
			{
				// Logged the first time; looked at again each scan (the node of a re-plugged device keeps its name rarely).
				const bool first_time = std::find(seen.begin(), seen.end(), n) == seen.end();
				if (!first_time)
					continue;
				seen.push_back(n);
				if (TryUgen(n, d))
					return true;
			}
		}
		// libSceUsbd when /dev has no USB nodes at all for this app.
		if (uhid.empty() && ugen.empty())
			return TryUsbd(d, seen);
		return false;
	}

	// ---- reading -------------------------------------------------------------------------------------------------------------

	// One report: >0 its length, 0 nothing in the timeout, -1 the device is gone.
	int ReadReport(Device& d, uint8_t* buf, size_t cap)
	{
		if (d.route == Route::Uhid || d.route == Route::UgenNode)
		{
			pollfd p = {d.ep, POLLIN, 0};
			const int pr = poll(&p, 1, kReadTimeoutMs);
			if (pr == 0)
				return 0;
			if (pr < 0)
				return errno == EINTR ? 0 : -1;
			if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
				return -1;
			const ssize_t n = read(d.ep, buf, cap);
			if (n < 0)
				return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
			return static_cast<int>(n);
		}
		if (d.route == Route::UgenFs)
		{
			d.fs_ptr = d.fs_buf.data();
			d.fs_len = static_cast<uint32_t>(d.fs_buf.size());
			d.fs_ep[0].ppBuffer = &d.fs_ptr;
			d.fs_ep[0].pLength = &d.fs_len;
			d.fs_ep[0].nFrames = 1;
			d.fs_ep[0].aFrames = 0;
			d.fs_ep[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
			d.fs_ep[0].timeout = kReadTimeoutMs;
			d.fs_ep[0].status = 0;
			usb_fs_start st = {};
			st.ep_index = 0;
			if (ioctl(d.ctl, USB_FS_START, &st) != 0)
				return -1;
			for (int tries = 0; tries < 4; tries++)
			{
				pollfd p = {d.ctl, POLLIN | POLLOUT, 0};
				poll(&p, 1, kReadTimeoutMs + 50);
				usb_fs_complete c = {};
				if (ioctl(d.ctl, USB_FS_COMPLETE, &c) == 0)
				{
					if (d.fs_ep[0].status == 0)
					{
						const size_t n = std::min<size_t>(d.fs_len, cap);
						memcpy(buf, d.fs_buf.data(), n);
						return static_cast<int>(n);
					}
					return d.fs_ep[0].status == 20 /* USB_ERR_TIMEOUT */ ? 0 : -1;
				}
				if (errno != EBUSY)
					return -1;
			}
			usb_fs_stop sp = {};
			sp.ep_index = 0;
			ioctl(d.ctl, USB_FS_STOP, &sp);
			return 0;
		}
		if (d.route == Route::Usbd)
		{
			int done = 0;
			const int rc = s_usbd.InterruptTransfer(d.usbd, d.target.ep_in, buf, static_cast<int>(std::min<size_t>(cap, d.target.max_packet ? d.target.max_packet : 64)),
				&done, kReadTimeoutMs);
			if (rc == 0)
				return done;
			return rc == -7 /* LIBUSB_ERROR_TIMEOUT */ ? 0 : -1;
		}
		return -1;
	}

	bool Decode(const Device& d, const uint8_t* r, size_t n, Common& c)
	{
		switch (d.format)
		{
			case Format::Hid: return DecodeHid(d.layout, r, n, c);
			case Format::Ps3Fixed: return DecodePs3Fixed(r, n, c);
			case Format::Ds3: return DecodeDs3(r, n, c);
			case Format::XInput: return DecodeXInput(r, n, c);
			default: return false;
		}
	}

	void* Thread(void*)
	{
		Device d;
		std::vector<std::string> seen;
		bool first = true;
		unsigned reports = 0, changes_logged = 0;
		std::vector<uint8_t> last;
		WhammyCal cal;
		auto since = std::chrono::steady_clock::now();
		for (;;)
		{
			if (d.route == Route::None)
			{
				const bool found = Scan(d, seen, first);
				if (first)
				{
					std::lock_guard<std::mutex> lock(s_scan_lock);
					s_first_scan_done = true;
					s_first_scan_guitar = found && d.guitar;
					s_scan_cv.notify_all();
					if (!found)
						Log("nothing found at the start; looking again every 2 s");
				}
				first = false;
				if (!found)
				{
					// A device plugged in later gets a new node name; nodes already looked at are only looked at again
					// after a while (a device whose driver let go later, a re-plug at the same address).
					static unsigned rounds = 0;
					if (++rounds % 15 == 0)
						seen.clear();
					sleep(2);
					continue;
				}
				reports = 0;
				changes_logged = 0;
				last.clear();
				cal = WhammyCal();
				since = std::chrono::steady_clock::now();
				char msg[160];
				snprintf(msg, sizeof(msg), "%s connected: %s", d.guitar ? "USB guitar" : "USB controller",
					d.product.empty() ? "player 1" : d.product.c_str());
				OrbisNotifyPlain(msg);
			}
			uint8_t buf[1024];
			const int n = ReadReport(d, buf, sizeof(buf));
			if (n < 0)
			{
				Log("%s: read failed (errno %d) after %u reports: the device is gone", d.name.c_str(), errno, reports);
				{
					std::lock_guard<std::mutex> lock(s_lock);
					s_connected = false;
				}
				CloseDevice(d);
				seen.clear();
				OrbisNotifyPlain("USB controller disconnected");
				sleep(1);
				continue;
			}
			if (n == 0)
				continue;
			reports++;
			// The first reports, then each change (the first 200, then every 100th), so a log shows what each press sends.
			const bool changed = last.size() != static_cast<size_t>(n) || memcmp(last.data(), buf, n) != 0;
			const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
			if (reports <= 4 || (changed && (changes_logged < 200 || changes_logged % 100 == 0)))
			{
				Log("report %u at %.2f s (%d bytes): %s", reports, secs, n, Hex(buf, static_cast<size_t>(n)).c_str());
				if (changed)
					changes_logged++;
			}
			if (changed)
				last.assign(buf, buf + n);
			Common c;
			if (!Decode(d, buf, static_cast<size_t>(n), c))
				continue;
			OrbisUsbPadOut o;
			o.guitar = d.guitar;
			if (d.guitar)
			{
				o.strings = ToGuitar(c, d.format, cal);
				o.pad = GuitarAsPad(o.strings);
			}
			else
				o.pad = ToPad(c);
			std::lock_guard<std::mutex> lock(s_lock);
			s_out = o;
			s_connected = true;
		}
		return nullptr;
	}
} // namespace

void OrbisUsbPadStart()
{
	if (s_started.exchange(true))
		return;
	if (OrbisFlag("nousbpad"))
	{
		Log("off (flag nousbpad)");
		std::lock_guard<std::mutex> lock(s_scan_lock);
		s_first_scan_done = true;
		s_scan_cv.notify_all();
		return;
	}
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	const int rc = pthread_create(&t, &attr, Thread, nullptr);
	pthread_attr_destroy(&attr);
	if (rc != 0)
	{
		Log("thread: pthread_create %d", rc);
		std::lock_guard<std::mutex> lock(s_scan_lock);
		s_first_scan_done = true;
		s_scan_cv.notify_all();
	}
}

bool OrbisUsbPadWaitGuitar(int ms)
{
	std::unique_lock<std::mutex> lock(s_scan_lock);
	s_scan_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return s_first_scan_done; });
	return s_first_scan_guitar;
}

bool OrbisUsbPadState(OrbisUsbPadOut& out)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (!s_connected)
		return false;
	out = s_out;
	return true;
}
