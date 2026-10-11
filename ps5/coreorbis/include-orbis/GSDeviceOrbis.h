// Orbis CPU-only GSDevice + GSTexture for the Software renderer.
// Presents the SW-rendered frame to a shared display buffer (demo_renderer).
#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include <cstdio>
#include <vector>
#include <cstring>

extern "C" void orbis_present_frame(const unsigned char* pixels, int w, int h, int pitch);

class GSTextureCPU final : public GSTexture
{
public:
	GSTextureCPU(int width, int height, GSTexture::Format fmt, GSTexture::Usage usage)
	{
		m_size = GSVector2i(width, height);
		m_format = fmt;
		m_usage = usage;
		m_pitch = width * (fmt == GSTexture::Format::UNorm8 ? 1 : 4);
		m_data.resize(static_cast<size_t>(m_pitch) * height);
	}

	void* GetNativeHandle() const override { return const_cast<u8*>(m_data.data()); }

	GSTexture::Format GetFormat2() const { return m_format; }
	GSVector2i GetSize2() const { return m_size; }

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override
	{
		{
			static unsigned long long updates = 0;
			unsigned long long n = updates++;
			if (n < 4 || (n % 2000) == 0)
			{
				const u32* src32 = static_cast<const u32*>(data);
				printf("[dbg] CPUtex Update#%llu: this=%p rect=(%d,%d,%d,%d) pitch=%d src0=%08x\n",
					n, (void*)this, r.x, r.y, r.z, r.w, pitch, src32 ? src32[0] : 0xdeadbeef);
				fflush(stdout);
			}
		}
		const int bpp = (m_format == GSTexture::Format::UNorm8) ? 1 : 4;
		for (int y = r.y; y < r.w; y++)
		{
			if (y < 0 || y >= m_size.y)
				continue;
			std::memcpy(m_data.data() + static_cast<size_t>(y) * m_pitch + static_cast<size_t>(r.x) * bpp,
				static_cast<const u8*>(data) + static_cast<size_t>(y - r.y) * pitch, static_cast<size_t>(r.z - r.x) * bpp);
		}
		return true;
	}

	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override
	{
		m.bits = m_data.data();
		m.pitch = m_pitch;
		return true;
	}

	void Unmap() override {}
	void GenerateMipmap() override {}

	std::vector<u8> m_data;
	int m_pitch = 0;
};

class GSDeviceOrbis final : public GSDevice
{
public:
	explicit GSDeviceOrbis(bool headless = false)
		: m_headless(headless)
	{
		m_max_texture_size = 8192;
	}

	~GSDeviceOrbis() override = default;

	bool Create(GSVSyncMode, bool) override
	{
		m_max_texture_size = 8192;
		return true;
	}
	void Destroy() override {}

	GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override
	{
		{
			static int count = 0;
			if (count < 10 || (count++ % 500) == 0)
			{
				printf("[dbg] CreateSurface[%d]: %dx%d fmt=%d usage=%d\\n", count++, width, height, (int)format, (int)usage);
				fflush(stdout);
			}
		}
		if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
			return nullptr;
		return new GSTextureCPU(width, height, format, usage);
	}

	RenderAPI GetRenderAPI() const override { return RenderAPI::Vulkan; }
	bool HasSurface() const override { return true; }
	void DestroySurface() override {}
	bool UpdateWindow() override { return true; }
	void ResizeWindow(u32, u32, float) override {}
	bool SupportsExclusiveFullscreen() const override { return false; }

	PresentResult BeginPresent(bool) override
	{
		{
			static bool logged = false;
			if (!logged) { logged = true; printf("[dbg] BeginPresent called\\n"); fflush(stdout); }
		}
		return PresentResult::OK;
	}
	void EndPresent() override {}
	void SetVSyncMode(GSVSyncMode, bool) override {}
	std::string GetDriverInfo() const override { return "Orbis CPU (software)"; }
	bool SetGPUTimingEnabled(bool) override { return false; }
	float GetAndResetAccumulatedGPUTime() override { return 0.0f; }
	bool SetGPUPipelineStatisticsEnabled(bool) override { return false; }
	GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override { return {}; }

	void PushDebugGroup(const char*, ...) override {}
	void PopDebugGroup() override {}
	void InsertDebugMessage(DebugMessageCategory, const char*, ...) override {}

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32, u32, GSTexture::Format) override { return nullptr; }

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override
	{
		GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
		GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
		if (!src || !dst)
			return;
		const int bpp = (src->GetFormat2() == GSTexture::Format::UNorm8) ? 1 : 4;
		for (int y = r.y; y < r.w; y++)
		{
			if (y < 0 || y >= src->GetSize2().y)
				continue;
			const int dy = destY + (y - r.y);
			if (dy < 0 || dy >= dst->GetSize2().y)
				continue;
			std::memcpy(dst->m_data.data() + static_cast<size_t>(dy) * dst->m_pitch + static_cast<size_t>(destX) * bpp,
				src->m_data.data() + static_cast<size_t>(y) * src->m_pitch + static_cast<size_t>(r.x) * bpp,
				static_cast<size_t>(r.z - r.x) * bpp);
		}
	}

	void PresentRect(GSTexture* sTex, const GSVector4&, GSTexture*, const GSVector4&, PresentShader, float, Filter) override
	{
		if (m_headless)
			return;
		GSTextureCPU* tex = static_cast<GSTextureCPU*>(sTex);
		if (!tex)
			return;
		static unsigned long long present_count = 0;
		const unsigned long long current_count = present_count++;
		if (current_count < 4 || (current_count % 100) == 0)
		{
			unsigned nz = 0;
			const u32* words = reinterpret_cast<const u32*>(tex->m_data.data());
			size_t nwords = tex->m_data.size() / 4;
			for (size_t k = 0; k < nwords; k += 7)
				if (words[k]) { nz++; if (nz > 4) break; }
			printf("[dbg] PresentRect[%llu] tex=%p data=%p size=%dx%d pitch=%d nzsample=%u\n",
				current_count, (void*)tex, (void*)tex->m_data.data(),
				tex->GetSize2().x, tex->GetSize2().y, tex->m_pitch, nz);
			fflush(stdout);
		}
		orbis_present_frame(tex->m_data.data(), tex->GetSize2().x, tex->GetSize2().y, tex->m_pitch);
	}

	void DoMerge(GSTexture* sTex[3], GSVector4*, GSTexture* dTex, GSVector4*, const GSRegPMODE& pmode, const GSRegEXTBUF&, u32, const Filter) override
	{
		if (m_headless)
			return;
		{
			static bool logged = false;
			if (!logged) { logged = true; printf("[dbg] DoMerge called\\n"); fflush(stdout); }
		}
		// Orbis: sTex[0]/[1] are the two PCRTC displays' outputs. Stock takes
		// [0], but when only display 1 is enabled [0] is a stale empty surface
		// (black screen with live pixels in [1]). Prefer the enabled display.
		GSTexture* live = nullptr;
		if (pmode.EN2)
			live = sTex[1];
		if (!live && pmode.EN1)
			live = sTex[0];
		if (!live)
			live = sTex[1] ? sTex[1] : sTex[0];
		{
			static unsigned long long merges = 0;
			unsigned long long n = merges++;
			if (n < 3 || (n % 200) == 0)
			{
				printf("[dbg] DoMerge#%llu: sTex0=%p sTex1=%p sTex2=%p EN1=%d EN2=%d picked=%p dTex=%p\n",
					n, (void*)sTex[0], (void*)sTex[1], (void*)sTex[2],
					(int)pmode.EN1, (int)pmode.EN2, (void*)live, (void*)dTex);
				fflush(stdout);
			}
		}
		m_current = live;
		// Orbis: the base Merge() afterwards sets m_current = m_merge (dTex),
		// and Interlace() redirects to m_weavebob/m_blend. Those stay empty
		// because the shader stages below are stubs, so carry the pixels
		// forward with a CPU blit now (and in DoInterlace below).
		if (live && dTex)
			cpu_blit(live, dTex);
	}
	static void cpu_blit(GSTexture* sTex, GSTexture* dTex)
	{
		GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
		GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
		if (!src || !dst)
			return;
		const int w = src->GetSize2().x < dst->GetSize2().x ? src->GetSize2().x : dst->GetSize2().x;
		const int h = src->GetSize2().y < dst->GetSize2().y ? src->GetSize2().y : dst->GetSize2().y;
		if (w <= 0 || h <= 0)
			return;
		for (int y = 0; y < h; y++)
		{
			std::memcpy(dst->m_data.data() + static_cast<size_t>(y) * dst->m_pitch,
				src->m_data.data() + static_cast<size_t>(y) * src->m_pitch,
				static_cast<size_t>(w) * 4);
		}
	}
	void DoInterlace(GSTexture* sTex, const GSVector4&, GSTexture* dTex, const GSVector4&, ShaderInterlace, Filter, const InterlaceConstantBuffer&) override
	{
		// Orbis: no interlace shaders here; weave-equivalent full copy so the
		// m_weavebob/m_blend targets the base pipeline selects stay live.
		if (sTex && dTex)
			cpu_blit(sTex, dTex);
	}
	void DoFXAA(GSTexture*, GSTexture*) override {}
	void DoShadeBoost(GSTexture*, GSTexture*, const float[4]) override {}
	bool DoCAS(GSTexture*, GSTexture*, bool, const std::array<u32, NUM_CAS_CONSTANTS>&) override { return false; }
	void DoStretchRect(GSTexture*, const GSVector4&, GSTexture*, const GSVector4&, ShaderConvertSelector, Filter) override {}
	void UpdateCLUTTexture(GSTexture*, float, u32, u32, GSTexture*, u32, u32) override {}
	void ConvertToIndexedTexture(GSTexture*, float, u32, u32, u32, u32, GSTexture*, u32, u32) override {}
	void FilteredDownsampleTexture(GSTexture*, GSTexture*, u32, const GSVector2i&, const GSVector4&) override {}
	void RenderHW(GSHWDrawConfig&) override {}
	void ClearSamplerCache() override {}

private:
	bool m_headless = false;
};
