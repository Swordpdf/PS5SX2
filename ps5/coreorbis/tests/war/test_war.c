// PS5SX2 (vk-285-119, AI-assisted): checks orbis-shims/orbis_ps5vk_war.c against the ps5vk driver's own code.
//
// run.sh links this with the release's ps5vk_cmd_buffer.o (6a20943), its two functions renamed *_orig exactly as
// link-vk.sh renames them, and with the shim. Two fake command buffers get the same random sequence of what the
// driver does to the write-after-read list -- a draw's samples gathered the way ps5vk_sampled_image gathers them,
// note_draw_samples, the WAR check for render targets, the drains that clear the list (the inlined
// forget_samples), lists of 0 to 600 images -- one through the driver's functions, one through the shim; every
// field the two touch must stay equal after every step. Then a timing at the list size Ratchet & Clank had.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200112L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void ps5vk_cmd_buffer_note_draw_samples_orig(void* cmd_buffer);
bool ps5vk_cmd_buffer_sampled_earlier_orig(const void* cmd_buffer, uint64_t address);
void ps5vk_cmd_buffer_note_draw_samples(void* cmd_buffer);
bool ps5vk_cmd_buffer_sampled_earlier(const void* cmd_buffer, uint64_t address);

// What the driver's object needs to link and run these two functions (util_dynarray with a NULL mem_ctx uses
// malloc/realloc; the stack-buffer and ralloc paths are never taken here).
char util_dynarray_is_data_stack_allocated;
void* reralloc_size(void* ctx, void* ptr, size_t size)
{
	(void)ctx; (void)ptr; (void)size;
	fprintf(stderr, "reralloc_size called: the test's lists have no mem_ctx\n");
	abort();
}

#define OFF_MEM_CTX 0x18e0u
#define OFF_DATA 0x18e8u
#define OFF_SIZE 0x18f0u
#define OFF_CAP 0x18f4u
#define OFF_OVERFLOW 0x18f8u
#define OFF_SAMPLED 0x1900u
#define OFF_COUNT 0x1980u
#define CMD_BYTES 0x2000u

struct fake { unsigned char* b; };

static uint64_t* f_data(struct fake f) { uint64_t* p; memcpy(&p, f.b + OFF_DATA, 8); return p; }
static uint32_t f_u32(struct fake f, unsigned off) { uint32_t v; memcpy(&v, f.b + off, 4); return v; }
static void f_set_u32(struct fake f, unsigned off, uint32_t v) { memcpy(f.b + off, &v, 4); }

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
	s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
	return s_rng;
}

// ps5vk_sampled_image's gathering of one draw's images into draw_sampled (driver/ps5vk_draw.c).
static void gather(struct fake f, uint64_t address)
{
	uint32_t count = f_u32(f, OFF_COUNT);
	if (count <= 16)
	{
		bool known = false;
		uint64_t* const sampled = (uint64_t*)(f.b + OFF_SAMPLED);
		for (uint32_t at = 0; at < (count < 16 ? count : 16); at++)
			known = known || sampled[at] == address;
		if (!known)
		{
			if (count < 16)
				sampled[count] = address;
			f_set_u32(f, OFF_COUNT, count + 1);
		}
	}
}

static int s_failures = 0;
static void compare(struct fake a, struct fake b, long step, const char* what)
{
	const uint32_t size_a = f_u32(a, OFF_SIZE), size_b = f_u32(b, OFF_SIZE);
	bool same = size_a == size_b && f_u32(a, OFF_CAP) == f_u32(b, OFF_CAP) && a.b[OFF_OVERFLOW] == b.b[OFF_OVERFLOW] &&
		f_u32(a, OFF_COUNT) == f_u32(b, OFF_COUNT) &&
		(size_a == 0 || memcmp(f_data(a), f_data(b), size_a) == 0) &&
		memcmp(a.b + OFF_SAMPLED, b.b + OFF_SAMPLED, 16 * 8) == 0;
	if (!same)
	{
		if (s_failures < 10)
			printf("FAIL step %ld (%s): size %u/%u cap %u/%u overflow %u/%u count %u/%u\n", step, what, size_a, size_b,
				f_u32(a, OFF_CAP), f_u32(b, OFF_CAP), a.b[OFF_OVERFLOW], b.b[OFF_OVERFLOW], f_u32(a, OFF_COUNT),
				f_u32(b, OFF_COUNT));
		s_failures++;
	}
}

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char** argv)
{
	const long steps = argc > 1 ? atol(argv[1]) : 3000000;
	struct fake a = {aligned_alloc(64, CMD_BYTES)}, b = {aligned_alloc(64, CMD_BYTES)};
	memset(a.b, 0, CMD_BYTES);
	memset(b.b, 0, CMD_BYTES);
	long notes = 0, fast_hits = 0, queries = 0, trues = 0, clears = 0;
	uint32_t pool = 64;
	for (long step = 0; step < steps; step++)
	{
		const uint64_t r = rnd() % 1000;
		if (r < 2)
		{
			// A drain: the driver's inlined forget_samples (size = 0, overflow cleared).
			f_set_u32(a, OFF_SIZE, 0); f_set_u32(b, OFF_SIZE, 0);
			a.b[OFF_OVERFLOW] = 0; b.b[OFF_OVERFLOW] = 0;
			clears++;
			if (rnd() % 8 == 0)
				pool = 1 + (uint32_t)(rnd() % 600); // lists from 1 to 600 distinct images
			compare(a, b, step, "drain");
		}
		else if (r < 600)
		{
			// One draw's samples, then note_draw_samples. Mostly 1-3 images, sometimes many (past the 16-entry scratch).
			const uint64_t kind = rnd() % 100;
			const uint32_t images = kind < 2 ? 17 + (uint32_t)(rnd() % 8) : kind < 10 ? (uint32_t)(rnd() % 17) : 1 + (uint32_t)(rnd() % 3);
			for (uint32_t i = 0; i < images; i++)
			{
				// Recent images more often, as a frame's draws reuse what the draws before them sampled.
				const uint32_t which = (rnd() % 4) ? (uint32_t)(rnd() % (pool < 8 ? pool : 8)) : (uint32_t)(rnd() % pool);
				const uint64_t address = 0x4000000000ull + (uint64_t)which * 0x100;
				gather(a, address);
				gather(b, address);
			}
			const uint32_t before = f_u32(b, OFF_SIZE);
			ps5vk_cmd_buffer_note_draw_samples_orig(a.b);
			ps5vk_cmd_buffer_note_draw_samples(b.b);
			fast_hits += f_u32(b, OFF_SIZE) == before;
			notes++;
			compare(a, b, step, "note");
		}
		else
		{
			// The WAR check for a draw's render targets: sometimes a sampled image, sometimes not.
			const uint64_t address = 0x4000000000ull + (rnd() % (pool * 2 + 1)) * 0x100 + ((rnd() % 16) == 0 ? 0x40 : 0);
			const bool ra = ps5vk_cmd_buffer_sampled_earlier_orig(a.b, address);
			const bool rb = ps5vk_cmd_buffer_sampled_earlier(b.b, address);
			if (ra != rb)
			{
				if (s_failures < 10)
					printf("FAIL step %ld: sampled_earlier(%#llx) driver %d shim %d\n", step, (unsigned long long)address, ra, rb);
				s_failures++;
			}
			queries++;
			trues += ra;
		}
	}
	printf("random: %ld steps (%ld notes, %ld left the list as it was; %ld WAR checks, %ld true; %ld drains), %d failure(s)\n",
		steps, notes, fast_hits, queries, trues, clears, s_failures);

	// Timing at Ratchet & Clank's size: 150 images listed, a draw sampling 2 of them, 1 render target not in the list.
	f_set_u32(a, OFF_SIZE, 0); a.b[OFF_OVERFLOW] = 0; f_set_u32(b, OFF_SIZE, 0); b.b[OFF_OVERFLOW] = 0;
	for (uint32_t i = 0; i < 150; i++)
	{
		gather(a, 0x5000000000ull + i * 0x100); gather(b, 0x5000000000ull + i * 0x100);
		ps5vk_cmd_buffer_note_draw_samples_orig(a.b); ps5vk_cmd_buffer_note_draw_samples(b.b);
	}
	compare(a, b, -1, "timing setup");
	const int draws = 2000000;
	volatile int sink = 0;
	for (int pass = 0; pass < 2; pass++)
	{
		struct fake f = pass == 0 ? a : b;
		const double t0 = now_s();
		for (int d = 0; d < draws; d++)
		{
			const uint64_t s1 = 0x5000000000ull + (uint64_t)((d * 7) % 150) * 0x100, s2 = 0x5000000000ull + (uint64_t)((d * 13 + 5) % 150) * 0x100;
			gather(f, s1); gather(f, s2);
			if (pass == 0)
			{
				sink += ps5vk_cmd_buffer_sampled_earlier_orig(f.b, 0x6000000000ull);
				ps5vk_cmd_buffer_note_draw_samples_orig(f.b);
			}
			else
			{
				sink += ps5vk_cmd_buffer_sampled_earlier(f.b, 0x6000000000ull);
				ps5vk_cmd_buffer_note_draw_samples(f.b);
			}
		}
		const double dt = now_s() - t0;
		printf("timing %s: %.1f ns a draw (150 listed images, 2 sampled, 1 target checked)\n", pass == 0 ? "driver" : "shim  ",
			dt * 1e9 / draws);
	}
	compare(a, b, -2, "timing");
	printf("%s\n", s_failures ? "FAILED" : "PASS");
	return s_failures ? 1 : 0;
}
