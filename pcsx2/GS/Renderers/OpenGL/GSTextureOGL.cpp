// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GSDeviceOGL.h"
#include "GS/Renderers/OpenGL/GSTextureOGL.h"
#include "GS/Renderers/OpenGL/GLState.h"
#include "GS/GSExtra.h"
#include "GS/GSPerfMon.h"
#include "GS/GSGL.h"

#include "common/Console.h"
#include "common/BitUtils.h"
#include "common/AlignedMalloc.h"
#include "common/StringUtil.h"

// Looking across a range of GPUs, the optimal copy alignment for Vulkan drivers seems
// to be between 1 (AMD/NV) and 64 (Intel). So, we'll go with 64 here.
static constexpr u32 TEXTURE_UPLOAD_ALIGNMENT = 64;

// The pitch alignment must be less or equal to the upload alignment.
// We need 32 here for AVX2, so 64 is also fine.
static constexpr u32 TEXTURE_UPLOAD_PITCH_ALIGNMENT = 64;

GSTextureOGL::GSTextureOGL(Usage usage, int width, int height, int levels, Format format)
{
	// OpenGL didn't like dimensions of size 0
	m_size.x = std::max(1, width);
	m_size.y = std::max(1, height);
	m_usage = usage;
	m_format = format;
	m_texture_id = 0;
	m_mipmap_levels = 1;

	// Bunch of constant parameter
	switch (m_format)
	{
		// 1 Channel integer
		case Format::PrimID:
			m_gl_format = GL_R32F;
			m_int_format = GL_RED;
			m_int_type = GL_INT;
			m_int_shift = 2;
			break;
		case Format::UInt32:
			m_gl_format = GL_R32UI;
			m_int_format = GL_RED_INTEGER;
			m_int_type = GL_UNSIGNED_INT;
			m_int_shift = 2;
			break;
		case Format::UInt16:
			m_gl_format = GL_R16UI;
			m_int_format = GL_RED_INTEGER;
			m_int_type = GL_UNSIGNED_SHORT;
			m_int_shift = 1;
			break;

		// 1 Channel normalized
		case Format::UNorm8:
			m_gl_format = GL_R8;
			m_int_format = GL_RED;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 0;
			break;
		
		// 1 channel float
		case Format::DepthColor:
			m_gl_format = GL_R32F;
			m_int_format = GL_RED;
			m_int_type = GL_FLOAT;
			m_int_shift = 2;
			break;

		// 4 channel normalized
		case Format::Color:
		case Format::ColorHQ:
		case Format::ColorHDR:
			m_gl_format = GL_RGBA8;
			m_int_format = GL_RGBA;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 2;
			break;

		// 4 channel float
		case Format::ColorClip:
			m_gl_format = GL_RGBA16;
			m_int_format = GL_RGBA;
			m_int_type = GL_UNSIGNED_SHORT;
			m_int_shift = 3;
			break;

		// Depth buffer
		case Format::DepthStencil:
		{
			if (!g_gs_device->Features().framebuffer_fetch)
			{
				m_gl_format = GL_DEPTH32F_STENCIL8;
				m_int_format = GL_DEPTH_STENCIL;
				m_int_type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
				m_int_shift = 3; // 4 bytes for depth + 4 bytes for stencil by texels
			}
			else
			{
				m_gl_format = GL_DEPTH_COMPONENT32F;
				m_int_format = GL_DEPTH_COMPONENT;
				m_int_type = GL_FLOAT;
				m_int_shift = 2;
			}
		}
		break;

		case Format::BC1:
			m_gl_format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			m_int_format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::BC2:
			m_gl_format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
			m_int_format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::BC3:
			m_gl_format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			m_int_format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::BC7:
			m_gl_format = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			m_int_format = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::Invalid:
			m_int_format = 0;
			m_int_type = 0;
			m_int_shift = 0;
			pxAssert(0);
	}

	// Only 32 bits input texture will be supported for mipmap
	if (IsTexture())
		m_mipmap_levels = levels;

	// Create a gl object (texture isn't allocated here)
	glCreateTextures(GL_TEXTURE_2D, 1, &m_texture_id);
	if (m_format == Format::UNorm8)
	{
		// Emulate DX behavior, beside it avoid special code in shader to differentiate
		// palette texture from a GL_RGBA target or a GL_R texture.
		glTextureParameteri(m_texture_id, GL_TEXTURE_SWIZZLE_A, GL_RED);
	}

	glTextureStorage2D(m_texture_id, m_mipmap_levels, m_gl_format, m_size.x, m_size.y);
}

GSTextureOGL::~GSTextureOGL()
{
	// Textures aren't cleared from attachments on deletion.
	GSDeviceOGL::GetInstance()->OMUnbindTexture(this);

	// But they are unbound.
	for (GLuint& tex : GLState::tex_unit)
	{
		if (m_texture_id == tex)
			tex = 0;
	}

	glDeleteTextures(1, &m_texture_id);
}

void* GSTextureOGL::GetNativeHandle() const
{
	return reinterpret_cast<void*>(static_cast<uintptr_t>(m_texture_id));
}

extern int g_orbis_upload_mode; // eerec-279
// eerec-276 upload check
#include <unistd.h>
#include <unordered_map>
static std::unordered_map<const void*, const u8*> s_orbis_map_ptr;
static void orbis_upload_check(GSTextureOGL* t, const char* path, int layer, int rx, int ry, int rw, int rh, const u8* src, int src_pitch)
{
	static const bool s_upchk_on = (access("/data/PCSX2/upchk", 0) == 0); // eerec-278: off unless flag
	if (!s_upchk_on) return;
	static int n = 0, logged = 0, bad = 0, zero_all = 0;
	if (!src || n > 60000) return;
	n++;
	const int W = std::max(t->GetWidth() >> layer, 1), H = std::max(t->GetHeight() >> layer, 1);
	const int bpp = 1 << t->GetIntShift();
	while (glGetError() != GL_NO_ERROR) {}
	std::vector<u8> buf((size_t)W * H * bpp, 0xcd);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glGetTextureImage(t->GetID(), layer, t->GetIntFormat(), t->GetIntType(), (GLsizei)buf.size(), buf.data());
	int badpx = 0, zpx = 0; int fx = -1, fy = -1; u32 fg = 0, fe = 0;
	for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) {
		const int X = rx + x, Y = ry + y; if (X >= W || Y >= H) continue;
		const u8* g = &buf[((size_t)Y * W + X) * bpp]; const u8* e = src + (size_t)y * src_pitch + (size_t)x * bpp;
		bool z = true; for (int b = 0; b < bpp; b++) z &= g[b] == 0; zpx += z;
		if (memcmp(g, e, bpp)) { if (fx < 0) { fx = X; fy = Y; memcpy(&fg, g, std::min(bpp, 4)); memcpy(&fe, e, std::min(bpp, 4)); } badpx++; } }
	if (badpx) { bad++; zero_all += (zpx == rw * rh); }
	const GLenum rerr = glGetError();
	static std::unordered_map<u64, int> s_combo;
	const u64 key = ((u64)t->GetWidth() << 40) | ((u64)t->GetHeight() << 24) | ((u64)layer << 16) | ((u64)bpp << 8) | (u64)(path[0] + path[7]);
	int& seen = s_combo[key];
	if (rerr && logged < 400) { logged++; printf("[upchk] GLERR %x path=%s size=%dx%d lvl=%d\n", rerr, path, t->GetWidth(), t->GetHeight(), layer); }
	if (badpx && seen++ < 3 && logged < 400) { logged++;
		printf("[upchk] BAD path=%s tex=%u size=%dx%d lvl=%d/%d fmt=%x/%x bpp=%d rect=%d,%d %dx%d pitch=%d bad=%d/%d zero=%d first(%d,%d) got=%08x exp=%08x\n",
			path, t->GetID(), t->GetWidth(), t->GetHeight(), layer, t->GetMipmapLevels(), t->GetIntFormat(), t->GetIntType(), bpp, rx, ry, rw, rh, src_pitch, badpx, rw * rh, zpx, fx, fy, fg, fe);
		fflush(stdout); }
	if ((n % 2000) == 0) { printf("[upchk] checks=%d bad=%d allzero=%d combos=%zu\n", n, bad, zero_all, s_combo.size()); fflush(stdout); }
}

bool GSTextureOGL::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	pxAssert(!IsDepthStencil());

	if (layer >= m_mipmap_levels)
		return true;

	// Default upload path for the texture is the Map/Unmap
	// This path is mostly used for palette. But also for texture that could
	// overflow the pbo buffer
	// Data upload is rather small typically 64B or 1024B. So don't bother with PBO
	// and directly send the data to the GL synchronously
	GSDeviceOGL::GetInstance()->CommitClear(this, true);

	const u32 preferred_pitch = Common::AlignUpPow2(r.width() << m_int_shift, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
	const u32 map_size = r.height() * preferred_pitch;

#if 0
	if (r.height() == 1) {
		// Palette data. Transfer is small either 64B or 1024B.
		// Sometimes it is faster, sometimes slower.
		glTextureSubImage2D(m_texture_id, GL_TEX_LEVEL_0, r.x, r.y, r.width(), r.height(), m_int_format, m_int_type, data);
		return true;
	}
#endif

	GL_PUSH("Upload Texture %d", m_texture_id);
	g_perfmon.Put(GSPerfMon::TextureUploads, 1);

	// Don't use PBOs for huge texture uploads, let the driver sort it out.
	// Otherwise we'll just be syncing, or worse, crashing because the PBO routine above isn't great.
	GLStreamBuffer* const sb = GSDeviceOGL::GetInstance()->GetTextureUploadBuffer();
	if (IsCompressedFormat())
	{
		const u32 row_length = CalcUploadRowLengthFromPitch(pitch);
		const u32 upload_size = CalcUploadSize(r.height(), pitch);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
		glCompressedTextureSubImage2D(m_texture_id, layer, r.x, r.y, r.width(), r.height(), m_int_format, upload_size, data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
	else if (!sb || map_size > sb->GetChunkSize() || g_orbis_upload_mode != 0) // eerec-279: live.ini upload=
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch >> m_int_shift);
		if (g_orbis_upload_mode == 2)
		{
			GLint prev = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
			glBindTexture(GL_TEXTURE_2D, m_texture_id);
			glTexSubImage2D(GL_TEXTURE_2D, layer, r.x, r.y, r.width(), r.height(), m_int_format, m_int_type, data);
			glBindTexture(GL_TEXTURE_2D, prev);
		}
		else
			glTextureSubImage2D(m_texture_id, layer, r.x, r.y, r.width(), r.height(), m_int_format, m_int_type, data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // Restore default behavior
	}
	else
	{
		const auto map = sb->Map(TEXTURE_UPLOAD_ALIGNMENT, map_size);
		StringUtil::StrideMemCpy(map.pointer, preferred_pitch, data, pitch, r.width() << m_int_shift, r.height());
		sb->Unmap(map_size);
		sb->Bind();

		const u32 row_length = CalcUploadRowLengthFromPitch(preferred_pitch);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);

		glTextureSubImage2D(m_texture_id, layer, r.x, r.y, r.width(), r.height(), m_int_format, m_int_type,
			reinterpret_cast<void*>(static_cast<uintptr_t>(map.buffer_offset)));

		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		sb->Unbind();
	}

	if (!IsCompressedFormat()) orbis_upload_check(this, (!sb || map_size > sb->GetChunkSize()) ? "update-direct" : "update-sb", layer, r.x, r.y, r.width(), r.height(), static_cast<const u8*>(data), pitch);

	m_needs_mipmaps_generated = true;

	return true;
}

bool GSTextureOGL::Map(GSMap& m, const GSVector4i* _r, int layer)
{
	if (layer >= m_mipmap_levels || IsCompressedFormat())
		return false;

	GSDeviceOGL::GetInstance()->CommitClear(this, true);

	GSVector4i r = _r ? *_r : GSVector4i(0, 0, m_size.x, m_size.y);
	// Will need some investigation
	pxAssert(r.width() != 0);
	pxAssert(r.height() != 0);

	const u32 pitch = Common::AlignUpPow2(r.width() << m_int_shift, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
	m.pitch = pitch;

	if (IsTexture() || IsRenderTarget())
	{
		const u32 upload_size = CalcUploadSize(r.height(), pitch);
		GLStreamBuffer* sb = GSDeviceOGL::GetInstance()->GetTextureUploadBuffer();
		if (!sb || upload_size > sb->GetChunkSize())
			return false;

		GL_PUSH_("Upload Texture %d", m_texture_id); // POP is in Unmap
		g_perfmon.Put(GSPerfMon::TextureUploads, 1);

		const auto map = sb->Map(TEXTURE_UPLOAD_ALIGNMENT, upload_size);
		m.bits = static_cast<u8*>(map.pointer);
		s_orbis_map_ptr[this] = m.bits;

		// Save the area for the unmap
		m_r_x = r.x;
		m_r_y = r.y;
		m_r_w = r.width();
		m_r_h = r.height();
		m_layer = layer;
		m_map_offset = map.buffer_offset;

		return true;
	}

	return false;
}

void GSTextureOGL::Unmap()
{
	if (IsTexture() || IsRenderTarget())
	{
		GSDeviceOGL::GetInstance()->CommitClear(this, true);

		const u32 pitch = Common::AlignUpPow2(m_r_w << m_int_shift, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
		const u32 upload_size = pitch * m_r_h;
		GLStreamBuffer* sb = GSDeviceOGL::GetInstance()->GetTextureUploadBuffer();
		sb->Unmap(upload_size);
		sb->Bind();

		const u32 row_length = CalcUploadRowLengthFromPitch(pitch);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);

		glTextureSubImage2D(m_texture_id, m_layer, m_r_x, m_r_y, m_r_w, m_r_h, m_int_format, m_int_type,
			reinterpret_cast<void*>(static_cast<uintptr_t>(m_map_offset)));

		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		sb->Unbind();

		orbis_upload_check(this, "unmap", m_layer, m_r_x, m_r_y, m_r_w, m_r_h, s_orbis_map_ptr[this], (int)pitch);

		m_needs_mipmaps_generated = true;

		GL_POP(); // PUSH is in Map
	}
}

void GSTextureOGL::GenerateMipmap()
{
	pxAssert(m_mipmap_levels > 1);
	GSDeviceOGL::GetInstance()->CommitClear(this, true);
	glGenerateTextureMipmap(m_texture_id);
}

#ifdef PCSX2_DEVBUILD

void GSTextureOGL::SetDebugName(std::string_view name)
{
	if (name.empty())
		return;

	if (glObjectLabel)
		glObjectLabel(GL_TEXTURE, m_texture_id, static_cast<GLsizei>(name.length()), static_cast<const GLchar*>(name.data()));

	m_debug_name = name;
}

#endif

GSDownloadTextureOGL::GSDownloadTextureOGL(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
}

GSDownloadTextureOGL::~GSDownloadTextureOGL()
{
	if (m_buffer_id != 0)
	{
		if (m_sync)
			glDeleteSync(m_sync);

		if (m_map_pointer)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_id);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}

		glDeleteBuffers(1, &m_buffer_id);
	}
	else if (m_cpu_buffer)
	{
		_aligned_free(m_cpu_buffer);
	}
}

std::unique_ptr<GSDownloadTextureOGL> GSDownloadTextureOGL::Create(u32 width, u32 height, GSTexture::Format format)
{
	const u32 buffer_size = GetBufferSize(width, height, format, TEXTURE_UPLOAD_PITCH_ALIGNMENT);

	const bool use_buffer_storage = (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage) &&
	                                !GSDeviceOGL::GetInstance()->IsDownloadPBODisabled();
	if (use_buffer_storage)
	{
		GLuint buffer_id;
		glGenBuffers(1, &buffer_id);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_id);

		const u32 flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		const u32 map_flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;

		if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
			glBufferStorage(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, flags);
		else if (GLAD_GL_EXT_buffer_storage)
			glBufferStorageEXT(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, flags);

		u8* buffer_map = static_cast<u8*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, buffer_size, map_flags));

		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

		if (!buffer_map)
		{
			Console.Error("Failed to map persistent download buffer");
			glDeleteBuffers(1, &buffer_id);
			return {};
		}

		std::unique_ptr<GSDownloadTextureOGL> ret(new GSDownloadTextureOGL(width, height, format));
		ret->m_buffer_id = buffer_id;
		ret->m_buffer_size = buffer_size;
		ret->m_map_pointer = buffer_map;
		return ret;
	}

	// Fallback to glReadPixels() + CPU buffer.
	u8* cpu_buffer = static_cast<u8*>(_aligned_malloc(buffer_size, VECTOR_ALIGNMENT));
	if (!cpu_buffer)
		return {};

	std::unique_ptr<GSDownloadTextureOGL> ret(new GSDownloadTextureOGL(width, height, format));
	ret->m_cpu_buffer = cpu_buffer;
	ret->m_map_pointer = cpu_buffer;
	return ret;
}

struct ps5_copyprof_t { unsigned long long n = 0, ticks = 0, px = 0, last = 0; unsigned long long maxt = 0; int mw = 0, mh = 0; };
extern ps5_copyprof_t ps5_readprof;
void ps5_copyprof_report();
struct ps5_copyprof_scope
{
	ps5_copyprof_t& p; unsigned long long t0; int w, h;
	ps5_copyprof_scope(ps5_copyprof_t& p_, int w_, int h_) : p(p_), t0(__builtin_ia32_rdtsc()), w(w_), h(h_) {}
	~ps5_copyprof_scope()
	{
		const unsigned long long dt = __builtin_ia32_rdtsc() - t0;
		p.n++; p.ticks += dt; p.px += (unsigned long long)w * h;
		if (dt > p.maxt) { p.maxt = dt; p.mw = w; p.mh = h; }
		ps5_copyprof_report();
	}
};

void GSDownloadTextureOGL::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureOGL* const glTex = static_cast<GSTextureOGL*>(stex);
	ps5_copyprof_scope ps5_rps(ps5_readprof, src.width(), src.height());
	GSDeviceOGL::GetInstance()->CommitClear(glTex, true);

	pxAssert(glTex->GetFormat() == m_format);
	pxAssert(drc.width() == src.width() && drc.height() == src.height());
	pxAssert(src.z <= glTex->GetWidth() && src.w <= glTex->GetHeight());
	pxAssert(static_cast<u32>(drc.z) <= m_width && static_cast<u32>(drc.w) <= m_height);
	pxAssert(src_level < static_cast<u32>(glTex->GetMipmapLevels()));
	pxAssert((drc.left == 0 && drc.top == 0) || !use_transfer_pitch);

	u32 copy_offset, copy_size, copy_rows;
	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);
	g_perfmon.Put(GSPerfMon::Readbacks, 1);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, GSDeviceOGL::GetInstance()->GetFBORead());
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex->GetID(), 0);

	glPixelStorei(GL_PACK_ALIGNMENT, 1u << glTex->GetIntShift());
	glPixelStorei(GL_PACK_ROW_LENGTH, GSTexture::CalcUploadRowLengthFromPitch(m_format, m_current_pitch));

	if (!m_cpu_buffer)
	{
		// Read to PBO.
		glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_id);
	}

	glReadPixels(src.left, src.top, src.width(), src.height(), glTex->GetIntFormat(), glTex->GetIntType(), m_cpu_buffer + copy_offset);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	if (m_cpu_buffer)
	{
		// If using CPU buffers, we never need to flush.
		m_needs_flush = false;
	}
	else
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

		// Create a sync object so we know when the GPU is done copying.
		if (m_sync)
			glDeleteSync(m_sync);

		m_sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		m_needs_flush = true;
	}

	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

bool GSDownloadTextureOGL::Map(const GSVector4i& read_rc)
{
	// Either always mapped, or CPU buffer.
	return true;
}

void GSDownloadTextureOGL::Unmap()
{
	// Either always mapped, or CPU buffer.
}

void GSDownloadTextureOGL::Flush()
{
	// If we're using CPU buffers, we did the readback synchronously...
	if (!m_needs_flush || !m_sync)
		return;

	m_needs_flush = false;

	glClientWaitSync(m_sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
	glDeleteSync(m_sync);
	m_sync = {};
}

#ifdef PCSX2_DEVBUILD

void GSDownloadTextureOGL::SetDebugName(std::string_view name)
{
	if (name.empty())
		return;

	if (glObjectLabel)
		glObjectLabel(GL_BUFFER, m_buffer_id, name.length(), name.data());
}

#endif