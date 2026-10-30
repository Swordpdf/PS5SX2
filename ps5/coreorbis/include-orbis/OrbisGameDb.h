// PS5 port (gamedb, 2026-10-30, AI-assisted): PCSX2's game database (resources/GameIndex.yaml) when the resources folder lacks it.
//
// PCSX2 reads GameIndex.yaml from EmuFolders::Resources (/data/PCSX2/resources). A console set up without that folder (the app
// from a store package, or the eboot copied over by hand) ran every game without the database: "GameDB: Unable to open GameDB
// file, file does not exist." and no game's own fixes. The Discord #bugs scan of 2026-10-30: Black corrupted on a PS5 Pro
// (GameIndex: vuClampMode 3), NFS Carbon hanging after the intro (eeClampMode 3), Burnout Revenge's vertical lines
// (halfPixelOffset 2); two of the three had no resources folder at all. The eboot now carries the GameIndex.yaml of the tree it is
// built from (orbis-shims/orbis_gamedb.cpp, .incbin, as the GS shaders are: OrbisGSShaders.h); the folder's copy still comes
// first (it is there to be edited), the eboot's when that one is missing, unreadable or isn't the database.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>

// The eboot's own GameIndex.yaml (its text, not NUL-terminated in `size`); size 0 when the build carries none.
void OrbisBuiltinGameDb(const char** data, size_t* size);

// Whether `text` looks like PCSX2's game database, not a cut-off copy or another file: big enough (a real one is 2.6 MB; a cut
// copy fails PCSX2's parse anyway, this only refuses the plainly wrong) and with serial entries in it.
bool OrbisGameDbTextUsable(const std::string& text);

// Where the GameDB text came from.
enum class OrbisGameDbSource
{
	None,    // neither the folder nor the eboot has one
	Folder,  // <resources>/GameIndex.yaml
	Builtin, // the eboot's copy (the folder's is missing, unreadable or not the database)
};

// GameIndex.yaml's text: <resources_dir>/GameIndex.yaml when it is readable and usable, else the eboot's copy. `problem` (may be
// null) says what was wrong with the folder's ("" when it was used). The result is also remembered for OrbisGameDbStatus.
OrbisGameDbSource OrbisReadGameDb(const std::string& resources_dir, std::string* out, std::string* problem);

// The eboot's copy, after the folder's failed PCSX2's own parse (GameDatabase.cpp); false when the build carries none.
bool OrbisReadBuiltinGameDb(std::string* out);
void OrbisGameDbNoteFolderUnparsable(const std::string& resources_dir);

// What the last read found, for the shelf's note and boot.log: whether the folder's copy is missing or unusable (the eboot's is
// used), and in a sentence why. Without any read yet it looks at the folder now (the shelf comes up before the first game).
bool OrbisGameDbFolderProblem(const std::string& resources_dir, std::string* why);
