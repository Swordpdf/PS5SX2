// PS5SX2 log relay: a Cloudflare Worker between the PS5SX2 Installer and a private Discord channel.
//
// The installer POSTs one session report (text/plain, at most 8 MB) to /v1/logs after each PS5SX2 session.
// This Worker checks it and posts it to the channel as a file, with a one-line summary. The Discord webhook
// address is a Worker secret (DISCORD_WEBHOOK_URL): it never ships inside the ELF, so it can't be pulled out
// of it and abused, and it can be changed here without a new ELF.
//
// Optional bindings (see wrangler.toml): UPLOAD_LIMITER and IP_LIMITER (rate limits), RECENT (a KV namespace
// that keeps a one-line record of every attempt for 7 days: time, result, the console ID's first 6 characters,
// build, how the session ended, tester name, size, Cloudflare location; no addresses), LOGS (an R2 bucket that
// keeps a copy of every report).
//   Reading the records: npx wrangler kv key list --binding RECENT --remote

const MAX_BYTES = 8 * 1024 * 1024;
const MAGIC = 'PS5SX2 session report';
const RECENT_TTL = 7 * 24 * 3600;

function json(obj, status) {
  return new Response(JSON.stringify(obj), { status, headers: { 'content-type': 'application/json' } });
}

// Printable ASCII only, cut to n characters.
function clean(s, n) {
  return (s || '').replace(/[^\x20-\x7e]/g, '?').trim().slice(0, n);
}

// Tester-controlled text goes into a code span: Discord shows it as typed, with no formatting, no links and no
// mentions. Backticks can't close the span early.
function code(s) {
  return '`' + s.replace(/`/g, "'") + '`';
}

async function handle(request, env, ctx) {
  if (!env.DISCORD_WEBHOOK_URL) return json({ error: 'relay not configured' }, 503);

  const h = (name) => request.headers.get(name) || '';
  const consoleId = h('x-ps5sx2-console').trim();
  if (!/^[0-9a-f]{16}$/.test(consoleId)) return json({ error: 'bad console id' }, 400);
  if (!h('content-type').toLowerCase().startsWith('text/plain')) return json({ error: 'text/plain only' }, 415);
  const declared = Number(h('content-length'));
  if (!declared || declared > MAX_BYTES) return json({ error: 'size' }, 413);

  // Two limits: per console ID (chosen by the client) and per address (not chosen by the client).
  if (env.UPLOAD_LIMITER) {
    const { success } = await env.UPLOAD_LIMITER.limit({ key: 'id:' + consoleId });
    if (!success) return json({ error: 'too many reports, try later' }, 429);
  }
  if (env.IP_LIMITER) {
    const ip = request.headers.get('cf-connecting-ip') || 'unknown';
    const { success } = await env.IP_LIMITER.limit({ key: 'ip:' + ip });
    if (!success) return json({ error: 'too many reports, try later' }, 429);
  }

  const body = await request.arrayBuffer();
  if (body.byteLength === 0 || body.byteLength > MAX_BYTES) return json({ error: 'size' }, 413);
  const head = new TextDecoder().decode(body.slice(0, MAGIC.length));
  if (head !== MAGIC) return json({ error: 'not a PS5SX2 report' }, 400);

  const build = clean(h('x-ps5sx2-build'), 40) || 'unknown';
  const end = clean(h('x-ps5sx2-end'), 24) || 'ok';
  const game = clean(h('x-ps5sx2-game'), 120);
  const tester = clean(h('x-ps5sx2-tester'), 40);
  const version = clean(h('x-ps5sx2-installer'), 16);
  const when = new Date().toISOString().slice(0, 16).replace('T', ' ');

  const problem = !end.startsWith('ok');
  const line = [
    problem ? '**' + end.replace(/[^a-z+-]/g, '') + '**' : 'ok',
    code(build),
    game ? code(game) : 'the shelf',
    tester ? code(tester) : 'console ' + consoleId.slice(0, 6),
    when + ' UTC',
  ].join(' · ');
  const stamp = when.slice(0, 10) + '_' + when.slice(11, 13) + when.slice(14, 16);
  const fileName = ('PS5SX2-' + build + '-' + consoleId.slice(0, 6) + '-' + stamp + '-' + end + '.txt')
    .replace(/[^A-Za-z0-9._+-]/g, '_');

  const form = new FormData();
  form.append('payload_json', JSON.stringify({ content: line.slice(0, 1900), allowed_mentions: { parse: [] } }));
  form.append('files[0]', new Blob([body], { type: 'text/plain' }), fileName);
  const r = await fetch(env.DISCORD_WEBHOOK_URL, { method: 'POST', body: form });
  if (!r.ok) {
    // 429 from Discord: the installer keeps the report and tries again later
    return json({ error: 'discord answered ' + r.status }, r.status === 429 ? 429 : 502);
  }
  if (env.LOGS) {
    ctx.waitUntil(env.LOGS.put(build + '/' + consoleId + '/' + fileName, body, {
      customMetadata: { end, game, tester, installer: version },
    }));
  }
  return json({ ok: true }, 201);
}

// One record per attempt, in the key's metadata so a single key listing shows them all. Needs proper testing on
// the free plan's KV write limit (1,000 a day): a failed write is ignored and never affects the answer.
async function note(request, env, ctx, res) {
  if (!env.RECENT) return;
  const h = (name) => request.headers.get(name) || '';
  let why = '';
  if (res.status !== 201) {
    try {
      why = String((await res.clone().json()).error || '');
    } catch {
      why = '';
    }
  }
  const cf = request.cf || {};
  const meta = {
    s: res.status,
    why: why.slice(0, 60),
    con: clean(h('x-ps5sx2-console'), 16).slice(0, 6),
    build: clean(h('x-ps5sx2-build'), 40),
    end: clean(h('x-ps5sx2-end'), 24),
    tester: clean(h('x-ps5sx2-tester'), 40),
    ver: clean(h('x-ps5sx2-installer'), 16),
    bytes: Number(h('content-length')) || 0,
    colo: clean(cf.colo, 8),
    cc: clean(cf.country, 4),
  };
  const key = new Date().toISOString() + ' ' + res.status + ' ' + Math.random().toString(36).slice(2, 8);
  console.log('report attempt', JSON.stringify(meta));
  ctx.waitUntil(env.RECENT.put(key, '', { expirationTtl: RECENT_TTL, metadata: meta }).catch(() => {}));
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname !== '/v1/logs') return json({ error: 'not found' }, 404);
    if (request.method !== 'POST') return json({ error: 'POST only' }, 405);
    let res;
    try {
      res = await handle(request, env, ctx);
    } catch (e) {
      res = json({ error: 'relay error: ' + String((e && e.message) || e).slice(0, 80) }, 500);
    }
    await note(request, env, ctx, res);
    return res;
  },
};
