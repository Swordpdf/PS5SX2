// PS5 port frontend: what every shader shares.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

layout(set = 0, binding = 0) uniform Frame
{
	mat4 u_view_proj;
	vec4 u_cam_pos;   // xyz, w = time in seconds
	vec4 u_glow;      // rgb glow colour, w = strength
	vec4 u_floor;     // x = floor height, y = reflection strength, z = reflection falloff, w = unused
	vec4 u_screen;    // width, height, 1/width, 1/height
};

// Screen-space dither against banding in the dark gradients (+-0.5 of an 8-bit step).
float fe_dither(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
}
