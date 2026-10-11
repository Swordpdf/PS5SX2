#!/usr/bin/env python3
"""Host tests for the PS5SX2 Installer (build/host/ps5sx2-installer, ASan+UBSan).

Every test runs the real program against a fake /data under a temporary root, with a local HTTPS server
standing in for GitHub and the log relay (tests/server.py, its own test CA). The console's own listing
(lsall.txt) and the real vk-285-112 zip are used when given:
  PS5SX2_TEST_REAL_ZIP=/path/PS5SX2-TestBuild1-vk-285-112.zip  PS5SX2_TEST_LSALL=/path/lsall.txt
"""
import hashlib
import io
import json
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
BIN = os.path.join(TOP, 'build', 'host', 'ps5sx2-installer')
WORK = os.path.join(TOP, 'build', 'test-work')
REAL_ZIP = os.environ.get('PS5SX2_TEST_REAL_ZIP')
LSALL = os.environ.get('PS5SX2_TEST_LSALL')
PORT = None
SERVER = None
RESULTS = []

TITLE = 'PPSA99203'


# ---------------------------------------------------------------- helpers

def sh(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw)


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def sha_bytes(b):
    return hashlib.sha256(b).hexdigest()


def snapshot(base, skip_work=True):
    """relative path -> ('d',) or ('f', size, sha256, mode) or ('l', target)"""
    out = {}
    for dirpath, dirnames, filenames in os.walk(base):
        rel_dir = os.path.relpath(dirpath, base)
        if skip_work and (rel_dir == 'data/PS5SX2-Installer' or rel_dir.startswith('data/PS5SX2-Installer/')):
            dirnames[:] = []
            continue
        for d in dirnames:
            p = os.path.join(dirpath, d)
            rel = os.path.normpath(os.path.join(rel_dir, d))
            if os.path.islink(p):
                out[rel] = ('l', os.readlink(p))
            elif not (skip_work and rel == 'data/PS5SX2-Installer'):
                out[rel] = ('d',)
        for f in filenames:
            p = os.path.join(dirpath, f)
            rel = os.path.normpath(os.path.join(rel_dir, f))
            if rel == 'notifications.txt':
                continue
            if os.path.islink(p):
                out[rel] = ('l', os.readlink(p))
            else:
                st = os.stat(p)
                out[rel] = ('f', st.st_size, sha(p), st.st_mode & 0o7777)
    return out


def diff(a, b):
    added = sorted(set(b) - set(a))
    removed = sorted(set(a) - set(b))
    changed = sorted(k for k in set(a) & set(b) if a[k][:3] != b[k][:3])
    return added, removed, changed


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def set_state(**kw):
    st = {'assets_dir': os.path.join(WORK, 'assets')}
    st.update(kw)
    with open(os.path.join(WORK, 'state.json'), 'w') as f:
        json.dump(st, f)


def release_json(tag, name, data, digest=None, size=None, url=None, extra_assets=()):
    assets = list(extra_assets) + [{
        'url': 'https://api.github.com/repos/Swordpdf/PS5SX2/releases/assets/1',
        'id': 1, 'name': name, 'label': '', 'content_type': 'application/zip', 'state': 'uploaded',
        'size': len(data) if size is None else size,
        'digest': 'sha256:' + sha_bytes(data) if digest is None else digest,
        'browser_download_url': url or 'https://localhost:%d/Swordpdf/PS5SX2/releases/download/%s/%s' % (PORT, tag, name),
    }]
    return {'tag_name': tag, 'name': 'PS5SX2 %s' % tag, 'draft': False, 'prerelease': False,
            'published_at': '2026-09-28T17:15:57Z', 'target_commitish': 'main',
            'body': 'Notes with · unicode, "quotes", a tab\t and \\ escapes 😀',
            'assets': assets}


def publish(tag, data, name=None, **kw):
    name = name or 'PS5SX2-TestBuild1-%s.zip' % tag
    os.makedirs(os.path.join(WORK, 'assets'), exist_ok=True)
    with open(os.path.join(WORK, 'assets', name), 'wb') as f:
        f.write(data)
    state_kw = {k: kw.pop(k) for k in list(kw) if k in ('api_status', 'api_headers', 'api_chunked', 'redirect_to',
                                                       'asset_chunked', 'asset_truncate', 'relay_status', 'api_body')}
    set_state(release=release_json(tag, name, data, **kw), **state_kw)
    return name


def run(root, env_extra=None, timeout=300, expect_rc=0, logger=False):
    env = dict(os.environ)
    env.update({
        'PS5SX2_ROOT': root,
        'PS5SX2_TEST_CA_FILE': os.path.join(WORK, 'ca.pem'),
        'PS5SX2_TEST_API_URL': 'https://localhost:%d/repos/Swordpdf/PS5SX2/releases/latest' % PORT,
        'PS5SX2_TEST_DOWNLOAD_PREFIX': 'https://localhost:%d/Swordpdf/PS5SX2/releases/download/' % PORT,
        'PS5SX2_TEST_ALLOW_HOST': 'localhost',
        'PS5SX2_TEST_RELAY_URL': 'https://localhost:%d/v1/logs' % PORT,
        'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1',
        'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
    })
    if not logger:
        env['PS5SX2_TEST_NO_LOGGER'] = '1'
    env.update(env_extra or {})
    nf = os.path.join(root, 'notifications.txt')
    before = open(nf).read().splitlines() if os.path.exists(nf) else []
    p = subprocess.run([BIN], env=env, capture_output=True, text=True, timeout=timeout)
    after = open(nf).read().splitlines() if os.path.exists(nf) else []
    notes = after[len(before):]
    if expect_rc is not None and p.returncode != expect_rc:
        print(p.stderr[-6000:])
        raise AssertionError('exit code %d (expected %d)' % (p.returncode, expect_rc))
    for bad in ('ERROR: AddressSanitizer', 'runtime error:', 'LeakSanitizer'):
        if bad in p.stderr:
            print(p.stderr[-6000:])
            raise AssertionError('sanitizer: ' + bad)
    return notes, p.stderr


def data(root, *parts):
    return os.path.join(root, 'data', *parts)


def zip_entries(zbytes):
    z = zipfile.ZipFile(io.BytesIO(zbytes))
    top = z.namelist()[0].split('/')[0]
    files = {}
    for i in z.infolist():
        if i.is_dir():
            continue
        rel = i.filename[len(top) + 1:]
        files[rel] = z.read(i)
    return top, files


def build_zip(top, files, dirs=(), mutate=None):
    """files: rel -> bytes (under top/). Directory entries for bios/ and games/ like the real zip."""
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, 'w', zipfile.ZIP_DEFLATED) as z:
        for d in dirs:
            zi = zipfile.ZipInfo(top + '/' + d.rstrip('/') + '/')
            zi.external_attr = (0o40755 << 16) | 0x10
            z.writestr(zi, b'')
        for rel, content in files.items():
            zi = zipfile.ZipInfo(top + '/' + rel)
            zi.external_attr = 0o100700 << 16
            zi.create_system = 3
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, content)
        if mutate:
            mutate(z)
    return bio.getvalue()


def eboot_with(tag, filler=b''):
    return b'\x7fELF fake eboot ' + filler + b'\x00[boot] build=' + tag.encode() + b'\x00' + os.urandom(3000)


# ---------------------------------------------------------------- fake consoles

def mk_fresh(root):
    os.makedirs(data(root), exist_ok=True)


def mk_user(root, files112, eboot_tag='vk-285-111', lsall=LSALL, small=False):
    """The user's console as listed by lsall.txt, with small made-up contents; some files equal the zip's."""
    pc = data(root, 'PCSX2')
    os.makedirs(pc, exist_ok=True)
    rnd = random.Random(1)
    listing = []
    if lsall and os.path.exists(lsall):
        for line in open(lsall, encoding='utf-8', errors='replace'):
            parts = line.rstrip('\n').split(' ', 5)
            if len(parts) == 6:
                listing.append((parts[0], parts[5]))
    for kind, rel in listing:
        if rel.startswith('cache/ps5vk-shader-cache/') and rnd.random() > 0.01:
            continue
        if small and rel.startswith('patches/') and rnd.random() > 0.02:
            continue
        p = os.path.join(pc, rel)
        if kind == 'd':
            os.makedirs(p, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w') as f:
                f.write('user file %s\n' % rel)
    for d in ('bios', 'games', 'memcards', 'savestates', 'flags', 'flags-off', 'settings', 'patches', 'cheats',
              'logs', 'textures/SCUS-97199/replacements'):
        os.makedirs(os.path.join(pc, d), exist_ok=True)
    for name, content in (('bios/SCPH-90001_BIOS_V18_USA_230.ROM0', b'BIOS' * 4096), ('games/Black (USA).iso', b'ISO' * 5000),
                          ('memcards/Mcd001.ps2', b'CARD1' * 1000), ('flags/vk_renderer', b'x' * 108),
                          ('flags-off/vk_offflag', b''), ('webui_token.txt', b'ikzdTESTTOKEN42\n'),
                          ('pid.txt', b'3999999')):
        with open(os.path.join(pc, name), 'wb') as f:
            f.write(content)
    # Files the zip also has: some the same as the zip's, some the user's own, some missing.
    same = {'gs.ini', 'settings/God of War (USA).ini', 'settings/Gran Turismo 4 (USA).ini', 'settings/Oni (USA).ini',
            'cheats/0F6FC6CF.pnach'}
    changed = {'live.ini', 'settings/Castlevania - Lament of Innocence (USA) (En,Ja).ini'}
    missing = {'cheats/77E61C8A.pnach', 'settings/SOCOM II - U.S. Navy SEALs (USA).ini'}
    k = 0
    for rel, content in sorted(files112.items()):
        if not rel.startswith('PCSX2/'):
            continue
        sub = rel[6:]
        if sub.startswith('flags/') or sub.startswith('resources/'):
            continue
        p = os.path.join(pc, sub)
        k += 1
        if sub in missing:
            if os.path.exists(p):
                os.remove(p)
            continue
        if sub in same or (sub not in changed and k % 3 == 0):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as f:
                f.write(content)
        elif sub in changed or os.path.exists(p) or k % 3 == 1:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w') as f:
                f.write('the user changed %s\n' % sub)
    app = data(root, 'homebrew', TITLE)
    for rel, content in (('eboot.bin', eboot_with(eboot_tag)), ('sce_module/libc.prx', b'old libc'),
                         ('sce_sys/param.json', b'{"titleId": "PPSA99203"}'), ('sce_sys/icon0.png', b'old icon'),
                         ('sce_sys/pic0.dds', b'old pic'), ('sce_sys/snd0.at9', b'old snd'),
                         ('sce_sys/pic1.dds.off', b'user kept this')):
        p = os.path.join(app, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(content)


def user_owned(rel):
    """Paths (relative to the root) the installer must never change."""
    if not rel.startswith('data/PCSX2/'):
        return False
    sub = rel[len('data/PCSX2/'):]
    first = sub.split('/', 1)[0]
    return first in ('bios', 'games', 'memcards', 'savestates', 'textures', 'cache', 'logs', 'covers', '_parked',
                     '_to_delete', 'flags', 'flags-off', 'webui_token.txt', 'pid.txt', 'playtime.dat',
                     'lastgame.txt', 'killapp.log', 'settings.log')


# ---------------------------------------------------------------- tests

def t_fresh_install(z112, files112):
    root = os.path.join(WORK, 'fresh')
    shutil.rmtree(root, ignore_errors=True)
    mk_fresh(root)
    publish('vk-285-112', z112)
    notes, err = run(root)
    check(any('checking for a new build' in n for n in notes), 'first notification')
    check(any('New build vk-285-112 (installed: none found)' in n for n in notes), 'new build notification')
    check(any(n.startswith('Downloading vk-285-112 (') for n in notes), 'download notification')
    check(any('Downloading vk-285-112: 50%' in n for n in notes), 'progress 50%')
    check(any('SHA-256 checked' in n for n in notes), 'sha notification')
    check(any(n.startswith('Unpacking') for n in notes), 'unpack notification')
    check(any(n.startswith('Installing vk-285-112') for n in notes), 'install notification')
    check(any('PS5SX2 vk-285-112 installed' in n for n in notes), 'done notification')
    for rel, content in files112.items():
        if rel.startswith(TITLE + '/'):
            p = data(root, 'homebrew', rel)
        elif rel.startswith('PCSX2/'):
            p = data(root, rel)
        else:
            p = data(root, 'PS5SX2-Installer', 'release-notes', 'vk-285-112', rel)
        check(os.path.isfile(p), 'installed: ' + rel)
        check(sha(p) == sha_bytes(content), 'content: ' + rel)
    for d in ('bios', 'games'):
        check(os.path.isdir(data(root, 'PCSX2', d)) and not os.listdir(data(root, 'PCSX2', d)), d + ' made, empty')
    work = data(root, 'PS5SX2-Installer')
    check(not os.path.exists(os.path.join(work, 'journal.txt')), 'journal gone')
    check(not os.path.exists(os.path.join(work, 'staging')), 'staging gone')
    check(not os.listdir(os.path.join(work, 'download')), 'download emptied')
    inst = open(os.path.join(work, 'installed.txt')).read()
    check('tag=vk-285-112' in inst and 'sha256=' + sha_bytes(z112) in inst, 'installed.txt')
    man = open(os.path.join(work, 'manifest.txt')).read()
    check('file %s %s/eboot.bin' % (sha_bytes(files112[TITLE + '/eboot.bin']), TITLE) in man, 'manifest eboot')
    check('flag vk_renderer' in man, 'manifest flags')
    # again: up to date, nothing changes
    before = snapshot(root, skip_work=False)
    notes, err = run(root)
    check(any('PS5SX2 is up to date (vk-285-112)' in n for n in notes), 'up to date')
    a, r, c = diff(before, snapshot(root, skip_work=False))
    c = [x for x in c if not x.endswith('installer.log')]
    check(not a and not r and not c, 'second run changed nothing: %s %s %s' % (a[:5], r[:5], c[:5]))
    return root


def t_user_install(z112, files112):
    root = os.path.join(WORK, 'user')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112)
    before = snapshot(root)
    publish('vk-285-112', z112)
    notes, err = run(root)
    check(any('New build vk-285-112 (installed: vk-285-111)' in n for n in notes), 'sees 111')
    done = [n for n in notes if 'PS5SX2 vk-285-112 installed' in n]
    check(done, 'installed')
    after = snapshot(root)
    a, r, c = diff(before, after)
    check(not r, 'nothing removed outside the work folder: %s' % r[:10])
    for k in a + c:
        check(not user_owned(k), 'user-owned path touched: ' + k)
    # user-owned: byte for byte
    for k, v in before.items():
        if user_owned(k):
            check(after.get(k) == v, 'user file changed: ' + k)
    # app and resources: the zip's
    for rel, content in files112.items():
        if rel.startswith(TITLE + '/'):
            check(sha(data(root, 'homebrew', rel)) == sha_bytes(content), 'app file: ' + rel)
        if rel.startswith('PCSX2/resources/'):
            check(sha(data(root, rel)) == sha_bytes(content), 'resource: ' + rel)
    check(os.path.exists(data(root, 'homebrew', TITLE, 'sce_sys', 'pic1.dds.off')), 'unknown app file kept')
    # config files: the user's kept, missing ones added
    kept = 0
    for rel, content in files112.items():
        if not rel.startswith('PCSX2/') or rel.startswith('PCSX2/resources/') or rel.startswith('PCSX2/flags/'):
            continue
        key = os.path.join('data', rel)
        if key in before:
            check(after[key] == before[key], 'existing config kept as it was: ' + rel)
            if before[key][2] != sha_bytes(content):
                kept += 1
        else:
            check(after[key][2] == sha_bytes(content), 'missing config added: ' + rel)
    check(('%d settings/patch file(s) of yours were kept' % kept) in done[0] if kept else True, 'kept count %d: %s' % (kept, done[0]))
    # backups of what was replaced
    bdirs = os.listdir(data(root, 'PS5SX2-Installer', 'backup'))
    check(len(bdirs) == 1 and bdirs[0].endswith('_before_vk-285-112'), 'one backup set')
    b = os.path.join(data(root, 'PS5SX2-Installer', 'backup'), bdirs[0])
    check(open(os.path.join(b, TITLE, 'sce_module', 'libc.prx'), 'rb').read() == b'old libc', 'old libc in backup')
    check(b'build=vk-285-111' in open(os.path.join(b, TITLE, 'eboot.bin'), 'rb').read(), 'old eboot in backup')
    # flags: first run on an existing setup: none added
    fl_before = sorted(k for k in before if k.startswith('data/PCSX2/flags/'))
    fl_after = sorted(k for k in after if k.startswith('data/PCSX2/flags/'))
    check(fl_before == fl_after, 'flags untouched on the first update')
    return root, before, after


def make_113(files112, z112):
    top = 'PS5SX2-TestBuild1-vk-285-113'
    files = dict(files112)
    eb = files[TITLE + '/eboot.bin']
    check(b'[boot] build=vk-285-112' in eb, 'real eboot has the marker')
    files[TITLE + '/eboot.bin'] = eb.replace(b'[boot] build=vk-285-112', b'[boot] build=vk-285-113')
    files['PCSX2/resources/shaders/vulkan/tfx.glsl'] += b'\n// vk-285-113\n'
    files['PCSX2/settings/Oni (USA).ini'] += b'\n# 113 tweak\n'
    files['PCSX2/settings/God of War (USA).ini'] += b'\n# 113 tweak\n'
    files['PCSX2/settings/Gran Turismo 4 (USA).ini'] += b'\n# 113 tweak\n'
    files['PCSX2/settings/New Game (USA).ini'] = b'upscale_multiplier=3\n'
    files['PCSX2/patches/SLUS-99999_12345678.pnach'] = b'gametitle=new\n'
    files['PCSX2/flags/vk_newflag'] = b''
    files['PCSX2/flags/vk_offflag'] = b''
    del files['PCSX2/cheats/77E61C8A.pnach']
    files['PCSX2/bios/evil.bin'] = b'not a bios'
    files['PCSX2/memcards/Mcd001.ps2'] = b'empty card that must not replace yours'
    files['PCSX2/webui_token.txt'] = b'leaked'
    files['PCSX2/logs/x.log'] = b'no'
    files['PCSX2/gs.ini'] = files112['PCSX2/gs.ini'] + b'# 113\n'
    files['extras/readme.txt'] = b'unknown folder'
    return top, files, build_zip(top, files, dirs=('PCSX2/bios', 'PCSX2/games'))


def t_update_113(root, files112, z112, before112):
    # the user's edits after 112: changes one preset 112 put there, removes another
    pc = data(root, 'PCSX2')
    man = open(data(root, 'PS5SX2-Installer', 'manifest.txt')).read()
    check('PCSX2/settings/God of War (USA).ini' in man and 'PCSX2/settings/Gran Turismo 4 (USA).ini' in man
          and 'PCSX2/settings/Oni (USA).ini' in man, 'the presets are in the manifest: ' + man[:400])
    with open(os.path.join(pc, 'settings', 'God of War (USA).ini'), 'ab') as f:
        f.write(b'user edit after 112\n')
    os.remove(os.path.join(pc, 'settings', 'Gran Turismo 4 (USA).ini'))
    before = snapshot(root)
    top, files113, z113 = make_113(files112, z112)
    publish('vk-285-113', z113)
    notes, err = run(root)
    check(any('New build vk-285-113 (installed: vk-285-112)' in n for n in notes), 'sees 112')
    check(any('PS5SX2 vk-285-113 installed' in n for n in notes), 'installed 113: %s' % notes[-3:])
    after = snapshot(root)
    a, r, c = diff(before, after)
    check(not r, 'nothing removed: %s' % r[:10])
    for k, v in before.items():
        if user_owned(k) and not k.startswith('data/PCSX2/flags/'):
            check(after.get(k) == v, 'user file changed: ' + k)
    check(sha(data(root, 'homebrew', TITLE, 'eboot.bin')) == sha_bytes(files113[TITLE + '/eboot.bin']), 'eboot 113')
    check(sha(data(root, 'PCSX2', 'resources/shaders/vulkan/tfx.glsl')) == sha_bytes(files113['PCSX2/resources/shaders/vulkan/tfx.glsl']), 'tfx 113')
    oni = open(os.path.join(pc, 'settings', 'Oni (USA).ini'), 'rb').read()
    oni_before = before.get('data/PCSX2/settings/Oni (USA).ini')
    if oni_before and oni_before[2] == sha_bytes(files112['PCSX2/settings/Oni (USA).ini']):
        check(oni == files113['PCSX2/settings/Oni (USA).ini'], 'Oni preset (unchanged since 112) updated to 113')
    else:
        check(sha_bytes(oni) == (oni_before[2] if oni_before else None), "Oni preset the user had kept")
    check(open(os.path.join(pc, 'settings', 'God of War (USA).ini'), 'rb').read().endswith(b'user edit after 112\n'), 'user edit kept')
    check(not os.path.exists(os.path.join(pc, 'settings', 'Gran Turismo 4 (USA).ini')), 'removed preset not re-added')
    check(open(os.path.join(pc, 'settings', 'New Game (USA).ini'), 'rb').read() == b'upscale_multiplier=3\n', 'new preset added')
    check(os.path.exists(os.path.join(pc, 'patches', 'SLUS-99999_12345678.pnach')), 'new patch added')
    check(os.path.exists(os.path.join(pc, 'flags', 'vk_newflag')), 'new switch added')
    check(not os.path.exists(os.path.join(pc, 'flags', 'vk_offflag')), 'switch in flags-off not added')
    check(os.path.exists(os.path.join(pc, 'cheats', '77E61C8A.pnach')), 'file dropped from the zip stays')
    check(not os.path.exists(os.path.join(pc, 'bios', 'evil.bin')), 'nothing written into bios/')
    check(open(os.path.join(pc, 'memcards', 'Mcd001.ps2'), 'rb').read() == b'CARD1' * 1000, 'memory card untouched')
    check(open(os.path.join(pc, 'webui_token.txt'), 'rb').read() == b'ikzdTESTTOKEN42\n', 'token untouched')
    check(not os.path.exists(os.path.join(pc, 'logs', 'x.log')), 'nothing written into logs/')
    check(not os.path.exists(data(root, 'extras')) and not os.path.exists(os.path.join(pc, 'extras')), 'unknown folder skipped')
    gs_before = before['data/PCSX2/gs.ini']
    if gs_before[2] == sha_bytes(files112['PCSX2/gs.ini']):
        check(sha(os.path.join(pc, 'gs.ini')) == sha_bytes(files113['PCSX2/gs.ini']), 'gs.ini (as shipped) updated')
    bsets = sorted(os.listdir(data(root, 'PS5SX2-Installer', 'backup')))
    check(len(bsets) == 2 and bsets[1].endswith('_before_vk-285-113'), 'two backup sets: %s' % bsets)
    log = open(data(root, 'PS5SX2-Installer', 'installer.log')).read()
    check('skip PCSX2/bios/evil.bin: inside one of your folders' in log, 'bios skip logged')
    return z113, files113


def t_first_run_history(files112, z112):
    """A console set up by hand from an older zip meets the installer for the first time at 113."""
    root = os.path.join(WORK, 'history')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112, small=True)
    pc = data(root, 'PCSX2')
    # an unchanged copy from an earlier build (= history), a changed one, and a switch the user turned off
    with open(os.path.join(pc, 'settings', 'Oni (USA).ini'), 'wb') as f:
        f.write(files112['PCSX2/settings/Oni (USA).ini'])
    with open(os.path.join(pc, 'settings', 'God of War (USA).ini'), 'wb') as f:
        f.write(files112['PCSX2/settings/God of War (USA).ini'] + b'mine\n')
    if os.path.exists(os.path.join(pc, 'flags', 'vk_16k')):
        os.remove(os.path.join(pc, 'flags', 'vk_16k'))
    top, files113, z113 = make_113(files112, z112)
    publish('vk-285-113', z113)
    notes, _ = run(root)
    check(any('PS5SX2 vk-285-113 installed' in n for n in notes), 'installed: %s' % notes[-2:])
    check(open(os.path.join(pc, 'settings', 'Oni (USA).ini'), 'rb').read() == files113['PCSX2/settings/Oni (USA).ini'],
          "an earlier build's unchanged preset is updated")
    check(open(os.path.join(pc, 'settings', 'God of War (USA).ini'), 'rb').read().endswith(b'mine\n'), 'changed preset kept')
    check(os.path.exists(os.path.join(pc, 'flags', 'vk_newflag')), 'a switch no build had before is added')
    check(not os.path.exists(os.path.join(pc, 'flags', 'vk_16k')), 'a switch the user turned off stays off')
    check(not os.path.exists(os.path.join(pc, 'flags', 'vk_offflag')), 'a switch in flags-off stays off')
    drop('history')


def t_downgrade_and_reinstall(root, z112):
    publish('vk-285-112', z112)
    before = snapshot(root)
    notes, _ = run(root)
    check(any('is newer than the latest release (vk-285-112)' in n for n in notes), 'no downgrade: %s' % notes)
    check(snapshot(root) == before, 'nothing changed on a downgrade refusal')


def t_running_app(root, z_next):
    # 'reinstall' makes it install the same build again, so it gets as far as the running check
    publish('vk-285-113', z_next)
    sw = data(root, 'PS5SX2-Installer', 'reinstall')
    open(sw, 'w').close()
    sleeper = subprocess.Popen(['sleep', '60'])
    try:
        with open(data(root, 'PCSX2', 'pid.txt'), 'w') as f:
            f.write(str(sleeper.pid))
        before = snapshot(root)
        notes, _ = run(root)
        check(any('Close PS5SX2 first' in n for n in notes), 'refuses while running: %s' % notes)
        a, r, c = diff(before, snapshot(root))
        check(not a and not r and not c, 'nothing changed while running')
        check(os.path.exists(sw), "the reinstall switch stays until it's used")
    finally:
        sleeper.kill()
        sleeper.wait()
        with open(data(root, 'PCSX2', 'pid.txt'), 'w') as f:
            f.write('3999999')
    # now it reinstalls 113 and uses the switch up
    notes, _ = run(root)
    check(any('PS5SX2 vk-285-113 installed' in n for n in notes), 'reinstall: %s' % notes[-2:])
    check(not os.path.exists(sw) and os.path.exists(sw + '.used'), 'switch used once')
    notes, _ = run(root)
    check(any('up to date (vk-285-113)' in n for n in notes), 'then up to date again')


def t_bad_downloads(z112, files112):
    root = os.path.join(WORK, 'bad')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112, small=True)
    base = snapshot(root)
    top = 'PS5SX2-TestBuild1-vk-285-120'
    good_files = dict(files112)
    good_files[TITLE + '/eboot.bin'] = eboot_with('vk-285-120')

    def zip_raw(entries):
        bio = io.BytesIO()
        with zipfile.ZipFile(bio, 'w', zipfile.ZIP_DEFLATED) as z:
            for name, content, attr in entries:
                zi = zipfile.ZipInfo(name)
                zi.create_system = 3
                zi.external_attr = attr << 16
                z.writestr(zi, content)
        return bio.getvalue()

    basic = [(top + '/' + TITLE + '/eboot.bin', good_files[TITLE + '/eboot.bin'], 0o100644),
             (top + '/' + TITLE + '/sce_sys/param.json', b'{"titleId": "PPSA99203"}', 0o100644),
             (top + '/PCSX2/resources/filler.bin', random.Random(7).randbytes(6000), 0o100644)]
    cases = {
        'dotdot': zip_raw(basic + [(top + '/PCSX2/../../evil.txt', b'x', 0o100644)]),
        'absolute': zip_raw(basic + [('/etc/evil', b'x', 0o100644)]),
        'backslash': zip_raw(basic + [(top + '/PCSX2\\..\\evil', b'x', 0o100644)]),
        'symlink': zip_raw(basic + [(top + '/PCSX2/link', b'/etc/passwd', 0o120777)]),
        'two tops': zip_raw(basic + [('Other/PCSX2/gs.ini', b'x', 0o100644)]),
        'root file': zip_raw(basic + [('README.txt', b'x', 0o100644)]),
        'duplicate': zip_raw(basic + [(top + '/PCSX2/gs.ini', b'a', 0o100644), (top + '/PCSX2/gs.ini', b'b', 0o100644)]),
        'file+folder': zip_raw(basic + [(top + '/PCSX2/x', b'a', 0o100644), (top + '/PCSX2/x/y', b'b', 0o100644)]),
        'no eboot': zip_raw(basic[1:] + [(top + '/PCSX2/gs.ini', b'a', 0o100644)]),
        'wrong app': zip_raw([basic[0], basic[2], (top + '/' + TITLE + '/sce_sys/param.json', b'{"titleId": "CUSA00001"}', 0o100644)]),
        'control char': zip_raw(basic + [(top + '/PCSX2/a\x01b', b'x', 0o100644)]),
    }
    # CRC damage: flip a byte inside the stored data of a real entry
    good = zip_raw(basic + [(top + '/PCSX2/gs.ini', b'gs' * 5000, 0o100644)])
    zi = zipfile.ZipFile(io.BytesIO(good)).getinfo(top + '/' + TITLE + '/eboot.bin')
    dmg = bytearray(good)
    off = zi.header_offset + 30 + len(zi.filename.encode()) + len(zi.extra) + zi.compress_size // 2
    dmg[off] ^= 0xff
    cases['crc'] = bytes(dmg)
    cases['truncated'] = good[:len(good) // 2]
    for name, zb in cases.items():
        publish('vk-285-120', zb)
        notes, _ = run(root)
        check(any('Nothing was changed' in n for n in notes), '%s refused: %s' % (name, notes[-2:]))
        a, r, c = diff(base, snapshot(root))
        check(not a and not r and not c, '%s: nothing changed (%s %s %s)' % (name, a[:3], r[:3], c[:3]))
    # GitHub-side problems
    gz = build_zip(top, good_files, dirs=('PCSX2/bios', 'PCSX2/games'))
    problems = [
        ('sha mismatch', dict(digest='sha256:' + '0' * 64), 'damaged'),
        ('no digest', dict(digest=''), 'no SHA-256'),
        ('size mismatch', dict(size=len(gz) + 10), "isn't the release's"),
        ('foreign url', dict(url='https://localhost:%d/Someone/Else/releases/download/vk-285-120/x.zip' % PORT), "address isn't"),
        ('404', dict(api_status=404), 'no build is published'),
        ('rate limit', dict(api_status=403, api_headers={'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': '1790620000'}), 'hourly limit'),
        ('bad json', dict(api_body='{"tag_name": "vk-285-120", "assets": [}'), "couldn't be read"),
        ('redirect elsewhere', dict(redirect_to='https://127.0.0.1:%d/assets/' % PORT), 'unexpected server'),
        ('truncated download', dict(asset_truncate=1000), 'download'),
    ]
    for name, kw, text in problems:
        publish('vk-285-120', gz, **kw)
        notes, _ = run(root)
        check(any(text in n for n in notes), '%s: expected "%s" in %s' % (name, text, notes[-2:]))
        a, r, c = diff(base, snapshot(root))
        check(not a and not r and not c, '%s: nothing changed' % name)
    # chunked answers work
    publish('vk-285-120', gz, api_chunked=True, asset_chunked=True)
    notes, _ = run(root)
    check(any('PS5SX2 vk-285-120 installed' in n for n in notes), 'chunked install: %s' % notes[-2:])
    drop('bad')


def t_crash_and_undo(files112, z112):
    ref = os.path.join(WORK, 'crash-ref')
    shutil.rmtree(ref, ignore_errors=True)
    mk_user(ref, files112, small=True)
    publish('vk-285-112', z112)
    run(ref)
    base = os.path.join(WORK, 'crash-base')
    shutil.rmtree(base, ignore_errors=True)
    shutil.copytree(ref, base, symlinks=True)
    start = snapshot(base)
    top, files113, z113 = make_113(files112, z112)
    publish('vk-285-113', z113)
    run(ref)
    final = snapshot(ref)
    log = open(data(ref, 'PS5SX2-Installer', 'installer.log')).read()
    part = log[log.rindex('latest release: vk-285-113'):]
    adds = sum(1 for l in part.splitlines() if ' plan add ' in l)
    repl = sum(1 for l in part.splitlines() if ' plan replace ' in l)
    total = adds * 2 + repl * 4  # crash points: MV journal, MV, PUT journal, PUT
    check(total >= 20, 'enough steps to test (%d adds, %d replaces)' % (adds, repl))
    points = range(1, total + 1)  # every step
    for k in points:
        root = os.path.join(WORK, 'crash-%d' % k)
        shutil.rmtree(root, ignore_errors=True)
        shutil.copytree(base, root, symlinks=True)
        publish('vk-285-113', z113)
        run(root, env_extra={'PS5SX2_TEST_CRASH_AFTER': str(k)}, expect_rc=None)
        # the next start undoes it; GitHub "down" so it stops right after
        publish('vk-285-113', z113, api_status=500)
        notes, _ = run(root)
        mid = snapshot(root)
        a, r, c = diff(start, mid)
        check(not a and not r and not c, 'crash at step %d: undo gives back the start (%s %s %s)' % (k, a[:3], r[:3], c[:3]))
        publish('vk-285-113', z113)
        run(root)
        a, r, c = diff(final, snapshot(root))
        check(not a and not r and not c, 'crash at step %d: the retry ends like a clean install (%s %s %s)' % (k, a[:3], r[:3], c[:3]))
        shutil.rmtree(root)
    drop('crash-ref', 'crash-base')


def t_logger(files112, z112):
    root = os.path.join(WORK, 'logger')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112, small=True)
    publish('vk-285-112', z112)
    run(root)  # installed
    shutil.rmtree(os.path.join(WORK, 'relay'), ignore_errors=True)
    pc = data(root, 'PCSX2')
    os.makedirs(os.path.join(pc, 'logs'), exist_ok=True)
    with open(data(root, 'PS5SX2-Installer', 'tester-name.txt'), 'w') as f:
        f.write('sword.pdf\n')
    env = dict(os.environ)
    env.update({'PS5SX2_ROOT': root, 'PS5SX2_TEST_CA_FILE': os.path.join(WORK, 'ca.pem'),
                'PS5SX2_TEST_API_URL': 'https://localhost:%d/repos/Swordpdf/PS5SX2/releases/latest' % PORT,
                'PS5SX2_TEST_DOWNLOAD_PREFIX': 'https://localhost:%d/Swordpdf/PS5SX2/releases/download/' % PORT,
                'PS5SX2_TEST_ALLOW_HOST': 'localhost',
                'PS5SX2_TEST_RELAY_URL': 'https://localhost:%d/v1/logs' % PORT,
                'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'})
    os.remove(os.path.join(pc, 'pid.txt'))  # no earlier session to report at start
    res = subprocess.Popen([BIN], env=env, stderr=open(os.path.join(WORK, 'logger.stderr'), 'w'))
    try:
        time.sleep(3)
        game = subprocess.Popen(['sleep', '120'])
        with open(os.path.join(pc, 'pid.txt'), 'w') as f:
            f.write(str(game.pid))
        with open(os.path.join(pc, 'logs', 'settings.log'), 'a') as f:
            f.write('2026-09-28 20:00:00  app start: Test build 1 · vk-285-111 (pid 1)\n')
            f.write('2026-09-28 21:00:00  app start: Test build 1 · vk-285-112 (pid %d)\n' % game.pid)
            f.write('2026-09-28 21:00:01  console: firmware 11.40\n')
            f.write('2026-09-28 21:00:05  game start: Black (USA).iso | its settings: Patches/Enable=60 FPS | all games\n')
            f.write('2026-09-28 21:01:00  web 192.168.1.137: tester note: fps drops at the docks [playing Black (USA).iso]\n')
            f.write('2026-09-28 21:02:00  crash: signal 11 at eboot+0x11396e8 (fault address 15396e8); the details are in this session\'s boot.log\n')
        with open(os.path.join(pc, 'logs', 'boot.log'), 'w') as f:
            f.write('[web] listening on port 8844 (token ikzd…)\n[web] http://192.168.1.169:8844/?t=ikzdTESTTOKEN42\nversion 1.2.3.4.5 stays\n')
        with open(os.path.join(pc, 'settings', 'Black (USA).ini'), 'w') as f:
            f.write('Patches/Enable=60 FPS\n')
        time.sleep(7)
        game.kill()
        game.wait()
        deadline = time.time() + 30
        rd = os.path.join(WORK, 'relay')
        while time.time() < deadline and not (os.path.isdir(rd) and any(x.endswith('.json') for x in os.listdir(rd))):
            time.sleep(0.5)
        check(os.path.isdir(rd) and os.listdir(rd), 'a report reached the relay')
        hdr = json.load(open(os.path.join(rd, '000.json')))
        body = open(os.path.join(rd, '000.body'), 'rb').read().decode('utf-8', 'replace')
        h = {k.lower(): v for k, v in hdr.items()}
        check(h.get('x-ps5sx2-end') == 'crash+note', 'end header: %s' % h.get('x-ps5sx2-end'))
        check(h.get('x-ps5sx2-game') == 'Black (USA).iso', 'game header')
        check(h.get('x-ps5sx2-build') == 'vk-285-112', 'build header: %s' % h.get('x-ps5sx2-build'))
        check(h.get('x-ps5sx2-tester') == 'sword.pdf', 'tester header')
        check(len(h.get('x-ps5sx2-console', '')) == 16, 'console id header')
        check('192.168.' not in body and 'x.x.x.x' in body, 'IPs removed')
        check('ikzdTESTTOKEN42' not in body and '<token>' in body, 'token removed')
        check('1.2.3.4.5 stays' in body, 'dotted versions left alone')
        check('fps drops at the docks' in body and 'crash: signal 11' in body, 'session lines in')
        check('pid 1)' not in body.split('===== settings.log')[1].split('===== end of settings.log')[0].split('(pid %d)' % game.pid)[1], 'only this session')
        check('Patches/Enable=60 FPS' in body and 'settings/Black (USA).ini' in body, 'game settings in')
        # a second copy installs (or not) and leaves
        notes, _ = run(root, logger=True, timeout=60)
        log = open(data(root, 'PS5SX2-Installer', 'installer.log')).read()
        check('another copy is already running' in log, 'second copy exits')
        # switch off
        open(data(root, 'PS5SX2-Installer', 'no-log-upload'), 'w').close()
        res.wait(timeout=20)
        check(res.returncode == 0, 'logger ended cleanly')
    finally:
        if res.poll() is None:
            res.kill()
    err = open(os.path.join(WORK, 'logger.stderr')).read()
    check('AddressSanitizer' not in err and 'runtime error' not in err, 'logger sanitizer clean')
    drop('logger')


# ---------------------------------------------------------------- harder cases (from the review)

SHIM = os.path.join(TOP, 'build', 'shim.so')


def drop(*names):
    for n in names:
        shutil.rmtree(os.path.join(WORK, n), ignore_errors=True)


def copy_root(src, name):
    dst = os.path.join(WORK, name)
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, symlinks=True)
    return dst


def plan_counts(root, tag):
    log = open(data(root, 'PS5SX2-Installer', 'installer.log')).read()
    part = log[log.rindex('latest release: ' + tag):]
    adds = sum(1 for l in part.splitlines() if ' plan add ' in l)
    repl = sum(1 for l in part.splitlines() if ' plan replace ' in l)
    return adds, repl


def same_tree(a, b, what):
    x, y, z = diff(a, b)
    check(not x and not y and not z, '%s (added %s, removed %s, changed %s)' % (what, x[:3], y[:3], z[:3]))


def no_part_files(root):
    for dp, dn, fn in os.walk(root):
        for f in fn:
            check(not f.endswith('.part'), 'a partial copy was left: ' + os.path.join(dp, f))


def manifest_lines(root):
    p = data(root, 'PS5SX2-Installer', 'manifest.txt')
    return sorted(l for l in open(p).read().splitlines() if l.startswith(('file ', 'flag '))) if os.path.exists(p) else []


class Env113:
    """A console after 112 (small listing), 113 published, and a reference install of 113 on a copy."""

    def __init__(self, files112, z112):
        self.base = os.path.join(WORK, 'h-base')
        shutil.rmtree(self.base, ignore_errors=True)
        mk_user(self.base, files112, small=True)
        publish('vk-285-112', z112)
        run(self.base)
        self.start = snapshot(self.base)
        self.start_manifest = manifest_lines(self.base)
        self.top, self.files113, self.z113 = make_113(files112, z112)
        ref = copy_root(self.base, 'h-ref')
        publish('vk-285-113', self.z113)
        run(ref)
        self.final = snapshot(ref)
        self.final_manifest = manifest_lines(ref)
        self.adds, self.repl = plan_counts(ref, 'vk-285-113')
        self.moves = self.adds * 2 + self.repl * 4


def t_first_install_crash(z112, files112):
    ref = os.path.join(WORK, 'fi-ref')
    shutil.rmtree(ref, ignore_errors=True)
    mk_fresh(ref)
    publish('vk-285-112', z112)
    run(ref)
    final = snapshot(ref)
    adds, repl = plan_counts(ref, 'vk-285-112')
    moves = adds * 2 + repl * 4
    flags = sorted(k for k in final if k.startswith('data/PCSX2/flags/'))
    check(len(flags) == 15, 'reference has the 15 switches')
    points = sorted(set(list(range(1, moves + 4, 3)) + [moves - 1, moves, moves + 1, moves + 2, moves + 3]))
    for k in points:
        root = os.path.join(WORK, 'fi-%d' % k)
        shutil.rmtree(root, ignore_errors=True)
        mk_fresh(root)
        publish('vk-285-112', z112)
        run(root, env_extra={'PS5SX2_TEST_CRASH_AFTER': str(k)}, expect_rc=None)
        run(root)
        same_tree(final, snapshot(root), 'first install cut at step %d, then run again: same as a clean install' % k)
        shutil.rmtree(root)
    drop('fi-ref')


def t_undo_keeps_changes(env):
    root = copy_root(env.base, 'h-change')
    publish('vk-285-113', env.z113)
    run(root, env_extra={'PS5SX2_TEST_CRASH_AFTER': str(env.moves)}, expect_rc=None)
    gs = data(root, 'PCSX2', 'gs.ini')
    ng = data(root, 'PCSX2', 'settings', 'New Game (USA).ini')
    check(os.path.exists(ng), 'the new preset was placed before the cut')
    with open(gs, 'ab') as f:
        f.write(b'user edit after the crash\n')
    with open(ng, 'ab') as f:
        f.write(b'mine\n')
    publish('vk-285-113', env.z113, api_status=500)
    notes, _ = run(root)
    check(any('except 2 you changed since' in n for n in notes), 'undo says 2 files were left: %s' % notes[:2])
    after = snapshot(root)
    kg, kn = 'data/PCSX2/gs.ini', 'data/PCSX2/settings/New Game (USA).ini'
    check(open(gs, 'rb').read().endswith(b'user edit after the crash\n'), 'edited gs.ini kept')
    check(open(ng, 'rb').read().endswith(b'mine\n'), 'edited new preset kept')
    rest_a = {k: v for k, v in env.start.items() if k not in (kg, kn)}
    rest_b = {k: v for k, v in after.items() if k not in (kg, kn)}
    same_tree(rest_a, rest_b, 'everything else is back as before')
    bks = [os.path.join(dp, f) for dp, dn, fn in os.walk(data(root, 'PS5SX2-Installer', 'backup')) for f in fn
           if dp.endswith('/PCSX2') and f == 'gs.ini']
    check(bks and sha(bks[0]) == env.start[kg][2], "112's gs.ini is still in the backup set")
    publish('vk-285-113', env.z113)
    notes, _ = run(root)
    check(any('PS5SX2 vk-285-113 installed' in n for n in notes), 'installs after')
    check(open(gs, 'rb').read().endswith(b'user edit after the crash\n'), 'and keeps the edited gs.ini')
    drop('h-change')


def t_undo_twice_and_torn(env):
    root = copy_root(env.base, 'h-twice')
    publish('vk-285-113', env.z113)
    run(root, env_extra={'PS5SX2_TEST_CRASH_AFTER': str(env.moves)}, expect_rc=None)
    publish('vk-285-113', env.z113, api_status=500)
    notes, _ = run(root)
    check(not any('except' in n for n in notes), 'nothing reported as changed: %s' % notes[:2])
    same_tree(env.start, snapshot(root), 'first undo')
    rb = data(root, 'PS5SX2-Installer', 'rollback')
    j = sorted(f for f in os.listdir(rb) if f.startswith('journal-undone-'))[-1]
    lines = open(os.path.join(rb, j)).read().split('\n')
    kept = [l for l in lines if not l.startswith('UNDONE')]
    jp = data(root, 'PS5SX2-Installer', 'journal.txt')
    with open(jp, 'w') as f:
        f.write('\n'.join(kept))
    notes, _ = run(root)
    same_tree(env.start, snapshot(root), 'the same undo again, without its UNDONE records, changes nothing')
    check(not any('except' in n for n in notes), 'and reports nothing as changed: %s' % notes[:2])
    with open(jp, 'w') as f:
        f.write('\n'.join(kept) + '\nMV\t%s\t%s' % (data(root, 'PCSX2', 'gs.ini'), data(root, 'PS5SX2-Installer', 'backup')))
    run(root)
    same_tree(env.start, snapshot(root), 'a cut-off record is ignored')
    log = open(data(root, 'PS5SX2-Installer', 'installer.log')).read()
    check('damaged journal record' in log, 'and logged')
    # zeros from a cut-off write in the middle
    with open(jp, 'wb') as f:
        f.write('\n'.join(kept[:3]).encode() + b'\0' * 64 + '\n'.join(kept[3:]).encode())
    run(root)
    same_tree(env.start, snapshot(root), 'zeros in the journal are ignored')
    drop('h-twice')


def t_crash_during_undo(env):
    # (the journal holds absolute paths, so every case crashes its own copy of the console)
    records = env.repl * 2 + env.adds
    for j in range(1, 2 * records + 1):
        root = copy_root(env.base, 'h-uc')
        publish('vk-285-113', env.z113)
        run(root, env_extra={'PS5SX2_TEST_CRASH_AFTER': str(env.moves)}, expect_rc=None)
        publish('vk-285-113', env.z113, api_status=500)
        _, err = run(root, env_extra={'PS5SX2_TEST_CRASH_UNDO_AFTER': str(j)}, expect_rc=None)
        check('simulated crash during the undo' in open(data(root, 'PS5SX2-Installer', 'installer.log')).read(),
              'the undo was cut at its step %d' % j)
        run(root)
        same_tree(env.start, snapshot(root), 'undo cut at its step %d, then run again' % j)
        check(manifest_lines(root) == env.start_manifest, 'manifest unchanged (undo step %d)' % j)
    drop('h-uc')


def t_commit_window(env):
    for k, want in ((env.moves + 1, 'start'), (env.moves + 2, 'final'), (env.moves + 3, 'final')):
        root = copy_root(env.base, 'h-commit-%d' % k)
        publish('vk-285-113', env.z113)
        run(root, env_extra={'PS5SX2_TEST_CRASH_AFTER': str(k)}, expect_rc=None)
        publish('vk-285-113', env.z113, api_status=500)
        notes, _ = run(root)
        if want == 'start':
            same_tree(env.start, snapshot(root), 'cut before COMMIT: undone')
            check(manifest_lines(root) == env.start_manifest, 'cut before COMMIT: old manifest kept')
        else:
            same_tree(env.final, snapshot(root), 'cut after COMMIT (step %d): finished' % k)
            check(manifest_lines(root) == env.final_manifest, 'cut after COMMIT: new manifest')
            check(any('finished now' in n for n in notes), 'says it finished: %s' % notes[:1])
        check(not os.path.exists(data(root, 'PS5SX2-Installer', 'manifest.new')), 'no manifest.new left')
        drop('h-commit-%d' % k)


def t_other_drive(env):
    if not os.path.exists(SHIM):
        raise AssertionError('build/shim.so missing (make -C tests shim)')
    base_env = {'LD_PRELOAD': SHIM, 'ASAN_OPTIONS': 'verify_asan_link_order=0:detect_leaks=1:abort_on_error=1'}
    cases = [
        ('backups on a full drive', {'SHIM_EXDEV': '/backup/', 'SHIM_ENOSPC_PATH': '.part', 'SHIM_ENOSPC_AFTER': '100'}),
        ('settings on a full drive', {'SHIM_EXDEV': '/PCSX2/settings/', 'SHIM_ENOSPC_PATH': '.part', 'SHIM_ENOSPC_AFTER': '100'}),
        ('app folder on a full drive', {'SHIM_EXDEV': '/homebrew/', 'SHIM_ENOSPC_PATH': '.part', 'SHIM_ENOSPC_AFTER': '5000'}),
    ]
    for name, shim in cases:
        root = copy_root(env.base, 'h-drive')
        publish('vk-285-113', env.z113)
        e = dict(base_env)
        e.update(shim)
        notes, _ = run(root, env_extra=e)
        check(any('failed' in n for n in notes), '%s: fails: %s' % (name, notes[-1:]))
        no_part_files(os.path.join(root, 'data'))
        # later, with room again: whatever the first run couldn't put back is put back now
        publish('vk-285-113', env.z113, api_status=500)
        run(root)
        same_tree(env.start, snapshot(root), '%s: everything as before' % name)
        no_part_files(os.path.join(root, 'data'))
    # another drive with room: works, and the backups are complete
    root = copy_root(env.base, 'h-drive-ok')
    publish('vk-285-113', env.z113)
    e = dict(base_env)
    e['SHIM_EXDEV'] = '/backup/'
    notes, _ = run(root, env_extra=e)
    check(any('PS5SX2 vk-285-113 installed' in n for n in notes), 'installs across drives: %s' % notes[-1:])
    same_tree(env.final, snapshot(root), 'same result as on one drive')
    bdir = data(root, 'PS5SX2-Installer', 'backup')
    newest = sorted(d for d in os.listdir(bdir) if d.endswith('_before_vk-285-113'))
    check(len(newest) == 1, 'one backup set for 113')
    for dp, dn, fn in os.walk(os.path.join(bdir, newest[0])):
        for f in fn:
            rel = os.path.relpath(os.path.join(dp, f), os.path.join(bdir, newest[0]))
            key = 'data/homebrew/' + rel if rel.startswith(TITLE + '/') else 'data/' + rel
            check(env.start[key][2] == sha(os.path.join(dp, f)), 'complete backup of ' + rel)
    drop('h-drive', 'h-drive-ok')


def t_links_in_work(env):
    root = copy_root(env.base, 'h-link')
    decoy = os.path.join(root, 'decoy', 'memcards')
    os.makedirs(decoy)
    with open(os.path.join(decoy, 'Mcd001.ps2'), 'wb') as f:
        f.write(b'precious')
    bk = data(root, 'PS5SX2-Installer', 'backup')
    shutil.rmtree(bk, ignore_errors=True)
    os.symlink(os.path.join(root, 'decoy'), bk)
    publish('vk-285-113', env.z113)
    notes, _ = run(root, expect_rc=1)
    check(any('is a link or a file' in n for n in notes), 'refuses a linked folder: %s' % notes)
    check(open(os.path.join(decoy, 'Mcd001.ps2'), 'rb').read() == b'precious', 'nothing behind the link touched')
    drop('h-link')


def t_stale_pid(env):
    root = copy_root(env.base, 'h-stale')
    sleeper = subprocess.Popen(['sleep', '60'])
    try:
        pp = data(root, 'PCSX2', 'pid.txt')
        with open(pp, 'w') as f:
            f.write(str(sleeper.pid))
        old = time.time() - 3600
        os.utime(pp, (old, old))
        publish('vk-285-113', env.z113)
        notes, _ = run(root, env_extra={'PS5SX2_TEST_BOOT_TIME': str(int(time.time()) - 60)})
        check(any('PS5SX2 vk-285-113 installed' in n for n in notes), 'a pid.txt from before the boot is ignored: %s' % notes[-2:])
    finally:
        sleeper.kill()
        sleeper.wait()
        drop('h-stale')


def wait_reports(n, timeout=40):
    rd = os.path.join(WORK, 'relay')
    t = time.time() + timeout
    while time.time() < t:
        if os.path.isdir(rd) and len([x for x in os.listdir(rd) if x.endswith('.json')]) >= n:
            time.sleep(0.3)
            return
        time.sleep(0.3)
    raise AssertionError('expected %d report(s) at the relay' % n)


def report(n):
    rd = os.path.join(WORK, 'relay')
    h = {k.lower(): v for k, v in json.load(open(os.path.join(rd, '%03d.json' % n))).items()}
    return h, open(os.path.join(rd, '%03d.body' % n), 'rb').read().decode('utf-8', 'replace')


def t_logger_short_and_relaunch(files112, z112):
    root = os.path.join(WORK, 'logger2')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112, small=True)
    publish('vk-285-112', z112)
    run(root)
    shutil.rmtree(os.path.join(WORK, 'relay'), ignore_errors=True)
    pc = data(root, 'PCSX2')
    logs = os.path.join(pc, 'logs')
    os.makedirs(logs, exist_ok=True)
    os.remove(os.path.join(pc, 'pid.txt'))
    env = dict(os.environ)
    env.update({'PS5SX2_ROOT': root, 'PS5SX2_TEST_CA_FILE': os.path.join(WORK, 'ca.pem'),
                'PS5SX2_TEST_API_URL': 'https://localhost:%d/repos/Swordpdf/PS5SX2/releases/latest' % PORT,
                'PS5SX2_TEST_DOWNLOAD_PREFIX': 'https://localhost:%d/Swordpdf/PS5SX2/releases/download/' % PORT,
                'PS5SX2_TEST_ALLOW_HOST': 'localhost', 'PS5SX2_TEST_RELAY_URL': 'https://localhost:%d/v1/logs' % PORT,
                'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'})
    logger = subprocess.Popen([BIN], env=env, stderr=open(os.path.join(WORK, 'logger2.stderr'), 'w'))

    def start_session(marker, rotate=True):
        if rotate and os.path.exists(os.path.join(logs, 'boot.log')):
            if os.path.exists(os.path.join(logs, 'boot.1.log')):
                os.replace(os.path.join(logs, 'boot.1.log'), os.path.join(logs, 'boot.2.log'))
            os.replace(os.path.join(logs, 'boot.log'), os.path.join(logs, 'boot.1.log'))
        g = subprocess.Popen(['sleep', '120'])
        with open(os.path.join(logs, 'boot.log'), 'w') as f:
            f.write('[boot] start\n[boot] pid=%d\n%s\n' % (g.pid, marker))
        with open(os.path.join(logs, 'settings.log'), 'a') as f:
            f.write('2026-09-28 22:00:00  app start: Test build 1 · vk-285-112 (pid %d)\n' % g.pid)
            f.write('2026-09-28 22:00:01  game start: %s.iso | its settings: x | all games\n' % marker)
        with open(os.path.join(pc, 'pid.txt'), 'w') as f:
            f.write(str(g.pid))
        return g

    try:
        time.sleep(3)
        # 1. a session that starts and ends between two checks
        g = start_session('SHORT-SESSION')
        time.sleep(0.5)
        g.kill()
        g.wait()
        wait_reports(1)
        h, body = report(0)
        check('SHORT-SESSION' in body and h.get('x-ps5sx2-game') == 'SHORT-SESSION.iso', 'short session reported')
        # 2. a relaunch between two checks: A ends, its logs become .1, B starts
        a = start_session('SESSION-A')
        time.sleep(7)
        a.kill()
        a.wait()
        b = start_session('SESSION-B')
        wait_reports(2)
        h, body = report(1)
        check('SESSION-A' in body.split('===== boot')[1] and 'SESSION-B' not in body.split('===== boot')[1].split('===== end of boot')[0],
              "A's report has A's boot.log, not B's")
        check(h.get('x-ps5sx2-game') == 'SESSION-A.iso', "A's game")
        b.kill()
        b.wait()
        wait_reports(3)
        h, body = report(2)
        check('SESSION-B' in body and h.get('x-ps5sx2-game') == 'SESSION-B.iso', "B's report")
        open(data(root, 'PS5SX2-Installer', 'no-log-upload'), 'w').close()
        logger.wait(timeout=20)
    finally:
        if logger.poll() is None:
            logger.kill()
    err = open(os.path.join(WORK, 'logger2.stderr')).read()
    check('AddressSanitizer' not in err and 'runtime error' not in err, 'sanitizer clean')
    drop('logger2')


def t_logger_release_layout(files112, z112):
    """A console set up from a release zip has no logs/ folder, so PS5SX2 keeps its logs in /data/PCSX2 itself
    (OrbisLogPath). 1.0 and 1.1 only looked in logs/: their reports from testers had no logs, build "unknown" and
    "the shelf". Then: logs/ made while a session runs, and a session whose settings.log lines are gone."""
    root = os.path.join(WORK, 'logger3')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112, small=True)
    publish('vk-285-112', z112)
    run(root)
    shutil.rmtree(os.path.join(WORK, 'relay'), ignore_errors=True)
    pc = data(root, 'PCSX2')
    shutil.rmtree(os.path.join(pc, 'logs'))
    for n in os.listdir(pc):
        if n.startswith(('boot', 'emulog', 'stderr', 'settings.')) and os.path.isfile(os.path.join(pc, n)):
            os.remove(os.path.join(pc, n))
    os.remove(os.path.join(pc, 'pid.txt'))
    env = dict(os.environ)
    env.update({'PS5SX2_ROOT': root, 'PS5SX2_TEST_CA_FILE': os.path.join(WORK, 'ca.pem'),
                'PS5SX2_TEST_API_URL': 'https://localhost:%d/repos/Swordpdf/PS5SX2/releases/latest' % PORT,
                'PS5SX2_TEST_DOWNLOAD_PREFIX': 'https://localhost:%d/Swordpdf/PS5SX2/releases/download/' % PORT,
                'PS5SX2_TEST_ALLOW_HOST': 'localhost', 'PS5SX2_TEST_RELAY_URL': 'https://localhost:%d/v1/logs' % PORT,
                'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'})
    logger = subprocess.Popen([BIN], env=env, stderr=open(os.path.join(WORK, 'logger3.stderr'), 'w'))

    def rotate(d, name, ext):
        # PS5SX2's orbis_rotate_log: <name>.<i-1> -> <name>.<i>, the newest one -> <name>.1
        for i in range(7, 0, -1):
            src = os.path.join(d, name + ext) if i == 1 else os.path.join(d, '%s.%d%s' % (name, i - 1, ext))
            if os.path.exists(src):
                os.replace(src, os.path.join(d, '%s.%d%s' % (name, i, ext)))

    def start_session(marker, d=pc, settings=True):
        for name, ext in (('boot', '.log'), ('emulog', '.txt'), ('stderr', '.log')):
            rotate(d, name, ext)
        g = subprocess.Popen(['sleep', '120'])
        with open(os.path.join(d, 'boot.log'), 'w') as f:
            f.write('[boot] stderr-ok\n[boot] build=vk-285-112\n[boot] pid=%d\n[fe] the shelf\n'
                    '[boot] game: /data/PCSX2/games/%s.iso\n%s-BOOT running the game\n' % (g.pid, marker, marker))
        with open(os.path.join(d, 'emulog.txt'), 'w') as f:
            f.write('%s-EMU PCSX2 log of the game\n' % marker)
        if settings:
            with open(os.path.join(d, 'settings.log'), 'a') as f:
                f.write('2026-09-29 06:00:00  app start: vk-285-112 (pid %d)\n' % g.pid)
                f.write('2026-09-29 06:00:09  game start: %s.iso | its settings: no file | all games\n' % marker)
        with open(os.path.join(pc, 'pid.txt'), 'w') as f:
            f.write(str(g.pid))
        return g

    def section(body, name):
        return body.split('===== %s ' % name)[1].split('===== end of %s' % name)[0]

    try:
        time.sleep(3)
        # 1. one session, logs in /data/PCSX2
        a = start_session('GAME-A')
        time.sleep(7)
        a.kill()
        a.wait()
        wait_reports(1)
        h, body = report(0)
        check(h.get('x-ps5sx2-game') == 'GAME-A.iso' and h.get('x-ps5sx2-build') == 'vk-285-112',
              'no logs/ folder: game and build found (%s, %s)' % (h.get('x-ps5sx2-game'), h.get('x-ps5sx2-build')))
        check('GAME-A-BOOT' in section(body, 'boot.log') and 'GAME-A-EMU' in section(body, 'emulog.txt'),
              "no logs/ folder: the session's boot.log and emulog.txt are in")
        check('Logs: /data/PCSX2, no logs/ folder (boot.log' in body, 'the header says where the logs were')
        # 2. back to the menu: B re-executes into C between two checks; B's report has B's logs (now .1)
        b = start_session('GAME-B')
        time.sleep(7)
        b.kill()
        b.wait()
        c = start_session('SHELF-C')
        wait_reports(2)
        h, body = report(1)
        check(h.get('x-ps5sx2-game') == 'GAME-B.iso' and 'GAME-B-BOOT' in section(body, 'boot.1.log') and
              'SHELF-C' not in section(body, 'boot.1.log') and 'GAME-B-EMU' in section(body, 'emulog.1.txt'),
              "relaunch: B's report has B's logs, not C's")
        # 3. logs/ made while C runs: C's logs are still found in /data/PCSX2
        os.makedirs(os.path.join(pc, 'logs'))
        c.kill()
        c.wait()
        wait_reports(3)
        h, body = report(2)
        check(h.get('x-ps5sx2-game') == 'SHELF-C.iso' and 'SHELF-C-BOOT' in section(body, 'boot.log'),
              'logs/ made during the session: its logs still found in /data/PCSX2')
        # 4. a session in logs/ whose settings.log lines are gone: build and game from its boot.log
        d = start_session('GAME-D', d=os.path.join(pc, 'logs'), settings=False)
        time.sleep(7)
        d.kill()
        d.wait()
        wait_reports(4)
        h, body = report(3)
        check(h.get('x-ps5sx2-game') == 'GAME-D.iso' and h.get('x-ps5sx2-build') == 'vk-285-112',
              'no settings.log line: build and game from boot.log (%s, %s)' % (h.get('x-ps5sx2-game'),
                                                                               h.get('x-ps5sx2-build')))
        check('GAME-D-BOOT' in section(body, 'boot.log') and 'Logs: /data/PCSX2/logs (boot.log' in body,
              "logs/ now used: D's boot.log from there")
        open(data(root, 'PS5SX2-Installer', 'no-log-upload'), 'w').close()
        logger.wait(timeout=20)
    finally:
        if logger.poll() is None:
            logger.kill()
    err = open(os.path.join(WORK, 'logger3.stderr')).read()
    check('AddressSanitizer' not in err and 'runtime error' not in err, 'sanitizer clean')
    drop('logger3')


def logger_env(root):
    env = dict(os.environ)
    env.update({'PS5SX2_ROOT': root, 'PS5SX2_TEST_CA_FILE': os.path.join(WORK, 'ca.pem'),
                'PS5SX2_TEST_API_URL': 'https://localhost:%d/repos/Swordpdf/PS5SX2/releases/latest' % PORT,
                'PS5SX2_TEST_DOWNLOAD_PREFIX': 'https://localhost:%d/Swordpdf/PS5SX2/releases/download/' % PORT,
                'PS5SX2_TEST_ALLOW_HOST': 'localhost', 'PS5SX2_TEST_RELAY_URL': 'https://localhost:%d/v1/logs' % PORT,
                'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'})
    return env


def wait_log(root, text, timeout=40):
    path = data(root, 'PS5SX2-Installer', 'installer.log')
    t = time.time() + timeout
    while time.time() < t:
        if os.path.exists(path) and text in open(path, errors='replace').read():
            return
        time.sleep(0.3)
    raise AssertionError('installer.log never said: %s' % text)


def relay_count():
    rd = os.path.join(WORK, 'relay')
    return len([x for x in os.listdir(rd) if x.endswith('.json')]) if os.path.isdir(rd) else 0


def t_logger_shelf_filter(files112, z112):
    """1.3: a session that only showed the shelf and ended normally isn't sent; one with a game or a problem is,
    and says how many weren't. No notification at start or when sending. The send-shelf-logs switch sends them."""
    root = os.path.join(WORK, 'logger4')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112, small=True)
    publish('vk-285-112', z112)
    run(root)
    shutil.rmtree(os.path.join(WORK, 'relay'), ignore_errors=True)
    pc = data(root, 'PCSX2')
    shutil.rmtree(os.path.join(pc, 'logs'))  # the release layout: logs in /data/PCSX2
    for n in os.listdir(pc):
        if n.startswith(('boot', 'emulog', 'stderr', 'settings.')) and os.path.isfile(os.path.join(pc, n)):
            os.remove(os.path.join(pc, n))
    os.remove(os.path.join(pc, 'pid.txt'))
    nf = os.path.join(root, 'notifications.txt')
    notes_before = len(open(nf).read().splitlines()) if os.path.exists(nf) else 0
    logger = subprocess.Popen([BIN], env=logger_env(root), stderr=open(os.path.join(WORK, 'logger4.stderr'), 'w'))

    def session(boot_lines, events, boot=True):
        for name in ('boot.log', 'emulog.txt', 'stderr.log'):
            if os.path.exists(os.path.join(pc, name)):
                stem, ext = os.path.splitext(name)
                os.replace(os.path.join(pc, name), os.path.join(pc, stem + '.1' + ext))
        g = subprocess.Popen(['sleep', '120'])
        if boot:
            with open(os.path.join(pc, 'boot.log'), 'w') as f:
                f.write('[boot] stderr-ok\n[boot] build=vk-285-112\n[boot] pid=%d\n' % g.pid)
                f.write(''.join(l + '\n' for l in boot_lines))
        if events is not None:
            with open(os.path.join(pc, 'settings.log'), 'a') as f:
                f.write('2026-09-29 10:00:00  app start: vk-285-112 (pid %d)\n' % g.pid)
                f.write(''.join('2026-09-29 10:00:05  %s\n' % e for e in events))
        with open(os.path.join(pc, 'pid.txt'), 'w') as f:
            f.write(str(g.pid))
        time.sleep(6)  # seen running at a check
        g.kill()
        g.wait()
        return g.pid

    shelf = ['[frontend] 12 disc image(s), 0 on USB, scanned in 40 ms']
    try:
        time.sleep(3)
        # 1. the shelf, closed from the system menu: not sent
        pid = session(shelf, ['console: firmware 11.40'])
        wait_log(root, 'session pid %d (vk-285-112, the shelf only, ended ok) not sent' % pid)
        check('(1 since the last report' in open(data(root, 'PS5SX2-Installer', 'installer.log')).read(), 'counted')
        # 2. the shelf left without picking a game ("no game to start"): not sent
        pid = session(shelf + ['[boot] no game to start: closing'],
                      ['no game to start (no disc image found, or none picked): the app closed'])
        wait_log(root, 'session pid %d (vk-285-112, the shelf only, ended no-game) not sent' % pid)
        check(relay_count() == 0, 'nothing sent for the shelf')
        # 3. an empty game list: sent, and it says how many weren't
        session(['[frontend] 0 disc image(s), 0 on USB, scanned in 3 ms', '[boot] no game to start: closing'],
                ['no game to start (no disc image found, or none picked): the app closed'])
        wait_reports(1)
        h, body = report(0)
        check(h.get('x-ps5sx2-end') == 'no-game' and not h.get('x-ps5sx2-game'), 'empty game list sent: %s' % h)
        check('Not sent since the last report: 2 session(s) that only showed the shelf' in body, 'count in the header')
        # 4. a crash on the shelf (settings.log line): sent
        session(shelf, ["crash: signal 11 at eboot+0x1234 (fault address 0); the details are in this session's boot.log"])
        wait_reports(2)
        h, body = report(1)
        check(h.get('x-ps5sx2-end') == 'crash' and 'Not sent since' not in body, 'shelf crash sent: %s' % h)
        # 5. a crash only boot.log shows (no settings.log line for the session): sent
        session(shelf + ['[crash] signal=11 fault_addr=0x0 pc=0x401234 tid=1'], None)
        wait_reports(3)
        h, body = report(2)
        check(not h.get('x-ps5sx2-game') and h.get('x-ps5sx2-end') == 'crash' and
              'Ended: crash - [crash] signal=11' in body, "crash in boot.log only: sent as a crash (%s)" % h)
        # 6. a tester note on the shelf: sent
        session(shelf, ['web x.x.x.x: tester note: the covers are wrong'])
        wait_reports(4)
        h, _ = report(3)
        check(h.get('x-ps5sx2-end') == 'ok+note', 'tester note sent: %s' % h.get('x-ps5sx2-end'))
        # 7. a game: sent
        session(shelf + ['[boot] game: /data/PCSX2/games/Black (USA).iso'],
                ['game start: Black (USA).iso | its settings: no file | all games'])
        wait_reports(5)
        h, _ = report(4)
        check(h.get('x-ps5sx2-game') == 'Black (USA).iso' and h.get('x-ps5sx2-end') == 'ok', 'game sent: %s' % h)
        # 8. a game boot.log names but settings.log doesn't: sent, with the game
        session(shelf + ['[boot] game: /data/PCSX2/games/Oni (USA).iso'], ['console: firmware 11.40'])
        wait_reports(6)
        h, _ = report(5)
        check(h.get('x-ps5sx2-game') == 'Oni (USA).iso', "boot.log's game when settings.log has none: %s" % h)
        # 9. no logs of the session anywhere: sent as no-logs
        pid = session([], None, boot=False)
        wait_reports(7)
        h, body = report(6)
        check(h.get('x-ps5sx2-end') == 'no-logs' and "Game: unknown (this session's logs weren't found)" in body,
              'no logs: sent as no-logs (%s)' % h.get('x-ps5sx2-end'))
        # 10. the switch: the shelf sent too
        open(data(root, 'PS5SX2-Installer', 'send-shelf-logs'), 'w').close()
        session(shelf, ['console: firmware 11.40'])
        wait_reports(8)
        h, body = report(7)
        check(h.get('x-ps5sx2-end') == 'ok' and 'Game: none (the shelf only)' in body, 'send-shelf-logs: sent')
        open(data(root, 'PS5SX2-Installer', 'no-log-upload'), 'w').close()
        logger.wait(timeout=20)
    finally:
        if logger.poll() is None:
            logger.kill()
    notes = open(nf).read().splitlines()[notes_before:]
    check(not any('logs sent' in n or 'log upload is on' in n or 'logs of' in n for n in notes),
          'no notification when starting or sending: %s' % notes)
    check('logger: running (relay https://localhost' in open(data(root, 'PS5SX2-Installer', 'installer.log')).read(),
          'installer.log says the logger runs')
    state = open(data(root, 'PS5SX2-Installer', 'logger-state.txt')).read()
    check('skipped_shelf=0' in state, 'the count restarts after a report: %s' % state)
    err = open(os.path.join(WORK, 'logger4.stderr')).read()
    check('AddressSanitizer' not in err and 'runtime error' not in err, 'sanitizer clean')
    drop('logger4')


BIN_NOLOG = os.path.join(TOP, 'build', 'host-nolog', 'ps5sx2-installer-nolog')


def t_nolog(files112, z112):
    """The no-log build installs and leaves: no logger, nothing kept or sent, whatever sessions there are."""
    root = os.path.join(WORK, 'nolog')
    shutil.rmtree(root, ignore_errors=True)
    mk_user(root, files112, small=True)
    publish('vk-285-112', z112)
    shutil.rmtree(os.path.join(WORK, 'relay'), ignore_errors=True)
    pc = data(root, 'PCSX2')
    with open(os.path.join(pc, 'logs', 'boot.log'), 'w') as f:  # a finished game session, which 1.3 would send
        f.write('[boot] build=vk-285-111\n[boot] pid=3999999\n[boot] game: /data/PCSX2/games/Black (USA).iso\n')
    env = logger_env(root)
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
    t0 = time.time()
    p = subprocess.run([BIN_NOLOG], env=env, capture_output=True, text=True, timeout=120)
    check(p.returncode == 0 and time.time() - t0 < 100, 'no-log build: installed and left (rc %d)' % p.returncode)
    for bad in ('ERROR: AddressSanitizer', 'runtime error:', 'LeakSanitizer'):
        check(bad not in p.stderr, 'sanitizer: ' + bad)
    work = data(root, 'PS5SX2-Installer')
    log = open(os.path.join(work, 'installer.log')).read()
    check('1.3 (no log upload) started' in log and 'the no-log build: no logs are kept or sent' in log, 'its log says so')
    check('PS5SX2 vk-285-112 installed' in open(os.path.join(root, 'notifications.txt')).read(), 'it installed')
    for name in ('outbox', 'console-id.txt', 'logger-state.txt', 'logger.lock'):
        check(not os.path.exists(os.path.join(work, name)), 'no-log build: no %s' % name)
    check(relay_count() == 0, 'no-log build: nothing reached the relay')
    notes = open(os.path.join(root, 'notifications.txt')).read()
    check('PS5SX2 Installer 1.3 (no log upload): checking' in notes and 'log upload is on' not in notes,
          'no-log build: its notifications')
    drop('nolog')


# ---------------------------------------------------------------- main

def main():
    global PORT, SERVER
    os.makedirs(WORK, exist_ok=True)
    for f in ('requests.log', 'state.json', 'server.ready'):
        if os.path.exists(os.path.join(WORK, f)):
            os.remove(os.path.join(WORK, f))
    # test CA and server certificate for localhost
    sh('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '3', '-subj', '/CN=PS5SX2 test CA',
       '-keyout', os.path.join(WORK, 'ca.key'), '-out', os.path.join(WORK, 'ca.pem'),
       '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
    sh('openssl', 'req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
       '-keyout', os.path.join(WORK, 'server.key'), '-out', os.path.join(WORK, 'server.csr'))
    with open(os.path.join(WORK, 'ext.cnf'), 'w') as f:
        f.write('subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n')
    sh('openssl', 'x509', '-req', '-in', os.path.join(WORK, 'server.csr'), '-CA', os.path.join(WORK, 'ca.pem'),
       '-CAkey', os.path.join(WORK, 'ca.key'), '-CAcreateserial', '-days', '3', '-out', os.path.join(WORK, 'server.pem'),
       '-extfile', os.path.join(WORK, 'ext.cnf'))
    PORT = free_port()
    SERVER = subprocess.Popen([sys.executable, os.path.join(HERE, 'server.py'), WORK, str(PORT)],
                              stdout=subprocess.DEVNULL, stderr=open(os.path.join(WORK, 'server.stderr'), 'w'))
    for _ in range(100):
        if os.path.exists(os.path.join(WORK, 'server.ready')):
            break
        time.sleep(0.1)
    try:
        if not REAL_ZIP:
            raise SystemExit('PS5SX2_TEST_REAL_ZIP is not set')
        z112 = open(REAL_ZIP, 'rb').read()
        top, files112 = zip_entries(z112)
        tests = [
            ('fresh install, then up to date', lambda: t_fresh_install(z112, files112)),
        ]
        state = {}

        def user():
            state['root'], state['before'], _ = t_user_install(z112, files112)

        def upd():
            state['z113'], state['f113'] = t_update_113(state['root'], files112, z112, state['before'])

        tests += [
            ("the user's console (from lsall.txt) gets 112", user),
            ('then 113: user edits kept, new files added, user folders untouched', upd),
            ('first installer run on a hand-made setup, straight to 113', lambda: t_first_run_history(files112, z112)),
            ('no downgrade', lambda: t_downgrade_and_reinstall(state['root'], z112)),
            ('refuses while PS5SX2 runs', lambda: t_running_app(state['root'], state['z113'])),
            ('bad zips and GitHub problems change nothing', lambda: t_bad_downloads(z112, files112)),
            ('crash at any step: undone at the next start', lambda: t_crash_and_undo(files112, z112)),
            ('logger: report after the session, redacted; second copy; off switch', lambda: t_logger(files112, z112)),
            ('first install cut at any step, then run again', lambda: t_first_install_crash(z112, files112)),
        ]
        env_holder = {}

        def env113():
            if 'e' not in env_holder:
                env_holder['e'] = Env113(files112, z112)
            return env_holder['e']

        tests += [
            ('undo leaves files changed after the crash', lambda: t_undo_keeps_changes(env113())),
            ('undo twice, lost UNDONE records, cut-off records, zeros', lambda: t_undo_twice_and_torn(env113())),
            ('undo cut at any of its steps', lambda: t_crash_during_undo(env113())),
            ('cut before or after COMMIT', lambda: t_commit_window(env113())),
            ('other drives, full drives (EXDEV/ENOSPC shim)', lambda: t_other_drive(env113())),
            ('a link in the work folder is refused', lambda: t_links_in_work(env113())),
            ('pid.txt from before the boot is ignored', lambda: t_stale_pid(env113())),
            ('logger: short session, relaunch between checks', lambda: t_logger_short_and_relaunch(files112, z112)),
            ('logger: release layout (no logs/ folder), logs/ made later, no settings.log line',
             lambda: t_logger_release_layout(files112, z112)),
            ('logger 1.3: shelf-only sessions not sent, problems and games are; no notifications',
             lambda: t_logger_shelf_filter(files112, z112)),
            ('the no-log build: installs and leaves, nothing kept or sent', lambda: t_nolog(files112, z112)),
        ]
        only = os.environ.get('ONLY')
        failed = 0
        for name, fn in tests:
            if only and not any(o in name for o in only.split('|')):
                continue
            t0 = time.time()
            try:
                fn()
                print('PASS  %-75s %5.1fs' % (name, time.time() - t0), flush=True)
            except Exception as e:  # noqa
                failed += 1
                print('FAIL  %-75s %s' % (name, e), flush=True)
                import traceback
                traceback.print_exc()
        print('%d failed' % failed)
        return 1 if failed else 0
    finally:
        SERVER.kill()


if __name__ == '__main__':
    sys.exit(main())
