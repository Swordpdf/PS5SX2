/* PS5SX2 disc tile art (2026-10-09, AI-assisted): the home screen's icon0.png (512x512) and pic0.dds (3840x2160 BC7, the
   format the PS5's own sce_sys files use) for a PS2 disc.
   - With SteamGridDB art: the background is the game's hero art across the screen's width over a blurred, darkened copy
     of itself; the icon is the game's logo on its hero art, with "PlayStation 2" under it in the console's own font.
   - Without: the background is the cover stretched, the icon the cover over a blurred copy of itself.
   BC7 is written in mode 6 only (one RGBA line per 4x4 block): plenty for these pictures. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "tile.h"

typedef struct
{
	int w, h, ch; /* ch = 3 (RGB) or 4 (RGBA) */
	uint8_t* px;
} Img;

static uint8_t* slurp(const char* path, size_t* n_out)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* buf = n > 0 ? malloc((size_t)n) : NULL;
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf && n_out)
		*n_out = (size_t)n;
	return buf;
}

static int load(const char* path, int ch, Img* img)
{
	memset(img, 0, sizeof *img);
	if (!path)
		return -1;
	size_t n = 0;
	uint8_t* buf = slurp(path, &n);
	if (!buf)
		return -1;
	int comp;
	img->px = stbi_load_from_memory(buf, (int)n, &img->w, &img->h, &comp, ch);
	free(buf);
	img->ch = ch;
	return img->px ? 0 : -1;
}

static int alloc(Img* img, int w, int h, int ch)
{
	img->w = w;
	img->h = h;
	img->ch = ch;
	img->px = calloc((size_t)w * h, (size_t)ch);
	return img->px ? 0 : -1;
}

/* bilinear: the source rectangle (cx, cy, cw, chh) of src into the rectangle (dx, dy, w, h) of dst; same channel count */
static void blit(const Img* src, float cx, float cy, float cw, float chh, Img* dst, int dx, int dy, int w, int h)
{
	const int ch = src->ch;
	for (int y = 0; y < h; y++)
	{
		const int Y = dy + y;
		if (Y < 0 || Y >= dst->h)
			continue;
		float fy = cy + (y + 0.5f) * chh / h - 0.5f;
		if (fy < 0) fy = 0;
		if (fy > src->h - 1) fy = (float)(src->h - 1);
		const int y0 = (int)fy, y1 = y0 + 1 < src->h ? y0 + 1 : y0;
		const float ty = fy - y0;
		for (int x = 0; x < w; x++)
		{
			const int X = dx + x;
			if (X < 0 || X >= dst->w)
				continue;
			float fx = cx + (x + 0.5f) * cw / w - 0.5f;
			if (fx < 0) fx = 0;
			if (fx > src->w - 1) fx = (float)(src->w - 1);
			const int x0 = (int)fx, x1 = x0 + 1 < src->w ? x0 + 1 : x0;
			const float tx = fx - x0;
			uint8_t* o = dst->px + ((size_t)Y * dst->w + X) * dst->ch;
			for (int c = 0; c < ch; c++)
			{
				const float a = src->px[((size_t)y0 * src->w + x0) * ch + c], b = src->px[((size_t)y0 * src->w + x1) * ch + c];
				const float d = src->px[((size_t)y1 * src->w + x0) * ch + c], e = src->px[((size_t)y1 * src->w + x1) * ch + c];
				o[c] = (uint8_t)((a + (b - a) * tx) * (1 - ty) + (d + (e - d) * tx) * ty + 0.5f);
			}
		}
	}
}

/* the whole of src stretched over the whole of dst */
static void stretch(const Img* src, Img* dst) { blit(src, 0, 0, (float)src->w, (float)src->h, dst, 0, 0, dst->w, dst->h); }

/* src cropped (centred) to dst's shape, then over dst */
static void cover_crop(const Img* src, Img* dst)
{
	float cw = (float)src->w, chh = (float)src->h;
	if (cw * dst->h > chh * dst->w)
		cw = chh * dst->w / dst->h;
	else
		chh = cw * dst->h / dst->w;
	blit(src, (src->w - cw) / 2, (src->h - chh) / 2, cw, chh, dst, 0, 0, dst->w, dst->h);
}

/* a box blur of radius r, three passes (close to a Gaussian), edges clamped: the small picture is blurred before it is blown up, so that the
   blow-up shows no blocks */
static void box_blur3(Img* im, int r)
{
	if (r < 1 || im->w < 3 || im->h < 3)
		return;
	const int w = im->w, h = im->h, ch = im->ch;
	uint8_t* tmp = (uint8_t*)malloc((size_t)w * h * ch);
	if (!tmp)
		return;
	for (int pass = 0; pass < 3; pass++)
	{
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				for (int c = 0; c < ch; c++)
				{
					int sum = 0;
					for (int k = -r; k <= r; k++)
					{
						const int xx = x + k < 0 ? 0 : (x + k >= w ? w - 1 : x + k);
						sum += im->px[((size_t)y * w + xx) * ch + c];
					}
					tmp[((size_t)y * w + x) * ch + c] = (uint8_t)(sum / (2 * r + 1));
				}
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				for (int c = 0; c < ch; c++)
				{
					int sum = 0;
					for (int k = -r; k <= r; k++)
					{
						const int yy = y + k < 0 ? 0 : (y + k >= h ? h - 1 : y + k);
						sum += tmp[((size_t)yy * w + x) * ch + c];
					}
					im->px[((size_t)y * w + x) * ch + c] = (uint8_t)(sum / (2 * r + 1));
				}
	}
	free(tmp);
}

/* a soft copy of src over dst: through a picture 1/div of the size and back */
static void blurred(const Img* src, Img* dst, int div, int keep_percent)
{
	Img small, mid;
	const int sw = dst->w / div > 4 ? dst->w / div : 4, sh = dst->h / div > 4 ? dst->h / div : 4;
	if (alloc(&small, sw, sh, src->ch) != 0)
		return;
	cover_crop(src, &small);
	box_blur3(&small, (sw < sh ? sw : sh) / 12 > 1 ? (sw < sh ? sw : sh) / 12 : 1);
	/* twice through, for a rounder blur than one blocky upscale */
	if (alloc(&mid, sw * 4, sh * 4, src->ch) == 0)
	{
		stretch(&small, &mid);
		Img small2;
		if (alloc(&small2, sw, sh, src->ch) == 0)
		{
			stretch(&mid, &small2);
			stretch(&small2, dst);
			free(small2.px);
		}
		else
			stretch(&small, dst);
		free(mid.px);
	}
	else
		stretch(&small, dst);
	free(small.px);
	for (size_t i = 0; i < (size_t)dst->w * dst->h * dst->ch; i++)
		dst->px[i] = (uint8_t)(dst->px[i] * keep_percent / 100);
}

/* an RGBA picture fitted inside the box (bx, by, bw, bh) of an RGB one, centred, blended by its alpha */
static void overlay_fit(const Img* rgba, Img* dst, int bx, int by, int bw, int bh)
{
	int w = bw, h = (int)((float)rgba->h * bw / rgba->w);
	if (h > bh)
	{
		h = bh;
		w = (int)((float)rgba->w * bh / rgba->h);
	}
	Img tmp;
	if (w <= 0 || h <= 0 || alloc(&tmp, w, h, 4) != 0)
		return;
	stretch(rgba, &tmp);
	const int ox = bx + (bw - w) / 2, oy = by + (bh - h) / 2;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			const int X = ox + x, Y = oy + y;
			if (X < 0 || Y < 0 || X >= dst->w || Y >= dst->h)
				continue;
			const uint8_t* s = tmp.px + ((size_t)y * w + x) * 4;
			uint8_t* d = dst->px + ((size_t)Y * dst->w + X) * 3;
			for (int c = 0; c < 3; c++)
				d[c] = (uint8_t)((s[c] * s[3] + d[c] * (255 - s[3])) / 255);
		}
	free(tmp.px);
}

/* white text, centred on cx, its baseline at y, px_height tall; 0 when drawn */
static int draw_text(Img* dst, const char* font_path, const char* text, int cx, int baseline, float px_height)
{
	size_t n = 0;
	uint8_t* font = font_path ? slurp(font_path, &n) : NULL;
	if (!font)
		return -1;
	stbtt_fontinfo info;
	if (!stbtt_InitFont(&info, font, stbtt_GetFontOffsetForIndex(font, 0)))
	{
		free(font);
		return -1;
	}
	const float scale = stbtt_ScaleForPixelHeight(&info, px_height);
	float width = 0;
	for (const char* p = text; *p; p++)
	{
		int adv, lsb;
		stbtt_GetCodepointHMetrics(&info, (unsigned char)*p, &adv, &lsb);
		width += adv * scale;
		if (p[1])
			width += stbtt_GetCodepointKernAdvance(&info, (unsigned char)p[0], (unsigned char)p[1]) * scale;
	}
	float x = cx - width / 2;
	for (const char* p = text; *p; p++)
	{
		const int cp = (unsigned char)*p;
		int x0, y0, x1, y1, adv, lsb;
		stbtt_GetCodepointBitmapBox(&info, cp, scale, scale, &x0, &y0, &x1, &y1);
		const int gw = x1 - x0, gh = y1 - y0;
		if (gw > 0 && gh > 0)
		{
			uint8_t* g = malloc((size_t)gw * gh);
			if (g)
			{
				stbtt_MakeCodepointBitmap(&info, g, gw, gh, gw, scale, scale, cp);
				for (int yy = 0; yy < gh; yy++)
					for (int xx = 0; xx < gw; xx++)
					{
						const int X = (int)x + x0 + xx, Y = baseline + y0 + yy;
						if (X < 0 || Y < 0 || X >= dst->w || Y >= dst->h)
							continue;
						const int a = g[yy * gw + xx];
						uint8_t* d = dst->px + ((size_t)Y * dst->w + X) * 3;
						for (int c = 0; c < 3; c++)
							d[c] = (uint8_t)((255 * a + d[c] * (255 - a)) / 255);
					}
				free(g);
			}
		}
		stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
		x += adv * scale;
		if (p[1])
			x += stbtt_GetCodepointKernAdvance(&info, cp, (unsigned char)p[1]) * scale;
	}
	free(font);
	return 0;
}

/* a darker band behind the label, fading in from above */
static void label_band(Img* dst, int top)
{
	for (int y = top; y < dst->h; y++)
	{
		const int fade = y - top < 24 ? (y - top) * 100 / 24 : 100;
		const int keep = 100 - fade * 88 / 100;
		for (int x = 0; x < dst->w; x++)
		{
			uint8_t* d = dst->px + ((size_t)y * dst->w + x) * 3;
			for (int c = 0; c < 3; c++)
				d[c] = (uint8_t)(d[c] * keep / 100);
		}
	}
}

/* ---- PNG writer: RGB8, stored (uncompressed) deflate blocks ---- */
static uint32_t crc_table[256];
static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++)
	{
		uint32_t c = n;
		for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}
static uint32_t crc(uint32_t c, const uint8_t* p, size_t n)
{
	c = ~c;
	while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return ~c;
}
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int put_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len)
{
	uint8_t hdr[8];
	be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	uint32_t c = crc(0, hdr + 4, 4);
	c = crc(c, data, len);
	uint8_t tail[4];
	be32(tail, c);
	return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

int tile_write_png(const char* path, const uint8_t* rgb, int w, int h)
{
	crc_init();
	const size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
	const size_t blocks = (raw_len + 65534) / 65535;
	const size_t z_len = 2 + raw_len + blocks * 5 + 4;
	uint8_t* z = malloc(z_len);
	uint8_t* raw = malloc(raw_len);
	if (!z || !raw)
	{
		free(z); free(raw);
		return -1;
	}
	for (int y = 0; y < h; y++)
	{
		raw[(size_t)y * (1 + w * 3)] = 0;
		memcpy(raw + (size_t)y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
	}
	size_t o = 0;
	z[o++] = 0x78; z[o++] = 0x01;
	uint32_t s1 = 1, s2 = 0;
	for (size_t i = 0; i < raw_len; i++) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
	for (size_t off = 0; off < raw_len; off += 65535)
	{
		const uint32_t n = raw_len - off > 65535 ? 65535 : (uint32_t)(raw_len - off);
		z[o++] = off + n >= raw_len ? 1 : 0;
		z[o++] = n & 0xFF; z[o++] = n >> 8;
		z[o++] = ~n & 0xFF; z[o++] = (~n >> 8) & 0xFF;
		memcpy(z + o, raw + off, n);
		o += n;
	}
	be32(z + o, (s2 << 16) | s1);
	o += 4;
	free(raw);

	FILE* f = fopen(path, "wb");
	if (!f)
	{
		free(z);
		return -1;
	}
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	uint8_t ihdr[13];
	be32(ihdr, w); be32(ihdr + 4, h);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	int ok = fwrite(sig, 1, 8, f) == 8 && put_chunk(f, "IHDR", ihdr, 13) && put_chunk(f, "IDAT", z, (uint32_t)o) && put_chunk(f, "IEND", NULL, 0);
	ok = fclose(f) == 0 && ok;
	free(z);
	return ok ? 0 : -1;
}

/* ---- BC7 mode 6 ---- */
static const int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

typedef struct { uint64_t lo, hi; int pos; } Bits;
static void put(Bits* b, uint32_t v, int n)
{
	for (int i = 0; i < n; i++, b->pos++)
	{
		const uint64_t bit = (v >> i) & 1;
		if (b->pos < 64) b->lo |= bit << b->pos;
		else b->hi |= bit << (b->pos - 64);
	}
}

static void bc7_block(const uint8_t px[16][3], uint8_t out[16])
{
	int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
	for (int i = 0; i < 16; i++)
		for (int c = 0; c < 3; c++)
		{
			if (px[i][c] < mn[c]) mn[c] = px[i][c];
			if (px[i][c] > mx[c]) mx[c] = px[i][c];
		}
	/* p-bit 1 on both endpoints so alpha is 255: the endpoints are the odd values (v7 << 1 | 1) */
	int e[2][3];
	for (int c = 0; c < 3; c++)
	{
		e[0][c] = mn[c] >> 1;
		e[1][c] = mx[c] >> 1;
	}
	int ev[2][3];
	for (int k = 0; k < 2; k++)
		for (int c = 0; c < 3; c++) ev[k][c] = (e[k][c] << 1) | 1;
	int idx[16];
	const int d[3] = {ev[1][0] - ev[0][0], ev[1][1] - ev[0][1], ev[1][2] - ev[0][2]};
	const int dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	for (int i = 0; i < 16; i++)
	{
		if (dd == 0)
		{
			idx[i] = 0;
			continue;
		}
		const int t = (px[i][0] - ev[0][0]) * d[0] + (px[i][1] - ev[0][1]) * d[1] + (px[i][2] - ev[0][2]) * d[2];
		/* nearest of the 16 weights along the line */
		const float f = (float)t / dd * 64.0f;
		int best = 0;
		float bd = 1e9f;
		for (int j = 0; j < 16; j++)
		{
			const float df = f - kW4[j];
			if (df * df < bd) { bd = df * df; best = j; }
		}
		idx[i] = best;
	}
	if (idx[0] & 8) /* the anchor index keeps its top bit 0: swap the endpoints, mirror the indices */
	{
		for (int c = 0; c < 3; c++) { int t = e[0][c]; e[0][c] = e[1][c]; e[1][c] = t; }
		for (int i = 0; i < 16; i++) idx[i] = 15 - idx[i];
	}
	Bits b = {0, 0, 0};
	put(&b, 1u << 6, 7);
	for (int c = 0; c < 3; c++) { put(&b, e[0][c], 7); put(&b, e[1][c], 7); }
	put(&b, 127, 7); put(&b, 127, 7);
	put(&b, 1, 1); put(&b, 1, 1);
	put(&b, idx[0], 3);
	for (int i = 1; i < 16; i++) put(&b, idx[i], 4);
	for (int i = 0; i < 8; i++) { out[i] = (uint8_t)(b.lo >> (8 * i)); out[8 + i] = (uint8_t)(b.hi >> (8 * i)); }
}

/* the 148-byte header of the app's own pic0.dds (3840x2160, DX10, BC7_UNORM), so ours matches it exactly */
static void dds_header(uint8_t h[148], int w, int hgt)
{
	memset(h, 0, 148);
	memcpy(h, "DDS ", 4);
	uint32_t* d = (uint32_t*)(h + 4);
	d[0] = 124;              /* dwSize */
	d[1] = 0xA1007;          /* CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE|MIPMAPCOUNT */
	d[2] = hgt;
	d[3] = w;
	d[4] = (uint32_t)w * hgt; /* linear size: 1 byte per pixel for BC7 */
	d[6] = 1;                /* mip count */
	d[18] = 32;              /* ddspf.dwSize */
	d[19] = 4;               /* DDPF_FOURCC */
	memcpy(&d[20], "DX10", 4);
	d[26] = 0x1000;          /* DDSCAPS_TEXTURE */
	uint32_t* x = (uint32_t*)(h + 128);
	x[0] = 98;               /* DXGI_FORMAT_BC7_UNORM */
	x[1] = 3;                /* TEXTURE2D */
	x[3] = 1;                /* array size */
}

int tile_write_bc7_dds(const char* path, const uint8_t* rgb, int w, int h)
{
	FILE* f = fopen(path, "wb");
	if (!f) return -1;
	uint8_t hdr[148];
	dds_header(hdr, w, h);
	int ok = fwrite(hdr, 1, 148, f) == 148;
	const int bw = w / 4;
	uint8_t* row = malloc((size_t)bw * 16);
	ok = ok && row;
	for (int by = 0; ok && by < h / 4; by++)
	{
		for (int bx = 0; bx < bw; bx++)
		{
			uint8_t px[16][3];
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++)
					memcpy(px[y * 4 + x], rgb + ((size_t)(by * 4 + y) * w + bx * 4 + x) * 3, 3);
			bc7_block((const uint8_t(*)[3])px, row + (size_t)bx * 16);
		}
		ok = fwrite(row, 1, (size_t)bw * 16, f) == (size_t)bw * 16;
	}
	free(row);
	ok = fclose(f) == 0 && ok;
	return ok ? 0 : -1;
}

/* a small screenshot as a background: a blurred, darkened copy fills the screen, the screenshot itself sits on the right
   (where the home screen shows nothing), its left side melting into the blur */
static void shot_background(const Img* shot, Img* pic)
{
	blurred(shot, pic, 96, 30);
	int h = 1500, w = shot->w * h / shot->h;
	if (w > 2300)
	{
		w = 2300;
		h = shot->h * w / shot->w;
	}
	Img s;
	if (alloc(&s, w, h, 3) != 0)
		return;
	stretch(shot, &s);
	const int x0 = 3840 - w - 170, y0 = (2160 - h) / 2 + 90;
	const int fl = w * 30 / 100, fe = 70; /* the left fade, the other edges' */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			int wgt = 256;
			if (x < fl) wgt = wgt * x / fl;
			if (w - 1 - x < fe) wgt = wgt * (w - 1 - x) / fe;
			if (y < fe) wgt = wgt * y / fe;
			if (h - 1 - y < fe) wgt = wgt * (h - 1 - y) / fe;
			uint8_t* d = pic->px + ((size_t)(y0 + y) * 3840 + x0 + x) * 3;
			const uint8_t* p = s.px + ((size_t)y * w + x) * 3;
			for (int c = 0; c < 3; c++)
				d[c] = (uint8_t)((p[c] * wgt + d[c] * (256 - wgt)) >> 8);
		}
	free(s.px);
}

/* a soft drop shadow: darkens dst around the rectangle (x, y, w, h) up to `reach` pixels out, a little more below it (the light is above),
   `strength` percent at the edge, fading out smoothly. Drawn before the rectangle's own picture goes on top. */
static void drop_shadow(Img* dst, int x, int y, int w, int h, int reach, int strength)
{
	const int oy = reach / 3;
	for (int Y = y - reach + oy; Y < y + h + reach + oy; Y++)
		for (int X = x - reach; X < x + w + reach; X++)
		{
			if (X < 0 || Y < 0 || X >= dst->w || Y >= dst->h)
				continue;
			const int dx = X < x ? x - X : (X >= x + w ? X - (x + w - 1) : 0);
			const int dy = Y < y + oy ? y + oy - Y : (Y >= y + oy + h ? Y - (y + oy + h - 1) : 0);
			const int d = dx > dy ? dx : dy;
			if (d >= reach)
				continue;
			const int a = strength * (reach - d) * (reach - d) / (reach * reach);
			uint8_t* p = dst->px + ((size_t)Y * dst->w + X) * dst->ch;
			for (int c = 0; c < 3; c++)
				p[c] = (uint8_t)(p[c] * (100 - a) / 100);
		}
}

/* a picture standing on dst at (x, y) as it is (src stretched to w x h), over a drop shadow */
static void stand(const Img* src, Img* dst, int x, int y, int w, int h, int reach, int strength)
{
	Img s;
	if (w <= 0 || h <= 0 || alloc(&s, w, h, 3) != 0)
		return;
	stretch(src, &s);
	drop_shadow(dst, x, y, w, h, reach, strength);
	for (int r = 0; r < h; r++)
	{
		const int Y = y + r;
		if (Y < 0 || Y >= dst->h)
			continue;
		for (int c = 0; c < w; c++)
		{
			const int X = x + c;
			if (X < 0 || X >= dst->w)
				continue;
			memcpy(dst->px + ((size_t)Y * dst->w + X) * 3, s.px + ((size_t)r * w + c) * 3, 3);
		}
	}
	free(s.px);
}

/* A PS2 case's front is a tall picture (about 5:7) and the home screen's background a wide one (16:9): stretched over it, a cover is not
   recognisable. Here a blurred, darkened copy of the cover fills the screen and the cover itself stands on the right, where the home screen
   shows nothing, over a soft shadow. Works for any shape of picture (a square SteamGridDB grid too). */
static void cover_background(const Img* cover, Img* pic)
{
	blurred(cover, pic, 48, 58);
	int h = 1640, w = cover->w * h / cover->h;
	if (w > 1560)
	{
		w = 1560;
		h = cover->h * w / cover->w;
	}
	stand(cover, pic, 3840 - w - 360, (2160 - h) / 2, w, h, 90, 60);
}

/* The square icon from a tall cover: the whole case, as tall as the icon allows, standing over a blurred copy of itself, so that the colours of
   the cover fill the sides. (Cropping the cover to a square would cut off its top and bottom: the console's banner and the rating.) */
static void cover_icon(const Img* cover, Img* icon)
{
	blurred(cover, icon, 24, 55);
	int fw = 512, fh = 512;
	if (cover->w > cover->h)
		fh = cover->h * 504 / cover->w, fw = 504;
	else
		fw = cover->w * 504 / cover->h, fh = 504;
	stand(cover, icon, (512 - fw) / 2, (512 - fh) / 2, fw, fh, 26, 55);
}

/* The icon of a cover-only tile. By default (the look picked on the test sheet) the middle of the cover fills the whole square. The other way
   (flag disctile_iconcase): the whole case over a blurred copy of itself, cover_icon above. */
static int g_icon_case;
void tile_set_icon_case(int on) { g_icon_case = on; }

/* ---- the two tile pictures ---- */
int tile_make_art(const TileArt* a, const char* icon_out, const char* pic_out, char* used, size_t used_size)
{
	Img cover, hero, logo, grid, shot, title;
	load(a->cover, 3, &cover);
	load(a->hero, 3, &hero);
	load(a->logo, 4, &logo);
	load(a->grid, 3, &grid);
	load(a->shot, 3, &shot);
	load(a->title, 3, &title);
	if (!cover.px && !hero.px && !grid.px && !shot.px && !title.px)
		return -2;
	int rc = 0;
	char bg_used[16] = "", icon_used[16] = "";

	/* the background */
	Img pic;
	if (alloc(&pic, 3840, 2160, 3) != 0)
		rc = -5;
	else
	{
		if (hero.px)
		{
			blurred(&hero, &pic, 32, 45);
			/* the hero art across the full width (cropped if it's taller than the screen), its edges fading in */
			int h = (int)((float)hero.h * 3840 / hero.w);
			if (h >= 2160)
			{
				Img sharp = {0};
				if (alloc(&sharp, 3840, 2160, 3) == 0)
				{
					cover_crop(&hero, &sharp);
					memcpy(pic.px, sharp.px, (size_t)3840 * 2160 * 3);
					free(sharp.px);
				}
			}
			else
			{
				Img band;
				if (alloc(&band, 3840, h, 3) == 0)
				{
					stretch(&hero, &band);
					const int top = (2160 - h) / 2, fade = h / 8;
					for (int y = 0; y < h; y++)
					{
						const int wgt = y < fade ? y * 256 / fade : (h - 1 - y < fade ? (h - 1 - y) * 256 / fade : 256);
						uint8_t* d = pic.px + (size_t)(top + y) * 3840 * 3;
						const uint8_t* s = band.px + (size_t)y * 3840 * 3;
						for (int i = 0; i < 3840 * 3; i++)
							d[i] = (uint8_t)((s[i] * wgt + d[i] * (256 - wgt)) >> 8);
					}
					free(band.px);
				}
			}
			strcpy(bg_used, "hero");
		}
		else if (shot.px || title.px)
		{
			shot_background(shot.px ? &shot : &title, &pic);
			strcpy(bg_used, shot.px ? "screenshot" : "title screen");
		}
		else
		{
			cover_background(cover.px ? &cover : &grid, &pic);
			strcpy(bg_used, cover.px ? "cover" : "grid");
		}
		if (pic_out && tile_write_bc7_dds(pic_out, pic.px, 3840, 2160) != 0)
			rc = -6;
		free(pic.px);
	}

	/* the icon */
	Img icon;
	if (rc == 0 && alloc(&icon, 512, 512, 3) != 0)
		rc = -3;
	else if (rc == 0)
	{
		int label = a->label && a->label[0];
		if (logo.px && (hero.px || cover.px))
		{
			/* the logo over the hero art (or the blurred cover), darkened so the logo stands out */
			if (hero.px)
			{
				cover_crop(&hero, &icon);
				for (size_t i = 0; i < (size_t)512 * 512 * 3; i++)
					icon.px[i] = (uint8_t)(icon.px[i] * 62 / 100);
			}
			else
				blurred(&cover, &icon, 32, 50);
			overlay_fit(&logo, &icon, 36, label ? 70 : 60, 440, label ? 300 : 392);
			strcpy(icon_used, "logo");
		}
		else if (grid.px)
		{
			cover_crop(&grid, &icon);
			strcpy(icon_used, "grid");
		}
		else if (title.px)
		{
			/* the title screen (the game's own logo, usually), centred, over a blurred copy of itself */
			blurred(&title, &icon, 16, 45);
			const int h = title.h * 512 / title.w;
			blit(&title, 0, 0, (float)title.w, (float)title.h, &icon, 0, (440 - h) / 2 > 0 ? (440 - h) / 2 : 0, 512, h);
			strcpy(icon_used, "title screen");
		}
		else if (cover.px)
		{
			if (g_icon_case)
				cover_icon(&cover, &icon);
			else
				cover_crop(&cover, &icon);
			strcpy(icon_used, g_icon_case ? "cover-case" : "cover-crop");
			label = 0; /* the cover carries the console's banner already */
		}
		else
		{
			cover_crop(&hero, &icon);
			strcpy(icon_used, "hero");
		}
		if (label)
		{
			label_band(&icon, 400);
			if (draw_text(&icon, a->font, a->label, 256, 474, 50) != 0)
				strcat(icon_used, "-nofont");
		}
		if (icon_out && tile_write_png(icon_out, icon.px, 512, 512) != 0)
			rc = -4;
		free(icon.px);
	}
	if (used)
		snprintf(used, used_size, "background %s, icon %s", bg_used, icon_used);
	stbi_image_free(cover.px);
	stbi_image_free(hero.px);
	stbi_image_free(logo.px);
	stbi_image_free(grid.px);
	stbi_image_free(shot.px);
	stbi_image_free(title.px);
	return rc;
}

int tile_make(const char* cover_path, const char* icon_out, const char* pic_out)
{
	TileArt a = {cover_path, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	return tile_make_art(&a, icon_out, pic_out, NULL, 0);
}

/* ---- the "Reload PS2 disc" tile (2026-10-14): drawn here, from no picture at all ----
   A blue backdrop with a soft glow, a silver disc, and two arrows chasing each other round it (the usual sign for "again"). Everything is a
   distance to a shape turned into coverage with a one-pixel soft edge, so the edges are smooth at both sizes. */
static float fsq(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); } /* no libm needed */
static float fabs_f(float x) { return x < 0 ? -x : x; }
static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* sine by a Taylor series after bringing the angle into [-pi, pi]: off by less than 0.0005, and the payload links no libm */
static float sin_f(float x)
{
	const float pi = 3.14159265f, tp = 6.28318531f;
	while (x > pi) x -= tp;
	while (x < -pi) x += tp;
	const float x2 = x * x;
	return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}
static float cos_f(float x) { return sin_f(x + 1.57079633f); }

/* a distance in pixels (positive = inside the shape) as coverage */
static float edge(float d) { return clamp01(0.5f + d); }

static void mix_px(Img* im, int x, int y, const float col[3], float a)
{
	if (a <= 0.0f)
		return;
	if (a > 1.0f)
		a = 1.0f;
	uint8_t* d = im->px + ((size_t)y * im->w + x) * 3;
	for (int c = 0; c < 3; c++)
		d[c] = (uint8_t)(d[c] + (col[c] - d[c]) * a + 0.5f);
}

/* an arc of a circle with an arrow head at its end; angles in radians, clockwise on the picture (y points down), the arc under 180 degrees */
typedef struct { float cx, cy, R, hw, a0, a1; } Arrow;

static void draw_arrow(Img* im, const Arrow* A, const float col[3], float alpha, float grow)
{
	const float u0x = cos_f(A->a0), u0y = sin_f(A->a0), u1x = cos_f(A->a1), u1y = sin_f(A->a1);
	const float hw = A->hw + grow, W = 2.2f * A->hw + grow, L = 3.4f * A->hw + grow;
	const float tx = -u1y, ty = u1x; /* the way the arrow points: on round, clockwise */
	/* the head: its base is across the arc's end, its tip ahead of it */
	const float vx[3] = {A->cx + (A->R + W) * u1x, A->cx + (A->R - W) * u1x, A->cx + A->R * u1x + L * tx};
	const float vy[3] = {A->cy + (A->R + W) * u1y, A->cy + (A->R - W) * u1y, A->cy + A->R * u1y + L * ty};
	float ex[3], ey[3], el[3];
	for (int i = 0; i < 3; i++)
	{
		const int j = (i + 1) % 3;
		ex[i] = vx[j] - vx[i];
		ey[i] = vy[j] - vy[i];
		el[i] = fsq(ex[i] * ex[i] + ey[i] * ey[i]);
	}
	const float sgn = (ex[0] * (vy[2] - vy[0]) - ey[0] * (vx[2] - vx[0])) >= 0 ? 1.0f : -1.0f;
	const float reach = A->R + W + L + 3.0f;
	const float lim = W + L * L / (2.0f * A->R) + 2.0f; /* no pixel of the band or the head is farther than this from the circle */
	int x0 = (int)(A->cx - reach), x1 = (int)(A->cx + reach) + 1, y0 = (int)(A->cy - reach), y1 = (int)(A->cy + reach) + 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > im->w) x1 = im->w;
	if (y1 > im->h) y1 = im->h;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
		{
			const float fx = x + 0.5f, fy = y + 0.5f, dx = fx - A->cx, dy = fy - A->cy;
			const float r = fsq(dx * dx + dy * dy);
			if (fabs_f(r - A->R) > lim)
				continue;
			float a = 0.0f;
			const float band = edge(hw - fabs_f(r - A->R));
			if (band > 0.0f) /* the band between the two radial cuts; its end runs a little under the head, so that no seam shows */
				a = band * edge(u0x * dy - u0y * dx) * edge(dx * u1y - dy * u1x + A->hw * 0.8f);
			float dmin = 1e9f;
			for (int i = 0; i < 3; i++)
			{
				const float d = sgn * (ex[i] * (fy - vy[i]) - ey[i] * (fx - vx[i])) / el[i];
				if (d < dmin)
					dmin = d;
			}
			const float head = edge(dmin);
			if (head > a)
				a = head;
			mix_px(im, x, y, col, a * alpha);
		}
}

/* a silver disc with a hole, two thin grooves, a clear ring round the hole and a faint rainbow sheen */
static void draw_disc(Img* im, float cx, float cy, float Rd, float Rh)
{
	int x0 = (int)(cx - Rd - 2), x1 = (int)(cx + Rd + 3), y0 = (int)(cy - Rd - 2), y1 = (int)(cy + Rd + 3);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > im->w) x1 = im->w;
	if (y1 > im->h) y1 = im->h;
	const float gw = Rd * 0.008f + 0.35f;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
		{
			const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			const float r = fsq(dx * dx + dy * dy);
			const float body = edge(Rd - r) * edge(r - Rh);
			if (body <= 0.0f)
				continue;
			const float t = clamp01(0.5f + (dx * 0.5f + dy * 0.85f) / (2.0f * Rd)); /* light at the top left, deeper at the bottom right */
			float col[3] = {226.0f - 62.0f * t, 236.0f - 54.0f * t, 252.0f - 36.0f * t};
			const float groove = edge(gw - fabs_f(r - 0.58f * Rd)) + edge(gw - fabs_f(r - 0.80f * Rd));
			const float clear = edge(r - Rh * 1.35f) * edge(Rh * 2.1f - r); /* the clear plastic ring round the hole */
			const float sheen = 2.0f * dx * dy / (r * r + 1.0f) * clamp01((r - 0.25f * Rd) / (0.15f * Rd)); /* sin(2 x angle): smooth, opposite in neighbouring quarters */
			for (int c = 0; c < 3; c++)
			{
				col[c] *= 1.0f - 0.2f * clamp01(groove);
				col[c] += (255.0f - col[c]) * 0.55f * clear;
			}
			col[0] += 12.0f * sheen;
			col[1] += 4.0f * sheen;
			col[2] -= 12.0f * sheen;
			if (r > Rd * 0.965f)
				for (int c = 0; c < 3; c++)
					col[c] *= 0.82f;
			for (int c = 0; c < 3; c++)
				col[c] = col[c] < 0 ? 0 : (col[c] > 255 ? 255 : col[c]);
			mix_px(im, x, y, col, body);
		}
}

/* the backdrop: a vertical blue gradient and a soft glow of radius gr round (gx, gy) */
static void reload_background(Img* im, float gx, float gy, float gr)
{
	static const float top[3] = {18.0f, 52.0f, 132.0f}, bot[3] = {5.0f, 12.0f, 48.0f}, glow[3] = {64.0f, 128.0f, 232.0f};
	for (int y = 0; y < im->h; y++)
	{
		const float t = (float)y / (float)(im->h - 1);
		const float dy = y + 0.5f - gy;
		for (int x = 0; x < im->w; x++)
		{
			float c[3];
			for (int k = 0; k < 3; k++)
				c[k] = top[k] + (bot[k] - top[k]) * t;
			const float dx = x + 0.5f - gx;
			if (fabs_f(dx) < gr && fabs_f(dy) < gr)
			{
				const float r2 = dx * dx + dy * dy;
				if (r2 < gr * gr)
				{
					float g = 1.0f - fsq(r2) / gr;
					g = g * g * 0.5f;
					for (int k = 0; k < 3; k++)
						c[k] += glow[k] * g;
				}
			}
			uint8_t* d = im->px + ((size_t)y * im->w + x) * 3;
			for (int k = 0; k < 3; k++)
				d[k] = (uint8_t)(c[k] < 0 ? 0 : (c[k] > 255 ? 255 : c[k] + 0.5f));
		}
	}
}

/* the sign itself, k times the size it has on the 512-pixel icon */
static void reload_glyph(Img* im, float cx, float cy, float k)
{
	static const float white[3] = {242.0f, 248.0f, 255.0f}, cyan[3] = {96.0f, 200.0f, 255.0f};
	const float deg = 0.01745329f;
	const Arrow up = {cx, cy, 146.0f * k, 16.0f * k, -150.0f * deg, -20.0f * deg};
	const Arrow dn = {cx, cy, 146.0f * k, 16.0f * k, 30.0f * deg, 160.0f * deg};
	static const float halo[4][2] = {{14.0f, 0.05f}, {10.0f, 0.07f}, {6.5f, 0.10f}, {3.2f, 0.14f}}; /* grow, alpha: a soft glow under each arrow, layer on layer */
	for (int i = 0; i < 4; i++)
	{
		draw_arrow(im, &up, cyan, halo[i][1], halo[i][0] * k);
		draw_arrow(im, &dn, cyan, halo[i][1], halo[i][0] * k);
	}
	draw_arrow(im, &up, white, 1.0f, 0.0f);
	draw_arrow(im, &dn, white, 1.0f, 0.0f);
	draw_disc(im, cx, cy, 92.0f * k, 17.0f * k);
}

int tile_make_reload_art(const char* icon_out, const char* pic_out, const char* font, const char* label)
{
	int rc = 0;
	Img icon;
	if (alloc(&icon, 512, 512, 3) != 0)
		return -3;
	reload_background(&icon, 256.0f, 212.0f, 330.0f);
	reload_glyph(&icon, 256.0f, 212.0f, 1.0f);
	if (label && label[0])
	{
		label_band(&icon, 404);
		if (font)
			draw_text(&icon, font, label, 256, 478, 44.0f); /* no font file: the sign alone */
	}
	if (icon_out && tile_write_png(icon_out, icon.px, 512, 512) != 0)
		rc = -4;
	free(icon.px);
	if (pic_out)
	{
		/* the home screen shows nothing on the right of its background: the sign stands there, as the covers do */
		Img pic;
		if (alloc(&pic, 3840, 2160, 3) != 0)
			return rc ? rc : -5;
		reload_background(&pic, 2860.0f, 1080.0f, 1750.0f);
		reload_glyph(&pic, 2860.0f, 1080.0f, 4.1f);
		if (tile_write_bc7_dds(pic_out, pic.px, 3840, 2160) != 0)
			rc = -6;
		free(pic.px);
	}
	return rc;
}

/* ---- a title card (2026-10-14): for a disc whose cover could not be fetched (a console without network or DNS), drawn from the game's name alone ----
   The reload tile's backdrop, a big silver disc, and the name in white under it (at most three lines of twenty characters: only ASCII is drawn, the
   font is looked at one byte at a time). */
static int wrap_title(const char* in, char lines[3][48], int maxc)
{
	char clean[160];
	size_t k = 0;
	for (const char* p = in; *p && k + 1 < sizeof(clean); p++)
	{
		const unsigned char c = (unsigned char)*p;
		if (c >= 0x20 && c < 0x7f && !(c == ' ' && (k == 0 || clean[k - 1] == ' ')))
			clean[k++] = (char)c;
	}
	while (k > 0 && clean[k - 1] == ' ')
		k--;
	clean[k] = '\0';
	if (k == 0)
	{
		memcpy(clean, "PS2 disc", 9);
		k = 8;
	}
	int n = 0;
	size_t pos = 0;
	while (pos < k && n < 3)
	{
		size_t take = k - pos < (size_t)maxc ? k - pos : (size_t)maxc;
		if (pos + take < k) /* more follows: break at the last space that fits, if there is one */
		{
			size_t cut = take;
			while (cut > 0 && clean[pos + cut] != ' ')
				cut--;
			if (cut > 0)
				take = cut;
		}
		memcpy(lines[n], clean + pos, take);
		lines[n][take] = '\0';
		n++;
		pos += take;
		while (pos < k && clean[pos] == ' ')
			pos++;
	}
	if (pos < k && n > 0) /* the name did not fit: the last line ends in three dots */
	{
		size_t l = strlen(lines[n - 1]);
		if (l > (size_t)maxc - 3)
			l = (size_t)maxc - 3;
		memcpy(lines[n - 1] + l, "...", 4);
	}
	return n;
}

int tile_make_title_art(const char* icon_out, const char* pic_out, const char* font, const char* title)
{
	int rc = 0;
	char lines[3][48];
	Img icon;
	if (alloc(&icon, 512, 512, 3) != 0)
		return -3;
	reload_background(&icon, 256.0f, 176.0f, 330.0f);
	draw_disc(&icon, 256.0f, 168.0f, 122.0f, 22.0f);
	if (font)
	{
		const int n = wrap_title(title ? title : "", lines, 20);
		const int line_h = 46, top = 512 - (n * line_h + 30);
		label_band(&icon, top - 10);
		for (int i = 0; i < n; i++)
			draw_text(&icon, font, lines[i], 256, top + 22 + 32 + i * line_h - 14, 40.0f); /* a line's baseline: its box top + ascent */
	}
	if (icon_out && tile_write_png(icon_out, icon.px, 512, 512) != 0)
		rc = -4;
	free(icon.px);
	if (pic_out)
	{
		Img pic;
		if (alloc(&pic, 3840, 2160, 3) != 0)
			return rc ? rc : -5;
		reload_background(&pic, 2860.0f, 1000.0f, 1750.0f);
		draw_disc(&pic, 2860.0f, 880.0f, 500.0f, 90.0f);
		if (font)
		{
			const int n = wrap_title(title ? title : "", lines, 17);
			for (int i = 0; i < n; i++)
				draw_text(&pic, font, lines[i], 2860, 1560 + i * 190, 160.0f);
		}
		if (tile_write_bc7_dds(pic_out, pic.px, 3840, 2160) != 0)
			rc = -6;
		free(pic.px);
	}
	return rc;
}
