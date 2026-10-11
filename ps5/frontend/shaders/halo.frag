#version 450
#extension GL_GOOGLE_include_directive : require
// PS5 port frontend: a rounded-rectangle outline with a soft glow, added to what is behind.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.glsl"

layout(push_constant) uniform Push
{
	mat4 model;
	vec4 size;
	vec4 color;
	vec4 shape;
	vec4 unused;
} pc;

layout(location = 0) in vec2 v_local;
layout(location = 0) out vec4 o_color;

float rounded_box(vec2 p, vec2 b, float r)
{
	vec2 q = abs(p) - b + vec2(r);
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
	float d = rounded_box(v_local, pc.size.xy + vec2(pc.shape.y * 1.5), pc.shape.x);
	float line = 1.0 - smoothstep(pc.shape.y * 0.35, pc.shape.y, abs(d));
	float glow = exp(-max(d, 0.0) / pc.shape.z) * step(0.0, d) * 0.55;
	float a = (line + glow) * pc.color.w;
	o_color = vec4(pc.color.rgb * a, a);
}
