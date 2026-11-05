// PS5 port frontend, PC harness (2026-10-16, AI-assisted): which folders the shelf lists games from (fe_roots.h). Builds a
// fake /data/PCSX2 and /mnt tree and checks what ScanGames lists from the roots ResolveGameRoots gives: the usual places
// when no games folder is set (the same list the app always built), only the set folders when one is (and the NFS
// shares), the Scan USB drives and Scan subfolders switches, and the cover list's header (usb-games.txt).
//
//   roots_test [<scratch folder>]
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_games.h"
#include "fe_roots.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>

static int fails = 0;
static void Check(bool ok, const std::string& w)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", w.c_str());
	fails += !ok;
}

static void Touch(const std::string& path)
{
	const size_t slash = path.rfind('/');
	std::system(("mkdir -p '" + path.substr(0, slash) + "'").c_str());
	std::ofstream(path) << "x";
}

// what the shelf lists from these roots: the file names
static std::string Names(const std::vector<std::string>& dirs, bool subfolders = true)
{
	std::set<std::string> names;
	for (const auto& g : fe::ScanGames(dirs, subfolders))
		names.insert(g.file);
	std::string s;
	for (const auto& n : names)
		s += (s.empty() ? "" : ", ") + n;
	return s;
}

static std::string Names(const fe::GameRoots& r) { return Names(r.dirs, r.scan_subfolders); }

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : "/tmp/roots-test";
	std::system(("rm -rf '" + root + "' && mkdir -p '" + root + "'").c_str());
	const std::string data = root + "/data/PCSX2", mnt = root + "/mnt";
	Touch(data + "/games/A.iso");
	Touch(data + "/games/Sub/C.chd");
	Touch(data + "/Top.iso");            // an old setup keeps images in the top folder
	Touch(data + "/misc/B.iso");         // a folder of /data/PCSX2
	Touch(data + "/textures/Skip.iso");  // PCSX2's own folder: skipped
	Touch(mnt + "/usb0/D.iso");
	Touch(mnt + "/usb0/PS2/E.iso");
	Touch(mnt + "/usb0/Other/F.iso");    // a drive root's first-level folder
	Touch(mnt + "/usb1/PCSX2/games/G.iso");
	Touch(mnt + "/ext0/H.iso");
	Touch(root + "/custom/I.iso");
	Touch(root + "/custom/Sub/J.iso");
	Touch(root + "/custom/Sub/Deep/K.iso"); // two folders down: not listed
	Touch(root + "/nfs/nas/PS2/L.iso");

	// What main-boot.cpp / fe_ps5.cpp built before this: {games_dir, top_dir} + the drives + the shares.
	const std::vector<std::string> top = {data + "/games", data};
	const std::vector<std::string> drives = {mnt + "/usb0", mnt + "/usb0/PS2", mnt + "/usb1", mnt + "/usb1/PCSX2/games", mnt + "/ext0"};
	const std::vector<std::string> today_dirs = [&] {
		std::vector<std::string> d = top;
		d.insert(d.end(), drives.begin(), drives.end());
		return d;
	}();
	const std::string today = Names(today_dirs);
	std::printf("today (unset): %s\n", today.c_str());

	// 1. unset: exactly today's list.
	{
		const fe::GameRoots r = fe::ResolveGameRoots({}, top, drives, {});
		Check(!r.only_set && r.dirs == today_dirs && Names(r) == today, "unset: the default places, the same list as before");
		Check(r.signature == "default" && r.scan_usb && r.scan_subfolders, "unset: signature is default");
		const fe::GameRoots w = fe::ResolveGameRoots({}, top, drives, {root + "/nfs/nas/PS2"});
		Check(Names(w) == today + ", L.iso" || Names(w).find("L.iso") != std::string::npos, "unset + a share: the share's games too");
		Check(w.dirs.size() == today_dirs.size() + 1 && w.dirs.back() == root + "/nfs/nas/PS2", "unset + a share: the share comes last, as before");
	}
	// 2. a folder set: only it, and its folders one level down.
	{
		const fe::GameRoots r = fe::ResolveGameRoots({root + "/custom"}, top, {mnt + "/usb0"}, {});
		Check(r.only_set && r.dirs.size() == 1, "set: one root");
		Check(Names(r) == "I.iso, J.iso", "set /custom: only I and J (K is two folders down)");
		Check(r.signature == "set:" + root + "/custom", "set: signature names the root");
		Check(!r.scan_usb, "set: the drives are not looked through");
	}
	// 3. the default games folder set on purpose: not /data/PCSX2's other folders, not the drives.
	{
		const fe::GameRoots r = fe::ResolveGameRoots({data + "/games"}, top, {mnt + "/usb0"}, {});
		Check(Names(r) == "A.iso, C.chd", "set games/: A and C only (not Top, B, D..H)");
	}
	// 4. a drive folder set: not the drive's other folders, not /data/PCSX2/games.
	{
		const fe::GameRoots r = fe::ResolveGameRoots({mnt + "/usb0/PS2"}, top, {}, {});
		Check(Names(r) == "E.iso", "set usb0/PS2: E only");
	}
	// 5. two folders + a share.
	{
		const fe::GameRoots r = fe::ResolveGameRoots({root + "/custom", mnt + "/ext0"}, {}, {}, {root + "/nfs/nas/PS2"});
		Check(Names(r) == "H.iso, I.iso, J.iso, L.iso", "two folders and a share: theirs, and the share's");
	}
	// 6. a folder that is not there: nothing from the defaults; named as missing.
	{
		const fe::GameRoots r = fe::ResolveGameRoots({root + "/gone"}, top, {mnt + "/usb0"}, {});
		Check(r.only_set && r.dirs.empty() && r.missing.size() == 1 && Names(r).empty(), "set but missing: no games, no fallback to the defaults");
	}
	// 7. one there, one missing.
	{
		const fe::GameRoots r = fe::ResolveGameRoots({root + "/gone", root + "/custom"}, {data}, {}, {});
		Check(Names(r) == "I.iso, J.iso" && r.missing.size() == 1, "one missing, one there: the one there");
	}
	// 7b. where a copied disc goes
	{
		Check(fe::ResolveGameRoots({}, {data + "/games"}, {}, {}).copy_to.empty(), "unset: a copy goes to games/ as before (copy_to empty)");
		Check(fe::ResolveGameRoots({root + "/custom", mnt + "/ext0"}, {}, {}, {}).copy_to == root + "/custom", "set: a copy goes to the first folder");
		Check(fe::ResolveGameRoots({root + "/gone", mnt + "/ext0"}, {}, {}, {}).copy_to == mnt + "/ext0", "set: the first folder that is there");
		Check(fe::ResolveGameRoots({root + "/gone"}, {}, {}, {}).copy_to.empty(), "set but missing: copy_to empty (the caller says so)");
		Check(fe::UnderNfs("/nfs") && fe::UnderNfs("/nfs/nas/PS2") && !fe::UnderNfs("/nfsx/a") && !fe::UnderNfs(data) && !fe::UnderNfs(mnt + "/usb0/nfs"),
			"a folder under /nfs is a share: no copy target");
		Check(fe::UnderSmb("/smb") && fe::UnderSmb("/smb/nas/PS2") && !fe::UnderSmb("/smbx/a") && !fe::UnderSmb(data),
			"a folder under /smb is a share: no copy target");
	}
	// 7c. entries that cannot be a folder: relative, the root '/', empty
	{
		const fe::GameRoots r = fe::ResolveGameRoots({"relative/dir", "/", root + "/custom"}, top, drives, {});
		Check(r.only_set && r.missing.size() == 2 && r.missing[0] == "relative/dir" && r.missing[1] == "/" && Names(r) == "I.iso, J.iso",
			"a relative entry and '/' land in missing; the folder next to them is listed");
		const fe::GameRoots all_bad = fe::ResolveGameRoots({"/"}, top, drives, {});
		Check(all_bad.only_set && all_bad.dirs.empty() && Names(all_bad).empty(), "only '/': set, nothing listed, no fallback");
	}
	// 8. the cover prefetch's usb list is keyed to the roots.
	{
		const std::string sig_a = fe::ResolveGameRoots({root + "/custom"}, {}, {}, {}).signature;
		const std::string sig_b = fe::ResolveGameRoots({mnt + "/ext0"}, {}, {}, {}).signature;
		Check(fe::ResolveGameRoots({root + "/gone"}, {}, {}, {}).signature == "set:" + root + "/gone" &&
				fe::ResolveGameRoots({root + "/custom"}, {}, {}, {root + "/nfs/nas/PS2"}).signature == sig_a,
			"signature: what was asked for (a folder not mounted yet counts), the shares left out (they are not there before the jailbreak)");
		const std::string text = fe::UsbListHeader(sig_a) + "SLUS-20001\tA\tA\n";
		Check(fe::UsbListMatches(text, sig_a) && !fe::UsbListMatches(text, sig_b) && !fe::UsbListMatches("SLUS-20001\tA\tA\n", sig_a),
			"usb-games.txt: used for the roots it was written for only (an old file has no header: not used once a folder is set)");
		Check(fe::UsbListMatches("SLUS-20001\tA\tA\n", "default") && !fe::UsbListMatches(fe::UsbListHeader(sig_a) + "x\ty\tz\n", "default"),
			"usb-games.txt: an old file (no header) still serves the default places; a list written for a set folder does not");
	}
	// 9. Scan USB drives off (PS5SX2/ScanUsb): the usual places without the drives, even with no games folder set.
	{
		fe::RootOptions no_usb;
		no_usb.scan_usb = false;
		const fe::GameRoots r = fe::ResolveGameRoots({}, top, drives, {}, no_usb);
		Check(!r.only_set && r.dirs == top && Names(r) == "A.iso, B.iso, C.chd, Top.iso",
			"no USB scan, nothing set: games/ and /data/PCSX2 (and their folders) only, no drive");
		Check(!r.scan_usb && r.signature == "default-nousb", "no USB scan: signature says so (an old usb-games.txt is not used)");
		Check(!fe::UsbListMatches("SLUS-20001\tA\tA\n", r.signature), "no USB scan: a header-less usb-games.txt (written with the drives) is not used");
		const fe::GameRoots w = fe::ResolveGameRoots({}, top, drives, {root + "/nfs/nas/PS2"}, no_usb);
		Check(w.dirs.size() == 3 && Names(w).find("L.iso") != std::string::npos && Names(w).find("D.iso") == std::string::npos,
			"no USB scan + a share: the share is still listed, the drives are not");
		// a folder the player set on a drive is theirs: listed whatever the switch says
		const fe::GameRoots s = fe::ResolveGameRoots({mnt + "/usb0/PS2"}, top, drives, {}, no_usb);
		Check(Names(s) == "E.iso", "no USB scan, a folder set on a drive: that folder is listed (the player named it)");
		// the switch on is today's list
		fe::RootOptions on;
		Check(fe::ResolveGameRoots({}, top, drives, {}, on).dirs == today_dirs, "USB scan on: today's list");
	}
	// 10. Scan subfolders off (PS5SX2/ScanSubfolders): only the files directly in each folder.
	{
		fe::RootOptions flat;
		flat.scan_subfolders = false;
		const fe::GameRoots r = fe::ResolveGameRoots({}, top, drives, {}, flat);
		Check(r.dirs == today_dirs && !r.scan_subfolders && Names(r) == "A.iso, D.iso, E.iso, G.iso, H.iso, Top.iso",
			"no subfolders, nothing set: the same roots, only the files directly in them (not B, C, F)");
		Check(r.signature == "default flat", "no subfolders: signature says so");
		const fe::GameRoots c = fe::ResolveGameRoots({root + "/custom"}, top, drives, {}, flat);
		Check(Names(c) == "I.iso", "no subfolders, /custom set: I only (not J in Sub)");
		const fe::GameRoots cs = fe::ResolveGameRoots({root + "/custom"}, top, drives, {}, fe::RootOptions());
		Check(Names(cs) == "I.iso, J.iso" && cs.scan_subfolders, "subfolders on: I and J as before");
		fe::RootOptions both;
		both.scan_usb = false;
		both.scan_subfolders = false;
		const fe::GameRoots b = fe::ResolveGameRoots({}, top, drives, {}, both);
		Check(Names(b) == "A.iso, Top.iso" && b.signature == "default-nousb flat", "no USB scan and no subfolders: A and Top only");
		// ScanGames' own flag: the default is the old behaviour
		Check(Names(today_dirs, true) == today && fe::ScanGames(today_dirs).size() == fe::ScanGames(today_dirs, true).size(),
			"ScanGames with no flag lists the folders' folders (as before)");
	}
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
