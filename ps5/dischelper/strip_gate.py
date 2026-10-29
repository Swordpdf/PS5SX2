#!/usr/bin/env python3
# strip_gate.py (helper 171f, 2026-10-16, AI-assisted): the check that stops build_helper_merged.sh when the symbol-stripped helper is not the same program
# for the ELF loader as the unstripped link. Usage: strip_gate.py <unstripped.elf> <stripped.elf>; exit 0 = identical for the loader, 1 = it is not (the reason is printed).
# What the loader sees: the ELF header, the program headers, every byte of the file that a PT_LOAD maps, and the allocated sections (the dynamic section, the
# dynamic symbols and strings, the relocations). Only the section table and the sections that are not loaded (.symtab, .strtab, .comment ...) may change.
# Copyright (C) 2026 swordpdf; SPDX-License-Identifier: GPL-3.0-or-later
import struct
import sys

SHF_ALLOC = 2
SHT_SYMTAB = 2
SHT_DYNSYM = 11
PT_LOAD = 1


def load(path):
    with open(path, "rb") as f:
        return f.read()


def ehdr(b):
    if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 1:
        raise SystemExit("not a little-endian ELF64: " + repr(b[:6]))
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
        "<HHIQQQIHHHHHH", b, 16)
    return dict(type=e_type, machine=e_machine, version=e_version, entry=e_entry, phoff=e_phoff, shoff=e_shoff, flags=e_flags, ehsize=e_ehsize,
                phentsize=e_phentsize, phnum=e_phnum, shentsize=e_shentsize, shnum=e_shnum, shstrndx=e_shstrndx)


def phdrs(b, h):
    out = []
    for i in range(h["phnum"]):
        off = h["phoff"] + i * h["phentsize"]
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from("<IIQQQQQQ", b, off)
        out.append((p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align))
    return out


def cstr(b, off):
    end = b.index(b"\0", off)
    return b[off:end].decode("latin-1")


def sections(b, h):
    raw = []
    for i in range(h["shnum"]):
        off = h["shoff"] + i * h["shentsize"]
        raw.append(struct.unpack_from("<IIQQQQIIQQ", b, off))  # name, type, flags, addr, offset, size, link, info, addralign, entsize
    stroff = raw[h["shstrndx"]][4] if raw else 0
    out = []
    for r in raw:
        name = cstr(b, stroff + r[0]) if raw else ""
        out.append(dict(name=name, type=r[1], flags=r[2], addr=r[3], offset=r[4], size=r[5], link=r[6], info=r[7], addralign=r[8], entsize=r[9]))
    for s in out:  # the linked section by name, so that section numbers may move
        s["linkname"] = out[s["link"]]["name"] if 0 < s["link"] < len(out) else ""
    return out


def undefined_imports(b, secs):
    names = []
    for s in secs:
        if s["type"] != SHT_DYNSYM:
            continue
        strtab = secs[s["link"]]
        for k in range(s["size"] // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", b, s["offset"] + k * 24)
            if st_shndx == 0 and st_name:
                names.append(cstr(b, strtab["offset"] + st_name))
    return sorted(names)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = load(sys.argv[1]), load(sys.argv[2])
    ha, hb = ehdr(a), ehdr(b)
    bad = []

    for k in ("type", "machine", "version", "entry", "phoff", "flags", "ehsize", "phentsize", "phnum", "shentsize"):
        if ha[k] != hb[k]:
            bad.append("ELF header %s differs: %#x vs %#x" % (k, ha[k], hb[k]))

    pa, pb = phdrs(a, ha), phdrs(b, hb)
    if pa != pb:
        bad.append("program headers differ")

    end = max([p[2] + p[5] for p in pa if p[0] == PT_LOAD] or [0])
    ends = max([p[2] + p[5] for p in pa] or [0])
    if ends > end:
        bad.append("a program header (not PT_LOAD) reaches %#x, beyond the last loaded byte %#x" % (ends, end))
    if len(b) < end:
        bad.append("the stripped file (%d bytes) is shorter than the loaded range (%d)" % (len(b), end))
    else:
        # the ELF header: everything but e_shoff (0x28), e_shnum (0x3c) and e_shstrndx (0x3e); then every byte up to the end of the last loaded segment
        mask = [(0x28, 0x30), (0x3C, 0x40)]
        ma, mb = bytearray(a[:64]), bytearray(b[:64])
        for lo, hi in mask:
            ma[lo:hi] = bytes(hi - lo)
            mb[lo:hi] = bytes(hi - lo)
        if ma != mb:
            bad.append("the ELF header differs outside the section table fields")
        if a[64:end] != b[64:end]:
            first = next(i for i in range(64, end) if a[i] != b[i])
            bad.append("the loaded bytes differ, first at file offset %#x" % first)

    sa, sb = sections(a, ha), sections(b, hb)
    by_name_b = {}
    for s in sb:
        by_name_b.setdefault(s["name"], []).append(s)
    keys = ("type", "flags", "addr", "offset", "size", "linkname", "info", "addralign", "entsize")
    n_alloc = 0
    for s in sa:
        if not s["flags"] & SHF_ALLOC:
            continue
        n_alloc += 1
        cand = [t for t in by_name_b.get(s["name"], []) if all(t[k] == s[k] for k in keys)]
        if len(cand) != 1:
            bad.append("allocated section %s is not in the stripped file as it was" % s["name"])
    n_alloc_b = sum(1 for s in sb if s["flags"] & SHF_ALLOC)
    if n_alloc_b != n_alloc:
        bad.append("the stripped file has %d allocated sections, the unstripped one %d" % (n_alloc_b, n_alloc))
    if any(s["type"] == SHT_SYMTAB for s in sb):
        bad.append("the stripped file still has a symbol table")
    if not any(s["type"] == SHT_SYMTAB for s in sa):
        print("note: the unstripped file has no symbol table either (nothing was stripped)")

    ia, ib = undefined_imports(a, sa), undefined_imports(b, sb)
    if ia != ib:
        bad.append("the dynamic import lists differ")

    print("strip gate: %d -> %d bytes, loaded range [0, %#x), %d allocated sections identical, %d dynamic imports identical, program headers %s" %
          (len(a), len(b), end, n_alloc, len(ia), "identical" if pa == pb else "DIFFERENT"))
    if bad:
        for m in bad:
            print("strip gate FAILED: " + m)
        return 1
    print("strip gate: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
