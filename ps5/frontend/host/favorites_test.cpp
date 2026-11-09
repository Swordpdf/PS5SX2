// Favorites settings and persistence with the actual OptionsSheet and game readers. (AI-assisted)
// Synthetic files only, under a new directory supplied as argv[1]; no GPU or player files.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fe_options.h"
#include "fe_settings.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>

static int checks = 0, failures = 0;
static void Check(bool ok, const char* what)
{
	++checks;
	failures += !ok;
	std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

static const fe::OptionsSheet::Row* Row(const fe::OptionsSheet& sheet, const char* key)
{
	for (const auto& row : sheet.rows())
		if (row.def && row.def->key == key)
			return &row;
	return nullptr;
}

static bool Step(fe::OptionsSheet& sheet, const char* key)
{
	const auto* row = Row(sheet, key);
	return row && sheet.Step(*row, 1);
}

int main(int argc, char** argv)
{
	if (argc != 2 || mkdir(argv[1], 0700) != 0)
	{
		std::fprintf(stderr, "Supply a new, nonexistent fixture directory.\n");
		return 2;
	}
	const std::string root = argv[1];
	fe::OptionsPaths paths;
	paths.settings_dir = root + "/settings";
	paths.gs_ini = root + "/gs.ini";
	paths.presets = "[@global]\nupscale_multiplier=2\n[SAME-00001]\nupscale_multiplier=3\n";
	Check(mkdir(paths.settings_dir.c_str(), 0700) == 0, "new settings directory created");
	Check(fe::settings::WriteFileAtomic(root + "/presets.ini", paths.presets), "synthetic presets written");
	fe::GameInfo a, b, unknown, second_disc;
	a.stem = "First game"; a.file = a.stem + ".iso"; a.title = "Same displayed title"; a.serial = "SAME-00001";
	b = a; b.stem = "Other edition"; b.file = b.stem + ".chd";
	unknown = a; unknown.stem = "Unknown game"; unknown.file = unknown.stem + ".iso"; unknown.serial.clear();
	second_disc = a; second_disc.stem = "First game (Disc 2)"; second_disc.file = second_disc.stem + ".iso";
	a.path = "/unmounted-synthetic-storage/First game.iso";
	a.hidden = true;
	a.favorite = true;
	fe::ReadBadges(a, paths.settings_dir, paths.gs_ini, root);
	Check(!a.favorite && !a.hidden, "absent preferences default off and reset stale scan values");
	Check(!fe::ShowFavoritesOnly(paths.gs_ini), "new installation shows all games");
	fe::OptionsSheet sheet;
	sheet.Open(paths, &a);
	const auto* favorite = Row(sheet, "PS5SX2/Favorite");
	Check(favorite != nullptr, "game settings expose Favorite");
	Check(Row(sheet, "PS5SX2/ShelfView") == nullptr, "shelf filter is not a per-game option");
	Check(Step(sheet, "PS5SX2/Favorite"), "favorite can be enabled through actual option action");
	fe::ReadBadges(a, paths.settings_dir, paths.gs_ini, root);
	Check(a.favorite, "favorite persists and game reader restores it");
	fe::ReadBadges(b, paths.settings_dir, paths.gs_ini, root);
	Check(!b.favorite, "different stems with duplicate titles and serials remain independent");
	fe::ReadBadges(second_disc, paths.settings_dir, paths.gs_ini, root);
	Check(!second_disc.favorite, "another disc retains its existing separate settings identity");
	fe::OptionsSheet reopened;
	reopened.Open(paths, &a);
	favorite = Row(reopened, "PS5SX2/Favorite");
	Check(favorite && reopened.Value(*favorite) == "On", "reopened sheet reads saved favorite");
	a.path = "/reconnected-synthetic-storage/First game.iso";
	fe::ReadBadges(a, paths.settings_dir, paths.gs_ini, root);
	Check(a.favorite, "storage path changes do not remove a saved favorite");
	fe::GameInfo same_stem = a;
	same_stem.file = a.stem + ".chd";
	fe::ReadBadges(same_stem, paths.settings_dir, paths.gs_ini, root);
	Check(same_stem.favorite, "same stem shares the existing per-game settings identity");
	sheet.Open(paths, &unknown);
	Check(Step(sheet, "PS5SX2/Favorite"), "an image without a serial can be favorited");
	fe::ReadBadges(unknown, paths.settings_dir, paths.gs_ini, root);
	Check(unknown.favorite, "missing serial does not prevent favorite persistence");
	sheet.Open(paths, nullptr);
	Check(Row(sheet, "PS5SX2/Favorite") == nullptr, "global sheet does not mark every game as favorite");
	Check(Row(sheet, "PS5SX2/ShelfView") != nullptr, "global sheet exposes shelf filter");
	Check(Step(sheet, "PS5SX2/ShelfView"), "Favorites filter can be selected");
	Check(fe::ShowFavoritesOnly(paths.gs_ini), "Favorites filter survives a fresh settings read");
	std::string what, error;
	Check(fe::settings::ApplyRecommended(paths.gs_ini, paths.presets, "@global", "# fixture", what, error), "recommended global settings apply");
	Check(fe::ShowFavoritesOnly(paths.gs_ini), "recommended settings preserve shelf filter");
	Check(fe::settings::ApplyRecommended(paths.settings_dir + "/" + a.stem + ".ini", paths.presets, a.serial, "# fixture", what, error), "recommended game settings apply");
	fe::ReadBadges(a, paths.settings_dir, paths.gs_ini, root);
	Check(a.favorite, "recommended settings preserve game favorite");
	Check(fe::settings::EditSettingsFile(paths.gs_ini, "# fixture", {{fe::settings::Change::Set, "PS5SX2/Favorite", "true"}}, what, error), "synthetic stray global favorite written");
	sheet.Open(paths, &b);
	favorite = Row(sheet, "PS5SX2/Favorite");
	Check(favorite && sheet.Value(*favorite) == "Off", "favorite does not inherit a stray global value");
	fe::ReadBadges(b, paths.settings_dir, paths.gs_ini, root);
	Check(!b.favorite, "game reader also ignores a stray global favorite");
	sheet.Open(paths, &a);
	Check(Step(sheet, "PS5SX2/HideGame"), "favorite can also be hidden");
	fe::ReadBadges(a, paths.settings_dir, paths.gs_ini, root);
	Check(a.favorite && a.hidden, "favorite and hidden choices persist independently");
	Check(Step(sheet, "PS5SX2/Favorite"), "favorite can be removed");
	fe::ReadBadges(a, paths.settings_dir, paths.gs_ini, root);
	Check(!a.favorite && a.hidden, "removing favorite does not unhide or delete game settings");
	sheet.Open(paths, nullptr);
	Check(Step(sheet, "PS5SX2/ShelfView"), "All games view can be restored");
	Check(!fe::ShowFavoritesOnly(paths.gs_ini), "All games view survives restart");
	Check(fe::settings::EditSettingsFile(paths.gs_ini, "# fixture", {{fe::settings::Change::Set, "PS5SX2/ShelfView", "unknown"}}, what, error), "synthetic invalid filter written");
	Check(!fe::ShowFavoritesOnly(paths.gs_ini), "unknown filter values safely show all games");
	std::printf("%d/%d checks passed; %d failed\n", checks - failures, checks, failures);
	return failures ? 1 : 0;
}
