// PS5 port frontend: the shelf's and the notifications' text in the PS5's language (vk-285-110).
//
// The PS5's system language (sceSystemServiceParamGetInt parameter 1) picks a table: English, French,
// Spanish (Spain and Latin America), German, Italian, Dutch and Portuguese (Portugal and Brazil). Other
// languages get English: the shelf's font has the accented Latin letters (Latin-1) but no Cyrillic, Greek
// or CJK. A file <lang_dir>/<code>.txt, when present, replaces entries ("hint.play = Jugar"), so a wrong
// word can be fixed without a new build.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace fe
{
enum class Str : int
{
	HintPlay,           // "Play"
	HintBrowse,         // "Browse"
	HintJump,           // "Jump"
	NoNetwork,          // "Game settings: no network"
	NoGames,            // "No games in /data/PCSX2/games"
	DownloadingCovers,  // "Downloading covers  %d / %d"
	SizeGB,             // "%s GB"
	SizeMB,             // "%s MB"
	Decimal,            // "."
	NotifyStarting,     // "PS5SX2: starting"
	NotifyStartingTest, // "PS5SX2 (testing build): starting"
	NotifyCoversOne,    // "PS5SX2: downloading %d cover"
	NotifyCoversMany,   // "PS5SX2: downloading %d covers"
	NotifyNowPlaying,   // "Now playing: %s\n%s · have fun!" (the title, then how it runs)
	HowNative,          // "native %s" (%s: Vulkan)
	HowSoftware,        // "software renderer"
	NotifyNoGame,       // "PS5SX2: no game to start. ..."
	NotifyNotStarted,   // "PS5SX2: the game didn't start.\n%s" (%s: PCSX2's reason, in English)
	NotifyMenuFailed,   // "PS5SX2: couldn't open the menu, back to the game"
	NotifyStopped,      // "PS5SX2: the game stopped"
	// vk-285-114: the options sheet (Square on the shelf).
	HintSettings,       // "Settings"
	HintMove,           // "Move"
	HintChange,         // "Change"
	HintReset,          // "Reset"
	HintBack,           // "Back"
	SheetThisGame,      // "This game"
	SheetAllGames,      // "All games"
	// vk-285-115: the two commonest failed starts, said briefly (PCSX2's own BIOS text is four paragraphs).
	NotifyNoBios,       // "PS5SX2: no PS2 BIOS found. Copy your BIOS file ... to %s ..." (%s: the BIOS folder)
	NotifyGsFailed,     // "PS5SX2: the game's graphics didn't start. ..."
	// vk-285-116: the sheet's second tab (L2 and R2 switch: "Settings" is HintSettings).
	SheetControls,      // "Controls"
	// vk-285-118: Triangle on the shelf shows the QR code large.
	HintQrCode,         // "QR code"
	// Shared RetroAchievements browser text (AI-assisted).
	Achievements,
	AchievementAll,
	AchievementUnlocked,
	AchievementLocked,
	AchievementPoints,
	AchievementRefresh,
	AchievementPrevious,
	AchievementNext,
	AchievementLoading,
	AchievementImages,
	AchievementEmptyFilter,
	AchievementEmpty,
	AchievementSelectRunning,
	AchievementShowRunning,
	AchievementStartGame,
	AchievementUnavailable,
	AchievementSignIn,
	AchievementConnecting,
	AchievementNoData,
	AchievementRefreshFailed,
	AchievementSelectGame,
	AchievementPending,
	AchievementTriangleRefresh,
	AchievementShelfSignIn,
	AchievementBootFailed,
	AchievementSecureFailed,
	AchievementCreateFailed,
	AchievementConnectFailed,
	AchievementSetFailed,
	AchievementListFailed,
	AchievementAccountMissing,
	AchievementUnlockFailed,
	AchievementMainFailed,
	AchievementCancelled,
	AchievementStartFailed,
	AchievementStatusUnlocked,
	AchievementStatusLocked,
	// 2026-10-05: the RetroAchievements account panel (L1 + Square), redesigned (AI-assisted).
	AccountIntro,       // "Sign in with your RetroAchievements account to earn achievements as you play."
	AccountUsername,    // "Username"
	AccountPassword,    // "Password"
	AccountUsernameHint, // "Your RetroAchievements username"
	AccountPasswordHint, // "Your password"
	AccountSignIn,      // "Sign in"
	AccountSigningIn,   // "Signing in..."
	AccountSignOut,     // "Sign out"
	AccountSignOutAgain, // "Press again to sign out"
	AccountSignedIn,    // "Signed in as %s"
	AccountSignedInNote, // "Achievements unlock as you play. Your password isn't kept on the console."
	HintSelect,         // "Select"
	HintEdit,           // "Edit"
	HintShowPassword,   // "Show password"
	HintHidePassword,   // "Hide password"
	HintDelete,         // "Delete"
	HintDone,           // "Done"
	KeySpace,           // "Space"
	HdTextures,         // "HD textures" (2026-10-05: the shelf's line while a texture pack downloads)
	HdTexturesReady,    // "HD textures ready" (the popup)
	HdTexturesFailed,   // "HD textures failed"
	HintDownload,       // "Download" (the sheet's HD texture pack row)
	HintCancel,         // "Cancel"
	// pr9n (AI-assisted): RetroAchievements' messages after the PR #9 review (Achievements.cpp, PS5 toasts).
	RaServerError,      // "RetroAchievements: server error" (the toast's title; the server's own message under it)
	RaUnreachable,      // "Couldn't reach RetroAchievements. You're still signed in: ..." (the token sign-in at a game's start)
	RaUnsent,           // "An unlock couldn't be sent to RetroAchievements before closing ..." (back to the shelf, offline)
	ShelfNoBios,        // vk-285-134: "No PS2 BIOS found: games can't start. Copy your BIOS file ... to %s" (the shelf's line)
	Count
};

// Picks the table for the PS5's language id and reads <lang_dir>/<code>.txt when it's there (an empty
// `lang_dir` reads nothing). Call once at start, before the shelf or any notification.
void SetLanguage(int ps5_language, const std::string& lang_dir);

// The text in the current language (English where the table or the file has nothing).
const char* Tr(Str id);

// "en", "fr", "es", "es-419", "de", "it", "nl", "pt" or "pt-BR".
const char* LanguageCode();

// The override file's key of an entry ("hint.play"), for messages and tests.
const char* Key(Str id);

// "Europe, Australia" with the region names the table knows translated; anything else unchanged.
std::string Region(const std::string& region);

// A disc image's size: "4.2 GB" / "4,2 GB" / "4,2 Go", or MB under 1 GB.
std::string Size(uint64_t bytes);

// The PS2 system language (0 Japanese, 1 English, 2 French, 3 Spanish, 4 German, 5 Italian, 6 Dutch,
// 7 Portuguese) for a PS5 language id: the regional variants fold in, and languages the PS2 didn't have
// get English.
int Ps2LanguageFor(int ps5_language);

// The name of a PS2 language (0..7) in English, for the logs.
const char* Ps2LanguageName(int ps2_language);

// True when `a` and `b` use the same printf conversions in the same order (a translation must).
bool SameFormat(const char* a, const char* b);
} // namespace fe
