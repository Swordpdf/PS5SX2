// PS5SX2 (vk-285-119, AI-assisted): a faster lookup for the ps5vk driver's write-after-read list.
//
// The driver (Swordpdf/PS5HB_Vulkan 6a20943, driver/ps5vk_cmd_buffer.c) keeps the images draws sampled since the GPU
// last drained in a plain array, cmd_buffer->sampled_images, and walks it one entry at a time twice per draw:
// ps5vk_cmd_buffer_sampled_earlier() for each colour target the draw renders into (the WAR drain check), and
// ps5vk_cmd_buffer_note_draw_samples() for each image the draw samples (append unless already there). vk-285-118's
// profiler put those two walks at ~5% of the GS thread in Ratchet & Clank at 6x (6,081 draws a frame) -- the list
// holds about 150 images there by the timing.
//
// These are the same two functions with the same results, walking the list four entries per compare (AVX2) and from
// its end, where the image a draw samples usually is (the one the previous draws sampled). link-vk.sh renames the
// driver's own two definitions to *_orig in its copy of ps5vk_cmd_buffer.o -- only when that object is byte for byte
// the 6a20943 release's, whose field offsets are below -- and links this file; the other driver objects call these
// through the GOT. Appending stays the driver's own code (note_draw_samples_orig), so allocation, growth and the
// overflow flag are exactly the driver's.
//
// struct ps5vk_cmd_buffer offsets (6a20943, from the release archive's DWARF and the functions' code):
//   0x18e0 sampled_images: struct util_dynarray { void *mem_ctx; void *data (+0x08); unsigned size (+0x10, bytes);
//                                                 unsigned capacity (+0x14); }
//   0x18f8 bool sampled_overflow
//   0x1900 uint64_t draw_sampled[16] (PS5VK_DRAW_SAMPLED_MAX)
//   0x1980 uint32_t draw_sampled_count
// tests/war/run.sh (test_war.c) runs these against the release's own object code.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include <immintrin.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WAR_LIST_DATA 0x18e8u
#define WAR_LIST_SIZE 0x18f0u
#define WAR_OVERFLOW 0x18f8u
#define WAR_DRAW_SAMPLED 0x1900u
#define WAR_DRAW_COUNT 0x1980u
#define WAR_DRAW_SAMPLED_MAX 16u

void ps5vk_cmd_buffer_note_draw_samples_orig(void* cmd_buffer);

// main-boot.cpp names the shim in boot.log when this is linked (a weak reference there).
const int orbis_ps5vk_war_shim = 1;

static inline const uint64_t* war_list(const void* cmd_buffer, size_t* count)
{
	const unsigned char* const base = (const unsigned char*)cmd_buffer;
	const uint64_t* data;
	uint32_t bytes;
	memcpy(&data, base + WAR_LIST_DATA, sizeof(data));
	memcpy(&bytes, base + WAR_LIST_SIZE, sizeof(bytes));
	*count = bytes / sizeof(uint64_t);
	return data;
}

// Whether address is in list[0..count), looking from the end.
static inline bool war_find(const uint64_t* list, size_t count, uint64_t address)
{
	const __m256i key = _mm256_set1_epi64x((long long)address);
	size_t at = count;
	while (at >= 8)
	{
		at -= 8;
		const __m256i a = _mm256_loadu_si256((const __m256i*)(list + at));
		const __m256i b = _mm256_loadu_si256((const __m256i*)(list + at + 4));
		const __m256i hit = _mm256_or_si256(_mm256_cmpeq_epi64(a, key), _mm256_cmpeq_epi64(b, key));
		if (!_mm256_testz_si256(hit, hit))
			return true;
	}
	if (at >= 4)
	{
		at -= 4;
		const __m256i a = _mm256_loadu_si256((const __m256i*)(list + at));
		const __m256i hit = _mm256_cmpeq_epi64(a, key);
		if (!_mm256_testz_si256(hit, hit))
			return true;
	}
	while (at > 0)
	{
		if (list[--at] == address)
			return true;
	}
	return false;
}

bool ps5vk_cmd_buffer_sampled_earlier(const void* cmd_buffer, uint64_t address)
{
	const unsigned char* const base = (const unsigned char*)cmd_buffer;
	if (base[WAR_OVERFLOW])
		return true;
	size_t count;
	const uint64_t* const list = war_list(cmd_buffer, &count);
	return count != 0 && war_find(list, count, address);
}

void ps5vk_cmd_buffer_note_draw_samples(void* cmd_buffer)
{
	unsigned char* const base = (unsigned char*)cmd_buffer;
	uint32_t draw_count;
	memcpy(&draw_count, base + WAR_DRAW_COUNT, sizeof(draw_count));
	// The common case: every image this draw sampled is in the list already, and the driver's walk would append
	// nothing and only clear the draw's count. Anything else -- a new image, a draw past the scratch's size -- goes
	// to the driver's own code, which appends, grows and sets the overflow flag as it always did.
	if (draw_count <= WAR_DRAW_SAMPLED_MAX)
	{
		size_t count;
		const uint64_t* const list = war_list(cmd_buffer, &count);
		const uint64_t* const sampled = (const uint64_t*)(base + WAR_DRAW_SAMPLED);
		uint32_t at = 0;
		for (; at < draw_count; at++)
		{
			if (count == 0 || !war_find(list, count, sampled[at]))
				break;
		}
		if (at == draw_count)
		{
			const uint32_t zero = 0;
			memcpy(base + WAR_DRAW_COUNT, &zero, sizeof(zero));
			return;
		}
	}
	ps5vk_cmd_buffer_note_draw_samples_orig(cmd_buffer);
}
