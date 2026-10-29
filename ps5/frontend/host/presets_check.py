#!/usr/bin/env python3
"""PS5 port frontend: checks assets/presets.ini, the Recommended button's settings (2026-10-29, AI-assisted): one section per id,
ids are "@global" or a disc serial, every line is a comment or key=value, resolution scales are 1-8, and the values that the
session reports set (Need for Speed: Most Wanted - Black Edition US at 4x, not 6x) stay as decided.

  presets_check.py <presets.ini>            (test-settings.sh runs it)

Copyright (C) 2026 swordpdf
SPDX-License-Identifier: GPL-3.0-or-later
"""
import re
import sys

# id -> {key: value} that must hold, with why.
EXPECTED = {
    # 2.15 reports: 6x ran at full speed 42% of the minutes (17 consoles), 4x 97% (17 consoles), 5x 98% (4).
    "SLUS-21351": {"upscale_multiplier": "4"},
}


def main():
    text = open(sys.argv[1], encoding="utf-8").read()
    sections, cur, errors = {}, None, []
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^\[(.+)\]$", line)
        if m:
            cur = m.group(1).strip()
            if cur in sections:
                errors.append("line %d: section [%s] twice" % (n, cur))
            if cur != "@global" and not re.match(r"^[A-Z]{4}-\d{5}$", cur):
                errors.append("line %d: [%s] is not @global or a serial" % (n, cur))
            sections[cur] = {}
            continue
        if cur is None:
            errors.append("line %d: a setting before any section" % n)
            continue
        if "=" not in line:
            errors.append("line %d: not key=value: %s" % (n, line))
            continue
        k, v = (x.strip() for x in line.split("=", 1))
        if k == "upscale_multiplier" and not (v.isdigit() and 1 <= int(v) <= 8):
            errors.append("line %d: [%s] upscale_multiplier=%s is not 1-8" % (n, cur, v))
        if k != "Patches/Enable":
            sections[cur][k] = v
    if "@global" not in sections:
        errors.append("no [@global] section")
    for sid, want in EXPECTED.items():
        for k, v in want.items():
            got = sections.get(sid, {}).get(k)
            if got != v:
                errors.append("[%s] %s=%s, expected %s" % (sid, k, got, v))
    for e in errors:
        print("FAIL: presets.ini: " + e)
    if not errors:
        print("PASS: presets.ini: %d sections, the report-based values hold" % len(sections))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
