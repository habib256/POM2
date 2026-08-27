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

// 2IMG (.2mg) container — the ONE definition of the flags-word semantics,
// shared by the three loaders (DiskImage 5.25", Disk35Image 800K,
// Block512Backing HDV). Spec: DiskImage_2MG_Info.txt; cross-checked
// against CiderPress (`kFlagLocked = 0x80000000`) and AppleWin.
//
//   bit 31   locked (write-protect)
//   bit  8   volume-number-valid
//   bits 0-7 DOS 3.3 volume number (only meaningful when bit 8 is set)
//
// History: all three loaders independently tested bit 0 for the lock —
// locked images mounted writable and odd volume numbers read as
// write-protected. The subtlety worth keeping in one place: bit 0 is
// retained as a LENIENT extra write-protect signal, but only when no
// volume field is declared (bit 8 clear) — with bit 8 set, bit 0 is just
// the low bit of the volume number.

#ifndef POM2_TWOIMG_H
#define POM2_TWOIMG_H

#include <cstdint>

namespace pom2 {

/// Write-protect ("locked") status of a 2IMG flags word.
inline constexpr bool twoImgWriteProtected(uint32_t flags)
{
    return (flags & (1u << 31)) != 0 ||
           ((flags & 1u) != 0 && (flags & (1u << 8)) == 0);
}

/// DOS 3.3 volume number of a 2IMG flags word (254 when not declared).
inline constexpr uint8_t twoImgVolume(uint32_t flags)
{
    return (flags & (1u << 8)) ? static_cast<uint8_t>(flags & 0xFF)
                               : static_cast<uint8_t>(254);
}

} // namespace pom2

#endif // POM2_TWOIMG_H
