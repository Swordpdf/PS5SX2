#version 450
#extension GL_GOOGLE_include_directive : require
// PS5 port frontend: a PS2 case: the cover under a glossy sleeve on the front, the spine on the
// left, black plastic elsewhere. Reflections (p0.z = 1) fade out below the floor.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.glsl"

layout(push_constant) uniform Push
{
	mat4 model;
	vec4 p0;
	vec4 p1;
	vec4 p2;
	vec4 p3;
} pc;

layout(set = 1, binding = 0) uniform sampler2D u_cover;
layout(set = 1, binding = 1) uniform sampler2D u_placeholder;
layout(set = 1, binding = 2) uniform sampler2D u_spine;

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) flat in int v_face;
layout(location = 0) out vec4 o_color;

void main()
{
	bool reflection = pc.p0.z > 0.5;
	vec3 n = normalize(v_normal);
	vec3 v = normalize(u_cam_pos.xyz - v_world);
	if (reflection)
		n.y = -n.y; // the mirrored box is lit as the real one is

	// All three sampled before any branch: inside one, the PS5 shader compiler (psbc) read the
	// coordinate as (u, u), so covers and spines came out as vertical stripes (vk-285-41; see
	// ui.frag).
	vec3 cover = texture(u_cover, v_uv).rgb;
	vec3 hold = texture(u_placeholder, v_uv).rgb;
	vec3 spine = texture(u_spine, v_uv).rgb;

	vec3 base;
	float gloss;
	if (v_face == 0)
	{
		base = mix(hold, cover, pc.p0.y);
		gloss = 1.0;
	}
	else if (v_face == 2)
	{
		base = spine;
		gloss = 0.85;
	}
	else if (v_face == 1)
	{
		base = vec3(0.030, 0.030, 0.036);
		gloss = 0.6;
	}
	else
	{
		base = vec3(0.020, 0.020, 0.025);
		gloss = 0.45;
	}

	// Key light from the upper left, a weaker fill from the right, and some ambient.
	vec3 l1 = normalize(vec3(-0.45, 0.55, 0.70));
	vec3 l2 = normalize(vec3(0.65, 0.15, 0.75));
	float ndl1 = max(dot(n, l1), 0.0);
	float ndl2 = max(dot(n, l2), 0.0);
	vec3 col = base * (0.46 + 0.62 * ndl1 + 0.16 * ndl2);

	// The sleeve: a sharp highlight, a soft studio light in the reflection, and Fresnel.
	float ndv = max(dot(n, v), 0.0);
	float fres = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
	vec3 h = normalize(l1 + v);
	float spec = pow(max(dot(n, h), 0.0), 120.0) * 0.45;
	vec3 r = reflect(-v, n);
	float soft = smoothstep(0.62, 0.97, dot(r, normalize(vec3(-0.30, 0.80, 0.52))));
	float env = (soft * 0.28 + 0.02) * mix(0.35, 1.0, fres);
	col += vec3(spec + env) * gloss;

	// A sweep of light across the front when the box is picked.
	if (v_face == 0 && pc.p1.y > 0.0)
	{
		float band = v_uv.x * 0.8 + (1.0 - v_uv.y) * 0.45 - pc.p1.x;
		col += vec3(exp(-band * band * 40.0) * pc.p1.y);
	}

	// Rim light in the glow colour on the selected box's edges.
	float rim = pow(1.0 - ndv, 3.0);
	col += u_glow.rgb * rim * 0.45 * pc.p0.w;

	col *= pc.p0.x;

	float alpha = pc.p1.z;
	if (reflection)
	{
		float below = max(u_floor.x - v_world.y, 0.0);
		alpha *= u_floor.y * exp(-below * u_floor.z);
		col *= 0.85;
	}
	o_color = vec4(clamp(col, 0.0, 1.0), alpha);
}
