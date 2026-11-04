// PS5 port frontend, PC test (2.12, AI-assisted; swordpdf: "add an option to add a game from the shelf to the xmb"): the settings page's Home screen tile row
// (assets/web/index.html). Runs the page's own script in node with a small fake DOM and a fake console (/api/state, /api/games, /api/settings, /api/tile), so what is
// checked is the real select(), render(), loadTile(), tileAct() and tileHtml(): the section "Home screen" comes right after the Shelf group of a game (never for all
// games, never when the console says it is not available, never for another game's answer), each phase shows the console's words (escaped) and the right button
// (add, remove, or disabled while busy), a busy row is looked at again every 1.5 s and an idle one is not, a press POSTs "add\n<game>" or "remove\n<game>" as text/plain.
//   node gametile_page_test.js <index.html>
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
"use strict";
const fs = require("fs"), vm = require("vm"), assert = require("assert");
const html = fs.readFileSync(process.argv[2], "utf8");
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- the fake console
const games = [
  { id: "God of War (USA).iso", title: "God of War", serial: "SCUS-97399", region: "USA", size: 3.7e9, badges: [], cover: false, playing: false },
  { id: "Okami (USA).iso", title: "Okami", serial: "SLUS-21309", region: "USA", size: 3.1e9, badges: [], cover: false, playing: false },
];
const tile = {};      // game id -> the answer /api/tile gives (the test sets it)
const tileDelay = {}; // game id -> ms the answer is held back
const calls = [];     // every request: {method, url, body, type}
let nowPlaying = null;
async function serve(url, opts) {
  const method = (opts && opts.method) || "GET";
  const body = opts && opts.body;
  const type = opts && opts.headers && opts.headers["Content-Type"];
  calls.push({ method, url, body, type });
  const u = new URL(url, "http://127.0.0.1");
  const id = u.searchParams.get("id");
  const ok = (j) => ({ ok: true, status: 200, statusText: "OK", json: async () => JSON.parse(JSON.stringify(j)) }); // a copy, as the network gives
  if (u.pathname === "/api/state") return ok({ build: "test", mode: "menu", playing: nowPlaying, strings: {}, test_build: 0 });
  if (u.pathname === "/api/games") return ok(games);
  if (u.pathname === "/api/memcards") return ok({ cards: [], sizes: [8, 16, 32, 64], defaults: ["Mcd001.ps2", "Mcd002.ps2"] });
  if (u.pathname === "/api/achievements") return ok({});
  if (u.pathname === "/api/settings")
    return ok({ id, file: id === "@global" ? "gs.ini" : "settings/" + id + ".ini", exists: false, values: {}, global: {}, patches: [], cheats: [], serial: "X", recommended: null });
  if (u.pathname === "/api/tile") {
    if (method === "POST") {
      const [op, game] = body.split("\n");
      const t = tile[game];
      if (t && t.post409) return { ok: false, status: 409, statusText: "Conflict", json: async () => ({ error: t.post409 }) };
      tile[game] = Object.assign({}, tile[game], tile[game] && tile[game].after ? tile[game].after : { phase: "asking", busy: true, value: "Adding...", can: "none", help: "Asked the helper." }, { id: game, available: true });
      return ok(tile[game]);
    }
    if (tileDelay[id]) await sleep(tileDelay[id]);
    if (!tile[id]) return { ok: false, status: 404, statusText: "Not Found", json: async () => ({ error: "no such game" }) };
    return ok(tile[id]);
  }
  return { ok: false, status: 404, statusText: "Not Found", json: async () => ({ error: "not found" }) };
}

// ---- a fake DOM: just enough for this script
const els = {};
function makeEl(key) {
  const handlers = {};
  const el = {
    key, innerHTML: "", textContent: "", value: "", src: "", style: { setProperty() {}, removeProperty() {} }, dataset: {}, hidden: false,
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    addEventListener(type, fn) { (handlers[type] = handlers[type] || []).push(fn); },
    fire(type) { (handlers[type] || []).forEach((f) => f({ preventDefault() {} })); },
    querySelectorAll(sel) {
      if (sel !== "[data-tile]") return [];
      const out = [];
      for (const m of el.innerHTML.matchAll(/<button[^>]*data-tile="([a-z]+)"([^>]*)>/g)) {
        const b = { dataset: { tile: m[1] }, disabled: /disabled/.test(m[2]), addEventListener(t, f) { b.click = f; } };
        out.push(b);
      }
      el.buttons = out;
      return out;
    },
    querySelector() { return null; }, setAttribute() {}, removeAttribute() {}, focus() {}, select() {}, setSelectionRange() {}, remove() {}, appendChild() {},
    scrollTo() {}, getContext() { return { drawImage() {}, getImageData() { return { data: [] }; } }; },
    scrollLeft: 0, clientWidth: 1000, offsetLeft: 0, offsetWidth: 100,
  };
  return el;
}
const document = {
  getElementById(id) { return els[id] || (els[id] = makeEl(id)); },
  querySelectorAll() { return []; },
  querySelector() { return null; },
  createElement() { return makeEl("new"); },
  body: { appendChild() {} },
  documentElement: { style: { setProperty() {} } },
  execCommand() { return true; },
  addEventListener() {},
};
const ctx = vm.createContext({
  console, document, URL, setTimeout, clearTimeout, setInterval, clearInterval, Map, Promise, JSON, Math, Date, Object, Array, String, Number, parseFloat, parseInt,
  location: { href: "http://127.0.0.1:8844/" }, history: { replaceState() {} }, localStorage: { getItem() { return null; }, setItem() {}, removeItem() {} },
  fetch: serve, requestAnimationFrame: (f) => setTimeout(f, 0), cancelAnimationFrame: clearTimeout, performance, navigator: {}, window: {},
});
vm.runInContext(script, ctx);
const run = (code) => vm.runInContext(code, ctx);
const settings = () => els["settings"].innerHTML;
// A click on the Home screen tile button: the fake's querySelectorAll kept the buttons render() found and wired.
const press = () => {
  const b = els["settings"].buttons && els["settings"].buttons[0];
  assert(b && b.click, "the page drew and wired a [data-tile] button");
  b.click();
};

function groupAfterShelf() {
  const groups = run("GROUPS").filter((g) => (g.tab || "settings") === "settings" && !g.global);
  const i = groups.findIndex((g) => g.title === "Shelf" && g.game);
  assert(i >= 0 && groups[i + 1], "the game's Shelf group has a group after it");
  return groups[i + 1].title;
}
const on = (extra) => Object.assign({ available: true, phase: "on", busy: false, value: "On", help: "On the home screen. Cross takes it off again.", tile: "PPSA99251", reason: "", can: "remove" }, extra);
const off = (extra) => Object.assign({ available: true, phase: "off", busy: false, value: "Off", help: "Puts God of War on the PS5 home screen.", tile: "", reason: "", can: "add" }, extra);

(async () => {
  const A = games[0].id, B = games[1].id;
  const tilePolls = () => calls.filter((c) => c.url.indexOf("/api/tile") === 0 && c.method === "GET").length;

  // Start: the page selects the first game (start()); the console answers /api/tile with Off.
  tile[A] = Object.assign(off(), { id: A });
  tile[B] = Object.assign(on(), { id: B });
  await sleep(150);
  let h = settings();
  assert(run("current") === A, "the first game is selected");
  assert(h.includes("<h2>Home screen</h2>"), "the Home screen section is drawn for a game when /api/tile says available");
  assert(h.includes('<div class="label">Home screen tile</div>'), "the row's label is there");
  assert(h.includes('<div class="from" style="margin-top:2px">Off</div>'), "the value is the console's word: Off");
  assert(h.includes("Puts God of War on the PS5 home screen."), "the help is the console's");
  assert(/data-tile="add"/.test(h) && !/data-tile="add"[^>]*disabled/.test(h) && h.includes(">Add to home screen</button>"), "Off: an enabled Add to home screen button");
  // where the Shelf group was (its one row is the eye by the title), before the next group
  const shelf = h.indexOf("<h2>Shelf</h2>"), tileAt = h.indexOf("<h2>Home screen</h2>"), next = h.indexOf("<h2>" + groupAfterShelf() + "</h2>");
  assert(shelf < 0 && tileAt >= 0 && tileAt < next, "the section is where the Shelf group was, above " + groupAfterShelf() + " (" + [shelf, tileAt, next] + ")");
  assert(h.indexOf("<h2>Home screen</h2>", tileAt + 1) < 0, "drawn once");
  assert(tilePolls() === 1, "one look at /api/tile for the game: " + tilePolls());
  assert(calls.find((c) => c.url.indexOf("/api/tile?id=") === 0 && c.url.endsWith(encodeURIComponent(A))), "the request names the game by its file name, encoded");
  await sleep(1700);
  assert(tilePolls() === 1, "an idle row is not looked at again: " + tilePolls());

  // A press: POST "add\n<game>" as text/plain; the answer is drawn; a busy row is looked at again every 1.5 s.
  tile[A].after = { phase: "asking", busy: true, value: "Adding...", can: "none", help: "Asked the helper." };
  press(); // the button the page drew (and wired) last
  await sleep(100);
  const post = calls.filter((c) => c.method === "POST" && c.url === "/api/tile");
  assert(post.length === 1 && post[0].body === "add\n" + A && post[0].type === "text/plain", "the press POSTs add, a line break and the game, as text/plain: " + JSON.stringify(post));
  h = settings();
  assert(h.includes(">Adding...</div>") && h.includes(">Working…</button>") && /data-tile="add"[^>]*disabled/.test(h), "asking: the console's word, a disabled Working... button");
  const before = tilePolls();
  tile[A] = Object.assign(tile[A], { phase: "installing", busy: true, value: "Installing...", help: "The tile is built." });
  await sleep(1700);
  assert(tilePolls() > before, "a busy row is looked at again (1.5 s): " + (tilePolls() - before));
  assert(settings().includes(">Installing...</div>") && settings().includes("The tile is built."), "the new answer is drawn");
  tile[A] = Object.assign(tile[A], on({ id: A }));
  await sleep(1700);
  h = settings();
  assert(h.includes('margin-top:2px">On</div>') && /data-tile="remove"/.test(h) && h.includes(">Remove from home screen</button>") && !/data-tile="remove"[^>]*disabled/.test(h), "On: an enabled Remove from home screen button");
  const idle = tilePolls();
  await sleep(1700);
  assert(tilePolls() === idle, "not busy any more: no more looks");
  // remove
  tile[A].after = { phase: "asking", busy: true, value: "Removing...", can: "none", help: "Asked the helper." };
  press();
  await sleep(100);
  const post2 = calls.filter((c) => c.method === "POST" && c.url === "/api/tile");
  assert(post2.length === 2 && post2[1].body === "remove\n" + A, "the second press POSTs remove: " + JSON.stringify(post2[1]));
  assert(settings().includes(">Removing...</div>"), "Removing... is drawn");
  tile[A] = Object.assign(tile[A], off({ id: A }));
  await sleep(1700);

  // An error from the console (409: still working / own tile) is a toast, and the row is looked at again.
  tile[A].post409 = "this game is running from its tile";
  press();
  await sleep(150);
  assert(els["toast"].textContent.includes("this game is running from its tile"), "a 409 is shown: " + els["toast"].textContent);
  delete tile[A].post409;

  // Another game: its own answer; the previous game's answer must not flash; a late answer for a game that is no longer shown is dropped.
  tileDelay[A] = 400;
  tile[A] = Object.assign(off({ id: A, value: "Off", help: "late answer for A" }), {});
  tile[B] = Object.assign(on({ id: B, help: "B is on <b>the</b> home screen" }), {});
  const sA = run("select")(A); // A's /api/tile is held back 400 ms
  await sleep(50);
  const sB = run("select")(B);
  await Promise.all([sA, sB]);
  await sleep(600);
  assert(run("current") === B, "B is the game shown");
  h = settings();
  assert(h.includes('margin-top:2px">On</div>') && !h.includes("late answer for A"), "B's answer is drawn; the late answer for A is not");
  assert(h.includes("B is on &lt;b&gt;the&lt;/b&gt; home screen") && !h.includes("<b>the</b>"), "what the console says is escaped");
  delete tileDelay[A];
  // tileInfo is cleared at the start of select(): the old answer is not drawn for the new game while the new one is on its way.
  tileDelay[A] = 300;
  const pending = run("select")(A);
  await sleep(30);
  assert(!settings().includes("Home screen tile") || run("tileInfo") === null, "select() cleared the previous game's tile answer: " + run("tileInfo"));
  await pending;
  await sleep(400);
  delete tileDelay[A];

  // available: false: no section.
  tile[A] = { id: A, available: false, why: "off" };
  await run("select")(A);
  await sleep(100);
  assert(!settings().includes("<h2>Home screen</h2>"), "available:false: no Home screen section");
  // not found (404): no section, no crash.
  delete tile[A];
  await run("select")(A);
  await sleep(100);
  assert(!settings().includes("<h2>Home screen</h2>"), "a 404: no section");
  // all games: never.
  tile["@global"] = off({ id: "@global" });
  await run("select")("@global");
  await sleep(100);
  assert(!settings().includes("<h2>Home screen</h2>"), "all games: no Home screen section");
  assert(!calls.some((c) => c.url.indexOf("/api/tile?id=%40global") === 0 || c.url.indexOf("/api/tile?id=@global") === 0), "all games: /api/tile is not asked at all");
  // another tab: the section is not drawn there.
  tile[A] = on({ id: A });
  await run("select")(A);
  await sleep(100);
  assert(settings().includes("<h2>Home screen</h2>"), "back on a game: the section is there again");
  run("tab = 'controls'; render();");
  assert(!settings().includes("<h2>Home screen</h2>"), "the Controls tab has no Home screen section");
  run("tab = 'settings'; render();");
  assert(settings().includes("<h2>Home screen</h2>"), "the Settings tab has it");
  // phases and their buttons
  const phases = [
    ["nohelper", "No helper", "none", false, "Add to home screen", true],
    ["noanswer", "No answer", "add", false, "Add to home screen", false],
    ["stuck", "Stuck", "remove", false, "Remove from home screen", false],
    ["failed", "Failed", "add", false, "Add to home screen", false],
    ["waiting", "Adding...", "none", true, "Working…", true],
    ["removing", "Removing...", "none", true, "Working…", true],
  ];
  for (const [phase, value, can, busy, label, disabled] of phases) {
    tile[A] = { id: A, available: true, phase, value, can, busy, help: "help for " + phase, tile: "", reason: "" };
    await run("select")(A);
    await sleep(60);
    h = settings();
    const m = h.match(/<button class="btn primary"[^>]*data-tile="([a-z]+)"([^>]*)>([^<]*)<\/button>/);
    assert(m, phase + ": a button");
    assert(m[3] === label && /disabled/.test(m[2]) === disabled, phase + ": the button says " + label + (disabled ? " and is disabled" : " and is enabled") + " (" + m[3] + ")");
    assert(h.includes("help for " + phase) && h.includes(">" + value + "</div>"), phase + ": the console's value and help");
  }
  // stop the poll timers of the busy rows, and finish.
  tile[A] = on({ id: A });
  await run("select")(A);
  await sleep(100);
  console.log("PASS: the page's Home screen tile row (drawn under Shelf for a game only, the console's words escaped, add/remove/disabled buttons, polls only while busy, late answers dropped)");
  process.exit(0);
})().catch((e) => {
  console.error("FAIL:", e && e.stack || e);
  process.exit(1);
});
