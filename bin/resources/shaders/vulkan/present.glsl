// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

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

layout(push_constant) uniform cb10
{
	vec4 u_source_rect;
	vec4 u_target_rect;
	vec2 u_source_size;
	vec2 u_target_size;
	vec2 u_target_resolution;
	vec2 u_rcp_target_resolution; // 1 / u_target_resolution
	vec2 u_source_resolution;
	vec2 u_rcp_source_resolution; // 1 / u_source_resolution
	float u_time;
	float u_orbis_sharp; // PS5 port (vk-285-12): DisplayConstantBuffer TimeAndPad.y, RCAS amount
	float u_orbis_split; // TimeAndPad.z, split-screen x in target pixels (0 = off)
};

layout(location = 0) in vec2 v_tex;

layout(location = 0) out vec4 o_col0;

layout(set = 0, binding = 0) uniform sampler2D samp0;

vec4 sample_c(vec2 uv)
{
	return texture(samp0, uv);
}

vec4 ps_crt(uint i)
{
	vec4 mask[4] = vec4[4](
		vec4(1, 0, 0, 0),
		vec4(0, 1, 0, 0),
		vec4(0, 0, 1, 0),
		vec4(1, 1, 1, 0));
	return sample_c(v_tex) * clamp((mask[i] + 0.5f), 0.0f, 1.0f);
}

vec4 ps_scanlines(uint i)
{
	vec4 mask[2] =
		{
			vec4(1, 1, 1, 0),
			vec4(0, 0, 0, 0)};

	return sample_c(v_tex) * clamp((mask[i] + 0.5f), 0.0f, 1.0f);
}

#ifdef ps_copy
void ps_copy()
{
	o_col0 = sample_c(v_tex);
}
#endif

// PS5 port (live-20, AI-assisted): the display filters testers asked for, matched to swordpdf's RE4 reference shots
// (2026-10-10): Scanlines (slot 1), VHS soft (slot 2), VHS (slot 3) and CRT (slot 5, see below). They work on the
// displayed picture (0..1 across the game's image, whatever the upscale), so the lines and the tape's softness stay the
// same size at 1x and at 6x. With ORBIS_TVFX 0 the slots are PCSX2's own filters again.
#ifndef ORBIS_TVFX
#define ORBIS_TVFX 1
#endif

#if ORBIS_TVFX && (defined(ps_filter_scanlines) || defined(ps_filter_diagonal) || defined(ps_filter_triangular) || defined(ps_filter_lottes))
// Where this pixel is in the game's picture, 0..1.
vec2 tv_pos()
{
	return (v_tex - u_source_rect.xy) / max(u_source_rect.zw - u_source_rect.xy, vec2(1e-6));
}

// The texture coordinate of a point of the game's picture.
vec2 tv_uv(vec2 p)
{
	return mix(u_source_rect.xy, u_source_rect.zw, p);
}

vec3 tv_fetch(vec2 p)
{
	return texture(samp0, tv_uv(clamp(p, vec2(0.0), vec2(1.0)))).rgb;
}

float tv_hash(vec2 p)
{
	p = fract(p * vec2(0.1031, 0.1030));
	p += dot(p, p.yx + 33.33);
	return fract((p.x + p.y) * p.x);
}

float tv_luma(vec3 c)
{
	return dot(c, vec3(0.299, 0.587, 0.114));
}

// Weight of a line of `count` lines across the picture at y (0..1): 1 on the line, `dark` in the gap between lines.
// `gap` is the share of the pitch that is dark. Lines closer than 2 screen pixels blur into an even shade (no moire).
float tv_lines(float y, float count, float gap, float dark)
{
	float l = y * count;
	float d = abs(fract(l) - 0.5); // 0 mid-line .. 0.5 mid-gap
	float aa = max(count / max(u_target_size.y, 1.0), 0.02); // lines per screen pixel (no derivatives: the PS5 driver)
	float g = smoothstep(0.5 - gap - aa, 0.5 - gap + aa, d);
	float w = mix(1.0, dark, g);
	float even = mix(1.0, dark, 2.0 * gap);
	return mix(w, even, clamp(aa * 2.0 - 0.5, 0.0, 1.0));
}
#endif

#ifdef ps_filter_scanlines
#if ORBIS_TVFX
// Scanlines: the flat picture with a thin dark line between each of 480 lines (the PS2's frame lines).
void ps_filter_scanlines()
{
	vec2 p = tv_pos();
	vec3 c = sample_c(v_tex).rgb;
	c *= tv_lines(p.y, 480.0, 0.17, 0.30) * 1.18;
	o_col0 = vec4(c, 1.0);
}
#else
void ps_filter_scanlines() // scanlines
{
	uvec4 p = uvec4(gl_FragCoord);

	o_col0 = ps_scanlines(p.y % 2);
}
#endif
#endif

#if ORBIS_TVFX && (defined(ps_filter_diagonal) || defined(ps_filter_triangular))
// A VHS tape: luma blurred across the line (about 3 lines of a 640-wide picture), colour blurred much more and pulled to
// the right, a faint echo of each edge to the left, washed-out contrast, grain, faint 240-line structure. `wave` adds
// the wobble of a worn tape (the picture bends a few pixels, ripples up the screen) and a rolling tracking band.
vec3 tv_vhs(vec2 p, float wave, float grain, float soft)
{
	const float TAU = 6.2831853;
	float t = u_time;
	if (wave > 0.0)
	{
		float ripple = 0.0013 * sin(p.y * 36.0 * TAU + t * 4.0) + 0.0006 * sin(p.y * 13.0 * TAU - t * 1.3);
		float roll = fract(p.y * 0.6 - t * 0.05);
		float band = exp(-pow((roll - 0.5) * 30.0, 2.0));
		p.x += wave * (ripple + 0.0035 * band * sin(t * 23.0 + p.y * 400.0));
		grain += 0.10 * wave * band;
	}

	const float w[7] = float[7](0.135, 0.411, 0.801, 1.0, 0.801, 0.411, 0.135);
	float sl = 0.00045 * soft; // luma tap spacing
	float sc = 0.0022 * soft;  // colour tap spacing
	float shift = 0.0010 * soft; // colour sits to the right of the luma
	float y = 0.0;
	vec2 iq = vec2(0.0);
	for (int k = 0; k < 7; k++)
	{
		float o = float(k - 3);
		vec3 a = tv_fetch(vec2(p.x + o * sl, p.y));
		y += w[k] * tv_luma(a);
		vec3 b = tv_fetch(vec2(p.x + shift + o * sc, p.y));
		iq += w[k] * vec2(dot(b, vec3(0.596, -0.274, -0.322)), dot(b, vec3(0.211, -0.523, 0.312)));
	}
	y /= 3.694;
	iq /= 3.694;

	float echo = tv_luma(tv_fetch(vec2(p.x - 0.0045, p.y)));
	y = mix(y, echo, 0.03 + 0.09 * wave);

	y = y * 0.90 + 0.045;           // tape black and white levels
	iq *= 0.82;                     // less saturated
	vec2 px = floor(gl_FragCoord.xy * 0.5);
	float n = tv_hash(px + fract(t * 7.31) * vec2(1789.0, 431.0)) - 0.5;
	y += n * grain;
	iq += (vec2(tv_hash(px.yx + t), tv_hash(px + 17.0 + t)) - 0.5) * grain * 0.35;
	y *= tv_lines(p.y, 240.0, 0.25, 0.90);

	vec3 c;
	c.r = y + 0.956 * iq.x + 0.621 * iq.y;
	c.g = y - 0.272 * iq.x - 0.647 * iq.y;
	c.b = y - 1.106 * iq.x + 1.703 * iq.y;
	return clamp(c, 0.0, 1.0);
}
#endif

#ifdef ps_filter_diagonal
#if ORBIS_TVFX
// VHS soft: a good tape: soft and a little washed out, no wobble.
void ps_filter_diagonal()
{
	o_col0 = vec4(tv_vhs(tv_pos(), 0.0, 0.02, 1.0), 1.0);
}
#else
void ps_filter_diagonal() // diagonal
{
	uvec4 p = uvec4(gl_FragCoord);
	o_col0 = ps_crt((p.x + (p.y % 3)) % 3);
}
#endif
#endif

#ifdef ps_filter_triangular
#if ORBIS_TVFX
// VHS: a worn tape: the same plus the wobble, the tracking band and more grain.
void ps_filter_triangular()
{
	o_col0 = vec4(tv_vhs(tv_pos(), 1.0, 0.08, 0.8), 1.0);
}
#else
void ps_filter_triangular() // triangular
{
	uvec4 p = uvec4(gl_FragCoord);

	// output.c = ps_crt(input, ((p.x + (p.y & 1) * 3) >> 1) % 3);
	o_col0 = ps_crt(((p.x + ((p.y >> 1) & 1) * 3) >> 1) % 3);
}
#endif
#endif

#ifdef ps_filter_complex
void ps_filter_complex() // triangular
{
	const float PI = 3.14159265359f;
	vec2 texdim = vec2(textureSize(samp0, 0));

	o_col0 = (0.9 - 0.4 * cos(2 * PI * v_tex.y * texdim.y)) * sample_c(vec2(v_tex.x, (floor(v_tex.y * texdim.y) + 0.5) / texdim.y));
}
#endif

// PS5 port: the Lottes CRT shader compiles to ~11.8 KiB of pixel code, more than the
// vk-285-12 driver's stage workspace took (~8 KiB, see FSR_RCAS); with ORBIS_LOTTES 0 the
// CRT mode is a light scanline pass over the source lines instead.
#ifndef ORBIS_LOTTES
#define ORBIS_LOTTES 1
#endif
#if defined(ps_filter_lottes) && !ORBIS_LOTTES && !ORBIS_TVFX
void ps_filter_lottes()
{
	vec3 c = sample_c(v_tex).rgb;
	float line = fract(v_tex.y * u_source_resolution.y) - 0.5; // -0.5..0.5 across a source line
	float w = 0.70 + 0.30 * cos(line * 6.2831853); // bright at the line centre
	o_col0 = vec4(c * w, 1.0);
}
#endif

#if defined(ps_filter_lottes) && ORBIS_LOTTES && !ORBIS_TVFX

#define MaskingType 4                      //[1|2|3|4] The type of CRT shadow masking used. 1: compressed TV style, 2: Aperture-grille, 3: Stretched VGA style, 4: VGA style.
#define ScanBrightness -8.00               //[-16.0 to 1.0] The overall brightness of the scanline effect. Lower for darker, higher for brighter.
#define FilterCRTAmount -3.00              //[-4.0 to 1.0] The amount of filtering used, to replicate the TV CRT look. Lower for less, higher for more.
#define HorizontalWarp 0.00                //[0.0 to 0.1] The distortion warping effect for the horizontal (x) axis of the screen. Use small increments.
#define VerticalWarp 0.00                  //[0.0 to 0.1] The distortion warping effect for the verticle (y) axis of the screen. Use small increments.
#define MaskAmountDark 0.50                //[0.0 to 1.0] The value of the dark masking line effect used. Lower for darker lower end masking, higher for brighter.
#define MaskAmountLight 1.50               //[0.0 to 2.0] The value of the light masking line effect used. Lower for darker higher end masking, higher for brighter.
#define BloomPixel -1.50                   //[-2.0 -0.5] Pixel bloom radius. Higher for increased softness of bloom.
#define BloomScanLine -2.0                 //[-4.0 -1.0] Scanline bloom radius. Higher for increased softness of bloom.
#define BloomAmount 0.15                   //[0.0 1.0] Bloom intensity. Higher for brighter.
#define Shape 2.0                          //[0.0 10.0] Kernal filter shape. Lower values will darken image and introduce moire patterns if used with curvature.
#define UseShadowMask 1                    //[0 or 1] Enables, or disables the use of the CRT shadow mask. 0 is disabled, 1 is enabled.

float ToLinear1(float c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

vec3 ToLinear(vec3 c)
{
	return vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b));
}

float ToSrgb1(float c)
{
	return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
}

vec3 ToSrgb(vec3 c)
{
	return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

vec3 Fetch(vec2 pos, vec2 off)
{
	pos = (floor(pos * u_target_size + off) + vec2(0.5, 0.5)) / u_target_size;
	if (max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5)
	{
		return vec3(0.0, 0.0, 0.0);
	}
	else
	{
		return ToLinear(texture(samp0, pos.xy).rgb);
	}
}

vec2 Dist(vec2 pos)
{
	pos = pos * vec2(640, 480);

	return -((pos - floor(pos)) - vec2(0.5, 0.5));
}

float Gaus(float pos, float scale)
{
	return exp2(scale * pow(abs(pos), Shape));
}

vec3 Horz3(vec2 pos, float off)
{
	vec3 b = Fetch(pos, vec2(-1.0, off));
	vec3 c = Fetch(pos, vec2(0.0, off));
	vec3 d = Fetch(pos, vec2(1.0, off));
	float dst = Dist(pos).x;

	// Convert distance to weight.
	float scale = FilterCRTAmount;
	float wb = Gaus(dst - 1.0, scale);
	float wc = Gaus(dst + 0.0, scale);
	float wd = Gaus(dst + 1.0, scale);

	return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

vec3 Horz5(vec2 pos, float off)
{
	vec3 a = Fetch(pos, vec2(-2.0, off));
	vec3 b = Fetch(pos, vec2(-1.0, off));
	vec3 c = Fetch(pos, vec2(0.0, off));
	vec3 d = Fetch(pos, vec2(1.0, off));
	vec3 e = Fetch(pos, vec2(2.0, off));
	float dst = Dist(pos).x;

	// Convert distance to weight.
	float scale = FilterCRTAmount;

	float wa = Gaus(dst - 2.0, scale);
	float wb = Gaus(dst - 1.0, scale);
	float wc = Gaus(dst + 0.0, scale);
	float wd = Gaus(dst + 1.0, scale);
	float we = Gaus(dst + 2.0, scale);

	return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}

vec3 Horz7(vec2 pos, float off)
{
	vec3 a = Fetch(pos, vec2(-3.0, off));
	vec3 b = Fetch(pos, vec2(-2.0, off));
	vec3 c = Fetch(pos, vec2(-1.0, off));
	vec3 d = Fetch(pos, vec2( 0.0, off));
	vec3 e = Fetch(pos, vec2( 1.0, off));
	vec3 f = Fetch(pos, vec2( 2.0, off));
	vec3 g = Fetch(pos, vec2( 3.0, off));

	float dst = Dist(pos).x;
	// Convert distance to weight.
	float scale = BloomPixel;
	float wa = Gaus(dst - 3.0, scale);
	float wb = Gaus(dst - 2.0, scale);
	float wc = Gaus(dst - 1.0, scale);
	float wd = Gaus(dst + 0.0, scale);
	float we = Gaus(dst + 1.0, scale);
	float wf = Gaus(dst + 2.0, scale);
	float wg = Gaus(dst + 3.0, scale);

	// Return filtered sample.
	return (a * wa + b * wb + c * wc + d * wd + e * we + f * wf + g * wg) / (wa + wb + wc + wd + we + wf + wg);
}

// Return scanline weight.
float Scan(vec2 pos, float off)
{
	float dst = Dist(pos).y;
	return Gaus(dst + off, ScanBrightness);
}

float BloomScan(vec2 pos, float off)
{
	float dst = Dist(pos).y;

	return Gaus(dst + off, BloomScanLine);
}

vec3 Tri(vec2 pos)
{
	vec3 a = Horz3(pos, -1.0);
	vec3 b = Horz5(pos, 0.0);
	vec3 c = Horz3(pos, 1.0);

	float wa = Scan(pos, -1.0);
	float wb = Scan(pos, 0.0);
	float wc = Scan(pos, 1.0);

	return (a * wa) + (b * wb) + (c * wc);
}

vec3 Bloom(vec2 pos)
{
	vec3 a = Horz5(pos,-2.0);
	vec3 b = Horz7(pos,-1.0);
	vec3 c = Horz7(pos, 0.0);
	vec3 d = Horz7(pos, 1.0);
	vec3 e = Horz5(pos, 2.0);

	float wa = BloomScan(pos,-2.0);
	float wb = BloomScan(pos,-1.0);
	float wc = BloomScan(pos, 0.0);
	float wd = BloomScan(pos, 1.0);
	float we = BloomScan(pos, 2.0);

	return a * wa + b * wb + c * wc + d * wd + e * we;
}

vec2 Warp(vec2 pos)
{
	pos = pos * 2.0 - 1.0;
	pos *= vec2(1.0 + (pos.y * pos.y) * HorizontalWarp, 1.0 + (pos.x * pos.x) * VerticalWarp);
	return pos * 0.5 + 0.5;
}

vec3 Mask(vec2 pos)
{
#if MaskingType == 1
	// Very compressed TV style shadow mask.
	float lines = MaskAmountLight;
	float odd = 0.0;

	if (fract(pos.x / 6.0) < 0.5)
	{
		odd = 1.0;
	}
	if (fract((pos.y + odd) / 2.0) < 0.5)
	{
		lines = MaskAmountDark;
	}
	pos.x = fract(pos.x / 3.0);
	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}

	mask *= lines;

	return mask;

#elif MaskingType == 2
	// Aperture-grille.
	pos.x = fract(pos.x / 3.0);
	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}

	return mask;

#elif MaskingType == 3
	// Stretched VGA style shadow mask (same as prior shaders).
	pos.x += pos.y * 3.0;
	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);
	pos.x = fract(pos.x / 6.0);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}

	return mask;

#else
	// VGA style shadow mask.
	pos.xy = floor(pos.xy * vec2(1.0, 0.5));
	pos.x += pos.y * 3.0;

	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);
	pos.x = fract(pos.x / 6.0);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}
	return mask;
#endif
}

vec4 LottesCRTPass()
{
	vec4 color;
	vec4 fragcoord = gl_FragCoord - u_target_rect;
	vec2 inSize = u_target_resolution - (2 * u_target_rect.xy);

	vec2 pos = Warp(fragcoord.xy / inSize);
	color.rgb = Tri(pos);
	color.rgb += Bloom(pos) * BloomAmount;
#if UseShadowMask
	color.rgb *= Mask(fragcoord.xy);
#endif
	color.rgb = ToSrgb(color.rgb);

	return color;
}

void ps_filter_lottes()
{
	o_col0 = LottesCRTPass();
}

#endif

#if defined(ps_filter_lottes) && ORBIS_TVFX
// CRT (live-20): a curved tube inside a black bezel, 240 bright beam lines that widen with brightness, a faint aperture
// grille and darker corners. (Lottes' CRT, which drew its lines at the upscaled resolution and so showed none at 4x-6x,
// is kept above for ORBIS_TVFX 0.)
void ps_filter_lottes()
{
	vec2 c = tv_pos() * 2.0 - 1.0;
	c *= vec2(1.0 / 0.90, 1.0 / 0.87);                            // the bezel
	c *= vec2(1.0 + c.y * c.y * 0.045, 1.0 + c.x * c.x * 0.065); // the curve
	vec2 e = 1.0 - abs(c);
	if (e.x <= 0.0 || e.y <= 0.0)
	{
		o_col0 = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec2 q = c * 0.5 + 0.5;
	float corner = length(max(vec2(0.07) - e * vec2(1.0, 1.15), vec2(0.0)));
	float edge = smoothstep(0.0, 0.006, min(e.x, e.y)) * (1.0 - smoothstep(0.062, 0.07, corner));

	vec3 col = tv_fetch(q);
	float l = q.y * 240.0;
	float f = fract(l) - 0.5;
	float sigma = mix(0.15, 0.30, tv_luma(col));
	float beam = exp(-0.5 * f * f / (sigma * sigma));
	beam = mix(beam, 0.62, clamp(240.0 / max(u_target_size.y, 1.0) * 2.0 - 1.0, 0.0, 1.0));
	col *= beam * 1.5;

	float m = mod(floor(gl_FragCoord.x), 3.0);
	col *= vec3(m == 0.0 ? 1.06 : 0.95, m == 1.0 ? 1.06 : 0.95, m == 2.0 ? 1.06 : 0.95);
	col *= 1.0 - 0.22 * dot(c * c, vec2(0.5));
	o_col0 = vec4(clamp(col, 0.0, 1.0) * edge, 1.0);
}
#endif

#if defined(ps_4x_rgss) || defined(ps_automagical_supersampling)
// PS5 port (vk-285-12): edge-adaptive upscaler for the software renderer's frame,
// ported from the port's GL present shader (eerec-278).
// Slot 6 (TVShader=6, was 4xRGSS): FSR1-style EASU upscale (plus RCAS sharpening when
//   FSR_RCAS is 1 and u_orbis_sharp > 0: four more EASU evaluations per pixel).
// Slot 7 (TVShader=7, was automagical): EASU upscale only.
//
// With FSR_RCAS the pixel shader compiles to ~12.6 KiB (EASU alone is ~2.8 KiB). The
// vk-285-12 driver fitted only ~8 KiB of pixel code in a pipeline's stage workspace
// ("shaders of 25216 bytes do not fit before the linked context"); the vk-285-13 driver
// places the linked context after bigger code. RCAS itself runs only when u_orbis_sharp
// is above zero (live.ini sharp=), so the default costs one uniform branch.
#ifndef FSR_RCAS
#define FSR_RCAS 1
#endif
// Algorithm after AMD FidelityFX Super Resolution 1.0 (MIT licensed); rewritten without
// textureGather, with exact reciprocals and NaN guards.
//
// u_orbis_sharp: RCAS amount 0..1 (1 = strongest, 0 = off)
// u_orbis_split: split-screen x in target pixels (left of it: plain bilinear), 0 = off

ivec2 fsr_lo = ivec2(0);
ivec2 fsr_hi = ivec2(0);

// Exact texel read via a sample at the texel centre (same sampling path as ps_copy).
vec3 fsr_load(ivec2 p)
{
	return textureLod(samp0, (vec2(clamp(p, fsr_lo, fsr_hi)) + 0.5) * u_rcp_source_resolution, 0.0).rgb;
}

// Luma times 2 (FSR's cheap approximation).
float fsr_luma(vec3 c)
{
	return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// Accumulate direction and length for one of the 4 bilinear corners.
//    a
//  b c d
//    e
void fsr_easu_set(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
	float dc = lD - lC;
	float cb = lC - lB;
	float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0 / 65536.0);
	float dirX = lD - lB;
	dir.x += dirX * w;
	lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
	len += lenX * lenX * w;

	float ec = lE - lC;
	float ca = lC - lA;
	float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0 / 65536.0);
	float dirY = lE - lA;
	dir.y += dirY * w;
	lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
	len += lenY * lenY * w;
}

void fsr_easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c)
{
	vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.y * dir.x - off.x * dir.y);
	v *= len2;
	float d2 = min(dot(v, v), clp);
	// Lanczos-2 approximation: (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2
	float wB = 0.4 * d2 - 1.0;
	float wA = lob * d2 - 1.0;
	wB *= wB;
	wA *= wA;
	wB = 1.5625 * wB - 0.5625;
	float w = wB * wA;
	aC += c * w;
	aW += w;
}

// pos = source position in texels (texel i covers [i, i+1)).
vec3 fsr_easu(vec2 pos)
{
	vec2 pp = pos - 0.5;
	vec2 fp = floor(pp);
	pp -= fp;
	ivec2 p = ivec2(fp);

	//    b c
	//  e f g h
	//  i j k l
	//    n o
	vec3 b = fsr_load(p + ivec2(0, -1));
	vec3 c = fsr_load(p + ivec2(1, -1));
	vec3 e = fsr_load(p + ivec2(-1, 0));
	vec3 f = fsr_load(p);
	vec3 g = fsr_load(p + ivec2(1, 0));
	vec3 h = fsr_load(p + ivec2(2, 0));
	vec3 i = fsr_load(p + ivec2(-1, 1));
	vec3 j = fsr_load(p + ivec2(0, 1));
	vec3 k = fsr_load(p + ivec2(1, 1));
	vec3 l = fsr_load(p + ivec2(2, 1));
	vec3 n = fsr_load(p + ivec2(0, 2));
	vec3 o = fsr_load(p + ivec2(1, 2));

	float bL = fsr_luma(b), cL = fsr_luma(c), eL = fsr_luma(e), fL = fsr_luma(f);
	float gL = fsr_luma(g), hL = fsr_luma(h), iL = fsr_luma(i), jL = fsr_luma(j);
	float kL = fsr_luma(k), lL = fsr_luma(l), nL = fsr_luma(n), oL = fsr_luma(o);

	vec2 dir = vec2(0.0);
	float len = 0.0;
	fsr_easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
	fsr_easu_set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
	fsr_easu_set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
	fsr_easu_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

	// Normalize direction; flat areas fall back to horizontal.
	float dirR = dir.x * dir.x + dir.y * dir.y;
	bool zro = dirR < (1.0 / 32768.0);
	dirR = zro ? 1.0 : inversesqrt(dirR);
	dir.x = zro ? 1.0 : dir.x;
	dir *= dirR;

	// Edge amount {0..2} -> {0..1}, shaped.
	len = len * 0.5;
	len *= len;
	// Stretch kernel from 1.0 (axis aligned) to sqrt(2) (diagonal).
	float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
	vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
	// Negative lobe strength and clipping window grow with edge amount.
	float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
	float clp = 1.0 / lob;

	vec3 mn4 = min(min(f, g), min(j, k));
	vec3 mx4 = max(max(f, g), max(j, k));

	vec3 aC = vec3(0.0);
	float aW = 0.0;
	fsr_easu_tap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
	fsr_easu_tap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
	fsr_easu_tap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
	fsr_easu_tap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
	fsr_easu_tap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
	fsr_easu_tap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
	fsr_easu_tap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
	fsr_easu_tap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
	fsr_easu_tap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
	fsr_easu_tap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
	fsr_easu_tap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
	fsr_easu_tap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);

	// Normalize and de-ring (clamp to the 4 nearest texels).
	vec3 res = (aW > 1.0e-5) ? aC / aW : f;
	return min(mx4, max(mn4, res));
}

#if FSR_RCAS
// Robust contrast-adaptive sharpening on the upscaled neighbourhood.
//    b
//  d e f
//    h
vec3 fsr_rcas(vec3 b, vec3 d, vec3 e, vec3 f, vec3 h, float con)
{
	vec3 mn4 = min(min(b, d), min(f, h));
	vec3 mx4 = max(max(b, d), max(f, h));
	vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0e-5));
	vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0e-5));
	vec3 lobeRGB = max(-hitMin, hitMax);
	float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * con;
	return (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
}
#endif

vec3 fsr_present(bool sharpen)
{
	vec4 r = u_source_rect * u_source_resolution.xyxy;
	fsr_lo = ivec2(floor(r.xy));
	fsr_hi = max(ivec2(ceil(r.zw)) - ivec2(1), fsr_lo);

	vec2 pos = v_tex * u_source_resolution;
	vec3 e = fsr_easu(pos);
#if FSR_RCAS
	float con = clamp(u_orbis_sharp, 0.0, 1.0);
	if (!sharpen || con <= 0.0)
		return e;

	// Source texels per output pixel.
	vec2 st = u_source_size / max(u_target_size, vec2(1.0));
	vec3 b = fsr_easu(pos - vec2(0.0, st.y));
	vec3 h = fsr_easu(pos + vec2(0.0, st.y));
	vec3 d = fsr_easu(pos - vec2(st.x, 0.0));
	vec3 f = fsr_easu(pos + vec2(st.x, 0.0));
	return clamp(fsr_rcas(b, d, e, f, h, con), 0.0, 1.0);
#else
	return e;
#endif
}

// Split-screen comparison: plain bilinear left of u_orbis_split, white divider.
bool fsr_split_left(out vec3 c)
{
	float split = u_orbis_split;
	c = vec3(1.0);
	if (split <= 0.0 || gl_FragCoord.x > split + 1.0)
		return false;
	if (gl_FragCoord.x < split - 1.0)
		c = sample_c(v_tex).rgb;
	return true;
}
#endif

#ifdef ps_4x_rgss
void ps_4x_rgss()
{
	vec3 c;
	if (!fsr_split_left(c))
		c = fsr_present(true);
	o_col0 = vec4(c, 1.0);
}
#endif

#ifdef ps_automagical_supersampling
void ps_automagical_supersampling()
{
	vec3 c;
	if (!fsr_split_left(c))
		c = fsr_present(false);
	o_col0 = vec4(c, 1.0);
}
#endif

#endif
