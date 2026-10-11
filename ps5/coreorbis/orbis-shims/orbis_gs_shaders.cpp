// PS5 port (vk-285-115, AI-assisted): built-in copies of the GS shader sources (include-orbis/OrbisGSShaders.h).
//
// The files are bin/resources/shaders of the tree the eboot is built from, embedded with .incbin (as the frontend's
// fonts are, fe_ps5.cpp), so they match the renderer's code: the same text the release's resources folder holds.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisGSShaders.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#ifndef ORBIS_GS_SHADER_DIR
#error "ORBIS_GS_SHADER_DIR must name PCSX2's bin/resources/shaders"
#endif

#define ORBIS_SHADER_INCBIN(sym, file) \
	__asm__(".section .rodata." #sym ",\"a\",@progbits\n" \
			".balign 16\n" \
			".global " #sym "\n" #sym ":\n" \
			".incbin \"" ORBIS_GS_SHADER_DIR "/" file "\"\n" \
			".global " #sym "_end\n" #sym "_end:\n" \
			".byte 0\n" \
			".previous\n")

ORBIS_SHADER_INCBIN(orbis_sh_vk_cas, "vulkan/cas.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_vk_convert, "vulkan/convert.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_vk_imgui, "vulkan/imgui.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_vk_interlace, "vulkan/interlace.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_vk_merge, "vulkan/merge.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_vk_present, "vulkan/present.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_vk_shadeboost, "vulkan/shadeboost.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_vk_tfx, "vulkan/tfx.glsl");
ORBIS_SHADER_INCBIN(orbis_sh_common_ffx_a, "common/ffx_a.h");
ORBIS_SHADER_INCBIN(orbis_sh_common_ffx_cas, "common/ffx_cas.h");
ORBIS_SHADER_INCBIN(orbis_sh_common_fxaa, "common/fxaa.fx");

extern "C" const char orbis_sh_vk_cas[], orbis_sh_vk_cas_end[], orbis_sh_vk_convert[], orbis_sh_vk_convert_end[],
	orbis_sh_vk_imgui[], orbis_sh_vk_imgui_end[], orbis_sh_vk_interlace[], orbis_sh_vk_interlace_end[],
	orbis_sh_vk_merge[], orbis_sh_vk_merge_end[], orbis_sh_vk_present[], orbis_sh_vk_present_end[],
	orbis_sh_vk_shadeboost[], orbis_sh_vk_shadeboost_end[], orbis_sh_vk_tfx[], orbis_sh_vk_tfx_end[],
	orbis_sh_common_ffx_a[], orbis_sh_common_ffx_a_end[], orbis_sh_common_ffx_cas[], orbis_sh_common_ffx_cas_end[],
	orbis_sh_common_fxaa[], orbis_sh_common_fxaa_end[];

namespace
{
struct Builtin
{
	const char* name;
	const char* begin;
	const char* end;
};

const Builtin kBuiltins[] = {
	{"shaders/vulkan/cas.glsl", orbis_sh_vk_cas, orbis_sh_vk_cas_end},
	{"shaders/vulkan/convert.glsl", orbis_sh_vk_convert, orbis_sh_vk_convert_end},
	{"shaders/vulkan/imgui.glsl", orbis_sh_vk_imgui, orbis_sh_vk_imgui_end},
	{"shaders/vulkan/interlace.glsl", orbis_sh_vk_interlace, orbis_sh_vk_interlace_end},
	{"shaders/vulkan/merge.glsl", orbis_sh_vk_merge, orbis_sh_vk_merge_end},
	{"shaders/vulkan/present.glsl", orbis_sh_vk_present, orbis_sh_vk_present_end},
	{"shaders/vulkan/shadeboost.glsl", orbis_sh_vk_shadeboost, orbis_sh_vk_shadeboost_end},
	{"shaders/vulkan/tfx.glsl", orbis_sh_vk_tfx, orbis_sh_vk_tfx_end},
	{"shaders/common/ffx_a.h", orbis_sh_common_ffx_a, orbis_sh_common_ffx_a_end},
	{"shaders/common/ffx_cas.h", orbis_sh_common_ffx_cas, orbis_sh_common_ffx_cas_end},
	{"shaders/common/fxaa.fx", orbis_sh_common_fxaa, orbis_sh_common_fxaa_end},
};

bool ReadFile(const std::string& path, std::string* out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out->clear();
	char buf[16384];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, n);
	const bool ok = !std::ferror(f);
	std::fclose(f);
	return ok;
}
} // namespace

bool OrbisBuiltinShaderSource(const char* filename, std::string* out)
{
	for (const Builtin& b : kBuiltins)
	{
		if (std::strcmp(b.name, filename) == 0)
		{
			out->assign(b.begin, static_cast<size_t>(b.end - b.begin));
			return true;
		}
	}
	return false;
}

// vk-285-134 (AI-assisted): the eboot's copy first. A folder copy from another release doesn't match this build's renderer:
// build 130's logs had a console whose every game failed with "Failed to initialize GS" ("Missing entry point" compiling a
// utility shader: 17 starts), its /data/PCSX2/resources holding older shader files. Only shadeboost.glsl, the present's
// sharpening that is there to be edited, still comes from the folder first; a file the eboot doesn't carry comes from the
// folder as before.
bool OrbisReadShaderSource(const std::string& resources_dir, const char* filename, std::string* out)
{
	const std::string path = resources_dir + "/" + filename;
	const bool editable = std::strcmp(filename, "shaders/vulkan/shadeboost.glsl") == 0;
	if (editable && ReadFile(path, out))
		return true;
	std::string builtin;
	if (!OrbisBuiltinShaderSource(filename, &builtin))
		return ReadFile(path, out);
	std::string folder;
	const bool have_folder = !editable && ReadFile(path, &folder);
	static std::mutex s_mutex;
	static std::set<std::string> s_noted;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_noted.insert(filename).second)
		{
			if (!have_folder)
				std::printf("[gs] %s isn't in %s: the eboot's built-in copy (%zu bytes)\n", filename, resources_dir.c_str(),
					builtin.size());
			else if (folder != builtin)
				std::printf("[gs] %s in %s differs from this build's (%zu bytes, the build's %zu): the build's own is used (a copy "
							"from another release breaks the GS)\n",
					filename, resources_dir.c_str(), folder.size(), builtin.size());
			std::fflush(stdout);
		}
	}
	*out = std::move(builtin);
	return true;
}
