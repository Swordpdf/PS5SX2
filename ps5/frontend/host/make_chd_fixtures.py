#!/usr/bin/env python3
# PS5 port frontend, PC tests (chdscan, AI-assisted): CHD v5 images for the shelf's scan tests, made here because chdman is not on
# the build machine. The map is compressed as chdman compresses it (Huffman-coded hunk types with run lengths, then each hunk's length,
# CRC16 or self reference), so libchdr reads it as it reads a real one and the map costs what a real one of that size costs. Only the
# first hunks hold data (an ISO 9660 volume with SYSTEM.CNF, as test-serialcache's images); the rest of the image is a hole in a sparse
# file that the map's lengths point into, so a 4 GB DVD image takes a few MB on disk.
#
#   make_chd_fixtures.py <out.chd> dvd|cd <logical MB> <hunk bytes> <boot ELF, e.g. SLUS_203.70> [--iso <out.iso>] [--elf-lba N] [--no-volume]
#
# --elf-lba: the boot ELF is a file at sector N (6000 bytes, elf_bytes()), far into the image (a read past the first hunks).
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
import random
import struct
import sys
import zlib

CRC16_TABLE = []
for i in range(256):
    c = i << 8
    for _ in range(8):
        c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
    CRC16_TABLE.append(c & 0xFFFF)


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc = ((crc << 8) ^ CRC16_TABLE[(crc >> 8) ^ b]) & 0xFFFF
    return crc


class Bits:
    def __init__(self):
        self.acc, self.n, self.out = 0, 0, bytearray()

    def put(self, value, nbits):
        if nbits == 0:
            return
        self.acc = (self.acc << nbits) | (value & ((1 << nbits) - 1))
        self.n += nbits
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def flush(self):
        if self.n:
            self.out.append((self.acc << (8 - self.n)) & 0xFF)
            self.acc, self.n = 0, 0
        return bytes(self.out)


def huffman_lengths(histo, maxbits=8):
    import heapq
    used = [i for i, n in enumerate(histo) if n]
    lengths = [0] * len(histo)
    if len(used) == 1:
        lengths[used[0]] = 1
        return lengths
    counts = list(histo)
    while True:
        heap = [(counts[i], i, (i,)) for i in used]
        heapq.heapify(heap)
        depth = {i: 0 for i in used}
        k = len(histo)
        while len(heap) > 1:
            a = heapq.heappop(heap)
            b = heapq.heappop(heap)
            for s in a[2] + b[2]:
                depth[s] += 1
            heapq.heappush(heap, (a[0] + b[0], k, a[2] + b[2]))
            k += 1
        if max(depth.values()) <= maxbits:
            for s, d in depth.items():
                lengths[s] = d
            return lengths
        counts = [(c + 1) // 2 if c else 0 for c in counts]  # flatter, until the deepest code fits


def canonical_codes(lengths):
    histo = [0] * 33
    for n in lengths:
        histo[n] += 1
    start = [0] * 33
    cur = 0
    for codelen in range(32, 0, -1):
        nxt = (cur + histo[codelen]) >> 1
        start[codelen] = cur
        cur = nxt
    codes = [0] * len(lengths)
    for sym, n in enumerate(lengths):
        if n:
            codes[sym] = start[n]
            start[n] += 1
    return codes


T0, T1, T2, T3, NONE, SELF, PARENT, RLE_SMALL, RLE_LARGE, SELF_0, SELF_1 = range(11)


def compress_map(entries, hunkbytes, firstoffs):
    """entries: per hunk (type, length, crc, selfref) with type T0..T3, NONE or SELF. Returns (map bytes incl. 16-byte header, raw map)."""
    # promote self references, as chdman does
    types, last_self = [], 0
    for t, _, _, ref in entries:
        if t == SELF:
            if ref == last_self:
                t = SELF_0
            elif ref == last_self + 1:
                t = SELF_1
            last_self = ref
        types.append(t)
    # the type stream: runs of three or more as RLE codes (the decoder's lastcomp starts at 0)
    syms, i, last = [], 0, 0
    while i < len(types):
        t = types[i]
        run = 1
        while i + run < len(types) and types[i + run] == t:
            run += 1
        i += run
        if t != last:
            syms.append(t)
            last = t
            run -= 1
        while run > 0:
            if run >= 19:
                k = min(run, 19 + 255)
                syms += [RLE_LARGE, (k - 19) >> 4, (k - 19) & 15]
                run -= k
            elif run >= 3:
                syms += [RLE_SMALL, run - 3]
                run = 0
            else:
                syms.append(t)
                run -= 1
    histo = [0] * 16
    for s in syms:
        histo[s] += 1
    lengths = huffman_lengths(histo)
    codes = canonical_codes(lengths)
    max_len = max([e[1] for e in entries if e[0] <= T3] + [0])
    max_self = max([e[3] for e in entries if e[0] == SELF] + [0])
    lengthbits = max(max_len.bit_length(), 1)
    selfbits = max(max_self.bit_length(), 1)
    bits = Bits()
    for n in lengths:  # the tree, 4 bits a code length (maxbits 8); a 1 is written twice
        if n == 1:
            bits.put(1, 4)
        bits.put(n, 4)
    for s in syms:
        bits.put(codes[s], lengths[s])
    raw = bytearray()
    offset = firstoffs
    last_self = 0
    for (t0, length, crc, ref), t in zip(entries, types):
        if t <= T3:
            bits.put(length, lengthbits)
            bits.put(crc, 16)
            raw += struct.pack(">B", t0) + length.to_bytes(3, "big") + offset.to_bytes(6, "big") + struct.pack(">H", crc)
            offset += length
        elif t == NONE:
            bits.put(crc, 16)
            raw += struct.pack(">B", NONE) + hunkbytes.to_bytes(3, "big") + offset.to_bytes(6, "big") + struct.pack(">H", crc)
            offset += hunkbytes
        else:
            if t == SELF:
                bits.put(ref, selfbits)
                last_self = ref
            elif t == SELF_1:
                last_self += 1
            raw += struct.pack(">B", SELF) + (0).to_bytes(3, "big") + last_self.to_bytes(6, "big") + struct.pack(">H", 0)
    data = bits.flush()
    return data, bytes(raw), lengthbits, selfbits


def elf_bytes(n=6000):
    return bytes((i * 7 + 3) & 0xFF for i in range(n))  # the test computes the same


def iso_sectors(boot_elf, sectors=64, elf_lba=0):
    """The first `sectors` sectors; with elf_lba, the root also lists the boot ELF (elf_bytes()) at that sector."""
    img = bytearray(sectors * 2048)

    def le32(at, v):
        img[at:at + 4] = struct.pack("<I", v)

    pvd = 16 * 2048
    img[pvd] = 1
    img[pvd + 1:pvd + 6] = b"CD001"
    img[pvd + 6] = 1
    root = pvd + 156
    img[root] = 34
    le32(root + 2, 18)
    le32(root + 10, 2048)
    img[root + 25] = 2
    img[root + 32] = 1
    term = 17 * 2048
    img[term] = 255
    img[term + 1:term + 6] = b"CD001"
    cnf = ("BOOT2 = cdrom0:\\" + boot_elf + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n").encode()
    name = b"SYSTEM.CNF;1"
    rec = 18 * 2048
    img[rec] = 33 + len(name) + ((33 + len(name)) & 1)
    le32(rec + 2, 19)
    le32(rec + 10, len(cnf))
    img[rec + 32] = len(name)
    img[rec + 33:rec + 33 + len(name)] = name
    img[19 * 2048:19 * 2048 + len(cnf)] = cnf
    if elf_lba:
        ename = (boot_elf + ";1").encode()
        r2 = rec + img[rec]
        img[r2] = 33 + len(ename) + ((33 + len(ename)) & 1)
        le32(r2 + 2, elf_lba)
        le32(r2 + 10, len(elf_bytes()))
        img[r2 + 32] = len(ename)
        img[r2 + 33:r2 + 33 + len(ename)] = ename
    rnd = random.Random(7)
    for s in range(20, sectors):  # some file data after it
        img[s * 2048:(s + 1) * 2048] = bytes(rnd.getrandbits(4) for _ in range(2048))
    return bytes(img)


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def main():
    out, kind, logical_mb, hunkbytes, boot = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    iso_out = sys.argv[sys.argv.index("--iso") + 1] if "--iso" in sys.argv else None
    elf_lba = int(sys.argv[sys.argv.index("--elf-lba") + 1]) if "--elf-lba" in sys.argv else 0
    iso = iso_sectors(boot, elf_lba=elf_lba)
    sectors = {s: iso[s * 2048:(s + 1) * 2048] for s in range(len(iso) // 2048)}
    if "--no-volume" in sys.argv:  # no ISO 9660 volume (another system's disc, an audio track first)
        sectors[16] = bytes(2048)
    elf = elf_bytes()
    for k in range(0, len(elf), 2048) if elf_lba else []:
        sectors[elf_lba + k // 2048] = elf[k:k + 2048].ljust(2048, b"\0")
    cd = kind == "cd"
    unit = 2448 if cd else 2048
    if hunkbytes % unit:
        raise SystemExit("the hunk size must be a multiple of %d" % unit)
    logical = logical_mb << 20
    logical -= logical % unit
    hunks = (logical + hunkbytes - 1) // hunkbytes
    # the image's first bytes as the CHD holds them: 2048-byte sectors, or 2448-byte frames (MODE2_RAW: 24 bytes of sync and
    # header, the sector's 2048 bytes, EDC/ECC left zero, 96 bytes of subcode)
    def unit_bytes(data):
        if not cd:
            return data
        frame = bytearray(2448)
        frame[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
        frame[15] = 2
        frame[24:24 + 2048] = data
        return bytes(frame)

    real_hunks = sorted({s * unit // hunkbytes for s in sectors} | {((s + 1) * unit - 1) // hunkbytes for s in sectors})
    real = (len(iso) // 2048 * unit + hunkbytes - 1) // hunkbytes  # the hunks of the first sectors

    def hunk_data(h):
        out = bytearray(hunkbytes)
        for s in range(h * hunkbytes // unit, (h + 1) * hunkbytes // unit + 1):
            if s in sectors:
                frame = unit_bytes(sectors[s])
                for i in range(unit):
                    at = s * unit + i - h * hunkbytes
                    if 0 <= at < hunkbytes:
                        out[at] = frame[i]
        return bytes(out)

    rnd = random.Random(logical_mb * 7919 + hunkbytes)
    entries, payloads = [], []
    pad_from = int(hunks * 0.75)  # the last quarter: zero padding, each hunk a copy of the first zero one
    for h in range(hunks):
        if h in real_hunks:
            data = hunk_data(h)
            if cd:  # cdzl: the ECC bitmap (none), the base's compressed length, the sectors' data deflated, the subcode deflated
                frames = hunkbytes // 2448
                base = b"".join(data[f * 2448:f * 2448 + 2352] for f in range(frames))
                sub = b"".join(data[f * 2448 + 2352:(f + 1) * 2448] for f in range(frames))
                zb, zs = deflate(base), deflate(sub)
                ecc = (frames + 7) // 8
                comp = bytes(ecc) + (len(zb).to_bytes(2 if hunkbytes < 65536 else 3, "big")) + zb + zs
            else:
                comp = deflate(data)
            entries.append((T1, len(comp), crc16(data), 0))
            payloads.append(comp)
        elif h == pad_from:
            entries.append((T0, 40, 0, 0))  # the first zero hunk, stored (lzma of zeros is tiny)
            payloads.append(None)
        elif h > pad_from:
            entries.append((SELF, 0, 0, pad_from))  # the rest of the padding: copies of it
            payloads.append(None)
        else:
            r = rnd.random()
            if r < 0.02 and h > real and h not in real_hunks:
                entries.append((SELF, 0, 0, rnd.randrange(real, h)))
            elif r < 0.04:
                entries.append((NONE, hunkbytes, rnd.getrandbits(16), 0))
            else:
                t = T0 if r < 0.62 else T1 if r < 0.92 else T2
                entries.append((t, rnd.randrange(hunkbytes * 3 // 10, hunkbytes * 95 // 100), rnd.getrandbits(16), 0))
            payloads.append(None)
    if cd:
        codecs = [b"cdlz", b"cdzl", b"cdfl", b"\0\0\0\0"]
        meta_text = ("TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0" % (logical // 2448)).encode() + b"\0"
        meta = struct.pack(">II Q", int.from_bytes(b"CHT2", "big"), (1 << 24) | len(meta_text), 0) + meta_text
    else:
        codecs = [b"lzma", b"zlib", b"huff", b"flac"]
        meta = b""
    metaoffset = 124 if meta else 0
    firstoffs = 124 + len(meta)
    mapdata, rawmap, lengthbits, selfbits = compress_map(entries, hunkbytes, firstoffs)
    data_bytes = sum(e[1] for e in entries if e[0] in (T0, T1, T2, T3, NONE))
    mapoffset = firstoffs + data_bytes
    header = bytearray(124)
    header[0:8] = b"MComprHD"
    struct.pack_into(">II", header, 8, 124, 5)
    for i, c in enumerate(codecs):
        header[16 + 4 * i:20 + 4 * i] = c
    struct.pack_into(">QQQII", header, 32, logical, mapoffset, metaoffset, hunkbytes, unit)
    maphdr = struct.pack(">I", len(mapdata)) + firstoffs.to_bytes(6, "big") + struct.pack(">HBBBB", crc16(rawmap), lengthbits, selfbits, 0, 0)
    with open(out, "wb") as f:
        f.write(header)
        f.write(meta)
        pos = firstoffs
        for e, p in zip(entries, payloads):
            if e[0] in (SELF,):
                continue
            if p is not None:
                f.seek(pos)
                f.write(p)
            pos += e[1]
        f.seek(mapoffset)
        f.write(maphdr)
        f.write(mapdata)
    if iso_out:  # the same disc as a plain ISO of the same logical size (sparse too)
        with open(iso_out, "wb") as f:
            f.write(iso)
            f.truncate((logical // unit) * 2048)
    print("%s: %s, %d MB, %d hunks of %d bytes, map %d bytes (types and fields), %d real hunks" % (out, kind, logical_mb, hunks, hunkbytes,
          len(mapdata), real))


if __name__ == "__main__":
    main()
