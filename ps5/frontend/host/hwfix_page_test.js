// PS5 port frontend, PC test (2026-10-08, AI-assisted): the settings page's own steps for the Hardware fixes group
// (assets/web/index.html): the group is for one game only, a fix shows the game's own value while manual fixes are off, and
// changing a fix (or turning manual fixes on) for a game writes UserHacks=true and the game's own fixes too, as the shelf's
// sheet does. Runs the page's GROUPS, effective(), manualFixesOn() and change() in node, without a browser.
//   node hwfix_page_test.js <index.html>
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
"use strict";
const fs = require("fs"), vm = require("vm"), assert = require("assert");
const html = fs.readFileSync(process.argv[2], "utf8");
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
const head = script.slice(0, script.indexOf("// ---- vk-285-113: memory cards."));
// A top-level function's source, by matching its braces (these have none in their strings).
function fn(name) {
  const start = script.indexOf("\nfunction " + name + "(");
  assert(start >= 0, "no function " + name);
  let i = script.indexOf("{", start), depth = 0;
  for (; i < script.length; i++) {
    if (script[i] === "{") depth++;
    else if (script[i] === "}" && --depth === 0) break;
  }
  return script.slice(start, i + 1);
}
const truthy = script.match(/\nconst truthy = .*\n/)[0];
const ctx = vm.createContext({ console });
vm.runInContext(head + "\nlet data = null; let pending = new Map(); let renders = 0;\n" + truthy + fn("effective") + fn("manualFixesOn") +
  fn("change") + "\nfunction render() { renders++; }\nfunction queueSave() {}\n" +
  "this.api = { GROUPS, effective, manualFixesOn, change, set data(d) { data = d; }, get data() { return data; }, get pending() { return pending; } };",
  ctx);
const api = ctx.api;
const grp = api.GROUPS.find((g) => g.title === "Hardware fixes");
assert(grp && grp.game === true, "the Hardware fixes group is for one game");
assert(api.GROUPS.indexOf(grp) < api.GROUPS.findIndex((g) => g.title === "Crop"), "before Crop, which stays last");
const item = (key) => grp.items.find((it) => it.key === key);
assert(item("UserHacks") && !item("UserHacks").fix, "the Manual row turns them on");
assert(item("UserHacks_TCOffsetX").options.some((o) => o[0] === "525"), "525 is on the texture offset list");

// Kingdom Hearts: the database's native scaling 3.
api.data = { id: "Kingdom Hearts (USA).iso", values: {}, global: { upscale_multiplier: "6" }, gamefixes: { UserHacks_native_scaling: "3" } };
let e = api.effective(item("UserHacks_native_scaling"));
assert.deepStrictEqual([e.v, e.from], ["3", "game"], "manual fixes off: the game's own fix shows");
e = api.effective(item("UserHacks_TCOffsetX"));
assert.deepStrictEqual([e.v, e.from], ["0", "default"], "a fix the game hasn't: the default");
api.change("UserHacks_TCOffsetX", "525");
assert.deepStrictEqual(Object.fromEntries(api.pending), { UserHacks_TCOffsetX: "525", UserHacks: "true", UserHacks_native_scaling: "3" },
  "a changed fix turns manual fixes on and writes the game's own");
e = api.effective(item("UserHacks_native_scaling"));
assert.deepStrictEqual([e.v, e.from], ["3", "own"], "then it is this game's setting");
// More changes while on: nothing else written.
api.pending.clear();
api.change("UserHacks_TCOffsetY", "0");
assert.deepStrictEqual(Object.fromEntries(api.pending), { UserHacks_TCOffsetY: "0" }, "while on, a change is only itself");
// Manual off: the rows keep their values; on again: the game's fix already set, nothing more.
api.change("UserHacks", "false");
assert.strictEqual(api.manualFixesOn(), false);
api.pending.clear();
api.change("UserHacks", "true");
assert.deepStrictEqual(Object.fromEntries(api.pending), { UserHacks: "true" }, "on again: only the switch");
// A reset row (null) never turns anything on.
api.data = { id: "Kingdom Hearts (USA).iso", values: {}, global: {}, gamefixes: { UserHacks_native_scaling: "3" } };
api.pending.clear();
api.change("UserHacks_TCOffsetX", null);
assert.deepStrictEqual(Object.fromEntries(api.pending), { UserHacks_TCOffsetX: null }, "a reset is only itself");
// A game without database fixes (no "gamefixes"): manual fixes on with the change, nothing else.
api.data = { id: "Sample Game (Europe).iso", values: {}, global: {} };
api.pending.clear();
api.change("UserHacks_round_sprite_offset", "1");
assert.deepStrictEqual(Object.fromEntries(api.pending), { UserHacks_round_sprite_offset: "1", UserHacks: "true" }, "a game without fixes");
// Manual fixes on for all games in gs.ini: a game's change is only itself, and its fixes show what the file says.
api.data = { id: "Kingdom Hearts (USA).iso", values: {}, global: { UserHacks: "true" }, gamefixes: { UserHacks_native_scaling: "3" } };
api.pending.clear();
api.change("UserHacks_TCOffsetX", "500");
assert.deepStrictEqual(Object.fromEntries(api.pending), { UserHacks_TCOffsetX: "500" }, "already on for all games");
e = api.effective(item("UserHacks_native_scaling"));
assert.strictEqual(e.from, "default", "manual fixes on: the game's own fix no longer applies by itself");
// The page for all games never gets here (the group isn't drawn), and a change there turns nothing on.
api.data = { id: "@global", values: {}, global: {} };
api.pending.clear();
api.change("UserHacks_TCOffsetX", "500");
assert.deepStrictEqual(Object.fromEntries(api.pending), { UserHacks_TCOffsetX: "500" }, "all games: only itself");
console.log("PASS: the page's Hardware fixes steps (one game only, the game's own fixes shown and written, the switch)");
