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

// DHGR NTSC 8-px chroma palette (][-pix, BSD-2-Clause) - see the .cpp for
// provenance. Indexed [x%4][trailing 8-dot pattern][rgb]; pattern bit 7 is
// the dot itself, bit 0 the dot 7 positions earlier.

#ifndef HGRPAINT_DHGR_NTSC8_PALETTE_H
#define HGRPAINT_DHGR_NTSC8_PALETTE_H

#include <cstdint>

namespace hgrpaint {
extern const uint8_t kDhgrNtsc8Srgb[4][256][3];
} // namespace hgrpaint

#endif // HGRPAINT_DHGR_NTSC8_PALETTE_H
