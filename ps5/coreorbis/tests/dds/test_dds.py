#!/usr/bin/env python3
"""Host test for orbis-shims/OrbisDDS.h, the DDS reader of the PS5 port's texture replacements.

Builds dds_dump.cpp with the system g++ (once with AddressSanitizer for the robustness checks), makes DDS files
with Pillow and ImageMagick (both independent encoders) and by hand, decodes each with the header and compares the
result with Pillow's own DDS decoder. Needs: g++, python3, numpy and Pillow; ImageMagick's `convert` is used when found.

    python3 ps5/coreorbis/tests/dds/test_dds.py

SPDX-License-Identifier: GPL-3.0-or-later
"""

import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
WORK = tempfile.mkdtemp(prefix="ddstest-")
FAILS = []


def build(name, extra):
    out = os.path.join(WORK, name)
    cmd = ["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-I" + REPO, "-I" + os.path.join(REPO, "ps5", "coreorbis")] + extra + [
        os.path.join(HERE, "dds_dump.cpp"), os.path.join(REPO, "common", "TextureDecompress.cpp"), "-o", out]
    subprocess.run(cmd, check=True)
    return out


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        FAILS.append(what)


def dump(tool, path, base_only=False):
    """(status, levels) with levels = [(w, h, rgba ndarray)]; status 'ok' or the failure reason."""
    prefix = os.path.join(WORK, "out")
    args = [tool, path, prefix] + (["base"] if base_only else [])
    r = subprocess.run(args, capture_output=True, text=True, timeout=60)
    if r.returncode not in (0, 1):
        return "crash: rc=%d %s" % (r.returncode, r.stderr[-300:]), []
    if r.returncode == 1:
        return r.stdout.strip(), []
    levels = []
    for line in r.stdout.split("\n"):
        if not line.strip():
            continue
        i, w, h = map(int, line.split())
        raw = np.fromfile("%s.%d.rgba" % (prefix, i), np.uint8)
        levels.append((w, h, raw.reshape(h, w, 4)))
    return "ok", levels


def pillow_rgba(path):
    return np.array(Image.open(path).convert("RGBA"))


def make_source(w, h):
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 0] = xx * 255 // max(w - 1, 1)
    img[..., 1] = yy * 255 // max(h - 1, 1)
    img[..., 2] = (xx + yy) * 255 // max(w + h - 2, 1)
    d = np.sqrt((xx - w // 2) ** 2 + (yy - h // 2) ** 2)
    img[..., 3] = np.clip(255 - d * 10, 0, 255).astype(np.uint8)
    checker = (xx // 8 + yy // 8) % 2 == 0
    img[checker, :3] = 255 - img[checker, :3]
    return img


# ---- a DDS writer for the formats the encoders above can't make
def dds_header(w, h, flags, pf, mips=0, pitch=0, dx10=None):
    fl = 0x1007 | flags | (0x20000 if mips else 0)
    hdr = struct.pack("<7I", 124, fl, h, w, pitch, 0, mips) + b"\0" * 44
    hdr += struct.pack("<8I", 32, *pf)
    hdr += struct.pack("<5I", 0x1000 | (0x400008 if mips else 0), 0, 0, 0, 0)
    out = b"DDS " + hdr
    if dx10:
        out += struct.pack("<5I", dx10, 3, 0, 1, 0)
    return out


PF_A8R8G8B8 = (0x41, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)
PF_X8R8G8B8 = (0x40, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0)
PF_A8B8G8R8 = (0x41, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000)
PF_X8B8G8R8 = (0x40, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0)
PF_R8G8B8 = (0x40, 0, 24, 0x00ff0000, 0x0000ff00, 0x000000ff, 0)
PF_R5G6B5 = (0x40, 0, 16, 0xf800, 0x07e0, 0x001f, 0)
PF_A1R5G5B5 = (0x41, 0, 16, 0x7c00, 0x03e0, 0x001f, 0x8000)
PF_A4R4G4B4 = (0x41, 0, 16, 0x0f00, 0x00f0, 0x000f, 0xf000)
PF_DX10 = (0x4, 0x30315844, 0, 0, 0, 0, 0)  # 'DX10'


def pack_pixels(img, bits, order):
    """img HxWx4 -> raw bytes; order 'bgra', 'rgba', 'bgrx', 'rgbx', 'bgr'."""
    r, g, b, a = (img[..., i].astype(np.uint32) for i in range(4))
    if order == "bgra":
        return (b | (g << 8) | (r << 16) | (a << 24)).astype("<u4").tobytes()
    if order == "rgba":
        return (r | (g << 8) | (b << 16) | (a << 24)).astype("<u4").tobytes()
    if order == "bgrx":
        return (b | (g << 8) | (r << 16) | (0xff << 24)).astype("<u4").tobytes()
    if order == "rgbx":
        return (r | (g << 8) | (b << 16) | (0xff << 24)).astype("<u4").tobytes()
    if order == "bgr":
        return np.dstack([b, g, r]).astype(np.uint8).tobytes()
    if order == "565":
        return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype("<u2").tobytes()
    if order == "1555":
        return (((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)).astype("<u2").tobytes()
    if order == "4444":
        return (((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)).astype("<u2").tobytes()
    raise ValueError(order)


def bc7_mode6_encode(img):
    """A valid (not clever) BC7 mode 6 encoder: endpoints = per-channel min and max of each block."""
    h, w = img.shape[:2]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    weights = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]
    pad = np.zeros((bh * 4, bw * 4, 4), np.uint8)
    pad[:h, :w] = img
    pad[h:, :w] = img[h - 1:h, :]
    pad[:, w:] = pad[:, w - 1:w]
    out = bytearray()
    for by in range(bh):
        for bx in range(bw):
            blk = pad[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].reshape(16, 4).astype(np.int32)
            lo, hi = blk.min(axis=0), blk.max(axis=0)
            e = []
            for src in (lo, hi):
                p = int(src.sum()) & 1  # any p-bit works; use a value-derived one
                e7 = [min(127, int(c) >> 1) for c in src]
                e.append((e7, p, [(v << 1) | p for v in e7]))
            (e0, p0, f0), (e1, p1, f1) = e
            idx = []
            for px in blk:
                best, bi = None, 0
                for i, wgt in enumerate(weights):
                    cand = [((64 - wgt) * f0[c] + wgt * f1[c] + 32) >> 6 for c in range(4)]
                    err = sum((cand[c] - int(px[c])) ** 2 for c in range(4))
                    if best is None or err < best:
                        best, bi = err, i
                idx.append(bi)
            if idx[0] >= 8:  # the anchor index has 3 bits: swap the endpoints
                e0, e1, p0, p1 = e1, e0, p1, p0
                idx = [15 - i for i in idx]
            bits = 0
            pos = 0

            def put(v, n):
                nonlocal bits, pos
                bits |= (v & ((1 << n) - 1)) << pos
                pos += n

            put(0x40, 7)  # mode 6: six zeros and a one
            for c in range(4):
                put(e0[c], 7)
                put(e1[c], 7)
            put(p0, 1)
            put(p1, 1)
            for i, v in enumerate(idx):
                put(v, 3 if i == 0 else 4)
            assert pos == 128
            out += bits.to_bytes(16, "little")
    return bytes(out)


def mip_chain(img, count):
    levels = [img]
    for i in range(1, count):
        w, h = max(img.shape[1] >> i, 1), max(img.shape[0] >> i, 1)
        levels.append(np.array(Image.fromarray(img, "RGBA").resize((w, h), Image.BILINEAR)))
    return levels


def compare(name, tool, path, ref_levels, tol, base_only=False):
    status, levels = dump(tool, path, base_only)
    if status != "ok":
        check(False, "%s: %s" % (name, status))
        return
    if len(levels) != len(ref_levels):
        check(False, "%s: %d levels, expected %d" % (name, len(levels), len(ref_levels)))
        return
    worst = 0
    for (w, h, got), ref in zip(levels, ref_levels):
        if ref.shape[:2] != (h, w):
            check(False, "%s: level is %dx%d, expected %dx%d" % (name, w, h, ref.shape[1], ref.shape[0]))
            return
        worst = max(worst, int(np.abs(got.astype(np.int32) - ref.astype(np.int32)).max()))
    check(worst <= tol, "%s: %d level(s), largest difference %d (allowed %d)" % (name, len(levels), worst, tol))


def main():
    print("building")
    tool = build("dds_dump", [])
    asan = build("dds_dump_asan", ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"])
    w, h = 62, 46  # not multiples of 4 on purpose
    src = make_source(w, h)
    Image.fromarray(src, "RGBA").save(os.path.join(WORK, "src.png"))
    have_convert = shutil.which("convert") is not None

    print("block-compressed, made by Pillow and ImageMagick, against Pillow's decoder")
    for pf in ("DXT1", "DXT3", "DXT5", "BC2", "BC3"):
        path = os.path.join(WORK, "pil_%s.dds" % pf)
        Image.fromarray(src, "RGBA").save(path, pixel_format=pf)
        compare("Pillow %s %dx%d" % (pf, w, h), tool, path, [pillow_rgba(path)], 3)
    if have_convert:
        for c in ("dxt1", "dxt3", "dxt5"):
            path = os.path.join(WORK, "im_%s.dds" % c)
            subprocess.run(["convert", os.path.join(WORK, "src.png"), "-define", "dds:compression=" + c, path], check=True)
            compare("ImageMagick %s %dx%d" % (c.upper(), w, h), tool, path, [pillow_rgba(path)], 3)

    print("BC7 (mode 6, a small encoder in this script), against Pillow's decoder")
    for (bw_, bh_) in ((64, 48), (62, 46), (7, 5), (4, 4), (1, 1)):
        im = make_source(bw_, bh_)
        data = bc7_mode6_encode(im)
        path = os.path.join(WORK, "bc7_%dx%d.dds" % (bw_, bh_))
        open(path, "wb").write(dds_header(bw_, bh_, 0x80000, PF_DX10, dx10=98, pitch=len(data)) + data)
        compare("BC7 %dx%d" % (bw_, bh_), tool, path, [pillow_rgba(path)], 0)

    print("uncompressed formats: exact")
    def ours(img, fmt, alpha=None):
        ref = img.copy()
        if alpha is not None:
            ref[..., 3] = alpha
        return ref

    cases = [
        ("A8R8G8B8", PF_A8R8G8B8, "bgra", src, 0, None),
        ("A8B8G8R8", PF_A8B8G8R8, "rgba", src, 0, None),
        ("X8R8G8B8 (alpha 0xFF)", PF_X8R8G8B8, "bgrx", src, 0, 0xFF),
        ("X8B8G8R8 (alpha 0x80, as PCSX2)", PF_X8B8G8R8, "rgbx", src, 0, 0x80),
        ("R8G8B8 (alpha 0xFF)", PF_R8G8B8, "bgr", src, 0, 0xFF),
    ]
    for name, pf, order, im, tol, alpha in cases:
        data = pack_pixels(im, 32, order)
        pitch = 0
        path = os.path.join(WORK, "raw_%s.dds" % name.split()[0])
        open(path, "wb").write(dds_header(w, h, 0, pf, pitch=pitch) + data)
        compare(name, tool, path, [ours(im, name, alpha)], tol)
    # 16-bit ones: expected values are the 5/6/4-bit channels scaled to 8 bits
    def scale(v, bits):
        m = (1 << bits) - 1
        return (v * 255 + m // 2) // m
    for name, pf, order, rb, gb, bb, ab in (("R5G6B5", PF_R5G6B5, "565", 5, 6, 5, 0), ("A1R5G5B5", PF_A1R5G5B5, "1555", 5, 5, 5, 1),
                                           ("A4R4G4B4", PF_A4R4G4B4, "4444", 4, 4, 4, 4)):
        data = pack_pixels(src, 16, order)
        path = os.path.join(WORK, "raw_%s.dds" % name)
        open(path, "wb").write(dds_header(w, h, 0, pf) + data)
        ref = np.zeros_like(src)
        ref[..., 0] = scale(src[..., 0].astype(np.int32) >> (8 - rb), rb)
        ref[..., 1] = scale(src[..., 1].astype(np.int32) >> (8 - gb), gb)
        ref[..., 2] = scale(src[..., 2].astype(np.int32) >> (8 - bb), bb)
        ref[..., 3] = scale(src[..., 3].astype(np.int32) >> (8 - ab), ab) if ab else 255
        compare(name, tool, path, [ref], 0)
    # padded rows: the header's pitch is used
    data = b"".join(pack_pixels(src[y:y + 1], 32, "bgra") + b"\xee" * 8 for y in range(h))
    path = os.path.join(WORK, "raw_pitch.dds")
    open(path, "wb").write(dds_header(w, h, 0x8, PF_A8R8G8B8, pitch=w * 4 + 8) + data)
    compare("A8R8G8B8 with padded rows (pitch flag)", tool, path, [src], 0)
    # DX10 header, 8-bit formats
    for dxgi, order, alpha in ((28, "rgba", None), (87, "bgra", None), (88, "bgrx", 0xFF)):
        path = os.path.join(WORK, "dx10_%d.dds" % dxgi)
        open(path, "wb").write(dds_header(w, h, 0, PF_DX10, dx10=dxgi) + pack_pixels(src, 32, order))
        compare("DX10 format %d" % dxgi, tool, path, [ours(src, dxgi, alpha)], 0)

    print("mip levels")
    for count in (4, 6):  # 6 is the whole chain of a 62 x 46 image
        levels = mip_chain(src, count)
        payload = b"".join(pack_pixels(lv, 32, "bgra") for lv in levels)
        path = os.path.join(WORK, "mips_%d.dds" % count)
        open(path, "wb").write(dds_header(w, h, 0, PF_A8R8G8B8, mips=count) + payload)
        compare("A8R8G8B8 with %d levels" % count, tool, path, levels, 0)
        compare("  ... base only", tool, path, levels[:1], 0, base_only=True)
    levels = mip_chain(src, 6)
    payload = b"".join(pack_pixels(lv, 32, "bgra") for lv in levels) + b"\0" * 64
    path = os.path.join(WORK, "mips_toomany.dds")
    open(path, "wb").write(dds_header(w, h, 0, PF_A8R8G8B8, mips=12) + payload)
    compare("A8R8G8B8 header says 12 levels: the 6 a 62 x 46 image has", tool, path, levels, 0)
    # BC3 mips: each level encoded by Pillow, the payloads joined
    levels = mip_chain(src, 5)
    payload = b""
    refs = []
    for i, lv in enumerate(levels):
        p = os.path.join(WORK, "lv%d.dds" % i)
        Image.fromarray(lv, "RGBA").save(p, pixel_format="DXT5")
        raw = open(p, "rb").read()
        payload += raw[128:]
        refs.append(pillow_rgba(p))
    path = os.path.join(WORK, "bc3_mips.dds")
    open(path, "wb").write(dds_header(w, h, 0x80000, (0x4, 0x35545844, 0, 0, 0, 0, 0), mips=5) + payload)
    compare("DXT5 with 5 levels", tool, path, refs, 3)
    # fewer levels in the file than the header says: the ones there are used
    path = os.path.join(WORK, "bc3_mips_short.dds")
    open(path, "wb").write(dds_header(w, h, 0x80000, (0x4, 0x35545844, 0, 0, 0, 0, 0), mips=9) + payload)
    compare("DXT5 header says 9 levels, file has 5", tool, path, refs, 3)

    print("what a GPU does that DecompressBlockBC1/2/3 don't")
    def one_block_file(fourcc, block, w4=4, h4=4):
        return dds_header(w4, h4, 0x80000, (0x4, fourcc, 0, 0, 0, 0, 0)) + block
    # BC1 with color0 <= color1: 3 colours, index 3 is transparent black; indices 0,1,2,3 repeated
    c0, c1 = 0x001F, 0xF800  # blue, red (color0 < color1)
    idx = 0
    for p in range(16):
        idx |= (p % 4) << (2 * p)
    path = os.path.join(WORK, "bc1_punch.dds")
    open(path, "wb").write(one_block_file(0x31545844, struct.pack("<HHI", c0, c1, idx)))
    status, lv = dump(tool, path)
    px = lv[0][2].reshape(16, 4) if lv else None
    check(px is not None and list(px[3]) == [0, 0, 0, 0] and list(px[0][:3]) == [0, 0, 255] and px[3][3] == 0 and px[0][3] == 255,
          "BC1 punch-through: index 3 is transparent black, the others opaque: %s" % (None if px is None else [list(p) for p in px[:4]]))
    ref = pillow_rgba(path)
    check(lv and int(np.abs(lv[0][2].astype(int) - ref.astype(int)).max()) <= 2, "  ... and Pillow decodes the same block the same way")
    # BC3 with color0 <= color1 must still use 4 colours
    alpha_block = bytes([255, 255]) + b"\0" * 6  # alpha 255 everywhere
    path = os.path.join(WORK, "bc3_fourcolor.dds")
    open(path, "wb").write(one_block_file(0x35545844, alpha_block + struct.pack("<HHI", c0, c1, idx)))
    status, lv = dump(tool, path)
    px = lv[0][2].reshape(16, 4) if lv else None
    want2 = [(2 * 0 + 255) // 3 if False else None]
    # pixel with index 2: (2*c0 + c1) / 3 with c0 = blue (0,0,255), c1 = red (255,0,0): (85, 0, 170); index 3: (170, 0, 85)
    check(px is not None and list(px[2][:3]) == [85, 0, 170] and list(px[3][:3]) == [170, 0, 85] and px[3][3] == 255,
          "BC3 colour block uses 4 colours whatever the order of its endpoints: %s" % (None if px is None else [list(p) for p in px[2:4]]))

    print("files it must refuse without harm (also under AddressSanitizer)")
    good = open(os.path.join(WORK, "pil_DXT5.dds"), "rb").read()
    def refuses(name, data, why_contains=None):
        path = os.path.join(WORK, "bad.dds")
        open(path, "wb").write(data)
        status, lv = dump(asan, path)
        ok = status.startswith("FAIL") and not lv and (why_contains is None or why_contains in status)
        check(ok, "%s -> %s" % (name, status))
    refuses("empty file", b"")
    refuses("not a DDS", b"RIFF" + b"\0" * 200, "not a DDS")
    refuses("header cut short", good[:60], "not a DDS")
    refuses("data cut short", good[:128 + 100], "truncated")
    refuses("width 0", good[:12] + struct.pack("<I", 0) + good[16:], "size")
    refuses("width 100000", good[:16] + struct.pack("<I", 100000) + good[20:], "size")
    refuses("volume flag", good[:8] + struct.pack("<I", struct.unpack("<I", good[8:12])[0] | 0x800000) + good[12:], "volume")
    refuses("BC5 (ATI2)", good[:84] + b"ATI2" + good[88:], "compressed format")
    refuses("luminance 8-bit", dds_header(8, 8, 0, (0x20000, 0, 8, 0xff, 0, 0, 0)) + b"\0" * 64, "uncompressed format")
    refuses("DX10 array texture", dds_header(8, 8, 0, PF_DX10, dx10=98)[:128] + struct.pack("<5I", 98, 3, 0, 4, 0) + b"\0" * 200, "array")
    refuses("DX10 header cut off", dds_header(8, 8, 0, PF_DX10, dx10=98)[:140], "DX10")
    refuses("masks that are not one run", dds_header(8, 8, 0, (0x41, 0, 32, 0x00ff00ff, 0xff00, 0, 0xff000000)) + b"\0" * 256, "masks")

    print("random damage to good files: never a crash")
    seeds = [open(os.path.join(WORK, n), "rb").read() for n in ("pil_DXT1.dds", "pil_DXT5.dds", "bc7_62x46.dds", "raw_A8R8G8B8.dds", "bc3_mips.dds", "mips_4.dds")]
    rng = random.Random(1234)
    crashes = 0
    path = os.path.join(WORK, "fuzz.dds")
    for n in range(300):
        data = bytearray(rng.choice(seeds))
        for _ in range(rng.randint(1, 6)):
            pos = rng.randint(0, min(len(data) - 1, 200)) if rng.random() < 0.7 else rng.randint(0, len(data) - 1)
            data[pos] = rng.randint(0, 255)
        if rng.random() < 0.2:
            data = data[: rng.randint(0, len(data))]
        open(path, "wb").write(data)
        status, _ = dump(asan, path)
        if status.startswith("crash"):
            crashes += 1
            print("    ", status[:200])
            shutil.copy(path, os.path.join(WORK, "crash%d.dds" % crashes))
    check(crashes == 0, "300 damaged files, %d crashes or sanitizer reports" % crashes)

    print()
    if FAILS:
        print("%d check(s) FAILED (files in %s)" % (len(FAILS), WORK))
        return 1
    print("all checks passed")
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
