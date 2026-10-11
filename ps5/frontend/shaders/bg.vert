#version 450
// PS5 port frontend: a full-screen triangle (its three corners come from a vertex buffer).
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
layout(location = 0) in vec2 a_p;
layout(location = 0) out vec2 v_uv;
void main()
{
	v_uv = a_p;
	gl_Position = vec4(a_p * 2.0 - 1.0, 0.0, 1.0);
}
