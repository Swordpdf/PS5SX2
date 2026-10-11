// PS5 port (vk-285-115): how the app ends itself.
//
// A title's _exit() (and exit(), or a return from main) ends in SIGSYS inside libkernel on the console, which the crash
// handlers report as a crash. OrbisExitApp (main-boot.cpp) drains the logs and asks the system to close the app
// (sceSystemServiceLoadExec("exit")), falling back to _exit() if it hasn't within 10 s.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

[[noreturn]] void OrbisExitApp(int status);
