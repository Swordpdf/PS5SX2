// PS5 port: the part of shaderc's C API that PCSX2's Vulkan renderer calls
// (GS/Renderers/Vulkan/VKShaderCache.cpp, dyn_shaderc), implemented statically
// over glslang's C interface. A PS5 title cannot load shaderc_shared at run time,
// and the port links glslang (PCSX2's own pin, 275822a / 16.3.0) instead.
//
// What differs from shaderc: no SPIRV-Tools, so optimization levels are accepted
// and ignored (the driver's compiler, ACO, optimizes the SPIR-V it receives), and
// no #include callbacks (PCSX2's Vulkan shaders include nothing). GLSL in, SPIR-V
// 1.0 for Vulkan 1.0 out -- the target PCSX2 asks for (shaderc_target_env_vulkan,
// version 0).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include <shaderc/shaderc.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

struct shaderc_compiler
{
	int initialized;
};

struct shaderc_compile_options
{
	bool generate_debug_info = false;
	shaderc_source_language language = shaderc_source_language_glsl;
	shaderc_target_env target_env = shaderc_target_env_vulkan;
	uint32_t target_version = 0;
};

struct shaderc_compilation_result
{
	shaderc_compilation_status status = shaderc_compilation_status_null_result_object;
	std::vector<char> bytes;
	std::string messages;
	size_t warnings = 0;
	size_t errors = 0;
};

namespace
{
	std::mutex s_glslang_lock;
	int s_glslang_users = 0;

	bool StageFor(shaderc_shader_kind kind, glslang_stage_t* stage)
	{
		switch (kind)
		{
			case shaderc_vertex_shader: *stage = GLSLANG_STAGE_VERTEX; return true;
			case shaderc_fragment_shader: *stage = GLSLANG_STAGE_FRAGMENT; return true;
			case shaderc_compute_shader: *stage = GLSLANG_STAGE_COMPUTE; return true;
			case shaderc_geometry_shader: *stage = GLSLANG_STAGE_GEOMETRY; return true;
			case shaderc_tess_control_shader: *stage = GLSLANG_STAGE_TESSCONTROL; return true;
			case shaderc_tess_evaluation_shader: *stage = GLSLANG_STAGE_TESSEVALUATION; return true;
			default: return false;
		}
	}

	// A log's lines that glslang marks "WARNING:" (shaderc counts them the same way).
	size_t CountPrefixed(const std::string& log, const char* prefix)
	{
		size_t count = 0;
		const size_t length = std::strlen(prefix);
		for (size_t at = 0; at < log.size();)
		{
			if (log.compare(at, length, prefix) == 0)
				count++;
			const size_t end = log.find('\n', at);
			if (end == std::string::npos)
				break;
			at = end + 1;
		}
		return count;
	}

	void Append(std::string* messages, const char* log)
	{
		if (log && *log)
			messages->append(log);
	}
} // namespace

extern "C" {

shaderc_compiler_t shaderc_compiler_initialize(void)
{
	std::lock_guard<std::mutex> guard(s_glslang_lock);
	if (s_glslang_users == 0 && !glslang_initialize_process())
		return nullptr;
	s_glslang_users++;
	return new shaderc_compiler{1};
}

void shaderc_compiler_release(shaderc_compiler_t compiler)
{
	if (!compiler)
		return;
	delete compiler;
	std::lock_guard<std::mutex> guard(s_glslang_lock);
	if (--s_glslang_users == 0)
		glslang_finalize_process();
}

shaderc_compile_options_t shaderc_compile_options_initialize(void)
{
	return new shaderc_compile_options();
}

void shaderc_compile_options_release(shaderc_compile_options_t options)
{
	delete options;
}

void shaderc_compile_options_set_source_language(shaderc_compile_options_t options, shaderc_source_language lang)
{
	if (options)
		options->language = lang;
}

void shaderc_compile_options_set_generate_debug_info(shaderc_compile_options_t options)
{
	if (options)
		options->generate_debug_info = true;
}

void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t options, shaderc_optimization_level level)
{
	(void)options;
	(void)level;
}

void shaderc_compile_options_set_target_env(shaderc_compile_options_t options, shaderc_target_env target, uint32_t version)
{
	if (!options)
		return;
	options->target_env = target;
	options->target_version = version;
}

shaderc_compilation_result_t shaderc_compile_into_spv(const shaderc_compiler_t compiler, const char* source_text,
	size_t source_text_size, shaderc_shader_kind shader_kind, const char* input_file_name, const char* entry_point_name,
	const shaderc_compile_options_t additional_options)
{
	(void)input_file_name;
	shaderc_compilation_result* const result = new shaderc_compilation_result();
	if (!compiler)
		return result;

	glslang_stage_t stage;
	if (!StageFor(shader_kind, &stage))
	{
		result->status = shaderc_compilation_status_invalid_stage;
		result->messages = "ps5_shaderc: unsupported shader kind\n";
		result->errors = 1;
		return result;
	}
	if (additional_options && (additional_options->language != shaderc_source_language_glsl ||
								  additional_options->target_env != shaderc_target_env_vulkan))
	{
		result->status = shaderc_compilation_status_configuration_error;
		result->messages = "ps5_shaderc: only GLSL for Vulkan is built in\n";
		result->errors = 1;
		return result;
	}

	// glslang reads a NUL-terminated string; PCSX2 passes a view.
	const std::string source(source_text, source_text_size);
	const glslang_input_t input = {
		GLSLANG_SOURCE_GLSL,
		stage,
		GLSLANG_CLIENT_VULKAN,
		GLSLANG_TARGET_VULKAN_1_0,
		GLSLANG_TARGET_SPV,
		GLSLANG_TARGET_SPV_1_0,
		source.c_str(),
		110, // shaderc's default version when the source names none
		GLSLANG_NO_PROFILE,
		0,
		0,
		static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT),
		glslang_default_resource(),
		{},
		nullptr,
	};

	glslang_shader_t* const shader = glslang_shader_create(&input);
	if (!shader)
	{
		result->status = shaderc_compilation_status_internal_error;
		result->errors = 1;
		return result;
	}
	if (entry_point_name && std::strcmp(entry_point_name, "main") != 0)
		glslang_shader_set_entry_point(shader, entry_point_name);

	if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input))
	{
		Append(&result->messages, glslang_shader_get_info_log(shader));
		Append(&result->messages, glslang_shader_get_info_debug_log(shader));
		result->status = shaderc_compilation_status_compilation_error;
		result->errors = CountPrefixed(result->messages, "ERROR:");
		result->warnings = CountPrefixed(result->messages, "WARNING:");
		glslang_shader_delete(shader);
		return result;
	}

	glslang_program_t* const program = glslang_program_create();
	glslang_program_add_shader(program, shader);
	if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
	{
		Append(&result->messages, glslang_shader_get_info_log(shader));
		Append(&result->messages, glslang_program_get_info_log(program));
		result->status = shaderc_compilation_status_compilation_error;
		result->errors = CountPrefixed(result->messages, "ERROR:");
		glslang_program_delete(program);
		glslang_shader_delete(shader);
		return result;
	}

	glslang_spv_options_t spv_options = {};
	spv_options.generate_debug_info = additional_options && additional_options->generate_debug_info;
	spv_options.disable_optimizer = true;
	spv_options.validate = false;
	glslang_program_SPIRV_generate_with_options(program, stage, &spv_options);

	const size_t words = glslang_program_SPIRV_get_size(program);
	result->bytes.resize(words * sizeof(unsigned int));
	if (words != 0)
		glslang_program_SPIRV_get(program, reinterpret_cast<unsigned int*>(result->bytes.data()));
	Append(&result->messages, glslang_shader_get_info_log(shader));
	Append(&result->messages, glslang_program_SPIRV_get_messages(program));
	result->warnings = CountPrefixed(result->messages, "WARNING:");
	result->status = words != 0 ? shaderc_compilation_status_success : shaderc_compilation_status_internal_error;

	glslang_program_delete(program);
	glslang_shader_delete(shader);
	return result;
}

void shaderc_result_release(shaderc_compilation_result_t result)
{
	delete result;
}

size_t shaderc_result_get_length(const shaderc_compilation_result_t result)
{
	return result ? result->bytes.size() : 0;
}

size_t shaderc_result_get_num_warnings(const shaderc_compilation_result_t result)
{
	return result ? result->warnings : 0;
}

size_t shaderc_result_get_num_errors(const shaderc_compilation_result_t result)
{
	return result ? result->errors : 0;
}

shaderc_compilation_status shaderc_result_get_compilation_status(const shaderc_compilation_result_t result)
{
	return result ? result->status : shaderc_compilation_status_null_result_object;
}

const char* shaderc_result_get_bytes(const shaderc_compilation_result_t result)
{
	return result ? result->bytes.data() : nullptr;
}

const char* shaderc_result_get_error_message(const shaderc_compilation_result_t result)
{
	return result ? result->messages.c_str() : "";
}

} // extern "C"
