#version 450
#extension GL_GOOGLE_include_directive : require
// PS5 port frontend: a PS2 case.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.glsl"

layout(push_constant) uniform Push
{
	mat4 model;
	vec4 p0;   // x = brightness, y = cover mix (0 placeholder .. 1 cover), z = reflection (0/1), w = selected (0..1)
	vec4 p1;   // x = sheen position, y = sheen strength, z = alpha, w = unused
	vec4 p2;   // unused
	vec4 p3;   // unused
} pc;

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in float a_face;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) flat out int v_face;

void main()
{
	vec4 world = pc.model * vec4(a_pos, 1.0);
	v_world = world.xyz;
	v_normal = mat3(pc.model) * a_normal;
	v_uv = a_uv;
	v_face = int(a_face + 0.5);
	gl_Position = u_view_proj * world;
}
