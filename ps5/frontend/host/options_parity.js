// PS5 port frontend, PC harness (vk-285-116, AI-assisted): the settings page's GROUPS (assets/web/index.html) as the
// lines options_dump.cpp writes for the shelf's sheet (see there). The page's text fields (Textures folder) have no row
// on the sheet: left out.
//   node options_parity.js <index.html>
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
"use strict";
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
// What the page offers: everything up to the memory cards' part, which needs no browser.
const head = script.slice(0, script.indexOf("// ---- vk-285-113: memory cards."));
const GROUPS = new Function(head + "\nreturn GROUPS;")();
const out = [];
for (const g of GROUPS) {
  if (g.items.every((it) => it.type === "text")) continue; // 2026-10-08: a group of text fields only (Folders) isn't on the sheet
  out.push("G " + (g.tab || "settings") + " " + g.title + (g.game ? " [one game]" : g.global ? " [all games]" : "")); // 2026-10-08: Hardware fixes; vk-285-137: Shelf
  for (const it of g.items) {
    if (it.type === "text") continue;
    const options = it.type === "toggle" ? [["false", "Off"], ["true", "On"]] : it.options;
    out.push("I " + it.key + " | " + it.label + " | " + it.def + " | " + (it.restart ? "restart" : "live") + " | " +
      options.map((o) => o[0] + "=" + o[1]).join(", "));
  }
}
process.stdout.write(out.join("\n") + "\n");
