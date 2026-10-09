// PS5SX2 cooperative privilege elevation.
// SPDX-License-Identifier: GPL-3.0-or-later
// live-15: the etaHEN marker broker is replaced by the Lapy owned-root daemon
// protocol (see ProsperoHenJailbreak.cpp). The old hen_jailbreak::Broker and the
// /download0/etahen_jailbreak write are gone.
#pragma once

// Cooperative elevation through the Lapy owned-root daemon. Must be called on the
// main thread before any other thread is created. True only when /data is verified
// writable after acknowledgement; false means the caller should take the existing
// no-jailbreak fallback. Runs at most once per process.
bool orbis_lapy_jailbreak();

// One-page JIT allocatability probe. True root makes this succeed even when
// geteuid() keeps reporting 1.
bool orbis_probe_jit();

// Logs whether this process can see a HEN config and our TitleID in it (diagnostic).
void orbis_log_hen_config();
