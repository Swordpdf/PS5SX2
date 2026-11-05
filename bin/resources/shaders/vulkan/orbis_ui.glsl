// PS5 port (2.31, AI-assisted): PS5SX2's own UI over the game, the in-game menu (ps5/frontend/fe_ingame.h) and the hold ring.
// The shelf's UI shapes (ps5/frontend/shaders/ui.frag: SDF text, rounded rectangles, RGBA images) and three more: a rounded
// rectangle whose edge fades over s pixels (glows, shadows), an image with rounded corners (the paused picture, the save
// states' screenshots) and a rounded outline. The vertices are fe::UiVertex (pixels, uv, RGBA8, four parameters; p3 the
// shape, fe_ingame.h ui_mode). ps5/frontend/host/ingame_preview.cpp draws the same on a PC: keep the two the same.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

layout(push_constant) uniform PushConstants
{
	vec2 u_scale;     // pixels -> clip space
	vec2 u_translate;
	vec2 u_uv_scale;  // the texture's part an image's 0..1 covers (the paused picture's place in the GS's frame)
	vec2 u_uv_offset;
};

#ifdef VERTEX_SHADER

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_params;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_params;

void vs_main()
{
	v_uv = a_uv;
	v_color = a_color;
	v_params = a_params;
	gl_Position = vec4(a_pos * u_scale + u_translate, 0.0f, 1.0f);
}

#endif

#ifdef FRAGMENT_SHADER

layout(binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_params;

layout(location = 0) out vec4 o_color;

// A rounded rectangle's signed distance: q pixels from its centre, h its half size, r its corners.
float rect_d(vec2 q, vec2 h, float r)
{
	vec2 d = abs(q) - h + vec2(r);
	return length(max(d, vec2(0.0f))) + min(max(d.x, d.y), 0.0f) - r;
}

void ps_main()
{
	// Sampled before any branch on purpose: the PS5's shader compiler moves a sample's coordinate out of divergent control
	// flow and builds it from the input's first component twice (vk-285-41, ps5/frontend/shaders/ui.frag).
	vec4 s = texture(u_tex, v_uv * u_uv_scale + u_uv_offset);
	float mode = v_params.w;
	vec3 rgb = v_color.rgb;
	float a = 1.0f;
	if (mode < 0.5f)
	{
		// SDF text: x the edge, y the softness, in field units. The atlas is a GSTexture::Format::UNorm8, which GSTextureVK views
		// as (0, 0, 0, R): its value is in alpha (s.r is 0 there; 188 read it and drew no text on the console).
		a = smoothstep(v_params.x - v_params.y, v_params.x + v_params.y, s.a);
	}
	else if (mode < 1.5f)
	{
		// rounded rectangle: uv pixels from the centre
		a = 1.0f - smoothstep(-0.75f, 0.75f, rect_d(v_uv, v_params.xy, v_params.z));
	}
	else if (mode < 2.5f)
	{
		// RGBA image
		rgb = s.rgb * v_color.rgb;
		a = s.a;
	}
	else if (mode < 3.5f)
	{
		// soft rectangle: solid inside, gone `soft` pixels out
		float soft = max((mode - 3.0f) * 4096.0f, 1.0f);
		float t = clamp(rect_d(v_uv, v_params.xy, v_params.z) / soft, 0.0f, 1.0f);
		a = (1.0f - t) * (1.0f - t);
	}
	else if (mode < 4.5f)
	{
		// image with rounded corners: uv 0..1 over it, opaque
		vec2 q = (v_uv - vec2(0.5f)) * 2.0f * v_params.xy;
		a = 1.0f - smoothstep(-0.75f, 0.75f, rect_d(q, v_params.xy, v_params.z));
		rgb = s.rgb * v_color.rgb;
	}
	else
	{
		// rounded outline, t pixels thick, inside the edge
		float t = (mode - 5.0f) * 4096.0f;
		float d = rect_d(v_uv, v_params.xy, v_params.z);
		a = 1.0f - smoothstep(-0.75f, 0.75f, abs(d + t * 0.5f) - t * 0.5f);
	}
	o_color = vec4(rgb, v_color.a * a);
}

#endif
