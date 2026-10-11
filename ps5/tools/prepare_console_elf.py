#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Produce the ELF form observed after the live loader consumes tail metadata."""
from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path
from struct import unpack_from


def main() -> None:
    parser = ArgumentParser()
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()

    source = args.input.read_bytes()
    if not source.startswith(b'\x7fELF'):
        raise SystemExit('input is not an ELF image')

    marker = source.rfind(b'PATH')
    if marker < len(source) - 0x1000 or marker + 12 > len(source):
        raise SystemExit('terminal PATH comment was not found')
    record_tail_size = unpack_from('<I', source, marker + 4)[0]
    file_name_size = unpack_from('<I', source, marker + 8)[0]
    comment_end = marker + 8 + record_tail_size
    if record_tail_size < 4 + file_name_size or comment_end >= len(source):
        raise SystemExit('terminal PATH comment has invalid lengths')
    if b'SIE\0' not in source[-0x40:]:
        raise SystemExit('expected terminal SIE note was not found')

    output = bytearray(source)
    output[comment_end:] = b'\0' * (len(output) - comment_end)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    print(f'console-ready metadata boundary: 0x{comment_end:x}')
    print(f'console-ready sha256: {sha256(output).hexdigest()}')


if __name__ == '__main__':
    main()
