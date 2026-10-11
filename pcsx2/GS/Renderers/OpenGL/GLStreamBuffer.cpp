// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLStreamBuffer.h"

#include "common/BitUtils.h"
#include "common/AlignedMalloc.h"
#include "common/Assertions.h"
#include "common/Console.h"

#include <array>
#include <cstring>

GLStreamBuffer::GLStreamBuffer(GLenum target, GLuint buffer_id, u32 size)
	: m_target(target)
	, m_buffer_id(buffer_id)
	, m_size(size)
{
}

GLStreamBuffer::~GLStreamBuffer()
{
	glDeleteBuffers(1, &m_buffer_id);
}

void GLStreamBuffer::Bind()
{
	glBindBuffer(m_target, m_buffer_id);
}

void GLStreamBuffer::Unbind()
{
	glBindBuffer(m_target, 0);
}

namespace
{
	// Uses glBufferSubData() to update. Preferred for drivers which don't support {ARB,EXT}_buffer_storage.
	class BufferSubDataStreamBuffer final : public GLStreamBuffer
	{
	public:
		~BufferSubDataStreamBuffer() override { _aligned_free(m_cpu_buffer); }

		MappingResult Map(u32 alignment, u32 min_size) override
		{
			return MappingResult{static_cast<void*>(m_cpu_buffer), 0, 0, m_size / alignment};
		}

		void Unmap(u32 used_size) override
		{
			if (used_size == 0)
				return;

			glBindBuffer(m_target, m_buffer_id);
			glBufferSubData(m_target, 0, used_size, m_cpu_buffer);
		}

		u32 GetChunkSize() const override { return m_size; }

		static std::unique_ptr<GLStreamBuffer> Create(GLenum target, u32 size)
		{
			glGetError();

			GLuint buffer_id;
			glGenBuffers(1, &buffer_id);
			glBindBuffer(target, buffer_id);
			glBufferData(target, size, nullptr, GL_STREAM_DRAW);

			GLenum err = glGetError();
			if (err != GL_NO_ERROR)
			{
				glBindBuffer(target, 0);
				glDeleteBuffers(1, &buffer_id);
				return {};
			}

			return std::unique_ptr<GLStreamBuffer>(new BufferSubDataStreamBuffer(target, buffer_id, size));
		}

	private:
		BufferSubDataStreamBuffer(GLenum target, GLuint buffer_id, u32 size)
			: GLStreamBuffer(target, buffer_id, size)
		{
			m_cpu_buffer = static_cast<u8*>(_aligned_malloc(size, 32));
			if (!m_cpu_buffer)
				pxFailRel("Failed to allocate CPU storage for GL buffer");
		}

		u8* m_cpu_buffer;
	};

	// Uses BufferData() to orphan the buffer after every update. Used on Mali where BufferSubData forces a sync.
	class BufferDataStreamBuffer final : public GLStreamBuffer
	{
	public:
		~BufferDataStreamBuffer() override { _aligned_free(m_cpu_buffer); }

		MappingResult Map(u32 alignment, u32 min_size) override
		{
			return MappingResult{static_cast<void*>(m_cpu_buffer), 0, 0, m_size / alignment};
		}

		void Unmap(u32 used_size) override
		{
			if (used_size == 0)
				return;

			// Orbis: a different size makes Mesa allocate a fresh resource (written UNSYNCHRONIZED)
			// instead of DISCARD_WHOLE_RESOURCE on the one queued draws still read.
			if (used_size == m_last_orphan_size && used_size + 64 <= m_size)
				used_size += 64;
			m_last_orphan_size = used_size;
			glBindBuffer(m_target, m_buffer_id);
			glBufferData(m_target, used_size, m_cpu_buffer, GL_STREAM_DRAW);
		}

		u32 GetChunkSize() const override { return m_size; }

		static std::unique_ptr<GLStreamBuffer> Create(GLenum target, u32 size)
		{
			glGetError();

			GLuint buffer_id;
			glGenBuffers(1, &buffer_id);
			glBindBuffer(target, buffer_id);
			glBufferData(target, size, nullptr, GL_STREAM_DRAW);

			GLenum err = glGetError();
			if (err != GL_NO_ERROR)
			{
				glBindBuffer(target, 0);
				glDeleteBuffers(1, &buffer_id);
				return {};
			}

			return std::unique_ptr<GLStreamBuffer>(new BufferDataStreamBuffer(target, buffer_id, size));
		}

	private:
		BufferDataStreamBuffer(GLenum target, GLuint buffer_id, u32 size)
			: GLStreamBuffer(target, buffer_id, size)
		{
			m_cpu_buffer = static_cast<u8*>(_aligned_malloc(size, 32));
			if (!m_cpu_buffer)
				pxFailRel("Failed to allocate CPU storage for GL buffer");
		}

		u8* m_cpu_buffer;
		u32 m_last_orphan_size = 0;
	};


	// Orbis: ring of small GL buffers updated with glBufferSubData. Same-size glBufferData orphaning
	// becomes DISCARD_WHOLE_RESOURCE in Mesa, which the PS5 driver can only honour by draining its
	// deferred draw batch. A ring entry is only rewritten after RING_SIZE uploads, so queued draws
	// keep their own constants and batches can grow up to RING_SIZE draws.
	class RingSubDataStreamBuffer final : public GLStreamBuffer
	{
	public:
		enum : u32
		{
			RING_SIZE = 768,
			INITIAL_CAPACITY = 4096
		};

		~RingSubDataStreamBuffer() override
		{
			glDeleteBuffers(RING_SIZE, m_ring.data());
			m_buffer_id = 0;
			_aligned_free(m_cpu_buffer);
		}

		MappingResult Map(u32 alignment, u32 min_size) override
		{
			return MappingResult{static_cast<void*>(m_cpu_buffer), 0, 0, m_size / alignment};
		}

		void Unmap(u32 used_size) override
		{
			if (used_size == 0)
				return;
			m_pos = (m_pos + 1) % RING_SIZE;
			m_buffer_id = m_ring[m_pos];
			glBindBuffer(m_target, m_buffer_id);
			if (used_size > m_capacity[m_pos])
			{
				glBufferData(m_target, used_size, m_cpu_buffer, GL_STREAM_DRAW);
				m_capacity[m_pos] = used_size;
			}
			else
			{
				glBufferSubData(m_target, 0, used_size, m_cpu_buffer);
			}
		}

		u32 GetChunkSize() const override { return m_size; }

		static std::unique_ptr<GLStreamBuffer> Create(GLenum target, u32 size)
		{
			glGetError();
			std::array<GLuint, RING_SIZE> ring{};
			glGenBuffers(RING_SIZE, ring.data());
			for (GLuint id : ring)
			{
				glBindBuffer(target, id);
				glBufferData(target, INITIAL_CAPACITY, nullptr, GL_STREAM_DRAW);
			}
			if (glGetError() != GL_NO_ERROR)
			{
				glBindBuffer(target, 0);
				glDeleteBuffers(RING_SIZE, ring.data());
				return {};
			}
			return std::unique_ptr<GLStreamBuffer>(new RingSubDataStreamBuffer(target, ring, size));
		}

	private:
		RingSubDataStreamBuffer(GLenum target, const std::array<GLuint, RING_SIZE>& ring, u32 size)
			: GLStreamBuffer(target, ring[0], size)
			, m_ring(ring)
		{
			m_capacity.fill(INITIAL_CAPACITY);
			m_cpu_buffer = static_cast<u8*>(_aligned_malloc(size, 32));
			if (!m_cpu_buffer)
				pxFailRel("Failed to allocate CPU storage for GL buffer");
		}

		std::array<GLuint, RING_SIZE> m_ring;
		std::array<u32, RING_SIZE> m_capacity;
		u32 m_pos = 0;
		u8* m_cpu_buffer;
	};

	// Base class for implementations which require syncing.
	class SyncingStreamBuffer : public GLStreamBuffer
	{
	public:
		enum : u32
		{
			NUM_SYNC_POINTS = 16
		};

		virtual ~SyncingStreamBuffer() override
		{
			for (u32 i = m_available_block_index; i <= m_used_block_index; i++)
			{
				pxAssert(m_sync_objects[i]);
				glDeleteSync(m_sync_objects[i]);
			}
		}

	protected:
		SyncingStreamBuffer(GLenum target, GLuint buffer_id, u32 size)
			: GLStreamBuffer(target, buffer_id, size)
			, m_bytes_per_block((size + (NUM_SYNC_POINTS)-1) / NUM_SYNC_POINTS)
		{
		}

		__fi u32 GetSyncIndexForOffset(u32 offset) { return offset / m_bytes_per_block; }

		__fi void AddSyncsForOffset(u32 offset)
		{
			const u32 end = GetSyncIndexForOffset(offset);
			for (; m_used_block_index < end; m_used_block_index++)
			{
				pxAssert(!m_sync_objects[m_used_block_index]);
				m_sync_objects[m_used_block_index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			}
		}

		__fi void WaitForSync(GLsync& sync)
		{
			glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
			glDeleteSync(sync);
			sync = nullptr;
		}

		__fi void EnsureSyncsWaitedForOffset(u32 offset)
		{
			const u32 end = std::min<u32>(GetSyncIndexForOffset(offset) + 1, NUM_SYNC_POINTS);
			for (; m_available_block_index < end; m_available_block_index++)
			{
				pxAssert(m_sync_objects[m_available_block_index]);
				WaitForSync(m_sync_objects[m_available_block_index]);
			}
		}

		void AllocateSpace(u32 size)
		{
			// add sync objects for writes since the last allocation
			AddSyncsForOffset(m_position);

			// wait for sync objects for the space we want to use
			EnsureSyncsWaitedForOffset(m_position + size);

			// wrap-around?
			if ((m_position + size) > m_size)
			{
				// current position ... buffer end
				AddSyncsForOffset(m_size);

				// rewind, and try again
				m_position = 0;

				// wait for the sync at the start of the buffer
				WaitForSync(m_sync_objects[0]);
				m_available_block_index = 1;

				// and however much more we need to satisfy the allocation
				EnsureSyncsWaitedForOffset(size);
				m_used_block_index = 0;
			}
		}

		u32 GetChunkSize() const override { return m_size / NUM_SYNC_POINTS; }

		u32 m_position = 0;
		u32 m_used_block_index = 0;
		u32 m_available_block_index = NUM_SYNC_POINTS;
		u32 m_bytes_per_block;
		std::array<GLsync, NUM_SYNC_POINTS> m_sync_objects{};
	};

	class BufferStorageStreamBuffer : public SyncingStreamBuffer
	{
	public:
		~BufferStorageStreamBuffer() override
		{
			glBindBuffer(m_target, m_buffer_id);
			glUnmapBuffer(m_target);
			glBindBuffer(m_target, 0);
		}

		MappingResult Map(u32 alignment, u32 min_size) override
		{
			if (m_position > 0)
				m_position = Common::AlignUp(m_position, alignment);

			AllocateSpace(min_size);
			pxAssert((m_position + min_size) <= (m_available_block_index * m_bytes_per_block));

			const u32 free_space_in_block = ((m_available_block_index * m_bytes_per_block) - m_position);
			return MappingResult{static_cast<void*>(m_mapped_ptr + m_position), m_position, m_position / alignment,
				free_space_in_block / alignment};
		}

		void Unmap(u32 used_size) override
		{
			pxAssert((m_position + used_size) <= m_size);
			if (!m_coherent)
			{
				Bind();
				glFlushMappedBufferRange(m_target, m_position, used_size);
			}

			m_position += used_size;
		}

		static std::unique_ptr<GLStreamBuffer> Create(GLenum target, u32 size, bool coherent = true)
		{
			glGetError();

			GLuint buffer_id;
			glGenBuffers(1, &buffer_id);
			glBindBuffer(target, buffer_id);

			const u32 flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? GL_MAP_COHERENT_BIT : 0);
			const u32 map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? 0 : GL_MAP_FLUSH_EXPLICIT_BIT);
			if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
				glBufferStorage(target, size, nullptr, flags);
			else if (GLAD_GL_EXT_buffer_storage)
				glBufferStorageEXT(target, size, nullptr, flags);

			GLenum err = glGetError();
			if (err != GL_NO_ERROR)
			{
				glBindBuffer(target, 0);
				glDeleteBuffers(1, &buffer_id);
				return {};
			}

			u8* mapped_ptr = static_cast<u8*>(glMapBufferRange(target, 0, size, map_flags));
			pxAssertRel(mapped_ptr, "Persistent buffer was mapped");

			return std::unique_ptr<GLStreamBuffer>(
				new BufferStorageStreamBuffer(target, buffer_id, size, mapped_ptr, coherent));
		}

	private:
		BufferStorageStreamBuffer(GLenum target, GLuint buffer_id, u32 size, u8* mapped_ptr, bool coherent)
			: SyncingStreamBuffer(target, buffer_id, size)
			, m_mapped_ptr(mapped_ptr)
			, m_coherent(coherent)
		{
		}

		u8* m_mapped_ptr;
		bool m_coherent;
	};
	// Orbis: unsynchronized ring mapping of one buffer (no persistent maps needed).
	class MapRangeStreamBuffer final : public SyncingStreamBuffer
	{
	public:
		~MapRangeStreamBuffer() override = default;

		MappingResult Map(u32 alignment, u32 min_size) override
		{
			if (m_position > 0)
				m_position = Common::AlignUp(m_position, alignment);

			AllocateSpace(min_size);
			pxAssert((m_position + min_size) <= (m_available_block_index * m_bytes_per_block));

			const u32 free_space_in_block = ((m_available_block_index * m_bytes_per_block) - m_position);
			Bind();
			void* ptr = glMapBufferRange(m_target, m_position, free_space_in_block,
				GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
			pxAssertRel(ptr, "Unsynchronized buffer range was mapped");
			return MappingResult{ptr, m_position, m_position / alignment, free_space_in_block / alignment};
		}

		void Unmap(u32 used_size) override
		{
			pxAssert((m_position + used_size) <= m_size);
			Bind();
			glUnmapBuffer(m_target);
			m_position += used_size;
		}

		static std::unique_ptr<GLStreamBuffer> Create(GLenum target, u32 size)
		{
			glGetError();
			GLuint buffer_id;
			glGenBuffers(1, &buffer_id);
			glBindBuffer(target, buffer_id);
			glBufferData(target, size, nullptr, GL_STREAM_DRAW);
			if (glGetError() != GL_NO_ERROR)
			{
				glBindBuffer(target, 0);
				glDeleteBuffers(1, &buffer_id);
				return {};
			}
			return std::unique_ptr<GLStreamBuffer>(new MapRangeStreamBuffer(target, buffer_id, size));
		}

	private:
		MapRangeStreamBuffer(GLenum target, GLuint buffer_id, u32 size)
			: SyncingStreamBuffer(target, buffer_id, size)
		{
		}
	};
} // namespace

std::unique_ptr<GLStreamBuffer> GLStreamBuffer::Create(GLenum target, u32 size)
{
	std::unique_ptr<GLStreamBuffer> buf;
	// Orbis: see RingSubDataStreamBuffer (uniforms stay on the proven ring).
	if (target == GL_UNIFORM_BUFFER)
	{
		buf = RingSubDataStreamBuffer::Create(target, size);
		if (buf)
			return buf;
	}

	if (target != GL_PIXEL_UNPACK_BUFFER && (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage))
	{
		buf = BufferStorageStreamBuffer::Create(target, size);
		if (buf)
		{
			Console.WriteLn("Orbis: persistent stream buffer target=%x size=%u", target, size);
			return buf;
		}
	}

	// Orbis: see MapRangeStreamBuffer.
	if (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER)
	{
		buf = MapRangeStreamBuffer::Create(target, size);
		if (buf)
			return buf;
	}

	// BufferSubData is slower on all drivers except NVIDIA...
	const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
	if (std::strstr(vendor, "NVIDIA"))
		return BufferSubDataStreamBuffer::Create(target, size);
	else
		return BufferDataStreamBuffer::Create(target, size);
}
