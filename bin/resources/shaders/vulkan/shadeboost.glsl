// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//#version 420 // Keep it for editor detection

// ORBIS_PRESHARP (PS5 port, vk-285-12): the ShadeBoost pass is repurposed as a
// native-resolution sharpen (cross unsharp mask) that runs before the upscale, as in the
// port's GL shadeboost.glsl (eerec-278). The eboot enables ShadeBoost on Vulkan only when
// this file carries the ORBIS_PRESHARP marker, so the stock shader never desaturates.
// Strength = params.z = ShadeBoost_Saturation / 50 (e.g. 25 -> 0.5). 0 = passthrough.
// params.xy = 1 / texture size, set by the eboot (GSDevice::ShadeBoost): the PS5 shader
// compiler has no image queries, so textureSize() is not available.

#ifdef VERTEX_SHADER

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_tex;

layout(location = 0) out vec2 v_tex;

void main()
{
	gl_Position = vec4(a_pos.x, -a_pos.y, a_pos.z, a_pos.w);
	v_tex = a_tex;
}

#endif

#ifdef FRAGMENT_SHADER

layout(push_constant) uniform cb0
{
	vec4 params;
};

layout(set = 0, binding = 0) uniform sampler2D samp0;
layout(location = 0) in vec2 v_tex;
layout(location = 0) out vec4 o_col0;

vec3 sb_load(ivec2 p, ivec2 hi, vec2 rcp_size)
{
	return textureLod(samp0, (vec2(clamp(p, ivec2(0), hi)) + 0.5) * rcp_size, 0.0).rgb;
}

void main()
{
	vec2 rcp_size = params.xy;
	ivec2 sz = ivec2(round(1.0 / max(rcp_size, vec2(1.0e-6))));
	ivec2 hi = sz - ivec2(1);
	ivec2 p = clamp(ivec2(v_tex * vec2(sz)), ivec2(0), hi);
	vec4 c = textureLod(samp0, (vec2(p) + 0.5) * rcp_size, 0.0);
	vec3 nb = sb_load(p + ivec2(-1, 0), hi, rcp_size) + sb_load(p + ivec2(1, 0), hi, rcp_size) +
	          sb_load(p + ivec2(0, -1), hi, rcp_size) + sb_load(p + ivec2(0, 1), hi, rcp_size);
	float k = clamp(params.z, 0.0, 2.0);
	vec3 s = c.rgb + k * (c.rgb - 0.25 * nb);
	o_col0 = vec4(clamp(s, 0.0, 1.0), c.a);
}

#endif
