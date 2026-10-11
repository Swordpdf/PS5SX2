#version 450
// PS5 port frontend: signed-distance-field glyphs (params.w = 0: x = edge, y = softness in field
// units) and rounded rectangles (params.w = 1: uv = pixels from the centre, x/y = half size,
// z = corner radius, softness one pixel).
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

layout(set = 1, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_params;
layout(location = 0) out vec4 o_color;

void main()
{
	// Sampled outside the branch on purpose: the PS5 shader compiler (psbc) moves the coordinate of
	// a sample inside divergent control flow to the top and builds it from the input's first
	// component twice, so the atlas was read at (u, u) and every glyph came out as bars (vk-285-41).
	vec4 sample_color = texture(u_atlas, v_uv);
	float field = sample_color.r;
	float a;
	if (v_params.w < 0.5)
	{
		a = smoothstep(v_params.x - v_params.y, v_params.x + v_params.y, field);
	}
	else
	{
		vec2 q = abs(v_uv) - v_params.xy + vec2(v_params.z);
		float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_params.z;
		a = 1.0 - smoothstep(-0.75, 0.75, d);
	}
	// Mode 2 draws RGBA achievement badges using the same vertex pipeline (AI-assisted).
	o_color = v_params.w > 1.5 ? sample_color * v_color : vec4(v_color.rgb, v_color.a * a);
}
