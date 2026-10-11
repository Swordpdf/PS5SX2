// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "vk_mem_alloc.h"

#include "common/AlignedMalloc.h"

#include <deque>
#include <memory>

// PS5 port (vk-285-91): an allocator that gives each allocation whole cache lines of its own. The stream
// buffers' fence lists (a deque read on every ReserveMemory, ~3 times a draw) came from the general heap, and
// the load of the deque's block map missed on ~1.9% of the GS thread's samples in vk-285-90's Shadow of the
// Colossus profile: most likely a line shared with memory another thread writes. Needs proper testing.
template <typename T>
struct OrbisLineAllocator
{
	using value_type = T;
	OrbisLineAllocator() = default;
	template <typename U>
	OrbisLineAllocator(const OrbisLineAllocator<U>&) noexcept
	{
	}
	T* allocate(std::size_t n)
	{
		return static_cast<T*>(_aligned_malloc((n * sizeof(T) + 63) & ~static_cast<std::size_t>(63), 64));
	}
	void deallocate(T* p, std::size_t) noexcept { _aligned_free(p); }
	template <typename U>
	bool operator==(const OrbisLineAllocator<U>&) const noexcept
	{
		return true;
	}
	template <typename U>
	bool operator!=(const OrbisLineAllocator<U>&) const noexcept
	{
		return false;
	}
};

class VKStreamBuffer
{
public:
	VKStreamBuffer();
	VKStreamBuffer(VKStreamBuffer&& move);
	VKStreamBuffer(const VKStreamBuffer&) = delete;
	~VKStreamBuffer();

	VKStreamBuffer& operator=(VKStreamBuffer&& move);
	VKStreamBuffer& operator=(const VKStreamBuffer&) = delete;

	__fi bool IsValid() const { return (m_buffer != VK_NULL_HANDLE); }
	__fi VkBuffer GetBuffer() const { return m_buffer; }
	__fi const VkBuffer* GetBufferPtr() const { return &m_buffer; }
	__fi u8* GetHostPointer() const { return m_host_pointer; }
	__fi u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
	__fi u32 GetCurrentSize() const { return m_size; }
	__fi u32 GetCurrentSpace() const { return m_current_space; }
	__fi u32 GetCurrentOffset() const { return m_current_offset; }

	bool Create(VkBufferUsageFlags usage, u32 size);
	void Destroy(bool defer);

	bool ReserveMemory(u32 num_bytes, u32 alignment);
	void CommitMemory(u32 final_num_bytes);

	// PS5 port (vk-285-106): the buffer is written with streaming stores (the vertex buffer: GSVector4i::storent),
	// so flags/gspfw's write prefetches skip it (a streaming store to a line in the cache costs more, not less).
	__fi void OrbisSetStreamed(bool streamed) { m_orbis_streamed = streamed; }

private:
	bool AllocateBuffer(VkBufferUsageFlags usage, u32 size);
	void UpdateCurrentFencePosition();
	void UpdateGPUPosition();
	void OrbisMaterializeFence(); // PS5 port (vk-285-97)
	void OrbisPrefetchAhead(); // PS5 port (vk-285-106), with flags/gspfw

	// Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
	bool WaitForClearSpace(u32 num_bytes);

	u32 m_size = 0;
	u32 m_current_offset = 0;
	u32 m_current_space = 0;
	u32 m_current_gpu_position = 0;

	VmaAllocation m_allocation = VK_NULL_HANDLE;
	bool m_orbis_coherent = false; // PS5 port (vk-285-85): see Create()
	VkBuffer m_buffer = VK_NULL_HANDLE;
	u8* m_host_pointer = nullptr;

	// PS5 port (vk-285-97): the newest fence's entry, kept here until something reads the list
	// (OrbisMaterializeFence), so a commit doesn't touch the deque.
	u64 m_orbis_fence_counter = 0;
	u32 m_orbis_fence_offset = 0;
	bool m_orbis_fence_pending = false;
	u32 m_orbis_pf_next = 0; // PS5 port (vk-285-106): the next line OrbisPrefetchAhead hasn't asked for
	bool m_orbis_streamed = false; // PS5 port (vk-285-106): OrbisSetStreamed

	// List of fences and the corresponding positions in the buffer
	std::deque<std::pair<u64, u32>, OrbisLineAllocator<std::pair<u64, u32>>> m_tracked_fences; // vk-285-91
};
