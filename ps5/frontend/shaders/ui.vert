#version 450
#extension GL_GOOGLE_include_directive : require
// PS5 port frontend: text and shapes in screen pixels.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.glsl"

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_params;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_params;

void main()
{
	v_uv = a_uv;
	v_color = a_color;
	v_params = a_params;
	gl_Position = vec4(a_pos * u_screen.zw * 2.0 - 1.0, 0.0, 1.0);
}
