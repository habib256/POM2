// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "RomLoader.h"
#include "Memory.h"
#include "ResourcePaths.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

bool RomLoader::loadBinary(Memory& mem,
                           const std::string& path,
                           uint16_t addr,
                           std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "Cannot open ROM: " + path; return false; }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    if (bytes.empty()) { error = "ROM is empty: " + path; return false; }
    return loadBytes(mem, bytes, addr, error);
}

bool RomLoader::loadBytes(Memory& mem,
                          const std::vector<uint8_t>& bytes,
                          uint16_t addr,
                          std::string& error)
{
    if (bytes.empty()) { error = "ROM payload is empty"; return false; }
    if (static_cast<size_t>(addr) + bytes.size() > 0x10000) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "ROM size %zu B at $%04X overflows $FFFF",
                      bytes.size(), addr);
        error = buf;
        return false;
    }
    if (!mem.loadRomBytes(bytes.data(), bytes.size(), addr)) {
        error = "Memory::loadRomBytes failed";
        return false;
    }
    error.clear();
    return true;
}

std::string RomLoader::probeRomPath(const std::vector<std::string>& candidates)
{
    // Each candidate is resolved against POM2's full resource search set
    // (CWD, build/-relative, executable-relative, FHS install) so a ROM
    // dropped in roms/ is found whether POM2 runs from the repo root, an
    // AppImage, or /usr/bin. See ResourcePaths.h.
    return pom2::findFirstResource(candidates);
}

std::string RomLoader::probeStandardRomPath(const std::string& filename)
{
    // The executable-relative / FHS roots are supplied by findResource;
    // a single "roms/<name>" candidate now covers every launch layout.
    return pom2::findResource("roms/" + filename);
}
