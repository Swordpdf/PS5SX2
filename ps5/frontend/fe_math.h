// PS5 port frontend: small vector and matrix helpers (column-major, Vulkan clip space).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>

namespace fe
{
struct Vec2
{
	float x = 0, y = 0;
};

struct Vec3
{
	float x = 0, y = 0, z = 0;
	Vec3() = default;
	constexpr Vec3(float x_, float y_, float z_)
		: x(x_), y(y_), z(z_)
	{
	}
	Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

struct Vec4
{
	float x = 0, y = 0, z = 0, w = 0;
	Vec4() = default;
	constexpr Vec4(float x_, float y_, float z_, float w_)
		: x(x_), y(y_), z(z_), w(w_)
	{
	}
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 Normalize(const Vec3& v)
{
	const float l = std::sqrt(Dot(v, v));
	return l > 0 ? v * (1.0f / l) : v;
}
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Mix(float a, float b, float t) { return a + (b - a) * t; }
inline Vec3 Mix(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float Smoothstep(float e0, float e1, float x)
{
	const float t = Clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Column-major 4x4: m[col * 4 + row].
struct Mat4
{
	float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

	static Mat4 Identity() { return {}; }

	Mat4 operator*(const Mat4& b) const
	{
		Mat4 r;
		for (int c = 0; c < 4; c++)
			for (int rr = 0; rr < 4; rr++)
			{
				float s = 0;
				for (int k = 0; k < 4; k++)
					s += m[k * 4 + rr] * b.m[c * 4 + k];
				r.m[c * 4 + rr] = s;
			}
		return r;
	}

	Vec4 operator*(const Vec4& v) const
	{
		return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w, m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
			m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w, m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w};
	}

	static Mat4 Translate(float x, float y, float z)
	{
		Mat4 r;
		r.m[12] = x;
		r.m[13] = y;
		r.m[14] = z;
		return r;
	}

	static Mat4 Scale(float x, float y, float z)
	{
		Mat4 r;
		r.m[0] = x;
		r.m[5] = y;
		r.m[10] = z;
		return r;
	}

	// Rotation about +Y by `a` radians (right-handed: +X turns towards -Z).
	static Mat4 RotateY(float a)
	{
		Mat4 r;
		const float c = std::cos(a), s = std::sin(a);
		r.m[0] = c;
		r.m[2] = -s;
		r.m[8] = s;
		r.m[10] = c;
		return r;
	}

	static Mat4 RotateX(float a)
	{
		Mat4 r;
		const float c = std::cos(a), s = std::sin(a);
		r.m[5] = c;
		r.m[6] = s;
		r.m[9] = -s;
		r.m[10] = c;
		return r;
	}

	static Mat4 RotateZ(float a)
	{
		Mat4 r;
		const float c = std::cos(a), s = std::sin(a);
		r.m[0] = c;
		r.m[1] = s;
		r.m[4] = -s;
		r.m[5] = c;
		return r;
	}

	// Right-handed view matrix looking from `eye` at `at`.
	static Mat4 LookAt(const Vec3& eye, const Vec3& at, const Vec3& up)
	{
		const Vec3 f = Normalize(at - eye);
		const Vec3 s = Normalize(Cross(f, up));
		const Vec3 u = Cross(s, f);
		Mat4 r;
		r.m[0] = s.x;
		r.m[4] = s.y;
		r.m[8] = s.z;
		r.m[1] = u.x;
		r.m[5] = u.y;
		r.m[9] = u.z;
		r.m[2] = -f.x;
		r.m[6] = -f.y;
		r.m[10] = -f.z;
		r.m[12] = -Dot(s, eye);
		r.m[13] = -Dot(u, eye);
		r.m[14] = Dot(f, eye);
		return r;
	}

	// Vulkan perspective: depth 0..1, y down in clip space (so +Y world is up on screen).
	static Mat4 Perspective(float fovy, float aspect, float znear, float zfar)
	{
		const float t = 1.0f / std::tan(fovy * 0.5f);
		Mat4 r;
		r.m[0] = t / aspect;
		r.m[5] = -t;
		r.m[10] = zfar / (znear - zfar);
		r.m[11] = -1.0f;
		r.m[14] = (znear * zfar) / (znear - zfar);
		r.m[15] = 0.0f;
		return r;
	}
};

constexpr float kPi = 3.14159265358979f;
inline float Radians(float deg) { return deg * (kPi / 180.0f); }
} // namespace fe
