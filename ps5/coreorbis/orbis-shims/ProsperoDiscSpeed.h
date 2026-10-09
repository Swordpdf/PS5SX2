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
};

// Finds the drive's pass device through /dev/cd0 (CAMGETPASSTHRU), logs what the drive says about its speeds, asks for
// more (SET STREAMING; the PS5's drive answered "invalid command" in 150), then times read methods on 16 MiB each and
// leaves the fastest in *out (pread on out->cd unless a pass method beats it by 15% and reads the same bytes). Logs
// every step ([discspeed] lines). Flag nodiscspeed: nothing is sent; *out stays pread.
void OrbisDiscSpeedUp(const char* cd_path, uint64_t bytes, OrbisDiscReadMethod* out);

// Reads like pread with the method (a pass method falls back to pread on cd for a range it can't read).
ssize_t OrbisDiscRead(const OrbisDiscReadMethod& m, uint64_t off, void* buf, size_t len);

// Closes the method's pass descriptors.
void OrbisDiscReadMethodClose(OrbisDiscReadMethod& m);

// Times 256 MB at the start of the disc and 256 MB at its end with the method, logs and shows the speeds (flag
// disc_readtest, for a disc already copied).
void OrbisDiscReadTest(const OrbisDiscReadMethod& m, uint64_t bytes);
