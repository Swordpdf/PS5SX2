// PS5SX2 account platform services (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ps5/frontend/fe_achievements.h"
#include "ps5/frontend/fe_game_achievements.h"

class SettingsInterface;

// Called once after /data is mounted, before the shelf and before CPUThreadInitialize.
bool OrbisAchievementsInit(SettingsInterface& base, const std::string& directory);
fe::AchievementAccountService OrbisAchievementsAccountService();
void OrbisAchievementsCommit();
void OrbisAchievementsConfigure(SettingsInterface& settings);
void OrbisAchievementsRestoreAccount(SettingsInterface& settings); // pr9n: after each ini merge (main-boot.cpp)
void OrbisAchievementsLoginRequired();
void OrbisAchievementsLoginSuccess(const char* username);
void OrbisAchievementsLoginUnreachable(); // pr9n: a token sign-in that got no answer keeps the account
void OrbisAchievementsWaitForLogin();

fe::GameAchievementsService OrbisAchievementsBrowserService(const std::string& cache_directory = "/data/PCSX2/achievement-badges");
void OrbisAchievementsWaitForBrowser();
void OrbisAchievementsStopBrowser(); // pr9n: cancels what it's loading, then waits (a game launching)
