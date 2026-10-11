// PS5 port frontend, PC harness (vk-285-116, AI-assisted): the shelf sheet's options (fe_options.cpp OptionGroups) as
// lines that options_parity.js writes the same way from the settings page's GROUPS (assets/web/index.html), so
// test-settings.sh can check that the two list the same options, in the same order, with the same values:
//   G <tab> <title>
//   I <key> | <label> | <default> | <restart> | <value>=<label>, ...
// The sheet's button symbols ("<glyph>  Cross") are left out of the labels; the hints may differ (the sheet's are shorter).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_options.h"

#include <cstdio>
#include <string>

namespace
{
std::string Plain(const std::string& label)
{
	if (!label.empty() && static_cast<unsigned char>(label[0]) >= 0x80)
	{
		const size_t gap = label.find("  ");
		if (gap != std::string::npos)
			return label.substr(gap + 2);
	}
	return label;
}
} // namespace

int main()
{
	for (const fe::OptionGroup& g : fe::OptionGroups())
	{
		std::printf("G %s %s%s\n", g.tab == fe::kTabControls ? "controls" : "settings", g.title.c_str(),
			g.game_only ? " [one game]" : g.global_only ? " [all games]" : ""); // 2026-10-08: Hardware fixes; vk-285-137: Shelf
		for (const fe::OptionDef& d : g.items)
		{
			std::string choices;
			for (const fe::OptionChoice& c : d.choices)
				choices += (choices.empty() ? "" : ", ") + c.value + "=" + Plain(c.label);
			std::printf("I %s | %s | %s | %s | %s\n", d.key.c_str(), Plain(d.label).c_str(), d.def.c_str(), d.restart ? "restart" : "live",
				choices.c_str());
		}
	}
	return 0;
}
