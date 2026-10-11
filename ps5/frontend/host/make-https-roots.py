#!/usr/bin/env python3
# PS5 port frontend: makes fe_https_roots.inc, Mozilla's roots for TLS servers as one PEM text for fe_https.cpp
# (2026-10-05, AI-assisted). The input is certifi's cacert.pem (github.com/certifi/python-certifi, Mozilla's NSS list
# with the TLS trust bit, MPL-2.0); the output keeps each certificate's name as a comment.
#
#   ps5/frontend/host/make-https-roots.py cacert.pem "certifi 2026.04.22" > ps5/frontend/fe_https_roots.inc
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import re
import sys

path, release = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
text = data.decode("ascii")
blocks = re.findall(r"((?:#[^\n]*\n)*)(-----BEGIN CERTIFICATE-----\n.*?-----END CERTIFICATE-----\n)", text, re.S)
if not blocks:
    sys.exit("no certificates in " + path)
out = [
    "// Mozilla's root certificates for TLS servers, from " + release + "'s cacert.pem",
    "// (SHA-256 " + hashlib.sha256(data).hexdigest() + ", " + str(len(blocks)) + " certificates, MPL-2.0).",
    "// Made by ps5/frontend/host/make-https-roots.py: don't edit by hand.",
]
for comments, pem in blocks:
    label = re.search(r"^# Label: \"?(.*?)\"?$", comments, re.M)
    out.append("// " + (label.group(1) if label else "(no label)"))
    for line in pem.splitlines():
        out.append('"' + line + '\\n"')
sys.stdout.write("\n".join(out) + "\n")
