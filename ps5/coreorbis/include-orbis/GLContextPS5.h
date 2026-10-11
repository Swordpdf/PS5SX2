// PS5 OpenGL context for PCSX2, backed by ps5-opengl's static EGL/GL 3.3 Core.
#pragma once

#include "GS/Renderers/OpenGL/GLContext.h"

#include <span>

class GLContextPS5 final : public GLContext
{
public:
	GLContextPS5(const WindowInfo& wi);
	~GLContextPS5() override;

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error);

	void* GetProcAddress(const char* name) override;
	bool ChangeSurface(const WindowInfo& new_wi) override;
	void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
	bool SwapBuffers() override;
	bool IsCurrent() override;
	bool MakeCurrent() override;
	bool DoneCurrent() override;
	bool SupportsNegativeSwapInterval() const override;
	bool SetSwapInterval(s32 interval) override;
	std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

private:
	bool Initialize(Error* error);
	void DestroyContext();

	void* m_display = nullptr; // EGLDisplay
	void* m_surface = nullptr; // EGLSurface
	void* m_context = nullptr; // EGLContext
	bool m_current = false;
};
