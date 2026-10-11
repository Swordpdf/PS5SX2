// PS5 port (vk-285-113): DDS files as texture replacements.
//
// PCSX2's own loader (pcsx2/GS/Renderers/HW/GSTextureReplacementLoaders.cpp, which also holds the libpng loader
// this port can't build) gives BC1, BC2, BC3 and BC7 textures to the GPU as they are. The PS5 driver has no
// block-compressed formats, so this decodes them to RGBA8 on the CPU and reads the uncompressed kinds PCSX2 reads,
// with PCSX2's alpha: A8R8G8B8, X8R8G8B8, X8B8G8R8, R8G8B8 and A8B8G8R8, and other 16, 24 and 32-bit RGB(A) masks
// besides. It reads what PCSX2 reads, the first image and its mip levels, and the DX10 header's BC1/BC2/BC3/BC7
// and 8-bit RGBA/BGRA formats (typeless and sRGB flavours too: the bytes are used as they are, as the PS2 works in
// gamma space).
//
// BC1, BC2 and BC3 are decoded here, not with common/TextureDecompress.cpp's DecompressBlockBC1/2/3, because those
// read a BC1 block's punch-through alpha as opaque black and let a BC2/BC3 colour block choose its 3-colour mode;
// a GPU makes that pixel transparent (BC1) and always uses 4 colours (BC2, BC3). BC7 is bc7decomp::unpack_bc7.
//
// Header only, so ps5/coreorbis/tests/dds/ can build it on a PC.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/TextureDecompress.h" // bc7decomp::unpack_bc7

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace OrbisDDS
{
// One image, RGBA8, width * 4 bytes a row, red first.
struct Image
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba;
};

// The biggest side taken: a 8192 x 8192 image is 256 MiB as RGBA8. (PCSX2 allows 32767; a PS2 texture is 1024 at most.)
constexpr uint32_t MAX_SIDE = 8192;

#pragma pack(push, 1)
struct PixelFormat
{
	uint32_t size, flags, fourcc, bits, rmask, gmask, bmask, amask;
};
struct Header
{
	uint32_t size, flags, height, width, pitch_or_linear, depth, mips, reserved1[11];
	PixelFormat pf;
	uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct HeaderDX10
{
	uint32_t dxgi, dimension, misc, array_size, misc2;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 124, "DDS header size");
static_assert(sizeof(HeaderDX10) == 20, "DDS DX10 header size");

constexpr uint32_t MAGIC = 0x20534444; // "DDS "
constexpr uint32_t DDSD_PITCH = 0x8, DDSD_MIPMAPCOUNT = 0x20000, DDSD_DEPTH = 0x800000;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40;
constexpr uint32_t DIMENSION_2D = 3;

constexpr uint32_t FourCC(char a, char b, char c, char d)
{
	return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
	       (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

enum class Kind
{
	None,
	BC1,
	BC2,
	BC3,
	BC7,
	Masks, // 16, 24 or 32 bits a pixel, channels by bit mask
};

// Channels of an uncompressed format: where each sits in a pixel, and how many bits.
struct Channel
{
	uint32_t mask = 0;
	int shift = 0;
	int bits = 0;
};

inline bool MakeChannel(uint32_t mask, Channel& out)
{
	out = Channel();
	if (mask == 0)
		return true; // no such channel
	out.mask = mask;
	while (((mask >> out.shift) & 1u) == 0)
		out.shift++;
	uint32_t v = mask >> out.shift;
	while (v & 1u)
	{
		out.bits++;
		v >>= 1;
	}
	return v == 0; // the bits must be one run
}

// A channel's value scaled to 0..255.
inline uint8_t Scale8(uint32_t v, int bits)
{
	if (bits >= 8)
		return static_cast<uint8_t>(v >> (bits - 8));
	const uint32_t max = (1u << bits) - 1u;
	return static_cast<uint8_t>((v * 255u + max / 2u) / max);
}

struct Format
{
	Kind kind = Kind::None;
	uint32_t block_bytes = 0; // BC: 8 or 16
	uint32_t bytes_per_pixel = 0; // Masks
	Channel r, g, b, a;
	uint8_t opaque = 0xFF; // the alpha of a format with none
};

inline uint32_t Read32(const uint8_t* p)
{
	uint32_t v;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

inline uint32_t FullChain(uint32_t w, uint32_t h)
{
	uint32_t n = 1;
	for (uint32_t s = std::max(w, h); s > 1; s >>= 1)
		n++;
	return n;
}

inline void Expand565(uint32_t c, uint8_t rgb[3])
{
	const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
	rgb[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
	rgb[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
	rgb[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

// A BC1/BC2/BC3 colour block (8 bytes) into 16 RGBA pixels, row by row. `bc1`: the 3-colour mode when color0 <= color1,
// where index 3 is transparent black; BC2 and BC3 always use 4 colours and leave the alpha to the caller.
inline void DecodeColorBlock(const uint8_t* b, bool bc1, uint8_t out[16][4])
{
	const uint32_t c0 = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8);
	const uint32_t c1 = static_cast<uint32_t>(b[2]) | (static_cast<uint32_t>(b[3]) << 8);
	uint8_t pal[4][4];
	Expand565(c0, pal[0]);
	Expand565(c1, pal[1]);
	pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
	if (!bc1 || c0 > c1)
	{
		for (int i = 0; i < 3; i++)
		{
			pal[2][i] = static_cast<uint8_t>((2 * pal[0][i] + pal[1][i]) / 3);
			pal[3][i] = static_cast<uint8_t>((pal[0][i] + 2 * pal[1][i]) / 3);
		}
	}
	else
	{
		for (int i = 0; i < 3; i++)
		{
			pal[2][i] = static_cast<uint8_t>((pal[0][i] + pal[1][i]) / 2);
			pal[3][i] = 0;
		}
		pal[3][3] = 0;
	}
	uint32_t idx = Read32(b + 4);
	for (int p = 0; p < 16; p++, idx >>= 2)
		std::memcpy(out[p], pal[idx & 3u], 4);
}

// BC3's alpha block (8 bytes): 16 alphas.
inline void DecodeAlphaBlockBC3(const uint8_t* b, uint8_t alpha[16])
{
	const uint32_t a0 = b[0], a1 = b[1];
	uint8_t pal[8];
	pal[0] = static_cast<uint8_t>(a0);
	pal[1] = static_cast<uint8_t>(a1);
	if (a0 > a1)
	{
		for (uint32_t i = 1; i <= 6; i++)
			pal[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
	}
	else
	{
		for (uint32_t i = 1; i <= 4; i++)
			pal[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
		pal[6] = 0;
		pal[7] = 255;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 6; i++)
		bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
	for (int p = 0; p < 16; p++, bits >>= 3)
		alpha[p] = pal[bits & 7u];
}

// A block-compressed image into RGBA8. `src` holds every block of the image, row by row.
inline void DecodeBlocks(Kind kind, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
	const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
	const uint32_t block_bytes = kind == Kind::BC1 ? 8u : 16u;
	out.assign(static_cast<size_t>(w) * h * 4, 0);
	for (uint32_t by = 0; by < bh; by++)
	{
		for (uint32_t bx = 0; bx < bw; bx++)
		{
			const uint8_t* blk = src + (static_cast<size_t>(by) * bw + bx) * block_bytes;
			uint8_t px[16][4];
			switch (kind)
			{
				case Kind::BC1:
					DecodeColorBlock(blk, true, px);
					break;
				case Kind::BC2:
					DecodeColorBlock(blk + 8, false, px);
					for (int p = 0; p < 16; p++)
					{
						const uint32_t nibble = (blk[p >> 1] >> ((p & 1) * 4)) & 15u;
						px[p][3] = static_cast<uint8_t>(nibble * 17u);
					}
					break;
				case Kind::BC3:
				{
					DecodeColorBlock(blk + 8, false, px);
					uint8_t alpha[16];
					DecodeAlphaBlockBC3(blk, alpha);
					for (int p = 0; p < 16; p++)
						px[p][3] = alpha[p];
					break;
				}
				default: // BC7
					if (!bc7decomp::unpack_bc7(blk, reinterpret_cast<bc7decomp::color_rgba*>(px)))
						std::memset(px, 0, sizeof(px)); // a reserved mode: transparent, as a GPU reads it
					break;
			}
			const uint32_t cw = std::min(4u, w - bx * 4), ch = std::min(4u, h - by * 4);
			for (uint32_t y = 0; y < ch; y++)
				std::memcpy(&out[(static_cast<size_t>(by * 4 + y) * w + bx * 4) * 4], px[y * 4], static_cast<size_t>(cw) * 4);
		}
	}
}

// An uncompressed image into RGBA8. `pitch` is the source's bytes a row.
inline void DecodeMasks(const Format& f, const uint8_t* src, size_t pitch, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
	out.resize(static_cast<size_t>(w) * h * 4);
	for (uint32_t y = 0; y < h; y++)
	{
		const uint8_t* in = src + y * pitch;
		uint8_t* o = &out[static_cast<size_t>(y) * w * 4];
		for (uint32_t x = 0; x < w; x++, in += f.bytes_per_pixel, o += 4)
		{
			uint32_t v = 0;
			for (uint32_t i = 0; i < f.bytes_per_pixel; i++)
				v |= static_cast<uint32_t>(in[i]) << (8 * i);
			o[0] = f.r.bits ? Scale8((v & f.r.mask) >> f.r.shift, f.r.bits) : 0;
			o[1] = f.g.bits ? Scale8((v & f.g.mask) >> f.g.shift, f.g.bits) : 0;
			o[2] = f.b.bits ? Scale8((v & f.b.mask) >> f.b.shift, f.b.bits) : 0;
			o[3] = f.a.bits ? Scale8((v & f.a.mask) >> f.a.shift, f.a.bits) : f.opaque;
		}
	}
}

inline bool BlockFormat(Kind kind, uint32_t bytes, Format& f)
{
	f.kind = kind;
	f.block_bytes = bytes;
	return true;
}

// 8-bit DXGI formats with a DX10 header: 27-29 R8G8B8A8, 90/87/91 B8G8R8A8, 92/88/93 B8G8R8X8 (TYPELESS, UNORM, UNORM_SRGB).
inline bool DxgiUncompressed(uint32_t dxgi, Format& f)
{
	f.kind = Kind::Masks;
	f.bytes_per_pixel = 4;
	const bool rgba = dxgi >= 27 && dxgi <= 29;
	const bool bgra = dxgi == 87 || dxgi == 90 || dxgi == 91;
	const bool bgrx = dxgi == 88 || dxgi == 92 || dxgi == 93;
	if (!rgba && !bgra && !bgrx)
		return false;
	MakeChannel(rgba ? 0x000000FFu : 0x00FF0000u, f.r);
	MakeChannel(0x0000FF00u, f.g);
	MakeChannel(rgba ? 0x00FF0000u : 0x000000FFu, f.b);
	MakeChannel(bgrx ? 0u : 0xFF000000u, f.a);
	return true;
}

// The format a header describes; false with `why` for one this loader doesn't read.
inline bool ChooseFormat(const Header& h, const HeaderDX10* dx10, Format& f, const char*& why)
{
	f = Format();
	if (h.pf.flags & DDPF_FOURCC)
	{
		const uint32_t cc = h.pf.fourcc;
		const uint32_t dxgi = dx10 ? dx10->dxgi : 0;
		if (cc == FourCC('D', 'X', '1', '0'))
		{
			// BC1 70-72, BC2 73-75, BC3 76-78, BC7 97-99: TYPELESS (Pillow writes these), UNORM, UNORM_SRGB.
			if (dxgi >= 70 && dxgi <= 72)
				return BlockFormat(Kind::BC1, 8, f);
			if (dxgi >= 73 && dxgi <= 75)
				return BlockFormat(Kind::BC2, 16, f);
			if (dxgi >= 76 && dxgi <= 78)
				return BlockFormat(Kind::BC3, 16, f);
			if (dxgi >= 97 && dxgi <= 99)
				return BlockFormat(Kind::BC7, 16, f);
			if (DxgiUncompressed(dxgi, f))
				return true;
			why = "a DX10 format this loader doesn't read (BC1, BC2, BC3, BC7 and 8-bit RGBA/BGRA only)";
			return false;
		}
		if (cc == FourCC('D', 'X', 'T', '1'))
			return BlockFormat(Kind::BC1, 8, f);
		if (cc == FourCC('D', 'X', 'T', '2') || cc == FourCC('D', 'X', 'T', '3'))
			return BlockFormat(Kind::BC2, 16, f);
		if (cc == FourCC('D', 'X', 'T', '4') || cc == FourCC('D', 'X', 'T', '5'))
			return BlockFormat(Kind::BC3, 16, f);
		why = "a compressed format this loader doesn't read (DXT1 to DXT5 and BC7 only)";
		return false;
	}
	if (!(h.pf.flags & DDPF_RGB) || (h.pf.bits != 16 && h.pf.bits != 24 && h.pf.bits != 32))
	{
		why = "an uncompressed format this loader doesn't read (16, 24 and 32-bit RGB only)";
		return false;
	}
	f.kind = Kind::Masks;
	f.bytes_per_pixel = h.pf.bits / 8;
	if (!MakeChannel(h.pf.rmask, f.r) || !MakeChannel(h.pf.gmask, f.g) || !MakeChannel(h.pf.bmask, f.b) ||
		!MakeChannel(h.pf.amask, f.a) || (!f.r.bits && !f.g.bits && !f.b.bits))
	{
		why = "channel masks this loader can't read";
		return false;
	}
	// PCSX2 makes a 32-bit X8B8G8R8 file's alpha 0x80 (the PS2's opaque) and the other formats without alpha 0xFF.
	if (f.a.bits == 0 && f.bytes_per_pixel == 4 && h.pf.rmask == 0x000000FFu && h.pf.gmask == 0x0000FF00u && h.pf.bmask == 0x00FF0000u)
		f.opaque = 0x80;
	return true;
}

// Reads a DDS file. levels[0] is the image and levels[1...] its mip levels, unless `only_base` (PCSX2 asks for the mip
// levels only when the game uses them). False, with `why`, for a file that can't be used.
inline bool Decode(const uint8_t* file, size_t size, bool only_base, std::vector<Image>& levels, const char*& why)
{
	levels.clear();
	why = "not a DDS file";
	if (!file || size < sizeof(uint32_t) + sizeof(Header) || Read32(file) != MAGIC)
		return false;
	Header h;
	std::memcpy(&h, file + sizeof(uint32_t), sizeof(h));
	if (h.size < sizeof(Header))
	{
		why = "a DDS header that is too short";
		return false;
	}
	if (h.width == 0 || h.height == 0 || h.width > MAX_SIDE || h.height > MAX_SIDE)
	{
		why = "a size outside 1 to 8192";
		return false;
	}
	if (h.flags & DDSD_DEPTH)
	{
		why = "a volume texture";
		return false;
	}
	size_t offset = sizeof(uint32_t) + sizeof(Header);
	HeaderDX10 dx10 = {};
	const bool has_dx10 = (h.pf.flags & DDPF_FOURCC) && h.pf.fourcc == FourCC('D', 'X', '1', '0');
	if (has_dx10)
	{
		if (size < offset + sizeof(dx10))
		{
			why = "a truncated DX10 header";
			return false;
		}
		std::memcpy(&dx10, file + offset, sizeof(dx10));
		offset += sizeof(dx10);
		if (dx10.dimension != DIMENSION_2D || dx10.array_size > 1) // Pillow writes 0 for "one"
		{
			why = "an array or non-2D texture";
			return false;
		}
	}
	Format f;
	if (!ChooseFormat(h, has_dx10 ? &dx10 : nullptr, f, why))
		return false;

	const uint32_t full = FullChain(h.width, h.height);
	uint32_t mip_count = 1;
	if (h.flags & DDSD_MIPMAPCOUNT)
		mip_count = h.mips ? std::min(h.mips, full) : full;
	if (only_base)
		mip_count = 1;

	for (uint32_t level = 0; level < mip_count; level++)
	{
		const uint32_t w = std::max(h.width >> level, 1u), h_px = std::max(h.height >> level, 1u);
		size_t level_bytes = 0, pitch = 0;
		if (f.kind == Kind::Masks)
		{
			pitch = static_cast<size_t>(w) * f.bytes_per_pixel;
			// The header's pitch, for the image itself: some writers pad rows.
			if (level == 0 && (h.flags & DDSD_PITCH) && h.pitch_or_linear >= pitch && h.pitch_or_linear < (1u << 28))
				pitch = h.pitch_or_linear;
			level_bytes = pitch * h_px;
		}
		else
			level_bytes = static_cast<size_t>((w + 3) / 4) * ((h_px + 3) / 4) * f.block_bytes;
		if (offset > size || level_bytes > size - offset)
		{
			if (level == 0)
			{
				why = "a truncated file";
				return false;
			}
			break; // fewer mip levels than the header says
		}
		Image img;
		img.width = w;
		img.height = h_px;
		if (f.kind == Kind::Masks)
			DecodeMasks(f, file + offset, pitch, w, h_px, img.rgba);
		else
			DecodeBlocks(f.kind, file + offset, w, h_px, img.rgba);
		levels.push_back(std::move(img));
		offset += level_bytes;
	}
	why = "";
	return !levels.empty();
}
} // namespace OrbisDDS
