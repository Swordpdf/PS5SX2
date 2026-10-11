// PS5SX2 cooperative privilege elevation.
// SPDX-License-Identifier: GPL-3.0-or-later
// live-15: the Lapy owned-root daemon protocol (see ProsperoHenJailbreak.cpp).
// 2.02: the /download0/etahen_jailbreak request is back beside it (orbis_elevate).
#pragma once

// 2.02: elevation by whichever answers -- the Lapy owned-root daemon
// (/download0/elevate_proc) or etaHEN/OnionHEN/the PS5SX2 Helper
// (/download0/etahen_jailbreak) -- then try_ports (main-boot's CMD ports 9028/9069).
// hint: what worked at the last start ("lapy", "etahen", "ports", "none", or ""
// when not known); that method goes first with its long wait, the others get
// short probes. Must be called on the main thread before any other thread is
// created (the Lapy daemon rejects a multi-threaded target). Returns "already"
// (euid 0 at the call: nothing requested), "lapy", "etahen", "ports" or "none".
// Runs at most once per process.
const char* orbis_elevate(const char* hint, bool (*try_ports)());

// One-page JIT allocatability probe. True root makes this succeed even when
// geteuid() keeps reporting 1.
bool orbis_probe_jit();

// Logs whether this process can see a HEN config and our TitleID in it (diagnostic).
void orbis_log_hen_config();
