// Dithering choices through the real options sheet and settings files. (AI-assisted)
// Synthetic settings only; this does not test the renderer or game visuals.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_options.h"

#include <cstdio>
#include <sys/stat.h>

static int s_checks = 0, s_failures = 0;

static void Check(bool ok, const char* what)
{
	++s_checks;
	s_failures += !ok;
	std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

static const fe::OptionsSheet::Row* Dithering(const fe::OptionsSheet& sheet)
{
	for (const auto& row : sheet.rows())
		if (row.def && row.def->key == "dithering_ps2")
			return &row;
	return nullptr;
}

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	const std::string root = argv[1]; // a fresh temporary directory supplied by the runner
	fe::OptionsPaths paths;
	paths.settings_dir = root + "/settings";
	paths.gs_ini = root + "/gs.ini";
	if (mkdir(paths.settings_dir.c_str(), 0700) != 0 ||
		!fe::settings::WriteFileAtomic(paths.gs_ini, "upscale_multiplier=4\n"))
		return 2;
	fe::GameInfo game;
	game.stem = "Synthetic game";
	game.title = "Synthetic game";
	game.file = "Synthetic game.iso";
	fe::OptionsSheet all, one;
	all.Open(paths, nullptr);
	one.Open(paths, &game);
	Check(Dithering(all) && Dithering(one), "dithering is available globally and per game");
	if (!Dithering(all) || !Dithering(one))
		return 1;
	using From = fe::OptionsSheet::From;
	const auto* row = Dithering(all);
	Check(row->def->def == "2" && !row->def->manual_fix && !row->def->restart,
		"existing default and live non-hack behavior are preserved");
	Check(all.Value(*row) == "Unscaled" && all.Source(*row) == From::Default,
		"global sheet starts at Unscaled without writing a setting");
	Check(one.Value(*Dithering(one)) == "Unscaled" && one.Source(*Dithering(one)) == From::Default,
		"game sheet starts at the same default");
	const char* labels[] = {"Off", "Scaled", "Unscaled", "Force 32bit"};
	// Cycle every choice through both sheets, reopen from disk, then remove the override.
	for (auto* sheet : {&all, &one})
	{
		for (const int value : {3, 0, 1, 2})
		{
			Check(sheet->Step(*Dithering(*sheet), 1), "native choice saves successfully");
			fe::OptionsSheet reopened;
			reopened.Open(paths, sheet == &all ? nullptr : &game);
			Check(reopened.OwnValue("dithering_ps2") == std::to_string(value) &&
					  reopened.Value(*Dithering(reopened)) == labels[value] && reopened.Source(*Dithering(reopened)) == From::Own,
				"chosen value and label survive reopening");
			Check(reopened.OwnValue("UserHacks").empty(), "dithering does not enable manual hardware fixes");
		}
		Check(sheet->Reset(*Dithering(*sheet)) && sheet->OwnValue("dithering_ps2").empty(), "reset removes the override");
	}
	all.Step(*Dithering(all), 1); // default 2 -> 3
	one.Reload();
	Check(one.Value(*Dithering(one)) == "Force 32bit" && one.Source(*Dithering(one)) == From::Global,
		"game inherits the global choice");
	one.Step(*Dithering(one), 1); // inherited 3 -> own 0
	all.Step(*Dithering(all), 1); // 3 -> 0
	all.Step(*Dithering(all), 1); // 0 -> 1
	one.Reload();
	Check(one.Value(*Dithering(one)) == "Off" && one.Source(*Dithering(one)) == From::Own,
		"explicit Off overrides a different global choice");
	one.Reset(*Dithering(one));
	Check(one.Value(*Dithering(one)) == "Scaled" && one.Source(*Dithering(one)) == From::Global,
		"game reset resumes inheritance");
	all.Reset(*Dithering(all));
	one.Reload();
	Check(one.Value(*Dithering(one)) == "Unscaled" && one.Source(*Dithering(one)) == From::Default,
		"global reset restores the core default for inheriting games");
	Check(all.OwnValue("upscale_multiplier") == "4", "unrelated settings are preserved");
	std::printf("%d/%d checks passed\n", s_checks - s_failures, s_checks);
	return s_failures ? 1 : 0;
}
