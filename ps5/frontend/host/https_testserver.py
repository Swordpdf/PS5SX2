#!/usr/bin/env python3
# PS5 port frontend: the local HTTPS servers for https_test.cpp (2026-10-05, AI-assisted). Makes a test PKI shaped like
# archive.org's (the leaf, an intermediate, the new root sent cross-signed by an old root, and that old root itself:
# a client must stop at the trusted new root), an ECDSA chain, an expired leaf and a leaf for another name, then serves
# each on its own port. Prints {"ports": {...}, "root": "<new root PEM>", "ecroot": "<EC root PEM>"} on one line and
# serves until killed.
#
#   https_testserver.py <work dir>
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
import datetime
import hashlib
import http.server
import json
import os
import random
import socketserver
import ssl
import sys
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

work = sys.argv[1]
os.makedirs(work, exist_ok=True)
now = datetime.datetime.now(datetime.timezone.utc)


def name(cn):
    return x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, "PS5SX2 test"), x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def cert(subject, key, issuer, issuer_key, ca, days=(-30, 365), sans=None, sha=hashes.SHA256()):
    b = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer).public_key(key.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(now + datetime.timedelta(days=days[0])).not_valid_after(now + datetime.timedelta(days=days[1])))
    if ca:
        b = b.add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        b = b.add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
                                          data_encipherment=False, key_agreement=False, key_cert_sign=True, crl_sign=True,
                                          encipher_only=False, decipher_only=False), critical=True)
    else:
        b = b.add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(s) for s in sans]), critical=False)
    b = b.add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
    b = b.add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(issuer_key.public_key()), critical=False)
    return b.sign(issuer_key, sha)


def pem(c):
    return c.public_bytes(serialization.Encoding.PEM).decode()


def rsa_key():
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


# archive.org's shape: *.archive.org <- Go Daddy Secure CA G2 <- Go Daddy Root G2 (cross-signed by Class 2) <- Class 2.
old_key, new_key, int_key, leaf_key = rsa_key(), rsa_key(), rsa_key(), rsa_key()
old_root = cert(name("Test Old Root"), old_key, name("Test Old Root"), old_key, True)  # like Class 2 (not trusted)
new_root = cert(name("Test New Root"), new_key, name("Test New Root"), new_key, True)
new_cross = cert(name("Test New Root"), new_key, name("Test Old Root"), old_key, True)
inter = cert(name("Test Secure CA"), int_key, name("Test New Root"), new_key, True)
leaf = cert(name("localhost"), leaf_key, name("Test Secure CA"), int_key, False, sans=["localhost"])
expired = cert(name("localhost"), leaf_key, name("Test Secure CA"), int_key, False, days=(-60, -1), sans=["localhost"])
other = cert(name("other.test"), leaf_key, name("Test Secure CA"), int_key, False, sans=["other.test"])

# An ECDSA chain (P-384 root, P-256 leaf), as Google's and Let's Encrypt's ECDSA chains are.
ec_root_key, ec_leaf_key = ec.generate_private_key(ec.SECP384R1()), ec.generate_private_key(ec.SECP256R1())
ec_root = cert(name("Test EC Root"), ec_root_key, name("Test EC Root"), ec_root_key, True, sha=hashes.SHA384())
ec_leaf = cert(name("localhost"), ec_leaf_key, name("Test EC Root"), ec_root_key, False, sans=["localhost"], sha=hashes.SHA384())


def write(fname, certs, key):
    p = os.path.join(work, fname)
    with open(p, "w") as f:
        f.write("".join(pem(c) for c in certs))
    k = os.path.join(work, fname + ".key")
    with open(k, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return p, k


chains = {
    "main": write("main.pem", [leaf, inter, new_cross, old_root], leaf_key),  # archive.org's order and length
    "second": write("second.pem", [leaf, inter, new_cross, old_root], leaf_key),  # a redirect target
    "ec": write("ec.pem", [ec_leaf], ec_leaf_key),
    "expired": write("expired.pem", [expired, inter, new_cross, old_root], leaf_key),
    "other": write("other.pem", [other, inter, new_cross, old_root], leaf_key),
}

# The file the servers hand out: 5 MiB that the test can make again from the same seed.
rng = random.Random(1234)
BLOB = bytes(rng.getrandbits(8) for _ in range(5 * 1024 * 1024))
ports = {}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def raw(self, data):
        self.wfile.write(data)
        self.wfile.flush()

    def send_blob(self, chunked=False, length=True):
        start, end = 0, len(BLOB) - 1
        status = 200
        rng_hdr = self.headers.get("Range")
        if rng_hdr and rng_hdr.startswith("bytes="):
            a, b = rng_hdr[6:].split("-")
            start, end = int(a), min(int(b), len(BLOB) - 1)
            status = 206
        body = BLOB[start:end + 1]
        self.send_response(status)
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(BLOB)))
        self.send_header("Content-Type", "application/octet-stream")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        elif length:
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if chunked:
            r = random.Random(len(body))
            i = 0
            while i < len(body):
                n = r.choice([1, 7, 100, 4096, 16385, 70000])
                part = body[i:i + n]
                self.raw(b"%x;ext=1\r\n" % len(part) + part + b"\r\n")
                i += n
            self.raw(b"0\r\nX-Trailer: yes\r\n\r\n")
        else:
            self.raw(body)
        self.close_connection = True

    def redirect(self, location, code=302):
        self.send_response(code)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def do_GET(self):
        p = self.path
        if p == "/file":
            self.send_blob()
        elif p == "/chunked":
            self.send_blob(chunked=True)
        elif p == "/nolength":
            self.send_blob(length=False)
        elif p == "/redirect":
            self.redirect("https://localhost:%d/file" % ports["second"])
        elif p == "/redirect-rel":
            self.redirect("file", 307)
        elif p == "/redirect-slash":
            self.redirect("/file", 301)
        elif p == "/redirect-http":
            self.redirect("http://localhost:%d/file" % ports["second"])
        elif p == "/loop":
            self.redirect("/loop")
        elif p == "/norange":
            self.send_response(200)
            self.send_header("Content-Length", str(len(BLOB)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.raw(BLOB)
            self.close_connection = True
        elif p == "/missing":
            self.send_response(404)
            self.send_header("Content-Length", "9")
            self.send_header("Connection", "close")
            self.end_headers()
            self.raw(b"not here\n")
        elif p == "/wrongrange":
            self.send_response(206)
            self.send_header("Content-Range", "bytes 0-9/%d" % len(BLOB))
            self.send_header("Content-Length", "10")
            self.send_header("Connection", "close")
            self.end_headers()
            self.raw(BLOB[:10])
        elif p == "/continue":
            self.raw(b"HTTP/1.1 100 Continue\r\n\r\n")
            self.send_blob()
        elif p == "/short":
            self.send_response(200)
            self.send_header("Content-Length", "1000")
            self.send_header("Connection", "close")
            self.end_headers()
            self.raw(BLOB[:400])
        elif p == "/slow":
            self.send_response(200)
            self.send_header("Content-Length", str(len(BLOB)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.raw(BLOB[:1000])
            time.sleep(20)
        else:
            self.send_response(400)
            self.send_header("Content-Length", "0")
            self.end_headers()


    # pr9n: an API's POST (RetroAchievements' form posts) and its errors, whose JSON bodies the client must deliver.
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        if self.path == "/echo":
            out = ("method=POST\nlength=%d\ntype=%s\nagent=%s\nbody=" % (length, self.headers.get("Content-Type", ""),
                                                                             self.headers.get("User-Agent", ""))).encode() + body
            self.send_response(201)
        elif self.path == "/api-error":
            out = b'{"Success":false,"Error":"Invalid user/password combination. Please try again."}'
            self.send_response(401)
        else:
            out = b""
            self.send_response(404)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(out)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.raw(out)
        self.close_connection = True


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass  # the tests hang up early on purpose (a stopped sink, Abort)


def serve(key, files):
    certfile, keyfile = files
    srv = Server(("127.0.0.1", 0), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(certfile, keyfile)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    ports[key] = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()


for k, files in chains.items():
    serve(k, files)
print(json.dumps({"ports": ports, "root": pem(new_root), "ecroot": pem(ec_root),
                  "blob_sha256": hashlib.sha256(BLOB).hexdigest()}), flush=True)
while True:
    time.sleep(3600)
