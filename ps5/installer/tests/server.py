#!/usr/bin/env python3
"""Test HTTPS server standing in for GitHub (release API, download redirect, asset CDN) and the log relay.

Behaviour comes from state.json in the work folder, re-read on every request:
  release        the JSON returned by /repos/Swordpdf/PS5SX2/releases/latest
  api_status     HTTP status for that call (default 200); api_headers: extra headers
  api_chunked    send the API answer chunked
  assets_dir     where /assets/<name> files come from
  redirect_to    base URL the download redirects to (default: this server's /assets/)
  asset_chunked  send the zip chunked; asset_truncate: send only this many bytes then close
  relay_status   status for POST /v1/logs (default 201)
Requests are logged to requests.log; relay posts are saved as relay/<n>.body and relay/<n>.json (headers).
"""
import http.server
import json
import os
import socketserver
import ssl
import sys
import threading

WORK = sys.argv[1]
PORT = int(sys.argv[2])
CERT = os.path.join(WORK, 'server.pem')
KEY = os.path.join(WORK, 'server.key')
lock = threading.Lock()


def state():
    try:
        with open(os.path.join(WORK, 'state.json')) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def log(line):
    with lock, open(os.path.join(WORK, 'requests.log'), 'a') as f:
        f.write(line + '\n')


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        log('%s %s' % (self.command, self.path))

    def send_body(self, status, body, ctype='application/octet-stream', chunked=False, headers=None,
                  truncate=None):
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        for k, v in (headers or {}).items():
            self.send_header(k, str(v))
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        else:
            self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        if chunked:
            step = 7777
            for i in range(0, len(body), step):
                part = body[i:i + step]
                self.wfile.write(b'%x;ext=1\r\n' % len(part) + part + b'\r\n')
            self.wfile.write(b'0\r\nX-Trailer: yes\r\n\r\n')
        elif truncate is not None:
            self.wfile.write(body[:truncate])
            self.wfile.flush()
            self.connection.shutdown(2)
            return
        else:
            self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        st = state()
        p = self.path
        if p == '/repos/Swordpdf/PS5SX2/releases/latest':
            body = json.dumps(st.get('release', {}), indent=1).encode()
            if 'api_body' in st:
                body = st['api_body'].encode()
            return self.send_body(st.get('api_status', 200), body, 'application/json; charset=utf-8',
                                  st.get('api_chunked', False), st.get('api_headers', {}))
        prefix = '/Swordpdf/PS5SX2/releases/download/'
        if p.startswith(prefix):
            name = p.rsplit('/', 1)[1]
            base = st.get('redirect_to', 'https://localhost:%d/assets/' % PORT)
            self.send_response(302)
            self.send_header('Location', base + name + '?sp=r&sig=abc%2Fdef')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        if p.startswith('/assets/'):
            name = p[len('/assets/'):].split('?', 1)[0]
            path = os.path.join(st.get('assets_dir', WORK), name)
            if not os.path.isfile(path):
                return self.send_body(404, b'not found', 'text/plain')
            data = open(path, 'rb').read()
            return self.send_body(200, data, 'application/octet-stream', st.get('asset_chunked', False),
                                  truncate=st.get('asset_truncate'))
        return self.send_body(404, b'not found', 'text/plain')

    def do_POST(self):
        st = state()
        n = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(n)
        if self.path == '/v1/logs':
            rd = os.path.join(WORK, 'relay')
            os.makedirs(rd, exist_ok=True)
            with lock:
                k = len([x for x in os.listdir(rd) if x.endswith('.body')])
                open(os.path.join(rd, '%03d.body' % k), 'wb').write(body)
                json.dump(dict(self.headers.items()), open(os.path.join(rd, '%03d.json' % k), 'w'), indent=1)
            return self.send_body(st.get('relay_status', 201), b'{"ok":true}', 'application/json')
        return self.send_body(404, b'not found', 'text/plain')


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
srv = S(('127.0.0.1', PORT), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
open(os.path.join(WORK, 'server.ready'), 'w').write('ok')
srv.serve_forever()
