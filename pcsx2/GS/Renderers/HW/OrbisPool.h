// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PS5 port (vk-285-125, AI-assisted): free lists for the texture cache's small, short-lived allocations. On the console
// the system malloc and free enter libkernel's syscall stubs (0x8000002d0): in vk-285-124's GS-thread profile of GTA
// Liberty City Stories (60 FPS patch, 6x, the GS thread 100% busy) that was ~4% of the thread, from
// SourceMap::RemoveAt's delete and the hash containers' node inserts, ~5% with free/new around it. A freed block goes
// on its size's list (up to MaxFree of them) and the next allocation of that size takes it back.
//
// Not thread-safe, on purpose: everything that uses these runs on the GS thread (the texture cache is the GS thread's).
// Blocks are never handed between lists of different sizes or alignments. Needs proper testing.

#include "common/AlignedMalloc.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>

// Counts for the [tcstat] line (GSTextureCache.cpp), every list together: allocations served from a list, and from the
// system. GS thread only.
inline unsigned long long g_orbis_pool_reused = 0;
inline unsigned long long g_orbis_pool_fresh = 0;

template <std::size_t Size, std::size_t Align, std::size_t MaxFree>
class OrbisFreeList
{
	static_assert(Size >= sizeof(void*) && Align >= alignof(void*));
	static constexpr bool kPlainNew = Align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__;

public:
	static void* Get()
	{
		if (void* const p = s_head)
		{
			s_head = *static_cast<void**>(p);
			s_free--;
			g_orbis_pool_reused++;
			return p;
		}
		g_orbis_pool_fresh++;
		if constexpr (kPlainNew)
			return ::operator new(Size);
		else
		{
			void* const p = _aligned_malloc(Size, Align);
			if (!p)
				std::abort(); // out of memory (the build has no exceptions)
			return p;
		}
	}

	static void Put(void* p) noexcept
	{
		if (!p)
			return;
		if (s_free < MaxFree)
		{
			*static_cast<void**>(p) = s_head;
			s_head = p;
			s_free++;
			return;
		}
		if constexpr (kPlainNew)
			::operator delete(p);
		else
			_aligned_free(p);
	}

private:
	static inline void* s_head = nullptr;
	static inline std::size_t s_free = 0;
};

// An allocator for node-based containers (std::unordered_map/set): single nodes come from OrbisFreeList, arrays (the
// bucket table on a rehash) from the system as before.
template <typename T>
struct OrbisNodeAllocator
{
	using value_type = T;

	static constexpr std::size_t kSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
	static constexpr std::size_t kAlign = alignof(T) < alignof(void*) ? alignof(void*) : alignof(T);
	using List = OrbisFreeList<kSize, kAlign, 16384>;

	OrbisNodeAllocator() noexcept = default;
	template <typename U>
	OrbisNodeAllocator(const OrbisNodeAllocator<U>&) noexcept
	{
	}

	T* allocate(std::size_t n)
	{
		if (n == 1)
			return static_cast<T*>(List::Get());
		return std::allocator<T>().allocate(n);
	}

	void deallocate(T* p, std::size_t n) noexcept
	{
		if (n == 1)
		{
			List::Put(p);
			return;
		}
		std::allocator<T>().deallocate(p, n);
	}

	template <typename U>
	bool operator==(const OrbisNodeAllocator<U>&) const noexcept
	{
		return true;
	}
	template <typename U>
	bool operator!=(const OrbisNodeAllocator<U>&) const noexcept
	{
		return false;
	}
};
