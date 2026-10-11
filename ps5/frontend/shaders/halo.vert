#version 450
#extension GL_GOOGLE_include_directive : require
// PS5 port frontend: the outline glow around the selected box's front, a quad in its front plane.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.glsl"

layout(push_constant) uniform Push
{
	mat4 model;
	vec4 size;   // half width, half height, depth of the plane (z), margin
	vec4 color;  // rgb, intensity
	vec4 shape;  // x = corner radius, y = line width, z = glow width, w = unused
	vec4 unused;
} pc;

layout(location = 0) in vec2 a_corner; // -1..1
layout(location = 0) out vec2 v_local;

void main()
{
	vec2 c = a_corner;
	vec2 half_size = pc.size.xy + vec2(pc.size.w);
	v_local = c * half_size;
	gl_Position = u_view_proj * (pc.model * vec4(v_local, pc.size.z, 1.0));
}
