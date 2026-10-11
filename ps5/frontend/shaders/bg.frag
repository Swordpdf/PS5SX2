#version 450
#extension GL_GOOGLE_include_directive : require
// PS5 port frontend: the background: a deep blue-violet gradient, a soft glow behind the selected
// box tinted by its cover, and a thin glow on the floor under it.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.glsl"

layout(push_constant) uniform Push
{
	vec4 glow_color;  // rgb, w = strength
	vec4 glow_pos;    // x, y (0..1 of the screen), radius x, radius y (fractions of the height)
	vec4 floor_glow;  // x, y, radius x, radius y
	vec4 col_top;
	vec4 col_mid;
	vec4 col_bottom;
	vec4 misc;        // x = aspect (w/h), y = horizon (0..1), z = vignette, w = time
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main()
{
	vec2 uv = v_uv;
	// Vertical gradient through three stops.
	float h = pc.misc.y;
	vec3 col = uv.y < h ? mix(pc.col_top.rgb, pc.col_mid.rgb, smoothstep(0.0, h, uv.y))
	                    : mix(pc.col_mid.rgb, pc.col_bottom.rgb, smoothstep(h, 1.0, uv.y));

	// The glow behind the selection: an ellipse in height units, with a soft core and a wide skirt.
	vec2 d = (uv - pc.glow_pos.xy) * vec2(pc.misc.x, 1.0);
	float r = length(d / pc.glow_pos.zw);
	float core = exp(-r * r * 2.2);
	float skirt = exp(-r * 1.6) * 0.55;
	col += pc.glow_color.rgb * (core * 0.85 + skirt) * pc.glow_color.w;

	// A faint secondary bloom of the same colour, higher up, like light spilling from behind.
	vec2 d2 = (uv - (pc.glow_pos.xy - vec2(0.0, pc.glow_pos.w * 0.55))) * vec2(pc.misc.x, 1.0);
	col += pc.glow_color.rgb * exp(-dot(d2, d2) * 5.0) * 0.18 * pc.glow_color.w;

	// The floor glow: a flat ellipse under the selected box.
	vec2 f = (uv - pc.floor_glow.xy) * vec2(pc.misc.x, 1.0);
	float fr = length(f / pc.floor_glow.zw);
	col += mix(pc.glow_color.rgb, vec3(1.0), 0.35) * exp(-fr * fr * 3.0) * 0.22 * pc.glow_color.w;

	// Vignette.
	vec2 c = (uv - 0.5) * vec2(pc.misc.x, 1.0);
	col *= 1.0 - pc.misc.z * smoothstep(0.35, 1.25, length(c));

	col += fe_dither(gl_FragCoord.xy) / 255.0;
	o_color = vec4(max(col, vec3(0.0)), 1.0);
}
