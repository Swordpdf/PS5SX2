// PS5SX2 (test build 186, AI-assisted): the path an exec of this app's own eboot names (sceSystemServiceLoadExec: "back to the menu", the eboot watcher,
// the disc helper's exec signal, the disc re-foreground).
//
// swordpdf, 2026-11-02: "hitting L1 + R1 + Touchpad briefly returns me to the home screen and then the shelf. I want to go straight to the shelf". Only the
// exec of the app's own eboot is instant (the shell answers it with a new process of the same app, no home screen), and since 179 it never came back
// (fix181 then sent the main app the relay's way: the home screen, then the shelf). The owner's console (fw 4.03, ShadowMount+ 1.7beta11) says why in its
// klog, for an exec of /data/homebrew/PPSA99203/eboot.bin:
//   [Syscore App] appParam: appType = 0, rootPath = /mnt/sandbox/PPSA99203_000, titleId = PPSA99203
//   [Syscore App] processParam: elfPath = /data/homebrew/PPSA99203/eboot.bin, fullPath = /system_ex/app/PPSA99203/eboot.bin
//   [Syscore App] An error occured while creating new process. error=0x80aa001a errno=2
//   [AppMgr] Exit spawn failed : 0x80aa001a ELF path=/data/homebrew/PPSA99203/eboot.bin
// The shell starts the new process inside the title's sandbox, and the path is looked up there: /data/homebrew is not in the sandbox (errno 2), the app sees
// it only through its jailbreak. 172 to 185 named that path whenever the app could stat it, so the exec worked only where something (a ShadowMount+ of
// the time) made /data visible in the sandbox too. /app0 is the title's own folder in its sandbox (the shell mounts it for every start of the title; for
// a disc tile, the tile's own folder), so /app0/eboot.bin is this title's eboot as the shell sees it, on every console. A new build uploaded over
// /data/homebrew/PPSA99203/eboot.bin is the same file there.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace orbis_self_exec
{
constexpr const char* kOwnEbootPath = "/app0/eboot.bin";
} // namespace orbis_self_exec
