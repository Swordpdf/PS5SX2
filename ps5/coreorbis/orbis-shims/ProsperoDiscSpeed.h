// PS5SX2 (vk-285-150/151, AI-assisted): reading the PS5's disc drive faster than the 2x /dev/cd0 gives a PS2 DVD. See
// ProsperoDiscSpeed.cpp and OrbisDiscScsi.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

// How the disc copy reads the drive: pread on /dev/cd0 (cmd_bytes 0), or READ commands of cmd_bytes through the drive's
// pass device, on one or two lanes (each with its own pass descriptor and thread).
struct OrbisDiscReadMethod
{
	int cd = -1;             // /dev/cd0, the caller's (never closed here)
	int pass[2] = {-1, -1};  // the pass device, one per lane
	uint32_t cmd_bytes = 0;  // 0: pread on cd
	bool read12 = false;     // READ(12) instead of READ(10)
	bool streaming = false;  // READ(12)'s Streaming bit
	int lanes = 1;
	std::string name = "pread on /dev/cd0";
	// vk-285-152: the drive speed set with Sony's command (rotation, speed); speed 0: left as it was
	int set_rotation = 0;
	uint16_t set_speed = 0;
};

// Finds the drive's pass device through /dev/cd0 (CAMGETPASSTHRU) and logs what the drive says about its speeds. Then
// (vk-285-152) times pread on out->cd as the drive is and at each of Sony's faster speed settings (SceShellCore's DB
// command: vk-285-154, 8.0 rotation 1 then 3.2 rotation 0), 16 MiB each, and leaves the drive at the fastest if it beats the drive as it
// was by 15% (out->set_rotation/set_speed; OrbisDiscSpeedRestore puts it back to 2.0), else back at 2.0. Flag
// disc_passbench: also 151's pass read methods. Logs every step ([discspeed] lines). Flag nodiscspeed: nothing is sent.
void OrbisDiscSpeedUp(const char* cd_path, uint64_t bytes, OrbisDiscReadMethod* out);

// Reads like pread with the method (a pass method falls back to pread on cd for a range it can't read).
ssize_t OrbisDiscRead(const OrbisDiscReadMethod& m, uint64_t off, void* buf, size_t len);

// vk-285-152: puts the drive back to its default speed (Sony's command, 2.0x, rotation 0) when the method raised it.
void OrbisDiscSpeedRestore(const char* cd_path, OrbisDiscReadMethod& m);

// Closes the method's pass descriptors.
void OrbisDiscReadMethodClose(OrbisDiscReadMethod& m);

// Times 256 MB at the start of the disc and 256 MB at its end with the method, logs and shows the speeds (flag
// disc_readtest, for a disc already copied).
void OrbisDiscReadTest(const OrbisDiscReadMethod& m, uint64_t bytes);

// 2026-10-10: a PS2 DVD played from the drive (flag discplay). `cd` is an open /dev/cd0. The layer break comes from
// READ DVD STRUCTURE (PCSX2's dual-layer arithmetic). The sector count is that layout, or the drive's media size when
// the media size is larger. Nothing is written to the disc.
struct OrbisDiscLayer
{
	bool ok = false;
	int media_type = 0; // 0 single, 1 parallel dual, 2 opposite dual
	uint32_t sectors = 0;
	uint32_t layer_break = 0;
	std::string why;
};
bool OrbisDiscReadLayer(int cd, OrbisDiscLayer* out);

// Sony's speed command that this drive takes for a PS2 DVD: rotation 0, 0x32 (3.2x). `restore` puts it back to 2.0
// (rotation 0, 0x20). Flag nodiscspeed sends nothing. False only when the pass device can't be opened.
bool OrbisDiscSetPlaySpeed(int cd, bool restore);

// A USB optical drive on this console often never gets a cd device (the cd driver fails to attach), so it is only
// a pass device. These talk to that fd with plain MMC. Sony's vendor speed command is not sent here. Needs proper
// testing on a console with a USB DVD drive.
struct OrbisPassInfo
{
	bool optical = false; // INQUIRY peripheral type 5
	bool sony = false;    // the console's own drive ("PS-SYSTEM")
	std::string text;
};
struct OrbisCdTrack
{
	uint32_t lba = 0;
	uint8_t number = 0;
	uint8_t adr = 0;
	uint8_t control = 0;
};
struct OrbisCdToc
{
	bool ok = false;
	uint32_t leadout = 0;
	uint32_t count = 0;
	OrbisCdTrack tracks[99] = {};
};

// libc open, then sceKernelOpen. The title's libc opendir/open of /dev is EPERM; the kernel call is what lists it.
// `kernel_err` receives the sceKernelOpen code when both fail (0 when libc failed and the kernel call was not made).
int OrbisOpenDev(const char* path, int flags, int* kernel_err = nullptr);
bool OrbisPassDescribe(int pass, OrbisPassInfo* out);
// The pass device for an open cd node (/dev/cd1 -> /dev/pass1), or empty.
std::string OrbisDiscPassPath(int cd);
void OrbisPassReleaseQueue(int pass);
bool OrbisPassReady(int pass);
uint32_t OrbisPassCapacity(int pass); // 2048-byte sectors, or 0
bool OrbisPassRead2048(int pass, uint32_t lba, uint32_t count, void* buf);
bool OrbisPassReadCdUser(int pass, uint32_t lba, uint32_t count, void* buf); // READ CD, 2048 user bytes (a PS2 CD)
bool OrbisPassRead2352(int pass, uint32_t lba, uint32_t count, void* buf);
bool OrbisPassReadCdToc(int pass, OrbisCdToc* out);
bool OrbisPassSetSpeed(int pass, uint16_t read_kbs); // SET CD SPEED; 0xFFFF is the drive's own choice
bool OrbisDiscReadLayerPass(int pass, OrbisDiscLayer* out); // `pass` is already the pass device (not closed)

// The game's /dev has no pass node (a payload sees it; this process does not). "/dev/usbdisc" is the drive reached
// through libSceUsbd's bulk-only mass storage, not a file. Needs proper testing on a console with a USB DVD drive.
inline bool OrbisUsbDiscPath(const std::string& path)
{
	return path == "/dev/usbdisc";
}
// Keeps the device open on success. `why` is a short line for the notification when it fails.
bool OrbisUsbDiscFind(std::string* what, std::string* why);
bool OrbisUsbDiscReady();
uint32_t OrbisUsbDiscCapacity();
bool OrbisUsbDiscRead2048(uint32_t lba, uint32_t count, void* buf);
bool OrbisUsbDiscReadCdUser(uint32_t lba, uint32_t count, void* buf);
bool OrbisUsbDiscRead2352(uint32_t lba, uint32_t count, void* buf);
bool OrbisUsbDiscReadToc(OrbisCdToc* out);
bool OrbisUsbDiscSetSpeed(uint16_t read_kbs);
bool OrbisUsbDiscReadLayer(OrbisDiscLayer* out);
// Gives the drive back to the shell and allows the tray to open. A mass-storage reset leaves the tray locked.
void OrbisUsbDiscRelease();
// The ugen session is still open. A failed read is not the disc being taken out.
bool OrbisUsbDiscSession();
