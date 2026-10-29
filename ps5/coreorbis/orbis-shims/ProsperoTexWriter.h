// PS5SX2 (2026-10-29, AI-assisted): the HD texture packs' writer process, started through the console's ELF loader (see tex-writer/tex_writer.c,
// frontend/fe_texwrite.h and ProsperoTexWriter.cpp). The console holds a title's own writes to ~2 MiB/s after its first ~1.3 GiB; a process of its
// own is not held back.
#pragma once

#include <string>

// True when this build embeds the payload.
bool OrbisTexWriterBuilt();

// Sends the payload, with `request` (fe::TexWriterRequest's text) written over its marker, to the ELF loader on 127.0.0.1:9021. True once it went
// out (the payload then connects back to the app); `why` says what failed otherwise.
bool OrbisTexWriterLaunch(const std::string& request, std::string& why);
