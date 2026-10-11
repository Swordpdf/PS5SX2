#version 450
// PS5 port frontend: FXAA over the rendered scene, into the display image.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

layout(push_constant) uniform Push
{
	vec4 texel;  // 1/width, 1/height, fade (0 black .. 1 full), unused
} pc;

layout(set = 1, binding = 0) uniform sampler2D u_scene;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
	// FXAA 3.11-style edge search, compact (the texel size comes from the push constants: the PS5
	// shader compiler refuses image queries).
	vec2 t = pc.texel.xy;
	vec3 rgbM = texture(u_scene, v_uv).rgb;
	float lumaM = luma(rgbM);
	float lumaNW = luma(texture(u_scene, v_uv + vec2(-1.0, -1.0) * t).rgb);
	float lumaNE = luma(texture(u_scene, v_uv + vec2(1.0, -1.0) * t).rgb);
	float lumaSW = luma(texture(u_scene, v_uv + vec2(-1.0, 1.0) * t).rgb);
	float lumaSE = luma(texture(u_scene, v_uv + vec2(1.0, 1.0) * t).rgb);
	float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
	float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
	vec3 result = rgbM;
	if (lumaMax - lumaMin > max(0.0312, lumaMax * 0.125))
	{
		vec2 dir;
		dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
		dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));
		float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);
		float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
		dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * t;
		vec3 rgbA = 0.5 * (texture(u_scene, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
		                   texture(u_scene, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
		vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_scene, v_uv + dir * -0.5).rgb +
		                                 texture(u_scene, v_uv + dir * 0.5).rgb);
		float lumaB = luma(rgbB);
		result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
	}
	o_color = vec4(result * pc.texel.z, 1.0);
}
