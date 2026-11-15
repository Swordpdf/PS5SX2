// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's src/video_core/frame_gen/lossless_dll.cpp (lossless_dll.h says what changed).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include "lossless_dll.h"
#include "lsfg_translate.h"

namespace lsfg
{
namespace
{
	constexpr u16 DOS_MAGIC = 0x5A4D;
	constexpr u32 PE_SIGNATURE = 0x00004550;
	constexpr u16 PE32_MAGIC = 0x010B;
	constexpr u16 PE32_PLUS_MAGIC = 0x020B;

	constexpr size_t DOS_LFANEW_OFFSET = 0x3C;
	constexpr size_t COFF_HEADER_SIZE = 20;
	constexpr size_t OPTIONAL_HEADER_SIZE_OFFSET = 16;
	constexpr size_t SECTION_HEADER_SIZE = 40;
	constexpr size_t DATA_DIRECTORY_ENTRY_SIZE = 8;
	constexpr size_t DATA_DIRECTORY_OFFSET_PE32 = 96;
	constexpr size_t DATA_DIRECTORY_OFFSET_PE32_PLUS = 112;
	constexpr size_t RESOURCE_DATA_DIRECTORY_INDEX = 2;

	constexpr size_t RESOURCE_DIRECTORY_SIZE = 16;
	constexpr size_t RESOURCE_NAMED_COUNT_OFFSET = 12;
	constexpr size_t RESOURCE_ID_COUNT_OFFSET = 14;
	constexpr size_t RESOURCE_ENTRY_SIZE = 8;
	constexpr u32 RESOURCE_SUBDIRECTORY_FLAG = 0x80000000;
	constexpr u32 RESOURCE_TYPE_RCDATA = 10;

	constexpr u32 PERFORMANCE_SHADER_ID_FIRST = 280;
	constexpr u32 PERFORMANCE_SHADER_ID_LAST = 302;

	// PS5SX2: a resource tree deeper or wider than this is not a Lossless.dll (and a loop in a damaged file ends here).
	constexpr size_t MAX_RESOURCE_ENTRIES = 65536;
	// PS5SX2: a DLL bigger than this is not read (Lossless.dll is a few MB).
	constexpr u64 MAX_DLL_SIZE = 512ull << 20;

	struct Section
	{
		u32 virtual_address;
		u32 virtual_size;
		u32 raw_address;
		u32 raw_size;
	};

	struct ResourceEntry
	{
		u32 id;
		u32 offset;
		bool is_directory;
		bool is_named;
	};

	class ImageReader
	{
	public:
		explicit ImageReader(std::span<const u8> image_) : image{image_} {}

		template <typename T>
		[[nodiscard]] bool Read(size_t offset, T& out_value) const
		{
			if (offset > image.size() || image.size() - offset < sizeof(T))
				return false;
			std::memcpy(&out_value, image.data() + offset, sizeof(T));
			return true;
		}

		[[nodiscard]] bool Slice(size_t offset, size_t size, std::span<const u8>& out_slice) const
		{
			if (offset > image.size() || image.size() - offset < size)
				return false;
			out_slice = image.subspan(offset, size);
			return true;
		}

	private:
		std::span<const u8> image;
	};

	[[nodiscard]] std::optional<size_t> FindPeHeader(const ImageReader& reader)
	{
		u16 dos_magic{};
		if (!reader.Read(0, dos_magic) || dos_magic != DOS_MAGIC)
			return std::nullopt;

		u32 pe_offset{};
		if (!reader.Read(DOS_LFANEW_OFFSET, pe_offset))
			return std::nullopt;

		u32 pe_signature{};
		if (!reader.Read(pe_offset, pe_signature) || pe_signature != PE_SIGNATURE)
			return std::nullopt;

		return static_cast<size_t>(pe_offset);
	}

	[[nodiscard]] std::optional<size_t> FindDataDirectory(const ImageReader& reader, size_t optional_header_offset)
	{
		u16 optional_magic{};
		if (!reader.Read(optional_header_offset, optional_magic))
			return std::nullopt;

		switch (optional_magic)
		{
			case PE32_MAGIC:
				return optional_header_offset + DATA_DIRECTORY_OFFSET_PE32;
			case PE32_PLUS_MAGIC:
				return optional_header_offset + DATA_DIRECTORY_OFFSET_PE32_PLUS;
			default:
				return std::nullopt;
		}
	}

	[[nodiscard]] bool ReadSections(const ImageReader& reader, size_t pe_offset, std::vector<Section>& out_sections)
	{
		u16 section_count{};
		u16 optional_header_size{};
		if (!reader.Read(pe_offset + 4 + 2, section_count) || !reader.Read(pe_offset + 4 + OPTIONAL_HEADER_SIZE_OFFSET, optional_header_size))
			return false;

		const size_t table_offset = pe_offset + 4 + COFF_HEADER_SIZE + optional_header_size;
		out_sections.reserve(section_count);
		for (size_t i = 0; i < section_count; ++i)
		{
			const size_t offset = table_offset + i * SECTION_HEADER_SIZE;
			Section section{};
			if (!reader.Read(offset + 8, section.virtual_size) || !reader.Read(offset + 12, section.virtual_address) ||
				!reader.Read(offset + 16, section.raw_size) || !reader.Read(offset + 20, section.raw_address))
			{
				return false;
			}
			out_sections.push_back(section);
		}
		return true;
	}

	[[nodiscard]] std::optional<size_t> RvaToFileOffset(std::span<const Section> sections, u32 rva)
	{
		for (const Section& section : sections)
		{
			const u32 span = std::max(section.virtual_size, section.raw_size);
			if (span == 0 || rva < section.virtual_address)
				continue;
			const u32 relative = rva - section.virtual_address;
			if (relative < span)
				return static_cast<size_t>(section.raw_address) + relative;
		}
		return std::nullopt;
	}

	[[nodiscard]] bool ReadResourceEntries(const ImageReader& reader, size_t directory_offset, std::vector<ResourceEntry>& out_entries)
	{
		u16 named_count{};
		u16 id_count{};
		if (!reader.Read(directory_offset + RESOURCE_NAMED_COUNT_OFFSET, named_count) ||
			!reader.Read(directory_offset + RESOURCE_ID_COUNT_OFFSET, id_count))
		{
			return false;
		}

		const size_t total = size_t{named_count} + size_t{id_count};
		if (total > MAX_RESOURCE_ENTRIES)
			return false;
		out_entries.clear();
		out_entries.reserve(total);
		for (size_t i = 0; i < total; ++i)
		{
			const size_t offset = directory_offset + RESOURCE_DIRECTORY_SIZE + i * RESOURCE_ENTRY_SIZE;
			u32 name{};
			u32 data{};
			if (!reader.Read(offset, name) || !reader.Read(offset + 4, data))
				return false;
			out_entries.push_back(ResourceEntry{
				name & ~RESOURCE_SUBDIRECTORY_FLAG,
				data & ~RESOURCE_SUBDIRECTORY_FLAG,
				(data & RESOURCE_SUBDIRECTORY_FLAG) != 0,
				(name & RESOURCE_SUBDIRECTORY_FLAG) != 0,
			});
		}
		return true;
	}

	[[nodiscard]] bool ReadResourceLeaf(
		const ImageReader& reader, std::span<const Section> sections, size_t leaf_offset, std::span<const u8>& out_data)
	{
		u32 data_rva{};
		u32 data_size{};
		if (!reader.Read(leaf_offset, data_rva) || !reader.Read(leaf_offset + 4, data_size) || data_size == 0)
			return false;

		const std::optional<size_t> data_offset = RvaToFileOffset(sections, data_rva);
		if (!data_offset)
			return false;
		return reader.Slice(*data_offset, data_size, out_data);
	}

	using ResourceSpans = std::map<u32, std::span<const u8>>;

	[[nodiscard]] bool CollectRcData(
		const ImageReader& reader, std::span<const Section> sections, size_t resource_base, ResourceSpans& out_resources)
	{
		std::vector<ResourceEntry> type_entries;
		if (!ReadResourceEntries(reader, resource_base, type_entries))
			return false;

		for (const ResourceEntry& type_entry : type_entries)
		{
			if (type_entry.is_named || type_entry.id != RESOURCE_TYPE_RCDATA || !type_entry.is_directory)
				continue;

			std::vector<ResourceEntry> name_entries;
			if (!ReadResourceEntries(reader, resource_base + type_entry.offset, name_entries))
				return false;

			for (const ResourceEntry& name_entry : name_entries)
			{
				if (name_entry.is_named || !name_entry.is_directory)
					continue;

				std::vector<ResourceEntry> language_entries;
				if (!ReadResourceEntries(reader, resource_base + name_entry.offset, language_entries))
					return false;

				for (const ResourceEntry& language_entry : language_entries)
				{
					if (language_entry.is_directory)
						continue;
					std::span<const u8> data;
					if (!ReadResourceLeaf(reader, sections, resource_base + language_entry.offset, data))
						continue;
					out_resources.insert_or_assign(name_entry.id, data);
					break;
				}
			}
		}

		return true;
	}

	template <typename Map>
	[[nodiscard]] bool HasNativeShaders(const Map& resources)
	{
		return std::ranges::all_of(PerformanceShaderIds(), [&](u32 id) {
			const auto hit = resources.find(id + PerformanceShader::NATIVE_FP16_OFFSET);
			return hit != resources.end() && IsSpirvModule(hit->second);
		});
	}

	[[nodiscard]] LosslessStatus TranslateAll(const ResourceSpans& resources, ShaderModules& out_modules)
	{
		out_modules.clear();
		for (const u32 id : PerformanceShaderIds())
		{
			const auto hit = resources.find(id + PerformanceShader::NATIVE_FP16_OFFSET);
			if (hit == resources.end())
				return LosslessStatus::MissingShaders;
			std::vector<u32> adopted = AdoptSpirvModule(hit->second);
			if (adopted.empty())
				return LosslessStatus::TranslationFailed;
			out_modules.emplace(id, std::move(adopted));
		}
		return LosslessStatus::Ok;
	}

	[[nodiscard]] LosslessStatus ParseShaderSpans(std::span<const u8> image, ResourceSpans& out_resources)
	{
		const ImageReader reader{image};
		const std::optional<size_t> pe_offset = FindPeHeader(reader);
		if (!pe_offset)
			return LosslessStatus::NotPortableExecutable;

		const std::optional<size_t> data_directory = FindDataDirectory(reader, *pe_offset + 4 + COFF_HEADER_SIZE);
		if (!data_directory)
			return LosslessStatus::NotPortableExecutable;

		std::vector<Section> sections;
		if (!ReadSections(reader, *pe_offset, sections))
			return LosslessStatus::NotPortableExecutable;

		u32 resource_rva{};
		if (!reader.Read(*data_directory + RESOURCE_DATA_DIRECTORY_INDEX * DATA_DIRECTORY_ENTRY_SIZE, resource_rva) || resource_rva == 0)
			return LosslessStatus::MissingShaders;

		const std::optional<size_t> resource_base = RvaToFileOffset(sections, resource_rva);
		if (!resource_base)
			return LosslessStatus::NotPortableExecutable;

		out_resources.clear();
		if (!CollectRcData(reader, sections, *resource_base, out_resources))
			return LosslessStatus::MissingShaders;

		if (!HasNativeShaders(out_resources))
			return LosslessStatus::MissingShaders;
		return LosslessStatus::Ok;
	}
} // Anonymous namespace

const char* LosslessStatusText(LosslessStatus status)
{
	switch (status)
	{
		case LosslessStatus::Ok: return "ok";
		case LosslessStatus::NotInstalled: return "Lossless.dll not installed";
		case LosslessStatus::UnreadableFile: return "Lossless.dll unreadable";
		case LosslessStatus::NotPortableExecutable: return "not a PE (Windows DLL) file";
		case LosslessStatus::MissingShaders: return "no LSFG shaders in it (another version of Lossless Scaling?)";
		case LosslessStatus::TranslationFailed: return "shader translation failed";
	}
	return "unknown";
}

std::vector<u32> PerformanceShaderIds()
{
	std::vector<u32> ids{PerformanceShader::MIPMAPS, PerformanceShader::GENERATE};
	for (u32 id = PERFORMANCE_SHADER_ID_FIRST; id <= PERFORMANCE_SHADER_ID_LAST; ++id)
		ids.push_back(id);
	return ids;
}

std::string FindLosslessDll(const std::string& folder)
{
	const std::string exact = folder + "/Lossless.dll";
	struct stat st;
	if (stat(exact.c_str(), &st) == 0)
		return exact;
	if (DIR* dir = opendir(folder.c_str()))
	{
		std::string found;
		while (const dirent* entry = readdir(dir))
		{
			if (strcasecmp(entry->d_name, "Lossless.dll") == 0)
			{
				found = folder + "/" + entry->d_name;
				break;
			}
		}
		closedir(dir);
		if (!found.empty())
			return found;
	}
	return exact;
}

LosslessStatus ParseShaderModules(std::span<const u8> image, ShaderModules& out_modules)
{
	ResourceSpans spans;
	const LosslessStatus parse_status = ParseShaderSpans(image, spans);
	if (parse_status != LosslessStatus::Ok)
		return parse_status;
	return TranslateAll(spans, out_modules);
}

LosslessStatus LoadShaderModules(const std::string& path, ShaderModules& out_modules, u64* out_size)
{
	if (out_size)
		*out_size = 0;
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return LosslessStatus::NotInstalled;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 || static_cast<u64>(st.st_size) > MAX_DLL_SIZE)
		return LosslessStatus::UnreadableFile;

	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (!file)
		return LosslessStatus::UnreadableFile;
	std::vector<u8> image(static_cast<size_t>(st.st_size));
	const size_t read = std::fread(image.data(), 1, image.size(), file);
	std::fclose(file);
	if (read != image.size())
		return LosslessStatus::UnreadableFile;
	if (out_size)
		*out_size = image.size();

	return ParseShaderModules(image, out_modules);
}
} // namespace lsfg
