// Actual web settings choices, inheritance, reset and save requests. (AI-assisted)
// Uses production functions with browser/network stand-ins; no visual-renderer claim.
// SPDX-License-Identifier: GPL-3.0-or-later
"use strict";
const fs = require("fs"), vm = require("vm"), assert = require("assert");
const html = fs.readFileSync(process.argv[2], "utf8");
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
const head = script.slice(0, script.indexOf("// ---- vk-285-113: memory cards."));
function fn(name, async = false) {
  const start = script.indexOf("\n" + (async ? "async " : "") + "function " + name + "(");
  assert(start >= 0, "missing function " + name);
  let i = script.indexOf("{", start), depth = 0;
  for (; i < script.length; i++) {
    if (script[i] === "{") depth++;
    else if (script[i] === "}" && --depth === 0) break;
  }
  return script.slice(start, i + 1);
}
const ctx = vm.createContext({ console });
vm.runInContext(head + "\nlet data, current; let pending = new Map(); let requests = []; let state = {};\n" +
  script.match(/\nconst truthy = .*\n/)[0] + fn("effective") + fn("manualFixesOn") + fn("change") + fn("save", true) + `
  function render() {} function queueSave() {} function toast() {} function refreshGames() {}
  async function api(path, options) { requests.push({path, body: options.body}); return data; }
  this.test = { GROUPS, effective, change, save, requests, pending,
    set data(d) { data = d; current = d.id; }, get data() { return data; } };
`, ctx);
const api = ctx.test;
const graphics = api.GROUPS.find(g => g.title === "Graphics");
const item = graphics.items.find(it => it.key === "dithering_ps2");
assert(item, "dithering is exposed in web graphics settings");
assert(!graphics.game && !graphics.global, "available globally and per game");
assert.strictEqual(item.def, "2");
assert(!item.restart && !item.fix, "live setting, without enabling manual hardware fixes");
assert.deepStrictEqual(JSON.parse(JSON.stringify(item.options)),
  [["0", "Off"], ["1", "Scaled"], ["2", "Unscaled"], ["3", "Force 32bit"]]);
const effective = () => { const e = api.effective(item); return [e.v, e.from]; };
(async () => {
  for (const id of ["@global", "Synthetic game.iso"]) {
    api.data = { id, values: { upscale_multiplier: "4" }, global: {} };
    assert.deepStrictEqual(effective(), ["2", "default"]);
    for (const value of ["0", "1", "2", "3"]) {
      api.change(item.key, value);
      assert.deepStrictEqual(effective(), [value, "own"]);
      assert.deepStrictEqual(Object.fromEntries(api.pending), { dithering_ps2: value });
      await api.save();
      assert.deepStrictEqual(JSON.parse(JSON.stringify(api.requests.at(-1))), {
        path: "/api/settings?id=" + encodeURIComponent(id), body: "set dithering_ps2=" + value + "\n"
      });
    }
    api.change(item.key, null);
    await api.save();
    assert.strictEqual(api.requests.at(-1).body, "unset dithering_ps2\n");
    assert.deepStrictEqual(effective(), ["2", "default"]);
    assert.strictEqual(api.data.values.upscale_multiplier, "4");
  }
  api.data = { id: "Synthetic game.iso", values: {}, global: { dithering_ps2: "3" } };
  assert.deepStrictEqual(effective(), ["3", "global"]);
  api.change(item.key, "0");
  assert.deepStrictEqual(effective(), ["0", "own"]);
  api.change(item.key, null);
  assert.deepStrictEqual(effective(), ["3", "global"]);
  console.log("PASS: web dithering choices, save requests, inheritance, reset and unrelated settings");
})().catch(e => { console.error(e); process.exitCode = 1; });
