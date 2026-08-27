// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// RomLoader — thin static helpers that factor the
//   "find the file on disk → temporarily allow writes to the ROM range
//    → load → restore the write-protect bitmap"
// pattern used by every card that needs to flash its ROM into Memory.
//
// Inspired by POM1's RomLoader, adapted to POM2's per-byte writable
// bitmap. Caller is responsible for serialising access to `mem` (typically
// by holding EmulationController::stateMutex).

#ifndef POM2_ROM_LOADER_H
#define POM2_ROM_LOADER_H

#include <cstdint>
#include <string>
#include <vector>

class Memory;

class RomLoader
{
public:
    /// Load `path` into memory at `addr`. Auto-detects file size; refuses
    /// to load past $FFFF. Temporarily disables write protection on the
    /// target range and restores it afterward.
    /// Returns true on success; on failure, fills `error` with a human-
    /// readable message.
    static bool loadBinary(Memory& mem, const std::string& path,
                           uint16_t addr, std::string& error);

    /// Same as loadBinary but takes pre-decoded bytes (useful for unit
    /// tests and embedded ROMs).
    static bool loadBytes(Memory& mem, const std::vector<uint8_t>& bytes,
                          uint16_t addr, std::string& error);

    /// Probe a list of relative paths in order, returning the first one
    /// that exists. Used by cards plugging into the bus to find their
    /// ROM regardless of whether POM2 was launched from build/, repo
    /// root, or an installed location.
    static std::string probeRomPath(const std::vector<std::string>& candidates);

    /// Convenience: probe for `name` under `roms/`, `../roms/`, `../../roms/`.
    /// Returns empty string if nothing found.
    static std::string probeStandardRomPath(const std::string& filename);
};

#endif // POM2_ROM_LOADER_H
