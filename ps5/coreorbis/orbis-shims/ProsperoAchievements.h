// PS5SX2 account platform services (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ps5/frontend/fe_achievements.h"

class SettingsInterface;

// Called once after /data is mounted, before the shelf and before CPUThreadInitialize.
bool OrbisAchievementsInit(SettingsInterface& base, const std::string& directory);
fe::AchievementAccountService OrbisAchievementsAccountService();
void OrbisAchievementsCommit();
void OrbisAchievementsConfigure(SettingsInterface& settings);
void OrbisAchievementsLoginRequired();
void OrbisAchievementsLoginSuccess(const char* username);
void OrbisAchievementsWaitForLogin();
