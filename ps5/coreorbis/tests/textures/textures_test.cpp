// Host test for orbis-shims/OrbisTextureRoots.h: builds a tree of fake USB drives in a temp folder and checks which
// game folder FindGameDir picks. Run: g++ -std=c++17 -I ps5/coreorbis/orbis-shims textures_test.cpp && ./a.out
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisTextureRoots.h"

#include <cstdlib>
#include <unistd.h>

static int g_fails = 0;

static void Check(bool ok, const std::string& what)
{
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok)
		g_fails++;
}

static void MkDirs(const std::string& path)
{
	std::string cur;
	size_t pos = 0;
	while (pos <= path.size())
	{
		const size_t slash = path.find('/', pos);
		const std::string part = path.substr(0, slash == std::string::npos ? path.size() : slash);
		if (!part.empty())
			mkdir(part.c_str(), 0777);
		if (slash == std::string::npos)
			break;
		pos = slash + 1;
	}
}

int main()
{
	char tmpl[] = "/tmp/texroots-XXXXXX";
	const std::string root = mkdtemp(tmpl);
	const std::string mnt = root + "/mnt";
	// usb0: a drive with the pack of SLUS-20265 under PS5SX2/textures, and SLUS-20072 under textures (no PS5SX2)
	MkDirs(mnt + "/usb0/PS5SX2/textures/SLUS-20265/replacements");
	MkDirs(mnt + "/usb0/textures/SLUS-20072/replacements");
	// usb1: names in other letter cases
	MkDirs(mnt + "/usb1/ps5sx2/Textures/slus-21000/Replacements");
	// usb2: PCSX2's own layout, and a pack copied to the drive's top as it is
	MkDirs(mnt + "/usb2/PCSX2/textures/SLES-52432/replacements");
	MkDirs(mnt + "/usb2/SCUS-97328/replacements");
	MkDirs(mnt + "/usb2/PS5SX2/SLPS-25000/replacements");
	// usb3: a drive with a textures folder that has other games only, and a folder of the serial without a replacements folder
	MkDirs(mnt + "/usb3/textures/SLUS-99999/replacements");
	MkDirs(mnt + "/usb3/SLUS-11111/notes");
	// usb4: empty; usb5: not there
	MkDirs(mnt + "/usb4");
	// a manual folder
	MkDirs(root + "/manual/SLUS-20265/replacements");
	MkDirs(root + "/manual2/SLES-50000");

	const std::vector<std::string> drives = OrbisTextures::UsbRoots(mnt);
	Check(drives.size() == 3 || drives.size() == 4, "the drives with anything on them: " + std::to_string(drives.size()));
	Check(!drives.empty() && drives[0] == mnt + "/usb0", "the first is usb0");
	bool has_usb4 = false;
	for (const std::string& d : drives)
		has_usb4 = has_usb4 || d == mnt + "/usb4";
	Check(!has_usb4, "an empty drive slot is not a drive");

	std::string how;
	Check(OrbisTextures::FindGameDir(drives, "", "SLUS-20265", &how) == mnt + "/usb0/PS5SX2/textures/SLUS-20265", "PS5SX2/textures on a drive: " + how);
	Check(OrbisTextures::FindGameDir(drives, "", "SLUS-20072", &how) == mnt + "/usb0/textures/SLUS-20072", "textures at the drive's top: " + how);
	Check(OrbisTextures::FindGameDir(drives, "", "SLUS-21000", &how) == mnt + "/usb1/ps5sx2/Textures/slus-21000", "names in other cases, path as on the drive: " + how);
	Check(OrbisTextures::FindGameDir(drives, "", "SLES-52432", &how) == mnt + "/usb2/PCSX2/textures/SLES-52432", "PCSX2/textures: " + how);
	Check(OrbisTextures::FindGameDir(drives, "", "SCUS-97328", &how) == mnt + "/usb2/SCUS-97328", "a pack copied to the drive's top: " + how);
	Check(OrbisTextures::FindGameDir(drives, "", "SLPS-25000", &how) == mnt + "/usb2/PS5SX2/SLPS-25000", "a pack in PS5SX2/: " + how);
	Check(OrbisTextures::FindGameDir(drives, "", "SLUS-11111", &how).empty(), "a serial folder without a replacements folder is no pack");
	Check(OrbisTextures::FindGameDir(drives, "", "SLUS-00000", &how).empty(), "a game with no pack");
	Check(OrbisTextures::FindGameDir(drives, "", "", &how).empty(), "no serial");
	Check(OrbisTextures::FindGameDir(drives, root + "/manual", "SLUS-20265", &how) == root + "/manual/SLUS-20265", "the setting wins over a drive: " + how);
	Check(OrbisTextures::FindGameDir(drives, root + "/manual", "SLUS-20072", &how) == mnt + "/usb0/textures/SLUS-20072", "the setting without this game: the drives are still looked at");
	Check(OrbisTextures::FindGameDir({}, root + "/manual2", "SLES-50000", &how) == root + "/manual2/SLES-50000", "the setting alone, no drives");
	Check(OrbisTextures::FindGameDir({}, "", "SLUS-20265", &how).empty(), "no drives, no setting");
	Check(OrbisTextures::FindGameDir({mnt + "/usb9"}, "/nonexistent/folder", "SLUS-20265", &how).empty(), "folders that aren't there");

	std::string cmd = "rm -rf '" + root + "'";
	if (g_fails == 0)
		(void)!system(cmd.c_str());
	std::printf(g_fails ? "%d FAILED (tree kept in %s)\n" : "all passed\n", g_fails, root.c_str());
	return g_fails ? 1 : 0;
}
