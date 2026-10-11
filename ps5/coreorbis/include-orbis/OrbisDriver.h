// PS5 port (vk-285-115): which Vulkan driver this eboot links.
//
// link-vk.sh links ps5vk (Swordpdf/PS5HB_Vulkan), link-radv.sh RADV (mihawk-99/PS5_Vulkan's port of Mesa's RADV). The same
// objects go into both eboots; the few places where the two drivers want different things ask here at run time
// (orbis-shims/orbis_vk.cpp: RADV's entry point is a weak reference, null in a ps5vk eboot).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

bool OrbisDriverIsRADV();
const char* OrbisDriverName(); // "RADV" or "ps5vk"
