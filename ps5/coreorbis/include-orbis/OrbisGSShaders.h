// PS5 port (vk-285-115): built-in copies of the shader sources the GS reads from resources/shaders.
//
// PCSX2 reads its Vulkan shaders as text from EmuFolders::Resources (/data/PCSX2/resources) when the GS device is
// made. A console without that folder (the eboot copied over by hand) failed every game with "Failed to initialize
// GS." (1.50's logs: 77 starts on 7 consoles, "Failed to read shaders/vulkan/tfx.glsl."). The eboot now carries the
// same files from bin/resources/shaders (orbis-shims/orbis_gs_shaders.cpp); the folder's own files still come first.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

// The built-in copy of resources/<filename> ("shaders/vulkan/tfx.glsl"); false for a name it doesn't carry.
bool OrbisBuiltinShaderSource(const char* filename, std::string* out);

// The built-in copy of resources/<filename>, else the folder's (vk-285-134: the eboot's first, a boot.log line when the
// folder holds a different copy; shadeboost.glsl, meant to be edited, the folder's first); false when neither has it.
bool OrbisReadShaderSource(const std::string& resources_dir, const char* filename, std::string* out);
